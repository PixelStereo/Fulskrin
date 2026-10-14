// Frame timing (FrameTiming.h).
#include "FrameTiming.h"

#include <QJsonArray>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <limits>

// Fine around the usual frame periods (120, 60, 50, 30, 25 Hz), so that the percentiles read where frames land
static constexpr double kEdges[FrameTiming::Histogram::kBuckets - 1] = {
    0.5, 1, 2, 4, 6, 8, 8.4, 10, 12, 14, 16, 16.8, 18, 20, 20.5, 22, 25, 30, 33.5, 40, 41, 50, 66.7, 100, 200, 500};

double FrameTiming::Histogram::edge(int k)
{
    return k >= 0 && k < kBuckets - 1 ? kEdges[k] : std::numeric_limits<double>::infinity();
}

void FrameTiming::Histogram::add(double ms)
{
    if (!std::isfinite(ms)) return;
    ms = std::max(0.0, ms);
    int k = 0;
    while (k < kBuckets - 1 && ms > kEdges[k]) ++k;
    ++buckets[size_t(k)];
    ++count;
    total += ms;
    max = std::max(max, ms);
}

double FrameTiming::Histogram::percentile(double p) const
{
    if (!count) return 0;
    const double want = std::max(1.0, std::ceil(std::clamp(p, 0.0, 1.0) * double(count)));
    quint64 seen = 0;
    for (int k = 0; k < kBuckets; ++k) {
        seen += buckets[size_t(k)];
        if (double(seen) >= want) return std::min(edge(k), max);
    }
    return max;
}

QJsonObject FrameTiming::Histogram::json() const
{
    auto r = [](double v) { return std::round(v * 100.0) / 100.0; };
    return QJsonObject{{"count", double(count)}, {"mean_ms", r(mean())}, {"p50_ms", r(percentile(0.5))},
                       {"p95_ms", r(percentile(0.95))}, {"p99_ms", r(percentile(0.99))}, {"max_ms", r(max)}};
}

QJsonObject FrameTiming::Stats::json() const
{
    return QJsonObject{{"frames", double(frames)},
                       {"presented", double(presented)},
                       {"missed_deadlines", double(missed)},
                       {"interval", interval.json()},
                       {"compute", compute.json()},
                       {"present", present.json()},
                       {"osc_to_frame", inputToFrame[Osc].json()},
                       {"cue_to_frame", inputToFrame[Cue].json()}};
}

qint64 FrameTiming::nowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

void FrameTiming::noteInput(Input kind, qint64 ns)
{
    if (kind < 0 || kind >= InputCount) return;
    m_inputCount[size_t(kind)].fetch_add(1, std::memory_order_relaxed);
    if (ns < 0) ns = nowNs();
    std::atomic<qint64> &slot = m_pending[size_t(kind)];
    qint64 cur = slot.load(std::memory_order_relaxed);
    while ((cur < 0 || ns < cur) && !slot.compare_exchange_weak(cur, ns, std::memory_order_relaxed)) {}
}

void FrameTiming::noteFrame(qint64 start, qint64 computed, qint64 end, qint64 periodNs, bool presented)
{
    std::array<qint64, InputCount> taken;
    for (int k = 0; k < InputCount; ++k) {
        std::atomic<qint64> &slot = m_pending[size_t(k)];
        const qint64 t = slot.exchange(-1, std::memory_order_relaxed);
        taken[size_t(k)] = -1;
        if (t < 0) continue;
        if (t <= start) {
            taken[size_t(k)] = t;
            continue;
        }
        // It came during this frame: the next one shows it (given back, unless an earlier one is there by now)
        qint64 cur = slot.load(std::memory_order_relaxed);
        while ((cur < 0 || t < cur) && !slot.compare_exchange_weak(cur, t, std::memory_order_relaxed)) {}
    }
    std::lock_guard<std::mutex> lk(m_mutex);
    const qint64 intervalNs = (m_lastStart >= 0 && start > m_lastStart) ? start - m_lastStart : 0;
    const bool missed = intervalNs > 0 && periodNs > 0 && intervalNs * 2 > periodNs * 3;
    // The cumulative and consumable-window histograms observe the exact same samples.
    // Both live behind the existing short frame-timing mutex: no disk work in this thread.
    for (Stats *stats : {&m_stats, &m_window.stats}) {
        ++stats->frames;
        if (presented) ++stats->presented;
        if (intervalNs > 0) {
            stats->interval.add(double(intervalNs) / 1e6);
            if (missed) ++stats->missed;
        }
        stats->compute.add(double(computed - start) / 1e6);
        stats->present.add(double(end - computed) / 1e6);
        for (int k = 0; k < InputCount; ++k)
            if (taken[size_t(k)] >= 0)
                stats->inputToFrame[size_t(k)].add(double(end - taken[size_t(k)]) / 1e6);
    }
    if (periodNs > 0 && intervalNs > 2 * periodNs) {
        if (m_window.eventCount < m_window.events.size())
            m_window.events[m_window.eventCount++] = {end, double(intervalNs) / 1e6, double(periodNs) / 1e6};
        else ++m_window.eventsLost;
    }
    m_lastStart = start;
}

FrameTiming::Stats FrameTiming::stats() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_stats;
}

void FrameTiming::noteEvent(const QString &kind, const QString &detail)
{
    const qint64 at = nowNs();
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_window.journal.size() < kJournalPerWindow) m_window.journal.push_back({at, kind, detail});
    else ++m_window.journalLost;
}

FrameTiming::Window FrameTiming::takeWindow()
{
    Window taken;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        taken = std::move(m_window);
        m_window = Window{};
    }
    for (int k = 0; k < InputCount; ++k) taken.inputs[size_t(k)] = m_inputCount[size_t(k)].exchange(0, std::memory_order_relaxed);
    return taken;
}

void FrameTiming::reset()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_stats = Stats();
    m_window = Window{};
    for (auto &c : m_inputCount) c.store(0, std::memory_order_relaxed);
    m_lastStart = -1;
    for (auto &p : m_pending) p.store(-1, std::memory_order_relaxed);
}
