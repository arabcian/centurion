"""No hardware or third-party dependencies required for these tests."""
import importlib.util
from pathlib import Path
import tempfile
import unittest
import json

SCRIPT = Path(__file__).resolve().parents[1] / "scripts" / "compare_autotune_timings.py"
spec = importlib.util.spec_from_file_location("compare_autotune_timings", SCRIPT)
mod = importlib.util.module_from_spec(spec)
spec.loader.exec_module(mod)


def make(ms, goal="gaming", mode="deep"):
    return {"ok": True, "goal": goal, "explore_mode": mode, "timings_ms": {
        "inputs": ms / 5, "model": ms / 4, "decision": ms / 2, "total": ms}}


class TimingComparisonTests(unittest.TestCase):
    def test_percentile(self):
        self.assertEqual(mod.percentile([5], 95), 5)
        self.assertEqual(mod.percentile([1, 3, 5], 50), 3)
        self.assertAlmostEqual(mod.percentile([1, 3, 5], 95), 4.8)

    def test_speedup_and_mode(self):
        summary = mod.summarize([make(100)] * 10, [make(40, mode="fast")] * 10)
        self.assertEqual(summary["stages"]["total"]["p50_speedup"], 2.5)
        self.assertEqual(summary["candidate_mode"], "fast")

    def test_rejects_mixed_goal(self):
        with self.assertRaises(ValueError):
            mod.summarize([make(100)], [make(50, goal="desktop")])

    def test_rejects_bad_timing(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory) / "bad.jsonl"
            p.write_text(json.dumps(make(-1)) + "\n")
            with self.assertRaises(ValueError):
                mod.read_timings(p)

    def test_reads_jsonl(self):
        with tempfile.TemporaryDirectory() as directory:
            p = Path(directory) / "timings.jsonl"
            p.write_text("\n".join(json.dumps(make(100 + i)) for i in range(5)))
            self.assertEqual(len(mod.read_timings(p)), 5)


if __name__ == "__main__":
    unittest.main()
