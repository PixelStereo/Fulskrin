// Deterministic widget regression: poll the actual mini sequencer at synthetic cue times.
#include "SequencePanel.h"
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
#include <QJsonArray>
#include <QThread>
#include <cstdio>
int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    app.setApplicationName("PinolSequenceUITest");
    Engine engine;
    Sequencer::Sequence sequence;
    Sequencer::Step first, second, third;
    first.preWait = 2;
    first.postWait = 4;
    first.next = Sequencer::StepContinue::AutoFollow;
    second.description = QString(2000, QChar('A'));
    second.text = QString(2000, QChar('B'));
    sequence.steps = {first, second, third};
    engine.sequencer().add(sequence);
    engine.sequencer().setCurrent(0);
    SequenceBar bar(&engine);
    auto *label = bar.findChild<QLabel *>("sequenceWaitProgress");
    auto *poller = bar.findChild<QTimer *>("sequenceWaitPoller");
    int failures = 0;
    auto check = [&](bool ok, const char *reason) { if (!ok) { ++failures; std::printf("FAIL sequence_ui: %s\n", reason); } };
    check(label && poller, "mini wait controls present");
    if (!label || !poller) return 1;
    poller->stop();
    auto poll = [&] { QMetaObject::invokeMethod(poller, "timeout", Qt::DirectConnection); };
    bar.resize(650, bar.height()); bar.show(); app.processEvents();
    const QSize fixedSize = bar.size();
    check(bar.minimumHeight() == bar.maximumHeight(), "mini sequencer height is fixed");
    check(engine.sequencer().toJson()[0].toObject()["steps"].toArray()[1].toObject()["description"].toString() == second.description, "description serialized independently of comment");
    engine.sequencer().go(); poll();
    check(label->text().contains("PRE-WAIT") && label->text().contains("0%"), "pre-wait starts at zero");
    engine.sequencer().advance(1); poll();
    check(label->text().contains("PRE-WAIT") && label->text().contains("50%"), "pre-wait progress halfway");
    engine.sequencer().advance(1); poll();
    check(label->text().contains("POST-WAIT") && label->text().contains("0%"), "post-wait starts after trigger");
    engine.sequencer().advance(2); poll();
    check(label->text().contains("POST-WAIT") && label->text().contains("50%"), "post-wait progress halfway");
    engine.sequencer().advance(2); poll();
    check(label->text().contains("press GO"), "manual wait asks for GO after follow");
    for (auto *button : bar.findChildren<QPushButton *>())
        check(button->text() != "Sequences…" && button->text() != "Timelines…", "no editor launcher below preview");
    app.processEvents();
    check(bar.size() == fixedSize, "long comments and waits do not resize mini sequencer");
    auto *description = bar.findChild<QLabel *>("nextCueDescription");
    check(description != nullptr, "next cue description field exists");
    auto *timing = bar.findChild<QLabel *>("nextCueTiming");
    check(timing && timing->text().contains("PRE") && timing->text().contains("POST"), "next cue action and wait durations shown");
    QThread::msleep(20); check(engine.sequencer().stepElapsed(0) > 0, "elapsed clock survives cue completion");
    engine.sequencer().stopAll(); poll();
    check(!label->text().contains("POST-WAIT") && !label->text().contains("PRE-WAIT"), "stop removes old wait progress");
    return failures ? 1 : 0;
}
