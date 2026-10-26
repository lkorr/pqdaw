#!/usr/bin/env python3
"""Tests of append_results.py (T1.11-fix1): dirty-tree refusal, load / preset labelling, higher-is-better summaries.
Run: python tools/bench/test_append_results.py (CTest: pq_tools_append_results)."""

import os
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import append_results as ar  # noqa: E402

HEADER = "suite,benchmark,name,unit,median,min,p99,max,samples,load_percent,cpu,build,preset,better\n"
OLD_HEADER = "suite,benchmark,name,unit,median,min,p99,max,samples,load_percent,cpu,build\n"


class AppendResults(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.results = os.path.join(self.dir.name, "RESULTS.md")
        with open(self.results, "w", encoding="utf-8", newline="\n") as f:
            f.write("# r\n\n| Date | Commit | Benchmark | Config | Result | Notes |\n|---|---|---|---|---|---|\n")

    def tearDown(self):
        self.dir.cleanup()

    def csv(self, lines, header=HEADER, name="r.csv"):
        path = os.path.join(self.dir.name, name)
        with open(path, "w", encoding="utf-8", newline="\n") as f:
            f.write(header + "".join(line + "\n" for line in lines))
        return path

    def run_main(self, csv_path, *extra, status=""):
        return ar.main([csv_path, "--results", self.results, "--date", "2026-10-25", "--dry-run", *extra], status=status)

    def test_baseline_row(self):
        p = self.csv(["pq_bench_dsp,atom/add,atom/add L1,ns/sample,0.08,0.07,0.09,0.1,15,3.0,CPU X,release,ship,lower"])
        [row] = self.run_main(p)
        self.assertIn("| atom/add L1 | ship (LTCG), pq_bench_dsp |", row)
        self.assertIn("0.0800 ns/sample median (min 0.0700, p99 0.0900, n=15)", row)
        self.assertNotIn("indicative", row)
        self.assertIn("system load 3 %", row)

    def test_dirty_tree_is_refused(self):
        p = self.csv(["s,b,n,ns/op,1,1,1,1,15,3.0,CPU,release,ship,lower"])
        with self.assertRaises(SystemExit) as e:
            self.run_main(p, status=" M src/core/src/cpu.cpp\n")
        self.assertIn("dirty", str(e.exception.code))
        with self.assertRaises(SystemExit):
            self.run_main(p, status="?? tools/new_untracked.py\n")  # untracked files count too
        with self.assertRaises(SystemExit):
            self.run_main(p, "--indicative", status=" M src/a.cpp\n")  # --indicative does not excuse a dirty tree

    def test_results_and_csv_changes_are_allowed(self):
        inside = os.path.join(ar.REPO, "bench", "fix1_test_rows.csv")
        rel_csv = "bench/fix1_test_rows.csv"
        with open(inside, "w", encoding="utf-8", newline="\n") as f:
            f.write(HEADER + "s,b,n,ns/op,1,1,1,1,15,3.0,CPU,release,ship,lower\n")
        try:
            rows = ar.main([inside, "--results", self.results, "--dry-run"],
                           status=f" M bench/RESULTS.md\n?? {rel_csv}\n")
            self.assertEqual(len(rows), 1)
        finally:
            os.remove(inside)

    def test_load_above_25_percent_needs_indicative(self):
        p = self.csv(["s,b,patch32/jit x8,ns/sample,72,70,75,76,15,67.0,CPU,release,ship,lower"])
        with self.assertRaises(SystemExit) as e:
            self.run_main(p)
        self.assertIn("system load 67 % > 25 %", str(e.exception.code))
        [row] = self.run_main(p, "--indicative")
        self.assertIn("indicative: system load 67 % > 25 %", row)
        [row] = self.run_main(p, "--max-load", "80")
        self.assertNotIn("indicative", row)
        unknown = self.csv(["s,b,n,ns/op,1,1,1,1,15,-1.0,CPU,release,ship,lower"], name="u.csv")
        [row] = self.run_main(unknown, "--indicative")
        self.assertIn("indicative: system load unknown", row)

    def test_preset_is_labelled(self):
        p = self.csv(["s,b,n,ns/op,1,1,1,1,15,3.0,CPU,release,release,lower"])
        with self.assertRaises(SystemExit):
            self.run_main(p)
        [row] = self.run_main(p, "--indicative")
        self.assertIn("| release (no LTCG), s |", row)
        self.assertIn("indicative: release (no LTCG) build, not ship", row)
        old = self.csv(["s,b,n,ns/op,1,1,1,1,15,3.0,CPU,release"], header=OLD_HEADER, name="old.csv")
        [row] = self.run_main(old, "--indicative")  # an old CSV cannot tell ship from release
        self.assertIn("indicative: unknown preset build, not ship", row)
        dev = self.csv(["s,b,n,ns/op,1,1,1,1,15,3.0,CPU,dev,dev,lower"], name="dev.csv")
        with self.assertRaises(SystemExit):
            self.run_main(dev, "--indicative")
        [row] = self.run_main(dev, "--indicative", "--allow-dev")
        self.assertIn("| dev, s |", row)

    def test_higher_is_better_summary(self):
        # A speed row: min = the slowest round (worst), p99 field = the low tail (p1), max = the fastest (best).
        p = self.csv(["s,b,render/x speed,x realtime,121,116,117,125,15,3.0,CPU,release,ship,higher"])
        [row] = self.run_main(p)
        self.assertIn("121 x realtime median (worst 116, p1 117, best 125, n=15)", row)
        old = self.csv(["s,b,render/x speed,x realtime,121,116,116,125,15,3.0,CPU,release"], header=OLD_HEADER,
                       name="old.csv")
        [row] = self.run_main(old, "--indicative")  # no `better` column: the unit decides
        self.assertIn("(worst 116, p1 116, best 125, n=15)", row)
        self.assertTrue(ar.higher_is_better("GB/s"))
        self.assertFalse(ar.higher_is_better("ns/sample"))

    def test_filter(self):
        p = self.csv(["s,b,keep me,ns/op,1,1,1,1,15,3.0,CPU,release,ship,lower",
                      "s,b,drop me,ns/op,1,1,1,1,15,3.0,CPU,release,ship,lower"])
        rows = self.run_main(p, "--filter", "^keep")
        self.assertEqual(len(rows), 1)
        self.assertIn("keep me", rows[0])


if __name__ == "__main__":
    unittest.main(verbosity=2)
