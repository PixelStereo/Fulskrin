#pragma once
// Sequences: ordered steps, each recalling a snapshot or driving a timeline (and carrying a text for the operator),
// played by GO / GO BACK — the cue list of a show. Several sequences; one is current. Saved with the project; the
// position is not. Owned by the Engine (Engine::sequencer()), it goes through the Engine's public API only, and
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

#include "Anim.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QObject>
#include <QRecursiveMutex>
#include "AnnotatedMutex.h"
#include <QString>
#include <algorithm>
#include <functional>
#include <vector>

class Engine;
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

    Sequencer(Engine &engine, Annotated<QRecursiveMutex> &mutex);

    int count() const;
    Sequence sequence(int i) const;
    void setSequence(int i, const Sequence &s);
    int add(const Sequence &s, int at = -1);
    void remove(int i);
    int current() const;           // -1: none
    void setCurrent(int i);        // its position goes back to before the first step
    int position() const;          // step last played in the current sequence (-1: none yet)
    void setPosition(int step);    // without recalling (the interface recalls, undoable)
    int next() const;              // the step GO plays (-1: none — the end, without loop)
    int previous() const;          // the step GO BACK plays (-1: none)
    // GO / a given step: its pre-wait, then its snapshot, then what follows (chain = false: at once, and nothing
    // follows). GO BACK: the previous step at once, the waits still running are stopped.
    bool go();
    bool back();
    bool goTo(int step, bool chain = true);
    void stop();                   // the waits still running: no step comes from them any more
    std::vector<Run> runs() const; // the steps of the current sequence on their way
    bool running() const;
    void advance(double dt);       // moves the waits on (a timer does, except with Engine::setFadesManual)
    // The snapshot a step recalls goes through this (the interface: its undo stack); Engine::recallSnapshot by default
    void setRecaller(std::function<void(int snapshotIndex)> f) { m_recaller = std::move(f); }
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
    void startTimer();

    Engine &m_engine;
    Annotated<QRecursiveMutex> &m_mutex;
    std::vector<Sequence> m_sequences;
    int m_current = -1, m_position = -1;
    std::vector<Run> m_runs; // of the current sequence
    std::function<void(int)> m_recaller;
    QTimer *m_timer = nullptr;
    QElapsedTimer m_clock;
};
