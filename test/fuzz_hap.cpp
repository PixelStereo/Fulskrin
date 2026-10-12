// Fuzz target (libFuzzer) for what Pinol reads from video files on its own: HAP frames (sections, chunks),
// their Snappy second stage, and the texture blocks decoded on the CPU when the GPU cannot sample them.
// A damaged or hostile file must be refused, never read or written out of bounds (ASan / UBSan check it).
//
//   cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DPINOL_FUZZ=ON
//   cmake --build build-fuzz --target fuzz_hap
//   ./build-fuzz/fuzz_hap -max_total_time=60 corpus/      (seeds: test/make_fuzz_corpus.sh)

#include "Hap.h"
#include "Snappy.h"

#include <cstddef>
#include <cstdint>
#include <vector>

static constexpr size_t kMaxOut = size_t(64) << 20; // a 8K BC7 frame is 33 MB

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    // 1. A HAP frame, as the decoder hands it over (the decoder also checks the size against the picture's)
    hap::Frame frame;
    if (hap::parse(data, size, &frame)) {
        for (int i = 0; i < frame.count; ++i) {
            const hap::Texture &t = frame.tex[i];
            if (t.outSize == 0 || t.outSize > kMaxOut) continue;
            std::vector<uint8_t> out(t.outSize);
            hap::decompress(t, out.data(), out.size());
        }
    }

    // 2. A raw Snappy stream, with the length its preamble announces
    size_t length = 0;
    if (snappy::uncompressedLength(data, size, &length) && length <= kMaxOut) {
        std::vector<uint8_t> out(length);
        snappy::decompress(data, size, out.data(), out.size());
    }

    // 3. Blocks decoded on the CPU: picture size and format from the first bytes, blocks after them
    if (size >= 3) {
        static const hap::Format kFormats[] = {hap::Format::RgbDxt1,    hap::Format::RgbaDxt5, hap::Format::YCoCgDxt5,
                                               hap::Format::RgbaBc7,    hap::Format::AlphaRgtc1, hap::Format::RgbBc6u,
                                               hap::Format::RgbBc6s};
        const hap::Format f = kFormats[data[2] % 7];
        const int w = 1 + data[0] % 64, h = 1 + data[1] % 64;
        const size_t need = hap::textureBytes(f, w, h);
        if (size - 3 >= need) {
            const size_t stride = size_t((w + 3) / 4 * 4) * size_t(hap::decodedChannels(f));
            std::vector<uint8_t> pixels(stride * size_t((h + 3) / 4 * 4));
            hap::decodeBlocks(f, data + 3, w, h, pixels.data(), stride);
        }
    }
    return 0;
}
