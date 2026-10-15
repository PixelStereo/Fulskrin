# CI validation policy

Every pull request has one stable **Validation** result. It fails when any selected job fails or is cancelled. A documentation-only change still tests the routing policy but does not rebuild a video engine on three operating systems.

| Change | Validation |
|---|---|
| C++ / shaders / application resources | Linux, macOS and Windows builds and engine/widget tests |
| UI or core changes | UI scenario and workspace AddressSanitizer |
| Engine threading / shared state | ThreadSanitizer sound, video and OSC areas |
| HAP / Snappy / OSC parser | Fuzzing with sanitizers |
| Build rules / CI rules | All relevant platform and safety jobs |
| Nightly | UI scenario, ASan, TSan, fuzzing and Valgrind; no redundant platform repackaging |
| Release tag or manual full validation | Full platform and safety validation |

Valgrind remains useful for allocation lifetime, complementing ASan. It moves to nightly/manual validation instead of duplicating a second complete workspace build on every pull request. The route is conservative for unknown changes, first pushes and manual runs. Eight unit tests cover documentation, UI, threading, codecs, build rules, nightly, release and manual routing. The selected checks are written to the run summary.

Superseded branch/PR runs are cancelled. Release-tag runs are never cancelled by this rule. The concurrency scope is the pull request or branch, not the whole repository.

No correctness assertion is relaxed. Timing tests still fail on their first attempt; diagnostic reruns do not turn a failure into success. ThreadSanitizer findings remain failures; the cue-clock fix uses explicit publication and join ordering instead of suppressing application code.
