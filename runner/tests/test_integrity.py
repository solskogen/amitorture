"""Audit regression cases using the real runner and packaging code, without emulators."""
import contextlib
import importlib.util
import io
import json
import os
from pathlib import Path
import socket
import struct
import sys
import tempfile
import threading
from types import SimpleNamespace
import unittest
from unittest import mock
import zlib

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "runner"))
sys.path.insert(0, str(ROOT / "tools"))

import amislot
import mkadf
from amitorture import baseline, cases, cli, diagnose, emulator, records, regress

spec = importlib.util.spec_from_file_location("smoke_disk", ROOT / "tests" / "smoke_disk.py")
smoke = importlib.util.module_from_spec(spec)
spec.loader.exec_module(smoke)


class Integrity(unittest.TestCase):
    def setUp(self):
        (ROOT / "work" / "tmp").mkdir(parents=True, exist_ok=True)
        self.tmp = tempfile.TemporaryDirectory(dir=ROOT / "work" / "tmp")
        self.addCleanup(self.tmp.cleanup)
        self.root = Path(self.tmp.name)

    def baseline_files(self):
        bdir = self.root / "baselines" / "test"
        for suffix, value in (("", "old"), (".new", "new")):
            (bdir / ("runs" + suffix)).mkdir(parents=True)
            (bdir / ("runs" + suffix) / "witness").write_text(value)
            for name in ("baseline.json", "BASELINE.md"):
                (bdir / (name + suffix)).write_text(value)
        return bdir

    def assert_old_baseline(self, bdir):
        self.assertEqual((bdir / "runs" / "witness").read_text(), "old")
        for name in ("baseline.json", "BASELINE.md"):
            self.assertEqual((bdir / name).read_text(), "old")

    def test_baseline_publish_rolls_back_each_failed_step(self):
        for exception in (OSError, KeyboardInterrupt):
            for name in ("runs", "baseline.json", "BASELINE.md"):
                for suffix in ("", ".new"):
                    with self.subTest(exception=exception, name=name, suffix=suffix):
                        with tempfile.TemporaryDirectory(dir=self.root) as d:
                            bdir = Path(d)
                            for item in ("runs", "runs.new"):
                                (bdir / item).mkdir()
                                (bdir / item / "witness").write_text("new" if item.endswith(".new") else "old")
                            for item in ("baseline.json", "BASELINE.md"):
                                (bdir / item).write_text("old")
                                (bdir / (item + ".new")).write_text("new")
                            replace = os.replace
                            failed = False

                            def fail_once(src, dst):
                                nonlocal failed
                                if not failed and Path(src).name == name + suffix:
                                    failed = True
                                    raise exception("fixture")
                                return replace(src, dst)

                            with mock.patch.object(regress.os, "replace", side_effect=fail_once):
                                with self.assertRaises(exception):
                                    regress._publish_baseline(str(bdir))
                            self.assertTrue(failed)
                            self.assert_old_baseline(bdir)

    def test_baseline_publish_keeps_previous_artifacts(self):
        bdir = self.baseline_files()
        regress._publish_baseline(str(bdir))
        for name in ("baseline.json", "BASELINE.md"):
            self.assertEqual((bdir / name).read_text(), "new")
            self.assertEqual((bdir / (name + ".previous")).read_text(), "old")
        self.assertEqual((bdir / "runs.previous" / "witness").read_text(), "old")

    def test_incomplete_preparation_does_not_touch_active_baseline(self):
        bdir = self.baseline_files()
        (bdir / "baseline.json.new").unlink()
        with self.assertRaisesRegex(emulator.RunnerError, "missing baseline.json.new"):
            regress._publish_baseline(str(bdir))
        self.assert_old_baseline(bdir)

    def test_nonpositive_repeat_does_not_touch_baseline(self):
        bdir = self.baseline_files()
        opts = SimpleNamespace(workdir=str(self.root))
        for n in (0, -1):
            with self.subTest(repeat=n), self.assertRaises(emulator.RunnerError):
                regress.cmd_baseline(opts, "test", n)
            with self.subTest(check_repeat=n), self.assertRaises(emulator.RunnerError):
                regress.cmd_check(opts, "test", [], n)
            self.assert_old_baseline(bdir)

    def test_baseline_preparation_failure_preserves_active_data(self):
        bdir = self.baseline_files()
        opts = SimpleNamespace(workdir=str(self.root), matrix="smoke", configs=None, sets={}, params={})

        def collect(*args, **kwargs):
            Path(args[2]).mkdir()
            return {}, [{"amiberry": {}}]

        with mock.patch.object(regress.R, "select_entries", return_value=[]), \
                mock.patch.object(regress, "_collect", side_effect=collect), \
                mock.patch.object(regress.B, "build", side_effect=OSError("cannot serialize")):
            with self.assertRaises(OSError):
                regress.cmd_baseline(opts, "test", 1)
        self.assert_old_baseline(bdir)
        self.assertFalse((bdir / "runs.new").exists())

    def test_baseline_command_publishes_all_prepared_artifacts(self):
        bdir = self.baseline_files()
        opts = SimpleNamespace(workdir=str(self.root), matrix="smoke", configs=None, sets={}, params={})
        entry = {"id": "test", "options": {}}

        def collect(*args, **kwargs):
            runs = Path(args[2])
            runs.mkdir()
            (runs / "witness").write_text("new")
            return {"test": {"entry": entry, "statuses": ["pass"], "runs": [{"cases": {}, "perf": {}}],
                             "meta": {}, "results": []}}, [{"amiberry": {}}]

        with mock.patch.object(regress.R, "select_entries", return_value=[entry]), \
                mock.patch.object(regress, "_collect", side_effect=collect):
            path, doc = regress.cmd_baseline(opts, "test", 1)
        self.assertEqual(json.loads(Path(path).read_text()), doc)
        self.assertIn("amitorture baseline", (bdir / "BASELINE.md").read_text())
        self.assertEqual((bdir / "runs" / "witness").read_text(), "new")
        self.assertEqual((bdir / "runs.previous" / "witness").read_text(), "old")
        self.assertEqual((bdir / "baseline.json.previous").read_text(), "old")

    def test_collect_rejects_isolation_failure_after_report(self):
        ctx = mock.Mock()
        ctx.environment = {"isolation": {"ok": False}}
        ctx.run_entries.return_value = []
        opts = SimpleNamespace(binary="unused")
        with mock.patch.object(regress.R, "Context", return_value=ctx), \
                mock.patch.object(regress.R, "log"), \
                mock.patch("amitorture.report.write_all") as write:
            with self.assertRaises(emulator.IsolationError):
                regress._collect(opts, [], str(self.root / "runs"), 1, "check")
        ctx.close.assert_called_once()
        write.assert_called_once()

    def test_cli_isolation_exit(self):
        with mock.patch.object(cli, "_setup_tmp"), mock.patch.object(cli, "_install_interrupt_handler"), \
                mock.patch.object(regress, "rediff", side_effect=emulator.IsolationError("fixture")), \
                contextlib.redirect_stderr(io.StringIO()):
            self.assertEqual(cli.main(["check", "--rediff", str(self.root)]), cli.EXIT_ISOLATION)

    def test_new_unexpected_failure_reaches_cli_exit_status(self):
        entry = {"id": "test", "options": {}}
        passed = {"v": 1, "seq": 0, "t": "case", "mod": "cpu", "case": "a", "st": "pass"}
        failed = dict(passed, seq=1, case="new", st="fail", expect=1, got=0, why="broken")
        before = {"entry": entry, "statuses": ["pass"], "runs": [cases.extract_records([passed])], "meta": {}}
        base = baseline.build({"test": before}, {"binary": {}, "disk": {}})
        baseline.save(str(self.root), "test", base)
        after = dict(before, statuses=["fail"], runs=[cases.extract_records([passed, failed])], results=[])
        output = io.StringIO()
        with mock.patch.object(regress, "_collect", return_value=({"test": after}, [{"amiberry": {}}])), \
                mock.patch.object(emulator, "find_binary", return_value="/unused"), \
                mock.patch.object(regress.xfailmod, "load", return_value={}), \
                mock.patch.object(cli, "_setup_tmp"), mock.patch.object(cli, "_install_interrupt_handler"), \
                mock.patch("subprocess.Popen", side_effect=AssertionError("must not launch an emulator")), \
                contextlib.redirect_stdout(output):
            result = cli.main(["check", "--amiberry", "/unused", "--baseline", "test",
                               "--workdir", str(self.root), "--kickstarts", str(self.root)])
        self.assertEqual(result, cli.EXIT_PROBLEMS)
        self.assertTrue(output.getvalue().startswith("REGRESSIONS"))
        reports = list((self.root / "results").glob("*/REPORT.md"))
        self.assertEqual(len(reports), 1)
        self.assertIn("broken", reports[0].read_text())

    def test_rediff_preserves_selected_scope_when_results_disappear(self):
        entry = lambda name: {"entry": {"id": name, "options": {}}, "statuses": ["pass"],
                              "runs": [{"cases": {}, "perf": {}}], "meta": {}}
        base = baseline.build({name: entry(name) for name in ("selected", "other")}, {"binary": {}, "disk": {}})
        baseline.save(str(self.root), "test", base)
        cdir = self.root / "check"
        cdir.mkdir()
        for legacy in (False, True):
            with self.subTest(legacy=legacy):
                doc = {"baseline": "test", "baseline_meta": base["meta"], "repeat": 1, "sets": {},
                       "binaries": [{"label": "binary"}],
                       "per_binary": [{"entries": {"selected": {}}, "findings": []}]}
                if not legacy:
                    doc["entry_ids"] = ["selected"]
                (cdir / "check.json").write_text(json.dumps(doc))
                d = regress.rediff(str(cdir), str(self.root))
                self.assertEqual(d["binaries"][0]["regressions"], 1)
                self.assertEqual([(f["kind"], f["entry"]) for f in d["per_binary"][0]["findings"]],
                                 [("missing_entry", "selected")])
                self.assertTrue(regress.check_verdict(d).startswith("REGRESSIONS"))
        raw = cdir / "bin1" / "run1"
        raw.mkdir(parents=True)
        (raw / "environment.json").write_text(json.dumps({"isolation": {"ok": False}}))
        with self.assertRaises(emulator.IsolationError):
            regress.rediff(str(cdir), str(self.root))

    def test_partial_control_is_not_a_passing_control(self):
        p = {"case_keys": ["cpu.a", "cpu.b"], "entries": ["failed"]}
        ctx = SimpleNamespace(results=[{"id": "control", "status": "pass"}],
                              fps={"control": {"cases": {"cpu.a": {"st": "pass"}}}},
                              options=lambda eid: {})
        result = diagnose.passes_diff(p, ctx)
        self.assertEqual(result["passes_on"], [])
        self.assertEqual(result["not_run_or_skipped_on"], ["control"])

    def test_ipc_timeout_closes_stream_and_clears_partial_replies(self):
        class FakeSocket:
            closed = False
            chunks = iter((b"OK\n", TimeoutError("late reply")))

            def settimeout(self, timeout):
                pass

            def sendall(self, data):
                if self.closed:
                    raise OSError("closed")

            def recv(self, size):
                value = next(self.chunks)
                if isinstance(value, Exception):
                    raise value
                return value

            def close(self):
                self.closed = True

        ipc = emulator.IPC.__new__(emulator.IPC)
        ipc.sock, ipc.buf, ipc.requests, ipc.lock = FakeSocket(), b"", 0, threading.Lock()
        with self.assertRaises(TimeoutError):
            ipc.pipeline(["SEND_KEY", "WAIT"])
        self.assertTrue(ipc.sock.closed)
        self.assertEqual(ipc.buf, b"")
        with self.assertRaises(OSError):
            ipc.request("WRITE_MEM")
        self.assertEqual(ipc.requests, 2)

    def test_ipc_complete_error_reply_does_not_close_stream(self):
        sock = mock.Mock()
        sock.recv.return_value = b"ERROR\tunsupported\n"
        ipc = emulator.IPC.__new__(emulator.IPC)
        ipc.sock, ipc.buf, ipc.requests, ipc.lock = sock, b"", 0, threading.Lock()
        self.assertEqual(ipc.request("UNKNOWN"), "ERROR\tunsupported")
        sock.close.assert_not_called()

    def test_stale_unlocked_registration_cannot_hide_orphan(self):
        (self.root / "emu-slot-0").write_text("holder 99999999 vanished\nemu 424242\n")
        with mock.patch.object(amislot, "lock_dir", return_value=str(self.root)), \
                mock.patch.object(amislot, "max_slots", return_value=1), \
                mock.patch.object(amislot, "_amiberry_procs", return_value={424242: 1}):
            with self.assertRaisesRegex(RuntimeError, "cancelled"):
                with amislot.emulator_slot(quiet=True, cancel=lambda: True, cancel_exc=RuntimeError):
                    self.fail("admitted an emulator above the limit")

    def test_available_slot_can_be_acquired(self):
        with mock.patch.object(amislot, "lock_dir", return_value=str(self.root)), \
                mock.patch.object(amislot, "max_slots", return_value=1), \
                mock.patch.object(amislot, "_amiberry_procs", return_value={}):
            with amislot.emulator_slot(quiet=True) as slot:
                self.assertEqual(slot, 0)

    def test_duplicate_named_flags_are_idempotent(self):
        p = mkadf.default_params()
        for value in ("unsafe,unsafe", "unsafe+unsafe", "1"):
            mkadf.apply_param(p, "flags=" + value)
            self.assertEqual(p["flags"], 1)
        mkadf.apply_param(p, "flags=unsafe,net,unsafe,noscreen")
        self.assertEqual(p["flags"], 1 | 8 | 16)
        with self.assertRaises(mkadf.Fail):
            mkadf.apply_param(p, "flags=unsafe,unknown")

    def adf(self, image=4, nrel=0):
        """A v2 ADF: `image` code bytes in segment 0, no data, `nrel` relocs."""
        adf = bytearray(mkadf.ADF_SIZE)
        adf[:1024] = mkadf.make_bootblock(b"\x4e\x75")
        csec = (image + 511) // 512
        dsec = (4 * nrel + 511) // 512
        blob = bytes(adf[mkadf.BLOB_OFF:mkadf.BLOB_OFF + csec * 512 + 4 * nrel])
        struct.pack_into(mkadf.HDR_FMT, adf, mkadf.HDR_OFF, b"AMTP", 2, image, 0, nrel, 0, csec + dsec,
                         zlib.crc32(blob) & 0xffffffff, csec * 512, mkadf.MEMF_ANY_CLEAR, 0, dsec,
                         max(dsec * 512, 4))
        path = self.root / "test.adf"
        path.write_bytes(adf)
        return path

    def test_adf_dump_rejects_truncated_blob_even_with_matching_crc(self):
        for image, nrel in ((2 * 1024 * 1024, 0), (4, 1 << 28)):
            with self.subTest(image=image, nrel=nrel):
                path = self.adf(image=image, nrel=nrel)
                errors = io.StringIO()
                with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(errors):
                    self.assertEqual(mkadf.dump(path), 1)
                self.assertIn("beyond the ADF", errors.getvalue())

    def test_two_segment_layout_and_relocs(self):
        # CODE (16 bytes: a pointer to data+4 at 4), DATA (8 bytes: a pointer
        # to code+8 at 0), BSS (32 bytes)
        H = struct.pack
        exe = H(">IIIII", mkadf.HUNK_HEADER, 0, 3, 0, 2) + H(">III", 4, 2, 8)
        exe += H(">II", mkadf.HUNK_CODE, 4) + H(">IIII", 0, 4, 0, 0)
        exe += H(">IIIII", mkadf.HUNK_RELOC32, 1, 1, 4, 0) + H(">I", mkadf.HUNK_END)
        exe += H(">II", mkadf.HUNK_DATA, 2) + H(">II", 8, 0)
        exe += H(">IIIII", mkadf.HUNK_RELOC32, 1, 0, 0, 0) + H(">I", mkadf.HUNK_END)
        exe += H(">II", mkadf.HUNK_BSS, 8) + H(">I", mkadf.HUNK_END)
        code, data, bss, relocs, entry = mkadf.flatten(mkadf.parse_hunks(exe), mkadf.MEMF_ANY_CLEAR)
        self.assertEqual((len(code), len(data), bss, entry), (16, 8, 32, 0))
        self.assertEqual(relocs, [4 | mkadf.RELOC_TGT_SEG1, 0 | mkadf.RELOC_LOC_SEG1])
        self.assertEqual(struct.unpack_from(">I", code, 4)[0], 4)
        self.assertEqual(struct.unpack_from(">I", data, 0)[0], 8)
        (self.root / "boot.bin").write_bytes(b"\x4e\x75")
        (self.root / "p.exe").write_bytes(exe)
        args = SimpleNamespace(boot=str(self.root / "boot.bin"), payload=str(self.root / "p.exe"),
                               output=str(self.root / "two.adf"), alloc_flags="0x10000", param=[], build_id="t")
        with contextlib.redirect_stdout(io.StringIO()):
            mkadf.build(args)
            self.assertEqual(mkadf.dump(args.output), 0)
        adf = (self.root / "two.adf").read_bytes()
        hdr = struct.unpack_from(mkadf.HDR_FMT, adf, mkadf.HDR_OFF)
        self.assertEqual(hdr[1], 2)
        self.assertEqual(hdr[8], 512)                           # alloc_size: one code sector
        self.assertEqual(hdr[10:], (8, 1, 512))                 # data_size, data_sectors, data_alloc
        # the CD path flattens segment 1 to offset alloc_size
        from amitorture import cdimage
        _, image, cbss, flat = cdimage.read_adf_payload(adf)
        self.assertEqual((len(image), cbss, sorted(flat)), (520, 32, [4, 512]))
        self.assertEqual(struct.unpack_from(">I", image, 4)[0], 512 + 4)
        self.assertEqual(struct.unpack_from(">I", image, 512)[0], 8)

    def test_valid_adf_and_exact_boundary_remain_accepted(self):
        for image in (4, mkadf.ADF_SIZE - mkadf.BLOB_OFF):
            with self.subTest(image=image):
                path = self.adf(image=image)
                with contextlib.redirect_stdout(io.StringIO()):
                    self.assertEqual(mkadf.dump(path), 0)

    def smoke_records(self, st="pass", aborted=0, summary=None):
        hello = {k: 0 for k in smoke.HELLO_FIELDS}
        hello["probe"] = {k: 0 for k in smoke.PROBE_FIELDS}
        counts = {s: int(st == s) for s in ("pass", "fail", "skip", "untestable")}
        done = dict(counts, aborted=aborted, records=3)
        done.update(summary or {})
        return b"".join(records.encode(dict(v=1, seq=i, t=t, **data)) for i, (t, data) in enumerate((
            ("hello", hello), ("case", {"mod": "cpu", "case": "a", "st": st}), ("done", done))))

    def test_smoke_rejects_hidden_case_failures_and_aborts(self):
        for st, aborted, summary in (("fail", 0, {"fail": 0}), ("pass", 1, {}), ("pass", 0, {"pass": 0})):
            with self.subTest(st=st, aborted=aborted, summary=summary):
                failures = []
                smoke.check_records(self.smoke_records(st, aborted, summary), failures.append)
                self.assertTrue(failures)

    def test_smoke_accepts_consistent_nonfailing_counts(self):
        for st in ("pass", "skip", "untestable"):
            with self.subTest(st=st):
                failures = []
                smoke.check_records(self.smoke_records(st), failures.append)
                self.assertEqual(failures, [])

    def test_smoke_setup_failure_stops_process(self):
        bad = self.root / "not-a-directory"
        bad.write_text("fixture")
        work = self.root / "smoke"
        work.mkdir()
        emu = mock.Mock()
        emu.proc.poll.return_value = None
        with mock.patch.object(smoke, "Emulator", return_value=emu), \
                mock.patch.object(smoke.time, "sleep"), \
                mock.patch.object(smoke.tempfile, "mkdtemp", return_value=str(work)), \
                mock.patch.object(sys, "argv", ["smoke", "--amiberry", sys.executable, "--screenshots", str(bad)]):
            with self.assertRaises(FileExistsError):
                smoke.main()
        emu.kill.assert_called_once()

    def test_smoke_sdl_exhaustion_preserves_log_and_fails(self):
        emu = mock.Mock()
        emu.proc.poll.return_value = 1
        emu.output = ["SDL could not initialize\n"]
        dirs = []
        mkdtemp = tempfile.mkdtemp

        def tempdir(**kwargs):
            path = mkdtemp(prefix="sdl-", dir=self.root)
            dirs.append(Path(path))
            return path

        with mock.patch.object(smoke, "Emulator", return_value=emu) as launch, \
                mock.patch.object(smoke.time, "sleep"), \
                mock.patch.object(smoke.tempfile, "mkdtemp", side_effect=tempdir), \
                mock.patch.object(smoke, "read_serial") as serial, \
                mock.patch.object(sys, "argv", ["smoke", "--amiberry", sys.executable]), \
                contextlib.redirect_stdout(io.StringIO()):
            self.assertEqual(smoke.main(), 1)
        self.assertEqual(launch.call_count, 3)
        serial.assert_not_called()
        self.assertFalse(dirs[0].exists())
        self.assertFalse(dirs[1].exists())
        self.assertEqual((dirs[2] / "amiberry.log").read_text(), "".join(emu.output))
