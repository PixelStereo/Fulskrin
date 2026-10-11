#include "TimecodeIo.h"

// The devices: not opened yet (the inputs and outputs come next). External sources say so.
struct TimecodeIo::Impl {
    QString status;
};

TimecodeIo::TimecodeIo(TimecodeClock *clock, const TimecodeSettings &s) : d(std::make_unique<Impl>())
{
    if (s.source != TimecodeSettings::Source::Internal)
        clock->setInputStatus(QStringLiteral("%1 input not available in this version").arg(TimecodeSettings::sourceName(s.source)));
    if (s.sendLtc || s.sendMtc) d->status = QStringLiteral("sending not available in this version");
}

TimecodeIo::~TimecodeIo() = default;

QString TimecodeIo::status() const { return d->status; }
