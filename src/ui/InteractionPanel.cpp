#include "InteractionPanel.h"
#include "AnimEditor.h"
#include "PresetEditor.h"
#include "Osc.h"
#include "Engine.h"
#include "Widgets.h"

#include <QApplication>
#include <QClipboard>
#include <QComboBox>
#include <QDialog>
#include <QDialogButtonBox>
#include <QListWidget>
#include <QContextMenuEvent>
#include <QDateTime>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QJsonDocument>
#include <QKeySequenceEdit>
#include <QLabel>
#include <QLineEdit>
#include <QMenu>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QScrollArea>
#include <QScrollBar>
#include <QSplitter>
#include <QTabBar>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QUndoCommand>
#include <QUndoStack>
#include <QVBoxLayout>
#include <cmath>

// ---------------------------------------------------------------------------
// Undo: the whole list before and after (edits of one drag merge into one step)
// ---------------------------------------------------------------------------
namespace {

class SetInteractions : public QUndoCommand
{
public:
    SetInteractions(Engine *e, std::vector<Interaction> before, std::vector<Interaction> after, const QString &text,
                    const QString &mergeKey)
        : QUndoCommand(text), m_e(e), m_before(std::move(before)), m_after(std::move(after)), m_key(mergeKey),
          m_time(QDateTime::currentMSecsSinceEpoch())
    {
    }
    void undo() override { m_e->setInteractions(m_before); }
    void redo() override { m_e->setInteractions(m_after); }
    int id() const override { return m_key.isEmpty() ? -1 : 0x1A7E; }
    bool mergeWith(const QUndoCommand *other) override
    {
        auto *o = static_cast<const SetInteractions *>(other);
        if (o->m_key != m_key || o->m_time - m_time > 1000) return false; // a new gesture: a step of its own
        m_after = o->m_after;
        m_time = o->m_time;
        return true;
    }

private:
    Engine *m_e;
    std::vector<Interaction> m_before, m_after;
    QString m_key;
    qint64 m_time;
};

constexpr char kMime[] = "application/x-pinol-interaction-settings";
const InteractionSource kKinds[] = {InteractionSource::Keyboard, InteractionSource::Midi, InteractionSource::Osc,
                                    InteractionSource::Internal};

QLabel *note(const QString &t)
{
    auto *l = new QLabel(t);
    l->setWordWrap(true);
    l->setStyleSheet("color:#888; font-size:11px;");
    return l;
}

QProgressBar *meter()
{
    auto *m = new QProgressBar;
    m->setRange(0, 1000);
    m->setTextVisible(false);
    m->setFixedHeight(6);
    m->setStyleSheet(QStringLiteral("QProgressBar { background:#1b1b1d; border:none; } QProgressBar::chunk { background:%1; }")
                         .arg(theme::css()));
    return m;
}

// The chain alone (not the source, not the target): what Copy / Paste Settings carry
QJsonObject chainJson(const Interaction &it)
{
    QJsonObject o = it.toJson();
    for (const char *k : {"id", "name", "enabled", "source", "key", "port", "channel", "message", "number", "address",
                          "argument", "source_layer", "source_param", "layer", "param"})
        o.remove(QLatin1String(k));
    return o;
}

void applyChain(Interaction &it, const QJsonObject &chain)
{
    QJsonObject o = it.toJson();
    for (auto c = chain.begin(); c != chain.end(); ++c) o[c.key()] = c.value();
    it = Interaction::fromJson(o);
}

} // namespace

// ---------------------------------------------------------------------------
// The panel
// ---------------------------------------------------------------------------
InteractionPanel::InteractionPanel(Engine *engine, QUndoStack *undo, QWidget *parent)
    : QWidget(parent), m_engine(engine), m_undo(undo)
{
    s_instance = this;
    auto *v = new QVBoxLayout(this);
    v->setContentsMargins(4, 4, 4, 4);
    v->setSpacing(6);

    m_kinds = new QTabBar;
    m_kinds->setDrawBase(false);
    m_kinds->setExpanding(false);
    m_kinds->setStyleSheet(theme::tabStyle()); // as the inspector's sub-tabs
    connect(theme::notifier(), &theme::Notifier::changed, m_kinds, [this] { m_kinds->setStyleSheet(theme::tabStyle()); });
    for (InteractionSource k : kKinds) m_kinds->addTab(interactionSourceName(k));
    m_kinds->addTab(QStringLiteral("All"));
    m_kinds->setTabToolTip(3, QStringLiteral("A value of the show driven by another one, with no protocol in between"));
    v->addWidget(m_kinds);

    auto *bar = new QHBoxLayout;
    auto *addButton = new QPushButton(QStringLiteral("+"));
    addButton->setFixedWidth(32);
    addButton->setToolTip(QStringLiteral("A new interaction of this kind"));
    m_remove = new QPushButton(QStringLiteral("−"));
    m_remove->setFixedWidth(32);
    m_remove->setToolTip(QStringLiteral("Remove the selected interactions"));
    m_learn = new QToolButton;
    m_learn->setText(QStringLiteral("Learn"));
    m_learn->setCheckable(true);
    m_learn->setToolTip(QStringLiteral("Move a controller, press a key or send an OSC message: the selected interaction "
                                       "takes it as its source (none selected: a new one is made)"));
    m_search = new QLineEdit;
    m_search->setPlaceholderText(QStringLiteral("Search…"));
    m_search->setClearButtonEnabled(true);
    bar->addWidget(addButton);
    bar->addWidget(m_remove);
    bar->addWidget(m_learn);
    bar->addWidget(m_search, 1);
    v->addLayout(bar);

    m_list = new QTreeWidget;
    m_list->setColumnCount(4);
    m_list->setHeaderLabels({QStringLiteral("Name"), QStringLiteral("Source"), QStringLiteral("Target"), QStringLiteral("Value")});
    m_list->setRootIsDecorated(false);
    m_list->setSelectionMode(QAbstractItemView::ExtendedSelection);
    m_list->header()->setSectionResizeMode(0, QHeaderView::Interactive);
    m_list->header()->setSectionResizeMode(1, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_list->header()->setSectionResizeMode(3, QHeaderView::ResizeToContents);
    m_list->setColumnWidth(0, 110);
    m_list->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed); // its name, in place

    auto *settingsBox = new QWidget;
    auto *sv = new QVBoxLayout(settingsBox);
    sv->setContentsMargins(0, 0, 0, 0);
    auto *head = new QHBoxLayout;
    m_settingsTitle = new QLabel(QStringLiteral("Settings"));
    m_settingsTitle->setStyleSheet("font-weight:bold;");
    auto *copy = new QPushButton(QStringLiteral("Copy Settings"));
    copy->setToolTip(QStringLiteral("Its chain (ranges, toggle, filter, curve, master, pick-up), not its source or target"));
    m_copy = copy;
    m_paste = new QPushButton(QStringLiteral("Paste Settings"));
    m_paste->setToolTip(QStringLiteral("The chain copied, onto the selected interactions (their sources and targets stay)"));
    head->addWidget(m_settingsTitle, 1);
    head->addWidget(copy);
    head->addWidget(m_paste);
    sv->addLayout(head);
    auto *scroll = new QScrollArea;
    scroll->setWidgetResizable(true);
    scroll->setFrameShape(QFrame::NoFrame);
    m_settings = new QWidget;
    m_settingsLayout = new QVBoxLayout(m_settings);
    m_settingsLayout->setContentsMargins(0, 0, 4, 0);
    scroll->setWidget(m_settings);
    m_scroll = scroll;
    sv->addWidget(scroll, 1);

    auto *split = new QSplitter(Qt::Vertical);
    split->addWidget(m_list);
    split->addWidget(settingsBox);
    split->setStretchFactor(0, 1);
    split->setStretchFactor(1, 3);
    split->setSizes({180, 540}); // the settings get the room
    v->addWidget(split, 1);

    connect(m_kinds, &QTabBar::currentChanged, this, [this] { rebuildList(); });
    connect(m_search, &QLineEdit::textChanged, this, [this] { rebuildList(); });
    connect(addButton, &QPushButton::clicked, this, [this, addButton] {
        if (const std::optional<InteractionSource> k = shownKind()) {
            add(*k);
            return;
        }
        QMenu menu;
        for (InteractionSource k : kKinds) menu.addAction(interactionSourceName(k), this, [this, k] { add(k); });
        menu.exec(addButton->mapToGlobal(QPoint(0, addButton->height())));
    });
    connect(m_remove, &QPushButton::clicked, this, &InteractionPanel::removeSelected);
    connect(m_learn, &QToolButton::toggled, this, [this](bool on) {
        m_engine->setInteractionLearn(on, on ? m_learnKind : std::nullopt);
        if (!on) m_learnKind.reset(); // the next Learn: of any kind
    });
    connect(m_list, &QTreeWidget::itemSelectionChanged, this, [this] {
        m_remove->setEnabled(!m_list->selectedItems().isEmpty());
        if (selectedId() != m_shown) rebuildSettings();
    });
    connect(m_list, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int col) {
        if (m_filling || col != 0) return;
        const quint64 id = item->data(0, Qt::UserRole).toULongLong();
        for (Interaction it : m_engine->interactions()) {
            if (it.id == id && !item->text(0).trimmed().isEmpty() && it.name != item->text(0).trimmed()) { // renamed
                it.name = item->text(0).trimmed();
                commit(it, QStringLiteral("Rename Interaction"));
                return;
            }
            if (it.id == id && it.enabled != (item->checkState(0) == Qt::Checked)) {
                it.enabled = item->checkState(0) == Qt::Checked;
                commit(it, it.enabled ? QStringLiteral("Enable Interaction") : QStringLiteral("Disable Interaction"));
            }
        }
    });
    connect(copy, &QPushButton::clicked, this, [this] {
        const std::optional<Interaction> it = selected();
        if (!it) return;
        auto *mime = new QMimeData;
        const QByteArray json = QJsonDocument(chainJson(*it)).toJson(QJsonDocument::Compact);
        mime->setData(kMime, json);
        mime->setText(QString::fromUtf8(json));
        QApplication::clipboard()->setMimeData(mime);
    });
    connect(m_paste, &QPushButton::clicked, this, [this] {
        const QMimeData *mime = QApplication::clipboard()->mimeData();
        if (!mime || !mime->hasFormat(kMime)) return;
        const QJsonObject chain = QJsonDocument::fromJson(mime->data(kMime)).object();
        QSet<quint64> ids;
        for (QTreeWidgetItem *item : m_list->selectedItems()) ids.insert(item->data(0, Qt::UserRole).toULongLong());
        std::vector<Interaction> list = m_engine->interactions();
        for (Interaction &it : list)
            if (ids.contains(it.id)) applyChain(it, chain);
        commitList(list, QStringLiteral("Paste Interaction Settings"));
        rebuildSettings();
    });
    connect(m_engine, &Engine::interactionsChanged, this, [this] {
        rebuildList();
        if (!m_committing) rebuildSettings(); // undone, opened: the settings shown again
    });
    m_valuesTimer = new QTimer(this);
    m_valuesTimer->setSingleShot(true);
    m_valuesTimer->setInterval(120);
    connect(m_valuesTimer, &QTimer::timeout, this, [this] {
        if (QApplication::mouseButtons() != Qt::NoButton) { // not under a drag: once let go
            m_valuesTimer->start();
            return;
        }
        rebuildList();
        rebuildSettings();
    });
    connect(m_engine, &Engine::interactionValuesChanged, this, [this] {
        if (!m_valuesTimer->isActive()) m_valuesTimer->start();
    });
    connect(m_engine, &Engine::layersChanged, this, [this] { // a layer renamed or gone: the targets' names
        QTimer::singleShot(0, this, [this] { rebuildList(); });
    });

    m_timer = new QTimer(this);
    m_timer->setInterval(50);
    connect(m_timer, &QTimer::timeout, this, [this] {
        if (!isVisible()) return;
        refreshLevels();
        pollLearn();
    });
    m_timer->start();
    rebuildList();
    rebuildSettings();
}

std::optional<InteractionSource> InteractionPanel::shownKind() const
{
    const int i = m_kinds->currentIndex();
    if (i >= 0 && i < kInteractionSourceCount) return kKinds[i];
    return std::nullopt;
}

QString InteractionPanel::keyName(int key, Qt::KeyboardModifiers mods)
{
    mods &= Qt::ShiftModifier | Qt::ControlModifier | Qt::AltModifier | Qt::MetaModifier;
    return QKeySequence(QKeyCombination(mods, Qt::Key(key))).toString(QKeySequence::PortableText);
}

void InteractionPanel::keyEvent(int key, Qt::KeyboardModifiers mods, bool pressed)
{
    if (key == 0 || key == Qt::Key_unknown || key == Qt::Key_Shift || key == Qt::Key_Control || key == Qt::Key_Alt ||
        key == Qt::Key_Meta)
        return;
    InteractionEvent e;
    e.source = InteractionSource::Keyboard;
    if (pressed) {
        e.address = keyName(key, mods);
        m_pressed.insert(key, e.address);
    } else {
        e.address = m_pressed.take(key); // let go: the name it was pressed with
        if (e.address.isEmpty()) e.address = keyName(key, mods);
    }
    e.value = pressed ? 1.0 : 0.0;
    m_engine->feedInteraction(e);
}

quint64 InteractionPanel::selectedId() const
{
    const QList<QTreeWidgetItem *> sel = m_list->selectedItems();
    QTreeWidgetItem *item = m_list->currentItem();
    if (!item || !sel.contains(item)) item = sel.isEmpty() ? nullptr : sel.first();
    return item ? item->data(0, Qt::UserRole).toULongLong() : 0;
}

std::optional<Interaction> InteractionPanel::selected() const
{
    const quint64 id = selectedId();
    for (const Interaction &it : m_engine->interactions())
        if (it.id == id) return it;
    return std::nullopt;
}

QString InteractionPanel::targetLabel(quint64 layer, const QString &param) const
{
    if (param.isEmpty()) return QStringLiteral("(no target)");
    QString owner = QStringLiteral("Composition");
    if (layer) {
        Engine::Lock lk(&m_engine->mutex());
        const int i = m_engine->indexOfId(layer);
        const Layer *l = i >= 0 ? m_engine->layer(i) : nullptr;
        owner = l ? l->name : QStringLiteral("(layer gone)");
    }
    ParamInfo info;
    if (m_engine->parameterInfo(layer, param, &info))
        return owner + QStringLiteral(" › ") + info.label;
    // Complete ISF colors are grouped under the parent of their R/G/B/A
    // scalar parameters. Keep old interaction targets readable too.
    if (m_engine->parameterInfo(layer, param + QStringLiteral("/r"), &info)) {
        const QString label = info.label.endsWith(QStringLiteral(" R")) ? info.label.chopped(2) : info.label;
        return owner + QStringLiteral(" › ") + label;
    }
    return owner + QStringLiteral(" › ") + param;
}

QString InteractionPanel::describe(const Interaction &it) const
{
    switch (it.source) {
    case InteractionSource::Keyboard: return it.key.isEmpty() ? QStringLiteral("Key (none)") : QStringLiteral("Key %1").arg(it.key);
    case InteractionSource::Midi: {
        QString s = QStringLiteral("MIDI %1 %2").arg(it.midiChannel ? QStringLiteral("ch %1").arg(it.midiChannel) : QStringLiteral("any ch"),
                                                     midiMessageName(it.midiMessage));
        if (it.midiMessage != MidiMessageKind::PitchBend) s += QStringLiteral(" %1").arg(it.midiNumber);
        if (!it.midiPort.isEmpty()) s += QStringLiteral(" · ") + it.midiPort;
        return s;
    }
    case InteractionSource::Osc:
        return it.oscAddress.isEmpty() ? QStringLiteral("OSC (no address)")
                                       : QStringLiteral("OSC %1").arg(it.oscAddress) + (it.oscArgument ? QStringLiteral(" [%1]").arg(it.oscArgument) : QString());
    case InteractionSource::Internal: return targetLabel(it.sourceLayer, it.sourceParam);
    }
    return QString();
}

void InteractionPanel::rebuildList()
{
    const quint64 keep = selectedId();
    QSet<quint64> keepAll;
    for (QTreeWidgetItem *item : m_list->selectedItems()) keepAll.insert(item->data(0, Qt::UserRole).toULongLong());
    m_filling = true;
    m_list->clear();
    const std::optional<InteractionSource> kind = shownKind();
    const QString search = m_search->text().trimmed();
    QTreeWidgetItem *current = nullptr;
    for (const Interaction &it : m_engine->interactions()) {
        if (kind && it.source != *kind) continue;
        const QString src = describe(it), dst = targetLabel(it.layer, it.param);
        if (!search.isEmpty() && !it.name.contains(search, Qt::CaseInsensitive) && !src.contains(search, Qt::CaseInsensitive) &&
            !dst.contains(search, Qt::CaseInsensitive))
            continue;
        auto *item = new QTreeWidgetItem(m_list, {it.name, kind ? src : interactionSourceName(it.source) + QStringLiteral(" · ") + src, dst, QString()});
        item->setData(0, Qt::UserRole, QVariant::fromValue<quint64>(it.id));
        item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsEditable);
        item->setToolTip(0, QStringLiteral("On / off · its name (double-click to rename): its address in OSC "
                                           "(/interaction/<name>/…), in the presets and the snapshots"));
        item->setCheckState(0, it.enabled ? Qt::Checked : Qt::Unchecked);
        if (keepAll.contains(it.id)) item->setSelected(true);
        if (it.id == keep) current = item;
    }
    if (current) m_list->setCurrentItem(current, 0, QItemSelectionModel::NoUpdate);
    m_filling = false;
    m_remove->setEnabled(!m_list->selectedItems().isEmpty());
    if (selectedId() != m_shown) rebuildSettings();
}

void InteractionPanel::commitList(const std::vector<Interaction> &list, const QString &text)
{
    m_committing = true;
    m_undo->push(new SetInteractions(m_engine, m_engine->interactions(), list, text, QString()));
    m_committing = false;
    emit edited();
}

void InteractionPanel::commit(const Interaction &it, const QString &text, const QString &mergeKey)
{
    std::vector<Interaction> before = m_engine->interactions(), after = before;
    bool found = false;
    for (Interaction &x : after)
        if (x.id == it.id) {
            x = it;
            found = true;
        }
    if (!found) return;
    m_committing = true;
    m_undo->push(new SetInteractions(m_engine, before, after, text, mergeKey.isEmpty() ? QString() : mergeKey + QString::number(it.id)));
    m_committing = false;
    emit edited();
}

void InteractionPanel::add(InteractionSource kind)
{
    std::vector<Interaction> list = m_engine->interactions();
    Interaction it = Interaction::made(kind);
    // An id now, so that it is selected (the same one when redone)
    quint64 id = 1;
    for (const Interaction &x : list) id = std::max(id, x.id + 1);
    it.id = id;
    list.push_back(it);
    commitList(list, QStringLiteral("Add Interaction"));
    if (shownKind() && *shownKind() != kind) m_kinds->setCurrentIndex(int(kind));
    rebuildList();
    for (int i = 0; i < m_list->topLevelItemCount(); ++i)
        if (m_list->topLevelItem(i)->data(0, Qt::UserRole).toULongLong() == id) {
            m_list->clearSelection();
            m_list->setCurrentItem(m_list->topLevelItem(i));
        }
}

void InteractionPanel::removeSelected()
{
    QSet<quint64> ids;
    for (QTreeWidgetItem *item : m_list->selectedItems()) ids.insert(item->data(0, Qt::UserRole).toULongLong());
    if (ids.isEmpty()) return;
    std::vector<Interaction> list = m_engine->interactions();
    list.erase(std::remove_if(list.begin(), list.end(), [&](const Interaction &x) { return ids.contains(x.id); }), list.end());
    commitList(list, ids.size() > 1 ? QStringLiteral("Remove Interactions") : QStringLiteral("Remove Interaction"));
}

void InteractionPanel::refreshLevels()
{
    for (int i = 0; i < m_list->topLevelItemCount(); ++i) {
        QTreeWidgetItem *item = m_list->topLevelItem(i);
        double level = 0, out = 0;
        const bool has = m_engine->interactionLevels(item->data(0, Qt::UserRole).toULongLong(), &level, &out);
        const QString text = has ? QStringLiteral("%1 %").arg(std::lround(out * 100)) : QStringLiteral("—");
        if (item->text(3) != text) {
            m_filling = true;
            item->setText(3, text);
            m_filling = false;
        }
    }
    double level = 0, out = 0, curveIn = 0;
    const bool has = m_shown && m_engine->interactionLevels(m_shown, &level, &out, &curveIn);
    if (m_lane) m_lane->setPlayhead(curveIn, has);
    if (m_inMeter) m_inMeter->setValue(has ? int(level * 1000) : 0);
    if (m_outMeter) m_outMeter->setValue(has ? int(out * 1000) : 0);
    if (m_copy) m_copy->setEnabled(m_shown != 0);
    if (m_paste) m_paste->setEnabled(QApplication::clipboard()->mimeData() && QApplication::clipboard()->mimeData()->hasFormat(kMime) &&
                                     !m_list->selectedItems().isEmpty());
}

void InteractionPanel::pollLearn()
{
    if (!m_learn->isChecked()) return;
    std::vector<InteractionEvent> heard = m_engine->takeLearnedInteractions();
    if (heard.empty()) return;
    m_learn->setChecked(false); // learning stops: what came in is chosen from
    InteractionEvent chosen = heard.front();
    if (heard.size() > 1) { // several moved together: which one?
        QDialog dialog(this);
        dialog.setWindowTitle(QStringLiteral("Learn"));
        auto *v = new QVBoxLayout(&dialog);
        v->addWidget(new QLabel(QStringLiteral("Several sources came in at the same time. Which one?")));
        auto *list = new QListWidget;
        for (const InteractionEvent &e : heard) list->addItem(e.describe() + (e.port.isEmpty() ? QString() : QStringLiteral(" · ") + e.port));
        list->setCurrentRow(0);
        v->addWidget(list);
        auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
        v->addWidget(buttons);
        connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
        connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
        connect(list, &QListWidget::itemDoubleClicked, &dialog, &QDialog::accept);
        if (dialog.exec() != QDialog::Accepted || list->currentRow() < 0) return;
        chosen = heard[size_t(list->currentRow())];
    }
    applyLearned(chosen);
}

void InteractionPanel::applyLearned(const InteractionEvent &ev)
{
    const InteractionEvent *e = &ev;
    std::optional<Interaction> sel = selected();
    Interaction it = sel ? *sel : Interaction::made(e->source);
    if (sel && sel->source != e->source) it.takeover = Interaction::made(e->source).takeover; // that kind's
    it.source = e->source;
    switch (e->source) {
    case InteractionSource::Keyboard: it.key = e->address; break;
    case InteractionSource::Midi:
        it.midiPort = e->port;
        it.midiChannel = e->channel;
        it.midiMessage = e->message;
        it.midiNumber = e->number;
        break;
    case InteractionSource::Osc:
        it.oscAddress = e->address;
        it.oscArgument = e->argument;
        break;
    case InteractionSource::Internal: break;
    }
    if (sel) {
        commit(it, QStringLiteral("Learn Interaction Source"));
        if (shownKind() && *shownKind() != it.source) m_kinds->setCurrentIndex(int(it.source));
    } else {
        std::vector<Interaction> list = m_engine->interactions();
        quint64 id = 1;
        for (const Interaction &x : list) id = std::max(id, x.id + 1);
        it.id = id;
        list.push_back(it);
        commitList(list, QStringLiteral("Learn Interaction"));
        if (shownKind() && *shownKind() != it.source) m_kinds->setCurrentIndex(int(it.source));
        select(id);
    }
    rebuildList();
    select(it.id);
    rebuildSettings();
}

void InteractionPanel::select(quint64 id)
{
    for (int i = 0; i < m_list->topLevelItemCount(); ++i)
        if (m_list->topLevelItem(i)->data(0, Qt::UserRole).toULongLong() == id) {
            m_list->clearSelection();
            m_list->setCurrentItem(m_list->topLevelItem(i));
        }
}

// From a value anywhere (its right-click): a new interaction driving it, its source learnt (of that kind), or chosen
// in its settings (Internal)
void InteractionPanel::learnFor(quint64 layer, const QString &path, InteractionSource kind)
{
    std::vector<Interaction> list = m_engine->interactions();
    Interaction it = Interaction::made(kind);
    quint64 id = 1;
    for (const Interaction &x : list) id = std::max(id, x.id + 1);
    it.id = id;
    it.layer = layer;
    it.param = path;
    ParamInfo info;
    if (m_engine->parameterInfo(layer, path, &info) && info.isNumber()) { // its whole range
        it.dstMin = info.min;
        it.dstMax = info.max;
    }
    list.push_back(it);
    commitList(list, QStringLiteral("New Interaction"));
    emit showRequested();
    m_kinds->setCurrentIndex(int(kind));
    rebuildList();
    select(id);
    rebuildSettings();
    if (kind != InteractionSource::Internal) {
        m_learnKind = kind;
        m_learn->setChecked(true); // the next source of that kind becomes its
    }
}

InteractionPanel *InteractionPanel::s_instance = nullptr;

void InteractionPanel::fillValueMenu(QMenu *menu, Engine *e, quint64 layer, const QStringList &paths)
{
    if (paths.isEmpty()) return;
    auto one = [e, layer](QMenu *into, const QString &path) {
        const QString addr = OscNamespace::addressOf(e, layer, path);
        if (!addr.isEmpty())
            into->addAction(QStringLiteral("Copy OSC Address  %1").arg(addr), into, [addr] { QApplication::clipboard()->setText(addr); });
        InteractionPanel *panel = instance();
        if (!panel) return;
        QMenu *learn = into->addMenu(QStringLiteral("Learn Interaction"));
        for (InteractionSource k : kKinds)
            learn->addAction(interactionSourceName(k), panel, [panel, layer, path, k] { panel->learnFor(layer, path, k); });
    };
    if (!menu->isEmpty()) menu->addSeparator();
    if (paths.size() == 1) {
        one(menu, paths.front());
        return;
    }
    for (const QString &path : paths) { // a sub-menu for each number (x, y…)
        ParamInfo info;
        const QString name = e->parameterInfo(layer, path, &info) ? info.name() : path.section('/', -1);
        one(menu->addMenu(name), path);
    }
}

// ---------------------------------------------------------------------------
// The settings of the one selected
// ---------------------------------------------------------------------------
void InteractionPanel::attachPreset(std::initializer_list<QWidget *> ws, const QStringList &paths)
{
    PresetEditor *pe = PresetEditor::instance();
    if (!pe || !m_shown) return;
    QStringList full;
    for (const QString &p : paths) full << QStringLiteral("interaction/%1/%2").arg(m_shown).arg(p);
    for (QWidget *w : ws) {
        if (!w) continue;
        pe->track(w, 0, full); // Edit mode: underlined, its name puts it in the preset or takes it out
        w->setProperty("presetPaths", full);
        w->installEventFilter(this);
        for (QWidget *c : w->findChildren<QWidget *>()) c->installEventFilter(this);
    }
}

bool InteractionPanel::eventFilter(QObject *o, QEvent *e)
{
    if (e->type() != QEvent::ContextMenu) return QWidget::eventFilter(o, e);
    auto *w = qobject_cast<QWidget *>(o);
    while (w && !w->property("presetPaths").isValid()) w = w->parentWidget();
    PresetEditor *pe = PresetEditor::instance();
    if (!w || !pe) return QWidget::eventFilter(o, e);
    auto *menu = new QMenu(w);
    menu->setAttribute(Qt::WA_DeleteOnClose);
    pe->fillMenu(menu, 0, w->property("presetPaths").toStringList()); // Add to New Preset / to Selected Preset
    fillValueMenu(menu, m_engine, 0, w->property("presetPaths").toStringList());
    if (menu->isEmpty()) {
        delete menu;
        return QWidget::eventFilter(o, e);
    }
    menu->popup(static_cast<QContextMenuEvent *>(e)->globalPos());
    return true;
}

void InteractionPanel::rebuildSettings()
{
    const std::optional<Interaction> cur = selected();
    m_shown = cur ? cur->id : 0;
    m_inMeter = m_outMeter = nullptr;
    m_lane = nullptr;
    while (QLayoutItem *item = m_settingsLayout->takeAt(0)) {
        if (QWidget *w = item->widget()) w->deleteLater();
        delete item;
    }
    if (!cur) {
        m_settingsTitle->setText(QStringLiteral("Settings"));
        m_settingsLayout->addWidget(note(QStringLiteral("Add an interaction (+), or press Learn and move a controller, "
                                                        "press a key or send an OSC message.")));
        m_settingsLayout->addStretch();
        return;
    }
    const Interaction it = *cur;
    m_settingsTitle->setText(QStringLiteral("Settings · %1").arg(interactionSourceName(it.source)));
    const quint64 id = it.id;
    // An edit of this one: read again as it is now (another edit may have come), changed, committed
    auto edit = [this, id](const QString &text, const std::function<void(Interaction &)> &fn, const QString &merge = QString()) {
        for (Interaction x : m_engine->interactions())
            if (x.id == id) {
                fn(x);
                commit(x, text, merge);
            }
    };

    auto *form = new QFormLayout;
    form->setLabelAlignment(Qt::AlignRight | Qt::AlignVCenter);
    form->setFieldGrowthPolicy(QFormLayout::AllNonFixedFieldsGrow);
    auto *formBox = new QWidget;
    formBox->setLayout(form);
    m_settingsLayout->addWidget(formBox);

    auto *name = new QLineEdit(it.name);
    {
        Interaction unnamed = it;
        unnamed.name.clear();
        name->setPlaceholderText(describe(unnamed));
    }
    form->addRow(QStringLiteral("Name"), name);
    attachPreset({name}, {QStringLiteral("name")});
    attachPreset({m_settingsTitle}, {QStringLiteral("source/kind")});
    connect(name, &QLineEdit::editingFinished, this, [edit, name] {
        edit(QStringLiteral("Rename Interaction"), [name](Interaction &x) { x.name = name->text().trimmed(); });
    });

    // The source
    double srcLo = 0, srcHi = 1; // the bar of the source range
    switch (it.source) {
    case InteractionSource::Keyboard: {
        auto *key = new QKeySequenceEdit(QKeySequence::fromString(it.key, QKeySequence::PortableText));
#if QT_VERSION >= QT_VERSION_CHECK(6, 5, 0)
        key->setMaximumSequenceLength(1);
#endif
        key->setToolTip(QStringLiteral("The key (with its modifiers): pressed 1, let go 0"));
        form->addRow(QStringLiteral("Key"), key);
        attachPreset({key}, {QStringLiteral("source/key")});
        connect(key, &QKeySequenceEdit::editingFinished, this, [edit, key] {
            const QKeySequence seq = key->keySequence();
            const QString k = seq.isEmpty() ? QString() : QKeySequence(seq[0]).toString(QKeySequence::PortableText); // one key
            edit(QStringLiteral("Change Interaction Key"), [k](Interaction &x) { x.key = k; });
        });
        break;
    }
    case InteractionSource::Midi: {
        auto *port = new QComboBox;
        port->addItem(QStringLiteral("Any input"), QString());
        for (const QString &p : Engine::midiInputPorts()) port->addItem(p, p);
        int at = port->findData(it.midiPort);
        if (at < 0) {
            port->addItem(QStringLiteral("%1 (not found)").arg(it.midiPort), it.midiPort);
            at = port->count() - 1;
        }
        port->setCurrentIndex(at);
        port->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
        port->setMinimumContentsLength(8);
        form->addRow(QStringLiteral("Input"), port);
        attachPreset({port}, {QStringLiteral("source/midi/input")});
        auto *row = new QHBoxLayout;
        auto *channel = new QComboBox;
        channel->addItem(QStringLiteral("Any channel"), 0);
        for (int c = 1; c <= 16; ++c) channel->addItem(QStringLiteral("Channel %1").arg(c), c);
        channel->setCurrentIndex(channel->findData(it.midiChannel));
        auto *message = new QComboBox;
        for (int k = 0; k < 4; ++k) message->addItem(midiMessageName(MidiMessageKind(k)), k);
        message->setCurrentIndex(int(it.midiMessage));
        auto *number = new IntBox;
        number->setRange(0, 127);
        number->setValue(it.midiNumber);
        number->setEnabled(it.midiMessage != MidiMessageKind::PitchBend);
        number->setToolTip(QStringLiteral("The controller, the note or the program"));
        row->addWidget(channel, 1);
        row->addWidget(message, 1);
        row->addWidget(number);
        form->addRow(QStringLiteral("Message"), row);
        attachPreset({channel}, {QStringLiteral("source/midi/channel")});
        attachPreset({message}, {QStringLiteral("source/midi/message")});
        attachPreset({number}, {QStringLiteral("source/midi/number")});
        connect(port, qOverload<int>(&QComboBox::activated), this, [edit, port](int) {
            const QString p = port->currentData().toString();
            edit(QStringLiteral("Change Interaction Input"), [p](Interaction &x) { x.midiPort = p; });
        });
        connect(channel, qOverload<int>(&QComboBox::activated), this, [edit, channel](int) {
            const int c = channel->currentData().toInt();
            edit(QStringLiteral("Change Interaction Channel"), [c](Interaction &x) { x.midiChannel = c; });
        });
        connect(message, qOverload<int>(&QComboBox::activated), this, [this, edit](int k) {
            edit(QStringLiteral("Change Interaction Message"), [k](Interaction &x) { x.midiMessage = MidiMessageKind(k); });
            rebuildSettings();
        });
        connect(number, qOverload<int>(&QSpinBox::valueChanged), this, [edit](int n) {
            edit(QStringLiteral("Change Interaction Number"), [n](Interaction &x) { x.midiNumber = n; }, QStringLiteral("midinumber"));
        });
        break;
    }
    case InteractionSource::Osc: {
        auto *address = new QLineEdit(it.oscAddress);
        address->setPlaceholderText(QStringLiteral("/fader/1"));
        auto *argument = new IntBox;
        argument->setRange(0, 63);
        argument->setValue(it.oscArgument);
        argument->setPrefix(QStringLiteral("arg "));
        argument->setToolTip(QStringLiteral("Which value of the message (the first: 0)"));
        auto *row = new QHBoxLayout;
        row->addWidget(address, 1);
        row->addWidget(argument);
        form->addRow(QStringLiteral("Address"), row);
        attachPreset({address}, {QStringLiteral("source/osc/address")});
        attachPreset({argument}, {QStringLiteral("source/osc/argument")});
        auto *scale = new QComboBox;
        const struct { const char *name; double lo, hi; } kScales[] = {
            {"0 … 1", 0, 1}, {"−1 … 1", -1, 1}, {"0 … 127", 0, 127}, {"0 … 255", 0, 255}, {"0 … 1000", 0, 1000}};
        for (const auto &k : kScales) scale->addItem(QString::fromUtf8(k.name), QPointF(k.lo, k.hi));
        int at = 0;
        for (int k = 0; k < 5; ++k)
            if (it.srcMin >= kScales[k].lo && it.srcMax <= kScales[k].hi) {
                at = k;
                break;
            }
        scale->setCurrentIndex(at);
        srcLo = kScales[at].lo;
        srcHi = kScales[at].hi;
        scale->setToolTip(QStringLiteral("The values the sender sends: the bar of the source range"));
        form->addRow(QStringLiteral("Values"), scale);
        connect(address, &QLineEdit::editingFinished, this, [edit, address] {
            const QString a = address->text().trimmed();
            edit(QStringLiteral("Change Interaction Address"), [a](Interaction &x) { x.oscAddress = a; });
        });
        connect(argument, qOverload<int>(&QSpinBox::valueChanged), this, [edit](int a) {
            edit(QStringLiteral("Change Interaction Argument"), [a](Interaction &x) { x.oscArgument = a; }, QStringLiteral("oscarg"));
        });
        connect(scale, qOverload<int>(&QComboBox::activated), this, [this, edit, scale](int) {
            const QPointF r = scale->currentData().toPointF();
            edit(QStringLiteral("Change Interaction Values"), [r](Interaction &x) {
                x.srcMin = r.x();
                x.srcMax = r.y();
            });
            rebuildSettings();
        });
        break;
    }
    case InteractionSource::Internal: {
        auto *source = new QToolButton;
        source->setText(targetLabel(it.sourceLayer, it.sourceParam));
        source->setPopupMode(QToolButton::InstantPopup);
        source->setToolButtonStyle(Qt::ToolButtonTextOnly);
        source->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        source->setToolTip(QStringLiteral("The value of the show it follows"));
        auto *menu = new QMenu(source);
        source->setMenu(menu);
        connect(menu, &QMenu::aboutToShow, this, [this, menu, edit] {
            menu->clear();
            auto fill = [&](QMenu *into, quint64 layer) {
                std::vector<ParamInfo> params;
                for (const ParamInfo &p : m_engine->parameters(layer))
                    if ((p.access & ParamInfo::Read) && (p.isNumber() || p.type == ParamInfo::Type::Bool)) params.push_back(p);
                addParamMenu(into, params, [this, edit, layer](QMenu *m, const ParamInfo &p, const QString &text) {
                    m->addAction(text, this, [this, edit, layer, p] {
                        edit(QStringLiteral("Change Interaction Source"), [layer, p](Interaction &x) {
                            x.sourceLayer = layer;
                            x.sourceParam = p.path;
                            x.srcMin = p.type == ParamInfo::Type::Bool ? 0 : p.min;
                            x.srcMax = p.type == ParamInfo::Type::Bool ? 1 : p.max;
                        });
                        rebuildSettings();
                    });
                });
            };
            fill(menu->addMenu(QStringLiteral("Composition")), 0);
            Engine::Lock lk(&m_engine->mutex());
            for (int i = 0; i < m_engine->layerCount(); ++i)
                if (const Layer *l = m_engine->layer(i); l && !l->isGroup) fill(menu->addMenu(l->name), l->id);
        });
        form->addRow(QStringLiteral("Follows"), source);
        attachPreset({source}, {QStringLiteral("source/internal")});
        ParamInfo info;
        if (m_engine->parameterInfo(it.sourceLayer, it.sourceParam, &info) && info.isNumber()) {
            srcLo = std::min(info.min, it.srcMin);
            srcHi = std::max(info.max, it.srcMax);
        }
        break;
    }
    }

    // The target
    auto *target = new QToolButton;
    target->setText(targetLabel(it.layer, it.param));
    target->setPopupMode(QToolButton::InstantPopup);
    target->setToolButtonStyle(Qt::ToolButtonTextOnly);
    target->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
    target->setToolTip(QStringLiteral("The value of the show it drives: a number, a switch, a choice or an action"));
    auto *targetMenu = new QMenu(target);
    target->setMenu(targetMenu);
    connect(targetMenu, &QMenu::aboutToShow, this, [this, targetMenu, edit] {
        targetMenu->clear();
        auto fill = [&](QMenu *into, quint64 layer) {
            std::vector<ParamInfo> params;
            const std::vector<ParamInfo> available = m_engine->parameters(layer);
            QSet<QString> writable;
            for (const ParamInfo &p : available) {
                if (!(p.access & ParamInfo::Write) || p.type == ParamInfo::Type::Text) continue;
                writable.insert(p.path);
                params.push_back(p);
            }
            // Add one selectable composite target above its individually
            // addressable channels (e.g. param/color as well as /r, /g, /b).
            for (const ParamInfo &p : available) {
                if (!p.path.endsWith(QStringLiteral("/r"))) continue;
                const QString base = p.path.chopped(2);
                if (!writable.contains(base + QStringLiteral("/r")) ||
                    !writable.contains(base + QStringLiteral("/g")) ||
                    !writable.contains(base + QStringLiteral("/b")) ||
                    writable.contains(base)) continue;
                ParamInfo color = p;
                color.path = base;
                if (color.label.endsWith(QStringLiteral(" R"))) color.label.chop(2);
                color.animatable = false;
                params.push_back(color);
            }
            addParamMenu(into, params, [this, edit, layer](QMenu *m, const ParamInfo &p, const QString &text) {
                m->addAction(text, this, [this, edit, layer, p] {
                    edit(QStringLiteral("Change Interaction Target"), [layer, p](Interaction &x) {
                        x.layer = layer;
                        x.param = p.path;
                        x.dstMin = p.isNumber() ? p.min : 0; // its whole range
                        x.dstMax = p.isNumber() ? p.max : 1;
                    });
                    rebuildSettings();
                    rebuildList();
                });
            });
        };
        fill(targetMenu->addMenu(QStringLiteral("Composition")), 0);
        Engine::Lock lk(&m_engine->mutex());
        for (int i = 0; i < m_engine->layerCount(); ++i)
            if (const Layer *l = m_engine->layer(i); l && !l->isGroup) fill(targetMenu->addMenu(l->name), l->id);
    });
    form->addRow(QStringLiteral("Target"), target);
    attachPreset({target}, {QStringLiteral("target")});

    // What it does now
    m_inMeter = meter();
    m_outMeter = meter();
    m_inMeter->setToolTip(QStringLiteral("The source, after its range and the toggle"));
    m_outMeter->setToolTip(QStringLiteral("The result, after the filter, the curve and the master level"));
    auto *meters = new QVBoxLayout;
    meters->setSpacing(3);
    meters->addWidget(m_inMeter);
    meters->addWidget(m_outMeter);
    form->addRow(QStringLiteral("In / Out"), meters);

    // The chain
    auto *master = new SliderField;
    master->setRange(0, 100);
    master->setDecimals(0);
    master->setSuffix(QStringLiteral(" %"));
    master->setSingleStep(1);
    master->setTicks(10);
    master->setSnaps({0, 100});
    master->setValue(it.master * 100);
    auto *masterLabel = new ResetLabel(QStringLiteral("Master Level"), [edit, master] {
        master->setValue(100);
        edit(QStringLiteral("Reset Master Level"), [](Interaction &x) { x.master = 1; });
    });
    form->addRow(masterLabel, master);
    attachPreset({masterLabel, master}, {QStringLiteral("master")});
    connect(master, &SliderField::valueEdited, this, [edit](double v) {
        edit(QStringLiteral("Change Master Level"), [v](Interaction &x) { x.master = v / 100.0; }, QStringLiteral("master"));
    });

    auto *takeover = new QComboBox;
    takeover->addItem(QStringLiteral("Pick-up"), int(Takeover::PickUp));
    takeover->addItem(QStringLiteral("Jump"), int(Takeover::Jump));
    takeover->setCurrentIndex(it.takeover == Takeover::PickUp ? 0 : 1);
    takeover->setToolTip(QStringLiteral("Pick-up: once the target was moved by something else (a snapshot, the interface), "
                                        "it waits for the controller to meet it · Jump: it sets it at once"));
    auto *takeoverLabel = new QLabel(QStringLiteral("Takeover"));
    form->addRow(takeoverLabel, takeover);
    attachPreset({takeoverLabel, takeover}, {QStringLiteral("takeover")});
    connect(takeover, qOverload<int>(&QComboBox::activated), this, [edit, takeover](int) {
        const Takeover t = Takeover(takeover->currentData().toInt());
        edit(QStringLiteral("Change Takeover"), [t](Interaction &x) { x.takeover = t; });
    });

    auto *toggle = new FlagBox(QStringLiteral("Toggle"));
    toggle->setChecked(it.toggle);
    toggle->setToolTip(QStringLiteral("Each time the source goes up past the middle, the result flips between 0 and 1"));
    form->addRow(QString(), toggle);
    attachPreset({toggle}, {QStringLiteral("toggle")});
    connect(toggle, &QCheckBox::toggled, this, [edit](bool on) {
        edit(QStringLiteral("Change Toggle"), [on](Interaction &x) { x.toggle = on; });
    });

    auto *srcRange = new RangeField;
    srcRange->setRange(std::min(srcLo, it.srcMin), std::max(srcHi, it.srcMax));
    srcRange->setDecimals(srcHi - srcLo > 10 ? 0 : 2);
    srcRange->setValues(std::min(it.srcMin, it.srcMax), std::max(it.srcMin, it.srcMax));
    srcRange->setToolTip(QStringLiteral("The part of the source's values used: its low end gives 0, its high end 1"));
    auto *srcLabel = new ResetLabel(QStringLiteral("Source range"), [edit, srcRange, srcLo, srcHi] {
        srcRange->setValues(srcLo, srcHi);
        edit(QStringLiteral("Reset Source Range"), [srcLo, srcHi](Interaction &x) {
            x.srcMin = srcLo;
            x.srcMax = srcHi;
        });
    });
    form->addRow(srcLabel, srcRange);
    attachPreset({srcLabel, srcRange}, {QStringLiteral("source/min"), QStringLiteral("source/max")});
    connect(srcRange, &RangeField::edited, this, [edit, srcRange](bool, double) {
        const double lo = srcRange->low(), hi = srcRange->high();
        edit(QStringLiteral("Change Source Range"), [lo, hi](Interaction &x) {
            x.srcMin = lo;
            x.srcMax = hi;
        }, QStringLiteral("srcrange"));
    });

    // Target range: within the target's own values; inverted, 0 lands on its high end
    ParamInfo tinfo;
    const bool numberTarget = m_engine->parameterInfo(it.layer, it.param, &tinfo) && tinfo.isNumber();
    const double tLo = numberTarget ? std::min({tinfo.min, it.dstMin, it.dstMax}) : 0;
    const double tHi = numberTarget ? std::max({tinfo.max, it.dstMin, it.dstMax}) : 1;
    auto *dstRange = new RangeField;
    dstRange->setRange(tLo, tHi);
    dstRange->setDecimals(tHi - tLo > 10 ? 1 : 2);
    dstRange->setValues(std::min(it.dstMin, it.dstMax), std::max(it.dstMin, it.dstMax));
    dstRange->setToolTip(QStringLiteral("Where in the target's values the result lands"));
    auto *invert = new FlagBox(QStringLiteral("Invert"));
    invert->setChecked(it.dstMin > it.dstMax);
    invert->setToolTip(QStringLiteral("0 lands on the high end, 1 on the low end"));
    auto *dstRow = new QHBoxLayout;
    dstRow->addWidget(dstRange, 1);
    dstRow->addWidget(invert);
    const double fullLo = numberTarget ? tinfo.min : 0, fullHi = numberTarget ? tinfo.max : 1;
    auto *dstLabel = new ResetLabel(QStringLiteral("Target range"), [edit, dstRange, invert, fullLo, fullHi] {
        dstRange->setValues(fullLo, fullHi);
        invert->blockSignals(true);
        invert->setChecked(false);
        invert->blockSignals(false);
        edit(QStringLiteral("Reset Target Range"), [fullLo, fullHi](Interaction &x) {
            x.dstMin = fullLo;
            x.dstMax = fullHi;
        });
    });
    form->addRow(dstLabel, dstRow);
    attachPreset({dstLabel, dstRange, invert}, {QStringLiteral("target/min"), QStringLiteral("target/max")});
    auto setTarget = [edit, dstRange, invert](const QString &merge) {
        const double lo = dstRange->low(), hi = dstRange->high();
        const bool inv = invert->isChecked();
        edit(QStringLiteral("Change Target Range"), [lo, hi, inv](Interaction &x) {
            x.dstMin = inv ? hi : lo;
            x.dstMax = inv ? lo : hi;
        }, merge);
    };
    connect(dstRange, &RangeField::edited, this, [setTarget](bool, double) { setTarget(QStringLiteral("dstrange")); });
    connect(invert, &QCheckBox::toggled, this, [setTarget](bool) { setTarget(QString()); });

    // Filter
    auto *filterRow = new QHBoxLayout;
    auto *filterOn = new FlagBox(QStringLiteral("Filter"));
    filterOn->setChecked(it.filterOn);
    auto *filter = new QComboBox;
    for (int k = 0; k < kInteractionFilterCount; ++k) filter->addItem(interactionFilterName(InteractionFilter(k)), k);
    filter->setCurrentIndex(int(it.filter));
    filter->setEnabled(it.filterOn);
    filter->setToolTip(QStringLiteral("Average: of the last moments · Weighted Average: smoothed · Pulse Detection: a pulse on "
                                      "each sudden rise · Attack Release: rises and falls at speeds of their own · Damper: "
                                      "a spring that follows"));
    filterRow->addWidget(filterOn);
    filterRow->addWidget(filter, 1);
    form->addRow(QString(), filterRow);
    attachPreset({filterOn}, {QStringLiteral("filter/enable")});
    attachPreset({filter}, {QStringLiteral("filter/type")});
    connect(filterOn, &QCheckBox::toggled, this, [this, edit](bool on) {
        edit(QStringLiteral("Change Filter"), [on](Interaction &x) { x.filterOn = on; });
        rebuildSettings();
    });
    connect(filter, qOverload<int>(&QComboBox::activated), this, [this, edit](int k) {
        edit(QStringLiteral("Change Filter"), [k](Interaction &x) {
            x.filter = InteractionFilter(k);
            x.filterA = interactionFilterSetting(x.filter, 0).def; // the new one's own settings
            x.filterB = interactionFilterSetting(x.filter, 1).def;
        });
        rebuildSettings();
    });
    if (it.filterOn)
        for (int which = 0; which < 2; ++which) {
            const FilterSetting fs = interactionFilterSetting(it.filter, which);
            if (fs.name.isEmpty()) continue;
            auto *f = new SliderField;
            f->setRange(fs.min, fs.max);
            f->setDecimals(fs.min < 0.01 ? 3 : 2);
            f->setSuffix(fs.suffix);
            f->setSingleStep((fs.max - fs.min) / 100.0);
            f->setTicks(10);
            f->setSnaps({fs.def});
            f->setValue(which == 0 ? it.filterA : it.filterB);
            const double def = fs.def;
            auto *fl = new ResetLabel(fs.name, [edit, f, which, def] {
                f->setValue(def);
                edit(QStringLiteral("Reset Filter"), [which, def](Interaction &x) { (which == 0 ? x.filterA : x.filterB) = def; });
            });
            form->addRow(fl, f);
            attachPreset({fl, f}, {which == 0 ? QStringLiteral("filter/a") : QStringLiteral("filter/b")});
            connect(f, &SliderField::valueEdited, this, [edit, which](double v) {
                edit(QStringLiteral("Change Filter"), [which, v](Interaction &x) { (which == 0 ? x.filterA : x.filterB) = v; },
                     QStringLiteral("filter%1").arg(which));
            });
        }

    // Curve
    auto *curveOn = new FlagBox(QStringLiteral("Curve"));
    curveOn->setChecked(it.curveOn);
    curveOn->setToolTip(QStringLiteral("The result shaped by a curve: click to add a key, drag it, double-click to remove it, "
                                       "right-click for its easing"));
    auto *resetCurve = new QPushButton(QStringLiteral("Straight"));
    resetCurve->setToolTip(QStringLiteral("Back to a straight line"));
    resetCurve->setEnabled(it.curveOn);
    auto *curveRow = new QHBoxLayout;
    curveRow->addWidget(curveOn);
    curveRow->addStretch();
    curveRow->addWidget(resetCurve);
    form->addRow(QString(), curveRow);
    if (!it.curveOn) attachPreset({curveOn}, {QStringLiteral("curve/enable")});
    connect(curveOn, &QCheckBox::toggled, this, [this, edit](bool on) {
        m_revealCurve = on;
        edit(QStringLiteral("Change Curve"), [on](Interaction &x) { x.curveOn = on; });
        rebuildSettings();
    });
    connect(resetCurve, &QPushButton::clicked, this, [this, edit] {
        edit(QStringLiteral("Reset Curve"), [](Interaction &x) { x.curve = Interaction::straightCurve(); });
        rebuildSettings();
    });
    if (it.curveOn) {
        // The animations' curve editor, its whole square shown (0..1 in, 0..1 out, a margin around), the playhead where
        // the source is now
        auto *lane = new CurveLane;
        lane->setTrack(it.curve, 1.0, 0.0, 1.0);
        lane->setView(-0.03, 1.03, -0.08, 1.08);
        lane->setFixedHeight(220);
        lane->setToolTip(QStringLiteral("Across: what comes in (after the filter) · up: what goes out. Click to add a key, drag "
                                        "it, double-click to remove it; right-click: its curve (easing, Bézier, hold)"));
        m_lane = lane;
        m_settingsLayout->addWidget(lane);
        // Its right-click is its keys' own (their easing): only Edit mode (a click on its title) puts it in a preset
        if (PresetEditor *pe = PresetEditor::instance()) pe->track(lane, 0, {QStringLiteral("interaction/%1/curve").arg(it.id)});
        attachPreset({curveOn}, {QStringLiteral("curve/enable"), QStringLiteral("curve")});
        if (m_revealCurve) { // just turned on: shown
            m_revealCurve = false;
            QTimer::singleShot(60, this, [this] { // once laid out
                if (!m_lane || !m_scroll) return;
                m_settings->adjustSize();
                m_scroll->verticalScrollBar()->setValue(m_scroll->verticalScrollBar()->maximum()); // it is last
            });
        }
        connect(lane, &CurveLane::edited, this, [edit](const AnimTrack &t, int gesture) {
            edit(QStringLiteral("Edit Curve"), [t](Interaction &x) {
                x.curve.keys = t.keys;
                for (AnimKey &k : x.curve.keys) k.t = std::clamp(k.t, 0.0, 1.0);
            }, gesture ? QStringLiteral("curve%1").arg(gesture) : QString());
        });
    }
    m_settingsLayout->addStretch();
}
