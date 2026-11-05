"""Conservative warp selection and execution evidence; no emulator launches."""
import copy
import json
from pathlib import Path
import re
import tempfile
import threading
import unittest
from unittest import mock

from amitorture import amiberry_cfg, analysis, baseline, cli, matrix, parity, problems, regress, report, runner
from tests.test_runner_evidence import result

ROOT = Path(__file__).resolve().parents[2]


class Warp(unittest.TestCase):
    def setUp(self):
        (ROOT / "work" / "tmp").mkdir(parents=True, exist_ok=True)
        tmp = tempfile.TemporaryDirectory(dir=ROOT / "work" / "tmp")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)
        self.entry = matrix.find("aros-aga-68020-fast")

    def opts(self, **kw):
        return runner.Options(binary="amiberry", kickstarts=str(self.root), workdir=str(self.root), **kw)

    def info(self, group):
        return {"build_id": f"rev 2026-11-05 2/7 {group}"}

    def test_auto_allowlist_and_conservative_fallback(self):
        for group in ("cpu", "fpu", "mmu", "core", "chip", "exp", "cpux", "unknown"):
            with self.subTest(group=group):
                w = runner.disk_warp(self.opts(warp="auto"), self.entry, self.info(group))
                self.assertEqual(w["effective"], group in ("cpu", "fpu", "mmu"))
                self.assertEqual((w["requested"], w["group"]), ("auto", group))
        for info in (None, {}, {"build_id": "cpu"}, {"build_id": "rev mixed cpu"}):
            self.assertFalse(runner.disk_warp(self.opts(warp="auto"), self.entry, info)["effective"])

    def test_auto_keeps_jit_entries_normally_paced(self):
        jit = matrix.find("a4000-68040nommu-fast-jit")
        self.assertNotEqual(str(jit["options"].get("cachesize", "0")), "0")
        for group in ("cpu", "fpu", "mmu"):
            with self.subTest(group=group):
                w = runner.disk_warp(self.opts(warp="auto"), jit, self.info(group))
                self.assertFalse(w["effective"])
                self.assertIn("JIT", w["reason"])
        # an explicit --warp on still forces it
        self.assertTrue(runner.disk_warp(self.opts(warp="on"), jit, self.info("cpu"))["effective"])

    def test_allowlisted_disk_groups_contain_only_reviewed_functional_modules(self):
        makefile = (ROOT / "disk" / "Makefile").read_text()
        for group, module in (("cpu", "cpu_basic"), ("fpu", "fpu"), ("mmu", "mmu")):
            found = re.search(r"^DISK_SET_" + group + r"\s*:=\s*(.*)$", makefile, re.M)
            self.assertIsNotNone(found)
            self.assertEqual(found[1].split(), [module],
                             "review warp eligibility before adding modules to accelerated disks")

    def test_capture_and_selftest_never_warp_automatically(self):
        for kw, capture in (({}, True), ({"selftest": True}, False)):
            self.assertFalse(runner.disk_warp(
                self.opts(warp="auto", **kw), self.entry, self.info("cpu"), capture)["effective"])

    def test_forced_modes_default_and_legacy_config(self):
        for mode, expected in ((None, False), ("off", False), ("on", True)):
            self.assertEqual(runner.disk_warp(
                self.opts(warp=mode), self.entry, None)["effective"], expected)
        for value in ("true", "yes", "1", "false", "no", "0"):
            entry = dict(self.entry, options=dict(self.entry["options"], warp=value))
            w = runner.disk_warp(self.opts(), entry, self.info("chip"))
            self.assertEqual(w["effective"], value in ("true", "yes", "1"))
            self.assertEqual(w["requested"], "config")
            self.assertFalse(runner.disk_warp(self.opts(warp="off"), entry, None)["effective"])
            self.assertFalse(runner.disk_warp(self.opts(warp="auto"), entry, self.info("chip"))["effective"])
        with self.assertRaisesRegex(runner.RunnerError, "invalid warp"):
            runner.disk_warp(self.opts(), dict(self.entry, options={"warp": "garbage"}), None)

    def test_conflicting_controls_rejected(self):
        for mode in ("on", "off", "auto"):
            with self.assertRaisesRegex(runner.RunnerError, "not both"):
                self.opts(warp=mode, sets={"warp": "true"})
        with self.assertRaisesRegex(runner.RunnerError, "unknown warp"):
            self.opts(warp="invalid")
        with self.assertRaisesRegex(runner.RunnerError, "extra options"):
            runner.disk_warp(self.opts(warp="auto"), self.entry, self.info("cpu"), extra={"warp": "true"})

    def test_generated_config_uses_actual_disk_header_not_filename(self):
        for group, name, mode, enabled in (("chip", "2-cpu", "auto", False),
                                            ("cpu", "renamed", "auto", True),
                                            ("cpu", "2-cpu", "off", False)):
            with self.subTest(group=group, mode=mode):
                ctx = {"tmp": str(self.root / "tmp"), "media": str(self.root / "media"),
                       "results": str(self.root / "results"), "seq": [0], "lock": threading.Lock()}
                (self.root / "results" / "configs").mkdir(parents=True, exist_ok=True)
                with mock.patch.object(runner.adfmod, "bake", return_value=self.info(group)), \
                     mock.patch.object(runner, "_media_options", return_value=({}, None)), \
                     mock.patch.object(runner, "audio_wanted", return_value=False), \
                     mock.patch.object(runner.emulator, "pick_port", return_value=24001), \
                     mock.patch.object(runner.emulator, "release_port"), \
                     mock.patch.object(amiberry_cfg, "verify", return_value={
                         "ok": False, "error": "stop before launch", "resolved_text": ""}):
                    r = runner._run_disk(self.opts(warp=mode), ctx, self.entry, disk=(name, "source.adf"))
                self.assertEqual(r["status"], "error")
                self.assertEqual(r["warp"]["effective"], enabled)
                cfg = self.root / "results" / r["config"]["file"]
                self.assertEqual(amiberry_cfg.parse(cfg.read_text())["warp"], str(enabled).lower())

    def test_runtime_state_must_match_and_be_supported(self):
        for expected in (True, False):
            w = {"effective": expected}
            ipc = mock.Mock()
            ipc.request.return_value = f"OK\t{int(expected)}"
            runner.verify_warp(ipc, w)
            ipc.request.assert_called_once_with("GET_WARP")
            self.assertTrue(w["verified"])
            for bad in (f"OK\t{int(not expected)}", "ERROR", "OK", "OK\tgarbage"):
                ipc.request.return_value = bad
                with self.assertRaisesRegex(runner.RunnerError, "verification failed"):
                    runner.verify_warp(ipc, w)
                self.assertFalse(w["verified"])

    def test_only_warped_launches_increase_transport_poll_frequency(self):
        for warp in (True, False):
            run = runner.EntryRun(self.opts(), self.entry, str(self.root), warp=warp)
            try:
                self.assertEqual(run.POLL, run.POLL_BUSY if warp else 0.25)
            finally:
                run.close()

    def test_dump_default_and_cli_modes(self):
        self.assertEqual(amiberry_cfg.COMMON["warp"], "false")
        self.assertEqual(amiberry_cfg.DWRITE_DEFAULTS["warp"], "false")
        parser = cli.build_parser()
        for command in ("run", "baseline", "check"):
            args = ["baseline", "--name", "test"] if command == "baseline" else [command]
            self.assertIsNone(parser.parse_args(args).warp)
            for mode in ("auto", "on", "off"):
                self.assertEqual(parser.parse_args(args + ["--warp", mode]).warp, mode)

    def pair(self):
        ref = result("m", options={"cachesize": "0"})
        twin = result("m-jit", options={"cachesize": "8192"}, twin_of="m", twin_kind="jit")
        for r in (ref, twin):
            r["disks"] = [{"disk_name": "2-cpu", "warp": {"effective": True}},
                          {"disk_name": "4-chip", "warp": {"effective": False}}]
        return ref, twin

    def test_parity_requires_matching_per_disk_pacing(self):
        ref, twin = self.pair()
        self.assertEqual(parity.compare(ref, twin)[1]["status"], "compared")
        twin = copy.deepcopy(twin)
        twin["disks"][0]["warp"]["effective"] = False
        self.assertIn("warp modes", parity.compare(ref, twin)[1]["why"])

    def test_per_disk_evidence_survives_combination_report_and_baseline(self):
        subs = []
        for name, enabled in (("2-cpu", True), ("4-chip", False)):
            subs.append(result(self.entry["id"], disk_name=name, warp={
                "requested": "auto", "effective": enabled, "group": name.split("-")[1],
                "reason": "policy", "verified": True, "reply": f"OK\t{int(enabled)}"}))
        r = runner.combine(self.entry, subs)
        self.assertEqual([s["effective"] for s in r["warp"]["disks"]], [True, False])
        self.assertEqual(baseline.entry_meta(r, {})["warp"], r["warp"])
        summary = report.write_all(str(self.root), {}, [r])
        self.assertEqual(summary["entries"][0]["warp"], r["warp"])
        self.assertIn("## Warp policy", (self.root / "REPORT.md").read_text())
        raw = json.loads((self.root / r["raw"] / "result.json").read_text())
        self.assertEqual(raw["warp"], r["warp"])

    def test_runtime_mode_mismatch_is_not_reported_as_running_as_designed(self):
        r = result("m", status="error", config={"dump_ok": True}, warp={
            "requested": "auto", "effective": True, "group": "cpu",
            "reason": "functional set disk", "verified": False, "reply": "OK\t0"})
        summary = report.write_all(str(self.root), {}, [r])
        self.assertFalse(summary["entries"][0]["as_designed"])

    def test_check_inherits_baseline_policy_but_allows_deliberate_override(self):
        base = {"entries": {}, "meta": {"warp": "auto", "binary": {}, "disk": {}}}
        for requested, sets, expected in ((None, {}, "auto"), ("off", {}, "off"),
                                          (None, {"warp": "false"}, None)):
            opts = self.opts(warp=requested, sets=sets)
            with mock.patch.object(baseline, "load", return_value=base), \
                 mock.patch.object(regress, "_collect", return_value=({}, [{"amiberry": {}}])) as collect:
                _, doc = regress.cmd_check(opts, "test", ["amiberry"], 1)
            self.assertEqual(collect.call_args.args[0].warp, expected)
            self.assertEqual(doc["warp"], expected)
            self.assertEqual(opts.warp, requested)

    def test_repro_pins_the_original_modules_effective_mode(self):
        source = copy.deepcopy(self.entry)
        r = {"disks": [{"modules": {"cpu_basic": 2}, "warp": {"effective": True, "requested": "auto"}},
                       {"modules": {"input": 17}, "warp": {"effective": False, "requested": "auto"}}]}
        for mod, enabled in (("cpu_basic", True), ("input", False)):
            with self.subTest(mod=mod):
                w = analysis.module_warp(r, mod)
                self.assertEqual(w["effective"], enabled)
                problems.write_pack(str(self.root), {"id": mod, "title": "test"},
                                    {"kind": "case_fail", "mod": mod}, self.entry,
                                    rom=None, ext_rom=None, adf_src="source", params={}, module_id=None,
                                    binary="amiberry", kickstarts=str(self.root), amiberry_src=None,
                                    pointers=[], bake=mock.Mock(), warp=w)
                packdir = self.root / "repro" / mod
                cfg = amiberry_cfg.parse((packdir / "config.uae").read_text())
                pack = json.loads((packdir / "pack.json").read_text())
                self.assertEqual(pack["warp"], w)
                self.assertEqual(cfg["warp"], str(enabled).lower())
                entry = problems.entry_from_pack(pack)
                self.assertEqual(runner.disk_warp(self.opts(), entry, None)["effective"], enabled)
        self.assertEqual(self.entry, source)
        self.assertIsNone(analysis.module_warp(r, "unknown"))


if __name__ == "__main__":
    unittest.main()
