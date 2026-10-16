#include "AudioOutput.h"
#include "AudioStream.h"

#include <miniaudio.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <thread>

struct AudioOutputPrivate {
    ma_context context{};
    ma_device device{};
    bool contextOk = false, deviceOk = false;
    QString name;

    static void callback(ma_device *dev, void *output, const void *, ma_uint32 frames)
    {
        auto *self = static_cast<AudioOutput *>(dev->pUserData);
        const uint64_t start = RealtimeTiming::nowNs();
        self->render(static_cast<float *>(output), int(frames));
        self->m_callbackTiming.record(RealtimeTiming::nowNs() - start,
                                     uint64_t(frames) * 1000000000ULL / AudioOutput::kSampleRate);
    }
};

AudioOutput::AudioOutput() : d(std::make_unique<AudioOutputPrivate>()) {}

AudioOutput::~AudioOutput() { stop(); }

QStringList AudioOutput::deviceNames()
{
    QStringList names;
    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return names;
    ma_device_info *playback = nullptr, *capture = nullptr;
    ma_uint32 nPlayback = 0, nCapture = 0;
    if (ma_context_get_devices(&ctx, &playback, &nPlayback, &capture, &nCapture) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < nPlayback; ++i) {
            const QString n = QString::fromUtf8(playback[i].name);
            if (playback[i].isDefault) names.prepend(n);
            else names.append(n);
        }
    }
    ma_context_uninit(&ctx);
    names.removeDuplicates();
    return names;
}

QStringList AudioOutput::captureDeviceNames()
{
    QStringList names;
    ma_context ctx;
    if (ma_context_init(nullptr, 0, nullptr, &ctx) != MA_SUCCESS) return names;
    ma_device_info *playback = nullptr, *capture = nullptr;
    ma_uint32 nPlayback = 0, nCapture = 0;
    if (ma_context_get_devices(&ctx, &playback, &nPlayback, &capture, &nCapture) == MA_SUCCESS) {
        for (ma_uint32 i = 0; i < nCapture; ++i) {
            const QString n = QString::fromUtf8(capture[i].name);
            if (capture[i].isDefault) names.prepend(n);
            else names.append(n);
        }
    }
    ma_context_uninit(&ctx);
    names.removeDuplicates();
    return names;
}

bool AudioOutput::start(const QString &device, QString *err, bool nullDevice)
{
    stop();
    ma_backend nullBackend[] = {ma_backend_null};
    if (ma_context_init(nullDevice ? nullBackend : nullptr, nullDevice ? 1 : 0, nullptr, &d->context) != MA_SUCCESS) {
        if (err) *err = QStringLiteral("No audio system available.");
        return false;
    }
    d->contextOk = true;

    ma_device_id id{};
    bool haveId = false;
    if (!device.isEmpty() && !nullDevice) {
        ma_device_info *playback = nullptr, *capture = nullptr;
        ma_uint32 nPlayback = 0, nCapture = 0;
        if (ma_context_get_devices(&d->context, &playback, &nPlayback, &capture, &nCapture) == MA_SUCCESS) {
            for (ma_uint32 i = 0; i < nPlayback; ++i)
                if (QString::fromUtf8(playback[i].name) == device) {
                    id = playback[i].id;
                    haveId = true;
                    break;
                }
        }
    }

    ma_device_config cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 2;
    cfg.playback.pDeviceID = haveId ? &id : nullptr;
    cfg.sampleRate = kSampleRate;
    cfg.dataCallback = &AudioOutputPrivate::callback;
    cfg.pUserData = this;
    cfg.performanceProfile = ma_performance_profile_low_latency;
    if (ma_device_init(&d->context, &cfg, &d->device) != MA_SUCCESS) {
        if (err) *err = QStringLiteral("Unable to open the audio output") + (device.isEmpty() ? QString() : " \"" + device + "\"") + ".";
        stop();
        return false;
    }
    d->deviceOk = true;
    d->name = QString::fromUtf8(d->device.playback.name);
    if (ma_device_start(&d->device) != MA_SUCCESS) {
        if (err) *err = QStringLiteral("Unable to start the audio output \"%1\".").arg(d->name);
        stop();
        return false;
    }
    return true;
}

void AudioOutput::stop()
{
    if (d->deviceOk) ma_device_uninit(&d->device); // waits for the callback to return
    if (d->contextOk) ma_context_uninit(&d->context);
    d->deviceOk = d->contextOk = false;
    d->name.clear();
    m_peak[0] = m_peak[1] = 0.0f;
    m_analysis.reset();
}

bool AudioOutput::isRunning() const { return d->deviceOk && ma_device_is_started(&d->device); }

QString AudioOutput::deviceName() const { return d->name; }

double AudioOutput::latency() const
{
    if (!d->deviceOk) return 0;
    const auto &p = d->device.playback;
    const double rate = p.internalSampleRate > 0 ? p.internalSampleRate : kSampleRate;
    return double(p.internalPeriodSizeInFrames) * std::max<ma_uint32>(1, p.internalPeriods) / rate;
}

void AudioOutput::publish(std::shared_ptr<const Mix> next)
{
    const std::shared_ptr<const Mix> old = std::move(m_owned);
    m_owned = std::move(next);
    m_mix.store(m_owned.get());
    // A callback that may still read the old list: wait for it to end (one period at most)
    const uint64_t e = m_epoch.load();
    if (e & 1)
        while (m_epoch.load() == e) std::this_thread::yield();
}

void AudioOutput::addStream(const std::shared_ptr<AudioStream> &s)
{
    if (!s) return;
    std::lock_guard<std::mutex> lk(m_streamsMutex);
    if (std::find(m_owned->streams.begin(), m_owned->streams.end(), s) != m_owned->streams.end()) return;
    auto next = std::make_shared<Mix>(*m_owned);
    next->streams.push_back(s);
    publish(std::move(next));
}

void AudioOutput::removeStream(const AudioStream *s)
{
    // Once this returns, the callback no longer uses the stream
    std::lock_guard<std::mutex> lk(m_streamsMutex);
    auto next = std::make_shared<Mix>(*m_owned);
    next->streams.erase(std::remove_if(next->streams.begin(), next->streams.end(), [s](const auto &p) { return p.get() == s; }),
                        next->streams.end());
    publish(std::move(next));
}

void AudioOutput::setTap(std::function<void(const float *, int)> fn)
{
    std::lock_guard<std::mutex> lk(m_streamsMutex);
    auto next = std::make_shared<Mix>(*m_owned);
    next->tap = std::move(fn);
    publish(std::move(next));
}

void AudioOutput::fadeTo(float target, double seconds)
{
    target = std::clamp(target, 0.0f, 1.0f);
    m_fadeStep = seconds > 0.0 ? float(1.0 / (seconds * kSampleRate)) : 1.0f;
    m_fadeTarget = target;
    if (!isRunning()) m_fadeLevel = target; // no callback to ramp it
}

void AudioOutput::render(float *out, int frames)
{
    std::memset(out, 0, size_t(frames) * 2 * sizeof(float));
    m_epoch.fetch_add(1); // odd: mixing (a writer waits for this callback before freeing the list it reads)
    const Mix *mix = m_mix.load();
    const double lat = latency();
    for (const auto &s : mix->streams) s->mix(out, frames, lat);
    m_analysis.process(out, frames, kSampleRate);

    const float goal = std::max(0.0f, m_volume.load());
    const float ramp = 1.0f / (0.01f * kSampleRate);
    const float fadeGoal = m_fadeTarget.load(), fadeStep = m_fadeStep.load();
    float fade = m_fadeLevel.load();
    float pk[2] = {0, 0};
    for (int i = 0; i < frames; ++i) {
        m_gain = m_gain < goal ? std::min(goal, m_gain + ramp) : std::max(goal, m_gain - ramp);
        fade = fade < fadeGoal ? std::min(fadeGoal, fade + fadeStep) : std::max(fadeGoal, fade - fadeStep);
        const float g = m_gain * fade * fade; // squared: a fade that sounds even
        for (int c = 0; c < 2; ++c) {
            float &v = out[i * 2 + c];
            v = std::clamp(v * g, -1.0f, 1.0f);
            pk[c] = std::max(pk[c], std::abs(v));
        }
    }
    m_fadeLevel = fade;
    for (int c = 0; c < 2; ++c) m_peak[c] = std::max(pk[c], m_peak[c].load() * 0.9f);
    if (mix->tap) mix->tap(out, frames);
    m_epoch.fetch_add(1);
}
