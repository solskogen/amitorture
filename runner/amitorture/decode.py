"""decode: a raw serial capture (real hardware, FPGA, e.g. a MiSTer log) as
a text summary, without a results directory (docs/USAGE.md "Running on FPGA
/ real hardware"). `import` makes the full report from the same file.

Prints, per guest boot: the hello, pass/fail/skip per module with the failing
cases and their why, the bench blocks (best time and ns per unit) and the
score record. Lines starting with "# " are the guest's human comments and
are ignored; bytes before a record on its line (NULs from a serial line
coming up) are counted, not judged.
"""
from __future__ import annotations

from . import records as recmod

STATES = recmod.CASE_STATES


def parse(data: bytes, source="serial"):
    """(merged, parser) of one capture."""
    sp = recmod.StreamParser(source)
    sp.feed(data)
    sp.close()
    return recmod.merge({source: sp.records}), sp


def tick_hz(env, hello):
    """The guest clock of the bench times: env.tick_hz, else the hello's colour clock."""
    for src in (env or {}, hello or {}):
        for k in ("tick_hz", "cck_hz"):
            if isinstance(src.get(k), (int, float)) and src[k] > 0:
                return src[k]
    return None


def ns_per_unit(begin, end, hz):
    """ns per unit of a bench end record: its nsu (ns per unit x 10) if
    present, else timing.best / hz over n x per. None if unknown."""
    if isinstance(end.get("nsu"), (int, float)):
        return end["nsu"] / 10
    best = (end.get("timing") or {}).get("best")
    n, per = end.get("n"), (begin or {}).get("per") or 1
    if not hz or not best or not n:
        return None
    return best / hz * 1e9 / (n * per)


def boot_summary(recs):
    """Everything decode prints about one boot (records in seq order)."""
    out = {"hello": None, "done": None, "modules": {}, "order": [], "bench": [], "skips": [], "score": None,
           "env": None}
    mods = out["modules"]

    def mod(name):
        if name not in mods:
            mods[name] = {s: 0 for s in STATES} | {"failed": [], "ended": None}
            out["order"].append(name)
        return mods[name]

    begin = None
    for r in recs:
        t = r.get("t")
        if t == "hello":
            out["hello"] = r
        elif t == "done":
            out["done"] = r
        elif t == "mod" and r.get("mod"):
            m = mod(r["mod"])
            if r.get("ev") == "end":
                m["ended"] = {s: r.get(s, 0) for s in STATES}
        elif t == "case":
            m = mod(r.get("mod") or "?")
            st = r.get("st")
            if st in STATES:
                m[st] += 1
            if st == "fail":
                m["failed"].append({"case": r.get("case"), "why": r.get("why")})
        elif t == "bench":
            ev = r.get("ev")
            if ev == "env":
                out["env"] = r
            elif ev == "begin":
                begin = r
            elif ev == "end":
                g = begin if begin and begin.get("blk") == r.get("blk") else {"blk": r.get("blk")}
                out["bench"].append((g, r))
                begin = None
            elif ev == "skip":
                out["skips"].append(r)
            elif ev == "score":
                out["score"] = r
    return out


def _fmt_ns(v):
    if v is None:
        return "-"
    return f"{v:.3f}" if v < 100 else f"{v:.1f}"


def _score_value(k, v):
    if isinstance(v, int) and k.endswith("_x100"):
        return k[:-5], f"{v / 100:.2f}"
    if isinstance(v, int) and k.endswith("_x10"):
        return k[:-4], f"{v / 10:.1f}"
    return k, str(v)


def format_boot(s, label=""):
    L = []
    h = s["hello"]
    if h:
        p = h.get("probe") or {}
        L.append(f"{label}hello: build {h.get('build')!r}, cpu {h.get('cpu')} fpu {h.get('fpu')} mmu {h.get('mmu')}, "
                 f"{p.get('chipset', '?')} {p.get('video', '')}, kickstart {p.get('kick', '?')}".rstrip())
    else:
        L.append(f"{label}no hello record")
    if s["order"]:
        L.append("")
        L.append(f"{'module':<20} {'pass':>5} {'fail':>5} {'skip':>5} {'untst':>5}  ended")
        for name in s["order"]:
            m = s["modules"][name]
            # modules without case records (the bench disk) count on their end record
            c = m if any(m[x] for x in STATES) or not m["ended"] else m["ended"]
            L.append(f"{name:<20} {c['pass']:>5} {c['fail']:>5} {c['skip']:>5} {c['untestable']:>5}  "
                     f"{'yes' if m['ended'] else 'NO'}")
            for f in m["failed"]:
                L.append(f"  FAIL {name}.{f['case']}: {f['why'] or '(no why)'}")
    if s["bench"] or s["skips"]:
        hz = tick_hz(s["env"], h)
        L.append("")
        L.append(f"bench (guest clock {hz or '?'} Hz):")
        L.append(f"  {'block':<20} {'tier':<7} {'mem':<5} {'kset':<6} {'st':<12} {'n':>9} {'best ticks':>11} "
                 f"{'ns/unit':>10}  unit")
        for g, e in s["bench"]:
            best = (e.get("timing") or {}).get("best")
            line = (f"  {e.get('blk', '?'):<20} {g.get('tier') or '-':<7} {g.get('mem') or '-':<5} "
                    f"{g.get('kset') or '-':<6} {e.get('st') or '?':<12} {e.get('n') or '-':>9} "
                    f"{best if best is not None else '-':>11} {_fmt_ns(ns_per_unit(g, e, hz)):>10}  "
                    f"{g.get('unit') or ''}")
            extra = "; ".join(str(x) for x in (e.get("why"), e.get("note")) if x)
            L.append(line + (f"  ({extra})" if extra else ""))
        for k in s["skips"]:
            L.append(f"  {k.get('blk', '?'):<20} {k.get('tier') or '-':<7} skipped: {k.get('why')}")
    if s["score"]:
        L.append("")
        fields = [_score_value(k, v) for k, v in s["score"].items() if k not in ("v", "seq", "t", "ev")]
        L.append("score: " + (", ".join(f"{k} {v}" for k, v in fields) or "(empty)"))
    d = s["done"]
    L.append("")
    if d:
        L.append(f"done: pass {d.get('pass')} fail {d.get('fail')} skip {d.get('skip')} "
                 f"untestable {d.get('untestable')} aborted {d.get('aborted')}, {d.get('records')} records")
    else:
        L.append("no done record: the run broke off or the capture is incomplete")
    return L


def decode(data: bytes, name="capture"):
    """(text, problems): the summary of a capture and what is wrong with it."""
    merged, sp = parse(data)
    boots = sorted({m["boot"] for m in merged.records})
    problems = []
    L = [f"{name}: {len(data)} bytes, {len(merged.records)} records, {len(boots)} boot(s)"]
    side = []
    if sp.junk_bytes:
        side.append(f"{sp.junk_bytes} junk byte(s) before records")
    if sp.noise_lines:
        side.append(f"{sp.noise_lines} non-record line(s)")
    if sp.comments:
        side.append(f"{sp.comments} comment line(s)")
    if side:
        L.append("  " + ", ".join(side) + " (ignored)")
    if merged.bad:
        problems.append(f"{len(merged.bad)} corrupt record(s) (bad CRC or JSON)")
    if merged.gaps:
        problems.append(f"{sum(g['count'] for g in merged.gaps)} record(s) missing (seq gaps)")
    if merged.schema_problems:
        problems.append(f"{len(merged.schema_problems)} record schema problem(s)")
    for b in boots:
        recs = [m["rec"] for m in merged.records if m["boot"] == b]
        s = boot_summary(recs)
        L.append("")
        L.extend(format_boot(s, f"boot {b}: " if len(boots) > 1 else ""))
        where = f"boot {b}: " if len(boots) > 1 else ""
        nfail = sum(len(m["failed"]) for m in s["modules"].values())
        if nfail:
            problems.append(f"{where}{nfail} failed case(s)")
        bfail = [e.get("blk") for _, e in s["bench"] if e.get("st") == "fail"]
        if bfail:
            problems.append(f"{where}failed bench block(s): {', '.join(map(str, bfail))}")
        if s["done"] is None:
            problems.append(f"{where}no done record")
        elif isinstance(s["done"].get("records"), int) and s["done"]["records"] != len(recs):
            problems.append(f"{where}done says {s['done']['records']} records, got {len(recs)}")
    if not boots:
        problems.append("no records in the capture")
    L.append("")
    L.append("PROBLEMS: " + "; ".join(problems) if problems else "OK")
    return "\n".join(L) + "\n", problems
