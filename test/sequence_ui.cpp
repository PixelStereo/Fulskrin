// Deterministic widget regression: poll the actual mini sequencer (its steps from top to bottom) at synthetic cue times.
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
    auto *stack = bar.findChild<CueStack *>("sequenceCueStack");
    auto *poller = bar.findChild<QTimer *>("sequenceWaitPoller");
    int failures = 0;
    auto check = [&](bool ok, const char *reason) { if (!ok) { ++failures; std::printf("FAIL sequence_ui: %s\n", reason); } };
    check(stack && poller, "mini wait controls present");
    if (!stack || !poller) return 1;
    // The row of a role: what it shows
    auto row = [&](CueStack::Role role) {
        for (const CueStack::Row &r : stack->rows())
            if (r.role == role) return r;
        return CueStack::Row();
    };
    poller->stop();
    auto poll = [&] { QMetaObject::invokeMethod(poller, "timeout", Qt::DirectConnection); };
    bar.resize(650, bar.height()); bar.show(); app.processEvents();
    const QSize fixedSize = bar.size();
    check(bar.minimumHeight() == bar.maximumHeight(), "mini sequencer height is fixed");
    check(engine.sequencer().toJson()[0].toObject()["steps"].toArray()[1].toObject()["description"].toString() == second.description, "description serialized independently of comment");
    engine.sequencer().go(); poll();
    check(stack->describe().contains("PRE-WAIT") && stack->describe().contains("0%"), "pre-wait starts at zero");
    const double elapsedBeforeEdit = engine.sequencer().stepElapsed(0);
    auto editedSequence = engine.sequencer().sequence(0);
    editedSequence.steps[0].description = "Edited while running";
    engine.sequencer().setSequence(0, editedSequence);
    check(engine.sequencer().stepElapsed(0) >= elapsedBeforeEdit, "editing a cue preserves its elapsed clock");
    engine.sequencer().advance(1); poll();
    check(stack->describe().contains("PRE-WAIT") && stack->describe().contains("50%"), "pre-wait progress halfway");
    engine.sequencer().advance(1); poll();
    check(stack->describe().contains("POST-WAIT") && stack->describe().contains("0%"), "post-wait starts after trigger");
    engine.sequencer().advance(2); poll();
    check(stack->describe().contains("POST-WAIT") && stack->describe().contains("50%"), "post-wait progress halfway");
    engine.sequencer().advance(2); poll();
    check(stack->describe().contains("press GO"), "manual wait asks for GO after follow");
    for (auto *button : bar.findChildren<QPushButton *>())
        check(button->text() != "Sequences…" && button->text() != "Timelines…", "no editor launcher below preview");
    app.processEvents();
    check(bar.size() == fixedSize, "long comments and waits do not resize mini sequencer");
    check(row(CueStack::Current).step == 1 && row(CueStack::Next).step == 2, "steps from top to bottom: played, then next");
    check(row(CueStack::Current).note == second.description, "the played step shows its description");
    check(row(CueStack::Previous).step == 0, "the step before is shown above");
    check(row(CueStack::Next).timing.contains("PRE") && row(CueStack::Next).timing.contains("POST"), "next cue action and wait durations shown");
    for (auto *button : bar.findChildren<QPushButton *>())
        check(button->width() <= 124 + 1 && button->x() < 140, "buttons on the left, one above the other");
    QThread::msleep(20); check(engine.sequencer().stepElapsed(0) > 0, "elapsed clock survives cue completion");
    engine.sequencer().stopAll(); poll();
    check(!stack->describe().contains("POST-WAIT") && !stack->describe().contains("PRE-WAIT"), "stop removes old wait progress");
    return failures ? 1 : 0;
}
