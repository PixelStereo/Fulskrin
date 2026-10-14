// Engine: the project file — a layer to and from JSON, saving and opening a .pinol.
#include "EngineInternal.h"

#include <set>
#include <functional>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QSaveFile>
#include <cmath>

static constexpr int kProjectFormat = 1; // the project file's format: the version that writes it reads only this one

QString Engine::projectPath() const
{
    Lock lk(&m_mutex);
    return m_projectPath;
}

void Engine::setProjectPath(const QString &p)
{
    Lock lk(&m_mutex);
    m_projectPath = p;
}

static QString missingMessage(const QString &path, const QString &err)
{
    return QFileInfo::exists(path) ? err : QStringLiteral("File not found: ") + path;
}

static void markMissing(Layer &l, SourceType type, const QString &path, const QString &err)
{
    l.missingType = type;
    l.sourcePath = path;
    l.error = missingMessage(path, err);
}

void Engine::newProject()
{
    clearProject();
    ensureViewport(); // a new project shows its composition through one viewport
    emit layersChanged();
    emit snapshotsChanged();
    emit presetsChanged();
    emit animationsChanged();
    emit m_sequencer->changed();
    emit m_sequencer->positionChanged();
}

// Empties everything (layers, viewports, snapshots, media bin)
void Engine::clearProject()
{
    std::vector<std::unique_ptr<Layer>> old;
    {
        Lock lk(&m_mutex);
        old.swap(m_layers);
        m_projectPath.clear();
        m_binItems.clear();
        m_snapshots.clear();
        m_nextSnapshotId = 1;
        m_snapshotTree.clear();
        m_nextSnapshotGroupId = 1;
        m_presets.clear();
        m_nextPresetId = 1;
        m_render = RenderSettings(); // the machine's defaults
        m_paused = false;
        m_compositionSpeed = 1.0;
        updateTimeScale();
        m_sequencer->clear();
        m_animations.clear();
        m_nextAnimationId = 1;
        m_fades.clear();
        m_recalledSnapshot = 0;
        m_recallTotal = 0;
        for (auto &[id, t] : m_transitions) retireTransition(std::move(t));
        m_transitions.clear();
        m_audio->setVolume(1.0f);
        m_tcOutputs.clear();
        m_midiOutputs.clear();
        m_audioOutputs.clear();
        m_atc.clear();
        m_interactions.clear();
        m_interactionRun.clear();
        m_interactionFades.clear();
        m_interactionParamsDirty = true;
        m_nextInteractionId = 1;
        m_nextPatchId = 1;
    }
    for (auto &l : old) {
        if (l->video) l->video->close();
        releaseAudio(*m_audio, l->audio);
        auto g = std::make_shared<Garbage>();
        g->layer = std::move(l);
        runGl([this, g] { releaseLayer(*g->layer); }, false);
    }
    setCompositionSize(QSize(1920, 1080));
    emit layersChanged();
    emit snapshotsChanged();
    emit presetsChanged();
    emit animationsChanged();
    emit timecodeRoutingChanged();
    emit interactionsChanged();
    updateMidiInputs();
}

QString Engine::resolvePath(const QJsonObject &o, const QString &projectDir) const
{
    const QString abs = o.value("path").toString();
    if (!abs.isEmpty() && QFile::exists(abs)) return abs;
    if (!projectDir.isEmpty()) {
        const QString rel = o.value("relative_path").toString();
        if (!rel.isEmpty()) {
            const QString p = QDir(projectDir).absoluteFilePath(rel);
            if (QFile::exists(p)) return QDir::cleanPath(p);
        }
        const QString same = QDir(projectDir).absoluteFilePath(QFileInfo(abs).fileName());
        if (QFile::exists(same)) return same;
    }
    const QString lib = m_library.findByFileName(QFileInfo(abs).fileName());
    if (!lib.isEmpty()) return lib;
    return abs;
}

// A layer in a file: what it is (its source, its effects, its mesh, where it is routed…) and the values of its
// parameters under "params", each at its address (the same as its OSC address, its timelines' and its snapshots'
// timing keys)
QJsonObject Engine::layerToJson(Layer &l, const QString &projectDir) const
{
    QJsonObject o;
    // Ids are strings: JSON numbers are doubles
    o["id"] = QString::number(l.id);
    if (l.parent) o["parent"] = QString::number(l.parent);
    if (l.isGroup) {
        o["group"] = true;
        o["collapsed"] = l.collapsed;
    }
    if (l.isViewport) {
        o["viewport"] = true;
        o["output"] = QJsonObject{{"width", l.vpWidth}, {"height", l.vpHeight}, {"screen", l.vpScreen}, {"mode", l.vpMode}};
        o["publish"] = l.vpPublish.toJson();
    }
    // How much of it each viewport shows (always there, so that a snapshot sets it)
    QJsonObject vo;
    for (const auto &[v, a] : l.viewportOpacity) vo[QString::number(v)] = double(a);
    o["viewports"] = vo;
    o["name"] = l.name;
    o["enable"] = l.enabled;
    o["locked"] = l.locked;
    if (!l.masks.empty()) o["masks"] = masks::toJson(l.masks);
    o["color_models"] = l.colorModels;
    if (l.color.maskLayer) o["mask"] = QString::number(l.color.maskLayer);

    QJsonObject src;
    const SourceType type = l.type != SourceType::None ? l.type : l.missingType;
    switch (type) {
    case SourceType::Video: src["type"] = "video"; break;
    case SourceType::Audio: src["type"] = "audio"; break;
    case SourceType::Image: src["type"] = "image"; break;
    case SourceType::Layer:
        src["type"] = "layer";
        src["layer"] = QString::number(l.sourceLayer);
        src["tap"] = layerTapKey(l.sourceTap);
        break;
    case SourceType::Isf:
        if (l.generator) src = l.generator->save(projectDir, false);
        src["type"] = "isf";
        src["width"] = l.genWidth;
        src["height"] = l.genHeight;
        break;
    case SourceType::Text:
        src["type"] = "text";
        if (l.timecode) src["timecode"] = l.timecode->toJson();
        break;
    case SourceType::Input:
        src["type"] = "input";
        src["uri"] = l.sourcePath;
        break;
    default: src["type"] = "none"; break;
    }
    if (type == SourceType::Video || type == SourceType::Audio) o["play"] = l.playing;
    if (type == SourceType::Video || type == SourceType::Audio || type == SourceType::Image || type == SourceType::Isf) {
        src["path"] = l.sourcePath;
        if (!projectDir.isEmpty()) src["relative_path"] = QDir(projectDir).relativeFilePath(l.sourcePath);
    }
    if (!l.transition.isEmpty()) src["transition"] = l.transition;
    o["source"] = src;
    QJsonArray fx;
    for (const auto &e : l.effects) fx.append(e->save(projectDir, false));
    o["fx"] = fx;
    if (!l.isViewport) o["mesh"] = l.mapping.meshJson();

    QJsonObject params = parametersToJson(l.parameters());
    for (auto it = l.extraParams.constBegin(); it != l.extraParams.constEnd(); ++it)
        if (!params.contains(it.key())) params.insert(it.key(), it.value());
    o["params"] = params;
    if (!l.anims.empty()) {
        QJsonArray anims;
        for (const Animation &a : l.anims) anims.append(layerAnimToJson(a));
        o["anims"] = anims;
    }
    return o;
}

void Engine::layerFromJson(int index, const QJsonObject &o, const QString &projectDir, QStringList *warnings)
{
    QString name;
    {
        Lock lk(&m_mutex);
        Layer *l = layer(index);
        if (!l) return;
        l->name = o.value("name").toString(l->name);
        l->enabled = o.value("enable").toBool(true);
        l->locked = o.value("locked").toBool(false);
        l->masks = masks::fromJson(o.value("masks").toArray());
        ++l->masksRevision;
        l->isGroup = o.value("group").toBool(false);
        l->collapsed = o.value("collapsed").toBool(false);
        l->isViewport = o.value("viewport").toBool(false);
        if (l->isViewport) {
            const QJsonObject out = o.value("output").toObject();
            l->vpWidth = std::clamp(out.value("width").toInt(1920), 1, 16384);
            l->vpHeight = std::clamp(out.value("height").toInt(1080), 1, 16384);
            l->vpScreen = out.value("screen").toString();
            l->vpMode = std::clamp(out.value("mode").toInt(0), 0, 2);
            l->vpPublish = PublishSettings::fromJson(o.value("publish").toObject());
        }
        l->viewportOpacity.clear();
        const QJsonObject vo = o.value("viewports").toObject();
        for (auto it = vo.begin(); it != vo.end(); ++it)
            l->viewportOpacity[it.key().toULongLong()] = float(std::clamp(it.value().toDouble(1.0), 0.0, 1.0));
        {
            const QString t = o.value("source").toObject().value("transition").toString();
            l->transition = t.isEmpty() ? QString() : resolvePath(QJsonObject{{"path", t}}, projectDir);
        }
        l->colorModels = o.value("color_models").toInt(l->colorModels);
        l->color.maskLayer = o.value("mask").toString().toULongLong();
        // Saved id kept unless another layer already has it
        const quint64 id = o.value("id").toString().toULongLong();
        bool taken = false;
        for (const auto &other : m_layers) taken |= other.get() != l && other->id == id;
        if (id && !taken) {
            l->id = id;
            m_nextId = std::max(m_nextId, id + 1);
        }
        l->parent = o.value("parent").toString().toULongLong();
        name = l->name;
    }

    const QJsonObject src = o.value("source").toObject();
    const QString type = src.value("type").toString();
    const QString path = type != "none" ? resolvePath(src, projectDir) : QString();
    QString err;
    if (type == "video" || type == "audio") {
        const bool video = type == "video";
        const bool ok = video ? setLayerVideo(index, path, &err) : setLayerAudio(index, path, &err);
        if (!ok && warnings) *warnings << name + ": " + missingMessage(path, err);
        if (!ok) {
            Lock lk(&m_mutex);
            markMissing(*layer(index), video ? SourceType::Video : SourceType::Audio, path, err);
        }
    } else if (type == "image") {
        const bool ok = setLayerImage(index, path, &err);
        if (!ok && warnings) *warnings << name + ": " + missingMessage(path, err);
        if (!ok) {
            Lock lk(&m_mutex);
            markMissing(*layer(index), SourceType::Image, path, err);
        }
    } else if (type == "layer") {
        // The referenced layer may not be loaded yet: the id is kept as it is and checked once the project is read
        // (fixLayerReferences); an unknown id simply shows nothing.
        Lock lk(&m_mutex);
        Layer *l = layer(index);
        l->type = SourceType::Layer;
        l->sourceLayer = src.value("layer").toString().toULongLong();
        l->sourceTap = layerTapFromKey(src.value("tap").toString());
    } else if (type == "isf") {
        {
            Lock lk(&m_mutex);
            layer(index)->genWidth = src.value("width").toInt(m_compSize.width());
            layer(index)->genHeight = src.value("height").toInt(m_compSize.height());
        }
        // A project: the generator stays even when its shader is missing or broken (path, values, error shown)
        if (!setLayerIsf(index, path, &err, IsfFailure::KeepWithError) && warnings) *warnings << name + ": " + err;
        IsfInstance *gen;
        {
            Lock lk(&m_mutex);
            gen = layer(index)->generator.get();
        }
        if (gen) runGl([gen, src, projectDir] { gen->restore(src, projectDir); });
    } else if (type == "input") { // a live input: kept even when it is not there now (it may come later)
        const QString uri = src.value("uri").toString();
        if (!setLayerInput(index, uri, &err)) {
            if (warnings) *warnings << name + ": " + err;
            Lock lk(&m_mutex);
            Layer *l = layer(index);
            l->type = SourceType::Input;
            l->sourcePath = uri;
            l->error = err;
        }
    } else if (type == "text") {
        {
            Lock lk(&m_mutex);
            Layer *l = layer(index);
            l->type = SourceType::Text;
            l->text = TextSource{};
        }
        if (src.contains("timecode")) { // a Timecode generator: its clock (its text style comes with the values)
            TimecodeSettings ts = TimecodeSettings::fromJson(src.value("timecode").toObject());
            // One layer per output (a copy drives none), and an output of the composition
            if (ts.output && (timecodeOutputUser(ts.output) >= 0 || !hasTimecodeOutput(ts.output))) ts.output = 0;
            auto clock = std::make_shared<TimecodeClock>(ts);
            Lock lk(&m_mutex);
            Layer *l = layer(index);
            l->timecode = ts;
            l->tcClock = clock;
        }
    }

    for (const QJsonValue &v : o.value("fx").toArray()) {
        const QJsonObject e = v.toObject();
        const QString p = resolvePath(e, projectDir);
        const int fi = addEffect(index, p, &err, IsfFailure::KeepWithError); // kept, broken or not (see IsfFailure)
        if (fi < 0) continue;
        IsfInstance *inst;
        {
            Lock lk(&m_mutex);
            inst = layer(index)->effects[size_t(fi)].get();
        }
        if (!inst->isValid() && warnings) *warnings << name + " / " + QFileInfo(p).fileName() + ": " + inst->error();
        runGl([inst, e, projectDir] { inst->restore(e, projectDir); });
    }
    // Its values once it is all there (the source would otherwise fit the mapping to its aspect ratio), in the order
    // of its parameters; the mesh's points; then its animations, the ones that are on playing from the start
    Lock lk(&m_mutex);
    Layer *l = layer(index);
    if (o.contains("mesh")) l->mapping.setMeshJson(o.value("mesh").toObject());
    parametersFromJson(l->parameters(), o.value("params").toObject(), &l->extraParams);
    if (l->hasTransport()) {
        // Played or paused: its parameter when the state has it (a snapshot that restarts it plays it), else the older
        // key of the projects
        const QJsonObject params = o.value("params").toObject();
        l->playing = params.value("restart").toBool(false) || params.value("play").toBool(o.value("play").toBool(true));
        startMedia(*l); // its speed: backwards starts from the end
    }
    l->anims.clear();
    for (const QJsonValue &v : o.value("anims").toArray()) {
        const Animation a = layerAnimFromJson(v.toObject(), l->id);
        const QString &param = a.tracks.front().param;
        const bool twice = std::any_of(l->anims.begin(), l->anims.end(), [&](const Animation &x) { return x.tracks.front().param == param; });
        if (!param.isEmpty() && !twice) l->anims.push_back(a);
    }
    for (Animation &a : l->anims)
        if (a.tracks.front().enabled) startLayerAnim(a);
}

bool Engine::saveProject(const QString &path, const QJsonObject &uiState, QString *err)
{
    const QJsonObject root = projectToJson(QFileInfo(path).absolutePath(), uiState);
    // Atomic write: a crash during save does not corrupt the existing file.
    QSaveFile f(path);
    if (!f.open(QIODevice::WriteOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    f.write(QJsonDocument(root).toJson(QJsonDocument::Indented));
    if (!f.commit()) {
        if (err) *err = f.errorString();
        return false;
    }
    return true;
}

QJsonObject Engine::projectToJson(const QString &dir, const QJsonObject &uiState) const
{
    QJsonObject root;
    {
        Lock lk(&m_mutex);
        root["app"] = "Pinol";
        root["format_version"] = kProjectFormat;
        root["composition"] = QJsonObject{{"width", m_compSize.width()}, {"height", m_compSize.height()},
                                    {"fps", renderSettings().frameRate}, {"speed", m_compositionSpeed.load()}};
        QJsonArray layers;
        for (const auto &l : m_layers) layers.append(layerToJson(*l, dir));
        root["layers"] = layers;
        QJsonArray bin;
        for (const QString &p : m_binItems)
            bin.append(QJsonObject{{"path", p}, {"relative_path", QDir(dir).relativeFilePath(p)}});
        root["bin"] = bin;
        root["audio"] = QJsonObject{{"volume", double(m_audio->volume())}};
        QJsonArray mems;
        for (const Snapshot &m : m_snapshots) mems.append(snapshotToJson(m, dir));
        root["snapshots"] = mems;
        root["snapshot_tree"] = snapshotTreeToJson(m_snapshotTree);
        QJsonArray presets;
        for (const Preset &p : m_presets) presets.append(presetToJson(p));
        root["presets"] = presets;
        root["timelines"] = animationsToJson();
        QJsonArray tcOutputs, midiOutputs;
        for (const TimecodeOutput &t : m_tcOutputs) tcOutputs.append(t.toJson());
        for (const MidiOutput &m : m_midiOutputs) midiOutputs.append(m.toJson());
        root["timecode_outputs"] = tcOutputs;
        root["midi_outputs"] = midiOutputs;
        QJsonArray audioOutputs;
        for (const AudioChannelOutput &a : m_audioOutputs) audioOutputs.append(a.toJson());
        root["audio_channel_outputs"] = audioOutputs;
        QJsonArray interactions;
        for (const Interaction &it : m_interactions) interactions.append(it.toJson());
        root["interactions"] = interactions;
        root["sequences"] = m_sequencer->toJson();
        root["current_sequence"] = m_sequencer->current();
    }
    root["ui"] = uiState;
    return root;
}

// Preflight is deliberately limited to structural errors: missing media and
// unplugged hardware must remain recoverable, reported per layer as warnings.
// Never clear the live show for a malformed but otherwise valid .pinol JSON.
static QString projectPreflightError(const QJsonObject &root)
{
    auto invalid = [](const QString &at, const QString &expect) {
        return QStringLiteral("Invalid Pinol project at %1: expected %2").arg(at, expect);
    };
    auto optionalObject = [&](const QJsonObject &o, const QString &k, const QString &at) -> QString {
        return o.contains(k) && !o.value(k).isObject() ? invalid(at + "/" + k, QStringLiteral("object")) : QString();
    };
    auto optionalArray = [&](const QJsonObject &o, const QString &k, const QString &at) -> QString {
        return o.contains(k) && !o.value(k).isArray() ? invalid(at + "/" + k, QStringLiteral("array")) : QString();
    };
    const QString rootPath = QStringLiteral("$");
    for (const QString &key : {QStringLiteral("composition"), QStringLiteral("audio"), QStringLiteral("ui")}) {
        const QString error = optionalObject(root, key, rootPath);
        if (!error.isEmpty()) return error;
    }
    const QJsonObject composition = root.value(QStringLiteral("composition")).toObject();
    for (const QString &dim : {QStringLiteral("width"), QStringLiteral("height")}) {
        const QJsonValue v = composition.value(dim);
        if (!v.isUndefined() && (!v.isDouble() || v.toDouble() < 1 || v.toDouble() > 16384))
            return invalid(QStringLiteral("$/composition/") + dim, QStringLiteral("size in [1, 16384]"));
    }
    for (const QString &key : {QStringLiteral("fps"), QStringLiteral("speed")}) {
        const QJsonValue v = composition.value(key);
        if (!v.isUndefined() && !v.isDouble())
            return invalid(QStringLiteral("$/composition/") + key, QStringLiteral("number"));
    }
    // snapshot_tree is intentionally heterogeneous: snapshot leaves are
    // string IDs, while nested groups are objects with a children array.
    // Do not validate it as a homogeneous array of objects.
    for (const QString &key : {QStringLiteral("layers"), QStringLiteral("bin"),
                               QStringLiteral("snapshots"),
                               QStringLiteral("presets"), QStringLiteral("timelines"),
                               QStringLiteral("timecode_outputs"), QStringLiteral("midi_outputs"),
                               QStringLiteral("audio_channel_outputs"), QStringLiteral("interactions"),
                               QStringLiteral("sequences")}) {
        const QString error = optionalArray(root, key, rootPath);
        if (!error.isEmpty()) return error;
        const QJsonArray arr = root.value(key).toArray();
        for (int i = 0; i < arr.size(); ++i) {
            if (!arr[i].isObject())
                return invalid(QStringLiteral("$/%1/%2").arg(key).arg(i), QStringLiteral("object"));
        }
    }
    const QString treeKey = QStringLiteral("snapshot_tree");
    const QString treeError = optionalArray(root, treeKey, rootPath);
    if (!treeError.isEmpty()) return treeError;
    // A snapshot leaf has the form "123"; a group is
    // {"group":"Name","id":"123","open":true,"children":[...]}.
    // Validate nested children too, and bound recursion on untrusted files.
    std::function<QString(const QJsonArray &, const QString &, int)> checkTree =
        [&](const QJsonArray &nodes, const QString &at, int depth) -> QString {
        if (depth > 64) return invalid(at, QStringLiteral("snapshot tree nesting <= 64"));
        for (int i = 0; i < nodes.size(); ++i) {
            const QString nodeAt = at + QLatin1Char('/') + QString::number(i);
            const QJsonValue v = nodes.at(i);
            if (v.isString()) {
                bool ok = false;
                const quint64 id = v.toString().toULongLong(&ok);
                if (!ok || !id) return invalid(nodeAt, QStringLiteral("nonzero snapshot ID string"));
                continue;
            }
            if (!v.isObject()) return invalid(nodeAt, QStringLiteral("snapshot ID string or group object"));
            const QJsonObject group = v.toObject();
            if (!group.value(QStringLiteral("group")).isString() ||
                !group.value(QStringLiteral("id")).isString())
                return invalid(nodeAt, QStringLiteral("group name and ID strings"));
            const QString childPath = nodeAt + QStringLiteral("/children");
            const QString childError = optionalArray(group, QStringLiteral("children"), nodeAt);
            if (!childError.isEmpty()) return childError;
            if (group.contains(QStringLiteral("children"))) {
                const QString nested = checkTree(group.value(QStringLiteral("children")).toArray(), childPath, depth + 1);
                if (!nested.isEmpty()) return nested;
            }
        }
        return {};
    };
    const QString invalidTree = checkTree(root.value(treeKey).toArray(), rootPath + QLatin1Char('/') + treeKey, 0);
    if (!invalidTree.isEmpty()) return invalidTree;

    const QJsonArray layers = root.value(QStringLiteral("layers")).toArray();
    for (int i = 0; i < layers.size(); ++i) {
        const QString where = QStringLiteral("$/layers/%1").arg(i);
        const QJsonObject l = layers[i].toObject();
        for (const QString &key : {QStringLiteral("source"), QStringLiteral("params"),
                                   QStringLiteral("mesh"), QStringLiteral("output"),
                                   QStringLiteral("publish"), QStringLiteral("viewports")}) {
            const QString error = optionalObject(l, key, where);
            if (!error.isEmpty()) return error;
        }
        for (const QString &key : {QStringLiteral("fx"), QStringLiteral("masks"),
                                   QStringLiteral("anims")}) {
            const QString error = optionalArray(l, key, where);
            if (!error.isEmpty()) return error;
            const QJsonArray list = l.value(key).toArray();
            for (int j = 0; j < list.size(); ++j)
                if (!list[j].isObject())
                    return invalid(QStringLiteral("%1/%2/%3").arg(where, key).arg(j), QStringLiteral("object"));
        }
        const QJsonObject src = l.value(QStringLiteral("source")).toObject();
        if (!src.isEmpty()) {
            const QJsonValue typeValue = src.value(QStringLiteral("type"));
            const QString type = typeValue.toString();
            if (!typeValue.isString() ||
                (type != QLatin1String("none") && type != QLatin1String("video") &&
                 type != QLatin1String("audio") && type != QLatin1String("image") &&
                 type != QLatin1String("layer") && type != QLatin1String("isf") &&
                 type != QLatin1String("input") && type != QLatin1String("text")))
                return invalid(where + QStringLiteral("/source/type"), QStringLiteral("known source type"));
            for (const QString &field : {QStringLiteral("path"), QStringLiteral("relative_path"),
                                         QStringLiteral("uri"), QStringLiteral("layer")}) {
                const QJsonValue value = src.value(field);
                if (!value.isUndefined() && !value.isString())
                    return invalid(where + QStringLiteral("/source/") + field, QStringLiteral("string"));
            }
        }
    }
    return {};
}

bool Engine::loadProjectAsTemplate(const QString &path, QJsonObject *uiState, QString *err)
{
    if (!loadProject(path, uiState, err)) return false;
    setProjectPath(QString()); // untitled: its media keep their absolute paths, made relative again where it is saved
    return true;
}

bool Engine::loadProject(const QString &path, QJsonObject *uiState, QString *err)
{
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly)) {
        if (err) *err = f.errorString();
        return false;
    }
    QJsonParseError pe;
    QJsonDocument doc = QJsonDocument::fromJson(f.readAll(), &pe);
    if (!doc.isObject()) {
        if (err) *err = QStringLiteral("Unreadable project: ") + pe.errorString();
        return false;
    }
    const QJsonObject root = doc.object();
    // A Pinol project, which this version reads, before anything of the current one is let go
    if (root.value("app").toString() != QLatin1String("Pinol") || !root.value("layers").isArray()) {
        if (err) *err = QStringLiteral("Not a Pinol project: ") + QFileInfo(path).fileName();
        return false;
    }
    const int version = root.value("format_version").toInt(0);
    if (version != kProjectFormat) {
        if (err)
            *err = QStringLiteral("%1: project format %2, this version of Pinol reads format %3")
                       .arg(QFileInfo(path).fileName())
                       .arg(version)
                       .arg(kProjectFormat);
        return false;
    }
    // Validate the complete JSON structure before detaching the current show.
    // A missing media file is still a warning, not an invalid project.
    const QString preflightError = projectPreflightError(root);
    if (!preflightError.isEmpty()) {
        if (err) *err = preflightError;
        return false;
    }
    clearProject();
    const QJsonObject comp = root.value("composition").toObject();
    setCompositionSize(QSize(comp.value("width").toInt(1920), comp.value("height").toInt(1080)));
    setCompositionSpeed(comp.value("speed").toDouble(1.0));
    {
        RenderSettings rs;
        rs.frameRate = comp.value("fps").toDouble(-1); // -1: the machine's default
        setRenderSettings(rs);
    }
    { // the patch before the layers: their timecode outputs are there to be taken
        Lock lk(&m_mutex);
        std::set<quint64> ids; // one id space for the three
        auto fresh = [&ids](quint64 id) { return id && ids.insert(id).second; };
        for (const QJsonValue &v : root.value("midi_outputs").toArray()) {
            const MidiOutput m = MidiOutput::fromJson(v.toObject());
            if (fresh(m.id)) m_midiOutputs.push_back(m);
        }
        for (const QJsonValue &v : root.value("audio_channel_outputs").toArray()) {
            const AudioChannelOutput a = AudioChannelOutput::fromJson(v.toObject());
            if (fresh(a.id)) m_audioOutputs.push_back(a);
        }
        for (const QJsonValue &v : root.value("timecode_outputs").toArray()) {
            const TimecodeOutput t = TimecodeOutput::fromJson(v.toObject());
            if (fresh(t.id)) m_tcOutputs.push_back(t); // its routes checked once the viewports are there
        }
        if (!ids.empty()) m_nextPatchId = *ids.rbegin() + 1;
    }
    const QString dir = QFileInfo(path).absolutePath();
    QStringList warnings;
    const QJsonArray layers = root.value("layers").toArray();
    for (int i = 0; i < layers.size(); ++i) {
        int idx = addLayer(QString(), layerCount());
        layerFromJson(idx, layers[i].toObject(), dir, &warnings);
    }
    {
        Lock lk(&m_mutex);
        normalizeLocked();
    }
    ensureViewport();
    fixLayerReferences(&warnings);
    {
        Lock lk(&m_mutex);
        std::set<QString> taken; // a destination takes one timecode output: the first one keeps it
        for (TimecodeOutput &t : m_tcOutputs)
            t.routes.removeIf([&](const QString &r) { return !routeTargetExists(r) || !taken.insert(r).second; });
    }
    {
        Lock lk(&m_mutex);
    }
    QStringList bin;
    for (const QJsonValue &v : root.value("bin").toArray()) bin << resolvePath(v.toObject(), dir);
    addBinItems(bin);
    const QJsonObject audio = root.value("audio").toObject();
    m_audio->setVolume(float(std::clamp(audio.value("volume").toDouble(1.0), 0.0, 2.0)));
    {
        Lock lk(&m_mutex);
        for (const QJsonValue &v : root.value("snapshots").toArray()) {
            Snapshot m = snapshotFromJson(v.toObject(), dir);
            bool taken = !m.id;
            for (const Snapshot &o : m_snapshots) taken = taken || o.id == m.id;
            if (taken) m.id = 0; // given below, after the saved ones
            m_snapshots.push_back(m);
        }
        for (const Snapshot &m : m_snapshots) m_nextSnapshotId = std::max(m_nextSnapshotId, m.id + 1);
        for (Snapshot &m : m_snapshots)
            if (!m.id) m.id = m_nextSnapshotId++;
        m_snapshotTree = snapshotTreeFromJson(root.value("snapshot_tree").toArray());
        normalizeSnapshotTree();
        for (const QJsonValue &v : root.value("presets").toArray()) {
            Preset p = presetFromJson(v.toObject());
            bool taken = !p.id;
            for (const Preset &o : m_presets) taken = taken || o.id == p.id;
            if (taken) p.id = 0;
            m_presets.push_back(p);
        }
        for (const Preset &p : m_presets) m_nextPresetId = std::max(m_nextPresetId, p.id + 1);
        for (Preset &p : m_presets)
            if (!p.id) p.id = m_nextPresetId++;
        animationsFromJson(root.value("timelines").toArray());
        m_sequencer->fromJson(root.value("sequences").toArray(), root.value("current_sequence").toInt(0));
    }
    emit snapshotsChanged();
    emit presetsChanged();
    emit animationsChanged();
    emit m_sequencer->changed();
    emit m_sequencer->positionChanged();
    if (uiState) *uiState = root.value("ui").toObject();
    setProjectPath(QFileInfo(path).absoluteFilePath());
    {
        std::vector<Interaction> interactions;
        for (const QJsonValue &v : root.value("interactions").toArray()) {
            Interaction it = Interaction::fromJson(v.toObject());
            if (std::any_of(interactions.begin(), interactions.end(), [&](const Interaction &x) { return x.id == it.id; }))
                it.id = 0; // given one
            interactions.push_back(it);
        }
        setInteractions(interactions);
    }
    emit layersChanged();
    emit timecodeRoutingChanged();
    if (!warnings.isEmpty() && err) *err = warnings.join('\n');
    return true;
}
