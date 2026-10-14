#pragma once
// Sequences: ordered steps, each recalling a snapshot or driving a timeline (and carrying a text for the operator),
// played by GO / GO BACK — the cue list of a show. Several sequences may execute concurrently;
// the selected sequence determines which one GO / STOP edits, not which running cues survive.
// Sequence steps are saved with the project; runtime playhead positions are not. Owned by the Engine (Engine::sequencer()), it goes through the Engine's public API only, and
// shares its lock.
//
// A step waits its pre-wait after its GO, then recalls its snapshot, whose fade is the step's action (its duration:
// the snapshot's longest time) — or acts on its timeline (Play: the time it plays, when it ends, followed as it goes:
// a speed or a loop mode changed meanwhile, by a step or by OSC, is taken into account, and a frozen timeline
// (speed 0) is waited for until it moves on and ends; the other actions take no time). What comes next (as in QLab):
//  - Wait: nothing, the next step waits for GO (button, Space, OSC);
//  - Follow: the next step gets its GO once this one was triggered (pre-wait over), after the post-wait, without
//    waiting for the action to end;
//  - AutoFollow: the next step gets its GO once the action is over, after the post-wait.
// A snapshot recalled does not stop the ones still running: only the values it holds itself are taken over.
//
// The waits run on a clock thread of their own (the cue clock): a pre-wait or a follow ends on time even while the
// interface is busy (a drag, a project loading, a flood of log lines). A step the clock fires recalls its snapshot
// from there at once; the interface is told afterwards (setRecallRecorder: its undo stack), with the state before.
// A GO from the interface or OSC is played on the Sequencer's own thread, as before (setRecaller).

#include "Anim.h"
#include "FrameTiming.h" // histogram reused for cue-clock execution timings

#include <QElapsedTimer>
#include <QJsonArray>
#include <QJsonObject>
#include <QObject>
#include <QRecursiveMutex>
#include "AnnotatedMutex.h"
#include <QString>
#include <algorithm>
#include <array>
#include <atomic>
#include <functional>
#include <memory>
#include <vector>

class Engine;
class QThread;
class QTimer;

class Sequencer : public QObject
{
    Q_OBJECT
public:
    enum class StepContinue { Wait = 0, Follow = 1, AutoFollow = 2 };
    struct Step {
        quint64 snapshot = 0; // its id (0: none yet)
        QString text;
        quint64 timeline = 0; // or a timeline's (an animation's) id, driven by `action`
        AnimAction action = AnimAction::Play;
        double seekTime = 0;            // Seek
        AnimLoop loop = AnimLoop::Loop; // LoopMode
        int repeat = 0;                 // LoopMode
        double speed = 1;               // Speed
        double preWait = 0, postWait = 0; // seconds
        StepContinue next = StepContinue::Wait;
    };
    // A step on its way: since its GO, through its pre-wait, its action, and until the next step's GO
    struct Run {
        int step = -1;
        quint64 sequenceId = 0; // stable runtime owner even when sequences are inserted or reordered
        Step target; // what it does (its snapshot, or its timeline and action)
        quint64 snapshot = 0;
        double elapsed = 0; // since its GO
        double preWait = 0, duration = 0, postWait = 0;
        StepContinue next = StepContinue::Wait;
        bool fired = false, continued = false;
        bool trackPending = false, tracking = false; // a Play: its action lasts as long as the timeline does
        // When the next step gets its GO, counted from this one's GO (< 0: never — Wait)
        double continueAt() const
        {
            switch (next) {
            case StepContinue::Follow: return preWait + postWait;
            case StepContinue::AutoFollow: return preWait + duration + postWait;
            default: return -1;
            }
        }
        double end() const { return std::max(preWait + duration, continueAt()); }
    };
    struct Sequence {
        QString name;
        bool loop = false; // GO on the last step goes to the first
        std::vector<Step> steps;
    };

    // What a recall replaced: given with a recall the cue clock made, for the interface's undo
    struct RecallBefore {
        QJsonArray layers;
        QJsonObject composition;
        QJsonArray interactions;
    };
    // How late the steps fired, after their pre-wait or their GO from the step before (seconds): the cue clock's
    // precision, measured. p99 and max over every step fired since the last reset.
    struct CueTiming {
        quint64 fired = 0;
        double last = 0, max = 0, total = 0;
        std::array<quint64, 12> buckets{}; // ≤ 0.5, 1, 2, 4, 8, 16, 32, 64, 128, 256, 512 ms, beyond
        double percentile(double p) const; // upper edge of the bucket (seconds); 0: none yet
    };
    // Clock-thread snapshot work; these separate preparation/undo cost
    // from actual scene application. All durations are milliseconds.
    // A manual GO through the UI recaller is not included.
    struct CueWorkTiming {
        FrameTiming::Histogram capture; // serialize affected layers, composition, interactions for Undo
        FrameTiming::Histogram recall;  // apply the snapshot (source changes, fades, interactions)
        FrameTiming::Histogram total;   // capture + recall, including clock path overhead
    };

    Sequencer(Engine &engine, Annotated<QRecursiveMutex> &mutex);
    ~Sequencer() override;
    void shutdownClock(); // the clock thread stopped (the engine, before it goes)

    int count() const;
    Sequence sequence(int i) const;
    void setSequence(int i, const Sequence &s);
    int add(const Sequence &s, int at = -1);
    void remove(int i);
    int current() const;           // -1: none
    void setCurrent(int i);        // select only; other sequences' pre-waits / follows keep running
    // Project-specific automatic GO when the project opens; -1 disables it.
    // Index tracks sequence insertions/deletions and survives renaming.
    int startupSequence() const;
    void setStartupSequence(int i);
    int position() const;          // step last played in the current sequence (-1: none yet)
    void setPosition(int step);    // without recalling (the interface recalls, undoable)
    int next() const;              // the step GO plays (-1: none — the end, without loop)
    int previous() const;          // the step GO BACK plays (-1: none)
    // GO / a given step: its pre-wait, then its snapshot, then what follows (chain = false: at once, and nothing
    // follows). GO BACK: the previous step at once, the waits still running are stopped.
    bool go();
    bool back();
    bool goTo(int step, bool chain = true);
    void stop();                   // cancel pending waits in the selected sequence only
    std::vector<Run> runs() const; // the steps of the selected sequence on their way
    bool running() const;
    void advance(double dt);       // moves the waits on (a timer does, except with Engine::setFadesManual)
    // The snapshot a step recalls goes through this (the interface: its undo stack); Engine::recallSnapshot by default
    void setRecaller(std::function<void(int snapshotIndex)> f);
    // A recall the cue clock made (its thread, on time): told afterwards on the Sequencer's thread, with the state it
    // replaced. None: the recall is made all the same, without undo
    void setRecallRecorder(std::function<void(int snapshotIndex, const RecallBefore &before)> f);
    CueTiming cueTiming() const;
    CueWorkTiming cueWorkTiming() const;
    void resetCueTiming(); // resets lateness and execution histograms together
    bool clockRunning() const; // the cue clock is ticking (waits on their way)
    // A step's action: its snapshot's, or the time its timeline will play (Play on a timeline that ends; else 0;
    // infinity: its timeline is frozen, speed 0, and plays on when its speed is raised)
    double stepDuration(const Step &st) const;

    // The project's: saved, loaded, cleared (the caller emits changed / positionChanged once it is all in)
    QJsonArray toJson() const;
    void fromJson(const QJsonArray &a, int current);
    void clear();

signals:
    void changed();         // their contents
    void positionChanged(); // current sequence or step

private:
    int sequenceIndexLocked(quint64 id) const; // under m_mutex; stable identity for queued GO
    bool goToSequence(quint64 id, int step, bool chain);
    void startTimer();
    void runClock(); // clock thread
    void tick();     // clock thread
    void fireSnapshot(int snapshotIndex);
    void noteFired(double late); // lock held

    Engine &m_engine;
    Annotated<QRecursiveMutex> &m_mutex;
    std::vector<Sequence> m_sequences;
    int m_current = -1; // UI selection only, never the owner of all active runs
    std::vector<int> m_positions; // playhead per sequence (not persisted)
    std::vector<quint64> m_sequenceIds; // runtime identity, aligned with m_sequences
    quint64 m_nextSequenceId = 1;
    int m_startupSequence = -1; // none by default; adjusted on insert/remove
    std::vector<Run> m_runs; // all active sequences; each run owns a stable sequenceId
    std::function<void(int)> m_recaller;                         // under m_mutex
    std::function<void(int, const RecallBefore &)> m_recorder;   // under m_mutex
    std::unique_ptr<QThread> m_clockThread;
    QObject *m_clockContext = nullptr; // lives in m_clockThread
    QTimer *m_timer = nullptr;         // clock thread
    QElapsedTimer m_clock;             // clock thread
    std::atomic<bool> m_ticking{false};
    CueTiming m_timing;                // under m_mutex
    CueWorkTiming m_workTiming;         // under m_mutex
};
