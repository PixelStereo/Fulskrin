#include "PreviewWorkspace.h"
#include "MappingView.h"
#include "Engine.h"
#include "GlDraw.h"
#include <QComboBox>
#include <QGridLayout>
#include <QMenu>
#include <QOpenGLContext>
#include <QOpenGLWidget>
#include <QStackedWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <algorithm>

class PreviewPicture : public QOpenGLWidget
{
public:
    PreviewPicture(Engine *engine, int slot) : m_engine(engine), m_slot(slot) { setMinimumSize(100, 70); }
    ~PreviewPicture() override {
        if (context()) { makeCurrent(); m_draw.destroy(); doneCurrent(); }
    }
protected:
    void initializeGL() override { m_draw.init(); }
    void paintGL() override {
        auto *f = context()->extraFunctions();
        const qreal dpr = devicePixelRatioF();
        f->glViewport(0, 0, int(width() * dpr), int(height() * dpr));
        f->glClearColor(0.07f, 0.07f, 0.08f, 1);
        f->glClear(GL_COLOR_BUFFER_BIT);
        Engine::Lock lock(&m_engine->mutex());
        m_engine->waitForFrame(f);
        QSize size;
        int buffer = 0;
        const GLuint texture = m_engine->previewTexture(m_slot, &size, &buffer);
        if (!texture || size.isEmpty()) return;
        size.scale(this->size(), Qt::KeepAspectRatio);
        f->glViewport(int((width() - size.width()) * dpr / 2), int((height() - size.height()) * dpr / 2),
                      int(size.width() * dpr), int(size.height() * dpr));
        m_draw.drawTexture(texture);
        m_engine->finishPreviewRead(m_slot, buffer, f);
    }
private:
    Engine *m_engine;
    int m_slot;
    GlDraw m_draw;
};

PreviewWorkspace::PreviewWorkspace(Engine *engine, MappingView *mapping, QWidget *parent)
    : QWidget(parent), m_engine(engine), m_mapping(mapping)
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(0, 0, 0, 0);
    auto *count = new QComboBox;
    count->setObjectName(QStringLiteral("previewLayout"));
    count->addItems({QStringLiteral("1 view"), QStringLiteral("2 views"), QStringLiteral("3 views"), QStringLiteral("4 views")});
    layout->addWidget(count);
    m_grid = new QGridLayout;
    m_grid->setContentsMargins(0, 0, 0, 0);
    layout->addLayout(m_grid, 1);
    for (int slot = 0; slot < 4; ++slot) {
        auto &cell = m_cells[size_t(slot)];
        cell.widget = new QWidget(this);
        auto *column = new QVBoxLayout(cell.widget);
        column->setContentsMargins(0, 0, 0, 0);
        cell.source = new QToolButton;
        cell.source->setObjectName(QStringLiteral("previewSource%1").arg(slot));
        cell.source->setText(QStringLiteral("Composition"));
        cell.source->setPopupMode(QToolButton::InstantPopup);
        auto *menu = new QMenu(cell.source);
        cell.source->setMenu(menu);
        connect(menu, &QMenu::aboutToShow, this, [this, slot, menu] {
            menu->clear();
            auto *composition = menu->addAction(QStringLiteral("Composition"));
            connect(composition, &QAction::triggered, this, [this, slot] { selectSource(slot, 0, QStringLiteral("Composition")); });
            auto *layers = menu->addMenu(QStringLiteral("Layers"));
            auto *viewports = menu->addMenu(QStringLiteral("Viewports"));
            Engine::Lock lock(&m_engine->mutex());
            for (int i = 0; i < m_engine->layerCount(); ++i) {
                const auto *layer = m_engine->layer(i);
                if (!layer) continue;
                const quint64 id = layer->id;
                const QString label = layer->name;
                auto *action = (layer->isViewport ? viewports : layers)->addAction(label);
                connect(action, &QAction::triggered, this, [this, slot, id, label] { selectSource(slot, id, label); });
            }
        });
        column->addWidget(cell.source);
        cell.pages = new QStackedWidget;
        cell.picture = new PreviewPicture(engine, slot);
        cell.pages->addWidget(cell.picture);
        if (slot == 0) cell.pages->addWidget(mapping);
        column->addWidget(cell.pages, 1);
    }
    connect(count, &QComboBox::currentIndexChanged, this, [this](int index) { setCount(index + 1); });
    selectSource(0, 0, QStringLiteral("Composition"));
    setCount(1);
}

PreviewWorkspace::~PreviewWorkspace()
{
    for (int slot = 0; slot < 4; ++slot) m_engine->setPreviewSource(slot, 0);
}

void PreviewWorkspace::selectSource(int slot, quint64 id, const QString &label)
{
    auto &cell = m_cells[size_t(slot)];
    cell.id = id;
    cell.source->setText(label);
    cell.pages->setCurrentWidget(slot == 0 && !id ? static_cast<QWidget *>(m_mapping) : cell.picture);
    m_engine->setPreviewSource(slot, id);
    cell.picture->update();
}

void PreviewWorkspace::setCount(int count)
{
    m_count = std::clamp(count, 1, 4);
    for (int slot = 0; slot < 4; ++slot) {
        auto &cell = m_cells[size_t(slot)];
        m_grid->removeWidget(cell.widget);
        cell.widget->setVisible(slot < m_count);
        if (slot < m_count) m_grid->addWidget(cell.widget, slot / 2, slot % 2, 1, m_count == 3 && slot == 2 ? 2 : 1);
        m_engine->setPreviewSource(slot, slot < m_count ? cell.id : 0);
    }
}

void PreviewWorkspace::refreshFrames()
{
    for (int slot = 0; slot < m_count; ++slot) m_cells[size_t(slot)].picture->update();
}
