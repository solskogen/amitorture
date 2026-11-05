"""hostspeed: the host.speed_<insn> pseudo cases (cases.speed_blocks), the
sampler, the matrix entries at cpu_speed=real and parity."""
import os
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from amitorture import cases, mailbox, matrix, parity, records, report, runner  # noqa: E402

PAL_FPS = cases.FRAME_HZ["PAL"]
FIELDS = ["t", "heartbeat", "case", "cpu_main_ns", "cpu_total_ns", "cpu_top_ns"]


def merged(recs):
    return records.Merged([{"boot": 0, "seq": i, "rec": dict(r, v=1, seq=i), "sources": ["serial"]}
                           for i, r in enumerate(recs)], [], [], [], 0, [], {})


def hello(video="PAL"):
    return {"t": "hello", "build": "x", "mode": 0, "seed": 1, "exec": "40.10", "cpu": "68020", "fpu": "68882",
            "mmu": 0, "probe": {"video": video}}


def block_recs(insn, pos, n, cck, cls="int", units=100):
    b = {"t": "speed", "mod": "hostspeed", "insn": insn, "cls": cls, "pos": pos}
    return [dict(b, ev="begin"), dict(b, ev="end", n=n, timing={"cck": cck, "empty_cck": 1000, "frames": 50,
                                                                 "units": units})]


def samples(pos, t0, rtf, k=4, every=13, cpu_per_s=None, f0=100, jitter=()):
    """k samples, `every` frames apart, of a block running at `rtf`. cpu_per_s:
    host CPU s per emulated s; default rtf (the thread fully busy, as in a real
    slowdown)."""
    if cpu_per_s is None:
        cpu_per_s = rtf
    out = []
    spf = rtf / PAL_FPS
    for i in range(k):
        f = f0 + i * every
        t = t0 + i * every * spf + (jitter[i] if i < len(jitter) else 0.0)
        cpu = int((t - t0) * 1e9 * cpu_per_s / rtf) + 10 ** 9
        out.append([round(t, 6), f, pos, cpu, cpu * 2, 0])
    return out


def sub_with(blocks, smp, options=None, ctx=None):
    recs = [hello()] + [r for b in blocks for r in b]
    return {"hostspeed": {"fields": FIELDS, "samples": smp}, "options": options or {"cpu_speed": "real"},
            "host_context": ctx or {"cpu_model": "Cortex-A76", "cpus": 4, "jobs": 2, "loadavg_start": [1.5]},
            "_merged": merged(recs)}


class SpeedVerdict(unittest.TestCase):
    def blocks(self, rtfs, **kw):
        bl, smp = [], []
        t = 10.0
        for i, (name, rtf) in enumerate(rtfs):
            bl.append(block_recs(name, 1000 + i, 3465216, 3537240))
            smp += samples(1000 + i, t, rtf, f0=100 + 60 * i, **kw)
            t += 2.0 * rtf
        sub = sub_with(bl, smp)
        return {b["insn"]: b for b in cases.speed_blocks(sub, sub["_merged"])}, sub

    def test_rtf_from_frames_and_host_time(self):
        b, _ = self.blocks([("add_l", 1.0), ("fsin", 1.5)])
        self.assertAlmostEqual(b["add_l"]["rtf"], 1.0, places=4)
        self.assertAlmostEqual(b["fsin"]["rtf"], 1.5, places=4)
        self.assertAlmostEqual(b["fsin"]["fps"], PAL_FPS / 1.5, places=2)
        self.assertEqual(b["add_l"]["st"], "pass")
        self.assertEqual(b["fsin"]["st"], "fail")
        self.assertEqual(b["fsin"]["samples"], 4)

    def test_threshold(self):
        b, _ = self.blocks([("add_l", 1.0), ("a", 1.005), ("b", 1.02)])
        self.assertEqual(b["a"]["st"], "pass")             # within 1 % of real time
        self.assertEqual(b["b"]["st"], "fail")             # 2 % slower, noise ~0
        self.assertEqual(cases.SPEED_RTF_LIMIT, 1.01)

    def test_never_fail_on_noise(self):
        # 3 ms of jitter per sample on 0.75 s: 3 sigma > 0.5 %, rtf 1.015 is not judged
        b, _ = self.blocks([("x", 1.015)], jitter=(0.0, 0.003, -0.003, 0.003))
        self.assertEqual(b["x"]["st"], "untestable")
        self.assertIn("noise", b["x"]["why"])
        # ... but far above the limit it fails, and far below it passes
        b, _ = self.blocks([("y", 1.3), ("z", 0.95)], jitter=(0.0, 0.003, -0.003, 0.003))
        self.assertEqual(b["y"]["st"], "fail")
        self.assertEqual(b["z"]["st"], "pass")

    def test_lag_while_idle_is_not_a_fail(self):
        # 5 % behind, but the emulation thread only 40 % busy: host scheduling
        b, _ = self.blocks([("a", 1.0), ("b", 1.05)], cpu_per_s=0.42)
        self.assertEqual(b["b"]["st"], "untestable")
        self.assertIn("busy", b["b"]["why"])
        b, _ = self.blocks([("a", 1.0), ("b", 1.05)])        # fully busy: a real slowdown
        self.assertEqual(b["b"]["st"], "fail")

    def test_one_late_sample_is_dropped(self):
        b, _ = self.blocks([("cas", 1.0)], k=5, jitter=(0, 0, 0, 0.013, 0))
        self.assertEqual(b["cas"]["st"], "pass")
        self.assertIn("dropped", b["cas"])
        self.assertAlmostEqual(b["cas"]["rtf"], 1.0, places=4)

    def test_untestable_without_samples(self):
        sub = sub_with([block_recs("fsin", 1000, 100, 3537240)], [])
        (b,) = cases.speed_blocks(sub, sub["_merged"])
        self.assertEqual(b["st"], "untestable")
        self.assertIn("no host samples", b["why"])
        sub = sub_with([block_recs("fsin", 1000, 100, 3537240)], samples(1000, 10.0, 1.0, k=2))
        (b,) = cases.speed_blocks(sub, sub["_merged"])
        self.assertEqual(b["st"], "untestable")          # 2 samples: an outlier could not be seen

    def test_skip_records_and_no_hostspeed(self):
        recs = [hello(), {"t": "speed", "mod": "hostspeed", "ev": "skip", "insn": "cas2", "why": "68060"}]
        sub = {"_merged": merged(recs)}
        (b,) = cases.speed_blocks(sub, sub["_merged"])
        self.assertEqual(b["st"], "skip")
        self.assertIsNone(cases.speed_case(b, sub))
        self.assertEqual(cases.speed_blocks({}, merged([hello()])), [])

    def test_cost_per_instruction(self):
        bl = [block_recs("stop", 1000, 50, 3537240, units=50), block_recs("empty", 1001, 16 * 100000, 3537240,
                                                                          units=100000),
              block_recs("add_l", 1002, 1024 * 3000, 3537240, units=3000),
              block_recs("fadd", 1003, 1024 * 3000, 3537240, cls="fpu", units=3000),
              block_recs("fsin", 1004, 1024 * 3000, 3537240, cls="fpu", units=3000)]
        cpu = {"stop": 0.2, "empty": 0.4, "add_l": 0.4, "fadd": 0.5, "fsin": 0.9}
        smp = []
        for i, name in enumerate(cpu):
            smp += samples(1000 + i, 10.0 + 2 * i, 1.0, cpu_per_s=cpu[name], f0=100 + 60 * i)
        sub = sub_with(bl, smp)
        b = {x["insn"]: x for x in cases.speed_blocks(sub, sub["_merged"])}
        self.assertAlmostEqual(b["fsin"]["cpu_per_emu_s"], 0.9, places=2)
        # fsin: (0.9 - 0.2) s per emulated second over 3072000 instructions, less 1/64 of a DBF iteration
        loop = (0.4 - 0.2) * 1e9 / 1600000 * 16 * 3000 / 3072000
        self.assertAlmostEqual(b["fsin"]["ns_per_op"], 0.7e9 / 3072000 - loop, delta=1.0)
        self.assertEqual(b["fsin"]["ref"], "fadd")
        self.assertAlmostEqual(b["fsin"]["ratio_ref"], b["fsin"]["ns_per_op"] / b["fadd"]["ns_per_op"], places=1)

    def test_pseudo_case_fingerprint_and_failure_item(self):
        _, sub = self.blocks([("add_l", 1.0), ("fsin", 1.6)])
        out = {"cases": {}}
        cases._speed_cases(out, [(None, sub, sub["_merged"])])
        c = out["cases"]["host.speed_fsin"]
        self.assertEqual(c["fp"], cases.canonical({"mod": "host", "case": "speed_fsin", "st": "fail"}))
        self.assertTrue(c["pseudo"] and c["engine_dependent"])
        self.assertAlmostEqual(c["timing"]["rtf"], 1.6, places=3)
        res = {"id": "e1", "status": "pass", "disks": [dict(sub, disk_name="6-cpux", status="pass")]}
        items = [i for i in report.collect_failures([res]) if i["kind"] == "case_fail"]
        self.assertEqual([i["case"] for i in items], ["speed_fsin"])
        why = items[0]["why"]
        self.assertIn("FSIN: the emulator ran at 0.62x real time", why)
        self.assertIn("Cortex-A76 x4", why)
        self.assertIn("--jobs 2", why)


class SpeedParity(unittest.TestCase):
    def test_speed_cases_are_not_compared(self):
        ref = {"id": "a", "status": "pass", "options": {"cachesize": "0"}}
        twin = {"id": "a-jit", "status": "pass", "twin_kind": "jit", "options": {"cachesize": "8192"}}
        body = {"mod": "host", "case": "speed_fsin"}
        fp_a = {"cases": {"host.speed_fsin": {"mod": "host", "case": "speed_fsin", "st": "fail", "engine_dependent":
                                              True, "fp": cases.canonical(dict(body, st="fail")), "rec": {}}}}
        fp_b = {"cases": {"host.speed_fsin": dict(fp_a["cases"]["host.speed_fsin"], st="pass",
                                                  fp=cases.canonical(dict(body, st="pass")))}}
        items, info = parity.compare(ref, twin, fp_a, fp_b)
        self.assertEqual(items, [])
        self.assertEqual(info["compared"], 0)
        self.assertEqual(info["status"], "compared")


class SpeedSampler(unittest.TestCase):
    def test_samples_only_inside_blocks(self):
        run = runner.EntryRun.__new__(runner.EntryRun)
        run.t0, run.pid, run.speed_samples = 0.0, None, []

        def h(mod, case, hb):
            return mailbox.Header(0, [1, 1, hb, 0, 4096, mod, case])
        run.speed_sample(h(26, 0, 5))          # hostspeed, between blocks
        run.speed_sample(h(21, 1003, 6))       # another module
        run.speed_sample(h(26, 1003, 7))
        self.assertEqual(len(run.speed_samples), 1)
        self.assertEqual(run.speed_samples[0][1:3], [7, 1003])

    def test_follower_reads_the_header_once_without_new_bytes(self):
        img = mailbox.build_image([], ring_size=64, state=1)

        class Mem:
            reads = 0

            def read_longs(self, addrs):
                Mem.reads += 1
                return [int.from_bytes(img[a:a + 4], "big") for a in addrs]
        seen = []
        f = mailbox.RingFollower(Mem(), 0, on_header=seen.append)
        r = f.poll()
        self.assertEqual(r["data"], b"")
        self.assertEqual(Mem.reads, 1)
        self.assertEqual(len(seen), 1)

    def test_thread_cpu_of_this_process(self):
        main, total, top = runner.thread_cpu(os.getpid())
        self.assertGreater(main, 0)
        self.assertGreaterEqual(total, main)


class SpeedMatrix(unittest.TestCase):
    def test_real_speed_entries_and_twins(self):
        std = {e["id"]: e for e in matrix.expand("standard")}
        want = {"a1200-68020-882-fast-real": True, "a4000-68040nommu-fast-real": True,
                "a4000-68060nommu-fast-real": True, "a500-68000-fast-real": False}
        for eid, twin in want.items():
            e = std[eid]
            self.assertEqual(e["options"]["cpu_speed"], "real")
            self.assertEqual(e["options"]["cycle_exact"], "false")
            self.assertEqual(e["disk_groups"], ["cpux"])
            self.assertEqual(e["params"]["modules"], matrix.module_mask(["hostspeed"]))
            t = std.get(eid + "-jit")
            if twin:
                self.assertEqual(t["twin_of"], eid)
                self.assertEqual(t["options"]["cachesize"], matrix.JIT_CACHE)
                self.assertEqual(t["options"]["cpu_speed"], "real")
                self.assertEqual(t["disk_groups"], ["cpux"])
                self.assertEqual(t["params"]["modules"], e["params"]["modules"])
            else:
                self.assertIsNone(t)
        smoke = {e["id"] for e in matrix.expand("smoke")}
        self.assertIn("a1200-68020-882-fast-real", smoke)
        self.assertIn("a1200-68020-882-fast-real-jit", smoke)
        self.assertNotIn("a4000-68060nommu-fast-real", smoke)
        full = {e["id"] for e in matrix.expand("full")}
        self.assertTrue(set(want) <= full)

    def test_module_id(self):
        self.assertEqual(matrix.MODULE_IDS["hostspeed"], 26)
        self.assertEqual(matrix.module_mask(["hostspeed"]), hex(1 << 26))


if __name__ == "__main__":
    unittest.main()
