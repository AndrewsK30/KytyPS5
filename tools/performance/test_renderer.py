import csv
import json
import sys
import tempfile
import unittest
from pathlib import Path

# The portable embedded Python does not put the script directory on sys.path.
sys.path.insert(0, str(Path(__file__).resolve().parent))
from renderer import analyze, analyze_batch, TIMINGS, PROFILES  # noqa: E402


class RendererCaptureTests(unittest.TestCase):
    def fixture(self, directory):
        metadata = {"schema": 1, "workload_version": "cube-grid-v1", "draws_per_frame": 64,
                    "width": 512, "height": 512, "measured_frames_per_profile": 2,
                    "profiles": {name: {"image_hash_u32_fnv1a64": "937"} for name in PROFILES}}
        (directory / "workload.json").write_text(json.dumps(metadata), encoding="utf-8")
        rows = []
        for name in PROFILES:
            for frame in range(2):
                rows.append(dict(profile=name, frame=frame, **{key: 1.0 for key in TIMINGS},
                                 render_submits=16 if name == "submits" else 1,
                                 readback_bytes=512*512*16 if name == "readback" else 0))
        return rows

    def write_rows(self, directory, rows):
        with (directory / "frames.csv").open("w", newline="", encoding="utf-8") as output:
            writer = csv.DictWriter(output, fieldnames=rows[0].keys())
            writer.writeheader()
            writer.writerows(rows)

    def test_complete_and_rejected_workloads(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            rows = self.fixture(directory)
            self.write_rows(directory, rows)
            self.assertEqual(analyze(directory)["profiles"]["raster"]["frames"], 2)
            self.write_rows(directory, rows[:-1])
            with self.assertRaisesRegex(ValueError, "incomplete"):
                analyze(directory)
            self.write_rows(directory, rows)
            metadata = json.loads((directory / "workload.json").read_text())
            metadata["profiles"]["submits"]["image_hash_u32_fnv1a64"] = "different"
            (directory / "workload.json").write_text(json.dumps(metadata))
            with self.assertRaisesRegex(ValueError, "different images"):
                analyze(directory)

    def test_rejects_false_performance_evidence(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            rows = self.fixture(directory)
            for field, value, reason in (("gpu_span_ms", "nan", "nonfinite"),
                                         ("frame", 1, "nonconsecutive"),
                                         ("render_submits", 2, "submit count"),
                                         ("readback_bytes", 4, "byte count")):
                broken = [dict(row) for row in rows]
                broken[0][field] = value
                self.write_rows(directory, broken)
                with self.assertRaisesRegex(ValueError, reason):
                    analyze(directory)

    def test_batch_requires_matching_complete_runs(self):
        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp)
            (directory / "manifest.json").write_text(json.dumps({"runs": 2, "complete": True}))
            for index in range(2):
                run = directory / f"run-{index:02}"
                run.mkdir()
                self.write_rows(run, self.fixture(run))
                metadata = json.loads((run / "workload.json").read_text())
                metadata.update(device_id=9860, vendor_id=4318, driver_version=1,
                                cubes=64, vertices_per_draw=36)
                (run / "workload.json").write_text(json.dumps(metadata))
            self.assertEqual(analyze_batch(directory)["runs"], 2)
            metadata["draws_per_frame"] = 128
            # Keep internal counts consistent; only the workload signature differs.
            self.write_rows(run, [dict(row, render_submits=32 if row["profile"] == "submits" else 1)
                                  for row in self.fixture(run)])
            (run / "workload.json").write_text(json.dumps(metadata))
            with self.assertRaisesRegex(ValueError, "mixed hardware/workload"):
                analyze_batch(directory)
            (directory / "manifest.json").write_text(json.dumps({"runs": 2, "complete": False}))
            with self.assertRaisesRegex(ValueError, "incomplete batch manifest"):
                analyze_batch(directory)


if __name__ == "__main__":
    unittest.main()
