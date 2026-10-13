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

void Sequencer::resetCueTiming()
{
    QMutexLocker lk(&m_mutex);
    m_timing = CueTiming();
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
        if (i == m_current && m_position >= int(s.steps.size())) m_position = int(s.steps.size()) - 1;
        if (i == m_current)
            m_runs.erase(std::remove_if(m_runs.begin(), m_runs.end(), [&](const Run &r) { return r.step >= int(s.steps.size()); }),
                         m_runs.end());
    }
    emit changed();
}

int Sequencer::add(const Sequence &s, int at)
{
    {
        QMutexLocker lk(&m_mutex);
        if (at < 0 || at > int(m_sequences.size())) at = int(m_sequences.size());
        m_sequences.insert(m_sequences.begin() + at, s);
        if (m_current >= at) ++m_current;
        if (m_current < 0) {
            m_current = at;
            m_position = -1;
        }
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
        m_sequences.erase(m_sequences.begin() + i);
        m_runs.clear();
        if (m_current == i) {
            m_current = m_sequences.empty() ? -1 : std::min(i, int(m_sequences.size()) - 1);
            m_position = -1;
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
        m_current = i;
        m_position = -1;
        m_runs.clear(); // the waits of the other one: no step comes from them any more
    }
    emit positionChanged();
}

int Sequencer::position() const
{
    QMutexLocker lk(&m_mutex);
    return m_position;
}

void Sequencer::setPosition(int step)
{
    {
        QMutexLocker lk(&m_mutex);
        if (m_current < 0) return;
        const int n = int(m_sequences[size_t(m_current)].steps.size());
        m_position = std::clamp(step, -1, n - 1);
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
    if (m_position + 1 < n) return m_position + 1;
    return s.loop ? 0 : -1;
}

int Sequencer::previous() const
{
    QMutexLocker lk(&m_mutex);
    if (m_current < 0) return -1;
    const Sequence &s = m_sequences[size_t(m_current)];
    const int n = int(s.steps.size());
    if (n == 0 || m_position < 0) return -1;
    if (m_position > 0) return m_position - 1;
    return s.loop ? n - 1 : -1;
}

// ---------------------------------------------------------------------------
// Playing: pre-wait, action, post-wait, and what follows

// A step's run, from its GO
static Sequencer::Run runOf(const Sequencer::Step &st, int k, bool chain)
{
    Sequencer::Run r;
    r.step = k;
    r.target = st;
    r.snapshot = st.timeline ? 0 : st.snapshot;
    r.preWait = chain ? std::max(0.0, st.preWait) : 0.0;
    r.postWait = std::max(0.0, st.postWait);
    r.next = chain ? st.next : Sequencer::StepContinue::Wait;
    return r;
}

bool Sequencer::goTo(int step, bool chain)
{
    if (QThread::currentThread() != thread()) {
        // OSC: played on the engine's thread (the timer, the recaller and its undo stack live there)
        bool ok = false;
        {
            QMutexLocker lk(&m_mutex);
            ok = m_current >= 0 && step >= 0 && step < int(m_sequences[size_t(m_current)].steps.size());
        }
        if (ok) QMetaObject::invokeMethod(this, [this, step, chain] { goTo(step, chain); }, Qt::QueuedConnection);
        return ok;
    }
    {
        QMutexLocker lk(&m_mutex);
        if (m_current < 0) return false;
        const Sequence &s = m_sequences[size_t(m_current)];
        if (step < 0 || step >= int(s.steps.size())) return false;
        const Step &st = s.steps[size_t(step)];
        if (!chain) m_runs.clear();
        Run r = runOf(st, step, chain);
        r.duration = stepDuration(st); // the lock is recursive
        m_runs.push_back(r);
        m_position = step; // the playhead moves at the GO, the snapshot comes after the pre-wait
    }
    advance(0); // what has no pre-wait: now
    emit positionChanged();
    startTimer();
    return true;
}

bool Sequencer::go() { return goTo(next()); }
bool Sequencer::back() { return goTo(previous(), false); }

void Sequencer::stop()
{
    QMutexLocker lk(&m_mutex);
    m_runs.clear();
}

std::vector<Sequencer::Run> Sequencer::runs() const
{
    QMutexLocker lk(&m_mutex);
    return m_runs;
}

bool Sequencer::running() const
{
    QMutexLocker lk(&m_mutex);
    for (const Run &r : m_runs)
        if (!r.fired || (r.next != StepContinue::Wait && !r.continued)) return true;
    return false;
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
    // The cue clock: the recall now, the interface told afterwards (what it replaced, for its undo)
    if (!recorder) {
        m_engine.recallSnapshot(mi);
        return;
    }
    RecallBefore before{m_engine.captureLayers(true), m_engine.captureComposition(), m_engine.captureInteractions()};
    m_engine.recallSnapshot(mi);
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
            if (m_current < 0) {
                m_runs.clear();
                break;
            }
            const Sequence &s = m_sequences[size_t(m_current)];
            const int n = int(s.steps.size());
            for (Run &r : m_runs) {
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
                Run x = runOf(st, k, true);
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
                m_runs.push_back(x);
                m_position = x.step;
            }
        }
        moved = true;
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
    m_current = m_sequences.empty() ? -1 : std::clamp(current, 0, int(m_sequences.size()) - 1);
    m_position = -1;
    m_runs.clear();
}

void Sequencer::clear()
{
    QMutexLocker lk(&m_mutex);
    m_sequences.clear();
    m_current = m_position = -1;
    m_runs.clear();
}

double Sequencer::stepDuration(const Step &st) const
{
    if (!st.timeline) return m_engine.snapshotDuration(st.snapshot);
    return st.action == AnimAction::Play ? m_engine.animationPlayTime(st.timeline) : 0;
}
