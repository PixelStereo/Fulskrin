#include "TimecodeOutputs.h"

#include "RtMidi.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <QRegularExpression>
#include <chrono>
#include <cmath>
#ifdef _WIN32
#include <windows.h>
#include <timeapi.h>
#endif

// ---------------------------------------------------------------------------
// Model
// ---------------------------------------------------------------------------
QJsonObject MidiOutput::toJson() const
{
    QJsonObject o;
    o["id"] = QString::number(id); // ids are strings in files
    o["name"] = name;
    if (!port.isEmpty()) o["port"] = port;
    return o;
}

MidiOutput MidiOutput::fromJson(const QJsonObject &o)
{
    MidiOutput m;
    m.id = o.value("id").toString().toULongLong();
    m.name = o.value("name").toString();
    m.port = o.value("port").toString();
    return m;
}

QJsonObject TimecodeOutput::toJson() const
{
    QJsonObject o;
    o["id"] = QString::number(id);
    o["name"] = name;
    o["routes"] = QJsonArray::fromStringList(routes);
    return o;
}

TimecodeOutput TimecodeOutput::fromJson(const QJsonObject &o)
{
    TimecodeOutput t;
    t.id = o.value("id").toString().toULongLong();
    t.name = o.value("name").toString();
    for (const QJsonValue &v : o.value("routes").toArray())
        if (!v.toString().isEmpty() && !t.routes.contains(v.toString())) t.routes << v.toString();
    return t;
}

namespace tcroute {

QString midi(quint64 midiOutput) { return QStringLiteral("midi:") + QString::number(midiOutput); }

std::optional<quint64> midiId(const QString &route)
{
    if (!route.startsWith(QLatin1String("midi:"))) return std::nullopt;
    bool ok = false;
    const quint64 id = route.mid(5).toULongLong(&ok);
    if (!ok || !id) return std::nullopt;
    return id;
}

QString protocol(const QString &route) { return midiId(route) ? QStringLiteral("MTC") : QString(); }

} // namespace tcroute

// ---------------------------------------------------------------------------
// MTC
// ---------------------------------------------------------------------------
void MtcSender::step(const std::optional<TcReading> &r, std::vector<Message> &out)
{
    if (!r) { // nothing shown: silence, and a full frame first when it comes back
        m_quarter = false;
        m_held = -1;
        return;
    }
    const bool rateChanged = r->rate != m_rate;
    m_rate = r->rate;
    const int64_t frame = int64_t(std::floor(r->frames + 1e-6));
    const int64_t q = int64_t(std::floor(r->frames * 4 + 1e-6));
    auto full = [&](int64_t f) { out.push_back(mtc::fullFrame(Timecode::fromFrames(f, m_rate))); };
    if (r->running && r->forward) {
        // Starts, or jumps (a seek, a loop back, midnight, a source that skipped): receivers locate on a full frame
        if (!m_quarter || rateChanged || q < m_nextQ - 1 || q > m_nextQ + 8) {
            full(frame);
            m_quarter = true;
            m_held = -1;
            m_nextQ = q + 1; // the pieces of a sequence already begun are dropped by receivers: the next one is whole
            return;
        }
        for (; m_nextQ <= q; ++m_nextQ) {
            const Timecode seq = Timecode::fromFrames((m_nextQ >> 3) * 2, m_rate); // the frame its piece 0 starts
            out.push_back({0xF1, mtc::quarterFrame(seq, int(m_nextQ & 7))});
        }
        return;
    }
    if (m_quarter || rateChanged || frame != m_held) { // held, or backwards: where it is now
        full(frame);
        m_held = frame;
    }
    m_quarter = false;
}

// ---------------------------------------------------------------------------
// Router
// ---------------------------------------------------------------------------
static constexpr const char *kClientName = "Fulskrin";
static constexpr qint64 kRetryMs = 2000; // a port missing or refused: tried again (plugged in later)

TimecodeRouter::TimecodeRouter() { m_thread = std::thread([this] { run(); }); }

TimecodeRouter::~TimecodeRouter()
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_stop = true;
    }
    m_wake.notify_all();
    m_thread.join();
}

void TimecodeRouter::configure(std::vector<Stream> streams, std::vector<MidiOutput> midi)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_streams = std::move(streams);
        m_midi = std::move(midi);
        m_dirty = true;
    }
    m_wake.notify_all();
}

QString TimecodeRouter::midiStatus(quint64 midiOutput) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_status.find(midiOutput);
    return it == m_status.end() ? QString() : it->second;
}

void TimecodeRouter::setTestSink(Sink sink)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_sink = std::move(sink);
        m_dirty = true; // the ports are closed while it is set
    }
    m_wake.notify_all();
}

// ALSA names its ports "Client:Port 20:0": the numbers change from a boot to the next, the names do not
static QString baseName(const QString &port)
{
    static const QRegularExpression numbers(QStringLiteral("\\s+\\d+:\\d+$"));
    return QString(port).remove(numbers);
}

static void quiet(RtMidiError::Type, const std::string &, void *) {} // errors are read from the port's state

QStringList TimecodeRouter::midiPorts()
{
    QStringList ports;
    try {
        RtMidiOut out(RtMidi::UNSPECIFIED, kClientName);
        out.setErrorCallback(&quiet);
        const unsigned n = out.getPortCount();
        for (unsigned i = 0; i < n; ++i) {
            const QString name = QString::fromStdString(out.getPortName(i));
            if (!name.isEmpty() && !name.startsWith(QLatin1String(kClientName) + QLatin1Char(':'))) ports << name; // not ours
        }
    } catch (const RtMidiError &) {
        // no MIDI system (Linux without ALSA's sequencer): no ports
    }
    return ports;
}

bool TimecodeRouter::virtualPorts()
{
#if defined(__APPLE__) || defined(__linux__)
    return true;
#else
    return false;
#endif
}

QString TimecodeRouter::portKey(const MidiOutput &m)
{
    if (m.port.isEmpty()) return QString();
    if (m.port == QLatin1String(MidiOutput::kVirtualPort)) return QStringLiteral("virtual:") + m.name;
    return QStringLiteral("port:") + m.port;
}

TimecodeRouter::Port *TimecodeRouter::openPort(const MidiOutput &m, qint64 nowMs)
{
    const QString key = portKey(m);
    if (key.isEmpty()) return nullptr;
    Port &p = m_ports[key];
    if (p.out && p.out->isPortOpen()) return &p;
    if (p.triedAt >= 0 && nowMs - p.triedAt < kRetryMs) return nullptr;
    p.triedAt = nowMs;
    p.out.reset();
    try {
        auto out = std::make_unique<RtMidiOut>(RtMidi::UNSPECIFIED, kClientName);
        out->setErrorCallback(&quiet);
        if (m.port == QLatin1String(MidiOutput::kVirtualPort)) {
            if (!virtualPorts()) {
                p.error = QStringLiteral("No virtual ports on this system");
                return nullptr;
            }
            out->openVirtualPort(m.name.toStdString());
        } else {
            const unsigned n = out->getPortCount();
            int found = -1;
            for (unsigned i = 0; i < n && found < 0; ++i)
                if (QString::fromStdString(out->getPortName(i)) == m.port) found = int(i);
            for (unsigned i = 0; i < n && found < 0; ++i)
                if (baseName(QString::fromStdString(out->getPortName(i))) == baseName(m.port)) found = int(i);
            if (found < 0) {
                p.error = QStringLiteral("Port not found");
                return nullptr;
            }
            out->openPort(unsigned(found), m.name.toStdString());
        }
        if (!out->isPortOpen()) {
            p.error = QStringLiteral("Port refused");
            return nullptr;
        }
        p.out = std::move(out);
        p.error.clear();
        return &p;
    } catch (const RtMidiError &e) {
        p.error = QString::fromStdString(e.getMessage());
        return nullptr;
    }
}

void TimecodeRouter::run()
{
#ifdef _WIN32
    timeBeginPeriod(1); // sleeps of a millisecond (quarter frames come every 8 to 10 ms)
#endif
    QElapsedTimer clock;
    clock.start();
    Sink sink;
    std::vector<MtcSender::Message> msgs;
    std::map<quint64, QString> status;
    for (;;) {
        std::vector<Stream> streams;
        std::vector<MidiOutput> midi;
        bool reconfigure = false;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            const auto wakeUp = [this] { return m_stop || m_dirty; };
            // Sending: each millisecond. Otherwise: now and then, to open a port plugged in since
            m_wake.wait_for(lk, m_running.empty() ? std::chrono::milliseconds(500) : std::chrono::milliseconds(1), wakeUp);
            if (m_stop) break;
            if (m_dirty) {
                streams = m_streams;
                midi = m_midi;
                m_dirty = false;
                reconfigure = true;
            }
            sink = m_sink;
        }
        const qint64 now = clock.elapsed();
        if (reconfigure) {
            m_midiNow.clear();
            for (const MidiOutput &m : midi) m_midiNow[m.id] = m;
            // Senders kept for the destinations that stay (no full frame sent again), made for the new ones
            std::vector<Running> next;
            for (Stream &s : streams) {
                Running r;
                const Running *before = nullptr;
                for (const Running &o : m_running)
                    if (o.stream.output == s.output) before = &o;
                for (quint64 id : s.midi) {
                    if (!m_midiNow.count(id)) continue;
                    Destination d;
                    d.midi = id;
                    if (before)
                        for (const Destination &o : before->dest)
                            if (o.midi == id) d = o;
                    r.dest.push_back(d);
                }
                r.stream = std::move(s);
                if (r.stream.clock && !r.dest.empty()) next.push_back(std::move(r));
            }
            m_running = std::move(next);
            // Ports no MIDI output names any more: closed
            for (auto it = m_ports.begin(); it != m_ports.end();) {
                bool used = !sink;
                if (used) {
                    used = false;
                    for (const auto &[id, m] : m_midiNow) used = used || portKey(m) == it->first;
                }
                it = used ? std::next(it) : m_ports.erase(it);
            }
        }

        // The ports of the patch (opened even with nothing to send: the patch shows whether it works)
        std::map<quint64, Port *> open;
        std::map<quint64, QString> st;
        for (const auto &[id, m] : m_midiNow) {
            if (sink) {
                st[id] = QStringLiteral("Test");
                continue;
            }
            if (m.port.isEmpty()) {
                st[id] = QStringLiteral("No port");
                continue;
            }
            Port *p = openPort(m, now);
            if (p) open[id] = p;
            else st[id] = m_ports[portKey(m)].error;
        }

        for (Running &r : m_running) {
            const std::optional<TcReading> reading = r.stream.clock->reading();
            for (Destination &d : r.dest) {
                msgs.clear();
                d.mtc.step(reading, msgs);
                if (sink) {
                    for (const auto &msg : msgs) sink(d.midi, msg);
                    continue;
                }
                auto it = open.find(d.midi);
                if (it == open.end()) continue;
                for (const auto &msg : msgs) it->second->out->sendMessage(msg.data(), msg.size());
            }
        }
        for (const auto &[id, p] : open) {
            int n = 0;
            for (const Running &r : m_running)
                for (const Destination &d : r.dest) n += d.midi == id;
            st[id] = n ? QStringLiteral("Sending MTC") : QStringLiteral("Open");
        }
        if (st != status) {
            status = st;
            std::lock_guard<std::mutex> lk(m_mutex);
            m_status = st;
        }
    }
    m_ports.clear(); // closed on this thread, as they were opened
    m_running.clear();
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}
