// Blackmagic DeckLink outputs (DeckLink.h): the ports, their modes, and the publisher that plays a viewport out.
#include "DeckLink.h"
#include "Publish.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <mutex>
#include <utility>

// ---------------------------------------------------------------------------
// BGRA to 4:2:2 Y'CbCr (also used by the tests, with or without the SDK)
// ---------------------------------------------------------------------------
void decklink::bgraToUyvy(const uint8_t *bgra, int stride, int width, int height, uint8_t *out, int outStride)
{
    // Video range: Y' 16 to 235, Cb Cr 16 to 240 around 128 (coefficients in 1/256ths, as integers)
    const bool hd = height >= 720;
    const int yr = hd ? 47 : 66, yg = hd ? 157 : 129, yb = hd ? 16 : 25;     // Y'  = (yr R + yg G + yb B) / 256 + 16
    const int ur = hd ? -26 : -38, ug = hd ? -87 : -74, ub = 112;            // Cb
    const int vr = 112, vg = hd ? -102 : -94, vb = hd ? -10 : -18;           // Cr
    auto clamp8 = [](int v) { return uint8_t(std::clamp(v, 0, 255)); };
    for (int y = 0; y < height; ++y) {
        const uint8_t *s = bgra + size_t(y) * size_t(stride);
        uint8_t *d = out + size_t(y) * size_t(outStride);
        for (int x = 0; x + 1 < width; x += 2, s += 8, d += 4) {
            const int b0 = s[0], g0 = s[1], r0 = s[2], b1 = s[4], g1 = s[5], r1 = s[6];
            const int r = r0 + r1, g = g0 + g1, b = b0 + b1; // the pair's chroma: their mean
            d[0] = clamp8(((ur * r + ug * g + ub * b + 256) >> 9) + 128);
            d[1] = clamp8(((yr * r0 + yg * g0 + yb * b0 + 128) >> 8) + 16);
            d[2] = clamp8(((vr * r + vg * g + vb * b + 256) >> 9) + 128);
            d[3] = clamp8(((yr * r1 + yg * g1 + yb * b1 + 128) >> 8) + 16);
        }
        if (width & 1) { // an odd width: the last pixel with itself
            const int b0 = s[0], g0 = s[1], r0 = s[2];
            d[0] = clamp8(((ur * r0 + ug * g0 + ub * b0 + 128) >> 8) + 128);
            d[1] = d[3] = clamp8(((yr * r0 + yg * g0 + yb * b0 + 128) >> 8) + 16);
            d[2] = clamp8(((vr * r0 + vg * g0 + vb * b0 + 128) >> 8) + 128);
        }
    }
}

const decklink::Mode *decklink::bestMode(const std::vector<Mode> &modes, QSize size, double fps)
{
    auto pick = [&](int w, int h) -> const Mode * {
        const Mode *best = nullptr;
        for (const Mode &m : modes) {
            if (m.width != w || m.height != h) continue;
            // Progressive first, then the closest rate (a whole rate before its 1000/1001 neighbour)
            auto cost = [&](const Mode &x) { return (x.interlaced ? 1000.0 : 0.0) + std::abs(x.fps - fps) + (std::abs(x.fps - std::round(x.fps)) > 0.01 ? 0.05 : 0.0); };
            if (!best || cost(m) < cost(*best)) best = &m;
        }
        return best;
    };
    if (const Mode *m = pick(size.width(), size.height())) return m;
    if (const Mode *m = pick(1920, 1080)) return m;
    return modes.empty() ? nullptr : &modes.front();
}

#ifndef FULSKRIN_HAS_DECKLINK

bool decklink::compiledIn() { return false; }
QString decklink::driverVersion() { return {}; }
std::vector<decklink::Port> decklink::outputPorts() { return {}; }
std::vector<decklink::Mode> decklink::outputModes(const QString &) { return {}; }
std::unique_ptr<CpuPublisher> createDeckLinkPublisher() { return nullptr; }

#else

// ---------------------------------------------------------------------------
// The SDK, per platform: strings, booleans, how its objects are made
// ---------------------------------------------------------------------------
#if defined(_WIN32)
#include <windows.h>
#include <oleauto.h>
#include "DeckLinkAPI_h.h"
#ifdef _MSC_VER
#pragma warning(push, 0)
#endif
#include "DeckLinkAPI_i.c"
#ifdef _MSC_VER
#pragma warning(pop)
#endif
using DlBool = BOOL;
using DlInt = LONGLONG;
using DlString = BSTR;
static QString takeString(DlString s)
{
    const QString q = s ? QString::fromWCharArray(s) : QString();
    if (s) SysFreeString(s);
    return q;
}
static void comInit() // each thread that calls the SDK
{
    thread_local bool done = false;
    if (!done) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED); // already in another mode (the interface's thread): fine
        done = true;
    }
}
static IDeckLinkIterator *newIterator()
{
    comInit();
    IDeckLinkIterator *it = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CDeckLinkIterator, nullptr, CLSCTX_ALL, IID_IDeckLinkIterator, reinterpret_cast<void **>(&it))))
        return nullptr;
    return it;
}
static IDeckLinkAPIInformation *newApiInformation()
{
    comInit();
    IDeckLinkAPIInformation *info = nullptr;
    if (FAILED(CoCreateInstance(CLSID_CDeckLinkAPIInformation, nullptr, CLSCTX_ALL, IID_IDeckLinkAPIInformation,
                                reinterpret_cast<void **>(&info))))
        return nullptr;
    return info;
}
#else
#include "DeckLinkAPI.h"
using DlBool = bool;
using DlInt = int64_t;
#if defined(__APPLE__)
using DlString = CFStringRef;
static QString takeString(DlString s)
{
    if (!s) return {};
    const QString q = QString::fromCFString(s);
    CFRelease(s);
    return q;
}
#else
using DlString = const char *;
static QString takeString(DlString s)
{
    if (!s) return {};
    const QString q = QString::fromUtf8(s);
    free(const_cast<char *>(s));
    return q;
}
#endif
static void comInit() {}
static IDeckLinkIterator *newIterator() { return CreateDeckLinkIteratorInstance(); }
static IDeckLinkAPIInformation *newApiInformation() { return CreateDeckLinkAPIInformationInstance(); }
#endif

#ifndef STDMETHODCALLTYPE
#define STDMETHODCALLTYPE
#endif

// A reference the SDK gave: released when it goes
template <class T> struct Ref {
    T *p = nullptr;
    Ref() = default;
    explicit Ref(T *x) : p(x) {}
    Ref(const Ref &) = delete;
    Ref &operator=(const Ref &) = delete;
    Ref(Ref &&o) noexcept : p(std::exchange(o.p, nullptr)) {}
    Ref &operator=(Ref &&o) noexcept
    {
        reset();
        p = std::exchange(o.p, nullptr);
        return *this;
    }
    ~Ref() { reset(); }
    void reset()
    {
        if (p) p->Release();
        p = nullptr;
    }
    T *operator->() const { return p; }
    explicit operator bool() const { return p != nullptr; }
};

template <class T> static Ref<T> query(IUnknown *from, REFIID iid)
{
    void *out = nullptr;
    if (!from || from->QueryInterface(iid, &out) != S_OK) return Ref<T>();
    return Ref<T>(static_cast<T *>(out));
}

static QString modeKey(BMDDisplayMode m)
{
    const char c[5] = {char((m >> 24) & 0xFF), char((m >> 16) & 0xFF), char((m >> 8) & 0xFF), char(m & 0xFF), 0};
    return QString::fromLatin1(c);
}

static BMDDisplayMode modeCode(const QString &key)
{
    const QByteArray b = key.toLatin1();
    if (b.size() != 4) return bmdModeUnknown;
    return BMDDisplayMode((uint32_t(uint8_t(b[0])) << 24) | (uint32_t(uint8_t(b[1])) << 16) | (uint32_t(uint8_t(b[2])) << 8) |
                          uint32_t(uint8_t(b[3])));
}

// A port's lasting identity: its persistent ID (the same after a restart), else its place, else its name
static QString portId(IDeckLink *d, const QString &name)
{
    if (auto attrs = query<IDeckLinkProfileAttributes>(d, IID_IDeckLinkProfileAttributes)) {
        DlInt v = 0;
        if (attrs->GetInt(BMDDeckLinkPersistentID, &v) == S_OK) return QStringLiteral("p%1").arg(qulonglong(v), 0, 16);
        if (attrs->GetInt(BMDDeckLinkTopologicalID, &v) == S_OK) return QStringLiteral("t%1").arg(qulonglong(v), 0, 16);
    }
    return name;
}

static bool playsOut(IDeckLink *d)
{
    auto attrs = query<IDeckLinkProfileAttributes>(d, IID_IDeckLinkProfileAttributes);
    DlInt io = 0;
    if (attrs && attrs->GetInt(BMDDeckLinkVideoIOSupport, &io) == S_OK) return (io & bmdDeviceSupportsPlayback) != 0;
    return bool(query<IDeckLinkOutput>(d, IID_IDeckLinkOutput));
}

// Every port that plays out, with its name and id (fn: true to stop there, keeping the device)
static Ref<IDeckLink> eachPort(const std::function<bool(IDeckLink *, const decklink::Port &)> &fn)
{
    Ref<IDeckLinkIterator> it(newIterator());
    if (!it) return {};
    IDeckLink *raw = nullptr;
    while (it->Next(&raw) == S_OK) {
        Ref<IDeckLink> d(raw);
        if (!playsOut(d.p)) continue;
        DlString s = nullptr;
        QString name = d->GetDisplayName(&s) == S_OK ? takeString(s) : QString();
        if (name.isEmpty()) name = QStringLiteral("DeckLink");
        if (fn(d.p, decklink::Port{portId(d.p, name), name})) return d;
    }
    return {};
}

bool decklink::compiledIn() { return true; }

QString decklink::driverVersion()
{
    Ref<IDeckLinkAPIInformation> info(newApiInformation());
    DlInt v = 0;
    if (!info || info->GetInt(BMDDeckLinkAPIVersion, &v) != S_OK) return {};
    const QString s = QStringLiteral("%1.%2").arg((v >> 24) & 0xFF).arg((v >> 16) & 0xFF);
    return (v >> 8) & 0xFF ? s + QStringLiteral(".%1").arg((v >> 8) & 0xFF) : s;
}

std::vector<decklink::Port> decklink::outputPorts()
{
    std::vector<Port> out;
    eachPort([&](IDeckLink *, const Port &p) {
        out.push_back(p);
        return false;
    });
    return out;
}

static Ref<IDeckLink> findPort(const QString &id, decklink::Port *found)
{
    return eachPort([&](IDeckLink *, const decklink::Port &p) {
        if (!id.isEmpty() && p.id != id && p.name != id) return false; // empty: the first one
        *found = p;
        return true;
    });
}

static bool supports(IDeckLinkOutput *o, BMDDisplayMode m, BMDPixelFormat f)
{
    DlBool ok = false;
    BMDDisplayMode actual = bmdModeUnknown;
    return o->DoesSupportVideoMode(bmdVideoConnectionUnspecified, m, f, bmdNoVideoOutputConversion, bmdSupportedVideoModeDefault,
                                   &actual, &ok) == S_OK &&
           ok;
}

static std::vector<decklink::Mode> modesOf(IDeckLinkOutput *o)
{
    std::vector<decklink::Mode> out;
    IDeckLinkDisplayModeIterator *rawIt = nullptr;
    if (o->GetDisplayModeIterator(&rawIt) != S_OK) return out;
    Ref<IDeckLinkDisplayModeIterator> it(rawIt);
    IDeckLinkDisplayMode *raw = nullptr;
    while (it->Next(&raw) == S_OK) {
        Ref<IDeckLinkDisplayMode> m(raw);
        const BMDDisplayMode code = m->GetDisplayMode();
        if (!supports(o, code, bmdFormat8BitBGRA) && !supports(o, code, bmdFormat8BitYUV)) continue;
        decklink::Mode x;
        x.key = modeKey(code);
        DlString s = nullptr;
        x.name = m->GetName(&s) == S_OK ? takeString(s) : x.key;
        x.width = int(m->GetWidth());
        x.height = int(m->GetHeight());
        BMDTimeValue dur = 0;
        BMDTimeScale scale = 0;
        if (m->GetFrameRate(&dur, &scale) == S_OK && dur > 0) x.fps = double(scale) / double(dur);
        const BMDFieldDominance f = m->GetFieldDominance();
        x.interlaced = f == bmdLowerFieldFirst || f == bmdUpperFieldFirst;
        out.push_back(x);
    }
    return out;
}

std::vector<decklink::Mode> decklink::outputModes(const QString &port)
{
    Port p;
    Ref<IDeckLink> d = findPort(port, &p);
    auto o = query<IDeckLinkOutput>(d.p, IID_IDeckLinkOutput);
    return o ? modesOf(o.p) : std::vector<Mode>{};
}

// ---------------------------------------------------------------------------
// Publisher: the card's clock asks for each frame (ScheduledFrameCompleted); it gets the latest picture received,
// again if none came since. So the render's rate and the output's never have to match.
// ---------------------------------------------------------------------------
namespace {

class DeckLinkPublisher;

class Callback : public IDeckLinkVideoOutputCallback
{
public:
    explicit Callback(DeckLinkPublisher *owner) : m_owner(owner) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, LPVOID *ppv) override
    {
        if (std::memcmp(&iid, &IID_IDeckLinkVideoOutputCallback, 16) == 0) {
            *ppv = static_cast<IDeckLinkVideoOutputCallback *>(this);
            AddRef();
            return S_OK;
        }
        *ppv = nullptr;
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++m_refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG n = --m_refs;
        if (n == 0) delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE ScheduledFrameCompleted(IDeckLinkVideoFrame *frame, BMDOutputFrameCompletionResult result) override;
    HRESULT STDMETHODCALLTYPE ScheduledPlaybackHasStopped() override { return S_OK; }
    void detach();

private:
    std::mutex m_mutex; // the owner goes: no call after detach()
    DeckLinkPublisher *m_owner;
    std::atomic<ULONG> m_refs{1};
};

class DeckLinkPublisher : public CpuPublisher
{
public:
    ~DeckLinkPublisher() override { stop(); }

    bool start(const QString &name, const PublishSettings &s, QString *err) override
    {
        comInit();
        m_wantMode = s.decklinkMode;
        m_device = findPort(name, &m_port);
        m_output = query<IDeckLinkOutput>(m_device.p, IID_IDeckLinkOutput);
        if (!m_output) {
            if (err) *err = name.isEmpty() ? QStringLiteral("No DeckLink output (is Desktop Video installed?)")
                                           : QStringLiteral("DeckLink output not found (unplugged?)");
            return false;
        }
        m_modes = modesOf(m_output.p);
        if (!m_wantMode.isEmpty()) { // a mode chosen: played out now (black until the first picture)
            const auto it = std::find_if(m_modes.begin(), m_modes.end(), [&](const decklink::Mode &m) { return m.key == m_wantMode; });
            if (it == m_modes.end()) {
                if (err) *err = QStringLiteral("%1 does not play this mode").arg(m_port.name);
                return false;
            }
            return play(*it, err);
        }
        return true; // automatic: the mode of the first picture's size and rate
    }

    void send(const CpuFrame &f) override
    {
        if (!m_playing && !m_failed) {
            comInit();
            const decklink::Mode *m = decklink::bestMode(m_modes, QSize(f.width, f.height), double(f.fpsN) / std::max(1, f.fpsD));
            QString err;
            if (!m || !play(*m, &err)) {
                std::lock_guard<std::mutex> lk(m_mutex);
                m_failed = true;
                m_error = m ? err : QStringLiteral("%1 has no mode to play").arg(m_port.name);
                return;
            }
        }
        std::lock_guard<std::mutex> lk(m_mutex);
        m_picture.assign(f.bgra.begin(), f.bgra.end());
        m_picW = f.width;
        m_picH = f.height;
        m_picStride = f.stride;
    }

    bool liveState(PublishState *st) const override
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (m_failed) {
            *st = {PublishState::Error, m_error, -1};
            return true;
        }
        if (!m_playing) {
            *st = {PublishState::Ok, QStringLiteral("%1: waiting for the picture").arg(m_port.name), -1};
            return true;
        }
        QString t = QStringLiteral("%1: %2 (%3)").arg(m_port.name, m_mode.name,
                                                     m_format == bmdFormat8BitBGRA ? QStringLiteral("RGB") : QStringLiteral("YUV 4:2:2"));
        if (m_picW && (m_picW != m_mode.width || m_picH != m_mode.height))
            t += QStringLiteral(" — scaled from %1 × %2").arg(m_picW).arg(m_picH);
        if (m_late) t += QStringLiteral(" — %1 late").arg(m_late);
        *st = {PublishState::Ok, t, -1};
        return true;
    }

    // The card is done with `frame`: it gets the latest picture and goes back in the queue
    void completed(IDeckLinkVideoFrame *frame, BMDOutputFrameCompletionResult result)
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_playing) return;
        if (result == bmdOutputFrameDisplayedLate || result == bmdOutputFrameDropped) {
            ++m_late;
            BMDTimeValue now = 0;
            double speed = 0;
            if (m_output->GetScheduledStreamTime(m_scale, &now, &speed) == S_OK && m_next < now + m_duration)
                m_next = (now / m_duration + kQueued) * m_duration; // behind: back ahead of the card
        }
        fill(frame);
        m_output->ScheduleVideoFrame(frame, m_next, m_duration, m_scale);
        m_next += m_duration;
    }

private:
    static constexpr int kQueued = 3; // frames ahead of what is shown (the latency, in frames)

    bool play(const decklink::Mode &mode, QString *err)
    {
        const BMDDisplayMode code = modeCode(mode.key);
        const BMDPixelFormat format = supports(m_output.p, code, bmdFormat8BitBGRA) ? bmdFormat8BitBGRA : bmdFormat8BitYUV;
        Ref<IDeckLinkDisplayMode> dm;
        {
            IDeckLinkDisplayMode *raw = nullptr;
            if (m_output->GetDisplayMode(code, &raw) != S_OK) {
                if (err) *err = QStringLiteral("%1 does not play %2").arg(m_port.name, mode.name);
                return false;
            }
            dm = Ref<IDeckLinkDisplayMode>(raw);
        }
        BMDTimeValue dur = 0;
        BMDTimeScale scale = 0;
        dm->GetFrameRate(&dur, &scale);
        if (m_output->EnableVideoOutput(code, bmdVideoOutputFlagDefault) != S_OK) {
            if (err) *err = QStringLiteral("%1 is in use (another viewport or application?)").arg(m_port.name);
            return false;
        }
        m_enabled = true;
        const int rowBytes = format == bmdFormat8BitBGRA ? mode.width * 4 : ((mode.width + 1) / 2) * 4;
        for (int i = 0; i < kQueued; ++i) {
            IDeckLinkMutableVideoFrame *raw = nullptr;
            if (m_output->CreateVideoFrame(mode.width, mode.height, rowBytes, format, bmdFrameFlagDefault, &raw) != S_OK) {
                if (err) *err = QStringLiteral("%1: no memory for its frames").arg(m_port.name);
                stop();
                return false;
            }
            m_frames.emplace_back(raw);
        }
        std::lock_guard<std::mutex> lk(m_mutex);
        m_mode = mode;
        m_format = format;
        m_rowBytes = rowBytes;
        m_duration = dur;
        m_scale = scale;
        m_callback = new Callback(this);
        m_output->SetScheduledFrameCompletionCallback(m_callback);
        m_next = 0;
        for (auto &f : m_frames) { // black, then the pictures as they come
            fill(f.p);
            m_output->ScheduleVideoFrame(f.p, m_next, m_duration, m_scale);
            m_next += m_duration;
        }
        if (m_output->StartScheduledPlayback(0, m_scale, 1.0) != S_OK) {
            m_failed = true;
            m_error = QStringLiteral("%1 did not start playing").arg(m_port.name);
            return true; // shown as an error, not retried every frame
        }
        m_playing = true;
        return true;
    }

    void stop()
    {
        if (m_callback) m_callback->detach(); // a call on its way returns without touching this
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_playing = false;
        }
        if (m_output) {
            if (m_enabled) {
                BMDTimeValue at = 0;
                m_output->StopScheduledPlayback(0, &at, 0);
                m_output->SetScheduledFrameCompletionCallback(nullptr);
                m_output->DisableVideoOutput();
            }
            m_enabled = false;
        }
        if (m_callback) m_callback->Release();
        m_callback = nullptr;
        m_frames.clear();
        m_output.reset();
        m_device.reset();
    }

    // The latest picture into a frame of the card's (lock held): at its size, else scaled (nearest); black before any
    void fill(IDeckLinkVideoFrame *frame)
    {
        void *bytes = nullptr;
        if (frame->GetBytes(&bytes) != S_OK || !bytes) return;
        uint8_t *dst = static_cast<uint8_t *>(bytes);
        const int w = m_mode.width, h = m_mode.height;
        if (m_picture.empty() || m_picW <= 0 || m_picH <= 0) {
            for (int y = 0; y < h; ++y) {
                uint8_t *row = dst + size_t(y) * size_t(m_rowBytes);
                if (m_format == bmdFormat8BitBGRA) {
                    for (int x = 0; x < w; ++x) std::memcpy(row + x * 4, "\0\0\0\xff", 4);
                } else {
                    for (int x = 0; x < m_rowBytes; x += 4) std::memcpy(row + x, "\x80\x10\x80\x10", 4);
                }
            }
            return;
        }
        const uint8_t *src = m_picture.data();
        int stride = m_picStride;
        if (m_picW != w || m_picH != h) { // another size: nearest pixel (the viewport's size should be the mode's)
            m_scaled.resize(size_t(w) * size_t(h) * 4);
            std::vector<int> xs(static_cast<size_t>(w));
            for (int x = 0; x < w; ++x) xs[size_t(x)] = std::min(m_picW - 1, int((x + 0.5) * m_picW / w));
            for (int y = 0; y < h; ++y) {
                const uint32_t *s = reinterpret_cast<const uint32_t *>(src + size_t(std::min(m_picH - 1, int((y + 0.5) * m_picH / h))) * size_t(stride));
                uint32_t *d = reinterpret_cast<uint32_t *>(m_scaled.data() + size_t(y) * size_t(w) * 4);
                for (int x = 0; x < w; ++x) d[x] = s[xs[size_t(x)]];
            }
            src = m_scaled.data();
            stride = w * 4;
        }
        if (m_format == bmdFormat8BitBGRA) {
            for (int y = 0; y < h; ++y) std::memcpy(dst + size_t(y) * size_t(m_rowBytes), src + size_t(y) * size_t(stride), size_t(w) * 4);
        } else {
            decklink::bgraToUyvy(src, stride, w, h, dst, m_rowBytes);
        }
    }

    QString m_wantMode;
    decklink::Port m_port;
    std::vector<decklink::Mode> m_modes;
    Ref<IDeckLink> m_device;
    Ref<IDeckLinkOutput> m_output;
    std::vector<Ref<IDeckLinkMutableVideoFrame>> m_frames;
    Callback *m_callback = nullptr;
    bool m_enabled = false;

    mutable std::mutex m_mutex; // the send thread, the card's thread, the states
    bool m_playing = false, m_failed = false;
    QString m_error;
    decklink::Mode m_mode;
    BMDPixelFormat m_format = bmdFormat8BitBGRA;
    int m_rowBytes = 0;
    BMDTimeValue m_duration = 1, m_next = 0;
    BMDTimeScale m_scale = 25;
    std::vector<uint8_t> m_picture, m_scaled;
    int m_picW = 0, m_picH = 0, m_picStride = 0;
    int m_late = 0;
};

HRESULT Callback::ScheduledFrameCompleted(IDeckLinkVideoFrame *frame, BMDOutputFrameCompletionResult result)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_owner) m_owner->completed(frame, result);
    return S_OK;
}

void Callback::detach()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_owner = nullptr;
}

} // namespace

std::unique_ptr<CpuPublisher> createDeckLinkPublisher() { return std::make_unique<DeckLinkPublisher>(); }

#endif // FULSKRIN_HAS_DECKLINK
