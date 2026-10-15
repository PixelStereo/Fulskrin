#include "SequencePanel.h"
#include "Engine.h"
#include "SnapshotPanel.h"
#include "TimelinePanel.h"
#include "Widgets.h"

#include <QApplication>
#include <QCheckBox>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QListWidget>
#include <QMenu>
#include <QMimeData>
#include <QPainter>
#include <QPainterPath>
#include <QStyledItemDelegate>
#include <QTimer>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
#include <cmath>

static QString snapshotName(Engine *e, quint64 id)
{
    if (!id) return QStringLiteral("—");
    const int i = e->indexOfSnapshot(id);
    if (i < 0) return QStringLiteral("(snapshot gone)");
    const Engine::Snapshot m = e->snapshot(i);
    return QStringLiteral("%1  %2").arg(i + 1).arg(m.name);
}

// What a timeline step does
static QString actionName(const Sequencer::Step &st)
{
    using A = Engine::AnimAction;
    switch (st.action) {
    case A::Play: return QStringLiteral("▶ Play");
    case A::Pause: return QStringLiteral("❚❚ Pause");
    case A::Stop: return QStringLiteral("■ Stop");
    case A::Rewind: return QStringLiteral("⏮ Rewind");
    case A::Seek: return QStringLiteral("Seek %1 s").arg(st.seekTime, 0, 'f', 2);
    case A::Speed: return QStringLiteral("Speed × %1").arg(st.speed, 0, 'f', 2);
    case A::LoopMode: {
        const QString n = st.repeat > 0 ? QStringLiteral(" × %1").arg(st.repeat) : QStringLiteral(" ∞");
        return st.loop == Engine::AnimLoop::Once ? QStringLiteral("Loop: once")
             : st.loop == Engine::AnimLoop::PingPong ? QStringLiteral("Loop: ping-pong") + n
                                                    : QStringLiteral("Loop") + n;
    }
    }
    return {};
}

static QString timelineName(Engine *e, quint64 id)
{
    const int i = e->indexOfAnimation(id);
    return i < 0 ? QStringLiteral("(timeline gone)") : e->animation(i).name;
}

// What a step does: its snapshot (with its number), or its timeline and the action on it
static QString stepTarget(Engine *e, const Sequencer::Step &st, bool numbered = true)
{
    if (st.timeline) return QStringLiteral("≋ %1 · %2").arg(timelineName(e, st.timeline), actionName(st));
    if (numbered) return snapshotName(e, st.snapshot);
    const int mi = e->indexOfSnapshot(st.snapshot);
    return !st.snapshot ? QStringLiteral("—") : mi < 0 ? QStringLiteral("(snapshot gone)") : e->snapshot(mi).name;
}

static bool targetGone(Engine *e, const Sequencer::Step &st)
{
    return st.timeline ? e->indexOfAnimation(st.timeline) < 0 : st.snapshot && e->indexOfSnapshot(st.snapshot) < 0;
}

static const char *kChanged = "#ffb347"; // something changed since the step was played
static const QColor kLive(76, 217, 100);  // a snapshot running
static const QColor kPreWait(224, 180, 58);  // waiting before the snapshot
static const QColor kPostWait(86, 156, 230); // waiting before the next step's GO

// Seconds as the cue list shows them: 2.5 · 1:05.0
static QString seconds(double t)
{
    if (!std::isfinite(t)) return QStringLiteral("∞");
    t = std::max(0.0, t);
    if (t < 60) return QString::number(t, 'f', 1);
    const int m = int(t / 60);
    return QStringLiteral("%1:%2").arg(m).arg(t - m * 60, 4, 'f', 1, QLatin1Char('0'));
}

static QString continueName(Sequencer::StepContinue c)
{
    switch (c) {
    case Sequencer::StepContinue::Follow: return QStringLiteral("↓ Follow");
    case Sequencer::StepContinue::AutoFollow: return QStringLiteral("⤓ Auto-follow");
    default: return QStringLiteral("Wait (GO)");
    }
}

// Where a step on its way is in each of its times: -1 not there yet (or none), 0..1 on the way, 1 over
struct RunPhases {
    double pre = -1, action = -1, post = -1;
    double preElapsed = 0, actionElapsed = 0, postElapsed = 0;
};
static RunPhases phasesOf(const Sequencer::Run &r)
{
    RunPhases ph;
    auto frac = [](double e, double len) { return len > 0 ? std::clamp(e / len, 0.0, 1.0) : (e >= 0 ? 1.0 : 0.0); };
    ph.preElapsed = std::min(r.elapsed, r.preWait);
    ph.pre = r.preWait > 0 ? frac(r.elapsed, r.preWait) : -1;
    if (r.fired) {
        ph.actionElapsed = std::clamp(r.elapsed - r.preWait, 0.0, r.duration);
        ph.action = frac(r.elapsed - r.preWait, r.duration);
    }
    const double at = r.continueAt();
    if (at >= 0 && r.fired) {
        const double start = at - r.postWait;
        if (r.elapsed >= start) {
            ph.postElapsed = std::clamp(r.elapsed - start, 0.0, r.postWait);
            ph.post = frac(r.elapsed - start, r.postWait);
        }
    }
    return ph;
}

// ---------------------------------------------------------------------------
// RecallProgressBar
// ---------------------------------------------------------------------------
RecallProgressBar::RecallProgressBar(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine)
{
    setFixedHeight(5);
    auto *t = new QTimer(this);
    t->setInterval(33);
    connect(t, &QTimer::timeout, this, &RecallProgressBar::poll);
    t->start();
}

void RecallProgressBar::poll()
{
    const Engine::RecallProgress r = m_engine->recallProgress();
    const double f = r.snapshot ? r.fraction() : 0.0;
    const bool running = r.running();
    if (std::abs(f - m_fraction) < 1e-4 && running == m_running) return;
    m_fraction = f;
    m_running = running;
    const int mi = m_engine->indexOfSnapshot(r.snapshot);
    setToolTip(mi < 0 ? QString()
                      : QStringLiteral("%1. %2 — %3 / %4 s").arg(mi + 1).arg(m_engine->snapshot(mi).name)
                            .arg(std::min(r.elapsed, r.total), 0, 'f', 1).arg(r.total, 0, 'f', 1));
    update();
}

void RecallProgressBar::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.fillRect(rect(), QColor(40, 40, 44));
    if (m_fraction <= 0) return;
    QColor c = m_running ? kLive : QColor(kLive.red(), kLive.green(), kLive.blue(), 90); // done: dimmed
    p.fillRect(QRectF(0, 0, width() * m_fraction, height()), c);
}

// ---------------------------------------------------------------------------
// CueStack
// ---------------------------------------------------------------------------

static const QColor kUpcoming(232, 76, 61); // the step GO plays: its frame
static const QColor kStackBack(32, 32, 36);

CueStack::CueStack(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine)
{
    setObjectName(QStringLiteral("sequenceCueStack"));
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Ignored); // long names give way, not the buttons
    setMinimumWidth(120);
}

void CueStack::setModified(bool on)
{
    if (on == m_modified) return;
    m_modified = on;
    rebuild();
}

// Heights of the rows: the step played gets the most room
static int rowHeight(CueStack::Role r)
{
    switch (r) {
    case CueStack::Current: return 58;
    case CueStack::Next: return 50;
    default: return 24;
    }
}
static constexpr int kRowGap = 4;

void CueStack::rebuild()
{
    const Sequencer &sq = m_engine->sequencer();
    const Sequencer::Sequence s = sq.sequence(sq.current());
    const int n = int(s.steps.size());
    const int pos = sq.position(), next = sq.next(), prev = sq.previous();
    const std::vector<Sequencer::Run> runs = sq.runs();

    auto fill = [&](Row &row, int k) {
        row.step = k;
        const Sequencer::Step &st = s.steps[size_t(k)];
        row.title = QStringLiteral("%1").arg(stepTarget(m_engine, st, false));
        row.text = st.text;
        row.note = st.description;
        const double d = sq.stepDuration(st);
        row.timing = QStringLiteral("PRE %1   ACTION %2   POST %3 s   ·   %4")
                         .arg(seconds(st.preWait), std::isfinite(d) ? seconds(d) : QStringLiteral("∞"), seconds(st.postWait),
                              continueName(st.next));
        // Its time on its way (the latest, should it be there twice in a loop)
        const Sequencer::Run *run = nullptr;
        for (const Sequencer::Run &r : runs)
            if (r.step == k) run = &r;
        if (!run) return;
        const RunPhases ph = phasesOf(*run);
        auto on = [](double f) { return f >= 0 && f < 1; };
        auto pct = [](double f) { return int(std::lround(std::clamp(f, 0.0, 1.0) * 100)); };
        row.pre = on(ph.pre) ? ph.pre : -1;
        row.action = run->fired && run->duration > 0 && on(ph.action) ? ph.action : -1;
        row.post = on(ph.post) ? ph.post : -1;
        if (!run->fired) {
            row.status = QStringLiteral("PRE-WAIT · %1 s left · %2%").arg(seconds(run->preWait - run->elapsed)).arg(pct(ph.pre));
        } else if (row.post >= 0) {
            const int to = k + 1 < n ? k + 1 : s.loop ? 0 : -1;
            row.status = QStringLiteral("POST-WAIT%1 · %2 s left · %3%")
                             .arg(to >= 0 ? QStringLiteral(" → %1").arg(to + 1) : QString())
                             .arg(seconds(run->continueAt() - run->elapsed)).arg(pct(ph.post));
        } else if (row.action >= 0) {
            row.status = QStringLiteral("ACTION · %1 s left · %2%")
                             .arg(seconds(run->duration - (run->elapsed - run->preWait))).arg(pct(ph.action));
        }
    };

    std::vector<Row> rows;
    auto add = [&](Role role, int k) {
        Row r;
        r.role = role;
        if (k >= 0 && k < n) fill(r, k);
        rows.push_back(r);
        return &rows.back();
    };
    if (prev >= 0 && prev != pos) add(Previous, prev);
    Row *current = add(Current, pos);
    if (n == 0) current->title = QStringLiteral("No step — add them in Sequences…");
    else if (pos < 0) current->title = QStringLiteral("Ready — GO plays the first step");
    else current->aside = QStringLiteral("%1 s since GO").arg(seconds(sq.stepElapsed(sq.current())));
    if (n > 0) {
        Row *upcoming = add(Next, next != pos ? next : -1);
        if (upcoming->step < 0) upcoming->title = QStringLiteral("End of the sequence");
        else if (upcoming->status.isEmpty()) {
            // Played by the step before it (a follow), or waiting for GO
            bool followed = false;
            for (const Sequencer::Run &r : runs)
                if (r.fired && !r.continued && r.continueAt() >= 0) followed = true;
            upcoming->status = followed ? QStringLiteral("follows") : pos >= 0 ? QStringLiteral("press GO") : QStringLiteral("GO");
        }
        // Then the steps after it, as many as there is room for
        int used = 0;
        for (const Row &r : rows) used += rowHeight(r.role) + kRowGap;
        for (int k = upcoming->step + 1; upcoming->step >= 0 && k < n && used + rowHeight(Later) <= height(); ++k) {
            add(Later, k);
            used += rowHeight(Later) + kRowGap;
        }
    }
    m_rows = std::move(rows);
    const QString drawn = describe() + QString::number(m_modified) + QString::number(width());
    if (drawn != m_drawn) {
        m_drawn = drawn;
        setToolTip(describe());
        update();
    }
}

QString CueStack::describe() const
{
    QStringList lines;
    for (const Row &r : m_rows) {
        QStringList parts;
        if (r.step >= 0) parts << QString::number(r.step + 1);
        for (const QString &t : {r.title, r.text, r.note, r.status, r.aside, r.timing})
            if (!t.isEmpty()) parts << t;
        lines << parts.join(QStringLiteral("  ·  "));
    }
    return lines.join(QLatin1Char('\n'));
}

void CueStack::paintEvent(QPaintEvent *)
{
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing);
    p.fillRect(rect(), kStackBack);
    const QColor played = m_modified ? QColor(QString::fromLatin1(kChanged)) : kLive;
    int y = 0;
    for (const Row &r : m_rows) {
        const int h = rowHeight(r.role);
        const QRectF box(0.5, y + 0.5, width() - 1.0, h - 1.0);
        y += h + kRowGap;
        const bool placeholder = r.step < 0;
        QColor frame = r.role == Current ? played : r.role == Next ? kUpcoming : QColor(58, 58, 66);
        if (placeholder) frame.setAlphaF(0.35);
        QPainterPath shape;
        shape.addRoundedRect(box, 3, 3);

        // The time going by, at half opacity, inside the frame; a thin line at its foot says exactly where
        p.save();
        p.setClipPath(shape);
        const QRectF inside = box.adjusted(1, 1, -1, -1);
        for (auto [f, c] : {std::pair{r.pre, kPreWait}, std::pair{r.action, kLive}, std::pair{r.post, kPostWait}}) {
            if (f < 0) continue;
            QColor half = c;
            half.setAlphaF(0.5);
            p.fillRect(QRectF(inside.left(), inside.top(), inside.width() * f, inside.height()), half);
            p.fillRect(QRectF(inside.left(), inside.bottom() - 2, inside.width() * f, 2), c);
        }
        p.restore();
        p.setPen(QPen(frame, 1));
        p.setBrush(Qt::NoBrush);
        p.drawPath(shape);

        // The number, then what the step does, its text and its times
        const bool big = r.role == Current, mid = r.role == Next;
        const bool dim = r.role == Previous || r.role == Later;
        const QRect area = box.toAlignedRect().adjusted(8, 3, -8, -3);
        QFont number = font();
        number.setPixelSize(big ? 20 : mid ? 15 : 11);
        number.setBold(true);
        const int numberWidth = big ? 34 : mid ? 30 : 26;
        QRect numberRect(area.left(), area.top(), numberWidth, big || mid ? 24 : area.height());
        p.setFont(number);
        p.setPen(r.role == Current ? played : r.role == Next ? kUpcoming.lighter(125) : QColor(120, 120, 128));
        if (!placeholder) p.drawText(numberRect, Qt::AlignLeft | Qt::AlignVCenter, QString::number(r.step + 1));

        const int textLeft = area.left() + (placeholder ? 0 : numberWidth);
        QFont title = font();
        title.setPixelSize(big ? 14 : mid ? 13 : 11);
        title.setBold(!dim && !placeholder);
        QFont small = font();
        small.setPixelSize(10);
        const QFontMetrics tm(title), sm(small);

        // Right of the first line: where its time is (or the time since GO)
        const QString right = !r.status.isEmpty() ? r.status : r.aside;
        int rightWidth = 0;
        if (!right.isEmpty()) {
            QFont status = small;
            status.setPixelSize(big || mid ? 11 : 10);
            status.setBold(r.pre >= 0 || r.action >= 0 || r.post >= 0);
            p.setFont(status);
            const QFontMetrics stm(status);
            rightWidth = std::min(stm.horizontalAdvance(right) + 4, area.width() / 2);
            p.setPen(r.status == QLatin1String("press GO") || r.status == QLatin1String("GO") ? QColor(150, 150, 158)
                                                                                                : QColor(236, 236, 240));
            p.drawText(QRect(area.right() - rightWidth, area.top(), rightWidth, tm.height() + 4), Qt::AlignRight | Qt::AlignVCenter,
                       stm.elidedText(right, Qt::ElideRight, rightWidth));
            rightWidth += 10;
        }
        QString name = r.title;
        if (r.role == Current && m_modified && !placeholder) name += QStringLiteral("  (changed)");
        p.setFont(title);
        p.setPen(placeholder ? QColor(130, 130, 138) : dim ? QColor(150, 150, 158) : QColor(240, 240, 244));
        const QRect titleRect(textLeft, area.top(), std::max(0, area.right() - rightWidth - textLeft), tm.height() + 4);
        p.drawText(titleRect, Qt::AlignLeft | Qt::AlignVCenter, tm.elidedText(name, Qt::ElideRight, titleRect.width()));
        if (dim || placeholder) continue;

        // Its text (and what the operator should know), then its times, in thin grey
        int line = titleRect.bottom() + 1;
        QString words = r.text;
        if (!r.note.isEmpty()) words += (words.isEmpty() ? QString() : QStringLiteral("   —   ")) + r.note;
        if (!words.isEmpty()) {
            QFont italic = font();
            italic.setPixelSize(big ? 12 : 11);
            italic.setItalic(true);
            p.setFont(italic);
            const QFontMetrics im(italic);
            p.setPen(QColor(200, 200, 206));
            p.drawText(QRect(textLeft, line, area.right() - textLeft, im.height()), Qt::AlignLeft | Qt::AlignVCenter,
                       im.elidedText(words, Qt::ElideRight, area.right() - textLeft));
            line += im.height();
        }
        p.setFont(small);
        p.setPen(QColor(128, 128, 136));
        p.drawText(QRect(textLeft, area.bottom() - sm.height(), area.right() - textLeft, sm.height() + 1),
                   Qt::AlignLeft | Qt::AlignBottom, sm.elidedText(r.timing, Qt::ElideRight, area.right() - textLeft));
    }
}

// ---------------------------------------------------------------------------
// SequenceBar
// ---------------------------------------------------------------------------

SequenceBar::SequenceBar(Engine *engine, QWidget *parent) : QWidget(parent), m_engine(engine)
{
    setObjectName("sequenceBar");
    setFixedHeight(190);
    setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
    setStyleSheet(
        "#sequenceBar { background:#202024; border-top:1px solid #34343a; }"
        "#sequenceBar QPushButton { background:#26262b; color:#d8d8dc; border:1px solid #3a3a42; border-radius:3px;"
        "  padding:3px 6px; }"
        "#sequenceBar QPushButton:hover { border-color:#56565f; }"
        "#sequenceBar QPushButton:disabled { color:#5a5a62; border-color:#2e2e34; }"
        "#sequenceBar QPushButton#sequenceGo { background:#24452b; border-color:#4cd964; color:white; font-weight:bold;"
        "  font-size:16px; }"
        "#sequenceBar QPushButton#sequenceGo:disabled { background:#232826; border-color:#2e3a31; color:#5a5a62; }"
        "#sequenceBar QPushButton#sequenceStop:enabled, #sequenceBar QPushButton#sequenceStopAll:enabled {"
        "  border-color:#e84c3d; color:#ff8f84; }");
    setAttribute(Qt::WA_StyledBackground);
    auto *h = new QHBoxLayout(this);
    h->setContentsMargins(8, 6, 8, 6);
    h->setSpacing(8);

    // On the left, one above the other: the sequence, GO, GO BACK, STOP, STOP ALL
    auto *buttons = new QVBoxLayout;
    buttons->setSpacing(4);
    m_sequence = new QComboBox;
    m_sequence->setToolTip(QStringLiteral("Sequence played by GO"));
    m_sequence->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);
    m_go = new QPushButton(QStringLiteral("GO"));
    m_go->setObjectName(QStringLiteral("sequenceGo"));
    m_go->setToolTip(QStringLiteral("GO: the next step (Space)"));
    m_go->setFixedHeight(48);
    m_back = new QPushButton(QStringLiteral("GO BACK"));
    m_back->setToolTip(QStringLiteral("GO BACK: the previous step (Shift+Space)"));
    m_stop = new QPushButton(QStringLiteral("STOP"));
    m_stop->setObjectName(QStringLiteral("sequenceStop"));
    m_stop->setToolTip(QStringLiteral("Stop the waits of the selected sequence: its pre-waits and follows still to "
                                      "come are dropped (the snapshots already running go on; the other sequences "
                                      "keep playing)"));
    // Several sequences may play at once: one button stops them all, whichever is selected
    m_stopAll = new QPushButton(QStringLiteral("STOP ALL"));
    m_stopAll->setObjectName(QStringLiteral("sequenceStopAll"));
    m_stopAll->setToolTip(QStringLiteral("Stop all: the pre-waits and follows still to come in every sequence, "
                                         "those playing in the background too (the snapshots already running go on)"));
    for (QPushButton *b : {m_stop, m_stopAll}) b->setEnabled(false);
    for (QWidget *w : std::initializer_list<QWidget *>{m_sequence, m_go, m_back, m_stop, m_stopAll}) {
        w->setFixedWidth(124);
        buttons->addWidget(w);
    }
    buttons->addStretch();
    h->addLayout(buttons);

    // The steps, from top to bottom
    m_stack = new CueStack(m_engine);
    h->addWidget(m_stack, 1);

    connect(m_go, &QPushButton::clicked, this, &SequenceBar::goRequested);
    connect(m_back, &QPushButton::clicked, this, &SequenceBar::backRequested);
    connect(m_stop, &QPushButton::clicked, this, &SequenceBar::stopRequested);
    connect(m_stopAll, &QPushButton::clicked, this, &SequenceBar::stopAllRequested);
    auto *waits = new QTimer(this);
    waits->setObjectName(QStringLiteral("sequenceWaitPoller"));
    waits->setInterval(50);
    connect(waits, &QTimer::timeout, this, &SequenceBar::pollWaits);
    waits->start();
    connect(m_sequence, qOverload<int>(&QComboBox::activated), this, [this](int i) {
        if (!m_filling) m_engine->sequencer().setCurrent(i);
    });
    connect(&m_engine->sequencer(), &Sequencer::changed, this, &SequenceBar::refresh);
    connect(&m_engine->sequencer(), &Sequencer::positionChanged, this, &SequenceBar::refresh);
    connect(m_engine, &Engine::snapshotsChanged, this, &SequenceBar::refresh);
    connect(m_engine, &Engine::animationsChanged, this, &SequenceBar::refresh);
    refresh();
}

// The times on their way, and what the stop buttons can stop
void SequenceBar::pollWaits()
{
    m_stack->rebuild();
    m_stop->setEnabled(m_engine->sequencer().running());
    m_stopAll->setEnabled(m_engine->sequencer().anyActive());
}

void SequenceBar::setModified(bool on)
{
    m_stack->setModified(on);
}

void SequenceBar::refresh()
{
    m_filling = true;
    const int n = m_engine->sequencer().count(), cur = m_engine->sequencer().current();
    m_sequence->clear();
    for (int i = 0; i < n; ++i) m_sequence->addItem(m_engine->sequencer().sequence(i).name);
    if (n == 0) m_sequence->addItem(QStringLiteral("No sequence"));
    m_sequence->setCurrentIndex(std::max(0, cur));
    m_sequence->setEnabled(n > 0);
    m_filling = false;
    m_go->setEnabled(m_engine->sequencer().next() >= 0);
    m_back->setEnabled(m_engine->sequencer().previous() >= 0);
    m_stack->rebuild();
}

// ---------------------------------------------------------------------------
// SequenceWindow
// ---------------------------------------------------------------------------

enum StepCol { ColNum, ColSnapshot, ColPre, ColAction, ColPost, ColNext, ColText, ColDescription, ColCount };

// The time columns (pre-wait, action, post-wait) filling as their time goes by, the continuation as a word;
// the waits are edited with the app's number field, the continuation with a menu
class StepDelegate : public QStyledItemDelegate
{
public:
    StepDelegate(SequenceWindow *w) : QStyledItemDelegate(w), m_w(w) {}

    QWidget *createEditor(QWidget *parent, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        if (idx.column() == ColPre || idx.column() == ColPost) {
            auto *b = new NumberBox(parent);
            b->setRange(0, 3600);
            b->setDecimals(2);
            b->setSingleStep(0.1);
            b->setSuffix(QStringLiteral(" s"));
            return b;
        }
        if (idx.column() == ColNext) {
            auto *c = new QComboBox(parent);
            for (auto k : {Sequencer::StepContinue::Wait, Sequencer::StepContinue::Follow, Sequencer::StepContinue::AutoFollow})
                c->addItem(continueName(k), int(k));
            connect(c, qOverload<int>(&QComboBox::activated), this, [this, c] {
                auto *self = const_cast<StepDelegate *>(this);
                emit self->commitData(c);
                emit self->closeEditor(c);
            });
            return c;
        }
        return QStyledItemDelegate::createEditor(parent, opt, idx);
    }
    void setEditorData(QWidget *e, const QModelIndex &idx) const override
    {
        if (auto *b = qobject_cast<QDoubleSpinBox *>(e)) b->setValue(idx.data(Qt::EditRole).toDouble());
        else if (auto *c = qobject_cast<QComboBox *>(e)) {
            c->setCurrentIndex(std::max(0, c->findData(idx.data(Qt::EditRole).toInt())));
            c->showPopup();
        } else QStyledItemDelegate::setEditorData(e, idx);
    }
    void setModelData(QWidget *e, QAbstractItemModel *m, const QModelIndex &idx) const override
    {
        if (auto *b = qobject_cast<QDoubleSpinBox *>(e)) m->setData(idx, b->value(), Qt::EditRole);
        else if (auto *c = qobject_cast<QComboBox *>(e)) m->setData(idx, c->currentData().toInt(), Qt::EditRole);
        else QStyledItemDelegate::setModelData(e, m, idx);
    }

    void paint(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const override
    {
        paintCell(p, opt, idx);
        paintFrame(p, opt, idx);
    }

private:
    // The step played and the one GO plays: a thin frame around their whole row (each cell draws its part of it)
    void paintFrame(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const
    {
        const int row = idx.row();
        bool playing = row == m_w->m_position;
        for (const Sequencer::Run &r : m_w->m_runs)
            if (r.step == row) playing = true;
        if (!playing && row != m_w->m_upcoming) return;
        const auto *view = qobject_cast<const QAbstractItemView *>(opt.widget);
        const QHeaderView *header = m_w->m_steps->horizontalHeader();
        if (!view || !header) return;
        const int first = header->logicalIndex(0), last = header->logicalIndex(header->count() - 1);
        const QRect left = view->visualRect(idx.sibling(row, first)), right = view->visualRect(idx.sibling(row, last));
        const QRectF whole = QRectF(QRect(left.topLeft(), right.bottomRight())).adjusted(1.5, 1.5, -1.5, -1.5);
        const QColor c = playing ? (m_w->m_modified ? QColor(QString::fromLatin1(kChanged)) : kLive) : kUpcoming;
        p->save();
        p->setClipRect(opt.rect);
        p->setRenderHint(QPainter::Antialiasing);
        p->setPen(QPen(c, 1));
        p->setBrush(Qt::NoBrush);
        p->drawRoundedRect(whole, 3, 3);
        p->restore();
    }

    void paintCell(QPainter *p, const QStyleOptionViewItem &opt, const QModelIndex &idx) const
    {
        const int col = idx.column();
        if (col != ColPre && col != ColAction && col != ColPost && col != ColNext) {
            QStyledItemDelegate::paint(p, opt, idx);
            return;
        }
        QStyleOptionViewItem o(opt);
        initStyleOption(&o, idx);
        o.text.clear();
        const QWidget *w = opt.widget;
        (w ? w->style() : QApplication::style())->drawControl(QStyle::CE_ItemViewItem, &o, p, w);
        const auto next = Sequencer::StepContinue(idx.sibling(idx.row(), ColNext).data(Qt::EditRole).toInt());
        const bool selected = opt.state & QStyle::State_Selected;
        QColor ink = opt.palette.color(selected ? QPalette::HighlightedText : QPalette::Text);
        if (col == ColNext) {
            if (next == Sequencer::StepContinue::Wait) ink.setAlphaF(0.45);
            p->setPen(ink);
            p->drawText(opt.rect.adjusted(6, 0, -4, 0), Qt::AlignVCenter | Qt::AlignLeft, continueName(next));
            return;
        }
        const double value = idx.data(Qt::EditRole).toDouble();
        // The step on its way (the latest, should it be there twice in a loop)
        const Sequencer::Run *run = nullptr;
        for (const Sequencer::Run &r : m_w->m_runs)
            if (r.step == idx.row()) run = &r;
        double frac = -1, elapsed = 0;
        QColor bar = kLive;
        if (run) {
            const RunPhases ph = phasesOf(*run);
            if (col == ColPre) frac = ph.pre, elapsed = ph.preElapsed, bar = kPreWait;
            else if (col == ColAction) frac = ph.action, elapsed = ph.actionElapsed;
            else frac = ph.post, elapsed = ph.postElapsed, bar = kPostWait;
        }
        const QRectF r = QRectF(opt.rect).adjusted(2, 3, -2, -3);
        if (frac >= 0) {
            QColor back = bar;
            back.setAlphaF(0.18);
            p->fillRect(r, back);
            QColor fill = bar;
            fill.setAlphaF(frac >= 1 ? 0.35 : 0.75);
            p->fillRect(QRectF(r.left(), r.top(), r.width() * frac, r.height()), fill);
        }
        // The post-wait only counts with a follow; a time of zero is quiet
        const bool unused = col == ColPost && next == Sequencer::StepContinue::Wait;
        if (unused || (value == 0 && frac < 0)) ink.setAlphaF(0.4);
        QFont f = opt.font;
        QString text = value < 0 ? QStringLiteral("∞") : seconds(value);
        if (frac >= 0 && frac < 1) { // on its way: the time gone by, counting up
            f.setBold(true);
            text = seconds(elapsed);
            ink = selected ? opt.palette.color(QPalette::HighlightedText) : QColor(Qt::white);
        }
        p->setFont(f);
        p->setPen(ink);
        p->drawText(opt.rect.adjusted(4, 0, -6, 0), Qt::AlignVCenter | Qt::AlignRight, text);
    }

    SequenceWindow *m_w;
};

SequenceWindow::SequenceWindow(Engine *engine, QWidget *parent)
    : QWidget(parent, Qt::Tool | Qt::WindowStaysOnTopHint), m_engine(engine)
{
    setWindowTitle(QStringLiteral("Sequences"));
    resize(1020, 460);
    auto *h = new QHBoxLayout(this);

    // Sequences
    auto *left = new QVBoxLayout;
    left->addWidget(new QLabel(QStringLiteral("<b>Sequences</b>")));
    m_sequences = new QListWidget;
    m_sequences->setToolTip(QStringLiteral("Select which sequence GO / Stop controls. Other sequences keep playing in the background. Double-click to rename."));
    left->addWidget(m_sequences, 1);
    auto *sb = new QHBoxLayout;
    m_add = new QPushButton(QStringLiteral("+"));
    m_add->setToolTip(QStringLiteral("New sequence"));
    m_dup = new QPushButton(QStringLiteral("Duplicate"));
    m_del = new QPushButton(QStringLiteral("−"));
    m_del->setToolTip(QStringLiteral("Delete the sequence"));
    for (QPushButton *b : {m_add, m_dup, m_del}) sb->addWidget(b);
    left->addLayout(sb);
    m_loop = new FlagBox(QStringLiteral("Loop: GO on the last step plays the first"));
    left->addWidget(m_loop);
    h->addLayout(left, 1);

    // Steps
    auto *right = new QVBoxLayout;
    right->addWidget(new QLabel(QStringLiteral("<b>Steps</b> — drag a snapshot or a timeline onto a step (or below the last one to add one)")));
    m_status = new QLabel;
    m_status->setTextFormat(Qt::RichText);
    right->addWidget(m_status);
    right->addWidget(new RecallProgressBar(m_engine));
    m_steps = new QTableWidget(0, ColCount);
    m_steps->setHorizontalHeaderLabels({QStringLiteral("#"), QStringLiteral("Snapshot / Timeline"), QStringLiteral("Pre-wait"),
                                        QStringLiteral("Action"), QStringLiteral("Post-wait"), QStringLiteral("Continue"),
                                        QStringLiteral("Comment"), QStringLiteral("Description")});
    m_steps->horizontalHeaderItem(ColPre)->setToolTip(QStringLiteral("Seconds between the step's GO and its snapshot"));
    m_steps->horizontalHeaderItem(ColAction)->setToolTip(QStringLiteral("The snapshot's time: its fade, or the longest time of its own values\n"
                                                                         "A timeline played: the time it plays, when it ends (endless: 0)"));
    m_steps->horizontalHeaderItem(ColPost)->setToolTip(
        QStringLiteral("Follow: seconds after the snapshot starts before the next step's GO\n"
                       "Auto-follow: seconds after the snapshot ends before the next step's GO\n"
                       "Wait: not used"));
    m_steps->horizontalHeaderItem(ColNext)->setToolTip(
        QStringLiteral("Wait: the next step waits for GO (button, Space, OSC)\n"
                       "Follow: the next step goes once this one is triggered (plus the post-wait), without waiting "
                       "for its snapshot to end\n"
                       "Auto-follow: the next step goes once this one's snapshot is over (plus the post-wait)"));
    m_steps->setItemDelegate(new StepDelegate(this));
    for (int c : {ColPre, ColAction, ColPost}) m_steps->setColumnWidth(c, 70);
    m_steps->setColumnWidth(ColNext, 110);
    m_steps->verticalHeader()->setVisible(false);
    m_steps->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_steps->setEditTriggers(QAbstractItemView::DoubleClicked | QAbstractItemView::EditKeyPressed);
    m_steps->horizontalHeader()->setSectionResizeMode(ColText, QHeaderView::Stretch);
    m_steps->setColumnWidth(ColNum, 40);
    m_steps->setColumnWidth(ColSnapshot, 220);
    m_steps->setColumnWidth(ColDescription, 220);
    m_steps->setObjectName(QStringLiteral("sequenceSteps"));
    m_steps->setAcceptDrops(true);
    m_steps->viewport()->setAcceptDrops(true);
    m_steps->viewport()->installEventFilter(this);
    m_steps->setContextMenuPolicy(Qt::CustomContextMenu);
    m_steps->setToolTip(QStringLiteral("Double-click a number to play that step · double-click a wait, a continuation "
                                       "or a text to edit it · double-click a timeline step: its action · right-click: "
                                       "choose the snapshot or the timeline"));
    right->addWidget(m_steps, 1);
    auto *stb = new QHBoxLayout;
    m_stepAdd = new QPushButton(QStringLiteral("+ Step"));
    m_stepDel = new QPushButton(QStringLiteral("− Delete"));
    m_up = new QPushButton(QStringLiteral("▲"));
    m_down = new QPushButton(QStringLiteral("▼"));
    for (QPushButton *b : {m_stepAdd, m_stepDel, m_up, m_down}) stb->addWidget(b);
    stb->addStretch();
    m_stop = new QPushButton(QStringLiteral("■ Stop the waits"));
    m_stop->setToolTip(QStringLiteral("The selected sequence: its pre-waits and follows still to come are dropped "
                                      "(the snapshots already running go on; the other sequences keep playing)"));
    m_stop->setEnabled(false);
    stb->addWidget(m_stop);
    connect(m_stop, &QPushButton::clicked, this, &SequenceWindow::stopRequested);
    m_stopAll = new QPushButton(QStringLiteral("■■ Stop all"));
    m_stopAll->setToolTip(QStringLiteral("The pre-waits and follows still to come in every sequence, those playing "
                                         "in the background too (the snapshots already running go on)"));
    m_stopAll->setEnabled(false);
    stb->addWidget(m_stopAll);
    connect(m_stopAll, &QPushButton::clicked, this, &SequenceWindow::stopAllRequested);
    auto *timelines = new QPushButton(QStringLiteral("Timelines…"));
    timelines->setToolTip(QStringLiteral("The timelines: drag one from there onto a step"));
    stb->insertWidget(stb->count() - 1, timelines);
    connect(timelines, &QPushButton::clicked, this, &SequenceWindow::timelinesRequested);
    auto *runs = new QTimer(this);
    runs->setInterval(33);
    connect(runs, &QTimer::timeout, this, &SequenceWindow::pollRuns);
    runs->start();
    right->addLayout(stb);
    h->addLayout(right, 3);

    auto current = [this] { return m_engine->sequencer().current(); };
    connect(m_sequences, &QListWidget::currentRowChanged, this, [this](int r) {
        if (!m_filling && r >= 0) m_engine->sequencer().setCurrent(r);
    });
    connect(m_sequences, &QListWidget::itemChanged, this, [this](QListWidgetItem *it) {
        if (m_filling) return;
        const int i = m_sequences->row(it);
        Sequencer::Sequence s = m_engine->sequencer().sequence(i);
        if (s.name == it->text()) return;
        s.name = it->text();
        m_engine->sequencer().setSequence(i, s);
        emit edited();
    });
    connect(m_add, &QPushButton::clicked, this, [this] {
        Sequencer::Sequence s;
        s.name = QStringLiteral("Sequence %1").arg(m_engine->sequencer().count() + 1);
        m_engine->sequencer().setCurrent(m_engine->sequencer().add(s));
        emit edited();
    });
    connect(m_dup, &QPushButton::clicked, this, [this, current] {
        if (current() < 0) return;
        Sequencer::Sequence s = m_engine->sequencer().sequence(current());
        s.name += QStringLiteral(" copy");
        m_engine->sequencer().setCurrent(m_engine->sequencer().add(s, current() + 1));
        emit edited();
    });
    connect(m_del, &QPushButton::clicked, this, [this, current] {
        if (current() < 0) return;
        m_engine->sequencer().remove(current());
        emit edited();
    });
    connect(m_loop, &QCheckBox::toggled, this, [this, current](bool on) {
        if (m_filling || current() < 0) return;
        Sequencer::Sequence s = m_engine->sequencer().sequence(current());
        s.loop = on;
        m_engine->sequencer().setSequence(current(), s);
        emit edited();
    });
    connect(m_steps, &QTableWidget::itemChanged, this, [this, current](QTableWidgetItem *it) {
        if (m_filling || current() < 0) return;
        Sequencer::Sequence s = m_engine->sequencer().sequence(current());
        if (it->row() >= int(s.steps.size())) return;
        Sequencer::Step &st = s.steps[size_t(it->row())];
        switch (it->column()) {
        case ColText: st.text = it->text(); break;
        case ColDescription: st.description = it->text(); break;
        case ColPre: st.preWait = std::clamp(it->data(Qt::EditRole).toDouble(), 0.0, 3600.0); break;
        case ColPost: st.postWait = std::clamp(it->data(Qt::EditRole).toDouble(), 0.0, 3600.0); break;
        case ColNext: st.next = Sequencer::StepContinue(std::clamp(it->data(Qt::EditRole).toInt(), 0, 2)); break;
        default: return;
        }
        m_engine->sequencer().setSequence(current(), s);
        emit edited();
    });
    connect(m_steps, &QTableWidget::cellDoubleClicked, this, [this](int row, int col) {
        if (col == ColNum) emit goToRequested(row);
        if (col == ColSnapshot) {
            const Sequencer::Sequence s = m_engine->sequencer().sequence(m_engine->sequencer().current());
            if (row < int(s.steps.size()) && s.steps[size_t(row)].timeline) {
                const QRect r = m_steps->visualItemRect(m_steps->item(row, col));
                QMenu menu;
                fillActionMenu(&menu, row);
                menu.exec(m_steps->viewport()->mapToGlobal(r.bottomLeft()));
            }
        }
    });
    connect(m_steps, &QTableWidget::customContextMenuRequested, this, [this](const QPoint &pos) {
        const int row = m_steps->rowAt(pos.y());
        QMenu menu;
        QMenu *mem = menu.addMenu(row >= 0 ? QStringLiteral("Snapshot of this step") : QStringLiteral("New step with snapshot"));
        for (int i = 0; i < m_engine->snapshotCount(); ++i) {
            const Engine::Snapshot m = m_engine->snapshot(i);
            mem->addAction(QStringLiteral("%1  %2").arg(i + 1).arg(m.name), this, [this, row, id = m.id] {
                if (row >= 0) setStepSnapshot(row, id);
                else addStep(-1, id);
            });
        }
        mem->setEnabled(!mem->isEmpty());
        QMenu *tl = menu.addMenu(row >= 0 ? QStringLiteral("Timeline of this step") : QStringLiteral("New step with timeline"));
        for (int i = 0; i < m_engine->animationCount(); ++i) {
            const Engine::Animation a = m_engine->animation(i);
            tl->addAction(a.name, this, [this, row, id = a.id] {
                if (row >= 0) setStepTimeline(row, id);
                else addStep(-1, 0, id);
            });
        }
        tl->setEnabled(!tl->isEmpty());
        const Sequencer::Sequence cs = m_engine->sequencer().sequence(m_engine->sequencer().current());
        if (row >= 0 && row < int(cs.steps.size()) && cs.steps[size_t(row)].timeline)
            fillActionMenu(menu.addMenu(QStringLiteral("Timeline action")), row);
        if (row >= 0) {
            menu.addAction(QStringLiteral("Play this step"), this, [this, row] { emit goToRequested(row); });
            menu.addAction(QStringLiteral("Delete"), this, &SequenceWindow::removeSteps);
        }
        menu.exec(m_steps->viewport()->mapToGlobal(pos));
    });
    connect(m_stepAdd, &QPushButton::clicked, this, [this] {
        const int r = m_steps->currentRow();
        addStep(r >= 0 ? r + 1 : -1, 0);
    });
    connect(m_stepDel, &QPushButton::clicked, this, &SequenceWindow::removeSteps);
    connect(m_up, &QPushButton::clicked, this, [this] { moveStep(-1); });
    connect(m_down, &QPushButton::clicked, this, [this] { moveStep(+1); });
    connect(&m_engine->sequencer(), &Sequencer::changed, this, &SequenceWindow::refresh);
    connect(&m_engine->sequencer(), &Sequencer::positionChanged, this, &SequenceWindow::refresh);
    connect(m_engine, &Engine::snapshotsChanged, this, &SequenceWindow::refresh);
    connect(m_engine, &Engine::animationsChanged, this, &SequenceWindow::refresh);
    refresh();
}

// The actions a timeline step can have; the one it has is checked
void SequenceWindow::fillActionMenu(QMenu *menu, int row)
{
    using A = Engine::AnimAction;
    using L = Engine::AnimLoop;
    const Sequencer::Sequence s = m_engine->sequencer().sequence(m_engine->sequencer().current());
    if (row < 0 || row >= int(s.steps.size())) return;
    const Sequencer::Step cur = s.steps[size_t(row)];
    auto set = [this, row](const std::function<void(Sequencer::Step &)> &f) {
        const int c = m_engine->sequencer().current();
        Sequencer::Sequence q = m_engine->sequencer().sequence(c);
        if (row >= int(q.steps.size())) return;
        f(q.steps[size_t(row)]);
        m_engine->sequencer().setSequence(c, q);
        emit edited();
    };
    for (A a : {A::Play, A::Pause, A::Stop, A::Rewind}) {
        Sequencer::Step x;
        x.action = a;
        QAction *act = menu->addAction(actionName(x), this, [set, a] { set([a](Sequencer::Step &st) { st.action = a; }); });
        act->setCheckable(true);
        act->setChecked(cur.action == a);
    }
    QAction *seek = menu->addAction(QStringLiteral("Seek…"), this, [this, set, cur] {
        bool ok = false;
        const double t = QInputDialog::getDouble(this, QStringLiteral("Seek"), QStringLiteral("Go to (seconds played)"),
                                                 cur.seekTime, 0, 360000, 2, &ok);
        if (ok) set([t](Sequencer::Step &st) { st.action = A::Seek; st.seekTime = t; });
    });
    seek->setCheckable(true);
    seek->setChecked(cur.action == A::Seek);
    QAction *speed = menu->addAction(cur.action == A::Speed ? QStringLiteral("Speed…  (× %1)").arg(cur.speed, 0, 'f', 2)
                                                            : QStringLiteral("Speed…"),
                                     this, [this, set, cur] {
        bool ok = false;
        const double v = QInputDialog::getDouble(this, QStringLiteral("Speed"),
                                                 QStringLiteral("Playback speed of the timeline (1 = normal, 0 = frozen)"),
                                                 cur.action == A::Speed ? cur.speed : 1.0, 0, 10, 2, &ok);
        if (ok) set([v](Sequencer::Step &st) { st.action = A::Speed; st.speed = v; });
    });
    speed->setCheckable(true);
    speed->setChecked(cur.action == A::Speed);
    QMenu *loop = menu->addMenu(cur.action == A::LoopMode ? QStringLiteral("Loop mode  (%1)").arg(actionName(cur))
                                                          : QStringLiteral("Loop mode"));
    auto mode = [set](L l, int n) {
        set([l, n](Sequencer::Step &st) {
            st.action = A::LoopMode;
            st.loop = l;
            st.repeat = n;
        });
    };
    loop->addAction(QStringLiteral("Once"), this, [mode] { mode(L::Once, 0); });
    loop->addAction(QStringLiteral("Loop ∞"), this, [mode] { mode(L::Loop, 0); });
    loop->addAction(QStringLiteral("Loop × N…"), this, [this, mode, cur] {
        bool ok = false;
        const int n = QInputDialog::getInt(this, QStringLiteral("Loop"), QStringLiteral("Passes"), std::max(2, cur.repeat), 1, 100000, 1, &ok);
        if (ok) mode(L::Loop, n);
    });
    loop->addAction(QStringLiteral("Ping-pong ∞"), this, [mode] { mode(L::PingPong, 0); });
    loop->addAction(QStringLiteral("Ping-pong × N…"), this, [this, mode, cur] {
        bool ok = false;
        const int n = QInputDialog::getInt(this, QStringLiteral("Ping-pong"), QStringLiteral("Passes"), std::max(2, cur.repeat), 1, 100000, 1, &ok);
        if (ok) mode(L::PingPong, n);
    });
}

// The steps on their way: their time columns are redrawn while they move (and once more when they are done)
void SequenceWindow::pollRuns()
{
    // A sequence can continue in the background while another is edited.
    // Show its activity on the list item without changing the editable name.
    for (int i = 0; i < m_sequences->count(); ++i) {
        QListWidgetItem *item = m_sequences->item(i);
        const bool active = m_engine->sequencer().active(i);
        if (item->data(Qt::UserRole).toBool() == active) continue;
        QSignalBlocker blocker(m_sequences); // styling must not trigger a rename
        item->setData(Qt::UserRole, active);
        QFont font = item->font();
        font.setBold(active);
        item->setFont(font);
        item->setForeground(active ? QColor(90, 215, 155) : palette().color(QPalette::Text));
        item->setToolTip(active ? QStringLiteral("Cues running (including in the background). Select to control this sequence.")
                                : QStringLiteral("Select to edit or GO this sequence. Other sequences keep playing."));
    }
    // Always update Stop: after selecting an idle sequence both run lists may be
    // empty, but the button may still be enabled for the former selection.
    m_stop->setEnabled(m_engine->sequencer().running());
    m_stopAll->setEnabled(m_engine->sequencer().anyActive());
    std::vector<Sequencer::Run> runs = m_engine->sequencer().runs();
    const int position = m_engine->sequencer().position(), upcoming = m_engine->sequencer().next();
    if (runs.empty() && m_runs.empty() && position == m_position && upcoming == m_upcoming) return;
    m_runs = std::move(runs);
    m_position = position;
    m_upcoming = upcoming;
    if (isVisible()) m_steps->viewport()->update();
}

void SequenceWindow::setModified(bool on)
{
    if (on == m_modified) return;
    m_modified = on;
    refresh();
}

void SequenceWindow::refresh()
{
    m_filling = true;
    const int n = m_engine->sequencer().count(), cur = m_engine->sequencer().current();
    m_sequences->clear();
    for (int i = 0; i < n; ++i) {
        auto *it = new QListWidgetItem(m_engine->sequencer().sequence(i).name, m_sequences);
        it->setFlags(it->flags() | Qt::ItemIsEditable);
    }
    m_sequences->setCurrentRow(cur);
    const Sequencer::Sequence s = m_engine->sequencer().sequence(cur);
    m_loop->setChecked(s.loop);
    for (QWidget *w : std::initializer_list<QWidget *>{m_dup, m_del, m_loop, m_stepDel, m_up, m_down}) // steps: a first one makes a sequence
        w->setEnabled(cur >= 0);
    const int keep = m_steps->currentRow();
    m_steps->setRowCount(int(s.steps.size()));
    const int pos = m_engine->sequencer().position();
    m_position = pos;
    m_upcoming = m_engine->sequencer().next();
    for (int k = 0; k < int(s.steps.size()); ++k) {
        const Sequencer::Step &st = s.steps[size_t(k)];
        auto *num = new QTableWidgetItem(QString::number(k + 1));
        num->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        num->setTextAlignment(Qt::AlignCenter);
        auto *mem = new QTableWidgetItem(stepTarget(m_engine, st));
        mem->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        if (targetGone(m_engine, st)) mem->setForeground(QColor(255, 120, 100));
        else if (st.timeline) mem->setForeground(QColor(86, 196, 255));
        auto *text = new QTableWidgetItem(st.text);
        auto *pre = new QTableWidgetItem;
        pre->setData(Qt::EditRole, st.preWait);
        auto *action = new QTableWidgetItem;
        // A timeline played without end: no time to wait for, shown as ∞
        const int ai = st.timeline ? m_engine->indexOfAnimation(st.timeline) : -1;
        const bool endless = ai >= 0 && st.action == Engine::AnimAction::Play && !std::isfinite(m_engine->animation(ai).length());
        const double length = m_engine->sequencer().stepDuration(st);
        action->setData(Qt::EditRole, endless || !std::isfinite(length) ? -1.0 : length); // ∞: a frozen timeline too
        action->setFlags(Qt::ItemIsEnabled | Qt::ItemIsSelectable);
        auto *post = new QTableWidgetItem;
        post->setData(Qt::EditRole, st.postWait);
        auto *next = new QTableWidgetItem;
        next->setData(Qt::EditRole, int(st.next));
        m_steps->setItem(k, ColNum, num);
        m_steps->setItem(k, ColSnapshot, mem);
        m_steps->setItem(k, ColPre, pre);
        m_steps->setItem(k, ColAction, action);
        m_steps->setItem(k, ColPost, post);
        m_steps->setItem(k, ColNext, next);
        m_steps->setItem(k, ColText, text);
        m_steps->setItem(k, ColDescription, new QTableWidgetItem(st.description));
        if (k == pos) { // the step played: green, orange once something changed since
            const QColor c = m_modified ? QColor(QString::fromLatin1(kChanged)) : kLive;
            num->setText(QStringLiteral("▶ %1").arg(k + 1));
            for (QTableWidgetItem *it : {num, mem, text}) {
                QFont f = it->font();
                f.setBold(true);
                it->setFont(f);
                it->setForeground(c);
            }
        }
    }
    if (keep >= 0 && keep < m_steps->rowCount()) m_steps->setCurrentCell(keep, ColSnapshot);
    if (pos >= 0 && pos < int(s.steps.size()))
        m_status->setText(QStringLiteral("Played: <b style='color:%1'>%2. %3</b>%4")
                              .arg((m_modified ? QColor(QString::fromLatin1(kChanged)) : kLive).name())
                              .arg(pos + 1)
                              .arg(stepTarget(m_engine, s.steps[size_t(pos)], false).toHtmlEscaped())
                              .arg(m_modified ? QStringLiteral(" <span style='color:%1'>— changed since it was played</span>")
                                                    .arg(QLatin1String(kChanged))
                                              : QString()));
    else
        m_status->setText(QStringLiteral("<span style='color:#8a8a90'>No step played yet</span>"));
    m_filling = false;
}

void SequenceWindow::addStep(int at, quint64 snapshot, quint64 timeline)
{
    int cur = m_engine->sequencer().current();
    if (cur < 0) { // a first sequence for it
        Sequencer::Sequence s;
        s.name = QStringLiteral("Sequence 1");
        cur = m_engine->sequencer().add(s);
        m_engine->sequencer().setCurrent(cur);
    }
    Sequencer::Sequence s = m_engine->sequencer().sequence(cur);
    if (at < 0 || at > int(s.steps.size())) at = int(s.steps.size());
    Sequencer::Step st;
    st.snapshot = timeline ? 0 : snapshot;
    st.timeline = timeline; // Play, by default
    s.steps.insert(s.steps.begin() + at, st);
    // The step played keeps being the same one
    const int pos = m_engine->sequencer().position();
    m_engine->sequencer().setSequence(cur, s);
    if (pos >= at) m_engine->sequencer().setPosition(pos + 1);
    m_steps->setCurrentCell(at, ColSnapshot);
    emit edited();
}

void SequenceWindow::setStepSnapshot(int step, quint64 snapshot)
{
    const int cur = m_engine->sequencer().current();
    Sequencer::Sequence s = m_engine->sequencer().sequence(cur);
    if (step < 0 || step >= int(s.steps.size())) return;
    s.steps[size_t(step)].snapshot = snapshot;
    s.steps[size_t(step)].timeline = 0;
    m_engine->sequencer().setSequence(cur, s);
    emit edited();
}

void SequenceWindow::setStepTimeline(int step, quint64 timeline)
{
    const int cur = m_engine->sequencer().current();
    Sequencer::Sequence s = m_engine->sequencer().sequence(cur);
    if (step < 0 || step >= int(s.steps.size())) return;
    Sequencer::Step &st = s.steps[size_t(step)];
    if (!st.timeline) st.action = Engine::AnimAction::Play; // a step that recalled a snapshot: Play
    st.timeline = timeline;
    st.snapshot = 0;
    m_engine->sequencer().setSequence(cur, s);
    emit edited();
}

void SequenceWindow::removeSteps()
{
    const int cur = m_engine->sequencer().current();
    Sequencer::Sequence s = m_engine->sequencer().sequence(cur);
    QList<int> rows;
    for (const QModelIndex &i : m_steps->selectionModel()->selectedRows()) rows << i.row();
    if (rows.isEmpty() && m_steps->currentRow() >= 0) rows << m_steps->currentRow();
    if (rows.isEmpty()) return;
    std::sort(rows.begin(), rows.end(), std::greater<int>());
    int pos = m_engine->sequencer().position();
    for (int r : rows) {
        if (r < 0 || r >= int(s.steps.size())) continue;
        s.steps.erase(s.steps.begin() + r);
        if (pos == r) pos = r - 1; // the step played is gone: as if the one before was
        else if (pos > r) --pos;
    }
    m_engine->sequencer().setSequence(cur, s);
    m_engine->sequencer().setPosition(pos);
    emit edited();
}

void SequenceWindow::moveStep(int delta)
{
    const int cur = m_engine->sequencer().current();
    Sequencer::Sequence s = m_engine->sequencer().sequence(cur);
    const int r = m_steps->currentRow(), to = r + delta;
    if (r < 0 || to < 0 || to >= int(s.steps.size())) return;
    std::swap(s.steps[size_t(r)], s.steps[size_t(to)]);
    int pos = m_engine->sequencer().position();
    if (pos == r) pos = to;
    else if (pos == to) pos = r;
    m_engine->sequencer().setSequence(cur, s);
    m_engine->sequencer().setPosition(pos);
    m_steps->setCurrentCell(to, ColSnapshot);
    emit edited();
}

// A snapshot or a timeline dragged from its list: onto a step, it becomes what the step does (a timeline: Play); below
// the last one, a new step
bool SequenceWindow::eventFilter(QObject *o, QEvent *e)
{
    if (o != m_steps->viewport()) return QWidget::eventFilter(o, e);
    if (e->type() == QEvent::DragEnter || e->type() == QEvent::DragMove) {
        auto *d = static_cast<QDragMoveEvent *>(e);
        if (!d->mimeData()->hasFormat(kSnapshotMime) && !d->mimeData()->hasFormat(kTimelineMime)) return false;
        d->acceptProposedAction();
        return true;
    }
    if (e->type() == QEvent::Drop) {
        auto *d = static_cast<QDropEvent *>(e);
        const bool timeline = d->mimeData()->hasFormat(kTimelineMime);
        if (!d->mimeData()->hasFormat(kSnapshotMime) && !timeline) return false;
        const quint64 id = d->mimeData()->data(timeline ? kTimelineMime : kSnapshotMime).toULongLong();
        const int row = m_steps->rowAt(d->position().toPoint().y());
        if (row >= 0) timeline ? setStepTimeline(row, id) : setStepSnapshot(row, id);
        else timeline ? addStep(-1, 0, id) : addStep(-1, id);
        d->acceptProposedAction();
        return true;
    }
    return QWidget::eventFilter(o, e);
}
