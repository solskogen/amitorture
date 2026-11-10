"""Coverage keys: what a matrix checks, independent of how the runs are laid out.

An old-style run boots every matrix entry with every disk and runs every
module (minus the entry's module mask). Most modules depend on only a few
configuration factors: cpu_basic on the CPU, the core and the engine, display
on the chipset, the video standard and the core, and so on. A module's
results on two entries that agree on its factors are the same check twice.

Each module declares its axes (AXES): the option groups and entry fields its
results may depend on. A coverage key is (module, the entry projected on
those axes). The keys of a matrix are those of every (entry, module) an
old-style run would run (``required``). A plan (plan.py) may lay the runs
out differently, but must run every key at least once; a unit test checks it
for every matrix. Whether the axes are wide enough is checked against
old-style results (``validate``): two entries with the same key must give the
same fingerprints for that module.

Axes err wide: a factor that may matter is an axis. Factors named by xfail
rules (``when`` options, ``configs`` globs) and by findings are axes too.
"""
import fnmatch
import functools

from . import matrix

# Option groups, by option name. Every option a matrix entry sets must belong
# to one (a unit test checks it), so a new option forces a decision.
OPTION_GROUPS = {
    "cpu": ("cpu_model", "mmu_model", "cpu060_revision", "cpu_24bit_addressing", "cpu_no_unimplemented"),
    "fpu": ("fpu_model", "fpu_softfloat", "fpu_strict", "fpu_no_unimplemented", "compfpu"),
    # the CPU variant beyond cpu/mmu_model: 68040 / 68LC040 / 68EC040 differ in
    # fpu_model (project: only on a 68040/68060, where the FPU is on the chip)
    "cpu_variant": ("fpu_model",),
    "core": ("cpu_compatible", "cycle_exact", "cpu_cycle_exact", "cpu_memory_cycle_exact", "blitter_cycle_exact",
             "cpu_speed", "cpu_multiplier"),
    "engine_class": ("cachesize", "cpu_threaded"),
    "engine": ("cachesize", "cpu_threaded", "comp_*", "compfpu"),
    "chipset": ("chipset", "chipset_compatible"),
    "video": ("ntsc",),
    "memory": ("*mem_size", "cpuboardmem*_size"),
    "boards": ("*_rom_file", "gfxcard_*", "a2065", "ne2000_*", "zz9000_net", "rtc", "cpuboard_type", "ppc_*"),
    "input": ("joyport*",),
    "model": ("flash_file",),
}
# Entry fields (not options) that are factors, by group.
FIELD_GROUPS = {
    "model": ("model", "rom", "ext_rom"),
    "boot": ("boot", "media", "media_files"),
    "flags": ("flags",),
    # a disk controller (media.controller: A2091, A4091, ...) is an expansion board
    "boards": ("board_roms", "media"),
}

# The groups each module's results may depend on (docs/cases/<module>.md, the
# module sources, the xfail rules). engine_class: JIT on/off and the CPU
# thread; engine: also every JIT sub-option. The CPU modules have cpu_variant: the
# CPU variant (68040 / 68LC040 / 68EC040) is cpu_model + fpu_model + mmu_model,
# and their skip reasons name it (axes check on standard, 2026-11-10).
AXES = {
    "takeover": ("model", "cpu", "core", "engine_class"),
    "rt": ("cpu", "core", "engine_class"),
    "cpu_basic": ("cpu", "cpu_variant", "core", "engine"),
    "flags": ("cpu", "cpu_variant", "core", "engine"),
    "exceptions": ("cpu", "fpu", "core", "engine", "flags"),
    "smc": ("cpu", "core", "engine", "memory"),
    "interrupts": ("video", "chipset", "model", "cpu", "core", "engine_class", "boards", "boot"),
    "cia": ("video", "chipset", "model", "cpu", "core", "engine_class", "boards", "boot"),
    "blitter": ("chipset", "cpu", "core", "engine_class"),
    "fpu": ("cpu", "fpu", "core", "engine"),
    "timing": ("video", "chipset", "cpu", "fpu", "core", "engine_class"),
    "devio": ("model", "boot", "memory", "boards", "cpu", "flags"),
    "display": ("chipset", "video", "model", "cpu", "core", "engine_class"),
    "copper": ("chipset", "video", "cpu", "core", "engine_class"),
    "paula": ("chipset", "video", "model", "boot", "cpu", "core", "engine_class", "boards"),
    "input": ("input", "model", "video", "cpu", "core", "engine_class"),
    "rtg": ("boards", "model", "cpu", "memory"),
    "expsound": ("boards", "model", "cpu", "core", "video", "engine_class", "flags"),
    "network": ("boards", "model", "flags"),
    "misc_hw": ("model", "boards", "chipset", "memory", "flags", "boot"),
    "mmu": ("cpu", "cpu_variant", "core", "model", "memory"),
    "memory": ("memory", "model", "cpu", "engine", "boards"),
    "dispatch": ("cpu", "cpu_variant", "core", "engine"),
    "dmacontention": ("chipset", "video", "cpu", "core", "boot", "engine_class"),
    "misc_hw_os": ("model", "boards", "memory"),
    "memory_os": ("memory", "model", "boards"),
    "hostspeed": ("cpu", "fpu", "core", "engine", "chipset", "video"),
}

# The disk group each module is built into (disk/Makefile DISK_SET_*; a unit
# test keeps this in step).
MODULE_GROUP = {}
for _g, _mods in (("core", "takeover rt flags exceptions smc interrupts cia blitter timing display copper paula "
                           "input dmacontention"),
                  ("cpu", "cpu_basic mmu dispatch"),
                  ("exp", "fpu rtg expsound network misc_hw misc_hw_os devio memory memory_os hostspeed")):
    for _m in _mods.split():
        MODULE_GROUP[_m] = _g
GROUPS = ("core", "cpu", "exp")

# Modules that must run in the same launch as another one: memory tests the
# blocks memory_os allocated.
COMPANIONS = {"memory": ("memory_os",)}


@functools.lru_cache(maxsize=None)
def option_group(key):
    """The groups an option belongs to (an option may be in engine and fpu)."""
    return tuple(g for g, pats in OPTION_GROUPS.items() if any(fnmatch.fnmatch(key, p) for p in pats))


def _mask(entry):
    m = (entry.get("params") or {}).get("modules")
    if m is None or str(m).lower() == "all":
        return matrix.ALL_MODULES
    return int(str(m), 0)


def modules_run(entry):
    """The modules an old-style run runs on this entry: those of its disk
    groups, minus its module mask."""
    groups = entry.get("disk_groups") or GROUPS
    mask = _mask(entry)
    return [m for m, i in sorted(matrix.MODULE_IDS.items(), key=lambda kv: kv[1])
            if MODULE_GROUP[m] in groups and mask >> i & 1]


def _freeze(v):
    if isinstance(v, dict):
        return tuple(sorted((k, _freeze(x)) for k, x in v.items()))
    if isinstance(v, (list, tuple)):
        return tuple(_freeze(x) for x in v)
    return v


def _jit_on(v):
    return str(v).strip() not in ("", "0")


def project(entry, groups):
    """The entry's values on these groups: (name, value) pairs, sorted.
    engine_class keeps only whether the JIT is on (not its cache size) and
    the CPU thread."""
    out = []
    on_chip_fpu = str(entry["options"].get("cpu_model")) in ("68040", "68060")
    for k, v in entry["options"].items():
        gs = option_group(k)
        if "cpu_variant" in groups and "cpu_variant" in gs and "fpu" not in groups:
            if on_chip_fpu:            # 68040 / 68LC040 / 68EC040; an external 68881/2 is no CPU variant
                out.append((k, str(v)))
        elif "engine" in groups and "engine" in gs:
            out.append((k, str(v)))
        elif "engine_class" in groups and "engine_class" in gs:
            out.append((k, str(_jit_on(v)) if k == "cachesize" else str(v)))
        elif any(g in groups for g in gs if g not in ("engine", "engine_class")):
            out.append((k, str(v)))
    for g in groups:
        for f in FIELD_GROUPS.get(g, ()):
            if entry.get(f) not in (None, "", [], {}):
                out.append((f, _freeze(entry[f])))
    return tuple(sorted(set(out), key=lambda kv: kv[0]))


def key(module, entry, axes=None):
    return (module, project(entry, (axes or AXES)[module]))


def required(entries):
    """{coverage key: [entry ids that an old-style run covers it on]}."""
    out = {}
    for e in entries:
        for m in modules_run(e):
            out.setdefault(key(m, e), []).append(e["id"])
    return out


def unclassified_options(entries):
    """Options set by some entry that belong to no group."""
    return sorted({k for e in entries for k in e["options"] if not option_group(k)})


# ---------------------------------------------------------------- validation
def fingerprints(results_dir):
    """{entry id: {module: {case: (status, fingerprint)}}} from the raw
    records of a results dir (the last boot of every disk)."""
    import json
    import os

    from . import cases as casemod
    raw = os.path.join(results_dir, "raw")
    out = {}
    for eid in sorted(os.listdir(raw)) if os.path.isdir(raw) else []:
        mods = {}
        edir = os.path.join(raw, eid)
        for d in sorted(os.listdir(edir)):
            rp = os.path.join(edir, d, "records.jsonl")
            if not os.path.isfile(rp):
                continue
            with open(rp, encoding="utf-8") as fh:
                recs = [json.loads(line) for line in fh if line.strip()]
            recs = [r for r in recs if not r.get("host")]
            last = recs[-1]["boot"] if recs else 0
            ex = casemod.extract_records([r["rec"] for r in recs if r["boot"] == last])
            for c in ex["cases"].values():
                if c.get("pseudo"):
                    continue
                mods.setdefault(c["mod"], {})[c["case"]] = (c["st"], c["fp"])
        if mods:
            out[eid] = mods
    return out


def validate(results_dirs, entries=None, axes=None):
    """Checks the axes against old-style results: two entries with the same
    coverage key for a module must give that module the same results.
    Returns {module: {"keys": n compared, "differ": [{"key_entries": [a, b],
    "cases": [...], "verdict_cases": [...], "detail_only": bool, "options": [...]}]}}.
    detail_only: the same verdicts, other details (addresses, a skip reason
    naming the CPU variant): look at them, they are not counted as a missing
    axis by the axes command. Entries are
    looked up by id in every matrix (or ``entries``); an id found in more than
    one results dir uses the first. ``axes``: {module: groups} to check
    instead of AXES (narrower candidates: which entries would merge)."""
    from . import matrix as matrixmod
    by_id = {e["id"]: e for e in (entries or matrixmod.all_entries().values())}
    fps = {}
    for rd in ([results_dirs] if isinstance(results_dirs, dict) else results_dirs):
        if isinstance(rd, dict):        # fingerprints already loaded
            for eid, mods in rd.items():
                fps.setdefault(eid, mods)
            continue
        for eid, mods in fingerprints(rd).items():
            fps.setdefault(eid, mods)
    groups = {}
    for eid, mods in fps.items():
        e = by_id.get(eid)
        if e is None:
            continue
        for m in mods:
            if m in AXES:
                groups.setdefault(key(m, e, axes), []).append(eid)
    out = {}
    for (m, proj), eids in sorted(groups.items(), key=lambda kv: kv[0][0]):
        o = out.setdefault(m, {"keys": 0, "differ": []})
        if len(eids) < 2:
            continue
        o["keys"] += 1
        ref = eids[0]
        for other in eids[1:]:
            a, b = fps[ref][m], fps[other][m]
            bad = sorted(c for c in set(a) | set(b) if a.get(c) != b.get(c))
            if not bad:
                continue
            # a verdict that differs (or a case only one ran) vs the same
            # verdict with other details (addresses, reasons naming the CPU)
            verdict = [c for c in bad if a.get(c, (None,))[0] != b.get(c, (None,))[0]]
            diff = [k for k, _, _ in matrixmod.option_diff(by_id[ref]["options"], by_id[other]["options"])]
            o["differ"].append({"entries": [ref, other], "cases": bad[:20], "n_cases": len(bad),
                                "verdict_cases": verdict[:20], "n_verdict": len(verdict),
                                "status": {c: [a.get(c, (None,))[0], b.get(c, (None,))[0]] for c in bad[:20]},
                                "detail_only": not verdict, "options": diff})
    return out


def format_validation(v):
    lines = []
    for m, o in sorted(v.items()):
        if not o["keys"]:
            continue
        nv = sum(1 for d in o["differ"] if not d["detail_only"])
        lines.append(f"{m}: {o['keys']} keys with 2+ entries, {nv} pair(s) with different verdicts, "
                     f"{len(o['differ']) - nv} with the same verdicts but other details")
        for d in sorted(o["differ"], key=lambda d: d["detail_only"])[:10]:
            sts = ", ".join(f"{c} {x}/{y}" for c, (x, y) in list(d["status"].items())[:6])
            lines.append(f"  {'detail ' if d['detail_only'] else 'VERDICT'} {d['entries'][0]} vs {d['entries'][1]} "
                         f"(options differ: {', '.join(d['options']) or 'none'}): {d['n_cases']} case(s): {sts}")
    return "\n".join(lines)


def axis_drops(results_dirs, modules=None, entries=None):
    """For every module and each of its axes: what dropping that axis would
    merge, and whether the merged entries agree. {module: {axis: {"keys": n
    with 2+ entries, "differ": n pairs, "pairs": [...]}}}. Evidence for a
    narrower axis set, from old-style results; never proof."""
    fps = {}
    for rd in results_dirs:
        for eid, mods in (rd.items() if isinstance(rd, dict) else fingerprints(rd).items()):
            fps.setdefault(eid, mods)
    out = {}
    for m in modules or sorted(AXES):
        for ax in AXES[m]:
            cand = dict(AXES)
            cand[m] = tuple(a for a in AXES[m] if a != ax)
            v = validate(fps, entries=entries, axes=cand).get(m, {"keys": 0, "differ": []})
            verdict = [d for d in v["differ"] if not d["detail_only"]]
            out.setdefault(m, {})[ax] = {"keys": v["keys"], "differ": len(verdict),
                                         "detail": len(v["differ"]) - len(verdict), "pairs": verdict[:5]}
    return out


def outcome_diff(dir_a, dir_b):
    """Case verdicts of two runs, per (entry, module) both ran: a plan run
    against an old-style one, warped against paced. {"compared": {"entries",
    "modules", "cases"}, "verdicts": [{entry, mod, case, a, b}], "missing":
    [{entry, mod, case, only}]}: missing are cases one run has and the other
    lacks within a module both ran."""
    fa, fb = fingerprints(dir_a), fingerprints(dir_b)
    out = {"compared": {"entries": 0, "modules": 0, "cases": 0}, "verdicts": [], "missing": []}
    for eid in sorted(set(fa) & set(fb)):
        out["compared"]["entries"] += 1
        for m in sorted(set(fa[eid]) & set(fb[eid])):
            out["compared"]["modules"] += 1
            a, b = fa[eid][m], fb[eid][m]
            for c in sorted(set(a) | set(b)):
                if c in a and c in b:
                    out["compared"]["cases"] += 1
                    if a[c][0] != b[c][0]:
                        out["verdicts"].append({"entry": eid, "mod": m, "case": c, "a": a[c][0], "b": b[c][0]})
                else:
                    out["missing"].append({"entry": eid, "mod": m, "case": c, "only": "a" if c in a else "b"})
    return out
