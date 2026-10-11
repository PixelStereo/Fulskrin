#pragma once
// Masks tab of a layer, a group or a viewport (Masks.h): tools that add a shape (drawn point by point in the
// preview, a rectangle, a triangle, an ellipse), the list of masks, and the selected one's settings: invert,
// feathering and where the soft edge goes, what it is attached to (its picture, or the composition), SVG
// import and export. The preview edits the selected mask's points.

#include "Masks.h"

#include <QWidget>
#include <functional>
#include <vector>

class Engine;
class QUndoStack;
class QListWidget;
class QPushButton;
class QComboBox;
class QLabel;
class SliderField;

class MaskPanel : public QWidget
{
    Q_OBJECT
public:
    MaskPanel(Engine *engine, QUndoStack *undo, quint64 layer, bool viewport, bool locked, QWidget *parent = nullptr);
    void refresh();        // the masks changed elsewhere (the preview, undo): list and settings again
    int current() const;   // the selected mask (-1: none)
    void select(int index);

signals:
    void currentChanged(int index); // the preview edits this one
    void drawRequested();           // pen: the preview draws a new mask, point by point
    void edited();                  // the masks changed (the preview redraws)

private:
    void add(Mask m);                                       // a new shape, selected
    void change(const std::function<void(Mask &)> &f, const QString &text);
    void changeLive(const std::function<void(Mask &)> &f); // while a slider moves: undone in one step at its end
    void endLive(const QString &text);
    void remove();
    void importSvg();
    void exportSvg();
    void showCurrent(); // the selected mask's settings
    std::vector<Mask> masks() const;

    Engine *m_engine;
    QUndoStack *m_undo;
    quint64 m_layer;
    bool m_viewport, m_locked;
    QListWidget *m_list = nullptr;
    QPushButton *m_invert = nullptr, *m_remove = nullptr;
    SliderField *m_feather = nullptr;
    QComboBox *m_mode = nullptr, *m_attach = nullptr;
    QWidget *m_settings = nullptr;
    QLabel *m_empty = nullptr;
    std::vector<Mask> m_liveBefore;
    bool m_live = false, m_syncing = false;
};
