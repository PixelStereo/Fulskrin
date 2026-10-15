// Regression: repeated parameter timelines, cue-chain deadlines and rendered pixels.
// Standalone CTest area: pinol_tests --area timeline_repeat.
// Manual frame stepping is deterministic, not proof of physical GPU display cadence.
#include "Engine.h"
#include "Sequencer.h"

#include <QColor>
#include <QImage>
#include <QString>
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace {
constexpr double kPi = 3.14159265358979323846;

int finiteRepeat(Engine &e, const QString &tmp, int hz)
{
    int failures = 0;
    auto check = [&](bool ok, const char *what) {
        if (!ok) {
            std::printf("FAIL timeline_repeat %dHz: %s\n", hz, what);
            ++failures;
        }
    };
    e.newProject();
    e.setFadesManual(true);
    e.setCompositionSize(QSize(64, 64));
    e.setViewportSize(e.indexOfId(e.mainViewportId()), QSize(64, 64));
    e.fadeCompositionOpacity(1, 0);

    QImage white(64, 64, QImage::Format_RGBA8888);
    white.fill(Qt::white);
    const QString filename = tmp + QStringLiteral("/timeline-repeat-white.png");
    check(white.save(filename), "write white image fixture");
    const int index = e.addLayer(QStringLiteral("Repeated white image"));
    QString error;
    if (!e.setLayerImage(index, filename, &error)) {
        check(false, "open white image");
        e.setFadesManual(false);
        return failures;
    }
    const quint64 layer = e.layerId(index);

    Engine::AnimTrack opacity;
    opacity.layer = layer;
    opacity.param = QStringLiteral("opacity");
    opacity.oscillator = true;
    opacity.wave = Engine::AnimWave::Sine;
    opacity.period = 1.0;
    opacity.center = 0.5;
    opacity.amplitude = 0.3;

    Engine::AnimTrack rotation;
    rotation.layer = layer;
    rotation.param = QStringLiteral("rotation");
    rotation.keys = {{0.0, 0.0, 0}, {0.5, 30.0, 0}, {1.0, 0.0, 0}};

    Engine::Animation repeated;
    repeated.name = QStringLiteral("3 passes");
    repeated.duration = 1.0;
    repeated.loop = Engine::AnimLoop::Loop;
    repeated.repeat = 3;
    repeated.tracks = {opacity, rotation};
    const int repeatedIndex = e.addAnimation(repeated);
    const quint64 repeatedId = e.animation(repeatedIndex).id;

    Engine::Animation marker;
    marker.name = QStringLiteral("Step B");
    marker.duration = 0.5;
    marker.loop = Engine::AnimLoop::Once;
    const int markerIndex = e.addAnimation(marker);
    const quint64 markerId = e.animation(markerIndex).id;

    Sequencer::Step first, second, third;
    first.timeline = repeatedId;
    first.next = Sequencer::StepContinue::AutoFollow;
    first.postWait = 0.25;
    second.timeline = markerId;
    second.action = Engine::AnimAction::Play;
    second.next = Sequencer::StepContinue::Follow;
    second.postWait = 0.10;
    third.timeline = markerId;
    third.action = Engine::AnimAction::Stop;
    Sequencer::Sequence seq;
    seq.name = QStringLiteral("Repeat 3 -> AutoFollow -> Follow -> Stop");
    seq.steps = {first, second, third};
    const int seqIndex = e.sequencer().add(seq);
    e.sequencer().setCurrent(seqIndex);
    check(e.sequencer().go(), "first GO accepted");
    check(e.animation(repeatedIndex).state == Engine::AnimState::Playing, "repeat timeline started");

    const double dt = 1.0 / hz;
    const int frames = int(std::round(3.55 * hz));
    double largestValueError = 0;
    int largestPixelError = 0;
    int pixelSamples = 0;
    double secondAt = -1, thirdAt = -1;
    int priorPosition = 0;
    for (int frame = 1; frame <= frames; ++frame) {
        // Same simulation time is applied to parameter updates and cue scheduler.
        e.advanceFades(dt);
        e.sequencer().advance(dt);
        e.renderFrame();
        const double t = frame * dt;
        const int step = e.sequencer().positionOf(seqIndex);
        if (priorPosition == 0 && step >= 1) secondAt = t;
        if (priorPosition <= 1 && step >= 2) thirdAt = t;
        priorPosition = step;

        // The oscillator is periodic in played time, not reset at each Repeat.
        const double phase = std::min(t, 3.0);
        const double wantOpacity = 0.5 + 0.3 * std::sin(2 * kPi * phase);
        double actual = -1;
        if (e.animParamValue(layer, QStringLiteral("opacity"), &actual))
            largestValueError = std::max(largestValueError, std::abs(actual - wantOpacity));
        else check(false, "layer opacity readable");

        if (frame % 4 != 0 && frame != hz && frame != 2 * hz && frame != 3 * hz) continue;
        double spin = 0;
        if (e.animParamValue(layer, QStringLiteral("rotation"), &spin)) {
            const double p = phase == 3.0 ? 1.0 : phase - std::floor(phase);
            const double wantRotation = p <= 0.5 ? p * 60 : (1 - p) * 60;
            check(std::abs(spin - wantRotation) <= 0.15, "rotation keys repeat without a phase error");
        } else check(false, "layer rotation readable");

        const QImage pixels = e.grabViewport(e.mainViewportId());
        check(!pixels.isNull(), "rendered viewport is available");
        if (!pixels.isNull()) {
            const QColor center = pixels.pixelColor(pixels.width() / 2, pixels.height() / 2);
            const int expected = int(std::round(wantOpacity * 255.0));
            largestPixelError = std::max(largestPixelError, std::abs(center.red() - expected));
            check(std::abs(center.red() - center.green()) <= 3 &&
                  std::abs(center.red() - center.blue()) <= 3, "animated viewport remains neutral white");
            ++pixelSamples;
        }
    }
    std::printf("Repeat %dHz: %d GPU samples; max opacity error %.5f; max pixel error %d/255; "
                "AutoFollow at %.4fs; Follow at %.4fs\n",
                hz, pixelSamples, largestValueError, largestPixelError, secondAt, thirdAt);
    check(largestValueError < 0.015, "oscillator remains phase-continuous at Repeat boundaries");
    check(largestPixelError <= 12, "rendered pixels track animated opacity");
    check(secondAt >= 3.25 - dt && secondAt <= 3.25 + 2 * dt,
          "AutoFollow fires after Repeat x3 and 250 ms");
    check(thirdAt >= 3.35 - dt && thirdAt <= 3.35 + 2 * dt,
          "Follow fires 100 ms after step two");
    check(e.sequencer().positionOf(seqIndex) == 2, "third step reached");
    check(e.animation(markerIndex).state == Engine::AnimState::Stopped, "third step stopped marker");

    e.sequencer().stopAll();
    e.setFadesManual(false);
    return failures;
}

int infiniteRepeatAndSelection(Engine &e)
{
    int failures = 0;
    e.newProject();
    e.setFadesManual(true);
    const int layerIndex = e.addLayer(QStringLiteral("Endless loop"));
    const quint64 layer = e.layerId(layerIndex);
    Engine::AnimTrack osc;
    osc.layer = layer;
    osc.param = QStringLiteral("opacity");
    osc.oscillator = true;
    osc.wave = Engine::AnimWave::Sine;
    osc.period = 1.0;
    osc.center = 0.5;
    osc.amplitude = 0.3;
    Engine::Animation animation;
    animation.duration = 1.0;
    animation.loop = Engine::AnimLoop::Loop;
    animation.repeat = 0; // infinite
    animation.tracks = {osc};
    const int ai = e.addAnimation(animation);
    const quint64 id = e.animation(ai).id;
    Sequencer::Step step;
    step.timeline = id;
    Sequencer::Sequence first;
    first.name = QStringLiteral("Playing sequence");
    first.steps = {step};
    const int firstIndex = e.sequencer().add(first);
    e.sequencer().setCurrent(firstIndex);
    if (!e.sequencer().go()) ++failures;
    Sequencer::Sequence second;
    second.name = QStringLiteral("Selected but not playing");
    const int secondIndex = e.sequencer().add(second);
    e.sequencer().setCurrent(secondIndex);
    // A selection change must not stop a parameter animation already in motion.
    for (int f = 1; f <= 600; ++f) {
        e.advanceFades(1.0 / 120.0);
        e.sequencer().advance(1.0 / 120.0);
        if (f % 120 == 60) {
            double value = 0;
            if (!e.animParamValue(layer, QStringLiteral("opacity"), &value) ||
                std::abs(value - 0.5) > 0.012) {
                std::printf("FAIL timeline_repeat: endless phase / selection at frame %d\n", f);
                ++failures;
            }
        }
    }
    if (e.animation(ai).state != Engine::AnimState::Playing ||
        e.sequencer().positionOf(firstIndex) != 0 ||
        e.sequencer().current() != secondIndex) ++failures;
    e.sequencer().stopAll();
    e.setFadesManual(false);
    return failures;
}

int pingPongBoundaries()
{
    int failures = 0;
    Engine::Animation a;
    a.duration = 1.0;
    a.loop = Engine::AnimLoop::PingPong;
    a.repeat = 4;
    for (int k = 1; k <= 3; ++k) {
        const double h = 1.0 / 120.0;
        if (std::abs(a.position(k - h) - a.position(k + h)) > 2 * h + 1e-6) {
            std::printf("FAIL timeline_repeat: PingPong turn at %d sec\n", k);
            ++failures;
        }
    }
    return failures;
}
} // namespace

int runTimelineRepeatTests(Engine &e, const QString &tmp)
{
    int errors = 0;
    errors += finiteRepeat(e, tmp, 60);
    errors += finiteRepeat(e, tmp, 120);
    errors += infiniteRepeatAndSelection(e);
    errors += pingPongBoundaries();
    e.newProject();
    std::printf("timeline_repeat: %s (%d failure(s))\n", errors ? "FAILED" : "OK", errors);
    return errors;
}
