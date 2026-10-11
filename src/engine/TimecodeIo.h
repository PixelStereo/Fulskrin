#pragma once
// The inputs of a timecode clock: LTC (audio), MTC (MIDI), ATC (NDI, DeckLink). Each opens its device on its own
// thread and tells the clock what comes in. (What a clock sends: TimecodeOutputs.h.)

#include "TimecodeClock.h"

#include <functional>

class TimecodeIo
{
public:
    TimecodeIo(TimecodeClock *clock, const TimecodeSettings &s);
    ~TimecodeIo();
    QString status() const; // what comes in, as it changes (a DeckLink input's mode)

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

// An audio device opened for the timecode (miniaudio, 48 kHz, 32-bit float): LTC read from an input's first channel,
// or written to channels of an output. Its callback runs on the device's thread.
namespace tcaudio {

constexpr int kRate = 48000;

class Device
{
public:
    using Capture = std::function<void(const float *mono, int frames)>;
    using Playback = std::function<void(float *interleaved, int frames, int channels)>; // silence when it returns
    Device();
    ~Device(); // stopped: its callback has returned
    bool openCapture(const QString &name, Capture cb, QString *err);   // empty name: the default input
    bool openPlayback(const QString &name, Playback cb, QString *err); // with the device's own channels
    QString name() const;
    int channels() const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};

} // namespace tcaudio
