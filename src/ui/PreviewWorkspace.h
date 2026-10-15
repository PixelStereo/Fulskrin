#pragma once
#include <QWidget>
#include <array>
class Engine;
class MappingView;
class QGridLayout;
class QStackedWidget;
class QToolButton;
class PreviewPicture;

// Four independent preview sources. Composition in the first cell retains mapping tools.
class PreviewWorkspace : public QWidget
{
public:
    PreviewWorkspace(Engine *engine, MappingView *mapping, QWidget *parent = nullptr);
    ~PreviewWorkspace() override;
    void refreshFrames();
private:
    void setCount(int count);
    void selectSource(int slot, quint64 id, const QString &label);
    Engine *m_engine;
    MappingView *m_mapping;
    QGridLayout *m_grid;
    struct Cell {
        QWidget *widget = nullptr;
        QStackedWidget *pages = nullptr;
        QToolButton *source = nullptr;
        PreviewPicture *picture = nullptr;
        quint64 id = 0;
    };
    std::array<Cell, 4> m_cells;
    int m_count = 1;
};
