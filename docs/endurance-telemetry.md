# PINOL endurance telemetry

Run `Pinol --endurance-csv /path/to/session.csv /path/to/show.pinol` during a real show.

This produces `session.csv` (metadata header with build SHA, OpenGL vendor/renderer/version, OS, project SHA-256, then one row per second) and `session.csv.events.jsonl` (timestamped frames whose interval exceeded twice the target period, plus project changes). Percentiles and maxima in CSV are **window-local**, not cumulative. Events are buffered in a fixed-size 128-entry ring per sample window: excess events are counted in the CSV. Disk I/O happens on a separate worker thread, never in rendering.

## Acceptance test

1. Record an eight-hour session on the show machine with actual project/media, projector, Syphon/Spout/NDI outputs and controller.
2. Compare interval_max_ms, compute_p99_ms, outputs_dropped_total, shares_skipped_total and resource budget warnings for consecutive seconds and across versions.
3. Repeat with additional layers, outputs, GO, loop, seeks and receiver reconnections. Correlate late frame events with cue changes by UTC timestamps.
4. Check exact driver using the OpenGL version string; set `PINOL_GPU_DRIVER` if additional vendor driver information is known.
5. **This does not validate receiver-side tearing**; test real receivers separately as described in `docs/gpu-share-validation.md` on the corresponding PR.

Some GPU drivers do not expose free VRAM. If a project was not saved at startup, the header fingerprint reports `unsaved`; project changes are logged separately. Decoder error counters become nonzero once the independent FFmpeg hardening PR (#170) adds `ffmpeg_errors_total` and `empty_video_loops_stopped_total` to `resourcesJson()`; until then these two columns remain zero. Merge #170 before using telemetry for a final endurance acceptance test.
