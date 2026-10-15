#include "Engine.h"
#include <QApplication>
#include <QSurfaceFormat>
#include <QTimer>
#include <QElapsedTimer>
#include <QThread>
#include <atomic>
#include <cstdio>
int main(int argc,char **argv) {
    QSurfaceFormat format;format.setVersion(3,3);format.setProfile(QSurfaceFormat::CoreProfile);QSurfaceFormat::setDefaultFormat(format);
    QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);QApplication app(argc,argv);
    Engine engine;QString error;if(!engine.initialize(&error)||!engine.start())return 1;
    engine.addLayer("Surface");Engine::Snapshot snapshot;snapshot.name="Slow cue";snapshot.layers=engine.captureLayers();snapshot.fade=0;
    const int index=engine.addSnapshot(snapshot);const auto id=engine.snapshot(index).id;
    std::atomic<bool> inSlowRecall{false};int recorded=0,heartbeats=0;bool correctThread=false;
    QObject::connect(&engine,&Engine::snapshotRecalled,&engine,[&](int){inSlowRecall=true;QThread::msleep(120);inSlowRecall=false;},Qt::DirectConnection);
    engine.sequencer().setRecallRecorder([&](int cue,const Sequencer::RecallBefore &before){++recorded;correctThread=QThread::currentThread()==app.thread()&&cue==index&&!before.layers.isEmpty();});
    QTimer heartbeat;heartbeat.setInterval(1);QObject::connect(&heartbeat,&QTimer::timeout,[&]{if(inSlowRecall)++heartbeats;});heartbeat.start();
    const bool accepted=engine.sequencer().requestSnapshot(id);
    QElapsedTimer wait;wait.start();while(!recorded&&wait.elapsed()<5000){app.processEvents();QThread::msleep(1);}
    int failures=0;
    auto check=[&](bool ok,const char *why){if(!ok){++failures;std::printf("FAIL cue_dispatch: %s\n",why);}};
    check(accepted,"snapshot dispatched to cue clock");check(recorded==1&&correctThread,"one undo record delivered on GUI thread");check(heartbeats>0,"GUI handles events during a slow recall");
    // A stale queued undo must not be delivered into a newly opened/cleared show.
    engine.sequencer().requestSnapshot(id);wait.restart();while(!inSlowRecall&&wait.elapsed()<5000){app.processEvents();QThread::msleep(1);}
    if(inSlowRecall)engine.sequencer().clear();
    wait.restart();while(wait.elapsed()<250){app.processEvents();QThread::msleep(1);}
    check(recorded==1,"project generation invalidates pending undo delivery");
    engine.sequencer().setRecallRecorder(nullptr);engine.sequencer().shutdownClock();engine.shutdown();
    return failures?1:0;
}
