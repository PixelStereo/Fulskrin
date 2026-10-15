#include "Engine.h"
#include "InteractionPanel.h"
#include "AudioSpectrum.h"
#include "Widgets.h"
#include <QApplication>
#include <QTabBar>
#include <QTreeWidget>
#include <QDoubleSpinBox>
#include <QComboBox>
#include <QMenu>
#include <QMouseEvent>
#include <QUndoStack>
#include <cstdio>
int main(int argc,char **argv){
    QApplication app(argc,argv);theme::applyToApplication(app);Engine engine;QUndoStack undo;int failures=0;
    auto check=[&](bool ok,const char *why){if(!ok){++failures;std::printf("FAIL audio_interaction_ui: %s\n",why);}};
    auto old=Interaction::fromJson({{"source","internal"},{"source_layer","0"},{"source_param","audio/bass"},{"layer","101"},{"param","opacity"}});
    check(old.source==InteractionSource::Audio && old.audioLow==20 && old.audioHigh==180,"legacy program audio migrates to Audio band");
    old.audioLow=340;old.audioHigh=1240;old.name="Metal";
    auto roundtrip=Interaction::fromJson(old.toJson());
    check(roundtrip.source==InteractionSource::Audio && roundtrip.audioLow==340 && roundtrip.audioHigh==1240 && roundtrip.name=="Metal","custom frequency interval survives project serialization");
    InteractionPanel panel(&engine,&undo);panel.resize(720,1000);panel.show();
    panel.learnFor(0,"opacity",InteractionSource::Audio);app.processEvents();
    auto *tabs=panel.findChild<QTabBar*>();check(tabs&&tabs->tabText(tabs->currentIndex())=="Audio","Audio has its own selected tab");
    auto *spectrum=panel.findChild<AudioSpectrum*>();auto *lo=panel.findChild<QDoubleSpinBox*>("audioLowHz");auto *hi=panel.findChild<QDoubleSpinBox*>("audioHighHz");
    check(spectrum&&lo&&hi,"spectrum and numeric frequency selection shown before target mapping");
    if(lo&&hi){lo->setValue(500);hi->setValue(1500);auto list=engine.interactions();check(list.size()==1&&list[0].audioLow==500&&list[0].audioHigh==1500,"frequency widgets update engine interaction");undo.undo();check(engine.interactions()[0].audioHigh==20000,"band drag/number edits undo together");undo.redo();}
    QMenu menu;InteractionPanel::fillValueMenu(&menu,&engine,0,{"opacity"});
    bool audioAction=false;for(auto *a:menu.actions())if(a->menu())for(auto *b:a->menu()->actions())if(b->text()=="Audio")audioAction=true;
    check(audioAction,"parameter right-click offers Audio mapping");
    QCoreApplication::sendPostedEvents(nullptr,QEvent::DeferredDelete);app.processEvents();
    if(argc>1)panel.grab().save(QString::fromLocal8Bit(argv[1]));
    std::printf("audio_interaction_ui: %d failures\n",failures);return failures?1:0;
}
