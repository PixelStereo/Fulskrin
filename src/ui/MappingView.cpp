#include "MappingView.h"

#include <algorithm>
#include <limits>
#include "Commands.h"
#include "Engine.h"
#include "Widgets.h"

#include <QUndoStack>

#include <QDoubleSpinBox>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QSignalBlocker>
#include <QLabel>
#include <QMouseEvent>
#include <QNativeGestureEvent>
#include <QToolButton>
#include <QPainter>
#include <QWheelEvent>
#include <cmath>
#include <functional>

static constexpr double kPi = 3.14159265358979323846;

static const QColor kSelected(255, 150, 40);
static const QColor kHandle(255, 255, 255, 230);
static const QColor kOutline(255, 150, 40, 220);
static const QColor kOutlineOther(255, 255, 255, 70);
static const QColor kGrid(255, 255, 255, 110);
static const QColor kViewport(120, 190, 255, 230);
static const QColor kGuide(255, 70, 200, 230);

MappingView::MappingView(Engine *engine, QWidget *parent) : QOpenGLWidget(parent), m_engine(engine)
{
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(false);
    setMinimumSize(320, 200);

    // Zoom buttons, top right of the preview
    m_zoomBar = new QWidget(this);
    m_zoomBar->setStyleSheet("QWidget { background:rgba(20,20,22,200); border-radius:4px; }"
                             "QToolButton { color:#ddd; min-width:24px; padding:2px 6px; }"
                             "QToolButton:hover { background:#3a3a3e; } QToolButton:checked { background:#2f2f33; }"
                             "QLabel { color:#aaa; padding:0 4px; }");
    auto *h = new QHBoxLayout(m_zoomBar);
    h->setContentsMargins(3, 2, 3, 2);
    h->setSpacing(1);
    auto button = [&](const QString &text, const QString &tip) {
        auto *b = new QToolButton;
        b->setText(text);
        b->setToolTip(tip);
        b->setFocusPolicy(Qt::NoFocus);
        b->setAutoRaise(true);
        h->addWidget(b);
        return b;
    };
    QToolButton *out = button(QStringLiteral("−"), QStringLiteral("Zoom out (mouse wheel)"));
    m_zoomLabel = new QLabel;
    m_zoomLabel->setMinimumWidth(44);
    m_zoomLabel->setAlignment(Qt::AlignCenter);
    h->addWidget(m_zoomLabel);
    QToolButton *in = button(QStringLiteral("+"), QStringLiteral("Zoom in (mouse wheel)"));
    QToolButton *fit = button(QStringLiteral("Fit"), QStringLiteral("Whole composition in view\n"
                                                                     "Pan: middle button, Alt/⌥ + drag, or two fingers on a trackpad"));
    m_magnetButton = button(QString(), QStringLiteral("Magnetism: dragged points, layers and viewports are caught by the "
                                                      "edges, centers and corners around them.\nHold Ctrl / ⌘ while "
                                                      "dragging to move freely. On or off at start: Settings."));
    m_magnetButton->setIcon(magnet::icon());
    m_magnetButton->setCheckable(true);
    m_magnetButton->setChecked(magnet::enabled());
    connect(m_magnetButton, &QToolButton::toggled, this, [](bool on) { magnet::setEnabled(on); });
    connect(magnet::notifier(), &magnet::Notifier::changed, this, [this] {
        QSignalBlocker b(m_magnetButton);
        m_magnetButton->setChecked(magnet::enabled());
    });
    connect(theme::notifier(), &theme::Notifier::changed, this, [this] {
        m_magnetButton->setIcon(magnet::icon());
        m_autoSelectButton->setIcon(autoselect::icon());
    });
    m_autoSelectButton = button(QString(), QStringLiteral("Auto-select: a click in the preview selects the layer or the "
                                                          "viewport under it.\nOff: they are chosen in the list; a click "
                                                          "only moves the one selected."));
    m_autoSelectButton->setIcon(autoselect::icon());
    m_autoSelectButton->setCheckable(true);
    m_autoSelectButton->setChecked(autoselect::enabled());
    connect(m_autoSelectButton, &QToolButton::toggled, this, [](bool on) { autoselect::setEnabled(on); });
    connect(autoselect::notifier(), &autoselect::Notifier::changed, this, [this] {
        QSignalBlocker b(m_autoSelectButton);
        m_autoSelectButton->setChecked(autoselect::enabled());
    });
    // The same canvas supports two deliberate tools: layer mapping or mask node editing.
    // Mask nodes never accidentally move while the layer tool is active.
    m_maskModeButton = button(QStringLiteral("Layer"), QStringLiteral("Layer mode: select and move layers. "
                                                      "Press to switch to Mask mode: click, snap and align mask nodes."));
    m_maskModeButton->setCheckable(true);
    connect(m_maskModeButton, &QToolButton::toggled, this, &MappingView::setMaskEditMode);
    for (int i = 0; i < 4; ++i) {
        static const char *icons[4] = {"←", "→", "↑", "↓"};
        static const char *labels[4] = {"Align mask nodes left", "Align mask nodes right",
                                        "Align mask nodes top", "Align mask nodes bottom"};
        m_alignMask[i] = button(QString::fromUtf8(icons[i]), QString::fromLatin1(labels[i]) +
                                QStringLiteral(" (Ctrl/Command-click at least two points first)"));
        m_alignMask[i]->setEnabled(false);
        connect(m_alignMask[i], &QToolButton::clicked, this, [this, i] { alignMaskNodes(i); });
    }
    connect(out, &QToolButton::clicked, this, [this] { zoomBy(1 / 1.25); });
    connect(in, &QToolButton::clicked, this, [this] { zoomBy(1.25); });
    connect(fit, &QToolButton::clicked, this, &MappingView::zoomToFit);
    updateZoomLabel();

    // Position of the point clicked last, in composition pixels: typed, dragged left / right, or the wheel
    m_coordBar = new QWidget(this);
    m_coordBar->setStyleSheet("QWidget#coords { background:rgba(20,20,22,210); border-radius:4px; }"
                              "QLabel { color:#bbb; padding:0 3px; }");
    m_coordBar->setObjectName("coords");
    auto *c = new QHBoxLayout(m_coordBar);
    c->setContentsMargins(6, 3, 6, 3);
    c->setSpacing(4);
    m_coordName = new QLabel;
    c->addWidget(m_coordName);
    for (QDoubleSpinBox **box : {&m_coordX, &m_coordY}) {
        auto *label = new QLabel(box == &m_coordX ? QStringLiteral("X") : QStringLiteral("Y"));
        auto *b = new NumberBox;
        b->setRange(-100000, 100000);
        b->setDecimals(1);
        b->setSuffix(QStringLiteral(" px"));
        b->setKeyboardTracking(false);
        b->setFixedWidth(96);
        b->setToolTip(QStringLiteral("Position of the point in the composition, in pixels (0, 0: top left). "
                                     "The other selected points move with it."));
        c->addWidget(label);
        c->addWidget(b);
        *box = b;
        connect(b, qOverload<double>(&QDoubleSpinBox::valueChanged), this, [this] { typeCoordinate(); });
    }
    m_coordBar->hide();
}

void MappingView::setMaskEditMode(bool on)
{
    if (m_maskEditMode == on) return;
    m_maskEditMode = on;
    m_maskDrag = MaskDrag::None;
    m_maskSelection.clear();
    m_maskNode = -1;
    m_selection.clear();
    m_dragLayer = m_dragHandle = m_dragScale = m_dragRotate = false;
    if (m_maskModeButton) {
        QSignalBlocker block(m_maskModeButton);
        m_maskModeButton->setChecked(on);
        m_maskModeButton->setText(on ? QStringLiteral("Mask") : QStringLiteral("Layer"));
    }
    update();
}

void MappingView::refreshCoordinateBar()
{
    QPointF out;
    QString name;
    bool show = false;
    bool mask = false;
    int aligned = 0;
    {
        Engine::Lock lk(&m_engine->mutex());
        if (m_maskEditMode) {
            Mask *k = editedMask();
            mask = k && m_maskNode >= 0 && m_maskNode < int(k->nodes.size()) &&
                   !m_engine->isLocked(m_layer);
            if (mask) {
                const QSize frame = maskInComposition(k) ? m_engine->compositionSize()
                                       : m_engine->maskFrame(m_engine->layerId(m_layer));
                const QPointF p = k->nodes[size_t(m_maskNode)].p;
                out = QPointF(p.x() * frame.width(), p.y() * frame.height());
                name = QStringLiteral("Mask node %1").arg(m_maskNode + 1);
                aligned = int(m_maskSelection.size());
                show = true;
            }
        } else {
            Mapping *m = mapping();
            show = m && !isViewport(m_layer) && !m_engine->isLocked(m_layer) &&
                   m_primary.valid() && isSelected(m_primary);
            if (show) {
                const QSize comp = m_engine->compositionSize();
                const QPointF n = outOf(m_layer, handlePos(m_primary));
                out = QPointF(n.x() * comp.width(), n.y() * comp.height());
                static const char *corners[4] = {"Top left", "Top right", "Bottom right", "Bottom left"};
                name = m_primary.kind == 0 ? QString::fromLatin1(corners[m_primary.i & 3])
                                           : QStringLiteral("Point %1, %2").arg(m_primary.i + 1).arg(m_primary.j + 1);
                if (m_selection.size() > 1) name += QStringLiteral(" (+%1)").arg(m_selection.size() - 1);
            }
        }
    }
    for (QToolButton *b : m_alignMask) if (b) b->setEnabled(mask && aligned >= 2);
    if (m_coordBar->isVisible() != show) m_coordBar->setVisible(show);
    if (!show) return;
    m_coordName->setText(name);
    for (auto [box, v] : {std::pair{m_coordX, out.x()}, std::pair{m_coordY, out.y()}}) {
        if (box->hasFocus() || std::abs(box->value() - v) < 0.05) continue;
        QSignalBlocker block(box);
        box->setValue(v);
    }
    for (QDoubleSpinBox *box : {m_coordX, m_coordY})
        box->setToolTip(mask ? QStringLiteral("Mask-node coordinate in pixels of the attached frame")
                             : QStringLiteral("Layer point in composition pixels"));
    m_coordBar->adjustSize();
    m_coordBar->move(8, height() - m_coordBar->height() - 8);
}

void MappingView::typeCoordinate()
{
    if (m_maskEditMode) {
        quint64 id = 0;
        std::vector<Mask> before, after;
        {
            Engine::Lock lk(&m_engine->mutex());
            Mask *k = editedMask();
            Layer *l = m_engine->layer(m_layer);
            if (!k || !l || m_maskNode < 0 || m_maskNode >= int(k->nodes.size()) ||
                m_engine->isLocked(m_layer)) return;
            id = l->id;
            before = after = l->masks;
            const QSize frame = maskInComposition(k) ? m_engine->compositionSize() : m_engine->maskFrame(id);
            if (frame.width() <= 0 || frame.height() <= 0) return;
            MaskNode &n = after[size_t(m_maskIndex)].nodes[size_t(m_maskNode)];
            const QPointF delta(m_coordX->value() / frame.width() - n.p.x(),
                                m_coordY->value() / frame.height() - n.p.y());
            n.p += delta;
            n.in += delta;
            n.out += delta;
        }
        if (before != after) {
            if (m_undo) m_undo->push(new cmd::SetMasks(m_engine, id, before, after, QStringLiteral("Move Mask Node")));
            else m_engine->setLayerMasks(id, after);
            emit masksEdited();
            update();
        }
        return;
    }
    Mapping before, after;
    {
        Engine::Lock lk(&m_engine->mutex());
        Mapping *m = mapping();
        if (!m || !m_primary.valid() || m_engine->isLocked(m_layer)) return;
        const QSize comp = m_engine->compositionSize();
        const QPointF target = canvasOf(m_layer, QPointF(m_coordX->value() / comp.width(), m_coordY->value() / comp.height()));
        before = *m;
        if (!isSelected(m_primary)) m_selection = {m_primary};
        moveSelection(target - handlePos(m_primary));
        after = *m;
    }
    if (m_undo && after != before)
        m_undo->push(new cmd::SetMapping(m_engine, m_layer, before, after, QStringLiteral("Move Point"), true));
    emit mappingEdited();
    update();
}

void MappingView::updateZoomLabel()
{
    m_zoomLabel->setText(QStringLiteral("%1%").arg(int(std::lround(m_zoom * 100))));
    m_zoomBar->adjustSize();
    m_zoomBar->move(width() - m_zoomBar->width() - 8, 8);
}

void MappingView::resizeEvent(QResizeEvent *e)
{
    QOpenGLWidget::resizeEvent(e);
    updateZoomLabel();
}

// The point of the composition under `widgetPos` stays under it.
void MappingView::zoomAt(QPointF widgetPos, double factor)
{
    const QPointF n = toNorm(widgetPos);
    m_zoom = std::clamp(m_zoom * factor, 0.25, 64.0);
    const QRectF fit = fitRect();
    const double w = fit.width() * m_zoom, h = fit.height() * m_zoom;
    const QPointF center(widgetPos.x() - n.x() * w + w / 2, widgetPos.y() - n.y() * h + h / 2);
    m_pan = center - fit.center();
    updateZoomLabel();
    update();
}

void MappingView::zoomBy(double factor) { zoomAt(QPointF(width() / 2.0, height() / 2.0), factor); }

void MappingView::zoomToFit()
{
    m_zoom = 1.0;
    m_pan = QPointF();
    updateZoomLabel();
    update();
}

void MappingView::wheelEvent(QWheelEvent *e)
{
    // Trackpad (continuous scroll): two fingers pan, pinch or Ctrl/⌘ + scroll zooms.
    // Mouse wheel: zooms around the cursor.
    const bool trackpad = e->phase() != Qt::NoScrollPhase && !e->pixelDelta().isNull();
    if (trackpad && !(e->modifiers() & Qt::ControlModifier)) {
        m_pan += QPointF(e->pixelDelta());
        update();
    } else {
        const double steps = e->angleDelta().y() != 0 ? e->angleDelta().y() : e->pixelDelta().y() * 2.0;
        zoomAt(e->position(), std::pow(1.0015, steps));
    }
    e->accept();
}

bool MappingView::event(QEvent *e)
{
    if (e->type() == QEvent::NativeGesture) { // macOS trackpad pinch
        auto *g = static_cast<QNativeGestureEvent *>(e);
        if (g->gestureType() == Qt::ZoomNativeGesture) {
            zoomAt(g->position(), 1.0 + g->value());
            return true;
        }
    }
    return QOpenGLWidget::event(e);
}



MappingView::~MappingView()
{
    makeCurrent();
    m_draw.destroy();
    doneCurrent();
}

void MappingView::setLayer(int index)
{
    if (index != m_layer) {
        m_selection.clear();
        m_primary = Handle{};
        m_maskIndex = -1;
        m_maskDrag = MaskDrag::None;
        if (m_drawing) cancelDrawing();
    }
    m_layer = index;
    update();
}

// An audio layer has no picture: nothing to map.
static bool hasPicture(const Layer *l) { return l && l->hasPicture(); }

Mapping *MappingView::mapping() const
{
    Layer *l = m_engine->layer(m_layer);
    return hasPicture(l) ? &l->mapping : nullptr;
}

// The mappings of the groups holding a layer, innermost first
std::vector<const Mapping *> MappingView::groupMappings(int layer) const
{
    std::vector<const Mapping *> out;
    for (int g = m_engine->groupIndexOf(layer); g >= 0; g = m_engine->groupIndexOf(g)) {
        const Mapping &m = m_engine->layer(g)->mapping;
        if (!m.isIdentity()) out.push_back(&m);
    }
    return out;
}

QPointF MappingView::outOf(int layer, QPointF p) const
{
    for (const Mapping *g : groupMappings(layer)) p = g->map(p.x(), p.y());
    return p;
}

QPointF MappingView::canvasOf(int layer, QPointF p) const
{
    const std::vector<const Mapping *> chain = groupMappings(layer);
    for (auto it = chain.rbegin(); it != chain.rend(); ++it) p = (*it)->unmap(p, p);
    return p;
}

bool MappingView::isViewport(int layer) const
{
    const Layer *l = m_engine->layer(layer);
    return l && l->isViewport;
}

// The viewport whose frame passes under the cursor (-1: none), the selected one first
int MappingView::hitViewportFrame(QPointF p) const
{
    std::vector<int> order;
    if (isViewport(m_layer)) order.push_back(m_layer);
    for (int i : m_engine->viewports())
        if (i != m_layer) order.push_back(i);
    for (int i : order) { // near one of the four sides of its frame (turned or not)
        const Mapping &m = m_engine->layer(i)->mapping;
        for (int k = 0; k < 4; ++k) {
            const QPointF a = toWidget(m.corner(k)), b = toWidget(m.corner((k + 1) % 4));
            const QPointF d = b - a;
            const double len2 = d.x() * d.x() + d.y() * d.y();
            const double t = len2 > 1e-9 ? std::clamp(QPointF::dotProduct(p - a, d) / len2, 0.0, 1.0) : 0.0;
            const QPointF q = a + d * t;
            if (std::hypot(p.x() - q.x(), p.y() - q.y()) <= 6) return i;
        }
    }
    return -1;
}

QRectF MappingView::viewRect() const
{
    const QRectF fit = fitRect();
    QRectF r(0, 0, fit.width() * m_zoom, fit.height() * m_zoom);
    r.moveCenter(fit.center() + m_pan);
    return r;
}

QRectF MappingView::fitRect() const
{
    const QSize comp = m_engine->compositionSize();
    const double margin = 24;
    const double aw = width() - 2 * margin, ah = height() - 2 * margin;
    if (aw <= 0 || ah <= 0 || comp.isEmpty()) return QRectF(0, 0, width(), height());
    const double s = std::min(aw / comp.width(), ah / comp.height());
    const double w = comp.width() * s, h = comp.height() * s;
    return QRectF((width() - w) / 2, (height() - h) / 2, w, h);
}

QPointF MappingView::toWidget(QPointF n) const
{
    const QRectF r = viewRect();
    return QPointF(r.left() + n.x() * r.width(), r.top() + n.y() * r.height());
}

QPointF MappingView::toNorm(QPointF p) const
{
    const QRectF r = viewRect();
    return QPointF((p.x() - r.left()) / r.width(), (p.y() - r.top()) / r.height());
}

void MappingView::pushLine(std::vector<float> &v, QPointF a, QPointF b) const
{
    auto ndc = [&](QPointF p) {
        v.push_back(float(p.x() / width() * 2.0 - 1.0));
        v.push_back(float(1.0 - p.y() / height() * 2.0));
    };
    ndc(a);
    ndc(b);
}

void MappingView::pushRect(std::vector<float> &v, QPointF c, float half) const
{
    const QPointF a(c.x() - half, c.y() - half), b(c.x() + half, c.y() - half), d(c.x() - half, c.y() + half),
        e(c.x() + half, c.y() + half);
    for (QPointF p : {a, b, d, b, e, d}) {
        v.push_back(float(p.x() / width() * 2.0 - 1.0));
        v.push_back(float(1.0 - p.y() / height() * 2.0));
    }
}

std::vector<QPointF> MappingView::outline(const Mapping &m, int steps) const
{
    std::vector<QPointF> pts;
    for (int k = 0; k < steps; ++k) pts.push_back(m.map(double(k) / steps, 0));
    for (int k = 0; k < steps; ++k) pts.push_back(m.map(1, double(k) / steps));
    for (int k = 0; k < steps; ++k) pts.push_back(m.map(1 - double(k) / steps, 1));
    for (int k = 0; k < steps; ++k) pts.push_back(m.map(0, 1 - double(k) / steps));
    return pts;
}

void MappingView::initializeGL() { m_draw.init(); }

void MappingView::paintGL()
{
    paintScene();
    paintMasks();
    paintGuides();
    paintViewportNames();
    paintCoordinates();
    refreshCoordinateBar();
}

// Next to each selected point (a few at most), its position in composition pixels
void MappingView::paintCoordinates()
{
    if (m_maskEditMode) return;
    std::vector<std::pair<QPointF, QString>> labels;
    {
        Engine::Lock lk(&m_engine->mutex());
        Mapping *m = mapping();
        if (!m || isViewport(m_layer) || m_selection.empty()) return;
        const QSize comp = m_engine->compositionSize();
        // Many points selected: only the one clicked last
        std::vector<Handle> shown = m_selection.size() <= 6 ? m_selection : std::vector<Handle>{m_primary};
        for (const Handle &h : shown) {
            if (!h.valid()) continue;
            const QPointF n = outOf(m_layer, handlePos(h));
            labels.push_back({toWidget(n), QStringLiteral("%1, %2").arg(n.x() * comp.width(), 0, 'f', 1)
                                                .arg(n.y() * comp.height(), 0, 'f', 1)});
        }
    }
    QPainter p(this);
    QFont f = p.font();
    f.setPointSizeF(f.pointSizeF() * 0.85);
    p.setFont(f);
    for (const auto &[at, text] : labels) {
        const QRectF r(at.x() + 9, at.y() - 22, 140, 16);
        p.setPen(QColor(0, 0, 0, 210));
        p.drawText(r.translated(1, 1), Qt::AlignLeft | Qt::AlignVCenter, text);
        p.setPen(kSelected);
        p.drawText(r, Qt::AlignLeft | Qt::AlignVCenter, text);
    }
}

// Names of the viewports, at the top left of their frame
void MappingView::paintViewportNames()
{
    std::vector<std::pair<QRectF, QString>> frames;
    {
        Engine::Lock lk(&m_engine->mutex());
        for (int i : m_engine->viewports()) {
            const Layer *l = m_engine->layer(i);
            QPolygonF poly;
            for (int k = 0; k < 4; ++k) poly << toWidget(l->mapping.corner(k));
            frames.push_back({poly.boundingRect(), l->name});
        }
    }
    QPainter p(this);
    QFont f = p.font();
    f.setPointSizeF(f.pointSizeF() * 0.9);
    p.setFont(f);
    std::vector<QRectF> placed; // viewports at the same place: their names one under the other
    for (const auto &[r, name] : frames) {
        QRectF t(r.left() + 4, r.top() + 3, std::max(40.0, r.width() - 8), 18);
        for (bool moved = true; moved;) {
            moved = false;
            for (const QRectF &o : placed)
                if (std::abs(o.top() - t.top()) < 16 && std::abs(o.left() - t.left()) < 60) {
                    t.translate(0, 16);
                    moved = true;
                }
        }
        placed.push_back(t);
        p.setPen(QColor(0, 0, 0, 200));
        p.drawText(t.translated(1, 1), Qt::AlignLeft | Qt::AlignTop, name);
        p.setPen(kViewport);
        p.drawText(t, Qt::AlignLeft | Qt::AlignTop, name);
    }
}

// Magnetism: what caught the point or the frame being dragged
void MappingView::paintGuides()
{
    if (m_guidePoint || !m_guideX.empty() || !m_guideY.empty()) {
        std::vector<float> guides;
        const QRectF vr = viewRect();
        for (double x : m_guideX) pushLine(guides, QPointF(vr.left() + x * vr.width(), 0), QPointF(vr.left() + x * vr.width(), height()));
        for (double y : m_guideY) pushLine(guides, QPointF(0, vr.top() + y * vr.height()), QPointF(width(), vr.top() + y * vr.height()));
        m_draw.drawLines(guides, kGuide);
        if (m_guidePoint) {
            std::vector<float> ring;
            const QPointF c = toWidget(m_guideAt);
            const double r = 9;
            for (int k = 0; k < 24; ++k) {
                const double a0 = k * kPi / 12, a1 = (k + 1) * kPi / 12;
                pushLine(ring, c + QPointF(r * std::cos(a0), r * std::sin(a0)), c + QPointF(r * std::cos(a1), r * std::sin(a1)));
            }
            m_draw.drawLines(ring, kGuide);
        }
    }

}

void MappingView::paintScene()
{
    auto f = context()->extraFunctions();
    const qreal dpr = devicePixelRatioF();
    f->glViewport(0, 0, int(width() * dpr), int(height() * dpr));
    f->glClearColor(0.10f, 0.10f, 0.11f, 1);
    f->glClear(GL_COLOR_BUFFER_BIT);

    const QRectF r = viewRect();
    // The OpenGL viewport origin is at the bottom left.
    f->glViewport(int(std::round(r.left() * dpr)), int(std::round((height() - r.bottom()) * dpr)),
                  int(std::round(r.width() * dpr)), int(std::round(r.height() * dpr)));
    m_engine->waitForFrame(f);
    m_draw.drawTexture(m_engine->outputTexture());
    f->glViewport(0, 0, int(width() * dpr), int(height() * dpr));

    // Composition frame
    std::vector<float> frame;
    pushLine(frame, r.topLeft(), r.topRight());
    pushLine(frame, r.topRight(), r.bottomRight());
    pushLine(frame, r.bottomRight(), r.bottomLeft());
    pushLine(frame, r.bottomLeft(), r.topLeft());
    m_draw.drawLines(frame, QColor(255, 255, 255, 40));

    // Read layers under the lock (the render thread uses them concurrently)
    Engine::Lock lk(&m_engine->mutex());

    // Frames of the viewports: always shown (the selected one is drawn below, as the current layer)
    {
        std::vector<float> frames;
        for (int i : m_engine->viewports()) {
            if (i == m_layer) continue;
            const Mapping &vm = m_engine->layer(i)->mapping;
            for (int k = 0; k < 4; ++k) pushLine(frames, toWidget(vm.corner(k)), toWidget(vm.corner((k + 1) % 4)));
        }
        m_draw.drawLines(frames, kViewport);
    }

    // Outlines of the other layers
    if (m_showAll) {
        std::vector<float> others;
        for (int i = 0; i < m_engine->layerCount(); ++i) {
            if (i == m_layer) continue;
            Layer *l = m_engine->layer(i);
            if (l->isViewport || !l->enabled || !hasPicture(l)) continue;
            auto pts = outline(l->mapping, 16);
            for (QPointF &q : pts) q = outOf(i, q);
            for (size_t k = 0; k < pts.size(); ++k)
                pushLine(others, toWidget(pts[k]), toWidget(pts[(k + 1) % pts.size()]));
        }
        m_draw.drawLines(others, kOutlineOther);
    }

    Mapping *m = mapping();
    if (!m) return;
    const int cur = m_layer;
    auto W = [&](QPointF p) { return toWidget(outOf(cur, p)); }; // canvas of the layer -> widget

    // The pivot of the rotation: a small cross
    auto drawPivot = [&](bool viewport) {
        const QPointF c = viewport ? toWidget(m->pivotPoint()) : W(m->pivotPoint());
        std::vector<float> cross;
        pushLine(cross, c + QPointF(-6, 0), c + QPointF(6, 0));
        pushLine(cross, c + QPointF(0, -6), c + QPointF(0, 6));
        m_draw.drawLines(cross, QColor(255, 210, 60, 230));
    };

    // A viewport: its frame, moved by dragging (no corners or mesh)
    if (isViewport(cur)) {
        std::vector<float> frame;
        for (int k = 0; k < 4; ++k) pushLine(frame, toWidget(m->corner(k)), toWidget(m->corner((k + 1) % 4)));
        m_draw.drawLines(frame, m_engine->isLocked(cur) ? QColor(230, 80, 70, 220) : kSelected);
        drawPivot(true);
        return;
    }

    // Warp mesh
    if (m->meshMode) {
        std::vector<float> grid;
        const int steps = 24;
        for (int j = 0; j < m->rows; ++j) {
            const double v = double(j) / (m->rows - 1);
            for (int k = 0; k < steps; ++k)
                pushLine(grid, W(m->map(double(k) / steps, v)), W(m->map(double(k + 1) / steps, v)));
        }
        for (int i = 0; i < m->cols; ++i) {
            const double u = double(i) / (m->cols - 1);
            for (int k = 0; k < steps; ++k)
                pushLine(grid, W(m->map(u, double(k) / steps)), W(m->map(u, double(k + 1) / steps)));
        }
        m_draw.drawLines(grid, kGrid);
    }

    std::vector<float> line;
    auto pts = outline(*m, 32);
    for (size_t k = 0; k < pts.size(); ++k) pushLine(line, W(pts[k]), W(pts[(k + 1) % pts.size()]));
    // Faint diagonals in corners mode, to judge the perspective
    if (!m->meshMode) {
        pushLine(line, W(m->corner(0)), W(m->corner(2)));
        pushLine(line, W(m->corner(1)), W(m->corner(3)));
    }
    // Locked layer: outline in red, no handles
    if (m_engine->isLocked(m_layer)) {
        m_draw.drawLines(line, QColor(230, 80, 70, 220));
        drawPivot(false);
        return;
    }
    m_draw.drawLines(line, kOutline);
    drawPivot(false);

    std::vector<float> handles, sel, shadow;
    auto addHandle = [&](const Handle &h, float half) {
        const QPointF p = W(handlePos(h));
        pushRect(shadow, p, half + 1.5f);
        pushRect(isSelected(h) ? sel : handles, p, half);
    };
    if (m->meshMode) {
        for (int j = 0; j < m->rows; ++j)
            for (int i = 0; i < m->cols; ++i) addHandle(Handle{1, i, j}, 3.5f);
    } else {
        for (int i = 0; i < 4; ++i) addHandle(Handle{0, i, 0}, 6.0f);
    }
    m_draw.drawTriangles(shadow, QColor(0, 0, 0, 160));
    m_draw.drawTriangles(handles, kHandle);
    m_draw.drawTriangles(sel, kSelected);
    if (!m->meshMode) paintGrips(layerGrips());

    if (m_rubber) { // selection rectangle
        const QRectF r = QRectF(m_rubberStart, m_rubberEnd).normalized();
        std::vector<float> band;
        pushLine(band, r.topLeft(), r.topRight());
        pushLine(band, r.topRight(), r.bottomRight());
        pushLine(band, r.bottomRight(), r.bottomLeft());
        pushLine(band, r.bottomLeft(), r.topLeft());
        m_draw.drawLines(band, kSelected);
    }
}

// The grips sit on the layer's own axes: scale above its top edge, rotation beside its right edge, both 22 px out
MappingView::Grips MappingView::layerGrips() const
{
    Grips g;
    Mapping *m = mapping();
    if (!m || m_layer < 0 || isViewport(m_layer)) return g;
    auto W = [&](QPointF p) { return toWidget(outOf(m_layer, p)); };
    const QPointF k0 = W(m->corner(0)), k1 = W(m->corner(1)), k2 = W(m->corner(2)), k3 = W(m->corner(3));
    g.center = (k0 + k1 + k2 + k3) / 4.0;
    g.top = (k0 + k1) / 2.0;
    g.right = (k1 + k2) / 2.0;
    auto unit = [](QPointF v) { const double n = std::hypot(v.x(), v.y()); return n > 1e-6 ? v / n : QPointF(0, -1); };
    g.scale = g.top + unit(g.top - g.center) * 22.0;
    g.rotate = g.right + unit(g.right - g.center) * 22.0;
    return g;
}

MappingView::Grip MappingView::hitGrip(QPointF p) const
{
    const Grips g = layerGrips();
    auto near = [&](QPointF q) { return std::hypot(p.x() - q.x(), p.y() - q.y()) <= 10.0; };
    if (near(g.scale)) return Grip::Scale;
    if (near(g.rotate)) return Grip::Rotate;
    return Grip::None;
}

// A line from each edge midpoint to its grip; the scale grip is a square, the rotation grip a ring
void MappingView::paintGrips(const Grips &g)
{
    std::vector<float> stems, shadow, boxes, ring;
    pushLine(stems, g.top, g.scale);
    pushLine(stems, g.right, g.rotate);
    m_draw.drawLines(stems, kOutline);
    pushRect(shadow, g.scale, 7.5f);
    pushRect(boxes, g.scale, 6.0f);
    m_draw.drawTriangles(shadow, QColor(0, 0, 0, 160));
    m_draw.drawTriangles(boxes, kHandle);
    const int steps = 20;
    for (int k = 0; k < steps; ++k) {
        const double a0 = 2 * kPi * k / steps, a1 = 2 * kPi * (k + 1) / steps;
        pushLine(ring, g.rotate + QPointF(std::cos(a0), std::sin(a0)) * 6.0,
                 g.rotate + QPointF(std::cos(a1), std::sin(a1)) * 6.0);
    }
    m_draw.drawLines(ring, kHandle);
}

bool MappingView::isSelected(const Handle &h) const
{
    return std::find(m_selection.begin(), m_selection.end(), h) != m_selection.end();
}

std::vector<MappingView::Handle> MappingView::allHandles() const
{
    std::vector<Handle> out;
    Mapping *m = mapping();
    if (!m || isViewport(m_layer)) return out;
    if (m->meshMode) {
        for (int j = 0; j < m->rows; ++j)
            for (int i = 0; i < m->cols; ++i) out.push_back(Handle{1, i, j});
    } else {
        for (int i = 0; i < 4; ++i) out.push_back(Handle{0, i, 0});
    }
    return out;
}

// All selected handles move by the same amount. Positions are read before moving:
// moving a corner changes the homography, hence the position of the other points.
void MappingView::moveSelection(QPointF delta)
{
    std::vector<QPointF> start;
    for (const Handle &h : m_selection) start.push_back(handlePos(h));
    for (size_t k = 0; k < m_selection.size(); ++k) moveHandle(m_selection[k], start[k] + delta);
}

QPointF MappingView::handlePos(const Handle &h) const
{
    Mapping *m = mapping();
    if (!m || !h.valid()) return {};
    if (h.kind == 0) return m->corner(h.i);
    return m->controlPoint(h.i, h.j);
}

void MappingView::moveHandle(const Handle &h, QPointF norm)
{
    Mapping *m = mapping();
    if (!m || !h.valid()) return;
    if (h.kind == 0) m->setCorner(h.i, norm);
    else m->setControlPoint(h.i, h.j, norm);
}

MappingView::Handle MappingView::hitHandle(QPointF p) const
{
    Mapping *m = mapping();
    if (!m || isViewport(m_layer)) return {};
    Handle best;
    double bestD = 12.0;
    auto test = [&](const Handle &h) {
        const QPointF w = toWidget(outOf(m_layer, handlePos(h)));
        const double d = std::hypot(w.x() - p.x(), w.y() - p.y());
        if (d < bestD) {
            bestD = d;
            best = h;
        }
    };
    if (m->meshMode) {
        for (int j = 0; j < m->rows; ++j)
            for (int i = 0; i < m->cols; ++i) test(Handle{1, i, j});
    } else {
        for (int i = 0; i < 4; ++i) test(Handle{0, i, 0});
    }
    return best;
}

bool MappingView::insideLayer(int index, QPointF p) const
{
    Layer *l = m_engine->layer(index);
    if (!hasPicture(l)) return false;
    auto pts = outline(l->mapping, 16);
    for (QPointF &q : pts) q = outOf(index, q);
    bool in = false;
    for (size_t i = 0, j = pts.size() - 1; i < pts.size(); j = i++) {
        const QPointF a = toWidget(pts[i]), b = toWidget(pts[j]);
        if (((a.y() > p.y()) != (b.y() > p.y())) && (p.x() < (b.x() - a.x()) * (p.y() - a.y()) / (b.y() - a.y()) + a.x()))
            in = !in;
    }
    return in;
}

// ---------------------------------------------------------------------------
// Magnetism
// ---------------------------------------------------------------------------
bool MappingView::snapping(Qt::KeyboardModifiers mods) const { return magnet::enabled() && !(mods & Qt::ControlModifier); }

void MappingView::clearGuides()
{
    m_guideX.clear();
    m_guideY.clear();
    m_guidePoint = false;
}

// What can catch the layer, its points or the viewport being dragged: the composition (edges, center lines,
// corners, center), the viewports' frames, the other layers shown (corners, edges of their bounds), and the
// layer's own points that are not moving
MappingView::SnapTargets MappingView::snapTargets() const
{
    SnapTargets t;
    auto box = [&t](const QRectF &b, bool corners) {
        t.xs.insert(t.xs.end(), {b.left(), b.center().x(), b.right()});
        t.ys.insert(t.ys.end(), {b.top(), b.center().y(), b.bottom()});
        if (corners) t.points.insert(t.points.end(), {b.topLeft(), b.topRight(), b.bottomRight(), b.bottomLeft()});
    };
    box(QRectF(0, 0, 1, 1), true);
    t.points.push_back(QPointF(0.5, 0.5));
    for (int i = 0; i < m_engine->layerCount(); ++i) {
        if (i == m_layer) continue;
        const Layer *l = m_engine->layer(i);
        if (!l || !hasPicture(l)) continue;
        if (l->isViewport) {
            box(l->mapping.bounds(), true);
            continue;
        }
        if (!l->enabled || isViewport(m_layer)) continue; // a viewport is caught by the composition and the others
        const Mapping &m = l->mapping;
        const QPointF c[4] = {outOf(i, m.map(0, 0)), outOf(i, m.map(1, 0)), outOf(i, m.map(1, 1)), outOf(i, m.map(0, 1))};
        t.points.insert(t.points.end(), c, c + 4);
        QRectF b(c[0], c[0]);
        for (const QPointF &p : c) b = b.united(QRectF(p, p));
        box(b, false);
    }
    // The layer's own points that stay where they are: they line up with each other
    if (m_dragHandle)
        for (const Handle &h : allHandles())
            if (!isSelected(h)) {
                const QPointF p = outOf(m_layer, handlePos(h));
                t.points.push_back(p);
                t.xs.push_back(p.x());
                t.ys.push_back(p.y());
            }
    return t;
}

QPointF MappingView::snapPoint(QPointF p, const SnapTargets &t)
{
    clearGuides();
    const QRectF vr = viewRect();
    const double dx = magnet::distance() / std::max(1.0, vr.width()), dy = magnet::distance() / std::max(1.0, vr.height());
    // A point within reach catches both coordinates
    double best = 1.0;
    for (const QPointF &q : t.points) {
        const double d = std::hypot((q.x() - p.x()) / dx, (q.y() - p.y()) / dy);
        if (d < best) {
            best = d;
            m_guidePoint = true;
            m_guideAt = q;
        }
    }
    if (m_guidePoint) return m_guideAt;
    // Otherwise each coordinate on the nearest line within reach
    QPointF r = p;
    double bx = dx, by = dy;
    for (double x : t.xs)
        if (std::abs(x - p.x()) < bx) {
            bx = std::abs(x - p.x());
            r.setX(x);
            m_guideX = {x};
        }
    for (double y : t.ys)
        if (std::abs(y - p.y()) < by) {
            by = std::abs(y - p.y());
            r.setY(y);
            m_guideY = {y};
        }
    return r;
}

QPointF MappingView::snapBox(const QRectF &b, const SnapTargets &t)
{
    clearGuides();
    const QRectF vr = viewRect();
    const double dx = magnet::distance() / std::max(1.0, vr.width()), dy = magnet::distance() / std::max(1.0, vr.height());
    QPointF off;
    double bx = dx, by = dy;
    for (double edge : {b.left(), b.center().x(), b.right()})
        for (double x : t.xs)
            if (std::abs(x - edge) < bx) {
                bx = std::abs(x - edge);
                off.setX(x - edge);
                m_guideX = {x};
            }
    for (double edge : {b.top(), b.center().y(), b.bottom()})
        for (double y : t.ys)
            if (std::abs(y - edge) < by) {
                by = std::abs(y - edge);
                off.setY(y - edge);
                m_guideY = {y};
            }
    return off;
}

void MappingView::mousePressEvent(QMouseEvent *e)
{
    // Pan: middle button, or Alt / ⌥ + drag (Space is play / pause)
    if (e->button() == Qt::MiddleButton || (e->button() == Qt::LeftButton && (e->modifiers() & Qt::AltModifier))) {
        m_panning = true;
        m_panLast = e->position();
        setCursor(Qt::ClosedHandCursor);
        return;
    }
    Engine::Lock lk(&m_engine->mutex());
    const QPointF p = e->position();
    m_lastNorm = toNorm(p);
    if ((m_drawing || (m_maskEditMode && m_maskIndex >= 0)) && pressMask(e)) { // a mask's point, handle or inside, or one being drawn
        update();
        return;
    }
    const bool locked = m_engine->isLocked(m_layer);
    // The grips of the layer (not for a viewport, a locked or a warped layer)
    const Mapping *gm = mapping();
    if (!locked && gm && !gm->meshMode) {
        const Grip grip = hitGrip(p);
        if (grip != Grip::None) {
            m_selection.clear();
            m_dragScale = grip == Grip::Scale;
            m_dragRotate = grip == Grip::Rotate;
            m_gestureCenter = layerGrips().center;
            m_gestureStart = p;
            m_dragBefore = *gm;
            clearGuides();
            update();
            return;
        }
    }
    Handle h = locked ? Handle{} : hitHandle(p);
    const bool additive = e->modifiers() & Qt::ControlModifier; // Ctrl, ⌘ on Mac
    if (h.valid()) {
        if (additive) { // Ctrl/⌘+click: add or remove the point
            const auto it = std::find(m_selection.begin(), m_selection.end(), h);
            if (it != m_selection.end()) m_selection.erase(it);
            else m_selection.push_back(h);
        } else if (!isSelected(h)) {
            m_selection = {h};
        }
        m_primary = h;
        m_dragHandle = isSelected(h); // dragging a selected point moves the whole selection
    } else if (additive && mapping()) { // Ctrl/⌘+drag: selection rectangle
        m_rubber = true;
        m_rubberStart = m_rubberEnd = p;
    } else if (const int vp = hitViewportFrame(p); vp >= 0 && (vp == m_layer || autoselect::enabled())) {
        // a viewport's frame: select it (auto-select) and drag it
        m_selection.clear();
        if (vp != m_layer) {
            lk.unlock();
            emit layerPicked(vp);
            lk.relock();
        }
        m_dragLayer = !m_engine->isLocked(vp);
    } else if (m_layer >= 0 && !isViewport(m_layer) && insideLayer(m_layer, p)) {
        m_selection.clear();
        m_dragLayer = !locked;
    } else if (autoselect::enabled()) {
        // Select the topmost visible layer under the cursor (in a group: its layers before the group itself,
        // at any depth). Viewports are picked by their frame.
        m_selection.clear();
        std::vector<int> order;
        const int n = m_engine->layerCount();
        std::function<void(int)> add = [&](int i) {
            const Layer *l = m_engine->layer(i);
            if (l->isGroup) {
                if (!l->enabled) return;
                for (int k = i + 1; k < n; ++k)
                    if (m_engine->layer(k)->parent == l->id) add(k);
            }
            order.push_back(i);
        };
        for (int i = 0; i < n; ++i) {
            const Layer *l = m_engine->layer(i);
            if (!l->parent && !l->isViewport) add(i);
        }
        for (int i : order) {
            if (m_engine->layer(i)->enabled && insideLayer(i, p)) {
                const bool pickedLocked = m_engine->isLocked(i);
                lk.unlock();
                emit layerPicked(i);
                lk.relock();
                m_dragLayer = !pickedLocked;
                break;
            }
        }
    }
    if (Mapping *m = mapping()) {
        m_dragBefore = *m;
        m_dragStartBox = m->bounds();
    }
    if (m_dragHandle) m_dragRaw = handlePos(m_primary);
    m_dragOffsetRaw = m_dragOffsetApplied = QPointF();
    clearGuides();
    update();
}

void MappingView::mouseMoveEvent(QMouseEvent *e)
{
    if (m_panning) {
        m_pan += e->position() - m_panLast;
        m_panLast = e->position();
        update();
        return;
    }
    if (m_drawing) { // the line to the cursor; pressed: the last point's handles follow it
        m_cursor = e->position();
        if (m_drawPressing && !m_drawNodes.empty()) {
            Engine::Lock lk(&m_engine->mutex());
            MaskNode &n = m_drawNodes.back();
            const QPointF w = frameToWidget(false, n.p);
            if (std::hypot(m_cursor.x() - w.x(), m_cursor.y() - w.y()) > 3) {
                n.out = widgetToFrame(false, m_cursor, n.p);
                n.in = 2 * n.p - n.out;
            }
        }
        update();
        return;
    }
    if (m_maskDrag != MaskDrag::None) {
        Engine::Lock lk(&m_engine->mutex());
        Mask *k = editedMask();
        Layer *l = m_engine->layer(m_layer);
        if (!k || !l || m_maskNode >= int(k->nodes.size())) return;
        const bool comp = maskInComposition(k);
        const QPointF w = e->position();
        if (m_maskDrag == MaskDrag::Body) {
            const QPointF q = widgetToFrame(comp, w, m_maskLast);
            k->translate(q - m_maskLast);
            m_maskLast = q;
        } else {
            MaskNode &n = k->nodes[size_t(m_maskNode)];
            // Magnetism works in composition space even when a surface mask is warped by mapping.
            // Other mask points are additional snap targets; Ctrl/Command disables magnetism.
            auto snapMask = [&](QPointF raw) {
                if (!snapping(e->modifiers())) { clearGuides(); return raw; }
                SnapTargets targets = snapTargets();
                for (size_t j = 0; j < k->nodes.size(); ++j) {
                    if (int(j) == m_maskNode) continue;
                    const QPointF p = toNorm(frameToWidget(comp, k->nodes[j].p));
                    targets.points.push_back(p);
                    targets.xs.push_back(p.x());
                    targets.ys.push_back(p.y());
                }
                const QPointF snapped = snapPoint(toNorm(frameToWidget(comp, raw)), targets);
                return widgetToFrame(comp, toWidget(snapped), raw);
            };
            if (m_maskDrag == MaskDrag::Node) {
                const QPointF d = snapMask(widgetToFrame(comp, w, n.p)) - n.p;
                if (m_maskSelection.empty()) m_maskSelection = {m_maskNode};
                for (int j : m_maskSelection) {
                    if (j < 0 || j >= int(k->nodes.size())) continue;
                    MaskNode &node = k->nodes[size_t(j)];
                    node.p += d;
                    node.in += d;
                    node.out += d;
                }
            } else { // a handle: the other one mirrors it (Shift: on its own)
                QPointF &h = m_maskDrag == MaskDrag::Out ? n.out : n.in;
                QPointF &o = m_maskDrag == MaskDrag::Out ? n.in : n.out;
                h = snapMask(widgetToFrame(comp, w, h));
                if (!(e->modifiers() & Qt::ShiftModifier) && o != n.p) o = 2 * n.p - h;
            }
        }
        ++l->masksRevision;
        emit masksEdited();
        update();
        return;
    }
    Engine::Lock lk(&m_engine->mutex());
    const QPointF n = toNorm(e->position());
    // Moves happen in the layer's own canvas (inside its group's mapping)
    QPointF delta = canvasOf(m_layer, n) - canvasOf(m_layer, m_lastNorm);
    m_lastNorm = n;
    if (e->modifiers() & Qt::ShiftModifier) delta *= 0.1; // fine movement
    if (m_rubber) {
        m_rubberEnd = e->position();
    } else if (m_dragHandle) {
        // The point follows the cursor from where it would be without magnetism, then is caught (or not)
        m_dragRaw += delta;
        QPointF target = m_dragRaw;
        clearGuides();
        if (snapping(e->modifiers())) target = canvasOf(m_layer, snapPoint(outOf(m_layer, m_dragRaw), snapTargets()));
        moveSelection(target - handlePos(m_primary));
        emit mappingEdited();
    } else if (m_dragScale || m_dragRotate) {
        if (Mapping *m = mapping()) {
            if (m_dragScale) { // the size grows with the distance from the center, around it
                const double d0 = std::max(4.0, std::hypot(m_gestureStart.x() - m_gestureCenter.x(), m_gestureStart.y() - m_gestureCenter.y()));
                const double d = std::hypot(e->position().x() - m_gestureCenter.x(), e->position().y() - m_gestureCenter.y());
                const double f = std::clamp(d / d0, 0.01, 100.0);
                m->size = QSizeF(m_dragBefore.size.width() * f, m_dragBefore.size.height() * f);
            } else { // the angle from the center, in widget pixels (clockwise); Shift: steps of 15°
                const double a0 = std::atan2(m_gestureStart.y() - m_gestureCenter.y(), m_gestureStart.x() - m_gestureCenter.x());
                const QPointF q = e->position();
                const double a = std::atan2(q.y() - m_gestureCenter.y(), q.x() - m_gestureCenter.x());
                double deg = (a - a0) * 180.0 / kPi;
                if (e->modifiers() & Qt::ShiftModifier) deg = std::round(deg / 15.0) * 15.0;
                m->rotation = m_dragBefore.rotation + deg;
            }
            ++m->revision;
        }
        emit mappingEdited();
    } else if (m_dragLayer) {
        if (Mapping *m = mapping()) {
            m_dragOffsetRaw += delta;
            QPointF want = m_dragOffsetRaw;
            clearGuides();
            if (snapping(e->modifiers())) {
                const QRectF box = m_dragStartBox.translated(m_dragOffsetRaw);
                const QPointF a = outOf(m_layer, box.topLeft()), b = outOf(m_layer, box.bottomRight());
                const QRectF out = QRectF(a, b).normalized();
                const QPointF s = snapBox(out, snapTargets());
                if (!s.isNull()) want += canvasOf(m_layer, out.topLeft() + s) - canvasOf(m_layer, out.topLeft());
            }
            m->translate(want - m_dragOffsetApplied);
            m_dragOffsetApplied = want;
        }
        emit mappingEdited();
    }
    update();
}

void MappingView::mouseReleaseEvent(QMouseEvent *)
{
    if (m_panning) {
        m_panning = false;
        unsetCursor();
        return;
    }
    if (m_drawing) {
        m_drawPressing = false;
        return;
    }
    if (m_maskDrag != MaskDrag::None) {
        const bool whole = m_maskDrag == MaskDrag::Body;
        m_maskDrag = MaskDrag::None;
        quint64 id = 0;
        std::vector<Mask> after;
        {
            Engine::Lock lk(&m_engine->mutex());
            if (Layer *l = m_engine->layer(m_layer)) {
                id = l->id;
                after = l->masks;
            }
        }
        if (id && m_undo && after != m_masksBefore)
            m_undo->push(new cmd::SetMasks(m_engine, id, m_masksBefore, after,
                                           whole ? QStringLiteral("Move Mask") : QStringLiteral("Move Mask Point")));
        emit masksEdited();
        return;
    }
    if (m_rubber) { // adds the points inside the rectangle to the selection
        m_rubber = false;
        Engine::Lock lk(&m_engine->mutex());
        const QRectF r = QRectF(m_rubberStart, m_rubberEnd).normalized();
        for (const Handle &h : allHandles())
            if (r.contains(toWidget(outOf(m_layer, handlePos(h)))) && !isSelected(h)) m_selection.push_back(h);
        update();
        return;
    }
    const bool grip = m_dragScale || m_dragRotate;
    const bool dragging = m_dragHandle || m_dragLayer || grip;
    const bool handle = m_dragHandle;
    const bool scaling = m_dragScale;
    m_dragHandle = m_dragLayer = m_dragScale = m_dragRotate = false;
    clearGuides();
    update();
    if (!dragging || !m_undo) return;
    const Mapping after = cmd::SetMapping::read(m_engine, m_layer);
    if (after == m_dragBefore) return;
    const QString text = grip ? (scaling ? QStringLiteral("Scale Layer") : QStringLiteral("Rotate Layer"))
                         : !handle ? (isViewport(m_layer) ? QStringLiteral("Move Viewport") : QStringLiteral("Move Layer"))
                         : m_selection.size() > 1 ? QStringLiteral("Move %1 Points").arg(m_selection.size())
                                                  : QStringLiteral("Move Handle");
    m_undo->push(new cmd::SetMapping(m_engine, m_layer, m_dragBefore, after, text));
}

void MappingView::keyPressEvent(QKeyEvent *e)
{
    if (m_drawing) {
        if (e->key() == Qt::Key_Return || e->key() == Qt::Key_Enter) finishDrawing();
        else if (e->key() == Qt::Key_Escape) cancelDrawing();
        return;
    }
    // A mask's point clicked last: Delete removes it (a mask keeps three)
    if (m_maskIndex >= 0 && m_maskNode >= 0 && (e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace)) {
        quint64 id = 0;
        std::vector<Mask> before, after;
        {
            Engine::Lock lk(&m_engine->mutex());
            Layer *l = m_engine->layer(m_layer);
            Mask *k = editedMask();
            if (l && k && !m_engine->isLocked(m_layer) && k->nodes.size() > 3 && m_maskNode < int(k->nodes.size())) {
                id = l->id;
                before = after = l->masks;
                Mask &a = after[size_t(m_maskIndex)];
                a.nodes.erase(a.nodes.begin() + m_maskNode);
                a.shape = Mask::Shape::Path;
            }
        }
        if (id && m_undo) {
            m_maskNode = -1;
            m_undo->push(new cmd::SetMasks(m_engine, id, before, after, QStringLiteral("Delete Mask Point")));
            emit masksEdited();
            update();
            return;
        }
    }
    Engine::Lock lk(&m_engine->mutex());
    Mapping *m = mapping();
    QPointF d;
    if (e->matches(QKeySequence::SelectAll)) { // Ctrl/⌘+A: every point of the current mode
        m_selection = allHandles();
        update();
        return;
    }
    switch (e->key()) {
    case Qt::Key_Left: d = {-1, 0}; break;
    case Qt::Key_Right: d = {1, 0}; break;
    case Qt::Key_Up: d = {0, -1}; break;
    case Qt::Key_Down: d = {0, 1}; break;
    case Qt::Key_Tab:
    case Qt::Key_Backtab: {
        // Next handle (Shift+Tab: previous)
        if (!m) break;
        const int step = e->key() == Qt::Key_Backtab ? -1 : 1;
        if (m->meshMode) {
            const int n = m->cols * m->rows;
            int k = m_primary.kind == 1 ? m_primary.j * m->cols + m_primary.i + step : (step > 0 ? 0 : n - 1);
            k = ((k % n) + n) % n;
            m_primary = Handle{1, k % m->cols, k / m->cols};
        } else {
            m_primary = Handle{0, m_primary.kind == 0 ? (m_primary.i + step + 4) % 4 : (step > 0 ? 0 : 3), 0};
        }
        m_selection = {m_primary};
        update();
        return;
    }
    case Qt::Key_Escape:
        m_selection.clear();
        update();
        return;
    default: QOpenGLWidget::keyPressEvent(e); return;
    }
    if (!m || m_engine->isLocked(m_layer)) return;
    const Mapping before = *m;
    // One composition pixel, ×10 with Shift
    const QSize comp = m_engine->compositionSize();
    const double step = (e->modifiers() & Qt::ShiftModifier) ? 10.0 : 1.0;
    const QPointF delta(d.x() * step / comp.width(), d.y() * step / comp.height());
    if (!m_selection.empty()) {
        moveSelection(delta);
    } else {
        m->translate(delta);
    }
    const Mapping after = *m;
    lk.unlock();
    if (m_undo) m_undo->push(new cmd::SetMapping(m_engine, m_layer, before, after, QStringLiteral("Nudge"), true));
    emit mappingEdited();
    update();
}

bool MappingView::focusNextPrevChild(bool) { return false; }

// ---------------------------------------------------------------------------
// Masks
// ---------------------------------------------------------------------------
static const QColor kMask(41, 199, 224), kMaskFaint(41, 199, 224, 90);

void MappingView::setMaskIndex(int index)
{
    m_maskIndex = index;
    m_maskSelection.clear();
    if (index >= 0) setMaskEditMode(true);
    m_maskNode = -1;
    m_maskDrag = MaskDrag::None;
    update();
}

// Align selected nodes along an extreme X or Y coordinate, preserving their Bézier handles.
void MappingView::alignMaskNodes(int edge)
{
    if (!m_maskEditMode || m_maskSelection.size() < 2) return;
    quint64 id = 0;
    std::vector<Mask> before, after;
    {
        Engine::Lock lk(&m_engine->mutex());
        Mask *k = editedMask();
        Layer *l = m_engine->layer(m_layer);
        if (!k || !l || m_engine->isLocked(m_layer)) return;
        id = l->id;
        before = after = l->masks;
        masks::alignNodes(after[size_t(m_maskIndex)], m_maskSelection, masks::AlignEdge(edge));
    }
    if (before != after) {
        if (m_undo) m_undo->push(new cmd::SetMasks(m_engine, id, before, after,
                                                  QStringLiteral("Align Mask Nodes")));
        else m_engine->setLayerMasks(id, after);
        emit masksEdited();
        update();
    }
}

void MappingView::startMaskDrawing()
{
    setMaskEditMode(true);
    {
        Engine::Lock lk(&m_engine->mutex());
        if (!mapping() || m_engine->isLocked(m_layer)) return;
    }
    m_drawing = true;
    m_drawPressing = false;
    m_drawNodes.clear();
    setMouseTracking(true);
    setCursor(Qt::CrossCursor);
    setFocus();
    update();
}

void MappingView::cancelDrawing()
{
    m_drawing = m_drawPressing = false;
    m_drawNodes.clear();
    setMouseTracking(false);
    unsetCursor();
    update();
}

void MappingView::finishDrawing()
{
    const std::vector<MaskNode> nodes = m_drawNodes;
    cancelDrawing();
    if (nodes.size() < 3) return;
    quint64 id = 0;
    std::vector<Mask> before, after;
    {
        Engine::Lock lk(&m_engine->mutex());
        Layer *l = m_engine->layer(m_layer);
        if (!l) return;
        id = l->id;
        before = after = l->masks;
        Mask k;
        k.shape = Mask::Shape::Path;
        k.nodes = nodes;
        k.name = masks::uniqueName(after);
        after.push_back(k);
    }
    if (m_undo) m_undo->push(new cmd::SetMasks(m_engine, id, before, after, QStringLiteral("Draw Mask")));
    else m_engine->setLayerMasks(id, after);
    m_maskIndex = int(after.size()) - 1;
    emit masksEdited();
    emit maskPicked(m_maskIndex);
    update();
}

Mask *MappingView::editedMask()
{
    Layer *l = m_engine->layer(m_layer);
    if (!l || m_maskIndex < 0 || m_maskIndex >= int(l->masks.size())) return nullptr;
    return &l->masks[size_t(m_maskIndex)];
}

// A viewport's masks are always in its own picture
bool MappingView::maskInComposition(const Mask *k) const
{
    return k && k->attach == Mask::Attach::Composition && !isViewport(m_layer);
}

// A point of a mask's frame on the widget: through the layer's mapping (its picture), or on its canvas (the
// composition, or its group's)
QPointF MappingView::frameToWidget(bool composition, QPointF q) const
{
    const Mapping *m = mapping();
    if (!composition && m) q = m->map(q.x(), q.y());
    return toWidget(outOf(m_layer, q));
}

QPointF MappingView::widgetToFrame(bool composition, QPointF w, QPointF guess) const
{
    const QPointF c = canvasOf(m_layer, toNorm(w));
    const Mapping *m = mapping();
    if (composition || !m) return c;
    return m->unmap(c, guess);
}

// Its outline on the widget: the curves flattened, the long segments cut (a warped picture bends them)
QPolygonF MappingView::maskOutline(const Mask &k) const
{
    QPolygonF out;
    if (!k.valid()) return out;
    const bool comp = maskInComposition(&k);
    const double scale = 1000;
    const QList<QPolygonF> polys = k.path(QSizeF(scale, scale)).toSubpathPolygons();
    for (const QPolygonF &poly : polys)
        for (int i = 0; i + 1 < poly.size(); ++i) {
            const QPointF a = poly[i] / scale, b = poly[i + 1] / scale;
            const int steps = std::max(1, int(std::ceil(std::hypot(b.x() - a.x(), b.y() - a.y()) / 0.02)));
            for (int s = 0; s < steps; ++s) out << frameToWidget(comp, a + (b - a) * (double(s) / steps));
        }
    if (!out.isEmpty()) out << out.front();
    return out;
}

void MappingView::paintMasks()
{
    Engine::Lock lk(&m_engine->mutex());
    const Layer *l = m_engine->layer(m_layer);
    if (!l || !mapping()) return;
    // Its masks: the edited one bright with its points and handles, the others faint
    for (int i = 0; i < int(l->masks.size()); ++i) {
        const Mask &k = l->masks[size_t(i)];
        const QPolygonF poly = maskOutline(k);
        std::vector<float> lines;
        for (int j = 0; j + 1 < poly.size(); ++j) pushLine(lines, poly[j], poly[j + 1]);
        m_draw.drawLines(lines, i == m_maskIndex ? kMask : kMaskFaint);
    }
    if (const Layer *cl = l; m_maskEditMode && m_maskIndex >= 0 && m_maskIndex < int(cl->masks.size())) {
        const Mask &k = cl->masks[size_t(m_maskIndex)];
        const bool comp = maskInComposition(&k);
        std::vector<float> stems, handles, points, shadow, sel;
        for (int i = 0; i < int(k.nodes.size()); ++i) {
            const MaskNode &n = k.nodes[size_t(i)];
            const QPointF p = frameToWidget(comp, n.p);
            for (const QPointF &hq : {n.in, n.out})
                if (hq != n.p) {
                    const QPointF h = frameToWidget(comp, hq);
                    pushLine(stems, p, h);
                    pushRect(handles, h, 3.0f);
                }
            pushRect(shadow, p, 5.5f);
            pushRect(std::find(m_maskSelection.begin(), m_maskSelection.end(), i) != m_maskSelection.end()
                         ? sel : points, p, 4.0f);
        }
        m_draw.drawLines(stems, kMaskFaint);
        m_draw.drawTriangles(handles, kMask);
        m_draw.drawTriangles(shadow, QColor(0, 0, 0, 170));
        m_draw.drawTriangles(points, QColor(255, 255, 255, 235));
        m_draw.drawTriangles(sel, kMask);
    }
    // Being drawn: its points so far, and the line to the cursor
    if (m_drawing && !m_drawNodes.empty()) {
        std::vector<float> lines, points;
        QPointF prev;
        for (size_t i = 0; i < m_drawNodes.size(); ++i) {
            const QPointF p = frameToWidget(false, m_drawNodes[i].p);
            if (i) pushLine(lines, prev, p);
            pushRect(points, p, i == 0 && m_drawNodes.size() >= 3 ? 6.0f : 4.0f); // the first one closes it
            if (m_drawNodes[i].out != m_drawNodes[i].p) {
                pushLine(lines, frameToWidget(false, m_drawNodes[i].in), frameToWidget(false, m_drawNodes[i].out));
            }
            prev = p;
        }
        if (!m_drawPressing) pushLine(lines, prev, m_cursor);
        m_draw.drawLines(lines, kMask);
        m_draw.drawTriangles(points, QColor(255, 255, 255, 235));
    }
}

bool MappingView::pressMask(QMouseEvent *e)
{
    const QPointF p = e->position();
    Layer *l = m_engine->layer(m_layer);
    if (!l || !mapping()) return false;
    if (m_engine->isLocked(m_layer)) {
        if (m_drawing) cancelDrawing();
        return false;
    }
    auto near = [&](QPointF a, double r) { return std::hypot(a.x() - p.x(), a.y() - p.y()) <= r; };
    if (m_drawing) {
        if (e->button() != Qt::LeftButton) return true;
        if (m_drawNodes.size() >= 3 && near(frameToWidget(false, m_drawNodes.front().p), 8)) { // on the first point
            finishDrawing();
            return true;
        }
        const QPointF guess = m_drawNodes.empty() ? QPointF(0.5, 0.5) : m_drawNodes.back().p;
        QPointF q = widgetToFrame(false, p, guess);
        if (snapping(e->modifiers())) {
            const QPointF caught = snapPoint(toNorm(frameToWidget(false, q)), snapTargets());
            q = widgetToFrame(false, toWidget(caught), guess);
        }
        m_drawNodes.push_back({q, q, q});
        m_drawPressing = true;
        m_cursor = p;
        return true;
    }
    Mask *k = editedMask();
    if (!k || !k->enabled || e->button() != Qt::LeftButton) return false;
    const bool comp = maskInComposition(k);
    m_maskDrag = MaskDrag::None;
    for (int i = 0; i < int(k->nodes.size()) && m_maskDrag == MaskDrag::None; ++i) { // the handles first
        const MaskNode &n = k->nodes[size_t(i)];
        if (n.out != n.p && near(frameToWidget(comp, n.out), 6)) m_maskDrag = MaskDrag::Out;
        else if (n.in != n.p && near(frameToWidget(comp, n.in), 6)) m_maskDrag = MaskDrag::In;
        if (m_maskDrag != MaskDrag::None) m_maskNode = i;
    }
    for (int i = 0; i < int(k->nodes.size()) && m_maskDrag == MaskDrag::None; ++i)
        if (near(frameToWidget(comp, k->nodes[size_t(i)].p), 7)) {
            m_maskDrag = MaskDrag::Node;
            m_maskNode = i;
        }
    if (m_maskDrag == MaskDrag::None && maskOutline(*k).containsPoint(p, Qt::OddEvenFill)) {
        m_maskDrag = MaskDrag::Body;
        m_maskLast = widgetToFrame(comp, p, k->bounds().center());
    }
    if (m_maskDrag == MaskDrag::None) return false;
    if (m_maskDrag == MaskDrag::Node) {
        const bool extend = e->modifiers() & Qt::ControlModifier;
        if (!extend && std::find(m_maskSelection.begin(), m_maskSelection.end(), m_maskNode) == m_maskSelection.end())
            m_maskSelection = {m_maskNode};
        else if (extend) {
            const auto it = std::find(m_maskSelection.begin(), m_maskSelection.end(), m_maskNode);
            if (it == m_maskSelection.end()) m_maskSelection.push_back(m_maskNode);
            else { m_maskSelection.erase(it); m_maskDrag = MaskDrag::None; }
        }
    } else if (m_maskDrag != MaskDrag::Body) {
        m_maskSelection = {m_maskNode};
    }
    m_masksBefore = l->masks;
    return true;
}

void MappingView::mouseDoubleClickEvent(QMouseEvent *e)
{
    if (m_drawing) {
        finishDrawing();
        return;
    }
    QOpenGLWidget::mouseDoubleClickEvent(e);
}
