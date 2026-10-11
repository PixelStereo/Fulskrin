#pragma once
// The clock of a Timecode layer: where its timecode comes from, and where it goes.
//  - Internal: the time of day, or a transport (Free run): play, pause, seek, in / out points and a play mode,
//    like a media's, over a day of timecode;
//  - LTC from an audio input, MTC from a MIDI input, ATC from an NDI source or a DeckLink input.
// An external timecode keeps running between the frames it receives (freewheel), and holds when it stops coming.
// What is shown is also sent to the layer's timecode output (one of the composition's, TimecodeOutputs.h), whose patch
// says where it goes and so in which protocol. The offset applies to both.
// Thread safe: the inputs and the outputs' thread read and write, the render thread reads.

#include "Timecode.h"
#include "Timeline.h"

#include <QElapsedTimer>
#include <QJsonObject>
#include <QString>
#include <memory>
#include <mutex>
#include <optional>

struct TimecodeSettings {
    enum class Source { Internal = 0, Ltc, Mtc, AtcNdi, AtcDeckLink };
    enum class Internal { TimeOfDay = 0, FreeRun };
    Source source = Source::Internal;
    Internal internal = Internal::TimeOfDay;
    TcRate rate = TcRate::Fps25;
    int64_t offset = 0;      // frames added to what is shown and sent
    QString input;           // LTC: an audio input; MTC: a MIDI input; ATC: an NDI source or a DeckLink device
    quint64 output = 0;      // the timecode output it drives (0: none); one layer per output

    static QString sourceName(Source s);
    QJsonObject toJson() const;
    static TimecodeSettings fromJson(const QJsonObject &o);
    bool operator==(const TimecodeSettings &o) const;
    bool operator!=(const TimecodeSettings &o) const { return !(*this == o); }
};

class TimecodeIo; // the inputs (TimecodeIo.cpp)

// What the transport does at its out point (backwards: its in point), as a media's play modes
enum class TcPlayMode { OneShot = 0, Loop, PingPong, Stop };
QString tcPlayModeKey(TcPlayMode m);                   // "oneshot", "loop", "pingpong", "stop"
TcPlayMode tcPlayModeFromKey(const QString &k);

// The clock at an instant, as the outputs send it: a frame count with its fraction (offset included)
struct TcReading {
    double frames = 0;    // since 00:00:00:00, real frames (drop frame: the numbers skipped are not counted)
    TcRate rate = TcRate::Fps25;
    bool running = false; // advancing (else held: a pause, a source that stopped coming)
    bool forward = true;  // a ping-pong coming back runs backwards
    Timecode timecode() const;
};

class TimecodeClock
{
public:
    explicit TimecodeClock(const TimecodeSettings &s);
    ~TimecodeClock();
    const TimecodeSettings &settings() const { return m_settings; }

    std::optional<Timecode> now() const; // what is shown now (none: nothing received yet)
    std::optional<TcReading> reading() const; // the same, with the fraction of the frame and how it moves
    QString display() const;             // "10:00:00:00", or dashes
    QString status() const;              // where it comes from and how it goes ("LTC 25 fps", "No MTC"…)
    void restart();                      // free run: from its in point, playing

    // Transport of the free run, in seconds of timecode (before the offset). Out < 0: the end of the day.
    bool usesTransport() const;          // internal free run (the time of day and external sources have none)
    bool playing() const;
    void setPlaying(bool on);            // played to its end (one-shot, stop): starts again from its in point
    double position() const;
    void seek(double seconds);
    void rewind();                       // to its in point, forwards
    double inPoint() const;
    double outPoint() const;
    void setInOut(double in, double out);
    TcPlayMode playMode() const;
    void setPlayMode(TcPlayMode m);
    void takeTransport(const TimecodeClock &other); // where another one was (its settings changed: a new clock)
    // Running now: playing an internal clock, or receiving (sent as running, else as a held position)
    bool running() const;

    // An external timecode came in (any thread)
    void received(const Timecode &tc);
    void setInputStatus(const QString &s); // what the input says (its device, an error)

    void setTestTime(qint64 ms); // tests: its time is this many ms since it was made (-1: real time again)

private:
    TimecodeSettings m_settings;
    mutable std::mutex m_mutex;
    QElapsedTimer m_run;               // free run, and the time since the last timecode received
    std::optional<Timecode> m_last;    // the last one received
    qint64 m_lastAt = 0;               // when (m_run, ms)
    QString m_inputStatus;
    std::unique_ptr<TimecodeIo> m_io;
    qint64 m_testMs = -1;
    qint64 msLocked() const;           // m_run, or the test's time

    // Free run: a clock running with real time while playing, turned into a position by a Timeline
    Timeline timelineLocked() const;
    double clockLocked() const;
    int dirLocked() const;                       // the way it goes now (+1, -1: a ping-pong coming back)
    void repositionLocked(double position, int dir);
    bool m_playing = true;
    double m_c0 = 0;      // clock (seconds) when it last paused or was repositioned
    qint64 m_runStart = 0; // m_run when it last started playing (or was repositioned while playing)
    double m_origin = 0;  // position at clock 0
    int m_dir = 1;        // direction at m_origin
    double m_in = 0, m_out = -1;
    TcPlayMode m_mode = TcPlayMode::Loop;
};
