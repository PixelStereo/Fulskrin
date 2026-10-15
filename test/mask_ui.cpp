#include "Engine.h"
#include "MappingView.h"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QMouseEvent>
#include <QSurfaceFormat>
#include <QToolButton>
#include <QUndoStack>
#include <cmath>
#include <cstdio>
int main(int argc, char **argv)
{
    QSurfaceFormat format; format.setVersion(3, 3); format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv); app.setApplicationName("PinolMaskUITest");
    Engine engine; QString error;
    if (!engine.initialize(&error)) return 1;
    engine.newProject(); engine.setCompositionSize(QSize(100, 100));
    const int index = engine.addLayer("Mask fixture");
    engine.setLayerText(index);
    const quint64 id = engine.layerId(index);
    Mask mask = Mask::rectangle(QRectF(.25, .25, .5, .5));
    mask.attach = Mask::Attach::Composition;
    engine.setLayerMasks(id, {mask});
    int failures = 0;
    auto check = [&](bool ok, const char *reason) { if (!ok) { ++failures; std::printf("FAIL mask_ui: %s\n", reason); } };
    auto close = [](double a, double b) { return std::abs(a - b) < 1e-6; };
    {
        QUndoStack undo;
        MappingView view(&engine); view.setUndoStack(&undo); view.setLayer(index); view.resize(600, 600); view.show();
        engine.renderFrame(); app.processEvents();
        auto *mode = view.findChild<QToolButton *>("mappingSelectionMode");
        auto *magnet = view.findChild<QToolButton *>("mappingMagnet");
        check(mode && magnet, "selection mode and magnet controls present");
        if (!mode || !magnet) return 1;
        auto point = [&](int node) { const auto p = engine.layer(index)->masks[0].nodes[size_t(node)].p; return QPointF(24 + p.x()*552, 24 + p.y()*552); };
        auto send = [&](QEvent::Type type, QPointF position, Qt::MouseButton button, Qt::MouseButtons buttons, Qt::KeyboardModifiers mods) {
            QMouseEvent event(type, position, view.mapToGlobal(position.toPoint()), button, buttons, mods);
            QApplication::sendEvent(&view, &event);
        };
        auto click = [&](QPointF p, Qt::KeyboardModifiers mods = Qt::NoModifier) {
            send(QEvent::MouseButtonPress, p, Qt::LeftButton, Qt::LeftButton, mods);
            send(QEvent::MouseButtonRelease, p, Qt::LeftButton, Qt::NoButton, mods);
            view.grabFramebuffer();
        };
        mode->setChecked(true);
        int picked = -1;
        QObject::connect(&view, &MappingView::maskPicked, &view, [&](int maskIndex) { picked = maskIndex; });
        click(point(0));
        check(picked == 0, "Mask mode picks a mask directly without prior inspector selection");
        auto *x = view.findChild<QDoubleSpinBox *>("mappingCoordX");
        auto *y = view.findChild<QDoubleSpinBox *>("mappingCoordY");
        check(x && y && x->isVisible() && close(x->value(), 25), "numeric coordinates follow picked node");
        x->setValue(35);
        check(close(engine.layer(index)->masks[0].nodes[0].p.x(), .35), "typed X updates normalized node coordinate");
        undo.undo(); check(close(engine.layer(index)->masks[0].nodes[0].p.x(), .25), "numeric edit undo");
        undo.redo(); check(close(engine.layer(index)->masks[0].nodes[0].p.x(), .35), "numeric edit redo");
        click(point(1), Qt::ControlModifier);
        auto *align = view.findChild<QToolButton *>("maskAlign0");
        check(align && align->isEnabled(), "multi-node selection enables alignment");
        align->click();
        check(close(engine.layer(index)->masks[0].nodes[0].p.x(), engine.layer(index)->masks[0].nodes[1].p.x()), "align left moves selected anchors");
        undo.undo();
        click(point(0)); magnet->setChecked(true);
        const QPointF start = point(0), nearCenter(24 + .503*552, start.y());
        send(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        send(QEvent::MouseMove, nearCenter, Qt::NoButton, Qt::LeftButton, Qt::NoModifier);
        send(QEvent::MouseButtonRelease, nearCenter, Qt::LeftButton, Qt::NoButton, Qt::NoModifier);
        check(close(engine.layer(index)->masks[0].nodes[0].p.x(), .5), "mask anchor snaps to composition center");
        view.hide();
    }
    engine.shutdown();
    return failures ? 1 : 0;
}
