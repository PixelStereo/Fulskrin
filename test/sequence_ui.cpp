// Deterministic widget regression: poll the actual mini sequencer at synthetic cue times.
#include "SequencePanel.h"
#include <QApplication>
#include <QLabel>
#include <QPushButton>
#include <QTimer>
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
    engine.sequencer().stopAll(); poll();
    check(!label->text().contains("POST-WAIT") && !label->text().contains("PRE-WAIT"), "stop removes old wait progress");
    return failures ? 1 : 0;
}
