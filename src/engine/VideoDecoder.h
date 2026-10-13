#pragma once
// Video decoding on a dedicated thread.
// Frames are kept as the decoder gives them (YUV planes, RGB, grey) or, for HAP, as their compressed textures:
// the GPU converts them (VideoTexture). They are queued, stamped with the clock of the layer's Timeline
// (monotonic whatever the direction or the play mode).
// Backward legs (negative speed, ping-pong) are produced in short windows: seek to the keyframe, decode
// forward, deliver the frames in reverse order — bounded memory whatever the GOP length.
//
// Upload buffers: the render thread hands the decoder mapped pixel buffers (addStaging); the decoder copies
// each frame into one of them as soon as it is decoded, so the render thread has no copy of its own to make.
// Without a free one, the frame stays in the decoder's memory and is uploaded from there.
//
// Hardware decoding (VideoToolbox on macOS, D3D11VA / DXVA2 on Windows, VAAPI on Linux) when the codec allows
// it; software otherwise, or when the hardware refuses a stream. On macOS the GPU samples VideoToolbox's frames
// where they were decoded (zero copy: no upload buffer, no copy on the CPU); elsewhere they are copied back to
// memory, then uploaded.

#include "Timeline.h"
#include "VideoFrame.h"

#include <QString>
#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <map>
#include <mutex>
#include <thread>
#include <tuple>
#include <vector>

struct AVFormatContext;
struct AVCodecContext;
struct AVFrame;
struct AVPacket;
struct AVBufferRef;
struct SwsContext;

class VideoDecoder
{
public:
    VideoDecoder() = default;
    ~VideoDecoder();
    VideoDecoder(const VideoDecoder &) = delete;
    VideoDecoder &operator=(const VideoDecoder &) = delete;

    bool open(const QString &path, QString *err);

    // File info without decoding it (media bin)
    struct Info {
        int width = 0, height = 0;
        double duration = 0, fps = 0;
        QString codec;
    };
    static bool probe(const QString &path, Info *info, QString *err = nullptr);
    void close();

    int width() const { return m_width; }
    int height() const { return m_height; }
    double duration() const { return m_duration; }
    double fps() const { return m_fps; }
    // "h264", "prores", "Hap Q"…, followed by the hardware decoder once it delivers frames
    QString codecName() const;
    bool isHap() const { return m_hapTag != 0; }
    bool hardwareActive() const { return m_hwActive.load(); }
    int decodeThreads() const; // FFmpeg's threads for this video (0: HAP, decoded here)

    // Timeline followed by the next seek (mode, origin, direction). Its duration is set by the decoder.
    void setTimeline(const Timeline &t);

    // Repositions on the clock `c` of the timeline (asynchronous): frames are then stamped from there.
    void seek(double c);

    // Fetches the most recent frame whose timestamp <= t into `out`. Returns true if it is a new one.
    // What `out` held before goes back to the decoder, its upload buffer included unless the render thread
    // took that one back (out.staging < 0).
    bool fetch(double t, VideoFrame &out);
    // The same as RGBA 8 bits, bottom row first (tests and tools; converted on the CPU)
    bool fetchRgba(double t, std::vector<uint8_t> &out, int *w, int *h);

    // True when non-looping playback has reached the end and the queue is empty.
    bool finished();

    // Lightweight real-time diagnostics. Counters are lock-free so the render thread can sample them.
    // p95DecodeMs is an approximate percentile from fixed buckets (no per-frame allocations).
    struct Metrics {
        uint64_t decodedFrames = 0;
        uint64_t fetchedFrames = 0;
        uint64_t skippedFrames = 0;   // queued frames made obsolete by a later fetch
        uint64_t fetchMisses = 0;     // no frame available; caller may have to repeat the previous frame
        uint64_t lateFrames = 0;      // fetched frame lag exceeded 1.5 source-frame intervals
        uint64_t budgetHolds = 0;     // times the stream kept fewer frames ahead: decoded frames over the memory budget
        uint64_t queueHighWatermark = 0;
        double decodeTimeTotalMs = 0;
        double decodeTimeMaxMs = 0;
        double decodeTimeP95Ms = 0;   // bucket estimate, not an exact percentile
        double lastFrameLatenessMs = 0;
        double maxFrameLatenessMs = 0;
    };
    Metrics metrics() const;
    void resetMetrics();

    // --- Upload buffers (render thread)
    size_t stagingSize() const { return m_stagingSize.load(); } // bytes of one frame (0: not known yet)
    int stagingCount() const;                                   // how many buffers it can use
    void addStaging(int id, uint8_t *ptr, size_t size);         // a mapped buffer, free to be written
    std::vector<int> takeFreeStaging();                         // the free ones, given back (size change)

    // --- Settings, for the decoders opened afterwards
    static void setHardwareDecoding(bool on);
    static bool hardwareDecoding();
    // Hardware frames handed to the GPU in place (set by the render thread when it can sample them: macOS)
    static void setZeroCopy(bool on);
    static bool zeroCopy();
    // A hardware frame whose surface the GPU refused: frames of its format are copied from now on (true: newly)
    static bool refuseSurface(const AVFrame *hw);
    // Compressed texture formats the GPU samples (HAP): DXT (S3TC) and BPTC (BC7, BC6). RGTC is always there.
    static void setGpuFormats(bool s3tc, bool bptc);

    // A frame as RGBA 8 bits on the CPU (not one held in an upload buffer). False if it cannot be read.
    static bool toRgba(const VideoFrame &f, std::vector<uint8_t> &out, bool bottomUp);
    // How the GPU reads a decoded picture of this format (null: converted on the CPU instead)
    static std::shared_ptr<const VideoLayout> layoutOf(const AVFrame *f);

private:
    void run();
    void doSeek(double t);
    int decodeNext(VideoFrame &f, double skipBefore = -1e9); // 1 = frame, 2 = skipped, 0 = end, -1 = error
    int readVideoPacket(); // demux next packet from the selected video stream
    int decodeHap(VideoFrame &f, double skipBefore);
    bool deliver(AVFrame *src, VideoFrame &f, bool mapped); // decoded picture -> f (upload buffer, or kept)
    void produceBackwardWindow(); // decode thread, backward leg
    void startLeg(const Timeline::Leg &leg, double position); // decode thread
    void finishLeg();                                          // decode thread: next leg, or end
    void recycle(VideoFrame &&f);       // takes the lock
    void recycleLocked(VideoFrame &&f); // lock held
    void takePooledBytes(VideoFrame &f); // takes the lock
    bool takeStaging(size_t size, int *id, uint8_t **ptr);
    std::shared_ptr<const VideoLayout> layoutFor(const AVFrame *f);
    std::shared_ptr<const VideoLayout> surfaceLayout(const AVFrame *hw); // null: not sampled in place
    std::shared_ptr<const VideoLayout> hapLayout(int count, int f0, int f1);
    bool setupHardware(const struct AVCodec *dec);
    friend struct HwFormat;

    AVFormatContext *m_fmt = nullptr;
    AVCodecContext *m_codec = nullptr;
    AVFrame *m_frame = nullptr, *m_swFrame = nullptr;
    AVPacket *m_packet = nullptr;
    SwsContext *m_sws = nullptr;
    AVBufferRef *m_hwDevice = nullptr;
    int m_hwPixFmt = -1;
    QString m_hwName;
    std::atomic<bool> m_hwActive{false};
    int m_stream = -1;
    double m_timeBase = 0, m_startTime = 0;

    int m_width = 0, m_height = 0;
    double m_duration = 0, m_fps = 25;
    QString m_codecName;
    uint32_t m_hapTag = 0;
    std::vector<uint8_t> m_hapScratch; // HAP textures decoded on the CPU: their blocks, decompressed

    // Layouts already made (decode thread)
    struct LayoutKey {
        int format, w, h, space, range, loc;
        bool operator<(const LayoutKey &o) const
        {
            return std::tie(format, w, h, space, range, loc) < std::tie(o.format, o.w, o.h, o.space, o.range, o.loc);
        }
    };
    std::map<LayoutKey, std::shared_ptr<const VideoLayout>> m_layouts;
    std::map<const VideoLayout *, std::shared_ptr<const VideoLayout>> m_surfaceLayouts; // by the copy path's layout
    std::shared_ptr<const VideoLayout> m_rgbaLayout; // CPU conversion of the formats the GPU does not take

    std::thread m_thread;
    std::mutex m_mutex;
    std::condition_variable m_cv;
    std::deque<VideoFrame> m_queue;
    std::vector<std::vector<uint8_t>> m_bytePool;
    std::vector<size_t> m_bytePoolBudgetBytes; // reservations transferred with pooled allocations
    struct Staging {
        int id = -1;
        uint8_t *ptr = nullptr;
        size_t size = 0;
    };
    std::vector<Staging> m_freeStaging;
    std::map<int, Staging> m_stagingById; // every buffer handed over, wherever it is
    std::atomic<size_t> m_stagingSize{0};

    // Metrics use atomics because decode and render/fetch run on different threads.
    std::atomic<uint64_t> m_metricDecoded{0}, m_metricFetched{0}, m_metricSkipped{0}, m_metricMisses{0};
    std::atomic<uint64_t> m_metricLate{0}, m_metricBudgetHolds{0}, m_metricQueueHigh{0};
    std::atomic<uint64_t> m_metricDecodeNsTotal{0}, m_metricDecodeNsMax{0};
    std::atomic<uint64_t> m_metricLastLateNs{0}, m_metricMaxLateNs{0};
    std::array<std::atomic<uint64_t>, 12> m_metricDecodeBuckets{};
    void recordQueueDepth(size_t depth);
    void recordDecodeTime(uint64_t nanoseconds);
    void accountFrame(VideoFrame &frame); // counted in the process-wide budget of decoded frames

    bool m_quit = false, m_seekPending = false, m_eof = false, m_needFirst = true;
    double m_seekTarget = 0;
    uint64_t m_generation = 0;
    Timeline m_timeline, m_pendingTimeline; // pending: applied at the next seek

    // Decode thread state
    bool m_draining = false;
    double m_lastPts = 0, m_discardBefore = -1e9;
    Timeline::Leg m_leg;                // leg being produced
    std::vector<VideoFrame> m_backStack; // backward leg: frames still to deliver (reverse order)
    double m_backEnd = 0;               // backward leg: end (position) of the next window
    bool m_legDone = false;
};
