#include "Engine.h"
#include "MediaBin.h"
#include <QFile>
#include "MainWindow.h"
#include "SequencePanel.h"
#include "SnapshotPanel.h"
#include "TimelinePanel.h"
#include "LayerInspector.h"
#include "PresetEditor.h"
#include "ColorPicker.h"
#include <QComboBox>
#include "Commands.h"
#include "LayerTable.h"
#include "Widgets.h"
#include <QTableWidget>
#include <QAction>
#include <QMenu>
#include <QUndoStack>
#include <QListWidget>
#include <QTreeWidget>
#include <QTabWidget>
#include <QPushButton>
#include <QApplication>
#include <QSettings>
#include <QStackedWidget>
#include <QSurfaceFormat>
#include <QTabBar>
#include <QTemporaryDir>
#include <cstdio>
#include <cmath>
int main(int argc, char **argv)
{
    QSurfaceFormat format; format.setVersion(3, 3); format.setProfile(QSurfaceFormat::CoreProfile);
    QSurfaceFormat::setDefaultFormat(format);
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);
    QApplication app(argc, argv);
    QTemporaryDir settings;
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope, settings.path());
    app.setOrganizationName("PinolTests"); app.setApplicationName("Workspace");
    Engine engine; QString error;
    if (!engine.initialize(&error)) return 1;
    engine.newProject();
    // Two CTest entries share this program: workspace_ui (no argument; also run under Valgrind, where every window
    // costs minutes) and selection_ui (--selection: multiple selections and reopened projects, never under Valgrind)
    bool selection = false;
    QString capture;
    for (int k = 1; k < argc; ++k) {
        const QString arg = QString::fromLocal8Bit(argv[k]);
        if (arg == QLatin1String("--selection")) selection = true;
        else capture = arg;
    }
    int failures = 0;
    auto check = [&](bool ok, const char *reason) {
        if (!ok) { ++failures; std::printf("FAIL %s: %s\n", selection ? "selection_ui" : "workspace_ui", reason); }
    };
    if (!selection) {
        MainWindow window(&engine); window.setAutosaveEnabled(false); window.setQuiet(true);
        window.show(); app.processEvents();
        auto *defaultColorView = window.findChild<QComboBox *>("defaultColorPickerView");
        check(defaultColorView && defaultColorView->currentData().toInt() == int(ColorPicker::HSL), "Settings defaults to HSL");
        if (defaultColorView) {
            ColorPicker unchosen, chosen;
            chosen.setView(ColorPicker::Palette);
            defaultColorView->setCurrentIndex(defaultColorView->findData(int(ColorPicker::RGB)));
            check(unchosen.view() == ColorPicker::RGB && chosen.view() == ColorPicker::Palette,
                  "Settings changes reach unchosen pickers and preserve local choices");
            defaultColorView->setCurrentIndex(defaultColorView->findData(int(ColorPicker::HSL)));
        }
        auto *tabs = window.findChild<QTabBar *>("showWorkspaceTabs");
        auto *pages = window.findChild<QStackedWidget *>("showWorkspacePages");
        auto *lists = window.findChild<QStackedWidget *>("snapshotPresetLists");
        auto *inspector = window.findChild<QStackedWidget *>("snapshotPresetInspector");
        auto *sequences = window.findChild<SequenceWindow *>();
        auto *timelines = window.findChild<TimelineWindow *>();
        check(tabs && pages && lists && inspector && sequences && timelines, "all docked workspace controls present");
        if (tabs && pages && lists && inspector && sequences && timelines) {
            check(!sequences->isWindow() && !timelines->isWindow(), "editors embedded rather than floating");
            check(tabs->count() == 3 && tabs->tabText(0) == "Snapshots" && tabs->tabText(1) == "Presets" && tabs->tabText(2) == "Timelines", "three named workspace tabs");
            auto *library = window.findChild<QWidget *>("showLibraryColumn");
            check(library && tabs->parentWidget() == library, "tabs belong only to the library column");
            const QPoint inspectorOrigin = pages->mapTo(&window, QPoint());
            for (int tab : {0, 1, 2, 0, 2, 1}) {
                tabs->setCurrentIndex(tab); app.processEvents();
                check(pages->mapTo(&window, QPoint()) == inspectorOrigin, "inspection area stays in the same position");
                check(sequences->isVisible(), "sequence editor stays visible in all workspace modes");
                check(pages->currentIndex() == (tab == 2 ? 1 : 0), "workspace page follows tab");
                if (tab < 2) check(lists->currentIndex() == tab && inspector->currentIndex() == tab, "list and inspector follow Snapshots/Presets tab");
                else check(timelines->editor()->isVisible(), "timeline editor replaces inspector in timeline tab");
            }
            check(sequences->mapTo(&window, QPoint()).x() < tabs->mapTo(&window, QPoint()).x(), "sequences sit to the left of the tabbed workspace");
            auto *viewportList = window.findChild<QListWidget *>("viewportList");
            auto *viewportInspector = window.findChild<LayerInspector *>("viewportInspector");
            auto *mainTabs = window.findChild<QTabWidget *>("mainInspectorTabs");
            check(mainTabs && mainTabs->tabText(1) == "Viewports" && mainTabs->tabText(2) == "Composition", "Viewports sits between Layer and Composition");
            check(viewportList && viewportList->currentRow() == 0 && viewportInspector && viewportInspector->layerIndex() == engine.viewports().front(), "first viewport selected by default");
            auto *update = window.findChild<QPushButton *>("snapshotUpdate");
            check(update && lists->isAncestorOf(update), "snapshot Update is in library toolbar");
            auto *remove = window.findChild<QPushButton *>("presetDelete");
            check(remove && lists->isAncestorOf(remove) && remove->text() == "−", "preset minus is in library toolbar");
            // Media Bin commands must target a source layer even after inspecting a viewport.
            auto *bin = window.findChild<MediaBin *>();
            QTemporaryDir shaderDir;
            const auto shaderPath = shaderDir.filePath("generator.fs");
            QFile shader(shaderPath);
            check(shader.open(QIODevice::WriteOnly), "create ISF fixture");
            shader.write("/*{\"ISFVSN\":\"2\",\"INPUTS\":[]}*/\nvoid main(){gl_FragColor=vec4(1.0);}"); shader.close();
            if (bin && mainTabs) {
                const int before = engine.layerCount();
                mainTabs->setCurrentIndex(1);
                bin->loadIntoNewLayerRequested(shaderPath);
                check(engine.layerCount() == before + 1, "ISF new-layer action creates a layer from Viewports tab");
                int loaded = -1;
                for (int i = 0; i < engine.layerCount(); ++i)
                    if (engine.layer(i)->sourcePath == shaderPath) loaded = i;
                check(loaded >= 0 && engine.layer(loaded)->type == SourceType::Isf, "new layer loads ISF generator");
                check(mainTabs->currentIndex() == 0, "source layer becomes selected");
                if (loaded >= 0) {
                    engine.setLayerText(loaded);
                    mainTabs->setCurrentIndex(1);
                    bin->useAsSourceRequested(shaderPath);
                    check(engine.layer(loaded)->type == SourceType::Isf, "selected-layer action replaces source with ISF");
                }
            }
            // A complete Spatial tab captures corner values and mesh in one undoable edit.
            auto *presets = PresetEditor::instance();
            auto *undo = window.findChild<QUndoStack *>();
            const int surface = engine.addLayer("Geometry surface");
            const quint64 surfaceId = engine.layerId(surface);
            Engine::Snapshot reference; reference.name = "Target cue"; reference.layers = engine.captureLayers();
            reference.composition = engine.captureComposition();
            const int snapshotIndex = engine.addSnapshot(reference);
            const quint64 snapshotId = engine.snapshot(snapshotIndex).id;
            auto *snapshotList = window.findChild<QTreeWidget *>("snapshotList");
            auto *snapshotRow = snapshotList && snapshotList->topLevelItemCount() ? snapshotList->topLevelItem(0) : nullptr;
            engine.recallSnapshot(snapshotIndex);
            check(snapshotRow && snapshotList->topLevelItem(0) == snapshotRow, "snapshot recall preserves library rows and parameter editors");
            auto *snapshotEditor = PresetEditor::instance();
            snapshotEditor->selectSnapshot(snapshotId);
            engine.setParameter(surfaceId, "opacity", .37);
            QMenu valueMenu;
            snapshotEditor->fillMenu(&valueMenu, surfaceId, {"opacity"});
            QAction *selectedUpdate = nullptr;
            QMenu *snapshotChoices = nullptr;
            for (auto *action : valueMenu.actions()) {
                if (action->text() == "Update in Selected Snapshot (Target cue)") selectedUpdate = action;
                if (action->text() == "Update in Snapshot") snapshotChoices = action->menu();
            }
            check(selectedUpdate && snapshotChoices && !snapshotChoices->actions().empty(), "snapshot update actions include selected name and target list");
            if (selectedUpdate) {
                const auto original = engine.snapshot(snapshotIndex).layers;
                selectedUpdate->trigger();
                const auto updated = engine.snapshot(snapshotIndex).layers;
                bool changed = false;
                for (int i = 0; i < updated.size(); ++i) {
                    auto a = original[i].toObject(), b = updated[i].toObject();
                    if (b.value("id").toString().toULongLong() != surfaceId) { check(a == b, "snapshot update preserves other layers"); continue; }
                    auto params = b.value("params").toObject();
                    check(std::abs(params.value("opacity").toDouble() - .37) < 1e-6, "selected snapshot receives live parameter value");
                    auto oldParams = a.value("params").toObject(); oldParams["opacity"] = params.value("opacity"); a["params"] = oldParams;
                    check(a == b, "snapshot update preserves every unrelated field"); changed = true;
                }
                check(changed, "target layer found in updated snapshot");
                window.findChild<QUndoStack *>()->undo();
                check(engine.snapshot(snapshotIndex).layers == original, "partial snapshot update is undoable");
            }

            engine.layer(surface)->mapping.resetMesh(5, 3);
            engine.layer(surface)->mapping.offsets[2] = QPointF(.1, -.2);
            ++engine.layer(surface)->mapping.revision;
            QMenu category;
            presets->fillCategoryMenu(&category, surfaceId, "Spatial");
            check(!category.actions().empty(), "Spatial tab offers preset capture");
            if (!category.actions().empty()) category.actions().first()->trigger();
            const quint64 presetId = presets->selected();
            int presetIndex = engine.indexOfPreset(presetId);
            check(presetIndex >= 0 && engine.preset(presetIndex).geometry.contains(QString::number(surfaceId)), "Spatial capture includes mesh");
            check(presetIndex >= 0 && engine.preset(presetIndex).indexOf(surfaceId, "corner/top_left/x") >= 0, "Spatial capture includes four-corner geometry");
            if (presetIndex >= 0 && undo) {
                auto value = engine.preset(presetIndex).values.front();
                cmd::editPresets(undo, &engine, "Delay", [&](std::vector<Engine::Preset> &all) { all[size_t(presetIndex)].values.front().delay = 2; });
                check(engine.preset(presetIndex).values.front().delay == 2, "delay-only preset edit is retained");
                undo->undo(); check(engine.preset(presetIndex).values.front().delay == value.delay, "delay-only edit can be undone");
                engine.layer(surface)->mapping.resetMesh(7, 7);
                undo->push(new cmd::RecallPreset(&engine, presetId));
                check(engine.layer(surface)->mapping.cols == 5, "preset PLAY restores stored mesh");
                undo->undo(); check(engine.layer(surface)->mapping.cols == 7, "undo restores previous mesh");
                undo->redo(); check(engine.layer(surface)->mapping.cols == 5, "redo restores stored mesh again");
                Interaction driver = Interaction::made(InteractionSource::Osc);
                driver.layer = surfaceId; driver.param = "opacity"; driver.name = "Surface fader";
                const quint64 driverId = engine.addInteraction(driver);
                presets->addInteraction(driverId, false);
                check(engine.preset(engine.indexOfPreset(presetId)).interactions.size() == 1, "interaction-only edit is retained in selected preset");
                undo->undo(); check(engine.preset(engine.indexOfPreset(presetId)).interactions.empty(), "interaction capture is undoable");
                // Undo must cancel a delayed value; it must never return after its deadline.
                Engine::Preset delayed; delayed.name = "Delayed test"; delayed.fade = 0;
                auto target = engine.presetValue(surfaceId, "opacity"); target.value = .1; target.delay = 1;
                delayed.values = {target}; const int di = engine.addPreset(delayed);
                engine.setParameter(surfaceId, "opacity", .8);
                undo->push(new cmd::RecallPreset(&engine, engine.preset(di).id)); undo->undo();
                engine.advanceFades(2);
                check(std::abs(engine.parameterValue(surfaceId, "opacity").toDouble() - .8) < .01, "undone delayed recall stays canceled after deadline");
            }
            tabs->setCurrentIndex(0); app.processEvents();
            if (!capture.isEmpty()) check(window.grab().save(capture), "save workspace capture");
        }
        // The controller is a direct MainWindow child and may die before the reparented lists.
        delete window.findChild<PresetEditor *>();
        if (lists) {
            if (auto *presetList = lists->widget(1)->findChild<QTreeWidget *>())
                presetList->setCurrentItem(nullptr);
        }
        app.processEvents();
        window.hide();
    }
    // An opened project may put another layer at the index shown: the inspector follows the layer, not the index
    if (selection) {
        engine.newProject();
        const QString solid = QStringLiteral(TEST_DIR) + "/../isf/generators/SolidColor.fs";
        engine.addViewport("Front", QSize(640, 360));
        for (const char *name : {"Under", "Over"}) engine.setLayerIsf(engine.addLayer(name), solid, &error);
        QTemporaryDir dir;
        const QString project = dir.path() + "/reopen.pinol";
        check(engine.saveProject(project, {}, &error), "save project to reopen");
        MainWindow window(&engine); window.setAutosaveEnabled(false); window.setQuiet(true);
        window.show(); app.processEvents();
        check(window.openProject(project, false), "reopen project");
        app.processEvents();
        LayerInspector *inspector = nullptr;
        for (LayerInspector *candidate : window.findChildren<LayerInspector *>())
            if (candidate->objectName() != "viewportInspector") inspector = candidate;
        check(inspector && inspector->layerIndex() >= 0 &&
                  inspector->layerId() == engine.layerId(inspector->layerIndex()),
              "inspector edits the layer it shows after a project is opened");
        if (auto *opacity = inspector ? inspector->findChild<SliderField *>("inspectorOpacity") : nullptr) {
            emit opacity->valueEdited(30);
            check(std::abs(engine.parameterValue(engine.layerId(inspector->layerIndex()), "opacity").toDouble() - .3) < 1e-6,
                  "inspector edit after opening a project reaches the layer");
        }
        window.hide();
    }
    // A multiple selection: the inspector's edits, the list's opacity bars and Paste Attributes reach all of it
    if (selection) {
        engine.newProject();
        const QString solid = QStringLiteral(TEST_DIR) + "/../isf/generators/SolidColor.fs";
        QList<quint64> ids;
        for (const char *name : {"A", "B", "C"}) {
            const int i = engine.addLayer(name);
            engine.setLayerIsf(i, solid, &error);
            ids << engine.layerId(i);
        }
        MainWindow window(&engine); window.setAutosaveEnabled(false); window.setQuiet(true);
        window.show(); app.processEvents();
        auto *table = qobject_cast<QTableWidget *>(window.findChild<LayerTable *>()->table());
        auto *undo = window.findChild<QUndoStack *>();
        LayerInspector *inspector = nullptr;
        for (LayerInspector *candidate : window.findChildren<LayerInspector *>())
            if (candidate->objectName() != "viewportInspector") inspector = candidate;
        auto opacityOf = [&](quint64 id) { return engine.parameterValue(id, "opacity").toDouble(); };
        check(table && undo && inspector, "layer list, undo stack and inspector present");
        if (table && undo && inspector) {
            const int a = engine.indexOfId(ids[0]), b = engine.indexOfId(ids[1]), c = engine.indexOfId(ids[2]);
            table->clearSelection();
            table->setCurrentCell(a, 3);
            table->selectionModel()->select(table->model()->index(c, 0),
                                            QItemSelectionModel::Select | QItemSelectionModel::Rows);
            app.processEvents();
            check(inspector->coTargets() == QList<quint64>{ids[2]}, "inspector knows the other selected layer");
            auto *note = inspector->findChild<QLabel *>("multiSelectionNote");
            check(note && note->isVisible(), "inspector says the edit applies to the whole selection");
            auto *opacity = inspector->findChild<SliderField *>("inspectorOpacity");
            check(opacity != nullptr, "inspector opacity field present");
            if (opacity) {
                const int steps = undo->count();
                emit opacity->valueEdited(40);
                check(std::abs(opacityOf(ids[0]) - .4) < 1e-6 && std::abs(opacityOf(ids[2]) - .4) < 1e-6,
                      "inspector opacity edit reaches every selected layer");
                check(std::abs(opacityOf(ids[1]) - 1) < 1e-6, "an unselected layer keeps its value");
                check(undo->count() == steps + 1, "one undo step for the whole selection");
                undo->undo();
                check(std::abs(opacityOf(ids[0]) - 1) < 1e-6 && std::abs(opacityOf(ids[2]) - 1) < 1e-6,
                      "undo restores every selected layer");
            }
            // The list's own opacity bar on a selected row
            if (auto *cell = table->cellWidget(c, 5); auto *bar = cell ? cell->findChild<SliderField *>() : nullptr) {
                emit bar->valueEdited(25);
                check(std::abs(opacityOf(ids[0]) - .25) < 1e-6 && std::abs(opacityOf(ids[2]) - .25) < 1e-6,
                      "list opacity bar of a selected row sets the whole selection");
                undo->undo();
            } else {
                check(false, "list opacity bar is the application's SliderField");
            }
            // Paste Attributes onto the selection: B's compositing copied onto A and C
            engine.setParameter(ids[1], "opacity", .6);
            QAction *copy = nullptr, *pasteAll = nullptr;
            for (QAction *act : window.findChildren<QAction *>()) {
                if (act->text() == "Copy Attributes") copy = act;
                if (act->text() == "Paste All Attributes") pasteAll = act;
            }
            check(copy && pasteAll, "Copy / Paste All Attributes actions present");
            if (copy && pasteAll) {
                table->clearSelection();
                table->setCurrentCell(b, 3); app.processEvents();
                copy->trigger();
                table->clearSelection();
                table->setCurrentCell(a, 3);
                table->selectionModel()->select(table->model()->index(c, 0),
                                                QItemSelectionModel::Select | QItemSelectionModel::Rows);
                app.processEvents();
                pasteAll->trigger();
                check(std::abs(opacityOf(ids[0]) - .6) < 1e-6 && std::abs(opacityOf(ids[2]) - .6) < 1e-6,
                      "paste reaches every selected layer");
            }
            // Viewports: several selected in their list, pasted onto together and edited together
            auto *tabs = window.findChild<QTabWidget *>("mainInspectorTabs");
            auto *viewportList = window.findChild<QListWidget *>("viewportList");
            auto *viewportInspector = window.findChild<LayerInspector *>("viewportInspector");
            const quint64 v1 = engine.layerId(engine.addViewport("V1")), v2 = engine.layerId(engine.addViewport("V2")),
                          v3 = engine.layerId(engine.addViewport("V3"));
            if (tabs) tabs->setCurrentIndex(1);
            app.processEvents();
            check(viewportList && viewportList->selectionMode() == QAbstractItemView::ExtendedSelection,
                  "viewport list takes a multiple selection");
            if (tabs && viewportList && viewportInspector && copy && pasteAll) {
                auto itemOf = [&](quint64 id) -> QListWidgetItem * {
                    for (int k = 0; k < viewportList->count(); ++k)
                        if (viewportList->item(k)->data(Qt::UserRole).toULongLong() == id) return viewportList->item(k);
                    return nullptr;
                };
                engine.setParameter(v1, "opacity", .3);
                viewportList->setCurrentItem(itemOf(v1)); app.processEvents();
                copy->trigger();
                viewportList->setCurrentItem(itemOf(v2));
                if (QListWidgetItem *item = itemOf(v3)) item->setSelected(true);
                app.processEvents();
                check(viewportInspector->coTargets() == QList<quint64>{v3}, "viewport inspector knows the selection");
                pasteAll->trigger();
                check(std::abs(opacityOf(v2) - .3) < 1e-6 && std::abs(opacityOf(v3) - .3) < 1e-6,
                      "paste reaches every selected viewport");
                if (auto *vo = viewportInspector->findChild<SliderField *>("inspectorOpacity")) {
                    emit vo->valueEdited(70);
                    check(std::abs(opacityOf(v2) - .7) < 1e-6 && std::abs(opacityOf(v3) - .7) < 1e-6 &&
                              std::abs(opacityOf(v1) - .3) < 1e-6,
                          "viewport inspector edit reaches every selected viewport only");
                }
            }
        }
        window.hide();
    }
    engine.shutdown();
    return failures ? 1 : 0;
}
