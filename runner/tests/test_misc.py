"""ROM identification, matrix, config generation, isolation, report/import."""
import json
import os
import random
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from amitorture import amiberry_cfg, isolation, matrix, records, report, romid  # noqa: E402


def write(path, data, mode="wb"):
    with open(path, mode) as fh:
        fh.write(data)


def read(path):
    with open(path, encoding="utf-8") as fh:
        return fh.read()


class FakeRomDB(unittest.TestCase):
    """Uses a made-up ROM registered in the in-memory table."""

    def setUp(self):
        rnd = random.Random(5)
        self.rom = bytes([0x11, 0x14, 0x4e, 0xf9]) + bytes(rnd.randrange(256) for _ in range(262144 - 4))
        doc, by_crc, by_sha = romid.romdb()
        import hashlib
        self.entry = {"id": 99999, "name": "TEST ROM", "category": "kick", "type": "ROMTYPE_KICK", "version": "9.9",
                      "exec": "99.9", "models": ["TEST"], "size": len(self.rom), "cpu_flags": 0, "cloanto": False,
                      "crc32": f"{zlib.crc32(self.rom):08x}", "sha1": hashlib.sha1(self.rom).hexdigest()}
        by_crc.setdefault(self.entry["crc32"], []).append(self.entry)
        by_sha[self.entry["sha1"]] = self.entry

    def tearDown(self):
        doc, by_crc, by_sha = romid.romdb()
        by_crc[self.entry["crc32"]].remove(self.entry)
        del by_sha[self.entry["sha1"]]

    def test_plain(self):
        r = romid.identify_bytes(self.rom)
        self.assertEqual(r["status"], "ok")
        self.assertEqual(r["rom"]["id"], 99999)

    def test_byteswapped(self):
        sw = bytearray(len(self.rom))
        sw[0::2], sw[1::2] = self.rom[1::2], self.rom[0::2]
        r = romid.identify_bytes(bytes(sw))
        self.assertEqual(r["status"], "byteswapped")
        self.assertTrue(r["byteswapped"])
        self.assertFalse(romid.usable(r))

    def test_doubled(self):
        r = romid.identify_bytes(self.rom + self.rom)
        self.assertEqual(r["rom"]["id"], 99999)
        self.assertTrue(r["doubled"])

    def test_cloanto(self):
        key = bytes(random.Random(1).randrange(256) for _ in range(1544))
        enc = romid.CLOANTO_MAGIC + bytes(b ^ key[i % len(key)] for i, b in enumerate(self.rom))
        self.assertEqual(romid.identify_bytes(enc)["status"], "needs rom.key")
        r = romid.identify_bytes(enc, key)
        self.assertEqual(r["status"], "ok")
        self.assertEqual(r["encrypted"], "cloanto")
        with tempfile.TemporaryDirectory() as d:
            write(os.path.join(d, "x.rom"), enc)
            self.assertEqual(romid.identify_file(os.path.join(d, "x.rom"))["status"], "needs rom.key")
            write(os.path.join(d, "rom.key"), key)
            r = romid.identify_file(os.path.join(d, "x.rom"))
            self.assertEqual(r["status"], "ok")
            self.assertTrue(r["key"].endswith("rom.key"))

    def test_unknown_kick_header(self):
        data = bytearray(self.rom)
        data[100] ^= 1
        data[12:16] = bytes([0, 47, 0, 115])
        r = romid.identify_bytes(bytes(data))
        self.assertEqual(r["status"], "unknown kickstart")
        self.assertEqual(r["header"]["version"], "47.115")

    def test_garbage(self):
        self.assertEqual(romid.identify_bytes(b"hello" * 100)["status"], "unknown")


class RomDB(unittest.TestCase):
    def test_known_entries(self):
        a1200 = romid.by_id(15)
        self.assertEqual(a1200["crc32"], "1483a091")
        self.assertEqual(romid.by_id(16)["crc32"], "d6bae334")
        aros = romid.by_id(66)
        self.assertEqual(aros.get("builtin"), ":AROS")

    def test_68000_entries_only_get_68000_roms(self):
        for e in matrix.all_entries().values():
            if e["options"]["cpu_model"] in ("68000", "68010"):
                for i in e["rom"].get("ids", []):
                    self.assertEqual(romid.by_id(i)["cpu_flags"], 0, f"{e['id']}: rom {i} needs a 68020+")

    def test_matrix_roms_exist(self):
        for e in matrix.all_entries().values():
            for i in e["rom"].get("ids", []) + (e.get("ext_rom") or {}).get("ids", []):
                self.assertIsNotNone(romid.by_id(i), f"{e['id']}: rom id {i} not in romdb")


class Matrix(unittest.TestCase):
    def test_strict_policy_lanes_preserve_hardware_trap_coverage(self):
        for name in ("smoke", "standard", "full"):
            entries = {e["id"]: e for e in matrix.expand(name)}
            for base_id in ("a3000-68030-882-compat", "a4000-68040nommu-fast", "a4000-68060nommu-fast"):
                base = entries[base_id]
                strict = entries[f"{base_id}-strict"]
                self.assertEqual(strict["disk_groups"], ["core", "exp"])
                self.assertEqual(strict["params"]["modules"], matrix.module_mask(["exceptions", "fpu"]))
                self.assertEqual(strict["options"]["cachesize"], "0")
                self.assertEqual(strict["options"]["fpu_softfloat"], "false")
                for key in ("cpu_no_unimplemented", "fpu_no_unimplemented", "fpu_strict"):
                    self.assertEqual(strict["options"][key], "true")
                    self.assertNotEqual(base["options"].get(key), "true")
                self.assertFalse(strict["jit_capable"])
                self.assertFalse(strict["threaded_capable"])
                self.assertIsNone(strict["twin_of"])

    def test_sizes(self):
        # smoke: 15, plus threaded twins of the A600 68000 and the A1200 (JIT and not)
        self.assertTrue(8 <= len(matrix.expand("smoke")) <= 18)
        self.assertTrue(100 <= len(matrix.expand("standard")) <= 160)
        self.assertGreater(len(matrix.expand("full")), 300)

    def test_twins_differ_only_by_their_options(self):
        for name in matrix.MATRICES:
            entries = {e["id"]: e for e in matrix.expand(name)}
            for t in entries.values():
                if not t["twin_of"]:
                    continue
                base = entries[t["twin_of"]]
                diff = {k for k, _, _ in matrix.option_diff(base["options"], t["options"])}
                allowed = {"jit": matrix.JIT_OPTION_KEYS | {"cpu_threaded", "fpu_softfloat"},
                           "threaded": {"cpu_threaded"}, "softfloat": {"fpu_softfloat"}}[t["twin_kind"]]
                self.assertTrue(diff and diff <= allowed, (t["id"], diff))
                self.assertEqual(set(t["twin_options"]), diff, t["id"])
                if t["twin_kind"] == "jit":
                    self.assertEqual(base["options"]["cachesize"], "0")
                    self.assertNotEqual(t["options"]["cachesize"], "0")

    def test_every_jit_capable_has_twin(self):
        ids = {e["id"] for e in matrix.expand("full")}
        for e in matrix.expand("full"):
            if e["twin_of"] is None:
                self.assertEqual(e["id"] + "-jit" in ids, bool(e["jit_capable"]), e["id"])
                if e["jit_capable"]:
                    self.assertEqual(e["options"]["mmu_model"], "0")
                    self.assertEqual(e["options"]["cycle_exact"], "false")

    def test_every_variant_in_full(self):
        full = matrix.expand("full")
        for ck, c in matrix.CPUS.items():
            for fpu in c["fpus"]:
                for core in matrix.CORES:
                    self.assertTrue(any(e["cpu_variant"] == ck and e["core"] == core and e["fpu"] ==
                                        matrix.fpu_label(ck, fpu) for e in full), (ck, fpu, core))
        for model in matrix.MODELS:
            for video in ("PAL", "NTSC"):
                self.assertTrue(any(e["model"] == model and e["video"] == video for e in full), (model, video))
        variants = {e.get("jit_variant") for e in full if e["jit"]}
        self.assertTrue(set(v or "default" for v in matrix.JIT_VARIANTS) - {""} <= variants | {"default"})

    def test_no_unsupported_combination(self):
        for e in matrix.all_entries().values():
            o = e["options"]
            if o["cpu_threaded"] == "true":
                # every core but cycle-exact (Amiberry's automatic CPU thread)
                self.assertEqual(o["cpu_cycle_exact"], "false", e["id"])
                self.assertEqual(o["cpu_memory_cycle_exact"], "false", e["id"])
            if int(o["cachesize"]):
                self.assertEqual(o["cycle_exact"], "false", e["id"])
                self.assertNotIn(o["cpu_model"], ("68000", "68010"), e["id"])
            if o["cpu_model"] in ("68000", "68010"):
                self.assertEqual(o["fpu_model"], "0", e["id"])
            if o["fpu_softfloat"] == "true" and int(o["cachesize"]):
                self.assertEqual(o["compfpu"], "false", e["id"])

    def test_ids_unique(self):
        for name in matrix.MATRICES:
            ids = [e["id"] for e in matrix.expand(name)]
            self.assertEqual(len(ids), len(set(ids)), name)

    def test_select(self):
        e = matrix.expand("smoke")
        self.assertEqual([x["id"] for x in matrix.select(e, ["a4000-*"]) if not x["jit"]],
                         ["a4000-68040nommu-fast", "a4000-68060nommu-fast",
                          "a4000-68040nommu-fast-strict", "a4000-68060nommu-fast-strict"])

    def test_neighbours_change_one_axis(self):
        e = matrix.find("a4000-68040nommu-fast-jit")
        ns = matrix.neighbours(e)
        axes = {a for a, _, _ in ns}
        self.assertTrue({"jit", "comp_nf", "comp_trustbyte", "cachesize", "cpu_model", "fpu_softfloat",
                         "cpu_threaded", "chipset", "video"} <= axes, axes)
        self.assertNotIn("core", axes)    # JIT is fast-core only
        for a, label, n in ns:
            self.assertEqual(n["narrow"]["axis"], a)
            changed = {k for k, _, _ in matrix.option_diff(e["options"], n["options"])}
            self.assertTrue(changed <= set(n["narrow"]["changes"]), (a, changed))
        off = matrix.neighbours(matrix.find("a500-68000-ce"))
        self.assertIn("core", {a for a, _, _ in off})
        self.assertNotIn("jit", {a for a, _, _ in off})


class Config(unittest.TestCase):
    def test_complete_and_no_comments(self):
        e = matrix.expand("smoke")[0]
        opts, text = amiberry_cfg.build(e, rom_path="/r/k.rom", floppy="/m/d.adf", serial_port=23210, media_dir="/m")
        self.assertFalse(any(line.startswith(";") for line in text.splitlines()))
        for k in ("cpu_model", "chipset", "chipmem_size", "floppy0", "serial_port", "sound_output", "cachesize",
                  "kickstart_rom_file", "use_gui", "amiberry.ipc_disable_logging"):
            self.assertIn(k, opts)
        self.assertEqual(opts["serial_port"], "TCP://127.0.0.1:23210/wait")
        self.assertEqual(amiberry_cfg.parse(text)["floppy0"], "/m/d.adf")


class Env(unittest.TestCase):
    def test_whitelist(self):
        from amitorture import emulator
        with tempfile.TemporaryDirectory() as d:
            env = emulator.isolated_env(d, {"PATH": "/bin", "SDL_VIDEO_DRIVER": "kmsdrm,wayland,x11",
                                             "DISPLAY": ":0", "AMIBERRY_CONFIG_DIR": "/x", "XDG_CONFIG_HOME": "/u"})
            self.assertEqual(env["SDL_VIDEO_DRIVER"], "offscreen")
            self.assertEqual(env["SDL_VIDEODRIVER"], "offscreen")
            self.assertEqual(env["SDL_AUDIO_DRIVER"], "dummy")
            self.assertNotIn("DISPLAY", env)
            self.assertNotIn("AMIBERRY_CONFIG_DIR", env)
            self.assertTrue(env["XDG_CONFIG_HOME"].startswith(d))
            self.assertEqual(env["PATH"], "/bin")


class Adf(unittest.TestCase):
    def test_missing_mkadf_is_a_clear_error(self):
        from amitorture import adf
        from amitorture.emulator import RunnerError
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "a.adf")
            write(src, bytes(901120))
            saved = adf.MKADF
            adf.MKADF = os.path.join(d, "nope", "mkadf.py")
            try:
                with self.assertRaises(RunnerError) as cm:
                    adf.bake(src, os.path.join(d, "b.adf"), {"mode": "quick"})
                self.assertIn("mkadf.py", str(cm.exception))
            finally:
                adf.MKADF = saved

    def test_bake_with_real_mkadf(self):
        from amitorture import adf
        core = os.path.join(os.path.dirname(adf.DEFAULT_ADF), "set", "amitorture-1-core.adf")
        src = adf.DEFAULT_ADF if os.path.isfile(adf.DEFAULT_ADF) else core
        if not (os.path.isfile(adf.MKADF) and os.path.isfile(src)):
            self.skipTest("disk not built (make -C disk)")
        with tempfile.TemporaryDirectory() as d:
            info = adf.bake(src, os.path.join(d, "x.adf"), {"mode": "quick", "seed": "7"})
            self.assertEqual(info["params"]["mode"], 1)
            self.assertEqual(info["params"]["seed"], 7)


class Isolation(unittest.TestCase):
    def test_detects_changes(self):
        with tempfile.TemporaryDirectory() as d:
            os.makedirs(os.path.join(d, "cfg"))
            write(os.path.join(d, "cfg", "a"), "x", "w")
            paths = [(os.path.join(d, "cfg"), True, "fail")]
            g = isolation.Guard(paths).__enter__()
            self.assertEqual(g.check(), [])
            write(os.path.join(d, "cfg", "b"), "y", "w")
            with open(os.path.join(d, "cfg", "a"), "a") as fh:
                fh.write("more")
            ch = {(os.path.basename(c["path"]), c["change"]) for c in g.check()}
            self.assertIn(("b", "created"), ch)
            self.assertIn(("a", "size changed"), ch)
            self.assertFalse(g.ok)

    def test_watched_uses_xdg(self):
        p = [x[0] for x in isolation.watched_paths({"HOME": "/h", "XDG_CONFIG_HOME": "/c"})]
        self.assertIn("/c/amiberry", p)
        self.assertIn("/h/Amiberry/Configurations", p)
        self.assertIn("/h/.local/share/amiberry", p)


class Import(unittest.TestCase):
    def capture(self, fail=False, drop=None, build="b", mod="cpu"):
        seq = iter(range(100))

        def r(t, **kw):
            return records.encode({"v": 1, "seq": next(seq), "t": t, **kw})
        lines = [r("hello", build=build, mode=0, seed=1, exec="40.68", cpu="68000", fpu="none", mmu=0,
                   probe={"cpu": "68000", "fpu": "none", "chipset": "OCS", "kick": "34.5"}),
                 r("mod", mod=mod, ev="begin")]
        if fail:
            lines.append(r("case", mod=mod, case="divu_overflow", st="fail", expect={"sr": "0002"},
                           got={"sr": "0000"}, why="V flag not set", ctx={"pc": "0001a2c4"}))
        else:
            lines.append(r("case", mod=mod, case="divu_overflow", st="pass"))
        lines.append(r("mod", mod=mod, ev="end", **{"pass": 0 if fail else 1, "fail": 1 if fail else 0, "skip": 0}))
        lines.append(r("done", **{"pass": 0 if fail else 1, "fail": 1 if fail else 0, "skip": 0, "untestable": 0,
                                  "aborted": 0, "records": 5}))
        if drop is not None:
            del lines[drop]
        return b"ROMInfo: noise\r\n" + b"".join(lines)

    def run_import(self, data):
        d = tempfile.mkdtemp()
        self.addCleanup(__import__("shutil").rmtree, d)
        p = os.path.join(d, "serial.bin")
        write(p, data)
        out = os.path.join(d, "out")
        summ = report.import_files([p], out, "t")
        return summ, out

    def test_pass(self):
        summ, out = self.run_import(self.capture())
        self.assertEqual(summ["verdict"], "PASS")
        for f in ("REPORT.md", "summary.json", "failures.jsonl", "environment.json"):
            self.assertTrue(os.path.exists(os.path.join(out, f)), f)

    def test_failure_has_context(self):
        summ, out = self.run_import(self.capture(fail=True))
        self.assertTrue(summ["verdict"].startswith("PROBLEMS"))
        items = [json.loads(x) for x in read(os.path.join(out, "failures.jsonl")).splitlines()]
        f = [i for i in items if i["kind"] == "case_fail"][0]
        self.assertEqual(f["why"], "V flag not set")
        self.assertEqual(f["mod"], "cpu")
        self.assertTrue(f["preceding"])
        self.assertIn("divu_overflow", read(os.path.join(out, "REPORT.md")))

    def test_gap_makes_incomplete(self):
        summ, out = self.run_import(self.capture(drop=2))
        self.assertEqual(summ["entries"][0]["status"], "incomplete")

    def import_paths(self, blobs):
        """blobs: [(relative path, bytes)] -> summary, records of the import"""
        d = tempfile.mkdtemp()
        self.addCleanup(__import__("shutil").rmtree, d)
        paths = []
        for rel, data in blobs:
            p = os.path.join(d, rel)
            os.makedirs(os.path.dirname(p), exist_ok=True)
            write(p, data)
            paths.append(p)
        out = os.path.join(d, "out")
        summ = report.import_files(paths, out, "t")
        recs = [json.loads(x) for x in read(os.path.join(out, "raw", "imported", "records.jsonl")).splitlines()]
        return summ, recs

    def test_disk_set_in_separate_files(self):
        # every disk numbers its records from 0, and the files share a name
        a = self.capture(build="x core", mod="cpu")
        b = self.capture(build="x fpu", mod="fpu", fail=True)
        summ, recs = self.import_paths([("1/serial.bin", a), ("2/serial.bin", b)])
        self.assertEqual(len(recs), 10)
        self.assertEqual({r["disk"] for r in recs}, {"core", "fpu"})
        e = summ["entries"][0]
        self.assertEqual(e["status"], "fail")
        self.assertIn("fpu: fail", e["reason"])
        self.assertNotIn("core:", e["reason"])

    def test_disk_set_in_one_capture(self):
        a = self.capture(build="x core", mod="cpu")
        b = self.capture(build="x fpu", mod="fpu")
        summ, recs = self.import_paths([("long.txt", a + b"\r\nAmiga reset\r\n" + b)])
        self.assertEqual(len(recs), 10)
        self.assertEqual(summ["entries"][0]["status"], "pass")

    def test_two_transports_of_one_disk(self):
        a = self.capture(build="x core")
        summ, recs = self.import_paths([("serial.bin", a), ("copy/serial.bin", a)])
        self.assertEqual(len(recs), 5)
        self.assertTrue(all(len(r["sources"]) == 2 for r in recs))
        self.assertEqual(summ["entries"][0]["status"], "pass")

    def test_unfinished_disk(self):
        a = self.capture(build="x core")
        b = self.capture(build="x fpu", mod="fpu", drop=4)    # no done record
        summ, recs = self.import_paths([("a", a), ("b", b)])
        e = summ["entries"][0]
        self.assertNotEqual(e["status"], "pass")
        self.assertIn("fpu:", e["reason"])
        self.assertIn("capture stopped", e["reason"])


class Interrupt(unittest.TestCase):
    """Ctrl+C plumbing (no emulator)."""

    def setUp(self):
        from amitorture import emulator
        self.em = emulator
        emulator.INTERRUPT.clear()
        self.addCleanup(emulator.INTERRUPT.clear)

    def test_parallel_cancels_queued_items(self):
        import threading
        import time as _t
        started = []
        lock = threading.Lock()

        def fn(i):
            with lock:
                started.append(i)
            if i == 0:
                raise self.em.Interrupted("ctrl+c")
            for _ in range(50):
                self.em.check_interrupt()
                _t.sleep(0.01)
        t0 = _t.monotonic()
        with self.assertRaises(self.em.Interrupted):
            self.em.run_parallel(fn, list(range(20)), 2)
        self.assertTrue(self.em.INTERRUPT.is_set())
        self.assertLess(len(started), 20)          # queued items were cancelled
        self.assertLess(_t.monotonic() - t0, 5)

    def test_no_launch_after_interrupt(self):
        self.em.INTERRUPT.set()
        with self.assertRaises(self.em.Interrupted):
            self.em.Process("/bin/true", [], {}, os.devnull)

    def test_max_slots_leaves_a_core(self):
        # nproc - 1, at least 1; AMT_MAX_EMU only lowers it
        import amislot
        from unittest import mock
        old = os.environ.pop("AMT_MAX_EMU", None)
        self.addCleanup(lambda: os.environ.__setitem__("AMT_MAX_EMU", old) if old is not None
                        else os.environ.pop("AMT_MAX_EMU", None))
        for cpus, want in ((8, 7), (4, 3), (2, 1), (1, 1), (None, 1)):
            with mock.patch.object(amislot.os, "cpu_count", return_value=cpus):
                self.assertEqual(amislot.max_slots(), want, cpus)
        with mock.patch.object(amislot.os, "cpu_count", return_value=8):
            os.environ["AMT_MAX_EMU"] = "2"
            self.assertEqual(amislot.max_slots(), 2)
            os.environ["AMT_MAX_EMU"] = "8"
            self.assertEqual(amislot.max_slots(), 7)       # never above nproc - 1

    def test_slot_wait_is_cancellable(self):
        import amislot
        d = tempfile.mkdtemp()
        self.addCleanup(__import__("shutil").rmtree, d)
        old = os.environ.get("AMT_LOCK_DIR"), os.environ.get("AMT_MAX_EMU")
        os.environ["AMT_LOCK_DIR"], os.environ["AMT_MAX_EMU"] = d, "1"
        orphans = amislot._orphans
        amislot._orphans = lambda d, n: []      # other emulators on this machine must not count here
        self.addCleanup(setattr, amislot, "_orphans", orphans)
        try:
            with amislot.emulator_slot(quiet=True):
                calls = []

                def cancel():
                    calls.append(1)
                    return len(calls) > 2
                with self.assertRaises(KeyboardInterrupt):
                    with amislot.emulator_slot(poll=0.01, quiet=True, cancel=cancel):
                        self.fail("got a second slot with AMT_MAX_EMU=1")
        finally:
            for k, v in zip(("AMT_LOCK_DIR", "AMT_MAX_EMU"), old):
                if v is None:
                    os.environ.pop(k, None)
                else:
                    os.environ[k] = v


class Show(unittest.TestCase):
    """--show: the user's own desktop settings, with the isolation kept."""

    def test_users_settings_pass_through(self):
        from amitorture import emulator
        e = emulator.display_env({"WAYLAND_DISPLAY": "/run/user/1000/wayland-1", "DISPLAY": ":0",
                                  "SDL_VIDEO_DRIVER": "kmsdrm,wayland,x11", "SDL_AUDIO_DRIVER": "pipewire",
                                  "XAUTHORITY": "/home/u/.Xauthority"})
        self.assertEqual(e["SDL_VIDEO_DRIVER"], "kmsdrm,wayland,x11")      # as the user has it
        self.assertNotIn("SDL_AUDIO_DRIVER", e)                            # audio stays the runner's
        self.assertEqual(e["DISPLAY"], ":0")
        self.assertEqual(e["XAUTHORITY"], "/home/u/.Xauthority")

    def test_no_driver_invented(self):
        from amitorture import emulator
        e = emulator.display_env({"WAYLAND_DISPLAY": "/w"})
        self.assertNotIn("SDL_VIDEO_DRIVER", e)
        self.assertNotIn("SDL_VIDEODRIVER", e)

    def test_relative_wayland_socket_keeps_pointing_at_it(self):
        from amitorture import emulator
        e = emulator.display_env({"WAYLAND_DISPLAY": "wayland-1", "XDG_RUNTIME_DIR": "/run/user/1000"})
        self.assertEqual(e["WAYLAND_DISPLAY"], "/run/user/1000/wayland-1")

    def test_no_session_is_an_error(self):
        from amitorture import emulator
        with self.assertRaises(emulator.RunnerError):
            emulator.display_env({"XDG_RUNTIME_DIR": "/run/user/1000"})

    def test_isolation_kept(self):
        from amitorture import emulator
        d = tempfile.mkdtemp()
        self.addCleanup(__import__("shutil").rmtree, d)
        env = emulator.isolated_env(d, {"PATH": "/bin", "WAYLAND_DISPLAY": "wayland-0",
                                        "XDG_RUNTIME_DIR": "/run/user/1000", "HOME": "/home/u"}, show=True)
        self.assertEqual(env["HOME"], d)
        self.assertTrue(env["XDG_RUNTIME_DIR"].startswith(d))     # Amiberry's IPC socket stays in the work dir
        self.assertEqual(env["WAYLAND_DISPLAY"], "/run/user/1000/wayland-0")
        self.assertNotIn("SDL_VIDEO_DRIVER", env)                 # no offscreen, no invented driver
        self.assertEqual(env["SDL_AUDIO_DRIVER"], "dummy")


class SpeedTrims(unittest.TestCase):
    """Board-entry module masks and the per-disk floppy speed."""

    def test_module_ids_match_the_sources(self):
        import glob
        import re
        from amitorture import matrix
        root = os.path.join(os.path.dirname(__file__), "..", "..", "disk", "modules")
        found = {}
        for f in glob.glob(os.path.join(root, "*.c")):
            for m in re.finditer(r"^AMT_MODULE\(\s*(\w+)\s*,\s*(\d+)\s*,", open(f).read(), re.M):
                found[m.group(1)] = int(m.group(2))
        self.assertEqual(found, matrix.MODULE_IDS)

    def test_masks_are_anded(self):
        from amitorture.runner import merge_params
        p = merge_params({"modules": "0x1000", "seed": "1"}, {"modules": "0x1010000"})
        self.assertEqual(p["modules"], "0x0")                      # narrowing to a module outside the entry
        p = merge_params({"modules": "all"}, {"modules": "0x1010000"})
        self.assertEqual(p["modules"], "0x1010000")
        self.assertEqual(merge_params({"seed": "1"}, {"modules": "0x10"})["modules"], "0x10")

    def test_board_entries_masked_but_one_per_bus_full(self):
        from amitorture import matrix
        es = {e["id"]: e for e in matrix.expand("standard")}
        rtg = es["a4000-rtg-a2410"]
        self.assertEqual(rtg["disk_groups"], ["exp"])
        self.assertEqual(int(rtg["params"]["modules"], 16), (1 << 16) | (1 << 24))
        # a full board entry runs every module
        full = int(es["a4000-rtg-cv64z3"].get("params", {}).get("modules", "0xffffffffffffffff"), 16)
        self.assertEqual(full, 0xFFFFFFFFFFFFFFFF)
        zz = es["a4000-rtg-zz9000z2"]
        self.assertTrue(int(zz["params"]["modules"], 16) & (1 << 18))      # network too
        self.assertIn("exp", es["a4000-z2z3-rtg"]["disk_groups"])           # the memory modules

    def test_floppy_speed_per_disk(self):
        from amitorture.runner import disk_floppy_speed as f, FLOPPY_TEST_SPEED as R_TEST
        self.assertEqual(f("1-core", {"options": {}}), R_TEST)  # paula, dmacontention: disk DMA
        self.assertEqual(f("2-cpu", {"options": {}}), "0")
        self.assertEqual(f("3-exp", {"options": {}}), "0")
        self.assertIsNone(f("3-exp", {"options": {"floppy_speed": "100"}}))   # an explicit setting wins
        self.assertIsNone(f(None, {"options": {}}))           # a single ADF: unknown modules

    def test_floppy_speed_single_set_disk(self):
        # a set disk passed with --adf gets the speed it has in the set
        import tempfile
        from unittest import mock
        from amitorture import runner as R
        with mock.patch.object(R.adfmod, "header_info", return_value={"build_id": "6809d17 2026-11-04 3/3 exp"}):
            self.assertEqual(R.disk_floppy_speed(None, {"options": {}}, adf="x.adf"), "0")
        with mock.patch.object(R.adfmod, "header_info", return_value={"build_id": "6809d17 2026-11-04 1/3 core"}):
            self.assertEqual(R.disk_floppy_speed(None, {"options": {}}, adf="x.adf"), R.FLOPPY_TEST_SPEED)
        with mock.patch.object(R.adfmod, "header_info", return_value={"build_id": "6809d17 2026-11-04"}):
            self.assertIsNone(R.disk_floppy_speed(None, {"options": {}}, adf="x.adf"))   # all-in-one
        with tempfile.NamedTemporaryFile(suffix=".adf") as fh:                          # not a disk
            self.assertIsNone(R.disk_floppy_speed(None, {"options": {}}, adf=fh.name))


class XfailLists(unittest.TestCase):
    def test_several_causes_per_case(self):
        from amitorture import xfail
        reg = {"m.c": [{"ref": "A", "configs": "*", "when": {"cpu_model": "68000"}},
                       {"ref": "B", "configs": "*-snd-*", "when": {"ntsc": "!true"}}]}
        self.assertEqual(xfail.match(reg, "m.c", "a500-snd-x", {"cpu_model": "68000"})["ref"], "A")
        self.assertEqual(xfail.match(reg, "m.c", "a1200-snd-x", {"cpu_model": "68020"})["ref"], "B")
        self.assertIsNone(xfail.match(reg, "m.c", "a1200-snd-x", {"cpu_model": "68020", "ntsc": "true"}))
        self.assertIsNone(xfail.match(reg, "m.c", "a1200-x", {"cpu_model": "68020"}))

    def test_registry_loads(self):
        from amitorture import xfail
        reg = xfail.load()
        self.assertIsInstance(reg["expsound.uaesnd_timer_irq"], list)
        m = xfail.match(reg, "expsound.uaesnd_timer_irq", "a500-snd-uaesndz2",
                        {"ntsc": "false", "cpu_model": "68000", "cpu_compatible": "true"})
        self.assertEqual(m["ref"], "finding S6")
        # S8 (fixed in Amiberry 07708117f) is no longer registered
        self.assertIsNone(xfail.match(reg, "expsound.uaesnd_timer_irq", "a1200-snd-uaesndz2",
                                      {"ntsc": "false", "cpu_model": "68020"}))


class FinalScreenshot(unittest.TestCase):
    """The final status screen is kept as final.png after a done record."""

    def setUp(self):
        from amitorture import runner
        self.runner = runner
        self.old = runner.FINAL_SHOT_DELAY
        runner.FINAL_SHOT_DELAY = 0
        self.dir = tempfile.mkdtemp()

    def tearDown(self):
        self.runner.FINAL_SHOT_DELAY = self.old

    def test_saved_and_recorded(self):
        class IPC:
            def request(self, line, timeout):
                write(line.split("\t", 1)[1], b"png")
                return "OK"
        res = {}
        self.runner.final_screenshot(IPC(), self.dir, res)
        self.assertEqual(res["final_screenshot"], os.path.join(self.dir, "final.png"))

    def test_failure_is_recorded_not_raised(self):
        class IPC:
            def request(self, line, timeout):
                return "ERROR\tFailed to create screenshot"
        res = {}
        self.runner.final_screenshot(IPC(), self.dir, res)
        self.assertIn("Failed", res["final_screenshot"]["error"])


class RunnerFixes(unittest.TestCase):
    def test_is_threaded(self):
        self.assertTrue(matrix.is_threaded({"cpu_threaded": " True "}))
        self.assertFalse(matrix.is_threaded({"cpu_threaded": "false"}))
        self.assertFalse(matrix.is_threaded({}))
        self.assertFalse(matrix.is_threaded(None))

    def test_pick_port_never_hands_out_a_held_port(self):
        from amitorture import emulator
        real = emulator.port_free
        emulator.port_free = lambda p: True
        try:
            got = {emulator.pick_port(50000, 50002) for _ in range(3)}
            self.assertEqual(got, {50000, 50001, 50002})
            with self.assertRaises(emulator.RunnerError):
                emulator.pick_port(50000, 50002)
            emulator.release_port(50001)
            self.assertEqual(emulator.pick_port(50000, 50002), 50001)
        finally:
            emulator.port_free = real
            for p in (50000, 50001, 50002):
                emulator.release_port(p)

    def test_port_in_time_wait_is_free_listening_is_not(self):
        import socket
        import time
        from amitorture import emulator
        lst = socket.socket()
        lst.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        lst.bind(("127.0.0.1", 0))
        lst.listen()
        port = lst.getsockname()[1]
        self.assertFalse(emulator.port_free(port))
        cli = socket.create_connection(("127.0.0.1", port))
        acc, _ = lst.accept()
        acc.close()                 # server side closes first -> TIME_WAIT on the port
        lst.close()
        time.sleep(0.1)
        cli.close()
        self.assertTrue(emulator.port_free(port))

    def test_mailbox_overrun_is_a_problem(self):
        from amitorture import runner
        self.assertEqual(runner.mailbox_problems({"lost_bytes": 0}), [])
        self.assertIn("mailbox overrun: 40", runner.mailbox_problems({"lost_bytes": 40})[0])

    def test_a_boot_seen_only_on_the_mailbox_resets_the_requests(self):
        import threading
        from amitorture import runner

        class Reqs:
            boots = 0
            def new_boot(self):
                self.boots += 1
            def offer(self, obj, src):
                pass
        er = object.__new__(runner.EntryRun)
        er.bench, er.requests, er.lock = None, Reqs(), threading.Lock()
        er._taken, er._boot_seen = {"serial": 0, "mailbox": 0}, 0
        er.serial_parser, er.mbox_parser = records.StreamParser("serial"), records.StreamParser("mailbox")
        enc = lambda q: records.encode({"v": 1, "seq": q, "t": "log", "msg": "m"})
        er.serial_parser.feed(enc(0) + enc(1))
        er.mbox_parser.feed(enc(0) + enc(1) + enc(0))
        er.harvest()
        self.assertEqual(er.requests.boots, 1)
        er.serial_parser.feed(enc(0))       # serial catches up: the same boot, no second reset
        er.harvest()
        self.assertEqual(er.requests.boots, 1)


class Decode(unittest.TestCase):
    @staticmethod
    def enc(seq, t, **kw):
        return records.encode({"v": 1, "seq": seq, "t": t, **kw})

    def capture(self, fail=True, done=True):
        e = self.enc
        recs = [e(0, "hello", build="b", cpu="68040", fpu="68040", mmu=1, probe={"chipset": "AGA"},
                  mode="all", seed=1, exec=1),
                e(1, "mod", ev="begin", mod="cpu"),
                e(2, "case", mod="cpu", case="add", st="pass"),
                e(3, "case", mod="cpu", case="div", st="fail" if fail else "pass", why="wrong flags",
                  expect="1", got="2"),
                e(4, "mod", ev="end", mod="cpu", **{"pass": 1, "fail": 1, "skip": 0, "untestable": 0}),
                e(5, "bench", ev="env", tick_hz=1000000),
                e(6, "bench", ev="begin", blk="add_l", tier="micro", id=3, unit="insn", per=2, mem="fast"),
                e(7, "bench", ev="end", blk="add_l", st="ok", n=1000, reps=2, timing={"best": 4000}),
                e(8, "bench", ev="begin", blk="fadd", tier="micro", id=4, unit="insn", per=1),
                e(9, "bench", ev="end", blk="fadd", st="ok", n=10, reps=2, nsu=123, timing={"best": 1}),
                e(10, "bench", ev="score", dmips_x100=4567, mmu_fault_ns=900)]
        if done:
            recs.append(e(11, "done", records=12, **{"pass": 1, "fail": int(fail), "skip": 0, "untestable": 0,
                                                       "aborted": 0}))
        return b"\x00" * 50 + recs[0] + b"# FAIL cpu.div: wrong flags\n" + b"".join(recs[1:])

    def test_summary(self):
        from amitorture import decode
        text, problems = decode.decode(self.capture())
        self.assertIn("FAIL cpu.div: wrong flags", text)
        self.assertIn("50 junk byte(s)", text)
        self.assertIn("1 comment line(s)", text)
        # add_l from timing.best: 4000 ticks at 1 MHz over 1000 x 2 insns; fadd from nsu (x10)
        self.assertRegex(text, r"add_l .* 2000\.0  insn")
        self.assertRegex(text, r"fadd .* 12\.300  insn")
        self.assertIn("dmips 45.67", text)
        self.assertIn("mmu_fault_ns 900", text)
        self.assertEqual(problems, ["1 failed case(s)"])

    def test_clean_and_broken_off_captures(self):
        from amitorture import decode
        self.assertEqual(decode.decode(self.capture(fail=False))[1], [])
        self.assertIn("no done record", decode.decode(self.capture(fail=False, done=False))[1])

    def test_cli_writes_nothing(self):
        from amitorture import cli
        work = os.path.join(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__)))),
                            "work", "tmp")
        os.makedirs(work, exist_ok=True)
        with tempfile.TemporaryDirectory(dir=work) as d:
            path = os.path.join(d, "cap.txt")
            with open(path, "wb") as f:
                f.write(self.capture(fail=False))
            import contextlib
            import io
            out = io.StringIO()
            with contextlib.redirect_stdout(out):
                rc = cli.main(["decode", path])
            self.assertEqual(rc, 0)
            self.assertIn("OK", out.getvalue())
            self.assertEqual(os.listdir(d), ["cap.txt"])


if __name__ == "__main__":
    unittest.main()
