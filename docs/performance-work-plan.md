# Real-time performance work

This branch is an incremental foundation for making multi-video playback predictable under load.

## Implemented in this tranche

- A process-wide 512 MiB budget for mapped OpenGL upload/PBO buffers.
- A separate process-wide 512 MiB budget for decoded frames that are retained in CPU memory, including pooled byte buffers. Frames that cannot be reserved are discarded rather than allowing unbounded retained memory; the benchmark reports those drops.
- A fair process-wide gate limits concurrent CPU-heavy FFmpeg/HAP decode calls. FFmpeg is no longer asked to auto-create a machine-sized worker team for every stream, and the shared HAP/CPU-fallback pool is bounded.
- Per-decoder metrics for decoded/fetched frames, skipped queued frames, fetch starvation, late frames, decode-time total/max and approximate p95, queue high-water mark, and memory-budget drops.
- The existing playback benchmark prints these metrics per layer.
- Metrics implementation is split into `VideoDecoderMetrics.cpp`.
- The selected-stream FFmpeg packet reader is isolated in `VideoDecoderDemux.cpp` and is shared by normal codecs and HAP.
- Specialized HAP packet parsing/decompression is isolated in `VideoDecoderHap.cpp`; the GPU-compressed texture path is retained.
- Frame delivery/upload-buffer handling is isolated in `VideoDecoderDelivery.cpp`.
- Pixel-format layout and colorimetry are isolated in `VideoDecoderLayout.cpp`.
- Seek requests and codec flushing are isolated in `VideoDecoderSeek.cpp`.

## Important definitions and trade-offs

- The decode-time p95 is an approximation from fixed histogram buckets; it avoids allocating or retaining a sample for every frame.
- “Late” means the fetched frame's age relative to the requested timeline clock exceeds 1.5 source-frame intervals. It is a useful warning signal, not a full render-deadline measurement.
- The memory caps cover mapped upload buffers and retained decoded frame payload estimates. They do not include every allocation made by FFmpeg, the GPU's output textures, or driver-internal surfaces.
- The gate bounds simultaneous decode/decompression work, not the number of lightweight per-stream sequencing threads. Replacing those threads with a shared scheduler would be a larger architectural step and should follow measurements rather than be mixed into this first patch.
- A memory-budget rejection is visible as a drop counter. It is preferable to unbounded retained frame memory, but the cap and scheduling policy should be tuned against representative compositions.

## Validation still required

- Run CI on all supported platforms.
- Run `pinol_bench` with mixed H.264/HEVC/ProRes/HAP compositions at 1080p and 4K, both with hardware decode and `--software`.
- Compare p95/max decode times, late/missed/skipped frames, memory-budget drops, and render FPS before and after.
- Stress open/close, seek, loop and ping-pong playback while the global budgets are under pressure.

## Next extraction steps

The next seam is playback scheduling: `run()`, timeline-leg transitions, and backward-window production. Move those only after making the decode gate and resource-accounting ownership explicit across translation units. Codec setup/open/close can then be separated from the remaining controller. Keep each extraction behavior-preserving and benchmarkable; do not replace the specialized HAP compressed-texture path with generic CPU decoding without evidence.
