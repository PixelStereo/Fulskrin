#pragma once
#include "../engine/Engine.h"
// Presets in the interface: which one is selected, the Edit mode, and the parameters' widgets that can go in one.
//
// The inspector's widgets of a value register here (AnimateMenu::attach does it). In Edit mode, each one in the
// selected preset is underlined: green, or orange once its value is no longer the preset's; a click on its name (or an
// Alt-click on its field) puts it in the preset or takes it out. Out of Edit mode the lines go. A right-click on it
// offers Add to New Preset / Add to Selected Preset. Every change is one undo step.

#include <QObject>
#include <QPointer>
#include <QStringList>
#include <QTimer>
#include <vector>

class Engine;
class QMenu;
class QUndoStack;
class QWidget;

class PresetEditor : public QObject
{
    Q_OBJECT
public:
    PresetEditor(Engine *engine, QUndoStack *undo, QObject *parent = nullptr);
    ~PresetEditor() override;
    static PresetEditor *instance(); // the application's (null in the tests)

    bool editing() const { return m_editing; }
    void setEditing(bool on);
    quint64 selected() const { return m_selected; } // the selected preset's id (0: none)
    void select(quint64 preset);

    // `w` (and what is in it) shows the values at `paths` of `layer` (0: the composition); those that are not stored
    // values are ignored
    void track(QWidget *w, quint64 layer, const QStringList &paths);
    // The preset actions for those values, at the end of a right-click menu
    void fillMenu(QMenu *menu, quint64 layer, const QStringList &paths);
    // Every stored value of `layer`, by category, in an "Add to Preset" sub-menu (the selected preset, else a new
    // one): whatever the inspector shows or not, any value can go in a preset
    void fillLayerMenu(QMenu *menu, quint64 layer);
    // The stored values of `layer` grouped by category ("Source", "Spatial"…), in the layer's order (tests)
    static std::vector<std::pair<QString, QStringList>> layerValues(Engine *engine, quint64 layer);
    void fillCategoryMenu(QMenu *menu, quint64 layer, const QString &category);
    void fillInteractionMenu(QMenu *menu, quint64 interaction);
    void addInteraction(quint64 id, bool newPreset);
    void capturePresetDrivers(Engine::Preset &preset, quint64 layer, const QStringList &paths) const;
    void addToNew(quint64 layer, const QStringList &paths);
    void addToSelected(quint64 layer, const QStringList &paths);
    void removeFromSelected(quint64 layer, const QStringList &paths);
    bool inSelected(quint64 layer, const QStringList &paths) const; // one of them at least

signals:
    void editingChanged(bool on);
    void selectedChanged(quint64 preset);

protected:
    bool eventFilter(QObject *o, QEvent *e) override;

private:
    struct Tracked {
        QPointer<QWidget> widget;
        quint64 layer = 0;
        QStringList paths;
        bool name = false; // a label or a switch: its name, the one marked
        int state = 0;     // 0 not in the selected preset, 1 in it, 2 in it with another value now
        bool driven = false; // an enabled animation track or interaction targets one of these values
    };
    void captureCategory(Engine::Preset &preset, quint64 layer, const QString &path) const;
    QStringList stored(quint64 layer, const QStringList &paths) const;
    void toggle(const Tracked &t);
    void updateMarks();
    const Tracked *trackedFor(QObject *o) const;

    Engine *m_engine;
    QUndoStack *m_undo;
    bool m_editing = false;
    quint64 m_selected = 0;
    std::vector<Tracked> m_tracked;
    QTimer m_timer;
    QObject *m_swallow = nullptr; // a press taken: its release and double-click go too
    class Overlay;
    QPointer<QWidget> m_overlay; // over the window: the lines, drawn where the widgets are seen
    friend class Overlay;
};
