#include "VideoDecoder.h"

#include <algorithm>
#include <cstdint>

// Instrumentation lives separately from packet parsing, codec setup and frame delivery so it can
// evolve and be tested without expanding the decoder's already-large implementation file.
VideoDecoder::Metrics VideoDecoder::metrics() const
{
    Metrics m;
    m.decodedFrames = m_metricDecoded.load(std::memory_order_relaxed);
    m.fetchedFrames = m_metricFetched.load(std::memory_order_relaxed);
    m.skippedFrames = m_metricSkipped.load(std::memory_order_relaxed);
    m.fetchMisses = m_metricMisses.load(std::memory_order_relaxed);
    m.lateFrames = m_metricLate.load(std::memory_order_relaxed);
    m.budgetHolds = m_metricBudgetHolds.load(std::memory_order_relaxed);
    m.queueHighWatermark = m_metricQueueHigh.load(std::memory_order_relaxed);
    m.decodeTimeTotalMs = double(m_metricDecodeNsTotal.load(std::memory_order_relaxed)) / 1e6;
    m.decodeTimeMaxMs = double(m_metricDecodeNsMax.load(std::memory_order_relaxed)) / 1e6;
    m.lastFrameLatenessMs = double(m_metricLastLateNs.load(std::memory_order_relaxed)) / 1e6;
    m.maxFrameLatenessMs = double(m_metricMaxLateNs.load(std::memory_order_relaxed)) / 1e6;

    // Percentiles from the buckets: a bucket's upper bound, never more than the longest measured (the last bucket,
    // beyond 2 s, has no bound: the longest is given)
    uint64_t counts[kDecodeBuckets];
    uint64_t total = 0;
    for (size_t i = 0; i < kDecodeBuckets; ++i) total += counts[i] = m_metricDecodeBuckets[i].load(std::memory_order_relaxed);
    auto percentile = [&](uint64_t pct) {
        if (!total) return 0.0;
        const uint64_t target = (total * pct + 99) / 100;
        uint64_t seen = 0;
        for (size_t i = 0; i < kDecodeBuckets; ++i) {
            seen += counts[i];
            if (seen >= target)
                return i < kDecodeBuckets - 1 ? std::min(double(kDecodeBucketNs[i]) / 1e6, m.decodeTimeMaxMs) : m.decodeTimeMaxMs;
        }
        return m.decodeTimeMaxMs;
    };
    m.decodeTimeP95Ms = percentile(95);
    m.decodeTimeP99Ms = percentile(99);
    m.decodesOverFrame = m_metricOverFrame.load(std::memory_order_relaxed);
    m.demuxErrors = m_metricDemuxErrors.load(std::memory_order_relaxed);
    m.codecErrors = m_metricCodecErrors.load(std::memory_order_relaxed);
    m.emptyLoopStops = m_metricEmptyLoopStops.load(std::memory_order_relaxed);
    m.ioRetries = m_metricIoRetries.load(std::memory_order_relaxed);
    m.lastAvError = m_lastAvError.load(std::memory_order_relaxed);
    return m;
}

void VideoDecoder::resetMetrics()
{
    m_metricDecoded = 0;
    m_metricFetched = 0;
    m_metricSkipped = 0;
    m_metricMisses = 0;
    m_metricLate = 0;
    m_metricBudgetHolds = 0;
    m_metricQueueHigh = 0;
    m_metricDecodeNsTotal = 0;
    m_metricDecodeNsMax = 0;
    m_metricLastLateNs = 0;
    m_metricMaxLateNs = 0;
    for (auto &bucket : m_metricDecodeBuckets) bucket = 0;
    m_metricOverFrame = 0;
    m_metricDemuxErrors = 0;
    m_metricCodecErrors = 0;
    m_metricEmptyLoopStops = 0;
    m_metricIoRetries = 0;
    m_lastAvError = 0;
}

void VideoDecoder::recordQueueDepth(size_t depth)
{
    uint64_t previous = m_metricQueueHigh.load(std::memory_order_relaxed);
    while (previous < depth && !m_metricQueueHigh.compare_exchange_weak(previous, uint64_t(depth), std::memory_order_relaxed)) {}
}

void VideoDecoder::recordDecodeTime(uint64_t nanoseconds)
{
    m_metricDecodeNsTotal.fetch_add(nanoseconds, std::memory_order_relaxed);
    uint64_t previous = m_metricDecodeNsMax.load(std::memory_order_relaxed);
    while (previous < nanoseconds && !m_metricDecodeNsMax.compare_exchange_weak(previous, nanoseconds, std::memory_order_relaxed)) {}
    size_t bucket = 0;
    while (bucket < kDecodeBuckets - 1 && nanoseconds > kDecodeBucketNs[bucket]) ++bucket;
    if (m_fps > 0 && double(nanoseconds) > 1e9 / m_fps) m_metricOverFrame.fetch_add(1, std::memory_order_relaxed);
    m_metricDecodeBuckets[bucket].fetch_add(1, std::memory_order_relaxed);
}

