#include "TimecodeOutputs.h"

#include "Midi.h"
#include "TimecodeIo.h"

#include <QElapsedTimer>
#include <QJsonArray>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <set>
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

QJsonObject AudioChannelOutput::toJson() const
{
    QJsonObject o;
    o["id"] = QString::number(id);
    o["name"] = name;
    if (!device.isEmpty()) o["device"] = device;
    o["channel"] = channel + 1; // as shown
    return o;
}

AudioChannelOutput AudioChannelOutput::fromJson(const QJsonObject &o)
{
    AudioChannelOutput a;
    a.id = o.value("id").toString().toULongLong();
    a.name = o.value("name").toString();
    a.device = o.value("device").toString();
    a.channel = std::clamp(o.value("channel").toInt(1) - 1, 0, 63);
    return a;
}

namespace tcroute {

static const char *const kKinds[] = {"", "midi:", "audio:", "viewport:"};

QString make(Kind k, quint64 id) { return QLatin1String(kKinds[int(k)]) + QString::number(id); }

Kind kind(const QString &route)
{
    for (int k = 1; k < 4; ++k)
        if (route.startsWith(QLatin1String(kKinds[k]))) return id(route) ? Kind(k) : Kind::None;
    return Kind::None;
}

quint64 id(const QString &route)
{
    const int colon = route.indexOf(QLatin1Char(':'));
    return colon < 0 ? 0 : route.mid(colon + 1).toULongLong();
}

QString protocol(Kind k)
{
    switch (k) {
    case Kind::Midi: return QStringLiteral("MTC");
    case Kind::Audio: return QStringLiteral("LTC");
    case Kind::Viewport: return QStringLiteral("ATC");
    case Kind::None: break;
    }
    return QString();
}

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
static constexpr qint64 kRetryMs = 2000; // a port or a device missing or refused: tried again (plugged in later)

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

void TimecodeRouter::configure(std::vector<Stream> streams, std::vector<MidiOutput> midi, std::vector<AudioChannelOutput> audio)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_streams = std::move(streams);
        m_midi = std::move(midi);
        m_audio = std::move(audio);
        m_dirty = true;
    }
    m_wake.notify_all();
}

QString TimecodeRouter::midiStatus(quint64 midiOutput) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_midiStatus.find(midiOutput);
    return it == m_midiStatus.end() ? QString() : it->second;
}

QString TimecodeRouter::audioStatus(quint64 audioOutput) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_audioStatus.find(audioOutput);
    return it == m_audioStatus.end() ? QString() : it->second;
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

QString TimecodeRouter::portKey(const MidiOutput &m)
{
    if (m.port.isEmpty()) return QString();
    if (m.port == QLatin1String(MidiOutput::kVirtualPort)) return QStringLiteral("virtual:") + m.name;
    return QStringLiteral("port:") + m.port;
}

void TimecodeRouter::reconfigure(std::vector<Stream> &streams, const std::vector<MidiOutput> &midi,
                                 const std::vector<AudioChannelOutput> &audio, bool testSink)
{
    m_midiNow.clear();
    for (const MidiOutput &m : midi) m_midiNow[m.id] = m;
    m_audioNow.clear();
    for (const AudioChannelOutput &a : audio) m_audioNow[a.id] = a;

    // MTC: the senders kept for the destinations that stay (no full frame sent again), made for the new ones
    std::vector<Running> next;
    for (const Stream &s : streams) {
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
        r.stream = s;
        if (r.stream.clock && !r.dest.empty()) next.push_back(std::move(r));
    }
    m_running = std::move(next);
    if (testSink != m_testSink) { // messages go elsewhere now: a full frame first there
        m_testSink = testSink;
        for (Running &r : m_running)
            for (Destination &d : r.dest) d.mtc = MtcSender();
    }
    // MIDI ports no MIDI output names any more: closed
    for (auto it = m_ports.begin(); it != m_ports.end();) {
        bool used = false;
        for (const auto &[id, m] : m_midiNow) used = used || (!testSink && portKey(m) == it->first);
        it = used ? std::next(it) : m_ports.erase(it);
    }

    // LTC: the lanes of each device (one per channel patched to a timecode), generators kept for those that stay
    std::map<QString, std::vector<Lane>> lanes;
    std::set<QString> devices;
    for (const auto &[id, a] : m_audioNow)
        if (!a.device.isEmpty()) devices.insert(a.device);
    for (const Stream &s : streams) {
        if (!s.clock) continue;
        for (quint64 id : s.audio) {
            auto a = m_audioNow.find(id);
            if (a == m_audioNow.end() || a->second.device.isEmpty()) continue;
            Lane l;
            l.output = s.output;
            l.audio = id;
            l.channel = a->second.channel;
            l.clock = s.clock;
            auto dev = m_devices.find(a->second.device);
            if (dev != m_devices.end() && dev->second.sink) {
                std::lock_guard<std::mutex> lk(dev->second.sink->mutex);
                for (const Lane &o : dev->second.sink->lanes)
                    if (o.output == l.output && o.audio == l.audio && o.channel == l.channel) l.gen = o.gen;
            }
            if (!l.gen) l.gen = std::make_shared<ltc::Generator>(tcaudio::kRate);
            lanes[a->second.device].push_back(std::move(l));
        }
    }
    for (auto it = m_devices.begin(); it != m_devices.end();)
        it = devices.count(it->first) ? std::next(it) : m_devices.erase(it); // closed: its callback has returned
    for (const QString &name : devices) {
        AudioDev &d = m_devices[name];
        if (!d.sink) {
            d.sink = std::make_shared<AudioSink>();
            d.sink->mono.resize(8192);
        }
        std::lock_guard<std::mutex> lk(d.sink->mutex);
        d.sink->lanes = lanes[name];
    }
}

void TimecodeRouter::openPorts(qint64 now, bool testSink, std::map<quint64, QString> &status)
{
    for (const auto &[id, m] : m_midiNow) {
        if (testSink) {
            status[id] = QStringLiteral("Test");
            continue;
        }
        const QString key = portKey(m);
        if (key.isEmpty()) {
            status[id] = QStringLiteral("No port");
            continue;
        }
        Port &p = m_ports[key];
        if (!p.out && (p.triedAt < 0 || now - p.triedAt >= kRetryMs)) {
            p.triedAt = now;
            auto out = std::make_unique<midi::Output>();
            QString err;
            const bool ok = m.port == QLatin1String(MidiOutput::kVirtualPort) ? out->openVirtual(m.name, &err) : out->open(m.port, &err);
            if (ok) {
                p.out = std::move(out);
                for (Running &r : m_running) // a port opened (plugged in, made again): its receivers get a full frame first
                    for (Destination &d : r.dest) {
                        const auto dm = m_midiNow.find(d.midi);
                        if (dm != m_midiNow.end() && portKey(dm->second) == key) d.mtc = MtcSender();
                    }
            }
            p.error = ok ? QString() : err;
        }
        if (!p.out) {
            status[id] = p.error;
            continue;
        }
        int n = 0;
        for (const Running &r : m_running)
            for (const Destination &d : r.dest) n += d.midi == id;
        status[id] = n ? QStringLiteral("Sending MTC") : QStringLiteral("Open");
    }
}

void TimecodeRouter::openAudio(qint64 now, std::map<quint64, QString> &status)
{
    for (auto &[name, d] : m_devices) {
        if (d.dev || (d.triedAt >= 0 && now - d.triedAt < kRetryMs)) continue;
        d.triedAt = now;
        auto dev = std::make_unique<tcaudio::Device>();
        std::shared_ptr<AudioSink> sink = d.sink;
        QString err;
        const QString device = name == QLatin1String(AudioChannelOutput::kDefaultDevice) ? QString() : name;
        const bool ok = dev->openPlayback(device, [sink](float *out, int frames, int channels) {
            std::lock_guard<std::mutex> lk(sink->mutex); // held by the router only to change the lanes
            if (sink->mono.size() < size_t(frames)) return; // a device asking for more at once than expected: silence
            for (Lane &l : sink->lanes) {
                if (l.channel >= channels) continue;
                TimecodeClock *clock = l.clock.get();
                l.gen->render(sink->mono.data(), frames, 1, [clock] { return clock->reading(); });
                for (int i = 0; i < frames; ++i) out[size_t(i) * size_t(channels) + size_t(l.channel)] = sink->mono[size_t(i)];
            }
        }, &err);
        if (ok) d.dev = std::move(dev);
        d.error = ok ? QString() : err;
    }
    for (const auto &[id, a] : m_audioNow) {
        if (a.device.isEmpty()) {
            status[id] = QStringLiteral("No device");
            continue;
        }
        const AudioDev &d = m_devices[a.device];
        if (!d.dev) {
            status[id] = d.error;
            continue;
        }
        const int channels = d.dev->channels();
        if (a.channel >= channels) {
            status[id] = QStringLiteral("Channel %1: %2 has %3").arg(a.channel + 1).arg(d.dev->name()).arg(channels);
            continue;
        }
        bool sending = false;
        {
            std::lock_guard<std::mutex> lk(d.sink->mutex);
            for (const Lane &l : d.sink->lanes) sending = sending || l.audio == id;
        }
        status[id] = sending ? QStringLiteral("Sending LTC") : QStringLiteral("Open");
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
    std::map<quint64, QString> midiStatus, audioStatus;
    for (;;) {
        std::vector<Stream> streams;
        std::vector<MidiOutput> midi;
        std::vector<AudioChannelOutput> audio;
        bool changed = false;
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            const auto wakeUp = [this] { return m_stop || m_dirty; };
            // MTC: each millisecond. Otherwise now and then, to open a port or a device plugged in since
            m_wake.wait_for(lk, m_running.empty() ? std::chrono::milliseconds(500) : std::chrono::milliseconds(1), wakeUp);
            if (m_stop) break;
            if (m_dirty) {
                streams = m_streams;
                midi = m_midi;
                audio = m_audio;
                m_dirty = false;
                changed = true;
            }
            sink = m_sink;
        }
        const qint64 now = clock.elapsed();
        if (changed) reconfigure(streams, midi, audio, bool(sink));

        std::map<quint64, QString> ms, as;
        openPorts(now, bool(sink), ms);
        openAudio(now, as);

        for (Running &r : m_running) {
            const std::optional<TcReading> reading = r.stream.clock->reading();
            for (Destination &d : r.dest) {
                msgs.clear();
                d.mtc.step(reading, msgs);
                if (sink) {
                    for (const auto &msg : msgs) sink(d.midi, msg);
                    continue;
                }
                auto m = m_midiNow.find(d.midi);
                if (m == m_midiNow.end()) continue;
                auto p = m_ports.find(portKey(m->second));
                if (p == m_ports.end() || !p->second.out) continue;
                for (const auto &msg : msgs) p->second.out->send(msg);
            }
        }
        if (ms != midiStatus || as != audioStatus) {
            midiStatus = ms;
            audioStatus = as;
            std::lock_guard<std::mutex> lk(m_mutex);
            m_midiStatus = ms;
            m_audioStatus = as;
        }
    }
    m_ports.clear(); // closed on this thread, as they were opened
    m_devices.clear();
    m_running.clear();
#ifdef _WIN32
    timeEndPeriod(1);
#endif
}
