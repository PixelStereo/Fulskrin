#include "Timecode.h"

#include <QRegularExpression>
#include <QStringList>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// Timecode
// ---------------------------------------------------------------------------
QString Timecode::rateName(TcRate r)
{
    switch (r) {
    case TcRate::Fps24: return QStringLiteral("24 fps");
    case TcRate::Fps25: return QStringLiteral("25 fps");
    case TcRate::Fps2997Df: return QStringLiteral("29.97 fps drop frame");
    case TcRate::Fps30: return QStringLiteral("30 fps");
    }
    return {};
}

bool Timecode::valid() const
{
    if (h < 0 || h > 23 || m < 0 || m > 59 || s < 0 || s > 59 || f < 0 || f >= nominalFps(rate)) return false;
    // Drop frame: frames 0 and 1 do not exist at the start of each minute but every tenth
    return !(dropFrame(rate) && s == 0 && f < 2 && m % 10 != 0);
}

int64_t Timecode::framesPerDay(TcRate r)
{
    if (r == TcRate::Fps2997Df) return 24LL * 6 * 17982; // 17982 frames each ten minutes
    return 24LL * 3600 * Timecode::nominalFps(r);
}

int64_t Timecode::toFrames() const
{
    const int fps = nominalFps(rate);
    int64_t n = (int64_t(h) * 3600 + m * 60 + s) * fps + f;
    if (dropFrame(rate)) {
        const int64_t minutes = int64_t(h) * 60 + m;
        n -= 2 * (minutes - minutes / 10);
    }
    return n;
}

Timecode Timecode::fromFrames(int64_t n, TcRate r)
{
    const int64_t day = framesPerDay(r);
    n %= day;
    if (n < 0) n += day;
    if (dropFrame(r)) { // back to the numbers shown: the two skipped each minute but every tenth added
        const int64_t tens = n / 17982, rest = n % 17982;
        n += 18 * tens + (rest > 1 ? 2 * ((rest - 2) / 1798) : 0);
    }
    const int fps = nominalFps(r);
    Timecode t;
    t.rate = r;
    t.f = int(n % fps);
    t.s = int((n / fps) % 60);
    t.m = int((n / (int64_t(fps) * 60)) % 60);
    t.h = int((n / (int64_t(fps) * 3600)) % 24);
    return t;
}

Timecode Timecode::fromSeconds(double s, TcRate r)
{
    return fromFrames(int64_t(std::floor(s * realFps(r) + 1e-6)), r);
}

QString Timecode::toString() const
{
    return QStringLiteral("%1:%2:%3%4%5")
        .arg(h, 2, 10, QLatin1Char('0'))
        .arg(m, 2, 10, QLatin1Char('0'))
        .arg(s, 2, 10, QLatin1Char('0'))
        .arg(dropFrame(rate) ? QLatin1Char(';') : QLatin1Char(':'))
        .arg(f, 2, 10, QLatin1Char('0'));
}

std::optional<Timecode> Timecode::parse(const QString &str, TcRate r)
{
    const QStringList parts = str.trimmed().split(QRegularExpression(QStringLiteral("[:;.]")));
    if (parts.size() != 4) return std::nullopt;
    int v[4];
    for (int i = 0; i < 4; ++i) {
        bool ok = false;
        v[i] = parts[i].toInt(&ok);
        if (!ok) return std::nullopt;
    }
    Timecode t{v[0], v[1], v[2], v[3], r};
    if (!t.valid()) return std::nullopt;
    return t;
}

// ---------------------------------------------------------------------------
// LTC
// ---------------------------------------------------------------------------
namespace ltc {

static const uint8_t kSync[16] = {0, 0, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 0, 1}; // bits 64 to 79

static void put(std::array<uint8_t, 80> &b, int at, int count, int value)
{
    for (int i = 0; i < count; ++i) b[size_t(at + i)] = uint8_t((value >> i) & 1);
}

static int get(const std::array<uint8_t, 80> &b, int at, int count)
{
    int v = 0;
    for (int i = 0; i < count; ++i) v |= (b[size_t(at + i)] & 1) << i;
    return v;
}

std::array<uint8_t, 80> encodeFrame(const Timecode &tc)
{
    std::array<uint8_t, 80> b{};
    put(b, 0, 4, tc.f % 10);
    put(b, 8, 2, tc.f / 10);
    b[10] = Timecode::dropFrame(tc.rate) ? 1 : 0;
    put(b, 16, 4, tc.s % 10);
    put(b, 24, 3, tc.s / 10);
    put(b, 32, 4, tc.m % 10);
    put(b, 40, 3, tc.m / 10);
    put(b, 48, 4, tc.h % 10);
    put(b, 56, 2, tc.h / 10);
    for (int i = 0; i < 16; ++i) b[size_t(64 + i)] = kSync[i];
    // Polarity: an even number of zeros in the frame (bit 59 at 25 fps, bit 27 otherwise)
    const int polarity = tc.rate == TcRate::Fps25 ? 59 : 27;
    int zeros = 0;
    for (uint8_t v : b) zeros += v == 0;
    if (zeros % 2) b[size_t(polarity)] = 1;
    return b;
}

std::optional<Timecode> decodeFrame(const std::array<uint8_t, 80> &b, TcRate hint)
{
    for (int i = 0; i < 16; ++i)
        if (b[size_t(64 + i)] != kSync[i]) return std::nullopt;
    Timecode t;
    t.rate = b[10] ? TcRate::Fps2997Df : (hint == TcRate::Fps2997Df ? TcRate::Fps30 : hint);
    t.f = get(b, 0, 4) + 10 * get(b, 8, 2);
    t.s = get(b, 16, 4) + 10 * get(b, 24, 3);
    t.m = get(b, 32, 4) + 10 * get(b, 40, 3);
    t.h = get(b, 48, 4) + 10 * get(b, 56, 2);
    if (get(b, 0, 4) > 9 || get(b, 16, 4) > 9 || get(b, 32, 4) > 9 || get(b, 48, 4) > 9 || !t.valid()) return std::nullopt;
    return t;
}

void Encoder::frame(const Timecode &tc, std::vector<float> &out)
{
    const std::array<uint8_t, 80> bits = encodeFrame(tc);
    const double half = double(m_rate) / (Timecode::realFps(tc.rate) * 160.0); // samples per half bit
    for (uint8_t bit : bits)
        for (int k = 0; k < 2; ++k) {
            if (k == 0 || bit) m_level = -m_level; // a transition at the start of each bit, and in the middle of a 1
            const double end = m_phase + half;
            const int n = int(std::floor(end)) - int(std::floor(m_phase));
            for (int i = 0; i < n; ++i) out.push_back(m_level * m_amp);
            m_phase = end - std::floor(end);
        }
}

Decoder::Decoder(int sampleRate) : m_rate(sampleRate), m_period(double(sampleRate) / (25.0 * 80.0)) {}

double Decoder::fps() const { return double(m_rate) / (80.0 * m_period); }

void Decoder::push(const float *samples, size_t n)
{
    for (size_t i = 0; i < n; ++i) {
        const float x = samples[i];
        m_count += 1;
        const bool high = m_high ? x > -0.01f : x > 0.01f; // a little hysteresis against noise around zero
        if (high != m_high) {
            edge(m_count);
            m_count = 0;
            m_high = high;
        }
    }
}

void Decoder::edge(double length)
{
    if (length < 0.75 * m_period) { // half a bit
        m_period += 0.02 * (2 * length - m_period);
        if (m_half) {
            m_half = false;
            bit(1);
        } else {
            m_half = true;
        }
        return;
    }
    if (length > 1.6 * m_period) { // a gap (silence, a cut): the frame starts again
        m_half = false;
        m_have = 0;
        return;
    }
    m_period += 0.02 * (length - m_period);
    m_half = false;
    bit(0);
}

void Decoder::bit(uint8_t b)
{
    if (m_have < 80) {
        m_bits[size_t(m_have++)] = b;
    } else {
        std::rotate(m_bits.begin(), m_bits.begin() + 1, m_bits.end());
        m_bits[79] = b;
    }
    if (m_have < 80) return;
    // Its rate: told by the drop-frame flag, else measured
    const double f = fps();
    TcRate hint = m_hint;
    if (f < 24.5) hint = TcRate::Fps24;
    else if (f < 27.5) hint = TcRate::Fps25;
    else hint = TcRate::Fps30;
    if (auto t = decodeFrame(m_bits, hint)) {
        m_last = t;
        m_have = 0;
    }
}

std::optional<Timecode> Decoder::take()
{
    std::optional<Timecode> t = m_last;
    m_last.reset();
    return t;
}

} // namespace ltc

// ---------------------------------------------------------------------------
// MTC
// ---------------------------------------------------------------------------
namespace mtc {

uint8_t quarterFrame(const Timecode &tc, int piece)
{
    int v = 0;
    switch (piece & 7) {
    case 0: v = tc.f & 0x0F; break;
    case 1: v = (tc.f >> 4) & 0x01; break;
    case 2: v = tc.s & 0x0F; break;
    case 3: v = (tc.s >> 4) & 0x03; break;
    case 4: v = tc.m & 0x0F; break;
    case 5: v = (tc.m >> 4) & 0x03; break;
    case 6: v = tc.h & 0x0F; break;
    case 7: v = ((tc.h >> 4) & 0x01) | (int(tc.rate) << 1); break;
    }
    return uint8_t(((piece & 7) << 4) | v);
}

std::vector<uint8_t> fullFrame(const Timecode &tc)
{
    return {0xF0, 0x7F, 0x7F, 0x01, 0x01, uint8_t((int(tc.rate) << 5) | (tc.h & 0x1F)), uint8_t(tc.m), uint8_t(tc.s), uint8_t(tc.f), 0xF7};
}

bool Decoder::push(const uint8_t *msg, size_t len)
{
    if (len >= 2 && msg[0] == 0xF1) {
        const int piece = (msg[1] >> 4) & 7, nibble = msg[1] & 0x0F;
        m_pieces[size_t(piece)] = nibble;
        m_received = piece == 0 ? 1 : m_received | (1 << piece);
        if (piece != 7 || m_received != 0xFF) return false;
        Timecode t;
        t.rate = TcRate((m_pieces[7] >> 1) & 3);
        t.f = m_pieces[0] | ((m_pieces[1] & 1) << 4);
        t.s = m_pieces[2] | ((m_pieces[3] & 3) << 4);
        t.m = m_pieces[4] | ((m_pieces[5] & 3) << 4);
        t.h = m_pieces[6] | ((m_pieces[7] & 1) << 4);
        if (!t.valid()) return false;
        // The eight pieces take two frames to come: the timecode they hold is two frames old by now
        m_current = Timecode::fromFrames(t.toFrames() + 2, t.rate);
        return true;
    }
    if (len >= 10 && msg[0] == 0xF0 && msg[1] == 0x7F && msg[3] == 0x01 && msg[4] == 0x01) { // full frame
        Timecode t;
        t.rate = TcRate((msg[5] >> 5) & 3);
        t.h = msg[5] & 0x1F;
        t.m = msg[6];
        t.s = msg[7];
        t.f = msg[8];
        if (!t.valid()) return false;
        m_current = t;
        m_received = 0;
        return true;
    }
    return false;
}

} // namespace mtc
