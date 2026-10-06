"""Analyze the deterministic offscreen renderer workload, not guest/game FPS."""
import argparse
import csv
import json
import math
import statistics
import struct
import zlib
from pathlib import Path

TIMINGS = ("cpu_record_ms", "cpu_submit_ms", "cpu_wait_ms", "cpu_readback_ms",
           "host_frame_ms", "gpu_span_ms", "gpu_readback_ms")
PROFILES = ("raster", "submits", "readback")


def export_preview(directory, width, height):
    """Convert the renderer's diagnostic RGB export to a standalone PNG."""
    directory = Path(directory)
    header = f"P6\n{width} {height}\n255\n".encode("ascii")
    ppm = (directory / "scene.ppm").read_bytes()
    if not ppm.startswith(header) or len(ppm) != len(header) + width * height * 3:
        raise ValueError("invalid preview image")
    rgb = ppm[len(header):]
    scanlines = b"".join(b"\0" + rgb[y*width*3:(y+1)*width*3] for y in range(height))
    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    png = (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
           + chunk(b"IDAT", zlib.compress(scanlines)) + chunk(b"IEND", b""))
    (directory / "scene.png").write_bytes(png)


def percentile(values, fraction):
    ordered = sorted(values)
    position = (len(ordered) - 1) * fraction
    low = int(position)
    high = min(low + 1, len(ordered) - 1)
    return ordered[low] + (ordered[high] - ordered[low]) * (position - low)


def analyze(directory):
    directory = Path(directory)
    metadata = json.loads((directory / "workload.json").read_text(encoding="utf-8"))
    if metadata["schema"] != 1 or metadata["workload_version"] not in ("cube-grid-v1", "cube-grid-v2"):
        raise ValueError("unsupported workload/schema")
    groups = {name: [] for name in PROFILES}
    with (directory / "frames.csv").open(newline="", encoding="utf-8") as source:
        for raw in csv.DictReader(source):
            name = raw["profile"]
            if name not in groups:
                raise ValueError("unknown profile")
            row = {key: float(raw[key]) for key in TIMINGS}
            if any(not math.isfinite(value) or value < 0 for value in row.values()):
                raise ValueError("nonfinite or negative timing")
            if row["host_frame_ms"] <= 0 or row["gpu_span_ms"] <= 0:
                raise ValueError("empty frame or missing GPU work")
            if int(raw["frame"]) != len(groups[name]):
                raise ValueError("nonconsecutive/duplicate frame")
            expected_submits = metadata["draws_per_frame"] // 4 if name == "submits" else 1
            if int(raw["render_submits"]) != expected_submits:
                raise ValueError("wrong renderer submit count")
            expected_bytes = metadata["width"] * metadata["height"] * 16 if name == "readback" else 0
            if int(raw["readback_bytes"]) != expected_bytes:
                raise ValueError("wrong readback byte count")
            groups[name].append(row)
    hashes = {metadata["profiles"][name]["image_hash_u32_fnv1a64"] for name in PROFILES}
    if len(hashes) != 1:
        raise ValueError("profiles rendered different images")
    result = {"schema": 1, "workload": metadata, "profiles": {},
              "note": "Offscreen serialized renderer integration, not PS5 or game FPS. GPU span across submits includes queue idle gaps. CPU wait overlaps GPU; do not add CPU and GPU durations. Readback timings include a separate submit/wait plus host mapping/copy. Compilation, geometry creation, warmup, CSV writing and verification are outside measured host_frame_ms."}
    for name, rows in groups.items():
        if len(rows) != metadata["measured_frames_per_profile"]:
            raise ValueError("incomplete capture")
        summary = {"frames": len(rows), "timing_ms": {}}
        for key in TIMINGS:
            values = [row[key] for row in rows]
            summary["timing_ms"][key] = {
                "mean": statistics.fmean(values), "median": statistics.median(values),
                "p95": percentile(values, .95), "p99": percentile(values, .99)}
        result["profiles"][name] = summary
    return result


def analyze_batch(directory):
    directory = Path(directory)
    manifest = json.loads((directory / "manifest.json").read_text(encoding="utf-8-sig"))
    if manifest.get("complete") is not True:
        raise ValueError("incomplete batch manifest")
    paths = sorted(directory.glob("run-*/workload.json"))
    if len(paths) != manifest["runs"]:
        raise ValueError("incomplete batch")
    reports = [analyze(path.parent) for path in paths]
    signature_keys = ("workload_version", "device_id", "vendor_id", "driver_version", "width", "height",
                      "cubes", "vertices_per_draw", "draws_per_frame", "measured_frames_per_profile")
    signature = {key: reports[0]["workload"][key] for key in signature_keys}
    if any({key: report["workload"][key] for key in signature_keys} != signature for report in reports):
        raise ValueError("mixed hardware/workload in batch")
    hashes = {report["workload"]["profiles"]["raster"]["image_hash_u32_fnv1a64"] for report in reports}
    if len(hashes) != 1:
        raise ValueError("runs rendered different images")
    result = {"schema": 1, "runs": len(reports), "signature": signature, "profiles": {},
              "image_hash_u32_fnv1a64": next(iter(hashes)),
              "note": "Range describes repeated runs of the same binary/workload, not confidence in a game-FPS improvement. Compare matching signatures and inspect raw distributions for a future A/B."}
    for name in PROFILES:
        result["profiles"][name] = {}
        for key in TIMINGS:
            medians = [report["profiles"][name]["timing_ms"][key]["median"] for report in reports]
            result["profiles"][name][key] = {
                "median_of_run_medians": statistics.median(medians),
                "minimum_run_median": min(medians), "maximum_run_median": max(medians)}
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory")
    parser.add_argument("--output")
    parser.add_argument("--batch", action="store_true")
    args = parser.parse_args()
    try:
        if args.batch:
            report = analyze_batch(args.directory)
        else:
            report = analyze(args.directory)
            export_preview(args.directory, report["workload"]["width"], report["workload"]["height"])
    except (ValueError, KeyError, OSError) as error:
        parser.exit(1, f"Invalid/incomplete renderer capture: {error}\n")
    encoded = json.dumps(report, indent=2, allow_nan=False) + "\n"
    if args.output:
        Path(args.output).write_text(encoded, encoding="utf-8")
    for name, profile in report["profiles"].items():
        if args.batch:
            print(f"{name}: repeated host median={profile['host_frame_ms']['median_of_run_medians']:.3f} ms, "
                  f"range={profile['host_frame_ms']['minimum_run_median']:.3f}.."
                  f"{profile['host_frame_ms']['maximum_run_median']:.3f} ms")
            continue
        timings = profile["timing_ms"]
        print(f"{name}: host median={timings['host_frame_ms']['median']:.3f} ms, "
              f"GPU span median={timings['gpu_span_ms']['median']:.3f} ms, "
              f"host p95={timings['host_frame_ms']['p95']:.3f} ms")
