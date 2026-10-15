# Timeline Repeat → sequence steps: functional and visual regression

This regression protects the **finite Loop/Repeat + AutoFollow + post-wait** path.

## Cause

When a finite animation stopped, `Engine::animationRemaining()` returned zero. The cue clock's running-step code nevertheless reset the expected action duration to `elapsed + 0` on **every** tick. For any positive `postWait`, the intended `continueAt = preWait + duration + postWait` deadline moved forward as fast as elapsed time: the next cue could wait forever.

This PR freezes `duration` when a tracked timeline finishes. While it is still playing, duration continues to track real remaining time, accounting for speed/pause/loop-mode changes.

## Automated regression

```sh
cmake --build build --target pinol_tests
ctest --test-dir build --output-on-failure -R '^timeline_repeat$'
```

`test/timeline_repeat.cpp` runs the actual Pinol engine with manual frame progression at **60 and 120 virtual Hz** and a real OpenGL render/readback of an animated image. It verifies:

- Three repetitions of a 1.0 s timeline (sinusoidal opacity and smooth keyframed rotation).
- `AutoFollow` + 250 ms after the third repeat, followed by a distinct timeline `Play` step; `Follow` + 100 ms to `Stop`.
- Step transition times within two synthetic frame periods, plus parameter values and GPU pixel samples across loop boundaries.
- An infinite Repeat continues when the UI selects a different sequence.
- PingPong value position is continuous at turnaround boundaries.

**A virtual-clock test cannot establish that a real GPU displays every frame on time.** A periodic oscillator can be visually smooth despite dropping output frames. It is necessary to test on a real output surface/receiver.

## Physical show acceptance

1. Compare the same project/media on the same GPU/driver with `main` before and after this PR, at 60 and 120 Hz.
2. Capture frames at the projector or Syphon/Spout receiver if possible; inspect actual frame IDs and duplicate/drop events.
3. Run `Pinol --endurance-csv session.csv show.pinol` for a sustained test. Use:
   ```sh
   python3 scripts/analyze_timeline_endurance.py session.csv --fps 60
   ```
4. Check `interval_max_ms`, one-second `interval_p99_ms`, output drops, and `frame_over_2x_budget` events around loop boundaries and cue triggers. Beware that the current endurance CSV's share-skip key may be mismatched; use `frameTimingJson()`'s `gpu_shares_skipped` until corrected.

## Merge criteria

CI succeeds (Linux/macOS/Windows), the new CTest area is green, and no unexpected change in cue timing or output pixels is detected. Real GPU/receiver smoothness remains a separate hardware acceptance step. No change to serialized project formats.
