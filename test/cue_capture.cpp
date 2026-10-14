// Regression tests for scheduled cue undo: only serialize layers that will
// be touched by the recalled snapshot, without losing any undo information.
#include "Engine.h"
#include <QJsonArray>
#include <QJsonObject>
#include <cmath>
#include <cstdio>

int runCueCaptureTests(Engine &e)
{
    int failures = 0;
    auto check = [&](bool ok, const char *what) {
        if (ok) std::printf("ok cue capture: %s\n", what);
        else {
            std::printf("FAIL cue capture: %s\n", what);
            ++failures;
        }
    };

    e.newProject();
    auto add = [&](const char *name, double opacity, bool enabled, bool locked = false) {
        const int i = e.addLayer(QString::fromLatin1(name));
        Layer *l = e.layer(i);
        l->opacity = float(opacity);
        l->enabled = enabled;
        l->locked = locked;
        return e.layerId(i);
    };
    const quint64 target = add("Touched by snapshot", 0.2, true);
    const quint64 excluded = add("Explicitly excluded", 0.42, true);
    const quint64 hidden = add("Not in snapshot", 0.66, true);
    const quint64 disabled = add("Disabled and not in snapshot", 0.33, false);
    const quint64 locked = add("Locked and not in snapshot", 0.7, true, true);
    // Real-world stress case: many disabled layers must not be serialized for
    // undo just because they happen to exist in the show.
    for (int k = 0; k < 60; ++k) add("Idle", 0.1, false);
    const auto layer = [&](quint64 id) { return e.layer(e.indexOfId(id)); };

    Engine::Snapshot s;
    s.name = "Selective recall";
    s.fade = 0.0;
    QJsonObject targetState = e.layerJson(e.indexOfId(target));
    QJsonObject values = targetState.value("params").toObject();
    values["opacity"] = 0.85;
    targetState["params"] = values;
    targetState["included"] = true;
    QJsonObject excludedState = e.layerJson(e.indexOfId(excluded));
    excludedState["included"] = false;
    s.layers = QJsonArray{targetState, excludedState};
    const int snapshot = e.addSnapshot(s);
    const QJsonArray undo = e.captureRecallLayers(snapshot);

    check(e.captureLayers(true).size() > 60, "test includes many layers");
    check(undo.size() == 2, "undo serializes only the target and the unknown visible layer");
    QSet<quint64> captured;
    for (const QJsonValue &v : undo)
        captured.insert(v.toObject().value("id").toString().toULongLong());
    check(captured.contains(target) && captured.contains(hidden), "affected layer IDs captured");
    check(!captured.contains(excluded) && !captured.contains(disabled) && !captured.contains(locked),
          "excluded, disabled and locked layers omitted");

    e.recallSnapshot(snapshot);
    check(std::abs(layer(target)->opacity - 0.85f) < 0.0001, "included layer changed");
    check(!layer(hidden)->enabled, "unmentioned visible layer hidden");
    check(layer(excluded)->enabled && std::abs(layer(excluded)->opacity - 0.42f) < 0.0001,
          "explicitly excluded layer is unchanged");
    check(!layer(disabled)->enabled && layer(locked)->enabled && layer(locked)->locked,
          "untouched disabled and locked layers preserved");

    // This is exactly how RecallSnapshot::undo() applies pre-cue layers.
    e.applyLayers(undo, 0.0);
    check(std::abs(layer(target)->opacity - 0.2f) < 0.0001, "undo restores animated-capable opacity");
    check(layer(hidden)->enabled && std::abs(layer(hidden)->opacity - 0.66f) < 0.0001,
          "undo restores the layer hidden by recall");
    check(layer(excluded)->enabled && layer(locked)->locked && !layer(disabled)->enabled,
          "undo does not modify uninvolved layers");

    // Cues that affect only the master composition must not serialize any
    // layers at all (applyLayers is skipped for an empty snapshot layer set).
    Engine::Snapshot masterOnly;
    masterOnly.name = "Master-only";
    masterOnly.composition = QJsonObject{{"included", true}, {"opacity", 0.5}};
    const int masterIndex = e.addSnapshot(masterOnly);
    check(e.captureRecallLayers(masterIndex).isEmpty(), "composition-only cue captures zero layers");
    check(e.captureRecallLayers(-1).isEmpty(), "invalid cue index captures zero layers");

    e.newProject();
    return failures;
}
