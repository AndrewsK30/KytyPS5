import csv
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).parent))
import analyze  # noqa: E402
import waits  # noqa: E402


def write_capture(path):
    sites = ["readback_cpu_read", "gpu_label_blocked"]
    fields = ["row_type", "elapsed_ns", "presentation_id", "present_interval_ns", "flip_completed",
              "renderer_submit", "shader_compile", "submit_label", "submit_drain", "gpu_process_ns",
              "gpu_command_ns", "gpu_semaphore_wait_ns", "gpu_idle_ns"]
    fields += [f"wait_{site}_{field}" for site in sites for field in waits.FIELDS]
    # Per row: readback 100 calls, 40 blocking, 4 ms, lag 80, 10 drains; label 5 calls, 5 ms.
    wait_values = [100, 40, 4_000_000, 80, 10, 5, 5, 5_000_000, 0, 0]
    rows = [["sample", 1_000_000_000, 0, 0, 0, 1, 0, 1, 0, 1, 0, 0, 0] + [0] * 10]
    for index in range(1, 5):
        rows.append(["present", (index + 1) * 1_000_000_000, index, 1_000_000_000 if index > 1 else 0,
                     1, 10, 0, 8, 2, 20_000_000, 2_000_000, 4_000_000, 1_000_000] + wait_values)
    with path.open("w", newline="") as target:
        writer = csv.writer(target)
        writer.writerow(fields)
        writer.writerows(rows)


class WaitReportTests(unittest.TestCase):
    def test_flip_window_selects_by_presentation_not_launch_time(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "renderer.csv"
            write_capture(path)
            rows = analyze.load_rows(path)
            self.assertEqual(analyze.flip_window(rows, 2, 4), (3.0, 5.0))
            with self.assertRaises(ValueError):
                analyze.flip_window(rows, 2, 9)

    def test_sites_ranked_with_instant_share_lag_and_per_present_cost(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "renderer.csv"
            write_capture(path)
            start, end = analyze.flip_window(analyze.load_rows(path), 2, 4)
            result = waits.report(path, start, end)
            self.assertEqual(result["presentations"], 2)
            first, second = result["wait_sites"]
            self.assertEqual(first["site"], "gpu_label_blocked")
            self.assertEqual(first["blocked_ms"], 10.0)
            self.assertEqual(second["site"], "readback_cpu_read")
            self.assertEqual(second["calls"], 200)
            self.assertAlmostEqual(second["instant_pct"], 60.0)
            self.assertAlmostEqual(second["blocked_ms_per_present"], 4.0)
            self.assertAlmostEqual(second["mean_blocked_us"], 100.0)
            self.assertAlmostEqual(second["mean_lag_ticks"], 2.0)
            self.assertEqual(second["drains"], 20)
            self.assertEqual(result["submits_by_reason"]["label"], 16)
            budget = result["gpu_thread_ms_per_present"]
            self.assertAlmostEqual(budget["busy_minus_semaphore_waits"], 18.0)
            self.assertAlmostEqual(budget["label_blocked"], 5.0)
            self.assertIn("gpu_label_blocked", waits.render_table(result))

    def test_sites_without_calls_are_omitted(self):
        totals = {f"wait_unknown_{field}": 0 for field in waits.FIELDS}
        self.assertEqual(waits.wait_sites(totals)["unknown"]["calls"], 0)


if __name__ == "__main__":
    unittest.main()
