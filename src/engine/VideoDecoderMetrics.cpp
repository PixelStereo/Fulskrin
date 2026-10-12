#include "VideoDecoder.h"

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
    m.memoryBudgetDrops = m_metricMemoryBudgetDrops.load(std::memory_order_relaxed);
    m.queueHighWatermark = m_metricQueueHigh.load(std::memory_order_relaxed);
    m.decodeTimeTotalMs = double(m_metricDecodeNsTotal.load(std::memory_order_relaxed)) / 1e6;
    m.decodeTimeMaxMs = double(m_metricDecodeNsMax.load(std::memory_order_relaxed)) / 1e6;
    m.lastFrameLatenessMs = double(m_metricLastLateNs.load(std::memory_order_relaxed)) / 1e6;
    m.maxFrameLatenessMs = double(m_metricMaxLateNs.load(std::memory_order_relaxed)) / 1e6;

    static constexpr double upperMs[] = {0.25, 0.5, 1, 2, 4, 8, 16, 33, 50, 100, 250, 500};
    uint64_t total = 0;
    for (const auto &bucket : m_metricDecodeBuckets) total += bucket.load(std::memory_order_relaxed);
    if (total) {
        const uint64_t target = (total * 95 + 99) / 100;
        uint64_t seen = 0;
        for (size_t i = 0; i < m_metricDecodeBuckets.size(); ++i) {
            seen += m_metricDecodeBuckets[i].load(std::memory_order_relaxed);
            if (seen >= target) {
                m.decodeTimeP95Ms = upperMs[i];
                break;
            }
        }
    }
    return m;
}

void VideoDecoder::resetMetrics()
{
    m_metricDecoded = 0;
    m_metricFetched = 0;
    m_metricSkipped = 0;
    m_metricMisses = 0;
    m_metricLate = 0;
    m_metricMemoryBudgetDrops = 0;
    m_metricQueueHigh = 0;
    m_metricDecodeNsTotal = 0;
    m_metricDecodeNsMax = 0;
    m_metricLastLateNs = 0;
    m_metricMaxLateNs = 0;
    for (auto &bucket : m_metricDecodeBuckets) bucket = 0;
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
    static constexpr uint64_t limitsNs[] = {250000, 500000, 1000000, 2000000, 4000000, 8000000,
                                             16000000, 33000000, 50000000, 100000000, 250000000};
    size_t bucket = 0;
    while (bucket < 11 && nanoseconds > limitsNs[bucket]) ++bucket;
    m_metricDecodeBuckets[bucket].fetch_add(1, std::memory_order_relaxed);
}

