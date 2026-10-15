# Cue and timeline UI responsiveness

## Changes

- Threaded-engine GO and snapshot recalls execute on the cue clock. The GUI receives the already applied recall as one Undo record. Manual/non-threaded engines keep synchronous behavior.
- Queued recalls use stable snapshot IDs. Clearing a project invalidates pending Undo delivery through a generation counter.
- Recall Undo captures only affected layers, rather than serializing unrelated layers.
- Recall UI updates are coalesced on a single timer. Existing inspector controls are refreshed in place when source/layout identity is unchanged.
- Snapshot recall moves the library marker in place and retains the selected snapshot parameter editor.
- GO, cue position and modified-state changes update transport and the old/new cue rows. They do not rebuild the sequence table or destroy an active editor.
- UI animation telemetry excludes track/keyframe definitions. Sampling uses a nonblocking engine lock; an occupied engine keeps the last UI sample instead of freezing the transport display.
- The live timeline rack paints one viewport for one or eighteen timelines. The overview always shows every active timeline; detailed rows scroll. Pause is visible and kept in the Active filter. All exposes stopped timelines. Individual play/pause/stop, overview selection and opening the editor use stable IDs.

## Verification

`timeline_activity` checks 18 simultaneous timelines with 10,000 keys each, nonblocking telemetry under contention, filters, one-timeline behavior and preservation of table items across cue changes. It prints synthetic copy/sampling timings for diagnostics; no machine-dependent speed ratio gates CI.

On the local Apple Silicon run, 200 polls over this fixture copied 36 million keys through the old definition API (85.028 ms). The new overview telemetry took 0.038 ms and copied no keys. This is a narrow sampling benchmark, not an end-to-end frame-rate or freeze-duration claim.

`cue_dispatch` deliberately holds a recall on the cue clock for 120 ms and verifies that a GUI heartbeat continues, exactly one Undo record returns on the GUI thread, and clearing the show drops stale delivery. It requires a real OpenGL context and therefore runs in platform CI.

`sequence_ui` and the workspace/mask regressions remain enabled. Local non-GL tests can run with `QT_QPA_PLATFORM=offscreen`; that platform cannot validate OpenGL rendering on macOS. GPU-dependent validation and the generated show render remain required in CI.
