#pragma once
#include <QHash>
#include <QWidget>
class QTableWidget;
class QToolButton;
class LogPanel final : public QWidget {
    Q_OBJECT
public:
    explicit LogPanel(QWidget *parent = nullptr);
    void appendEvent(const QString &channel, const QString &message);
private:
    QTableWidget *m_table = nullptr;
    QHash<QString, QToolButton *> m_filters;
};
