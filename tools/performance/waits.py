"""Rank blocking waits by call site and split GPU-thread sync from work. Standard library only.

Reads the wait_<site>_<field> columns written by performanceDiagnostics. Host times overlap
across threads: per-site blocked ms is the time the waiting thread lost, not a frame cost to add.
"""
import argparse
import json
import sys
from pathlib import Path

# The portable embedded Python does not put the script directory on sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from analyze import analyze, flip_window, load_rows  # noqa: E402

FIELDS = ("calls", "blocking", "ns", "lag_ticks", "drains")
SUBMITS = ("label", "processor", "flip", "drain", "finish", "shutdown", "other")
CATEGORIES = ("pm4", "draw", "dispatch", "bindings", "buffer_cache", "image_cache", "tiler", "pipeline",
              "wait", "submit", "gc", "command", "materialize")


def wait_sites(totals):
    sites = sorted({name[len("wait_"):-len("_calls")] for name in totals
                    if name.startswith("wait_") and name.endswith("_calls")})
    return {site: {field: totals[f"wait_{site}_{field}"] for field in FIELDS} for site in sites}


def report(path, start_seconds, end_seconds, rows=None):
    base = analyze(path, start_seconds, end_seconds, rows)
    totals = base["totals"]
    presents = base["presentations"]

    def per_present(value):
        return value / presents if presents else None

    table = []
    for site, values in wait_sites(totals).items():
        if values["calls"] == 0:
            continue
        blocking = values["blocking"]
        table.append({
            "site": site,
            "calls": values["calls"],
            "blocking": blocking,
            "instant_pct": 100.0 * (values["calls"] - blocking) / values["calls"],
            "blocked_ms": values["ns"] / 1e6,
            "blocked_ms_per_present": per_present(values["ns"] / 1e6),
            "mean_blocked_us": values["ns"] / blocking / 1e3 if blocking else 0.0,
            "mean_lag_ticks": values["lag_ticks"] / blocking if blocking else 0.0,
            "drains": values["drains"],
        })
    table.sort(key=lambda entry: entry["blocked_ms"], reverse=True)

    submits = {reason: totals.get(f"submit_{reason}", 0) for reason in SUBMITS}

    def ms(name):
        return totals.get(name, 0) / 1e6

    gpu_waits = ms("gpu_semaphore_wait_ns")
    categories = {name: {"ms_per_present": per_present(ms(f"cat_{name}_ns")),
                         "calls_per_present": per_present(totals.get(f"cat_{name}_calls", 0)),
                         "us_per_call": (totals.get(f"cat_{name}_ns", 0) / totals[f"cat_{name}_calls"] / 1e3)
                         if totals.get(f"cat_{name}_calls") else None}
                  for name in CATEGORIES if f"cat_{name}_ns" in totals}
    return {
        "gpu_thread_categories": categories,
        "source": base["source"], "range_seconds": base["range_seconds"],
        "duration_seconds": base["duration_seconds"], "presentations": presents,
        "presentation_fps": base["presentation_fps"],
        "present_interval_ms": base.get("present_interval_ms"),
        "wait_sites": table,
        "renderer_submits": totals.get("renderer_submit", 0),
        "submits_by_reason": submits,
        "submits_by_reason_per_present": {reason: per_present(count) for reason, count in submits.items()},
        "gpu_thread_ms_per_present": {
            "processing_pm4": per_present(ms("gpu_process_ns")),
            "sync_commands": per_present(ms("gpu_command_ns")),
            "semaphore_waits_inside_busy": per_present(gpu_waits),
            "busy_minus_semaphore_waits": per_present(ms("gpu_process_ns") + ms("gpu_command_ns") - gpu_waits),
            "label_blocked": per_present(ms("wait_gpu_label_blocked_ns")),
            "idle": per_present(ms("gpu_idle_ns")),
        },
        "note": "blocked_ms counts real waits only; instant_pct are waits whose tick or predicate was "
                "already satisfied. Lag ticks = submitted-but-incomplete ticks when a semaphore wait "
                "started. GPU-thread semaphore waits happen inside processing/commands, so "
                "busy_minus_semaphore_waits approximates GPU-thread work; condition-variable waits on "
                "that thread (publish_wait) are not subtracted.",
    }


def render_table(result):
    header = (f"{'site':<26}{'calls':>10}{'block':>9}{'inst%':>7}{'ms':>11}{'ms/pres':>9}"
              f"{'us/blk':>9}{'lag':>7}{'drain':>8}")
    lines = [header]
    for entry in result["wait_sites"]:
        per = entry["blocked_ms_per_present"] or 0.0
        lines.append(f"{entry['site']:<26}{entry['calls']:>10}{entry['blocking']:>9}"
                     f"{entry['instant_pct']:>7.1f}{entry['blocked_ms']:>11.1f}{per:>9.2f}"
                     f"{entry['mean_blocked_us']:>9.1f}{entry['mean_lag_ticks']:>7.1f}{entry['drains']:>8}")
    lines.append("")
    lines.append("submits/present: " + ", ".join(
        f"{reason}={value:.1f}" for reason, value in result["submits_by_reason_per_present"].items()
        if value))
    categories = result.get("gpu_thread_categories") or {}
    if categories:
        lines.append("")
        lines.append(f"{'gpu thread category (exclusive)':<32}{'ms/pres':>10}{'calls/pres':>12}{'us/call':>10}")
        total = 0.0
        for name, entry in sorted(categories.items(), key=lambda item: -(item[1]["ms_per_present"] or 0)):
            ms_per = entry["ms_per_present"] or 0.0
            total += ms_per
            calls = entry["calls_per_present"] or 0.0
            per_call = entry["us_per_call"] or 0.0
            lines.append(f"{name:<32}{ms_per:>10.2f}{calls:>12.1f}{per_call:>10.1f}")
        lines.append(f"{'total':<32}{total:>10.2f}")
    lines.append("gpu thread ms/present: " + ", ".join(
        f"{name}={value:.2f}" for name, value in result["gpu_thread_ms_per_present"].items()
        if value is not None))
    return "\n".join(lines)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("csv")
    parser.add_argument("--start-seconds", type=float, default=0)
    parser.add_argument("--end-seconds", type=float, default=float("inf"))
    parser.add_argument("--start-flip", type=int)
    parser.add_argument("--end-flip", type=int)
    parser.add_argument("--output")
    parser.add_argument("--table", action="store_true", help="print a text table instead of JSON")
    args = parser.parse_args()
    if (args.start_flip is None) != (args.end_flip is None):
        parser.error("--start-flip and --end-flip go together")
    try:
        rows = load_rows(args.csv)
        if args.start_flip is not None:
            args.start_seconds, args.end_seconds = flip_window(rows, args.start_flip, args.end_flip)
        result = report(args.csv, args.start_seconds, args.end_seconds, rows)
    except (ValueError, KeyError) as error:
        parser.exit(1, f"Invalid or incomplete diagnostic data: {error}\n")
    encoded = json.dumps(result, indent=2, allow_nan=False)
    if args.output:
        Path(args.output).write_text(encoded + "\n", encoding="utf-8")
    print(render_table(result) if args.table else encoded)
