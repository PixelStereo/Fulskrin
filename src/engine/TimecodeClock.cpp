#include "TimecodeClock.h"
#include "TimecodeIo.h"

#include <QTime>
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
    o["send_ltc"] = sendLtc;
    if (!ltcOutput.isEmpty()) o["ltc_output"] = ltcOutput;
    o["send_mtc"] = sendMtc;
    if (!mtcOutput.isEmpty()) o["mtc_output"] = mtcOutput;
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
    s.sendLtc = o.value("send_ltc").toBool(false);
    s.ltcOutput = o.value("ltc_output").toString();
    s.sendMtc = o.value("send_mtc").toBool(false);
    s.mtcOutput = o.value("mtc_output").toString();
    return s;
}

bool TimecodeSettings::operator==(const TimecodeSettings &o) const
{
    return source == o.source && internal == o.internal && rate == o.rate && offset == o.offset && input == o.input
           && sendLtc == o.sendLtc && ltcOutput == o.ltcOutput && sendMtc == o.sendMtc && mtcOutput == o.mtcOutput;
}

// ---------------------------------------------------------------------------
// Clock
// ---------------------------------------------------------------------------
TimecodeClock::TimecodeClock(const TimecodeSettings &s) : m_settings(s)
{
    m_run.start();
    m_io = std::make_unique<TimecodeIo>(this, s);
}

TimecodeClock::~TimecodeClock() { m_io.reset(); } // its threads stop before the clock goes

void TimecodeClock::restart()
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_run.restart();
    m_lastAt = 0;
}

void TimecodeClock::received(const Timecode &tc)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_last = tc;
    m_lastAt = m_run.elapsed();
}

void TimecodeClock::setInputStatus(const QString &s)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    m_inputStatus = s;
}

std::optional<Timecode> TimecodeClock::now() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const TcRate rate = m_settings.rate;
    if (m_settings.source == TimecodeSettings::Source::Internal) {
        const double secs = m_settings.internal == TimecodeSettings::Internal::TimeOfDay
                                ? QTime::currentTime().msecsSinceStartOfDay() / 1000.0
                                : m_run.elapsed() / 1000.0;
        const Timecode t = Timecode::fromSeconds(secs, rate);
        return Timecode::fromFrames(t.toFrames() + m_settings.offset, rate);
    }
    if (!m_last) return std::nullopt;
    // Between two frames received: it runs on (freewheel) for a second, then holds
    const qint64 since = m_run.elapsed() - m_lastAt;
    int64_t n = m_last->toFrames();
    if (since < 1000) n += int64_t(std::floor(since / 1000.0 * Timecode::realFps(m_last->rate)));
    return Timecode::fromFrames(n + m_settings.offset, m_last->rate);
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
            const bool live = m_run.elapsed() - m_lastAt < 1000;
            s = name + QStringLiteral(" · ") + Timecode::rateName(m_last->rate)
                + (live ? QStringLiteral(" · receiving") : QStringLiteral(" · no signal (held)"));
        }
        if (!m_inputStatus.isEmpty()) s += QStringLiteral(" · ") + m_inputStatus;
    }
    const QString out = m_io ? m_io->status() : QString();
    if (!out.isEmpty()) s += QStringLiteral(" · ") + out;
    return s;
}
