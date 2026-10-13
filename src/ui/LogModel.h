#pragma once
// The Logs panel's rows: a model over EventLog's ring (Core only: the tests use it), updated by sequence number.
#include <QAbstractTableModel>
#include <vector>

#include "EventLog.h"

class LogModel final : public QAbstractTableModel
{
    Q_OBJECT
public:
    explicit LogModel(QObject *parent = nullptr) : QAbstractTableModel(parent) {}
    int rowCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : int(m_rows.size()); }
    int columnCount(const QModelIndex &parent = {}) const override { return parent.isValid() ? 0 : 3; }
    QVariant data(const QModelIndex &index, int role) const override;
    QVariant headerData(int section, Qt::Orientation o, int role) const override;
    // The ring's lines now (oldest first): rows removed, changed and added to match. True: rows were added
    bool update(const std::vector<EventLog::Entry> &now);

private:
    std::vector<EventLog::Entry> m_rows;
};
