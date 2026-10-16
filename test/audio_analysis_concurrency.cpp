#include "AudioAnalysis.h"
#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

int main() {
    // Each sample rate identifies an exact complete FFT fixture. Every read
    // must match one full fixture (or coherent reset/contention silence),
    // including bins whose power differs by fourfold between publications.
    std::array<float, AudioAnalysis::fftSize * 2> input[2];
    AudioAnalysis::Spectrum expected[2];
    const int rates[] = {32000, 48000};
    for (int fixture = 0; fixture < 2; ++fixture) {
        for (int frame = 0; frame < AudioAnalysis::fftSize; ++frame) {
            const float sample = (frame & 1 ? -.1f : .1f) * (fixture + 1);
            input[fixture][2 * frame] = sample;
            input[fixture][2 * frame + 1] = -sample;
        }
        AudioAnalysis analysis;
        analysis.process(input[fixture].data(), AudioAnalysis::fftSize, rates[fixture]);
        expected[fixture] = analysis.spectrum();
    }
    AudioAnalysis analysis;
    std::atomic<bool> start{false}, done{false};
    std::atomic<unsigned> failures{0}, reads{0}, signals{0};
    std::vector<std::thread> readers;
    for (int reader = 0; reader < 4; ++reader) readers.emplace_back([&] {
        while (!start.load(std::memory_order_acquire)) std::this_thread::yield();
        do {
            const auto snapshot = analysis.spectrum();
            const int fixture = snapshot.rate == rates[0] ? 0 : 1;
            const bool silent = std::all_of(snapshot.power.begin(), snapshot.power.end(),
                                            [](float value) { return value == 0.f; });
            if (snapshot.rate != rates[fixture] ||
                (!silent && snapshot.power != expected[fixture].power)) ++failures;
            if (!silent) ++signals;
            ++reads;
        } while (!done.load(std::memory_order_acquire));
    });
    start.store(true, std::memory_order_release);
    for (int block = 0; block < 1000; ++block) {
        const int fixture = block & 1;
        analysis.process(input[fixture].data(), AudioAnalysis::fftSize, rates[fixture]);
        if (block % 7 == 0) analysis.reset();
    }
    done.store(true, std::memory_order_release);
    for (auto &reader : readers) reader.join();
    const bool passed = failures == 0 && reads > 1000 && signals > 0;
    std::printf("audio_analysis_concurrency: %u reads, %u signal snapshots, %u failures\n",
                reads.load(), signals.load(), failures.load());
    return passed ? 0 : 1;
}
