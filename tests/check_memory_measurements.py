"""Tests of measurement semantics and failure handling, not parser speed."""
import json
import io
import os
from pathlib import Path
import subprocess
import sys
import unittest
from unittest.mock import patch
import tempfile

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "bench"))
import memory_audit as audit
import report
import bench as benchmark


class MemoryMeasurements(unittest.TestCase):
    def test_native_buffer_is_visible(self):
        # Run separately to avoid a previous test's allocator high-water mark.
        code = """import json, memory_audit as m
m.snapshot()
a=m.snapshot()
buffer=bytearray(32*1024*1024)
for i in range(0,len(buffer),4096): buffer[i]=1
b=m.snapshot()
print(json.dumps([a,b]))
"""
        p = subprocess.run([sys.executable, "-c", code], cwd=ROOT / "bench", capture_output=True, text=True, check=True)
        before, held = json.loads(p.stdout)
        self.assertGreater(held["current_bytes"] - before["current_bytes"], 16 * 1048576)
        self.assertGreaterEqual(held["peak_bytes"], before["peak_bytes"])

    def test_freed_buffers_do_not_accumulate_after_warmup(self):
        # The growth flag compares rounds with a snapshot taken after warmup.
        # An allocator may keep a freed buffer for reuse (glibc keeps one, so
        # Linux RSS stays one buffer higher), but freeing a buffer of the same
        # size again and again must not keep adding to current memory. On
        # macOS, RSS from ps did: it rose by one freed buffer every few rounds.
        code = """import gc, json, memory_audit as m
def churn():
    b=bytearray(16*1024*1024)
    for i in range(0,len(b),4096): b[i]=1
    keep.append(bytearray(64*1024))  # shifts the heap so a new region may be used
    del b
    gc.collect()
keep=[]
for r in range(3): churn()
a=m.snapshot()
for r in range(12): churn()
c=m.snapshot()
print(json.dumps([a,c]))
"""
        p = subprocess.run([sys.executable, "-c", code], cwd=ROOT / "bench", capture_output=True, text=True, check=True)
        warm, after = json.loads(p.stdout)
        # 12 x 64 KiB is kept on purpose (0.75 MiB); a freed 16 MiB buffer must not add to it
        self.assertLess(after["current_bytes"] - warm["current_bytes"], 8 * 1048576)

    @unittest.skipIf(sys.platform == "darwin", "macOS ru_maxrss did not keep a freed 32 MiB buffer as the peak "
                     "(CI, macos-14: the later 8 MiB buffer raised it by 8.0 MiB); this masking is Windows/Linux behaviour")
    def test_zero_peak_delta_can_hide_a_real_buffer(self):
        code ="""import gc,json,memory_audit as m
m.snapshot()
big=bytearray(32*1024*1024)
for i in range(0,len(big),4096): big[i]=1
del big
gc.collect()
peak=m.peak_memory()
small=bytearray(8*1024*1024)
for i in range(0,len(small),4096): small[i]=1
print(json.dumps([len(small),m.peak_memory()-peak]))
"""
        p = subprocess.run([sys.executable, "-c", code], cwd=ROOT / "bench", capture_output=True, text=True, check=True)
        size, peak_delta = json.loads(p.stdout)
        self.assertEqual(size, 8 * 1048576)
        self.assertEqual(peak_delta, 0)

    def test_crashed_worker_with_valid_output_is_failure(self):
        with self.assertRaises(RuntimeError):
            audit.collect([sys.executable, "-c", "import json; print(json.dumps(dict(baseline={}))); raise SystemExit(1)"], os.environ.copy())

    def test_empty_and_invalid_output_are_failures(self):
        for code in ("pass", "print('{}')", "print('[]')"):
            with self.assertRaises((ValueError, json.JSONDecodeError)):
                audit.collect([sys.executable, "-c", code], os.environ.copy())

    def test_failed_or_growing_audit_cannot_pass(self):
        for value in (RuntimeError("metric unavailable"), {"baseline": {},
                "first_call_peak_delta_bytes": 0, "growth_flag": True}):
            with tempfile.TemporaryDirectory() as folder:
                target = Path(folder) / "audit.json"
                argv = ["memory_audit.py", "--libs", "oxjson", "--docs", "unused",
                        "--repeats", "1", "--output", str(target)]
                kwargs = {"side_effect": value} if isinstance(value, Exception) else {"return_value": value}
                with patch.object(sys, "argv", argv), patch.object(audit, "collect", **kwargs), patch("sys.stdout", new_callable=io.StringIO):
                    self.assertEqual(audit.main(), 1)
                self.assertEqual(json.loads(target.read_text())["status"], "FAIL")

    def test_incomplete_success_payload_is_rejected(self):
        with self.assertRaises(ValueError):
            audit.collect([sys.executable, "-c", "import json; print(json.dumps(dict(baseline={})))"], os.environ.copy())

    def test_intermediate_growth_is_not_hidden_by_final_drop(self):
        warm = {"current_bytes": 100}
        rounds = [{"current_bytes": 100 + 9 * 1048576}, {"current_bytes": 100}]
        self.assertEqual(audit.growth_metrics(warm, rounds), (0, 9 * 1048576, True))

    def test_library_probe_crash_is_not_missing_package(self):
        failed = subprocess.CompletedProcess([], 1, '{"version":"x", "disk":0}', "crash")
        with patch.object(sys, "argv", ["bench.py"]), patch.object(benchmark.subprocess, "run", return_value=failed):
            with self.assertRaises(RuntimeError):
                benchmark.main()

    def test_small_positive_delta_is_not_rendered_as_zero(self):
        self.assertEqual(report.fmt_mb(4096), "<0.1")

    def test_zero_hwm_delta_and_failed_rows_are_not_winners(self):
        result = {"docs": ["x"], "libs": {
            "a": {"docs": {"x": {"loads_peak_bytes": 0}}},
            "b": {"docs": {"x": {"loads_peak_bytes": 100, "error": "worker crash"}}}}}
        rendered = report.table(result, "loads_peak_bytes", report.fmt_mb, ["a", "b"])
        self.assertIn("| x | - | - |", rendered)
        self.assertNotIn("**", rendered)
        result["libs"]["a"]["docs"]["x"]["loads_peak_bytes"] = 1048576
        self.assertNotIn("**", report.table(result, "loads_peak_bytes", report.fmt_mb, ["a"]))


if __name__ == "__main__":
    unittest.main()
