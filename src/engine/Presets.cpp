// How the snapshots are arranged (groups, order), and the presets: a few values recalled with a fade.
#include "EngineInternal.h"

#include <QSet>
#include <QJsonArray>
#include <functional>

// ---------------------------------------------------------------------------
// The snapshots' tree

bool Engine::SnapshotNode::operator==(const SnapshotNode &o) const
{
    return snapshot == o.snapshot && group == o.group && name == o.name && open == o.open && children == o.children;
}

std::vector<Engine::SnapshotNode> Engine::snapshotTree() const
{
    Lock lk(&m_mutex);
    return m_snapshotTree;
}

quint64 Engine::newSnapshotGroupId()
{
    Lock lk(&m_mutex);
    return m_nextSnapshotGroupId++;
}

void Engine::setSnapshotTree(const std::vector<SnapshotNode> &tree)
{
    {
        Lock lk(&m_mutex);
        m_snapshotTree = tree;
        normalizeSnapshotTree();
    }
    emit snapshotsChanged();
}

void Engine::normalizeSnapshotTree()
{
    QSet<quint64> known, seen, groups;
    for (const Snapshot &m : m_snapshots) known.insert(m.id);
    std::vector<quint64> order;
    std::function<void(std::vector<SnapshotNode> &)> clean = [&](std::vector<SnapshotNode> &nodes) {
        for (auto it = nodes.begin(); it != nodes.end();) {
            if (it->snapshot) {
                if (!known.contains(it->snapshot) || seen.contains(it->snapshot)) {
                    it = nodes.erase(it);
                    continue;
                }
                seen.insert(it->snapshot);
                order.push_back(it->snapshot);
                it->children.clear();
                it->group = 0;
            } else {
                if (!it->group || groups.contains(it->group)) it->group = m_nextSnapshotGroupId++;
                groups.insert(it->group);
                m_nextSnapshotGroupId = std::max(m_nextSnapshotGroupId, it->group + 1);
                clean(it->children);
            }
            ++it;
        }
    };
    clean(m_snapshotTree);
    for (const Snapshot &m : m_snapshots) // not in it yet: at the end
        if (!seen.contains(m.id)) {
            SnapshotNode n;
            n.snapshot = m.id;
            m_snapshotTree.push_back(n);
            order.push_back(m.id);
        }
    std::vector<Snapshot> sorted;
    sorted.reserve(m_snapshots.size());
    for (quint64 id : order)
        for (const Snapshot &m : m_snapshots)
            if (m.id == id) sorted.push_back(m);
    m_snapshots.swap(sorted);
}

// ---------------------------------------------------------------------------
// Presets

int Engine::Preset::indexOf(quint64 layer, const QString &path) const
{
    for (size_t k = 0; k < values.size(); ++k)
        if (values[k].layer == layer && values[k].path == path) return int(k);
    return -1;
}

int Engine::presetCount() const
{
    Lock lk(&m_mutex);
    return int(m_presets.size());
}

std::vector<Engine::Preset> Engine::presets() const
{
    Lock lk(&m_mutex);
    return m_presets;
}

void Engine::setPresets(const std::vector<Preset> &all)
{
    {
        Lock lk(&m_mutex);
        m_presets = all;
        for (const Preset &p : m_presets) m_nextPresetId = std::max(m_nextPresetId, p.id + 1);
    }
    emit presetsChanged();
}

Engine::Preset Engine::preset(int i) const
{
    Lock lk(&m_mutex);
    return i >= 0 && i < int(m_presets.size()) ? m_presets[size_t(i)] : Preset();
}

int Engine::indexOfPreset(quint64 id) const
{
    Lock lk(&m_mutex);
    for (size_t i = 0; i < m_presets.size(); ++i)
        if (m_presets[i].id == id) return int(i);
    return -1;
}

void Engine::setPreset(int i, const Preset &p)
{
    {
        Lock lk(&m_mutex);
        if (i < 0 || i >= int(m_presets.size())) return;
        const quint64 id = m_presets[size_t(i)].id;
        m_presets[size_t(i)] = p;
        m_presets[size_t(i)].id = id;
    }
    emit presetsChanged();
}

int Engine::addPreset(const Preset &p, int at)
{
    {
        Lock lk(&m_mutex);
        if (at < 0 || at > int(m_presets.size())) at = int(m_presets.size());
        Preset copy = p;
        bool taken = !copy.id;
        for (const Preset &o : m_presets) taken = taken || o.id == copy.id;
        if (taken) copy.id = m_nextPresetId++;
        m_nextPresetId = std::max(m_nextPresetId, copy.id + 1);
        m_presets.insert(m_presets.begin() + at, copy);
    }
    emit presetsChanged();
    return at;
}

void Engine::removePreset(int i)
{
    {
        Lock lk(&m_mutex);
        if (i < 0 || i >= int(m_presets.size())) return;
        m_presets.erase(m_presets.begin() + i);
    }
    emit presetsChanged();
}

Engine::PresetValue Engine::presetValue(quint64 layer, const QString &path) const
{
    PresetValue v;
    v.layer = layer;
    v.path = path;
    v.value = parameterValue(layer, path);
    return v;
}

// --- In the project

QJsonObject presetToJson(const Engine::Preset &p)
{
    QJsonArray values;
    for (const Engine::PresetValue &v : p.values) {
        QJsonObject o{{"layer", QString::number(v.layer)}, {"path", v.path}, {"value", v.value}};
        if (v.time >= 0) o["time"] = v.time;
        if (!v.easing.isEmpty()) o["easing"] = v.easing;
        values.append(o);
    }
    QJsonArray animations, interactions;
    for (const Animation &a : p.animations) {
        if (a.tracks.empty()) continue;
        QJsonObject o = layerAnimToJson(a);
        o["layer"] = QString::number(a.tracks.front().layer);
        animations.append(o);
    }
    for (const Interaction &it : p.interactions) interactions.append(it.toJson());
    return QJsonObject{{"id", QString::number(p.id)}, {"name", p.name}, {"fade", p.fade},
                       {"values", values}, {"animations", animations}, {"interactions", interactions}};
}

Engine::Preset presetFromJson(const QJsonObject &o)
{
    Engine::Preset p;
    p.id = o.value("id").toString().toULongLong();
    p.name = o.value("name").toString();
    p.fade = std::clamp(o.value("fade").toDouble(1.0), 0.0, 600.0);
    for (const QJsonValue &x : o.value("values").toArray()) {
        const QJsonObject vo = x.toObject();
        Engine::PresetValue v;
        v.layer = vo.value("layer").toString().toULongLong();
        v.path = vo.value("path").toString();
        v.value = vo.value("value");
        v.time = vo.contains("time") ? std::clamp(vo.value("time").toDouble(), 0.0, 600.0) : -1.0;
        v.easing = vo.value("easing").toString();
        if (!v.path.isEmpty()) p.values.push_back(v);
    }
    for (const QJsonValue &v : o.value("animations").toArray()) {
        const QJsonObject item = v.toObject();
        const quint64 layer = item.value("layer").toString().toULongLong();
        if (layer && !item.value("param").toString().isEmpty()) p.animations.push_back(layerAnimFromJson(item, layer));
    }
    for (const QJsonValue &v : o.value("interactions").toArray()) {
        const Interaction it = Interaction::fromJson(v.toObject());
        if (!it.param.isEmpty()) p.interactions.push_back(it);
    }
    return p;
}

QJsonArray snapshotTreeToJson(const std::vector<Engine::SnapshotNode> &nodes)
{
    QJsonArray a;
    for (const Engine::SnapshotNode &n : nodes) {
        if (n.snapshot) a.append(QString::number(n.snapshot));
        else
            a.append(QJsonObject{{"group", n.name}, {"id", QString::number(n.group)}, {"open", n.open},
                                 {"children", snapshotTreeToJson(n.children)}});
    }
    return a;
}

std::vector<Engine::SnapshotNode> snapshotTreeFromJson(const QJsonArray &a)
{
    std::vector<Engine::SnapshotNode> out;
    for (const QJsonValue &v : a) {
        Engine::SnapshotNode n;
        if (v.isString()) {
            n.snapshot = v.toString().toULongLong();
            if (!n.snapshot) continue;
        } else {
            const QJsonObject o = v.toObject();
            n.name = o.value("group").toString();
            n.group = o.value("id").toString().toULongLong();
            n.open = o.value("open").toBool(true);
            n.children = snapshotTreeFromJson(o.value("children").toArray());
        }
        out.push_back(n);
    }
    return out;
}
