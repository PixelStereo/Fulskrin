# Per-parameter execution delays

Preset values now have `delay` (seconds, default **0.00**); a snapshot's saved **parameter** JSON can have a `delay` map, mirroring `timing` and `easing` keys.

Examples:

```json
{"layer":"42","path":"opacity","value":0.8,"time":0.5,"delay":2.0}
```

and, in a snapshot's layer or composition object:

```json
{"params":{"opacity":0.8},"timing":{"opacity":0.5},"delay":{"opacity":2.0}}
```

Delay and transition time are independent: hold the existing parameter for 2 s; then fade from its **current value** to 0.8 over 0.5 s. A zero-duration transition makes an instantaneous cut after 2 s. All delays of zero use the legacy immediate path without a timer.

## Engine design

Delayed values are ordered by due timestamp in the same **show clock** that drives fades and timelines. The rendering loop only examines due actions, no sleeping, file I/O, or UI timers. A newer recall of the same parameter cancels its prior scheduled action. Loading a new project clears all pending actions.

Supported by this initial PR: ordinary layer **Parameter** addresses (including boolean/choice and typed paths), preset composition opacity/volume/other Parameter addresses, snapshot composition opacity/volume, and interaction parameters. Structural snapshot operations (source replacement, mesh topology, mask topology, visibility, creation/deletion) are *not* yet independently delayed; they remain tied to the snapshot's established source/structure transition semantics. Their timing still works as before. No artificial delay control is shown for those unsupported structural fields.

## Verification

`ctest --test-dir build -R '^recall_delay$' --output-on-failure` asserts:

- value unchanged before delay; cut at deadline;
- old delayed recall canceled by a newer immediate recall;
- delay plus subsequent fade starts from the live value, finishes later;
- snapshot JSON address delay and duration calculation;
- composition fade/cut dispatch.

The test uses explicit synthetic show-clock advancement. A physical stress test should additionally measure cue timing p95/p99 under GPU load. Pressing Esc must also discard the queue before this is merged with the separate emergency-stop PR: add `m_deferredRecall.clear()` in `stopShowPlayback()` after merging those branches. **Do not merge both independently without resolving this integration requirement.**
