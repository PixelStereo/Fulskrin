#pragma once
// The inputs of a timecode clock: LTC (audio), MTC (MIDI), ATC (NDI, DeckLink). Each opens its device on its own
// thread and tells the clock what comes in. (What a clock sends: TimecodeOutputs.h.)

#include "TimecodeClock.h"

class TimecodeIo
{
public:
    TimecodeIo(TimecodeClock *clock, const TimecodeSettings &s);
    ~TimecodeIo();
    QString status() const; // what is sent ("sending LTC", an error)

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
