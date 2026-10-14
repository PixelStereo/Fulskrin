#include "VideoDecoder.h"
#include "Hap.h"
#include "VideoDecoderInternal.h"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/hwcontext.h>
#include <libavutil/imgutils.h>
#include <libavutil/pixdesc.h>
#include <libswscale/swscale.h>
}

#include <QFileInfo>
#include <QStringList>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <set>

static constexpr size_t kMaxQueue = 6;
static constexpr size_t kMinQueue = 2; // frames a stream keeps ahead whatever the memory used by the others
static constexpr size_t kStagingBudget = size_t(128) << 20; // upload buffers of one stream, at most
static constexpr size_t kBytePool = 8;

// Decoded frames kept in memory, across every decoder: a soft limit (see VideoDecoderInternal.h)
// PINOL_FRAME_BUDGET_MB overrides the 512 MiB (measurements)
static size_t defaultDecodedLimit()
{
    const int mb = qEnvironmentVariableIntValue("PINOL_FRAME_BUDGET_MB");
    return mb > 0 ? size_t(mb) << 20 : size_t(512) << 20;
}
static std::atomic<size_t> s_decodedLimit{defaultDecodedLimit()};
static std::atomic<size_t> s_decodedBytes{0};

void decodedBudget::add(size_t bytes)
{
    if (bytes) s_decodedBytes.fetch_add(bytes, std::memory_order_relaxed);
}
void decodedBudget::remove(size_t bytes)
{
    if (bytes) s_decodedBytes.fetch_sub(bytes, std::memory_order_relaxed);
}
bool decodedBudget::over() { return s_decodedBytes.load(std::memory_order_relaxed) > s_decodedLimit.load(std::memory_order_relaxed); }
void decodedBudget::setLimit(size_t bytes) { s_decodedLimit = bytes; }
size_t decodedBudget::used() { return s_decodedBytes.load(std::memory_order_relaxed); }

// Byte buffers kept for reuse, across every decoder
static constexpr size_t kBytePoolBudget = size_t(64) << 20;
static std::atomic<size_t> s_bytePoolBytes{0};

bool bytePoolBudget::reserve(size_t bytes)
{
    size_t used = s_bytePoolBytes.load(std::memory_order_relaxed);
    for (;;) {
        if (bytes > kBytePoolBudget || used > kBytePoolBudget - bytes) return false;
        if (s_bytePoolBytes.compare_exchange_weak(used, used + bytes, std::memory_order_relaxed)) return true;
    }
}
void bytePoolBudget::release(size_t bytes)
{
    if (bytes) s_bytePoolBytes.fetch_sub(bytes, std::memory_order_relaxed);
}

// FFmpeg's threads of a software decoder: the same for every one, whatever the order they were opened in (half the
// cores, 2 to 16: one heavy stream keeps most of the machine, several share it evenly). PINOL_DECODE_THREADS
// overrides it (measurements).
//
// A process-wide budget bounds them all (audit P1): twice the cores by default (PINOL_DECODE_THREAD_BUDGET,
// setSoftwareThreadBudget). Up to it, nothing changes (four heavy streams on any machine). Beyond it — 8, 16 software
// streams — a new decoder gets an even share of the budget among the software decoders open, at least 2, instead of
// half the cores: the CPU is not given several times more runnable threads than it has cores, and each FFmpeg frame
// thread is a set of frames in memory less. The decoders opened earlier keep theirs (FFmpeg's threads are set at open).
static std::atomic<int> s_softwareThreadsInUse{0}, s_softwareDecodersOpen{0}, s_threadBudgetOverride{0};

static int softwareThreadBudget()
{
    if (const int o = s_threadBudgetOverride.load(std::memory_order_relaxed); o > 0) return o;
    static const int env = qEnvironmentVariableIntValue("PINOL_DECODE_THREAD_BUDGET");
    if (env > 0) return env;
    return 2 * int(std::max(1u, std::thread::hardware_concurrency()));
}

static int softwareThreads()
{
    static const int forced = qEnvironmentVariableIntValue("PINOL_DECODE_THREADS");
    if (forced > 0) return std::min(forced, 64);
    const int cores = int(std::max(1u, std::thread::hardware_concurrency()));
    const int base = std::min(std::max(1, cores), std::clamp(cores / 2, 2, 16));
    const int budget = softwareThreadBudget();
    const int inUse = s_softwareThreadsInUse.load(std::memory_order_relaxed);
    if (inUse + base <= budget) return base;
    const int open = s_softwareDecodersOpen.load(std::memory_order_relaxed);
    return std::min(base, std::max(2, budget / (open + 1)));
}

int VideoDecoder::softwareThreadsInUse() { return s_softwareThreadsInUse.load(std::memory_order_relaxed); }
int VideoDecoder::softwareDecodersOpen() { return s_softwareDecodersOpen.load(std::memory_order_relaxed); }
int VideoDecoder::softwareThreadBudgetNow() { return softwareThreadBudget(); }
void VideoDecoder::setSoftwareThreadBudget(int threads) { s_threadBudgetOverride = std::max(0, threads); }
size_t VideoDecoder::decodedBytesInUse() { return decodedBudget::used(); }
size_t VideoDecoder::decodedBytesLimit() { return s_decodedLimit.load(std::memory_order_relaxed); }

static std::atomic<bool> s_hardware{true}, s_s3tc{true}, s_bptc{false}, s_zeroCopy{false};

void VideoDecoder::setHardwareDecoding(bool on) { s_hardware = on; }
bool VideoDecoder::hardwareDecoding() { return s_hardware; }
void VideoDecoder::setZeroCopy(bool on) { s_zeroCopy = on; }
bool VideoDecoder::zeroCopy() { return s_zeroCopy; }

// Surface formats the GPU refused to sample in place (copied instead)
static std::mutex s_refusedMutex;
static std::set<int> s_refusedSurfaces;

static int surfaceFormat(const AVFrame *hw)
{
    if (!hw || !hw->hw_frames_ctx) return -1;
    return int(reinterpret_cast<const AVHWFramesContext *>(hw->hw_frames_ctx->data)->sw_format);
}

bool VideoDecoder::refuseSurface(const AVFrame *hw)
{
    const int fmt = surfaceFormat(hw);
    std::lock_guard<std::mutex> lk(s_refusedMutex);
    if (fmt >= 0) return s_refusedSurfaces.insert(fmt).second;
    const bool was = s_zeroCopy.exchange(false);
    return was;
}
void VideoDecoder::setGpuFormats(bool s3tc, bool bptc)
{
    s_s3tc = s3tc;
    s_bptc = bptc;
}

// ---------------------------------------------------------------------------
// VideoFrame
// ---------------------------------------------------------------------------
VideoFrame::~VideoFrame() { releaseAv(); }

VideoFrame::VideoFrame(VideoFrame &&o) noexcept
    : pts(o.pts), layout(std::move(o.layout)), staging(o.staging), bytes(std::move(o.bytes)), av(o.av), budgetBytes(o.budgetBytes)
{
    o.staging = -1;
    o.av = nullptr;
    o.bytes = {};
    o.budgetBytes = 0;
}

VideoFrame &VideoFrame::operator=(VideoFrame &&o) noexcept
{
    if (this != &o) {
        releaseAv();
        pts = o.pts;
        layout = std::move(o.layout);
        staging = o.staging;
        bytes = std::move(o.bytes);
        av = o.av;
        budgetBytes = o.budgetBytes;
        o.staging = -1;
        o.av = nullptr;
        o.bytes = {};
        o.budgetBytes = 0;
    }
    return *this;
}

void VideoFrame::releaseAv()
{
    if (av) av_frame_free(&av);
    decodedBudget::remove(budgetBytes);
    budgetBytes = 0;
}

// ---------------------------------------------------------------------------
// Opening, closing
// ---------------------------------------------------------------------------
VideoDecoder::~VideoDecoder() { close(); }

static QString avErr(int code)
{
    char buf[256] = {0};
    av_strerror(code, buf, sizeof buf);
    return QString::fromUtf8(buf);
}

// get_format: the hardware's format when offered, the first software one otherwise (the hardware refused)
struct HwFormat {
    static AVPixelFormat get(AVCodecContext *ctx, const AVPixelFormat *fmts)
    {
        const auto *self = static_cast<const VideoDecoder *>(ctx->opaque);
        for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p)
            if (int(*p) == self->m_hwPixFmt) return *p;
        for (const AVPixelFormat *p = fmts; *p != AV_PIX_FMT_NONE; ++p) {
            const AVPixFmtDescriptor *d = av_pix_fmt_desc_get(*p);
            if (d && !(d->flags & AV_PIX_FMT_FLAG_HWACCEL)) return *p;
        }
        return fmts[0];
    }
};

bool VideoDecoder::setupHardware(const AVCodec *dec)
{
    static const AVHWDeviceType kTypes[] = {
#if defined(__APPLE__)
        AV_HWDEVICE_TYPE_VIDEOTOOLBOX,
#elif defined(_WIN32)
        AV_HWDEVICE_TYPE_D3D11VA, AV_HWDEVICE_TYPE_DXVA2,
#else
        AV_HWDEVICE_TYPE_VAAPI,
#endif
    };
    // A device type that could not be opened once is not tried again (no hardware, no driver)
    static std::mutex failedMutex;
    static std::set<int> failed;
    for (AVHWDeviceType type : kTypes) {
        {
            std::lock_guard<std::mutex> lk(failedMutex);
            if (failed.count(int(type))) continue;
        }
        for (int i = 0;; ++i) {
            const AVCodecHWConfig *cfg = avcodec_get_hw_config(dec, i);
            if (!cfg) break;
            if (!(cfg->methods & AV_CODEC_HW_CONFIG_METHOD_HW_DEVICE_CTX) || cfg->device_type != type) continue;
            AVBufferRef *dev = nullptr;
            if (av_hwdevice_ctx_create(&dev, type, nullptr, nullptr, 0) < 0) {
                std::lock_guard<std::mutex> lk(failedMutex);
                failed.insert(int(type));
                break;
            }
            m_hwDevice = dev;
            m_codec->hw_device_ctx = av_buffer_ref(dev);
            m_hwPixFmt = int(cfg->pix_fmt);
            m_hwName = QString::fromUtf8(av_hwdevice_get_type_name(type));
            m_codec->opaque = this;
            m_codec->get_format = &HwFormat::get;
            return true;
        }
    }
    return false;
}

// These are *declared source tags*. Unknown values must stay unknown; a
// 10-bit file with missing metadata must not be mislabelled Rec.709/sRGB.
static void readSourceColor(const AVCodecParameters *par, VideoDecoder::Info *out)
{
    if (!par || !out) return;
    const AVPixelFormat fmt = AVPixelFormat(par->format);
    if (const char *name = av_get_pix_fmt_name(fmt)) out->pixelFormat = QString::fromLatin1(name);
    if (const AVPixFmtDescriptor *desc = av_pix_fmt_desc_get(fmt)) {
        for (int i = 0; i < desc->nb_components; ++i)
            out->bitDepth = std::max(out->bitDepth, int(desc->comp[i].depth));
    }
    if (!out->bitDepth && par->bits_per_raw_sample > 0)
        out->bitDepth = par->bits_per_raw_sample;
    if (par->color_space != AVCOL_SPC_UNSPECIFIED)
        if (const char *n = av_color_space_name(par->color_space)) out->matrix = QString::fromLatin1(n);
    if (par->color_primaries != AVCOL_PRI_UNSPECIFIED)
        if (const char *n = av_color_primaries_name(par->color_primaries)) out->primaries = QString::fromLatin1(n);
    if (par->color_trc != AVCOL_TRC_UNSPECIFIED)
        if (const char *n = av_color_transfer_name(par->color_trc)) out->transfer = QString::fromLatin1(n);
    if (par->color_range == AVCOL_RANGE_MPEG) out->range = QStringLiteral("limited (TV)");
    else if (par->color_range == AVCOL_RANGE_JPEG) out->range = QStringLiteral("full (PC)");
}

QString VideoDecoder::Info::colorDescription() const
{
    QStringList fields;
    if (bitDepth > 0) fields << QStringLiteral("%1-bit source").arg(bitDepth);
    if (!pixelFormat.isEmpty()) fields << QStringLiteral("pixel format: ") + pixelFormat;
    fields << QStringLiteral("matrix: ") + (matrix.isEmpty() ? QStringLiteral("unspecified") : matrix);
    fields << QStringLiteral("primaries: ") + (primaries.isEmpty() ? QStringLiteral("unspecified") : primaries);
    fields << QStringLiteral("transfer: ") + (transfer.isEmpty() ? QStringLiteral("unspecified") : transfer);
    fields << QStringLiteral("range: ") + (range.isEmpty() ? QStringLiteral("unspecified") : range);
    return fields.join(QStringLiteral(" · "));
}

bool VideoDecoder::open(const QString &path, QString *err)
{
    close();
    QByteArray p = QFileInfo(path).absoluteFilePath().toUtf8();
    int r = avformat_open_input(&m_fmt, p.constData(), nullptr, nullptr);
    if (r < 0) {
        if (err) *err = QStringLiteral("Unable to open video: ") + avErr(r);
        m_fmt = nullptr;
        return false;
    }
    avformat_find_stream_info(m_fmt, nullptr);
    m_stream = av_find_best_stream(m_fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (m_stream < 0) {
        if (err) *err = QStringLiteral("No decodable video stream.");
        close();
        return false;
    }
    // Only the picture is read here (the sound has its own reader)
    for (unsigned i = 0; i < m_fmt->nb_streams; ++i)
        if (int(i) != m_stream) m_fmt->streams[i]->discard = AVDISCARD_ALL;
    AVStream *st = m_fmt->streams[m_stream];
    const AVCodecParameters *par = st->codecpar;

    if (hap::isHapTag(par->codec_tag) || par->codec_id == AV_CODEC_ID_HAP) {
        // HAP: the packets are read here, the GPU samples their textures
        m_hapTag = hap::isHapTag(par->codec_tag) ? par->codec_tag : MKTAG('H', 'a', 'p', '1');
        m_codecName = hap::variantName(m_hapTag);
        m_width = par->width;
        m_height = par->height;
        if (m_width <= 0 || m_height <= 0) {
            if (err) *err = QStringLiteral("HAP video without a size.");
            close();
            return false;
        }
    } else {
        const AVCodec *dec = avcodec_find_decoder(par->codec_id);
        if (!dec) {
            const AVCodecDescriptor *d = avcodec_descriptor_get(par->codec_id);
            if (err) *err = QStringLiteral("No decoder for this video (%1).").arg(d ? QString::fromUtf8(d->name) : QStringLiteral("unknown codec"));
            close();
            return false;
        }
        m_codec = avcodec_alloc_context3(dec);
        avcodec_parameters_to_context(m_codec, par);
        m_codec->pkt_timebase = st->time_base;
        const bool hw = s_hardware && setupHardware(dec);
        // Hardware: a few threads feed it. Software: an even share of the cores (see softwareThreads)
        m_codec->thread_count = hw ? 4 : softwareThreads();
        m_codec->thread_type = FF_THREAD_FRAME | FF_THREAD_SLICE;
        r = avcodec_open2(m_codec, dec, nullptr);
        if (r < 0) {
            if (err) *err = QStringLiteral("Decoder unavailable: ") + avErr(r);
            close();
            return false;
        }
        if (!hw) { // counted in the software budget until it closes
            m_softwareThreads = std::max(1, m_codec->thread_count);
            s_softwareThreadsInUse += m_softwareThreads;
            ++s_softwareDecodersOpen;
        }
        m_codecName = QString::fromUtf8(dec->name);
        m_width = m_codec->width;
        m_height = m_codec->height;
    }

    m_timeBase = av_q2d(st->time_base);
    m_startTime = st->start_time != AV_NOPTS_VALUE ? st->start_time * m_timeBase : 0.0;
    AVRational fr = av_guess_frame_rate(m_fmt, st, nullptr);
    m_fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 25.0;
    m_sourceInfo = {};
    m_sourceInfo.width = m_width;
    m_sourceInfo.height = m_height;
    m_sourceInfo.fps = m_fps;
    m_sourceInfo.codec = m_codecName;
    readSourceColor(par, &m_sourceInfo);
    if (m_fmt->duration > 0) m_duration = double(m_fmt->duration) / AV_TIME_BASE;
    else if (st->duration > 0) m_duration = st->duration * m_timeBase;
    else m_duration = 0;
    m_sourceInfo.duration = m_duration;

    m_frame = av_frame_alloc();
    m_swFrame = av_frame_alloc();
    m_packet = av_packet_alloc();

    m_quit = false;
    m_eof = false;
    m_seekPending = false;
    m_needFirst = true;
    m_draining = false;
    m_lastPts = 0;
    m_discardBefore = -1e9;
    m_backStack.clear();
    m_timeline.duration = m_pendingTimeline.duration = m_duration;
    m_leg = m_timeline.firstLeg();
    m_legDone = false;
    m_thread = std::thread(&VideoDecoder::run, this);
    return true;
}

bool VideoDecoder::probe(const QString &path, Info *info, QString *err)
{
    AVFormatContext *fmt = nullptr;
    QByteArray p = QFileInfo(path).absoluteFilePath().toUtf8();
    int r = avformat_open_input(&fmt, p.constData(), nullptr, nullptr);
    if (r < 0) {
        if (err) *err = avErr(r);
        return false;
    }
    avformat_find_stream_info(fmt, nullptr);
    const int s = av_find_best_stream(fmt, AVMEDIA_TYPE_VIDEO, -1, -1, nullptr, 0);
    if (s < 0) {
        avformat_close_input(&fmt);
        if (err) *err = QStringLiteral("No video stream");
        return false;
    }
    AVStream *st = fmt->streams[s];
    if (info) {
        info->width = st->codecpar->width;
        info->height = st->codecpar->height;
        const AVCodecDescriptor *d = avcodec_descriptor_get(st->codecpar->codec_id);
        info->codec = hap::isHapTag(st->codecpar->codec_tag) ? hap::variantName(st->codecpar->codec_tag)
                      : d ? QString::fromUtf8(d->name) : QString();
        readSourceColor(st->codecpar, info);
        AVRational fr = av_guess_frame_rate(fmt, st, nullptr);
        info->fps = (fr.num > 0 && fr.den > 0) ? av_q2d(fr) : 0.0;
        if (fmt->duration > 0) info->duration = double(fmt->duration) / AV_TIME_BASE;
        else if (st->duration > 0) info->duration = st->duration * av_q2d(st->time_base);
    }
    avformat_close_input(&fmt);
    return true;
}

void VideoDecoder::close()
{
    if (m_thread.joinable()) {
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            m_quit = true;
        }
        m_cv.notify_all();
        m_thread.join();
    }
    // Upload buffers belong to the render thread: they are only forgotten here
    m_queue.clear();
    m_backStack.clear();
    for (size_t reserved : m_bytePoolBudgetBytes) {
        bytePoolBudget::release(reserved);
        decodedBudget::remove(reserved);
    }
    m_bytePool.clear();
    m_bytePoolBudgetBytes.clear();
    m_freeStaging.clear();
    m_stagingById.clear();
    m_stagingSize = 0;
    m_layouts.clear();
    m_rgbaLayout.reset();
    m_hapScratch = {};
    if (m_sws) sws_freeContext(m_sws);
    m_sws = nullptr;
    if (m_frame) av_frame_free(&m_frame);
    if (m_swFrame) av_frame_free(&m_swFrame);
    if (m_packet) av_packet_free(&m_packet);
    if (m_codec) avcodec_free_context(&m_codec);
    if (m_softwareThreads) {
        s_softwareThreadsInUse -= m_softwareThreads;
        --s_softwareDecodersOpen;
        m_softwareThreads = 0;
    }
    if (m_hwDevice) av_buffer_unref(&m_hwDevice);
    if (m_fmt) avformat_close_input(&m_fmt);
    m_hwPixFmt = -1;
    m_hwName.clear();
    m_hwActive = false;
    m_hapTag = 0;
    m_stream = -1;
    m_width = m_height = 0;
    m_sourceInfo = {};
}

int VideoDecoder::decodeThreads() const { return m_codec ? m_codec->thread_count : 0; }

QString VideoDecoder::codecName() const
{
    return m_hwActive && !m_hwName.isEmpty() ? QStringLiteral("%1 (%2)").arg(m_codecName, m_hwName) : m_codecName;
}

void VideoDecoder::setTimeline(const Timeline &t)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_pendingTimeline = t;
    m_pendingTimeline.duration = m_duration;
}

// ---------------------------------------------------------------------------
// Frames: delivery, recycling, upload buffers
// ---------------------------------------------------------------------------
bool VideoDecoder::fetch(double t, VideoFrame &out)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_queue.empty()) {
        ++m_metricMisses;
        return false;
    }
    int pick = -1;
    for (size_t i = 0; i < m_queue.size(); ++i) {
        if (m_queue[i].pts <= t + 1e-4) pick = int(i);
        else break;
    }
    if (pick < 0) {
        if (!m_needFirst) {
            ++m_metricMisses;
            return false;
        }
        pick = 0;
    }
    if (pick > 0) m_metricSkipped.fetch_add(uint64_t(pick), std::memory_order_relaxed);
    for (int i = 0; i < pick; ++i) {
        recycleLocked(std::move(m_queue.front()));
        m_queue.pop_front();
    }
    recycleLocked(std::move(out));
    out = std::move(m_queue.front());
    m_queue.pop_front();
    m_needFirst = false;
    ++m_metricFetched;
    const double latenessMs = std::max(0.0, t - out.pts) * 1000.0;
    const uint64_t latenessNs = uint64_t(latenessMs * 1000000.0);
    m_metricLastLateNs.store(latenessNs, std::memory_order_relaxed);
    uint64_t previous = m_metricMaxLateNs.load(std::memory_order_relaxed);
    while (previous < latenessNs && !m_metricMaxLateNs.compare_exchange_weak(previous, latenessNs, std::memory_order_relaxed)) {}
    if (m_fps > 0 && latenessMs > 1500.0 / m_fps) ++m_metricLate;
    m_cv.notify_all();
    return true;
}

bool VideoDecoder::finished()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_eof && m_queue.empty();
}

void VideoDecoder::recycle(VideoFrame &&f)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    recycleLocked(std::move(f));
}

void VideoDecoder::recycleLocked(VideoFrame &&f)
{
    if (f.staging >= 0) {
        const auto it = m_stagingById.find(f.staging);
        if (it != m_stagingById.end()) m_freeStaging.push_back(it->second);
        f.staging = -1;
    }
    // Kept for reuse when its whole capacity is counted (its count goes to the pool with it)
    const size_t pooledBytes = f.budgetBytes;
    if (f.bytes.capacity() && f.budgetBytes >= f.bytes.capacity() && m_bytePool.size() < kBytePool
        && bytePoolBudget::reserve(pooledBytes)) {
        m_bytePool.push_back(std::move(f.bytes));
        m_bytePoolBudgetBytes.push_back(pooledBytes);
        f.budgetBytes = 0; // the pool now owns the reservation with the allocation
    } else {
        f.bytes = {};
    }
    f.releaseAv();
    f.layout.reset();
}

// A byte buffer kept for reuse, if any, into f (its count goes with it: a frame's again, no longer the pool's)
void VideoDecoder::takePooledBytes(VideoFrame &f)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_bytePool.empty()) return;
    f.bytes = std::move(m_bytePool.back());
    decodedBudget::remove(f.budgetBytes);
    f.budgetBytes = m_bytePoolBudgetBytes.back();
    bytePoolBudget::release(f.budgetBytes);
    m_bytePool.pop_back();
    m_bytePoolBudgetBytes.pop_back();
}

int VideoDecoder::stagingCount() const
{
    const size_t size = m_stagingSize.load();
    if (!size) return 0;
    return int(std::clamp<size_t>(kStagingBudget / size, 2, kMaxQueue + 2));
}

void VideoDecoder::addStaging(int id, uint8_t *ptr, size_t size)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const Staging s{id, ptr, size};
    m_stagingById[id] = s;
    m_freeStaging.push_back(s);
}

std::vector<int> VideoDecoder::takeFreeStaging()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<int> ids;
    for (const Staging &s : m_freeStaging) {
        ids.push_back(s.id);
        m_stagingById.erase(s.id);
    }
    m_freeStaging.clear();
    return ids;
}

bool VideoDecoder::takeStaging(size_t size, int *id, uint8_t **ptr)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    for (size_t i = 0; i < m_freeStaging.size(); ++i)
        if (m_freeStaging[i].size >= size) {
            *id = m_freeStaging[i].id;
            *ptr = m_freeStaging[i].ptr;
            m_freeStaging.erase(m_freeStaging.begin() + long(i));
            return true;
        }
    return false;
}

// ---------------------------------------------------------------------------
// Layouts: how the GPU reads a frame
// ---------------------------------------------------------------------------
// A VideoToolbox frame sampled in place: its planes as the copy path describes them (from the surface's format),
// when each is one or two channels of 8 or 16 bits (NV12, P010, P210, P410, P216, P416…). Others are copied.
std::shared_ptr<const VideoLayout> VideoDecoder::surfaceLayout(const AVFrame *hw)
{
#if defined(__APPLE__)
    if (!s_zeroCopy || !hw->hw_frames_ctx || hw->format != AV_PIX_FMT_VIDEOTOOLBOX) return nullptr;
    {
        std::lock_guard<std::mutex> lk(s_refusedMutex);
        if (s_refusedSurfaces.count(surfaceFormat(hw))) return nullptr;
    }
    const auto *frames = reinterpret_cast<const AVHWFramesContext *>(hw->hw_frames_ctx->data);
    AVFrame *probe = av_frame_alloc();
    if (!probe) return nullptr;
    probe->format = frames->sw_format;
    probe->width = hw->width;
    probe->height = hw->height;
    probe->colorspace = hw->colorspace;
    probe->color_range = hw->color_range;
    probe->chroma_location = hw->chroma_location;
    const std::shared_ptr<const VideoLayout> base = layoutFor(probe);
    av_frame_free(&probe);
    if (!base || base->mode != VideoLayout::Mode::Yuv) return nullptr;
    for (int p = 0; p < base->planeCount; ++p)
        if (base->plane[p].channels > 2
            || (base->plane[p].texels != VideoLayout::Texels::U8 && base->plane[p].texels != VideoLayout::Texels::U16))
            return nullptr;
    auto &found = m_surfaceLayouts[base.get()];
    if (!found) {
        auto L = std::make_shared<VideoLayout>(*base);
        L->surface = true;
        L->totalBytes = 0; // nothing to upload
        L->description += QStringLiteral(" · zero copy");
        found = L;
    }
    return found;
#else
    (void)hw;
    return nullptr;
#endif
}

std::shared_ptr<const VideoLayout> VideoDecoder::hapLayout(int count, int f0, int f1)
{
    const LayoutKey key{-1, m_width, m_height, count, f0, f1};
    const auto found = m_layouts.find(key);
    if (found != m_layouts.end()) return found->second;

    auto L = std::make_shared<VideoLayout>();
    L->width = m_width;
    L->height = m_height;
    L->planeCount = count;
    const int w4 = (m_width + 3) & ~3, h4 = (m_height + 3) & ~3;
    bool cpu = false;
    size_t offset = 0;
    for (int i = 0; i < count; ++i) {
        const hap::Format f = hap::Format(i == 0 ? f0 : f1);
        VideoLayout::Plane &p = L->plane[i];
        p.width = w4;
        p.height = h4;
        bool gpu = true;
        switch (f) {
        case hap::Format::RgbDxt1: p.texels = VideoLayout::Texels::Dxt1, gpu = s_s3tc; break;
        case hap::Format::RgbaDxt5:
        case hap::Format::YCoCgDxt5: p.texels = VideoLayout::Texels::Dxt5, gpu = s_s3tc; break;
        case hap::Format::AlphaRgtc1: p.texels = VideoLayout::Texels::Rgtc1; break; // OpenGL 3.0
        case hap::Format::RgbaBc7: p.texels = VideoLayout::Texels::Bc7, gpu = s_bptc; break;
        case hap::Format::RgbBc6u: p.texels = VideoLayout::Texels::Bc6u, gpu = s_bptc; break;
        case hap::Format::RgbBc6s: p.texels = VideoLayout::Texels::Bc6s, gpu = s_bptc; break;
        default: m_layouts[key] = nullptr; return nullptr;
        }
        if (gpu) {
            p.channels = f == hap::Format::AlphaRgtc1 ? 1 : 4;
            p.size = hap::textureBytes(f, m_width, m_height);
        } else { // decoded on the CPU into plain texels
            cpu = true;
            p.texels = VideoLayout::Texels::U8;
            p.channels = hap::decodedChannels(f);
            p.rowBytes = size_t(w4) * size_t(p.channels);
            p.size = p.rowBytes * size_t(h4);
        }
        p.offset = offset;
        offset += (p.size + 63) & ~size_t(63);
    }
    L->totalBytes = offset;
    const hap::Format first = hap::Format(f0);
    if (first == hap::Format::YCoCgDxt5) L->mode = count > 1 ? VideoLayout::Mode::HapYCoCgAlpha : VideoLayout::Mode::HapYCoCg;
    else if (first == hap::Format::AlphaRgtc1) L->mode = VideoLayout::Mode::HapAlphaOnly;
    else L->mode = VideoLayout::Mode::HapRgb;
    L->alpha = L->mode == VideoLayout::Mode::HapYCoCgAlpha || first == hap::Format::RgbaDxt5 || first == hap::Format::RgbaBc7;
    QStringList fmts;
    for (int i = 0; i < count; ++i) fmts << hap::formatName(hap::Format(i == 0 ? f0 : f1));
    L->description = hap::variantName(m_hapTag) + QStringLiteral(" · ") + fmts.join(QStringLiteral(" + "))
                     + (cpu ? QStringLiteral(" · decoded on the CPU") : QString());
    m_layouts[key] = L;
    return L;
}

// ---------------------------------------------------------------------------
// Decoding
// ---------------------------------------------------------------------------
// Packed planes, at the layout's offsets
// mapped: src is the hardware's memory seen in place (m_frame being the hardware frame)
int VideoDecoder::decodeNext(VideoFrame &f, double skipBefore)
{
    if (m_hapTag) return decodeHap(f, skipBefore);
    for (;;) {
        int r = avcodec_receive_frame(m_codec, m_frame);
        if (r == 0) {
            int64_t ts = m_frame->best_effort_timestamp;
            if (ts == AV_NOPTS_VALUE) ts = m_frame->pts;
            double local = ts != AV_NOPTS_VALUE ? ts * m_timeBase - m_startTime : m_lastPts + 1.0 / m_fps;
            m_lastPts = local;
            f.pts = local; // position; stamped with the clock by the caller
            if (local < skipBefore - 0.25 / m_fps) { // not needed: not copied
                av_frame_unref(m_frame);
                return 2;
            }
            AVFrame *src = m_frame;
            bool mapped = false;
            if (m_hwPixFmt >= 0 && m_frame->format == m_hwPixFmt) {
                // Sampled by the GPU where it was decoded: the frame itself is handed over
                if (auto L = surfaceLayout(m_frame)) {
                    AVFrame *keep = av_frame_alloc();
                    if (keep && av_frame_ref(keep, m_frame) >= 0) {
                        f.layout = L;
                        f.av = keep;
                        m_hwActive = true;
                        av_frame_unref(m_frame);
                        return 1;
                    }
                    av_frame_free(&keep);
                }
                // The hardware's memory: seen in place, or copied back when it cannot be
                av_frame_unref(m_swFrame);
                int mr = av_hwframe_map(m_swFrame, m_frame, AV_HWFRAME_MAP_READ);
                mapped = mr >= 0;
                if (mr < 0) {
                    av_frame_unref(m_swFrame);
                    mr = av_hwframe_transfer_data(m_swFrame, m_frame, 0);
                }
                if (mr < 0) {
                    av_frame_unref(m_frame);
                    return 2;
                }
                av_frame_copy_props(m_swFrame, m_frame);
                m_hwActive = true;
                src = m_swFrame;
            }
            const bool ok = deliver(src, f, mapped);
            av_frame_unref(m_swFrame);
            av_frame_unref(m_frame);
            if (!ok) {
                recycle(std::move(f));
                return 2;
            }
            return 1;
        }
        if (r == AVERROR_EOF) return 0;
        if (r != AVERROR(EAGAIN)) return -1;
        if (m_draining) return 0;

        r = readVideoPacket();
        if (r < 0) {
            avcodec_send_packet(m_codec, nullptr); // flush
            m_draining = true;
            continue;
        }
        avcodec_send_packet(m_codec, m_packet);
        av_packet_unref(m_packet);
    }
}

// Starts producing a leg at a position within it.
void VideoDecoder::startLeg(const Timeline::Leg &leg, double position)
{
    for (VideoFrame &f : m_backStack) recycle(std::move(f));
    m_backStack.clear();
    m_leg = leg;
    m_legDone = false;
    if (leg.forward) {
        doSeek(position);
        m_discardBefore = position - 0.5 / m_fps;
    } else {
        m_backEnd = position;
    }
}

// End of the current leg: the next one (Loop, PingPong), or the end of playback (Once).
void VideoDecoder::finishLeg()
{
    if (m_timeline.mode == Timeline::Once || m_duration <= 0) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_eof = true;
        return;
    }
    const Timeline::Leg next = m_timeline.nextLeg(m_leg);
    startLeg(next, next.from); // frames before the in point (from the keyframe) are discarded
}

// Backward leg: the frames of [a, m_backEnd) in reverse order, a = one short window earlier.
// Clock of a frame at position L: clockStart + from - L - frame duration.
void VideoDecoder::produceBackwardWindow()
{
    const double dt = 1.0 / m_fps;
    const double window = std::max(4.0, std::min(12.0, m_fps * 0.3)) * dt; // a few frames: bounded memory
    const double a = std::max(m_leg.to, m_backEnd - window);
    doSeek(a);
    std::vector<VideoFrame> got;
    for (;;) {
        VideoFrame f;
        int r = 0;
        uint64_t decodeNs = 0;
        {
            const auto decodeStarted = std::chrono::steady_clock::now();
            r = decodeNext(f, a);
            decodeNs = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - decodeStarted).count());
        }
        if (r == 1) recordDecodeTime(decodeNs);
        if (r == 2) continue;
        if (r != 1) break;
        ++m_metricDecoded;
        accountFrame(f);
        const double local = f.pts;
        if (local >= m_backEnd - 0.25 * dt) {
            recycle(std::move(f));
            break;
        }
        f.pts = m_leg.clockStart + m_leg.from - local - dt;
        got.push_back(std::move(f));
    }
    m_backStack = std::move(got); // delivered from its end: the latest position first
    m_backEnd = a;
    if (a <= m_leg.to + 1e-9) m_legDone = true; // finished once the stack is delivered
}

void VideoDecoder::run()
{
    uint64_t gen = 0;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        gen = m_generation;
    }
    for (;;) {
        {
            std::unique_lock<std::mutex> lk(m_mutex);
            // Fewer frames ahead while the decoded frames of every stream take more memory than the budget: each
            // stream keeps its minimum (never dropped, never starved), the others wait for theirs to be shown
            bool held = false;
            m_cv.wait(lk, [&] {
                if (m_quit || m_seekPending) return true;
                if (m_eof || m_queue.size() >= kMaxQueue) return false;
                if (m_queue.size() >= kMinQueue && decodedBudget::over()) {
                    if (!held) ++m_metricBudgetHolds;
                    held = true;
                    return false;
                }
                return true;
            });
            if (m_quit) return;
            if (m_seekPending) {
                m_seekPending = false;
                const double c = m_seekTarget;
                m_timeline = m_pendingTimeline;
                gen = m_generation;
                lk.unlock();
                startLeg(m_timeline.legAt(c), m_timeline.position(c));
                continue;
            }
        }

        if (!m_leg.forward) {
            if (!m_backStack.empty()) {
                VideoFrame f = std::move(m_backStack.back());
                m_backStack.pop_back();
                std::lock_guard<std::mutex> lk(m_mutex);
                if (gen != m_generation) recycleLocked(std::move(f));
                else {
                    m_queue.push_back(std::move(f));
                    recordQueueDepth(m_queue.size());
                }
                continue;
            }
            if (!m_legDone) produceBackwardWindow();
            else finishLeg();
            continue;
        }

        VideoFrame f;
        int r = 0;
        uint64_t decodeNs = 0;
        {
            const auto decodeStarted = std::chrono::steady_clock::now();
            r = decodeNext(f);
            decodeNs = uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now() - decodeStarted).count());
        }
        if (r == 1) recordDecodeTime(decodeNs);
        if (r == 1) {
            ++m_metricDecoded;
            accountFrame(f);
            const double local = f.pts;
            if (local < m_discardBefore) {
                recycle(std::move(f));
                continue;
            }
            if (m_duration > 0 && local >= m_leg.to) { // end of the leg (out point, or the end of the media)
                recycle(std::move(f));
                finishLeg();
                continue;
            }
            f.pts = m_leg.clockStart + local - m_leg.from;
            std::lock_guard<std::mutex> lk(m_mutex);
            if (gen != m_generation) recycleLocked(std::move(f));
            else {
                m_queue.push_back(std::move(f));
                recordQueueDepth(m_queue.size());
            }
            continue;
        }
        if (r == 2) continue;
        finishLeg(); // end of file (or error)
    }
}

// A decoded frame kept in memory, counted in the process-wide budget. Not counted: one in an upload buffer
// (the upload buffers have their own), one the GPU samples where it was decoded (a hardware surface)
void VideoDecoder::accountFrame(VideoFrame &frame)
{
    if (frame.staging >= 0 || !frame.layout) return;
    if (frame.av && frame.av->hw_frames_ctx) return;
    const size_t wanted = std::max(frame.layout->totalBytes, frame.bytes.capacity());
    if (wanted > frame.budgetBytes) decodedBudget::add(wanted - frame.budgetBytes);
    else decodedBudget::remove(frame.budgetBytes - wanted);
    frame.budgetBytes = wanted;
}

// ---------------------------------------------------------------------------
// CPU conversion (tests, tools)
// ---------------------------------------------------------------------------
bool VideoDecoder::fetchRgba(double t, std::vector<uint8_t> &out, int *w, int *h)
{
    VideoFrame f;
    if (!fetch(t, f)) return false;
    const bool ok = toRgba(f, out, true);
    if (ok) {
        if (w) *w = f.layout->width;
        if (h) *h = f.layout->height;
    }
    recycle(std::move(f));
    return ok;
}

static inline float clamp01(float v) { return v < 0 ? 0 : v > 1 ? 1 : v; }

bool VideoDecoder::toRgba(const VideoFrame &f, std::vector<uint8_t> &out, bool bottomUp)
{
    if (!f.layout || f.staging >= 0) return false;
    const VideoLayout &L = *f.layout;
    // A hardware surface: copied back to memory first
    struct Freer {
        AVFrame *f = nullptr;
        ~Freer() { av_frame_free(&f); }
    } readBack;
    const AVFrame *av = f.av;
    if (L.surface) {
        if (!f.av) return false;
        readBack.f = av_frame_alloc();
        if (!readBack.f || av_hwframe_transfer_data(readBack.f, f.av, 0) < 0) return false;
        av = readBack.f;
    }
    const int W = L.width, H = L.height;
    out.assign(size_t(W) * H * 4, 0);
    const uint8_t *base[4] = {};
    size_t stride[4] = {};
    for (int p = 0; p < L.planeCount; ++p) {
        if (av) {
            base[p] = av->data[p];
            stride[p] = size_t(av->linesize[p]);
        } else {
            base[p] = f.bytes.data() + L.plane[p].offset;
            stride[p] = L.plane[p].rowBytes;
        }
    }
    // Compressed textures: their blocks decoded on the CPU first
    std::vector<uint8_t> decoded[4];
    int channels[4] = {};
    bool wide[4] = {};
    for (int p = 0; p < L.planeCount; ++p) {
        const VideoLayout::Plane &pl = L.plane[p];
        channels[p] = pl.channels;
        wide[p] = pl.texels == VideoLayout::Texels::U16;
        if (pl.texels == VideoLayout::Texels::U8 || pl.texels == VideoLayout::Texels::U16) continue;
        const hap::Format hf = pl.texels == VideoLayout::Texels::Dxt1    ? hap::Format::RgbDxt1
                               : pl.texels == VideoLayout::Texels::Dxt5  ? hap::Format::RgbaDxt5
                               : pl.texels == VideoLayout::Texels::Rgtc1 ? hap::Format::AlphaRgtc1
                               : pl.texels == VideoLayout::Texels::Bc7   ? hap::Format::RgbaBc7
                               : pl.texels == VideoLayout::Texels::Bc6u  ? hap::Format::RgbBc6u
                                                                         : hap::Format::RgbBc6s;
        channels[p] = hap::decodedChannels(hf);
        decoded[p].resize(size_t(pl.width) * pl.height * channels[p]);
        hap::decodeBlocks(hf, base[p], W, H, decoded[p].data(), size_t(pl.width) * channels[p]);
        base[p] = decoded[p].data();
        stride[p] = size_t(pl.width) * channels[p];
    }
    // A texel of a plane at picture pixel (x, y): nearest (subsampled planes stretched)
    auto texel = [&](int p, int x, int y, int ch) -> float {
        const VideoLayout::Plane &pl = L.plane[p];
        const bool hapPlane = L.mode >= VideoLayout::Mode::HapRgb; // padded, not stretched
        const int px = hapPlane ? x : std::min(pl.width - 1, int(int64_t(x) * pl.width / W));
        const int py = hapPlane ? y : std::min(pl.height - 1, int(int64_t(y) * pl.height / H));
        if (wide[p]) {
            uint16_t v;
            std::memcpy(&v, base[p] + size_t(py) * stride[p] + (size_t(px) * channels[p] + ch) * 2, 2);
            return v / 65535.0f;
        }
        return base[p][size_t(py) * stride[p] + size_t(px) * channels[p] + ch] / 255.0f;
    };
    for (int y = 0; y < H; ++y) {
        uint8_t *row = out.data() + size_t(bottomUp ? H - 1 - y : y) * W * 4;
        for (int x = 0; x < W; ++x) {
            float r = 0, g = 0, b = 0, a = 1;
            auto chan = [&](int c) { return L.srcPlane[c] < 0 ? 0.0f : texel(L.srcPlane[c], x, y, L.srcChannel[c]) * L.scale[c]; };
            switch (L.mode) {
            case VideoLayout::Mode::Yuv: {
                const float Y = (chan(0) - L.yOffset) * L.yScale, U = (chan(1) - L.cOffset) * L.cScale,
                            V = (chan(2) - L.cOffset) * L.cScale;
                r = L.matrix[0] * Y + L.matrix[1] * U + L.matrix[2] * V;
                g = L.matrix[3] * Y + L.matrix[4] * U + L.matrix[5] * V;
                b = L.matrix[6] * Y + L.matrix[7] * U + L.matrix[8] * V;
                if (L.alpha) a = chan(3);
                break;
            }
            case VideoLayout::Mode::Rgb:
                r = chan(0), g = chan(1), b = chan(2);
                if (L.alpha) a = chan(3);
                break;
            case VideoLayout::Mode::Gray:
                r = g = b = chan(0);
                if (L.alpha) a = chan(3);
                break;
            case VideoLayout::Mode::HapRgb:
                r = texel(0, x, y, 0), g = texel(0, x, y, 1), b = texel(0, x, y, 2);
                if (L.alpha) a = texel(0, x, y, 3);
                break;
            case VideoLayout::Mode::HapYCoCg:
            case VideoLayout::Mode::HapYCoCgAlpha: {
                const float k = 128.0f / 255.0f;
                const float s = texel(0, x, y, 2) * (255.0f / 8.0f) + 1.0f;
                const float co = (texel(0, x, y, 0) - k) / s, cg = (texel(0, x, y, 1) - k) / s, Y = texel(0, x, y, 3);
                r = Y + co - cg, g = Y + cg, b = Y - co - cg;
                if (L.mode == VideoLayout::Mode::HapYCoCgAlpha) a = texel(1, x, y, 0);
                break;
            }
            case VideoLayout::Mode::HapAlphaOnly: r = g = b = texel(0, x, y, 0); break;
            }
            uint8_t *o = row + size_t(x) * 4;
            o[0] = uint8_t(clamp01(r) * 255.0f + 0.5f);
            o[1] = uint8_t(clamp01(g) * 255.0f + 0.5f);
            o[2] = uint8_t(clamp01(b) * 255.0f + 0.5f);
            o[3] = uint8_t(clamp01(a) * 255.0f + 0.5f);
        }
    }
    return true;
}
