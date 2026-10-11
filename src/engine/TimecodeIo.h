#pragma once
// The inputs and outputs of a timecode clock: LTC (audio), MTC (MIDI), ATC (NDI, DeckLink). Each opens its device on
// its own thread and tells the clock what comes in; the outputs send the clock's timecode.

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
