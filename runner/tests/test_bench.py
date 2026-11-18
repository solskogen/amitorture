"""The bench disk's host side (bench.py): serial stamps, the guest's
requests, results, diffs, the matrix and the runner's bench policy."""
import json
import os
import re
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

from amitorture import bench, compare, hostreq, mailbox, matrix, records, runner
from tests.test_m3 import FakeIPC, req

ROOT = Path(__file__).resolve().parents[2]
WORK_TMP = ROOT / "work" / "tmp"
TICK_HZ = 709379


def line(seq, **o):
    js = json.dumps({"v": 1, "seq": seq, "t": "bench", **o}, separators=(",", ":")).encode()
    return js + b" *" + records.crc32_hex(js).encode() + b"\n"


class Launch:
    """A serial stream fed through a Stamper (each chunk with its arrival
    time) and the parser, the records into a Host, as EntryRun does."""

    def __init__(self, host=True):
        self.cpu = [0, 0, 0]
        self.stamper = bench.Stamper(cpu=lambda: tuple(self.cpu)) if host else None
        self.parser = records.StreamParser("serial")
        self.taken = 0
        self.h = bench.Host(self.stamper, pull=self.pull)
        self.seq = 0
        self.t = 0.0
        self.send(ev="env", tick_hz=TICK_HZ, res_ticks=1, min_ticks=70938, host=int(host), plan=-1)

    def pull(self):
        for r in self.parser.records[self.taken:]:
            if r.crc_ok:
                self.h.ingest(r)
        self.taken = len(self.parser.records)

    def send(self, t=None, **o):
        if t is not None:
            self.t = t
        data = line(self.seq, **o)
        self.seq += 1
        if self.stamper is not None:
            self.stamper.feed(data, self.t)
        self.parser.feed(data)
        self.pull()

    def begin(self, blk, bid, unit="insn", per=1, tier="micro", **kw):
        self.bid, self.blk = bid, blk
        self.rs = 0
        self.send(ev="begin", blk=blk, tier=tier, id=bid, unit=unit, per=per, **kw)

    def run(self, n, host_s, ticks, ph="rep", cpu_total=None, rs=None):
        """go at self.t, the run record host_s later; cpu_total: host CPU
        ns of the emulator during the run."""
        self.rs += 1
        self.send(ev="go", id=self.bid, rs=self.rs)
        if cpu_total is not None:
            self.cpu = [self.cpu[0] + cpu_total // 2, self.cpu[1] + cpu_total, self.cpu[2] + cpu_total // 2]
        self.send(t=self.t + host_s, ev="run", blk=self.blk, ph=ph, n=n, rs=self.rs if rs is None else rs,
                  timing={"t": ticks})
        self.t += 0.01

    def end(self, st="ok", n=None, reps=None, **kw):
        self.send(ev="end", blk=self.blk, st=st, n=n, reps=reps, **kw)


class Stamps(unittest.TestCase):
    def test_host_time_is_go_newline_to_the_next_bytes(self):
        L = Launch()
        L.begin("add_l", 3, per=8)
        L.run(1, 0.001, 50, ph="cold")
        L.run(1024, 0.2500, 70000)
        L.run(1024, 0.2502, 70010)
        L.end(n=1024, reps=2)
        b = L.h.result()["blocks"][0]
        self.assertEqual(b["basis"], "host")
        self.assertEqual(b["host_s"], [0.25, 0.2502])
        self.assertAlmostEqual(b["value"], 0.25e9 / (1024 * 8), places=3)
        self.assertTrue(b["confirmed"])
        self.assertAlmostEqual(b["cold_guest_ns"], 50 / TICK_HZ * 1e9, places=0)

    def test_a_go_inside_a_chunk_is_not_a_stamp(self):
        st = bench.Stamper()
        st.feed(line(0, ev="go", id=1, rs=1) + b"{\"v\":1", 1.0)
        self.assertEqual(st.stamps, {})

    def test_a_run_with_another_sequence_falls_back_to_guest_time(self):
        L = Launch()
        L.begin("add_l", 3)
        L.run(1024, 0.25, 70000, rs=9)
        L.run(1024, 0.25, 70000)
        L.end(n=1024, reps=2)
        b = L.h.result()["blocks"][0]
        self.assertEqual(b["basis"], "guest")
        self.assertAlmostEqual(b["value"], 70000 / TICK_HZ * 1e9 / 1024, places=2)

    def test_cost_blocks_are_host_cpu_per_emulated_second(self):
        L = Launch()
        L.begin("stop", 200, unit="frame", tier="frame")
        L.run(50, 1.0, TICK_HZ, cpu_total=300_000_000)
        L.run(50, 1.0, TICK_HZ, cpu_total=301_000_000)
        L.end(n=50, reps=2)
        b = L.h.result()["blocks"][0]
        self.assertEqual(b["metric"], "cpu_per_emu_s")
        self.assertAlmostEqual(b["value"], 0.3)
        self.assertAlmostEqual(b["rtf"], 1.0)
        self.assertAlmostEqual(b["cpu_main_per_emu_s"], 0.15)
        self.assertTrue(b["confirmed"])        # within 1%

    def test_rebuilt_from_the_saved_capture(self):
        L = Launch()
        L.begin("add_l", 3)
        L.run(1024, 0.25, 70000)
        L.run(1024, 0.25, 70000)
        L.end(n=1024, reps=2)
        data = b"".join(r.raw + b"\n" for r in L.parser.records)
        again = bench.from_serial(data, L.stamper.export())
        self.assertEqual(again["blocks"], L.h.result()["blocks"])


class Decide(unittest.TestCase):
    def test_longer_until_the_host_time_is_long_enough(self):
        L = Launch()
        L.begin("add_l", 3)
        L.run(512, 0.15, 70000, ph="size")
        self.assertTrue(L.h.decide("bench_longer", {"id": 3, "n": 512, "reps": 0})[0])
        L.run(1024, 0.30, 140000, ph="size")
        self.assertFalse(L.h.decide("bench_longer", {"id": 3, "n": 1024, "reps": 0})[0])

    def test_cost_blocks_need_longer(self):
        L = Launch()
        L.begin("stop", 200, unit="frame", tier="frame")
        L.run(16, 0.3, 70000, ph="size", cpu_total=1)
        self.assertTrue(L.h.decide("bench_longer", {"id": 200, "n": 16, "reps": 0})[0])

    def test_more_until_the_two_fastest_agree(self):
        L = Launch()
        L.begin("add_l", 3)
        L.run(1024, 0.25, 70000, ph="size")
        L.run(1024, 0.26, 70000)
        self.assertTrue(L.h.decide("bench_more", {"id": 3, "n": 1024, "reps": 2})[0])
        L.run(1024, 0.2503, 70000)
        self.assertFalse(L.h.decide("bench_more", {"id": 3, "n": 1024, "reps": 3})[0])
        self.assertFalse(L.h.decide("bench_more", {"id": 3, "n": 1024, "reps": bench.MAX_REPS})[0])
        self.assertEqual([d["ack"] for d in L.h.decisions], [True, False, False])

    def test_without_a_host_clock_the_guest_rule(self):
        L = Launch(host=False)
        L.begin("add_l", 3)
        L.run(512, 0.1, 70000, ph="size")
        self.assertTrue(L.h.decide("bench_longer", {"id": 3, "n": 512, "reps": 0})[0])
        L.run(1024, 0.2, 80000, ph="size")
        self.assertFalse(L.h.decide("bench_longer", {"id": 3, "n": 1024, "reps": 0})[0])

    def test_no_run_to_judge_is_an_error(self):
        clock = [0.0]
        h = bench.Host(bench.Stamper(), clock=lambda: clock[0],
                       sleep=lambda s: clock.__setitem__(0, clock[0] + s))
        ack, info = h.decide("bench_more", {"id": 3, "n": 4, "reps": 2})
        self.assertFalse(ack)
        self.assertIn("error", info)

    def test_quiet_while_a_run_is_open(self):
        L = Launch()
        L.begin("add_l", 3)
        self.assertIsNone(L.h.quiet())
        L.send(ev="go", id=3, rs=1)
        self.assertIsNotNone(L.h.quiet())
        L.begin("flood_recovery", 212, unit="run", tier="thread")
        L.send(ev="go", id=212, rs=1)
        self.assertIsNone(L.h.quiet())       # it needs mailbox samples


class Requests(unittest.TestCase):
    MBOX = 0x5070

    def setUp(self):
        os.makedirs(WORK_TMP, exist_ok=True)
        self.d = tempfile.mkdtemp(dir=WORK_TMP)

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def test_a_no_is_answered_not_failed(self):
        L = Launch()
        L.begin("add_l", 3)
        L.run(1024, 0.30, 140000, ph="size")
        ipc = FakeIPC()
        rq = hostreq.Requests(ipc, self.d, bench=L.h)
        rq.offer(req(10, 7, "bench_longer", "add_l", {"id": 3, "n": 1024, "reps": 0}), "serial")
        h = rq.service(self.MBOX)[0]
        self.assertEqual(ipc.lines[-1], f"WRITE_MEM\t{self.MBOX + 44:#x}\t4\t{7 | 0x80000000:#x}")
        self.assertTrue(h["acked"])
        self.assertTrue(h["result"]["declined"])

    def test_without_a_bench_host_refused(self):
        ipc = FakeIPC()
        rq = hostreq.Requests(ipc, self.d)
        rq.offer(req(10, 7, "bench_more", "add_l", {"id": 3, "n": 4, "reps": 2}), "serial")
        h = rq.service(self.MBOX)[0]
        self.assertFalse(h["acked"])
        self.assertTrue(h["result"]["refused"])


def entry(eid, blocks, **kw):
    return dict({"entry": eid, "disk": None, "pacing": {"effective": False}, "blocks": blocks}, **kw)


def blk(name, value, bid=1, metric="ns_per_unit", confirmed=True, st="ok", n=1024, **kw):
    return dict({"blk": name, "id": bid, "metric": metric, "unit": "insn", "basis": "host", "value": value,
                 "confirmed": confirmed, "st": st, "n": n}, **kw)


class Diff(unittest.TestCase):
    def verdicts(self, a, b):
        return {r["blk"]: r["verdict"] for r in bench.diff([entry("e", a)], [entry("e", b)])}

    def test_tolerances(self):
        v = self.verdicts([blk("a", 100, 1), blk("b", 100, 2), blk("c", 100, 3), blk("d", 100, 4),
                           blk("e", 100, 5, confirmed=False)],
                          [blk("a", 100.4, 1), blk("b", 100.8, 2), blk("c", 102, 3), blk("d", 97, 4),
                           blk("e", 102, 5)])
        self.assertEqual(v, {"a": "same", "b": "borderline", "c": "slower", "d": "faster", "e": "noisy"})

    def test_cost_blocks_have_wider_tolerances(self):
        m = "cpu_per_emu_s"
        v = self.verdicts([blk("s", 0.3, 1, metric=m), blk("t", 0.3, 2, metric=m)],
                          [blk("s", 0.305, 1, metric=m), blk("t", 0.33, 2, metric=m)])
        self.assertEqual(v, {"s": "same", "t": "slower"})

    def test_what_is_not_compared(self):
        v = self.verdicts([blk("k", 100, 1, kset="k020"), blk("f", 100, 2), blk("gone", 100, 3),
                           blk("flood_recovery", 1, 4, metric="cpu_per_emu_s")],
                          [blk("k", 100, 1, kset="k020f"), blk("f", 100, 2, st="fail"), blk("new", 1, 5),
                           blk("flood_recovery", 2, 4, metric="cpu_per_emu_s")])
        self.assertEqual(v, {"k": "incomparable", "f": "fail", "gone": "incomplete", "new": "new",
                             "flood_recovery": "info"})

    def test_other_pacing_is_incomparable(self):
        rows = bench.diff([entry("e", [blk("a", 100)])],
                          [entry("e", [blk("a", 200)], pacing={"effective": True})])
        self.assertEqual(rows[0]["verdict"], "incomparable")

    def test_exit_status(self):
        self.assertEqual(bench.exit_status([{"verdict": "same"}, {"verdict": "faster"}]), (0, "OK"))
        self.assertEqual(bench.exit_status([{"verdict": "same"}, {"verdict": "noisy"}])[0], 1)
        self.assertEqual(bench.exit_status([{"verdict": "new"}]), (1, "NOTHING COMPARED"))
        self.assertEqual(bench.exit_status([{"verdict": "slower"}, {"verdict": "fail"}]), (1, "SLOWER"))


class Plans(unittest.TestCase):
    def test_the_baseline_counts_for_the_next_run(self):
        base = [entry("bench-68020", [blk("a", 1, 3, n=4096), blk("b", 1, 4, st="fail", n=8),
                                      {"blk": "c", "id": 5, "st": "skip"}])]
        ents = [{"id": "bench-68020"}, {"id": "bench-68030"}]
        os.makedirs(WORK_TMP, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=WORK_TMP) as d:
            bench.freeze_plans(ents, base, os.path.join(d, "plans"))
            with open(ents[0]["bench_plan"]) as fh:
                self.assertEqual(json.load(fh), {"entries": [{"id": 3, "count": 4096}]})
            self.assertNotIn("bench_plan", ents[1])

    def test_write_diff(self):
        os.makedirs(WORK_TMP, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=WORK_TMP) as d:
            got = bench.write_diff(d, [entry("e", [blk("a", 100)])], [entry("e", [blk("a", 110)])], "ref")
            self.assertEqual((got["status"], got["word"]), (1, "SLOWER"))
            self.assertIn("**slower**", Path(d, "BENCH-DIFF.md").read_text())


class Compare(unittest.TestCase):
    def test_collect_takes_timed_blocks_lower_is_better(self):
        summ = {"bench": [entry("e", [blk("a", 5.0), blk("stop", 0.3, 2, metric="cpu_per_emu_s"),
                                      blk("u", 7.0, 3, st="unconfirmed"), blk("f", 1, 4, st="fail"),
                                      blk("flood_recovery", 1, 5, metric="cpu_per_emu_s")])]}
        self.assertEqual(compare.collect(summ), {("e", None, "a"): (5.0, False, "host ns/insn"),
                                                 ("e", None, "stop"): (0.3, False, "host CPU/emu s"),
                                                 ("e", None, "u"): (7.0, False, "host ns/insn")})


class Matrix(unittest.TestCase):
    def test_every_cpu_family_threaded_twins_on_the_68020(self):
        ents = {e["id"]: e for e in matrix.expand("bench")}
        self.assertEqual({e["options"]["cpu_model"] for e in ents.values()},
                         {"68000", "68010", "68020", "68030", "68040", "68060"})
        for e in ents.values():
            self.assertEqual(e["params"], {"flags": "host"})
            self.assertTrue(matrix.is_bench(e))
            if e["options"].get("cpu_threaded") == "true":
                self.assertEqual(e["options"]["cpu_model"], "68020")
        self.assertIn("bench-68000-ce", ents)
        self.assertEqual(ents["bench-68020-thr-1cpu"]["host_cpus"], 1)

    def test_bench_entries_skip_the_coverage_plan(self):
        opts = SimpleNamespace(matrix="bench", configs=[], sets={}, plan=True, params={})
        ents = runner.select_entries(opts)
        self.assertEqual(len(ents), len(matrix.expand("bench")))
        self.assertTrue(all(e["module_warp"] == [] for e in ents))

    def test_bench_matrix_uses_the_bench_disk(self):
        opts = SimpleNamespace(matrix="bench", adf=None, disk_set=None)
        self.assertIsNone(runner.resolve_disk_set(opts))

    def test_bench_target_contains_only_bench_code(self):
        make = shutil.which("make")
        if not make:
            self.skipTest("GNU make unavailable")
        os.makedirs(WORK_TMP, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=WORK_TMP) as tmp:
            out = subprocess.run([make, "-C", str(ROOT / "disk"), "--dry-run", f"B={tmp}", "bench"],
                                 check=True, capture_output=True, text=True).stdout
            sources = set(re.findall(r" -c -o \S+ ((?:modules|bench)/[^/\s]+\.c)", out))
            self.assertEqual(sources, {"bench/harness.c", "bench/micro.c", "bench/kern.c", "bench/system.c"})
            self.assertIn(f"{tmp}/amitorture-bench.adf", out)


class Pacing(unittest.TestCase):
    INFO = {"build_id": "amitorture 1/1 bench"}

    def warp(self, **opt):
        e = {"options": dict({"cpu_speed": "real"}, **opt)}
        return runner.disk_warp(SimpleNamespace(warp=None, selftest=False), e, self.INFO)["effective"]

    def test_warp_only_where_the_cpu_is_paced_to_real_time(self):
        self.assertEqual(runner.build_group(self.INFO), "bench")
        self.assertTrue(self.warp())
        self.assertFalse(self.warp(cpu_speed="max", cachesize="8192"))
        self.assertFalse(self.warp(cpu_threaded="true"))
        self.assertFalse(self.warp(warp="false"))        # explicit option wins


class Sampling(unittest.TestCase):
    def test_samples_only_inside_timed_runs(self):
        h = bench.Host(bench.Stamper())

        def hdr(case, hb):
            return mailbox.Header(0, [1, 1, hb, 0, 4096, 42, case])
        for case, hb in ((0, 1), (bench.POS_RUN | 212 << 8 | 3, 2), (bench.POS_QUIET, 3), (1003, 4)):
            h.add_sample(hb / 10, hdr(case, hb), (1, 2, 3))
        self.assertEqual([s[1] for s in h.samples], [2, 3])

    def test_follower_reads_the_header_once_without_new_bytes(self):
        img = mailbox.build_image([], ring_size=64, state=1)

        class Mem:
            reads = 0

            def read_longs(self, addrs):
                Mem.reads += 1
                return [int.from_bytes(img[a:a + 4], "big") for a in addrs]
        seen = []
        f = mailbox.RingFollower(Mem(), 0, on_header=seen.append)
        self.assertEqual(f.poll()["data"], b"")
        self.assertEqual((Mem.reads, len(seen)), (1, 1))

    def test_thread_cpu_of_this_process(self):
        main, total, top = runner.thread_cpu(os.getpid())
        self.assertGreater(main, 0)
        self.assertGreaterEqual(total, main)


def thread_result(eid, threaded, blocks, engine=None, pin=None):
    return {"id": eid, "binary": "/bin/amiberry", "params": {"flags": "host"}, "jit": False, "machine": "A1200",
            "options": {"cpu_threaded": "true" if threaded else "false"}, "engine": engine or {},
            "host_cpus": pin, "status": "pass",
            "bench": {"basis": "host", "pacing": None, "env": {}, "blocks": blocks}}


class ThreadItems(unittest.TestCase):
    def cost(self, name, value, rtf, bid):
        return blk(name, value, bid, metric="cpu_per_emu_s", unit="frame", rtf=rtf)

    def items(self, off_blocks, on_blocks, engine=None):
        eng = dict({"cpu_thread_runs_on": "CPU"}, **(engine or {}))
        res = [thread_result("bench-68020", False, off_blocks),
               thread_result("bench-68020-thr", True, on_blocks, engine=eng)]
        return [i["case"] for i in bench.thread_items(res)]

    def test_behind_real_time_where_the_twin_keeps_up(self):
        self.assertEqual(self.items([self.cost("chipset_load", 0.5, 1.0, 202)],
                                    [self.cost("chipset_load", 0.6, 1.2, 202)]), ["chipset_load"])
        self.assertEqual(self.items([self.cost("chipset_load", 0.5, 1.0, 202)],
                                    [self.cost("chipset_load", 0.6, 1.02, 202)]), [])

    def test_idle_cpu_thread_that_spins(self):
        self.assertEqual(self.items([self.cost("stop", 0.2, 1.0, 200)], [self.cost("stop", 1.0, 1.0, 200)]),
                         ["stop"])

    def test_cpu_held_on_the_main_thread_after_a_flood(self):
        fr = lambda q: blk("flood_recovery", None, 212, metric="cpu_per_emu_s", unit="run",
                           quiet_thread_share=q, flood_thread_share=0.1)
        self.assertEqual(self.items([fr(None)], [fr(0.2)], engine={"cpu_thread_flood_holds": 6}),
                         ["flood_recovery"])
        self.assertEqual(self.items([fr(None)], [fr(0.9)], engine={"cpu_thread_flood_holds": 6}), [])
        self.assertEqual(self.items([fr(None)], [fr(0.2)], engine={"cpu_thread_flood_holds": 0}), [])

    def test_refloods(self):
        self.assertEqual(self.items([], [], engine={"cpu_thread_refloods": 3}), ["thread_refloods"])

    def test_one_host_cpu(self):
        r = thread_result("bench-68020-thr-1cpu", True, [], engine={"cpu_thread_runs_on": "CPU"},
                          pin={"asked": 1, "pinned_to": [0]})
        self.assertEqual([i["case"] for i in bench.thread_items([r])], ["thread_one_cpu"])
        r["engine"]["cpu_thread_runs_on"] = "main"
        self.assertEqual(bench.thread_items([r]), [])


if __name__ == "__main__":
    unittest.main()
