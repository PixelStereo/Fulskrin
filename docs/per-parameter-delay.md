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

Delayed values are ordered by due timestamp in the same **show clock** that drives fades and timelines. The rendering loop only examines due actions, no sleeping, file I/O, or UI timers. A newer recall of the same parameter cancels its prior scheduled action. Address lookup uses an ordered index instead of scanning the queue for each value. Each render tick is split at execution deadlines: a long frame advances a newly started fade only by the time actually remaining after its delay. Loading a new project clears all pending actions.

Supported by this initial PR: ordinary layer **Parameter** addresses (including boolean/choice and typed paths), preset composition opacity/volume/other Parameter addresses, snapshot composition opacity/volume, and interaction parameters. Structural snapshot operations (source replacement, mesh topology, mask topology, visibility, creation/deletion) are *not* yet independently delayed; they remain tied to the snapshot's established source/structure transition semantics. Their timing still works as before. No artificial delay control is shown for those unsupported structural fields.

## Precise ownership and source-transition behavior

A layer parameter recalled again takes ownership immediately and freezes any older snapshot fade of that address during the delay. Manual edits remain possible; the delayed fade samples the live value at execution. A running timeline continues to take precedence over snapshot fades as before. Composition and interaction snapshot ramps also relinquish the recalled address immediately. Other addresses continue normally. A delayed opacity preserves a snapshot’s visibility semantics: a hidden layer stays hidden until its fade-in starts; a fade-out hides the layer only after its delay and fade.

A simultaneous source cut or crossfade does not install the delayed target early. An address common to both sources retains its current value. An address new to the incoming source retains that source's default until execution. Structural operations still execute on their established snapshot timeline; the independently delayed values are the Parameter addresses shown in the Delay column.

Escape discards both the deadline queue and its address index while holding the current values. It also cancels every sequence wait, fade and timeline through the engine's emergency-stop method. This PR is stacked on #173 to make that integration explicit.

## Verification

`ctest --test-dir build -R '^recall_delay$' --output-on-failure` asserts:

- value unchanged before delay; cut at deadline;
- old delayed recall canceled by a newer immediate recall;
- delay plus subsequent fade starts from the live value, finishes later;
- snapshot JSON address delay and duration calculation;
- composition fade/cut dispatch;
- coarse ticks crossing a delay plus fade;
- Escape cancels deferred actions permanently;
- delayed opacity during both source cuts and source crossfades.

The test uses explicit synthetic show-clock advancement. A physical stress test should additionally measure cue timing p95/p99 under GPU load. Cross-platform CI remains a merge gate.
