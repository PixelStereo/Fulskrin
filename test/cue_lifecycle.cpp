// Deterministic command-queue and reentrant cancellation regressions.
#include "Engine.h"
#include "Sequencer.h"
#include <QCoreApplication>
#include <QEvent>
#include <cstdio>
#include <thread>

int runCueLifecycleTests(Engine &engine)
{
    int failures = 0;
    auto check = [&](bool ok, const char *reason) {
        if (!ok) { ++failures; std::printf("FAIL cue_lifecycle: %s\n", reason); }
    };
    engine.newProject();
    engine.setFadesManual(true);
    auto &seq = engine.sequencer();
    std::vector<int> fired;
    for (int i = 0; i < 3; ++i) {
        Engine::Snapshot snapshot; snapshot.name = QString::number(i);
        engine.addSnapshot(snapshot);
    }
    auto reset = [&] {
        seq.clear(); fired.clear();
        seq.setRecaller([&](int index) { fired.push_back(index); });
        Sequencer::Sequence s;
        for (int i = 0; i < 3; ++i) {
            Sequencer::Step step; step.snapshot = engine.snapshot(i).id;
            s.steps.push_back(step);
        }
        seq.add(s); seq.setCurrent(0);
    };
    auto drain = [&] { QCoreApplication::sendPostedEvents(&seq, QEvent::MetaCall); };
    reset();
    std::thread burst([&] { seq.go(); seq.go(); seq.go(); }); burst.join();
    check(fired.empty(), "worker commands wait for the owner thread");
    drain();
    check(fired == std::vector<int>({0, 1, 2}), "three queued GO commands advance three times");
    std::thread backs([&] { seq.back(); seq.back(); }); backs.join(); drain();
    check(fired == std::vector<int>({0, 1, 2, 1, 0}), "queued BACK commands resolve in execution order");

    reset();
    std::thread canceled([&] { seq.go(); seq.goTo(2); }); canceled.join();
    seq.stopAll(); drain();
    check(fired.empty() && !seq.anyActive(), "STOP invalidates queued relative and absolute commands");
    seq.go(); check(fired == std::vector<int>({0}), "a deliberate GO after STOP still works");

    reset();
    const int other = seq.add(seq.sequence(0));
    std::thread independent([&] { seq.goSequence(0); seq.goSequence(other); }); independent.join();
    seq.stopSequence(0); seq.setCurrent(other); drain();
    check(fired == std::vector<int>({0}) && seq.positionOf(0) == -1 && seq.positionOf(other) == 0,
          "selected STOP preserves another sequence's queued GO and original owner");

    reset();
    auto s = seq.sequence(0);
    s.steps[0].preWait = 1; s.steps[0].next = Sequencer::StepContinue::Follow;
    s.steps[0].postWait = 1;
    seq.setSequence(0, s); seq.go();
    s = seq.sequence(0);
    Sequencer::Step inserted; inserted.snapshot = engine.snapshot(2).id;
    s.steps.insert(s.steps.begin(), inserted); seq.setSequence(0, s);
    check(seq.position() == 1 && seq.runs()[0].step == 1, "insertion rebinds the active cue and playhead");
    seq.advance(1); seq.advance(1);
    check(fired == std::vector<int>({0, 1}), "insertion does not replay the active cue as its own follow");

    reset(); s = seq.sequence(0); s.steps[0].preWait = 1;
    seq.setSequence(0, s); seq.go();
    s = seq.sequence(0); s.steps.erase(s.steps.begin()); seq.setSequence(0, s);
    seq.advance(2);
    check(fired.empty(), "deleting a waiting cue cancels its old snapshot");

    reset();
    std::thread absolute([&] { seq.goTo(1); }); absolute.join();
    s = seq.sequence(0); std::swap(s.steps[0], s.steps[1]); seq.setSequence(0, s); drain();
    check(fired == std::vector<int>({1}) && seq.position() == 0, "queued absolute cue follows its identity through reorder");

    reset(); s = seq.sequence(0);
    s.steps[0].preWait = 1; s.steps[0].next = Sequencer::StepContinue::Follow;
    seq.setSequence(0, s);
    seq.setRecaller([&](int index) { fired.push_back(index); seq.stopAll(); });
    seq.go(); seq.advance(1); seq.advance(10);
    check(fired == std::vector<int>({0}) && !seq.anyActive(), "STOP during recall cancels its already prepared follow");

    reset(); s = seq.sequence(0); s.steps[0].preWait = 1; s.steps[1].preWait = 1;
    seq.setSequence(0, s); seq.go(); seq.go();
    seq.setRecaller([&](int index) { fired.push_back(index); seq.stopAll(); });
    seq.advance(1);
    check(fired == std::vector<int>({0}), "STOP during first recall cancels remaining actions in the same batch");

    seq.setRecaller(nullptr); seq.clear(); engine.setFadesManual(false); engine.newProject();
    return failures;
}
