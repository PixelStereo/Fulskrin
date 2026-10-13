#include "LogPanel.h"
#include <QDateTime>
#include <QHeaderView>
#include <QHBoxLayout>
#include <QPushButton>
#include <QTableWidget>
#include <QToolButton>
#include <QVBoxLayout>
LogPanel::LogPanel(QWidget *parent) : QWidget(parent) {
    auto *layout = new QVBoxLayout(this);
    layout->setContentsMargins(6, 6, 6, 6);
    auto *filters = new QHBoxLayout;
    const QStringList channels = {QStringLiteral("OSC IN"), QStringLiteral("OSC OUT"), QStringLiteral("MIDI IN"), QStringLiteral("MIDI OUT"), QStringLiteral("KEYBOARD")};
    for (const QString &channel : channels) {
        auto *button = new QToolButton(this);
        button->setText(channel); button->setCheckable(true); button->setChecked(true);
        button->setToolTip(QStringLiteral("Show or hide %1 events").arg(channel));
        m_filters.insert(channel, button); filters->addWidget(button);
    }
    auto *clear = new QPushButton(QStringLiteral("Clear"), this);
    filters->addWidget(clear); filters->addStretch(1); layout->addLayout(filters);
    m_table = new QTableWidget(0, 3, this);
    m_table->setHorizontalHeaderLabels({QStringLiteral("Time"), QStringLiteral("Channel"), QStringLiteral("Event")});
    m_table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(1, QHeaderView::ResizeToContents);
    m_table->horizontalHeader()->setSectionResizeMode(2, QHeaderView::Stretch);
    m_table->setEditTriggers(QAbstractItemView::NoEditTriggers);
    m_table->setSelectionBehavior(QAbstractItemView::SelectRows);
    m_table->setAlternatingRowColors(true); m_table->verticalHeader()->hide();
    layout->addWidget(m_table, 1);
    connect(clear, &QPushButton::clicked, this, [this] { m_table->setRowCount(0); });
}
void LogPanel::appendEvent(const QString &channel, const QString &message) {
    auto it = m_filters.find(channel);
    if (it == m_filters.end() || !it.value()->isChecked()) return;
    constexpr int maxRows = 2000;
    if (m_table->rowCount() >= maxRows) m_table->removeRow(0);
    const int row = m_table->rowCount(); m_table->insertRow(row);
    m_table->setItem(row, 0, new QTableWidgetItem(QDateTime::currentDateTime().toString(QStringLiteral("HH:mm:ss.zzz"))));
    m_table->setItem(row, 1, new QTableWidgetItem(channel));
    m_table->setItem(row, 2, new QTableWidgetItem(message));
    m_table->scrollToBottom();
}
