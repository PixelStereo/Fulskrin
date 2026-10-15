#include "Engine.h"
#include "PresetEditor.h"
#include <QApplication>
#include <QMenu>
#include <QUndoStack>
#include <cmath>
#include <cstdio>

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    Engine engine;
    engine.setFadesManual(true);
    const int row = engine.addLayer("Surface");
    const auto layer = engine.layerId(row);
    QUndoStack undo;
    PresetEditor editor(&engine, &undo);
    Engine::Snapshot snapshot;
    snapshot.name = "Cue A";
    snapshot.layers = engine.captureLayers();
    snapshot.composition = engine.captureComposition();
    const int index = engine.addSnapshot(snapshot);
    const auto target = engine.snapshot(index).id;
    int failures = 0;
    auto check = [&](bool ok, const char *why) { if (!ok) { ++failures; std::printf("FAIL snapshot_update_ui: %s\n", why); } };
    for (const auto &path : {"audio/level","audio/bass","audio/mid","audio/high","audio/onset"}) {
        ParamInfo info;
        check(engine.parameterInfo(0,path,&info) && info.access==ParamInfo::Read, "audio analysis is a readable Internal interaction source");
        check(!engine.setParameter(0,path,.5), "audio analysis cannot be overwritten by presets or OSC");
    }
    QString reason;
    check(engine.snapshotMatches(target, &reason), qPrintable("fresh capture matches: " + reason));
    engine.layer(row)->colorModels = 7;
    check(engine.snapshotMatches(target), "changing displayed color controls is not a scene edit");
    editor.selectSnapshot(target);
    engine.setParameter(layer, "opacity", .37);
    QMenu menu;
    editor.fillMenu(&menu, layer, {"opacity"});
    QAction *selected = nullptr;
    QMenu *choices = nullptr;
    for (auto *action : menu.actions()) {
        if (action->text() == "Update in Selected Snapshot (Cue A)") selected = action;
        if (action->text() == "Update in Snapshot") choices = action->menu();
    }
    check(selected && selected->isEnabled() && choices && choices->actions().size() == 1, "named selected target and snapshot list");
    const auto before = engine.snapshot(index);
    if (selected) selected->trigger();
    auto expected = before.layers[0].toObject();
    auto params = expected.value("params").toObject(); params["opacity"] = engine.parameterValue(layer, "opacity"); expected["params"] = params;
    check(engine.snapshot(index).layers[0].toObject() == expected, "only requested value updated");
    check(engine.snapshot(index).composition == before.composition, "composition untouched");
    check(engine.snapshotMatches(target), "updated snapshot matches scene despite UI changes");
    undo.undo(); check(engine.snapshot(index).layers == before.layers, "undo restores stored values");
    undo.redo(); check(engine.snapshot(index).layers[0].toObject() == expected, "redo reapplies partial update");
    Mask mask = Mask::ellipse(QRectF(.2,.2,.5,.5));
    engine.setLayerMasks(layer, {mask});
    QMenu category;
    editor.fillCategoryMenu(&category, layer, "Masks");
    QAction *categoryUpdate = nullptr;
    for (auto *action : category.actions()) if (action->text() == "Update in Selected Snapshot (Cue A)") categoryUpdate = action;
    check(categoryUpdate, "category menu has snapshot update");
    if (categoryUpdate) categoryUpdate->trigger();
    check(engine.snapshot(index).layers[0].toObject().value("masks") == masks::toJson({mask}), "Masks tab captures geometry");
    undo.undo(); check(!engine.snapshot(index).layers[0].toObject().contains("masks"), "category update undo");
    engine.setParameter(0, "opacity", .63);
    editor.updateSnapshot(target, 0, {"opacity"});
    check(std::abs(engine.snapshot(index).composition.value("opacity").toDouble()-.63)<1e-6, "composition value update");
    std::printf("snapshot_update_ui: %d failures\n", failures);
    return failures ? 1 : 0;
}
