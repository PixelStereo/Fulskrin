#pragma once
// Where the layers' timecodes go.
//  - Timecode outputs: named by the user, in the composition. A Timecode layer drives one of them (one layer per output,
//    TimecodeSettings::output); an output is patched to any number of destinations, and each destination says the
//    protocol: a MIDI output carries MTC (to come: an audio channel LTC, a DeckLink output or an NDI sender ATC).
//  - MIDI outputs (the MIDI patch): named by the user too, each one given a port of this machine (a hardware port, or
//    a virtual port other software can read). A project opened on another machine is patched again there: its routes
//    name the MIDI outputs, not the ports.
// TimecodeRouter sends: a thread of its own reads the clocks and writes the messages, with no engine lock.

#include "TimecodeClock.h"

#include <QJsonObject>
#include <QString>
#include <QStringList>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

struct MidiOutput {
    quint64 id = 0;
    QString name;
    QString port; // a port of this machine, by its name ("": none; kVirtualPort: a virtual port named after the output)
    static constexpr const char *kVirtualPort = "@virtual";

    QJsonObject toJson() const;
    static MidiOutput fromJson(const QJsonObject &o);
    bool operator==(const MidiOutput &o) const { return id == o.id && name == o.name && port == o.port; }
};

struct TimecodeOutput {
    quint64 id = 0;
    QString name;
    QStringList routes; // destinations: "midi:<id>" (MTC)

    QJsonObject toJson() const;
    static TimecodeOutput fromJson(const QJsonObject &o);
    bool operator==(const TimecodeOutput &o) const { return id == o.id && name == o.name && routes == o.routes; }
};

namespace tcroute {
QString midi(quint64 midiOutput);               // "midi:12"
std::optional<quint64> midiId(const QString &route);
QString protocol(const QString &route);         // what goes there: "MTC" ("" for a route this version does not know)
} // namespace tcroute

// MTC from a clock's readings, called often (each millisecond or so). While the clock runs forwards: quarter frames, four
// per frame, each sequence of eight starting on an even frame and carrying its timecode; when it starts, jumps or changes
// rate: a full frame first. Held still or running backwards: a full frame each time the frame shown changes (receivers
// locate on it). Pure: no I/O, no clock of its own.
class MtcSender
{
public:
    using Message = std::vector<uint8_t>;
    // The messages due since the last call (none: the clock shows nothing, nothing is sent)
    void step(const std::optional<TcReading> &r, std::vector<Message> &out);

private:
    bool m_quarter = false;  // sending quarter frames
    int64_t m_nextQ = 0;     // the next quarter frame to send (frame × 4 + its quarter)
    TcRate m_rate = TcRate::Fps25;
    int64_t m_held = -1;     // the frame last sent as a full frame while held (-1: none)
};

class RtMidiOut;

class TimecodeRouter
{
public:
    // A layer's clock and where its output goes
    struct Stream {
        quint64 output = 0;
        std::shared_ptr<TimecodeClock> clock;
        std::vector<quint64> midi; // MIDI outputs (MTC)
    };

    TimecodeRouter();
    ~TimecodeRouter();
    void configure(std::vector<Stream> streams, std::vector<MidiOutput> midi);
    QString midiStatus(quint64 midiOutput) const; // "Open: 2 timecodes", "Port not found"…
    static QStringList midiPorts();               // the MIDI outputs of this machine
    static bool virtualPorts();                   // virtual ports can be made (macOS, Linux)
    // Tests: the messages for each MIDI output go here instead of its port (none: back to the ports)
    using Sink = std::function<void(quint64 midiOutput, const std::vector<uint8_t> &msg)>;
    void setTestSink(Sink sink);

private:
    struct Port {
        std::unique_ptr<RtMidiOut> out;
        QString error;
        qint64 triedAt = -1;
    };
    struct Destination {
        quint64 midi = 0;
        MtcSender mtc;
    };
    struct Running {
        Stream stream;
        std::vector<Destination> dest;
    };
    void run();
    void applyLocked(); // the new configuration, on the thread: ports opened and closed, senders kept or made
    static QString portKey(const MidiOutput &m);
    Port *openPort(const MidiOutput &m, qint64 nowMs);

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stop = false, m_dirty = false;
    std::vector<Stream> m_streams;              // configured (main thread)
    std::vector<MidiOutput> m_midi;
    Sink m_sink;
    std::map<quint64, QString> m_status;        // by MIDI output, written by the thread

    // The thread's own
    std::vector<Running> m_running;
    std::map<quint64, MidiOutput> m_midiNow;
    std::map<QString, Port> m_ports;            // by port key
    std::thread m_thread;
};
