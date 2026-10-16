#include "AudioAnalysis.h"
#include <array>
#include <cstdio>
#include <limits>
#include <random>
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
    auto spectrumOf=[&](float hz,bool opposite){AudioAnalysis fft;double phase=0;for(int block=0;block<30;++block){for(int i=0;i<frames;++i){float x=.1f*std::sin(phase);phase+=6.283185307179586*hz/rate;buffer[2*i]=x;buffer[2*i+1]=opposite?-x:x;}fft.process(buffer.data(),frames,rate);}return fft.spectrum();};
    const auto selected=spectrumOf(1000,false),opposite=spectrumOf(1000,true),air=spectrumOf(8000,false);
    check(selected.band(900,1100)>.24f && selected.band(900,1100)<.25f,"FFT band is calibrated to stereo RMS");
    check(selected.band(5000,10000)<.001f,"out-of-band tone is rejected");
    check(air.band(7500,8500)>.24f && air.band(20,250)<.001f,"high region selects high spectral content");
    check(std::abs(selected.band(900,1100)-opposite.band(900,1100))<1e-5f,"FFT combines powers without stereo phase cancellation");
    check(selected.band(1100,900)==0,"invalid frequency interval is silent");
    check(a.spectrum().band(20,20000)==0,"reset clears the spectrum");
    // Nyquist has no distinct conjugate partner. Alternating samples have
    // exactly 0.1 RMS, including with opposite-phase stereo.
    for(int sampleRate:{32000,48000}){
        for(bool oppositePhase:{false,true}){
            AudioAnalysis nyquist;
            std::array<float,AudioAnalysis::fftSize*2> alternating{};
            for(int i=0;i<AudioAnalysis::fftSize;++i){
                const float x=(i&1)?-.1f:.1f;
                alternating[2*i]=x;
                alternating[2*i+1]=oppositePhase?-x:x;
            }
            nyquist.process(alternating.data(),AudioAnalysis::fftSize,sampleRate);
            const float measured=nyquist.spectrum().band(0,float(sampleRate));
            check(std::abs(measured-(1.f-std::exp(-.4f)))<1e-5f,"Nyquist energy is counted once and calibrated to RMS");
        }
    }
    // Differential regression against the original full scan: preserve the
    // fractional edge weights, accumulation order and normalization exactly.
    auto fullScan=[](const AudioAnalysis::Spectrum &s,float lo,float hi){
        if(!std::isfinite(lo)||!std::isfinite(hi)||hi<=lo)return 0.f;
        const float step=float(s.rate)/AudioAnalysis::fftSize;
        double sum=0;
        for(int k=1;k<AudioAnalysis::binCount;++k){
            const float left=(k-.5f)*step,right=(k+.5f)*step;
            const float weight=std::clamp((std::min(hi,right)-std::max(lo,left))/step,0.f,1.f);
            sum+=s.power[size_t(k)]*weight;
        }
        return 1.f-std::exp(-4.f*float(std::sqrt(std::max(0.,sum))));
    };
    std::mt19937 random(188);
    std::uniform_real_distribution<float> power(0.f,1e-5f),frequency(-50000.f,100000.f);
    AudioAnalysis::Spectrum fixture;
    for(auto &p:fixture.power)p=power(random);
    bool same=true;
    for(int sampleRate:{1,8000,44100,48000,96000,192000}){
        fixture.rate=sampleRate;
        for(int n=0;n<2000;++n){
            float lo=frequency(random),hi=frequency(random);
            if(lo>hi)std::swap(lo,hi);
            same &= fixture.band(lo,hi)==fullScan(fixture,lo,hi);
        }
        const float step=float(sampleRate)/AudioAnalysis::fftSize;
        for(int k=1;k<AudioAnalysis::binCount;++k){
            const float edge=(k+.5f)*step;
            for(float hi:{std::nextafter(edge,0.f),edge,std::nextafter(edge,std::numeric_limits<float>::infinity())})
                same &= fixture.band(edge-step,hi)==fullScan(fixture,edge-step,hi);
        }
        const float huge=std::numeric_limits<float>::max();
        same &= fixture.band(-huge,huge)==fullScan(fixture,-huge,huge);
        same &= fixture.band(huge/2,huge)==0.f;
        same &= fixture.band(-huge,-huge/2)==0.f;
    }
    check(same,"bounded band scan matches full scan for random bands and FFT boundaries");
    fixture.rate=0;check(fixture.band(20,20000)==0,"invalid sample rate is silent");
    fixture.rate=-48000;check(fixture.band(20,20000)==0,"negative sample rate is silent");
    std::printf("audio_analysis: %d failures\n",failures);return failures?1:0;
}
