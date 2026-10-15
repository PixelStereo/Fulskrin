#!/usr/bin/env python3
"""Report Pinol wall-clock pacing around cue/timeline actions.

Usage: python3 scripts/analyze_timeline_endurance.py session.csv --fps 60
The CSV comes from Pinol --endurance-csv, with adjacent .events.jsonl.
This is diagnostics, not an assertion of zero stutter at the physical receiver.
"""
import argparse
import csv
import datetime as dt
import json
from pathlib import Path


def number(row, key):
    try:
        return float(row.get(key) or 0)
    except (TypeError, ValueError):
        return 0.0


def instant(value):
    if not value:
        return None
    try:
        return dt.datetime.fromisoformat(str(value).replace("Z", "+00:00"))
    except ValueError:
        return None


def summarize(path, fps):
    with path.open(encoding="utf-8-sig", newline="") as f:
        rows = list(csv.DictReader(line for line in f if not line.startswith("#")))
    if not rows:
        raise ValueError("No 1Hz metrics rows found")
    budget_ms = 1000.0 / fps
    worst = max(rows, key=lambda r: number(r, "interval_max_ms"))
    slow = [r for r in rows if number(r, "interval_max_ms") > 1.5 * budget_ms]
    misses = sum(number(r, "missed") for r in rows)

    def increase(key):
        vals = [number(row, key) for row in rows]
        return sum(max(0, b - a) for a, b in zip(vals, vals[1:]))

    events_path = Path(str(path) + ".events.jsonl")
    events = []
    if events_path.is_file():
        for line in events_path.read_text(encoding="utf-8").splitlines():
            try:
                events.append(json.loads(line))
            except json.JSONDecodeError:
                continue
    cue_times = [instant(e.get("utc")) for e in events
                 if e.get("kind") in ("cue", "timeline", "snapshot")]
    cue_times = [v for v in cue_times if v is not None]
    late = [e for e in events if e.get("kind") == "frame_over_2x_budget"]
    near = 0
    for event in late:
        t = instant(event.get("utc"))
        if t is not None and any(abs((t - mark).total_seconds()) <= 0.25 for mark in cue_times):
            near += 1

    print(f"Session: {len(rows)} samples / {fps} Hz (budget {budget_ms:.3f} ms)")
    print(f"Worst frame interval: {number(worst, 'interval_max_ms'):.3f} ms at {worst.get('utc')}")
    print(f"Worst 1Hz p99 interval: {max(number(row, 'interval_p99_ms') for row in rows):.3f} ms")
    print(f"Seconds containing a >1.5x-budget frame: {len(slow)}/{len(rows)}")
    print(f"Missed deadlines (sum): {int(misses)}")
    print(f"Dropped output counter increase: {int(increase('outputs_dropped_total'))}")
    print(f">2x-budget events: {len(late)}, within 250 ms of a cue/timeline: {near}")
    if events_path.is_file():
        print(f"Correlated event log: {events_path}")
    else:
        print("No JSONL event file found: action correlation unavailable")
    print("Note: evaluate GPU sharing skips from frameTimingJson['gpu_shares_skipped'];")
    print("older endurance CSV versions may silently log zero due to a field-name mismatch.")
    print("This report does not prove receiver-side smoothness: compare against an unchanged hardware baseline.")


if __name__ == "__main__":
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("csv", type=Path)
    p.add_argument("--fps", type=int, choices=(25, 30, 50, 60, 120), required=True)
    args = p.parse_args()
    summarize(args.csv, args.fps)
