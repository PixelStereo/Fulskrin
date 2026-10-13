// Fuzz target (libFuzzer) for the OSC packets Pinol receives from the network (UDP, OSCQuery WebSocket frames):
// any datagram from any machine listened on reaches osc::decode. A malformed or hostile packet must be refused,
// never read out of bounds (ASan / UBSan check it). What decodes is encoded again and decoded again, and the address
// pattern matcher runs on what came in.
//
//   cmake -S . -B build-fuzz -DCMAKE_CXX_COMPILER=clang++ -DPINOL_FUZZ=ON
//   cmake --build build-fuzz --target fuzz_osc
//   ./build-fuzz/fuzz_osc -max_total_time=60 corpus/      (seeds: test/make_fuzz_corpus.sh)

#include "OscCodec.h"

#include <cstddef>
#include <cstdint>
#include <cstdlib>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    const QByteArray packet = QByteArray::fromRawData(reinterpret_cast<const char *>(data), qsizetype(size));
    std::vector<osc::Message> messages;
    if (!osc::decode(packet, messages)) return 0;
    for (const osc::Message &m : messages) {
        // Round trip: what was understood is written again and read the same
        std::vector<osc::Message> again;
        if (!osc::decode(osc::encode(m), again) || again.size() != 1 || again[0].address != m.address) std::abort();
        osc::match(m.address, QStringLiteral("/layer/a/opacity"));
        osc::match(QStringLiteral("/layer/*/opacity"), m.address);
    }
    return 0;
}
