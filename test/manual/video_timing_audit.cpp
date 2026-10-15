#include "Engine.h"
#include "Sequencer.h"
#include <QApplication>
#include <QCoreApplication>
#include <QSurfaceFormat>
#include <QTemporaryDir>
#include <QFile>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonDocument>
#include <thread>
#include <cstdio>
#include <cmath>
static void reset(Engine &e) { e.stop(); e.shutdown(); QString initError; e.initialize(&initError); e.newProject(); e.setFadesManual(true); e.setPaused(false); e.setCompositionSpeed(1); e.fadeCompositionOpacity(1,0); e.setBlackout(false); e.setCompositionSize({64,64}); e.setViewportSize(e.indexOfId(e.mainViewportId()),{64,64}); }
static quint64 addAnim(Engine &e,double duration=1) { Animation a;a.duration=duration;a.loop=AnimLoop::Once;int i=e.addAnimation(a);return e.animation(i).id; }
static void waitFrames(Engine &e,int n) { auto target=e.frameCount()+n; QElapsedTimer t;t.start();while(e.frameCount()<target&&t.elapsed()<7000) QThread::msleep(1); }
int main(int argc,char **argv) {
 setvbuf(stdout,nullptr,_IONBF,0); QSurfaceFormat f;f.setVersion(3,3);f.setProfile(QSurfaceFormat::CoreProfile);QSurfaceFormat::setDefaultFormat(f);QApplication::setAttribute(Qt::AA_ShareOpenGLContexts);QApplication app(argc,argv);Engine e;QString error;if(!e.initialize(&error))return 2;
 reset(e);auto id=addAnim(e);Sequencer::Step st;st.timeline=id;Sequencer::Sequence s;s.steps={st,st};int qi=e.sequencer().add(s);
 std::thread go([&]{e.sequencer().goSequence(qi);});go.join();e.stopShowPlayback();QCoreApplication::processEvents();std::printf("PROBE queued_GO_after_Escape: animation_playing=%d sequence_position=%d (expected stopped/-1)\n",e.animation(0).state==AnimState::Playing,e.sequencer().positionOf(qi));
 reset(e);id=addAnim(e);st.timeline=id;s.steps={st,st};qi=e.sequencer().add(s);std::thread gos([&]{e.sequencer().goSequence(qi);e.sequencer().goSequence(qi);});gos.join();QCoreApplication::processEvents();std::printf("PROBE queued_two_GO: sequence_position=%d (expected 1) runs=%zu\n",e.sequencer().positionOf(qi),e.sequencer().runs().size());
 reset(e);int li=e.addLayer("marker");Engine::Snapshot snap;snap.layers=e.captureLayers();int si=e.addSnapshot(snap);st={};st.snapshot=e.snapshot(si).id;st.preWait=.2;s.steps={st};int q1=e.sequencer().add(s),q2=e.sequencer().add(s);int recalls=0;auto con=QObject::connect(&e,&Engine::snapshotRecalled,[&](int){++recalls;if(recalls==1)e.stopShowPlayback();});e.sequencer().goSequence(q1);e.sequencer().goSequence(q2);e.sequencer().advance(.2);QObject::disconnect(con);std::printf("PROBE in_flight_batch_after_Escape: recalls=%d (expected 1)\n",recalls);
 reset(e);id=addAnim(e,.2);st={};st.timeline=id;st.preWait=.3;st.postWait=.1;st.next=Sequencer::StepContinue::AutoFollow;Sequencer::Step next;next.timeline=addAnim(e,1);s.steps={st,next};qi=e.sequencer().add(s);e.sequencer().goSequence(qi);e.sequencer().advance(1);std::printf("PROBE late_prewait_timeline: position=%d first_playing=%d first_clock=%.3f (expected still step 0 until actual completion)\n",e.sequencer().positionOf(qi),e.animation(0).state==AnimState::Playing,e.animation(0).clock);
 reset(e);li=e.addLayer("osc");AnimTrack tr;tr.layer=e.layerId(li);tr.param="opacity";tr.oscillator=true;tr.period=.7;tr.center=.5;tr.amplitude=.4;Animation a;a.duration=1;a.loop=AnimLoop::Loop;a.tracks={tr};int ai=e.addAnimation(a);id=e.animation(ai).id;e.controlAnimation(id,AnimAction::Play);e.advanceFades(1.2);double before=0,after=0;e.animParamValue(tr.layer,tr.param,&before);e.controlAnimation(id,AnimAction::LoopMode,0,AnimLoop::PingPong);e.advanceFades(0);e.animParamValue(tr.layer,tr.param,&after);std::printf("PROBE oscillator_loop_mode_edit: before=%.6f after=%.6f jump=%.6f (expected no discontinuity)\n",before,after,std::abs(after-before));
 reset(e);li=e.addLayer("visibility");e.layer(li)->opacity=0;auto states=e.captureLayers();auto o=states[0].toObject(); // viewport may come first: locate layer id instead below
 for(int k=0;k<states.size();++k){auto x=states[k].toObject();if(x.value("id").toString().toULongLong()==e.layerId(li)){auto p=x.value("params").toObject();p["opacity"]=1;x["params"]=p;states[k]=x;}}
 e.applyLayers(states,2);e.fadeCompositionOpacity(0,0);e.renderFrame();e.applyComposition({{"included",true},{"opacity",1.0}},2);e.setCompositionSpeed(0);e.setFadesManual(false);e.start();QThread::msleep(650);std::printf("PROBE global_paused_fades: layer=%.5f composition=%.5f (expected same progress)\n",double(e.layer(li)->opacity),e.compositionOpacity());e.stop();
 reset(e);QTemporaryDir dir;QString shader=dir.path()+"/clock.fs";QFile file(shader);file.open(QIODevice::WriteOnly);file.write("/*{\"ISFVSN\":\"2\",\"CATEGORIES\":[\"Generator\"]}*/\nvoid main(){gl_FragColor=vec4(TIME/20.0,0.0,0.0,1.0);}\n");file.close();li=e.addLayer("clock shader");if (!e.setLayerIsf(li,shader,&error)) std::printf("SET_SHADER_ERROR %s\n",qPrintable(error));e.setGeneratorSize(li,64,64);e.setFadesManual(false);e.start();waitFrames(e,5);e.stopShowPlayback();double r0=e.grabOutput().pixelColor(32,32).redF();QThread::msleep(600);double r1=e.grabOutput().pixelColor(32,32).redF();std::printf("PROBE shader_after_Escape: red_before=%.6f red_after=%.6f time_advanced~%.3fs (expected frozen animation)\n",r0,r1,(r1-r0)*20);e.stop();
 reset(e);li=e.addLayer("clock shader");if (!e.setLayerIsf(li,shader,&error)) std::printf("SET_SHADER_ERROR %s\n",qPrintable(error));e.setGeneratorSize(li,64,64);id=addAnim(e,30);e.controlAnimation(id,AnimAction::Play);e.setFadesManual(false);e.start();waitFrames(e,5);double c0=e.animation(0).clock;r0=e.grabOutput().pixelColor(32,32).redF();e.runGl([]{QThread::msleep(650);});waitFrames(e,2);double c1=e.animation(0).clock;r1=e.grabOutput().pixelColor(32,32).redF();std::printf("PROBE stall_650ms: timeline_delta=%.4f shader_delta~%.4f (expected same elapsed time)\n",c1-c0,(r1-r0)*20);
 c0=e.animation(0).clock;QElapsedTimer wall;wall.start();e.runGl([]{QThread::msleep(3100);});waitFrames(e,2);c1=e.animation(0).clock;std::printf("PROBE stall_3100ms: wall_delta=%.4f timeline_delta=%.4f (show loses wall time beyond 2s)\n",wall.elapsed()/1000.,c1-c0);e.stop();
 e.shutdown();return 0;
}
