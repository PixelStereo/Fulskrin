#include "SequenceFeedback.h"
#include "Engine.h"

#include <QHostInfo>
#include <QUdpSocket>
#include <cmath>

SequenceFeedback::SequenceFeedback(Engine *engine, QObject *parent) : QObject(parent), m_e(engine)
{
    m_socket = new QUdpSocket(this);
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, &SequenceFeedback::tick);
    // At each cue and each edit, at once (queued: the cue clock may say so from its own thread)
    connect(&m_e->sequencer(), &Sequencer::positionChanged, this, [this] { sendNow(false); }, Qt::QueuedConnection);
    connect(&m_e->sequencer(), &Sequencer::changed, this, [this] { sendNow(false); }, Qt::QueuedConnection);
    connect(m_e, &Engine::snapshotsChanged, this, [this] { sendNow(false); });
    connect(m_e, &Engine::animationsChanged, this, [this] { sendNow(false); });
}

bool SequenceFeedback::parseTarget(const QString &target, QString *host, quint16 *port)
{
    const QString t = target.trimmed();
    const int colon = t.lastIndexOf(QLatin1Char(':'));
    if (colon <= 0) return false;
    bool ok = false;
    const int p = t.mid(colon + 1).toInt(&ok);
    if (!ok || p < 1 || p > 65535) return false;
    QString h = t.left(colon).trimmed();
    if (h.startsWith(QLatin1Char('[')) && h.endsWith(QLatin1Char(']'))) h = h.mid(1, h.size() - 2); // [IPv6]:port
    if (h.isEmpty()) return false;
    if (host) *host = h;
    if (port) *port = quint16(p);
    return true;
}

void SequenceFeedback::setTargets(const QStringList &targets)
{
    m_targets.clear();
    for (const QString &t : targets) {
        Target target;
        if (!parseTarget(t, &target.host, &target.port)) continue;
        if (target.host.compare(QLatin1String("localhost"), Qt::CaseInsensitive) == 0)
            target.address = QHostAddress(QHostAddress::LocalHost);
        else
            target.address = QHostAddress(target.host);
        m_targets.push_back(target);
        if (target.address.isNull()) {
            // A name: resolved in the background, sent to once it is known
            const QString host = target.host;
            QHostInfo::lookupHost(host, this, [this, host](const QHostInfo &info) {
                for (Target &t : m_targets)
                    if (t.host == host && !info.addresses().isEmpty()) t.address = info.addresses().constFirst();
                sendNow(true);
            });
        }
    }
    sendNow(true);
}

void SequenceFeedback::setEnabled(bool on)
{
    if (on == m_enabled) return;
    m_enabled = on;
    if (on) {
        m_timer.start();
        sendNow(true);
    } else {
        m_timer.stop();
    }
}

void SequenceFeedback::setModified(bool on)
{
    if (on == m_modified) return;
    m_modified = on;
    sendNow(false);
}

void SequenceFeedback::tick()
{
    // Everything again every 2 seconds: an interface started late gets the whole state
    sendNow(++m_ticks % 20 == 0);
}

static QString clockText(double t)
{
    if (!std::isfinite(t)) return QStringLiteral("∞");
    t = std::max(0.0, t);
    if (t < 60) return QString::number(t, 'f', 1);
    const int m = int(t / 60);
    return QStringLiteral("%1:%2").arg(m).arg(t - m * 60, 4, 'f', 1, QLatin1Char('0'));
}

static QString continueWord(Sequencer::StepContinue c)
{
    switch (c) {
    case Sequencer::StepContinue::Follow: return QStringLiteral("follow");
    case Sequencer::StepContinue::AutoFollow: return QStringLiteral("auto-follow");
    default: return QStringLiteral("wait");
    }
}

static QString actionWord(const Sequencer::Step &st)
{
    using A = Engine::AnimAction;
    switch (st.action) {
    case A::Play: return QStringLiteral("Play");
    case A::Pause: return QStringLiteral("Pause");
    case A::Stop: return QStringLiteral("Stop");
    case A::Rewind: return QStringLiteral("Rewind");
    case A::Seek: return QStringLiteral("Seek %1 s").arg(st.seekTime, 0, 'f', 2);
    case A::Speed: return QStringLiteral("Speed × %1").arg(st.speed, 0, 'f', 2);
    case A::LoopMode: return QStringLiteral("Loop mode");
    }
    return {};
}

std::vector<osc::Message> SequenceFeedback::state() const
{
    const Sequencer &sq = m_e->sequencer();
    const int cur = sq.current();
    const Sequencer::Sequence s = sq.sequence(cur);
    const int n = int(s.steps.size());
    const std::vector<Sequencer::Run> runs = sq.runs();
    std::vector<osc::Message> out;
    auto i = [&](const QString &a, int v) { out.push_back({a, QStringLiteral("i"), {v}}); };
    auto f = [&](const QString &a, double v) { out.push_back({a, QStringLiteral("f"), {v}}); };
    auto str = [&](const QString &a, const QString &v) { out.push_back({a, QStringLiteral("s"), {v}}); };

    str(QStringLiteral("/sequence/name"), cur >= 0 ? s.name : QString());
    i(QStringLiteral("/sequence/index"), cur + 1);
    i(QStringLiteral("/sequence/steps"), n);
    i(QStringLiteral("/sequence/running"), sq.running() ? 1 : 0);
    i(QStringLiteral("/sequence/go_enabled"), sq.next() >= 0 ? 1 : 0);
    i(QStringLiteral("/sequence/back_enabled"), sq.previous() >= 0 ? 1 : 0);
    i(QStringLiteral("/sequence/modified"), m_modified ? 1 : 0);

    const int pos = sq.position();
    auto cue = [&](const QString &role, int k) {
        const QString base = QStringLiteral("/sequence/cue/") + role;
        const bool valid = k >= 0 && k < n;
        Sequencer::Step st;
        if (valid) st = s.steps[size_t(k)];
        QString name;
        if (valid && st.timeline) {
            const int a = m_e->indexOfAnimation(st.timeline);
            name = (a >= 0 ? m_e->animation(a).name : QStringLiteral("(timeline gone)")) + QStringLiteral(" · ") + actionWord(st);
        } else if (valid && st.snapshot) {
            const int m = m_e->indexOfSnapshot(st.snapshot);
            name = m >= 0 ? m_e->snapshot(m).name : QStringLiteral("(snapshot gone)");
        }
        const double duration = valid ? sq.stepDuration(st) : 0.0;
        i(base + QStringLiteral("/number"), valid ? k + 1 : 0);
        str(base + QStringLiteral("/name"), name);
        str(base + QStringLiteral("/label"), valid ? QStringLiteral("%1 · %2").arg(k + 1).arg(name) : QString());
        str(base + QStringLiteral("/text"), st.text);
        str(base + QStringLiteral("/description"), st.description);
        f(base + QStringLiteral("/prewait"), st.preWait);
        f(base + QStringLiteral("/action"), std::isfinite(duration) ? duration : -1.0);
        f(base + QStringLiteral("/postwait"), st.postWait);
        str(base + QStringLiteral("/continue"), valid ? continueWord(st.next) : QString());

        // Where its time is: its run (the latest, should it be there twice in a loop)
        const Sequencer::Run *run = nullptr;
        for (const Sequencer::Run &r : runs)
            if (valid && r.step == k) run = &r;
        QString phase;
        double elapsed = 0, end = 0, phaseProgress = 0;
        auto frac = [](double e, double len) { return len > 0 ? std::clamp(e / len, 0.0, 1.0) : 1.0; };
        if (run) {
            elapsed = run->elapsed;
            end = run->end();
            const double at = run->continueAt();
            const double postStart = at - run->postWait;
            if (!run->fired) {
                phase = QStringLiteral("pre-wait");
                phaseProgress = frac(elapsed, run->preWait);
            } else if (at >= 0 && !run->continued && elapsed >= postStart && elapsed < at) {
                phase = QStringLiteral("post-wait");
                phaseProgress = frac(elapsed - postStart, run->postWait);
            } else if (elapsed - run->preWait < run->duration) {
                phase = QStringLiteral("action");
                phaseProgress = frac(elapsed - run->preWait, run->duration);
            } else {
                phase = QStringLiteral("done");
                phaseProgress = 1;
            }
        } else if (valid && k == pos) {
            // Played and over: its time since GO goes on
            phase = QStringLiteral("done");
            elapsed = sq.stepElapsed(cur);
            end = elapsed;
            phaseProgress = 1;
        }
        const double remaining = std::isfinite(end) ? std::max(0.0, end - elapsed) : -1.0;
        const double progress = phase.isEmpty() ? 0.0 : std::isfinite(end) ? frac(elapsed, end) : 0.0;
        str(base + QStringLiteral("/phase"), phase);
        f(base + QStringLiteral("/elapsed"), elapsed);
        str(base + QStringLiteral("/elapsed_text"), phase.isEmpty() ? QString() : clockText(elapsed));
        f(base + QStringLiteral("/remaining"), remaining);
        str(base + QStringLiteral("/remaining_text"), phase.isEmpty() ? QString() : clockText(remaining < 0 ? INFINITY : remaining));
        f(base + QStringLiteral("/progress"), progress);
        i(base + QStringLiteral("/percent"), int(std::lround(progress * 100)));
        f(base + QStringLiteral("/phase_progress"), phaseProgress);
        i(base + QStringLiteral("/phase_percent"), int(std::lround(phaseProgress * 100)));
    };
    cue(QStringLiteral("previous"), sq.previous());
    cue(QStringLiteral("current"), pos);
    cue(QStringLiteral("next"), sq.next());
    return out;
}

void SequenceFeedback::sendNow(bool everything)
{
    if (!m_enabled || m_targets.empty()) return;
    int count = 0;
    for (const osc::Message &m : state()) {
        auto it = m_sent.find(m.address);
        if (!everything && it != m_sent.end() && it.value() == m.args) continue;
        m_sent.insert(m.address, m.args);
        const QByteArray packet = osc::encode(m);
        for (const Target &t : m_targets)
            if (!t.address.isNull()) m_socket->writeDatagram(packet, t.address, t.port);
        ++count;
    }
    if (count) emit sent(count);
}
