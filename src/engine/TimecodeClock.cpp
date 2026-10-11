#include "TimecodeClock.h"
#include "TimecodeIo.h"

#include <QTime>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Settings
// ---------------------------------------------------------------------------
QString TimecodeSettings::sourceName(Source s)
{
    switch (s) {
    case Source::Internal: return QStringLiteral("Internal");
    case Source::Ltc: return QStringLiteral("LTC");
    case Source::Mtc: return QStringLiteral("MTC");
    case Source::AtcNdi: return QStringLiteral("ATC (NDI)");
    case Source::AtcDeckLink: return QStringLiteral("ATC (DeckLink)");
    }
    return {};
}

static const char *const kSourceKeys[] = {"internal", "ltc", "mtc", "atc_ndi", "atc_decklink"};
static const char *const kRateKeys[] = {"24", "25", "29.97df", "30"};

QJsonObject TimecodeSettings::toJson() const
{
    QJsonObject o;
    o["source"] = kSourceKeys[int(source)];
    o["internal"] = internal == Internal::FreeRun ? "free_run" : "time_of_day";
    o["rate"] = kRateKeys[int(rate)];
    o["offset"] = double(offset);
    if (!input.isEmpty()) o["input"] = input;
    if (output) o["output"] = QString::number(output); // ids are strings in files
    return o;
}

TimecodeSettings TimecodeSettings::fromJson(const QJsonObject &o)
{
    TimecodeSettings s;
    const QString src = o.value("source").toString();
    for (int i = 0; i < 5; ++i)
        if (src == QLatin1String(kSourceKeys[i])) s.source = Source(i);
    s.internal = o.value("internal").toString() == "free_run" ? Internal::FreeRun : Internal::TimeOfDay;
    const QString rate = o.value("rate").toString();
    for (int i = 0; i < 4; ++i)
        if (rate == QLatin1String(kRateKeys[i])) s.rate = TcRate(i);
    s.offset = int64_t(o.value("offset").toDouble(0));
    s.input = o.value("input").toString();
    s.output = o.value("output").toString().toULongLong();
    return s;
}

bool TimecodeSettings::operator==(const TimecodeSettings &o) const
{
    return source == o.source && internal == o.internal && rate == o.rate && offset == o.offset && input == o.input
           && output == o.output;
}

static const char *const kPlayKeys[] = {"oneshot", "loop", "pingpong", "stop"};

QString tcPlayModeKey(TcPlayMode m) { return QString::fromLatin1(kPlayKeys[int(m)]); }

TcPlayMode tcPlayModeFromKey(const QString &k)
{
    for (int i = 0; i < 4; ++i)
        if (k == QLatin1String(kPlayKeys[i])) return TcPlayMode(i);
    return TcPlayMode::Loop;
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------
TimecodeClock::TimecodeClock(const TimecodeSettings &s) : m_settings(s)
{
    m_run.start();
    m_runStart = msLocked();
    m_io = std::make_unique<TimecodeIo>(this, s);
}

TimecodeClock::~TimecodeClock() { m_io.reset(); } // its threads stop before the clock goes

void TimecodeClock::restart()
{
    rewind();
    setPlaying(true);
}

// ---------------------------------------------------------------------------
// Transport (free run)
// ---------------------------------------------------------------------------
static constexpr double kDay = 24.0 * 3600.0;

Timeline TimecodeClock::timelineLocked() const
{
    Timeline t;
    t.duration = kDay;
    t.mode = m_mode == TcPlayMode::Loop ? Timeline::Loop : m_mode == TcPlayMode::PingPong ? Timeline::PingPong : Timeline::Once;
    t.origin = m_origin;
    t.dir = m_dir;
    t.in = m_in;
    t.out = m_out;
    return t;
}

double TimecodeClock::clockLocked() const { return m_c0 + (m_playing ? (msLocked() - m_runStart) / 1000.0 : 0.0); }

qint64 TimecodeClock::msLocked() const { return m_testMs >= 0 ? m_testMs : m_run.elapsed(); }

void TimecodeClock::setTestTime(qint64 ms)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_testMs = ms;
}

int TimecodeClock::dirLocked() const { return timelineLocked().legAt(clockLocked()).forward ? 1 : -1; }

void TimecodeClock::repositionLocked(double position, int dir)
{
    m_dir = dir;
    const Timeline t = timelineLocked();
    m_origin = std::clamp(position, t.lo(), t.hi());
    m_c0 = 0;
    m_runStart = msLocked();
}

bool TimecodeClock::usesTransport() const
{
    return m_settings.source == TimecodeSettings::Source::Internal && m_settings.internal == TimecodeSettings::Internal::FreeRun;
}

bool TimecodeClock::playing() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_playing;
}

void TimecodeClock::setPlaying(bool on)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const bool ended = timelineLocked().ended(clockLocked());
    if (on && ended) repositionLocked(timelineLocked().lo(), 1); // played to its end: from its in point again
    if (on == m_playing) return;
    if (on) {
        m_runStart = msLocked();
    } else {
        m_c0 = clockLocked();
    }
    m_playing = on;
}

double TimecodeClock::position() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return timelineLocked().position(clockLocked());
}

void TimecodeClock::seek(double seconds)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    repositionLocked(seconds, dirLocked());
}

void TimecodeClock::rewind()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    repositionLocked(timelineLocked().lo(), 1);
}

double TimecodeClock::inPoint() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_in;
}

double TimecodeClock::outPoint() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_out;
}

void TimecodeClock::setInOut(double in, double out)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    in = std::clamp(in, 0.0, kDay);
    if (out >= 0) out = std::clamp(out, in, kDay);
    if (out >= kDay - 1e-9) out = -1;
    if (in == m_in && out == m_out) return;
    const double pos = timelineLocked().position(clockLocked());
    const int dir = dirLocked();
    m_in = in;
    m_out = out;
    repositionLocked(pos, dir); // kept within the new range
}

TcPlayMode TimecodeClock::playMode() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_mode;
}

void TimecodeClock::setPlayMode(TcPlayMode m)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m == m_mode) return;
    const double pos = timelineLocked().position(clockLocked());
    const int dir = dirLocked();
    m_mode = m;
    repositionLocked(pos, dir); // the same place, the new mode
}

void TimecodeClock::takeTransport(const TimecodeClock &o)
{
    if (&o == this) return;
    std::scoped_lock lk(m_mutex, o.m_mutex);
    m_in = o.m_in;
    m_out = o.m_out;
    m_mode = o.m_mode;
    m_playing = o.m_playing;
    m_c0 = 0;
    repositionLocked(o.timelineLocked().position(o.clockLocked()), o.dirLocked());
}

bool TimecodeClock::running() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (m_settings.source != TimecodeSettings::Source::Internal) return m_last && msLocked() - m_lastAt < 1000;
    if (m_settings.internal == TimecodeSettings::Internal::TimeOfDay) return true;
    return m_playing && !timelineLocked().ended(clockLocked());
}

void TimecodeClock::received(const Timecode &tc)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_last = tc;
    m_lastAt = msLocked();
}

void TimecodeClock::setInputStatus(const QString &s)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_inputStatus = s;
}

std::optional<TcReading> TimecodeClock::reading() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    TcReading r;
    r.rate = m_settings.rate;
    if (m_settings.source == TimecodeSettings::Source::Internal) {
        double secs;
        if (m_settings.internal == TimecodeSettings::Internal::TimeOfDay) {
            secs = QTime::currentTime().msecsSinceStartOfDay() / 1000.0;
            r.running = true;
        } else {
            const Timeline t = timelineLocked();
            const double c = clockLocked();
            if (m_mode == TcPlayMode::Stop && t.ended(c)) return std::nullopt; // stop: played to its end, nothing sent
            secs = t.position(c);
            r.running = m_playing && !t.ended(c) && t.hi() > t.lo();
            r.forward = t.legAt(c).forward;
        }
        r.frames = secs * Timecode::realFps(r.rate) + double(m_settings.offset);
    } else {
        if (!m_last) return std::nullopt;
        // Between two frames received: it runs on (freewheel) for a second, then holds
        const qint64 since = msLocked() - m_lastAt;
        r.rate = m_last->rate;
        r.running = since < 1000;
        r.frames = double(m_last->toFrames() + m_settings.offset);
        if (r.running) r.frames += since / 1000.0 * Timecode::realFps(r.rate);
    }
    const double day = double(Timecode::framesPerDay(r.rate));
    r.frames = std::fmod(r.frames, day);
    if (r.frames < 0) r.frames += day;
    return r;
}

std::optional<Timecode> TimecodeClock::now() const
{
    const std::optional<TcReading> r = reading();
    if (!r) return std::nullopt;
    return r->timecode();
}

QString TimecodeClock::display() const
{
    const std::optional<Timecode> t = now();
    if (t) return t->toString();
    return Timecode::dropFrame(m_settings.rate) ? QStringLiteral("--:--:--;--") : QStringLiteral("--:--:--:--");
}

QString TimecodeClock::status() const
{
    QString s;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        const QString name = TimecodeSettings::sourceName(m_settings.source);
        if (m_settings.source == TimecodeSettings::Source::Internal) {
            s = (m_settings.internal == TimecodeSettings::Internal::TimeOfDay ? QStringLiteral("Time of day")
                                                                             : QStringLiteral("Free run"))
                + QStringLiteral(" · ") + Timecode::rateName(m_settings.rate);
        } else if (!m_last) {
            s = QStringLiteral("No %1 received").arg(name);
        } else {
            const bool live = msLocked() - m_lastAt < 1000;
            s = name + QStringLiteral(" · ") + Timecode::rateName(m_last->rate)
                + (live ? QStringLiteral(" · receiving") : QStringLiteral(" · no signal (held)"));
        }
        if (!m_inputStatus.isEmpty()) s += QStringLiteral(" · ") + m_inputStatus;
    }
    const QString out = m_io ? m_io->status() : QString();
    if (!out.isEmpty()) s += QStringLiteral(" · ") + out;
    return s;
}
