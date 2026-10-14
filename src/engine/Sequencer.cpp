// Sequences: ordered steps recalling snapshots, played with GO / GO BACK (the cue list of a show).
#include "Sequencer.h"
#include "EngineInternal.h"

#include <iterator>
#include <limits>

#include <QJsonArray>
#include <QThread>
#include <cmath>
#include <QTimer>

Sequencer::Sequencer(Engine &engine, Annotated<QRecursiveMutex> &mutex) : QObject(&engine), m_engine(engine), m_mutex(mutex) {}

Sequencer::~Sequencer() { shutdownClock(); }

void Sequencer::shutdownClock()
{
    if (!m_clockThread) return;
    if (m_clockContext) {
        // Its timer and context go in their own thread, which then stops: nothing fires any more
        QObject *ctx = m_clockContext;
        m_clockContext = nullptr;
        QMetaObject::invokeMethod(ctx, [this, ctx] {
            m_timer = nullptr;
            delete ctx; // the timer with it
        }, Qt::BlockingQueuedConnection);
    }
    m_clockThread->quit();
    m_clockThread->wait();
    m_clockThread.reset();
    m_ticking = false;
}

void Sequencer::setRecaller(std::function<void(int snapshotIndex)> f)
{
    QMutexLocker lk(&m_mutex);
    m_recaller = std::move(f);
}

void Sequencer::setRecallRecorder(std::function<void(int, const RecallBefore &)> f)
{
    QMutexLocker lk(&m_mutex);
    m_recorder = std::move(f);
}

double Sequencer::CueTiming::percentile(double p) const
{
    if (!fired) return 0;
    static constexpr double edges[] = {0.0005, 0.001, 0.002, 0.004, 0.008, 0.016, 0.032, 0.064, 0.128, 0.256, 0.512};
    const double want = std::ceil(p * double(fired));
    quint64 seen = 0;
    for (size_t k = 0; k < buckets.size(); ++k) {
        seen += buckets[k];
        if (double(seen) >= want) return k < std::size(edges) ? std::min(edges[k], max) : max;
    }
    return max;
}

void Sequencer::noteFired(double late)
{
    late = std::max(0.0, late);
    static constexpr double edges[] = {0.0005, 0.001, 0.002, 0.004, 0.008, 0.016, 0.032, 0.064, 0.128, 0.256, 0.512};
    size_t k = 0;
    while (k < std::size(edges) && late > edges[k]) ++k;
    ++m_timing.buckets[k];
    ++m_timing.fired;
    m_timing.last = late;
    m_timing.total += late;
    m_timing.max = std::max(m_timing.max, late);
}

Sequencer::CueTiming Sequencer::cueTiming() const
{
    QMutexLocker lk(&m_mutex);
    return m_timing;
}

Sequencer::CueWorkTiming Sequencer::cueWorkTiming() const
{
    QMutexLocker lk(&m_mutex);
    return m_workTiming;
}

void Sequencer::resetCueTiming()
{
    QMutexLocker lk(&m_mutex);
    m_timing = CueTiming();
    m_workTiming = CueWorkTiming();
}

bool Sequencer::clockRunning() const { return m_ticking.load(); }

int Sequencer::count() const
{
    QMutexLocker lk(&m_mutex);
    return int(m_sequences.size());
}

Sequencer::Sequence Sequencer::sequence(int i) const
{
    QMutexLocker lk(&m_mutex);
    return i >= 0 && i < int(m_sequences.size()) ? m_sequences[size_t(i)] : Sequence();
}

void Sequencer::setSequence(int i, const Sequence &s)
{
    {
        QMutexLocker lk(&m_mutex);
        if (i < 0 || i >= int(m_sequences.size())) return;
        m_sequences[size_t(i)] = s;
        if (m_positions[size_t(i)] >= int(s.steps.size())) m_positions[size_t(i)] = int(s.steps.size()) - 1;
        const quint64 owner = m_sequenceIds[size_t(i)];
        m_runs.erase(std::remove_if(m_runs.begin(), m_runs.end(), [&](const Run &r) {
            return r.sequenceId == owner && r.step >= int(s.steps.size());
        }), m_runs.end());
    }
    emit changed();
}

int Sequencer::add(const Sequence &s, int at)
{
    {
        QMutexLocker lk(&m_mutex);
        if (at < 0 || at > int(m_sequences.size())) at = int(m_sequences.size());
        m_sequences.insert(m_sequences.begin() + at, s);
        m_sequenceIds.insert(m_sequenceIds.begin() + at, m_nextSequenceId++);
        m_positions.insert(m_positions.begin() + at, -1);
        if (m_startupSequence >= at) ++m_startupSequence;
        if (m_current >= at) ++m_current;
        if (m_current < 0) m_current = at;
    }
    emit changed();
    emit positionChanged();
    return at;
}

void Sequencer::remove(int i)
{
    {
        QMutexLocker lk(&m_mutex);
        if (i < 0 || i >= int(m_sequences.size())) return;
        const quint64 owner = m_sequenceIds[size_t(i)];
        m_runs.erase(std::remove_if(m_runs.begin(), m_runs.end(),
            [owner](const Run &r) { return r.sequenceId == owner; }), m_runs.end());
        m_sequences.erase(m_sequences.begin() + i);
        m_positions.erase(m_positions.begin() + i);
        m_sequenceIds.erase(m_sequenceIds.begin() + i);
        if (m_startupSequence == i) m_startupSequence = -1;
        else if (m_startupSequence > i) --m_startupSequence;
        if (m_current == i) {
            m_current = m_sequences.empty() ? -1 : std::min(i, int(m_sequences.size()) - 1);
        } else if (m_current > i) {
            --m_current;
        }
    }
    emit changed();
    emit positionChanged();
}

int Sequencer::current() const
{
    QMutexLocker lk(&m_mutex);
    return m_current;
}

void Sequencer::setCurrent(int i)
{
    {
        QMutexLocker lk(&m_mutex);
        if (i < -1 || i >= int(m_sequences.size()) || i == m_current) return;
        m_current = i; // UI selection never interrupts background cues
    }
    emit positionChanged();
}

int Sequencer::startupSequence() const
{
    QMutexLocker lk(&m_mutex);
    return m_startupSequence;
}

void Sequencer::setStartupSequence(int i)
{
    {
        QMutexLocker lk(&m_mutex);
        if (i < -1 || i >= int(m_sequences.size()) || m_startupSequence == i) return;
        m_startupSequence = i;
    }
    emit changed();
}

int Sequencer::position() const
{
    QMutexLocker lk(&m_mutex);
    return m_current >= 0 ? m_positions[size_t(m_current)] : -1;
}

void Sequencer::setPosition(int step)
{
    {
        QMutexLocker lk(&m_mutex);
        if (m_current < 0) return;
        const int n = int(m_sequences[size_t(m_current)].steps.size());
        m_positions[size_t(m_current)] = std::clamp(step, -1, n - 1);
    }
    emit positionChanged();
}

int Sequencer::next() const
{
    QMutexLocker lk(&m_mutex);
    if (m_current < 0) return -1;
    const Sequence &s = m_sequences[size_t(m_current)];
    const int n = int(s.steps.size());
    if (n == 0) return -1;
    if (m_positions[size_t(m_current)] + 1 < n) return m_positions[size_t(m_current)] + 1;
    return s.loop ? 0 : -1;
}

int Sequencer::previous() const
{
    QMutexLocker lk(&m_mutex);
    if (m_current < 0) return -1;
    const Sequence &s = m_sequences[size_t(m_current)];
    const int n = int(s.steps.size());
    if (n == 0 || m_positions[size_t(m_current)] < 0) return -1;
    if (m_positions[size_t(m_current)] > 0) return m_positions[size_t(m_current)] - 1;
    return s.loop ? n - 1 : -1;
}

// ---------------------------------------------------------------------------
// Playing: pre-wait, action, post-wait, and what follows

// A step's run, from its GO
static Sequencer::Run runOf(const Sequencer::Step &st, int k, bool chain, quint64 owner)
{
    Sequencer::Run r;
    r.step = k;
    r.sequenceId = owner;
    r.target = st;
    r.snapshot = st.timeline ? 0 : st.snapshot;
    r.preWait = chain ? std::max(0.0, st.preWait) : 0.0;
    r.postWait = std::max(0.0, st.postWait);
    r.next = chain ? st.next : Sequencer::StepContinue::Wait;
    return r;
}

int Sequencer::sequenceIndexLocked(quint64 id) const
{
    const auto found = std::find(m_sequenceIds.begin(), m_sequenceIds.end(), id);
    return found == m_sequenceIds.end() ? -1 : int(found - m_sequenceIds.begin());
}

bool Sequencer::goTo(int step, bool chain)
{
    quint64 owner = 0;
    {
        QMutexLocker lk(&m_mutex);
        if (m_current < 0) return false;
        owner = m_sequenceIds[size_t(m_current)];
    }
    return goToSequence(owner, step, chain);
}

bool Sequencer::goToSequence(quint64 owner, int step, bool chain)
{
    if (QThread::currentThread() != thread()) {
        // Bind delayed OSC commands to their original sequence, even if selection changes.
        bool ok = false;
        {
            QMutexLocker lk(&m_mutex);
            const int index = sequenceIndexLocked(owner);
            ok = index >= 0 && step >= 0 && step < int(m_sequences[size_t(index)].steps.size());
        }
        if (ok) QMetaObject::invokeMethod(this, [this, owner, step, chain] {
            goToSequence(owner, step, chain);
        }, Qt::QueuedConnection);
        return ok;
    }
    {
        QMutexLocker lk(&m_mutex);
        const int index = sequenceIndexLocked(owner);
        if (index < 0) return false; // removed before a queued GO arrived
        const Sequence &s = m_sequences[size_t(index)];
        if (step < 0 || step >= int(s.steps.size())) return false;
        const Step &st = s.steps[size_t(step)];
        if (!chain) {
            m_runs.erase(std::remove_if(m_runs.begin(), m_runs.end(),
                [owner](const Run &r) { return r.sequenceId == owner; }), m_runs.end());
        }
        Run r = runOf(st, step, chain, owner);
        r.duration = stepDuration(st);
        m_runs.push_back(r);
        m_positions[size_t(index)] = step;
    }
    advance(0);
    emit positionChanged();
    startTimer();
    return true;
}

bool Sequencer::go() { return goTo(next()); }
bool Sequencer::back() { return goTo(previous(), false); }

void Sequencer::stop()
{
    QMutexLocker lk(&m_mutex);
    if (m_current < 0) return;
    const quint64 owner = m_sequenceIds[size_t(m_current)];
    m_runs.erase(std::remove_if(m_runs.begin(), m_runs.end(),
        [owner](const Run &r) { return r.sequenceId == owner; }), m_runs.end());
}

std::vector<Sequencer::Run> Sequencer::runs() const
{
    QMutexLocker lk(&m_mutex);
    if (m_current < 0) return {};
    const quint64 owner = m_sequenceIds[size_t(m_current)];
    std::vector<Run> shown;
    for (const Run &r : m_runs)
        if (r.sequenceId == owner) shown.push_back(r);
    return shown;
}

bool Sequencer::running() const
{
    QMutexLocker lk(&m_mutex);
    if (m_current < 0) return false;
    const quint64 owner = m_sequenceIds[size_t(m_current)];
    for (const Run &r : m_runs)
        if (r.sequenceId == owner &&
            (!r.fired || (r.next != StepContinue::Wait && !r.continued))) return true;
    return false;
}

bool Sequencer::active(int index) const
{
    QMutexLocker lk(&m_mutex);
    if (index < 0 || index >= int(m_sequenceIds.size())) return false;
    const quint64 owner = m_sequenceIds[size_t(index)];
    return std::any_of(m_runs.begin(), m_runs.end(),
                       [owner](const Run &r) { return r.sequenceId == owner; });
}

void Sequencer::startTimer()
{
    if (m_engine.fadesManual()) return;
    {
        QMutexLocker lk(&m_mutex);
        if (m_runs.empty()) return;
    }
    if (!m_clockThread) {
        m_clockThread = std::make_unique<QThread>();
        m_clockThread->setObjectName(QStringLiteral("Pinol cue clock"));
        m_clockContext = new QObject;
        m_clockContext->moveToThread(m_clockThread.get());
        m_clockThread->start(QThread::TimeCriticalPriority);
    }
    if (m_clockContext) QMetaObject::invokeMethod(m_clockContext, [this] { runClock(); }, Qt::QueuedConnection);
}

void Sequencer::runClock()
{
    if (!m_timer) {
        m_timer = new QTimer(m_clockContext);
        m_timer->setTimerType(Qt::PreciseTimer);
        m_timer->setInterval(2); // a wait ends within ~2 ms of its time, whatever the interface does
        connect(m_timer, &QTimer::timeout, m_clockContext, [this] { tick(); });
    }
    if (!m_timer->isActive()) {
        m_clock.restart(); // the time since the GO is already in the runs: the clock counts from now
        m_ticking = true;
        m_timer->start();
    }
}

void Sequencer::tick()
{
    // The time that really went by (a monotonic clock): a tick that came late is caught up at once, the waits do not
    // drift with the timer's own lateness
    const double dt = m_clock.nsecsElapsed() * 1e-9 * timeScale();
    m_clock.restart();
    advance(dt);
    bool empty;
    {
        QMutexLocker lk(&m_mutex);
        empty = m_runs.empty();
    }
    if (empty && m_timer) {
        m_timer->stop();
        m_ticking = false;
    }
}

void Sequencer::fireSnapshot(int mi)
{
    std::function<void(int)> recaller;
    std::function<void(int, const RecallBefore &)> recorder;
    {
        QMutexLocker lk(&m_mutex);
        recaller = m_recaller;
        recorder = m_recorder;
    }
    if (QThread::currentThread() == thread()) { // a GO, on the Sequencer's own thread: through the interface
        if (recaller) recaller(mi);
        else m_engine.recallSnapshot(mi);
        return;
    }
    // The cue clock: record how long it takes to *actually* execute a
    // snapshot. CueTiming::noteFired() only measures timer lateness, which
    // can be tiny even when Undo capture or scene application is slow.
    const qint64 start = FrameTiming::nowNs();
    const auto recordWork = [this, start](qint64 captured, qint64 applied) {
        QMutexLocker lk(&m_mutex);
        m_workTiming.capture.add(double(captured - start) / 1e6);
        m_workTiming.recall.add(double(applied - captured) / 1e6);
        m_workTiming.total.add(double(applied - start) / 1e6);
    };
    if (!recorder) {
        m_engine.recallSnapshot(mi);
        const qint64 done = FrameTiming::nowNs();
        recordWork(start, done); // no UI Undo recording, so capture time is zero
        return;
    }
    RecallBefore before{m_engine.captureRecallLayers(mi), m_engine.captureComposition(), m_engine.captureInteractions()};
    const qint64 captured = FrameTiming::nowNs();
    m_engine.recallSnapshot(mi);
    recordWork(captured, FrameTiming::nowNs());
    QMetaObject::invokeMethod(this, [this, mi, before] {
        std::function<void(int, const RecallBefore &)> f;
        {
            QMutexLocker lk(&m_mutex);
            f = m_recorder;
        }
        if (f) f(mi, before);
    }, Qt::QueuedConnection);
}

// Each step on its way moves on by dt: its snapshot once its pre-wait is over, the next step's GO when its
// continuation says so (the time beyond it carried over, so that chained waits do not drift). A step is
// forgotten once its action is over and nothing more comes from it.
void Sequencer::advance(double dt)
{
    dt = std::max(0.0, dt);
    bool moved = false;
    for (int round = 0; round < 256; ++round) { // steps without any wait follow each other in the same call
        std::vector<Step> fire;
        std::vector<Run> started;
        {
            QMutexLocker lk(&m_mutex);
            for (Run &r : m_runs) {
                // Active runs refer to stable identities rather than the UI selection.
                const int index = sequenceIndexLocked(r.sequenceId);
                if (index < 0) continue; // deleted during an earlier recall
                const Sequence &s = m_sequences[size_t(index)];
                const int n = int(s.steps.size());
                r.elapsed += dt;
                if (!r.fired && r.elapsed >= r.preWait) {
                    r.fired = true;
                    noteFired(r.elapsed - r.preWait); // how late the clock let it fire
                    if (r.target.timeline) r.duration = stepDuration(r.target); // where the timeline is now
                    r.trackPending = r.target.timeline && r.target.action == AnimAction::Play;
                    fire.push_back(r.target);
                } else if (r.tracking) {
                    // The timeline plays on its own clock: its speed or loop mode may have changed since
                    r.duration = std::max(0.0, r.elapsed - r.preWait) + m_engine.animationRemaining(r.target.timeline);
                }
                const double at = r.continueAt();
                if (!r.fired || r.continued || at < 0 || r.elapsed < at) continue;
                r.continued = true;
                const int k = r.step + 1 < n ? r.step + 1 : s.loop ? 0 : -1;
                if (k < 0) continue;
                const Step &st = s.steps[size_t(k)];
                Run x = runOf(st, k, true, r.sequenceId);
                x.elapsed = r.elapsed - at;
                x.duration = stepDuration(st);
                started.push_back(x);
            }
            m_runs.erase(std::remove_if(m_runs.begin(), m_runs.end(),
                                        [](const Run &r) {
                                            return r.fired && (r.next == StepContinue::Wait || r.continued) &&
                                                   r.elapsed >= r.preWait + r.duration;
                                        }),
                         m_runs.end());
        }
        // The snapshots and timelines, in the order of their GO (outside the lock: a recall loads, and may go
        // through the interface's undo stack)
        if (!fire.empty()) m_engine.noteInput(FrameTiming::Cue);
        for (const Step &st : fire) {
            if (st.timeline) {
                m_engine.controlAnimation(st.timeline, st.action, st.action == AnimAction::Speed ? st.speed : st.seekTime, st.loop, st.repeat);
                continue;
            }
            const int mi = m_engine.indexOfSnapshot(st.snapshot);
            if (mi < 0) continue; // a step without its snapshot (gone): the position moves, nothing else
            fireSnapshot(mi);
        }
        {
            QMutexLocker lk(&m_mutex); // the Plays are issued: their steps follow the timelines from now on
            for (Run &r : m_runs)
                if (r.trackPending) r.trackPending = false, r.tracking = true;
        }
        if (started.empty()) break;
        {
            QMutexLocker lk(&m_mutex);
            for (Run &x : started) {
                const int index = sequenceIndexLocked(x.sequenceId);
                if (index < 0) continue; // sequence removed while an action fired
                if (x.step >= int(m_sequences[size_t(index)].steps.size())) continue;
                m_runs.push_back(x);
                m_positions[size_t(index)] = x.step;
                moved = true;
            }
        }
        dt = 0; // the next round: only what the new steps carry over
    }
    if (moved) emit positionChanged();
}

QJsonArray Sequencer::toJson() const
{
    QJsonArray out;
    for (const Sequence &s : m_sequences) {
        QJsonArray steps;
        for (const Step &st : s.steps) {
            QJsonObject o;
            if (st.timeline) {
                static const char *const actions[] = {"play", "pause", "stop", "rewind", "seek", "loop_mode", "speed"};
                o["timeline"] = QString::number(st.timeline);
                o["action"] = QString::fromLatin1(actions[std::clamp(int(st.action), 0, 6)]);
                if (st.action == AnimAction::Seek) o["time"] = st.seekTime;
                if (st.action == AnimAction::Speed) o["speed"] = st.speed;
                if (st.action == AnimAction::LoopMode) {
                    o["loop"] = animLoopKey(st.loop);
                    if (st.repeat > 0) o["repeat"] = st.repeat;
                }
            } else {
                o["snapshot"] = QString::number(st.snapshot);
            }
            if (!st.text.isEmpty()) o["text"] = st.text;
            if (st.preWait > 0) o["pre_wait"] = st.preWait;
            if (st.postWait > 0) o["post_wait"] = st.postWait;
            if (st.next != StepContinue::Wait) o["continue"] = st.next == StepContinue::Follow ? "follow" : "auto_follow";
            steps.append(o);
        }
        out.append(QJsonObject{{"name", s.name}, {"loop", s.loop}, {"steps", steps}});
    }
    return out;
}

void Sequencer::fromJson(const QJsonArray &a, int current)
{
    m_sequences.clear();
    for (const QJsonValue &v : a) {
        const QJsonObject o = v.toObject();
        Sequence s;
        s.name = o.value("name").toString();
        s.loop = o.value("loop").toBool(false);
        for (const QJsonValue &sv : o.value("steps").toArray()) {
            const QJsonObject so = sv.toObject();
            Step st;
            st.snapshot = so.value("snapshot").toString().toULongLong();
            st.timeline = so.value("timeline").toString().toULongLong();
            if (st.timeline) {
                const QString a = so.value("action").toString();
                st.action = a == "pause" ? AnimAction::Pause : a == "stop" ? AnimAction::Stop : a == "rewind" ? AnimAction::Rewind
                          : a == "seek" ? AnimAction::Seek : a == "loop_mode" ? AnimAction::LoopMode
                          : a == "speed" ? AnimAction::Speed : AnimAction::Play;
                st.speed = std::clamp(so.value("speed").toDouble(1), 0.0, 10.0);
                st.seekTime = std::max(0.0, so.value("time").toDouble(0));
                st.loop = animLoopFromKey(so.value("loop").toString());
                st.repeat = std::clamp(so.value("repeat").toInt(0), 0, 100000);
                st.snapshot = 0;
            }
            st.text = so.value("text").toString();
            st.preWait = std::clamp(so.value("pre_wait").toDouble(0), 0.0, 3600.0);
            st.postWait = std::clamp(so.value("post_wait").toDouble(0), 0.0, 3600.0);
            const QString c = so.value("continue").toString();
            st.next = c == "follow" ? StepContinue::Follow : c == "auto_follow" ? StepContinue::AutoFollow : StepContinue::Wait;
            s.steps.push_back(st);
        }
        m_sequences.push_back(s);
    }
    m_positions.assign(m_sequences.size(), -1);
    m_sequenceIds.clear();
    for (size_t i = 0; i < m_sequences.size(); ++i) m_sequenceIds.push_back(m_nextSequenceId++);
    m_current = m_sequences.empty() ? -1 : std::clamp(current, 0, int(m_sequences.size()) - 1);
    m_startupSequence = -1; // chosen by the project after sequences are parsed
    m_runs.clear();
}

void Sequencer::clear()
{
    QMutexLocker lk(&m_mutex);
    m_sequences.clear();
    m_positions.clear();
    m_sequenceIds.clear();
    m_startupSequence = -1;
    m_current = -1;
    m_runs.clear();
}

double Sequencer::stepDuration(const Step &st) const
{
    if (!st.timeline) return m_engine.snapshotDuration(st.snapshot);
    return st.action == AnimAction::Play ? m_engine.animationPlayTime(st.timeline) : 0;
}
