"""Milestone-3 runner logic on fake data (no emulator): the host request
channel (CONTRACT §13) with a fake IPC, audio analysis, fail_detail_changed."""
import json
import math
import os
import shutil
import struct
import sys
import tempfile
import unittest
import zlib

sys.path.insert(0, os.path.join(os.path.dirname(__file__), ".."))

from amitorture import audio, hostreq, records  # noqa: E402
from amitorture import baseline as B  # noqa: E402
from amitorture import runner as R  # noqa: E402

WORK_TMP = os.path.join(os.path.dirname(__file__), "..", "..", "work", "tmp")


def tiny_png(path, w=4, h=2):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xffffffff)
    raw = b"".join(b"\0" + b"\0\0\0" * w for _ in range(h))
    with open(path, "wb") as fh:
        fh.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                 + chunk(b"IDAT", zlib.compress(raw)) + chunk(b"IEND", b""))


class FakeIPC:
    def __init__(self, fail=()):
        self.lines = []
        self.mem = {}
        self.fail = set(fail)

    def request(self, line, timeout=15):
        self.lines.append(line)
        cmd, *args = line.split("\t")
        if cmd in self.fail:
            return "ERROR\tnope"
        if cmd == "SCREENSHOT":
            tiny_png(args[0])
            return f"OK\t{args[0]}"
        if cmd == "WRITE_MEM":
            self.mem[int(args[0], 0)] = int(args[2], 0)
            return "OK"
        return "OK"


def req(seq, rid, what, tag="t", args=None):
    o = {"v": 1, "seq": seq, "t": "request", "id": rid, "what": what, "tag": tag}
    if args is not None:
        o["args"] = args
    return o


class Clock:
    def __init__(self):
        self.t = 100.0
        self.slept = []

    def __call__(self):
        return self.t

    def sleep(self, s):
        self.slept.append(s)
        self.t += s


class HostRequests(unittest.TestCase):
    MBOX = 0x5070

    def setUp(self):
        os.makedirs(WORK_TMP, exist_ok=True)
        self.d = tempfile.mkdtemp(dir=WORK_TMP)
        self.ipc = FakeIPC()
        self.clk = Clock()
        self.rq = hostreq.Requests(self.ipc, self.d, "raw/e/1-core/", options={"joyport0": "mouse", "joyport1": "none"},
                                   clock=self.clk, sleep=self.clk.sleep)

    def tearDown(self):
        shutil.rmtree(self.d, ignore_errors=True)

    def acks(self):
        return [ln for ln in self.ipc.lines if ln.startswith("WRITE_MEM")]

    def test_waits_for_mailbox_then_acks_big_endian_long_at_44(self):
        self.assertTrue(self.rq.offer(req(5, 1, "wait"), "serial"))
        self.assertEqual(self.rq.service(None), [])
        self.assertTrue(self.rq.waiting())
        self.assertEqual(self.acks(), [])
        made = self.rq.service(self.MBOX)
        self.assertEqual(len(made), 1)
        self.assertEqual(self.acks(), [f"WRITE_MEM\t{self.MBOX + 44:#x}\t4\t0x1"])
        h = made[0]
        self.assertEqual((h["t"], h["id"], h["what"], h["acked"], h["req_seq"]), ("host", 1, "wait", True, 5))
        self.assertFalse(self.rq.waiting())

    def test_same_request_on_both_transports_is_served_once(self):
        self.rq.offer(req(5, 1, "wait"), "serial")
        self.assertFalse(self.rq.offer(req(5, 1, "wait"), "mailbox"))
        self.rq.service(self.MBOX)
        self.assertEqual(len(self.acks()), 1)
        self.rq.new_boot()        # after a reboot the same id/seq is a new request
        self.assertTrue(self.rq.offer(req(5, 1, "wait"), "serial"))

    def test_unknown_and_joystick_are_never_acked(self):
        self.rq.offer(req(1, 1, "teleport"), "serial")
        self.rq.offer(req(2, 2, "joy", args={"port": 1, "dirs": "u", "fire": 1}), "serial")
        self.rq.offer(req(3, 3, "cd32pad", args={"buttons": 1}), "serial")
        made = self.rq.service(self.MBOX)
        self.assertEqual(self.acks(), [])
        self.assertEqual([h["acked"] for h in made], [False, False, False])
        self.assertIn("unknown", made[0]["result"]["error"])
        self.assertIn("no joystick", made[1]["result"]["error"])
        self.assertEqual(self.rq.summary()["not_acked"], 3)

    def test_key_and_mouse(self):
        self.rq.offer(req(1, 1, "key", args={"code": 0x45, "up": 0}), "serial")
        self.rq.offer(req(2, 2, "key", args={"code": 0x45, "up": 1}), "serial")
        self.rq.offer(req(3, 3, "mouse", args={"dx": 5, "dy": -3, "buttons": 1, "port": 0}), "serial")
        self.rq.offer(req(4, 4, "mouse", args={"dx": 1, "dy": 1, "buttons": 0, "port": 1}), "serial")
        self.rq.offer(req(5, 5, "key", args={"code": 300}), "serial")
        made = self.rq.service(self.MBOX)
        sent = [ln for ln in self.ipc.lines if not ln.startswith("WRITE_MEM")]
        self.assertEqual(sent, ["SEND_KEY\t69\t1", "SEND_KEY\t69\t0", "SEND_MOUSE\t5\t-3\t1"])
        self.assertEqual([h["acked"] for h in made], [True, True, True, False, False])
        self.assertIn("joyport1=none", made[3]["result"]["error"])

    def test_screenshot(self):
        self.rq.offer(req(7, 9, "screenshot", tag="bars/1", args={"frames": 25}), "mailbox")
        h = self.rq.service(self.MBOX)[0]
        self.assertTrue(h["acked"])
        self.assertEqual(self.clk.slept, [0.5])                 # 25 frames at 50 Hz
        r = h["result"]
        self.assertEqual(r["path"], "raw/e/1-core/shot-9-bars_1.png")
        self.assertEqual((r["width"], r["height"]), (4, 2))
        self.assertEqual(len(r["sha256"]), 64)
        self.assertTrue(os.path.isfile(os.path.join(self.d, "shot-9-bars_1.png")))

    def test_failed_action_is_not_acked(self):
        self.ipc.fail.add("SCREENSHOT")
        self.rq.offer(req(1, 1, "screenshot"), "serial")
        h = self.rq.service(self.MBOX)[0]
        self.assertFalse(h["acked"])
        self.assertEqual(self.acks(), [])

    def test_audio_mark_without_capture(self):
        self.rq.offer(req(1, 1, "audio_mark", args={"edge": "start"}), "serial")
        h = self.rq.service(self.MBOX)[0]
        self.assertFalse(h["acked"])
        self.assertIn("no audio capture", h["result"]["error"])

    def test_capture_records_need_no_mailbox_and_no_ack(self):
        self.rq.offer({"v": 1, "seq": 3, "t": "capture", "what": "screen", "tag": "x"}, "serial")
        h = self.rq.service(None)[0]
        self.assertIsNone(h["acked"])
        self.assertEqual(self.acks(), [])
        self.assertTrue(h["result"]["path"].endswith("shot-c3-x.png"))

    def test_attach_after_request_and_integrity_counts_guest_only(self):
        guest = [{"v": 1, "seq": 0, "t": "hello", "build": "b", "mode": 0, "seed": 1, "exec": "40.68", "cpu": "68020",
                  "fpu": "none", "mmu": 0, "probe": {}},
                 req(1, 1, "wait"),
                 {"v": 1, "seq": 2, "t": "case", "mod": "input", "case": "c", "st": "pass"},
                 {"v": 1, "seq": 3, "t": "done", "pass": 1, "fail": 0, "skip": 0, "untestable": 0, "aborted": 0,
                  "records": 4}]
        m = records.merge({"serial": [records.parse_line(records.encode(r)) for r in guest]})
        self.assertEqual(m.schema_problems, [])     # "request" is a known record type
        self.rq.offer(guest[1], "serial")
        hosts = self.rq.service(self.MBOX)
        hostreq.attach(m, hosts)
        self.assertEqual([x["rec"]["t"] for x in m.records], ["hello", "request", "host", "case", "done"])
        st, why = R.classify({}, m, "done")
        self.assertEqual(st, "pass", why)


class FakeOpts:
    verbose = False
    hang = 30
    timeout = 10


class Harvest(unittest.TestCase):
    def test_requests_from_both_parsers(self):
        os.makedirs(WORK_TMP, exist_ok=True)
        d = tempfile.mkdtemp(dir=WORK_TMP)
        try:
            run = R.EntryRun(FakeOpts(), {"id": "e", "options": {"chipmem_size": "4"}}, d)
            ipc = FakeIPC()
            run.requests = hostreq.Requests(ipc, d)
            line = records.encode(req(4, 2, "wait"))
            run.on_serial(line)
            run.mbox_parser.feed(line)
            run.harvest()
            self.assertEqual(len(run.requests.pending), 1)
            run.requests.service(0x1000)
            self.assertEqual(ipc.mem, {0x1000 + 44: 2})
            run.close()
        finally:
            shutil.rmtree(d, ignore_errors=True)


def tone(freq, secs, rate=44100, amp=0.5, square=False, lead=0.0):
    n = int(secs * rate)
    pre = [0.0] * int(lead * rate)
    if square:
        return pre + [amp if math.sin(2 * math.pi * freq * i / rate) >= 0 else -amp for i in range(n)]
    return pre + [amp * math.sin(2 * math.pi * freq * i / rate) for i in range(n)]


class Audio(unittest.TestCase):
    def test_frequency_and_rms_per_channel(self):
        left = tone(440, 0.5, lead=0.1)
        right = tone(1000, 0.5, square=True, amp=0.25, lead=0.1)
        a = audio.analyse([left, right], 44100, expect_hz=440)
        L, Rc = a["channels"]["L"], a["channels"]["R"]
        self.assertAlmostEqual(L["freq_fft_hz"], 440, delta=1.0)
        self.assertAlmostEqual(L["freq_zc_hz"], 440, delta=2.0)
        self.assertAlmostEqual(Rc["freq_fft_hz"], 1000, delta=2.0)
        self.assertAlmostEqual(L["active_rms"], 0.5 / math.sqrt(2), delta=0.01)
        self.assertAlmostEqual(Rc["active_rms"], 0.25, delta=0.01)
        self.assertAlmostEqual(L["onset_s"], 0.1, delta=0.002)
        self.assertEqual(a["loudest"], "L")
        self.assertTrue(a["freq_ok"])

    def test_silence(self):
        a = audio.analyse([[0.0] * 4410, [0.0] * 4410], 44100, expect_hz=440)
        self.assertTrue(a["silent"])
        self.assertFalse(a["freq_ok"])

    def test_s16_roundtrip_and_capture_marks(self):
        os.makedirs(WORK_TMP, exist_ok=True)
        d = tempfile.mkdtemp(dir=WORK_TMP)
        try:
            path = os.path.join(d, "audio.raw")
            clk = Clock()
            cap = audio.Capture(path, d, "raw/x/", clock=clk, sleep=clk.sleep)

            def append(samples_l, samples_r):
                with open(path, "ab") as fh:
                    for a_, b_ in zip(samples_l, samples_r):
                        fh.write(struct.pack("<hh", int(a_ * 32767), int(b_ * 32767)))
            append([0.0] * 1000, [0.0] * 1000)
            s = cap.mark("start", "beep", 1)
            self.assertEqual(s["offset"], 4000)
            append(tone(880, 0.3), [0.0] * int(0.3 * 44100))
            st = cap.mark("stop", "beep", 2, {"expect_hz": 880})
            self.assertEqual(st["start"]["offset"], 4000)
            an = st["analysis"]
            self.assertAlmostEqual(an["dominant_hz"], 880, delta=2)
            self.assertTrue(an["freq_ok"])
            self.assertTrue(an["channels"]["R"]["silent"])
            self.assertEqual(st["wav"], "raw/x/audio-2-beep.wav")
            self.assertTrue(os.path.isfile(os.path.join(d, "audio-2-beep.wav")))
            self.assertEqual(clk.slept, [audio.STOP_SETTLE])
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_wants_audio(self):
        os.makedirs(WORK_TMP, exist_ok=True)
        d = tempfile.mkdtemp(dir=WORK_TMP)
        try:
            a, b = os.path.join(d, "a.adf"), os.path.join(d, "b.adf")
            with open(a, "wb") as fh:
                fh.write(b"\0" * 100 + b"audio_mark\0" + b"\0" * 100)
            with open(b, "wb") as fh:
                fh.write(b"\0" * 211)
            self.assertTrue(audio.wants_audio(a))
            self.assertFalse(audio.wants_audio(b))

            class O:
                audio = "auto"
            self.assertTrue(R.audio_wanted(O, {}, a))
            self.assertFalse(R.audio_wanted(O, {}, b))
            self.assertTrue(R.audio_wanted(O, {"audio": True}, b))
            O.audio = "off"
            self.assertFalse(R.audio_wanted(O, {}, a))
        finally:
            shutil.rmtree(d, ignore_errors=True)


class FailDetailChanged(unittest.TestCase):
    """A failing case whose only differences are under ctx (e.g. addresses
    after a disk rebuild) is fail_detail_changed, not a regression."""
    ENTRY = {"id": "e1", "options": {"cpu_model": "68020", "cachesize": "0"}}

    def fp(self, case_list):
        from tests.test_m2 import fp_of
        return fp_of(case_list)

    def diff(self, before, after):
        base = B.build({"e1": {"entry": self.ENTRY, "statuses": ["pass"] * 3,
                               "runs": [self.fp(before)] * 3, "meta": {}}}, {"name": "t"})
        return B.diff(base, {"e1": {"entry": self.ENTRY, "statuses": ["pass"], "runs": [self.fp(after)],
                                    "meta": {}}})

    def test_only_ctx_changed(self):
        f = {"case": "x", "st": "fail", "expect": 1, "got": 2, "why": "w", "in": {"a": 1}, "ctx": {"pc": "00001000"}}
        g = dict(f, ctx={"pc": "00002000", "extra": 1})
        d = self.diff([f], [g])
        self.assertEqual([x["kind"] for x in d["findings"]], ["fail_detail_changed"])
        self.assertEqual(d["regressions"], 0)
        self.assertIn("only ctx changed", d["findings"][0]["why"])

    def test_got_changed_is_a_regression(self):
        f = {"case": "x", "st": "fail", "expect": 1, "got": 2, "why": "w", "ctx": {"pc": "1"}}
        g = dict(f, got=3, ctx={"pc": "2"})
        d = self.diff([f], [g])
        self.assertEqual([x["kind"] for x in d["findings"]], ["regression"])
        self.assertEqual(d["regressions"], 1)

    def test_known_bug_with_new_values(self):
        f = {"case": "x", "st": "fail", "expect": 1, "got": 2, "why": "w"}
        base = B.build({"e1": {"entry": self.ENTRY, "statuses": ["pass"] * 3,
                               "runs": [self.fp([f])] * 3, "meta": {}}}, {"name": "t"})
        d = B.diff(base, {"e1": {"entry": self.ENTRY, "statuses": ["pass"], "runs": [self.fp([dict(f, got=3)])],
                                 "meta": {}}}, {"cpu.x": {"ref": "bug#7"}})
        self.assertEqual([x["kind"] for x in d["findings"]], ["fail_detail_changed"])
        self.assertIn("bug#7", d["findings"][0]["why"])
        self.assertEqual(d["regressions"], 0)

    def test_pass_with_ctx_change_is_a_regression(self):
        f = {"case": "x", "st": "pass", "ctx": {"pc": "1"}}
        d = self.diff([f], [dict(f, ctx={"pc": "2"})])
        self.assertEqual([x["kind"] for x in d["findings"]], ["regression"])


if __name__ == "__main__":
    unittest.main()


class Catalogue(unittest.TestCase):
    TEXT = """
# comment
- id: a.one
  area: A
  what: 'first # not a comment'
  cases: [cpu_basic.*, flags.x*]
- id: a.two
  area: A
  what: second
  cases: [rtg.*]
  entries: [a4000-rtg-*]
- id: a.three
  area: A
  what: third
  not_covered: "no hardware interface"
- id: a.four
  area: A
  what: four
  cases: [paula.*]
  partial: only part
"""

    def test_parse_and_status(self):
        from amitorture import catalogue
        items = catalogue.parse(self.TEXT)
        self.assertEqual([i["id"] for i in items], ["a.one", "a.two", "a.three", "a.four"])
        self.assertEqual(items[0]["what"], "first # not a comment")
        self.assertEqual(items[0]["cases"], ["cpu_basic.*", "flags.x*"])
        results = [{"id": "e1", "status": "pass"}, {"id": "a4000-rtg-x", "status": "pass"}]
        fps = {"e1": {"cases": {"cpu_basic.add": {"st": "pass"}, "paula.aud0": {"st": "pass"}}},
               "a4000-rtg-x": {"cases": {"rtg.vram": {"st": "skip"}}}}
        rows = {r["id"]: r for r in catalogue.coverage(items, results, fps)}
        self.assertEqual(rows["a.one"]["status"], "covered")
        self.assertEqual(rows["a.two"]["status"], "partial")         # only skip on the matching entry
        self.assertEqual(rows["a.three"]["status"], "not_covered")
        self.assertEqual(rows["a.three"]["why"], "no hardware interface")
        self.assertEqual(rows["a.four"]["status"], "partial")
        rows = {r["id"]: r for r in catalogue.coverage(items, results[:1], {"e1": fps["e1"]})}
        self.assertEqual(rows["a.two"]["status"], "not_covered")     # no a4000-rtg-* entry ran
        self.assertIn("a4000-rtg-*", rows["a.two"]["why"])

    def test_bad_lines(self):
        from amitorture import catalogue
        with self.assertRaises(ValueError):
            catalogue.parse("- id: x\n  bogus: 1\n")
        with self.assertRaises(ValueError):
            catalogue.parse("- id: x\n  area: y\n")                  # neither cases nor not_covered

    def test_shipped_catalogue(self):
        from amitorture import catalogue, matrix
        items = catalogue.load()
        self.assertGreater(len(items), 100)
        import fnmatch
        ids = set(matrix.all_entries())
        for it in items:
            pats = it.get("entries") or []
            if pats:
                self.assertTrue(any(fnmatch.fnmatchcase(i, p) for i in ids for p in pats),
                                f"{it['id']}: {pats} match no matrix entry")


class CdImage(unittest.TestCase):
    def adf(self):
        """A fake baked ADF: header + image with the CLI marker + relocs."""
        image = bytearray(64) + b"AMT-CLI-HEADER!!" + bytes(512) + bytearray(16)
        struct.pack_into(">I", image, 8, 0x20)               # a relocated pointer
        relocs = [8]
        hdr = bytearray(512)
        hdr[:4] = b"AMTP"
        struct.pack_into(">6I", hdr, 4, 1, len(image), 100, len(relocs), 0x1e, 3)
        hdr[64:68] = b"PARM"
        blob = bytes(image) + struct.pack(">I", 8)
        adf = bytearray(901120)
        adf[1024:1536] = hdr
        adf[1536:1536 + len(blob)] = blob
        return bytes(adf), bytes(hdr), len(image)

    def test_hunk_exe(self):
        from amitorture import cdimage
        adf, hdr, n = self.adf()
        exe, info = cdimage.hunk_exe(adf)
        self.assertEqual(struct.unpack_from(">5I", exe, 0), (0x3F3, 0, 1, 0, 0))
        longs = struct.unpack_from(">I", exe, 20)[0]
        self.assertEqual(longs * 4, (n + 100 + 3) // 4 * 4)
        self.assertEqual(struct.unpack_from(">2I", exe, 24), (0x3E9, longs))
        code = exe[32:32 + longs * 4]
        self.assertEqual(code[80:80 + 512], hdr)              # header copied after the marker
        self.assertEqual(code[n:], bytes(len(code) - n))      # BSS zeroed
        rel = 32 + longs * 4
        self.assertEqual(struct.unpack_from(">5I", exe, rel), (0x3EC, 1, 0, 8, 0))
        self.assertEqual(struct.unpack_from(">I", exe, rel + 20)[0], 0x3F2)

    def test_iso_layout(self):
        from amitorture import cdimage
        adf, _, _ = self.adf()
        files, _ = cdimage.boot_files(adf, tm={"CD32.TM": b"x" * 2048})
        iso = cdimage.build_iso(files)
        self.assertEqual(len(iso) % 2048, 0)
        self.assertGreaterEqual(len(iso), 16384)
        self.assertEqual(iso[16 * 2048 + 8:16 * 2048 + 40], b"CDTV".ljust(32))
        listing = {p: (lba, size) for p, lba, size in cdimage.list_iso(iso)}
        self.assertEqual(set(listing), {"AMITORTURE", "CD32.TM", "S/STARTUP-SEQUENCE"})
        lba, size = listing["S/STARTUP-SEQUENCE"]
        self.assertEqual(iso[lba * 2048:lba * 2048 + size], b"AMITORTURE\n")
        lba, size = listing["CD32.TM"]
        self.assertEqual(iso[lba * 2048:lba * 2048 + size], b"x" * 2048)

    def test_lzss(self):
        from amitorture import cdimage
        # "abcabcabc!": literals a b c, then a back reference (offset 3, length 6), literal !
        stream = bytes([0b00010111]) + b"abc" + bytes([3, 6 - 3]) + b"!"
        blob = struct.pack(">IHHII", 0, 1, 0, 10, len(stream)) + stream
        self.assertEqual(cdimage.lzss_unpack(blob), b"abcabcabc!")
        self.assertIsNone(cdimage.lzss_unpack(blob[:-1]))     # truncated: wrong size

    def test_no_trademark_rom_is_an_error(self):
        from amitorture import cdimage
        self.assertEqual(cdimage.trademarks([]), {})


class HardwareEntries(unittest.TestCase):
    def test_boards_and_mmu(self):
        from amitorture import matrix
        std = {e["id"]: e for e in matrix.expand("standard")}
        full = {e["id"]: e for e in matrix.expand("full")}
        for name, _, _ in matrix.RTG_CARDS:
            eid = f"a4000-rtg-{matrix._slug(name)}"
            self.assertIn(eid, std)
            self.assertIn(eid, full)
            self.assertEqual(std[eid]["options"]["gfxcard_type"], name)
        self.assertEqual(std["a4000-rtg-picassoivz3"]["board_roms"][0]["ids"], [91])
        for eid in ("a4000-net-a2065", "a600-68000-compat", "a500plus-rtc", "a4000-snd-toccata", "a4000-z2z3-rtg",
                    "cd32-68ec020-fast", "cdtv-68000-ce", "a4000-68ec040mmuec-fast", "a4000-68060-compat"):
            self.assertIn(eid, std)
            self.assertIn(eid, full)
        self.assertEqual(std["cd32-68ec020-fast"]["boot"], "cd")
        self.assertNotIn("a4000-net-ariadne", std)
        self.assertIn("a4000-net-ariadne", full)
        for e in full.values():                               # no JIT twin with an MMU
            if e.get("jit"):
                self.assertEqual(e["options"]["mmu_model"], "0", e["id"])

    def test_disk_groups_and_repeated_keys(self):
        from amitorture import amiberry_cfg, matrix
        disks = [("1-core", "a"), ("2-cpu", "b"), ("5-exp", "c")]
        e = matrix.find("a4000-rtg-cv64z3")
        self.assertEqual([d[0] for d in R.entry_disks({"disks": disks}, e)], ["1-core", "5-exp"])
        self.assertEqual(R.entry_disks({"disks": disks[1:2]}, e), [])
        e2 = dict(e, disk_only="5-exp")                       # narrowing wins
        self.assertEqual([d[0] for d in R.entry_disks({"disks": disks}, e2)], ["5-exp"])
        from amitorture import media
        s = matrix.find("a600-68000-compat")
        extra = {"hardfile2": "rw,AMT0:/m/x.hdf,0,0,0,512,-128,,ide0", **media.pcmcia_sram_options("/m/c.img")}
        _, text = amiberry_cfg.build(s, rom_path="k", floppy="f", serial_port=1, media_dir="/m", extra=extra)
        lines = [ln for ln in text.splitlines() if ln.startswith("hardfile2=")]
        self.assertEqual(len(lines), 2)
        self.assertIn("custom0_pcmciasram", lines[1])


class AudioExpectation(unittest.TestCase):
    def test_from_start_args(self):
        self.assertAlmostEqual(audio.expected_hz({"clock": 3546895, "period": 254, "samples": 32}),
                               3546895 / (254 * 32))
        self.assertEqual(audio.expected_hz({"freq_mhz": 440000}), 440.0)
        a = audio.merged_args({"freq_mhz": 440000, "side": "left"}, {"edge": "stop"})
        self.assertEqual(a["expect_hz"], 440.0)
        self.assertEqual(a["side"], "left")
        r = audio.analyse([tone(440, 0.2), [0.0] * 8820], 44100, a["expect_hz"])
        audio.side_check(r, a["side"])
        self.assertTrue(r["freq_ok"])
        self.assertTrue(r["side_ok"])
        audio.side_check(r, "right")
        self.assertFalse(r["side_ok"])
        # a slow DC step on the other channel is not the tone
        left = tone(440, 0.2)
        right = [0.2 * math.exp(-i / 2000.0) for i in range(len(left))]
        r = audio.analyse([left, right], 44100, 440.0)
        audio.side_check(r, "left", [left, right])
        self.assertTrue(r["side_ok"], r.get("side_amp"))


class Cd32Pad(unittest.TestCase):
    def test_released_only(self):
        os.makedirs(WORK_TMP, exist_ok=True)
        ipc = FakeIPC()
        rq = hostreq.Requests(ipc, WORK_TMP, options={"joyport1": "kbd1", "joyport1mode": "cd32joy"})
        rq.offer(req(1, 1, "cd32pad", args={"buttons": 0}), "serial")
        rq.offer(req(2, 2, "cd32pad", args={"buttons": 4}), "serial")
        made = rq.service(0x100)
        self.assertEqual([h["acked"] for h in made], [True, False])
        self.assertEqual(ipc.mem, {0x100 + 44: 1})
        rq2 = hostreq.Requests(FakeIPC(), WORK_TMP, options={})
        rq2.offer(req(1, 1, "cd32pad", args={"buttons": 0}), "serial")
        self.assertFalse(rq2.service(0x100)[0]["acked"])

    def test_cd32_entry_has_the_pad(self):
        from amitorture import matrix
        o = matrix.find("cd32-68ec020-fast")["options"]
        self.assertEqual((o["joyport1"], o["joyport1mode"]), ("kbd1", "cd32joy"))


class Debox(unittest.TestCase):
    def test_column_rle_and_nested(self):
        from amitorture import cdimage
        # method 2, stride 2, 4 bytes out: column 0 gets (1, 3), column 1 (2, 4)
        rle = struct.pack(">H", 2) + bytes([2, 1, 3, 0xfe, 9])      # 2 literals, then 9 twice
        blob = struct.pack(">IHHII", 0, 2, 0, 4, len(rle)) + rle
        self.assertEqual(cdimage.debox_unpack(blob)[0], bytes([1, 9, 3, 9]))
        # flag 0x4000: LZSS-coded inner stream (all literals) that is itself a method-2 chunk
        inner = blob
        lz = b""
        for i in range(0, len(inner), 8):
            part = inner[i:i + 8]
            lz += bytes([(1 << len(part)) - 1]) + part
        data = struct.pack(">I", len(inner)) + lz
        outer = struct.pack(">IHHII", 0, 0x4001, 0, 4, len(data)) + data
        out, end = cdimage.debox_unpack(outer)
        self.assertEqual(out, bytes([1, 9, 3, 9]))
        self.assertEqual(end, 16 + len(data) + (len(data) & 1))
        self.assertEqual(cdimage.app_use_tags(22152, 30)[:4], b"TM\x00\x14")


class PcmciaSram(unittest.TestCase):
    def test_image_and_prepare(self):
        from amitorture import media
        os.makedirs(WORK_TMP, exist_ok=True)
        d = tempfile.mkdtemp(dir=WORK_TMP)
        try:
            p = os.path.join(d, "s.img")
            info = media.create_pcmcia_sram(p)
            self.assertEqual(os.path.getsize(p), media.SRAM_BYTES + media.SRAM_ATTR)
            v = media.verify(p)
            self.assertTrue(v["ok"], v)
            self.assertEqual(v["first"]["kind"], "pcmcia_sram")
            with open(p, "rb") as fh:
                fh.seek(media.SRAM_BYTES)
                attr = fh.read(16)
            self.assertEqual(attr[0], 0x01)            # CISTPL_DEVICE on an even address
            self.assertEqual(attr[2 * 5], 0x41)        # then CISTPL_FORMAT
            self.assertEqual(info["blocks"], 8192)
            out = media.prepare(entry={"options": {"chipset_compatible": "A600"}}, media_dir=d)
            self.assertIn("hardfile2#pcmcia", out["options"])
            self.assertIn("hardfile2", out["options"])  # the Gayle IDE hardfile too
            out = media.prepare(entry={"options": {"chipset_compatible": "A500"}}, media_dir=d)
            self.assertNotIn("hardfile2#pcmcia", out["options"])
        finally:
            shutil.rmtree(d, ignore_errors=True)


class ScreenshotFingerprint(unittest.TestCase):
    def test_only_cycle_exact(self):
        from amitorture import cases
        guest = [req(1, 1, "screenshot", tag="bars")]
        m = records.merge({"serial": [records.parse_line(records.encode(r)) for r in guest]})
        hostreq.attach(m, [{"t": "host", "id": 1, "what": "screenshot", "tag": "bars", "acked": True,
                            "result": {"sha256": "ab" * 32}, "req_seq": 1, "req_t": "request"}])
        ce = cases.extract({"options": {"cycle_exact": "true"}, "_merged": m})
        self.assertEqual(ce["cases"]["host.shot_bars"]["rec"]["sha256"], "ab" * 32)
        self.assertTrue(ce["cases"]["host.shot_bars"]["pseudo"])
        fast = cases.extract({"options": {"cycle_exact": "false"}, "_merged": m})
        self.assertNotIn("host.shot_bars", fast["cases"])


class HrmTiming(unittest.TestCase):
    def test_only_cycle_exact_and_only_differences(self):
        from amitorture import analysis
        fp = {"cases": {"dmacontention.bpl_hires4": {"mod": "dmacontention", "case": "bpl_hires4", "st": "pass",
                                                     "timing": {"timing": {"in_fetch": 3, "hrm_in_fetch": 0,
                                                                           "busy": 5, "hrm_busy": 5}}}}}
        res = [{"id": "ce", "options": {"cycle_exact": "true"}}, {"id": "fast", "options": {"cycle_exact": "false"}}]
        items = analysis.hrm_timing_items(res, {"ce": fp, "fast": fp})
        self.assertEqual(len(items), 1)
        it = items[0]
        self.assertEqual((it["entry"], it["path"], it["hrm"], it["measured"]), ("ce", "timing.in_fetch", 0, 3))
        self.assertEqual(it["confidence"], "low")
        from amitorture import problems
        items[0]["id"] = "F001"
        probs = problems.group(items, res, None)
        self.assertEqual(probs[0]["severity"], "low")


class Diagnosis(unittest.TestCase):
    def test_logmine_windows_from_serial_times(self):
        from amitorture import logmine
        os.makedirs(WORK_TMP, exist_ok=True)
        d = tempfile.mkdtemp(dir=WORK_TMP)
        try:
            lines = [records.encode({"v": 1, "seq": 0, "t": "mod", "mod": "mmu", "id": 20, "ev": "begin"}),
                     records.encode({"v": 1, "seq": 1, "t": "mod", "mod": "mmu", "id": 20, "ev": "end",
                                     "pass": 0, "fail": 1, "skip": 0}),
                     records.encode({"v": 1, "seq": 2, "t": "mod", "mod": "dispatch", "id": 22, "ev": "begin"}),
                     records.encode({"v": 1, "seq": 3, "t": "mod", "mod": "dispatch", "id": 22, "ev": "end",
                                     "pass": 0, "fail": 1, "skip": 0})]
            data, marks, t = b"", [], 1.0
            for ln in lines:
                data += ln
                marks.append([t, len(data)])
                t += 1.0
            with open(os.path.join(d, "serial.bin"), "wb") as fh:
                fh.write(data)
            with open(os.path.join(d, "serial-times.json"), "w") as fh:
                json.dump(marks, fh)
            with open(os.path.join(d, "amiberry.log"), "w") as fh:
                fh.write("[    1.500] 01-001 [1 2-3/4]: Gary timeout: 00f00000 2 W PC=00001000\n")
                fh.write("[    3.500] JIT: dummy_bank store at 28048641, ignored\n")
                fh.write("[    3.600] JIT: dummy_bank store at 2804863d, ignored\n")
                fh.write("[    3.700] ALSA lib error\n")
            m = logmine.mine(d)
            self.assertTrue(m["exact"])
            self.assertEqual(m["counts"], {"bus_timeout": 1, "dummy_bank": 2})
            x = logmine.for_module(m, "dispatch")
            self.assertEqual(x["groups"][0]["count"], 2)
            self.assertEqual(x["groups"][0]["addresses"], ["28048641", "2804863d"])
            self.assertEqual(logmine.for_module(m, "mmu")["counts"], {"bus_timeout": 1})
        finally:
            shutil.rmtree(d, ignore_errors=True)

    def test_split_by_fields_and_entries(self):
        from amitorture import problems
        def it(i, case, entry, expect):
            return {"id": f"F{i:03d}", "kind": "case_fail", "entry": entry, "mod": "exceptions", "case": case,
                    "case_key": f"exceptions.{case}", "expect": expect, "got": expect, "why": "w"}
        items = [it(1, "chk_w", "a3000", {"addr_off": 0}), it(2, "divu_w_zero", "a3000", {"addr_off": 0}),
                 it(3, "unimpl060_movep_w", "a4000-060", {"vec": 61}),
                 it(4, "unimpl060_cas2_l", "a4000-060", {"vec": 61, "mem": "x"}),
                 it(5, "rte_format", "a3000", {"sr": "2700"})]
        res = [{"id": "a3000", "status": "fail"}, {"id": "a4000-060", "status": "fail"}]
        probs = problems.group(items, res, None)
        sets = sorted(sorted(p["case_keys"]) for p in probs)
        self.assertEqual(sets, [["exceptions.chk_w", "exceptions.divu_w_zero"], ["exceptions.rte_format"],
                                ["exceptions.unimpl060_cas2_l", "exceptions.unimpl060_movep_w"]])

    def test_conditions_and_variants(self):
        from amitorture import diagnose, matrix, narrow
        c = diagnose._named_conditions("Amiberry only traps these with cpu_no_unimplemented=true; "
                                       "with cpu_compatible=false it loses sprites")
        self.assertEqual(c, [("cpu_no_unimplemented", "true", True), ("cpu_compatible", "false", False)])
        e = matrix.find("a3000-68030-882-compat")
        axes = [a for a, _, _ in narrow.module_variants(e, "exceptions")]
        self.assertEqual(axes[0], "mmu_model")
        e = matrix.find("a1200-68ec020-fast")
        labels = [(a, l) for a, l, _ in narrow.module_variants(e, "display")]
        self.assertIn(("gfx_resolution", "superhires"), labels)
        self.assertNotIn(("gfx_resolution", "hires"), labels)     # the current value is no variant

    def test_passes_diff_separates(self):
        from amitorture import diagnose
        results = [{"id": "ecs", "status": "fail", "options": {"chipset": "ecs", "cachesize": "0"}},
                   {"id": "aga1", "status": "pass", "options": {"chipset": "aga", "cachesize": "0"}},
                   {"id": "aga2", "status": "pass", "options": {"chipset": "aga", "cachesize": "8192"}},
                   {"id": "ocs", "status": "pass", "options": {"chipset": "ocs", "cachesize": "0"}}]
        fps = {"ecs": {"cases": {"display.m": {"st": "fail"}}}, "aga1": {"cases": {"display.m": {"st": "pass"}}},
               "aga2": {"cases": {"display.m": {"st": "pass"}}}, "ocs": {"cases": {"display.m": {"st": "skip"}}}}
        ctx = diagnose.Ctx(results, fps, [], None)
        out = diagnose.passes_diff({"case_keys": ["display.m"], "entries": ["ecs"], "mod": "display"}, ctx)
        self.assertEqual(out["passes_on"], ["aga1", "aga2"])
        self.assertEqual(out["not_run_or_skipped_on"], ["ocs"])
        sep = {x["option"]: x for x in out["separating"]}
        self.assertEqual(sep["chipset"]["failing"], ["ecs"])
        self.assertTrue(sep["chipset"]["affects_module"])
        self.assertNotIn("cachesize", sep)

    def test_abbreviated_case_ids_in_docs(self):
        from amitorture import problems
        d = problems.case_doc("exceptions", "unimpl060_cas2_l")
        if d is None or d.get("line") is None:
            self.skipTest("docs/cases/exceptions.md has no abbreviated row")
        self.assertIn("_cas2_l", d["text"])
