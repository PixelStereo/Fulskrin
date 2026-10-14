// Engine: the layers — creation, order and groups, sources (video, image, sound, ISF generator, another
// layer), transport, effect chain, and the copy of parameters from one layer to another.
#include "EngineInternal.h"
#include "Midi.h"

#include <QFontDatabase>
#include <QDir>
#include <QFileInfo>
#include <QImage>
#include <QColorSpace>
#include <QJsonArray>
#include <cmath>

QString blendModeName(BlendMode m)
{
    switch (m) {
    case BlendMode::Add: return QStringLiteral("Add");
    case BlendMode::Screen: return QStringLiteral("Screen");
    case BlendMode::Multiply: return QStringLiteral("Multiply");
    case BlendMode::Subtract: return QStringLiteral("Subtract");
    case BlendMode::Difference: return QStringLiteral("Difference");
    default: return QStringLiteral("Normal");
    }
}

QString blendModeKey(BlendMode m)
{
    switch (m) {
    case BlendMode::Add: return "add";
    case BlendMode::Screen: return "screen";
    case BlendMode::Multiply: return "multiply";
    case BlendMode::Subtract: return "subtract";
    case BlendMode::Difference: return "difference";
    default: return "normal";
    }
}

QString playModeName(PlayMode m)
{
    switch (m) {
    case PlayMode::OneShot: return QStringLiteral("One-shot");
    case PlayMode::PingPong: return QStringLiteral("Ping-pong");
    case PlayMode::Stop: return QStringLiteral("Stop");
    default: return QStringLiteral("Loop");
    }
}

QString playModeKey(PlayMode m)
{
    switch (m) {
    case PlayMode::OneShot: return "oneshot";
    case PlayMode::PingPong: return "pingpong";
    case PlayMode::Stop: return "stop";
    default: return "loop";
    }
}

PlayMode playModeFromKey(const QString &k, PlayMode fallback)
{
    if (k == "oneshot") return PlayMode::OneShot;
    if (k == "loop") return PlayMode::Loop;
    if (k == "pingpong") return PlayMode::PingPong;
    if (k == "stop") return PlayMode::Stop;
    return fallback;
}

QString layerTapKey(LayerTap t) { return t == LayerTap::PreFx ? QStringLiteral("prefx") : QStringLiteral("postfx"); }

LayerTap layerTapFromKey(const QString &k) { return k == "prefx" ? LayerTap::PreFx : LayerTap::PostFx; }

BlendMode blendModeFromKey(const QString &k)
{
    if (k == "add") return BlendMode::Add;
    if (k == "screen") return BlendMode::Screen;
    if (k == "multiply") return BlendMode::Multiply;
    if (k == "subtract") return BlendMode::Subtract;
    if (k == "difference") return BlendMode::Difference;
    return BlendMode::Normal;
}

void Engine::attachAudio(Layer &l, std::shared_ptr<AudioStream> s)
{
    releaseAudio(*m_audio, l.audio);
    l.audio = std::move(s);
    if (!l.audio) return;
    l.audio->setTransport(l.clock, l.playing, std::abs(l.speed) * timeScale(), l.timeline(), l.timelineId, l.audioGain());
    m_audio->addStream(l.audio);
}

int Engine::layerCount() const
{
    Lock lk(&m_mutex);
    return int(m_layers.size());
}

Layer *Engine::layer(int i)
{
    Lock lk(&m_mutex);
    return (i >= 0 && i < int(m_layers.size())) ? m_layers[size_t(i)].get() : nullptr;
}

int Engine::indexOf(const Layer *l) const
{
    Lock lk(&m_mutex);
    for (size_t i = 0; i < m_layers.size(); ++i)
        if (m_layers[i].get() == l) return int(i);
    return -1;
}

int Engine::addLayer(const QString &name, int at)
{
    {
        Lock lk(&m_mutex);
        auto l = std::make_unique<Layer>();
        l->compSize = &m_compSize;
        l->mapping.aspect = double(m_compSize.width()) / std::max(1, m_compSize.height());
        l->id = newIdLocked();
        l->colorModels = m_defaultColorModels;
        l->name = name.isEmpty() ? QStringLiteral("Layer %1").arg(m_layers.size() + 1) : name;
        l->genWidth = m_compSize.width();
        l->genHeight = m_compSize.height();
        at = std::clamp(at, viewportCountLocked(), int(m_layers.size()));
        // Inserted inside a group's block: the new layer joins that group
        if (at < int(m_layers.size())) l->parent = m_layers[size_t(at)]->parent;
        m_layers.insert(m_layers.begin() + at, std::move(l));
    }
    emit layersChanged();
    return at;
}

// ---------------------------------------------------------------------------
// Viewports
// ---------------------------------------------------------------------------

int Engine::viewportCountLocked() const
{
    int n = 0;
    for (const auto &l : m_layers) n += l->isViewport;
    return n;
}

// A new viewport is placed to the right of the last one, at its size in the composition's pixels
int Engine::addViewport(const QString &name, QSize size)
{
    quint64 id;
    {
        Lock lk(&m_mutex);
        auto l = std::make_unique<Layer>();
        l->compSize = &m_compSize;
        l->mapping.aspect = double(m_compSize.width()) / std::max(1, m_compSize.height());
        l->id = id = newIdLocked();
        l->isViewport = true;
        // By default, the size of the last viewport (the same projectors, side by side)
        QSize px = m_compSize;
        for (const auto &o : m_layers)
            if (o->isViewport) px = o->viewportSize();
        if (size.isValid() && !size.isEmpty()) px = size;
        l->vpWidth = px.width();
        l->vpHeight = px.height();
        const int n = viewportCountLocked();
        l->name = name.isEmpty() ? QStringLiteral("Viewport %1").arg(n + 1) : name;
        l->vpPublish = PublishSettings();
        for (PublishTarget &t : l->vpPublish.targets) t.name = QStringLiteral("Pinol %1").arg(l->name);
        double x = 0;
        for (const auto &o : m_layers)
            if (o->isViewport) x = std::max(x, o->mapping.bounds().right());
        const double w = double(px.width()) / std::max(1, m_compSize.width());
        const double h = double(px.height()) / std::max(1, m_compSize.height());
        if (x + w > 1.0 + 1e-9) x = 0; // no room left on the right: over the first one
        l->mapping.size = QSizeF(w, h);
        l->mapping.position = QPointF(x + w / 2, h / 2);
        m_layers.push_back(std::move(l));
        normalizeLocked();
    }
    emit layersChanged();
    return indexOfId(id);
}

bool Engine::isViewport(int i) const
{
    Lock lk(&m_mutex);
    return i >= 0 && i < int(m_layers.size()) && m_layers[size_t(i)]->isViewport;
}

QList<int> Engine::viewports() const
{
    Lock lk(&m_mutex);
    QList<int> out;
    for (int i = 0; i < int(m_layers.size()); ++i)
        if (m_layers[size_t(i)]->isViewport) out << i;
    return out;
}

quint64 Engine::mainViewportId() const
{
    Lock lk(&m_mutex);
    for (const auto &l : m_layers)
        if (l->isViewport) return l->id;
    return 0;
}

void Engine::setViewportSize(int i, QSize size)
{
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l || !l->isViewport) return;
        l->vpWidth = std::clamp(size.width(), 1, 16384);
        l->vpHeight = std::clamp(size.height(), 1, 16384);
    }
    emit layersChanged();
}

void Engine::setViewportOutput(int i, const QString &screen, int mode)
{
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l || !l->isViewport) return;
        l->vpScreen = screen;
        l->vpMode = std::clamp(mode, 0, 2);
    }
    emit layersChanged();
}

// Only an item at the top of the list is routed; inside a group, the group decides
void Engine::setOpacityIn(int i, quint64 viewport, float opacity)
{
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l || l->isViewport || l->parent) return;
        opacity = std::clamp(opacity, 0.0f, 1.0f);
        if (l->opacityIn(viewport) == opacity) return;
        if (opacity >= 1.0f) l->viewportOpacity.erase(viewport);
        else l->viewportOpacity[viewport] = opacity;
    }
    emit layersChanged();
}

// A composition always has a viewport: the one the screen shows
void Engine::ensureViewport()
{
    bool has = false;
    {
        Lock lk(&m_mutex);
        has = viewportCountLocked() > 0;
    }
    if (!has) addViewport();
}

int Engine::addGroup(const QString &name, int at)
{
    {
        Lock lk(&m_mutex);
        int groups = 0;
        for (const auto &l : m_layers) groups += l->isGroup;
        auto l = std::make_unique<Layer>();
        l->compSize = &m_compSize;
        l->mapping.aspect = double(m_compSize.width()) / std::max(1, m_compSize.height());
        l->id = newIdLocked();
        l->isGroup = true;
        l->name = name.isEmpty() ? QStringLiteral("Group %1").arg(groups + 1) : name;
        at = std::clamp(at, viewportCountLocked(), int(m_layers.size()));
        // Inside a group's block, the new group goes into that group
        if (at < int(m_layers.size())) l->parent = m_layers[size_t(at)]->parent;
        m_layers.insert(m_layers.begin() + at, std::move(l));
    }
    emit layersChanged();
    return at;
}

quint64 Engine::newIdLocked()
{
    for (const auto &l : m_layers) m_nextId = std::max(m_nextId, l->id + 1);
    return m_nextId++;
}

quint64 Engine::layerId(int i) const
{
    Lock lk(&m_mutex);
    return (i >= 0 && i < int(m_layers.size())) ? m_layers[size_t(i)]->id : 0;
}

int Engine::indexOfId(quint64 id) const
{
    Lock lk(&m_mutex);
    if (!id) return -1;
    for (size_t i = 0; i < m_layers.size(); ++i)
        if (m_layers[i]->id == id) return int(i);
    return -1;
}

// Node of a layer in the structure
static TreeNode nodeOf(const Layer &l)
{
    TreeNode n;
    n.id = l.id;
    n.parent = l.parent;
    n.kind = l.isViewport ? TreeNode::Viewport : l.isGroup ? TreeNode::Group : TreeNode::Item;
    return n;
}

LayerTree Engine::structure() const
{
    Lock lk(&m_mutex);
    LayerTree t;
    t.reserve(m_layers.size());
    for (const auto &l : m_layers) t.push_back(nodeOf(*l));
    return t;
}

void Engine::normalizeLocked()
{
    LayerTree t;
    for (const auto &l : m_layers) t.push_back(nodeOf(*l));
    const LayerTree n = tree::normalized(t);
    if (n == t) return;
    std::vector<std::unique_ptr<Layer>> out;
    out.reserve(m_layers.size());
    for (const TreeNode &node : n) {
        for (auto &l : m_layers) {
            if (l && l->id == node.id) {
                l->parent = node.parent;
                out.push_back(std::move(l));
                break;
            }
        }
    }
    m_layers.swap(out);
}

void Engine::setStructure(const LayerTree &t)
{
    {
        Lock lk(&m_mutex);
        std::vector<std::unique_ptr<Layer>> out;
        out.reserve(m_layers.size());
        for (const TreeNode &node : t) {
            for (auto &l : m_layers) {
                if (l && l->id == node.id) {
                    l->parent = node.parent;
                    out.push_back(std::move(l));
                    break;
                }
            }
        }
        for (auto &l : m_layers) // layers missing from the structure keep their place at the end
            if (l) out.push_back(std::move(l));
        m_layers.swap(out);
        normalizeLocked();
    }
    emit layersChanged();
}

int Engine::groupIndexOf(int i) const
{
    Lock lk(&m_mutex);
    if (i < 0 || i >= int(m_layers.size()) || !m_layers[size_t(i)]->parent) return -1;
    return indexOfId(m_layers[size_t(i)]->parent);
}

QList<int> Engine::groupMembers(int g) const
{
    Lock lk(&m_mutex);
    QList<int> out;
    if (g < 0 || g >= int(m_layers.size()) || !m_layers[size_t(g)]->isGroup) return out;
    const quint64 id = m_layers[size_t(g)]->id;
    for (int i = 0; i < int(m_layers.size()); ++i)
        if (m_layers[size_t(i)]->parent == id) out << i;
    return out;
}

std::vector<Mask> Engine::layerMasks(quint64 id) const
{
    Lock lk(&m_mutex);
    const int i = indexOfId(id);
    return i >= 0 ? m_layers[size_t(i)]->masks : std::vector<Mask>{};
}

void Engine::setLayerMasks(quint64 id, const std::vector<Mask> &m)
{
    Lock lk(&m_mutex);
    const int i = indexOfId(id);
    if (i < 0) return;
    takeOverFades(id, {QStringLiteral("masks")}); // set here: a snapshot's fade no longer moves them
    Layer &l = *m_layers[size_t(i)];
    l.masks = m;
    ++l.masksRevision;
}

QSize Engine::maskFrame(quint64 id) const
{
    Lock lk(&m_mutex);
    const int i = indexOfId(id);
    if (i < 0) return m_compSize;
    const Layer &l = *m_layers[size_t(i)];
    if (l.isViewport) return l.viewportSize();
    if (l.isGroup) return m_compSize;
    if (l.finalW > 0 && l.finalH > 0) return QSize(l.finalW, l.finalH); // its picture, after its ROI
    if (l.sourceWidth() > 0 && l.sourceHeight() > 0) return QSize(l.sourceWidth(), l.sourceHeight());
    return m_compSize;
}

bool Engine::isLocked(int i) const
{
    Lock lk(&m_mutex);
    if (i < 0 || i >= int(m_layers.size())) return false;
    const Layer &l = *m_layers[size_t(i)];
    if (l.locked) return true;
    for (int g = groupIndexOf(i); g >= 0; g = groupIndexOf(g))
        if (m_layers[size_t(g)]->locked) return true;
    return false;
}

void Engine::releaseLayer(Layer &l)
{
    if (l.video) l.video->close();
    l.video.reset();
    l.frame = VideoFrame{};
    if (l.videoTex) l.videoTex->destroy();
    l.videoTex.reset();
    releaseAudio(*m_audio, l.audio);
    l.sourceTex.destroy();
    l.textTex.destroy();
    if (l.generator) l.generator->releaseGl();
    l.generator.reset();
    l.generatorTarget.destroy();
    for (auto &fx : l.effects) fx->releaseGl();
    l.effects.clear();
    l.fxTarget[0].destroy();
    l.fxTarget[1].destroy();
    l.maskTarget[0].destroy();
    l.maskTarget[1].destroy();
    l.groupTarget.destroy();
    l.prepTarget.destroy();
    l.shapeMaskSurface.destroy();
    l.shapeMaskComp.destroy();
    l.shapeMaskTarget.destroy();
    l.shapeMaskSurfaceRev = l.shapeMaskCompRev = 0;
    for (RenderTarget &out : l.vpOut) out.destroy();
    if (l.meshVbo) gl()->glDeleteBuffers(1, &l.meshVbo);
    l.meshVbo = 0;
    l.finalTex = l.rawTex = 0;
}

void Engine::removeLayer(int i)
{
    auto g = std::make_shared<Garbage>();
    {
        Lock lk(&m_mutex);
        if (i < 0 || i >= int(m_layers.size())) return;
        if (m_layers[size_t(i)]->isViewport && viewportCountLocked() <= 1) return; // there is always one
        g->layer = std::move(m_layers[size_t(i)]);
        m_layers.erase(m_layers.begin() + i);
        // A source transition running on it ends with it (the id may come back, undo: not the transition)
        if (auto t = m_transitions.find(g->layer->id); t != m_transitions.end()) {
            retireTransition(std::move(t->second));
            m_transitions.erase(t);
        }
        if (g->layer->isGroup) // its contents go up one level, where they are
            for (auto &l : m_layers)
                if (l->parent == g->layer->id) l->parent = g->layer->parent;
        normalizeLocked();
    }
    if (g->layer->video) g->layer->video->close(); // stop the decode thread outside the render thread
    releaseAudio(*m_audio, g->layer->audio);
    runGl([this, g] { releaseLayer(*g->layer); }, false);
    emit layersChanged();
}

void Engine::moveLayer(int from, int to)
{
    {
        Lock lk(&m_mutex);
        const int n = int(m_layers.size());
        if (from < 0 || from >= n) return;
        to = std::clamp(to, 0, n - 1);
        if (from == to) return;
        auto l = std::move(m_layers[size_t(from)]);
        m_layers.erase(m_layers.begin() + from);
        m_layers.insert(m_layers.begin() + to, std::move(l));
        normalizeLocked();
    }
    emit layersChanged();
}

QJsonObject Engine::layerJson(int i) const
{
    Lock lk(&m_mutex);
    if (i < 0 || i >= int(m_layers.size())) return {};
    return layerToJson(*m_layers[size_t(i)], QString());
}

int Engine::insertLayerJson(int at, const QJsonObject &o)
{
    const int idx = addLayer(QString(), at);
    layerFromJson(idx, o, QString(), nullptr);
    quint64 id;
    {
        Lock lk(&m_mutex);
        id = m_layers[size_t(idx)]->id;
        // A viewport (undo of its deletion) goes back to its place among the viewports
        if (m_layers[size_t(idx)]->isViewport && at >= 0 && at < idx) {
            auto l = std::move(m_layers[size_t(idx)]);
            m_layers.erase(m_layers.begin() + idx);
            m_layers.insert(m_layers.begin() + at, std::move(l));
        }
        normalizeLocked();
    }
    emit layersChanged();
    return indexOfId(id);
}

void Engine::replaceLayerJson(int i, const QJsonObject &o)
{
    if (i < 0 || i >= layerCount()) return;
    const QList<int> members = groupMembers(i);
    std::vector<quint64> memberIds;
    for (int m : members) memberIds.push_back(layerId(m));
    removeLayer(i);
    const int ni = insertLayerJson(i, o);
    if (memberIds.empty()) return;
    {
        Lock lk(&m_mutex);
        const quint64 gid = layerId(ni);
        for (auto &l : m_layers)
            if (std::find(memberIds.begin(), memberIds.end(), l->id) != memberIds.end()) l->parent = gid;
        normalizeLocked();
    }
    emit layersChanged();
}

// The part of a layer a parameter belongs to (what a paste of some parts carries), by its address
int Engine::partOf(const QString &path)
{
    auto under = [&path](const char *base) { return path.startsWith(QLatin1String(base)); };
    if (under("roi/")) return PartRoi;
    if (path == "color/enable" || path == "temp" || path == "tint" || under("temp/") || under("tint/") || under("add/") ||
        under("remove/") || path == "mask/invert")
        return PartColor;
    if (path == "rotation" || path == "width" || path == "height" || under("pivot/") || under("position/") ||
        under("scale/") || under("corner/"))
        return PartSpatial;
    if (under("fx/")) return PartEffects;
    if (path == "opacity" || path == "blend_mode") return PartCompositing;
    if (path == "speed" || path == "play_mode" || path == "in" || path == "out" || path == "play" || path == "restart" ||
        path == "volume" || path == "mute")
        return PartTransport;
    return PartSource; // the generator's, the text's, the timecode's
}

bool Engine::applyLayerParts(int i, const QJsonObject &o, int parts)
{
    if (i < 0 || i >= layerCount() || o.isEmpty() || !parts || isLocked(i)) return false;
    bool group = false;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l) return false;
        group = l->isGroup || l->isViewport;
    }
    if (group) parts &= ~PartSource; // a group has no source of its own: it composites its layers
    // The values of those parts, the others as the target has them
    const QJsonObject from = o.value("params").toObject();
    auto merge = [&](QJsonObject params) {
        for (auto it = params.begin(); it != params.end();)
            it = (partOf(it.key()) & parts) ? params.erase(it) : it + 1;
        for (auto it = from.constBegin(); it != from.constEnd(); ++it)
            if (partOf(it.key()) & parts) params.insert(it.key(), it.value());
        return params;
    };

    // With the source, the layer is rebuilt from a merged state (the media is opened again, as when one is
    // loaded); the target keeps its identity and whatever the paste does not carry.
    if (parts & PartSource) {
        QJsonObject merged = layerJson(i);
        merged["source"] = o.value("source");
        if ((parts & PartTransport) && o.contains("play")) merged["play"] = o.value("play");
        if (parts & PartColor) merged["mask"] = o.value("mask");
        if (parts & PartSpatial) merged["mesh"] = o.value("mesh");
        if (parts & PartEffects) merged["fx"] = o.value("fx");
        if (parts & PartMasks) merged["masks"] = o.value("masks");
        if (parts & PartViewports) merged["viewports"] = o.value("viewports");
        if (parts & PartAnimations) merged["anims"] = o.value("anims");
        merged["params"] = merge(merged.value("params").toObject());
        replaceLayerJson(i, merged);
        fixLayerReferences(); // a pasted layer source must not make the picture feed back
        return true;
    }

    // Everything else is set in place: the media goes on playing.
    if (parts & PartEffects) setEffectsJson(i, o.value("fx").toArray());
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l) return false;
        if (parts & PartColor) l->color.maskLayer = o.value("mask").toString().toULongLong();
        if ((parts & PartSpatial) && o.contains("mesh") && !l->isViewport) l->mapping.setMeshJson(o.value("mesh").toObject());
        if (parts & PartMasks) {
            l->masks = masks::fromJson(o.value("masks").toArray());
            ++l->masksRevision; // drawn again
        }
        if ((parts & PartViewports) && !l->isViewport) {
            l->viewportOpacity.clear();
            const QJsonObject vo = o.value("viewports").toObject();
            for (auto v = vo.begin(); v != vo.end(); ++v)
                if (const quint64 vid = v.key().toULongLong(); vid && indexOfId(vid) >= 0)
                    l->viewportOpacity[vid] = float(std::clamp(v.value().toDouble(1.0), 0.0, 1.0));
        }
        if (parts & PartAnimations) recallLayerAnims(*l, o.value("anims").toArray());
        QJsonObject values;
        for (auto it = from.constBegin(); it != from.constEnd(); ++it)
            if (partOf(it.key()) & parts) values.insert(it.key(), it.value());
        QJsonObject unknown;
        parametersFromJson(l->parameters(), values, &unknown);
    }
    fixLayerReferences();
    emit layersChanged();
    return true;
}

int Engine::duplicateLayer(int i)
{
    QJsonObject o = layerJson(i);
    if (o.isEmpty()) return -1;
    const quint64 original = layerId(i);
    QList<QJsonObject> inside; // everything the group holds, in order
    for (quint64 id : tree::descendants(structure(), original)) inside << layerJson(indexOfId(id));
    o["name"] = o.value("name").toString() + QStringLiteral(" copy");
    o.remove("id"); // the copies get their own ids
    const int gi = insertLayerJson(i, o);
    std::map<QString, quint64> newId{{QString::number(original), layerId(gi)}};
    int at = gi + 1;
    for (QJsonObject m : inside) {
        const QString oldId = m.value("id").toString();
        m.remove("id");
        m["parent"] = QString::number(newId[m.value("parent").toString()]);
        const int k = insertLayerJson(at, m);
        newId[oldId] = layerId(k);
        at = k + 1;
    }
    return indexOfId(newId[QString::number(original)]);
}

QJsonArray Engine::effectsJson(int i) const
{
    Lock lk(&m_mutex);
    QJsonArray a;
    if (i < 0 || i >= int(m_layers.size())) return a;
    for (const auto &e : m_layers[size_t(i)]->effects) a.append(e->save(QString()));
    return a;
}

void Engine::setEffectsJson(int i, const QJsonArray &a)
{
    auto g = std::make_shared<Garbage>();
    {
        Lock lk(&m_mutex);
        if (i < 0 || i >= int(m_layers.size())) return;
        g->effects = std::move(m_layers[size_t(i)]->effects);
        m_layers[size_t(i)]->effects.clear();
    }
    runGl([g] { for (auto &e : g->effects) e->releaseGl(); }, false);
    for (const QJsonValue &v : a) {
        const QJsonObject e = v.toObject();
        // An undo, a snapshot: the chain as it was, a broken effect included (its path and values are not lost)
        const int fi = addEffect(i, resolvePath(e, QString()), nullptr, IsfFailure::KeepWithError);
        if (fi < 0) continue;
        IsfInstance *inst;
        {
            Lock lk(&m_mutex);
            inst = m_layers[size_t(i)]->effects[size_t(fi)].get();
        }
        runGl([inst, e] { inst->restore(e, QString()); });
    }
}

std::shared_ptr<Engine::Garbage> Engine::detachSource(Layer &l)
{
    auto g = std::make_shared<Garbage>();
    l.extraParams = QJsonObject(); // another source: what its missing file kept is not for this one
    g->video = std::move(l.video);
    g->videoTex = std::move(l.videoTex);
    l.frame = VideoFrame{}; // its upload buffer belongs to the texture that goes
    l.frameUploaded = true;
    g->audio = std::move(l.audio);
    g->tex = l.sourceTex;
    l.sourceTex = Texture2D{};
    g->textTex = l.textTex;
    l.textTex = Texture2D{};
    l.textKey.clear();
    l.text.stopTyping();
    g->generator = std::move(l.generator);
    g->input = std::move(l.input);
    g->timecode = std::move(l.tcClock);
    l.timecode.reset();
    g->generatorTarget = l.generatorTarget;
    l.generatorTarget = RenderTarget{};
    l.pendingImage = QImage();
    l.type = SourceType::None;
    l.sourcePath.clear();
    l.error.clear();
    l.srcWidth = l.srcHeight = 0;
    l.missingType = SourceType::None;
    l.sourceLayer = 0;
    l.finalTex = 0;
    l.preFxTex = 0;
    return g;
}

// Releases a detached source: decode thread stopped here, GL resources in the render thread.
void Engine::releaseGarbage(const std::shared_ptr<Garbage> &g)
{
    if (g->video) g->video->close();
    releaseAudio(*m_audio, g->audio);
    runGl([g] {
        g->tex.destroy();
        g->textTex.destroy();
        if (g->videoTex) g->videoTex->destroy();
        if (g->generator) g->generator->releaseGl();
        g->generatorTarget.destroy();
    }, false);
}

void Engine::clearLayerSource(int i)
{
    std::shared_ptr<Garbage> g;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l) return;
        g = detachSource(*l);
    }
    releaseGarbage(g);
}

// True when the picture of layer `id` depends on layer `onId`: directly (it is used as its source),
// through a chain of such references, or because `id` is a group and one of its members depends on it.
// The lock must be held.
bool Engine::layerDependsOn(quint64 id, quint64 onId) const
{
    if (!id || !onId) return false;
    std::vector<quint64> seen;
    std::function<bool(quint64)> visit = [&](quint64 cur) -> bool {
        if (cur == onId) return true;
        if (std::find(seen.begin(), seen.end(), cur) != seen.end()) return false;
        seen.push_back(cur);
        for (const auto &l : m_layers) {
            if (l->id != cur) continue;
            if (l->type == SourceType::Layer && l->sourceLayer && visit(l->sourceLayer)) return true;
            for (const auto &fx : l->effects) // the masks of its effects
                if (fx->maskLayer && visit(fx->maskLayer)) return true;
            if (l->color.maskLayer && visit(l->color.maskLayer)) return true; // the mask of its color
            if (l->isGroup)
                for (const auto &m : m_layers)
                    if (m->parent == cur && visit(m->id)) return true;
            break;
        }
        return false;
    };
    return visit(id);
}

// After a project is read (or a layer re-created): drops the references to a layer that is not there, and any
// that would make a picture feed back on itself — a project edited by hand must not be able to do that.
void Engine::fixLayerReferences(QStringList *warnings)
{
    Lock lk(&m_mutex);
    for (auto &l : m_layers) {
        if (l->type != SourceType::Layer) continue;
        const bool exists = indexOfId(l->sourceLayer) >= 0;
        if (exists && l->sourceLayer != l->id && !layerDependsOn(l->sourceLayer, l->id)) continue;
        if (warnings)
            *warnings << l->name + (exists ? QStringLiteral(": the layer it used feeds back on it, source dropped.")
                                           : QStringLiteral(": the layer it used is gone, source dropped."));
        l->type = SourceType::None;
        l->sourceLayer = 0;
    }
    // Masks of the effects: a layer that is gone, a viewport, or one that would feed back
    for (auto &l : m_layers)
        for (auto &fx : l->effects) {
            if (!fx->maskLayer) continue;
            const int mi = indexOfId(fx->maskLayer);
            const bool bad = mi < 0 || m_layers[size_t(mi)]->isViewport || fx->maskLayer == l->id;
            if (!bad && !layerDependsOn(fx->maskLayer, l->id)) continue;
            if (warnings)
                *warnings << l->name + " / " + fx->name() +
                                 (mi < 0 ? QStringLiteral(": its mask layer is gone, mask dropped.")
                                         : QStringLiteral(": its mask would feed back on the layer, mask dropped."));
            fx->maskLayer = 0;
        }
    // Mask of the color section: the same rules
    for (auto &l : m_layers) {
        if (!l->color.maskLayer) continue;
        const int mi = indexOfId(l->color.maskLayer);
        const bool bad = mi < 0 || m_layers[size_t(mi)]->isViewport || l->color.maskLayer == l->id;
        if (!bad && !layerDependsOn(l->color.maskLayer, l->id)) continue;
        if (warnings)
            *warnings << l->name + (mi < 0 ? QStringLiteral(": the mask of its color is gone, mask dropped.")
                                           : QStringLiteral(": the mask of its color would feed back on it, mask dropped."));
        l->color.maskLayer = 0;
    }
}

bool Engine::setColorMask(int layerIndex, quint64 maskId, bool invert, QString *err)
{
    auto fail = [err](const QString &m) {
        if (err) *err = m;
        return false;
    };
    {
        Lock lk(&m_mutex);
        Layer *l = layer(layerIndex);
        if (!l) return false;
        if (maskId) {
            const int mi = indexOfId(maskId);
            if (mi < 0) return fail(QStringLiteral("That layer no longer exists."));
            if (m_layers[size_t(mi)]->isViewport) return fail(QStringLiteral("A viewport cannot be a mask."));
            if (maskId == l->id) return fail(QStringLiteral("A layer cannot mask its own color."));
            if (layerDependsOn(maskId, l->id))
                return fail(QStringLiteral("\"%1\" already uses this layer: the picture would feed back on itself.")
                                .arg(m_layers[size_t(mi)]->name));
        }
        l->color.maskLayer = maskId;
        l->color.maskInvert = invert;
    }
    emit layersChanged();
    return true;
}

bool Engine::setEffectMaskTap(int layerIndex, int effect, bool preFx)
{
    {
        Lock lk(&m_mutex);
        Layer *l = layer(layerIndex);
        if (!l || effect < 0 || effect >= int(l->effects.size())) return false;
        l->effects[size_t(effect)]->maskPreFx = preFx;
    }
    emit layersChanged();
    return true;
}

bool Engine::setEffectMask(int layerIndex, int effect, quint64 maskId, bool invert, QString *err)
{
    auto fail = [err](const QString &m) {
        if (err) *err = m;
        return false;
    };
    {
        Lock lk(&m_mutex);
        Layer *l = layer(layerIndex);
        if (!l || effect < 0 || effect >= int(l->effects.size())) return false;
        if (maskId) {
            const int mi = indexOfId(maskId);
            if (mi < 0) return fail(QStringLiteral("That layer no longer exists."));
            if (m_layers[size_t(mi)]->isViewport) return fail(QStringLiteral("A viewport cannot be a mask."));
            if (maskId == l->id) return fail(QStringLiteral("A layer cannot mask its own FX."));
            if (layerDependsOn(maskId, l->id))
                return fail(QStringLiteral("\"%1\" already uses this layer: the picture would feed back on itself.")
                                .arg(m_layers[size_t(mi)]->name));
        }
        IsfInstance &fx = *l->effects[size_t(effect)];
        fx.maskLayer = maskId;
        fx.maskInvert = invert;
    }
    emit layersChanged();
    return true;
}

bool Engine::setLayerSourceLayer(int i, quint64 sourceId, LayerTap tap, QString *err)
{
    auto fail = [err](const QString &m) {
        if (err) *err = m;
        return false;
    };
    std::shared_ptr<Garbage> g;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l) return false;
        if (l->isGroup) return fail(QStringLiteral("A group has no source: its picture is the composite of its layers."));
        if (l->isViewport) return fail(QStringLiteral("A viewport has no source: it shows the composition."));
        const int si = indexOfId(sourceId);
        if (si < 0) return fail(QStringLiteral("That layer no longer exists."));
        if (m_layers[size_t(si)]->isViewport) return fail(QStringLiteral("A viewport cannot be used as a source."));
        if (sourceId == l->id) return fail(QStringLiteral("A layer cannot be its own source."));
        if (layerDependsOn(sourceId, l->id))
            return fail(QStringLiteral("\"%1\" already uses this layer: the picture would feed back on itself.")
                            .arg(m_layers[size_t(si)]->name));
        g = detachSource(*l);
        l->type = SourceType::Layer;
        l->sourceLayer = sourceId;
        l->sourceTap = tap;
    }
    releaseGarbage(g);
    emit layersChanged();
    return true;
}

// Tap of a layer already using another layer as its source
bool Engine::setLayerTap(int i, LayerTap tap)
{
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l || l->type != SourceType::Layer) return false;
        if (l->sourceTap == tap) return true;
        l->sourceTap = tap;
    }
    emit layersChanged();
    return true;
}

static bool isDefaultMapping(const Mapping &m) { return m.isIdentity(); }

bool Engine::setLayerVideo(int i, const QString &path, QString *err)
{
    if (!layer(i)) return false;
    auto dec = std::make_unique<VideoDecoder>();
    QString e;
    if (!dec->open(path, &e)) { // opened outside the lock: may take a while
        if (err) *err = e;
        return false;
    }
    auto sound = std::make_shared<AudioStream>(); // the file's sound, if it has any
    if (!sound->open(path, AudioOutput::kSampleRate, nullptr)) sound.reset();
    std::shared_ptr<Garbage> g;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l) return false;
        g = detachSource(*l);
        l->mode = m_defaultPlayMode; // a newly loaded video takes the default mode (preferences)
        l->ended = false;
        l->srcWidth = dec->width();
        l->srcHeight = dec->height();
        l->video = std::move(dec);
        l->type = SourceType::Video;
        l->sourcePath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        l->playing = true;
        l->inPoint = 0; // a new media is played whole
        l->outPoint = -1;
        attachAudio(*l, std::move(sound));
        startMedia(*l);
        if (isDefaultMapping(l->mapping) && l->srcHeight > 0)
            l->mapping.fitAspect(double(l->srcWidth) / l->srcHeight, double(m_compSize.width()) / m_compSize.height());
    }
    releaseGarbage(g);
    return true;
}

bool Engine::setLayerFile(int i, const QString &path, QString *err)
{
    if (isImageFile(path)) return setLayerImage(i, path, err);
    if (isAudioFile(path)) return setLayerAudio(i, path, err);
    QString e;
    if (setLayerVideo(i, path, &e)) return true; // video and unknown extensions: FFmpeg decides
    if (AudioStream::probe(path, nullptr) && setLayerAudio(i, path, err)) return true; // sound only
    if (err) *err = e;
    return false;
}

bool Engine::setLayerAudio(int i, const QString &path, QString *err)
{
    if (!layer(i)) return false;
    auto sound = std::make_shared<AudioStream>();
    QString e;
    if (!sound->open(path, AudioOutput::kSampleRate, &e)) {
        if (err) *err = e;
        return false;
    }
    std::shared_ptr<Garbage> g;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l) return false;
        g = detachSource(*l);
        l->type = SourceType::Audio;
        l->sourcePath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        l->mode = m_defaultPlayMode;
        l->ended = false;
        l->playing = true;
        l->inPoint = 0; // a new media is played whole
        l->outPoint = -1;
        attachAudio(*l, std::move(sound));
        startMedia(*l);
    }
    releaseGarbage(g);
    return true;
}

bool Engine::setLayerImage(int i, const QString &path, QString *err)
{
    if (!layer(i)) return false;
    QImage img(path);
    if (img.isNull()) {
        if (err) *err = QStringLiteral("Unreadable image: ") + path;
        return false;
    }
    const QColorSpace profile = img.colorSpace();
    const QString colorInfo = QStringLiteral("%1 source · %2").arg(
        imageSourceDepth(img.format()),
        profile.isValid() ? QStringLiteral("ICC: ") +
            (profile.description().isEmpty() ? QStringLiteral("embedded profile") : profile.description())
                          : QStringLiteral("color profile: untagged/unknown"));
    img = flippedVertically(img.convertToFormat(QImage::Format_RGBA8888));
    std::shared_ptr<Garbage> g;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l) return false;
        g = detachSource(*l);
        l->pendingImage = img; // uploaded to the GPU by the render thread
        l->imageColorInfo = colorInfo;
        l->srcWidth = img.width();
        l->srcHeight = img.height();
        l->type = SourceType::Image;
        l->sourcePath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        if (isDefaultMapping(l->mapping))
            l->mapping.fitAspect(double(img.width()) / img.height(), double(m_compSize.width()) / m_compSize.height());
    }
    releaseGarbage(g);
    return true;
}

bool Engine::setLayerInput(int i, const QString &uri, QString *err)
{
    if (!layer(i)) return false;
    QString e;
    std::unique_ptr<LiveInput> input = createLiveInput(uri, QString(), &e);
    if (!input) {
        if (err) *err = e;
        return false;
    }
    std::shared_ptr<Garbage> g;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l || l->isGroup || l->isViewport) return false;
        g = detachSource(*l);
        l->input = std::move(input);
        l->type = SourceType::Input;
        l->sourcePath = uri;
    }
    releaseGarbage(g);
    return true;
}

bool Engine::setLayerIsf(int i, const QString &path, QString *err, IsfFailure onFailure)
{
    if (!layer(i)) return false;
    auto inst = std::make_unique<IsfInstance>();
    IsfInstance *raw = inst.get();
    bool ok = false;
    runGl([raw, path, &ok] { ok = raw->load(path); });
    if (!ok) {
        if (err) *err = raw->error();
        if (onFailure == IsfFailure::Refuse) {
            runGl([raw] { raw->releaseGl(); });
            return false; // leave the previous source and GPU picture untouched
        }
    }
    std::shared_ptr<Garbage> g;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l) {
            runGl([raw] { raw->releaseGl(); });
            return false;
        }
        g = detachSource(*l);
        l->type = SourceType::Isf;
        l->sourcePath = QDir::cleanPath(QFileInfo(path).absoluteFilePath());
        l->error = inst->error();
        l->generator = std::move(inst);
    }
    releaseGarbage(g);
    return ok;
}

void Engine::setGeneratorSize(int i, int w, int h)
{
    Lock lk(&m_mutex);
    if (Layer *l = layer(i)) {
        l->genWidth = std::clamp(w, 1, 16384);
        l->genHeight = std::clamp(h, 1, 16384);
    }
}

void Engine::setLayerPlaying(int i, bool playing)
{
    Lock lk(&m_mutex);
    Layer *l = layer(i);
    if (!l || !l->hasTransport()) return;
    if (playing && l->atEnd()) startMedia(*l); // played to the end: starts again
    l->playing = playing;
    if (playing) l->ended = false;
}

void Engine::setLayerPlayMode(int i, PlayMode mode)
{
    Lock lk(&m_mutex);
    if (Layer *l = layer(i)) l->setPlayMode(mode);
}

void Engine::setLayerInOut(int i, double in, double out)
{
    Lock lk(&m_mutex);
    if (Layer *l = layer(i)) l->setInOut(in, out);
}

void Engine::setLayerSpeed(int i, double speed)
{
    Lock lk(&m_mutex);
    if (Layer *l = layer(i)) l->setSpeed(speed);
}

void Engine::seekLayer(int i, double t)
{
    Lock lk(&m_mutex);
    Layer *l = layer(i);
    if (!l || !l->hasTransport()) return;
    journal(QStringLiteral("seek"), QStringLiteral("%1 @ %2 s").arg(l->name).arg(t, 0, 'f', 3));
    reposition(*l, t, l->dir);
}

void Engine::setLayerVolume(int i, float volume)
{
    Lock lk(&m_mutex);
    if (Layer *l = layer(i)) l->volume = std::clamp(volume, 0.0f, 2.0f);
}

void Engine::setLayerMuted(int i, bool muted)
{
    Lock lk(&m_mutex);
    if (Layer *l = layer(i)) l->muted = muted;
}

bool Engine::setLayerText(int i)
{
    std::shared_ptr<Garbage> g;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l || l->isGroup || l->isViewport) return false;
        g = detachSource(*l);
        l->type = SourceType::Text;
        l->srcWidth = l->text.width;
        l->srcHeight = l->text.height;
    }
    releaseGarbage(g);
    return true;
}

bool Engine::setLayerTimecode(int i, const TimecodeSettings &s)
{
    bool already = false;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l || l->isGroup || l->isViewport) return false;
        if (s.output && !hasTimecodeOutput(s.output)) return false;
        const int user = s.output ? timecodeOutputUser(s.output) : -1;
        if (user >= 0 && user != i) return false; // a timecode output is driven by one layer only
        already = l->type == SourceType::Text && l->timecode;
        if (already && *l->timecode == s) return true;
    }
    if (!already) { // a text source first, in a style that suits a timecode
        if (!setLayerText(i)) return false;
        Lock lk(&m_mutex);
        if (Layer *l = layer(i)) {
            l->text.font = QFontDatabase::systemFont(QFontDatabase::FixedFont).family();
            l->text.size = 220;
            l->text.bold = true;
            l->text.width = 1600;
            l->text.height = 320;
            l->srcWidth = l->text.width;
            l->srcHeight = l->text.height;
        }
    }
    auto clock = std::make_shared<TimecodeClock>(s); // it may open devices: outside the lock
    std::shared_ptr<TimecodeClock> old;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(i);
        if (!l || l->type != SourceType::Text) return false;
        old = std::move(l->tcClock);
        if (old) clock->takeTransport(*old); // the transport goes on where it was
        l->timecode = s;
        l->tcClock = clock;
    }
    old.reset(); // its devices closed outside the lock too
    emit timecodeRoutingChanged();
    return true;
}

int Engine::timecodeOutputUser(quint64 output) const
{
    Lock lk(&m_mutex);
    if (!output) return -1;
    for (size_t k = 0; k < m_layers.size(); ++k)
        if (m_layers[k]->timecode && m_layers[k]->timecode->output == output) return int(k);
    return -1;
}

// ---------------------------------------------------------------------------
// Timecode outputs and their patch (MIDI outputs, audio channels)
// ---------------------------------------------------------------------------
QString Engine::uniquePatchName(const QString &base, PatchKind kind) const
{
    auto taken = [&](const QString &n) {
        switch (kind) {
        case PatchKind::Timecode: return std::any_of(m_tcOutputs.begin(), m_tcOutputs.end(), [&](const auto &x) { return x.name == n; });
        case PatchKind::Midi: return std::any_of(m_midiOutputs.begin(), m_midiOutputs.end(), [&](const auto &x) { return x.name == n; });
        case PatchKind::Audio: return std::any_of(m_audioOutputs.begin(), m_audioOutputs.end(), [&](const auto &x) { return x.name == n; });
        }
        return false;
    };
    if (!base.isEmpty() && !taken(base)) return base;
    const QString stem = !base.isEmpty()                 ? base
                         : kind == PatchKind::Midi      ? QStringLiteral("MIDI")
                         : kind == PatchKind::Audio     ? QStringLiteral("LTC")
                                                        : QStringLiteral("Timecode");
    for (int k = base.isEmpty() ? 1 : 2;; ++k)
        if (!taken(QStringLiteral("%1 %2").arg(stem).arg(k))) return QStringLiteral("%1 %2").arg(stem).arg(k);
}

bool Engine::routeTargetExists(const QString &route) const
{
    const quint64 id = tcroute::id(route);
    switch (tcroute::kind(route)) {
    case tcroute::Kind::Midi: return std::any_of(m_midiOutputs.begin(), m_midiOutputs.end(), [id](const auto &x) { return x.id == id; });
    case tcroute::Kind::Audio: return std::any_of(m_audioOutputs.begin(), m_audioOutputs.end(), [id](const auto &x) { return x.id == id; });
    case tcroute::Kind::Viewport: {
        const int i = indexOfId(id);
        return i >= 0 && m_layers[size_t(i)]->isViewport;
    }
    case tcroute::Kind::None: break;
    }
    return false;
}

bool Engine::hasTimecodeOutput(quint64 id) const
{
    Lock lk(&m_mutex);
    return std::any_of(m_tcOutputs.begin(), m_tcOutputs.end(), [id](const TimecodeOutput &t) { return t.id == id; });
}

std::vector<TimecodeOutput> Engine::timecodeOutputs() const
{
    Lock lk(&m_mutex);
    return m_tcOutputs;
}

quint64 Engine::addTimecodeOutput(const QString &name)
{
    quint64 id;
    {
        Lock lk(&m_mutex);
        TimecodeOutput o;
        o.id = id = m_nextPatchId++;
        o.name = uniquePatchName(name.trimmed(), PatchKind::Timecode);
        m_tcOutputs.push_back(o);
    }
    emit timecodeRoutingChanged();
    return id;
}

bool Engine::setTimecodeOutput(const TimecodeOutput &o)
{
    {
        Lock lk(&m_mutex);
        auto it = std::find_if(m_tcOutputs.begin(), m_tcOutputs.end(), [&](const TimecodeOutput &t) { return t.id == o.id; });
        if (it == m_tcOutputs.end()) return false;
        TimecodeOutput n = o;
        n.name = n.name.trimmed();
        if (n.name.isEmpty()) n.name = it->name;
        n.routes.removeDuplicates();
        n.routes.removeIf([&](const QString &r) {
            if (!routeTargetExists(r)) return true; // a destination that is not there (yet): left out
            for (const TimecodeOutput &t : m_tcOutputs) // a destination takes one timecode output
                if (t.id != n.id && t.routes.contains(r)) return true;
            return false;
        });
        if (n == *it) return true;
        *it = n;
    }
    emit timecodeRoutingChanged();
    return true;
}

void Engine::removeTimecodeOutput(quint64 id)
{
    const int user = timecodeOutputUser(id);
    if (user >= 0) {
        if (std::optional<TimecodeSettings> s = layerTimecode(user)) {
            s->output = 0;
            setLayerTimecode(user, *s);
        }
    }
    {
        Lock lk(&m_mutex);
        const auto n = m_tcOutputs.size();
        m_tcOutputs.erase(std::remove_if(m_tcOutputs.begin(), m_tcOutputs.end(), [id](const TimecodeOutput &t) { return t.id == id; }),
                          m_tcOutputs.end());
        if (n == m_tcOutputs.size()) return;
    }
    emit timecodeRoutingChanged();
}

QString Engine::routeName(const QString &route) const
{
    Lock lk(&m_mutex);
    const quint64 id = tcroute::id(route);
    switch (tcroute::kind(route)) {
    case tcroute::Kind::Midi:
        for (const MidiOutput &x : m_midiOutputs)
            if (x.id == id) return x.name;
        break;
    case tcroute::Kind::Audio:
        for (const AudioChannelOutput &x : m_audioOutputs)
            if (x.id == id) return x.name;
        break;
    case tcroute::Kind::Viewport: {
        const int i = indexOfId(id);
        if (i >= 0) return m_layers[size_t(i)]->name;
        break;
    }
    case tcroute::Kind::None: break;
    }
    return QString();
}

QString Engine::timecodeOutputStatus(quint64 id) const
{
    Lock lk(&m_mutex);
    auto it = std::find_if(m_tcOutputs.begin(), m_tcOutputs.end(), [&](const TimecodeOutput &t) { return t.id == id; });
    if (it == m_tcOutputs.end()) return QString();
    const int user = timecodeOutputUser(id);
    QString s = user >= 0 ? QStringLiteral("From %1").arg(m_layers[size_t(user)]->name) : QStringLiteral("No layer sends to it");
    QStringList to;
    for (const QString &r : it->routes) {
        const QString name = routeName(r);
        if (!name.isEmpty()) to << QStringLiteral("%1 to %2").arg(tcroute::protocol(tcroute::kind(r)), name);
    }
    return s + QStringLiteral(" · ") + (to.isEmpty() ? QStringLiteral("not patched") : to.join(QStringLiteral(", ")));
}

std::vector<MidiOutput> Engine::midiOutputs() const
{
    Lock lk(&m_mutex);
    return m_midiOutputs;
}

quint64 Engine::addMidiOutput(const QString &name)
{
    quint64 id;
    {
        Lock lk(&m_mutex);
        MidiOutput m;
        m.id = id = m_nextPatchId++;
        m.name = uniquePatchName(name.trimmed(), PatchKind::Midi);
        m_midiOutputs.push_back(m);
    }
    emit timecodeRoutingChanged();
    return id;
}

bool Engine::setMidiOutput(const MidiOutput &o)
{
    {
        Lock lk(&m_mutex);
        auto it = std::find_if(m_midiOutputs.begin(), m_midiOutputs.end(), [&](const MidiOutput &m) { return m.id == o.id; });
        if (it == m_midiOutputs.end()) return false;
        MidiOutput n = o;
        n.name = n.name.trimmed();
        if (n.name.isEmpty()) n.name = it->name;
        if (n == *it) return true;
        *it = n;
    }
    emit timecodeRoutingChanged();
    return true;
}

void Engine::removeMidiOutput(quint64 id)
{
    {
        Lock lk(&m_mutex);
        const auto n = m_midiOutputs.size();
        m_midiOutputs.erase(std::remove_if(m_midiOutputs.begin(), m_midiOutputs.end(), [id](const MidiOutput &m) { return m.id == id; }),
                            m_midiOutputs.end());
        if (n == m_midiOutputs.size()) return;
        for (TimecodeOutput &t : m_tcOutputs) t.routes.removeAll(tcroute::midi(id));
    }
    emit timecodeRoutingChanged();
}

QString Engine::midiOutputStatus(quint64 id) const { return m_tcRouter->midiStatus(id); }

std::vector<AudioChannelOutput> Engine::audioChannelOutputs() const
{
    Lock lk(&m_mutex);
    return m_audioOutputs;
}

quint64 Engine::addAudioChannelOutput(const QString &name)
{
    quint64 id;
    {
        Lock lk(&m_mutex);
        AudioChannelOutput a;
        a.id = id = m_nextPatchId++;
        a.name = uniquePatchName(name.trimmed(), PatchKind::Audio);
        m_audioOutputs.push_back(a);
    }
    emit timecodeRoutingChanged();
    return id;
}

bool Engine::setAudioChannelOutput(const AudioChannelOutput &o)
{
    {
        Lock lk(&m_mutex);
        auto it = std::find_if(m_audioOutputs.begin(), m_audioOutputs.end(), [&](const AudioChannelOutput &a) { return a.id == o.id; });
        if (it == m_audioOutputs.end()) return false;
        AudioChannelOutput n = o;
        n.name = n.name.trimmed();
        if (n.name.isEmpty()) n.name = it->name;
        n.channel = std::clamp(n.channel, 0, 63);
        if (n == *it) return true;
        *it = n;
    }
    emit timecodeRoutingChanged();
    return true;
}

void Engine::removeAudioChannelOutput(quint64 id)
{
    {
        Lock lk(&m_mutex);
        const auto n = m_audioOutputs.size();
        m_audioOutputs.erase(std::remove_if(m_audioOutputs.begin(), m_audioOutputs.end(), [id](const AudioChannelOutput &a) { return a.id == id; }),
                             m_audioOutputs.end());
        if (n == m_audioOutputs.size()) return;
        for (TimecodeOutput &t : m_tcOutputs) t.routes.removeAll(tcroute::audio(id));
    }
    emit timecodeRoutingChanged();
}

QString Engine::audioChannelOutputStatus(quint64 id) const { return m_tcRouter->audioStatus(id); }

QStringList Engine::midiPorts() { return midi::outputNames(); }
QStringList Engine::midiInputPorts() { return midi::inputNames(); }
bool Engine::midiVirtualPorts() { return midi::virtualPorts(); }
QStringList Engine::audioOutputDevices() { return AudioOutput::deviceNames(); }
QStringList Engine::audioInputDevices() { return AudioOutput::captureDeviceNames(); }

void Engine::updateTimecodeRouting()
{
    std::vector<TimecodeRouter::Stream> streams;
    std::vector<MidiOutput> midi;
    std::vector<AudioChannelOutput> audio;
    {
        Lock lk(&m_mutex);
        midi = m_midiOutputs;
        audio = m_audioOutputs;
        m_atc.clear();
        for (const auto &l : m_layers) {
            if (!l->tcClock || !l->timecode || !l->timecode->output) continue;
            for (const TimecodeOutput &t : m_tcOutputs) {
                if (t.id != l->timecode->output) continue;
                TimecodeRouter::Stream s;
                s.output = t.id;
                s.clock = l->tcClock;
                for (const QString &r : t.routes) {
                    const quint64 id = tcroute::id(r);
                    switch (tcroute::kind(r)) {
                    case tcroute::Kind::Midi: s.midi.push_back(id); break;
                    case tcroute::Kind::Audio: s.audio.push_back(id); break;
                    case tcroute::Kind::Viewport: m_atc[id] = l->tcClock; break; // read by Engine::frame
                    case tcroute::Kind::None: break;
                    }
                }
                streams.push_back(std::move(s));
            }
        }
    }
    m_tcRouter->configure(std::move(streams), std::move(midi), std::move(audio));
}

std::shared_ptr<TimecodeClock> Engine::layerTimecodeClock(int i) const
{
    Lock lk(&m_mutex);
    return i >= 0 && i < int(m_layers.size()) ? m_layers[size_t(i)]->tcClock : nullptr;
}

std::optional<TimecodeSettings> Engine::layerTimecode(int i) const
{
    Lock lk(&m_mutex);
    if (i < 0 || i >= int(m_layers.size())) return std::nullopt;
    return m_layers[size_t(i)]->timecode;
}

QString Engine::layerTimecodeStatus(int i) const
{
    std::shared_ptr<TimecodeClock> c;
    {
        Lock lk(&m_mutex);
        if (i >= 0 && i < int(m_layers.size())) c = m_layers[size_t(i)]->tcClock;
    }
    return c ? c->status() : QString();
}

void Engine::restartLayerTimecode(int i)
{
    Lock lk(&m_mutex);
    if (i >= 0 && i < int(m_layers.size()) && m_layers[size_t(i)]->tcClock) m_layers[size_t(i)]->tcClock->restart();
}

void Engine::setLayerTextContent(int i, const QString &text)
{
    Lock lk(&m_mutex);
    if (Layer *l = layer(i)) {
        l->text.content = text;
        l->text.stopTyping(); // typed by hand: no typewriter in progress
    }
}

void Engine::editLayerText(int i, const std::function<void(Layer &)> &edit)
{
    Lock lk(&m_mutex);
    if (Layer *l = layer(i)) edit(*l);
}

int Engine::addEffect(int li, const QString &path, QString *err, IsfFailure onFailure)
{
    if (!layer(li)) return -1;
    auto inst = std::make_unique<IsfInstance>();
    IsfInstance *raw = inst.get();
    bool ok = false;
    runGl([raw, path, &ok] { ok = raw->load(path); });
    if (!ok) {
        if (err) *err = raw->error();
        if (onFailure == IsfFailure::Refuse) {
            runGl([raw] { raw->releaseGl(); });
            return -1; // an operator adding it live: a broken effect never joins the chain
        }
    }
    Lock lk(&m_mutex);
    Layer *l = layer(li);
    if (!l) {
        runGl([raw] { raw->releaseGl(); });
        return -1;
    }
    l->effects.push_back(std::move(inst));
    return int(l->effects.size()) - 1;
}

void Engine::removeEffect(int li, int fx)
{
    std::shared_ptr<IsfInstance> victim;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(li);
        if (!l || fx < 0 || fx >= int(l->effects.size())) return;
        victim.reset(l->effects[size_t(fx)].release());
        l->effects.erase(l->effects.begin() + fx);
    }
    runGl([victim] { victim->releaseGl(); }, false);
}

void Engine::moveEffect(int li, int from, int to)
{
    Lock lk(&m_mutex);
    Layer *l = layer(li);
    if (!l) return;
    const int n = int(l->effects.size());
    if (from < 0 || from >= n) return;
    to = std::clamp(to, 0, n - 1);
    if (from == to) return;
    auto e = std::move(l->effects[size_t(from)]);
    l->effects.erase(l->effects.begin() + from);
    l->effects.insert(l->effects.begin() + to, std::move(e));
}

bool Engine::setIsfImageInput(IsfInstance *inst, int input, const QString &path, QString *err)
{
    if (!inst) return false;
    bool ok = false;
    QString e;
    runGl([&] { ok = inst->setImageInput(input, path, &e); });
    if (err) *err = e;
    return ok;
}

bool Engine::reloadIsf(IsfInstance *inst)
{
    if (!inst) return false;
    bool ok = false;
    runGl([&] {
        // Transactional reload: the previous shader, its uniforms, images
        // and last good output survive any parse/compile failure.
        ok = inst->reloadSafely();
    });
    Lock lk(&m_mutex);
    for (auto &l : m_layers) {
        if (l->generator.get() == inst) {
            l->error = inst->error();
            if (ok) l->paramKey = 0; // changed input defaults/ranges need a new Parameter registry
        }
        for (auto &fx : l->effects)
            if (fx.get() == inst && ok) l->paramKey = 0;
    }
    return ok;
}
