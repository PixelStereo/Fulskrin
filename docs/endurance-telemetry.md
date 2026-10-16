# PINOL endurance telemetry

Run `Pinol --endurance-csv /path/to/session.csv /path/to/show.pinol` during a real show.

This produces `session.csv` (metadata header with build SHA, OpenGL vendor/renderer/version, OS, project SHA-256, then one row per second) and `session.csv.events.jsonl`: timestamped frames whose interval exceeded twice the target period, and the show's own events (`snapshot_recall`, `timeline`, `seek`, `project_changed`), so that a late frame can be related to the cue that caused it.

The build SHA is regenerated at every build (`cmake/BuildSha.cmake`), not frozen at configure time, and ends in `-dirty` when the build had uncommitted changes. `process_resident_mb` is the whole process's resident memory (Pinol, Qt, FFmpeg, drivers): a sustained rise over hours warrants a leak investigation, but is not proof by itself. `osc_inputs` / `cue_inputs` count the inputs of each second. Existing frame percentiles and maxima in CSV are **window-local**. The new audio callback, render-lock and interaction-overflow columns carry a `_lifetime` suffix and are cumulative since process start; CSV schema is version 2. See [real-time acceptance gates](realtime-acceptance.md) for thresholds and the 18-layer reference workload. Deadline events are buffered 128 per second and journal events 256 per second: excess events are counted in the CSV (`late_events_lost`, `journal_lost`). Disk I/O happens on a separate worker thread, never in rendering.

## Acceptance test

1. Record an eight-hour session on the show machine with actual project/media, projector, Syphon/Spout/NDI outputs and controller.
2. Compare interval_max_ms, compute_p99_ms, outputs_dropped_total, shares_skipped_total and resource budget warnings for consecutive seconds and across versions.
3. Repeat with additional layers, outputs, GO, loop, seeks and receiver reconnections. Correlate late frame events with cue changes by UTC timestamps.
4. Check exact driver using the OpenGL version string; set `PINOL_GPU_DRIVER` if additional vendor driver information is known.
5. **This does not validate receiver-side tearing**; test real receivers separately as described in `docs/gpu-share-validation.md` on the corresponding PR.

Some GPU drivers do not expose free VRAM. If a project was not saved at startup, the header fingerprint reports `unsaved`; project changes are logged separately. Decoder error counters are exported by the engine in `resourcesJson()`.
