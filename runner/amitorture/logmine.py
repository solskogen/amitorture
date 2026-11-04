"""Emulator log mining: suspicious amiberry.log lines, mapped to modules.

Amiberry logs things a guest cannot see: stores the JIT sent to the dummy
bank (a wrong effective address), compiled-code faults, Gary/bus timeouts,
illegal and unimplemented instructions, MMU messages, the CPU cycle unit.
This file classifies the lines of one launch's amiberry.log and assigns each
one to the guest module that was running when it was written.

Time base: the runner timestamps every log line when it reads it (seconds
since the process started, `[  t.ttt]`). Module boundaries come from the
arrival time of the guest's `mod` begin/end records:
  * serial-times.json (written by the runner since milestone 3): the arrival
    time of every serial chunk, so every record line gets an exact time;
  * otherwise mailbox.jsonl: the time of every ring read (coarse: several
    records can arrive in one read).
A module may have run anywhere between the arrival before its begin record
and the arrival of its end record; a log line inside that window belongs to
it. When windows overlap (coarse times), a line lists every candidate module
(`modules`), and `exact` is false.
"""
from __future__ import annotations

import base64
import json
import os
import re

# (category, regex) in priority order: the first match decides the category
PATTERNS = [
    ("dummy_bank", re.compile(r"dummy_bank|dummy_[bwl](?:get|put)\b|Address bank: NONE", re.I)),
    ("jit_fault", re.compile(r"Error in compiled code|JIT: fault|JIT: regs\.|New exception ---|JIT: AARCH64 opcode|"
                             r"store from register|JIT:.*(?:segv|sigsegv|not compil|fallback|flush)", re.I)),
    ("bus_timeout", re.compile(r"Gary timeout|bus error|buserror", re.I)),
    ("unimplemented", re.compile(r"unimplemented|unimpl", re.I)),
    ("illegal", re.compile(r"Illegal instruction|illegal opcode", re.I)),
    ("trap", re.compile(r"\b[AB]-Trap\b")),
    ("mmu", re.compile(r"\bMMU\b|mmu030|mmu_|\bATC\b", re.I)),
    ("cycleunit", re.compile(r"cycleunit|cycle unit", re.I)),
    ("warning", re.compile(r"\bwarning\b|\berror\b|\bfailed\b|\bbad\b", re.I)),
]
# host noise that matches the generic words but is never about the guest
NOISE = re.compile(r"ALSA lib|DLOPEN|NFD_Init|SDL_GL_|EGL|realpath\(\) failed|Mouse capture|Renderer context|"
                   r"SDL_SetWindowRelativeMouseMode|IPC: |VirtualProtect|VirtualAlloc|PAGE_READ|ROM VP|MEM_COMMIT|"
                   r"commit prot|mapped_free|libfloppybridge|plugins/", re.I)
LINE_RX = re.compile(r"^\[\s*([0-9.]+)\]\s?(.*)$")
STAMP_RX = re.compile(r"^\d+-\d+ \[[^\]]*\]: ")
HEX_RX = re.compile(r"\b(?:0x)?[0-9A-Fa-f]{5,16}\b")
MAX_GROUPS = 12
MAX_EXAMPLES = 4


def classify(text):
    if NOISE.search(text):
        return None
    for cat, rx in PATTERNS:
        if rx.search(text):
            return cat
    return None


def read_log(path):
    """[(t, text)] of amiberry.log (the emulator's own frame stamp stripped)."""
    out = []
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                m = LINE_RX.match(line.rstrip("\n"))
                if not m:
                    continue
                out.append((float(m.group(1)), STAMP_RX.sub("", m.group(2))))
    except OSError:
        pass
    return out


def _records_from_stream(chunks):
    """chunks: [(t, bytes)] in order -> [(t_arrival, record dict)]."""
    buf, out = b"", []
    for t, data in chunks:
        buf += data
        while b"\n" in buf:
            line, buf = buf.split(b"\n", 1)
            s = line.find(b"{")
            star = line.rfind(b" *")
            if s < 0 or star < s:
                continue
            try:
                out.append((t, json.loads(line[s:star].decode("ascii", "replace"))))
            except ValueError:
                continue
    return out


def arrivals(rawdir):
    """([(t, rec)], source, exact) from serial-times.json + serial.bin, else mailbox.jsonl."""
    st = os.path.join(rawdir, "serial-times.json")
    sb = os.path.join(rawdir, "serial.bin")
    if os.path.isfile(st) and os.path.isfile(sb):
        try:
            with open(st, encoding="utf-8") as fh:
                marks = json.load(fh)          # [[t, total bytes received so far], ...]
            with open(sb, "rb") as fh:
                data = fh.read()
            chunks, prev = [], 0
            for t, total in marks:
                chunks.append((t, data[prev:total]))
                prev = total
            recs = _records_from_stream(chunks)
            if recs:
                return recs, "serial", True
        except (OSError, ValueError, TypeError):
            pass
    mj = os.path.join(rawdir, "mailbox.jsonl")
    chunks = []
    try:
        with open(mj, encoding="utf-8") as fh:
            for line in fh:
                try:
                    e = json.loads(line)
                except ValueError:
                    continue
                if e.get("event") == "data" and e.get("data_b64"):
                    chunks.append((e["t"], base64.b64decode(e["data_b64"])))
    except OSError:
        return [], None, False
    return _records_from_stream(chunks), "mailbox", False


def module_windows(recs):
    """[{"mod", "lo", "hi"}]: the module may have run in (lo, hi]."""
    times = sorted({t for t, _ in recs})
    prev_of = {t: (times[i - 1] if i else 0.0) for i, t in enumerate(times)}
    wins, open_ = [], {}
    for t, r in recs:
        if r.get("t") != "mod":
            continue
        if r.get("ev") == "begin":
            open_[r.get("mod")] = prev_of.get(t, 0.0)
        elif r.get("ev") == "end" and r.get("mod") in open_:
            wins.append({"mod": r.get("mod"), "lo": open_.pop(r.get("mod")), "hi": t})
    for m, lo in open_.items():          # never ended (abort, hang): until the end of the log
        wins.append({"mod": m, "lo": lo, "hi": float("inf"), "unfinished": True})
    return wins


def _norm(text):
    return HEX_RX.sub("#", text)[:160]


def mine(rawdir):
    """Mining result for one launch dir (raw/<entry>/<disk>/), or None."""
    log = read_log(os.path.join(rawdir, "amiberry.log"))
    if not log:
        return None
    recs, source, exact = arrivals(rawdir)
    wins = module_windows(recs)
    counts = {}
    groups = {}
    for t, text in log:
        cat = classify(text)
        if cat is None:
            continue
        counts[cat] = counts.get(cat, 0) + 1
        mods = [w["mod"] for w in wins if w["lo"] < t <= w["hi"]]
        key = (cat, _norm(text), tuple(mods))
        g = groups.get(key)
        if g is None:
            g = groups[key] = {"category": cat, "pattern": key[1], "modules": mods, "count": 0, "examples": [],
                               "first_t": t, "last_t": t, "addresses": []}
        g["count"] += 1
        g["last_t"] = t
        if len(g["examples"]) < MAX_EXAMPLES:
            g["examples"].append(f"[{t:.3f}] {text[:240]}")
        for a in HEX_RX.findall(text)[:2]:
            if len(g["addresses"]) < 16 and a not in g["addresses"]:
                g["addresses"].append(a)
    rank = {c: i for i, (c, _) in enumerate(PATTERNS)}
    gl = sorted(groups.values(), key=lambda g: (rank[g["category"]], -g["count"], g["first_t"]))
    return {"counts": counts, "time_source": source, "exact": exact and bool(wins), "windows": [
        dict(w, hi=None if w["hi"] == float("inf") else w["hi"]) for w in wins], "groups": gl}


def for_module(mined, mod, limit=MAX_GROUPS):
    """The groups that may belong to `mod` ({"counts", "groups", "exact", "ambiguous"})."""
    if not mined:
        return None
    gs = [g for g in mined["groups"] if mod in g["modules"]]
    if not gs:
        return None
    counts = {}
    for g in gs:
        counts[g["category"]] = counts.get(g["category"], 0) + g["count"]
    return {"counts": counts, "groups": gs[:limit], "more": max(0, len(gs) - limit),
            "exact": mined.get("exact"), "time_source": mined.get("time_source"),
            "ambiguous": any(len(g["modules"]) > 1 for g in gs)}


LOG_COUNT_KEYS = [c for c, _ in PATTERNS]
