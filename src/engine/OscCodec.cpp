#include "OscCodec.h"

#include <QtEndian>
#include <algorithm>
#include <cstring>

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
    out.push_back(m);
    return true;
}

bool decode(const QByteArray &packet, std::vector<Message> &out) { return decodeInto(packet, out, 0); }

// One segment: * ? [set] {alt,alt}. Matched in O(pattern × address) whatever the pattern: a backtracking matcher
// takes exponential time on runs of '*' ("/**************a"), and any machine on the network can send one (the
// fuzzer found it: a 1-minute run stuck for over 20 minutes). The pattern becomes tokens, then the positions of the
// address it can have reached are carried from one token to the next (two rows).
namespace {
struct Token {
    enum Kind { Char, Any, Star, Set, Alt } kind = Char;
    QChar c;
    QString set; // Set: its characters and ranges (a-z), without the '!'
    bool negate = false;
    QStringList alts; // Alt: literal alternatives (OSC 1.0: "{foo,bar}")
};

bool inSet(const Token &t, QChar ch)
{
    bool in = false;
    for (int k = 0; k < t.set.size(); ++k) {
        if (k + 2 < t.set.size() && t.set[k + 1] == '-') {
            in |= ch >= t.set[k] && ch <= t.set[k + 2];
            k += 2;
        } else {
            in |= ch == t.set[k];
        }
    }
    return in != t.negate;
}

// false: malformed ('[' or '{' never closed), which matches nothing
bool tokenize(const QString &p, std::vector<Token> &out)
{
    for (int pi = 0; pi < p.size(); ++pi) {
        const QChar c = p[pi];
        Token t;
        if (c == '*') {
            if (!out.empty() && out.back().kind == Token::Star) continue; // "**" is "*"
            t.kind = Token::Star;
        } else if (c == '?') {
            t.kind = Token::Any;
        } else if (c == '[') {
            const int end = p.indexOf(']', pi);
            if (end < 0) return false;
            t.kind = Token::Set;
            t.set = p.mid(pi + 1, end - pi - 1);
            t.negate = t.set.startsWith('!');
            if (t.negate) t.set.remove(0, 1);
            pi = end;
        } else if (c == '{') {
            const int end = p.indexOf('}', pi);
            if (end < 0) return false;
            t.kind = Token::Alt;
            t.alts = p.mid(pi + 1, end - pi - 1).split(',');
            pi = end;
        } else {
            t.c = c;
        }
        out.push_back(std::move(t));
    }
    return true;
}
} // namespace

static bool matchSeg(const QString &p, const QString &s)
{
    static const QString special = QStringLiteral("*?[{");
    if (std::none_of(p.begin(), p.end(), [](QChar c) { return special.contains(c); })) return p == s; // the usual case
    std::vector<Token> tokens;
    if (!tokenize(p, tokens)) return false;
    const int n = int(s.size());
    std::vector<char> at(size_t(n) + 1, 0), next(size_t(n) + 1, 0);
    at[0] = 1;
    for (const Token &t : tokens) {
        std::fill(next.begin(), next.end(), 0);
        bool any = false;
        if (t.kind == Token::Star) { // from the first position reached, every one after it
            bool on = false;
            for (int si = 0; si <= n; ++si) {
                on = on || at[size_t(si)];
                next[size_t(si)] = on;
                any = any || on;
            }
        } else {
            for (int si = 0; si <= n; ++si) {
                if (!at[size_t(si)]) continue;
                switch (t.kind) {
                case Token::Char:
                    if (si < n && s[si] == t.c) next[size_t(si) + 1] = any = true;
                    break;
                case Token::Any:
                    if (si < n) next[size_t(si) + 1] = any = true;
                    break;
                case Token::Set:
                    if (si < n && inSet(t, s[si])) next[size_t(si) + 1] = any = true;
                    break;
                case Token::Alt:
                    for (const QString &alt : t.alts)
                        if (QStringView(s).mid(si).startsWith(alt)) next[size_t(si) + size_t(alt.size())] = any = true;
                    break;
                case Token::Star:
                    break;
                }
            }
        }
        if (!any) return false;
        at.swap(next);
    }
    return at[size_t(n)];
}

bool match(const QString &pattern, const QString &address)
{
    const QStringList p = pattern.split('/'), a = address.split('/');
    if (p.size() != a.size()) return false;
    for (int k = 0; k < p.size(); ++k)
        if (!matchSeg(p[k], a[k])) return false;
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
