#pragma once
// "Interactions" tab (left, after Media Bin and Layers): values of the show driven by a key, a MIDI controller, an OSC
// message, or by another value of the show (Internal). One sub-tab per kind of source and one with all of them; a list
// (on / off, source, target, what it does now), + / − / Learn / search; under it, the settings of the one selected:
// its source, its target, master level, pick-up or jump, toggle, source and target ranges, a filter, a curve. Copy and
// Paste Settings carry the chain from one to others. Edits are undone as any other.

#include "Interactions.h"

#include <QHash>
#include <QPointer>
#include <QWidget>
#include <optional>

class Engine;
class QUndoStack;
class QTabBar;
class QTreeWidget;
class QTreeWidgetItem;
class QLineEdit;
class QPushButton;
class QToolButton;
class QVBoxLayout;
class QLabel;
class QProgressBar;
class QTimer;

class InteractionPanel : public QWidget
{
    Q_OBJECT
public:
    InteractionPanel(Engine *engine, QUndoStack *undo, QWidget *parent = nullptr);
    // A key of the keyboard, pressed or let go, while no text is typed (the main window passes them all)
    void keyEvent(int key, Qt::KeyboardModifiers mods, bool pressed);
    static QString keyName(int key, Qt::KeyboardModifiers mods); // "Space", "Shift+A" (portable)

signals:
    void edited(); // the list changed: the project is modified

protected:
    bool eventFilter(QObject *o, QEvent *e) override; // right-click on a value: the preset actions

private:
    // The widgets of a value of the one shown ("master", "source/min"…): presets can take it (Edit mode, right-click)
    void attachPreset(std::initializer_list<QWidget *> ws, const QStringList &paths);
    std::optional<InteractionSource> shownKind() const; // the sub-tab's (none: All)
    void rebuildList();
    void rebuildSettings();
    void refreshLevels();
    void pollLearn();
    quint64 selectedId() const;
    std::optional<Interaction> selected() const;
    // The list with `it` replacing the one of its id, as an undo step (edits with the same key that follow one another
    // form one step: a drag)
    void commit(const Interaction &it, const QString &text, const QString &mergeKey = QString());
    void commitList(const std::vector<Interaction> &list, const QString &text);
    void add(InteractionSource kind);
    void removeSelected();
    QString targetLabel(quint64 layer, const QString &param) const;
    QString describe(const Interaction &it) const;

    Engine *m_engine;
    QUndoStack *m_undo;
    QTabBar *m_kinds;
    QTreeWidget *m_list;
    QLineEdit *m_search;
    QToolButton *m_learn;
    QPushButton *m_remove;
    QWidget *m_settings = nullptr;
    QVBoxLayout *m_settingsLayout = nullptr;
    QLabel *m_settingsTitle = nullptr;
    QPushButton *m_copy = nullptr, *m_paste = nullptr;
    QProgressBar *m_inMeter = nullptr, *m_outMeter = nullptr;
    QPointer<class CurveLane> m_lane;
    bool m_revealCurve = false;
    class QScrollArea *m_scroll = nullptr;
    QTimer *m_timer;
    QTimer *m_valuesTimer = nullptr;
    quint64 m_shown = 0;        // the interaction whose settings are shown
    bool m_committing = false;  // its own edit going to the engine: the settings are not made again
    bool m_filling = false;
    QHash<int, QString> m_pressed; // keys held: the name they were pressed with (their modifiers may change since)
};
