#include "OscCodec.h"

#include <QtEndian>
#include <QHash>
#include <algorithm>
#include <cstring>
#include <functional>

// ---------------------------------------------------------------------------
// OSC encoding / decoding
// ---------------------------------------------------------------------------

namespace osc {

static void pad(QByteArray &b)
{
    while (b.size() % 4) b.append('\0');
}

static void putString(QByteArray &b, const QString &s)
{
    b.append(s.toUtf8());
    b.append('\0');
    pad(b);
}

template <typename T> static void putBE(QByteArray &b, T v)
{
    char raw[sizeof(T)];
    qToBigEndian(v, raw);
    b.append(raw, int(sizeof(T)));
}

QByteArray encode(const Message &m)
{
    QByteArray b;
    putString(b, m.address);
    putString(b, QStringLiteral(",") + m.types);
    for (int k = 0; k < m.types.size(); ++k) {
        const QVariant v = m.args.value(k);
        switch (m.types[k].toLatin1()) {
        case 'i': putBE<qint32>(b, qint32(v.toInt())); break;
        case 'h': putBE<qint64>(b, v.toLongLong()); break;
        case 'f': {
            const float f = v.toFloat();
            quint32 u;
            std::memcpy(&u, &f, 4);
            putBE<quint32>(b, u);
            break;
        }
        case 'd': {
            const double d = v.toDouble();
            quint64 u;
            std::memcpy(&u, &d, 8);
            putBE<quint64>(b, u);
            break;
        }
        case 's':
        case 'S': putString(b, v.toString()); break;
        case 'b': {
            const QByteArray blob = v.toByteArray();
            putBE<qint32>(b, qint32(blob.size()));
            b.append(blob);
            pad(b);
            break;
        }
        case 'c': { // a character, as a 32-bit int
            const QString c = v.toString();
            putBE<qint32>(b, c.isEmpty() ? 0 : qint32(c.at(0).unicode()));
            break;
        }
        case 'r':
        case 'm': putBE<quint32>(b, quint32(v.toLongLong())); break; // color, MIDI: 4 bytes
        case 't': putBE<qint64>(b, v.toLongLong()); break;          // time tag
        default: break; // T F N I: no data
        }
    }
    return b;
}

namespace {
// Reads a packet from the network: every length comes from it, so no check may overflow (sizes as qsizetype, the
// room left compared, never pos + n)
struct Reader {
    const QByteArray &b;
    qsizetype pos = 0;
    bool ok = true;
    qsizetype left() const { return pos <= b.size() ? b.size() - pos : 0; }
    bool need(qsizetype n)
    {
        if (n < 0 || pos > b.size() || n > b.size() - pos) ok = false;
        return ok;
    }
    void pad() { pos = std::min<qsizetype>((pos + 3) & ~qsizetype(3), b.size()); } // to a multiple of 4
    QString string()
    {
        if (pos >= b.size()) {
            ok = false;
            return {};
        }
        const qsizetype end = b.indexOf('\0', pos);
        if (end < 0) {
            ok = false;
            return {};
        }
        const QString s = QString::fromUtf8(b.constData() + pos, end - pos);
        pos = end + 1;
        pad();
        return s;
    }
    template <typename T> T be()
    {
        if (!need(qsizetype(sizeof(T)))) return T();
        const T v = qFromBigEndian<T>(b.constData() + pos);
        pos += qsizetype(sizeof(T));
        return v;
    }
};
} // namespace

static constexpr qsizetype kMaxPacket = 1 << 20; // OSC over UDP stays far below; TCP and WebSocket frames too
static constexpr int kMaxArgs = 1024;
static constexpr int kMaxDepth = 8;          // nested bundles
static constexpr size_t kMaxMessages = 1024; // cap per-packet work across all nested bundles

static bool decodeInto(const QByteArray &packet, std::vector<Message> &out, int depth)
{
    if (packet.isEmpty() || packet.size() > kMaxPacket || depth > kMaxDepth) return false;
    if (packet.startsWith(QByteArray("#bundle", 8))) {
        if (packet.size() < 16) return false;
        Reader r{packet};
        r.pos = 16; // "#bundle\0" + time tag
        while (r.ok && r.pos < packet.size()) {
            const qint32 size = r.be<qint32>();
            if (!r.ok || size <= 0 || !r.need(size)) return false;
            if (!decodeInto(packet.mid(r.pos, size), out, depth + 1)) return false;
            r.pos += size;
        }
        return r.ok;
    }
    if (packet[0] != '/') return false;
    Reader r{packet};
    Message m;
    m.address = r.string();
    if (!r.ok) return false;
    if (r.pos < packet.size() && packet[r.pos] == ',') {
        const QString tags = r.string();
        if (!r.ok || tags.size() - 1 > kMaxArgs) return false;
        m.types = tags.mid(1);
        for (const QChar c : std::as_const(m.types)) {
            switch (c.toLatin1()) {
            case 'i': m.args << QVariant(int(r.be<qint32>())); break;
            case 'h': m.args << QVariant(qlonglong(r.be<qint64>())); break;
            case 'f': {
                const quint32 u = r.be<quint32>();
                float f;
                std::memcpy(&f, &u, 4);
                m.args << QVariant(double(f));
                break;
            }
            case 'd': {
                const quint64 u = r.be<quint64>();
                double d;
                std::memcpy(&d, &u, 8);
                m.args << QVariant(d);
                break;
            }
            case 's':
            case 'S': m.args << QVariant(r.string()); break;
            case 'b': {
                const qint32 n = r.be<qint32>();
                if (!r.ok || !r.need(n)) return false;
                m.args << QVariant(packet.mid(r.pos, n));
                r.pos += n;
                r.pad();
                break;
            }
            case 'c': m.args << QVariant(QString(QChar(char16_t(r.be<qint32>() & 0xffff)))); break;
            case 'r':
            case 'm': m.args << QVariant(qlonglong(r.be<quint32>())); break;
            case 't': m.args << QVariant(qlonglong(r.be<qint64>())); break;
            case 'T': m.args << QVariant(true); break;
            case 'F': m.args << QVariant(false); break;
            default: m.args << QVariant(); break; // N, I, arrays: no value
            }
            if (!r.ok) return false;
        }
    }
    if (out.size() >= kMaxMessages) return false; // reject oversized bundle before consuming extra CPU/memory
    out.push_back(std::move(m));
    return true;
}

bool decode(const QByteArray &packet, std::vector<Message> &out) { return decodeInto(packet, out, 0); }

// One segment: * ? [set] {alt,alt}. This matcher also receives patterns
// from the network: naive recursion across repeated '*' has exponential
// complexity (e.g. 40 '*' followed by an unmatched 'z'). Memoize each
// (pattern position, address position) pair, and share an explicit operation
// budget across nested {alternatives} and all path segments.
static constexpr int kMaxMatchChars = 8192;
static constexpr int kMaxMatchSegment = 4096;
static constexpr int kMaxMatchSteps = 150000;

static bool matchSeg(const QString &p, int startPi, const QString &s, int startSi, int &remaining)
{
    if (p.size() > kMaxMatchSegment || s.size() > kMaxMatchSegment || remaining <= 0) return false;
    QHash<quint64, quint8> memo;
    std::function<bool(int, int)> visit = [&](int pi, int si) -> bool {
        if (--remaining < 0) return false;
        const quint64 key = (quint64(quint32(pi)) << 32) | quint32(si);
        const auto found = memo.constFind(key);
        if (found != memo.cend()) return found.value() != 0;

        bool result = false;
        if (pi >= p.size()) {
            result = si == s.size();
        } else {
            const QChar c = p[pi];
            if (c == '*') {
                for (int k = si; k <= s.size(); ++k)
                    if (visit(pi + 1, k)) {
                        result = true;
                        break;
                    }
            } else if (c == '?') {
                result = si < s.size() && visit(pi + 1, si + 1);
            } else if (c == '[') {
                const int end = p.indexOf(']', pi);
                if (end >= 0 && si < s.size()) {
                    QString set = p.mid(pi + 1, end - pi - 1);
                    const bool negate = set.startsWith('!');
                    if (negate) set.remove(0, 1);
                    bool inside = false;
                    for (int k = 0; k < set.size(); ++k) {
                        if (k + 2 < set.size() && set[k + 1] == '-') {
                            inside |= s[si] >= set[k] && s[si] <= set[k + 2];
                            k += 2;
                        } else {
                            inside |= s[si] == set[k];
                        }
                    }
                    if (inside != negate) result = visit(end + 1, si + 1);
                }
            } else if (c == '{') {
                const int end = p.indexOf('}', pi);
                if (end >= 0) {
                    const QString rest = p.mid(end + 1);
                    for (const QString &alt : p.mid(pi + 1, end - pi - 1).split(',')) {
                        if (remaining <= 0) break;
                        if (matchSeg(alt + rest, 0, s, si, remaining)) {
                            result = true;
                            break;
                        }
                    }
                }
            } else {
                result = si < s.size() && s[si] == c && visit(pi + 1, si + 1);
            }
        }
        memo.insert(key, result ? 1 : 0);
        return result;
    };
    return visit(startPi, startSi);
}

bool match(const QString &pattern, const QString &address)
{
    // A malformed OSC request must never create unbounded split lists or
    // exhaust a control thread's CPU on hostile wildcard combinations.
    if (pattern.size() > kMaxMatchChars || address.size() > kMaxMatchChars) return false;
    if (pattern == address) return true; // the common literal path costs almost nothing
    const QStringList p = pattern.split('/'), a = address.split('/');
    if (p.size() != a.size()) return false;
    int remaining = kMaxMatchSteps;
    for (int k = 0; k < p.size(); ++k)
        if (!matchSeg(p[k], 0, a[k], 0, remaining)) return false;
    return true;
}

QString safeName(const QString &name)
{
    QString s = name.trimmed();
    for (QChar &c : s)
        if (c.isSpace() || QStringLiteral("#*,/?[]{}!").contains(c)) c = '_';
    return s.isEmpty() ? QStringLiteral("_") : s;
}

QStringList uniqueSegments(const QStringList &names)
{
    QStringList out;
    QHash<QString, int> used;
    for (const QString &n : names) {
        const QString base = safeName(n);
        QString seg = base;
        if (int k = used.value(base)) seg += QStringLiteral("_%1").arg(k + 1);
        used[base] += 1;
        out << seg;
    }
    return out;
}

} // namespace osc
