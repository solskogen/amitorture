"""Per-problem diagnosis aids for REPORT.md (what a reader without the test's
sources needs to find the cause):

  case_table       every failing case with its docs/cases/<module>.md line
                   (intent, rule, failure meaning) and `evidence:` field, the
                   entries it fails on, and the expect/got/ctx/in field names
                   that docs/cases does not define (for the module owners)
  config_flags     the emulator options that change the module's semantics,
                   with the values the failing entries resolved them to, and
                   every option=value a case's own text names whose value in
                   the config contradicts it ("precondition not met")
  passes_diff      the entries where the cases pass, and the settings that
                   separate all failing entries from all passing ones
                   ("fails on ECS only, passes on AGA", "24-bit addressing")
  emulator_log     suspicious amiberry.log lines (logmine.py) of the failing
                   launches that fall into the module's run time

`enrich()` adds them to the problems; report.py renders them.
"""
from __future__ import annotations

import os
import re

from . import logmine
from . import problems as pm

# Options that change what a module's cases may expect (resolved values are
# shown in every problem of the module).
COMMON_CPU = ["cpu_model", "cpu_compatible", "cycle_exact", "cpu_cycle_exact", "cachesize", "cpu_24bit_addressing",
              "mmu_model", "cpu_threaded"]
MODULE_OPTIONS = {
    "cpu_basic": COMMON_CPU + ["cpu_no_unimplemented", "comp_nf", "comp_constjump", "comp_trustlong"],
    "flags": COMMON_CPU + ["comp_nf"],
    "exceptions": COMMON_CPU + ["cpu_no_unimplemented", "fpu_model", "cpu_halt_auto_reset", "cpu060_revision"],
    "smc": COMMON_CPU + ["comp_flushmode", "cpu_data_cache", "comp_constjump"],
    "fpu": ["cpu_model", "fpu_model", "fpu_softfloat", "fpu_no_unimplemented", "fpu_strict", "compfpu", "cachesize",
            "cpu_compatible", "cpu060_revision"],
    "mmu": ["cpu_model", "mmu_model", "cpu_compatible", "cycle_exact", "cpu_data_cache", "cpu_24bit_addressing"],
    "memory": COMMON_CPU + ["chipmem_size", "bogomem_size", "fastmem_size", "z3mem_size", "mbresmem_size",
                            "cpu_data_cache"],
    "dispatch": COMMON_CPU + ["comp_nf", "comp_constjump", "comp_trustbyte", "comp_trustword", "comp_trustlong",
                              "comp_trustnaddr", "comp_flushmode", "comp_catchfault"],
    "interrupts": ["cpu_model", "cpu_compatible", "cycle_exact", "cpu_threaded", "cachesize", "cpu_speed"],
    "cia": ["cycle_exact", "cpu_compatible", "chipset_compatible", "ntsc", "cia_todbug", "rtc"],
    "timing": ["cpu_model", "cycle_exact", "cpu_compatible", "cpu_speed", "cpu_multiplier", "cachesize", "ntsc"],
    "blitter": ["chipset", "blitter_cycle_exact", "immediate_blits", "waiting_blits", "cycle_exact", "cpu_compatible"],
    "display": ["chipset", "chipset_compatible", "gfx_resolution", "gfx_lores_mode", "gfx_linemode", "collision_level",
                "cycle_exact", "cpu_compatible", "display_optimizations", "ntsc"],
    "copper": ["chipset", "cycle_exact", "cpu_compatible", "blitter_cycle_exact", "immediate_blits", "ntsc"],
    "paula": ["floppy_speed", "cycle_exact", "cpu_compatible", "sound_output", "sound_interpol", "sound_filter",
              "sound_stereo_separation", "ntsc"],
    "input": ["joyport0", "joyport1", "joyport1mode", "input.mouse_speed", "absolute_mouse", "chipset_compatible"],
    "dmacontention": ["chipset", "cycle_exact", "cpu_compatible", "cpu_speed", "blitter_cycle_exact"],
    "devio": ["floppy_speed", "chipset_compatible", "scsi", "ide"],
    "rtg": ["gfxcard_type", "gfxcard_size", "cpu_24bit_addressing", "z3mem_size"],
    "expsound": ["cpu_24bit_addressing", "sound_output"],
    "network": ["a2065", "zz9000_net"],
    "misc_hw": ["chipset_compatible", "rtc", "ide", "pcmcia", "cd32nvram", "flash_file"],
    "misc_hw_os": ["chipset_compatible", "fastmem_size", "z3mem_size", "gfxcard_type"],
}
OPT_RX = re.compile(r"\b([a-z][a-z0-9_.]{3,})\s*=\s*([A-Za-z0-9_./+-]+)")
TRUE = {"true", "yes"}
FALSE = {"false", "no"}


def _norm(v):
    v = str(v).strip().lower()
    return "true" if v in TRUE else ("false" if v in FALSE else v)


def _load_cfg(path):
    out = {}
    try:
        with open(path, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                if "=" in line and not line.startswith(";"):
                    k, v = line.rstrip("\n").split("=", 1)
                    out.setdefault(k.strip(), v.strip())
    except OSError:
        pass
    return out


class Ctx:
    def __init__(self, results, fps, items, results_dir):
        self.results = results
        self.by_id = {r["id"]: r for r in results}
        self.fps = fps
        self.items = {i["id"]: i for i in items}
        self.dir = results_dir
        self._cfg = {}
        self._mined = {}

    def sub(self, eid, disk):
        r = self.by_id.get(eid) or {}
        for s in r.get("disks") or []:
            if s.get("disk_name") == disk or disk is None:
                return s
        return r

    def value(self, eid, disk, name):
        """Resolved value, with Amiberry's default for options --dump-config
        leaves out when they have it."""
        from . import amiberry_cfg
        o = self.options(eid, disk)
        if name in o:
            return o[name]
        if name in amiberry_cfg.DWRITE_DEFAULTS:
            return f"{amiberry_cfg.DWRITE_DEFAULTS[name] or '(empty)'} (default)"
        if name in amiberry_cfg.COMMON:
            return amiberry_cfg.COMMON[name]
        return "(not set)"

    def options(self, eid, disk=None):
        """The resolved options of an entry (the disk's resolved config file,
        else the written options)."""
        key = (eid, disk)
        if key not in self._cfg:
            r = self.by_id.get(eid) or {}
            s = self.sub(eid, disk)
            o = dict(r.get("options") or {})
            o.update(r.get("resolved_options") or {})
            rf = (s.get("config") or {}).get("resolved") or (r.get("config") or {}).get("resolved")
            if rf and self.dir:
                o.update(_load_cfg(os.path.join(self.dir, rf)))
            self._cfg[key] = o
        return self._cfg[key]

    def mined(self, eid, disk):
        key = (eid, disk)
        if key not in self._mined:
            s = self.sub(eid, disk)
            raw = s.get("raw")
            self._mined[key] = logmine.mine(os.path.join(self.dir, raw)) if raw and self.dir else None
        return self._mined[key]


def problem_items(p, ctx):
    return [ctx.items[f] for f in p.get("failures", []) if f in ctx.items]


# --------------------------------------------------------------- case table
def _doc_lines(mod):
    pm.case_doc(mod, "__none__")        # fills the cache
    return (pm._doc_cache.get(mod) or (None, None))[1] or []


# register and machine names need no definition
SELF_EXPLAINING = re.compile(r"^(d[0-7]|a[0-7]|sr|ccr|pc|usp|ssp|isp|msp|vbr|cacr|fp[0-7]|fpcr|fpsr|fpiar|cpu|fpu|"
                             r"opcode|op|mod|case|why|note|seq)$", re.I)


def field_docs(mod, names):
    """{name: "file:line" or None}: where docs/cases/<mod>.md mentions a field."""
    lines = _doc_lines(mod)
    out = {}
    for n in names:
        rx = re.compile(r"(^|[^A-Za-z0-9_])" + re.escape(n) + r"($|[^A-Za-z0-9_])")
        out[n] = next((f"docs/cases/{mod}.md:{i + 1}" for i, line in enumerate(lines) if rx.search(line)), None)
    return out


def _evidence(doc_text, mod, case):
    if doc_text:
        m = re.search(r"evidence:\s*(.+?)(?:\s*\||$)", doc_text, re.I)
        if m:
            return m.group(1).strip()
    lines = _doc_lines(mod)
    for i, line in enumerate(lines):
        if case in line:
            for nxt in lines[i:i + 4]:
                m = re.search(r"evidence:\s*(.+)", nxt, re.I)
                if m:
                    return m.group(1).strip()
            break
    return None


def case_table(p, ctx):
    rows = {}
    for it in problem_items(p, ctx):
        key = it.get("case_key")
        if not key:
            continue
        row = rows.setdefault(key, {"case_key": key, "entries": [], "fields": set(), "why": it.get("why")})
        if it["entry"] not in row["entries"]:
            row["entries"].append(it["entry"])
        for k in ("expect", "got", "ctx", "in"):
            v = it.get(k)
            if isinstance(v, dict):
                row["fields"] |= {str(x) for x in v}
    out = []
    for key, row in sorted(rows.items()):
        mod, _, case = key.partition(".")
        case = case.split("#")[0].split("@")[0]
        doc = pm.case_doc(mod, case)
        fd = field_docs(mod, sorted(row["fields"]))
        out.append({"case_key": key, "entries": row["entries"], "why": row["why"],
                    "doc": doc.get("text") if doc else None,
                    "doc_where": f"{doc['file']}:{doc['line']}" if doc and doc.get("line") else None,
                    "evidence": _evidence(doc.get("text") if doc else None, mod, case),
                    "undefined_fields": sorted(k for k, v in fd.items() if v is None and not SELF_EXPLAINING.match(k)),
                    "defined_fields": {k: v for k, v in fd.items() if v}})
    return out


# --------------------------------------------------------------- config flags
PRECOND_RX = re.compile(r"\b(only|needs?|requires?|must|unless|provided)\b", re.I)


def _named_conditions(text):
    """[(option, value, is_precondition)]: option=value pairs a case's text
    names; a precondition when "only/needs/requires/must/unless" comes
    shortly before it ("Amiberry only traps these with X=true"), otherwise a
    mere mention ("with X=false it loses ...")."""
    text = text or ""
    out = []
    for m in OPT_RX.finditer(text):
        before = re.split(r"[;.!?]\s|,\s", text[max(0, m.start() - 60):m.start()])[-1]   # this clause only
        out.append((m.group(1), m.group(2).rstrip(".,;)"), bool(PRECOND_RX.search(before))))
    return out


def config_flags(p, ctx):
    mod = p.get("mod") or ""
    its = problem_items(p, ctx)
    names = list(MODULE_OPTIONS.get(mod, COMMON_CPU))
    per_entry = []
    for eid in p.get("entries", [])[:12]:
        disk = next((i.get("disk") for i in its if i["entry"] == eid), None)
        o = None
        per_entry.append({"entry": eid, "values": {n: ctx.value(eid, disk, n) for n in names}})
        del o
    conflicts, flagged = {}, set()
    for it in its:
        texts = [it.get("why") or ""]
        for k in ("ctx", "record"):
            v = it.get(k)
            if isinstance(v, dict):
                texts += [str(x) for kk, x in v.items() if kk in ("note", "why", "requires", "needs")]
        doc = pm.case_doc(mod, (it.get("case") or "").split("#")[0])
        if doc and doc.get("text"):
            texts.append(doc["text"])
        o = ctx.options(it["entry"], it.get("disk"))
        from . import amiberry_cfg
        for name, want, precond in _named_conditions(" ".join(texts)):
            if name not in o and name not in names and name not in amiberry_cfg.DWRITE_DEFAULTS:
                continue
            have = ctx.value(it["entry"], it.get("disk"), name)
            if _norm(have.replace(" (default)", "")) != _norm(want):
                k = (name, want, have, precond)
                c = conflicts.setdefault(k, {"option": name, "case_names": want, "config_has": have,
                                             "precondition": precond, "cases": [], "entries": []})
                if it.get("case_key") not in c["cases"]:
                    c["cases"].append(it.get("case_key"))
                if it["entry"] not in c["entries"]:
                    c["entries"].append(it["entry"])
                if precond:
                    flagged.add((it.get("case_key"), it["entry"]))
    all_items = {(i.get("case_key"), i["entry"]) for i in its if i.get("case_key")}
    return {"options": names, "per_entry": per_entry, "conflicts": list(conflicts.values())[:20],
            "all_conflict": bool(all_items) and all_items <= flagged}


# --------------------------------------------------------------- passes_on
DIFF_IGNORE = {"config_description", "rndseed"}


def passes_diff(p, ctx):
    keys = p.get("case_keys") or []
    if not keys:
        return None
    fail = set(p.get("entries", []))
    passing, skipped = [], []
    for r in ctx.results:
        if r["id"] in fail or r.get("status") == "skip" or "~" in r["id"]:
            continue
        cs = (ctx.fps.get(r["id"]) or {}).get("cases", {})
        sts = [cs[k]["st"] for k in keys if k in cs]
        if len(sts) == len(keys) and all(s == "pass" for s in sts):
            passing.append(r["id"])
        elif len(sts) != len(keys) or all(s in ("skip", "untestable") for s in sts):
            skipped.append(r["id"])
    out = {"passes_on": passing, "not_run_or_skipped_on": skipped, "separating": [], "nearest": []}
    if not passing:
        return out

    def opts(eid):
        o = {k: _norm(v) for k, v in ctx.options(eid).items() if k not in DIFF_IGNORE
             and not k.startswith(("amiberry.", "floppy", "kickstart", "serial", "cdimage", "hardfile", "uaehf",
                                   "filesystem", "flash_file", "gfx_width", "gfx_height"))}
        try:
            o["jit"] = "on" if int(ctx.options(eid).get("cachesize", "0") or 0) > 0 else "off"
        except ValueError:
            o["jit"] = "?"
        return o
    fo = {e: opts(e) for e in fail}
    po = {e: opts(e) for e in passing}
    for k in sorted(set().union(*fo.values(), *po.values())):
        fv = {o.get(k, "(default)") for o in fo.values()}
        pv = {o.get(k, "(default)") for o in po.values()}
        if not fv & pv:
            out["separating"].append({"option": k, "failing": sorted(fv), "passing": sorted(pv)})
    rep = next(iter(sorted(fail)), None)
    if rep:
        diffs = []
        for e in passing:
            d = [(k, fo[rep].get(k), po[e].get(k)) for k in sorted(set(fo[rep]) | set(po[e]))
                 if fo[rep].get(k) != po[e].get(k)]
            diffs.append((len(d), e, d))
        diffs.sort()
        out["nearest"] = [{"entry": e, "vs": rep, "differences": [{"option": k, "failing": a, "passing": b}
                                                                   for k, a, b in d[:12]], "n": n}
                          for n, e, d in diffs[:3]]
    # the same, among the passing entries that share the failing entries' engine
    # or machine class ("among JIT entries: cpu_24bit_addressing")
    out["within"] = []
    for x in ("jit", "chipset", "mmu_model", "cpu_model"):
        fv = {o.get(x, "(default)") for o in fo.values()}
        if len(fv) != 1:
            continue
        v = next(iter(fv))
        sub = {e: o for e, o in po.items() if o.get(x, "(default)") == v}
        if not sub or len(sub) == len(po):
            continue
        sep = []
        for k in sorted(set().union(*fo.values(), *sub.values())):
            if k == x:
                continue
            a_ = {o.get(k, "(default)") for o in fo.values()}
            b_ = {o.get(k, "(default)") for o in sub.values()}
            if not a_ & b_:
                sep.append({"option": k, "failing": sorted(a_), "passing": sorted(b_)})
        relm = set(MODULE_OPTIONS.get(p.get("mod") or "", COMMON_CPU))
        sep.sort(key=lambda y: (y["option"] not in relm, y["option"]))
        out["within"].append({"among": f"{x}={v}", "passing": sorted(sub), "separating": sep[:8]})
    rel = set(MODULE_OPTIONS.get(p.get("mod") or "", COMMON_CPU)) | {"jit"}
    for x in out["separating"]:
        x["affects_module"] = x["option"] in rel
    out["separating"].sort(key=lambda x: (not x["affects_module"], x["option"]))
    out["separating"] = out["separating"][:12]
    return out


# --------------------------------------------------------------- emulator log
def emulator_log(p, ctx, limit=10):
    mod = p.get("mod")
    out = []
    seen = set()
    for it in problem_items(p, ctx):
        key = (it["entry"], it.get("disk"))
        if key in seen:
            continue
        seen.add(key)
        m = ctx.mined(*key)
        x = logmine.for_module(m, mod, limit)
        if x:
            out.append(dict(x, entry=it["entry"], disk=it.get("disk"),
                            raw=(ctx.sub(*key).get("raw") or "") + "amiberry.log"))
        if len(out) >= 6:
            break
    return out


def fill_log_counts(results, results_dir):
    """log_counts (logmine categories) on every launch and entry result."""
    for r in results:
        tot = {}
        for s in (r.get("disks") or [r]):
            raw = s.get("raw")
            if not raw or not results_dir:
                continue
            m = logmine.mine(os.path.join(results_dir, raw))
            if m is None:
                continue
            s["log_counts"] = dict(s.get("log_counts") or {}, **m["counts"])
            s["log_mining"] = {"time_source": m["time_source"], "exact": m["exact"],
                               "groups": [{k: g[k] for k in ("category", "pattern", "modules", "count", "addresses")}
                                          for g in m["groups"][:30]]}
            for k, v in m["counts"].items():
                tot[k] = tot.get(k, 0) + v
        if r.get("disks"):
            r["log_counts"] = tot


# --------------------------------------------------------------- all
def _jit_only(p):
    nar = p.get("narrowing") or {}
    return any(f.get("axis") == "jit" for f in nar.get("flips") or []) or bool(p.get("engine_mismatch_also")) \
        or p.get("kind") == "engine_mismatch"


def jit_pointers(p):
    """JIT-only problems: name the code generator of this host."""
    from . import codemap
    if p.get("kind") not in pm.GROUPED or not _jit_only(p):
        return
    have = {x.get("file") for x in p.get("pointers") or []}
    for f, why in codemap.jit_backend_files():
        if "src/" + f not in have:
            p.setdefault("pointers", []).append({"file": "src/" + f, "function": None, "confidence": "hint",
                                                 "why": f"JIT backend of this host: {why}"})


def enrich(probs, items, results, fps, results_dir):
    """Adds the diagnosis aids; reorders and renumbers the problems (call it
    before anything is written under the problem ids)."""
    ctx = Ctx(results, fps, items, results_dir)
    for p in probs:
        if p.get("kind") not in pm.GROUPED:
            continue
        p["case_table"] = case_table(p, ctx)
        p["config_flags"] = config_flags(p, ctx)
        p["passes_diff"] = passes_diff(p, ctx)
        if p["passes_diff"]:
            p["passes_on"] = p["passes_diff"]["passes_on"]
        p["emulator_log"] = emulator_log(p, ctx)
        if p["config_flags"]["all_conflict"]:
            p["severity"] = "low"
            p["title"] = "[config contradicts the cases' stated condition] " + p["title"]
    order = {"crash": 0, "hang": 1, "case_fail": 2, "engine_mismatch": 3, "timeout": 4, "incomplete": 5}
    probs.sort(key=lambda p: (p.get("severity") == "low", order.get(p["kind"], 9), -len(p["failures"]), p["title"]))
    for n, p in enumerate(probs):
        p["id"] = f"P{n + 1:02d}"
    return probs
