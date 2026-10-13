#include "LogModel.h"

#include <QDateTime>
#include <algorithm>
#include <cstddef>

QVariant LogModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() >= int(m_rows.size())) return {};
    const EventLog::Entry &e = m_rows[size_t(index.row())];
    if (role == Qt::DisplayRole) {
        switch (index.column()) {
        case 0: return QDateTime::fromMSecsSinceEpoch(e.time).toString(QStringLiteral("HH:mm:ss.zzz"));
        case 1: return EventLog::channelName(e.channel);
        case 2: return e.repeats > 1 ? QStringLiteral("%1   (×%2)").arg(e.message).arg(e.repeats) : e.message;
        default: return {};
        }
    }
    if (role == Qt::ToolTipRole && index.column() == 2 && e.repeats > 1)
        return QStringLiteral("%1 messages on this address within a second of each other: the last one shown").arg(e.repeats);
    return {};
}

QVariant LogModel::headerData(int section, Qt::Orientation o, int role) const
{
    if (o != Qt::Horizontal || role != Qt::DisplayRole) return {};
    static const char *const names[] = {"Time", "Channel", "Event"};
    return section >= 0 && section < 3 ? QString::fromLatin1(names[section]) : QVariant();
}

bool LogModel::update(const std::vector<EventLog::Entry> &now)
{
    // Rows gone from the ring: at the start
    size_t gone = 0;
    while (gone < m_rows.size() && (now.empty() || m_rows[gone].seq < now.front().seq)) ++gone;
    if (gone) {
        beginRemoveRows({}, 0, int(gone) - 1);
        m_rows.erase(m_rows.begin(), m_rows.begin() + std::ptrdiff_t(gone));
        endRemoveRows();
    }
    // Rows kept: the same lines in the same order, the last one perhaps merged with repeats since
    const size_t kept = std::min(m_rows.size(), now.size());
    for (size_t k = 0; k < kept; ++k)
        if (m_rows[k].seq != now[k].seq) { // not what was expected (cleared…): all again
            beginResetModel();
            m_rows = now;
            endResetModel();
            return true;
        }
    int firstChanged = -1, lastChanged = -1;
    for (size_t k = 0; k < kept; ++k) {
        EventLog::Entry &r = m_rows[k];
        if (r.repeats == now[k].repeats && r.time == now[k].time) continue;
        r = now[k];
        if (firstChanged < 0) firstChanged = int(k);
        lastChanged = int(k);
    }
    if (firstChanged >= 0) emit dataChanged(index(firstChanged, 0), index(lastChanged, 2));
    if (now.size() <= m_rows.size()) return false;
    beginInsertRows({}, int(m_rows.size()), int(now.size()) - 1);
    m_rows.insert(m_rows.end(), now.begin() + std::ptrdiff_t(m_rows.size()), now.end());
    endInsertRows();
    return true;
}
