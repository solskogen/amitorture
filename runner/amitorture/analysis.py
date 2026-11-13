"""After the entries of a `run` finished: fingerprints, engine parity,
xfail, narrowing, problems, code pointers, repro packs, history.

Everything here only reads results (and launches narrowing runs through the
same Context), so `run`, `check` and the unit tests share it."""
from __future__ import annotations

import json
import os

from . import adf as adfmod
from . import baseline as basemod
from . import cases as casemod
from . import codemap, diagnose, narrow, parity, problems
from . import xfail as xfailmod
from .runner import log


def module_id_lookup(fps):
    ids = casemod.module_ids_from_source(adfmod.REPO)
    for fp in fps.values():
        ids.update(fp.get("modules") or {})
    return ids


def failure_items(results, fps, registry):
    """report.collect_failures + case keys + xfail tags + parity items."""
    from . import report
    items = report.collect_failures(results)
    by_id = {r["id"]: r for r in results}
    for it in items:
        if it["kind"] == "case_fail":
            key = f"{it['mod']}.{it['case']}"
            it["case_key"] = key
            xf = xfailmod.match(registry, key, it["entry"], (by_id.get(it["entry"]) or {}).get("options"))
            if xf:
                it["xfail"] = xf
    par, infos = parity.check_all(results, fps)
    n = len(items)
    for i, it in enumerate(par):
        it["id"] = f"F{n + i + 1:03d}"
        r = by_id.get(it["entry"]) or {}
        it.update({"jit": r.get("jit"), "twin_of": r.get("twin_of"), "machine": r.get("machine"),
                   "rom": (r.get("rom") or {}).get("name"), "config": (r.get("config") or {}).get("file")})
    items.extend(par)
    hrm = hrm_timing_items(results, fps)
    n = len(items)
    for i, it in enumerate(hrm):
        it["id"] = f"F{n + i + 1:03d}"
    items.extend(hrm)
    thr = thread_speed_items(results)
    n = len(items)
    for i, it in enumerate(thr):
        it["id"] = f"F{n + i + 1:03d}"
    items.extend(thr)
    return items, infos


# Threaded CPU twins (report.speed_summary pairs: entries that differ only in
# cpu_threaded, same binary, parameters and run, so the same host). At
# cpu_speed=max a single block's rtf says little (Amiberry gives the CPU the
# host time a frame leaves, so the frames stay on time), but the threaded CPU
# can break exactly that: its hardware accesses are hand-offs to the main
# thread, and a flood of them held the chipset below real time (Kickstart
# 1.3's Alert loop, hostspeed cia_write). Its idle CPU can also spin a host
# core where the unthreaded one sleeps (hostspeed stop_timer). Both are
# compared with the unthreaded twin, not with a fixed host figure.
THREAD_RTF_MARGIN = 0.03         # threaded rtf over real time and the twin's, beyond both 3 sigma
THREAD_IDLE_BLOCKS = ("stop", "stop_timer")
THREAD_IDLE_COST_MAX = 1.25      # total CPU/s over the twin's, x
THREAD_IDLE_COST_SLACK = 0.05    # + host CPU s per emulated s
THREAD_REFLOODS = 2              # re-floods in a row that mean the main thread's count misses floods
THREAD_RECOVERY_SHARE = 0.6      # flood_recovery: the CPU thread's share of the quiet work
                                 # (held on the main thread: ~0.2, the other threads; back: ~1)


def thread_speed_items(results):
    """`thread_speed` failure items: a threaded entry that falls behind real
    time on a hostspeed block where its unthreaded twin keeps up, or whose
    idle CPU (THREAD_IDLE_BLOCKS) costs much more host CPU than the twin's."""
    from . import report
    out = []
    by_id = {r["id"]: r for r in results}
    speeds = report.speed_summary(results)
    by_entry = {(e["entry"], e.get("disk")): e for e in speeds}
    # noisy blocks too ("untestable" for the benchmark's 5 % limit): the
    # comparison widens its margin by both 3 sigma instead
    timed = ("measured", "pass", "fail", "untestable")
    for on in speeds:
        off = by_entry.get((on.get("thread_reference"), on.get("disk")))
        if not on.get("cpu_threaded") or off is None:
            continue
        refs = {b["insn"]: b for b in off["blocks"]}
        r = by_id.get(on["entry"]) or {}
        # Amiberry reports its own counting mismatches: a flood hold ended by
        # a count on the main thread that saw no flood, while the thread then
        # flooded again at once. Twice in a row and the workload makes the
        # CPU go back and forth between the threads (each move flushing the
        # JIT), wherever the host is slow enough to lose it.
        if (on.get("refloods") or 0) >= THREAD_REFLOODS:
            out.append({"kind": "thread_speed", "entry": on["entry"], "ref_entry": off["entry"], "mod": "host",
                        "case": "thread_refloods", "case_key": "host.thread_refloods", "disk": on.get("disk"),
                        "why": (f"the CPU went back to its thread when the count on the main thread saw no flood, "
                                f"and flooded again at once, {on['refloods']} times in a row: the count misses "
                                f"requests the thread makes (Amiberry newcpu.cpp cpu_thread_count_read, "
                                f"cpu_thread_snap_read)"),
                        "ref": {}, "twin": {"refloods": on["refloods"], "flood_holds": on.get("flood_holds")},
                        "differing_options": [{"option": "cpu_threaded", "ref": "false", "twin": "true"}],
                        "host": casemod.host_desc(on.get("host")), "jit": r.get("jit"),
                        "machine": r.get("machine"), "config": (r.get("config") or {}).get("file")})
        for b in on["blocks"]:
            if b["insn"] == "flood_recovery":
                qs, fs = b.get("quiet_thread_share"), b.get("flood_thread_share")
                # Judged when the emulator's flood fallback was live in this
                # run (its log counts the holds; cia_write is the same flood):
                # the flood phases' own share says nothing then, as a flood
                # that comes just after a retry is not measured and runs on
                # the thread. Without that count (older results), floods the
                # thread kept up with (flood share high) are not judged.
                holds = on.get("flood_holds")
                exercised = holds > 0 if holds is not None else (fs is None or fs < THREAD_RECOVERY_SHARE)
                if qs is not None and qs < THREAD_RECOVERY_SHARE and exercised:
                    keys = ("quiet_thread_share", "flood_thread_share", "quiet_samples")
                    out.append({"kind": "thread_speed", "entry": on["entry"], "ref_entry": off["entry"], "mod": "host",
                                "case": casemod.speed_name(b["insn"]), "case_key": SPEED_KEY + str(b["insn"]),
                                "disk": on.get("disk"),
                                "why": (f"flood_recovery: after each flood of CIA writes moved the CPU to the main "
                                        f"thread, the CPU thread ran only {qs:.0%} of the quiet work that followed "
                                        f"(at least {THREAD_RECOVERY_SHARE:.0%} expected): the CPU stays off its thread "
                                        f"long after the flood (Amiberry newcpu.cpp cpu_thread_check_flood)"),
                                "ref": {k: refs.get(b["insn"], {}).get(k) for k in keys
                                        if refs.get(b["insn"], {}).get(k) is not None},
                                "twin": {k: b.get(k) for k in keys if b.get(k) is not None},
                                "differing_options": [{"option": "cpu_threaded", "ref": "false", "twin": "true"}],
                                "host": casemod.host_desc(on.get("host")), "jit": r.get("jit"),
                                "machine": r.get("machine"), "config": (r.get("config") or {}).get("file")})
                continue
            a = refs.get(b["insn"])
            if a is None or a.get("st") not in timed or b.get("st") not in timed:
                continue
            why = None
            if a.get("rtf") is not None and b.get("rtf") is not None:
                sa, sb = 3 * (a.get("sigma") or 0), 3 * (b.get("sigma") or 0)
                keeps_up = a["rtf"] <= casemod.SPEED_RTF_LIMIT + sa
                # behind real time itself: an unthreaded twin that runs ahead
                # of real time (rtf < 1, the JIT at cpu_speed=max) is no bar
                if keeps_up and b["rtf"] > max(a["rtf"], 1.0) + THREAD_RTF_MARGIN + sa + sb:
                    why = (f"{b['insn']}: with the threaded CPU the emulator ran at {1 / b['rtf']:.2f}x real time "
                           f"(rtf {b['rtf']:.3f} +- {sb:.3f}), where the unthreaded twin {off['entry']} kept up "
                           f"(rtf {a['rtf']:.3f} +- {sa:.3f}): the chipset fell behind serving the CPU thread "
                           f"(Amiberry newcpu.cpp run_cpu_thread, service_cpu_indirect_request)")
            ca, cb = a.get("cpu_total_per_emu_s"), b.get("cpu_total_per_emu_s")
            if why is None and b["insn"] in THREAD_IDLE_BLOCKS and ca is not None and cb is not None:
                limit = ca * THREAD_IDLE_COST_MAX + THREAD_IDLE_COST_SLACK
                if cb > limit:
                    why = (f"{b['insn']}: with the CPU idle in STOP the threaded CPU cost {cb:.3f} host CPU s per "
                           f"emulated s, the unthreaded twin {off['entry']} {ca:.3f} (limit {limit:.3f}): the idle "
                           f"CPU thread spins instead of sleeping (Amiberry newcpu.cpp cpu_thread_idle_wait, "
                           f"cpu_thread_main_wait_wake)")
            if why is None:
                continue
            keys = ("rtf", "sigma", "cpu_total_per_emu_s")
            out.append({"kind": "thread_speed", "entry": on["entry"], "ref_entry": off["entry"], "mod": "host",
                        "case": casemod.speed_name(b["insn"]), "case_key": SPEED_KEY + str(b["insn"]),
                        "disk": on.get("disk"), "why": why,
                        "ref": {k: a.get(k) for k in keys if a.get(k) is not None},
                        "twin": {k: b.get(k) for k in keys if b.get(k) is not None},
                        "differing_options": [{"option": "cpu_threaded", "ref": "false", "twin": "true"}],
                        "host": casemod.host_desc(on.get("host")), "jit": r.get("jit"), "machine": r.get("machine"),
                        "config": (r.get("config") or {}).get("file")})
    return out


SPEED_KEY = "host.speed_"


def _num(v):
    if isinstance(v, bool):
        return None
    if isinstance(v, (int, float)):
        return v
    try:
        return float(v)
    except (TypeError, ValueError):
        return None


def hrm_timing_items(results, fps):
    """Cycle-exact entries only: every timing field `...hrm_<x>` (the Hardware
    Reference Manual's exact figure, e.g. dmacontention's timing.hrm_in_fetch)
    is compared with its measured partner `...<x>` in the same object. A
    difference is an `hrm_timing_mismatch` item: informational (low
    confidence, so it lands in Notes, not in the verdict, and is never a
    baseline regression). Other core modes are legitimately not cycle-exact."""
    out = []
    by_id = {r["id"]: r for r in results}
    for eid, fp in fps.items():
        r = by_id.get(eid) or {}
        if str((r.get("options") or {}).get("cycle_exact", "false")).lower() != "true":
            continue
        for key, c in (fp or {}).get("cases", {}).items():
            timing = {}
            for tp, tv in (c.get("timing") or {}).items():    # {"timing": {...}} -> {"timing.x": n}
                timing.update(casemod.numeric_leaves(tv, tp))
            for path, hv in timing.items():
                head, _, leaf = path.rpartition(".")
                if not leaf.startswith("hrm_"):
                    continue
                partner = (head + "." if head else "") + leaf[4:]
                if partner not in timing:
                    continue
                h, m = _num(hv), _num(timing[partner])
                if h is None or m is None or h == m:
                    continue
                out.append({"kind": "hrm_timing_mismatch", "entry": eid, "mod": c.get("mod"), "case": c.get("case"),
                            "case_key": key, "path": partner, "hrm": hv, "measured": timing[partner],
                            "confidence": "low", "machine": r.get("machine"),
                            "why": f"{key}: measured {partner} = {timing[partner]}, the HRM figure ({path}) is {hv} "
                                   "on a cycle-exact entry"})
    return out


def analyze_run(opts, ctx, entries, results):
    fps = {r["id"]: casemod.extract(r) for r in results}
    registry = xfailmod.load(opts.xfail_path)
    items, parity_info = failure_items(results, fps, registry)
    xf, fixed = xfailmod.classify(registry, fps, results)
    entries_by_id = {e["id"]: e for e in entries}
    mids = module_id_lookup(fps)
    src_path = codemap.find_src(opts.binary, opts.amiberry_src)
    src = codemap.Source(src_path) if src_path else None
    # narrowing
    narrowing = None
    groups = narrow.groups_from(items, results)
    if groups and opts.narrow and opts.narrow_budget > 0 and not opts.no_disk:
        def launch(e, binary=None):
            r = ctx.run_entries([e], binary=binary)[0]
            return casemod.extract(r)

        def entry_of(x):
            eid = x if isinstance(x, str) else x["id"]
            return entries_by_id.get(eid)

        bins = [{"label": os.path.basename(os.path.dirname(os.path.dirname(b))) + "/" + os.path.basename(b)
                 if b else b, "path": b} for b in opts.narrow_binaries]
        log(opts, f"narrowing {len(groups)} (entry, module) group(s), budget {opts.narrow_budget:.0f} s")
        def disk_of(result, mod):
            for sub in (result.get("disks") or []):
                if mod in (sub.get("modules") or {}):
                    return sub.get("disk_name")
            return None

        narrowing = narrow.run(groups, launch, entry_of, mids.get, opts.narrow_budget, reruns=opts.narrow_reruns,
                               disk_of=disk_of,
                               binaries=bins, log=lambda m: log(opts, m), jobs=opts.jobs,
                               known=[(r.get("options") or {}, fps.get(r["id"]), r["id"]) for r in results
                                      if r.get("status") in ("pass", "fail")])
        with open(os.path.join(ctx.results, "narrowing.json"), "w", encoding="utf-8") as fh:
            json.dump(narrowing, fh, indent=1)
    elif groups:
        narrowing = {"groups": [], "skipped": "narrowing off (--no-narrow or --narrow-budget 0)",
                     "budget_s": opts.narrow_budget, "used_s": 0, "launches": 0}
    probs = problems.group(items, results, narrowing)
    diagnose.fill_log_counts(results, ctx.results)
    diagnose.enrich(probs, items, results, fps, ctx.results)
    items_by_id = {i["id"]: i for i in items}
    by_res = {r["id"]: r for r in results}
    base = basemod.load(opts.workdir, opts.baseline) if opts.baseline else None
    for p in probs:
        rep = items_by_id[p["representative"]]
        r = by_res.get(rep["entry"]) or {}
        p["pointers"] = problems.pointers_for(rep, r, src) if p["kind"] in problems.GROUPED else []
        diagnose.jit_pointers(p)
        p["core"] = codemap.core_table(dict(r.get("options") or {}, **(r.get("resolved_options") or {}))) \
            if r.get("options") else None
        p["docs"] = {}
        for k in p["case_keys"]:
            mod, _, case = k.partition(".")
            d = problems.case_doc(mod, case.split("#")[0])
            if d:
                p["docs"][k] = d
        p["history"] = history(p, items_by_id, base, registry, by_res)
        e = entries_by_id.get(rep["entry"])
        if opts.repro and p["kind"] in problems.GROUPED and e is not None and not opts.no_disk:
            rom = r.get("rom") or {}
            romd = {"path": rom.get("source"), "crc32": rom.get("crc32"), "id": rom.get("id"),
                    "name": rom.get("name")} if rom.get("source") else None
            ext = r.get("ext_rom") or {}
            extd = {"path": ext.get("source"), "id": ext.get("id"), "name": ext.get("name")} if ext else None
            params = dict(opts.params, **e.get("params", {}))
            try:
                p["repro"] = problems.write_pack(
                    ctx.results, p, rep, e, rom=romd, ext_rom=extd,
                    adf_src=disk_with_module(r, rep.get("mod"), ctx.d.get("disks")) or ctx.d["adf_src"], params=params,
                    module_id=mids.get(rep.get("mod")), binary=opts.binary, kickstarts=os.path.abspath(opts.kickstarts),
                    amiberry_src=src_path, pointers=p["pointers"], bake=adfmod.bake,
                    warp=module_warp(r, rep.get("mod")))
            except Exception as ex:
                p["repro"] = {"error": f"{type(ex).__name__}: {ex}"}
    return {"fps": fps, "items": items, "parity": parity_info, "xfail": xf, "fixed": fixed, "problems": probs,
            "narrowing": narrowing, "amiberry_src": src_path, "baseline": opts.baseline,
            "module_ids": mids}


def module_warp(result, mod):
    return next((s.get("warp") for s in (result.get("disks") or [result])
                 if mod in (s.get("modules") or {})), None)


def disk_with_module(result, mod, disks):
    """Path of the disk of the set that ran module `mod` for this entry."""
    if not disks:
        return None
    paths = dict(disks)
    for sub in result.get("disks") or []:
        if mod in (sub.get("modules") or {}):
            return paths.get(sub.get("disk_name")) or (sub.get("disk") or {}).get("path")
    return None


def history(problem, items_by_id, base, registry, by_res):
    """What the baseline and the xfail registry say about the problem's cases."""
    out = []
    for fid in problem["failures"][:20]:
        it = items_by_id[fid]
        key = it.get("case_key")
        if not key:
            continue
        h = {"entry": it["entry"], "case_key": key}
        if it.get("xfail"):
            h["xfail"] = {k: it["xfail"].get(k) for k in ("ref", "why", "key")}
        if base is not None:
            be = base["entries"].get(it["entry"])
            if be is None:
                h["baseline"] = "entry not in the baseline"
            else:
                bc = be["cases"].get(key)
                if bc is None:
                    h["baseline"] = "case not in the baseline"
                else:
                    h["baseline"] = (f"{'stable' if bc['stable'] else 'unstable'} in baseline "
                                     f"{base['meta'].get('name')}, st {bc['st']}")
        out.append(h)
    return out


# ---------------------------------------------------------------------------
# rebuild a results dir from its saved files (no emulator)

def load_results(results_dir):
    """The entry results of a run directory (raw/*/result.json + records.jsonl),
    without the narrowing/repro launches (ids containing '~')."""
    from . import records as recmod
    from .emulator import RunnerError
    out = []
    raw = os.path.join(results_dir, "raw")
    for name in sorted(os.listdir(raw)):
        p = os.path.join(raw, name, "result.json")
        if not os.path.isfile(p):
            continue
        with open(p, encoding="utf-8") as fh:
            r = json.load(fh)
        if "~" in r.get("id", "") or r.get("narrow"):
            continue
        def merged_from(sub, rp):
            if not os.path.isfile(rp):
                if sub.get("status") in ("skip", "error") and not (sub.get("records") or {}).get("merged"):
                    return None
                raise RunnerError(f"saved records missing: {rp}")
            with open(rp, encoding="utf-8") as fh:
                recs = [json.loads(line) for line in fh if line.strip()]
            st = sub.get("records") or {}
            schema = list(st.get("schema_problems", []))
            if st.get("bad_count", 0) > len(st.get("bad", [])):
                schema.append({"problems": ["saved transport corruption metadata is incomplete"]})
            return recmod.Merged(recs, st.get("gaps", []), st.get("bad", []), st.get("conflicts", []),
                                 st.get("duplicates_dropped", 0), schema,
                                 st.get("per_source", {}))
        if r.get("disks"):
            from .runner import merge_disks
            for sub in r["disks"]:
                sub["_merged"] = merged_from(sub, os.path.join(results_dir, sub.get("raw") or "", "records.jsonl"))
            r["_merged"] = merge_disks(r["disks"])
        else:
            m = merged_from(r, os.path.join(raw, name, "records.jsonl"))
            if m is not None:
                r["_merged"] = m
        from .runner import integrate_host_verdicts
        integrate_host_verdicts(r)
        out.append(r)
    return out


def rebuild(results_dir, opts_like=None, repro=True, xfail_path=None):
    """Recomputes parity, xfail, problems, pointers (and repro packs) and
    rewrites REPORT.md/summary.json/failures.jsonl in results_dir."""
    from . import matrix, report
    with open(os.path.join(results_dir, "environment.json"), encoding="utf-8") as fh:
        env = json.load(fh)
    results = load_results(results_dir)
    narrowing = None
    np_ = os.path.join(results_dir, "narrowing.json")
    if os.path.isfile(np_):
        with open(np_, encoding="utf-8") as fh:
            narrowing = json.load(fh)
    fps = {r["id"]: casemod.extract(r) for r in results}
    registry = xfailmod.load(xfail_path)
    items, parity_info = failure_items(results, fps, registry)
    xf, fixed = xfailmod.classify(registry, fps, results)
    probs = problems.group(items, results, narrowing)
    diagnose.fill_log_counts(results, results_dir)
    diagnose.enrich(probs, items, results, fps, results_dir)
    binary = (env.get("amiberry") or {}).get("path")
    sp = codemap.find_src(binary)
    src = codemap.Source(sp) if sp else None
    mids = module_id_lookup(fps)
    items_by_id = {i["id"]: i for i in items}
    by_res = {r["id"]: r for r in results}
    ds = env.get("disk_source") or {}
    set_disks = [(d.get("name"), d.get("path")) for d in ds.get("disks") or []] or None
    adf_src = ds.get("path") if not set_disks else None
    for p in probs:
        rep = items_by_id[p["representative"]]
        r = by_res.get(rep["entry"]) or {}
        p["pointers"] = problems.pointers_for(rep, r, src) if p["kind"] in problems.GROUPED else []
        diagnose.jit_pointers(p)
        p["core"] = codemap.core_table(dict(r.get("options") or {}, **(r.get("resolved_options") or {}))) \
            if r.get("options") else None
        p["docs"] = {}
        for k in p["case_keys"]:
            mod, _, case = k.partition(".")
            d = problems.case_doc(mod, case.split("#")[0])
            if d:
                p["docs"][k] = d
        p["history"] = history(p, items_by_id, None, registry, by_res)
        e = matrix.find(rep["entry"]) or (dict(r, rom={"ids": []}) if r else None)
        src_adf = disk_with_module(r, rep.get("mod"), set_disks) if set_disks else adf_src
        if repro and p["kind"] in problems.GROUPED and e is not None and src_adf and os.path.isfile(src_adf):
            e = dict(e, options=dict(r.get("options") or e["options"]), params=r.get("params") or {})
            rom = r.get("rom") or {}
            romd = {"path": rom.get("source"), "crc32": rom.get("crc32"), "id": rom.get("id"),
                    "name": rom.get("name")} if rom.get("source") else None
            ext = r.get("ext_rom") or {}
            extd = {"path": ext.get("source"), "id": ext.get("id"), "name": ext.get("name")} if ext else None
            try:
                p["repro"] = problems.write_pack(
                    results_dir, p, rep, e, rom=romd, ext_rom=extd, adf_src=src_adf,
                    params=dict(env.get("params") or {}, **(r.get("params") or {})),
                    module_id=mids.get(rep.get("mod")), binary=binary, kickstarts=env.get("kickstarts_dir"),
                    amiberry_src=sp, pointers=p["pointers"], bake=adfmod.bake,
                    warp=module_warp(r, rep.get("mod")))
            except Exception as ex:
                p["repro"] = {"error": f"{type(ex).__name__}: {ex}"}
    an = {"fps": fps, "items": items, "parity": parity_info, "xfail": xf, "fixed": fixed, "problems": probs,
          "narrowing": narrowing, "amiberry_src": sp}
    if repro:
        import shutil
        old = os.path.join(results_dir, "repro")
        keep = {p["id"] for p in probs if p.get("repro") and not p["repro"].get("error")}
        if os.path.isdir(old):
            for d in os.listdir(old):
                if d not in keep:
                    shutil.rmtree(os.path.join(old, d))
    return report.write_all(results_dir, env, results, source=env.get("source", "run") or "run", analysis=an)
