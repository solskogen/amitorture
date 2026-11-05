"""Engine comparisons must distinguish sampling coverage from correctness."""
import copy
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from amitorture import cases, parity

PAULA_INCONCLUSIVE = "bounded beam sampling could not obtain a coherent audio timestamp"
RTC_BUSY_INCONCLUSIVE = (
    "bounded polling did not witness the sub-millisecond RTC carry pulse; "
    "host-paced execution can miss the entire pulse"
)
HSCAN_INCONCLUSIVE = (
    "CPU polling did not cover both ends of the VHPOSR H range; access timing can "
    "skip these positions even over whole frames, so the line-end check is inconclusive"
)


def record(mod, name, st="pass", **kw):
    return {"t": "case", "mod": mod, "case": name, "st": st, **kw}


class ObservationParity(unittest.TestCase):
    def compare(self, a, b):
        ref = {"id": "ref", "status": "pass", "options": {"cachesize": "0"}}
        twin = {"id": "jit", "status": "pass", "options": {"cachesize": "8192"}, "twin_kind": "jit"}
        self.results = [ref, twin]
        self.fps = {"ref": cases.extract_records(a), "jit": cases.extract_records(b)}
        items, self.info = parity.compare(ref, twin, self.fps["ref"], self.fps["jit"])
        return items

    def test_bounded_paula_sampling_is_reported_as_a_parity_mismatch(self):
        untested = record("paula", "aud0_dma_start", "untestable", why=PAULA_INCONCLUSIVE)
        passed = record("paula", "aud0_dma_start", **{"in": {"unit": "cck"}})
        self.assertEqual(len(self.compare([untested], [passed])), 1)
        self.assertEqual(self.info["compared"], 1)
        self.assertEqual(self.info["not_comparable"], [])
        self.assertEqual(self.info["mismatches"], 1)
        self.assertEqual(self.fps["ref"]["cases"]["paula.aud0_dma_start"]["st"], "untestable")

    def test_fail_missing_wrong_reason_other_case_and_extra_fields_still_detected(self):
        u = record("paula", "aud0_dma_start", "untestable", why=PAULA_INCONCLUSIVE)
        for rhs in (record("paula", "aud0_dma_start", "fail", got={"irq": 0}),
                    dict(u, why="no DMA interrupts"),
                    record("paula", "aud0_dma_start", got={"architectural_register": "bad"}),
                    dict(u, architectural_register="bad")):
            with self.subTest(rhs=rhs):
                self.assertEqual(len(self.compare([u], [rhs])), 1)
        self.assertEqual(self.compare([u], [])[0]["subkind"], "missing_case")
        other = dict(u, case="adkcon_rw")
        self.assertEqual(len(self.compare([other], [dict(other, st="pass")])), 1)
        cpu = dict(u, mod="cpu")
        self.assertEqual(len(self.compare([cpu], [dict(cpu, st="pass")])), 1)

    def test_deterministic_bounds_are_compared(self):
        a = record("paula", "aud0_dma_start", **{"in": {"bounds": [{"lo": 10, "hi": 20}]}})
        b = copy.deepcopy(a)
        b["in"]["bounds"][0]["hi"] = 30
        self.assertEqual(self.compare([a], [b])[0]["paths"][0]["path"], "in.bounds[0].hi")

    def test_rtc_busy_requires_a_witness_without_turning_absence_into_a_pass(self):
        u = record("misc_hw", "rtc_busy", "untestable", why=RTC_BUSY_INCONCLUSIVE,
                   expect={"busy_samples": ">0 in 1.2 s"}, got={"busy_samples": 0})
        p = record("misc_hw", "rtc_busy")
        self.assertEqual(len(self.compare([u], [p])), 1)
        self.assertEqual(self.info["compared"], 1)
        self.assertFalse(self.info["not_comparable"])
        for bad in (dict(u, st="fail"), dict(u, got={"busy_samples": 1}),
                    dict(u, why="BUSY stuck low"), dict(u, case="rtc_counting"),
                    dict(u, architectural_register="bad")):
            with self.subTest(bad=bad):
                self.assertEqual(len(self.compare([bad], [dict(p, case=bad["case"])])), 1)

    def test_hscan_sampling_gap_and_out_of_range_are_both_parity_mismatches(self):
        u = record("timing", "cck_per_line", "untestable", why=HSCAN_INCONCLUSIVE,
                   expect={"hmax": "e2", "hmin": "00"}, got={"hmax": "e2", "hmin": "01"},
                   ctx={"pal": 1, "frames": 8})
        p = record("timing", "cck_per_line", timing={"samples": 8000, "lines": 400})
        self.assertEqual(len(self.compare([u], [p])), 1)
        self.assertEqual(self.info["compared"], 1)
        f = dict(u, st="fail", why="H exceeds architectural limit", got={"hmax": "ff", "hmin": "00"})
        self.assertEqual(len(self.compare([f], [p])), 1)
        self.assertEqual(len(self.compare([u], [dict(u, ctx={"pal": 0, "frames": 8})])), 1)
        self.assertEqual(len(self.compare([dict(u, got={"hmax": "ff", "hmin": "00"})], [p])), 1)
        self.assertEqual(len(self.compare([dict(u, expect={"hmax": "e3", "hmin": "00"})], [p])), 1)

    def test_timing_values_excluded_but_architectural_values_compared(self):
        a = record("timing", "other", timing={"irqs": 5}, got={"sr": "2000"})
        b = record("timing", "other", timing={"irqs": 9}, got={"sr": "2000"})
        self.assertEqual(self.compare([a], [b]), [])
        b["got"]["sr"] = "2700"
        self.assertEqual(len(self.compare([a], [b])), 1)
        self.assertEqual(len(self.compare([record("cpu", "other", got=1)],
                                          [record("cpu", "other", got=1.0)])), 1)

    @staticmethod
    def stopped(second, advanced=1):
        return record("misc_hw", "rtc_stop", "fail",
                      expect={"time": f"y26-11-05 13:45:{second:02d} w4"},
                      got={"time": f"y26-11-05 13:45:{second + advanced:02d} w4",
                           "digits": f"{str(second + advanced)[::-1]}54315011624"},
                      why="the counters advanced with STOP=1", **{"in": {"chip": "MSM6242B", "mode24": 1}})

    def test_wall_clock_stop_normalized_but_relationships_and_registers_are_not(self):
        a, b = self.stopped(21), self.stopped(24)
        original = copy.deepcopy(a)
        self.assertEqual(self.compare([a], [b]), [])
        self.assertEqual(len(self.info["observations"]), 1)
        self.assertEqual(a, original)
        for bad in (dict(b, st="pass"), self.stopped(24, advanced=0),
                    dict(b, **{"in": {"chip": "MSM6242B", "mode24": 0}})):
            self.assertEqual(len(self.compare([a], [bad])), 1)
        bad = copy.deepcopy(b)
        bad["got"]["time"] = "not a calendar date"
        self.assertEqual(len(self.compare([a], [bad])), 1)
        bad = copy.deepcopy(b)
        bad["got"]["digits"] = "fffffffffffff"
        self.assertEqual(len(self.compare([a], [bad])), 1)
        bad = copy.deepcopy(b)
        bad["got"]["time"] = "y27-11-05 13:45:25 w4"
        self.assertEqual(len(self.compare([a], [bad])), 1)
        # Two different inconsistent-but-valid digit values must not collapse.
        bad_a, bad_b = copy.deepcopy(a), copy.deepcopy(b)
        bad_a["got"]["digits"] = "5254315011624"
        bad_b["got"]["digits"] = "6254315011624"
        self.assertEqual(len(self.compare([bad_a], [bad_b])), 1)

    def test_hold_compares_assertions_not_calendar_date(self):
        a = record("misc_hw", "rtc_hold", "fail",
                   expect={"held_later": "3254315011624", "after_release": "y26-11-05 13:45:24 w4",
                           "next_edge_ms": "1850-2150"},
                   got={"held_later": "4254315011624", "after_release": "4254315011624"},
                   why="registers changed while HOLD=1")
        b = copy.deepcopy(a)
        b["expect"].update(held_later="1254315011624", after_release="y26-11-05 13:45:22 w4")
        b["got"].update(held_later="2254315011624", after_release="2254315011624")
        self.assertEqual(self.compare([a], [b]), [])
        b["got"]["held_later"] = b["expect"]["held_later"]
        self.assertEqual(len(self.compare([a], [b])), 1)

    def test_write_target_and_non_host_readback_remain_exact(self):
        def write(second):
            return record("misc_hw", "rtc_write_readback", "fail",
                          expect={"time": "y95-07-14 13:37:42 w5 leap3"},
                          got={"time": f"y26-11-05 13:45:{second:02d} w4 leap3",
                               "digits": f"{str(second)[::-1]}54315011624"}, why="time write ignored")
        a, b = write(22), write(25)
        self.assertEqual(self.compare([self.stopped(21), a], [self.stopped(24), b]), [])
        for replacement in ("y95-07-14 13:37:41 w5 leap3", "y26-11-05 13:45:25 w4 leap2"):
            bad = copy.deepcopy(b)
            bad["got"]["time"] = replacement
            self.assertTrue(self.compare([self.stopped(21), a], [self.stopped(24), bad]))
        bad = copy.deepcopy(b)
        bad["expect"]["time"] = "y95-07-14 13:37:41 w5 leap3"
        self.assertTrue(self.compare([self.stopped(21), a], [self.stopped(24), bad]))
        # No anchor: no evidence the readback is host time, so preserve it exactly.
        self.assertTrue(self.compare([a], [b]))


if __name__ == "__main__":
    unittest.main()
