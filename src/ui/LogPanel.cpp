#include "LogPanel.h"

#include <QDateTime>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QPushButton>
#include <QScrollBar>
#include <QTableView>
#include <QToolButton>
#include <QVBoxLayout>

LogPanel::LogPanel(QWidget *parent, EventLog *log) : QWidget(parent), m_log(log ? log : &EventLog::instance())
{
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    auto *filters = new QHBoxLayout;
    for (int c = 0; c < EventLog::ChannelCount; ++c) {
        const QString channel = EventLog::channelName(c);
        auto *button = new QToolButton(this);
        button->setText(channel);
        button->setCheckable(true);
        button->setChecked(m_log->wants(c));
        button->setToolTip(QStringLiteral("Show or hide %1 events (hidden: not even recorded)").arg(channel));
        connect(button, &QToolButton::toggled, this, [this, c](bool on) { m_log->setWanted(c, on); });
        m_filters.insert(channel, button);
        filters->addWidget(button);
    }
    auto *clear = new QPushButton(QStringLiteral("Clear"), this);
    filters->addWidget(clear);
    filters->addStretch(1);
    m_status = new QLabel(this);
    filters->addWidget(m_status);
    layout->addLayout(filters);

    m_model = new LogModel(this);
    m_view = new QTableView(this);
    m_view->setModel(m_model);
    m_view->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_view->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_view->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_view->verticalHeader()->hide();
    m_view->verticalHeader()->setSectionResizeMode(QHeaderView::Fixed); // no row measured: rows stay cheap
    m_view->verticalHeader()->setDefaultSectionSize(m_view->fontMetrics().height() + 4);
    m_view->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_view->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_view->setAlternatingRowColors(true);
    m_view->setWordWrap(false);
    layout->addWidget(m_view, 1);
    connect(clear, &QPushButton::clicked, this, [this] {
        m_log->clear();
        refresh();
    });

    // 10 times a second at most, and only when the ring changed
    m_timer.setInterval(100);
    connect(&m_timer, &QTimer::timeout, this, &LogPanel::refresh);
    m_timer.start();
}

void LogPanel::appendEvent(const QString &channel, const QString &message) { m_log->append(channel, message); }

void LogPanel::refresh()
{
    const quint64 g = m_log->generation();
    if (g == m_seen) return;
    m_seen = g;
    QScrollBar *bar = m_view->verticalScrollBar();
    const bool atEnd = bar->value() >= bar->maximum() - 2; // following the end (or nothing to scroll yet)
    const bool added = m_model->update(m_log->entries());
    if (added && atEnd) m_view->scrollToBottom();
    const EventLog::Counters c = m_log->counters();
    QStringList info;
    if (c.merged) info << QStringLiteral("%1 repeats merged").arg(c.merged);
    if (c.discarded) info << QStringLiteral("%1 over the rate, not kept").arg(c.discarded);
    if (c.overwritten) info << QStringLiteral("%1 older lines out").arg(c.overwritten);
    m_status->setText(info.join(QStringLiteral(" · ")));
    m_status->setToolTip(QStringLiteral("%1 events received").arg(c.received));
}
