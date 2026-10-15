#pragma once
#include "Engine.h"
#include <QWidget>

class Engine;
class QComboBox;
class QLabel;
class QListWidget;
class QPushButton;
class QTableWidget;
class QCheckBox;
class QTimer;
class QMenu;

// Where the snapshot recalled last is in its fade: a thin bar filling over its time (green while it runs), with the
// snapshot's name and the seconds as tooltip. Follows the engine by itself.
class RecallProgressBar : public QWidget
{
    Q_OBJECT
public:
    explicit RecallProgressBar(Engine *engine, QWidget *parent = nullptr);
    QSize sizeHint() const override { return QSize(100, 5); }
    QSize minimumSizeHint() const override { return QSize(10, 5); }

protected:
    void paintEvent(QPaintEvent *) override;

private:
    void poll();
    Engine *m_engine;
    double m_fraction = 1;
    bool m_running = false;
};

// The steps of the selected sequence from top to bottom, as in QLab: the one before (dimmed), the one played (a
// thin green frame, orange once something changed since it was played), the one GO plays (a thin red frame), then
// those after it. The time going by fills each step's frame at half opacity: its pre-wait (yellow), its action
// (green), its post-wait (blue). Each step: its number and target, its text, its times and what comes next.
class CueStack : public QWidget
{
    Q_OBJECT
public:
    enum Role { Previous, Current, Next, Later };
    struct Row {
        Role role = Later;
        int step = -1;       // -1: a placeholder (no step yet, the end)
        QString title;       // "2  Snapshot name"
        QString text;        // the step's text (comment)
        QString note;        // its description, for the operator
        QString timing;      // "PRE 2.0  ACTION 3.0  POST 4.0 s · Auto-follow"
        QString status;      // where its time is: "PRE-WAIT · 1.0 s left · 50%", "press GO"…
        QString aside;       // the step played: the time since its GO
        double pre = -1, action = -1, post = -1; // its times on their way (0..1), -1: not now
    };
    explicit CueStack(Engine *engine, QWidget *parent = nullptr);
    void rebuild();            // from the sequencer (structure and times)
    void setModified(bool on); // something changed since the step was played
    const std::vector<Row> &rows() const { return m_rows; }
    QString describe() const;  // every text drawn, one row per line

protected:
    void paintEvent(QPaintEvent *) override;
    void resizeEvent(QResizeEvent *) override { rebuild(); }

private:
    Engine *m_engine;
    std::vector<Row> m_rows;
    bool m_modified = false;
    QString m_drawn; // what is drawn: no repaint while nothing moves
};

// The reduced view of the sequences, under the preview: the sequence and its buttons on the left, one above the
// other (GO, GO BACK, STOP — the waits still to come —, STOP ALL), and its steps from top to bottom (CueStack).
// SequenceWindow: a floating window, always in front, to edit everything: the sequences (new, duplicate, delete,
// rename, loop), their steps (+, delete, move, a snapshot or a timeline dragged onto a step, a timeline step's action,
// its text), and to play any step. The step played and the one GO plays have the same thin frames as in the bar.
class SequenceBar : public QWidget
{
    Q_OBJECT
public:
    explicit SequenceBar(Engine *engine, QWidget *parent = nullptr);
    void refresh();
    void setModified(bool on); // something changed since the current step was played

signals:
    void goRequested();     // GO: the next step
    void backRequested();   // GO BACK: the previous step
    void stopRequested();   // the pre-waits and follows still to come
    void stopAllRequested(); // those of every sequence, the ones playing in the background too
    void edited();

private:
    Engine *m_engine;
    QComboBox *m_sequence;
    QPushButton *m_go, *m_back, *m_stop, *m_stopAll;
    CueStack *m_stack;
    void pollWaits();
    bool m_filling = false;
};

class SequenceWindow : public QWidget
{
    Q_OBJECT
public:
    explicit SequenceWindow(Engine *engine, QWidget *parent = nullptr);
    void refresh();
    void setModified(bool on); // something changed since the current step was played

signals:
    void goToRequested(int step); // play that step of the current sequence
    void timelinesRequested();    // the timelines' window
    void stopRequested();         // the pre-waits and follows still to come
    void stopAllRequested();      // those of every sequence, the ones playing in the background too
    void edited();                // the sequences changed: the project is modified

protected:
    bool eventFilter(QObject *o, QEvent *e) override; // snapshots dropped onto the steps

private:
    void addStep(int at, quint64 snapshot, quint64 timeline = 0);
    void setStepSnapshot(int step, quint64 snapshot);
    void setStepTimeline(int step, quint64 timeline); // Play, unless it was already a timeline step
    void fillActionMenu(QMenu *menu, int row);
    void removeSteps();
    void moveStep(int delta);
    Engine *m_engine;
    QListWidget *m_sequences;
    QCheckBox *m_loop;
    QTableWidget *m_steps;
    QLabel *m_status; // the step played, and whether something changed since
    bool m_modified = false;
    QPushButton *m_add, *m_dup, *m_del, *m_stepAdd, *m_stepDel, *m_up, *m_down, *m_stop, *m_stopAll;
    bool m_filling = false;
    std::vector<Sequencer::Run> m_runs; // the steps on their way, drawn in their time columns
    int m_position = -1, m_upcoming = -1; // the step played (green frame) and the one GO plays (red frame)
    void pollRuns();
    friend class StepDelegate;
};
