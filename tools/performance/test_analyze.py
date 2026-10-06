import csv
import importlib.util
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location("analyze", Path(__file__).with_name("analyze.py"))
module = importlib.util.module_from_spec(spec)
spec.loader.exec_module(module)
analyze = module.analyze
trace_spec = importlib.util.spec_from_file_location("readbacks", Path(__file__).with_name("readbacks.py"))
trace_module = importlib.util.module_from_spec(trace_spec)
trace_spec.loader.exec_module(trace_module)


class AnalysisTests(unittest.TestCase):
    def test_readback_samples_sort_completion_order_and_keep_sample_totals(self):
        first = dict(elapsed_ns=1000000, source=2, address=4096, buffer_address=4096,
                     window_address=4096, tick_begin=10, tick_end=11, copies=1,
                     payload_bytes=4, packed_bytes=64, dispatch_ns=100000,
                     prepare_ns=1000, gpu_wait_ns=10000, publish_wait_ns=2000, total_ns=113000)
        later = dict(first, elapsed_ns=3000000, address=8192, tick_begin=11, tick_end=11,
                     copies=0, payload_bytes=0, packed_bytes=0)
        result = trace_module.summarize([later, first])
        source = result['by_source']['cpu_write']
        self.assertEqual(source['samples'], 2)
        self.assertEqual(source['no_copy_samples'], 1)
        self.assertEqual(source['packed_bytes'], 64)
        self.assertEqual(result['top_windows'][0]['median_gap_ms'], 2)
        self.assertEqual(result['top_windows'][0]['next_begin_equals_previous_end'], 1)
        self.assertEqual(trace_module.summarize([])['samples'], 0)

    def test_samples_do_not_count_as_frames_and_counters_are_not_double_counted(self):
        with tempfile.TemporaryDirectory() as folder:
            path = Path(folder) / "sample.csv"
            fields = ["row_type", "elapsed_ns", "presentation_id", "present_interval_ns",
                      "flip_completed", "renderer_submit", "shader_compile", "queue_submit_ns"]
            with path.open("w", newline="") as target:
                writer = csv.writer(target)
                writer.writerow(fields)
                writer.writerows([
                    ["sample", 1000000000, 0, 0, 0, 10, 1, 1000000],
                    ["present", 2000000000, 1, 0, 1, 20, 0, 2000000],
                    ["present", 3000000000, 2, 1000000000, 1, 30, 0, 3000000],
                    ["sample", 4000000000, 2, 0, 0, 40, 0, 4000000],
                ])
            result = analyze(path, 1, 4)
            self.assertEqual(result["presentations"], 2)
            self.assertEqual(result["totals"]["renderer_submit"], 90)
            self.assertEqual(result["shader_compilations"], 0)
            self.assertEqual(result["presentation_fps"], 2 / 3)
            self.assertEqual(result["present_interval_ms"]["median"], 1000)
            self.assertEqual(result["host_timing_ms"]["queue_submit_ns"], 9)
            # The incomplete delta across the start boundary must not leak into the report.
            narrowed = analyze(path, 1.5, 4)
            self.assertEqual(narrowed["totals"]["renderer_submit"], 70)


if __name__ == "__main__":
    unittest.main()
