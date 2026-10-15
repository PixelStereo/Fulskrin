#pragma once
#include "GlDraw.h"
#include "Mapping.h"
#include "Masks.h"
#include <QOpenGLWidget>
#include <QPointF>
#include <QRectF>
#include <vector>

class Engine;
class QUndoStack;
class QLabel;
class QToolButton;
class QDoubleSpinBox;

// Preview of the whole composition + interactive mapping editing for the selected layer. The viewports are
// drawn as frames over it, with their names; dragging a frame moves the viewport in the composition.
// Zoom (wheel, pinch, −/+/Fit buttons) gives finer control when moving the layer or its points:
// moves are computed in composition coordinates, so a higher zoom means smaller steps.
// The selected points show their position in composition pixels; the last one clicked can be typed in
// (bottom left), the other selected points following it.
// Magnetism (button next to the zoom): a dragged point is caught by the corners and edges around it — the
// composition's, its center lines, the viewports' frames, the other layers', the other points of the layer —
// and a dragged layer or viewport by its edges or center. Hold Shift while dragging to snap; otherwise move freely.
class MappingView : public QOpenGLWidget
{
    Q_OBJECT
public:
    explicit MappingView(Engine *engine, QWidget *parent = nullptr);
    ~MappingView() override;

    void setLayer(int index);
    void setShowAllOutlines(bool on) { m_showAll = on; update(); }
    void setUndoStack(QUndoStack *s) { m_undo = s; }
    void zoomBy(double factor);           // around the center of the view
    void zoomToFit();
    double zoom() const { return m_zoom; }
    // Masks (Masks.h): the current layer's mask edited here (its points, its handles, dragged whole), and a new one
    // drawn point by point (click; drag to curve; the first point, a double-click or Enter closes it; Esc cancels)
    void setMaskIndex(int index); // -1: none
    void setMaskEditMode(bool on); // preview selects mask points or layer geometry explicitly
    void startMaskDrawing();

signals:
    void masksEdited();         // a mask moved or drawn here
    void maskPicked(int index); // the mask just drawn
    void layerPicked(int index);
    void mappingEdited();

protected:
    void initializeGL() override;
    void paintGL() override;
    void mousePressEvent(QMouseEvent *e) override;
    void mouseMoveEvent(QMouseEvent *e) override;
    void mouseReleaseEvent(QMouseEvent *e) override;
    void mouseDoubleClickEvent(QMouseEvent *e) override;
    void contextMenuEvent(QContextMenuEvent *e) override;
    void keyPressEvent(QKeyEvent *e) override;
    void wheelEvent(QWheelEvent *e) override;
    void resizeEvent(QResizeEvent *e) override;
    bool event(QEvent *e) override;
    bool focusNextPrevChild(bool next) override; // Tab cycles through the handles

private:
    struct Handle {
        int kind = -1; // 0 = corner, 1 = mesh point
        int i = 0, j = 0;
        bool valid() const { return kind >= 0; }
        bool operator==(const Handle &o) const { return kind == o.kind && i == o.i && j == o.j; }
    };

    Mapping *mapping() const;
    // Members of a group are mapped inside the group's canvas, itself mapped by the group, and so on up (lock held)
    std::vector<const Mapping *> groupMappings(int layer) const;
    bool isViewport(int layer) const;
    int hitViewportFrame(QPointF widgetPos) const;
    void paintScene();
    void paintViewportNames();
    void paintCoordinates();     // labels of the selected points (composition pixels)
    // Magnetism (lock held). Targets in composition coordinates (normalized, origin top left).
    struct SnapTargets {
        std::vector<double> xs, ys;   // lines
        std::vector<QPointF> points;  // corners, centers, points
    };
    SnapTargets snapTargets() const;
    QPointF snapPoint(QPointF p, const SnapTargets &t);  // nearest point, else each axis on its own lines
    QPointF snapBox(const QRectF &box, const SnapTargets &t); // offset catching its edges or its center
    bool snapping(Qt::KeyboardModifiers mods) const;
    void clearGuides();
    void paintGuides();
    void refreshCoordinateBar(); // values of the point typed in, follows the selection and the mapping
    void typeCoordinate();       // a value typed: the point (and the selection) moves there
    QPointF outOf(int layer, QPointF canvas) const;
    QPointF canvasOf(int layer, QPointF out) const;
    QRectF fitRect() const;  // composition fitted in the widget (zoom 1)
    QRectF viewRect() const; // with zoom and pan
    void zoomAt(QPointF widgetPos, double factor);
    void updateZoomLabel();
    QPointF toWidget(QPointF norm) const;
    QPointF toNorm(QPointF widgetPos) const;
    QPointF handlePos(const Handle &h) const;
    void moveHandle(const Handle &h, QPointF norm);
    Handle hitHandle(QPointF widgetPos) const;
    // Grips around the selected layer (MadMapper-like): scale above its top edge, rotation at the side (widget pixels)
    struct Grips {
        QPointF center, top, scale, right, rotate;
    };
    Grips layerGrips() const;
    enum class Grip { None, Scale, Rotate };
    Grip hitGrip(QPointF widgetPos) const;
    void paintGrips(const Grips &g);
    bool insideLayer(int index, QPointF widgetPos) const;
    std::vector<QPointF> outline(const Mapping &m, int steps) const;
    void pushLine(std::vector<float> &v, QPointF a, QPointF b) const;
    void pushRect(std::vector<float> &v, QPointF c, float half) const;

    Engine *m_engine;
    QUndoStack *m_undo = nullptr;
    Mapping m_dragBefore; // state at the start of the gesture, for undo
    GlDraw m_draw;
    int m_layer = -1;
    bool m_showAll = true;

    // Selection: one or several handles (Ctrl/⌘+click, Ctrl/⌘+drag a rectangle, Ctrl/⌘+A); moved together
    std::vector<Handle> m_selection;
    Handle m_primary; // last handle clicked (Tab goes on from it)
    bool isSelected(const Handle &h) const;
    std::vector<Handle> allHandles() const;
    void moveSelection(QPointF delta);
    bool m_dragHandle = false, m_dragLayer = false, m_rubber = false;
    bool m_dragScale = false, m_dragRotate = false;     // the grips of the layer
    QPointF m_gestureCenter, m_gestureStart;          // the layer's center and the press, widget pixels
    QPointF m_lastNorm, m_rubberStart, m_rubberEnd;
    QPointF m_dragRaw;                            // the dragged point without magnetism (canvas of the layer)
    QPointF m_dragOffsetRaw, m_dragOffsetApplied; // the dragged layer: its move without / with magnetism
    QRectF m_dragStartBox;                        // its bounds when the drag started (canvas)
    std::vector<double> m_guideX, m_guideY;       // what caught it, drawn while dragging
    bool m_guidePoint = false;
    QPointF m_guideAt;
    QToolButton *m_magnetButton = nullptr;
    QToolButton *m_autoSelectButton = nullptr;
    QToolButton *m_maskModeButton = nullptr;
    bool m_maskEditMode = false;

    // Masks
    int m_maskIndex = -1;
    bool m_drawing = false, m_drawPressing = false;
    std::vector<MaskNode> m_drawNodes; // the mask being drawn
    QPointF m_cursor;
    enum class MaskDrag { None, Node, In, Out, Body } m_maskDrag = MaskDrag::None;
    int m_maskNode = -1;  // the point dragged, or last clicked
    std::vector<int> m_maskSelection; // Ctrl/Command-click multiple points for alignment
    QPointF m_maskLast;   // the frame point of the last move (dragging it whole)
    std::vector<Mask> m_masksBefore;
    Mask *editedMask();   // lock held
    bool maskInComposition(const Mask *k) const;
    QPointF frameToWidget(bool composition, QPointF q) const;               // lock held
    QPointF widgetToFrame(bool composition, QPointF w, QPointF guess) const; // lock held
    QPolygonF maskOutline(const Mask &k) const;                              // in the widget
    void paintMasks();
    bool pressMask(QMouseEvent *e); // lock held; true: taken by the masks
    void finishDrawing();
    void cancelDrawing();

    double m_zoom = 1.0;
    QPointF m_pan;                // offset of the view center, widget pixels
    bool m_panning = false;
    QPointF m_panLast;
    QWidget *m_zoomBar = nullptr;
    QLabel *m_zoomLabel = nullptr;
    QWidget *m_coordBar = nullptr;
    QLabel *m_coordName = nullptr;
    QDoubleSpinBox *m_coordX = nullptr, *m_coordY = nullptr;
};
