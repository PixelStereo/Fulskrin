#include "Engine.h"
#include "MappingView.h"
#include "PreviewWorkspace.h"
#include <QApplication>
#include <QComboBox>
#include <QMenu>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QToolButton>
#include <QTimer>
#include <cstdio>
int main(int argc, char **argv)
{
    QSurfaceFormat format;
    format.setVersion(3, 3);
    format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    app.setApplicationName("PinolPreviewTest");
    Engine engine;
    QString error;
    if (!engine.initialize(&error)) { std::fprintf(stderr, "%s\n", qPrintable(error)); return 1; }
    engine.newProject();
    engine.setCompositionSize(QSize(64, 64));
    const int index = engine.addLayer("Test layer");
    QTemporaryDir directory;
    QImage image(64, 64, QImage::Format_RGBA8888); image.fill(Qt::red);
    const QString path = directory.path() + "/red.png";
    if (!image.save(path) || !engine.setLayerImage(index, path, &error)) return 1;
    int failures = 0;
    auto check = [&](bool ok, const char *message) { if (!ok) { ++failures; std::printf("FAIL preview UI: %s\n", message); } };
    {
        auto *mapping = new MappingView(&engine);
        PreviewWorkspace workspace(&engine, mapping);
        workspace.resize(960, 700);
        workspace.show();
        auto *layout = workspace.findChild<QComboBox *>("previewLayout");
        check(layout != nullptr, "layout selector present");
        auto *source = workspace.findChild<QToolButton *>("previewSource0");
        for (int count = 1; count <= 4; ++count) {
            layout->setCurrentIndex(count - 1);
            engine.renderFrame(); workspace.refreshFrames(); app.processEvents();
            for (int slot = 0; slot < 4; ++slot)
                check(workspace.findChild<QToolButton *>(QString("previewSource%1").arg(slot))->isVisible() == (slot < count), "visible cells follow selected split");
        }
        QMetaObject::invokeMethod(source->menu(), "aboutToShow", Qt::DirectConnection);
        bool selected = false;
        for (auto *action : source->menu()->actions()) if (action->menu() && action->text() == "Layers")
            for (auto *layer : action->menu()->actions()) if (layer->text() == "Test layer") { layer->trigger(); selected = true; }
        check(selected && !mapping->isVisible(), "layer source replaces mapping view");
        engine.renderFrame(); workspace.refreshFrames(); app.processEvents();
        source->menu()->actions().first()->trigger(); app.processEvents();
        check(mapping->isVisible(), "Composition restores mapping editor");
        layout->setCurrentIndex(0); app.processEvents();
        check(!workspace.grab().isNull(), "preview renders to a widget image");
        if (argc > 1) check(workspace.grab().save(QString::fromLocal8Bit(argv[1])), "save UI capture");
        workspace.hide();
    }
    engine.shutdown();
    return failures ? 1 : 0;
}
