#pragma once
// Audio track of a file (audio file, or sound of a video file), decoded with FFmpeg on a dedicated thread,
// resampled to the output format (stereo float, output sample rate) and mixed by AudioOutput.
//
// Synchronization: the stream follows the layer's transport (position, play / pause, speed, play mode),
// which the engine updates every frame. In the audio callback, the stream extrapolates the layer position
// to the moment the samples will actually be heard (output latency), corrects small drifts by
// adjusting its playback rate very slightly (at most 0.5 %, inaudible), and resynchronizes (short fade,
// then seek) when the gap is too large: seek, speed change, render stall.

#include "Timeline.h"

#include <QString>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <thread>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct SwrContext;

class AudioStream
{
public:
    AudioStream() = default;
    ~AudioStream();
    AudioStream(const AudioStream &) = delete;
    AudioStream &operator=(const AudioStream &) = delete;

    struct Info {
        double duration = 0;
        int sampleRate = 0, channels = 0;
        QString codec;
    };
    // Audio track info without decoding it. Returns false if the file has no audio track.
    static bool probe(const QString &path, Info *info, QString *err = nullptr);

    // Opens the best audio track of the file. Returns false (with err) if there is none.
    bool open(const QString &path, int outputRate, QString *err);
    void close();
    const Info &info() const { return m_info; }
    double duration() const { return m_info.duration; }

    // Engine side (any thread), every frame: layer transport and gain (volume, mute, layer on/off).
    // `clock`: the layer's timeline clock (advances with |speed|); `timelineId` changes whenever the engine
    // repositions the layer (seek, direction or mode change): the stream then resynchronizes.
    // `stampNs`: steady_clock time at which `clock` was exact (0 = now).
    void setTransport(double clock, bool playing, double speed, const Timeline &timeline, uint64_t timelineId,
                      float gain, int64_t stampNs = 0);
    static int64_t clockNs(); // steady_clock, nanoseconds

    // Audio callback: adds `frames` stereo interleaved samples into `out`.
    // `latency` = time between this call and the moment the samples are heard (seconds).
    void mix(float *out, int frames, double latency);

    // Meters and diagnostics (any thread)
    float peak() const { return m_peak.load(); }
    double syncError() const { return m_syncError.load(); } // heard position - expected position (s)
    int resyncCount() const { return m_resyncs.load(); }

private:
    // Decoded samples handed from the decode thread to the audio callback: a ring of slots, one writer (the decode
    // thread) and one reader (the callback), no lock. A slot keeps its buffer: no allocation once warmed up.
    struct Chunk {
        double pts = 0;             // timeline clock of the first sample, seconds
        uint64_t generation = 0;    // the seek it follows (older ones are dropped by the callback)
        size_t frames = 0;
        std::vector<float> samples; // stereo interleaved, capacity kept
    };
    static constexpr size_t kSlots = 128;
    // A seek asked by the callback, read by the decode thread (a sequence lock over atomics: never waits)
    struct SeekRequest {
        std::atomic<double> target{0}, duration{0}, origin{0}, in{0}, out{-1};
        std::atomic<int> mode{Timeline::Loop}, dir{1};
        std::atomic<uint64_t> generation{0};
    };
    struct Transport {
        double clock = 0, speed = 1;
        bool playing = false;
        Timeline timeline;
        uint64_t timelineId = 0;
        float gain = 1;
        int64_t stampNs = 0;
    };

    void run();
    void doSeek(double t);
    bool decodeFrame();           // false = end of file or error
    void pushSamples(const float *s, int frames, double pts);
    void pushSilence(int frames, double pts);
    void requestSeek(double t);   // audio callback
    // Converted samples of the next decoded frame (start at local time *local). False at the end of the file.
    bool decodeRaw(const float **samples, int *frames, double *local);
    void produceBackwardWindow(); // backward leg: reversed samples of a short window
    void startLeg(const Timeline::Leg &leg, double position);
    double legClock(double position) const; // clock of a position on the forward leg being decoded

    // Decoder (decode thread)
    AVFormatContext *m_fmt = nullptr;
    AVCodecContext *m_codec = nullptr;
    AVFrame *m_frame = nullptr;
    AVPacket *m_packet = nullptr;
    SwrContext *m_swr = nullptr;
    int m_stream = -1;
    double m_timeBase = 0, m_startTime = 0;
    int m_rate = 48000;
    Info m_info;
    bool m_draining = false;
    double m_nextPts = 0; // position of the next decoded sample
    std::vector<float> m_convert;

    std::thread m_thread;
    std::atomic<bool> m_quit{false};
    std::vector<Chunk> m_ring;                       // kSlots slots
    std::atomic<size_t> m_head{0}, m_tail{0};        // chunks written (decode thread), read (callback)
    std::atomic<size_t> m_queuedFrames{0};
    SeekRequest m_seekReq;
    std::atomic<uint64_t> m_seekSeq{0};              // 2 × requests made (callback); odd: one being written
    uint64_t m_seekSeen = 0;                         // decode thread: the last request handled
    uint64_t m_generation = 0;                       // decode thread: the generation of what it pushes
    bool m_eof = false;                              // decode thread
    bool seekAsked() const { return (m_seekSeq.load(std::memory_order_acquire) & ~uint64_t(1)) != m_seekSeen; }
    bool takeSeek(double *target, Timeline *timeline, uint64_t *generation); // decode thread
    Timeline m_timeline; // decode thread
    Timeline::Leg m_leg;
    double m_trimBefore = 0; // forward leg: samples before this position are dropped (seek lands earlier)
    double m_backEnd = 0;
    std::vector<float> m_window;

    // Transport (written by the engine, read by the audio callback)
    std::mutex m_transportMutex;
    Transport m_transport;

    // Mixer state (audio callback only)
    Transport m_snapshot;
    std::vector<float> m_buf; // stereo interleaved, m_bufFrames frames
    size_t m_bufFrames = 0;
    double m_bufPos = 0;      // fractional read index in m_buf
    double m_bufPts = 0;      // monotonic time of m_buf frame 0
    uint64_t m_mixGeneration = 0; // the generation the callback keeps (its last seek)
    bool m_aligning = true;   // waiting for data at the target position (after open or seek)
    bool m_fadingForSeek = false;
    float m_gain = 0;         // current gain (ramped)
    double m_drift = 0;       // filtered gap between the layer and the stream (s)
    uint64_t m_mixTimelineId = ~0ull;
    bool m_timelineChanged = false; // the data in the buffer belongs to an old timeline: seek

    std::atomic<float> m_peak{0};
    std::atomic<double> m_syncError{0};
    std::atomic<int> m_resyncs{0};
};
