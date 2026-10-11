#include "MaskPanel.h"
#include "Commands.h"
#include "Engine.h"
#include "Widgets.h"

#include <QComboBox>
#include <QFile>
#include <QFileDialog>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QSignalBlocker>
#include <QToolButton>
#include <QUndoStack>
#include <QVBoxLayout>

namespace {

const QColor kMaskColor(41, 199, 224); // the masks' color, here and in the preview

// A tool's icon: its shape, with a + at the bottom right
QIcon toolIcon(Mask::Shape shape)
{
    const int s = 40; // drawn at twice the size shown
    QPixmap pm(s, s);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    QPen pen(QColor(225, 225, 228), 3);
    p.setPen(pen);
    p.setBrush(Qt::NoBrush);
    const QRectF r(4, 6, 24, 22);
    switch (shape) {
    case Mask::Shape::Path:
        p.drawLine(QPointF(r.left(), r.bottom()), QPointF(r.right(), r.top()));
        p.fillRect(QRectF(r.right() - 3, r.top() - 3, 6, 6), QColor(225, 225, 228));
        break;
    case Mask::Shape::Rectangle: p.drawRect(r); break;
    case Mask::Shape::Triangle: {
        QPainterPath t;
        t.moveTo(r.center().x(), r.top());
        t.lineTo(r.bottomRight());
        t.lineTo(r.bottomLeft());
        t.closeSubpath();
        p.drawPath(t);
        break;
    }
    case Mask::Shape::Ellipse: p.drawEllipse(r); break;
    }
    pen.setWidth(4);
    p.setPen(pen);
    p.drawLine(QPointF(30, 33), QPointF(38, 33));
    p.drawLine(QPointF(34, 29), QPointF(34, 37));
    p.end();
    return QIcon(pm);
}

QIcon swatch()
{
    QPixmap pm(28, 28);
    pm.fill(Qt::transparent);
    QPainter p(&pm);
    p.setRenderHint(QPainter::Antialiasing);
    p.setPen(Qt::NoPen);
    p.setBrush(kMaskColor);
    p.drawRoundedRect(QRectF(3, 3, 22, 22), 4, 4);
    return QIcon(pm);
}

// The list: Delete removes the selected mask
class MaskList : public QListWidget
{
public:
    std::function<void()> onDelete;

protected:
    void keyPressEvent(QKeyEvent *e) override
    {
        if ((e->key() == Qt::Key_Delete || e->key() == Qt::Key_Backspace) && onDelete && state() != EditingState) {
            onDelete();
            return;
        }
        QListWidget::keyPressEvent(e);
    }
};

} // namespace

MaskPanel::MaskPanel(Engine *engine, QUndoStack *undo, quint64 layer, bool viewport, bool locked, QWidget *parent)
    : QWidget(parent), m_engine(engine), m_undo(undo), m_layer(layer), m_viewport(viewport), m_locked(locked)
{
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(0, 0, 0, 0);
    v->setSpacing(8);
    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    v->addLayout(form);

    // Tools
    auto *tools = new QHBoxLayout;
    tools->setSpacing(4);
    struct Tool {
        Mask::Shape shape;
        QString tip;
    };
    const Tool kinds[] = {{Mask::Shape::Path, QStringLiteral("Draw a mask in the preview: click its points (drag to curve), "
                                                            "click the first point, double-click or press Enter to close it")},
                         {Mask::Shape::Rectangle, QStringLiteral("Add a rectangle")},
                         {Mask::Shape::Triangle, QStringLiteral("Add a triangle")},
                         {Mask::Shape::Ellipse, QStringLiteral("Add an ellipse")}};
    for (const Tool &t : kinds) {
        auto *b = new QToolButton;
        b->setIcon(toolIcon(t.shape));
        b->setIconSize(QSize(20, 20));
        b->setToolTip(t.tip);
        b->setAutoRaise(true);
        b->setEnabled(!locked);
        tools->addWidget(b);
        const Mask::Shape shape = t.shape;
        connect(b, &QToolButton::clicked, this, [this, shape] {
            const QRectF box(0.25, 0.25, 0.5, 0.5);
            switch (shape) {
            case Mask::Shape::Path: emit drawRequested(); break;
            case Mask::Shape::Rectangle: add(Mask::rectangle(box)); break;
            case Mask::Shape::Triangle: add(Mask::triangle(box)); break;
            case Mask::Shape::Ellipse: add(Mask::ellipse(box)); break;
            }
        });
    }
    tools->addStretch();
    form->addRow(QStringLiteral("Tools"), tools);

    // The masks
    auto *list = new MaskList;
    list->onDelete = [this] { remove(); };
    m_list = list;
    m_list->setMinimumHeight(110);
    m_list->setMaximumHeight(170);
    m_list->setIconSize(QSize(14, 14));
    m_list->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_list->setToolTip(QStringLiteral("Its masks: the ones not inverted keep the picture inside them, the inverted ones "
                                      "cut it out · double-click to rename · the box turns one off · Delete removes it"));
    m_list->setStyleSheet(QStringLiteral("QListWidget::item:selected { background: #0b5c69; color: white; }"));
    auto *listCol = new QVBoxLayout;
    listCol->setSpacing(4);
    listCol->addWidget(m_list);
    m_remove = new QPushButton(QStringLiteral("Delete Mask"));
    m_remove->setEnabled(false);
    listCol->addWidget(m_remove);
    form->addRow(QStringLiteral("Layers"), listCol);
    m_empty = new QLabel(QStringLiteral("No mask: add one with a tool above."));
    m_empty->setStyleSheet("color:#888;");
    form->addRow(QString(), m_empty);

    // The selected mask
    m_settings = new QWidget;
    auto *sf = new QFormLayout(m_settings);
    sf->setContentsMargins(0, 0, 0, 0);
    sf->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    m_invert = new QPushButton(QStringLiteral("Invert Mask"));
    m_invert->setCheckable(true);
    m_invert->setToolTip(QStringLiteral("Inverted: the mask cuts the picture out instead of keeping it"));
    m_invert->setStyleSheet(QStringLiteral("QPushButton:checked { background:#0b5c69; color:white; }"));
    sf->addRow(QString(), m_invert);
    m_feather = new SliderField;
    m_feather->setRange(0, 100);
    m_feather->setDecimals(0);
    m_feather->setSuffix(QStringLiteral(" %"));
    m_feather->setTicks(4);
    m_feather->setToolTip(QStringLiteral("Width of the soft edge, as a part of the picture's short side"));
    sf->addRow(QStringLiteral("Feathering"), m_feather);
    m_mode = new QComboBox;
    m_mode->addItems({QStringLiteral("Centered"), QStringLiteral("Inner"), QStringLiteral("Outer")});
    m_mode->setToolTip(QStringLiteral("Where the soft edge goes: across the shape's edge, inside it, or outside it"));
    sf->addRow(QString(), m_mode);
    m_attach = new QComboBox;
    m_attach->addItem(QStringLiteral("Surface"));
    m_attach->addItem(QStringLiteral("Composition"));
    m_attach->setToolTip(QStringLiteral("Surface: drawn on its picture, it follows its mapping (warp included) · "
                                        "Composition: fixed on the composition (in a group: on the group's canvas), the "
                                        "picture is cut where it lands"));
    sf->addRow(QStringLiteral("Attach To"), m_attach);
    if (m_viewport) { // a viewport's masks are in its own picture
        m_attach->setVisible(false);
        sf->labelForField(m_attach)->setVisible(false);
    }
    v->addWidget(m_settings);

    auto *importBtn = new QPushButton(QStringLiteral("Import SVG"));
    auto *exportBtn = new QPushButton(QStringLiteral("Export SVG"));
    importBtn->setToolTip(QStringLiteral("Adds the closed shapes of an SVG file as masks (path, rect, circle, ellipse, polygon), "
                                         "the SVG's page over the picture"));
    exportBtn->setToolTip(QStringLiteral("Saves the masks as SVG paths over the picture's size"));
    importBtn->setEnabled(!locked);
    v->addWidget(importBtn);
    v->addWidget(exportBtn);
    v->addStretch();

    for (QWidget *w : {static_cast<QWidget *>(m_invert), static_cast<QWidget *>(m_feather), static_cast<QWidget *>(m_mode),
                       static_cast<QWidget *>(m_attach)})
        w->setEnabled(!locked);

    connect(m_list, &QListWidget::currentRowChanged, this, [this](int row) {
        if (m_syncing) return;
        showCurrent();
        emit currentChanged(row);
    });
    connect(m_list, &QListWidget::itemChanged, this, [this](QListWidgetItem *it) {
        if (m_syncing || m_locked) return;
        const int row = m_list->row(it);
        const QString name = it->text().trimmed();
        const bool on = it->checkState() == Qt::Checked;
        std::vector<Mask> before = masks(), after = before;
        if (row < 0 || row >= int(after.size())) return;
        if (!name.isEmpty()) after[size_t(row)].name = name;
        after[size_t(row)].enabled = on;
        if (after == before) return;
        m_undo->push(new cmd::SetMasks(m_engine, m_layer, before, after,
                                       before[size_t(row)].enabled != on ? QStringLiteral("Turn Mask On / Off")
                                                                         : QStringLiteral("Rename Mask")));
        emit edited();
    });
    connect(m_remove, &QPushButton::clicked, this, &MaskPanel::remove);
    connect(m_invert, &QPushButton::toggled, this, [this](bool on) {
        if (!m_syncing) change([on](Mask &k) { k.invert = on; }, QStringLiteral("Invert Mask"));
    });
    connect(m_feather, &SliderField::valueEdited, this, [this](double pc) {
        if (!m_syncing) changeLive([pc](Mask &k) { k.feather = pc / 100.0; });
    });
    connect(m_feather, &SliderField::editingFinished, this, [this](double) { endLive(QStringLiteral("Change Feathering")); });
    connect(m_mode, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        change([i](Mask &k) { k.featherMode = Mask::Feather(i); }, QStringLiteral("Change Feathering"));
    });
    connect(m_attach, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        change([i](Mask &k) { k.attach = Mask::Attach(i); }, QStringLiteral("Attach Mask"));
    });
    connect(importBtn, &QPushButton::clicked, this, &MaskPanel::importSvg);
    connect(exportBtn, &QPushButton::clicked, this, &MaskPanel::exportSvg);
    refresh();
}

std::vector<Mask> MaskPanel::masks() const { return m_engine->layerMasks(m_layer); }

int MaskPanel::current() const { return m_list->currentRow(); }

void MaskPanel::select(int index)
{
    if (index == m_list->currentRow()) return;
    {
        const QSignalBlocker b(m_list);
        m_list->setCurrentRow(index);
    }
    showCurrent();
}

void MaskPanel::refresh()
{
    const std::vector<Mask> m = masks();
    const int keep = m_list->currentRow();
    m_syncing = true;
    if (m_list->count() != int(m.size())) {
        m_list->clear();
        for (const Mask &k : m) {
            auto *it = new QListWidgetItem(swatch(), k.name);
            it->setFlags(it->flags() | Qt::ItemIsEditable | Qt::ItemIsUserCheckable);
            m_list->addItem(it);
        }
    }
    for (int i = 0; i < int(m.size()); ++i) {
        QListWidgetItem *it = m_list->item(i);
        if (it->text() != m[size_t(i)].name) it->setText(m[size_t(i)].name);
        const Qt::CheckState st = m[size_t(i)].enabled ? Qt::Checked : Qt::Unchecked;
        if (it->checkState() != st) it->setCheckState(st);
        if (m_locked) it->setFlags(it->flags() & ~(Qt::ItemIsEditable | Qt::ItemIsUserCheckable));
    }
    m_list->setCurrentRow(keep >= int(m.size()) ? int(m.size()) - 1 : keep);
    m_syncing = false;
    m_empty->setVisible(m.empty());
    showCurrent();
}

void MaskPanel::showCurrent()
{
    const std::vector<Mask> m = masks();
    const int i = m_list->currentRow();
    const bool has = i >= 0 && i < int(m.size());
    m_settings->setVisible(has);
    m_remove->setEnabled(has && !m_locked);
    if (!has) return;
    const Mask &k = m[size_t(i)];
    m_syncing = true;
    m_invert->setChecked(k.invert);
    m_feather->setValue(k.feather * 100.0);
    m_mode->setCurrentIndex(int(k.featherMode));
    m_attach->setCurrentIndex(int(k.attach));
    m_syncing = false;
}

void MaskPanel::add(Mask m)
{
    if (m_locked) return;
    std::vector<Mask> before = masks(), after = before;
    m.name = masks::uniqueName(after);
    after.push_back(m);
    m_undo->push(new cmd::SetMasks(m_engine, m_layer, before, after, QStringLiteral("Add Mask")));
    refresh();
    select(int(after.size()) - 1);
    emit currentChanged(int(after.size()) - 1);
    emit edited();
}

void MaskPanel::change(const std::function<void(Mask &)> &f, const QString &text)
{
    const int i = m_list->currentRow();
    std::vector<Mask> before = masks(), after = before;
    if (m_locked || i < 0 || i >= int(after.size())) return;
    f(after[size_t(i)]);
    if (after == before) return;
    m_undo->push(new cmd::SetMasks(m_engine, m_layer, before, after, text));
    emit edited();
}

void MaskPanel::changeLive(const std::function<void(Mask &)> &f)
{
    const int i = m_list->currentRow();
    std::vector<Mask> now = masks();
    if (m_locked || i < 0 || i >= int(now.size())) return;
    if (!m_live) {
        m_liveBefore = now;
        m_live = true;
    }
    f(now[size_t(i)]);
    m_engine->setLayerMasks(m_layer, now);
    emit edited();
}

void MaskPanel::endLive(const QString &text)
{
    if (!m_live) return;
    m_live = false;
    const std::vector<Mask> after = masks();
    if (after != m_liveBefore) m_undo->push(new cmd::SetMasks(m_engine, m_layer, m_liveBefore, after, text));
}

void MaskPanel::remove()
{
    const int i = m_list->currentRow();
    std::vector<Mask> before = masks(), after = before;
    if (m_locked || i < 0 || i >= int(after.size())) return;
    after.erase(after.begin() + i);
    m_undo->push(new cmd::SetMasks(m_engine, m_layer, before, after, QStringLiteral("Delete Mask")));
    refresh();
    emit currentChanged(m_list->currentRow());
    emit edited();
}

void MaskPanel::importSvg()
{
    if (m_locked) return;
    const QString path = QFileDialog::getOpenFileName(this, QStringLiteral("Import SVG Masks"), QString(),
                                                      QStringLiteral("SVG (*.svg)"));
    if (path.isEmpty()) return;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) {
        QMessageBox::warning(this, QStringLiteral("Import SVG"), QStringLiteral("Cannot read %1").arg(path));
        return;
    }
    QString note;
    std::vector<Mask> got = masks::fromSvg(file.readAll(), &note);
    if (got.empty()) {
        QMessageBox::warning(this, QStringLiteral("Import SVG"), note);
        return;
    }
    std::vector<Mask> before = masks(), after = before;
    for (Mask &k : got) {
        if (m_viewport) k.attach = Mask::Attach::Surface;
        const bool taken = k.name.isEmpty() || std::any_of(after.begin(), after.end(), [&](const Mask &o) { return o.name == k.name; });
        if (taken) k.name = masks::uniqueName(after);
        after.push_back(k);
    }
    m_undo->push(new cmd::SetMasks(m_engine, m_layer, before, after, QStringLiteral("Import SVG Masks")));
    refresh();
    select(int(after.size()) - 1);
    emit currentChanged(int(after.size()) - 1);
    emit edited();
    if (!note.isEmpty()) QMessageBox::information(this, QStringLiteral("Import SVG"), QStringLiteral("Imported; ") + note + '.');
}

void MaskPanel::exportSvg()
{
    const std::vector<Mask> m = masks();
    if (m.empty()) {
        QMessageBox::information(this, QStringLiteral("Export SVG"), QStringLiteral("No mask to export."));
        return;
    }
    QString path = QFileDialog::getSaveFileName(this, QStringLiteral("Export SVG Masks"), QStringLiteral("masks.svg"),
                                                QStringLiteral("SVG (*.svg)"));
    if (path.isEmpty()) return;
    if (!path.endsWith(QLatin1String(".svg"), Qt::CaseInsensitive)) path += QStringLiteral(".svg");
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        QMessageBox::warning(this, QStringLiteral("Export SVG"), QStringLiteral("Cannot write %1").arg(path));
        return;
    }
    file.write(masks::toSvg(m, m_engine->maskFrame(m_layer)).toUtf8());
}
