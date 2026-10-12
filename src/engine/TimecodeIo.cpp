// The inputs of a timecode clock (TimecodeIo.h):
//  - LTC: an audio input decoded (miniaudio);
//  - MTC: a MIDI input decoded (RtMidi, Midi.h);
//  - ATC: the timecode an NDI source's frames carry, or a DeckLink input's (RP188 / VITC).
// And the audio devices of the timecode, inputs and outputs (tcaudio::Device), used by the LTC outputs too.
#include "TimecodeIo.h"
#include "DeckLink.h"
#include "Midi.h"
#include "NetVideo.h"

#include <miniaudio.h>

#include <atomic>
#include <cmath>
#include <thread>

// ---------------------------------------------------------------------------
// Audio devices
// ---------------------------------------------------------------------------
struct tcaudio::Device::Impl {
    ma_context ctx{};
    ma_device dev{};
    bool ctxOk = false, devOk = false;
    QString name;
    Capture capture;
    Playback playback;

    static void callback(ma_device *dev, void *output, const void *input, ma_uint32 frames)
    {
        auto *self = static_cast<Impl *>(dev->pUserData);
        if (self->capture) self->capture(static_cast<const float *>(input), int(frames));
        if (self->playback) self->playback(static_cast<float *>(output), int(frames), int(dev->playback.channels));
    }

    bool open(ma_device_type type, const QString &want, QString *err)
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
                const bool isCapture = type == ma_device_type_capture;
                ma_device_info *list = isCapture ? cap : play;
                const ma_uint32 n = isCapture ? nCap : nPlay;
                for (ma_uint32 i = 0; i < n && !haveId; ++i)
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
        cfg.sampleRate = tcaudio::kRate;
        cfg.dataCallback = &Impl::callback;
        cfg.pUserData = this;
        if (type == ma_device_type_capture) {
            cfg.capture.format = ma_format_f32;
            cfg.capture.channels = 1; // the first channel (or a mix down)
            cfg.capture.pDeviceID = haveId ? &id : nullptr;
        } else {
            cfg.playback.format = ma_format_f32;
            cfg.playback.channels = 0; // the device's own: each LTC on its channel, none mixed into another
            cfg.playback.pDeviceID = haveId ? &id : nullptr;
            cfg.noClip = MA_TRUE;
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

    ~Impl()
    {
        if (devOk) ma_device_uninit(&dev); // waits for its callback
        if (ctxOk) ma_context_uninit(&ctx);
    }
};

tcaudio::Device::Device() : d(std::make_unique<Impl>()) {}
tcaudio::Device::~Device() = default;

bool tcaudio::Device::openCapture(const QString &name, Capture cb, QString *err)
{
    d->capture = std::move(cb);
    return d->open(ma_device_type_capture, name, err);
}

bool tcaudio::Device::openPlayback(const QString &name, Playback cb, QString *err)
{
    d->playback = std::move(cb);
    return d->open(ma_device_type_playback, name, err);
}

QString tcaudio::Device::name() const { return d->name; }
int tcaudio::Device::channels() const { return d->devOk ? int(d->dev.playback.channels) : 0; }

// ---------------------------------------------------------------------------
// Inputs
// ---------------------------------------------------------------------------
struct TimecodeIo::Impl {
    TimecodeClock *clock = nullptr;
    TimecodeSettings s;

    // LTC
    std::unique_ptr<tcaudio::Device> ltcIn;
    std::unique_ptr<ltc::Decoder> ltcDecoder;

    // MTC
    std::unique_ptr<midi::Input> mtcIn;
    mtc::Decoder mtcDecoder;

    // ATC
    std::atomic<bool> quit{false};
    std::thread ndiThread;
    std::unique_ptr<decklink::TimecodeInput> deckLinkIn;

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
            c.p_ndi_recv_name = "Pinol timecode";
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
        if (ndiThread.joinable()) ndiThread.join();
        deckLinkIn.reset();
        mtcIn.reset();
        ltcIn.reset();
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
    case Src::Ltc: {
        d->ltcDecoder = std::make_unique<ltc::Decoder>(tcaudio::kRate);
        d->ltcDecoder->setRateHint(s.rate);
        d->ltcIn = std::make_unique<tcaudio::Device>();
        Impl *impl = d.get();
        if (d->ltcIn->openCapture(s.input, [impl](const float *mono, int frames) {
                impl->ltcDecoder->push(mono, size_t(frames));
                if (std::optional<Timecode> tc = impl->ltcDecoder->take()) // read at its end: one frame on already
                    impl->clock->received(Timecode::fromFrames(tc->toFrames() + 1, tc->rate));
            }, &err)) {
            clock->setInputStatus(d->ltcIn->name());
        } else {
            clock->setInputStatus(QStringLiteral("audio input: ") + err);
            d->ltcIn.reset();
        }
        break;
    }
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
}

TimecodeIo::~TimecodeIo() = default;

QString TimecodeIo::status() const
{
    if (d->deckLinkIn) {
        const QString in = d->deckLinkIn->status();
        if (!in.isEmpty()) d->clock->setInputStatus(in); // the mode coming in, as it changes
    }
    return QString();
}
