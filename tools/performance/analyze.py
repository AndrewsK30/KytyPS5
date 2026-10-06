"""Analyze native renderer diagnostics. Uses only the Python standard library."""
import argparse
import csv
import json
import math
from pathlib import Path


def percentile(values, fraction):
    values = sorted(values)
    position = (len(values) - 1) * fraction
    low = math.floor(position)
    high = math.ceil(position)
    return values[low] + (values[high] - values[low]) * (position - low)


def flip_window(rows, start_flip, end_flip):
    """Seconds bounding presentations start_flip..end_flip (presentation_id is cumulative).

    Selecting by presentation count keeps the same scene across runs whose boot/loading time
    differs, which a window measured from launch does not."""
    times = {int(row["presentation_id"]): int(row["elapsed_ns"]) for row in rows if row["row_type"] == "present"}
    if start_flip not in times or end_flip not in times:
        raise ValueError(f"Presentations {start_flip}..{end_flip} not in capture (last {max(times, default=0)})")
    return times[start_flip] / 1e9, times[end_flip] / 1e9


def load_rows(path):
    with Path(path).open(newline="", encoding="utf-8") as source:
        rows = list(csv.DictReader(source))
    # The harness force-stops the emulator, which can cut the last row mid-write. Only a
    # trailing short row is tolerated; a short row elsewhere still fails the analysis.
    if rows and None in rows[-1].values():
        rows.pop()
    return rows


def analyze(path, start_seconds, end_seconds, rows=None):
    if rows is None:
        rows = load_rows(path)
    previous_time = 0
    selected = []
    intervals = []
    # A delta belongs to (previous row time, row time]. Include only complete deltas in range.
    for row in rows:
        timestamp = int(row["elapsed_ns"])
        if timestamp < previous_time:
            raise ValueError("Nonmonotonic diagnostic timestamps")
        if previous_time >= start_seconds * 1e9 and timestamp <= end_seconds * 1e9:
            selected.append((previous_time, timestamp, row))
            duration = int(row["present_interval_ns"])
            if row["row_type"] == "present" and duration > 0 and timestamp - duration >= start_seconds * 1e9:
                intervals.append(duration / 1e6)
        previous_time = timestamp
    if not selected:
        raise ValueError("No complete diagnostic intervals in selected range")
    elapsed = (selected[-1][1] - selected[0][0]) / 1e9
    totals = {name: sum(int(row[name]) for _, _, row in selected)
              for name in rows[0] if name not in {"row_type", "elapsed_ns", "presentation_id", "present_interval_ns"}}
    presents = sum(row["row_type"] == "present" for _, _, row in selected)
    result = {"source": str(path), "range_seconds": [selected[0][0] / 1e9, selected[-1][1] / 1e9],
              "duration_seconds": elapsed, "presentations": presents,
              "presentation_fps": presents / elapsed if elapsed else None,
              "completed_flips": totals["flip_completed"],
              "flip_completion_rate": totals["flip_completed"] / elapsed if elapsed else None,
              "shader_compilations": totals["shader_compile"], "totals": totals,
              "host_timing_ms": {key: value / 1e6 for key, value in totals.items() if key.endswith("_ns")},
              "note": "Host wall times can overlap across threads; do not add them as frame costs. Counter deltas are temporal correlation, not guest frame ownership. Timings are attributed when the scope completes."}
    if intervals:
        result["present_interval_ms"] = {"median": percentile(intervals, .5),
                                          "p95": percentile(intervals, .95), "p99": percentile(intervals, .99)}
    if presents:
        result["renderer_submits_per_present"] = totals["renderer_submit"] / presents
    categories = ["cpu_readback_bytes", "cpu_writeback_bytes", "gpu_readback_bytes",
                  "gpu_writeback_bytes", "metadata_readback_bytes", "gc_readback_bytes",
                  "unknown_readback_bytes"]
    if all(name in totals for name in categories):
        result["readback_attribution_bytes"] = {name: totals[name] for name in categories}
        result["readback_attribution_matches_total"] = sum(totals[name] for name in categories) == totals["buffer_readback_bytes"]
        result["readback_attribution_note"] = "Snapshots may split related counters across interval boundaries. Cpu/Gpu refers to the requesting host thread; fault requested bytes can be one byte, not the instruction's complete access width."
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv")
    parser.add_argument("--start-seconds", type=float, default=0)
    parser.add_argument("--end-seconds", type=float, default=float("inf"))
    parser.add_argument("--start-flip", type=int, help="first presentation id; overrides seconds")
    parser.add_argument("--end-flip", type=int, help="last presentation id; overrides seconds")
    parser.add_argument("--output")
    args = parser.parse_args()
    if (args.start_flip is None) != (args.end_flip is None):
        parser.error("--start-flip and --end-flip go together")
    if args.start_flip is not None and args.end_flip <= args.start_flip:
        parser.error("Require start flip < end flip")
    try:
        rows = load_rows(args.csv)
        if args.start_flip is not None:
            args.start_seconds, args.end_seconds = flip_window(rows, args.start_flip, args.end_flip)
        if args.start_seconds < 0 or args.end_seconds <= args.start_seconds:
            parser.error("Require 0 <= start < end")
        report = analyze(args.csv, args.start_seconds, args.end_seconds, rows)
        if args.start_flip is not None:
            report["flip_range"] = [args.start_flip, args.end_flip]
    except (ValueError, KeyError) as error:
        parser.exit(1, f"Invalid or incomplete diagnostic data: {error}\n")
    encoded = json.dumps(report, indent=2, allow_nan=False)
    if args.output:
        Path(args.output).write_text(encoded + "\n", encoding="utf-8")
    print(encoded)
