#pragma once
#include <algorithm>
#include <atomic>
#include <array>
#include <complex>
#include <cmath>

// Audio-thread processing: fixed storage; no allocation, locks or callbacks.
// Readers see normalized, smoothed program-audio energy before the master fader.
class AudioAnalysis {
public:
    static constexpr int fftSize = 4096, binCount = fftSize / 2 + 1, hopSize = 1024;
    struct Spectrum {
        std::array<float, binCount> power{};
        int rate = 48000;
        // Integrated, stereo-safe RMS over the selected FFT bins. Fractional
        // edge weights avoid jumps as a band handle crosses a bin boundary.
        float band(float lo, float hi) const {
            if (!std::isfinite(lo) || !std::isfinite(hi) || hi <= lo) return 0;
            const float step = float(rate) / fftSize;
            double sum = 0;
            for (int k = 1; k < binCount; ++k) {
                const float left = (k - .5f) * step, right = (k + .5f) * step;
                const float weight = std::clamp((std::min(hi,right)-std::max(lo,left))/step,0.f,1.f);
                sum += power[size_t(k)] * weight;
            }
            return 1.f - std::exp(-4.f * float(std::sqrt(std::max(0.,sum))));
        }
    };
    AudioAnalysis() {
        constexpr double pi=3.14159265358979323846;
        for(int n=0;n<fftSize;++n)m_window[n]=float(.5-.5*std::cos(2*pi*n/fftSize));
        for(int n=0;n<fftSize/2;++n)m_twiddle[n]=std::polar(1.f,float(-2*pi*n/fftSize));
        for(auto &v:m_power)v.store(0);
    }
    Spectrum spectrum() const {
        Spectrum result;
        // Atomic bins, bounded retry: readers never block the audio callback.
        for(int attempt=0;attempt<3;++attempt){
            const auto before=m_generation.load(std::memory_order_acquire);
            if(before&1)continue;
            result.rate=m_rate.load(std::memory_order_relaxed);
            for(int k=0;k<binCount;++k)result.power[k]=m_power[k].load(std::memory_order_relaxed);
            if(before==m_generation.load(std::memory_order_acquire))return result;
        }
        return result;
    }
    struct Values { float level=0, bass=0, mid=0, high=0, onset=0; };
    Values values() const { return {m_level.load(),m_bass.load(),m_mid.load(),m_high.load(),m_onset.load()}; }
    void reset() {
        for(int c=0;c<2;++c){m_low[c]=m_upper[c]=0;}
        m_write=m_filled=m_hop=0;
        for(auto &channel:m_ring)channel.fill(0);
        m_generation.fetch_add(1,std::memory_order_acq_rel);
        for(auto &v:m_power)v.store(0,std::memory_order_relaxed);
        m_generation.fetch_add(1,std::memory_order_release);
        m_slow=0; m_level=0; m_bass=0; m_mid=0; m_high=0; m_onset=0;
    }
    void process(const float *stereo,int frames,int rate) {
        if(!stereo||frames<=0||rate<=0)return;
        constexpr double pi=3.14159265358979323846;
        const float low=1-std::exp(float(-2*pi*180/rate)), upper=1-std::exp(float(-2*pi*2500/rate));
        double energy[4]={};
        if(m_rate.load(std::memory_order_relaxed)!=rate){reset();m_rate.store(rate,std::memory_order_relaxed);}
        for(int i=0;i<frames;++i) {
          for(int c=0;c<2;++c) {
            float x=stereo[2*i+c]; if(!std::isfinite(x))x=0;
            m_ring[c][m_write]=x;
            m_low[c]+=low*(x-m_low[c]);m_upper[c]+=upper*(x-m_upper[c]);
            const float bands[4]={x,m_low[c],m_upper[c]-m_low[c],x-m_upper[c]};
            for(int band=0;band<4;++band)energy[band]+=double(bands[band])*bands[band];
        }
          m_write=(m_write+1)%fftSize;
          m_filled=std::min(m_filled+1,fftSize);
          if(++m_hop>=hopSize && m_filled==fftSize){m_hop=0;publishSpectrum();}
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
    void publishSpectrum(){
        std::array<float,binCount> power{};
        for(int c=0;c<2;++c){
            for(int i=0;i<fftSize;++i)m_fft[i]={m_ring[c][(m_write+i)%fftSize]*m_window[i],0};
            for(int i=1,j=0;i<fftSize;++i){int bit=fftSize>>1;for(;j&bit;bit>>=1)j^=bit;j^=bit;if(i<j)std::swap(m_fft[i],m_fft[j]);}
            for(int length=2;length<=fftSize;length<<=1)
                for(int base=0;base<fftSize;base+=length)
                    for(int j=0;j<length/2;++j){const auto a=m_fft[base+j],b=m_fft[base+j+length/2]*m_twiddle[j*fftSize/length];m_fft[base+j]=a+b;m_fft[base+j+length/2]=a-b;}
            // Hann power correction (sum w²=3N/8), two one-sided spectra
            // averaged as powers, so opposite-phase stereo never cancels.
            for(int k=1;k<binCount;++k)power[k]+=std::norm(m_fft[k])/(fftSize*fftSize*.375f);
        }
        m_generation.fetch_add(1,std::memory_order_acq_rel);
        for(int k=0;k<binCount;++k)m_power[k].store(power[k],std::memory_order_relaxed);
        m_generation.fetch_add(1,std::memory_order_release);
    }
    std::array<float,fftSize> m_window{};
    std::array<std::array<float,fftSize>,2> m_ring{};
    std::array<std::complex<float>,fftSize> m_fft{};
    std::array<std::complex<float>,fftSize/2> m_twiddle{};
    std::array<std::atomic<float>,binCount> m_power{};
    std::atomic<unsigned> m_generation{0};
    std::atomic<int> m_rate{48000};
    int m_write=0,m_filled=0,m_hop=0;
    float m_low[2]={},m_upper[2]={},m_slow=0;
    std::atomic<float> m_level{0},m_bass{0},m_mid{0},m_high{0},m_onset{0};
};
