#pragma once
#include <algorithm>
#include <atomic>
#include <cmath>

// Audio-thread only processing: fixed state, no allocation, FFT, locks or callbacks.
// Readers see normalized, smoothed program-audio energy before the master fader.
class AudioAnalysis {
public:
    struct Values { float level=0, bass=0, mid=0, high=0, onset=0; };
    Values values() const { return {m_level.load(),m_bass.load(),m_mid.load(),m_high.load(),m_onset.load()}; }
    void reset() {
        for(int c=0;c<2;++c){m_low[c]=m_upper[c]=0;}
        m_slow=0; m_level=0; m_bass=0; m_mid=0; m_high=0; m_onset=0;
    }
    void process(const float *stereo,int frames,int rate) {
        if(!stereo||frames<=0||rate<=0)return;
        constexpr double pi=3.14159265358979323846;
        const float low=1-std::exp(float(-2*pi*180/rate)), upper=1-std::exp(float(-2*pi*2500/rate));
        double energy[4]={};
        for(int i=0;i<frames;++i) for(int c=0;c<2;++c) {
            float x=stereo[2*i+c]; if(!std::isfinite(x))x=0;
            m_low[c]+=low*(x-m_low[c]);m_upper[c]+=upper*(x-m_upper[c]);
            const float bands[4]={x,m_low[c],m_upper[c]-m_low[c],x-m_upper[c]};
            for(int band=0;band<4;++band)energy[band]+=double(bands[band])*bands[band];
        }
        const float seconds=float(frames)/rate;
        float rms[4];for(int band=0;band<4;++band)rms[band]=float(std::sqrt(energy[band]/(2*frames)));
        auto smooth=[seconds](float before,float target,float attack,float release){return target+(before-target)*std::exp(-seconds/(target>before?attack:release));};
        auto normalized=[](float value){return 1.f-std::exp(-std::max(0.f,value)*4.f);};
        const float transient=std::max(0.f,rms[0]-m_slow);
        m_slow=smooth(m_slow,rms[0],.15f,.35f);
        m_level=smooth(m_level.load(),normalized(rms[0]),.012f,.20f);
        m_bass=smooth(m_bass.load(),normalized(rms[1]),.025f,.30f);
        m_mid=smooth(m_mid.load(),normalized(rms[2]),.015f,.22f);
        m_high=smooth(m_high.load(),normalized(rms[3]),.006f,.12f);
        m_onset=smooth(m_onset.load(),std::clamp(transient*12.f,0.f,1.f),.002f,.08f);
    }
private:
    float m_low[2]={},m_upper[2]={},m_slow=0;
    std::atomic<float> m_level{0},m_bass{0},m_mid{0},m_high{0},m_onset{0};
};
