#include "TestCard.h"

#include <QFont>
#include <QFontMetricsF>
#include <QImage>
#include <QPainter>
#include <QPainterPath>
#include <algorithm>
#include <cmath>

namespace {

const char *kVs = "#version 330 core\nlayout(location=0) in vec2 a_pos; out vec2 v_uv;\n"
                  "void main(){ v_uv = a_pos*0.5+0.5; gl_Position = vec4(a_pos,0.0,1.0); }\n";

// The still card, and its motion: a sweep turning clockwise in the circle once every 4 s with a fading trail, and
// a white block going to and fro in its band. Coordinates in pixels, origin bottom left.
const char *kFs = R"(#version 330 core
uniform sampler2D u_tex;
uniform float u_time;
uniform vec2 u_size;
uniform vec3 u_circle; // center, radius
uniform vec4 u_band;   // x0, x1, y0, y1
in vec2 v_uv; out vec4 o;
void main() {
    vec2 p = v_uv * u_size;
    vec3 c = texture(u_tex, vec2(v_uv.x, 1.0 - v_uv.y)).rgb; // the still is stored top row first
    vec2 d = p - u_circle.xy;
    if (length(d) < u_circle.z) {
        float s = mod(-u_time * 1.5707963, 6.2831853);
        float behind = mod(atan(d.y, d.x) - s, 6.2831853);
        c = mix(c, vec3(0.25, 1.0, 0.45), 0.6 * exp(-behind * 2.5));
    }
    float w = (u_band.y - u_band.x) * 0.06;
    float x = u_band.x + (u_band.y - u_band.x - w) * (0.5 + 0.5 * sin(u_time * 2.0));
    if (p.y >= u_band.z && p.y <= u_band.w && p.x >= x && p.x <= x + w) c = vec3(1.0);
    o = vec4(c, 1.0);
}
)";

// Where the moving parts are, in pixels from the top left (the still card leaves room for them)
struct Geometry {
    double w, h, unit;      // unit: the short side
    QPointF center;
    double radius;
    QRectF bars, steps, band;
    double corner;
};

Geometry geometry(QSize size)
{
    Geometry g;
    g.w = std::max(1, size.width());
    g.h = std::max(1, size.height());
    g.unit = std::min(g.w, g.h);
    g.center = QPointF(g.w / 2, g.h / 2);
    g.radius = g.unit * 0.40;
    g.bars = QRectF(g.w * 0.16, g.h * 0.66, g.w * 0.68, g.h * 0.08);
    g.steps = QRectF(g.bars.left(), g.bars.bottom(), g.bars.width(), g.h * 0.05);
    g.band = QRectF(g.bars.left(), g.steps.bottom() + g.h * 0.015, g.bars.width(), g.h * 0.035);
    g.corner = g.unit * 0.13;
    return g;
}

} // namespace

QImage TestCard::still(const QString &name, QSize size)
{
    const Geometry g = geometry(size);
    QImage img(int(g.w), int(g.h), QImage::Format_RGBA8888);
    img.fill(QColor(28, 28, 30));
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);

    // Grid of squares through the center, the center cross a little brighter, the diagonals
    const double step = std::max(8.0, std::round(g.h / 10));
    p.setPen(QPen(QColor(85, 85, 90), std::max(1.0, g.unit / 1080)));
    for (double x = std::fmod(g.center.x(), step); x < g.w; x += step) p.drawLine(QPointF(x, 0), QPointF(x, g.h));
    for (double y = std::fmod(g.center.y(), step); y < g.h; y += step) p.drawLine(QPointF(0, y), QPointF(g.w, y));
    p.setPen(QPen(QColor(150, 150, 155), std::max(1.0, g.unit / 720)));
    p.drawLine(QPointF(g.center.x(), 0), QPointF(g.center.x(), g.h));
    p.drawLine(QPointF(0, g.center.y()), QPointF(g.w, g.center.y()));
    p.setPen(QPen(QColor(110, 110, 115), std::max(1.0, g.unit / 1080)));
    p.drawLine(QPointF(0, 0), QPointF(g.w, g.h));
    p.drawLine(QPointF(g.w, 0), QPointF(0, g.h));

    // The circle: round only if the picture's aspect is right
    p.setPen(QPen(Qt::white, std::max(2.0, g.unit / 360)));
    p.setBrush(Qt::NoBrush);
    p.drawEllipse(g.center, g.radius, g.radius);

    // Color bars and grey steps
    const QColor bars[8] = {Qt::white, QColor(255, 255, 0), QColor(0, 255, 255), QColor(0, 255, 0),
                            QColor(255, 0, 255), QColor(255, 0, 0), QColor(0, 0, 255), Qt::black};
    p.setPen(Qt::NoPen);
    for (int i = 0; i < 8; ++i)
        p.fillRect(QRectF(g.bars.left() + g.bars.width() * i / 8, g.bars.top(), g.bars.width() / 8 + 1, g.bars.height()), bars[i]);
    for (int i = 0; i <= 10; ++i) {
        const int v = int(std::lround(255.0 * i / 10));
        p.fillRect(QRectF(g.steps.left() + g.steps.width() * i / 11, g.steps.top(), g.steps.width() / 11 + 1, g.steps.height()),
                   QColor(v, v, v));
    }
    p.fillRect(g.band, QColor(55, 55, 60)); // where the block moves

    // The four corners: numbered and colored, an arrow to the corner
    struct Corner {
        QPointF at;   // its corner
        QPointF dir;  // towards the inside
        QColor color;
        QString label;
    };
    const Corner corners[4] = {{{0, 0}, {1, 1}, QColor(230, 40, 40), QStringLiteral("1")},
                               {{g.w, 0}, {-1, 1}, QColor(40, 200, 70), QStringLiteral("2")},
                               {{g.w, g.h}, {-1, -1}, QColor(50, 110, 255), QStringLiteral("3")},
                               {{0, g.h}, {1, -1}, QColor(240, 210, 30), QStringLiteral("4")}};
    QFont cf(QStringLiteral("Helvetica"));
    cf.setBold(true);
    cf.setPixelSize(std::max(8, int(g.corner * 0.55)));
    p.setFont(cf);
    for (const Corner &c : corners) {
        QPainterPath tri; // a triangle filling the corner
        tri.moveTo(c.at);
        tri.lineTo(c.at + QPointF(c.dir.x() * g.corner * 1.6, 0));
        tri.lineTo(c.at + QPointF(0, c.dir.y() * g.corner * 1.6));
        tri.closeSubpath();
        p.fillPath(tri, c.color);
        const QPointF labelCenter = c.at + QPointF(c.dir.x() * g.corner * 0.5, c.dir.y() * g.corner * 0.5);
        p.setPen(c.color == QColor(240, 210, 30) ? Qt::black : Qt::white);
        p.drawText(QRectF(labelCenter - QPointF(g.corner, g.corner), QSizeF(g.corner * 2, g.corner * 2)), Qt::AlignCenter, c.label);
    }

    // A frame on the outermost pixels: whatever is cropped shows
    p.setPen(QPen(Qt::white, 1));
    p.setRenderHint(QPainter::Antialiasing, false);
    p.drawRect(QRectF(0, 0, g.w - 1, g.h - 1));
    p.setRenderHint(QPainter::Antialiasing);

    // Its name and size, on a dark plate in the middle
    QFont nf(QStringLiteral("Helvetica"));
    nf.setBold(true);
    nf.setPixelSize(std::max(10, int(g.unit * 0.11)));
    QFont sf(QStringLiteral("Helvetica"));
    sf.setPixelSize(std::max(8, int(g.unit * 0.045)));
    const QString sizeText = QStringLiteral("%1 × %2").arg(size.width()).arg(size.height());
    const QFontMetricsF nm(nf), sm(sf);
    const double plateW = std::min(g.w * 0.9, std::max(nm.horizontalAdvance(name), sm.horizontalAdvance(sizeText)) + g.unit * 0.08);
    const double plateH = nm.height() + sm.height() + g.unit * 0.04;
    const QRectF plate(g.center.x() - plateW / 2, g.center.y() - plateH / 2, plateW, plateH);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 200));
    p.drawRoundedRect(plate, g.unit * 0.015, g.unit * 0.015);
    p.setPen(Qt::white);
    p.setFont(nf);
    p.drawText(QRectF(plate.left(), plate.top() + g.unit * 0.015, plate.width(), nm.height()), Qt::AlignCenter,
               nm.elidedText(name, Qt::ElideRight, plate.width() - g.unit * 0.04));
    p.setPen(QColor(200, 200, 205));
    p.setFont(sf);
    p.drawText(QRectF(plate.left(), plate.top() + g.unit * 0.015 + nm.height(), plate.width(), sm.height()),
               Qt::AlignCenter, sizeText);
    p.end();
    return img;
}

bool TestCard::init()
{
    if (m_program) return true;
    QString log;
    m_program = compileProgram(QString::fromLatin1(kVs), QString::fromLatin1(kFs), &log);
    if (!m_program) {
        qWarning("Test card shader: %s", qPrintable(log));
        return false;
    }
    auto f = gl();
    m_texLoc = f->glGetUniformLocation(m_program, "u_tex");
    m_timeLoc = f->glGetUniformLocation(m_program, "u_time");
    m_sizeLoc = f->glGetUniformLocation(m_program, "u_size");
    m_circleLoc = f->glGetUniformLocation(m_program, "u_circle");
    m_bandLoc = f->glGetUniformLocation(m_program, "u_band");
    m_clock.start();
    return true;
}

void TestCard::render(const QString &name, QSize size, RenderTarget &target, GLuint quadVao)
{
    if (size.isEmpty() || !init()) return;
    const QString key = QStringLiteral("%1|%2x%3").arg(name).arg(size.width()).arg(size.height());
    auto it = m_stills.find(key);
    if (it == m_stills.end()) {
        if (m_stills.size() >= 8) release(); // names or sizes no longer shown
        if (!init()) return;
        const QImage img = still(name, size);
        it = m_stills.emplace(key, Texture2D()).first;
        it->second.upload(img.constBits(), img.width(), img.height());
    }
    const Geometry g = geometry(size);
    auto f = gl();
    target.ensure(size.width(), size.height());
    target.bind();
    f->glDisable(GL_BLEND);
    f->glUseProgram(m_program);
    f->glActiveTexture(GL_TEXTURE0);
    f->glBindTexture(GL_TEXTURE_2D, it->second.tex);
    f->glUniform1i(m_texLoc, 0);
    f->glUniform1f(m_timeLoc, float(m_clock.elapsed() / 1000.0));
    f->glUniform2f(m_sizeLoc, float(g.w), float(g.h));
    f->glUniform3f(m_circleLoc, float(g.center.x()), float(g.h - g.center.y()), float(g.radius));
    f->glUniform4f(m_bandLoc, float(g.band.left()), float(g.band.right()), float(g.h - g.band.bottom()), float(g.h - g.band.top()));
    f->glBindVertexArray(quadVao);
    f->glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    f->glBindVertexArray(0);
}

void TestCard::release()
{
    for (auto &[key, tex] : m_stills) tex.destroy();
    m_stills.clear();
    if (m_program) gl()->glDeleteProgram(m_program);
    m_program = 0;
}
