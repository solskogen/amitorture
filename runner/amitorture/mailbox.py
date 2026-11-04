"""Memory mailbox (CONTRACT.md §7): finding it in chip RAM and streaming its ring.

The memory access is abstract (``reader.read_longs(addresses) -> [int|None]``)
so this works over Amiberry IPC READ_MEM, over a dump file, and in unit tests.
"""
from __future__ import annotations

import struct
import time

MAGIC = b"AMITORTURE-MBOX!"
MAGIC_LONGS = struct.unpack(">4I", MAGIC)
HEADER_SIZE = 48
STATES = {0: "init", 1: "running", 2: "done", 3: "aborted", 4: "halted-after-done"}
MAX_RING = 1 << 20


class Header:
    __slots__ = ("addr", "version", "state", "heartbeat", "total", "ring_size", "mod", "case")

    def __init__(self, addr, longs):
        self.addr = addr
        (self.version, self.state, self.heartbeat, self.total, self.ring_size,
         self.mod, self.case) = longs[:7]

    @property
    def state_name(self):
        return STATES.get(self.state, f"unknown({self.state})")

    def as_dict(self):
        return {"addr": f"{self.addr:08x}", "version": self.version, "state": self.state_name,
                "heartbeat": self.heartbeat, "total": self.total, "ring_size": self.ring_size,
                "mod": self.mod, "case": self.case}


def plausible(longs, addr, mem_top):
    """longs = the 12 header longs after the magic (offset 16..63 is enough)."""
    version, state, _hb, _total, ring_size = longs[:5]
    return (version == 1 and state in STATES and 16 <= ring_size <= MAX_RING and ring_size % 4 == 0
            and addr + HEADER_SIZE + ring_size <= mem_top)


def scan(reader, start: int, end: int, step: int = 16, batch: int = 4096, stats: dict | None = None):
    """Returns the candidate mailbox addresses in [start, end) (magic + plausible header).

    Reads only the first long at each step; reads the rest of the magic and
    the header for the hits only.
    """
    hits = []
    reads = 0
    addrs = list(range(start, end, step))
    first = MAGIC_LONGS[0]
    for i in range(0, len(addrs), batch):
        chunk = addrs[i:i + batch]
        vals = reader.read_longs(chunk)
        reads += len(chunk)
        for a, v in zip(chunk, vals):
            if v == first:
                hits.append(a)
    found = []
    for a in hits:
        vals = reader.read_longs([a + 4 * k for k in range(1, 4)] + [a + 16 + 4 * k for k in range(7)])
        reads += 10
        if any(v is None for v in vals):
            continue
        if tuple([first] + vals[:3]) != MAGIC_LONGS:
            continue
        if plausible(vals[3:], a, end):
            found.append(a)
    if stats is not None:
        stats["reads"] = stats.get("reads", 0) + reads
        stats["first_long_hits"] = stats.get("first_long_hits", 0) + len(hits)
    return found


def read_header(reader, addr):
    vals = reader.read_longs([addr + 4 * k for k in range(4)] + [addr + 16 + 4 * k for k in range(7)])
    if any(v is None for v in vals) or tuple(vals[:4]) != MAGIC_LONGS:
        return None
    return Header(addr, vals[4:])


def read_ring_bytes(reader, addr, ring_size, pos, n):
    """Reads n bytes starting at ring position pos (mod ring_size), wrapping."""
    out = bytearray()
    base = addr + HEADER_SIZE
    # Read whole aligned longs covering the range, then cut.
    while n > 0:
        p = pos % ring_size
        run = min(n, ring_size - p)
        a0 = p & ~3
        a1 = (p + run + 3) & ~3
        vals = reader.read_longs([base + a for a in range(a0, a1, 4)])
        if any(v is None for v in vals):
            raise OSError("ring read failed")
        blob = b"".join(struct.pack(">I", v & 0xffffffff) for v in vals)
        out += blob[p - a0:p - a0 + run]
        pos += run
        n -= run
    return bytes(out)


class RingFollower:
    """Follows one mailbox: each poll() returns the new ring bytes.

    Consistency: the guest may write while we read. We read total (t1), the
    bytes [last, t1), then the header again (t2). Bytes older than
    t2 - ring_size may have been overwritten while reading; they are reported
    as lost instead of being trusted.
    """

    def __init__(self, reader, addr):
        self.reader = reader
        self.addr = addr
        self.pos = 0          # total bytes consumed
        self.header = None
        self.lost = 0

    def poll(self):
        """Returns dict(header=Header, data=bytes, lost=int, lost_at=int) or None if the mailbox vanished."""
        h1 = read_header(self.reader, self.addr)
        if h1 is None:
            return None
        if h1.total < self.pos:
            # total went backwards: the mailbox was re-initialised at the same address.
            self.pos = 0
        lost, lost_at = 0, self.pos
        avail = h1.total - self.pos
        if avail > h1.ring_size:
            lost = avail - h1.ring_size
            self.pos += lost
            avail = h1.ring_size
        data = b""
        if avail:
            data = read_ring_bytes(self.reader, self.addr, h1.ring_size, self.pos, avail)
        h2 = read_header(self.reader, self.addr)
        if h2 is not None and h2.total - h2.ring_size > self.pos:
            over = min(h2.total - h2.ring_size - self.pos, len(data))
            # The first `over` bytes may be torn: drop them.
            data = data[over:]
            lost += over
            self.pos += over
        self.pos += len(data)
        self.lost += lost
        self.header = h2 or h1
        return {"header": self.header, "data": data, "lost": lost, "lost_at": lost_at}


class Watch:
    """Tracks the heartbeat to tell running from hung."""

    def __init__(self):
        self.last_hb = None
        self.last_change = time.monotonic()

    def update(self, hb):
        now = time.monotonic()
        if hb != self.last_hb:
            self.last_hb = hb
            self.last_change = now
        return now - self.last_change


def build_image(records: list[bytes], ring_size=4096, state=2, heartbeat=1, mod=0, case=0, total=None):
    """Builds a mailbox image (header + ring) from encoded record lines.

    Used by the unit tests and by ``run --selftest`` (written into guest
    memory over IPC to check the scan and ring reader end to end).
    """
    data = b"".join(records)
    total = len(data) if total is None else total
    ring = bytearray(ring_size)
    for i, b in enumerate(data[-ring_size:] if len(data) > ring_size else data):
        start = len(data) - min(len(data), ring_size)
        ring[(start + i) % ring_size] = b
    hdr = MAGIC + struct.pack(">8I", 1, state, heartbeat, total, ring_size, mod, case, 0)
    return hdr + bytes(ring)
