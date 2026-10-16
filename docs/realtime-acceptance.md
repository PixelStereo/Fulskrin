# Real-time acceptance gates

These are proposed engineering acceptance targets, fixed before collecting results,
not claims about currently measured hardware. Freeze the Pinol SHA, machine, OS,
GPU/driver, audio device/buffer size, media hashes and output configuration per run.
Do not apply hardware performance gates to software-rendering or sanitizer CI.

## Reference workload

18 active video layers, 1920x1080 composition at 60 Hz, two local outputs, looped
sequence with snapshots, forward/backward seeks and audio mappings. Record codec,
resolution and frame rate of every media asset; repeat with the intended HAP and
interframe-codec mixes rather than treating them as equivalent workloads.
Warm up for 10 minutes, then capture a fresh process/session for the measured run.
Repeat with 1, 8 and 18 layers to identify scaling. No hidden retries of failed runs.

| Metric | Target | Decision if exceeded |
|---|---|---|
| Audio callback duration | p99 <= 1 ms AND <=25% of the callback buffer period for at least 99% of calls; zero calls over the full period | Profile mixing/FFT before considering worker offload; also inspect scheduler/device underruns |
| Render acquisition of engine mutex | p99 <= 1 ms; max <= 4 ms | Correlate with cue work and lock hold time before splitting engine ownership |
| Render mutex hold | p99 <= 8 ms at 60 Hz | Separate source/GL/parameter costs before changing architecture |
| Interaction queue overflow | Zero drops in normal workload | Move flood handling ahead of parameter-index optimization if nonzero |
| Frame pacing | <0.1% intervals over 1.5 periods; no interval over 2 periods outside logged output-loss injection | Correlate compute, present, lock and cue telemetry |

Histogram percentiles are conservative bucket upper bounds, capped by observed
max. New realtime metrics are cumulative since process start (not reset by the
OSC frame-timing reset), clearly named `_lifetime` in JSON/CSV. Read snapshots may
straddle one writer update; use a stationary run or stopped producer for final
boundary decisions. Mutex acquisition includes servicing pending GL tasks while waiting.
Callback duration is wall-clock elapsed time, not hardware
underruns or the OS scheduling lateness before entering the callback.

## Eight-hour endurance run

Existing recorder: `Pinol --endurance-csv session.csv show.pinol`. Use the reference
workload and actual output receivers for eight hours after warm-up. Keep CSV and
adjacent JSONL, media/project hashes and hardware manifest. Require no crash,
no stuck transport, zero unexpected decoder errors or control drops, and the
pacing/audio gates above. Resident memory: compare ten-minute medians after warm-up;
flag a sustained rise above 5 MiB/hour or a final median > baseline + max(64 MiB,5%).
This flags a leak investigation, not proof that RSS growth alone is a leak.
Check that media/sound transport does not accumulate drift (A/V offset <40 ms and
no sustained offset growth). Validate using an external recording/measurement,
not just software clocks. A short CI run cannot establish eight-hour endurance.

## Output disappearance / recovery

On each supported hardware platform, repeat screen unplug/replug and DeckLink
loss/recovery 20 times while the loop is active; include loss during GO and seek.
Require: no process crash/hang, audio and remaining outputs continue, UI remains
responsive, and failure is visible. After reconnect, output must either resume
within 5 seconds or show a clear actionable disconnected/error status; no silent
black output reported healthy. Record recovery latency and device/SDK versions.
Deliberately unavailable output frames are excluded only within logged injection
windows, never excluded from audio or unaffected-output checks.

The current code has output failure counters and DeckLink error states, but this
is not proof of physical hot-unplug recovery. No hardware run is claimed here.

## Scope discipline

Only refactor code touched by a confirmed fix (seek, spectrum, overflow). Index
caches, GPU readback budgets and render-lock restructuring require these measured
gates to fail first. No backward-compatibility layer is required.
