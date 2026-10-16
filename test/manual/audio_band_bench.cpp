// Standalone: c++ -std=c++17 -O2 -Isrc/engine test/manual/audio_band_bench.cpp -o /tmp/audio-band-bench
#include "AudioAnalysis.h"
#include <chrono>
#include <cstdio>
#include <random>

static float fullScan(const AudioAnalysis::Spectrum &s, float lo, float hi)
{
    const float step = float(s.rate) / AudioAnalysis::fftSize;
    double sum = 0;
    for (int k = 1; k < AudioAnalysis::binCount; ++k) {
        const float left = (k - .5f) * step, right = (k + .5f) * step;
        const float weight = std::clamp((std::min(hi, right) - std::max(lo, left)) / step, 0.f, 1.f);
        sum += s.power[size_t(k)] * weight;
    }
    return 1.f - std::exp(-4.f * float(std::sqrt(std::max(0., sum))));
}

int main()
{
    AudioAnalysis::Spectrum s;
    std::mt19937 random(188);
    std::uniform_real_distribution<float> power(0.f, 1e-5f);
    for (auto &p : s.power) p = power(random);
    // Five bands repeated on three layers, as in Champ de rupture.
    const std::array<std::array<float, 2>, 5> bands{{{20,20000},{28,120},{700,3200},{6000,16000},{20,20000}}};
    volatile float sink = 0;
    auto measure = [&](auto band) {
        const auto start = std::chrono::steady_clock::now();
        for (int frame = 0; frame < 10000; ++frame)
            for (int layer = 0; layer < 3; ++layer)
                for (const auto &b : bands) sink = band(s, b[0], b[1]);
        return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now()-start).count();
    };
    for (int trial = 0; trial < 3; ++trial) {
        const double old = measure(fullScan);
        const double optimized = measure([](const auto &s, float lo, float hi) { return s.band(lo, hi); });
        std::printf("trial %d: full scan %.3f ms, bounded scan %.3f ms, ratio %.2fx\n", trial+1, old, optimized, old/optimized);
    }
    (void)sink;
}
