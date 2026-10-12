# Real-time performance work

Making multi-video playback predictable under load, without slowing down the common case (one to three heavy
layers).

## In this change

- **Threads.** The first software decoder takes every core (FFmpeg's automatic count: a single 4K HEVC or
  ProRes stream needs them). Each next software decoder gets a share (`cores / open software decoders`, at
  least 2), so many layers no longer each start a machine-sized team. Hardware decoders keep 4 threads.
  There is no global decode gate: it serialized file reads and hardware waits behind a CPU limit. The HAP
  worker pool keeps `min(16, cores - 1)` threads (parallel Snappy chunks are what makes 4K HAP real time).
- **Decoded frames in memory.** They are counted process-wide against 512 MiB, as a soft limit. Over it, a
  frame is never dropped. Instead, every stream keeps only 2 frames ahead (instead of 6) until its frames are
  shown. A stream can never be starved by the others, and a paused layer keeps the right frame. Not counted:
  frames in upload buffers (they have their own budget) and hardware surfaces sampled in place (GPU memory).
- **Upload buffers (PBO).** 512 MiB shared fairly among the layers that have some. Each gets
  `512 MiB / layers`, capped by its own 128 MiB, and always at least 2 buffers. A layer opened first no longer
  keeps the whole budget, and the extra buffers go back as they return from an upload.
- **Byte buffers kept for reuse.** At most 64 MiB across every decoder.
- **Metrics per decoder.** Lock-free counters, no allocation per frame:
  - decoded, fetched, skipped and late frames, fetch misses;
  - decode time (total, max, approximate p95 from fixed buckets);
  - queue high-water mark;
  - budget holds: how many times the stream kept fewer frames ahead.

  `pinol_bench` prints them.
- **The decoder in focused files.** The split leaves behavior unchanged:
  - `VideoDecoderDemux.cpp`: packets of the video stream;
  - `VideoDecoderHap.cpp`: HAP, compressed textures for the GPU;
  - `VideoDecoderDelivery.cpp`: upload buffers, CPU fallback;
  - `VideoDecoderLayout.cpp`: pixel layouts, colorimetry;
  - `VideoDecoderSeek.cpp`: seek and codec flush;
  - `VideoDecoderMetrics.cpp`: metrics;
  - `VideoDecoderInternal.h`: what they share.

## Measured

`pinol_bench --decode --software --seconds 5`, Linux, 4 cores, frames/s decoded (the files are 30 fps;
mix: the average of 3 runs).

| Files | main | #115 as proposed | this change |
|---|---|---|---|
| HEVC 4K alone | 129 | 36 | 126 |
| ProRes 1080p alone | 419 | 138 | 417 |
| HAP Q 4K alone (8 chunks) | 1280 | 718 | 1287 |
| Mix: HEVC 4K | 26 | 25 | **39** |
| Mix: 3 × H.264 1080p | 115–150 | 47–51 | 98–114 |
| Mix: ProRes 1080p | 86–103 | 44–48 | 65–74 |
| Mix: HAP Q 4K | 142–165 | 49–53 | **278–287** |

In the mix, only this change keeps every stream above 30 fps. main starves the 4K HEVC (each decoder starts
a machine-sized team of threads), and #115 slows everything down (one thread per stream, at most 2 decodes at
once on 4 cores). Not measured here: hardware decoding, and 8+ cores.

## Definitions

- "Late": the fetched frame is older than the requested clock by more than 1.5 source frames.
- The budgets cover the frame payloads Pinol keeps. They do not cover FFmpeg's or the driver's own
  allocations.

## Still to measure

- The same with hardware decoding, on 8+ core machines, and the full playback (GPU) bench.
- Open, close, seek, loop and ping-pong stress with the budgets under pressure.

## Next steps

Playback scheduling (`run()`, legs, backward windows), then codec setup. Keep each step behavior-preserving
and benchmarked.
