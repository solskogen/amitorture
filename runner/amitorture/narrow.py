"""Automatic narrowing: which settings flip a failure.

For every (entry, module) with failing cases, the runner reruns only that
module (run parameter modules=<1<<id>, same seed; CONTRACT §4) on:
  1. the same config, N times            -> deterministic / intermittent + rate
  2. neighbouring configs, one setting changed at a time (matrix.neighbours):
     JIT off/on, each comp_* option, the cache size, fast/compat/CE, CPU
     model, FPU native/softfloat, threaded off/on, chipset, PAL/NTSC
  3. the other binaries given with --narrow-binary
and records for every failing case which changes make it pass ("flip") and
which do not ("hold"). Result: a statement like "fails only with JIT on and
comp_nf=true".

A case is *bad* in a run when:
  case_fail        its st is "fail"
  engine_mismatch  its fingerprint differs from the reference entry's
It is *good* when st is "pass" (case_fail) or the fingerprints match
(engine_mismatch); anything else (missing, skip, crash) is inconclusive.

The whole thing is capped by --narrow-budget seconds (wall time of the
launches); what was not tried is listed.

`analyze()` is pure (unit-tested); `run()` does the launches via a callback.
"""
from __future__ import annotations

import time

from . import cases as casemod
from . import emulator, matrix

AXIS_ORDER = ["jit", "comp_nf", "comp_constjump", "comp_flushmode", "comp_catchfault", "comp_trustbyte",
              "comp_trustword", "comp_trustlong", "comp_trustnaddr", "compfpu", "cachesize", "core", "cpu_model",
              "fpu_softfloat", "cpu_threaded", "chipset", "video", "binary"]


def outcome(fp_entry, key, kind, ref_fp=None):
    """'bad' | 'good' | 'inconclusive:<why>' for one case in one run."""
    if fp_entry is None:
        return "inconclusive:run failed"
    c = fp_entry["cases"].get(key)
    if c is None:
        return f"inconclusive:case missing (entry {fp_entry.get('status')})"
    if kind == "engine_mismatch":
        if ref_fp is None:
            return "inconclusive:no reference"
        return "bad" if c["fp"] != ref_fp else "good"
    if c["st"] == "fail":
        return "bad"
    if c["st"] == "pass":
        return "good"
    return f"inconclusive:st={c['st']}"


def current_setting(axis, options, binary_label=None):
    o = options
    if axis == "jit":
        return "JIT on" if int(o.get("cachesize", "0") or 0) > 0 else "JIT off"
    if axis == "core":
        return f"core={matrix._core_of(o)}"
    if axis == "fpu_softfloat":
        return "softfloat FPU" if o.get("fpu_softfloat") == "true" else "native FPU"
    if axis == "video":
        return "NTSC" if o.get("ntsc") == "true" else "PAL"
    if axis == "binary":
        return f"binary {binary_label or 'under test'}"
    if axis == "compfpu":
        return f"compfpu={o.get('compfpu', 'true')}"
    return f"{axis}={o.get(axis, matrix.JIT_DEFAULTS.get(axis, '?'))}"


def _axis_rank(axis):
    return AXIS_ORDER.index(axis) if axis in AXIS_ORDER else len(AXIS_ORDER)


def analyze(case_key, kind, options, base_runs, variants, ref_fp=None, binary_label=None, budget_left=None):
    """base_runs: [fp_entry,...] (reruns of the failing config);
    variants: [{"axis","label","changes","fp": fp_entry or None, "ref_fp": optional}].
    Returns the narrowing result for one case."""
    outs = [outcome(f, case_key, kind, ref_fp) for f in base_runs]
    bad = sum(1 for x in outs if x == "bad")
    good = sum(1 for x in outs if x == "good")
    n = bad + good
    if n == 0:
        det = "not reproduced (no conclusive rerun)"
    elif bad == n:
        det = "deterministic"
    elif bad == 0:
        det = "not reproduced"
    else:
        det = "intermittent"
    flips, holds, incon = [], [], []
    for v in variants:
        vref = ref_fp
        if kind == "engine_mismatch" and "ref_run" in v:
            # the reference machine with the same change, run alongside
            c = v["ref_run"]["cases"].get(case_key) if v["ref_run"] else None
            vref = c["fp"] if c else None
        o = outcome(v.get("fp"), case_key, kind, vref)
        rec = {"axis": v["axis"], "label": v["label"], "changes": v.get("changes", {})}
        if o == "good":
            flips.append(rec)
        elif o == "bad":
            holds.append(rec)
        else:
            incon.append(dict(rec, why=o.split(":", 1)[-1]))
    if n and bad == 0:
        # never reproduced: a variant that "passes" proves nothing
        incon = incon + [dict(r, why="original config did not reproduce either") for r in flips]
        flips = []
    flips.sort(key=lambda r: _axis_rank(r["axis"]))
    holds.sort(key=lambda r: _axis_rank(r["axis"]))
    stmt = statement(flips, holds, incon, options, det, bad, n, binary_label)
    sig = tuple(sorted(f"{r['axis']}={r['label']}" for r in flips))
    return {"case_key": case_key, "kind": kind, "determinism": det, "fail_rate": f"{bad}/{n}" if n else "0/0",
            "reruns": outs, "flips": flips, "holds": holds, "inconclusive": incon, "statement": stmt,
            "signature": list(sig), "budget_left_s": budget_left}


def statement(flips, holds, incon, options, det, bad, n, binary_label=None):
    pre = ""
    if det == "intermittent":
        pre = (f"intermittent ({bad}/{n} reruns fail; each variant ran once, so a pass there may be chance): ")
    elif det.startswith("not reproduced"):
        pre = f"not reproduced in {n} rerun(s) of the module alone: "
    if not flips and not holds:
        return pre + "no neighbouring config gave a conclusive result" + (
            f" ({len(incon)} inconclusive)" if incon else " (none tried)")
    parts = []
    if flips:
        axes = []
        for r in flips:
            s = current_setting(r["axis"], options, binary_label)
            if s not in axes:
                axes.append(s)
        parts.append("fails only with " + " and ".join(axes))
        parts.append("passes with " + ", ".join(f"{r['axis']}={r['label']}" for r in flips))
    else:
        parts.append("fails with every neighbouring setting tried")
    if holds:
        parts.append("still fails with " + ", ".join(f"{r['axis']}={r['label']}" for r in holds))
    if incon:
        parts.append(f"inconclusive: {', '.join(r['axis'] + '=' + r['label'] for r in incon)}")
    return pre + "; ".join(parts)


def groups_from(failures, results):
    """[(entry result, module name, {case_key: kind, ...}, {case_key: ref_fp})] to narrow.
    failures: items of kind case_fail / engine_mismatch (not xfail)."""
    by_id = {r["id"]: r for r in results}
    groups = {}
    for it in failures:
        if it.get("kind") not in ("case_fail", "engine_mismatch") or it.get("xfail"):
            continue
        r = by_id.get(it["entry"])
        if r is None or it.get("mod") in (None, "probe"):
            continue
        key = it.get("case_key") or f"{it['mod']}.{it['case']}"
        g = groups.setdefault((r["id"], it["mod"]), {"result": r, "mod": it["mod"], "cases": {}, "ref_fp": {},
                                                    "ref_entry": it.get("ref_entry"), "whys": []})
        if it.get("why") and it["why"] not in g["whys"]:
            g["whys"].append(it["why"])
        if g["cases"].get(key) == "case_fail":
            continue          # a failing case is the stronger statement than "differs from the reference"
        g["cases"][key] = it["kind"]
        if it["kind"] == "engine_mismatch" and isinstance(it.get("ref"), dict):
            g["ref_fp"][key] = casemod.canonical(it["ref"])
        else:
            g["ref_fp"].pop(key, None)
    return list(groups.values())


# Axes that keep the guest machine identical to a twin's reference entry:
# for an engine_mismatch the reference fingerprint stays valid. For the
# others the reference entry is rerun with the same change.
ENGINE_AXES = {"jit", "cachesize", "cpu_threaded", "binary", "compfpu", *matrix.JIT_DEFAULTS}


DISPLAY_MODULES = ("display", "copper", "dmacontention")
GFX_RES = ("lores", "hires", "superhires")


def module_variants(entry, mod, whys=()):
    """Variants that separate causes for this module (tried before the
    generic neighbours): [(axis, label, entry)].
      MMU entries        mmu_model=0 (the MMU core vs the plain one)
      020/030            cpu_24bit_addressing toggled
      display modules    gfx_resolution (Amiberry renders and collides at
                         the output resolution)
      68060 integer      cpu_no_unimplemented toggled
      fpu                fpu_no_unimplemented, fpu_strict toggled
      paula              floppy_speed=100 when it is not
      named in a why     option=value the case text names (precondition)"""
    import copy
    from . import amiberry_cfg, diagnose
    o = dict(amiberry_cfg.COMMON, **entry["options"])
    out = []

    def var(axis, label, changes):
        if all(str(o.get(k)) == str(v) for k, v in changes.items()):
            return
        n = copy.deepcopy(entry)
        n["options"].update(changes)
        n["id"] = f"{entry['id']}~{axis}={label}"
        n["title"] = f"{entry['title']} [narrowing: {axis}={label}]"
        n["narrow"] = {"axis": axis, "label": label, "changes": changes, "of": entry["id"]}
        n["twin_of"], n["twin_kind"] = None, None
        n["jit"] = int(n["options"].get("cachesize", "0") or 0) > 0
        out.append((axis, label, n))
    cpu = str(o.get("cpu_model"))
    if str(o.get("mmu_model", "0")) not in ("0", ""):
        var("mmu_model", "0", {"mmu_model": "0"})
    if cpu in ("68020", "68030"):
        v = "false" if o.get("cpu_24bit_addressing") == "true" else "true"
        ch = {"cpu_24bit_addressing": v}
        if v == "true":
            # no RAM above 16 MB on a 24-bit bus (Amiberry stops otherwise)
            ch.update({k: "0" for k in ("z3mem_size", "mbresmem_size", "z3mem2_size", "megachipmem_size")
                       if str(o.get(k, "0")) not in ("0", "")})
        var("cpu_24bit_addressing", v, ch)
    if mod in DISPLAY_MODULES:
        for r in GFX_RES:
            var("gfx_resolution", r, {"gfx_resolution": r})
    if cpu == "68060" and mod in ("exceptions", "cpu_basic", "dispatch"):
        v = "false" if o.get("cpu_no_unimplemented") == "true" else "true"
        var("cpu_no_unimplemented", v, {"cpu_no_unimplemented": v})
    if mod == "fpu":
        for k in ("fpu_no_unimplemented", "fpu_strict"):
            v = "false" if o.get(k) == "true" else "true"
            var(k, v, {k: v})
    if mod == "paula" and str(o.get("floppy_speed", "100")) != "100":
        var("floppy_speed", "100", {"floppy_speed": "100"})
    for w in whys or ():
        for name, val, precond in diagnose._named_conditions(w):
            if precond and (name in o or name in diagnose.MODULE_OPTIONS.get(mod, ())):
                var(name, val, {name: val})
    seen, uniq = set(), []
    for t in out:
        if t[2]["id"] not in seen:
            seen.add(t[2]["id"])
            uniq.append(t)
    return uniq


def is_noop(entry, variant):
    """A core-mode variant that runs the same CPU table (e.g. core=fast on an
    MMU machine: the MMU core either way) changes nothing worth a launch."""
    from . import codemap
    ch = (variant.get("narrow") or {}).get("changes") or {}
    if not ch or not set(ch) <= {"cpu_compatible", "cpu_cycle_exact", "cpu_memory_cycle_exact", "blitter_cycle_exact",
                                 "cycle_exact", "cpu_speed"}:
        return False
    a, b = codemap.core_table(entry["options"]), codemap.core_table(variant["options"])
    return a.get("table") == b.get("table") and ch.keys() <= {"cpu_compatible", "cpu_speed"}


def _priority(axis, kinds, entry):
    """Order of the variants within a group: the axes most likely to flip
    the failure first (the twin's own axis for an engine mismatch)."""
    jit_on = int(entry["options"].get("cachesize", "0") or 0) > 0
    if "engine_mismatch" in kinds:
        first = ["comp_nf", "comp_constjump", "comp_trustbyte", "comp_trustlong", "comp_trustword",
                 "comp_trustnaddr", "comp_flushmode", "comp_catchfault", "compfpu", "cachesize", "cpu_threaded",
                 "jit", "binary", "cpu_model", "fpu_softfloat", "core", "chipset", "video"]
    elif jit_on:
        first = ["jit", "comp_nf", "comp_constjump", "comp_trustbyte", "comp_trustlong", "compfpu", "cachesize",
                 "binary", "cpu_model", "fpu_softfloat", "cpu_threaded", "chipset", "video", "comp_trustword",
                 "comp_trustnaddr", "comp_flushmode", "comp_catchfault"]
    else:
        first = ["jit", "core", "binary", "cpu_model", "fpu_softfloat", "cpu_threaded", "chipset", "video"]
    return first.index(axis) if axis in first else len(first)


def run(groups, launch, entry_of, module_id, budget_s, reruns=3, binaries=(), log=print, clock=time.monotonic,
        jobs=1, known=None, disk_of=None):
    """Runs the narrowing launches, breadth-first over the groups so that a
    small budget still gives every group its most informative results:
      round 1   one rerun of every group (module alone; a group whose
                failures do not reproduce that way switches to the full disk)
      then      round-robin: each group's next task (the other reruns
                interleaved with the variants, most informative axis first)
    `jobs` launches run at once (each launch holds a machine-wide emulator
    slot). Budget: no launch starts after budget_s seconds of wall time.
    `known`: [(options, fp_entry, id)] of runs already made (the main run's
    entries): a variant with exactly those options reuses the result instead
    of launching (e.g. jit=off of a JIT twin is its reference entry; that
    run had the full disk, which only matters for order-dependent failures).

    launch(entry, binary=None) -> fp_entry (cases.extract of the result)
    entry_of(result_or_id)     -> the matrix entry (dict with options) that ran
    module_id(mod)             -> module id or None
    Returns {"groups": [...], "budget_s", "used_s", "launches", "exhausted"}."""
    import threading
    t0 = clock()
    lock = threading.Lock()
    state = {"launches": 0, "exhausted": False}
    out_groups, work = [], []

    def left():
        return budget_s - (clock() - t0)

    for g in groups:
        r = g["result"]
        entry = entry_of(r)
        mid = module_id(g["mod"])
        res = {"entry": r["id"], "mod": g["mod"], "module_id": mid, "cases": {}, "launches": [], "not_tried": []}
        out_groups.append(res)
        if mid is None or entry is None:
            res["note"] = f"module id of {g['mod']} unknown: cannot mask" if mid is None else "entry unknown"
            continue
        mismatch = any(k == "engine_mismatch" for k in g["cases"].values())
        disk = disk_of(r, g["mod"]) if disk_of else None
        res["disk"] = disk
        w = {"g": g, "res": res, "entry": entry, "params": dict(entry.get("params", {}), modules=f"{1 << mid:#x}"),
             "disk": disk,
             "base_runs": [], "variants": [], "tasks": [],
             "ref_entry": entry_of(g["ref_entry"]) if mismatch and g.get("ref_entry") else None,
             "mismatch": mismatch}
        special = module_variants(entry, g["mod"], g.get("whys"))
        sp_axes = [a for a, _, _ in special]
        nb = []
        for axis, label, ne in matrix.neighbours(entry):
            if is_noop(entry, ne):
                res.setdefault("skipped_noop", []).append(f"{axis}={label} (same CPU core table: no effect)")
                continue
            nb.append((axis, label, ne))
        nb = special + [t for t in nb if t[2]["id"] not in {x[2]["id"] for x in special}]
        variants = [("variant", axis, label, ne, None) for axis, label, ne in nb]
        variants += [("variant", "binary", b.get("label"), dict(entry, id=f"{entry['id']}~binary={b.get('label')}",
                                                                 narrow={"changes": {"binary": b.get("path")}}),
                      b.get("path")) for b in binaries]
        variants.sort(key=lambda t: (-1, sp_axes.index(t[1])) if t[1] in sp_axes else
                      (0, _priority(t[1], set(g["cases"].values()), entry)))
        reruns_t = [("rerun", "rerun", str(i + 1), dict(entry, id=f"{entry['id']}~rerun={i + 1}"), None)
                    for i in range(1, reruns)]
        # interleave: variant, variant, rerun, variant, variant, rerun, ...
        tasks = []
        while variants or reruns_t:
            for _ in range(2):
                if variants:
                    tasks.append(variants.pop(0))
            if reruns_t:
                tasks.append(reruns_t.pop(0))
        w["first"] = ("rerun", "rerun", "1", dict(entry, id=f"{entry['id']}~rerun=1"), None) if reruns > 0 else None
        w["tasks"] = tasks
        work.append(w)

    def go(w, e, axis, label, binary=None):
        res = w["res"]
        with lock:
            if left() <= 0:
                state["exhausted"] = True
                res["not_tried"].append(f"{axis}={label}")
                return None, False
        e = dict(e)
        e["params"] = dict(w["params"])
        if w.get("disk"):
            e["disk_only"] = w["disk"]     # only the disk of the set that has this module
        t = clock()
        fp = launch(e, binary)
        with lock:
            state["launches"] += 1
            res["launches"].append({"axis": axis, "label": label, "entry": e["id"], "binary": binary,
                                    "status": None if fp is None else fp.get("status"),
                                    "seconds": round(clock() - t, 1)})
        return fp, True

    def reuse(e, binary):
        if binary or not known:
            return None
        for opts, fp, kid in known:
            if fp is not None and not matrix.option_diff(opts, e["options"]):
                return fp, kid
        return None

    def do_task(w, task):
        kind, axis, label, e, binary = task
        hit = reuse(e, binary) if kind == "variant" else None
        if hit is not None:
            fp, ok = hit[0], True
            with lock:
                w["res"]["launches"].append({"axis": axis, "label": label, "entry": e["id"], "reused": hit[1],
                                             "status": fp.get("status"), "seconds": 0})
        else:
            fp, ok = go(w, e, axis, label, binary)
        if not ok:
            return
        if kind == "rerun":
            w["base_runs"].append(fp)
            return
        v = {"axis": axis, "label": label, "changes": (e.get("narrow") or {}).get("changes", {}), "fp": fp}
        if w["mismatch"] and axis not in ENGINE_AXES:
            re_ = w["ref_entry"]
            if re_ is None:
                v["ref_run"] = None
            else:
                re2 = dict(re_, options=dict(re_["options"], **v["changes"]), id=f"{re_['id']}~{axis}={label}")
                hit = reuse(re2, None)
                rfp, rok = hit if hit is not None else go(w, re2, f"{axis}(reference)", label)
                v["ref_run"] = rfp if rok else None
        w["variants"].append(v)

    def run_batch(pairs):
        if jobs <= 1 or len(pairs) <= 1:
            for w, t in pairs:
                do_task(w, t)
            return
        emulator.run_parallel(lambda wt: do_task(*wt), pairs, jobs)

    # round 1: one rerun per group
    run_batch([(w, w["first"]) for w in work if w["first"]])
    for w in work:
        g = w["g"]
        if w["base_runs"] and all(outcome(f, k, kind, g["ref_fp"].get(k)) != "bad"
                                  for f in w["base_runs"] for k, kind in g["cases"].items()):
            w["res"]["isolation_note"] = ("no failing case reproduced with the module alone; the other runs use "
                                          "the full disk")
            # the full disk, as far as the entry runs it (a board entry's mask)
            if "modules" in (w["entry"].get("params") or {}):
                w["params"]["modules"] = w["entry"]["params"]["modules"]
            else:
                w["params"].pop("modules", None)
    # then round-robin over the groups, `jobs` tasks at a time
    queue = []
    depth = max((len(w["tasks"]) for w in work), default=0)
    for i in range(depth):
        for w in work:
            if i < len(w["tasks"]):
                queue.append((w, w["tasks"][i]))
    step = max(1, jobs)
    for i in range(0, len(queue), step):
        if left() <= 0:
            for w, t in queue[i:]:
                w["res"]["not_tried"].append(f"{t[1]}={t[2]}")
            state["exhausted"] = True
            break
        run_batch(queue[i:i + step])
    for w in work:
        g, res, entry = w["g"], w["res"], w["entry"]
        for key, kind in g["cases"].items():
            res["cases"][key] = analyze(key, kind, entry["options"], w["base_runs"], w["variants"],
                                        g["ref_fp"].get(key), budget_left=round(left(), 1))
            if res.get("skipped_noop"):
                res["cases"][key]["skipped_noop"] = res["skipped_noop"]
            if not res["launches"]:
                res["cases"][key].update(determinism="not narrowed", fail_rate="0/0",
                                         statement="not narrowed: --narrow-budget exhausted before this group")
        log(f"narrowing {res['entry']}/{g['mod']}: {len(res['launches'])} launch(es); "
            + "; ".join(f"{k}: {v['statement']}" for k, v in list(res["cases"].items())[:3]))
    return {"groups": out_groups, "budget_s": budget_s, "used_s": round(clock() - t0, 1),
            "launches": state["launches"], "exhausted": state["exhausted"]}
