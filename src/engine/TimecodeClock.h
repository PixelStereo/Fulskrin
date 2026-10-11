#pragma once
// The clock of a Timecode layer: where its timecode comes from, and where it goes.
//  - Internal: the time of day, or a free run from its start (restarted on demand);
//  - LTC from an audio input, MTC from a MIDI input, ATC from an NDI source or a DeckLink input.
// An external timecode keeps running between the frames it receives (freewheel), and holds when it stops coming.
// What is shown can also be sent: LTC to an audio output, MTC to a MIDI output. The offset applies to both.
// Thread safe: the inputs write, the render thread reads.

#include "Timecode.h"

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
    bool sendLtc = false;
    QString ltcOutput;       // an audio output
    bool sendMtc = false;
    QString mtcOutput;       // a MIDI output

    static QString sourceName(Source s);
    QJsonObject toJson() const;
    static TimecodeSettings fromJson(const QJsonObject &o);
    bool operator==(const TimecodeSettings &o) const;
    bool operator!=(const TimecodeSettings &o) const { return !(*this == o); }
};

class TimecodeIo; // the inputs and outputs (TimecodeIo.cpp)

class TimecodeClock
{
public:
    explicit TimecodeClock(const TimecodeSettings &s);
    ~TimecodeClock();
    const TimecodeSettings &settings() const { return m_settings; }

    std::optional<Timecode> now() const; // what is shown now (none: nothing received yet)
    QString display() const;             // "10:00:00:00", or dashes
    QString status() const;              // where it comes from and how it goes ("LTC 25 fps", "No MTC"…)
    void restart();                      // free run: from zero (plus the offset) again

    // An external timecode came in (any thread)
    void received(const Timecode &tc);
    void setInputStatus(const QString &s); // what the input says (its device, an error)

private:
    TimecodeSettings m_settings;
    mutable std::mutex m_mutex;
    QElapsedTimer m_run;               // free run, and the time since the last timecode received
    std::optional<Timecode> m_last;    // the last one received
    qint64 m_lastAt = 0;               // when (m_run, ms)
    QString m_inputStatus;
    std::unique_ptr<TimecodeIo> m_io;
};
