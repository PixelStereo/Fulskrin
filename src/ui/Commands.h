#pragma once
// Undo commands (QUndoStack). Each command is applied to the engine under its lock.
// Values and properties are found by layer id; structural commands by index (the stack keeps the order consistent
// at the time a command is undone or redone).

#include "Engine.h"

#include <QJsonArray>
#include <QJsonObject>
#include <QUndoCommand>
#include <QVariant>
#include <functional>
#include <optional>

namespace cmd {

enum Id { ValuesId = 1, PropId = 2, NudgeId = 3 };

// A layer's ISF instance: slot -1 = generator, otherwise the effect index. Engine lock required.
IsfInstance *resolveIsf(Engine *e, int layer, int slot);

// One value (Parameter.h) of a layer (0: the composition), by its address
struct ValueChange {
    quint64 layer = 0;
    QString path;
    QJsonValue before, after;
};

// Values edited together (a color's components, a ROI, Reset to Defaults: one step), through Engine::setParameter.
// Every value the interface edits goes this way. By layer id: the list may change in between. The same values
// edited again soon after (a slider dragged, the arrows of a number) merge into the step.
class SetValues : public QUndoCommand
{
public:
    SetValues(Engine *e, std::vector<ValueChange> changes, const QString &text);
    void undo() override { apply(false); }
    void redo() override { apply(true); }
    int id() const override { return ValuesId; }
    bool mergeWith(const QUndoCommand *other) override;

private:
    void apply(bool after);
    Engine *m_e;
    std::vector<ValueChange> m_changes;
    qint64 m_time;
};

// Pushes a change of values of one layer (address → value), each read as it is now; nothing when none changes.
// False: nothing pushed
bool setValues(QUndoStack *undo, Engine *e, quint64 layer, const QJsonObject &after, const QString &text);
inline bool setValue(QUndoStack *undo, Engine *e, quint64 layer, const QString &path, const QJsonValue &after,
                     const QString &text)
{
    return setValues(undo, e, layer, QJsonObject{{path, after}}, text);
}
// The addresses of a shader input's value (one; x y; r g b a) and its value there
QJsonObject isfValueJson(const QString &prefix, const IsfInput &in, const IsfValue &v);

// What a layer is, rather than a value of it: its name, switch, lock, transition, the layer its color mask comes from
class SetLayerProp : public QUndoCommand
{
public:
    enum Prop { Name, Enabled, Locked, Transition, ColorMask };
    SetLayerProp(Engine *e, quint64 layer, Prop prop, const QVariant &before, const QVariant &after);
    void undo() override { apply(m_before); }
    void redo() override { apply(m_after); }
    int id() const override { return PropId; }
    bool mergeWith(const QUndoCommand *other) override;
    static QVariant read(Engine *e, quint64 layer, Prop prop);

private:
    void apply(const QVariant &v);
    Engine *m_e;
    quint64 m_layer;
    Prop m_prop;
    QVariant m_before, m_after;
    qint64 m_time;
};

// Full mapping (corners + mesh) before / after an edit
class SetMapping : public QUndoCommand
{
public:
    SetMapping(Engine *e, int layer, const Mapping &before, const Mapping &after, const QString &text, bool nudge = false);
    void undo() override { apply(m_before); }
    void redo() override { apply(m_after); }
    int id() const override { return m_nudge ? NudgeId : -1; }
    bool mergeWith(const QUndoCommand *other) override;
    static Mapping read(Engine *e, int layer);

private:
    void apply(const Mapping &m);
    Engine *m_e;
    int m_layer;
    Mapping m_before, m_after;
    bool m_nudge;
    qint64 m_time;
};

// A layer's masks (by its id): drawn, moved, edited
class SetMasks : public QUndoCommand
{
public:
    SetMasks(Engine *e, quint64 layer, const std::vector<Mask> &before, const std::vector<Mask> &after, const QString &text);
    void undo() override;
    void redo() override;

private:
    Engine *m_e;
    quint64 m_layer;
    std::vector<Mask> m_before, m_after;
};

// Layer added (already done at push time)
class AddLayer : public QUndoCommand
{
public:
    AddLayer(Engine *e, int index, const QString &text);
    void undo() override;
    void redo() override;

private:
    Engine *m_e;
    int m_index;
    QJsonObject m_json;
    bool m_first = true;
};

// Layer deleted
class RemoveLayer : public QUndoCommand
{
public:
    RemoveLayer(Engine *e, int index);
    void undo() override;
    void redo() override;

private:
    Engine *m_e;
    int m_index;
    QJsonObject m_json;
};

class MoveLayer : public QUndoCommand
{
public:
    MoveLayer(Engine *e, int from, int to);
    void undo() override { m_e->moveLayer(m_to, m_from); }
    void redo() override { m_e->moveLayer(m_from, m_to); }

private:
    Engine *m_e;
    int m_from, m_to;
};

// Layer replaced by another full state (source change…); already done at push time
class ReplaceLayer : public QUndoCommand
{
public:
    ReplaceLayer(Engine *e, int index, const QJsonObject &before, const QString &text);
    void undo() override { m_e->replaceLayerJson(m_index, m_before); }
    void redo() override;

private:
    Engine *m_e;
    int m_index;
    QJsonObject m_before, m_after;
    bool m_first = true;
};

// Order and grouping of the layers (moves, groups); applied by redo
class SetStructure : public QUndoCommand
{
public:
    SetStructure(Engine *e, const LayerTree &before, const LayerTree &after, const QString &text);
    void undo() override { m_e->setStructure(m_before); }
    void redo() override { m_e->setStructure(m_after); }

private:
    Engine *m_e;
    LayerTree m_before, m_after;
};

// Snapshot recalled: the layers' states before, then the snapshot's (with its fade the first time)
class RecallSnapshot : public QUndoCommand
{
public:
    RecallSnapshot(Engine *e, int snapshot);
    // Already recalled elsewhere (the cue clock), with the state it replaced: the first redo does nothing
    RecallSnapshot(Engine *e, int snapshot, const QJsonArray &before, const QJsonObject &beforeComposition,
                   const QJsonArray &beforeInteractions);
    void undo() override
    {
        m_e->applyLayers(m_before, 0);
        m_e->applyComposition(m_beforeComposition, 0);
        m_e->applyInteractions(m_beforeInteractions, 0);
    }
    void redo() override;

private:
    Engine *m_e;
    int m_snapshot;
    QJsonArray m_before;
    QJsonObject m_beforeComposition;
    QJsonArray m_beforeInteractions;
    bool m_first = true;
    bool m_done = false; // recalled before it was pushed
};

// Effect chain before / after (add, remove, reorder, enable); already done at push time
class SetEffects : public QUndoCommand
{
public:
    SetEffects(Engine *e, int index, const QJsonArray &before, const QString &text);
    void undo() override { m_e->setEffectsJson(m_index, m_before); }
    void redo() override;

private:
    Engine *m_e;
    int m_index;
    QJsonArray m_before, m_after;
    bool m_first = true;
};

// How much of an item at the top of the list a viewport shows (by ids: the list may change in between).
// Dragging the slider is one step: the same item and viewport merge.
class SetOpacityIn : public QUndoCommand
{
public:
    SetOpacityIn(Engine *e, quint64 layer, quint64 viewport, float before, float after, const QString &text);
    void undo() override { apply(m_before); }
    void redo() override { apply(m_after); }
    int id() const override { return 9107; }
    bool mergeWith(const QUndoCommand *o) override;

private:
    void apply(float v);
    Engine *m_e;
    quint64 m_layer, m_viewport;
    float m_before, m_after;
};

// A timeline (animation) edited: tracks, keys, duration, loop, repeat, speed, name. By id (the list may change).
// Edits with the same non-empty merge key that follow one another (a key dragged, the arrows of a number) are one step.
class SetAnimation : public QUndoCommand
{
public:
    SetAnimation(Engine *e, quint64 id, const Engine::Animation &before, const Engine::Animation &after, const QString &text,
                 const QString &mergeKey = {});
    void undo() override { apply(m_before); }
    void redo() override { apply(m_after); }
    int id() const override { return m_mergeKey.isEmpty() ? -1 : 9201; }
    bool mergeWith(const QUndoCommand *other) override;

private:
    void apply(const Engine::Animation &a);
    Engine *m_e;
    quint64 m_id;
    Engine::Animation m_before, m_after;
    QString m_mergeKey;
};

// A timeline added at `index` (applied by redo); its id is kept across undo / redo
class AddAnimation : public QUndoCommand
{
public:
    AddAnimation(Engine *e, const Engine::Animation &a, int index, const QString &text);
    void undo() override;
    void redo() override;
    quint64 animationId() const { return m_a.id; }

private:
    Engine *m_e;
    Engine::Animation m_a;
    int m_index;
};

// A timeline deleted (applied by redo); undo puts it back with its id: the steps that drive it work again
class RemoveAnimation : public QUndoCommand
{
public:
    RemoveAnimation(Engine *e, int index);
    void undo() override { m_e->addAnimation(m_a, m_index); }
    void redo() override;

private:
    Engine *m_e;
    Engine::Animation m_a;
    int m_index;
};

// One animation of a layer's number (the Anim tab), by the layer's id and the number: before / after, either of them
// absent for one added or removed (it goes back to its place). The others are left alone (an edit made meanwhile by
// OSC is not undone with it). Edits with the same non-empty merge key that follow one another (a key dragged, the
// arrows of a number) are one step.
class SetLayerAnim : public QUndoCommand
{
public:
    SetLayerAnim(Engine *e, quint64 layer, const QString &param, std::optional<Animation> before, std::optional<Animation> after,
                 const QString &text, const QString &mergeKey = {});
    void undo() override { apply(m_before); }
    void redo() override { apply(m_after); }
    int id() const override { return m_mergeKey.isEmpty() ? -1 : 9202; }
    bool mergeWith(const QUndoCommand *other) override;

private:
    void apply(const std::optional<Animation> &a);
    Engine *m_e;
    quint64 m_layer;
    QString m_param;
    std::optional<Animation> m_before, m_after;
    int m_index; // its place when it was there
    QString m_mergeKey;
};

// A preset recalled (with its fade); undone: its values back as they were, at once
class RecallPreset : public QUndoCommand
{
public:
    RecallPreset(Engine *e, quint64 preset);
    void undo() override;
    void redo() override;

private:
    Engine *m_e;
    quint64 m_preset;
    std::vector<Engine::PresetValue> m_before;
    bool m_first = true;
};

// The snapshots rearranged (dragged, grouped, a group renamed or removed)
class SetSnapshotTree : public QUndoCommand
{
public:
    SetSnapshotTree(Engine *e, std::vector<Engine::SnapshotNode> before, std::vector<Engine::SnapshotNode> after,
                    const QString &text)
        : m_e(e), m_before(std::move(before)), m_after(std::move(after))
    {
        setText(text);
    }
    void undo() override { m_e->setSnapshotTree(m_before); }
    void redo() override { m_e->setSnapshotTree(m_after); }

private:
    Engine *m_e;
    std::vector<Engine::SnapshotNode> m_before, m_after;
};

// The presets edited (one made, removed, a value added or taken out, a name, a fade, a time). Edits with the same
// non-empty merge key that follow one another are one step
class SetPresets : public QUndoCommand
{
public:
    SetPresets(Engine *e, std::vector<Engine::Preset> before, std::vector<Engine::Preset> after, const QString &text,
               const QString &mergeKey = QString());
    void undo() override { m_e->setPresets(m_before); }
    void redo() override { m_e->setPresets(m_after); }
    int id() const override { return m_mergeKey.isEmpty() ? -1 : 9501; }
    bool mergeWith(const QUndoCommand *other) override;

private:
    Engine *m_e;
    std::vector<Engine::Preset> m_before, m_after;
    QString m_mergeKey;
    qint64 m_time;
};
// Pushes `fn`'s change of the presets as one step (nothing when it changes nothing)
void editPresets(QUndoStack *undo, Engine *e, const QString &text, const std::function<void(std::vector<Engine::Preset> &)> &fn,
                 const QString &mergeKey = QString());

} // namespace cmd
