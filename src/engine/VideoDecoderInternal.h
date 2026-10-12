#pragma once
// Shared between the translation units of VideoDecoder (not part of its interface).

#include <QString>
#include <cstddef>

extern "C" {
#include <libavutil/pixfmt.h>
}

// The YUV → RGB matrix of a colour space (3×3, row-major) and its name ("BT.709"…)
void videoDecoderYuvMatrix(AVColorSpace space, int height, float m[9], QString *name);

// Process-wide accounting of the decoded frames kept in memory (not in upload buffers, not on the GPU).
// A soft limit: going over it never drops a frame, it makes every stream keep fewer frames ahead.
namespace decodedBudget {
void add(size_t bytes);
void remove(size_t bytes);
bool over(); // more than the limit is kept: queues keep their minimum
size_t used();
void setLimit(size_t bytes); // tests (default 512 MiB)
}

// Byte buffers kept for reuse, across every decoder (a hard limit: beyond it they are freed)
namespace bytePoolBudget {
bool reserve(size_t bytes);
void release(size_t bytes);
}
