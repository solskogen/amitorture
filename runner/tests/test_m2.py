"""Milestone-2 runner logic on fake data (no emulator)."""
import copy
import json
import os
import shutil
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from amitorture import baseline as B  # noqa: E402
from amitorture import cases, codemap, matrix, narrow, parity, problems, records, xfail  # noqa: E402
from amitorture import report  # noqa: E402

WORK_TMP = os.path.join(os.path.dirname(__file__), "..", "..", "work", "tmp")


def rec(seq, t, **kw):
    return {"v": 1, "seq": seq, "t": t, **kw}


def boot(case_list, perf=(), mod="cpu", mid=2):
    """Record dicts for one boot: mod begin, the cases, perf, mod end."""
    out = [rec(0, "hello", build="b", mode=0, seed=1, exec="40.68", cpu="68020", fpu="none", mmu=0,
               probe={"cpu": "68020", "fpu": "none", "chipset": "AGA"}),
           rec(1, "mod", mod=mod, id=mid, ev="begin")]
    for c in case_list:
        out.append(rec(len(out), "case", mod=mod, **c))
    for p in perf:
        out.append(rec(len(out), "perf", mod=mod, **p))
    out.append(rec(len(out), "mod", mod=mod, id=mid, ev="end", **{"pass": 0, "fail": 0, "skip": 0}))
    return out


def fp_of(case_list, perf=()):
    return cases.extract_records(boot(case_list, perf))


def fake_result(rid, case_list, status="pass", twin_of=None, twin_kind=None, options=None, perf=()):
    m = records.merge({"serial": [records.parse_line(records.encode(r)) for r in boot(case_list, perf)]})
    return {"id": rid, "status": status, "twin_of": twin_of, "twin_kind": twin_kind, "jit": twin_kind == "jit",
            "options": options or {"cachesize": "0"}, "_merged": m, "machine": {}, "title": rid}


class Fingerprints(unittest.TestCase):
    def test_timing_removed_everywhere(self):
        r = {"mod": "irq", "case": "lat", "st": "pass", "ctx": {"ticks": 5, "latency_us": 3, "a": {"timing": 1},
                                                              "b": "00ff"}}
        stripped, timing = cases.strip_timing(r)
        self.assertEqual(stripped, {"mod": "irq", "case": "lat", "st": "pass", "ctx": {"a": {}, "b": "00ff"}})
        self.assertEqual(timing, {"ctx.ticks": 5, "ctx.latency_us": 3, "ctx.a.timing": 1})

    def test_extract_keys_perf_and_probe(self):
        f = fp_of([{"case": "a", "st": "pass"}, {"case": "a", "st": "pass"}],
                  perf=[{"name": "loop", "ops": 100, "ticks": 250}])
        self.assertIn("cpu.a", f["cases"])
        self.assertIn("cpu.a#2", f["cases"])
        self.assertIn(cases.PROBE_KEY, f["cases"])
        self.assertEqual(f["perf"]["cpu.loop"]["value"], 2.5)
        self.assertEqual(f["modules"], {"cpu": 2})
        # seq does not matter
        f2 = cases.extract_records([dict(x, seq=x["seq"] + 7) for x in boot([{"case": "a", "st": "pass"}])])
        self.assertEqual(f2["cases"]["cpu.a"]["fp"], f["cases"]["cpu.a"]["fp"])

    def test_diff_paths(self):
        d = cases.diff_paths({"a": 1, "b": {"c": [1, 2]}}, {"a": 1, "b": {"c": [1, 3]}, "d": 0})
        self.assertEqual(d, [("b.c[1]", 2, 3), ("d", "<absent>", 0)])


def disk_result(rid, name, case_list, status="pass", mod="cpu", mid=2, probe_extra=None):
    recs = boot(case_list, mod=mod, mid=mid)
    if probe_extra:
        recs[0]["probe"].update(probe_extra)
    m = records.merge({"serial": [records.parse_line(records.encode(r)) for r in recs]})
    hello = m.by_type("hello")
    return {"id": rid, "title": rid, "status": status, "reason": f"{status} reason", "disk_name": name,
            "options": {"cachesize": "0"}, "machine": {}, "_merged": m, "raw": f"raw/{rid}/{name}/",
            "records": {"merged": len(m.records), "hello": hello[-1] if hello else None, "done": None},
            "modules": {mod: mid}, "disk": {"path": f"/d/{name}.adf", "build_id": f"b {name}"},
            "duration_s": 2.0, "process": {"exit_before_stop": {"running": True}}}


class DiskSet(unittest.TestCase):
    def test_combine_worst_status_and_cases_from_every_disk(self):
        from amitorture import runner
        entry = {"id": "e"}
        subs = [disk_result("e", "1-core", [{"case": "a", "st": "pass"}], mod="rt", mid=1),
                disk_result("e", "2-cpu", [{"case": "b", "st": "fail", "expect": 1, "got": 2, "why": "w"}]),
                disk_result("e", "3-fpu", [], status="hang", mod="fpu", mid=9)]
        r = runner.combine(entry, subs)
        self.assertEqual(r["status"], "hang")
        self.assertIn("3-fpu: hang", r["reason"])
        self.assertEqual(r["modules"], {"rt": 1, "cpu": 2, "fpu": 9})
        fp = cases.extract(r)
        self.assertEqual({k for k in fp["cases"] if k != cases.PROBE_KEY}, {"rt.a", "cpu.b"})
        self.assertEqual(fp["cases"]["cpu.b"]["disk"], "2-cpu")
        self.assertEqual(len({m["boot"] for m in r["_merged"].records}), 3)     # one boot per disk
        items = report.collect_failures([r])
        kinds = [(i["kind"], i.get("disk")) for i in items]
        self.assertIn(("case_fail", "2-cpu"), kinds)
        self.assertIn(("hang", "3-fpu"), kinds)
        self.assertEqual(sum(1 for k, _ in kinds if k == "probe_mismatch"), 0)

    def test_status_order(self):
        from amitorture import runner
        subs = [disk_result("e", "1", [], status="fail"), disk_result("e", "2", [], status="timeout"),
                disk_result("e", "3", [], status="pass")]
        self.assertEqual(runner.combine({"id": "e"}, subs)["status"], "timeout")
        subs = [disk_result("e", "1", [], status="pass"), disk_result("e", "2", [], status="fail")]
        self.assertEqual(runner.combine({"id": "e"}, subs)["status"], "fail")

    def test_probe_consistency(self):
        from amitorture import runner
        same = [disk_result("e", "1", [], probe_extra={"mem": [{"name": "chip", "free": 100}]}),
                disk_result("e", "2", [], probe_extra={"mem": [{"name": "chip", "free": 999}]})]
        r = runner.combine({"id": "e"}, same)
        self.assertTrue(r["probe_consistency"]["ok"])           # free memory differs per payload: ignored
        diff = [disk_result("e", "1", []), disk_result("e", "2", [], probe_extra={"cpu": "68030"})]
        r = runner.combine({"id": "e"}, diff)
        self.assertFalse(r["probe_consistency"]["ok"])
        items = [i for i in report.collect_failures([r]) if i["kind"] == "probe_inconsistent"]
        self.assertEqual(len(items), 1)
        self.assertEqual(items[0]["differences"][0]["path"], "probe.cpu")

    def test_disk_set_order_and_entry_disks(self):
        from amitorture import adf, runner
        d = tempfile.mkdtemp(dir=os.path.abspath(WORK_TMP) if os.path.isdir(WORK_TMP) else None)
        try:
            for n in ("amitorture-3-fpu.adf", "amitorture-1-core.adf", "amitorture-2-cpu.adf", "notes.txt"):
                open(os.path.join(d, n), "w").close()
            ds = adf.disk_set(d)
            self.assertEqual([n for n, _ in ds], ["1-core", "2-cpu", "3-fpu"])
            ctx = {"disks": ds}
            self.assertEqual(len(runner.entry_disks(ctx, {"id": "x"})), 3)
            self.assertEqual([n for n, _ in runner.entry_disks(ctx, {"id": "x", "disk_only": "2-cpu"})], ["2-cpu"])
            self.assertEqual(runner.entry_disks({"disks": None}, {"id": "x"}), [None])
        finally:
            shutil.rmtree(d)

    def test_undef_markers_not_in_fingerprint(self):
        a = fp_of([{"case": "smc", "st": "pass", "ctx": {"undef_observed": "old", "undef_changed_at": 3, "x": 1}}])
        b = fp_of([{"case": "smc", "st": "pass", "ctx": {"undef_observed": "new", "x": 1}}])
        self.assertEqual(a["cases"]["cpu.smc"]["fp"], b["cases"]["cpu.smc"]["fp"])
        self.assertEqual(a["cases"]["cpu.smc"]["excluded"], {"ctx.undef_observed": "old", "ctx.undef_changed_at": 3})
        ref = fake_result("m", [{"case": "smc", "st": "pass", "ctx": {"undef_observed": "old"}}])
        twin = fake_result("m-jit", [{"case": "smc", "st": "pass", "ctx": {"undef_observed": "new"}}],
                           twin_of="m", twin_kind="jit", options={"cachesize": "8192"})
        self.assertEqual(parity.check_all([ref, twin])[0], [])

    def test_narrowing_uses_only_the_module_disk(self):
        entry = dict(matrix.find("a1200-68ec020-fast"))
        res = {"id": entry["id"]}
        items = [{"kind": "case_fail", "entry": entry["id"], "mod": "cpu", "case": "addx", "case_key": "cpu.addx"}]
        seen = []

        def launch(e, binary=None):
            seen.append(e.get("disk_only"))
            return fp_of([{"case": "addx", "st": "fail", "expect": 1, "got": 0, "why": "w"}])
        narrow.run(narrow.groups_from(items, [res]), launch, lambda x: entry, {"cpu": 2}.get, budget_s=1e9,
                   reruns=1, log=lambda m: None, disk_of=lambda r, mod: "2-cpu")
        self.assertTrue(seen and set(seen) == {"2-cpu"})


class EngineFromLog(unittest.TestCase):
    def test_jit_active_and_refused(self):
        from amitorture import runner
        on = runner.engine_from_log(["[ 2.3] CPU=68040, FPU=68040 (host 64b), MMU=0, JIT=CPU/FPU=8192. fast"])
        self.assertEqual((on["cachesize"], on["fpu_model"], on["mode"]), ("8192", "68040", "fast"))
        opts = {"cachesize": "8192", "fpu_model": "68040", "cpu_model": "68040", "mmu_model": "0",
                "cpu_threaded": "false", "fpu_softfloat": "false"}
        self.assertEqual(runner.engine_adjustments(opts, on), [])
        off = runner.engine_from_log(["JIT and cycle-exact can't be enabled simultaneously.",
                                      "CPU=68020, FPU=0, MMU=0, JIT=0. fast 24-bit"])
        adj = runner.engine_adjustments(dict(opts, cpu_model="68020", fpu_model="0"), off)
        self.assertEqual([(a["option"], a["resolved"]) for a in adj], [("cachesize", "0 (JIT not active)")])
        self.assertIn("JIT and cycle-exact can't be enabled simultaneously.", adj[0]["because"])
        self.assertIsNone(runner.engine_from_log(["nothing"]))

    def test_threaded_and_softfloat(self):
        from amitorture import runner
        base = {"cachesize": "0", "fpu_model": "68882", "cpu_model": "68030", "mmu_model": "0",
                "cpu_threaded": "true", "fpu_softfloat": "true"}
        e = runner.engine_from_log(['uae_start_thread "cpu" function at 0x1 arg (nil)',
                                    "CPU=68030, FPU=68882 (softfloat), MMU=0, JIT=0. fast"])
        self.assertEqual(runner.engine_adjustments(base, e), [])
        e = runner.engine_from_log(['uae_start_thread "cpu" function at 0x1 arg (nil)',
                                    "CPU=68030, FPU=68882 (exact), MMU=0, JIT=0. fast"])
        self.assertEqual(runner.engine_adjustments(base, e), [])
        e = runner.engine_from_log(["Selected CPU emulation has no CPU thread, running the CPU on the main thread.",
                                    "CPU=68030, FPU=68882 (host 64b), MMU=0, JIT=0. fast"])
        got = {a["option"] for a in runner.engine_adjustments(base, e)}
        self.assertEqual(got, {"cpu_threaded", "fpu_softfloat"})

    def test_adjusted_twin_not_compared(self):
        ref = fake_result("m", [{"case": "add", "st": "pass"}])
        twin = fake_result("m-jit", [{"case": "add", "st": "fail", "expect": 1, "got": 2, "why": "w"}],
                           twin_of="m", twin_kind="jit", options={"cachesize": "8192"})
        twin["adjusted"] = [{"option": "cachesize", "wrote": "8192", "resolved": "0 (JIT not active)"}]
        items, infos = parity.check_all([ref, twin])
        self.assertEqual(items, [])
        self.assertIn("config adjusted by emulator", infos[0]["why"])


class RingLoss(unittest.TestCase):
    def test_partial_line_after_loss_is_not_corrupt(self):
        lines = [records.encode(rec(i, "log", msg=f"m{i}")) for i in range(4)]
        sp = records.StreamParser("mailbox")
        sp.feed(lines[0])
        sp.lose(len(lines[1]) + 10, 0, "ring overrun")
        sp.feed(lines[2][10:] + lines[3])          # resumes mid-record
        sp.close()
        self.assertEqual([r.seq for r in sp.records], [0, 3])
        self.assertTrue(all(r.crc_ok for r in sp.records))
        self.assertEqual(sp.lost[0]["partial_line_dropped"], len(lines[2]) - 11)
        sp = records.StreamParser("mailbox")      # loss exactly at a line boundary keeps the next line
        sp.feed(lines[0])
        sp.lose(len(lines[1]), 0, "ring overrun")
        sp.feed(lines[2])
        self.assertEqual([r.seq for r in sp.records], [0, 2])


class Parity(unittest.TestCase):
    def setUp(self):
        self.ref = fake_result("m", [{"case": "add", "st": "pass", "ctx": {"sr": "2000", "ticks": 10}},
                                     {"case": "sub", "st": "pass"}])
        self.opts_jit = {"cachesize": "8192", "comp_nf": "true"}

    def test_identical_except_timing(self):
        twin = fake_result("m-jit", [{"case": "add", "st": "pass", "ctx": {"sr": "2000", "ticks": 3}},
                                     {"case": "sub", "st": "pass"}], twin_of="m", twin_kind="jit",
                           options=self.opts_jit)
        items, infos = parity.check_all([self.ref, twin])
        self.assertEqual(items, [])
        self.assertEqual(infos[0]["status"], "compared")
        self.assertEqual(infos[0]["compared"], 3)   # add, sub, probe

    def test_value_difference(self):
        twin = fake_result("m-jit", [{"case": "add", "st": "pass", "ctx": {"sr": "2004"}},
                                     {"case": "sub", "st": "pass"}], twin_of="m", twin_kind="jit",
                           options=self.opts_jit)
        items, _ = parity.check_all([self.ref, twin])
        self.assertEqual(len(items), 1)
        it = items[0]
        self.assertEqual((it["kind"], it["subkind"], it["case_key"]), ("engine_mismatch", "value", "cpu.add"))
        self.assertEqual(it["paths"], [{"path": "ctx.sr", "ref": "2000", "twin": "2004"}])
        self.assertEqual({d["option"] for d in it["differing_options"]}, {"cachesize", "comp_nf"})
        self.assertEqual(it["ref"]["ctx"], {"sr": "2000"})

    def test_missing_case_is_failure(self):
        twin = fake_result("m-jit", [{"case": "add", "st": "pass", "ctx": {"sr": "2000"}}], twin_of="m",
                           twin_kind="jit", options=self.opts_jit)
        items, _ = parity.check_all([self.ref, twin])
        self.assertEqual([(i["subkind"], i["case_key"]) for i in items], [("missing_case", "cpu.sub")])

    def test_softfloat_is_informational_and_crash_not_compared(self):
        sf = fake_result("m-sf", [{"case": "add", "st": "fail", "expect": 1, "got": 2, "why": "x"},
                                  {"case": "sub", "st": "pass"}], twin_of="m", twin_kind="softfloat",
                         options={"cachesize": "0", "fpu_softfloat": "true"})
        items, _ = parity.check_all([self.ref, sf])
        self.assertEqual({i["kind"] for i in items}, {"fpu_mode_difference"})
        crash = dict(fake_result("m-jit", [], twin_of="m", twin_kind="jit"), status="crash")
        items, infos = parity.check_all([self.ref, crash])
        self.assertEqual(items, [])
        self.assertEqual(infos[0]["status"], "not compared")


class Xfail(unittest.TestCase):
    REG = {"fpu.inf_roundtrip": {"ref": "PR #1", "why": "w", "configs": "a4000-* a1200-*",
                                 "when": {"fpu_softfloat": "false"}},
           "timing.ce_*": {"ref": "x", "configs": "*-ce"}}

    def test_match_rules(self):
        self.assertIsNotNone(xfail.match(self.REG, "fpu.inf_roundtrip", "a4000-x", {"fpu_softfloat": "false"}))
        self.assertIsNone(xfail.match(self.REG, "fpu.inf_roundtrip", "a4000-x", {"fpu_softfloat": "true"}))
        self.assertIsNone(xfail.match(self.REG, "fpu.inf_roundtrip", "a500-x", {"fpu_softfloat": "false"}))
        self.assertIsNotNone(xfail.match(self.REG, "timing.ce_unit#2", "a4000-68040-ce", {}))
        self.assertIsNone(xfail.match(self.REG, "timing.ce_unit", "a4000-68040-fast", {}))

    def test_fixed_and_expected(self):
        res = [{"id": "a4000-x", "options": {"fpu_softfloat": "false"}},
               {"id": "a1200-y", "options": {"fpu_softfloat": "false"}}]
        fps = {"a4000-x": cases.extract_records(boot([{"case": "inf_roundtrip", "st": "fail", "expect": 1, "got": 0,
                                                       "why": "w"}], mod="fpu")),
               "a1200-y": cases.extract_records(boot([{"case": "inf_roundtrip", "st": "pass"}], mod="fpu"))}
        xf, fixed = xfail.classify(self.REG, fps, res)
        self.assertEqual([(x["entry"], x["case_key"]) for x in xf], [("a4000-x", "fpu.inf_roundtrip")])
        self.assertEqual([(x["entry"], x["xfail"]["ref"]) for x in fixed], [("a1200-y", "PR #1")])

    def test_shipped_registry_loads(self):
        reg = xfail.load()
        self.assertIn("fpu.frestore_null_nan", reg)
        self.assertIn("fpu.inf_roundtrip", reg)
        entries = [x for v in reg.values() for x in (v if isinstance(v, list) else [v])]
        self.assertTrue(entries and all(x.get("ref") for x in entries))


class Baselines(unittest.TestCase):
    ENTRY = {"id": "e1", "options": {"cpu_model": "68020", "cachesize": "0"}}

    def runs(self, variants):
        return [fp_of(v) for v in variants]

    def make(self, variants, perf=None):
        runs = [fp_of(v, perf=(perf[i] if perf else ())) for i, v in enumerate(variants)]
        return B.build({"e1": {"entry": self.ENTRY, "statuses": ["pass"] * len(runs), "runs": runs,
                               "meta": {"rom": 15}}}, {"name": "t"})

    def test_stability(self):
        base = self.make([[{"case": "a", "st": "pass"}, {"case": "b", "st": "pass", "ctx": {"x": 1}}],
                          [{"case": "a", "st": "pass"}, {"case": "b", "st": "pass", "ctx": {"x": 2}}],
                          [{"case": "a", "st": "pass"}]])
        c = base["entries"]["e1"]["cases"]
        self.assertTrue(c["cpu.a"]["stable"])
        self.assertFalse(c["cpu.b"]["stable"])
        self.assertEqual(c["cpu.b"]["present"], "2/3")
        self.assertEqual(base["meta"]["unstable_cases"], 1)

    def check(self, base, variants, entry=None, perf=None, registry=None):
        runs = [fp_of(v, perf=(perf[i] if perf else ())) for i, v in enumerate(variants)]
        return B.diff(base, {"e1": {"entry": entry or self.ENTRY, "statuses": ["pass"] * len(runs), "runs": runs,
                                    "meta": {"rom": 15}}}, registry)

    def test_same_is_clean(self):
        v = [{"case": "a", "st": "pass"}, {"case": "b", "st": "fail", "expect": 1, "got": 2, "why": "w"}]
        base = self.make([v, v, v])
        d = self.check(base, [v])
        self.assertEqual(d["regressions"], 0)
        self.assertEqual(d["findings"], [])

    def test_new_unexpected_failure_regresses(self):
        old = [{"case": "a", "st": "pass"}]
        failed = {"case": "new", "st": "fail", "expect": 1, "got": 0, "why": "broken"}
        d = self.check(self.make([old]), [old + [failed]])
        self.assertEqual(d["regressions"], 1)
        self.assertEqual([f["kind"] for f in d["findings"]], ["new_failure"])

    def test_new_intermittent_failure_regresses(self):
        old = [{"case": "a", "st": "pass"}]
        d = self.check(self.make([old]), [old + [{"case": "new", "st": "pass"}],
                                       old + [{"case": "new", "st": "fail", "expect": 1, "got": 0, "why": "broken"}]])
        self.assertEqual(d["regressions"], 1)
        self.assertEqual(d["findings"][0]["kind"], "new_failure")

    def test_new_registered_failure_is_informational(self):
        old = [{"case": "a", "st": "pass"}]
        now = old + [{"case": "new", "st": "fail", "expect": 1, "got": 0, "why": "known"}]
        registry = {"cpu.new": {"ref": "known", "when": {"cpu_model": "68020"}}}
        d = self.check(self.make([old]), [now], registry=registry)
        self.assertEqual(d["regressions"], 0)
        self.assertEqual(d["findings"][0]["kind"], "new")
        self.assertEqual(d["findings"][0]["xfail"]["ref"], "known")
        other = dict(self.ENTRY, options=dict(self.ENTRY["options"], cpu_model="68030"))
        self.assertEqual(self.check(self.make([old]), [now], entry=other, registry=registry)["regressions"], 1)

    def test_empty_baseline_cases_do_not_hide_new_failures(self):
        base = self.make([[]])
        base["entries"]["e1"]["cases"] = {}
        d = self.check(base, [[{"case": "new", "st": "fail", "expect": 1, "got": 0, "why": "broken"}]])
        self.assertEqual(d["regressions"], 1)
        self.assertIn("new_failure", {f["kind"] for f in d["findings"]})

    def test_missing_measurements_regress_even_on_max_speed(self):
        old = [{"case": "a", "st": "pass", "ctx": {"latency": 100}}]
        base = self.make([old], perf=[[{"name": "loop", "ops": 10, "ticks": 100}]])
        fast = dict(self.ENTRY, options=dict(self.ENTRY["options"], cpu_speed="max"))
        d = self.check(base, [[{"case": "a", "st": "pass", "ctx": {}}]], entry=fast)
        self.assertEqual({f["kind"] for f in d["findings"]}, {"perf_missing", "timing_missing"})
        self.assertEqual(d["regressions"], 2)

    def test_perf_only_baseline_still_compares_measurements(self):
        base = self.make([[]], perf=[[{"name": "loop", "ops": 10, "ticks": 100}]])
        base["entries"]["e1"]["cases"] = {}
        d = self.check(base, [[]])
        self.assertEqual(d["regressions"], 1)
        self.assertIn("perf_missing", {f["kind"] for f in d["findings"]})

    def test_all_kinds(self):
        good = [{"case": "a", "st": "pass"}, {"case": "b", "st": "fail", "expect": 1, "got": 2, "why": "w"},
                {"case": "c", "st": "pass"}, {"case": "d", "st": "pass"}]
        flaky = [dict(x) for x in good] + [{"case": "u", "st": "pass", "ctx": {"n": 1}}]
        flaky2 = [dict(x) for x in good] + [{"case": "u", "st": "pass", "ctx": {"n": 2}}]
        base = self.make([flaky, flaky2, flaky])
        now1 = [{"case": "a", "st": "fail", "expect": 1, "got": 0, "why": "broke"},   # regression
                {"case": "b", "st": "pass"},                                         # fixed
                {"case": "d", "st": "pass", "ctx": {"r": 1}},                        # became unstable
                {"case": "n", "st": "pass"},                                         # new
                {"case": "u", "st": "pass", "ctx": {"n": 9}}]                        # unstable_excluded; c missing
        now2 = [dict(x) for x in now1]
        now2[2] = {"case": "d", "st": "pass", "ctx": {"r": 2}}
        d = self.check(base, [now1, now2], entry={"id": "e1", "options": {"cpu_model": "68030", "cachesize": "0"}},
                       registry={"cpu.b": {"ref": "bug#1"}})
        kinds = {(f["kind"], f.get("case_key")) for f in d["findings"]}
        self.assertIn(("regression", "cpu.a"), kinds)
        self.assertIn(("fixed", "cpu.b"), kinds)
        self.assertIn(("became_unstable", "cpu.d"), kinds)
        self.assertIn(("new", "cpu.n"), kinds)
        self.assertIn(("missing", "cpu.c"), kinds)
        self.assertIn(("unstable_excluded", "cpu.u"), kinds)
        reg = next(f for f in d["findings"] if f["kind"] == "regression")
        self.assertEqual(reg["transition"], "pass->fail")
        self.assertEqual(reg["config_changes"], [{"option": "cpu_model", "baseline": "68020", "now": "68030"}])
        self.assertIn("cpu_model 68020->68030", reg["why"])
        fx = next(f for f in d["findings"] if f["kind"] == "fixed")
        self.assertEqual(fx["xfail"]["ref"], "bug#1")
        self.assertEqual(d["regressions"], 3)    # regression, became_unstable, missing

    def test_perf_band_both_directions(self):
        v = [{"case": "a", "st": "pass"}]
        perf = [[{"name": "p", "ops": 10, "ticks": t}] for t in (1000, 1010, 990)]
        base = self.make([v, v, v], perf=perf)
        b = base["entries"]["e1"]["perf"]["cpu.p"]
        self.assertAlmostEqual(b["mean"], 100.0)
        self.assertEqual(b["ops"], [10])
        d = self.check(base, [v], perf=[[{"name": "p", "ops": 10, "ticks": 1005}]])
        self.assertEqual(d["findings"], [])
        d = self.check(base, [v], perf=[[{"name": "p", "ops": 10, "ticks": 500}]])
        self.assertEqual([(f["kind"], f["direction"]) for f in d["findings"]], [("perf_out_of_band", "lower")])
        d = self.check(base, [v], perf=[[{"name": "p", "ops": 10, "ticks": 2000}]])
        self.assertEqual([(f["kind"], f["direction"]) for f in d["findings"]], [("perf_out_of_band", "higher")])

    def test_perf_on_max_speed_is_informational(self):
        v = [{"case": "a", "st": "pass"}]
        perf = [[{"name": "p", "ops": 10, "ticks": t}] for t in (1000, 1010, 990)]
        base = self.make([v, v, v], perf=perf)
        fast = {"id": "e1", "options": dict(self.ENTRY["options"], cpu_speed="max")}
        d = self.check(base, [v], entry=fast, perf=[[{"name": "p", "ops": 10, "ticks": 500}]])
        self.assertEqual(d["regressions"], 0)
        self.assertTrue(d["findings"][0]["host_dependent"])

    def test_band_minimums(self):
        b = B.band([100, 100, 100])
        self.assertEqual((b["lo"], b["hi"]), (95.0, 105.0))      # 5 % minimum
        self.assertEqual(B.band([0, 0])["hi"], 1.0)                 # absolute minimum

    def test_timing_band(self):
        v = lambda t: [{"case": "a", "st": "pass", "ctx": {"latency": t}}]  # noqa: E731
        base = self.make([v(100), v(102), v(98)])
        self.assertTrue(base["entries"]["e1"]["cases"]["cpu.a"]["stable"])   # timing is not in the fingerprint
        d = self.check(base, [v(300)])
        self.assertEqual([f["kind"] for f in d["findings"]], ["timing_out_of_band"])

    def test_incomplete_runs_do_not_make_cases_unstable(self):
        v = [{"case": "a", "st": "pass"}, {"case": "b", "st": "pass"}]
        runs = [fp_of(v), fp_of(v[:1]), fp_of(v)]
        base = B.build({"e1": {"entry": self.ENTRY, "statuses": ["pass", "hang", "pass"], "runs": runs,
                               "meta": {}}}, {"name": "t"})
        e = base["entries"]["e1"]
        self.assertEqual((e["complete_runs"], e["excluded_runs"]), (2, [2]))
        self.assertTrue(all(c["stable"] for c in e["cases"].values()))
        same = B.diff(base, {"e1": {"entry": self.ENTRY, "statuses": ["hang", "pass"],
                                    "runs": [fp_of(v[:1]), fp_of(v)], "meta": {}}})
        self.assertEqual(same["findings"], [])          # baseline had a hang too; complete runs agree
        d = B.diff(base, {"e1": {"entry": self.ENTRY, "statuses": ["crash", "pass"], "runs": [fp_of([]), fp_of(v)],
                                 "meta": {}}})
        self.assertEqual({f["kind"] for f in d["findings"]}, {"entry_status"})

    def test_entry_status_and_first_changes(self):
        v = [{"case": "a", "st": "pass"}]
        base = self.make([v, v])
        same = self.check(base, [v])
        bad = self.check(base, [[{"case": "a", "st": "fail", "expect": 0, "got": 1, "why": "w"}]])
        crash = B.diff(base, {"e1": {"entry": self.ENTRY, "statuses": ["crash"], "runs": [fp_of([])], "meta": {}}})
        self.assertIn("entry_status", {f["kind"] for f in crash["findings"]})
        first = B.first_changes([("A", same), ("B", bad), ("C", bad)])
        self.assertEqual([(f["key"], f["first_binary"], f["on"]) for f in first], [("cpu.a", "B", ["B", "C"])])

    def test_save_load_roundtrip(self):
        d = tempfile.mkdtemp(dir=os.path.abspath(WORK_TMP) if os.path.isdir(WORK_TMP) else None)
        try:
            base = self.make([[{"case": "a", "st": "pass"}]])
            B.save(d, "x", base)
            self.assertEqual(B.load(d, "x")["entries"]["e1"]["cases"]["cpu.a"]["fp"],
                             base["entries"]["e1"]["cases"]["cpu.a"]["fp"])
            self.assertIsNone(B.load(d, "nope"))
        finally:
            shutil.rmtree(d)


class Narrowing(unittest.TestCase):
    OPTS = matrix.find("a4000-68040nommu-fast-jit")["options"]

    def fpx(self, st):
        return fp_of([{"case": "addx", "st": st, **({"expect": 1, "got": 0, "why": "w"} if st == "fail" else {})}])

    def test_comp_nf_signature(self):
        base = [self.fpx("fail")] * 3
        variants = [{"axis": "jit", "label": "off", "fp": self.fpx("pass")},
                    {"axis": "comp_nf", "label": "false", "fp": self.fpx("pass")},
                    {"axis": "comp_constjump", "label": "false", "fp": self.fpx("fail")},
                    {"axis": "cpu_model", "label": "68030", "fp": self.fpx("fail")},
                    {"axis": "chipset", "label": "ocs", "fp": None}]
        r = narrow.analyze("cpu.addx", "case_fail", self.OPTS, base, variants)
        self.assertEqual(r["determinism"], "deterministic")
        self.assertEqual(r["fail_rate"], "3/3")
        self.assertTrue(r["statement"].startswith("fails only with JIT on and comp_nf=true"), r["statement"])
        self.assertIn("still fails with comp_constjump=false, cpu_model=68030", r["statement"])
        self.assertEqual(r["signature"], ["comp_nf=false", "jit=off"])
        self.assertEqual([x["axis"] for x in r["inconclusive"]], ["chipset"])

    def test_intermittent(self):
        base = [self.fpx("fail"), self.fpx("pass"), self.fpx("fail")]
        r = narrow.analyze("cpu.addx", "case_fail", self.OPTS, base, [])
        self.assertEqual((r["determinism"], r["fail_rate"]), ("intermittent", "2/3"))
        self.assertTrue(r["statement"].startswith("intermittent (2/3"))

    def test_not_reproduced_claims_no_flips(self):
        base = [self.fpx("pass")] * 3
        r = narrow.analyze("cpu.addx", "case_fail", self.OPTS, base,
                           [{"axis": "jit", "label": "off", "fp": self.fpx("pass")}])
        self.assertEqual(r["flips"], [])
        self.assertEqual(r["signature"], [])
        self.assertTrue(r["statement"].startswith("not reproduced"), r["statement"])

    def test_case_fail_wins_and_known_runs_are_reused(self):
        twin = matrix.find("a4000-68040nommu-fast-jit")
        ref = matrix.find("a4000-68040nommu-fast")
        items = [{"kind": "engine_mismatch", "entry": twin["id"], "mod": "cpu", "case": "addx",
                  "case_key": "cpu.addx", "ref": {"st": "pass"}, "ref_entry": ref["id"]},
                 {"kind": "case_fail", "entry": twin["id"], "mod": "cpu", "case": "addx", "case_key": "cpu.addx"}]
        groups = narrow.groups_from(items, [{"id": twin["id"]}])
        self.assertEqual(groups[0]["cases"], {"cpu.addx": "case_fail"})
        launched = []

        def launch(e, binary=None):
            launched.append(e["id"])
            return self.fpx("fail")
        known = [(ref["options"], self.fpx("pass"), ref["id"])]
        out = narrow.run(groups, launch, lambda x: twin, {"cpu": 2}.get, budget_s=1e9, reruns=1,
                         log=lambda m: None, known=known)
        g = out["groups"][0]
        self.assertNotIn(twin["id"] + "~jit=off", launched)
        reused = [x for x in g["launches"] if x.get("reused")]
        self.assertEqual([(x["axis"], x["reused"]) for x in reused], [("jit", ref["id"])])
        self.assertEqual([f["axis"] for f in g["cases"]["cpu.addx"]["flips"]], ["jit"])

    def test_engine_mismatch_outcome(self):
        ref = fp_of([{"case": "addx", "st": "pass", "ctx": {"x": 1}}])
        bad = fp_of([{"case": "addx", "st": "pass", "ctx": {"x": 2}}])
        rfp = ref["cases"]["cpu.addx"]["fp"]
        self.assertEqual(narrow.outcome(bad, "cpu.addx", "engine_mismatch", rfp), "bad")
        self.assertEqual(narrow.outcome(ref, "cpu.addx", "engine_mismatch", rfp), "good")
        # a variant that changes the machine compares with the reference rerun with the same change
        r = narrow.analyze("cpu.addx", "engine_mismatch", self.OPTS, [bad],
                           [{"axis": "cpu_model", "label": "68030", "fp": bad, "ref_run": bad},
                            {"axis": "jit", "label": "off", "fp": ref}], ref_fp=rfp)
        self.assertEqual([x["axis"] for x in r["flips"]], ["jit", "cpu_model"])   # AXIS_ORDER

    def test_missing_paired_reference_is_inconclusive(self):
        ref = fp_of([{"case": "addx", "st": "pass", "ctx": {"x": 1}}])
        bad = fp_of([{"case": "addx", "st": "pass", "ctx": {"x": 2}}])
        r = narrow.analyze("cpu.addx", "engine_mismatch", self.OPTS, [bad],
                           [{"axis": "cpu_model", "label": "68030", "fp": ref, "ref_run": None}],
                           ref_fp=ref["cases"]["cpu.addx"]["fp"])
        self.assertEqual(r["flips"], [])
        self.assertEqual(r["inconclusive"][0]["why"], "no reference")

    def test_run_budget_and_launch_plan(self):
        entry = dict(matrix.find("a4000-68040nommu-fast-jit"))
        res = {"id": entry["id"]}
        items = [{"kind": "case_fail", "entry": entry["id"], "mod": "cpu", "case": "addx", "case_key": "cpu.addx"}]
        groups = narrow.groups_from(items, [res])
        self.assertEqual(len(groups), 1)
        launched = []
        t = [0.0]

        def clock():
            return t[0]

        def launch(e, binary=None):
            launched.append((e["id"], e["params"]))
            t[0] += 10
            opts = e["options"]
            ok = opts["cachesize"] == "0" or opts.get("comp_nf") == "false"
            return self.fpx("pass" if ok else "fail")

        out = narrow.run(groups, launch, lambda x: entry, {"cpu": 2}.get, budget_s=1000, reruns=2, log=lambda m: None,
                         clock=clock)
        g = out["groups"][0]
        self.assertTrue(all(p == {"modules": "0x4"} for _, p in launched))
        ids = [x[0] for x in launched]
        self.assertEqual(ids[0], entry["id"] + "~rerun=1")                  # round 1: one rerun
        self.assertEqual(ids[1], entry["id"] + "~jit=off")                  # then the most informative axis
        self.assertIn(entry["id"] + "~rerun=2", ids)
        r = g["cases"]["cpu.addx"]
        self.assertEqual(r["signature"], ["comp_nf=false", "jit=off"])
        self.assertFalse(out["exhausted"])
        # tight budget: stops and lists what was not tried
        launched.clear()
        t[0] = 0.0
        out = narrow.run(groups, launch, lambda x: entry, {"cpu": 2}.get, budget_s=35, reruns=2, log=lambda m: None,
                         clock=clock)
        self.assertTrue(out["exhausted"])
        self.assertEqual(len(launched), 4)
        self.assertTrue(out["groups"][0]["not_tried"])
        # two groups, budget for 2 launches: both get their first rerun (breadth first); one never
        # started is labelled "not narrowed"
        launched.clear()
        t[0] = 0.0
        e2 = dict(entry, id="other")
        g2 = narrow.groups_from(items + [dict(items[0], entry="other")], [res, {"id": "other"}])
        out = narrow.run(g2, launch, lambda x: e2 if (x if isinstance(x, str) else x["id"]) == "other" else entry,
                         {"cpu": 2}.get, budget_s=15, reruns=2, log=lambda m: None, clock=clock)
        self.assertEqual([x[0].split("~")[1] for x in launched], ["rerun=1", "rerun=1"])
        # parallel launches give the same analysis
        launched.clear()
        out = narrow.run(groups, launch, lambda x: entry, {"cpu": 2}.get, budget_s=1e9, reruns=2,
                         log=lambda m: None, jobs=3)
        self.assertEqual(out["groups"][0]["cases"]["cpu.addx"]["signature"], ["comp_nf=false", "jit=off"])


class CodeMap(unittest.TestCase):
    def test_core_tables(self):
        ct = codemap.core_table
        self.assertEqual(ct({"cpu_model": "68000", "cycle_exact": "true", "cpu_compatible": "true"})["table"], 14)
        self.assertEqual(ct({"cpu_model": "68000", "cpu_compatible": "true", "cycle_exact": "false"})["table"], 12)
        self.assertEqual(ct({"cpu_model": "68020"})["table"], 3)
        self.assertEqual(ct({"cpu_model": "68030", "mmu_model": "68030", "cpu_compatible": "true"})["table"], 34)
        self.assertEqual(ct({"cpu_model": "68040", "cachesize": "8192"})["table"], 41)
        self.assertEqual(ct({"cpu_model": "68040", "cachesize": "8192", "comp_trustbyte": "indirect"})["table"], 51)
        self.assertEqual(ct({"cpu_model": "68060", "mmu_model": "68060"})["table"], 33)
        self.assertEqual(ct({"cpu_model": "68060"})["file"], "cpuemu_0.cpp")
        self.assertEqual(codemap.handler_file(14), "cpuemu_13.cpp")
        self.assertEqual(codemap.handler_file(45), "cpuemu_40.cpp")

    def test_opcode_from_ctx(self):
        self.assertEqual(codemap.opcode_from({"op": "d090"}), 0xd090)
        self.assertEqual(codemap.opcode_from({"x": 1}, {"words": ["4e71", "0000"]}), 0x4e71)
        self.assertEqual(codemap.opcode_from({"opcode": "0xd0810000"}), 0xd081)
        self.assertIsNone(codemap.opcode_from({"pc": "00001234"}))

    def test_without_source(self):
        h = codemap.handler(0xd090, {"cpu_model": "68020", "cachesize": "8192"}, None)
        self.assertEqual(h["function"], "op_d090_43_ff")
        self.assertIn("guess", h["confidence"])
        self.assertTrue(h["jit"]["function"].startswith("op_d090_0_comp"))

    def test_jit_pointer_without_source(self):
        p = codemap.pointers({"mod": "cpu_basic", "kind": "engine_mismatch", "ctx": {"op": "4480"}},
                             {"cpu_model": "68020", "cachesize": "8192"}, None)
        self.assertTrue(any(x["function"] == "op_4480_0_comp_ff" for x in p), p)

    def test_pointers_by_module(self):
        p = codemap.pointers({"mod": "blitter", "kind": "case_fail"}, {"cpu_model": "68020"}, None)
        self.assertTrue(any(x["file"] == "src/blitter.cpp" for x in p))
        p = codemap.pointers({"mod": "fpu", "kind": "case_fail"}, {"cpu_model": "68040", "fpu_model": "68040",
                                                                   "fpu_softfloat": "true"}, None)
        self.assertTrue(any(x["file"] == "src/fpp_softfloat.cpp" for x in p))
        self.assertFalse(any(x["file"] == "src/fpp_native.cpp" for x in p))


class Problems(unittest.TestCase):
    def test_grouping_by_signature(self):
        nar = {"groups": [{"entry": "e-jit", "cases": {
            "cpu.addx": {"flips": [{"axis": "comp_nf"}], "holds": [], "determinism": "deterministic",
                         "signature": ["comp_nf=false"], "statement": "fails only with comp_nf=true"},
            "cpu.subx": {"flips": [{"axis": "comp_nf"}], "holds": [], "determinism": "deterministic",
                         "signature": ["comp_nf=false"], "statement": "fails only with comp_nf=true"},
            "cpu.mulu": {"flips": [], "holds": [{"axis": "jit"}], "determinism": "deterministic",
                         "signature": [], "statement": "fails with every neighbouring setting tried"}}}]}
        mk = lambda i, c: {"id": f"F{i}", "kind": "case_fail", "entry": "e-jit", "mod": "cpu", "case": c,  # noqa
                           "case_key": f"cpu.{c}"}
        items = [mk(1, "addx"), mk(2, "subx"), mk(3, "mulu"), dict(mk(4, "x"), xfail={"ref": "r"}),
                 {"id": "F5", "kind": "crash", "entry": "e2", "why": "boom"}]
        probs = problems.group(items, [{"id": "e-jit", "status": "fail"}, {"id": "e2", "status": "crash"}], nar)
        by = {tuple(p["case_keys"]): p for p in probs}
        self.assertIn(("cpu.addx", "cpu.subx"), by)
        self.assertIn(("cpu.mulu",), by)
        self.assertNotIn(("cpu.x",), by)          # xfail is not a problem
        self.assertEqual(probs[0]["kind"], "crash")
        self.assertIn("2 cpu cases fail", by[("cpu.addx", "cpu.subx")]["title"])

    def test_case_doc_lookup(self):
        d = tempfile.mkdtemp(dir=os.path.abspath(WORK_TMP) if os.path.isdir(WORK_TMP) else None)
        try:
            os.makedirs(os.path.join(d, "docs", "cases"))
            with open(os.path.join(d, "docs", "cases", "zz.md"), "w") as fh:
                fh.write("# zz\n\n| case | intent | rule |\n|---|---|---|\n"
                         "| `add_v` | ADD sets V | PRM 4-4 |\n- `addx_z`: Z only cleared (PRM ADDX)\n")
            problems._doc_cache.clear()
            self.assertIn("PRM 4-4", problems.case_doc("zz", "add_v", repo=d)["text"])
            self.assertIn("Z only cleared", problems.case_doc("zz", "addx_z", repo=d)["text"])
            self.assertIsNone(problems.case_doc("zz", "nope", repo=d)["text"])
            self.assertIsNone(problems.case_doc("none", "x", repo=d))
        finally:
            problems._doc_cache.clear()
            shutil.rmtree(d)


class ReportRender(unittest.TestCase):
    def test_coverage_first_and_problem_sections(self):
        ref = fake_result("m", [{"case": "add", "st": "pass", "ctx": {"sr": "2000"}}])
        twin = fake_result("m-jit", [{"case": "add", "st": "pass", "ctx": {"sr": "2004"}}], twin_of="m",
                           twin_kind="jit", options={"cachesize": "8192"})
        failing = fake_result("x", [{"case": "divu", "st": "fail", "expect": {"sr": "0002"}, "got": {"sr": "0000"},
                                     "why": "V not set", "in": {"d0": "1"}, "ctx": {"op": "80c1"}}], status="fail",
                              options={"cpu_model": "68020", "cachesize": "0"})
        failing["adjusted"] = [{"option": "cpu_threaded", "wrote": "true", "resolved": "false", "because": ["T"]}]
        skipped = {"id": "s", "status": "skip", "reason": "no ROM", "options": {}, "machine": {}, "title": "s"}
        d = tempfile.mkdtemp(dir=os.path.abspath(WORK_TMP) if os.path.isdir(WORK_TMP) else None)
        try:
            summ = report.write_all(d, {"run_id": "t"}, [ref, twin, failing, skipped])
            md = open(os.path.join(d, "REPORT.md")).read()
            self.assertLess(md.index("## Coverage"), md.index("## Problems"))
            self.assertIn("config adjusted by emulator: `cpu_threaded` true -> false", md)
            self.assertIn("engine_mismatch", md)
            self.assertIn("`cpu.divu`", md)
            self.assertIn('expect: `{"sr":"0002"}`', md)
            self.assertIn("op_80c1_3_ff", md)   # handler named from ctx.op (no source: guessed name)
            self.assertTrue(summ["verdict"].startswith("PROBLEMS"))
            mods = summ["modules"]["cpu"]
            self.assertEqual((mods["fail"], mods["engine_mismatch"]), (1, 1))
            json.dumps(summ)
        finally:
            shutil.rmtree(d)


if __name__ == "__main__":
    unittest.main()
