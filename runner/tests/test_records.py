"""Record parsing, CRC checking and merging (CONTRACT.md §5). Run from runner/:
    python3 -m unittest discover -s tests
"""
import os
import sys
import unittest
import zlib

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from amitorture import records  # noqa: E402


def rec(seq, t="log", **kw):
    if t == "log" and "msg" not in kw:
        kw["msg"] = f"m{seq}"
    return records.encode({"v": 1, "seq": seq, "t": t, **kw})


class ParseLine(unittest.TestCase):
    def test_good_line(self):
        line = rec(0, "log", msg="hi")
        r = records.parse_line(line, "serial", 10)
        self.assertTrue(r.crc_ok)
        self.assertEqual(r.obj["msg"], "hi")
        self.assertEqual(r.seq, 0)
        self.assertEqual(r.offset, 10)
        self.assertEqual(r.problems, [])

    def test_crc_is_over_exact_json_bytes(self):
        js = b'{"v":1,"seq":3,"t":"log","msg":"x"}'
        line = js + b" *" + f"{zlib.crc32(js):08x}".encode()
        self.assertTrue(records.parse_line(line).crc_ok)
        # Same JSON value, different bytes (a space) -> CRC no longer matches.
        js2 = b'{"v":1, "seq":3,"t":"log","msg":"x"}'
        r = records.parse_line(js2 + line[len(js):])
        self.assertFalse(r.crc_ok)
        self.assertIsNotNone(r.obj)

    def test_bad_crc(self):
        line = bytearray(rec(1))
        line[10] ^= 0x01  # flip a bit inside the JSON
        r = records.parse_line(bytes(line))
        self.assertFalse(r.crc_ok)
        self.assertTrue(any("bad crc" in p for p in r.problems))

    def test_short_crc_rejected(self):
        line = rec(1).rstrip(b"\n")
        self.assertFalse(records.parse_line(line[:-1]).crc_ok)

    def test_uppercase_crc_tolerated(self):
        line = rec(1).rstrip(b"\n")
        js, crc = line.rsplit(b" *", 1)
        self.assertTrue(records.parse_line(js + b" *" + crc.upper()).crc_ok)

    def test_cr_tolerated(self):
        r = records.parse_line(rec(0).rstrip(b"\n") + b"\r\n")
        self.assertTrue(r.crc_ok)

    def test_noise_is_not_a_record(self):
        self.assertIsNone(records.parse_line(b"ROMInfo: 1MiB ROM detected\r"))
        self.assertIsNone(records.parse_line(b""))
        self.assertIsNone(records.parse_line(b"{no star here}"))

    def test_junk_before_record(self):
        r = records.parse_line(b"\x00\xffgarbage" + rec(2))
        self.assertTrue(r.crc_ok)
        self.assertTrue(any("junk" in p for p in r.problems))

    def test_unparsable_json_with_good_crc(self):
        js = b'{"v":1,"seq":'
        r = records.parse_line(js + b" *" + records.crc32_hex(js).encode())
        self.assertTrue(r.crc_ok)
        self.assertIsNone(r.obj)
        self.assertTrue(any(p.startswith("json") for p in r.problems))

    def test_schema(self):
        self.assertEqual(records.validate({"v": 1, "seq": 0, "t": "log", "msg": "x"}), [])
        self.assertTrue(records.validate({"v": 2, "seq": 0, "t": "log", "msg": "x"}))
        self.assertTrue(records.validate({"v": 1, "seq": 0, "t": "nope"}))
        self.assertTrue(records.validate({"v": 1, "seq": 0, "t": "case", "mod": "m", "case": "c", "st": "fail"}))
        self.assertEqual(records.validate({"v": 1, "seq": 0, "t": "case", "mod": "m", "case": "c", "st": "fail",
                                           "expect": 1, "got": 2, "why": "w"}), [])
        self.assertTrue(records.validate({"v": 1, "seq": 0, "t": "mod", "mod": "m", "ev": "end"}))
        self.assertEqual(records.validate({"v": 1, "seq": 0, "t": "mod", "mod": "m", "ev": "end",
                                           "pass": 1, "fail": 0, "skip": 0}), [])


class Streams(unittest.TestCase):
    def test_chunked_feed(self):
        data = b"".join(rec(i) for i in range(5))
        sp = records.StreamParser("serial")
        for i in range(0, len(data), 7):
            sp.feed(data[i:i + 7])
        sp.close()
        self.assertEqual([r.seq for r in sp.records], [0, 1, 2, 3, 4])
        self.assertTrue(all(r.crc_ok for r in sp.records))

    def test_noise_counted(self):
        sp = records.parse_bytes(b"ROMInfo: x\r\nROMInfo: y\r\n" + rec(0) + b"\r\n", "serial")
        self.assertEqual(sp.noise_lines, 2)
        self.assertEqual(len(sp.records), 1)

    def test_partial_last_line(self):
        sp = records.parse_bytes(rec(0) + rec(1)[:-5], "serial")
        self.assertEqual(len(sp.records), 2)
        self.assertFalse(sp.records[1].crc_ok)

    def test_lost_bytes_drop_partial_line(self):
        sp = records.StreamParser("mailbox")
        sp.feed(rec(0) + rec(1)[:10])
        sp.lose(100, 0, "overrun")
        sp.feed(rec(1)[20:] + rec(2))
        sp.close()
        # The torn line after the loss is either dropped or flagged bad; seq 0 and 2 survive.
        good = [r.seq for r in sp.records if r.crc_ok]
        self.assertEqual(good, [0, 2])

    def test_boot_detection(self):
        sp = records.parse_bytes(rec(0) + rec(1) + rec(0) + rec(1), "serial")
        self.assertEqual([r.boot for r in sp.records], [0, 0, 1, 1])

    def test_new_boot_without_hello(self):
        # --resets: the runner resets the emulator; the next mailbox ring has
        # wrapped before it is found, so its hello (seq 0) is lost
        sp = records.StreamParser("mailbox")
        sp.feed(rec(0) + rec(1) + rec(2) + rec(3)[:10])   # a partial line of the old boot
        sp.new_boot(1)
        sp.feed(rec(2) + rec(3))
        self.assertEqual([(r.boot, r.seq) for r in sp.records if r.crc_ok], [(0, 0), (0, 1), (0, 2), (1, 2), (1, 3)])

    def test_new_boot_then_hello_counts_once(self):
        sp = records.StreamParser("mailbox")
        sp.feed(rec(0) + rec(1))
        sp.new_boot(1)
        sp.feed(rec(0) + rec(1))
        self.assertEqual([r.boot for r in sp.records], [0, 0, 1, 1])

    def test_new_boot_before_any_record(self):
        # the mailbox was never found on the first boot: its first records are
        # still the second boot's, as on serial
        sp = records.StreamParser("mailbox")
        sp.new_boot(1)
        sp.feed(rec(0))
        self.assertEqual([r.boot for r in sp.records], [1])

    def test_reset_boots_merge_with_serial(self):
        # serial has both boots; the mailbox lost the second boot's hello
        serial = records.parse_bytes(rec(0) + rec(1) + rec(2) + rec(0) + rec(1) + rec(2), "serial").records
        sp = records.StreamParser("mailbox")
        sp.feed(rec(0) + rec(1) + rec(2))
        sp.new_boot(1)
        sp.feed(rec(1) + rec(2))
        m = records.merge({"serial": serial, "mailbox": sp.records})
        self.assertEqual([(x["boot"], x["seq"]) for x in m.records], [(0, 0), (0, 1), (0, 2), (1, 0), (1, 1), (1, 2)])
        self.assertEqual(m.schema_problems, [])
        self.assertEqual([x["sources"] for x in m.records if x["boot"] == 1 and x["seq"] > 0],
                         [["serial", "mailbox"], ["serial", "mailbox"]])


class Merge(unittest.TestCase):
    def parse(self, data, src):
        return records.parse_bytes(data, src).records

    def test_dedup_across_transports(self):
        lines = [rec(i) for i in range(4)]
        m = records.merge({"serial": self.parse(b"".join(lines), "serial"),
                           "mailbox": self.parse(b"".join(lines), "mailbox")})
        self.assertEqual([x["seq"] for x in m.records], [0, 1, 2, 3])
        self.assertTrue(all(x["sources"] == ["serial", "mailbox"] for x in m.records))
        self.assertEqual(m.gaps, [])

    def test_fill_from_other_transport(self):
        lines = [rec(i) for i in range(6)]
        serial = b"".join(lines[:2] + lines[4:])          # serial lost 2,3
        mbox = b"".join(lines[1:5])                       # mailbox missed 0 and 5
        m = records.merge({"serial": self.parse(serial, "serial"), "mailbox": self.parse(mbox, "mailbox")})
        self.assertEqual([x["seq"] for x in m.records], list(range(6)))
        self.assertEqual(m.gaps, [])
        self.assertEqual(m.records[2]["sources"], ["mailbox"])

    def test_gap_flagged(self):
        m = records.merge({"serial": self.parse(rec(0) + rec(1) + rec(4) + rec(5), "serial")})
        self.assertEqual(m.gaps, [{"boot": 0, "from": 2, "to": 3, "count": 2}])

    def test_gap_at_start(self):
        m = records.merge({"serial": self.parse(rec(2) + rec(3), "serial")})
        self.assertEqual(m.gaps[0]["from"], 0)

    def test_bad_crc_never_trusted_but_filled(self):
        good = rec(1)
        bad = bytearray(good)
        bad[12] ^= 1
        m = records.merge({"serial": self.parse(rec(0) + bytes(bad) + rec(2), "serial"),
                           "mailbox": self.parse(good, "mailbox")})
        self.assertEqual([x["seq"] for x in m.records], [0, 1, 2])
        self.assertEqual(m.records[1]["sources"], ["mailbox"])
        self.assertEqual(len(m.bad), 1)
        self.assertEqual(m.per_source["serial"]["bad_crc"], 1)

    def test_conflict(self):
        a = records.encode({"v": 1, "seq": 0, "t": "log", "msg": "a"})
        b = records.encode({"v": 1, "seq": 0, "t": "log", "msg": "b"})
        m = records.merge({"serial": self.parse(a, "serial"), "mailbox": self.parse(b, "mailbox")})
        self.assertEqual(len(m.records), 1)
        self.assertEqual(m.records[0]["rec"]["msg"], "a")
        self.assertEqual(len(m.conflicts), 1)

    def test_duplicate_in_one_source(self):
        m = records.merge({"serial": self.parse(rec(0) + rec(1) + rec(1), "serial")})
        self.assertEqual(len(m.records), 2)
        self.assertEqual(m.duplicates, 1)

    def test_schema_problems_kept_but_reported(self):
        line = records.encode({"v": 1, "seq": 0, "t": "case", "mod": "m", "case": "c", "st": "fail"})
        m = records.merge({"serial": self.parse(line, "serial")})
        self.assertEqual(len(m.records), 1)
        self.assertEqual(len(m.schema_problems), 1)

    def test_boots_merge_separately(self):
        data = rec(0) + rec(1) + rec(0) + rec(1)
        m = records.merge({"serial": self.parse(data, "serial"), "mailbox": self.parse(data, "mailbox")})
        self.assertEqual([(x["boot"], x["seq"]) for x in m.records], [(0, 0), (0, 1), (1, 0), (1, 1)])


if __name__ == "__main__":
    unittest.main()
