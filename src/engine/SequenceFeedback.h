#pragma once
// The state of the selected sequence sent by OSC (UDP) to one or more addresses, for a remote interface (Bitfocus
// Companion, TouchOSC, an iPhone OSC app…). The commands come back to the OSC server: /sequence/go, /sequence/back,
// /sequence/stop, /sequence/stop_all, /sequence/step <n>.
//
// Sent at each cue (GO, GO BACK, a follow, an edit) and, while a step's time goes by, ten times a second — only the
// values that changed; everything again every 2 seconds, for an interface started late.
//
//   /sequence/name s                 the selected sequence
//   /sequence/index i                its number (1 = first, 0: none)
//   /sequence/steps i                how many steps it has
//   /sequence/running i              1: a wait or a follow is on its way (STOP has something to stop)
//   /sequence/go_enabled i           1: GO plays a step
//   /sequence/back_enabled i         1: GO BACK plays a step
//   /sequence/modified i             1: something changed since the step was played
// For each of /sequence/cue/previous, /sequence/cue/current (the step played) and /sequence/cue/next (the one GO
// plays):
//   …/number i                       the step's number (0: none)
//   …/name s                         what it recalls (its snapshot, or its timeline and action)
//   …/label s                        "2 · Name", ready for a button
//   …/text s, …/description s        the step's text and its description
//   …/prewait f, …/action f, …/postwait f   its times in seconds (action: -1 when endless)
//   …/continue s                     "wait", "follow" or "auto-follow"
//   …/phase s                        where its time is: "pre-wait", "action", "post-wait", "done" ("" before its GO)
//   …/elapsed f, …/elapsed_text s    seconds since its GO ("1:05.0")
//   …/remaining f, …/remaining_text s   seconds left until its end (pre-wait + action, and the post-wait of a follow)
//   …/progress f, …/percent i        how far it is from its GO to its end, 0..1 and 0..100
//   …/phase_progress f, …/phase_percent i   how far it is in its phase

#include <QHostAddress>
#include <QHash>
#include <QObject>
#include <QStringList>
#include <QTimer>
#include <QVariant>
#include <vector>

#include "OscCodec.h"

class Engine;
class QUdpSocket;

class SequenceFeedback : public QObject
{
    Q_OBJECT
public:
    explicit SequenceFeedback(Engine *engine, QObject *parent = nullptr);

    // "host:port" each ("localhost:11111", "192.168.1.20:9000"); names are resolved once, in the background
    void setTargets(const QStringList &targets);
    void setEnabled(bool on);
    bool isEnabled() const { return m_enabled; }
    void setModified(bool on); // something changed since the step was played (the interface knows it)

    static QStringList defaultTargets() { return {QStringLiteral("127.0.0.1:11111")}; }
    // A "host:port" target: false if it is not one
    static bool parseTarget(const QString &target, QString *host, quint16 *port);

    // Every value of the state now (tests, and what is sent)
    std::vector<osc::Message> state() const;
    void sendNow(bool everything); // sends the values that changed (all of them: everything)

signals:
    void sent(int messages); // tests

private:
    void tick();
    Engine *m_e;
    QUdpSocket *m_socket = nullptr;
    struct Target {
        QString host;
        quint16 port = 0;
        QHostAddress address; // null until resolved
    };
    std::vector<Target> m_targets;
    bool m_enabled = false, m_modified = false;
    QTimer m_timer;
    int m_ticks = 0;
    QHash<QString, QVariantList> m_sent; // last value sent per address
};
