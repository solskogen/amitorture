"""Pacing per module: the policy (pacing.py) and the pacing request handler."""
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from amitorture import coverage, hostreq, matrix, pacing, runner  # noqa: E402


class WarpIPC:
    def __init__(self):
        self.warp, self.lines = 1, []

    def request(self, line, timeout=15):
        self.lines.append(line)
        if line == "GET_WARP":
            return f"OK\t{self.warp}"
        if line.startswith("SET_WARP"):
            self.warp = int(line.split("\t")[1])
            return "OK"
        return "OK"

    def write_long(self, addr, value):
        self.lines.append(f"WRITE_MEM {addr:x} {value}")
        return True

    def read_longs(self, addrs):
        return [0 for _ in addrs]


def req(seq, rid, tag):
    return {"t": "request", "seq": seq, "id": rid, "what": "pacing", "tag": tag}


class Policy(unittest.TestCase):
    def test_every_matrix(self):
        for name in matrix.MATRICES:
            es = pacing.assign(matrix.expand(name))
            by_id = {e["id"]: e for e in es}
            fam = pacing.families(es)
            for e in es:
                w = set(e["module_warp"])
                self.assertLessEqual(w, pacing.WARP_SAFE, e["id"])
                self.assertTrue(w <= set(coverage.AXES), e["id"])
                if pacing._threaded(e):
                    self.assertFalse(w, f"{name}: {e['id']} is threaded")
                # twins share the policy, so parity compares like with like
                ref = by_id.get(e.get("twin_of"))
                if ref is not None:
                    self.assertEqual(ref["module_warp"], e["module_warp"], f"{name}: {e['id']}")
                    self.assertEqual(fam[e["id"]], fam[ref["id"]])

    def test_slow_machines_warp_the_safe_modules(self):
        es = pacing.assign(matrix.expand("smoke"))
        a500 = next(e for e in es if e["id"] == "a500-68000-ce")
        self.assertEqual(set(a500["module_warp"]), pacing.WARP_SAFE)
        self.assertNotIn("misc_hw", a500["module_warp"])
        fast = next(e for e in es if e["id"] == "a1200-68ec020-fast")
        self.assertEqual(fast["module_warp"], [])      # its family has a threaded twin
        nothr = next(e for e in es if e["id"] == "a4000-68040nommu-fast")
        self.assertEqual(nothr["module_warp"], ["display"])

    def test_disk_modules(self):
        class O:
            module_warp, selftest, warp = "auto", False, None
        e = {"module_warp": ["cpu_basic", "display", "flags"]}
        self.assertEqual(runner.module_warp(O, e, "1-core", "x.adf", {}), ["display", "flags"])
        self.assertEqual(runner.module_warp(O, e, "2-cpu", "x.adf", {}), ["cpu_basic"])
        mask = matrix.module_mask(["flags"])
        self.assertEqual(runner.module_warp(O, e, "1-core", "x.adf", {"modules": mask}), ["flags"])
        self.assertEqual(runner.module_warp(O, e, "1-core", "x.adf", {}, capture=True), ["display", "flags"])
        self.assertEqual(runner.module_warp(O, e, "1-core", "x.adf", {"flags": "4"}), [])   # nombox
        O.module_warp = "off"
        self.assertEqual(runner.module_warp(O, e, "1-core", "x.adf", {}), [])


class Handler(unittest.TestCase):
    MBOX = 0x10000

    def setUp(self):
        self.d = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, self.d)

    def test_each_module_gets_its_pacing_before_the_ack(self):
        ipc = WarpIPC()
        rq = hostreq.Requests(ipc, self.d, pacing=0, module_warp=["display"], sleep=lambda s: None)
        rq.offer(req(1, 1, "boot"), "serial")
        h = rq.service(self.MBOX)[0]
        self.assertTrue(h["acked"])
        self.assertEqual(ipc.warp, 0)                  # the boot's warp ends
        for seq, (mod, want) in enumerate((("display", 1), ("copper", 0), ("display", 1)), start=2):
            rq.offer(req(seq, seq, mod), "serial")
            h = rq.service(self.MBOX)[0]
            self.assertTrue(h["acked"], mod)
            self.assertEqual((h["result"]["module"], h["result"]["warp"]), (mod, want))
            self.assertEqual(ipc.warp, want)
            set_at = max((i for i, ln in enumerate(ipc.lines) if ln.startswith("SET_WARP")), default=-1)
            ack_at = max(i for i, ln in enumerate(ipc.lines) if ln.startswith("WRITE_MEM"))
            self.assertLess(set_at, ack_at)
        self.assertEqual(rq.module_pacing, {"display": 1, "copper": 0})

    def test_without_a_policy_modules_keep_the_disk_pacing(self):
        ipc = WarpIPC()
        rq = hostreq.Requests(ipc, self.d, pacing=1, sleep=lambda s: None)
        rq.offer(req(1, 1, "flags"), "serial")
        h = rq.service(self.MBOX)[0]
        self.assertEqual(h["result"]["warp"], 1)
        self.assertNotIn("SET_WARP\t0", ipc.lines)


if __name__ == "__main__":
    unittest.main()
