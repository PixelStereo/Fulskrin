#pragma once
// The Logs panel: the events of EventLog (OSC, MIDI, keyboard), shown at most 10 times a second whatever the traffic.
// Its rows are a model over the ring, updated by sequence number (added at the end, removed at the start, the last
// one changed when a repeat merged into it): no row rebuilt, no allocation per message. It follows the end only while
// the view is at the end: an operator reading older lines is not pulled down.
#include <QHash>
#include <QTimer>
#include <QWidget>
#include <vector>

#include "EventLog.h"
#include "LogModel.h"

class QLabel;
class QTableView;
class QToolButton;

class LogPanel final : public QWidget
{
    Q_OBJECT
public:
    explicit LogPanel(QWidget *parent = nullptr, EventLog *log = nullptr);
    void appendEvent(const QString &channel, const QString &message); // into the log (kept for older callers)
    void refresh();                                                   // what the ring holds now (the timer does)

private:
    EventLog *m_log;
    LogModel *m_model = nullptr;
    QTableView *m_view = nullptr;
    QLabel *m_status = nullptr;
    QHash<QString, QToolButton *> m_filters;
    QTimer m_timer;
    quint64 m_seen = ~0ull; // the ring's generation shown
};
