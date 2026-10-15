#include "Engine.h"
#include "MappingView.h"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QMouseEvent>
#include <QKeyEvent>
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
        for (int edge = 0; edge < 4; ++edge)
            check(!view.findChild<QToolButton *>(QStringLiteral("maskAlign%1").arg(edge)), "no alignment arrows in preview");
        magnet->setChecked(true);
        auto at = [](double x, double y) { return QPointF(24 + x*552, 24 + y*552); };
        auto anchor = [&] { return engine.layer(index)->masks[0].nodes[0].p; };
        auto move = [&](QPointF p, Qt::KeyboardModifiers mods) {
            send(QEvent::MouseMove, p, Qt::NoButton, Qt::LeftButton, mods);
        };
        auto guidePixels = [&] {
            const QImage image = view.grabFramebuffer();
            int count = 0;
            for (int yy = 0; yy < image.height(); ++yy)
                for (int xx = 0; xx < image.width(); ++xx) {
                    const QColor c = image.pixelColor(xx, yy);
                    if (c.red() > 180 && c.blue() > 130 && c.green() < 120) ++count;
                }
            return count;
        };
        click(point(0));
        const QPointF start = point(0);
        send(QEvent::MouseButtonPress, start, Qt::LeftButton, Qt::LeftButton, Qt::NoModifier);
        move(at(.753, .45), Qt::NoModifier);
        check(close(anchor().x(), .753), "without Shift a nearby point does not catch the anchor");
        const int freePixels = guidePixels();
        move(at(.753, .45), Qt::ShiftModifier);
        check(close(anchor().x(), .75) && close(anchor().y(), .45), "Shift snaps vertically to another anchor without changing Y");
        check(guidePixels() > freePixels + 100, "vertical snap draws a visible temporary guide");
        QKeyEvent releaseShift(QEvent::KeyRelease, Qt::Key_Shift, Qt::NoModifier);
        QApplication::sendEvent(&view, &releaseShift);
        check(guidePixels() <= freePixels + 10, "guide disappears immediately on Shift release");
        move(at(.753, .45), Qt::NoModifier);
        check(close(anchor().x(), .753), "releasing Shift restores free movement");
        move(at(.70, .45), Qt::ShiftModifier);
        check(close(anchor().x(), .70), "Shift does not attract points outside the pixel tolerance");
        move(at(.55, .753), Qt::ShiftModifier);
        check(close(anchor().x(), .55) && close(anchor().y(), .75), "Shift snaps horizontally without changing X");
        check(guidePixels() > freePixels + 100, "horizontal snap draws its guide");
        move(at(.753, .753), Qt::ShiftModifier);
        check(close(anchor().x(), .75) && close(anchor().y(), .75), "near another anchor both coordinates snap together");
        send(QEvent::MouseButtonRelease, at(.753, .753), Qt::LeftButton, Qt::NoButton, Qt::ShiftModifier);
        check(guidePixels() <= freePixels + 10, "mouse release clears the guides");
        undo.undo(); check(close(anchor().x(), .35) && close(anchor().y(), .25), "whole snapped gesture is one undo step");
        undo.redo(); check(close(anchor().x(), .75) && close(anchor().y(), .75), "snapped gesture can be redone");
        view.hide();
    }
    engine.shutdown();
    return failures ? 1 : 0;
}
