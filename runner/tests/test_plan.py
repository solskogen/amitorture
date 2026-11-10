"""Coverage keys and plans: a planned run makes every check the matrix makes."""
import json
import os
import re
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from amitorture import coverage, matrix, plan  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


class Coverage(unittest.TestCase):
    def test_every_option_has_a_group(self):
        for name in matrix.MATRICES:
            self.assertEqual(coverage.unclassified_options(matrix.expand(name)), [], name)

    def test_modules_match_the_disk_sets(self):
        with open(os.path.join(ROOT, "disk", "Makefile")) as fh:
            mk = fh.read()
        sets = {g: m.split() for g, m in re.findall(r"^DISK_SET_(\w+)\s*:=\s*(.*)$", mk, re.M)}
        want = {m: g for g, mods in sets.items() for m in mods}
        self.assertEqual(coverage.MODULE_GROUP, want)
        self.assertEqual(set(coverage.AXES), set(matrix.MODULE_IDS))
        for m, axes in coverage.AXES.items():
            for g in axes:
                self.assertTrue(g in coverage.OPTION_GROUPS or g in coverage.FIELD_GROUPS, (m, g))

    def test_xfail_factors_are_axes(self):
        # an option an xfail rule depends on is a factor of that module
        with open(os.path.join(ROOT, "runner", "amitorture", "xfail.json")) as fh:
            rules = json.load(fh)
        for case_key, entries in rules.items():
            if case_key.startswith("_"):
                continue
            mod = case_key.split(".", 1)[0]
            if mod not in coverage.AXES:
                continue
            for rule in entries if isinstance(entries, list) else [entries]:
                for opt in (rule.get("when") or {}):
                    groups = coverage.option_group(opt)
                    self.assertTrue(any(g in coverage.AXES[mod] or (g == "engine" and "engine_class" in
                                                                     coverage.AXES[mod] and opt in
                                                                     ("cachesize", "cpu_threaded"))
                                        for g in groups),
                                    f"xfail {case_key} depends on {opt}, not an axis of {mod}")


class Plans(unittest.TestCase):
    def test_plan_runs_exactly_the_matrix_keys(self):
        for name in matrix.MATRICES:
            es = matrix.expand(name)
            p, info = plan.build(es)
            self.assertEqual(plan.covered(p), set(coverage.required(es)), name)
            self.assertEqual(info["keys_plan"], info["keys"], name)
            self.assertLessEqual(info["module_runs_plan"], info["module_runs_old"], name)

    def test_twins_run_with_their_reference_and_companions_together(self):
        es = matrix.expand("full")
        p, _ = plan.build(es)
        by = {e["id"]: e for e in p}
        for e in p:
            mods = set(e["plan_modules"])
            if "memory" in mods:
                self.assertIn("memory_os", mods, e["id"])
            ref = e.get("twin_of")
            if ref:
                ref_runs = set(coverage.modules_run(next(x for x in es if x["id"] == ref)))
                self.assertIn(ref, by, e["id"])
                self.assertLessEqual(mods & ref_runs, set(by[ref]["plan_modules"]), e["id"])
            groups = {coverage.MODULE_GROUP[m] for m in mods}
            self.assertEqual(set(e["disk_groups"]), groups, e["id"])
            self.assertEqual(int(e["params"]["modules"], 0) & ~int(matrix.module_mask(e["plan_modules"]), 0), 0)

    def test_allowed_mask(self):
        es = matrix.expand("standard")
        p, _ = plan.build(es, allowed=1 << matrix.MODULE_IDS["blitter"])
        self.assertTrue(p)
        for e in p:
            self.assertEqual(e["plan_modules"], ["blitter"], e["id"])
            self.assertEqual(e["disk_groups"], ["core"], e["id"])
        want = {k for k in coverage.required(es) if k[0] == "blitter"}
        self.assertEqual(plan.covered(p), want)


if __name__ == "__main__":
    unittest.main()
