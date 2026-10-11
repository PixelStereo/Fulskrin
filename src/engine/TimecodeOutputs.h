#pragma once
// Where the layers' timecodes go.
//  - Timecode outputs: named by the user, in the composition. A Timecode layer drives one of them (one layer per output,
//    TimecodeSettings::output); an output is patched to any number of destinations, and each destination says the
//    protocol: a MIDI output carries MTC, an audio channel LTC, a viewport ATC (in the pictures of its NDI and DeckLink
//    outputs). A destination takes one timecode output.
//  - MIDI outputs and audio channels (the patch): named by the user too, each one given a port or a channel of a device
//    of this machine. A project opened on another machine is patched again there: its routes name the patch, not the
//    hardware.
// TimecodeRouter sends MTC and LTC: a thread of its own reads the clocks and writes the MIDI messages, the audio devices'
// callbacks write the LTC; none takes the engine's lock. ATC goes with the frames (Engine::frame).

#include "Timecode.h"
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

struct AudioChannelOutput {
    quint64 id = 0;
    QString name;
    QString device;  // an audio output of this machine, by its name ("": none; kDefaultDevice: the system's)
    int channel = 0; // from 0 (shown from 1)
    static constexpr const char *kDefaultDevice = "@default";

    QJsonObject toJson() const;
    static AudioChannelOutput fromJson(const QJsonObject &o);
    bool operator==(const AudioChannelOutput &o) const
    {
        return id == o.id && name == o.name && device == o.device && channel == o.channel;
    }
};

struct TimecodeOutput {
    quint64 id = 0;
    QString name;
    QStringList routes; // destinations: "midi:<id>" (MTC), "audio:<id>" (LTC), "viewport:<layer id>" (ATC)

    QJsonObject toJson() const;
    static TimecodeOutput fromJson(const QJsonObject &o);
    bool operator==(const TimecodeOutput &o) const { return id == o.id && name == o.name && routes == o.routes; }
};

namespace tcroute {
enum class Kind { None, Midi, Audio, Viewport };
QString make(Kind k, quint64 id);              // "midi:12"
Kind kind(const QString &route);               // None: a route this version does not know
quint64 id(const QString &route);              // 0: none
QString protocol(Kind k);                      // "MTC", "LTC", "ATC"
inline QString midi(quint64 id) { return make(Kind::Midi, id); }
inline QString audio(quint64 id) { return make(Kind::Audio, id); }
inline QString viewport(quint64 id) { return make(Kind::Viewport, id); }
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

namespace midi {
class Output;
}
namespace tcaudio {
class Device;
}

class TimecodeRouter
{
public:
    // A layer's clock and where its output goes
    struct Stream {
        quint64 output = 0;
        std::shared_ptr<TimecodeClock> clock;
        std::vector<quint64> midi;  // MIDI outputs (MTC)
        std::vector<quint64> audio; // audio channels (LTC)
    };

    TimecodeRouter();
    ~TimecodeRouter();
    void configure(std::vector<Stream> streams, std::vector<MidiOutput> midi, std::vector<AudioChannelOutput> audio);
    QString midiStatus(quint64 midiOutput) const;    // "Sending MTC", "Port not found"…
    QString audioStatus(quint64 audioOutput) const;  // "Sending LTC", "Channel 3: the device has 2"…
    // Tests: the MIDI messages for each MIDI output go here instead of its port (none: back to the ports)
    using Sink = std::function<void(quint64 midiOutput, const std::vector<uint8_t> &msg)>;
    void setTestSink(Sink sink);

private:
    struct Port {
        std::unique_ptr<midi::Output> out;
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
    // LTC: the lanes of an audio device (a channel each), read by its callback
    struct Lane {
        quint64 output = 0, audio = 0;
        int channel = 0;
        std::shared_ptr<TimecodeClock> clock;
        std::shared_ptr<ltc::Generator> gen;
    };
    struct AudioSink {
        std::mutex mutex;
        std::vector<Lane> lanes;
        std::vector<float> mono;
        int channels = 0; // the device's
    };
    struct AudioDev {
        std::unique_ptr<tcaudio::Device> dev;
        std::shared_ptr<AudioSink> sink;
        QString error;
        qint64 triedAt = -1;
    };
    void run();
    void reconfigure(std::vector<Stream> &streams, const std::vector<MidiOutput> &midi,
                     const std::vector<AudioChannelOutput> &audio, bool testSink);
    void openPorts(qint64 nowMs, bool testSink, std::map<quint64, QString> &status);
    void openAudio(qint64 nowMs, std::map<quint64, QString> &status);
    static QString portKey(const MidiOutput &m);

    mutable std::mutex m_mutex;
    std::condition_variable m_wake;
    bool m_stop = false, m_dirty = false;
    std::vector<Stream> m_streams; // configured (main thread)
    std::vector<MidiOutput> m_midi;
    std::vector<AudioChannelOutput> m_audio;
    Sink m_sink;
    std::map<quint64, QString> m_midiStatus, m_audioStatus; // written by the thread

    // The thread's own
    std::vector<Running> m_running;
    std::map<quint64, MidiOutput> m_midiNow;
    std::map<quint64, AudioChannelOutput> m_audioNow;
    std::map<QString, Port> m_ports;      // by port key
    std::map<QString, AudioDev> m_devices; // by device name
    bool m_testSink = false;
    std::thread m_thread;
};
