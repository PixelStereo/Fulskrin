#include "Engine.h"
#include "EngineInternal.h"
#include <QTemporaryDir>
#include <QFile>
#include <cmath>
#include <cstdio>
int runOperatorStateTests(Engine &e)
{
 int failures=0;
 auto check=[&](bool ok,const char *why){if(!ok){++failures;std::printf("FAIL operator_state: %s\n",why);}};
 e.newProject();e.setFadesManual(true);
 const int li=e.addLayer("Surface");const quint64 id=e.layerId(li);
 e.setParameter(id,"opacity",.4);
 Engine::Snapshot snap;snap.name="Reference";snap.layers=e.captureLayers();snap.fade=0;const int si=e.addSnapshot(snap);const auto sid=e.snapshot(si).id;
 QString reason;check(e.snapshotMatches(sid,&reason),qPrintable("captured state matches: "+reason));
 e.setParameter(id,"opacity",.9);check(!e.snapshotMatches(sid),"static parameter edit changes concordance");e.setParameter(id,"opacity",.4);
 Animation animation;AnimTrack track;track.layer=id;track.param="opacity";track.oscillator=true;animation.tracks={track};const int ai=e.addAnimation(animation);e.controlAnimation(e.animation(ai).id,AnimAction::Play);e.advanceFades(.13);
 check(e.snapshotMatches(sid),"active animation output excluded from concordance");
 e.setParameter(id,"rotation",30);check(!e.snapshotMatches(sid),"manual edit still detected while other parameter is animated");e.setParameter(id,"rotation",0);
 e.controlAnimation(e.animation(ai).id,AnimAction::Stop);e.setParameter(id,"opacity",.4);
 Interaction interaction=Interaction::made(InteractionSource::Osc);interaction.layer=id;interaction.param="opacity";interaction.name="Fader";interaction.id=e.addInteraction(interaction);
 // Include the interaction definition in the reference so only its live output differs.
 snap=e.snapshot(si);snap.interactions=e.captureInteractions();e.setSnapshot(si,snap);e.setParameter(id,"opacity",.8);check(e.snapshotMatches(sid),"interaction-driven value excluded from concordance");
 e.layer(li)->mapping.offsets[0]=QPointF(.1,.2);++e.layer(li)->mapping.revision;check(!e.snapshotMatches(sid),"mesh point edit detected");
 e.layer(li)->mapping.resetMesh(4,4);e.setParameter(id,"opacity",.4);
 Engine::Preset preset;preset.fade=0;preset.name="Shape and drivers";preset.values={e.presetValue(id,"corner/top_left/x")};preset.geometry[QString::number(id)]=e.layer(li)->mapping.meshJson();preset.animations={animation};preset.interactions=e.interactions();
 const auto restored=presetFromJson(presetToJson(preset));check(restored.geometry==preset.geometry&&restored.interactions.size()==1&&restored.animations.size()==1,"geometry and driver definitions survive serialization");
 const int pi=e.addPreset(restored);e.layer(li)->mapping.resetMesh(7,6);e.setParameter(id,"corner/top_left/x",15);e.setInteractions({});e.setLayerAnims(id,{});e.recallPreset(pi);
 check(e.layer(li)->mapping.cols==4&&e.layer(li)->mapping.rows==4,"preset restores mesh topology");
 check(std::abs(e.parameterValue(id,"corner/top_left/x").toDouble())<1e-6,"preset restores corner pins");check(e.interactions().size()==1,"preset recreates interaction definition");Animation live;check(e.layerAnim(id,"opacity",&live),"preset restores animation definition");
 // Reload all live instances from disk; a broken replacement must keep last good shader.
 QTemporaryDir dir;const QString shader=dir.path()+"/reload.fs";QFile file(shader);
 auto write=[&](const QByteArray &code){if(!file.open(QIODevice::WriteOnly|QIODevice::Truncate))return false;file.write(code);file.close();return true;};
 const QByteArray prefix="/*{\"ISFVSN\":\"2\",\"INPUTS\":[]}*/\n";
 check(write(prefix+"void main(){gl_FragColor=vec4(1,0,0,1);}"),"write ISF fixture");QString error;
 const int shaderLayer=e.addLayer("Live shader");check(e.setLayerIsf(shaderLayer,shader,&error),"open live ISF");
 check(write(prefix+"void main(){gl_FragColor=vec4(0,1,0,1);}"),"edit ISF fixture");check(e.reloadIsfFile(shader,&error)==1&&error.isEmpty(),"reload updated live ISF by path");
 check(write(prefix+"this is invalid GLSL"),"write broken replacement");check(e.reloadIsfFile(shader,&error)==0&&!error.isEmpty(),"reload failure reported");check(e.layer(shaderLayer)->generator->isValid(),"last valid shader survives failed reload");
 e.setFadesManual(false);e.newProject();return failures;
}
