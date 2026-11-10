"""Axis validation (coverage.validate / axis_drops) on synthetic results."""
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from amitorture import coverage  # noqa: E402


def entry(eid, **opts):
    o = {"cpu_model": "68020", "chipset": "aga", "cachesize": "0"}
    o.update(opts)
    return {"id": eid, "options": o, "model": "A1200"}


class Validate(unittest.TestCase):
    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.d)

    def write(self, eid, cases):
        p = os.path.join(self.d, "raw", eid, "2-cpu")
        os.makedirs(p)
        recs = [{"t": "mod", "mod": "cpu_basic", "id": 2, "ev": "begin"}]
        recs += [{"t": "case", "mod": "cpu_basic", "case": c, "st": st, "got": g} for c, (st, g) in cases.items()]
        with open(os.path.join(p, "records.jsonl"), "w") as fh:
            for i, r in enumerate(recs):
                fh.write(json.dumps({"boot": 0, "seq": i, "rec": dict(r, seq=i)}) + "\n")

    def test_same_key_must_agree(self):
        es = [entry("a"), entry("b", cachesize="0", ntsc="true"), entry("c", cpu_model="68030")]
        self.write("a", {"add": ("pass", 1), "sub": ("pass", 2)})
        self.write("b", {"add": ("pass", 1), "sub": ("fail", 3)})     # same cpu_basic key as a (ntsc: no axis)
        self.write("c", {"add": ("pass", 1), "sub": ("fail", 3)})
        v = coverage.validate([self.d], entries=es)
        self.assertEqual(v["cpu_basic"]["keys"], 1)
        (d,) = v["cpu_basic"]["differ"]
        self.assertEqual(d["entries"], ["a", "b"])
        self.assertEqual(d["cases"], ["sub"])
        self.assertFalse(d["detail_only"])
        self.assertEqual(d["verdict_cases"], ["sub"])
        self.assertEqual(d["options"], ["ntsc"])
        drops = coverage.axis_drops([self.d], modules=["cpu_basic"], entries=es)
        self.assertEqual(drops["cpu_basic"]["cpu"]["keys"], 1)      # a, b and c merge
        self.assertEqual(drops["cpu_basic"]["cpu"]["differ"], 2)

    def test_detail_only(self):
        es = [entry("a"), entry("b", ntsc="true")]
        self.write("a", {"add": ("pass", 1)})
        self.write("b", {"add": ("pass", 2)})
        (d,) = coverage.validate([self.d], entries=es)["cpu_basic"]["differ"]
        self.assertTrue(d["detail_only"])
        self.assertEqual(d["verdict_cases"], [])

    def test_agreeing_results(self):
        es = [entry("a"), entry("b", ntsc="true")]
        self.write("a", {"add": ("pass", 1)})
        self.write("b", {"add": ("pass", 1)})
        v = coverage.validate([self.d], entries=es)
        self.assertEqual((v["cpu_basic"]["keys"], v["cpu_basic"]["differ"]), (1, []))


class Outcomes(unittest.TestCase):
    def test_verdicts_and_missing_cases(self):
        dirs = []
        for cases in ({"add": ("pass", 1), "sub": ("pass", 2)}, {"add": ("fail", 1), "mul": ("pass", 2)}):
            v = Validate("test_agreeing_results")
            v.setUp()
            self.addCleanup(shutil.rmtree, v.d)
            v.write("a", cases)
            dirs.append(v.d)
        d = coverage.outcome_diff(*dirs)
        self.assertEqual(d["compared"], {"entries": 1, "modules": 1, "cases": 1})
        self.assertEqual(d["verdicts"], [{"entry": "a", "mod": "cpu_basic", "case": "add", "a": "pass", "b": "fail"}])
        self.assertEqual({(m["case"], m["only"]) for m in d["missing"]}, {("mul", "b"), ("sub", "a")})


if __name__ == "__main__":
    unittest.main()
