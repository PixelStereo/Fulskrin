#include "Engine.h"
#include "SnapshotPanel.h"
#include <QApplication>
#include <QDoubleSpinBox>
#include <QPushButton>
#include <QSurfaceFormat>
#include <QTreeWidget>
#include <QTreeWidgetItemIterator>
#include <QUndoStack>
#include <cmath>
#include <cstdio>
int main(int argc, char **argv)
{
    QSurfaceFormat format; format.setVersion(3, 3); format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication app(argc, argv); app.setApplicationName("PinolRecallDelayUITest");
    Engine engine; QString error;
    if (!engine.initialize(&error)) return 1;
    engine.newProject(); engine.setCompositionSize(QSize(64, 64));
    const int index = engine.addLayer("Delay fixture");
    const quint64 id = engine.layerId(index);
    Engine::Snapshot snapshot; snapshot.name = "Metadata"; snapshot.layers = engine.captureLayers(true);
    for (int k = 0; k < snapshot.layers.size(); ++k) {
        auto state = snapshot.layers[k].toObject();
        if (state.value("id").toString().toULongLong() == id) state["delay"] = QJsonObject{{"roi", .8}, {"opacity", .5}};
        snapshot.layers[k] = state;
    }
    snapshot.composition = engine.captureComposition();
    snapshot.composition["delay"] = QJsonObject{{"opacity", .7}};
    const int si = engine.addSnapshot(snapshot);
    engine.renderFrame();
    int failures = 0;
    auto check = [&](bool ok, const char *reason) { if (!ok) { ++failures; std::printf("FAIL recall_delay_ui: %s\n", reason); } };
    {
        QUndoStack undo; SnapshotPanel panel(&engine, &undo); panel.resize(1400, 400); panel.show();
        auto *list = panel.findChild<QTreeWidget *>("snapshotList");
        auto *values = panel.findChild<QTreeWidget *>("snapshotValues");
        auto *update = panel.findChild<QPushButton *>("snapshotUpdate");
        check(list && values && update, "snapshot controls present");
        if (list && values && update) {
            list->setCurrentItem(list->topLevelItem(0)); app.processEvents();
            QTreeWidgetItem *roi = nullptr;
            for (QTreeWidgetItemIterator it(values); *it; ++it)
                if ((*it)->data(0, Qt::UserRole + 3).toString().endsWith("/params/roi/right")) { roi = *it; break; }
            check(roi, "ROI value found in snapshot inspector");
            if (roi) {
                values->setCurrentItem(roi); app.processEvents();
                auto *delay = panel.findChild<QDoubleSpinBox *>("snapshotParameterDelay");
                check(delay && std::abs(delay->value() - .8) < 1e-9, "delay editor displays inherited group delay");
                if (delay) delay->setValue(0);
            }
            engine.setParameter(id, "opacity", .4);
            check(update->isEnabled(), "selected snapshot can be updated");
            update->click();
            const auto updated = engine.snapshot(si);
            QJsonObject metadata;
            for (const auto &v : updated.layers) {
                const auto state = v.toObject();
                if (state.value("id").toString().toULongLong() == id) metadata = state.value("delay").toObject();
            }
            check(std::abs(metadata.value("roi").toDouble() - .8) < 1e-9 && std::abs(metadata.value("opacity").toDouble() - .5) < 1e-9, "snapshot update preserves layer delay metadata");
            check(metadata.contains("roi/right") && metadata.value("roi/right").toDouble(-1) == 0, "explicit zero overrides group delay and survives snapshot update");
            check(std::abs(updated.composition.value("delay").toObject().value("opacity").toDouble() - .7) < 1e-9, "snapshot update preserves composition delay metadata");
        }
        panel.hide();
    }
    engine.shutdown();
    return failures ? 1 : 0;
}
