#include "PresetEditor.h"
#include "Commands.h"
#include "Engine.h"
#include "Interactions.h"

#include <QAbstractButton>
#include <QEvent>
#include <QLabel>
#include <QMenu>
#include <algorithm>
#include <QMouseEvent>
#include <QPainter>
#include <QUndoStack>
#include <QWidget>
#include <cmath>

static PresetEditor *s_instance = nullptr;
static const QColor kIn(76, 217, 100), kChanged(255, 159, 10), kDriven(135, 206, 250);

PresetEditor::PresetEditor(Engine *engine, QUndoStack *undo, QObject *parent)
    : QObject(parent), m_engine(engine), m_undo(undo)
{
    s_instance = this;
    m_timer.setInterval(200); // the values move (a fade, OSC, an edit): orange as soon as one is not the preset's
    connect(&m_timer, &QTimer::timeout, this, &PresetEditor::updateMarks);
    connect(engine, &Engine::presetsChanged, this, [this] {
        if (m_selected && m_engine->indexOfPreset(m_selected) < 0) select(0);
        updateMarks();
    });
    connect(engine, &Engine::layerAnimsChanged, this, [this](quint64) { updateMarks(); });
    connect(engine, &Engine::animationsChanged, this, [this] { updateMarks(); });
    connect(engine, &Engine::interactionsChanged, this, [this] { updateMarks(); });
}

PresetEditor::~PresetEditor()
{
    if (s_instance == this) s_instance = nullptr;
}

PresetEditor *PresetEditor::instance() { return s_instance; }

void PresetEditor::setEditing(bool on)
{
    if (on == m_editing) return;
    m_editing = on;
    if (on) m_timer.start();
    else m_timer.stop();
    updateMarks();
    emit editingChanged(on);
}

void PresetEditor::select(quint64 preset)
{
    if (preset == m_selected) return;
    m_selected = preset;
    updateMarks();
    emit selectedChanged(preset);
}

QStringList PresetEditor::stored(quint64 layer, const QStringList &paths) const
{
    QStringList out;
    if (layer && m_engine->indexOfId(layer) < 0) return out;
    for (const QString &p : paths) {
        if (layer && p.startsWith(QLatin1String("category/"))) { out << p; continue; }
        if (layer && p == QLatin1String("geometry/mesh")) { out << p; continue; }
        ParamInfo info;
        if (m_engine->parameterInfo(layer, p, &info) && info.type != ParamInfo::Type::Trigger &&
            (info.access & ParamInfo::Write))
            out << p;
    }
    return out;
}

void PresetEditor::track(QWidget *w, quint64 layer, const QStringList &paths)
{
    if (!w) return;
    const QStringList keep = stored(layer, paths);
    if (keep.isEmpty()) return;
    m_tracked.erase(std::remove_if(m_tracked.begin(), m_tracked.end(), [w](const Tracked &t) { return !t.widget || t.widget == w; }),
                    m_tracked.end());
    Tracked t;
    t.widget = w;
    t.layer = layer;
    t.paths = keep;
    t.name = qobject_cast<QLabel *>(w) || qobject_cast<QAbstractButton *>(w);
    m_tracked.push_back(t);
    w->installEventFilter(this);
    for (QWidget *c : w->findChildren<QWidget *>()) c->installEventFilter(this);
    updateMarks();
}

bool PresetEditor::inSelected(quint64 layer, const QStringList &paths) const
{
    const int i = m_engine->indexOfPreset(m_selected);
    if (i < 0) return false;
    const Engine::Preset p = m_engine->preset(i);
    for (const QString &path : paths)
        if (p.indexOf(layer, path) >= 0) return true;
    return false;
}

// Captures all drivers of the selected parameters, without affecting other preset entries.
void PresetEditor::capturePresetDrivers(Engine::Preset &preset, quint64 layer, const QStringList &paths) const
{
    for (const QString &path : paths) {
        preset.animations.erase(std::remove_if(preset.animations.begin(), preset.animations.end(), [&](const Animation &a) {
            return !a.tracks.empty() && a.tracks.front().layer == layer && a.tracks.front().param == path;
        }), preset.animations.end());
        Animation a;
        if (layer && m_engine->layerAnim(layer, path, &a)) preset.animations.push_back(a);

        preset.interactions.erase(std::remove_if(preset.interactions.begin(), preset.interactions.end(), [&](const Interaction &it) {
            return it.layer == layer && it.param == path;
        }), preset.interactions.end());
        for (const Interaction &it : m_engine->interactions())
            if (it.layer == layer && it.param == path) preset.interactions.push_back(it);
    }
}

void PresetEditor::addToNew(quint64 layer, const QStringList &paths)
{
    const QStringList keep = stored(layer, paths);
    if (keep.isEmpty()) return;
    Engine::Preset p;
    p.name = QStringLiteral("Preset %1").arg(m_engine->presetCount() + 1);
    for (const QString &path : keep) {
        if (path.startsWith(QLatin1String("category/"))) { captureCategory(p, layer, path); continue; }
        if (path == QLatin1String("geometry/mesh")) {
            Engine::Lock lock(&m_engine->mutex());
            if (const Layer *l = m_engine->layer(m_engine->indexOfId(layer))) p.geometry[QString::number(layer)] = l->mapping.meshJson();
        } else p.values.push_back(m_engine->presetValue(layer, path));
    }
    capturePresetDrivers(p, layer, keep);
    quint64 id = 0;
    cmd::editPresets(m_undo, m_engine, QStringLiteral("New Preset"), [&](std::vector<Engine::Preset> &all) {
        quint64 next = 1;
        for (const Engine::Preset &x : all) next = std::max(next, x.id + 1);
        p.id = id = next;
        all.push_back(p);
    });
    select(id);
}

void PresetEditor::addToSelected(quint64 layer, const QStringList &paths)
{
    if (!m_selected) return addToNew(layer, paths);
    const QStringList keep = stored(layer, paths);
    cmd::editPresets(m_undo, m_engine, QStringLiteral("Add to Preset"), [&](std::vector<Engine::Preset> &all) {
        for (Engine::Preset &p : all) {
            if (p.id != m_selected) continue;
            for (const QString &path : keep) {
                if (path.startsWith(QLatin1String("category/"))) { captureCategory(p, layer, path); continue; }
                if (path == QLatin1String("geometry/mesh")) {
                    Engine::Lock lock(&m_engine->mutex());
                    if (const Layer *l = m_engine->layer(m_engine->indexOfId(layer))) p.geometry[QString::number(layer)] = l->mapping.meshJson();
                    continue;
                }
                const int k = p.indexOf(layer, path);
                const Engine::PresetValue now = m_engine->presetValue(layer, path);
                if (k >= 0) p.values[size_t(k)].value = now.value; // in already: its value now
                else p.values.push_back(now);
            }
            capturePresetDrivers(p, layer, keep);
        }
    });
}

void PresetEditor::removeFromSelected(quint64 layer, const QStringList &paths)
{
    cmd::editPresets(m_undo, m_engine, QStringLiteral("Remove from Preset"), [&](std::vector<Engine::Preset> &all) {
        for (Engine::Preset &p : all) {
            if (p.id != m_selected) continue;
            if (paths.contains(QStringLiteral("geometry/mesh"))) p.geometry.remove(QString::number(layer));
            auto &v = p.values;
            v.erase(std::remove_if(v.begin(), v.end(),
                                   [&](const Engine::PresetValue &x) { return x.layer == layer && paths.contains(x.path); }),
                    v.end());
            for (const QString &path : paths) {
                p.animations.erase(std::remove_if(p.animations.begin(), p.animations.end(), [&](const Animation &a) {
                    return !a.tracks.empty() && a.tracks.front().layer == layer && a.tracks.front().param == path;
                }), p.animations.end());
                p.interactions.erase(std::remove_if(p.interactions.begin(), p.interactions.end(), [&](const Interaction &it) {
                    return it.layer == layer && it.param == path;
                }), p.interactions.end());
            }
        }
    });
}

void PresetEditor::fillMenu(QMenu *menu, quint64 layer, const QStringList &paths)
{
    const QStringList keep = stored(layer, paths);
    if (keep.isEmpty()) return;
    if (!menu->isEmpty()) menu->addSeparator();
    menu->addAction(QStringLiteral("Add to New Preset"), this, [this, layer, keep] { addToNew(layer, keep); });
    const int i = m_engine->indexOfPreset(m_selected);
    const QString name = i >= 0 ? m_engine->preset(i).name : QString();
    QAction *add = menu->addAction(i >= 0 ? QStringLiteral("Add to Preset \"%1\"").arg(name) : QStringLiteral("Add to Selected Preset"),
                                   this, [this, layer, keep] { addToSelected(layer, keep); });
    add->setEnabled(i >= 0);
    if (i >= 0 && inSelected(layer, keep))
        menu->addAction(QStringLiteral("Remove from Preset \"%1\"").arg(name), this,
                        [this, layer, keep] { removeFromSelected(layer, keep); });
}

const PresetEditor::Tracked *PresetEditor::trackedFor(QObject *o) const
{
    for (auto *w = qobject_cast<QWidget *>(o); w; w = w->parentWidget())
        for (const Tracked &t : m_tracked)
            if (t.widget == w) return &t;
    return nullptr;
}

void PresetEditor::toggle(const Tracked &t)
{
    if (inSelected(t.layer, t.paths)) removeFromSelected(t.layer, t.paths);
    else addToSelected(t.layer, t.paths);
}

bool PresetEditor::eventFilter(QObject *o, QEvent *e)
{
    if (!m_editing) return false;
    const QEvent::Type type = e->type();
    if ((type == QEvent::MouseButtonRelease || type == QEvent::MouseButtonDblClick) && o == m_swallow) {
        if (type == QEvent::MouseButtonRelease) m_swallow = nullptr;
        return true;
    }
    if (type != QEvent::MouseButtonPress) return false;
    auto *me = static_cast<QMouseEvent *>(e);
    if (me->button() != Qt::LeftButton) return false;
    const Tracked *t = trackedFor(o);
    if (!t) return false;
    // Its name: a click; its field: an Alt-click (a plain click edits the value, as always)
    if (!t->name && !(me->modifiers() & Qt::AltModifier)) return false;
    const Tracked copy = *t;
    m_swallow = o;
    toggle(copy);
    return true;
}

// The lines, over the whole window: each under the part of its widget that is seen (a scrolled panel clips it)
class PresetEditor::Overlay : public QWidget
{
public:
    Overlay(PresetEditor *editor, QWidget *window) : QWidget(window), m_editor(editor)
    {
        setAttribute(Qt::WA_TransparentForMouseEvents);
        setAttribute(Qt::WA_NoSystemBackground);
        window->installEventFilter(this);
        setGeometry(window->rect());
        raise();
    }

protected:
    bool eventFilter(QObject *o, QEvent *e) override
    {
        if (o == parentWidget() && e->type() == QEvent::Resize) {
            setGeometry(parentWidget()->rect());
            raise();
        }
        return false;
    }
    void paintEvent(QPaintEvent *) override
    {
        QPainter p(this);
        QWidget *win = parentWidget();
        for (const Tracked &t : m_editor->m_tracked) {
            if ((!t.state && !t.driven) || !t.widget || !t.widget->isVisible() || t.widget->window() != win) continue;
            const QPoint at = t.widget->mapTo(win, QPoint(0, 0));
            const QRegion seen = t.widget->visibleRegion().translated(at);
            const QRect line(at.x(), at.y() + t.widget->height() - 2, t.widget->width(), 2);
            const QColor c = t.state == 2 ? kChanged : t.state == 1 ? kIn : kDriven;
            for (const QRect &r : seen.intersected(line)) p.fillRect(r, c);
        }
    }

private:
    PresetEditor *m_editor;
};

// The lines under the widgets of the values in the selected preset (Edit mode only): on a value's name when it has one,
// on its field otherwise
void PresetEditor::updateMarks()
{
    m_tracked.erase(std::remove_if(m_tracked.begin(), m_tracked.end(), [](const Tracked &t) { return !t.widget; }),
                    m_tracked.end());
    const int pi = m_engine->indexOfPreset(m_selected);
    const Engine::Preset preset = pi >= 0 ? m_engine->preset(pi) : Engine::Preset();
    auto hasName = [this](const Tracked &t) {
        for (const Tracked &o : m_tracked)
            if (o.name && o.layer == t.layer && o.paths == t.paths && o.widget) return true;
        return false;
    };
    QWidget *window = nullptr;
    const std::vector<Interaction> interactions = m_engine->interactions();
    for (Tracked &t : m_tracked) {
        t.state = 0;
        t.driven = false;
        for (const Animation &animation : m_engine->layerAnims(t.layer)) {
            for (const AnimTrack &track : animation.tracks) {
                if (track.enabled && t.paths.contains(track.param)) {
                    t.driven = true;
                    break;
                }
            }
            if (t.driven) break;
        }
        if (!t.driven) {
            for (const Interaction &interaction : interactions) {
                if (interaction.enabled && interaction.layer == t.layer && t.paths.contains(interaction.param)) {
                    t.driven = true;
                    break;
                }
            }
        }
        if (m_editing && pi >= 0 && (t.name || !hasName(t)))
            for (const QString &path : t.paths) {
                const int k = preset.indexOf(t.layer, path);
                if (k < 0) continue;
                t.state = std::max(t.state, 1);
                const QJsonValue now = m_engine->parameterValue(t.layer, path), was = preset.values[size_t(k)].value;
                const bool same = now.isDouble() && was.isDouble() ? std::abs(now.toDouble() - was.toDouble()) < 1e-6 : now == was;
                if (!same) t.state = 2;
            }
        if (!window && t.widget) window = t.widget->window();
    }
    if (window && (!m_overlay || m_overlay->parentWidget() != window)) {
        delete m_overlay;
        m_overlay = new Overlay(this, window);
    }
    if (m_overlay) {
        const bool hasDriven = std::any_of(m_tracked.begin(), m_tracked.end(), [](const Tracked &t) { return t.driven && t.widget; });
        m_overlay->setVisible(m_editing || hasDriven);
        m_overlay->raise();
        m_overlay->update();
    }
}

std::vector<std::pair<QString, QStringList>> PresetEditor::layerValues(Engine *engine, quint64 layer)
{
    std::vector<std::pair<QString, QStringList>> out;
    for (const ParamInfo &i : engine->parameters(layer)) {
        if (i.type == ParamInfo::Type::Trigger || i.access != ParamInfo::ReadWrite) continue;
        const int cut = i.label.indexOf(QStringLiteral(" › "));
        const QString cat = cut < 0 ? QStringLiteral("Layer") : i.label.left(cut);
        auto it = std::find_if(out.begin(), out.end(), [&](const auto &c) { return c.first == cat; });
        if (it == out.end()) {
            out.push_back({cat, {}});
            it = out.end() - 1;
        }
        it->second << i.path;
    }
    return out;
}

void PresetEditor::fillLayerMenu(QMenu *menu, quint64 layer)
{
    const auto groups = layerValues(m_engine, layer);
    if (groups.empty()) return;
    const int i = m_engine->indexOfPreset(m_selected);
    const QString target = i >= 0 ? QStringLiteral("Add to Preset \"%1\"").arg(m_engine->preset(i).name)
                                  : QStringLiteral("Add to New Preset");
    QMenu *sub = menu->addMenu(target);
    sub->setToolTip(QStringLiteral("Any value of this layer, whether the inspector shows it or not"));
    auto add = [this, layer](const QStringList &paths) {
        if (m_selected && m_engine->indexOfPreset(m_selected) >= 0) addToSelected(layer, paths);
        else addToNew(layer, paths);
    };
    QStringList all;
    for (const auto &g : groups) all << g.second;
    sub->addAction(QStringLiteral("All Values"), this, [add, all] { add(all); });
    sub->addSeparator();
    for (const auto &[cat, paths] : groups) {
        QMenu *c = sub->addMenu(cat);
        c->addAction(QStringLiteral("All %1 Values").arg(cat), this, [add, paths = paths] { add(paths); });
        c->addSeparator();
        for (const QString &p : paths) {
            ParamInfo info;
            QString label = p;
            if (m_engine->parameterInfo(layer, p, &info)) {
                const int cut = info.label.indexOf(QStringLiteral(" › "));
                label = cut < 0 ? info.label : info.label.mid(cut + 3);
            }
            QAction *a = c->addAction(label, this, [add, p] { add({p}); });
            a->setToolTip(p);
            a->setCheckable(true);
            a->setChecked(i >= 0 && inSelected(layer, {p}));
        }
    }
}

void PresetEditor::fillCategoryMenu(QMenu *menu, quint64 layer, const QString &category)
{
    QStringList paths;
    for (const auto &group : layerValues(m_engine, layer))
        if (group.first == category || (category == QLatin1String("Anim") && !group.second.isEmpty())) paths += group.second;
    if (category == QLatin1String("Spatial")) paths << QStringLiteral("geometry/mesh");
    int part = 0;
    if (category == QLatin1String("Source")) part = Engine::PartSource;
    if (category == QLatin1String("FX")) part = Engine::PartEffects;
    if (category == QLatin1String("Masks")) part = Engine::PartMasks;
    if (category == QLatin1String("Viewports")) part = Engine::PartViewports;
    if (part) paths << QStringLiteral("category/%1").arg(part);
    if (category == QLatin1String("Source"))
        for (const auto &group : layerValues(m_engine, layer))
            for (const auto &path : group.second) if (Engine::partOf(path) & (Engine::PartSource | Engine::PartTransport)) paths << path;
    if (category == QLatin1String("Anim")) {
        paths.clear();
        for (const auto &animation : m_engine->layerAnims(layer)) if (!animation.tracks.empty()) paths << animation.tracks.front().param;
    }
    paths.removeDuplicates();
    fillMenu(menu, layer, paths);
}

void PresetEditor::addInteraction(quint64 id, bool newPreset)
{
    const auto interactions = m_engine->interactions();
    const auto found = std::find_if(interactions.begin(), interactions.end(), [id](const Interaction &it) { return it.id == id; });
    if (found == interactions.end()) return;
    quint64 selected = m_selected;
    if (newPreset || m_engine->indexOfPreset(selected) < 0) selected = 0;
    cmd::editPresets(m_undo, m_engine, QStringLiteral("Store Interaction in Preset"), [&](std::vector<Engine::Preset> &all) {
        if (!selected) {
            Engine::Preset p; p.name = found->name;
            p.id = 1; for (const auto &item : all) p.id = std::max(p.id, item.id + 1);
            selected = p.id; all.push_back(p);
        }
        for (auto &preset : all) if (preset.id == selected) {
            auto it = std::find_if(preset.interactions.begin(), preset.interactions.end(), [id](const Interaction &item) { return item.id == id; });
            if (it == preset.interactions.end()) preset.interactions.push_back(*found); else *it = *found;
        }
    });
    select(selected);
}

void PresetEditor::fillInteractionMenu(QMenu *menu, quint64 id)
{
    menu->addAction(QStringLiteral("Add Interaction to New Preset"), this, [this, id] { addInteraction(id, true); });
    auto *action = menu->addAction(QStringLiteral("Add Interaction to Selected Preset"), this, [this, id] { addInteraction(id, false); });
    action->setEnabled(m_engine->indexOfPreset(m_selected) >= 0);
}

void PresetEditor::captureCategory(Engine::Preset &preset, quint64 layer, const QString &path) const
{
    const int part = path.section('/', 1).toInt();
    if (!(part & (Engine::PartSource | Engine::PartEffects | Engine::PartMasks | Engine::PartViewports))) return;
    const int index = m_engine->indexOfId(layer);
    if (index < 0) return;
    QJsonObject state = m_engine->layerJson(index);
    state.remove("params"); state.remove("anims"); state.remove("play");
    // Store only the structural data this category owns.
    QJsonObject category;
    if (part == Engine::PartSource) category["source"] = state.value("source");
    if (part == Engine::PartEffects) category["fx"] = state.value("fx");
    if (part == Engine::PartMasks) category["masks"] = state.value("masks");
    if (part == Engine::PartViewports) category["viewports"] = state.value("viewports");
    preset.categories[QStringLiteral("%1:%2").arg(layer).arg(part)] = category;
}
