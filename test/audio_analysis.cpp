#include "AudioAnalysis.h"
#include <array>
#include <cstdio>
#include <limits>
int main(){
    int failures=0;auto check=[&](bool ok,const char *why){if(!ok){++failures;std::printf("FAIL audio_analysis: %s\n",why);}};
    constexpr int frames=480,rate=48000;std::array<float,frames*2> buffer{};
    auto measure=[&](float frequency,bool antiphase=false){AudioAnalysis a;double phase=0;for(int block=0;block<100;++block){for(int i=0;i<frames;++i){const float x=.1f*std::sin(phase);phase+=6.283185307179586*frequency/rate;buffer[2*i]=x;buffer[2*i+1]=antiphase?-x:x;}a.process(buffer.data(),frames,rate);}return a.values();};
    auto bass=measure(70),mid=measure(1000),high=measure(10000),stereo=measure(1000,true);
    check(bass.bass>bass.mid&&bass.bass>bass.high,"low tone drives bass");
    check(mid.mid>mid.bass&&mid.mid>mid.high,"mid tone drives mid");
    check(high.high>high.mid&&high.high>high.bass,"high tone drives high");
    check(std::abs(stereo.level-mid.level)<1e-5,"opposite stereo channels do not cancel analysis");
    check(mid.level>.23&&mid.level<.26,"normalized RMS is calibrated");
    AudioAnalysis a;buffer.fill(.2f);a.process(buffer.data(),frames,rate);check(a.values().onset>.5,"attack produces an onset");
    for(int block=0;block<250;++block)a.process(buffer.data(),frames,rate);
    check(a.values().onset<.01,"steady tone does not repeatedly trigger attacks");
    buffer.fill(0);for(int block=0;block<500;++block)a.process(buffer.data(),frames,rate);check(a.values().level<1e-4&&a.values().bass<1e-4,"silence releases envelopes");
    buffer.fill(std::numeric_limits<float>::quiet_NaN());a.process(buffer.data(),frames,rate);check(std::isfinite(a.values().level),"invalid sample cannot poison envelopes");
    a.reset();check(a.values().level==0&&a.values().onset==0,"reset clears meters");
    std::printf("audio_analysis: %d failures\n",failures);return failures?1:0;
}
