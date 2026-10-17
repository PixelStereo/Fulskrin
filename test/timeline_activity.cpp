#include "Engine.h"
#include "SequencePanel.h"
#include "TimelineActivity.h"
#include <QApplication>
#include <QElapsedTimer>
#include <QTableWidget>
#include <QMouseEvent>
#include <QScrollBar>
#include <QTimer>
#include <atomic>
#include <thread>
#include <cstdio>

int main(int argc, char **argv) {
    QApplication app(argc,argv);
    Engine engine; engine.setFadesManual(true);
    int failures=0;
    auto check=[&](bool ok,const char *why){ if(!ok){++failures; std::printf("FAIL timeline_activity: %s\n",why);} };
    const QStringList names={"01 · Poussière", "02 · Impacts", "03 · Résonances", "04 · Filaments", "05 · Respiration", "06 · Faille", "07 · Trajectoires", "08 · Grain", "09 · Mémoire", "10 · Turbulence", "11 · Halo", "12 · Débris", "13 · Dérive", "14 · Torsion", "15 · Friction", "16 · Aimantation", "17 · Rupture", "18 · Retour au noir"};
    quint64 first=0;
    for(int i=0;i<18;++i){
        Animation a; a.name=names[i]; a.duration=90+i*7; a.clock=2+i*4; a.loop=AnimLoop::Loop;
        a.state=i%5==0?AnimState::Paused:AnimState::Playing;
        AnimTrack track; track.param="opacity";
        for(int k=0;k<10000;++k) track.keys.push_back({k*.01,.5,0});
        a.tracks.push_back(std::move(track));
        const int row=engine.addAnimation(a); const auto id=engine.animation(row).id; if(i==0) first=id;
        engine.controlAnimation(id,AnimAction::Play); engine.advanceFades(.5);
        if(i%5==0) engine.controlAnimation(id,AnimAction::Pause);
    }
    std::vector<Engine::AnimationPlayback> sample;
    check(engine.sampleAnimations(&sample)&&sample.size()==18,"all 18 timelines sampled");
    for(const auto &a:sample) check(a.tracks==1&&a.captured.empty(),"overview does not copy keys or capture arrays");
    std::atomic<bool> held{false},done{false};
    std::thread busy([&]{ Engine::Lock lock(&engine.mutex()); held=true; while(!done) std::this_thread::yield(); });
    while(!held) std::this_thread::yield();
    check(!engine.sampleAnimations(&sample),"busy engine skips telemetry instead of blocking GUI");
    std::vector<Engine::InteractionLevel> interactionSample{{999,.25,.5,.75}};
    const bool sampledInteractions = engine.sampleInteractionLevels(&interactionSample);
    check(!sampledInteractions && interactionSample.size()==1 && interactionSample[0].id==999,
          "busy engine skips the whole meter poll and preserves the last displayed sample");
    done=true;busy.join();
    check(engine.sampleInteractionLevels(&interactionSample) && interactionSample.empty(),
          "idle engine clears stale meter values after interactions disappear");
    QElapsedTimer clock; clock.start();
    for(int repeat=0;repeat<200;++repeat) engine.sampleAnimations(&sample);
    const auto lightweight=clock.nsecsElapsed(); clock.restart();
    size_t keys=0;
    for(int repeat=0;repeat<200;++repeat) for(int i=0;i<18;++i) keys+=engine.animation(i).tracks[0].keys.size();
    const auto copies=clock.nsecsElapsed();
    std::printf("18 timelines x 10000 keys, 200 polls: telemetry %.3f ms; full definition copies %.3f ms (%zu keys copied)\n",lightweight/1e6,copies/1e6,keys);
    TimelineActivity view(&engine);view.resize(900,220);view.show();app.processEvents();view.poll();
    check(view.rows().size()==18,"18 simultaneous active rows");
    check(view.describe().contains("14 playing")&&view.describe().contains("4 paused"),"playing and paused counts visible");
    view.verticalScrollBar()->setValue(view.verticalScrollBar()->maximum());
    check(view.verticalScrollBar()->maximum()>0,"18 rows have a scrollable detail view");
    if(argc>1) check(view.grab().save(QString::fromLocal8Bit(argv[1])),"save real UI preview");
    engine.controlAnimation(first,AnimAction::Stop);view.poll();check(view.rows().size()==17,"stopped timeline leaves active filter");
    view.setShowStopped(true);check(view.rows().size()==18,"All filter keeps stopped timelines available");
    // GO and modified-state changes must not delete/recreate table items or interrupt text editing.
    Sequencer::Sequence sequence;sequence.name="Transport";sequence.steps.resize(18);engine.sequencer().add(sequence);engine.sequencer().setCurrent(0);
    SequenceWindow window(&engine);auto *table=window.findChild<QTableWidget *>("sequenceSteps");
    auto *item=table?table->item(0,1):nullptr;
    for(int step=0;step<18;++step){engine.sequencer().setPosition(step);window.setModified(step%2);}
    check(item&&table->item(0,1)==item,"transport preserves table items instead of rebuilding the editor");
    check(table&&table->rowCount()==18,"all cue rows retained");
    for(int i=0;i<engine.animationCount();++i) engine.controlAnimation(engine.animation(i).id,AnimAction::Stop);
    engine.controlAnimation(first,AnimAction::Play);view.setShowStopped(false);check(view.rows().size()==1,"same view works with one timeline");
    if(argc>2) check(view.grab().save(QString::fromLocal8Bit(argv[2])),"save single timeline preview");
    std::printf("timeline_activity: %d failures\n",failures);return failures?1:0;
}
