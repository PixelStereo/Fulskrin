// Emergency ESC transport regression. No GPU thread: test immediate cancellation
// without waiting for a real clock, and ensure future ticks never execute old cues.
#include "Engine.h"
#include "Sequencer.h"
#include <cmath>
#include <cstdio>
#include <QTemporaryDir>

int runEmergencyStopTests(Engine &engine)
{
    int failures = 0;
    auto check = [&](bool ok, const char *reason) {
        if (!ok) { std::printf("FAIL emergency_stop: %s\n", reason); ++failures; }
    };
    engine.newProject();
    engine.setFadesManual(true);
    const int li = engine.addLayer(QStringLiteral("Stop layer"));
    const quint64 layer = engine.layerId(li);

    Engine::AnimTrack t;
    t.layer = layer;
    t.param = QStringLiteral("opacity");
    t.keys = {{0.0, 0.15, 0}, {2.0, 0.95, 0}};
    Engine::Animation anim;
    anim.duration = 2.0;
    anim.loop = Engine::AnimLoop::Loop;
    anim.tracks = {t};
    const int ai = engine.addAnimation(anim);
    const quint64 animId = engine.animation(ai).id;
    engine.controlAnimation(animId, Engine::AnimAction::Play);
    engine.advanceFades(0.3);
    check(engine.animation(ai).state == Engine::AnimState::Playing, "timeline playing before stop");

    // A long snapshot fade is in progress when Escape is pressed.
    Engine::Snapshot snap;
    snap.name = QStringLiteral("Slow snapshot");
    snap.fade = 3.0;
    snap.layers = engine.captureLayers();
    const int si = engine.addSnapshot(snap);
    engine.layer(engine.indexOfId(layer))->opacity = 0.1f;
    engine.recallSnapshot(si);
    check(engine.isFading(), "snapshot fade exists before stop");

    Sequencer::Step a;
    a.snapshot = engine.snapshot(si).id;
    a.preWait = 4.0;
    a.next = Sequencer::StepContinue::Follow;
    Sequencer::Step b = a;
    b.preWait = 1.0;
    Sequencer::Sequence first, second;
    first.name = QStringLiteral("First");
    first.steps = {a, b};
    second.name = QStringLiteral("Second");
    second.steps = {a};
    const int i1 = engine.sequencer().add(first);
    const int i2 = engine.sequencer().add(second);
    check(engine.sequencer().goSequence(i1) && engine.sequencer().goSequence(i2), "both cue lists started");
    check(engine.sequencer().anyActive(), "pre-waits active before stop");

    engine.stopShowPlayback();
    check(!engine.sequencer().anyActive(), "all sequence waits canceled");
    check(!engine.isFading(), "snapshot transitions stopped");
    check(engine.animation(ai).state == Engine::AnimState::Stopped, "timeline transport stopped");
    check(engine.recallProgress().running() == false, "snapshot progress stopped");
    const double held = engine.layer(engine.indexOfId(layer))->opacity;
    engine.advanceFades(7.0);
    engine.sequencer().advance(7.0);
    engine.renderFrame();
    check(std::abs(double(engine.layer(engine.indexOfId(layer))->opacity) - held) < 1e-5,
          "parameters do not continue fading after stop");
    check(!engine.sequencer().anyActive(), "canceled follow never fires on future tick");

    // A source crossfade must hold its actual mixed pixels, rather than jumping to the new source.
    engine.newProject();
    engine.setCompositionSize(QSize(64, 64));
    engine.setViewportSize(engine.indexOfId(engine.mainViewportId()), QSize(64, 64));
    QTemporaryDir files;
    QImage red(64, 64, QImage::Format_RGBA8888), blue = red;
    red.fill(Qt::red); blue.fill(Qt::blue);
    const QString redPath = files.path() + "/red.png", bluePath = files.path() + "/blue.png";
    check(red.save(redPath) && blue.save(bluePath), "create transition images");
    const int imageLayer = engine.addLayer("Crossfade");
    QString error;
    check(engine.setLayerImage(imageLayer, bluePath, &error), "load destination image");
    Engine::Snapshot mixed;
    mixed.name = "Blue"; mixed.fade = 2;
    mixed.layers = engine.captureLayers();
    const int mi = engine.addSnapshot(mixed);
    check(engine.setLayerImage(imageLayer, redPath, &error), "load outgoing image");
    engine.renderFrame();
    engine.recallSnapshot(mi);
    engine.advanceFades(0.5);
    engine.renderFrame();
    const QColor before = engine.grabOutput().pixelColor(32, 32);
    check(before.red() > 10 && before.blue() > 10, "mixed frame exists before stop");
    const quint64 imageId = engine.layerId(imageLayer);
    engine.stopShowPlayback();
    check(!engine.isTransitioning(imageId), "frozen source transition is no longer a running transport");
    engine.advanceFades(10);
    engine.renderFrame();
    const QColor after = engine.grabOutput().pixelColor(32, 32);
    check(before == after, "Escape preserves exact source-transition pixels on subsequent frames");

    // Typewriter output is held as shown, including after another manual fade tick.
    const int textLayer = engine.addLayer("Typing");
    check(engine.setLayerText(textLayer), "create text layer");
    auto &text = engine.layer(textLayer)->text;
    text.typedFrom = "A"; text.content = "ABCDE"; text.typeDur = 10; text.typeProgress = 0.5;
    const QString shown = text.shown();
    engine.stopShowPlayback();
    engine.advanceFades(20);
    check(text.shown() == shown && text.typeDur == 0, "typewriter does not finish after Escape");

    engine.setFadesManual(false);
    engine.newProject();
    return failures;
}
