// Emergency ESC transport regression. No GPU thread: test immediate cancellation
// without waiting for a real clock, and ensure future ticks never execute old cues.
#include "Engine.h"
#include "Sequencer.h"
#include <cmath>
#include <cstdio>

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

    engine.setFadesManual(false);
    engine.newProject();
    return failures;
}
