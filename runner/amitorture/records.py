"""Record wire format (CONTRACT.md §5): parsing, CRC checking and merging.

A record is one line: ``<json> *<crc8hex>\\n``. The CRC is CRC-32 (zlib) of the
exact JSON bytes. The same lines go to serial and to the mailbox ring; the host
merges both by ``seq`` per boot.
"""
from __future__ import annotations

import json
import zlib
from dataclasses import dataclass, field

SCHEMA_VERSION = 1

# Required fields per record type (§5). hello's probe object is checked loosely.
REQUIRED = {
    "hello": ("build", "mode", "seed", "exec", "cpu", "fpu", "mmu", "probe"),
    "log": ("msg",),
    "mod": ("mod", "ev"),
    "case": ("mod", "case", "st"),
    "capture": ("what", "tag"),
    "perf": ("mod", "ops", "ticks"),
    "done": ("pass", "fail", "skip", "untestable", "aborted", "records"),
    "request": ("id", "what", "tag"),      # host request channel (CONTRACT §13)
}
MOD_END_REQUIRED = ("pass", "fail", "skip")
CASE_FAIL_REQUIRED = ("expect", "got", "why")
CASE_STATES = ("pass", "fail", "skip", "untestable")


def crc32_hex(data: bytes) -> str:
    return f"{zlib.crc32(data) & 0xffffffff:08x}"


def encode(obj: dict) -> bytes:
    """Encodes a record the way the disk does (used by tests and the selftest)."""
    js = json.dumps(obj, separators=(",", ":"), ensure_ascii=True).encode("ascii")
    return js + b" *" + crc32_hex(js).encode() + b"\n"


@dataclass
class Record:
    """One parsed line from one transport."""
    source: str             # "serial" | "mailbox" | "import:<file>"
    raw: bytes              # the line without the trailing newline
    offset: int             # byte offset of the line in that transport's stream
    obj: dict | None = None  # parsed JSON (None if it could not be parsed)
    crc_ok: bool = False
    crc_given: str | None = None
    crc_calc: str | None = None
    problems: list[str] = field(default_factory=list)
    boot: int = 0           # boot index within the source (seq restarts at 0 on reboot)

    @property
    def seq(self):
        if self.obj is not None and isinstance(self.obj.get("seq"), int):
            return self.obj["seq"]
        return None

    @property
    def type(self):
        return None if self.obj is None else self.obj.get("t")

    def to_json(self) -> dict:
        d = {"source": self.source, "offset": self.offset, "boot": self.boot, "crc_ok": self.crc_ok}
        if self.obj is not None:
            d["rec"] = self.obj
        else:
            d["raw"] = self.raw.decode("latin-1")
        if not self.crc_ok:
            d["crc_given"], d["crc_calc"] = self.crc_given, self.crc_calc
        if self.problems:
            d["problems"] = self.problems
        return d


def looks_like_record(line: bytes) -> bool:
    s = line.strip()
    return s.startswith(b"{") and b" *" in s


def parse_line(line: bytes, source: str = "?", offset: int = 0) -> Record | None:
    """Parses one line. Returns None if the line is not a record at all
    (noise), else a Record whose crc_ok / problems say what is wrong."""
    line = line.rstrip(b"\r\n")
    # Tolerate leading garbage before the JSON (e.g. a ROM's debug output on
    # serial without a newline): start at the first '{'.
    start = line.find(b"{")
    if start < 0:
        return None
    star = line.rfind(b" *")
    if star < start:
        return None
    js, crc = line[start:star], line[star + 2:].strip()
    rec = Record(source=source, raw=line[start:], offset=offset + start)
    if start > 0:
        rec.problems.append(f"{start} junk byte(s) before the record")
    rec.crc_given = crc.decode("latin-1")
    rec.crc_calc = crc32_hex(js)
    rec.crc_ok = rec.crc_given.lower() == rec.crc_calc and len(crc) == 8
    if not rec.crc_ok:
        rec.problems.append(f"bad crc: line says {rec.crc_given!r}, data gives {rec.crc_calc}")
    try:
        obj = json.loads(js.decode("ascii"))
        if not isinstance(obj, dict):
            raise ValueError("not an object")
        rec.obj = obj
    except (UnicodeDecodeError, ValueError) as e:
        rec.problems.append(f"json: {e}")
        return rec
    rec.problems.extend(validate(obj))
    return rec


def validate(obj: dict) -> list[str]:
    """Schema problems of a parsed record (empty list if it is fine)."""
    p = []
    if obj.get("v") != SCHEMA_VERSION:
        p.append(f"v={obj.get('v')!r}, expected {SCHEMA_VERSION}")
    if not isinstance(obj.get("seq"), int) or obj["seq"] < 0:
        p.append("missing or bad seq")
    t = obj.get("t")
    if t not in REQUIRED:
        p.append(f"unknown record type {t!r}")
        return p
    missing = [f for f in REQUIRED[t] if f not in obj]
    if t == "mod" and obj.get("ev") == "end":
        missing += [f for f in MOD_END_REQUIRED if f not in obj]
    if t == "mod" and obj.get("ev") not in ("begin", "end"):
        p.append(f"mod.ev={obj.get('ev')!r}")
    if t == "case":
        if obj.get("st") not in CASE_STATES:
            p.append(f"case.st={obj.get('st')!r}")
        if obj.get("st") == "fail":
            missing += [f for f in CASE_FAIL_REQUIRED if f not in obj]
    if missing:
        p.append("missing field(s): " + ",".join(missing))
    return p


class LineSplitter:
    """Turns a byte stream (fed in arbitrary chunks) into lines with offsets."""

    def __init__(self, source: str):
        self.source = source
        self.buf = b""
        self.base = 0  # stream offset of buf[0]

    def feed(self, data: bytes):
        self.buf += data
        out = []
        while True:
            nl = self.buf.find(b"\n")
            if nl < 0:
                break
            out.append((self.base, self.buf[:nl]))
            self.buf = self.buf[nl + 1:]
            self.base += nl + 1
        return out

    def skip(self, n_lost: int):
        """Bytes were lost in the stream: drop the partial line, it is corrupt."""
        self.base += len(self.buf) + n_lost
        self.buf = b""

    def flush(self):
        out = [(self.base, self.buf)] if self.buf.strip() else []
        self.base += len(self.buf)
        self.buf = b""
        return out


class StreamParser:
    """Parses one transport's stream into records + noise, tracking boots."""

    MAX_NOISE_SAMPLES = 20

    def __init__(self, source: str):
        self.source = source
        self.splitter = LineSplitter(source)
        self.records: list[Record] = []
        self.noise_lines = 0
        self.noise_bytes = 0
        self.noise_samples: list[dict] = []
        self.lost: list[dict] = []  # byte ranges known to be lost (e.g. ring overrun)
        self._boot = 0
        self._last_seq = None
        self._resync = False

    def _line(self, off, line):
        rec = parse_line(line, self.source, off)
        if self._resync:
            # first line after a known loss (ring overrun): it normally starts
            # mid-record. Keep it only if it is a complete valid record.
            self._resync = False
            if rec is None or not rec.crc_ok:
                if self.lost:
                    self.lost[-1]["partial_line_dropped"] = len(line)
                return None
        if rec is None:
            if line.strip():
                self.noise_lines += 1
                self.noise_bytes += len(line) + 1
                if len(self.noise_samples) < self.MAX_NOISE_SAMPLES:
                    self.noise_samples.append({"offset": off, "text": line[:200].decode("latin-1")})
            return None
        s = rec.seq
        if s is not None and rec.crc_ok:
            # seq restarts at 0 per boot: a hello with seq 0 after other records is a new boot.
            if self._last_seq is not None and s == 0 and self._last_seq >= 0:
                self._boot += 1
            self._last_seq = s
        rec.boot = self._boot
        self.records.append(rec)
        return rec

    def feed(self, data: bytes):
        return [r for r in (self._line(o, l) for o, l in self.splitter.feed(data)) if r is not None]

    def lose(self, n: int, at: int, why: str):
        self.lost.append({"offset": at, "bytes": n, "why": why})
        self.splitter.skip(n)
        self._resync = n > 0

    def close(self):
        return [r for r in (self._line(o, l) for o, l in self.splitter.flush()) if r is not None]


@dataclass
class Merged:
    records: list[dict]                 # merged, ordered by (boot, seq); each has "rec", "sources"
    gaps: list[dict]                    # missing seq ranges per boot
    bad: list[dict]                     # records rejected (bad crc / unparsable) with source info
    conflicts: list[dict]               # same (boot, seq) with different content on two transports
    duplicates: int                     # identical copies dropped
    schema_problems: list[dict]         # CRC-valid records that break the schema
    per_source: dict                    # source -> counts

    def by_type(self, t):
        return [m["rec"] for m in self.records if m["rec"].get("t") == t]


def bad_unrecovered(merged) -> list:
    """Bad-CRC/unparsable lines whose record did NOT arrive intact on another
    transport. A corrupted serial line is harmless when the mailbox (or the
    other transport) delivered the same record: then there is no seq gap and
    the done record count matches. Lines that parse to a (boot, seq) present
    in the merged table are recovered; if any seq is missing, every bad line
    is treated as unrecovered (it may be the missing one)."""
    if not merged.bad:
        return []
    if merged.gaps:
        return list(merged.bad)
    have = {(m.get("boot"), (m.get("rec") or {}).get("seq")) for m in merged.records}
    out = []
    for b in merged.bad:
        rec = b.get("rec") if isinstance(b, dict) else None
        key = (b.get("boot") if isinstance(b, dict) else None, (rec or {}).get("seq"))
        if rec is None or key not in have:
            # unparsable: recovered only if nothing is missing (no gaps) and
            # the stream ended with a consistent done record
            done = [m["rec"] for m in merged.records if (m.get("rec") or {}).get("t") == "done"]
            n_guest = len([m for m in merged.records if not (m.get("rec") or {}).get("host")
                           and m.get("boot") == merged.records[-1].get("boot")]) if merged.records else 0
            if not (done and done[-1].get("records") == n_guest):
                out.append(b)
    return out


def merge(streams: dict[str, list[Record]]) -> Merged:
    """Merges the records of several transports by (boot, seq).

    A record seen on several transports is kept once (``sources`` lists them).
    Records with a bad CRC are never trusted: they go to ``bad`` and only fill
    nothing. If two CRC-valid copies differ, the first source wins and the
    difference is reported in ``conflicts``.
    """
    table: dict[tuple, dict] = {}
    bad, conflicts, schema = [], [], []
    dups = 0
    per_source = {}
    for src, recs in streams.items():
        c = per_source.setdefault(src, {"records": 0, "bad_crc": 0, "unparsable": 0, "schema": 0})
        for r in recs:
            c["records"] += 1
            if not r.crc_ok or r.obj is None or r.seq is None:
                c["bad_crc" if not r.crc_ok else "unparsable"] += 1
                bad.append(r.to_json())
                continue
            key = (r.boot, r.seq)
            schema_issues = [p for p in r.problems if not p.startswith("bad crc")]
            if key in table:
                have = table[key]
                if have["rec"] == r.obj:
                    if src not in have["sources"]:
                        have["sources"].append(src)
                    else:
                        dups += 1
                else:
                    conflicts.append({"boot": r.boot, "seq": r.seq, "kept": {"sources": have["sources"], "rec": have["rec"]},
                                      "other": {"source": src, "rec": r.obj}})
                continue
            if schema_issues:
                c["schema"] += 1
                schema.append({"boot": r.boot, "seq": r.seq, "source": src, "problems": schema_issues, "rec": r.obj})
            table[key] = {"boot": r.boot, "seq": r.seq, "rec": r.obj, "sources": [src]}
    ordered = [table[k] for k in sorted(table)]
    gaps = []
    boots = sorted({k[0] for k in table})
    for b in boots:
        seqs = sorted(k[1] for k in table if k[0] == b)
        expect = 0
        for s in seqs:
            if s > expect:
                gaps.append({"boot": b, "from": expect, "to": s - 1, "count": s - expect})
            expect = s + 1
    return Merged(ordered, gaps, bad, conflicts, dups, schema, per_source)


def parse_bytes(data: bytes, source: str) -> StreamParser:
    sp = StreamParser(source)
    sp.feed(data)
    sp.close()
    return sp
