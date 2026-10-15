#pragma once
#include "Engine.h"
#include <QAbstractScrollArea>
#include <QTimer>

// One painted viewport for any number of timelines; no widgets or keyframe copies per row.
class TimelineActivity : public QAbstractScrollArea {
    Q_OBJECT
public:
    explicit TimelineActivity(Engine *engine, QWidget *parent = nullptr);
    void poll();
    void setShowStopped(bool show);
    const std::vector<Engine::AnimationPlayback> &rows() const { return m_rows; }
    QString describe() const;
signals:
    void editRequested(quint64 timeline);
protected:
    void paintEvent(QPaintEvent *) override;
    void mousePressEvent(QMouseEvent *) override;
    void mouseDoubleClickEvent(QMouseEvent *) override;
    void resizeEvent(QResizeEvent *) override;
    void showEvent(QShowEvent *) override;
    void keyPressEvent(QKeyEvent *) override;
private:
    int rowAt(const QPoint &point) const;
    void select(int row);
    void updateRange();
    Engine *m_engine;
    QTimer m_timer;
    std::vector<Engine::AnimationPlayback> m_rows;
    quint64 m_selected = 0;
    bool m_showStopped = false;
    int m_playing = 0, m_paused = 0, m_total = 0;
};
