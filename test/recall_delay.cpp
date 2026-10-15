// Delayed preset and snapshot recall: deterministic show-clock tests, no wall-time sleep.
#include "Engine.h"
#include <QJsonObject>
#include <cmath>
#include <cstdio>

int runRecallDelayTests(Engine &e)
{
    int failures = 0;
    auto check = [&](bool ok, const char *message) {
        if (!ok) { std::printf("FAIL recall_delay: %s\n", message); ++failures; }
    };
    auto near = [](double a, double b) { return std::abs(a - b) < 0.015; };
    e.newProject();
    e.setFadesManual(true);
    const int index = e.addLayer(QStringLiteral("Delayed values"));
    const quint64 id = e.layerId(index);
    auto opacity = [&] { double v = -1; return e.animParamValue(id, QStringLiteral("opacity"), &v) ? v : -1.0; };
    check(e.setParameter(id, QStringLiteral("opacity"), 0.15), "initial opacity");

    Engine::Preset p;
    p.name = QStringLiteral("Delayed cut");
    p.fade = 0;
    Engine::PresetValue v = e.presetValue(id, QStringLiteral("opacity"));
    v.value = QJsonValue(0.9);
    v.time = 0;    // cut on execution
    v.delay = 0.25;
    p.values.push_back(v);
    const int pi = e.addPreset(p);
    e.recallPreset(pi);
    check(near(opacity(), 0.15), "preset delay does not apply immediately");
    e.advanceFades(0.24);
    check(near(opacity(), 0.15), "preset delay does not apply early");
    e.advanceFades(0.01);
    check(near(opacity(), 0.9), "preset value cuts at deadline");

    // A second recall of the same address cancels the previously scheduled change.
    p.values[0].value = 0.1;
    p.values[0].delay = 1.0;
    e.setPreset(pi, p);
    e.recallPreset(pi);
    p.values[0].value = 0.6;
    p.values[0].delay = 0;
    e.setPreset(pi, p);
    e.recallPreset(pi);
    check(near(opacity(), 0.6), "immediate preset takes precedence over delayed previous");
    e.advanceFades(1.5);
    check(near(opacity(), 0.6), "stale delayed recall never overwrites the new value");

    // Delay + fade: hold, start, interpolate, finish (time is relative to execution).
    p.values[0].value = 1.0;
    p.values[0].time = 1.0;
    p.values[0].delay = 0.5;
    e.setPreset(pi, p);
    e.recallPreset(pi);
    e.advanceFades(0.4);
    check(near(opacity(), 0.6), "delayed fade holds its initial value");
    e.advanceFades(0.1);
    check(near(opacity(), 0.6), "delayed fade begins at current value");
    e.advanceFades(0.5);
    check(opacity() > 0.65 && opacity() < 1.0, "delayed fade is in progress");
    e.advanceFades(0.5);
    check(near(opacity(), 1.0), "delayed fade completes");

    // Snapshot's layer params keep a per-address delay in the existing JSON layer entry.
    check(e.setParameter(id, QStringLiteral("opacity"), 0.3), "snapshot base value");
    Engine::Snapshot snap;
    snap.name = QStringLiteral("Delayed snapshot");
    snap.fade = 0;
    snap.layers = e.captureLayers(true);
    bool recorded = false;
    for (int k = 0; k < snap.layers.size(); ++k) {
        QJsonObject layer = snap.layers[k].toObject();
        if (layer.value("id").toString().toULongLong() != id) continue;
        QJsonObject params = layer.value("params").toObject();
        params["opacity"] = 0.7;
        layer["params"] = params;
        layer["delay"] = QJsonObject{{"opacity", 0.4}};
        layer["timing"] = QJsonObject{{"opacity", 0.0}};
        snap.layers[k] = layer;
        recorded = true;
    }
    check(recorded, "snapshot target layer exists");
    const int si = e.addSnapshot(snap);
    check(e.snapshotDuration(e.snapshot(si).id) >= 0.399, "snapshot duration accounts for delay");
    e.recallSnapshot(si);
    check(near(opacity(), 0.3), "snapshot param not recalled early");
    e.advanceFades(0.39);
    check(near(opacity(), 0.3), "snapshot still pending");
    e.advanceFades(0.02);
    check(near(opacity(), 0.7), "snapshot delayed param applied");

    // Composition values also use the same engine clock.
    Engine::Preset cp;
    cp.name = QStringLiteral("Delayed composition");
    cp.fade = 0;
    Engine::PresetValue comp;
    comp.layer = 0;
    comp.path = QStringLiteral("opacity");
    comp.value = 0.2;
    comp.time = 0;
    comp.delay = 0.25;
    cp.values.push_back(comp);
    const int ci = e.addPreset(cp);
    e.fadeCompositionOpacity(1, 0);
    e.recallPreset(ci);
    check(near(e.compositionOpacityTarget(), 1), "composition delay holds current value");
    e.advanceFades(0.3);
    check(near(e.compositionOpacityTarget(), 0.2), "composition executes after deadline");

    e.setFadesManual(false);
    e.newProject();
    return failures;
}
