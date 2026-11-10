import unittest

from amitorture import compare, matrix
from tests.test_hostspeed import block_recs, samples, sub_with
from amitorture import cases


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

    def test_metric_and_collect(self):
        summary = {"hostspeed": [{"entry": "bench-68020", "disk": None, "blocks": [
            {"insn": "add_l", "st": "measured", "ops_per_host_s": 2e8, "cpu_total_per_emu_s": 1.0},
            {"insn": "stop", "st": "measured", "ops_per_host_s": 50.0, "cpu_total_per_emu_s": 0.9},
            {"insn": "vbl_poll", "st": "untestable", "cpu_total_per_emu_s": 2.0},
            {"insn": "movep", "st": "skip"}]}]}
        got = compare.collect(summary)
        self.assertEqual(got, {("bench-68020", None, "add_l"): (2e8, True, "host ops/s"),
                               ("bench-68020", None, "stop"): (0.9, False, "total CPU/s")})

    def test_analyse_marks_missing_side_incomplete(self):
        runs = [{"label": "A", "values": {("e", None, "add_l"): (100, True, "host ops/s"),
                                          ("e", None, "fadd"): (5, True, "host ops/s")}},
                {"label": "B", "values": {("e", None, "add_l"): (80, True, "host ops/s")}}]
        rows = {r["insn"]: r for r in compare.analyse(runs, ["A", "B"], 2)}
        self.assertEqual(rows["add_l"]["verdict"], "slower")
        self.assertEqual(rows["fadd"]["verdict"], "incomplete")

    def test_parse_cpus(self):
        self.assertEqual(compare.parse_cpus("2-4,8"), {2, 3, 4, 8})
        with self.assertRaises(ValueError):
            compare.parse_cpus(",")


class FrameBlocks(unittest.TestCase):
    def test_cost_over_stop_instead_of_ns_per_op(self):
        bl = [block_recs("stop", 1000, 50, 3537240, units=50),
              block_recs("empty", 1001, 160000, 3537240, units=10000),
              block_recs("vbl_poll", 1002, 50, 3537240, units=50),
              block_recs("chipset_load", 1003, 50, 3537240, units=50)]
        smp = (samples(1000, 10.0, 1.0, k=6, cpu_per_s=0.2, f0=100)
               + samples(1001, 20.0, 1.0, k=6, cpu_per_s=1.0, f0=200)
               + samples(1002, 30.0, 1.0, k=6, cpu_per_s=0.9, f0=300)
               + samples(1003, 40.0, 1.0, k=6, cpu_per_s=0.3, f0=400))
        sub = sub_with(bl, smp)
        out = {b["insn"]: b for b in cases.speed_blocks(sub, sub["_merged"])}
        self.assertAlmostEqual(out["vbl_poll"]["over_idle_per_emu_s"], 0.7, places=2)
        self.assertAlmostEqual(out["chipset_load"]["over_idle_per_emu_s"], 0.1, places=2)
        for name in ("stop", "vbl_poll", "chipset_load"):
            self.assertNotIn("ns_per_op", out[name])
        self.assertNotIn("over_idle_per_emu_s", out["stop"])


class CycleExactBenchmark(unittest.TestCase):
    def test_ce_entry_without_twins(self):
        entries = {e["id"]: e for e in matrix.expand("benchmarks")}
        ce = entries["bench-68000-ce"]
        self.assertEqual(ce["options"]["cpu_speed"], "real")
        self.assertEqual(ce["options"]["cpu_cycle_exact"], "true")
        self.assertFalse([i for i in entries if i.startswith("bench-68000-ce-")])
        self.assertEqual(entries["bench-68000"]["options"]["cpu_cycle_exact"], "false")


if __name__ == "__main__":
    unittest.main()
