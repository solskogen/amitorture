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
    "speed": ("mod", "ev", "insn"),        # hostspeed timed blocks (docs/schema.md 1.10)
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
    if t == "hello" and "modules" in obj:
        mods = obj["modules"]
        if not isinstance(mods, dict) or any(
                not isinstance(k, str) or not k or type(v) is not int or not 0 <= v < 64
                for k, v in mods.items()):
            p.append("hello.modules must map module names to ids in 0..63")
        elif len(set(mods.values())) != len(mods):
            p.append("hello.modules contains duplicate ids")
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

    def new_boot(self, boot: int):
        """The runner reset the emulator: what this source delivers next is
        boot ``boot`` (the runner's count, the same on every transport), even
        when its hello is lost (a mailbox ring that wrapped before it was found
        again). A partial line of the old boot is dropped."""
        self.splitter.skip(0)
        self._boot = max(self._boot, boot)
        self._last_seq = None
        self._resync = False

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


def summarize(merged):
    hello, done = merged.by_type("hello"), merged.by_type("done")
    return {"merged": len(merged.records), "per_source": merged.per_source, "gaps": merged.gaps,
            "bad": merged.bad, "bad_count": len(merged.bad), "conflicts": merged.conflicts,
            "duplicates_dropped": merged.duplicates, "schema_problems": merged.schema_problems,
            "boots": sorted({m["boot"] for m in merged.records}),
            "host": sum(1 for m in merged.records if m.get("host")),
            "hello": hello[-1] if hello else None, "done": done[-1] if done else None}


def module_problems(merged):
    """Lifecycle and declared coverage of the latest guest boot."""
    guest = [m for m in merged.records if not m.get("host")]
    if not guest:
        return ["no modules executed"]
    boot = guest[-1]["boot"]
    recs = [m["rec"] for m in guest if m["boot"] == boot]
    hello = next((r for r in recs if r.get("t") == "hello"), {})
    expected = hello.get("modules")
    if expected == {} and not any(r.get("t") in ("mod", "case", "perf", "speed") for r in recs):
        done = next((r for r in recs if r.get("t") == "done"), {})
        if done and not any(done.get(k) for k in CASE_STATES):
            return []
    active, seen, completed, evidence, problems = None, set(), set(), False, []
    if not hello:
        problems.append("no hello record in the latest guest boot")
    for r in recs:
        t, mod = r.get("t"), r.get("mod")
        if t == "mod":
            if not isinstance(mod, str) or not mod:
                problems.append("module record has no valid module name")
                continue
            if isinstance(expected, dict) and (mod not in expected or r.get("id") != expected[mod]):
                problems.append(f"module {mod} id={r.get('id')} differs from hello.modules")
            if r.get("ev") == "begin":
                if active is not None:
                    problems.append(f"module {mod} began before module {active} ended")
                if mod in seen:
                    problems.append(f"module {mod} began more than once")
                seen.add(mod)
                active = mod
            elif r.get("ev") == "end":
                if active != mod:
                    problems.append(f"module {mod} ended without a matching begin")
                else:
                    completed.add(mod)
                    active = None
        elif t in ("case", "perf", "speed"):
            evidence = True
            if active != mod:
                problems.append(f"{t} record for module {mod} outside its begin/end")
    if active is not None:
        problems.append(f"module {active} has no matching end")
    if not seen:
        problems.append("no modules executed")
    elif not evidence:
        problems.append("no case, perf or speed records were produced")
    if isinstance(expected, dict):
        missing = sorted(set(expected) - completed)
        if missing:
            problems.append("declared modules did not complete: " + ", ".join(missing))
    return problems


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


def boot_alignment(streams):
    """Map source-local boots to common epochs, retaining uncertain epochs separately."""
    grouped = {}
    for src, recs in streams.items():
        boots = {}
        for r in recs:
            boots.setdefault(r.boot, []).append(r)
        if boots:
            grouped[src] = boots
    if not grouped:
        return {}, []
    anchor = max(grouped, key=lambda s: (len(grouped[s]), sum(r.crc_ok for r in streams[s])))
    anchor_boots = sorted(grouped[anchor])
    if all(len(boots) == len(anchor_boots) for boots in grouped.values()):
        return {src: dict(zip(sorted(boots), anchor_boots)) for src, boots in grouped.items()}, []
    views = {b: {r.seq: r.obj for r in grouped[anchor][b]
                 if r.crc_ok and r.obj is not None and r.seq is not None} for b in anchor_boots}
    complete = {}
    for b, view in views.items():
        last = view.get(len(view) - 1, {})
        complete[b] = (view.get(0, {}).get("t") == "hello" and last.get("t") == "done"
                       and last.get("records") == len(view) and set(view) == set(range(len(view))))
    maps = {anchor: {b: b for b in anchor_boots}}
    issues, next_boot = [], max(anchor_boots) + 1
    for src, boots in grouped.items():
        if src == anchor:
            continue
        order = sorted(boots)
        if len(order) == len(anchor_boots):
            maps[src] = dict(zip(order, anchor_boots))
            continue
        candidates = {}
        for b in order:
            valid = [r for r in boots[b] if r.crc_ok and r.obj is not None and r.seq is not None]
            hello = next((r.obj for r in valid if r.seq == 0 and r.type == "hello"), None)
            matches = [a for a in anchor_boots if hello is not None and views[a].get(0) == hello]
            if len(matches) != 1:
                matches = [a for a in anchor_boots if complete[a] and valid
                           and all(views[a].get(r.seq) == r.obj for r in valid)]
            candidates[b] = matches
        unique = [c[0] for c in candidates.values() if len(c) == 1]
        monotonic = unique == sorted(set(unique))
        maps[src] = {}
        for b in order:
            matches = candidates[b]
            if monotonic and len(matches) == 1:
                maps[src][b] = matches[0]
            else:
                maps[src][b] = next_boot
                issues.append({"boot": next_boot, "source": src, "source_boot": b, "seq": None,
                               "problems": [f"cannot reconcile source-local boot {b} on {src} "
                                            f"with {anchor}: ambiguous or incomplete boot evidence"]})
                next_boot += 1
    return maps, issues


def merge(streams: dict[str, list[Record]]) -> Merged:
    """Merges the records of several transports by (boot, seq).

    A record seen on several transports is kept once (``sources`` lists them).
    Records with a bad CRC are never trusted: they go to ``bad`` and only fill
    nothing. If two CRC-valid copies differ, the first source wins and the
    difference is reported in ``conflicts``.
    Unequal observed boot histories are reconciled first; ambiguous epochs
    remain separate and produce schema problems rather than filling gaps.
    """
    table: dict[tuple, dict] = {}
    boot_maps, schema = boot_alignment(streams)
    bad, conflicts = [], []
    dups = 0
    per_source = {}
    for src, recs in streams.items():
        c = per_source.setdefault(src, {"records": 0, "bad_crc": 0, "unparsable": 0, "schema": 0})
        for r in recs:
            boot = boot_maps[src][r.boot]
            c["records"] += 1
            if not r.crc_ok or r.obj is None or r.seq is None:
                c["bad_crc" if not r.crc_ok else "unparsable"] += 1
                bad.append(r.to_json() | {"boot": boot})
                continue
            key = (boot, r.seq)
            schema_issues = [p for p in r.problems if not p.startswith("bad crc")]
            if key in table:
                have = table[key]
                if have["rec"] == r.obj:
                    if src not in have["sources"]:
                        have["sources"].append(src)
                    else:
                        dups += 1
                else:
                    conflicts.append({"boot": boot, "seq": r.seq, "kept": {"sources": have["sources"], "rec": have["rec"]},
                                      "other": {"source": src, "rec": r.obj}})
                continue
            if schema_issues:
                c["schema"] += 1
                schema.append({"boot": boot, "seq": r.seq, "source": src, "problems": schema_issues, "rec": r.obj})
            table[key] = {"boot": boot, "seq": r.seq, "rec": r.obj, "sources": [src]}
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
