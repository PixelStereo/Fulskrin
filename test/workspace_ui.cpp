#include "Engine.h"
#include "MainWindow.h"
#include "SequencePanel.h"
#include "SnapshotPanel.h"
#include "TimelinePanel.h"
#include <QApplication>
#include <QSettings>
#include <QStackedWidget>
#include <QSurfaceFormat>
#include <QTabBar>
#include <QTemporaryDir>
#include <cstdio>
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
            for (int tab : {0, 1, 2, 0, 2, 1}) {
                tabs->setCurrentIndex(tab); app.processEvents();
                check(sequences->isVisible(), "sequence editor stays visible in all workspace modes");
                check(pages->currentIndex() == (tab == 2 ? 1 : 0), "workspace page follows tab");
                if (tab < 2) check(lists->currentIndex() == tab && inspector->currentIndex() == tab, "list and inspector follow Snapshots/Presets tab");
                else check(timelines->isVisible(), "timeline editor shown in timeline tab");
            }
            check(sequences->mapTo(&window, QPoint()).x() < tabs->mapTo(&window, QPoint()).x(), "sequences sit to the left of the tabbed workspace");
            if (argc > 1) check(window.grab().save(QString::fromLocal8Bit(argv[1])), "save workspace capture");
        }
        window.hide();
    }
    engine.shutdown();
    return failures ? 1 : 0;
}
