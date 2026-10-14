# Diagnosing cue latency in PINOL

The cue clock already measures when a step *starts* relative to its scheduled
deadline (`cue_late`). That alone cannot detect expensive Undo capture or
scene changes. A cue can fire exactly on time but not finish applying its
snapshot until after the next display frame.

## Measurement

Ask OSCQuery `/composition/timing?VALUE` or read the OSC
`/composition/timing` string. The JSON now contains:

- `cue_late`: lateness of the cue clock's step scheduling (milliseconds).
  Includes cue steps acting on timelines, not only snapshots.
- `cue_capture`: clock-thread time spent preparing the **Undo state** of
  an automatic snapshot recall. Zero if no Undo recorder is connected.
- `cue_recall`: time spent applying that snapshot to the active engine
  (new media, parameters, fades, interactions, etc.).
- `cue_execution`: total elapsed time from beginning of capture until
  completion of snapshot recall, before notifying the interface. This does
  **not** measure subsequent GPU frame rendering/present.
- `cue_to_frame`: separate render-thread histogram measuring time between a
  cue event and the first subsequent completed frame.

The three new work measures use fixed-size millisecond histograms with
`count`, `mean_ms`, `p50_ms`, `p95_ms`, `p99_ms`, and `max_ms`.
Each scheduled snapshot contributes one sample even if Undo recording is
disabled. UI-initiated manual GO calls routed through the interface are **not**
measured by these clock-thread work histograms.

Reset via OSC `/composition/timing/reset`. This resets frame, cue lateness,
cue execution, and dropped-output statistics for the next test run.

## How to evaluate

1. Run the same show with 1, 8, 32 and 64 active sources, and separately
   with equivalent numbers of disabled layers.
2. Reset timing after opening and prewarming all sources.
3. Run 100+ representative cues, mixing snapshots changing simple parameters
   and snapshots swapping media/effect chains.
4. Inspect p95/p99 of `cue_execution` and `cue_to_frame`; compare the
   `cue_capture` and `cue_recall` contributions.
5. Record the GPU, CPU, media formats, composition size, output devices and
   output rates alongside every measurement.

Do not infer a hard real-time guarantee from successful CI. These histograms
are observations, not deadline enforcement. Avoid mixing cold-load and warmed
results. They are especially useful for identifying where a targeted
refactor is actually justified.
