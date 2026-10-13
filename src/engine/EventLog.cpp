// The Logs panel's events: a bounded ring (EventLog.h).
#include "EventLog.h"

#include <QDateTime>
#include <algorithm>

static const char *const kNames[EventLog::ChannelCount] = {"OSC IN", "OSC OUT", "MIDI IN", "MIDI OUT", "KEYBOARD"};

QString EventLog::channelName(int c)
{
    return c >= 0 && c < ChannelCount ? QString::fromLatin1(kNames[c]) : QString();
}

int EventLog::channelOf(const QString &name)
{
    for (int c = 0; c < ChannelCount; ++c)
        if (name == QLatin1String(kNames[c])) return c;
    return -1;
}

EventLog::EventLog(int capacity, int maxPerSecond)
    : m_capacity(std::max(1, capacity)), m_maxPerSecond(std::max(1, maxPerSecond))
{
    m_ring.resize(size_t(m_capacity));
}

EventLog &EventLog::instance()
{
    static EventLog log;
    return log;
}

bool EventLog::wants(int channel) const
{
    return channel >= 0 && channel < ChannelCount && (m_wanted.load(std::memory_order_relaxed) >> channel & 1u);
}

void EventLog::setWanted(int channel, bool on)
{
    if (channel < 0 || channel >= ChannelCount) return;
    if (on) m_wanted.fetch_or(1u << channel, std::memory_order_relaxed);
    else m_wanted.fetch_and(~(1u << channel), std::memory_order_relaxed);
}

QString EventLog::keyOf(const QString &message)
{
    int cut = message.indexOf(QLatin1String("  "));
    const int eq = message.indexOf(QLatin1String(" = "));
    if (cut < 0 || (eq >= 0 && eq < cut)) cut = eq;
    return cut < 0 ? message : message.left(cut);
}

void EventLog::append(const QString &channel, const QString &message, qint64 nowMs)
{
    append(channelOf(channel), message, nowMs);
}

void EventLog::append(int channel, const QString &message, qint64 nowMs)
{
    if (channel < 0 || channel >= ChannelCount || !wants(channel)) return;
    if (nowMs < 0) nowMs = QDateTime::currentMSecsSinceEpoch();
    std::lock_guard<std::mutex> lk(m_mutex);
    ++m_counters.received;
    // The same address again: the last line takes the new value and counts it
    if (m_count) {
        Entry &last = m_ring[(m_head + m_count - 1) % m_ring.size()];
        if (last.channel == channel && nowMs - last.time < 1000 && keyOf(last.message) == keyOf(message)) {
            last.message = message;
            last.time = nowMs;
            ++last.repeats;
            ++m_counters.merged;
            m_generation.fetch_add(1, std::memory_order_release);
            return;
        }
    }
    // A new line: within the rate
    if (nowMs - m_windowStart >= 1000 || nowMs < m_windowStart) {
        m_windowStart = nowMs;
        m_inWindow = 0;
    }
    if (m_inWindow >= m_maxPerSecond) {
        ++m_counters.discarded;
        m_generation.fetch_add(1, std::memory_order_release); // the counter is shown
        return;
    }
    ++m_inWindow;
    Entry e;
    e.seq = m_nextSeq++;
    e.time = nowMs;
    e.channel = channel;
    e.message = message;
    if (m_count == m_ring.size()) { // full: the oldest goes
        m_ring[m_head] = std::move(e);
        m_head = (m_head + 1) % m_ring.size();
        ++m_counters.overwritten;
    } else {
        m_ring[(m_head + m_count) % m_ring.size()] = std::move(e);
        ++m_count;
    }
    m_generation.fetch_add(1, std::memory_order_release);
}

void EventLog::clear()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    for (Entry &e : m_ring) e = Entry();
    m_head = m_count = 0;
    m_counters = Counters();
    m_inWindow = 0;
    m_generation.fetch_add(1, std::memory_order_release);
}

std::vector<EventLog::Entry> EventLog::entries() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<Entry> out;
    out.reserve(m_count);
    for (size_t k = 0; k < m_count; ++k) out.push_back(m_ring[(m_head + k) % m_ring.size()]);
    return out;
}

EventLog::Counters EventLog::counters() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_counters;
}
