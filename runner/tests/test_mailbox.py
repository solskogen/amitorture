"""Mailbox scan and ring follower (CONTRACT.md §7) against a fake memory."""
import os
import struct
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
from amitorture import mailbox, records  # noqa: E402


class FakeMem:
    def __init__(self, size):
        self.mem = bytearray(size)
        self.reads = 0
        self.hook = None  # called after each batch (simulates the guest writing meanwhile)

    def put(self, addr, data):
        self.mem[addr:addr + len(data)] = data

    def read_longs(self, addrs):
        self.reads += len(addrs)
        out = []
        for a in addrs:
            out.append(struct.unpack(">I", bytes(self.mem[a:a + 4]))[0] if 0 <= a <= len(self.mem) - 4 else None)
        if self.hook:
            self.hook()
        return out


class Guest:
    """Writes records into a mailbox the way disk/rt/out.c does."""

    def __init__(self, mem, addr, ring_size):
        self.mem, self.addr, self.ring = mem, addr, ring_size
        self.total = 0
        self.hb = 0
        mem.put(addr, mailbox.build_image([], ring_size=ring_size, state=1, heartbeat=0))

    def write(self, line):
        for i, b in enumerate(line):
            self.mem.mem[self.addr + 48 + (self.total + i) % self.ring] = b
        self.total += len(line)
        self.hb += 1
        self.mem.put(self.addr + 24, struct.pack(">II", self.hb, self.total))

    def state(self, s):
        self.mem.put(self.addr + 20, struct.pack(">I", s))


def recs(n, start=0):
    return [records.encode({"v": 1, "seq": i, "t": "log", "msg": f"message {i:04d}"}) for i in range(start, start + n)]


class Scan(unittest.TestCase):
    def test_finds_aligned_mailbox(self):
        mem = FakeMem(0x80000)
        mem.put(0x3ec0, mailbox.build_image(recs(3)))
        self.assertEqual(mailbox.scan(mem, 0, 0x80000), [0x3ec0])

    def test_ignores_bare_magic_string(self):
        # The magic string as a constant in the payload's data, with no header after it.
        mem = FakeMem(0x80000)
        mem.put(0x1000, mailbox.MAGIC + b"\xff" * 32)
        mem.put(0x5000, mailbox.build_image(recs(1)))
        self.assertEqual(mailbox.scan(mem, 0, 0x80000), [0x5000])

    def test_ring_past_end_is_rejected(self):
        mem = FakeMem(0x2000)
        mem.put(0x1800, mailbox.build_image([], ring_size=4096)[:0x800])
        self.assertEqual(mailbox.scan(mem, 0, 0x2000), [])

    def test_reads_one_long_per_step(self):
        mem = FakeMem(0x10000)
        mailbox.scan(mem, 0, 0x10000)
        self.assertEqual(mem.reads, 0x10000 // 16)


class Follow(unittest.TestCase):
    def test_incremental_and_wrap(self):
        mem = FakeMem(0x20000)
        g = Guest(mem, 0x4000, 256)
        f = mailbox.RingFollower(mem, 0x4000)
        sp = records.StreamParser("mailbox")
        all_lines = recs(40)
        for i, line in enumerate(all_lines):
            g.write(line)
            if i % 3 == 0:  # poll often enough that nothing is overwritten
                r = f.poll()
                self.assertEqual(r["lost"], 0)
                sp.feed(r["data"])
        sp.feed(f.poll()["data"])
        self.assertEqual([r.seq for r in sp.records], list(range(40)))
        self.assertTrue(all(r.crc_ok for r in sp.records))
        self.assertGreater(g.total, 256)  # it did wrap

    def test_overrun_reported(self):
        mem = FakeMem(0x20000)
        g = Guest(mem, 0x4000, 128)
        f = mailbox.RingFollower(mem, 0x4000)
        for line in recs(10):
            g.write(line)
        r = f.poll()
        self.assertEqual(r["lost"], g.total - 128)
        self.assertEqual(len(r["data"]), 128)
        sp = records.StreamParser("mailbox")
        sp.lose(r["lost"], r["lost_at"], "overrun")
        sp.feed(r["data"])
        sp.close()
        good = [x.seq for x in sp.records if x.crc_ok]
        self.assertTrue(good and good[-1] == 9)
        self.assertNotIn(0, good)

    def test_guest_writes_during_read(self):
        """Bytes overwritten while we read them must not be trusted."""
        mem = FakeMem(0x20000)
        g = Guest(mem, 0x4000, 128)
        f = mailbox.RingFollower(mem, 0x4000)
        for line in recs(2):
            g.write(line)
        more = iter(recs(6, start=2))

        def hook():
            line = next(more, None)
            if line:
                g.write(line)
        mem.hook = hook
        r = f.poll()
        mem.hook = None
        sp = records.StreamParser("mailbox")
        if r["lost"]:
            sp.lose(r["lost"], r["lost_at"], "torn")
        sp.feed(r["data"])
        r2 = f.poll()
        if r2["lost"]:
            sp.lose(r2["lost"], r2["lost_at"], "overrun")
        sp.feed(r2["data"])
        sp.close()
        # Whatever survived must be CRC-clean and in order.
        good = [x for x in sp.records if x.crc_ok]
        self.assertTrue(good)
        self.assertEqual([x.seq for x in good], sorted(x.seq for x in good))

    def test_vanished(self):
        mem = FakeMem(0x20000)
        Guest(mem, 0x4000, 128)
        f = mailbox.RingFollower(mem, 0x4000)
        mem.put(0x4000, bytes(16))
        self.assertIsNone(f.poll())

    def test_header_fields(self):
        mem = FakeMem(0x20000)
        g = Guest(mem, 0x4000, 128)
        g.write(recs(1)[0])
        g.state(4)
        h = mailbox.read_header(mem, 0x4000)
        self.assertEqual(h.state_name, "halted-after-done")
        self.assertEqual(h.total, len(recs(1)[0]))
        self.assertEqual(h.ring_size, 128)


if __name__ == "__main__":
    unittest.main()
