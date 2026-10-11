// The inputs and outputs of a timecode clock (TimecodeIo.h):
//  - LTC: an audio input decoded, an audio output generated (miniaudio);
//  - MTC: a MIDI input decoded, a MIDI output sent four times a frame (RtMidi, Midi.h);
//  - ATC in: the timecode an NDI source's frames carry, or a DeckLink input's (RP188 / VITC).
// ATC out is not here: the viewports' NDI and DeckLink outputs carry the timecode of the layer that sends it.
#include "TimecodeIo.h"
#include "DeckLink.h"
#include "Midi.h"
#include "NetVideo.h"

#include <miniaudio.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <thread>

namespace {

constexpr int kRate = 48000;

// A miniaudio device by name (empty: the default one), opened and started
struct AudioDevice {
    ma_context ctx{};
    ma_device dev{};
    bool ctxOk = false, devOk = false;
    QString name;

    bool open(ma_device_type type, const QString &want, ma_device_data_proc cb, void *user, QString *err)
    {
        if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) {
            if (err) *err = QStringLiteral("no audio system");
            return false;
        }
        ctxOk = true;
        ma_device_id id{};
        bool haveId = false;
        if (!want.isEmpty()) {
            ma_device_info *play = nullptr, *cap = nullptr;
            ma_uint32 nPlay = 0, nCap = 0;
            if (ma_context_get_devices(&ctx, &play, &nPlay, &cap, &nCap) == MA_SUCCESS) {
                const bool capture = type == ma_device_type_capture;
                ma_device_info *list = capture ? cap : play;
                const ma_uint32 n = capture ? nCap : nPlay;
                for (ma_uint32 i = 0; i < n; ++i)
                    if (QString::fromUtf8(list[i].name) == want) {
                        id = list[i].id;
                        haveId = true;
                    }
            }
            if (!haveId) {
                if (err) *err = QStringLiteral("\"%1\" not found").arg(want);
                return false;
            }
        }
        ma_device_config cfg = ma_device_config_init(type);
        cfg.sampleRate = kRate;
        cfg.dataCallback = cb;
        cfg.pUserData = user;
        if (type == ma_device_type_capture) {
            cfg.capture.format = ma_format_f32;
            cfg.capture.channels = 1; // the first channel (or a mix down)
            cfg.capture.pDeviceID = haveId ? &id : nullptr;
        } else {
            cfg.playback.format = ma_format_f32;
            cfg.playback.channels = 2; // LTC on both
            cfg.playback.pDeviceID = haveId ? &id : nullptr;
        }
        if (ma_device_init(&ctx, &cfg, &dev) != MA_SUCCESS) {
            if (err) *err = QStringLiteral("cannot open %1").arg(want.isEmpty() ? QStringLiteral("the default device") : want);
            return false;
        }
        devOk = true;
        name = QString::fromUtf8(type == ma_device_type_capture ? dev.capture.name : dev.playback.name);
        if (ma_device_start(&dev) != MA_SUCCESS) {
            if (err) *err = QStringLiteral("cannot start %1").arg(name);
            return false;
        }
        return true;
    }
    ~AudioDevice()
    {
        if (devOk) ma_device_uninit(&dev); // waits for its callback
        if (ctxOk) ma_context_uninit(&ctx);
    }
};

} // namespace

struct TimecodeIo::Impl {
    TimecodeClock *clock = nullptr;
    TimecodeSettings s;
    QStringList out; // what is sent, or why not ("LTC → Built-in Output")

    // LTC
    std::unique_ptr<AudioDevice> ltcIn, ltcOut;
    std::unique_ptr<ltc::Decoder> ltcDecoder;
    std::unique_ptr<ltc::Generator> ltcGenerator;
    static void ltcInCallback(ma_device *dev, void *, const void *input, ma_uint32 frames)
    {
        auto *self = static_cast<Impl *>(dev->pUserData);
        self->ltcDecoder->push(static_cast<const float *>(input), frames);
        if (std::optional<Timecode> tc = self->ltcDecoder->take()) // read at its end: one frame on already
            self->clock->received(Timecode::fromFrames(tc->toFrames() + 1, tc->rate));
    }
    static void ltcOutCallback(ma_device *dev, void *output, const void *, ma_uint32 frames)
    {
        auto *self = static_cast<Impl *>(dev->pUserData);
        self->ltcGenerator->render(static_cast<float *>(output), int(frames), 2, [self] { return self->clock->now(); });
    }

    // MTC
    std::unique_ptr<midi::Input> mtcIn;
    mtc::Decoder mtcDecoder;
    std::unique_ptr<midi::Output> mtcOut;
    std::thread mtcThread;
    std::atomic<bool> quit{false};

    // ATC in
    std::thread ndiThread;
    std::unique_ptr<decklink::TimecodeInput> deckLinkIn;

    void startMtcOut()
    {
        mtcThread = std::thread([this] {
            mtc::Sender sender;
            auto at = std::chrono::steady_clock::now();
            while (!quit) {
                const std::optional<Timecode> now = clock->now();
                for (const auto &m : sender.tick(now)) mtcOut->send(m);
                const double fps = Timecode::realFps(now ? now->rate : s.rate);
                at += std::chrono::microseconds(int64_t(1e6 / (4 * fps)));
                const auto t = std::chrono::steady_clock::now();
                if (at < t - std::chrono::milliseconds(100)) at = t; // a long stall: from now on
                std::this_thread::sleep_until(at);
            }
        });
    }

    void startNdiIn()
    {
        ndi::Lib &lib = ndi::lib(QString());
        if (!lib.ok || !lib.canReceive()) {
            clock->setInputStatus(lib.ok ? QStringLiteral("this NDI library cannot receive") : lib.error);
            return;
        }
        if (s.input.isEmpty()) {
            clock->setInputStatus(QStringLiteral("no NDI source chosen"));
            return;
        }
        clock->setInputStatus(s.input);
        ndiThread = std::thread([this, &lib] {
            const QByteArray name = s.input.toUtf8();
            ndi::RecvCreateV3 c{};
            c.source_to_connect_to.p_ndi_name = name.constData();
            c.color_format = ndi::kRecvRGBX_RGBA;
            c.bandwidth = 0; // the lowest: only its timecode is read
            c.allow_video_fields = false;
            c.p_ndi_recv_name = "Fulskrin timecode";
            void *recv = lib.recv_create_v3(&c);
            if (!recv) {
                clock->setInputStatus(QStringLiteral("cannot receive %1").arg(s.input));
                return;
            }
            while (!quit) {
                ndi::VideoFrameV2 v{};
                if (lib.recv_capture_v2(recv, &v, nullptr, nullptr, 100) != ndi::kFrameVideo) continue;
                const int64_t t = v.timecode;
                lib.recv_free_video_v2(recv, &v);
                if (t == INT64_MAX || t < 0) continue; // none
                const double secs = std::fmod(double(t) / 1e7, 86400.0);
                clock->received(Timecode::fromSeconds(secs, s.rate));
            }
            lib.recv_destroy(recv);
        });
    }

    ~Impl()
    {
        quit = true;
        if (mtcThread.joinable()) mtcThread.join();
        if (ndiThread.joinable()) ndiThread.join();
        deckLinkIn.reset();
        mtcIn.reset();
        ltcIn.reset();
        ltcOut.reset();
    }
};

TimecodeIo::TimecodeIo(TimecodeClock *clock, const TimecodeSettings &s) : d(std::make_unique<Impl>())
{
    d->clock = clock;
    d->s = s;
    QString err;
    using Src = TimecodeSettings::Source;
    switch (s.source) {
    case Src::Internal: break;
    case Src::Ltc:
        d->ltcDecoder = std::make_unique<ltc::Decoder>(kRate);
        d->ltcDecoder->setRateHint(s.rate);
        d->ltcIn = std::make_unique<AudioDevice>();
        if (d->ltcIn->open(ma_device_type_capture, s.input, &Impl::ltcInCallback, d.get(), &err)) {
            clock->setInputStatus(d->ltcIn->name);
        } else {
            clock->setInputStatus(QStringLiteral("audio input: ") + err);
            d->ltcIn.reset();
        }
        break;
    case Src::Mtc:
        d->mtcIn = std::make_unique<midi::Input>();
        if (d->mtcIn->open(s.input, [this](const uint8_t *m, size_t n) {
                if (d->mtcDecoder.push(m, n))
                    if (std::optional<Timecode> tc = d->mtcDecoder.current()) d->clock->received(*tc);
            }, &err)) {
            clock->setInputStatus(d->mtcIn->name());
        } else {
            clock->setInputStatus(err);
            d->mtcIn.reset();
        }
        break;
    case Src::AtcNdi: d->startNdiIn(); break;
    case Src::AtcDeckLink:
        d->deckLinkIn = std::make_unique<decklink::TimecodeInput>();
        if (d->deckLinkIn->open(s.input, [clock](const Timecode &tc) { clock->received(tc); }, &err)) {
            clock->setInputStatus(d->deckLinkIn->status());
        } else {
            clock->setInputStatus(err);
            d->deckLinkIn.reset();
        }
        break;
    }
    if (s.sendLtc) {
        d->ltcGenerator = std::make_unique<ltc::Generator>(kRate);
        d->ltcOut = std::make_unique<AudioDevice>();
        if (d->ltcOut->open(ma_device_type_playback, s.ltcOutput, &Impl::ltcOutCallback, d.get(), &err)) {
            d->out << QStringLiteral("LTC → %1").arg(d->ltcOut->name);
        } else {
            d->out << QStringLiteral("LTC: ") + err;
            d->ltcOut.reset();
        }
    }
    if (s.sendMtc) {
        d->mtcOut = std::make_unique<midi::Output>();
        if (d->mtcOut->open(s.mtcOutput, &err)) {
            d->out << QStringLiteral("MTC → %1").arg(d->mtcOut->name());
            d->startMtcOut();
        } else {
            d->out << QStringLiteral("MTC: ") + err;
            d->mtcOut.reset();
        }
    }
    if (s.sendAtc) d->out << QStringLiteral("ATC → NDI and DeckLink outputs");
}

TimecodeIo::~TimecodeIo() = default;

QString TimecodeIo::status() const
{
    QString s = d->out.join(QStringLiteral(" · "));
    if (d->deckLinkIn) {
        const QString in = d->deckLinkIn->status();
        if (!in.isEmpty()) d->clock->setInputStatus(in); // the mode coming in, as it changes
    }
    return s;
}
