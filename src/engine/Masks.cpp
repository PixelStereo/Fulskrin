#include "Masks.h"

#include <QPainter>
#include <QRegularExpression>
#include <QXmlStreamReader>
#include <algorithm>
#include <cmath>

// ---------------------------------------------------------------------------
// A mask
// ---------------------------------------------------------------------------
QPainterPath Mask::path(QSizeF frame) const
{
    QPainterPath p;
    if (nodes.empty()) return p;
    auto px = [&](QPointF q) { return QPointF(q.x() * frame.width(), q.y() * frame.height()); };
    p.moveTo(px(nodes[0].p));
    const size_t n = nodes.size();
    for (size_t i = 0; i < n; ++i) {
        const MaskNode &a = nodes[i], &b = nodes[(i + 1) % n];
        if (a.out == a.p && b.in == b.p) p.lineTo(px(b.p));
        else p.cubicTo(px(a.out), px(b.in), px(b.p));
    }
    p.closeSubpath();
    return p;
}

void Mask::translate(QPointF d)
{
    for (MaskNode &k : nodes) {
        k.p += d;
        k.in += d;
        k.out += d;
    }
}

QRectF Mask::bounds() const
{
    if (nodes.empty()) return {};
    double x0 = 1e9, y0 = 1e9, x1 = -1e9, y1 = -1e9;
    for (const MaskNode &k : nodes)
        for (const QPointF &q : {k.p, k.in, k.out}) {
            x0 = std::min(x0, q.x());
            y0 = std::min(y0, q.y());
            x1 = std::max(x1, q.x());
            y1 = std::max(y1, q.y());
        }
    return QRectF(QPointF(x0, y0), QPointF(x1, y1));
}

bool Mask::operator==(const Mask &o) const
{
    return name == o.name && shape == o.shape && nodes == o.nodes && enabled == o.enabled && invert == o.invert
           && feather == o.feather && featherMode == o.featherMode && attach == o.attach;
}

static MaskNode corner(QPointF p) { return {p, p, p}; }

Mask Mask::rectangle(const QRectF &r)
{
    Mask m;
    m.shape = Shape::Rectangle;
    m.nodes = {corner(r.topLeft()), corner(r.topRight()), corner(r.bottomRight()), corner(r.bottomLeft())};
    return m;
}

Mask Mask::triangle(const QRectF &r)
{
    Mask m;
    m.shape = Shape::Triangle;
    m.nodes = {corner(QPointF(r.center().x(), r.top())), corner(r.bottomRight()), corner(r.bottomLeft())};
    return m;
}

Mask Mask::ellipse(const QRectF &r)
{
    Mask m;
    m.shape = Shape::Ellipse;
    const double k = 0.5522847498; // a quarter of a circle as one cubic
    const QPointF c = r.center();
    const double rx = r.width() / 2, ry = r.height() / 2;
    m.nodes = {{QPointF(c.x(), r.top()), QPointF(c.x() - k * rx, r.top()), QPointF(c.x() + k * rx, r.top())},
               {QPointF(r.right(), c.y()), QPointF(r.right(), c.y() - k * ry), QPointF(r.right(), c.y() + k * ry)},
               {QPointF(c.x(), r.bottom()), QPointF(c.x() + k * rx, r.bottom()), QPointF(c.x() - k * rx, r.bottom())},
               {QPointF(r.left(), c.y()), QPointF(r.left(), c.y() + k * ry), QPointF(r.left(), c.y() - k * ry)}};
    return m;
}

// --- JSON
static const char *shapeKey(Mask::Shape s)
{
    switch (s) {
    case Mask::Shape::Rectangle: return "rectangle";
    case Mask::Shape::Triangle: return "triangle";
    case Mask::Shape::Ellipse: return "ellipse";
    default: return "path";
    }
}
static const char *featherKey(Mask::Feather f)
{
    return f == Mask::Feather::Inner ? "inner" : f == Mask::Feather::Outer ? "outer" : "centered";
}
static const char *attachKey(Mask::Attach a) { return a == Mask::Attach::Composition ? "composition" : "surface"; }

QJsonObject Mask::toJson() const
{
    QJsonObject o;
    o["name"] = name;
    o["shape"] = shapeKey(shape);
    QJsonArray pts;
    for (const MaskNode &k : nodes)
        pts.append(QJsonArray{k.p.x(), k.p.y(), k.in.x(), k.in.y(), k.out.x(), k.out.y()});
    o["nodes"] = pts;
    o["enabled"] = enabled;
    o["invert"] = invert;
    o["feather"] = feather;
    o["feather_mode"] = featherKey(featherMode);
    o["attach"] = attachKey(attach);
    return o;
}

Mask Mask::fromJson(const QJsonObject &o)
{
    Mask m;
    m.name = o.value("name").toString();
    const QString s = o.value("shape").toString();
    m.shape = s == "rectangle" ? Shape::Rectangle : s == "triangle" ? Shape::Triangle : s == "ellipse" ? Shape::Ellipse : Shape::Path;
    for (const QJsonValue &v : o.value("nodes").toArray()) {
        const QJsonArray a = v.toArray();
        if (a.size() < 2) continue;
        MaskNode k;
        k.p = QPointF(a.at(0).toDouble(), a.at(1).toDouble());
        k.in = a.size() >= 4 ? QPointF(a.at(2).toDouble(), a.at(3).toDouble()) : k.p;
        k.out = a.size() >= 6 ? QPointF(a.at(4).toDouble(), a.at(5).toDouble()) : k.p;
        m.nodes.push_back(k);
    }
    m.enabled = o.value("enabled").toBool(true);
    m.invert = o.value("invert").toBool(false);
    m.feather = std::clamp(o.value("feather").toDouble(0), 0.0, 1.0);
    const QString f = o.value("feather_mode").toString();
    m.featherMode = f == "inner" ? Feather::Inner : f == "outer" ? Feather::Outer : Feather::Centered;
    m.attach = o.value("attach").toString() == "composition" ? Attach::Composition : Attach::Surface;
    return m;
}

namespace masks {

QJsonArray toJson(const std::vector<Mask> &m)
{
    QJsonArray a;
    for (const Mask &k : m) a.append(k.toJson());
    return a;
}

std::vector<Mask> fromJson(const QJsonArray &a)
{
    std::vector<Mask> out;
    for (const QJsonValue &v : a) out.push_back(Mask::fromJson(v.toObject()));
    return out;
}

QString uniqueName(const std::vector<Mask> &m, const QString &base)
{
    for (int i = 1;; ++i) {
        const QString n = QStringLiteral("%1-%2").arg(base).arg(i);
        if (std::none_of(m.begin(), m.end(), [&](const Mask &k) { return k.name == n; })) return n;
    }
}

bool any(const std::vector<Mask> &m, Mask::Attach attach)
{
    return std::any_of(m.begin(), m.end(), [&](const Mask &k) { return k.enabled && k.valid() && k.attach == attach; });
}

// ---------------------------------------------------------------------------
// Raster
// ---------------------------------------------------------------------------
// A box filter of 2r+1 along the rows or the columns, the edges extended (the frame's own edge stays sharp)
static void boxPass(std::vector<float> &v, int w, int h, int r, bool rows)
{
    if (r < 1) return;
    const int n = rows ? w : h, lines = rows ? h : w;
    std::vector<float> line(static_cast<size_t>(n)), out(static_cast<size_t>(n));
    const float inv = 1.0f / float(2 * r + 1);
    auto at = [&](int i) { return line[size_t(std::clamp(i, 0, n - 1))]; };
    for (int L = 0; L < lines; ++L) {
        for (int i = 0; i < n; ++i) line[size_t(i)] = rows ? v[size_t(L) * size_t(w) + size_t(i)] : v[size_t(i) * size_t(w) + size_t(L)];
        double sum = 0;
        for (int k = -r; k <= r; ++k) sum += at(k);
        for (int i = 0; i < n; ++i) {
            out[size_t(i)] = float(sum) * inv;
            sum += at(i + r + 1) - at(i - r);
        }
        for (int i = 0; i < n; ++i) {
            if (rows) v[size_t(L) * size_t(w) + size_t(i)] = out[size_t(i)];
            else v[size_t(i) * size_t(w) + size_t(L)] = out[size_t(i)];
        }
    }
}

// One mask's alpha: its shape, softened over `feather` of the short side
static void rasterOne(const Mask &k, int w, int h, std::vector<float> &out)
{
    QImage img(w, h, QImage::Format_Grayscale8);
    img.fill(0);
    {
        QPainter p(&img);
        p.setRenderHint(QPainter::Antialiasing);
        p.fillPath(k.path(QSizeF(w, h)), Qt::white);
    }
    out.resize(size_t(w) * size_t(h));
    for (int y = 0; y < h; ++y) {
        const uchar *row = img.constScanLine(y);
        for (int x = 0; x < w; ++x) out[size_t(y) * size_t(w) + size_t(x)] = row[x] / 255.0f;
    }
    // Two box passes of half the width make a smooth ramp about that width across the edge (centered on it).
    // Inner and outer: a ramp twice as wide, then its inner or outer half.
    double width = k.feather * std::min(w, h);
    if (k.featherMode != Mask::Feather::Centered) width *= 2;
    if (width < 1.5) return;
    const int r = std::max(1, int(std::lround((width / 2 - 1) / 2)));
    for (int pass = 0; pass < 2; ++pass) {
        boxPass(out, w, h, r, true);
        boxPass(out, w, h, r, false);
    }
    if (k.featherMode == Mask::Feather::Inner)
        for (float &a : out) a = std::clamp(2 * a - 1, 0.0f, 1.0f);
    else if (k.featherMode == Mask::Feather::Outer)
        for (float &a : out) a = std::clamp(2 * a, 0.0f, 1.0f);
}

QImage raster(const std::vector<Mask> &m, Mask::Attach attach, QSize size)
{
    if (size.isEmpty() || !any(m, attach)) return {};
    const int w = size.width(), h = size.height();
    bool adds = false;
    for (const Mask &k : m)
        if (k.enabled && k.valid() && k.attach == attach && !k.invert) adds = true;
    std::vector<float> acc(size_t(w) * size_t(h), adds ? 0.0f : 1.0f), one;
    // The masks not inverted add up, then the inverted ones cut
    for (int pass = 0; pass < 2; ++pass)
        for (const Mask &k : m) {
            if (!k.enabled || !k.valid() || k.attach != attach || k.invert != (pass == 1)) continue;
            rasterOne(k, w, h, one);
            if (pass == 0)
                for (size_t i = 0; i < acc.size(); ++i) acc[i] = std::max(acc[i], one[i]);
            else
                for (size_t i = 0; i < acc.size(); ++i) acc[i] *= 1.0f - one[i];
        }
    QImage out(w, h, QImage::Format_Grayscale8);
    for (int y = 0; y < h; ++y) {
        uchar *row = out.scanLine(y);
        for (int x = 0; x < w; ++x) row[x] = uchar(std::lround(std::clamp(acc[size_t(y) * size_t(w) + size_t(x)], 0.0f, 1.0f) * 255));
    }
    return out;
}

// ---------------------------------------------------------------------------
// SVG
// ---------------------------------------------------------------------------
static QString num(double v) { return QString::number(v, 'f', 3).remove(QRegularExpression(QStringLiteral("\\.?0+$"))); }

QString toSvg(const std::vector<Mask> &m, QSize frame)
{
    const double w = std::max(1, frame.width()), h = std::max(1, frame.height());
    QString s = QStringLiteral("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
                               "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%1\" height=\"%2\" viewBox=\"0 0 %1 %2\">\n")
                    .arg(num(w), num(h));
    auto pt = [&](QPointF q) { return num(q.x() * w) + ' ' + num(q.y() * h); };
    for (const Mask &k : m) {
        if (!k.valid()) continue;
        QString d = QStringLiteral("M ") + pt(k.nodes[0].p);
        const size_t n = k.nodes.size();
        for (size_t i = 0; i < n; ++i) {
            const MaskNode &a = k.nodes[i], &b = k.nodes[(i + 1) % n];
            if (a.out == a.p && b.in == b.p) {
                if (i + 1 < n) d += QStringLiteral(" L ") + pt(b.p);
            } else {
                d += QStringLiteral(" C ") + pt(a.out) + ' ' + pt(b.in) + ' ' + pt(b.p);
            }
        }
        d += QStringLiteral(" Z");
        QString name = k.name.toHtmlEscaped();
        s += QStringLiteral("  <path id=\"%1\" d=\"%2\" fill=\"%3\" data-invert=\"%4\" data-feather=\"%5\" "
                            "data-feather-mode=\"%6\" data-attach=\"%7\" data-enabled=\"%8\"/>\n")
                 .arg(name, d, k.invert ? QStringLiteral("#000000") : QStringLiteral("#ffffff"), k.invert ? "1" : "0",
                      num(k.feather), QString::fromLatin1(featherKey(k.featherMode)), QString::fromLatin1(attachKey(k.attach)),
                      k.enabled ? "1" : "0");
    }
    s += QStringLiteral("</svg>\n");
    return s;
}

namespace {

// Numbers and commands of a path's d attribute
class PathReader
{
public:
    explicit PathReader(const QString &d) : m_d(d) {}
    bool atCommand()
    {
        skip();
        return m_i < m_d.size() && m_d[m_i].isLetter() && m_d[m_i] != 'e' && m_d[m_i] != 'E';
    }
    QChar command() { return m_d[m_i++]; }
    bool atNumber()
    {
        skip();
        if (m_i >= m_d.size()) return false;
        const QChar c = m_d[m_i];
        return c.isDigit() || c == '-' || c == '+' || c == '.';
    }
    double number()
    {
        skip();
        const int start = m_i;
        if (m_i < m_d.size() && (m_d[m_i] == '-' || m_d[m_i] == '+')) ++m_i;
        bool dot = false;
        while (m_i < m_d.size() && (m_d[m_i].isDigit() || (m_d[m_i] == '.' && !dot))) {
            if (m_d[m_i] == '.') dot = true;
            ++m_i;
        }
        if (m_i < m_d.size() && (m_d[m_i] == 'e' || m_d[m_i] == 'E')) {
            ++m_i;
            if (m_i < m_d.size() && (m_d[m_i] == '-' || m_d[m_i] == '+')) ++m_i;
            while (m_i < m_d.size() && m_d[m_i].isDigit()) ++m_i;
        }
        return m_d.mid(start, m_i - start).toDouble();
    }
    bool done()
    {
        skip();
        return m_i >= m_d.size();
    }
    void skipOne() { ++m_i; } // something unknown

private:
    void skip()
    {
        while (m_i < m_d.size() && (m_d[m_i].isSpace() || m_d[m_i] == ',')) ++m_i;
    }
    const QString &m_d;
    int m_i = 0;
};

// Closed shapes of a path (in the SVG's units)
std::vector<std::vector<MaskNode>> parsePath(const QString &d, bool *approximated)
{
    std::vector<std::vector<MaskNode>> shapes;
    std::vector<MaskNode> cur;
    QPointF at, start, lastCubic, lastQuad;
    QChar prev;
    auto finish = [&] {
        if (cur.size() >= 2) {
            // A closing point on the first one: the same node
            if ((cur.back().p - cur.front().p).manhattanLength() < 1e-6) {
                cur.front().in = cur.back().in;
                cur.pop_back();
            }
            if (cur.size() >= 2) shapes.push_back(cur);
        }
        cur.clear();
    };
    auto add = [&](QPointF p, QPointF in) {
        cur.push_back({p, in, p});
        at = p;
    };
    PathReader r(d);
    QChar cmd;
    while (!r.done()) {
        if (r.atCommand()) cmd = r.command();
        else if (cmd.isNull()) {
            r.skipOne();
            continue;
        }
        const bool rel = cmd.isLower();
        const QChar c = cmd.toUpper();
        const QPointF o = rel ? at : QPointF();
        if (c == 'Z') {
            finish();
            at = start;
            prev = c;
            cmd = QChar();
            continue;
        }
        if (!r.atNumber()) { // a command without its numbers
            r.skipOne();
            continue;
        }
        if (c == 'M') {
            finish();
            const double x = r.number(), y = r.number();
            start = o + QPointF(x, y);
            add(start, start);
            cmd = rel ? 'l' : 'L'; // the next pairs are lines
        } else if (c == 'L') {
            const double x = r.number(), y = r.number();
            const QPointF p = o + QPointF(x, y);
            add(p, p);
        } else if (c == 'H') {
            const double x = r.number();
            const QPointF p(rel ? at.x() + x : x, at.y());
            add(p, p);
        } else if (c == 'V') {
            const double y = r.number();
            const QPointF p(at.x(), rel ? at.y() + y : y);
            add(p, p);
        } else if (c == 'C' || c == 'S') {
            QPointF c1;
            if (c == 'C') {
                const double x1 = r.number(), y1 = r.number();
                c1 = o + QPointF(x1, y1);
            } else {
                c1 = (prev == 'C' || prev == 'S') ? 2 * at - lastCubic : at;
            }
            const double x2 = r.number(), y2 = r.number(), x = r.number(), y = r.number();
            const QPointF c2 = o + QPointF(x2, y2), p = o + QPointF(x, y);
            if (!cur.empty()) cur.back().out = c1;
            add(p, c2);
            lastCubic = c2;
        } else if (c == 'Q' || c == 'T') {
            QPointF q;
            if (c == 'Q') {
                const double x1 = r.number(), y1 = r.number();
                q = o + QPointF(x1, y1);
            } else {
                q = (prev == 'Q' || prev == 'T') ? 2 * at - lastQuad : at;
            }
            const double x = r.number(), y = r.number();
            const QPointF p = o + QPointF(x, y);
            if (!cur.empty()) cur.back().out = at + 2.0 / 3.0 * (q - at);
            add(p, p + 2.0 / 3.0 * (q - p));
            lastQuad = q;
        } else if (c == 'A') { // arcs: a straight line to their end
            for (int k = 0; k < 5; ++k) r.number();
            const double x = r.number(), y = r.number();
            const QPointF p = o + QPointF(x, y);
            add(p, p);
            if (approximated) *approximated = true;
        } else {
            r.skipOne();
        }
        prev = c;
        if (cur.size() == 1 && c != 'M') start = cur.front().p;
    }
    finish();
    return shapes;
}

std::vector<double> numbers(const QString &s)
{
    std::vector<double> v;
    PathReader r(s);
    while (!r.done()) {
        if (r.atNumber()) v.push_back(r.number());
        else r.skipOne();
    }
    return v;
}

double length(const QString &s) // "120", "120px": other units are taken as pixels too
{
    QString t = s.trimmed();
    t.remove(QRegularExpression(QStringLiteral("[a-z%]+$")));
    return t.toDouble();
}

} // namespace

std::vector<Mask> fromSvg(const QByteArray &svg, QString *err)
{
    std::vector<Mask> out;
    QXmlStreamReader x(svg);
    QRectF frame;
    bool approximated = false, transformed = false;
    struct Raw {
        std::vector<MaskNode> nodes;
        QXmlStreamAttributes attrs;
        Mask::Shape shape;
    };
    std::vector<Raw> raws;
    while (!x.atEnd()) {
        x.readNext();
        if (!x.isStartElement()) continue;
        const QString tag = x.name().toString();
        const QXmlStreamAttributes a = x.attributes();
        if (a.hasAttribute(QStringLiteral("transform"))) transformed = true;
        if (tag == "svg" && frame.isEmpty()) {
            const std::vector<double> vb = numbers(a.value(QStringLiteral("viewBox")).toString());
            if (vb.size() == 4 && vb[2] > 0 && vb[3] > 0) frame = QRectF(vb[0], vb[1], vb[2], vb[3]);
            else {
                const double w = length(a.value(QStringLiteral("width")).toString()), h = length(a.value(QStringLiteral("height")).toString());
                if (w > 0 && h > 0) frame = QRectF(0, 0, w, h);
            }
            continue;
        }
        auto val = [&](const char *k) { return a.value(QLatin1String(k)).toString().toDouble(); };
        if (tag == "path") {
            for (auto &n : parsePath(a.value(QStringLiteral("d")).toString(), &approximated))
                raws.push_back({n, a, Mask::Shape::Path});
        } else if (tag == "rect") {
            const QRectF r(val("x"), val("y"), length(a.value(QStringLiteral("width")).toString()),
                           length(a.value(QStringLiteral("height")).toString()));
            if (r.width() > 0 && r.height() > 0) raws.push_back({Mask::rectangle(r).nodes, a, Mask::Shape::Rectangle});
        } else if (tag == "circle" || tag == "ellipse") {
            const double rx = tag == "circle" ? val("r") : val("rx"), ry = tag == "circle" ? val("r") : val("ry");
            if (rx > 0 && ry > 0)
                raws.push_back({Mask::ellipse(QRectF(val("cx") - rx, val("cy") - ry, 2 * rx, 2 * ry)).nodes, a, Mask::Shape::Ellipse});
        } else if (tag == "polygon" || tag == "polyline") {
            const std::vector<double> v = numbers(a.value(QStringLiteral("points")).toString());
            std::vector<MaskNode> n;
            for (size_t i = 0; i + 1 < v.size(); i += 2) n.push_back(corner(QPointF(v[i], v[i + 1])));
            if (n.size() >= 3) raws.push_back({n, a, n.size() == 3 ? Mask::Shape::Triangle : Mask::Shape::Path});
        }
    }
    if (x.hasError()) {
        if (err) *err = QStringLiteral("Not a readable SVG: ") + x.errorString();
        return {};
    }
    if (raws.empty()) {
        if (err) *err = QStringLiteral("No shape in this SVG (path, rect, circle, ellipse, polygon).");
        return {};
    }
    if (frame.isEmpty()) { // no size: the shapes' own bounds
        QRectF b;
        for (const Raw &r : raws)
            for (const MaskNode &k : r.nodes) b = b.isNull() ? QRectF(k.p, QSizeF(1e-9, 1e-9)) : b.united(QRectF(k.p, QSizeF(1e-9, 1e-9)));
        frame = b;
    }
    const double fw = std::max(1e-9, frame.width()), fh = std::max(1e-9, frame.height());
    auto norm = [&](QPointF q) { return QPointF((q.x() - frame.left()) / fw, (q.y() - frame.top()) / fh); };
    for (Raw &r : raws) {
        Mask m;
        m.shape = r.shape;
        for (MaskNode k : r.nodes) m.nodes.push_back({norm(k.p), norm(k.in), norm(k.out)});
        m.name = r.attrs.value(QStringLiteral("id")).toString();
        const QString fill = r.attrs.value(QStringLiteral("fill")).toString().trimmed().toLower();
        m.invert = r.attrs.hasAttribute(QStringLiteral("data-invert"))
                       ? r.attrs.value(QStringLiteral("data-invert")) == QLatin1String("1")
                       : (fill == "black" || fill == "#000" || fill == "#000000");
        m.feather = std::clamp(r.attrs.value(QStringLiteral("data-feather")).toString().toDouble(), 0.0, 1.0);
        const QString fm = r.attrs.value(QStringLiteral("data-feather-mode")).toString();
        m.featherMode = fm == "inner" ? Mask::Feather::Inner : fm == "outer" ? Mask::Feather::Outer : Mask::Feather::Centered;
        m.attach = r.attrs.value(QStringLiteral("data-attach")) == QLatin1String("composition") ? Mask::Attach::Composition
                                                                                                 : Mask::Attach::Surface;
        m.enabled = r.attrs.value(QStringLiteral("data-enabled")) != QLatin1String("0");
        out.push_back(m);
    }
    if (err) {
        QStringList notes;
        if (approximated) notes << QStringLiteral("arcs drawn as straight lines");
        if (transformed) notes << QStringLiteral("transforms ignored");
        *err = notes.join(QStringLiteral(", "));
    }
    return out;
}

} // namespace masks
