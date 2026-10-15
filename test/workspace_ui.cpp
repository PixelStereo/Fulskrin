#include "Engine.h"
#include "MainWindow.h"
#include "SequencePanel.h"
#include "SnapshotPanel.h"
#include "TimelinePanel.h"
#include "LayerInspector.h"
#include "PresetEditor.h"
#include "ColorPicker.h"
#include <QComboBox>
#include "Commands.h"
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
    int failures = 0;
    auto check = [&](bool ok, const char *reason) { if (!ok) { ++failures; std::printf("FAIL workspace_ui: %s\n", reason); } };
    {
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
            // A complete Spatial tab captures corner values and mesh in one undoable edit.
            auto *presets = PresetEditor::instance();
            auto *undo = window.findChild<QUndoStack *>();
            const int surface = engine.addLayer("Geometry surface");
            const quint64 surfaceId = engine.layerId(surface);
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
            if (argc > 1) check(window.grab().save(QString::fromLocal8Bit(argv[1])), "save workspace capture");
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
    engine.shutdown();
    return failures ? 1 : 0;
}
