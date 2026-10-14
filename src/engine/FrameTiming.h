#pragma once
// Frame timing: what the show's deadlines look like from the outputs' side, measured on a monotonic clock.
//
// Per frame of the render thread: the interval since the previous one (and whether it missed its deadline: over
// 1.5 periods), the time to compute it (layers, effects, composition), the time to hand it to the windows (present:
// blit, Metal, vertical sync). Per input: the time from an OSC message that changed a value, or a cue step that fired,
// to the end of the first frame started after it — the latency an operator sees. Each as a histogram (p50, p95, p99,
// max), with no allocation per frame.
//
// The render thread writes the frames; any thread notes the inputs; any thread reads the totals.

#include <QJsonObject>
#include <QtGlobal>
#include <array>
#include <atomic>
#include <mutex>

// The render thread's pace without a vertical sync to wait for: absolute deadlines, one period apart
struct FramePacer {
    // A frame started at `start`, done at `now`: how long to sleep (ns) before the next one is due. `next`: when it is
    // due (-1: not yet known; kept between calls). Late by less than a period: no sleep, the schedule is kept (caught
    // up); by more: the schedule starts again from now (no burst of frames to catch up)
    static qint64 next(qint64 &next, qint64 start, qint64 now, qint64 periodNs)
    {
        if (periodNs <= 0) return 0;
        if (next < 0 || start - next > periodNs || next - start > periodNs) next = start; // first frame, or far off
        next += periodNs;
        if (now >= next) {
            if (now - next > periodNs) next = now; // too late: from now on
            return 0;
        }
        return next - now;
    }
    // The vertical sync paced this frame: the schedule follows it (no sleep)
    static qint64 resync(qint64 &next, qint64 now, qint64 periodNs)
    {
        next = now;
        (void)periodNs;
        return 0;
    }
};

class FrameTiming
{
public:
    // Milliseconds in fixed buckets (FrameTiming.cpp), fine around 8.3, 16.7, 20, 33.3 and 40 ms; the last: beyond
    struct Histogram {
        static constexpr int kBuckets = 27;
        std::array<quint64, kBuckets> buckets{};
        quint64 count = 0;
        double total = 0, max = 0;
        void add(double ms);
        double percentile(double p) const; // upper edge of its bucket, never above max (0: none)
        double mean() const { return count ? total / double(count) : 0; }
        QJsonObject json() const;
        static double edge(int k); // upper edge of bucket k (ms; infinity for the last)
    };
    enum Input { Osc = 0, Cue, InputCount };
    struct Stats {
        quint64 frames = 0, presented = 0, missed = 0;
        Histogram interval, compute, present;
        std::array<Histogram, InputCount> inputToFrame;
        QJsonObject json() const;
    };

    // Any thread: an input that changes what is shown, at `ns` (steady clock; < 0: now)
    void noteInput(Input kind, qint64 ns = -1);
    // Render thread: a frame started at `start`, computed at `computed`, handed to the outputs at `end` (steady clock
    // ns); `periodNs` its deadline (one frame at the rate asked or the screen's); `presented`: a window got it
    void noteFrame(qint64 start, qint64 computed, qint64 end, qint64 periodNs, bool presented);
    Stats stats() const;
    // Consumed by the optional endurance logger. Independent one-second maxima / histograms,
    // plus a bounded in-memory log of frames whose intervals exceeded twice the budget.
    struct DeadlineEvent { qint64 atNs = 0; double intervalMs = 0, budgetMs = 0; };
    struct Window {
        Stats stats;
        std::array<DeadlineEvent, 128> events{};
        size_t eventCount = 0;
        quint64 eventsLost = 0;
    };
    Window takeWindow();
    void reset();
    static qint64 nowNs(); // the steady clock

private:
    mutable std::mutex m_mutex; // the totals (short: copied out by stats())
    Stats m_stats;
    Window m_window;
    qint64 m_lastStart = -1;
    // The earliest input not yet in a frame, per kind (-1: none): the frame started after it takes it (the latency of
    // the earliest is the one counted: the worst of that frame's)
    std::array<std::atomic<qint64>, InputCount> m_pending{{std::atomic<qint64>{-1}, std::atomic<qint64>{-1}}};
};
