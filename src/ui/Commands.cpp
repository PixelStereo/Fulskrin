#include "Commands.h"
#include "Osc.h"

#include <QColor>
#include <QDateTime>
#include <QUndoStack>
#include <algorithm>

namespace cmd {

static qint64 nowMs() { return QDateTime::currentMSecsSinceEpoch(); }
static constexpr qint64 kMergeWindowMs = 1500; // continuous gestures merged into a single step

IsfInstance *resolveIsf(Engine *e, int layer, int slot)
{
    Layer *l = e->layer(layer);
    if (!l) return nullptr;
    if (slot < 0) return l->generator.get();
    return slot < int(l->effects.size()) ? l->effects[size_t(slot)].get() : nullptr;
}

// --- SetValues --------------------------------------------------------------

SetValues::SetValues(Engine *e, std::vector<ValueChange> changes, const QString &text)
    : m_e(e), m_changes(std::move(changes)), m_time(nowMs())
{
    setText(text);
}

void SetValues::apply(bool after)
{
    // Twice when several: values that hold each other in (a ROI's sides, in and out) all reach theirs
    const int passes = m_changes.size() > 1 ? 2 : 1;
    for (int k = 0; k < passes; ++k)
        for (const ValueChange &c : m_changes) m_e->setParameter(c.layer, c.path, after ? c.after : c.before);
}

bool SetValues::mergeWith(const QUndoCommand *other)
{
    auto *o = static_cast<const SetValues *>(other);
    if (o->m_changes.size() != m_changes.size() || o->m_time - m_time > kMergeWindowMs) return false;
    for (size_t k = 0; k < m_changes.size(); ++k)
        if (o->m_changes[k].layer != m_changes[k].layer || o->m_changes[k].path != m_changes[k].path) return false;
    for (size_t k = 0; k < m_changes.size(); ++k) m_changes[k].after = o->m_changes[k].after;
    m_time = o->m_time;
    return true;
}

bool setValues(QUndoStack *undo, Engine *e, quint64 layer, const QJsonObject &after, const QString &text)
{
    std::vector<ValueChange> changes;
    for (auto it = after.begin(); it != after.end(); ++it) {
        const QJsonValue before = e->parameterValue(layer, it.key());
        if (before.isUndefined() || before == it.value()) continue;
        changes.push_back({layer, it.key(), before, it.value()});
    }
    if (changes.empty()) return false;
    undo->push(new SetValues(e, std::move(changes), text));
    return true;
}

QJsonObject isfValueJson(const QString &prefix, const IsfInput &in, const IsfValue &v)
{
    const QString path = prefix + QStringLiteral("param/") + osc::safeName(in.name);
    switch (in.type) {
    case IsfInput::Float: return {{path, v.f}};
    case IsfInput::Long: return {{path, v.l}};
    case IsfInput::Bool: return {{path, v.b}};
    case IsfInput::Point2D: return {{path + "/x", v.p.x()}, {path + "/y", v.p.y()}};
    case IsfInput::Color: {
        QJsonObject o;
        for (int c = 0; c < 4; ++c) o.insert(path + "/" + QChar("rgba"[c]), double(v.c[c]));
        return o;
    }
    default: return {};
    }
}

// --- SetLayerProp -----------------------------------------------------------

static QString propText(SetLayerProp::Prop p)
{
    switch (p) {
    case SetLayerProp::Name: return QStringLiteral("Rename Layer");
    case SetLayerProp::Enabled: return QStringLiteral("Change Enable");
    case SetLayerProp::Locked: return QStringLiteral("Lock / Unlock Layer");
    case SetLayerProp::Transition: return QStringLiteral("Change Transition");
    case SetLayerProp::ColorMask: return QStringLiteral("Change Color Mask");
    }
    return {};
}

SetLayerProp::SetLayerProp(Engine *e, quint64 layer, Prop prop, const QVariant &before, const QVariant &after)
    : m_e(e), m_layer(layer), m_prop(prop), m_before(before), m_after(after), m_time(nowMs())
{
    setText(propText(prop));
}

QVariant SetLayerProp::read(Engine *e, quint64 layer, Prop prop)
{
    Engine::Lock lk(&e->mutex());
    const Layer *l = e->layer(e->indexOfId(layer));
    if (!l) return {};
    switch (prop) {
    case Name: return l->name;
    case Enabled: return l->enabled;
    case Locked: return l->locked;
    case Transition: return l->transition;
    case ColorMask: return QVariant(qulonglong(l->color.maskLayer));
    }
    return {};
}

void SetLayerProp::apply(const QVariant &v)
{
    if (m_prop == ColorMask) { // checked against feedback by the engine
        bool invert;
        int index;
        {
            Engine::Lock lk(&m_e->mutex());
            index = m_e->indexOfId(m_layer);
            const Layer *l = m_e->layer(index);
            if (!l) return;
            invert = l->color.maskInvert;
        }
        m_e->setColorMask(index, v.toULongLong(), invert);
        return;
    }
    Engine::Lock lk(&m_e->mutex());
    Layer *l = m_e->layer(m_e->indexOfId(m_layer));
    if (!l) return;
    switch (m_prop) {
    case Name: l->name = v.toString(); break;
    case Enabled: l->enabled = v.toBool(); break;
    case Locked: l->locked = v.toBool(); break;
    case Transition: l->transition = v.toString(); break;
    case ColorMask: break;
    }
}

bool SetLayerProp::mergeWith(const QUndoCommand *other)
{
    auto *o = static_cast<const SetLayerProp *>(other);
    if (o->m_layer != m_layer || o->m_prop != m_prop || m_prop != Name) return false;
    if (o->m_time - m_time > kMergeWindowMs) return false;
    m_after = o->m_after;
    m_time = o->m_time;
    return true;
}

// --- SetMapping -------------------------------------------------------------

SetMapping::SetMapping(Engine *e, int layer, const Mapping &before, const Mapping &after, const QString &text, bool nudge)
    : m_e(e), m_layer(layer), m_before(before), m_after(after), m_nudge(nudge), m_time(nowMs())
{
    setText(text);
}

Mapping SetMapping::read(Engine *e, int layer)
{
    Engine::Lock lk(&e->mutex());
    Layer *l = e->layer(layer);
    return l ? l->mapping : Mapping();
}

void SetMapping::apply(const Mapping &m)
{
    Engine::Lock lk(&m_e->mutex());
    Layer *l = m_e->layer(m_layer);
    if (!l) return;
    const unsigned rev = l->mapping.revision;
    const double aspect = l->mapping.aspect; // the composition's now
    l->mapping = m;
    l->mapping.aspect = aspect;
    l->mapping.revision = rev + 1;
}

bool SetMapping::mergeWith(const QUndoCommand *other)
{
    auto *o = static_cast<const SetMapping *>(other);
    if (!m_nudge || !o->m_nudge || o->m_layer != m_layer) return false;
    if (o->m_time - m_time > kMergeWindowMs) return false;
    m_after = o->m_after;
    m_time = o->m_time;
    return true;
}

SetMasks::SetMasks(Engine *e, quint64 layer, const std::vector<Mask> &before, const std::vector<Mask> &after, const QString &text)
    : m_e(e), m_layer(layer), m_before(before), m_after(after)
{
    setText(text);
}
void SetMasks::undo() { m_e->setLayerMasks(m_layer, m_before); }
void SetMasks::redo() { m_e->setLayerMasks(m_layer, m_after); }

// --- Layers -----------------------------------------------------------------

AddLayer::AddLayer(Engine *e, int index, const QString &text) : m_e(e), m_index(index)
{
    m_json = e->layerJson(index);
    setText(text);
}

void AddLayer::undo() { m_e->removeLayer(m_index); }

void AddLayer::redo()
{
    if (m_first) {
        m_first = false;
        return;
    }
    m_e->insertLayerJson(m_index, m_json);
}

RemoveLayer::RemoveLayer(Engine *e, int index) : m_e(e), m_index(index)
{
    m_json = e->layerJson(index);
    setText(QStringLiteral("Delete \"%1\"").arg(m_json.value("name").toString()));
}

void RemoveLayer::undo() { m_e->insertLayerJson(m_index, m_json); }
void RemoveLayer::redo() { m_e->removeLayer(m_index); }

MoveLayer::MoveLayer(Engine *e, int from, int to) : m_e(e), m_from(from), m_to(to)
{
    setText(QStringLiteral("Reorder Layers"));
}

ReplaceLayer::ReplaceLayer(Engine *e, int index, const QJsonObject &before, const QString &text)
    : m_e(e), m_index(index), m_before(before)
{
    m_after = e->layerJson(index);
    setText(text);
}

void ReplaceLayer::redo()
{
    if (m_first) {
        m_first = false;
        return;
    }
    m_e->replaceLayerJson(m_index, m_after);
}

SetStructure::SetStructure(Engine *e, const LayerTree &before, const LayerTree &after, const QString &text)
    : m_e(e), m_before(before), m_after(after)
{
    setText(text);
}

RecallSnapshot::RecallSnapshot(Engine *e, int snapshot) : m_e(e), m_snapshot(snapshot)
{
    m_before = e->captureLayers(true);
    m_beforeComposition = e->captureComposition();
    m_beforeInteractions = e->captureInteractions();
    setText(QStringLiteral("Recall \"%1\"").arg(e->snapshot(snapshot).name));
}

void RecallSnapshot::redo()
{
    if (m_first) {
        m_first = false;
        m_e->recallSnapshot(m_snapshot);
    } else {
        const Engine::Snapshot m = m_e->snapshot(m_snapshot);
        if (!m.layers.isEmpty()) m_e->applyLayers(m.layers, 0, true);
        m_e->applyComposition(m.composition, 0);
        m_e->applyInteractions(m.interactions, 0, m.interactionsKept);
    }
}

SetEffects::SetEffects(Engine *e, int index, const QJsonArray &before, const QString &text)
    : m_e(e), m_index(index), m_before(before)
{
    m_after = e->effectsJson(index);
    setText(text);
}

void SetEffects::redo()
{
    if (m_first) {
        m_first = false;
        return;
    }
    m_e->setEffectsJson(m_index, m_after);
}

SetOpacityIn::SetOpacityIn(Engine *e, quint64 layer, quint64 viewport, float before, float after, const QString &text)
    : m_e(e), m_layer(layer), m_viewport(viewport), m_before(before), m_after(after)
{
    setText(text);
}

bool SetOpacityIn::mergeWith(const QUndoCommand *o)
{
    auto *c = dynamic_cast<const SetOpacityIn *>(o);
    if (!c || c->m_layer != m_layer || c->m_viewport != m_viewport) return false;
    m_after = c->m_after;
    return true;
}

void SetOpacityIn::apply(float v) { m_e->setOpacityIn(m_e->indexOfId(m_layer), m_viewport, v); }

SetAnimation::SetAnimation(Engine *e, quint64 id, const Engine::Animation &before, const Engine::Animation &after,
                           const QString &text, const QString &mergeKey)
    : m_e(e), m_id(id), m_before(before), m_after(after), m_mergeKey(mergeKey)
{
    setText(text);
}

bool SetAnimation::mergeWith(const QUndoCommand *other)
{
    const auto *o = static_cast<const SetAnimation *>(other); // same id(): a SetAnimation
    if (o->m_id != m_id || o->m_mergeKey != m_mergeKey) return false;
    m_after = o->m_after;
    return true;
}

void SetAnimation::apply(const Engine::Animation &a)
{
    const int i = m_e->indexOfAnimation(m_id);
    if (i >= 0) m_e->setAnimation(i, a);
}

AddAnimation::AddAnimation(Engine *e, const Engine::Animation &a, int index, const QString &text)
    : m_e(e), m_a(a), m_index(index)
{
    setText(text);
}

void AddAnimation::redo()
{
    const int i = m_e->addAnimation(m_a, m_index); // the first time: gets its id
    m_a.id = m_e->animation(i).id;
}

void AddAnimation::undo()
{
    const int i = m_e->indexOfAnimation(m_a.id);
    if (i >= 0) {
        m_a = m_e->animation(i); // as it is now, should it be redone
        m_e->removeAnimation(i);
    }
}

RemoveAnimation::RemoveAnimation(Engine *e, int index) : m_e(e), m_a(e->animation(index)), m_index(index)
{
    setText(QStringLiteral("Delete Timeline"));
}

void RemoveAnimation::redo()
{
    const int i = m_e->indexOfAnimation(m_a.id);
    if (i >= 0) {
        m_index = i;
        m_e->removeAnimation(i);
    }
}

SetLayerAnim::SetLayerAnim(Engine *e, quint64 layer, const QString &param, std::optional<Animation> before,
                           std::optional<Animation> after, const QString &text, const QString &mergeKey)
    : m_e(e), m_layer(layer), m_param(param), m_before(std::move(before)), m_after(std::move(after)),
      m_index(e->layerAnimIndex(layer, param)), m_mergeKey(mergeKey)
{
    setText(text);
}

bool SetLayerAnim::mergeWith(const QUndoCommand *other)
{
    const auto *o = static_cast<const SetLayerAnim *>(other); // same id(): a SetLayerAnim
    if (o->m_layer != m_layer || o->m_param != m_param || o->m_mergeKey != m_mergeKey) return false;
    m_after = o->m_after;
    return true;
}

void SetLayerAnim::apply(const std::optional<Animation> &a)
{
    m_e->setLayerAnim(m_layer, m_param, a ? &*a : nullptr, m_index);
}

// --- Presets -----------------------------------------------------------------

RecallPreset::RecallPreset(Engine *e, quint64 preset) : m_e(e), m_preset(preset)
{
    const Engine::Preset p = e->preset(e->indexOfPreset(preset));
    for (const Engine::PresetValue &v : p.values) m_before.push_back(e->presetValue(v.layer, v.path));
    setText(QStringLiteral("Recall Preset \"%1\"").arg(p.name));
}

void RecallPreset::undo()
{
    for (const Engine::PresetValue &v : m_before)
        if (!v.value.isUndefined()) m_e->setParameter(v.layer, v.path, v.value);
}

void RecallPreset::redo()
{
    const int i = m_e->indexOfPreset(m_preset);
    if (i < 0) return;
    if (m_first) {
        m_first = false;
        m_e->recallPreset(i);
        return;
    }
    for (const Engine::PresetValue &v : m_e->preset(i).values) m_e->setParameter(v.layer, v.path, v.value);
}

static bool samePresets(const std::vector<Engine::Preset> &a, const std::vector<Engine::Preset> &b)
{
    if (a.size() != b.size()) return false;
    for (size_t k = 0; k < a.size(); ++k) {
        const Engine::Preset &x = a[k], &y = b[k];
        if (x.id != y.id || x.name != y.name || x.fade != y.fade || x.values.size() != y.values.size()) return false;
        for (size_t v = 0; v < x.values.size(); ++v) {
            const Engine::PresetValue &p = x.values[v], &q = y.values[v];
            if (p.layer != q.layer || p.path != q.path || p.value != q.value || p.time != q.time || p.easing != q.easing)
                return false;
        }
    }
    return true;
}

SetPresets::SetPresets(Engine *e, std::vector<Engine::Preset> before, std::vector<Engine::Preset> after, const QString &text,
                       const QString &mergeKey)
    : m_e(e), m_before(std::move(before)), m_after(std::move(after)), m_mergeKey(mergeKey), m_time(nowMs())
{
    setText(text);
}

bool SetPresets::mergeWith(const QUndoCommand *other)
{
    auto *o = static_cast<const SetPresets *>(other);
    if (o->m_mergeKey != m_mergeKey || o->m_time - m_time > kMergeWindowMs) return false;
    m_after = o->m_after;
    m_time = o->m_time;
    return true;
}

void editPresets(QUndoStack *undo, Engine *e, const QString &text, const std::function<void(std::vector<Engine::Preset> &)> &fn,
                 const QString &mergeKey)
{
    const std::vector<Engine::Preset> before = e->presets();
    std::vector<Engine::Preset> after = before;
    fn(after);
    if (samePresets(before, after)) return;
    undo->push(new SetPresets(e, before, after, text, mergeKey));
}

} // namespace cmd
