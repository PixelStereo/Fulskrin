# Real-time performance work

Making multi-video playback predictable under load, without slowing down the common case (one to three heavy
layers).

## In this change

- **Threads.** Every software decoder gets the same number of FFmpeg threads, whatever the order the videos were
  opened in: half the cores, between 2 and 16 (`PINOL_DECODE_THREADS` overrides it, for measurements). Before,
  the first decoder opened took every core and the next ones shared what was left, so the last layer opened got
  the fewest threads. Hardware decoders keep 4 threads. There is no global decode gate: it held file reads and
  hardware waits behind a CPU limit. The HAP worker pool keeps `min(16, cores - 1)` threads (decompressing
  Snappy chunks in parallel is what makes 4K HAP real time).
- **Decoded frames in memory.** They are counted process-wide against 512 MiB, as a soft limit. Over it, a
  frame is never dropped. Instead, every stream keeps only 2 frames ahead (instead of 6) until its frames are
  shown. A stream can never be starved by the others, and a paused layer keeps the right frame. Not counted:
  frames in upload buffers (they have their own budget) and hardware surfaces sampled in place (GPU memory).
- **Upload buffers (PBO).** Each stream gets up to 128 MiB of them. They are shared fairly among the layers only
  once they would take more than 2 GiB in all (each layer keeps at least 2). `PINOL_PBO_BUDGET_MB` overrides the
  2 GiB, and `PINOL_FRAME_BUDGET_MB` the 512 MiB of decoded frames (measurements).
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

### Apple M3 Ultra (28 cores), main before #117 → after #117

- **Playback (GPU), hardware and software, 1 layer and the 6-layer mix:** every layer at 30 fps in both versions,
  render at 56.7 fps (60 asked), no budget hold.
- **Hardware decode (VideoToolbox), as fast as possible:** the same within noise (HEVC 4K alone 127 → 125 fps;
  mix 192 → 192 fps).
- **Software decode, alone:** the same (HEVC 4K 1200 / 1200 fps, ProRes 2746 / 2748 fps).
- **Software decode, mix:** HEVC 4K +54 %, HAP Q 4K +35 %, H.264 −4 %, ProRes −45 %. ProRes was opened last and
  got 5 threads against 28 for the first decoder. This uneven share is what the even per-decoder count fixes.

Decode-only runs are throughput tests: every stream decodes as fast as it can, so equal threads give equal CPU
time and the heaviest stream gets the lowest rate. In playback, a stream that is far enough ahead waits, which
leaves the CPU to the others. Playback with many layers is the test that decides.

### Apple M3 Ultra, playback under load (software decoding, 8 s, render asked 60 fps)

The slowest layer and the render rate. "Slow uploads": frames the render thread uploaded from the decoder's
memory because no upload buffer was free.

| Layers | before #117 | #117 (512 MiB of buffers) | 2 GiB of buffers | 2 GiB, but 512 MiB forced |
|---|---|---|---|---|
| 12 × HEVC 4K | 30 / 56.6 | 22 / 24.1 | 30 / 56.7, 0 slow uploads | 23 / 24.2, 1319 of 2602 slow |
| 20 × HEVC 4K | 29 / 56.5 | 16 / 17.2 | 30 / 56.6, 7 of 5573 slow | 16 / 16.8, 1268 of 3100 slow |
| 18-layer mix | 30 / 51.9 | 26 / 28.3 | 30 / 46.6 | 26 / 29.5 |

The 512 MiB budget of #117 was the cause: forced back, it brings the drop back. 8 GiB of buffers, 8 GiB of
decoded frames or 4 threads change nothing more. With hardware decoding (20 × HEVC 4K, 18-layer mix) and with
16 × HAP Q 4K, every version keeps 30 fps.

## Definitions

- "Late": the fetched frame is older than the requested clock by more than 1.5 source frames.
- The budgets cover the frame payloads Pinol keeps. They do not cover FFmpeg's or the driver's own
  allocations.

## Still to measure

- The same on a machine with less memory and fewer cores (the 2 GiB of upload buffers is the trade-off).
- Open, close, seek, loop and ping-pong stress with the budgets under pressure.

## Next steps

Playback scheduling (`run()`, legs, backward windows), then codec setup. Keep each step behavior-preserving
and benchmarked.

## Audit of 13 October 2026 (611f228): what was done

Each point of the audit, what changed, and what is left to measure on show hardware (CI has neither a GPU nor a
sound card, nor Syphon / NDI receivers).

| Audit | Change | Tested by | Still to measure |
|---|---|---|---|
| P0 OSC direct write and Interactions | An enabled OSC interaction on a value's own address owns it: the direct write is not made (`Engine::oscAddressClaimed`). Bytes 0..255 read the same on both paths. | `interactions`: ffff with A=0, iiii, packed `r`, Map alpha, disabled again | Chataigne on a real network |
| P1 Metal surface reused after the 100 ms timeout | A surface still read is never written: first free one, else 8 ms, else the frame is dropped and counted. Release waits for its command buffer. | — (no window on CI) | Outputs under GPU load, `outputs_dropped` |
| P1 cues on the interface's event loop | A cue clock thread (2 ms precise timer, monotonic) fires the waits and recalls from there; undo recorded afterwards. | `snapshots`: GO, then this thread blocked 400 ms: recalled on time | Lateness (`Sequencer::cueTiming`) during drags, loads, OSC floods |
| P1 logs: one table row per message | `EventLog` ring: repeats merged, 500 new lines/s, bounded; the panel reads it 10 times a second through a model. | `osc`: merge, rate, ring, 4 threads, model rows | UI responsiveness at 1000+ OSC messages/s |
| P1 no global thread budget | Software decoders share a budget of 2 × cores beyond it (an even share, at least 2). Unchanged up to it (#117's even share). | `video`: counts, even share beyond the budget | `pinol_bench` with 8 / 16 software streams, p99 |
| P1 soft memory limits | Bytes of decoded frames, upload buffers, threads, graphics memory (NVX / ATI) against their budgets: `/composition/resources`, warnings logged. | `video`: the JSON | Several 4K with effects and outputs |
| P1 glFinish / PBO mapping | NDI / OMT: fenced read backs, a frame not ready is skipped (2 ms max), not waited for. Syphon / Spout: optional "one frame later" (no glFinish). | — (no publishers on CI) | Render p99 with Syphon / NDI receivers |
| P1/P2 composite colors | Each channel through range, filter, curve, master (not alpha), target range; pick-up on the whole color. | `interactions`: range, master, curve, filter, pick-up | Controllers with filters on colors |
| P2 mipmaps | Once per frame of the render whatever the number of viewports (plain video: once per video frame). `pinol_bench --mipmaps`. | `rendering`: 3 frames, 2 viewports: 3 | 4K layers mapped small on several outputs |
| P2 the presented frame | `FrameTiming`: interval, compute, present, missed deadlines, OSC / cue to frame: `/composition/timing`, `pinol_bench`. | `osc`: fixed times and live | Real outputs with vertical sync |
| P2 CI | Timing areas labelled and retried (thresholds kept); ThreadSanitizer: a race fails at once, a timing failure is retried; dependency versions in each package; each package extracted elsewhere and rendering a project; the interface scenario as a job. Tags `v*` and `1.2` publish. | the workflow | Pin a dated BtbN FFmpeg release (`FFMPEG_RELEASE`) |

Not done here: the long runs (1 h to 8 h), the macOS Intel matrix, and splitting the large classes (MainWindow,
LayerInspector, SnapshotPanel, VideoDecoder). These need either hardware or a refactor plan of their own.
