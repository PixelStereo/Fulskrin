#include "Engine.h"
#include "EnduranceLogger.h"
#include "MainWindow.h"
#include "Permissions.h"
#include "Widgets.h"

#include <QFileInfo>
#include <QApplication>
#include <QCommandLineParser>
#include <QMessageBox>
#include <QSurfaceFormat>
#include <QThread>
#include <QTimer>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <thread>

#ifdef Q_OS_MACOS
#include "MacPresentation.h"
#endif

static std::atomic<bool> s_quitDone{false}; // set once quitting is over (the watchdog below)

int main(int argc, char *argv[])
{
    // OpenGL 3.3 core everywhere (macOS then provides 4.1 core). Contexts shared between engine and windows.
    QSurfaceFormat fmt;
    fmt.setRenderableType(QSurfaceFormat::OpenGL);
    fmt.setVersion(3, 3);
    fmt.setProfile(QSurfaceFormat::CoreProfile);
    fmt.setDepthBufferSize(0);
    fmt.setStencilBufferSize(8);
    fmt.setSwapInterval(1);
    QSurfaceFormat::setDefaultFormat(fmt);
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);

    QApplication app(argc, argv);
    QApplication::setApplicationName("Pinol");
    QApplication::setOrganizationName("Pinol");
    QApplication::setApplicationVersion("0.1.0");
    theme::applyToApplication(app);

    QCommandLineParser cli;
    cli.setApplicationDescription("Pinol — multi-layer video mapping, FFmpeg playback, ISF shaders");
    cli.addHelpOption();
    cli.addVersionOption();
    QCommandLineOption renderOpt("render", "Render the project headless and save the output as PNG.", "output.png");
    QCommandLineOption framesOpt("frames", "Number of frames to render before capture (with --render).", "n", "30");
    QCommandLineOption shotOpt("screenshot", "Capture the main window, then quit (tests).", "capture.png");
    QCommandLineOption enduranceOpt("endurance-csv", "Write 1 Hz frame/resource metrics and deadline events to a CSV and JSONL pair.", "session.csv");
    QCommandLineOption strictOpt("strict", "With --render: a layer or effect in error fails the render (exit code 4).");
    cli.addOption(renderOpt);
    cli.addOption(strictOpt);
    cli.addOption(framesOpt);
    cli.addOption(shotOpt);
    cli.addOption(enduranceOpt);
    cli.addPositionalArgument("project", ".pinol project to open");
    // --render exit codes: 0 rendered, 1 OpenGL unavailable, 2 output not written, 3 project not loaded,
    // 4 a layer or an effect in error (--strict)
    cli.process(app);
    const QString project = cli.positionalArguments().value(0);

    Engine engine;
    QString err;
    if (!engine.initialize(&err)) {
        if (cli.isSet(renderOpt)) {
            qCritical("%s", qPrintable(err));
            return 1;
        }
        QMessageBox::critical(nullptr, "Pinol", QStringLiteral("Could not initialize OpenGL:\n") + err);
        return 1;
    }

    // Headless mode: render N frames and write the output (automated tests, project checking).
    if (cli.isSet(renderOpt)) {
        if (!project.isEmpty()) {
            QString perr;
            if (!engine.loadProject(project, nullptr, &perr)) {
                qCritical("Project not loaded: %s", qPrintable(perr.isEmpty() ? project : perr));
                engine.shutdown();
                return 3;
            }
            if (!perr.isEmpty()) qWarning("%s", qPrintable(perr)); // loaded, with warnings (missing media…)
        }
        const int frames = qMax(1, cli.value(framesOpt).toInt());
        for (int i = 0; i < frames; ++i) {
            engine.renderFrame();
            QThread::msleep(16);
        }
        bool layerErrors = false;
        for (int i = 0; i < engine.layerCount(); ++i) {
            Layer *l = engine.layer(i);
            if (!l->error.isEmpty()) {
                qWarning("Layer %s: %s", qPrintable(l->name), qPrintable(l->error));
                layerErrors = true;
            }
            for (auto &fx : l->effects)
                if (!fx->error().isEmpty()) {
                    qWarning("FX %s: %s", qPrintable(fx->name()), qPrintable(fx->error()));
                    layerErrors = true;
                }
        }
        for (int v : engine.viewports()) {
            const quint64 id = engine.layerId(v);
            for (int k = 0; k < kPublishKindCount; ++k) {
                const PublishState st = engine.publishState(id, PublishKind(k));
                if (st.level != PublishState::Off)
                    qInfo("Publishing %s %s: %s (receivers: %d)", qPrintable(engine.layer(v)->name),
                          qPrintable(publishKindName(PublishKind(k))), qPrintable(st.text), st.receivers);
            }
        }
        const bool ok = engine.grabOutput().save(cli.value(renderOpt));
        engine.shutdown();
        if (!ok) {
            qCritical("Output not written: %s", qPrintable(cli.value(renderOpt)));
            return 2;
        }
        return layerErrors && cli.isSet(strictOpt) ? 4 : 0;
    }

    // GUI: rendering runs in its own thread, independent of the UI. The system asks for the camera and local network
    // access first, before the media bin looks for NDI and OMT sources.
    requestSystemPermissions();
#ifdef Q_OS_MACOS
    macKeepLiveOutputActive();
#endif
    engine.start();
    // Start before any project-open or recovery dialog: a modal prompt must not
    // prevent telemetry from recording the show startup and subsequent events.
    EnduranceLogger endurance;
    if (cli.isSet(enduranceOpt)) {
        QString logError;
        if (!endurance.start(engine, cli.value(enduranceOpt), &logError))
            qWarning("Pinol endurance recorder: %s", qPrintable(logError));
    }

    int rc;
    {
        MainWindow w(&engine);
        w.show();
        if (cli.isSet(shotOpt)) {
            w.setAutosaveEnabled(false);
            w.setQuiet(true);
        }
        const bool restored = !cli.isSet(shotOpt) && w.offerRecovery();
        // A project given to open after a session was restored: the restored session is not dropped silently —
        // the same project stays as restored; another one asks first whether to save the session
        if (!project.isEmpty()) {
            const auto same = [](const QString &a, const QString &b) {
                const QString ca = QFileInfo(a).canonicalFilePath(), cb = QFileInfo(b).canonicalFilePath();
                return !ca.isEmpty() && ca == cb;
            };
            if (!restored) w.openProject(project);
            else if (!same(project, engine.projectPath()) && w.maybeSave()) w.openProject(project);
        } else if (!restored && !cli.isSet(shotOpt)) {
            w.openStartupProject(); // the one chosen in Settings ▸ Interface, else stays empty
        }
        if (cli.isSet(shotOpt)) {
            QTimer::singleShot(2500, &w, [&w, &cli, &shotOpt] {
                w.grab().save(cli.value(shotOpt));
                QCoreApplication::exit(0); // no "Save?" dialog
            });
        }
        rc = app.exec();
        // Quitting: the windows, the render thread and the decoders stop below. A last resort if that never ends
        // (a driver or a screen that does not answer): after 10 s the process ends anyway, instead of staying hung
        // with no window left. Its project is already saved (or was declined) by then.
        std::thread([] {
            for (int k = 0; k < 100 && !s_quitDone.load(); ++k) std::this_thread::sleep_for(std::chrono::milliseconds(100));
            if (s_quitDone.load()) return;
            std::fprintf(stderr, "Pinol: quitting took more than 10 s, the process ends now\n");
            std::_Exit(3);
        }).detach();
    }
    endurance.stop();
    engine.shutdown();
    s_quitDone = true;
    return rc;
}
