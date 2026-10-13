#pragma once
// The events shown in the Logs panel (OSC in / out, MIDI in, keyboard): a bounded ring, written from any thread at a
// cost that does not depend on the traffic, read by the interface a few times a second.
//
//  - A message repeated on the same address (a fader sending its value 100 times a second) updates the last line
//    and counts the repeats, instead of adding one line each time (within a second of the last one).
//  - Beyond `maxPerSecond` new lines a second, the others are counted as discarded, not kept.
//  - The oldest lines leave the ring when it is full (counted too).
//  - A channel hidden in the panel is not even formatted by its producer (wants()).
//
// Its lines have a sequence number: the panel adds and removes rows by it instead of rebuilding the table.

#include <QString>
#include <QtGlobal>
#include <array>
#include <atomic>
#include <mutex>
#include <vector>

class EventLog
{
public:
    enum Channel { OscIn = 0, OscOut, MidiIn, MidiOut, Keyboard, ChannelCount };
    static QString channelName(int c);        // "OSC IN"…
    static int channelOf(const QString &name); // -1: not one of them

    struct Entry {
        quint64 seq = 0;  // increasing, one per line
        qint64 time = 0;  // ms since the epoch: the last message of the line
        int channel = 0;
        QString message;
        int repeats = 1;  // messages merged in this line
    };
    struct Counters {
        quint64 received = 0;    // every message appended
        quint64 merged = 0;      // into the line before (repeats)
        quint64 discarded = 0;   // over the rate
        quint64 overwritten = 0; // pushed out of the ring
    };

    explicit EventLog(int capacity = 2000, int maxPerSecond = 500);
    static EventLog &instance(); // the application's

    bool wants(int channel) const;
    bool wants(const QString &channel) const { return wants(channelOf(channel)); }
    void setWanted(int channel, bool on);

    // Any thread. nowMs < 0: the clock now
    void append(int channel, const QString &message, qint64 nowMs = -1);
    void append(const QString &channel, const QString &message, qint64 nowMs = -1);
    void clear();

    quint64 generation() const { return m_generation.load(std::memory_order_acquire); } // changes on every change
    std::vector<Entry> entries() const; // oldest first
    Counters counters() const;
    int capacity() const { return m_capacity; }

    // What a message merges on: its address (before two spaces or " = "), on the same channel
    static QString keyOf(const QString &message);

private:
    mutable std::mutex m_mutex;
    std::vector<Entry> m_ring;
    size_t m_head = 0, m_count = 0; // m_head: the oldest
    int m_capacity, m_maxPerSecond;
    quint64 m_nextSeq = 1;
    qint64 m_windowStart = 0;
    int m_inWindow = 0;
    Counters m_counters;
    std::atomic<quint64> m_generation{0};
    std::atomic<quint32> m_wanted{(1u << ChannelCount) - 1};
};
