import unittest

from amitorture import compare, matrix


class Verdict(unittest.TestCase):
    def test_higher_is_better(self):
        self.assertEqual(compare.verdict([100, 101, 99], [90, 91, 89], True, 2)[0], "slower")
        self.assertEqual(compare.verdict([100, 101, 99], [110, 111, 109], True, 2)[0], "faster")
        self.assertEqual(compare.verdict([100, 101, 99], [100.5, 99.5, 101], True, 2)[0], "same")
        # beyond the threshold, but the repeats overlap
        self.assertEqual(compare.verdict([100, 80, 99], [90, 91, 81], True, 2)[0], "borderline")

    def test_lower_is_better_frame_blocks(self):
        v, change = compare.verdict([1.0, 1.02, 0.98], [2.0, 2.1, 1.9], False, 2)
        self.assertEqual(v, "slower")
        self.assertAlmostEqual(change, -50.0)
        self.assertEqual(compare.verdict([1.0, 1.02, 0.98], [0.5, 0.55, 0.52], False, 2)[0], "faster")

    def test_analyse_marks_missing_side_incomplete(self):
        runs = [{"label": "A", "values": {("e", None, "add_l"): (100, False, "host ns/insn"),
                                          ("e", None, "fadd"): (5, False, "host ns/insn")}},
                {"label": "B", "values": {("e", None, "add_l"): (120, False, "host ns/insn")}}]
        rows = {r["blk"]: r for r in compare.analyse(runs, ["A", "B"], 1)}
        self.assertEqual(rows["add_l"]["verdict"], "slower")
        self.assertEqual(rows["fadd"]["verdict"], "incomplete")

    def test_exit_status_fails_when_nothing_or_not_everything_was_measured(self):
        self.assertEqual(compare.exit_status([])[0], 1)
        self.assertEqual(compare.exit_status([{"verdict": "same"}, {"verdict": "incomplete"}])[0], 1)
        self.assertEqual(compare.exit_status([{"verdict": "same"}, {"verdict": "faster"}])[0], 0)
        self.assertEqual(compare.exit_status([{"verdict": "slower"}])[0], 1)

    def test_non_positive_medians_are_incomparable(self):
        self.assertEqual(compare.verdict([0, 0, 0], [1, 1, 1], True, 2), ("incomparable", None))
        self.assertEqual(compare.verdict([1, 1, 1], [-1, -1, -1], False, 2), ("incomparable", None))

    def test_a_failed_launch_fails_the_comparison(self):
        rows = [{"verdict": "same"}]
        ok = {"returncode": 1, "verdict": "PASS (bench diff)"}
        self.assertEqual(compare.exit_status(rows, [ok]), (0, "OK"))
        self.assertEqual(compare.exit_status(rows, [ok, {"returncode": 2, "verdict": "PASS"}]),
                         (1, "LAUNCH FAILED"))
        self.assertEqual(compare.exit_status(rows, [{"returncode": 1, "verdict": "PROBLEMS"}]),
                         (1, "LAUNCH FAILED"))

    def test_parse_cpus(self):
        self.assertEqual(compare.parse_cpus("2-4,8"), {2, 3, 4, 8})
        with self.assertRaises(ValueError):
            compare.parse_cpus(",")


class CycleExactBenchmark(unittest.TestCase):
    def test_ce_entry_without_twins(self):
        entries = {e["id"]: e for e in matrix.expand("bench")}
        ce = entries["bench-68000-ce"]
        self.assertEqual(ce["options"]["cpu_speed"], "real")
        self.assertEqual(ce["options"]["cpu_cycle_exact"], "true")
        self.assertFalse([i for i in entries if i.startswith("bench-68000-ce-")])
        self.assertEqual(entries["bench-68000"]["options"]["cpu_cycle_exact"], "false")


if __name__ == "__main__":
    unittest.main()
