#!/usr/bin/env python3
"""Run time-sensitive CTest cases without hiding their first-pass failures.

The first run is the authoritative CI outcome. A second run is diagnostic
only: it helps distinguish a deterministic regression from shared-runner
jitter, but never turns the CI check green.
"""
import subprocess
import sys


def main() -> int:
    cmd = sys.argv[1:]
    if not cmd:
        print("usage: run_timing_tests.py <test command and arguments>", file=sys.stderr)
        return 2

    print("Timing tests: authoritative first pass:", " ".join(cmd), flush=True)
    first = subprocess.run(cmd, check=False).returncode
    if first == 0:
        return 0

    print(
        f"::error::Timing tests failed on first pass (exit {first}). "
        "Re-running once for diagnosis; the original failure remains CI-failing.",
        flush=True,
    )
    second = subprocess.run(cmd, check=False).returncode
    if second == 0:
        print("::warning::Timing tests recovered on diagnostic rerun: investigate intermittent failures.", flush=True)
    else:
        print(f"::error::Timing tests also failed on diagnostic rerun (exit {second}).", flush=True)
    return first


if __name__ == "__main__":
    raise SystemExit(main())
