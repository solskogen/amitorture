"""The bench disk on the host side (docs/BENCH.md, CONTRACT §15).

The guest times every block with its own clock and needs nothing from the
host. When the disk was baked with the ``host`` flag the runner also times
each run with the host's clock, from serial arrival times:

  * the guest sends a ``go`` record right before a timed run and nothing
    else until its ``run`` record, writing serial synchronously (rt/out.c);
  * Amiberry hands every serial byte to the socket as the guest sends it
    (TCP_NODELAY), so the chunk that ends with the go record's newline
    arrives when the run starts, and the next chunk when it has ended.

``Stamper`` keeps those two instants (and the emulator's thread CPU times
then) per go record; ``Host`` matches them to the run records and answers
the guest's ``bench_longer`` / ``bench_more`` requests by the host's times;
``Host.result`` turns a launch into block results (one metric per block,
lower is better). ``diff`` compares two runs block by block; ``plan``
freezes a run's counts into the work plan mkadf patches into the disk.

While a run is open the runner reads nothing through the IPC (Amiberry
serves the IPC on its emulation thread, once per frame: polling would be
part of what is timed), except for blocks whose analysis needs mailbox
samples (SAMPLED).
"""
from __future__ import annotations

import json
import os
import re
import time

HOST_MIN_S = 0.2        # shortest host time of a timed run (micro, kernel, cia_write)
COST_MIN_S = 0.5        # the same for the cost blocks (host CPU per emulated second)
CONFIRM = {"ns_per_unit": 0.0025, "cpu_per_emu_s": 0.01}   # two fastest agree within
MAX_REPS = 12           # BH_MAX_REPS
# |change| in percent: same up to the first, borderline up to the second
TOLERANCE = {"ns_per_unit": (0.5, 1.0), "cpu_per_emu_s": (2.0, 5.0)}
SAMPLED = ("flood_recovery",)       # blocks that need mailbox samples while they run
COST_UNITS = ("frame", "run")       # blocks whose measure is host CPU per emulated second
POS_RUN, POS_QUIET = 0x10000, 0x20000
PHASE_GAP = 30          # frames: samples further apart belong to different phases
SAMPLE_FIELDS = ["t", "heartbeat", "case", "cpu_main_ns", "cpu_total_ns", "cpu_top_ns"]

# threaded CPU checks against the unthreaded twin (docs/BENCH.md "Thread tier")
THREAD_RTF_MARGIN = 0.03
THREAD_IDLE_BLOCKS = ("stop", "stop_timer")
THREAD_IDLE_COST_MAX = 1.25
THREAD_IDLE_COST_SLACK = 0.05
THREAD_REFLOODS = 2
THREAD_RECOVERY_SHARE = 0.6

_GO = re.compile(rb'"t":"bench","ev":"go"')


def metric_of(unit):
    return "cpu_per_emu_s" if unit in COST_UNITS else "ns_per_unit"


def sample_wanted(case):
    """A mailbox header read worth keeping: a timed run or flood_recovery's quiet work."""
    return case == POS_QUIET or (case & 0xFFFF0000) == POS_RUN


class Stamper:
    """Serial arrival times around each timed run. ``feed`` runs on the
    serial thread with every chunk, before the parser. stamps: the go
    record's newline offset -> [t0, cpu0, t1, cpu1] (cpu: (main, total,
    top) thread ns or None; t1/cpu1 None until the next chunk)."""

    def __init__(self, cpu=None, clock=time.monotonic):
        self.cpu = cpu or (lambda: None)
        self.clock = clock
        self.pos = 0
        self.tail = b""
        self.stamps = {}
        self.open = None

    def feed(self, data, t=None):
        if not data:
            return
        t = self.clock() if t is None else t
        if self.open is not None:
            st = self.stamps[self.open]
            st[2], st[3] = t, self.cpu()
            self.open = None
        self.pos += len(data)
        buf = self.tail + data
        if buf.endswith(b"\n"):
            last = buf[buf.rfind(b"\n", 0, len(buf) - 1) + 1:]
            if _GO.search(last):
                self.stamps[self.pos - 1] = [t, self.cpu(), None, None]
                self.open = self.pos - 1
        self.tail = buf[-256:]

    def export(self):
        return [[k] + v for k, v in sorted(self.stamps.items())]


def _cpu_delta(a, b):
    if not a or not b or None in a or None in b:
        return None
    return [(y - x) / 1e9 for x, y in zip(a, b)]


class Host:
    """Bench records of one launch (serial, in order), matched with the
    stamper's times. ``pull``: a callable that feeds the records parsed so
    far (a request may be served before the run record was taken)."""

    def __init__(self, stamper=None, pull=None, min_s=HOST_MIN_S, cost_min_s=COST_MIN_S, sleep=time.sleep,
                 clock=time.monotonic):
        self.stamper = stamper
        self.pull = pull
        self.min_s, self.cost_min_s = min_s, cost_min_s
        self.sleep, self.clock = sleep, clock
        self.env = None
        self.blocks = []        # {"begin", "runs", "end", "skip", "boot"}
        self.cur = None
        self.last = None        # the last bench record (obj)
        self._go = None         # (boot, seq, rs, newline offset)
        self.samples = []
        self.decisions = []

    # -- intake ------------------------------------------------------------
    def ingest(self, rec):
        o = rec.obj if hasattr(rec, "obj") else rec
        if not o or o.get("t") != "bench":
            return
        ev = o.get("ev")
        boot = getattr(rec, "boot", 0)
        self.last = o
        if ev == "env":
            self.env = o
        elif ev == "begin":
            self.cur = {"begin": o, "skip": None, "runs": [], "end": None, "boot": boot}
            self.blocks.append(self.cur)
        elif ev == "go":
            off = getattr(rec, "offset", None)
            nl = off + len(rec.raw) if off is not None and getattr(rec, "raw", None) is not None else None
            self._go = (boot, o.get("seq"), o.get("rs"), nl)
        elif ev == "run":
            run = {"ph": o.get("ph"), "n": o.get("n"), "rs": o.get("rs"),
                   "t": (o.get("timing") or {}).get("t")}
            g, self._go = self._go, None
            if self.stamper is not None:
                run["host_s"] = None
                if g and g[0] == boot and g[1] == (o.get("seq") or 0) - 1 and g[2] == o.get("rs"):
                    st = self.stamper.stamps.get(g[3])
                    if st and st[2] is not None:
                        run["host_s"] = st[2] - st[0]
                        run["cpu_s"] = _cpu_delta(st[1], st[3])
            if self.cur is not None and self.cur["begin"].get("blk") == o.get("blk"):
                self.cur["runs"].append(run)
        elif ev == "end":
            if self.cur is not None and self.cur["begin"].get("blk") == o.get("blk"):
                self.cur["end"] = o
            self.cur = None
        elif ev == "skip":
            self.blocks.append({"begin": None, "skip": o, "runs": [], "end": None, "boot": boot})
            self.cur = None

    def add_sample(self, t, h, cpu):
        if sample_wanted(h.case):
            self.samples.append([round(t, 6), h.heartbeat, h.case] + (list(cpu) if cpu else [None] * 3))

    def quiet(self):
        """Seconds a timed run has been open (go seen, its run record not
        yet) in a block that needs no mailbox samples, else None."""
        if not self.last or self.last.get("ev") != "go" or self.cur is None:
            return None
        if self.cur["begin"].get("blk") in SAMPLED:
            return None
        st = self.stamper.stamps.get(self._go[3]) if self.stamper and self._go else None
        return (self.clock() - st[0]) if st else 0.0

    # -- the guest's requests ----------------------------------------------
    def _block(self, bid):
        for b in reversed(self.blocks):
            if b["begin"] and b["begin"].get("id") == bid:
                return b
        return None

    def _values(self, b, runs):
        """The block's measure per run: host times if every run has one, else guest ticks."""
        unit = b["begin"].get("unit")
        tick_hz = (self.env or {}).get("tick_hz") or 0
        if self.stamper is not None and runs and all(r.get("host_s") is not None for r in runs):
            if metric_of(unit) == "cpu_per_emu_s":
                vals = []
                for r in runs:
                    emu = r["t"] / tick_hz if tick_hz and r.get("t") else 0
                    if not emu or not r.get("cpu_s"):
                        return None, "host"
                    vals.append(r["cpu_s"][1] / emu)
                return vals, "host"
            return [r["host_s"] for r in runs], "host"
        return [r["t"] for r in runs if r.get("t") is not None], "guest"

    def decide(self, what, args):
        """(ack, info) for a bench_longer / bench_more request."""
        bid, n, reps = args.get("id"), args.get("n"), args.get("reps", 0)
        if what == "bench_more" and (reps or 0) >= MAX_REPS:
            self.decisions.append({"what": what, "id": bid, "n": n, "reps": reps, "ack": False, "max": True})
            return False, {"max": True}
        deadline = self.clock() + 1.0
        while True:
            if self.pull:
                self.pull()
            b = self._block(bid)
            runs = [r for r in (b["runs"] if b else []) if r["n"] == n and r["ph"] != "cold"]
            if b and (runs if what == "bench_longer" else len(runs) >= reps):
                break
            if self.clock() > deadline:
                return False, {"error": f"block {bid}: no run with n={n} to judge"}
            self.sleep(0.01)
        unit = b["begin"].get("unit")
        m = metric_of(unit)
        if what == "bench_longer":
            r = runs[-1]
            if self.stamper is None:
                ack = r["t"] < ((self.env or {}).get("min_ticks") or 0)
                info = {"basis": "guest", "t": r["t"]}
            else:
                hs = r.get("host_s")
                need = self.cost_min_s if m == "cpu_per_emu_s" else self.min_s
                ack = hs is None or hs < need
                info = {"basis": "host", "host_s": None if hs is None else round(hs, 6), "need_s": need}
        else:
            vals, basis = self._values(b, runs)
            info = {"basis": basis, "reps": len(runs)}
            if reps >= MAX_REPS or len(runs) >= MAX_REPS:
                ack = False
            elif vals is None or len(vals) < 2:
                ack = True
            else:
                a, c = sorted(vals)[:2]
                ack = not (c - a <= a * CONFIRM[m])
                info["spread"] = round((c - a) / a, 6) if a else None
        self.decisions.append({"what": what, "id": bid, "n": n, "reps": reps, "ack": ack, **info})
        return ack, info

    # -- results -----------------------------------------------------------
    def result(self, pacing=None):
        tick_hz = (self.env or {}).get("tick_hz") or 0
        out = []
        for b in self.blocks:
            if b["skip"] is not None:
                s = b["skip"]
                out.append({"blk": s.get("blk"), "tier": s.get("tier"), "id": s.get("id"), "st": "skip",
                            "why": s.get("why")})
                continue
            g, e = b["begin"], b["end"] or {}
            unit, per = g.get("unit"), g.get("per") or 1
            m = metric_of(unit)
            r = {"blk": g.get("blk"), "tier": g.get("tier"), "id": g.get("id"), "unit": unit, "per": per,
                 "metric": m}
            for k in ("mem", "kset"):
                if g.get(k):
                    r[k] = g[k]
            if not e:
                r.update(st="fail", why="the block never ended")
                out.append(r)
                continue
            r.update(st=e.get("st"), n=e.get("n"), reps=e.get("reps"))
            for k in ("why", "sum", "sum_last", "nonlinear"):
                if e.get(k) is not None:
                    r[k] = e[k]
            n = e.get("n") or 0
            runs = [x for x in b["runs"] if x["n"] == n and x["ph"] != "cold"]
            vals, basis = self._values(b, runs)
            r["basis"] = basis
            cold = [x for x in b["runs"] if x["ph"] == "cold"]
            if cold and cold[0].get("t") and tick_hz:
                r["cold_guest_ns"] = round(cold[0]["t"] / tick_hz * 1e9, 1)
            r["guest_t"] = [x["t"] for x in runs]
            if self.stamper is not None:
                r["host_s"] = [None if x.get("host_s") is None else round(x["host_s"], 6) for x in runs]
            if not vals:
                r["value"] = None
                out.append(r)
                continue
            order = sorted(range(len(vals)), key=lambda i: vals[i])
            best = vals[order[0]]
            second = vals[order[1]] if len(vals) > 1 else None
            if m == "cpu_per_emu_s":
                if basis != "host":
                    r["value"] = None
                    r["why_no_value"] = "a cost per emulated second needs host times"
                else:
                    r["value"] = round(best, 6)
                    x = runs[order[0]]
                    r["rtf"] = round(x["host_s"] / (x["t"] / tick_hz), 6)
                    r["cpu_main_per_emu_s"] = round(x["cpu_s"][0] / (x["t"] / tick_hz), 6)
            else:
                div = n * per
                scale = 1e9 / div if basis == "host" else 1e9 / (tick_hz * div) if tick_hz else None
                r["value"] = round(best * scale, 4) if scale and div else None
            if g.get("blk") == "flood_recovery":
                r.update(self._flood_shares(g.get("id")))
                r["confirmed"] = True
            else:
                r["confirmed"] = second is not None and second - best <= best * CONFIRM[m]
            if second is not None and best:
                r["spread_pct"] = round((second - best) / best * 100, 4)
            out.append(r)
        return {"env": self.env, "basis": "host" if self.stamper is not None else "guest", "pacing": pacing,
                "blocks": out, "decisions": self.decisions}

    def _flood_shares(self, bid):
        flood = [s for s in self.samples if (s[2] & 0xFFFF0000) == POS_RUN and (s[2] >> 8) & 0xFF == (bid & 0xFF)]
        quiet = [s for s in self.samples if s[2] == POS_QUIET]
        pts = lambda ss: [dict(zip(SAMPLE_FIELDS, s)) for s in ss]
        return {"quiet_thread_share": thread_share(pts(quiet)), "flood_thread_share": thread_share(pts(flood)),
                "quiet_samples": len(quiet)}


def thread_share(pts):
    """Host CPU of the emulator's threads other than the main one, per host
    second, over consecutive samples of one phase: about 1 while a threaded
    CPU runs on its thread, near 0 while it runs on the main thread. A
    thread that ends between two samples takes its time out of the total:
    that pair is left out."""
    busy = span = 0.0
    for a, b in zip(pts, pts[1:]):
        if b["heartbeat"] - a["heartbeat"] > PHASE_GAP or None in (
                a.get("cpu_total_ns"), a.get("cpu_main_ns"), b.get("cpu_total_ns"), b.get("cpu_main_ns")):
            continue
        dt = b["t"] - a["t"]
        other = (b["cpu_total_ns"] - b["cpu_main_ns"]) - (a["cpu_total_ns"] - a["cpu_main_ns"])
        if dt <= 0 or other < 0:
            continue
        busy += other / 1e9
        span += dt
    return round(busy / span, 3) if span > 0 else None


def from_serial(data, stamps=None, samples=None, pacing=None):
    """Host.result from a saved serial capture and the stamps/samples of
    result.json (rebuilds a launch's bench results without the emulator)."""
    from . import records
    st = None
    if stamps is not None:
        st = Stamper()
        st.stamps = {s[0]: list(s[1:]) for s in stamps}
    h = Host(st)
    p = records.StreamParser("serial")
    p.feed(data)
    p.close()
    for r in p.records:
        if r.crc_ok and r.obj:
            h.ingest(r)
    h.samples = list(samples or [])
    return h.result(pacing)


def from_merged(merged, pacing=None):
    """Host.result from the guest's own times (merged records): imported
    captures (real hardware, FPGA) and launches the runner did not time."""
    h = Host(None)
    for m in merged.records:
        h.ingest(m["rec"])
    return h.result(pacing) if h.blocks or h.env else None


# -- runs and summaries ------------------------------------------------------
def summary(results):
    """summary.json "bench": per entry (and disk) its bench blocks."""
    from . import cases as casemod
    out = []

    def thread_key(r):
        options = tuple(sorted((k, v) for k, v in (r.get("options") or {}).items() if k != "cpu_threaded"))
        return r.get("binary"), json.dumps(r.get("params") or {}, sort_keys=True), options

    def threaded(r):
        return str((r.get("options") or {}).get("cpu_threaded", "false")).lower() == "true"

    refs = {thread_key(r): r["id"] for r in results if not threaded(r) and not casemod.cpu_thread_active(r)}
    for r in results:
        for sub in r.get("disks") or [r]:
            b = sub.get("bench")
            if not b:
                continue
            eng = sub.get("engine") or r.get("engine") or {}
            out.append({"entry": r["id"], "disk": sub.get("disk_name"), "jit": r.get("jit"),
                        "machine": r.get("machine"), "status": sub.get("status"),
                        "cpu_threaded": casemod.cpu_thread_active(sub if sub.get("adjusted") else r),
                        "cpu_threaded_requested": threaded(r),
                        "thread_reference": refs.get(thread_key(r)) if threaded(r) else None,
                        "cpu_speed": (r.get("options") or {}).get("cpu_speed"), "host": sub.get("host_context"),
                        "flood_holds": eng.get("cpu_thread_flood_holds"), "refloods": eng.get("cpu_thread_refloods"),
                        "basis": b.get("basis"), "pacing": b.get("pacing"), "env": b.get("env"),
                        "blocks": b.get("blocks") or []})
    return out


def plan(entry_summary):
    """The work plan (mkadf --plan JSON) that repeats a run's counts."""
    ents = []
    for b in entry_summary.get("blocks") or []:
        if b.get("st") in ("ok", "unconfirmed") and isinstance(b.get("n"), int) and b["n"] > 0:
            ents.append({"id": b["id"], "count": b["n"]})
    return {"entries": ents}


def load(path):
    """summary.json "bench" of a results directory (or a summary.json path)."""
    f = os.path.join(path, "summary.json") if os.path.isdir(path) else path
    try:
        with open(f, encoding="utf-8") as fh:
            summ = json.load(fh)
    except (OSError, ValueError) as e:
        raise ValueError(f"{f}: {e}") from e
    ents = summ.get("bench")
    if not ents:
        raise ValueError(f"{f}: no bench results (a run of --matrix bench writes them)")
    return ents


def freeze_plans(entries, baseline, plandir):
    """Gives each entry with a baseline result the counts that run used
    (entry["bench_plan"], a mkadf --plan file), so both runs do the same work."""
    by = {}
    for e in baseline:
        by.setdefault(e["entry"], e)
    for ent in entries:
        b = by.get(ent["id"])
        if b is None:
            continue
        p = plan(b)
        if not p["entries"]:
            continue
        os.makedirs(plandir, exist_ok=True)
        f = os.path.join(plandir, f"{ent['id']}.json")
        with open(f, "w", encoding="utf-8") as fh:
            json.dump(p, fh, indent=1)
        ent["bench_plan"] = f


def write_diff(outdir, a_entries, b_entries, a_label):
    """BENCH-DIFF.md of this run against a reference; the summary "bench_diff"."""
    rows = diff(a_entries, b_entries)
    status, word = exit_status(rows)
    counts = {}
    for r in rows:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    L = [f"# Bench diff: {word}", "", f"Reference (A): `{a_label}`; this run (B): `{outdir}`.", ""]
    L.extend(diff_markdown(rows, "A", "B"))
    with open(os.path.join(outdir, "BENCH-DIFF.md"), "w", encoding="utf-8") as fh:
        fh.write("\n".join(L) + "\n")
    return {"against": a_label, "status": status, "word": word, "counts": counts, "file": "BENCH-DIFF.md",
            "rows": rows}


def _verdict(a, b):
    """One block, A (reference) against B. Returns a row."""
    row = {"blk": (b or a)["blk"], "id": (b or a).get("id"), "metric": (b or a).get("metric")}
    if a is None:
        return dict(row, verdict="new")
    if b is None:
        return dict(row, verdict="incomplete", a=a.get("value"), why="not in this run")
    row.update(a=a.get("value"), b=b.get("value"))
    if "fail" in (a.get("st"), b.get("st")):
        return dict(row, verdict="fail", why=b.get("why") or a.get("why"))
    if "skip" in (a.get("st"), b.get("st")):
        return dict(row, verdict="skipped", why=b.get("why") or a.get("why"))
    for k in ("kset", "mem", "basis", "metric", "unit"):
        if a.get(k) != b.get(k):
            return dict(row, verdict="incomparable", why=f"{k}: {a.get(k)} vs {b.get(k)}")
    if a.get("value") is None or b.get("value") is None:
        return dict(row, verdict="skipped", why=b.get("why_no_value") or "no value")
    if a.get("blk") == "flood_recovery":
        return dict(row, verdict="info")
    if a.get("n") != b.get("n"):
        row["note"] = f"count {a.get('n')} vs {b.get('n')}"
    change = (b["value"] / a["value"] - 1) * 100 if a["value"] else 0.0
    row["change_pct"] = round(change, 3)
    same, border = TOLERANCE.get(row["metric"], TOLERANCE["ns_per_unit"])
    if abs(change) <= same:
        v = "same"
    elif not (a.get("confirmed") and b.get("confirmed")):
        v = "noisy"
    elif abs(change) <= border:
        v = "borderline"
    else:
        v = "slower" if change > 0 else "faster"
    return dict(row, verdict=v)


def diff(a_entries, b_entries):
    """Rows per (entry, block) of two summary "bench" lists, A the reference."""
    rows = []
    a_by = {(e["entry"], e.get("disk")): e for e in a_entries}
    for be in b_entries:
        ae = a_by.pop((be["entry"], be.get("disk")), None)
        pace = None
        if ae is not None and ae.get("pacing") != be.get("pacing"):
            pace = f"pacing {ae.get('pacing')} vs {be.get('pacing')}"
        ab = {x["id"]: x for x in (ae or {}).get("blocks") or []}
        for b in be.get("blocks") or []:
            a = ab.pop(b["id"], None) if ae is not None else None
            r = _verdict(a, b) if ae is not None else dict(_verdict(None, b), why="entry not in the reference")
            if pace and r["verdict"] not in ("fail", "new"):
                r.update(verdict="incomparable", why=pace)
            rows.append(dict(r, entry=be["entry"], disk=be.get("disk")))
        for a in ab.values():
            rows.append(dict(_verdict(a, None), entry=be["entry"], disk=be.get("disk")))
    for ae in a_by.values():
        rows.append({"entry": ae["entry"], "disk": ae.get("disk"), "blk": "*", "verdict": "incomplete",
                     "why": "entry not in this run"})
    return rows


FAILING = ("slower", "fail", "noisy", "incomplete")


def exit_status(rows):
    """(status, word): 1 on a slower, failed, noisy or missing block, or nothing compared."""
    if not any(r["verdict"] not in ("new", "incomparable", "skipped") for r in rows):
        return 1, "NOTHING COMPARED"
    for v in FAILING:
        if any(r["verdict"] == v for r in rows):
            return 1, v.upper()
    return 0, "OK"


def _fmt(v, m=None):
    if v is None:
        return "-"
    return f"{v:.4f}" if m == "cpu_per_emu_s" else f"{v:.4g}"


def diff_markdown(rows, a_label="A", b_label="B"):
    counts = {}
    for r in rows:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    L = [f"Verdicts: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())), "",
         "Lower is better: host ns per unit (micro, kernel) or host CPU seconds per emulated second (frame, "
         "thread). Change > 0: slower. same within "
         f"{TOLERANCE['ns_per_unit'][0]}% ({TOLERANCE['cpu_per_emu_s'][0]}% for CPU/s), borderline up to "
         f"{TOLERANCE['ns_per_unit'][1]}% ({TOLERANCE['cpu_per_emu_s'][1]}%); noisy: beyond 'same' but a side "
         "never confirmed its time.", "",
         f"| entry | block | {a_label} | {b_label} | change | verdict | note |", "|---|---|---|---|---|---|---|"]
    order = {v: i for i, v in enumerate(("fail", "slower", "noisy", "incomplete", "borderline", "faster",
                                          "incomparable", "skipped", "new", "info", "same"))}
    for r in sorted(rows, key=lambda r: (order.get(r["verdict"], 99), r["entry"], str(r.get("blk")))):
        v = r["verdict"]
        ch = "-" if r.get("change_pct") is None else f"{r['change_pct']:+.2f}%"
        name = r["entry"] + (f" [{r['disk']}]" if r.get("disk") else "")
        note = "; ".join(x for x in (r.get("why"), r.get("note")) if x)
        L.append(f"| {name} | {r.get('blk')} | {_fmt(r.get('a'), r.get('metric'))} | "
                 f"{_fmt(r.get('b'), r.get('metric'))} | {ch} | "
                 f"{'**' + v + '**' if v in FAILING else v} | {note} |")
    return L


def report_lines(entries):
    """REPORT.md section for the bench entries of a run."""
    if not entries:
        return []
    from . import cases as casemod
    L = ["### Bench (docs/BENCH.md)", "",
         "One timed result per block, the fastest of its repetitions: host ns per unit for the micro and kernel "
         "tiers (guest ns when the host could not time it), host CPU s per emulated second for the frame and "
         "thread tiers (**rtf**: host s per emulated s). Compare runs with `amitorture bench diff`.", ""]
    for e in entries:
        L.append(f"- `{e['entry']}`" + (f" [{e['disk']}]" if e.get("disk") else "")
                 + f": basis {e.get('basis')}, pacing {e.get('pacing')}, cpu_speed={e.get('cpu_speed')}, "
                 f"JIT {'on' if e.get('jit') else 'off'}, threaded CPU {'on' if e.get('cpu_threaded') else 'off'}"
                 + (" (requested, but the emulator ran the CPU on the main thread)"
                    if e.get("cpu_threaded_requested") and not e.get("cpu_threaded") else "") + "; "
                 f"host {casemod.host_desc(e.get('host') or {})}")
        L.extend(["", "  | block | tier | st | value | measure | reps | spread | note |",
                  "  |---|---|---|---|---|---|---|---|"])
        for b in e["blocks"]:
            if b.get("st") == "skip":
                L.append(f"  | {b['blk']} | {b.get('tier')} | skip | | | | | {b.get('why') or ''} |")
                continue
            if b.get("metric") == "cpu_per_emu_s":
                measure = "host CPU/emu s"
            else:
                measure = f"{'host' if b.get('basis') == 'host' else 'guest'} ns/{b.get('unit')}"
            note = [x for x in (b.get("why"), b.get("kset"), b.get("mem")) if x]
            if b.get("rtf") is not None:
                note.append(f"rtf {b['rtf']:.3f}")
            if b.get("quiet_thread_share") is not None:
                note.append(f"quiet thread share {b['quiet_thread_share']}")
            st = b.get("st")
            spread = "" if b.get("spread_pct") is None else f"{b['spread_pct']:.2f}%"
            L.append(f"  | {b['blk']} | {b.get('tier')} | {'**FAIL**' if st == 'fail' else st} | "
                     f"{_fmt(b.get('value'), b.get('metric'))} | {measure} | {b.get('reps', '')} | "
                     f"{spread} | {', '.join(str(x) for x in note)} |")
        L.append("")
    return L


def thread_items(results):
    """`thread_speed` failure items (analysis.py): a threaded entry against
    its unthreaded twin on the bench disk's frame and thread blocks."""
    from . import cases as casemod
    out = []
    by_id = {r["id"]: r for r in results}

    def item(on, off, case, why, ref, twin):
        r = by_id.get(on["entry"]) or {}
        return {"kind": "thread_speed", "entry": on["entry"], "ref_entry": off and off["entry"], "mod": "bench",
                "case": case, "case_key": f"bench.{case}", "disk": on.get("disk"), "why": why, "ref": ref,
                "twin": twin, "differing_options": [{"option": "cpu_threaded", "ref": "false", "twin": "true"}],
                "host": casemod.host_desc(on.get("host") or {}), "jit": r.get("jit"), "machine": r.get("machine"),
                "config": (r.get("config") or {}).get("file")}

    for r in results:
        launches = [r] + list(r.get("disks") or [])
        pin = next((x["host_cpus"] for x in launches if isinstance(x.get("host_cpus"), dict)), None)
        eng = next((x["engine"] for x in launches if x.get("engine")), None) or {}
        if not pin or pin.get("asked") != 1:
            continue
        if str((r.get("options") or {}).get("cpu_threaded", "false")).lower() != "true":
            continue
        if eng.get("cpu_thread_runs_on") == "CPU":
            it = item({"entry": r["id"], "host": r.get("host_context")}, None, "thread_one_cpu",
                      f"the emulator ran the CPU on its own thread with one usable host CPU (pinned to "
                      f"{pin.get('pinned_to')}): the CPU thread and the main thread share it, and every hand-off "
                      f"waits for the scheduler (Amiberry newcpu.cpp cpu_thread_wanted, cpu_thread.cpp "
                      f"cpu_thread_usable_host_cpus)", {}, {"runs_on": "CPU thread",
                                                         "line": eng.get("cpu_thread_runs_line")})
            it["differing_options"] = []
            out.append(it)
    entries = summary(results)
    by_entry = {(e["entry"], e.get("disk")): e for e in entries}
    for on in entries:
        off = by_entry.get((on.get("thread_reference"), on.get("disk")))
        if not on.get("cpu_threaded") or off is None:
            continue
        if (on.get("refloods") or 0) >= THREAD_REFLOODS:
            out.append(item(on, off, "thread_refloods",
                            f"the CPU went back to its thread when the count on the main thread saw no flood, and "
                            f"flooded again at once, {on['refloods']} times in a row: the count misses requests the "
                            f"thread makes (Amiberry newcpu.cpp cpu_thread_count_read, cpu_thread_snap_read)",
                            {}, {"refloods": on["refloods"], "flood_holds": on.get("flood_holds")}))
        refs = {b["blk"]: b for b in off["blocks"]}
        for b in on["blocks"]:
            a = refs.get(b["blk"])
            if b["blk"] == "flood_recovery":
                qs, fs = b.get("quiet_thread_share"), b.get("flood_thread_share")
                holds = on.get("flood_holds")
                exercised = holds > 0 if holds is not None else (fs is None or fs < THREAD_RECOVERY_SHARE)
                if qs is not None and qs < THREAD_RECOVERY_SHARE and exercised:
                    keys = ("quiet_thread_share", "flood_thread_share", "quiet_samples")
                    out.append(item(on, off, "flood_recovery",
                                    f"flood_recovery: after each flood of CIA writes moved the CPU to the main "
                                    f"thread, the CPU thread ran only {qs:.0%} of the quiet work that followed (at "
                                    f"least {THREAD_RECOVERY_SHARE:.0%} expected): the CPU stays off its thread long "
                                    f"after the flood (Amiberry newcpu.cpp cpu_thread_check_flood)",
                                    {k: (a or {}).get(k) for k in keys if (a or {}).get(k) is not None},
                                    {k: b.get(k) for k in keys if b.get(k) is not None}))
                continue
            if a is None or b.get("metric") != "cpu_per_emu_s" or "fail" in (a.get("st"), b.get("st")):
                continue
            why = None
            ra, rb = a.get("rtf"), b.get("rtf")
            if ra is not None and rb is not None and ra <= 1.01 and rb > max(ra, 1.0) + THREAD_RTF_MARGIN:
                why = (f"{b['blk']}: with the threaded CPU the emulator ran at {1 / rb:.2f}x real time (rtf "
                       f"{rb:.3f}), where the unthreaded twin {off['entry']} kept up (rtf {ra:.3f}): the chipset "
                       f"fell behind serving the CPU thread (Amiberry newcpu.cpp run_cpu_thread, "
                       f"service_cpu_indirect_request)")
            ca, cb = a.get("value"), b.get("value")
            if why is None and b["blk"] in THREAD_IDLE_BLOCKS and ca is not None and cb is not None:
                limit = ca * THREAD_IDLE_COST_MAX + THREAD_IDLE_COST_SLACK
                if cb > limit:
                    why = (f"{b['blk']}: with the CPU idle in STOP the threaded CPU cost {cb:.3f} host CPU s per "
                           f"emulated s, the unthreaded twin {off['entry']} {ca:.3f} (limit {limit:.3f}): the idle "
                           f"CPU thread spins instead of sleeping (Amiberry newcpu.cpp cpu_thread_idle_wait, "
                           f"cpu_thread_main_wait_wake)")
            if why:
                keys = ("rtf", "value")
                out.append(item(on, off, b["blk"], why, {k: a.get(k) for k in keys if a.get(k) is not None},
                                {k: b.get(k) for k in keys if b.get(k) is not None}))
    return out
