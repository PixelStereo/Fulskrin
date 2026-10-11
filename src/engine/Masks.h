#pragma once
// Masks of a layer, a group or a viewport: closed shapes (Bézier paths) that keep the picture inside them, or cut it
// out (inverted), with a soft edge (feathering). Each mask is drawn in one of two frames:
//  - Surface: the picture's own frame (0..1 over it, origin top left): it follows the mapping, its warp included;
//  - Composition: the composition (0..1, origin top left): the picture is cut where it lands (layers and groups).
// A viewport's masks are in its own picture (what goes to its screen).
// Several masks: the ones not inverted are added (nothing but them shows); the inverted ones then cut holes.
// Without any mask not inverted, the whole picture shows and the inverted ones cut it.

#include <QImage>
#include <QJsonArray>
#include <QJsonObject>
#include <QPainterPath>
#include <QPointF>
#include <QRectF>
#include <QSize>
#include <QString>
#include <vector>

struct MaskNode {
    QPointF p;       // the point
    QPointF in, out; // its Bézier handles (absolute); equal to p: a corner
    bool operator==(const MaskNode &o) const { return p == o.p && in == o.in && out == o.out; }
};

struct Mask {
    enum class Shape { Path, Rectangle, Triangle, Ellipse }; // how it was made (its icon)
    enum class Feather { Centered, Inner, Outer };           // where the soft edge goes, against the shape's edge
    enum class Attach { Surface, Composition };

    QString name;
    Shape shape = Shape::Path;
    std::vector<MaskNode> nodes; // closed: the last joins the first
    bool enabled = true, invert = false;
    double feather = 0; // width of the soft edge: a fraction of the frame's short side (0 to 1)
    Feather featherMode = Feather::Centered;
    Attach attach = Attach::Surface;

    bool valid() const { return nodes.size() >= 2; }
    QPainterPath path(QSizeF frame) const; // in pixels of a frame of this size
    void translate(QPointF d);
    QRectF bounds() const; // of its points and handles (normalized)
    bool operator==(const Mask &o) const;
    bool operator!=(const Mask &o) const { return !(*this == o); }

    // New shapes, in a frame's normalized box
    static Mask rectangle(const QRectF &r);
    static Mask triangle(const QRectF &r);
    static Mask ellipse(const QRectF &r);

    QJsonObject toJson() const;
    static Mask fromJson(const QJsonObject &o);
};

namespace masks {

QJsonArray toJson(const std::vector<Mask> &m);
std::vector<Mask> fromJson(const QJsonArray &a);
QString uniqueName(const std::vector<Mask> &m, const QString &base = QStringLiteral("Mask")); // "Mask-1", "Mask-2"…

// The alpha the masks drawn in `attach` give over a frame of `size` pixels: Grayscale8, top row first, 255 where the
// picture shows. Null image: no such mask (nothing to apply).
QImage raster(const std::vector<Mask> &m, Mask::Attach attach, QSize size);
bool any(const std::vector<Mask> &m, Mask::Attach attach); // a mask there to apply

// SVG: the masks as paths over a frame of `frame` pixels (white; the inverted ones black), and back. Reading takes
// path, rect, circle, ellipse, polygon and polyline, each closed shape a mask (normalized by the SVG's size).
QString toSvg(const std::vector<Mask> &m, QSize frame);
std::vector<Mask> fromSvg(const QByteArray &svg, QString *err);

} // namespace masks
