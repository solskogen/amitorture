"""Execution evidence, transport epochs and report round trips; no emulators."""
import contextlib
import copy
import io
import json
from pathlib import Path
import subprocess
import sys
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest import mock

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "runner"))

from amitorture import amiberry_cfg, analysis, cases, catalogue, cli, emulator, matrix, parity, report, records, runner


def trace(value=1, build="b core", mod="cpu_basic", mid=2):
    rows = [{"t": "hello", "build": build, "mode": 0, "seed": 1, "exec": "40.68", "cpu": "68020",
             "fpu": "none", "mmu": 0, "probe": {}, "modules": {mod: mid}},
            {"t": "mod", "mod": mod, "id": mid, "ev": "begin"},
            {"t": "case", "mod": mod, "case": "add", "st": "pass", "ctx": {"value": value}},
            {"t": "mod", "mod": mod, "id": mid, "ev": "end", "pass": 1, "fail": 0, "skip": 0},
            {"t": "done", "pass": 1, "fail": 0, "skip": 0, "untestable": 0, "aborted": 0, "records": 5}]
    return [dict(r, v=1, seq=n) for n, r in enumerate(rows)]


def wire(rows):
    return b"".join(records.encode(r) for r in rows)


def merged(rows):
    return records.merge({"serial": records.parse_bytes(wire(rows), "serial").records})


def result(eid, rows=None, options=None, **extra):
    m = merged(rows if rows is not None else trace())
    status, reason = runner.classify({}, m, "done")
    return {"id": eid, "title": eid, "status": status, "reason": reason, "ended": "done",
            "options": options or {"cachesize": "0"}, "_merged": m, "records": records.summarize(m),
            "machine": {}, "machine_from_probe": True, **extra}


class Evidence(unittest.TestCase):
    def setUp(self):
        (ROOT / "work" / "tmp").mkdir(parents=True, exist_ok=True)
        tmp = tempfile.TemporaryDirectory(dir=ROOT / "work" / "tmp")
        self.addCleanup(tmp.cleanup)
        self.root = Path(tmp.name)

    def pair(self):
        ref = result("m")
        twin = result("m-jit", options={"cachesize": "8192"}, twin_of="m", twin_kind="jit")
        return ref, twin

    def test_guest_xfail_cannot_hide_passing_case_parity_difference(self):
        ref, twin = self.pair()
        twin = result(twin["id"], trace(2), options=twin["options"], twin_of="m", twin_kind="jit")
        registry = {"cpu_basic.add": {"ref": "known guest failure", "why": "not a parity exception"}}
        fps = {r["id"]: cases.extract(r) for r in (ref, twin)}
        items, _ = analysis.failure_items([ref, twin], fps, registry)
        mismatches = [i for i in items if i["kind"] == "engine_mismatch"]
        self.assertEqual(len(mismatches), 1)
        self.assertNotIn("xfail", mismatches[0])
        with mock.patch.object(analysis.xfailmod, "load", return_value=registry):
            summary = report.write_all(self.root, {}, [ref, twin])
        self.assertTrue(summary["verdict"].startswith("PROBLEMS"))

    def test_overridden_engine_pairs_are_not_compared(self):
        for kind, axis, enabled in (("jit", "cachesize", "8192"),
                                    ("threaded", "cpu_threaded", "true"),
                                    ("softfloat", "fpu_softfloat", "true")):
            for same in ("0" if kind == "jit" else "false", enabled):
                with self.subTest(kind=kind, same=same):
                    ref, twin = self.pair()
                    twin["twin_kind"] = kind
                    ref["options"] = twin["options"] = {axis: same}
                    items, info = parity.compare(ref, twin)
                    self.assertEqual(items, [])
                    self.assertEqual(info["status"], "not compared")
                    self.assertEqual(info["compared"], 0)
                    self.assertIn("requires", info["why"])

    def test_valid_engine_pairs_and_unrelated_machine_changes(self):
        for kind, axis, enabled in (("jit", "cachesize", "8192"),
                                    ("threaded", "cpu_threaded", "true"),
                                    ("softfloat", "fpu_softfloat", "true")):
            with self.subTest(kind=kind):
                ref, twin = self.pair()
                twin.update(twin_kind=kind, options={"cachesize": "0", axis: enabled})
                self.assertEqual(parity.compare(ref, twin)[1]["status"], "compared")
                twin["options"]["chipmem_size"] = "99"
                self.assertIn("outside", parity.compare(ref, twin)[1]["why"])

    def test_resolved_engine_identity_overrides_requested_identity(self):
        ref, twin = self.pair()
        twin["resolved_options"] = {"cachesize": "0"}
        self.assertEqual(parity.compare(ref, twin)[1]["status"], "not compared")
        twin.pop("resolved_options")
        twin["disks"] = [{"config": {"dump_ok": False}}]
        self.assertIn("verification failed", parity.compare(ref, twin)[1]["why"])

    def test_every_shipped_twin_is_a_valid_engine_pair(self):
        entries = matrix.all_entries()
        for e in entries.values():
            if not e.get("twin_of"):
                continue
            ref = dict(entries[e["twin_of"]], status="pass")
            twin = dict(e, status="pass")
            self.assertIsNone(parity._pair_problem(ref, twin, e["twin_kind"]), e["id"])

    def test_catalogue_requires_matching_completed_case_comparisons(self):
        item = next(i for i in catalogue.load() if i["id"] == "cpu.jit_parity")
        ref, twin = self.pair()
        fps = {r["id"]: cases.extract(r) for r in (ref, twin)}
        self.assertEqual(catalogue.coverage([item], [twin], fps)[0]["status"], "not_covered")
        self.assertEqual(catalogue.coverage([item], [ref, twin], fps)[0]["status"], "covered")
        twin["options"]["cachesize"] = "0"
        self.assertEqual(catalogue.coverage([item], [ref, twin], fps)[0]["status"], "not_covered")
        twin["options"]["cachesize"] = "8192"
        del fps["m"]["cases"]["cpu_basic.add"]
        self.assertEqual(catalogue.coverage([item], [ref, twin], fps)[0]["status"], "not_covered")
        with self.assertRaises(ValueError):
            catalogue.parse("- id: invalid\n  cases: [cpu.*]\n  parity: unknown\n")

    def test_unfinished_and_empty_execution_are_incomplete(self):
        for rows in ([trace()[0], trace()[1], trace()[-1]], [trace()[0], trace()[-1]],
                     [trace()[0], trace()[1], trace()[3], trace()[-1]]):
            with self.subTest(types=[r["t"] for r in rows]):
                rows = [dict(r, seq=i) for i, r in enumerate(rows)]
                rows[-1]["records"] = len(rows)
                m = merged(rows)
                status, why = runner.classify({}, m, "done")
                self.assertEqual(status, "incomplete", why)
                r = result("legacy", rows)
                r["status"] = "pass"
                self.assertTrue(any("module lifecycle" in i["why"] for i in report.collect_failures([r])))

    def test_module_manifest_requires_every_declared_module_and_correct_ids(self):
        for edit in ("missing", "id"):
            rows = trace()
            if edit == "missing":
                rows[0]["modules"]["flags"] = 3
            else:
                rows[1]["id"] = 3
            status, why = runner.classify({}, merged(rows), "done")
            self.assertEqual(status, "incomplete")
            self.assertIn("module", why)

    def test_manifest_validation_rejects_bad_types_and_duplicate_ids(self):
        for mods in ([], {"a": True}, {"a": 64}, {"a": -1}, {"a": 1, "b": 1}):
            self.assertTrue(records.validate(dict(trace()[0], modules=mods)), mods)

    def test_completed_legacy_speed_and_zero_assertion_os_modules(self):
        rows = trace()
        del rows[0]["modules"]
        self.assertEqual(runner.classify({}, merged(rows), "done")[0], "pass")
        rows = trace(mod="hostspeed", mid=26)
        rows[2] = dict(rows[2], t="speed", ev="skip", insn="FSIN")
        rows[3]["pass"] = rows[4]["pass"] = 0
        self.assertEqual(runner.classify({}, merged(rows), "done")[0], "pass")
        rows = trace()
        rows[1:1] = [{"v": 1, "t": "mod", "mod": "memory_os", "id": 29, "ev": "begin"},
                     {"v": 1, "t": "mod", "mod": "memory_os", "id": 29, "ev": "end", "pass": 0, "fail": 0, "skip": 0}]
        rows[0]["modules"]["memory_os"] = 29
        rows = [dict(r, seq=i) for i, r in enumerate(rows)]
        rows[-1]["records"] = len(rows)
        self.assertEqual(runner.classify({}, merged(rows), "done")[0], "pass")

    def test_selftest_is_not_mistaken_for_guest_module_execution(self):
        selftest = [trace()[0], trace()[2], trace()[-1]]
        selftest = [dict(r, seq=i) for i, r in enumerate(selftest)]
        selftest[-1]["records"] = len(selftest)
        self.assertEqual(runner.classify({}, merged(selftest), "selftest")[0], "pass")

    def test_explicitly_empty_selection_skips_instead_of_passing(self):
        rows = [trace()[0], trace()[-1]]
        rows[0]["modules"] = {}
        rows[1].update(seq=1, records=2, **{"pass": 0})
        self.assertEqual(runner.classify({}, merged(rows), "done")[0], "skip")
        rows[1]["pass"] = 1
        self.assertEqual(runner.classify({}, merged(rows), "done")[0], "incomplete")

    def test_empty_disk_selection_does_not_skip_the_rest_of_the_set(self):
        entry = matrix.find("a4000-68040nommu-fast")
        first = result(entry["id"], status="skip", disk_name="1-core", process={})
        second = result(entry["id"], disk_name="2-cpu", process={})
        with mock.patch.object(runner, "_run_disk", side_effect=[first, second]) as launch:
            r = runner.run_entry(None, {"disks": [("1-core", "a.adf"), ("2-cpu", "b.adf")]}, entry)
        self.assertEqual(launch.call_count, 2)
        self.assertEqual(r["status"], "pass")

    def test_differing_boot_histories_align_unique_complete_boot_without_mutation(self):
        current = trace()
        earlier = [current[0], {"v": 1, "seq": 1, "t": "log", "msg": "reset before tests"}]
        serial = records.parse_bytes(wire(earlier + current), "serial").records
        mailbox = records.parse_bytes(wire(current), "mailbox").records
        snapshot = copy.deepcopy((serial, mailbox))
        for _ in range(2):
            m = records.merge({"serial": serial, "mailbox": mailbox})
            self.assertEqual(m.conflicts, [])
            self.assertEqual(m.schema_problems, [])
            self.assertEqual(len(m.records), len(earlier) + len(current))
            self.assertTrue(all(x["sources"] == ["serial", "mailbox"] for x in m.records if x["boot"] == 1))
            self.assertEqual(runner.classify({}, m, "done")[0], "pass")
        self.assertEqual((serial, mailbox), snapshot)

    def test_ambiguous_boot_epochs_cannot_create_a_complete_run(self):
        rows = trace()
        serial = records.parse_bytes(wire(rows + rows), "serial").records
        mailbox = records.parse_bytes(wire(rows), "mailbox").records
        m = records.merge({"serial": serial, "mailbox": mailbox})
        self.assertTrue(any("cannot reconcile" in p["problems"][0] for p in m.schema_problems))
        self.assertEqual(runner.classify({}, m, "done")[0], "incomplete")

    def test_unique_boot_header_keeps_real_content_conflicts(self):
        first, current = trace(build="first"), trace(build="current")
        other = trace(2, build="current")
        m = records.merge({"serial": records.parse_bytes(wire(first + current), "serial").records,
                           "mailbox": records.parse_bytes(wire(other), "mailbox").records})
        self.assertEqual(len(m.conflicts), 1)
        self.assertEqual(m.conflicts[0]["boot"], 1)
        self.assertEqual(runner.classify({}, m, "done")[0], "incomplete")

    def test_partial_epochs_cannot_be_right_aligned_to_fill_gaps(self):
        serial = records.parse_bytes(wire(trace(0) + trace()[:3]), "serial").records
        mailbox = records.parse_bytes(wire(trace()[2:]), "mailbox").records
        m = records.merge({"serial": serial, "mailbox": mailbox})
        self.assertTrue(m.schema_problems)
        self.assertEqual(runner.classify({}, m, "done")[0], "incomplete")

    def test_missed_middle_boot_and_mapped_bad_record_recovery(self):
        serial = records.parse_bytes(wire(trace(1) + trace(2) + trace(3)), "serial").records
        last = trace(3)
        bad = wire([last[2]]).replace(b'"value":3', b'"value":4')
        mailbox = records.parse_bytes(wire(trace(1)) + wire(last[:2]) + bad + wire(last[2:]), "mailbox").records
        m = records.merge({"serial": serial, "mailbox": mailbox})
        self.assertEqual(m.conflicts, [])
        self.assertEqual(m.schema_problems, [])
        self.assertEqual(m.bad[0]["boot"], 2)
        self.assertEqual(records.bad_unrecovered(m), [])
        self.assertEqual(runner.classify({}, m, "done")[0], "pass")

    def test_reversed_epoch_matches_are_not_silently_reordered(self):
        serial = records.parse_bytes(wire(trace(1) + trace(2) + trace(3)), "serial").records
        mailbox = records.parse_bytes(wire(trace(2) + trace(1)), "mailbox").records
        m = records.merge({"serial": serial, "mailbox": mailbox})
        self.assertTrue(m.schema_problems)
        self.assertEqual(runner.classify({}, m, "done")[0], "incomplete")

    def import_report(self, blobs):
        paths = []
        for n, blob in enumerate(blobs):
            p = self.root / f"capture{n}.bin"
            p.write_bytes(blob)
            paths.append(str(p))
        out = self.root / "report"
        return report.import_files(paths, out), out

    def test_multidisk_import_rebuild_preserves_cases_and_integrity(self):
        a = trace(build="b core")
        b = trace(build="b fpu", mod="fpu", mid=9)
        b[2].update(st="fail", expect=1, got=2, why="bad result")
        b[3].update({"pass": 0, "fail": 1})
        b[4].update({"pass": 0, "fail": 1})
        b.insert(3, {"v": 1, "seq": 3, "t": "unrecognized"})
        b = [dict(r, seq=i) for i, r in enumerate(b)]
        b[-1]["records"] = len(b)
        initial, out = self.import_report([wire(a), wire(b)])
        before = (out / "failures.jsonl").read_text()
        saved = analysis.load_results(out)
        self.assertEqual(set(cases.extract(saved[0])["cases"]) - {cases.PROBE_KEY},
                         {"cpu_basic.add", "fpu.add"})
        self.assertTrue(all(s["_merged"] is not None for s in saved[0]["disks"]))
        rebuilt = analysis.rebuild(out, repro=False)
        self.assertEqual(initial["verdict"], rebuilt["verdict"])
        self.assertEqual(initial["entries"][0]["cases"], rebuilt["entries"][0]["cases"])
        after = (out / "failures.jsonl").read_text()
        for text in (before, after):
            self.assertIn('"kind": "schema"', text)
            self.assertIn('"kind": "case_fail"', text)

    def test_single_import_retains_transport_conflicts_and_corruption(self):
        rows = trace()
        corrupt = wire([rows[2]]).replace(b'"value":1', b'"value":9')
        initial, out = self.import_report([wire(rows[:2]) + corrupt + wire(rows[2:]), wire(trace(2))])
        saved = analysis.load_results(out)[0]
        self.assertEqual(len(saved["_merged"].bad), 1)
        self.assertEqual(len(saved["_merged"].conflicts), 1)
        rebuilt = analysis.rebuild(out, repro=False)
        self.assertEqual(initial["verdict"], rebuilt["verdict"])
        self.assertIn('"kind": "records_conflict"', (out / "failures.jsonl").read_text())

    def test_import_aligns_histories_without_mutating_sources(self):
        rows = trace()
        earlier = [rows[0], {"v": 1, "seq": 1, "t": "log", "msg": "reset"}]
        initial, out = self.import_report([wire(earlier + rows), wire(rows)])
        loaded = analysis.load_results(out)[0]
        self.assertEqual(len(loaded["disks"]), 2)
        self.assertEqual(loaded["disks"][1]["status"], "pass")
        self.assertEqual(loaded["disks"][1]["_merged"].conflicts, [])
        self.assertEqual(analysis.rebuild(out, repro=False)["verdict"], initial["verdict"])

    def test_missing_import_disk_artifact_is_an_explicit_error(self):
        _, out = self.import_report([wire(trace()), wire(trace(build="b fpu", mod="fpu", mid=9))])
        r = analysis.load_results(out)[0]
        (out / r["disks"][0]["raw"] / "records.jsonl").unlink()
        with self.assertRaisesRegex(emulator.RunnerError, "saved records missing"):
            analysis.load_results(out)

    def test_live_and_imported_integrity_metadata_is_not_truncated(self):
        rows = trace()
        bad = wire([rows[2]]).replace(b'"value":1', b'"value":2')
        _, out = self.import_report([wire(rows[:2]) + bad * 61 + wire(rows[2:])])
        r = analysis.load_results(out)[0]
        self.assertEqual(len(r["_merged"].bad), 61)
        parsers = SimpleNamespace(serial_parser=records.StreamParser("serial"),
                                  mbox_parser=records.StreamParser("mailbox"))
        self.assertEqual(len(runner.summarize_records(r["_merged"], parsers)["bad"]), 61)
        raw = out / "raw" / "imported" / "result.json"
        saved = json.loads(raw.read_text())
        saved["records"]["bad"] = []
        raw.write_text(json.dumps(saved))
        rebuilt = analysis.rebuild(out, repro=False)
        self.assertTrue(rebuilt["verdict"].startswith("PROBLEMS"))
        self.assertIn("metadata is incomplete", (out / "failures.jsonl").read_text())

    def test_dump_failure_and_empty_dump_are_not_validated(self):
        conf = self.root / "v.uae"
        conf.write_text("cpu_model=68020\n")
        for response in (OSError("cannot launch"), subprocess.TimeoutExpired("amiberry", 1),
                         SimpleNamespace(stdout=b"", stderr=b"", returncode=0),
                         SimpleNamespace(stdout=b"cpu_model=68020\n", stderr=b"", returncode=0),
                         SimpleNamespace(stdout=b"cpu_model=68020\n", stderr=b"failed", returncode=1)):
            with self.subTest(response=response):
                kw = {"side_effect": response} if isinstance(response, Exception) else {"return_value": response}
                with mock.patch.object(emulator, "run_amiberry", **kw):
                    v = amiberry_cfg.verify("amiberry", str(conf), {})
                self.assertFalse(v["ok"])
                self.assertIsNotNone(amiberry_cfg.verification_error(v))

    def test_normal_verification_adjustments_and_unknown_options_remain_distinct(self):
        conf = self.root / "v.uae"
        conf.write_text("cpu_model=68020\ncachesize=8192\n")
        for stderr in (b"", b"; unknown configuration entries\nbogus=value\n"):
            p = SimpleNamespace(stdout=b"cpu_model=68020\ncachesize=0\n", stderr=stderr, returncode=0)
            with mock.patch.object(emulator, "run_amiberry", return_value=p):
                v = amiberry_cfg.verify("amiberry", str(conf), {})
            self.assertEqual(v["ok"], not bool(stderr))
            self.assertEqual(v["differences"][0]["option"], "cachesize")
            self.assertEqual(amiberry_cfg.verification_error(v) is None, not bool(stderr))

    def test_validate_returns_host_error_and_prints_failure(self):
        entry = matrix.find("a4000-68040nommu-fast")
        args = SimpleNamespace(amiberry="amiberry", workdir=str(self.root), kickstarts=None,
                               keep=False, verbose=True, json=True)
        stdout = io.StringIO()
        with mock.patch.object(emulator, "find_binary", return_value="amiberry"), \
                mock.patch.object(amiberry_cfg, "verify", return_value={
                    "ok": False, "error": "dump timed out", "differences": []}), contextlib.redirect_stdout(stdout):
            code = cli.validate(args, [entry])
        self.assertEqual(code, cli.EXIT_ERROR)
        self.assertIn("FAILED", stdout.getvalue())
        self.assertIn("dump timed out", stdout.getvalue())
        self.assertIn("0 entries validated", stdout.getvalue())
        self.assertNotIn(": ok", stdout.getvalue())

    def test_failed_config_verification_prevents_launch_and_preserves_diagnostics(self):
        tmp, out = self.root / "tmp", self.root / "out"
        tmp.mkdir()
        (out / "configs").mkdir(parents=True)
        entry = dict(matrix.find("a4000-68040nommu-fast"), rom={"builtin": ":AROS"})
        opts = SimpleNamespace(binary="amiberry", params={}, no_disk=True, ports=None, keep=False)
        ctx = {"tmp": str(tmp), "results": str(out), "lock": threading.Lock(), "seq": [0]}
        v = {"ok": False, "error": "dump timed out", "returncode": 7, "stderr": "diagnostic", "differences": []}
        with mock.patch.object(runner, "_media_options", return_value=({}, None)), \
                mock.patch.object(runner, "audio_wanted", return_value=False), \
                mock.patch.object(emulator, "pick_port", return_value=12345), \
                mock.patch.object(emulator, "release_port") as release, \
                mock.patch.object(amiberry_cfg, "verify", return_value=v), \
                mock.patch.object(runner, "_launch_once", side_effect=AssertionError("must not launch")):
            r = runner._run_disk(opts, ctx, entry)
        self.assertEqual(r["status"], "error")
        self.assertIn("dump timed out", r["reason"])
        self.assertEqual(r["config"]["returncode"], 7)
        self.assertEqual(r["config"]["stderr"], "diagnostic")
        release.assert_called_once_with(12345)
        self.assertEqual(list(tmp.iterdir()), [])
        summary = report.write_all(out, {}, [r])
        self.assertFalse(summary["entries"][0]["as_designed"])
        self.assertTrue(summary["verdict"].startswith("PROBLEMS"))

    def test_legacy_failed_verification_cannot_rebuild_as_success(self):
        r = result("old", config={"dump_ok": False, "error": "dump timed out"})
        summary = report.write_all(self.root, {}, [r])
        self.assertFalse(summary["entries"][0]["as_designed"])
        self.assertTrue(summary["verdict"].startswith("PROBLEMS"))
        self.assertIn("unverified", (self.root / "REPORT.md").read_text())

    def test_as_designed_requires_every_disk_to_be_verified(self):
        entry = matrix.find("a4000-68040nommu-fast")
        subs = [result(entry["id"], config={"dump_ok": True}, disk_name="1-core"),
                result(entry["id"], config={"dump_ok": False}, disk_name="2-cpu")]
        r = runner.combine(entry, subs)
        summary = report.write_all(self.root, {}, [r])
        self.assertFalse(summary["entries"][0]["as_designed"])
        self.assertTrue(summary["verdict"].startswith("PROBLEMS"))
        subs[1]["config"]["dump_ok"] = True
        r = runner.combine(entry, subs)
        self.assertTrue(report.write_all(self.root, {}, [r])["entries"][0]["as_designed"])


if __name__ == "__main__":
    unittest.main()
