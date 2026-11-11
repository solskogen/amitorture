"""Plans: the launches that run every coverage key of a matrix once.

An old-style run boots every entry with every disk. A plan keeps the entries
(same ids, options and media) but gives each the modules it is chosen for:
greedy set cover over the coverage keys (coverage.py), preferring entries
that cover many uncovered keys, so few machines boot. A twin that runs a
module makes its reference run it too, so engine parity still compares the
pair; companions (coverage.COMPANIONS) run together.

The result is a list of ordinary matrix entries with a narrower module mask
(params.modules) and only the disk groups those modules are on, so the
runner, xfail, parity and the reports work on it unchanged.
"""
import copy

from . import coverage, matrix

# Host seconds per module from measured runs (amitorture times, Pi 5, paced,
# 2026-11-10): (68000/68010 or cycle-exact, any other machine). Only the
# estimate uses them.
MODULE_COST = {
    "takeover": (0, 0), "rt": (0.3, 0.1), "cpu_basic": (80, 2), "flags": (49, 4), "exceptions": (5, 1),
    "smc": (2, 0.5), "interrupts": (9, 8), "cia": (6, 3), "blitter": (28, 2), "fpu": (3, 2), "timing": (11, 2),
    "devio": (60, 6), "display": (82, 98), "copper": (5, 2.5), "paula": (45, 27), "input": (12, 10),
    "rtg": (1, 1), "expsound": (2, 2), "network": (1, 1), "misc_hw": (20, 20), "mmu": (1, 1), "memory": (15, 1),
    "dispatch": (5, 0.5), "dmacontention": (12, 3), "misc_hw_os": (0.2, 0.2), "memory_os": (0.2, 0.2),
    "hostspeed": (20, 20),
}
# Host seconds to the first record per disk (emulator start, Kickstart,
# loader, probe; boot warp on, payload loaded at turbo speed), and outside
# the guest per launch (baking from the cache, media, stop; the config is
# checked once per machine). Measured 2026-11-10 (smoke, 20261110-224112Z).
BOOT_COST = {"core": (18, 5), "cpu": (14, 5), "exp": (15, 5)}
LAUNCH_COST = 3
# Host time of a module warped by pacing.py, relative to paced (same runs):
# a 68000 machine (blitter 0.35 .. exceptions 0.42), display on a fast core.
WARP_FACTOR = (0.38, 0.71)


def _slow(entry):
    o = entry["options"]
    return (str(o.get("cpu_model")) in ("68000", "68010") or str(o.get("cycle_exact")) == "true"
            or str(o.get("cpu_cycle_exact")) == "true")


def entry_cost(entry, modules, module_warp=None):
    """Estimated host seconds of one entry's launches running these modules.
    module_warp: the modules that run warped (pacing.py; default: the
    entry's module_warp if pacing per module is on, else none)."""
    i = 0 if _slow(entry) else 1
    warped = set(module_warp or ())
    groups = {coverage.MODULE_GROUP[m] for m in modules}
    return (sum(MODULE_COST.get(m, (5, 5))[i] * (WARP_FACTOR[i] if m in warped else 1) for m in modules)
            + sum(BOOT_COST[g][i] + LAUNCH_COST for g in groups))


def build(entries, allowed=None):
    """(plan entries, info) for a matrix's entries. allowed: a module mask
    (--param modules=) the plan keeps to. info: counts and cost estimates."""
    by_id = {e["id"]: e for e in entries}

    def runs(e):
        mods = coverage.modules_run(e)
        if allowed is not None:
            mods = [m for m in mods if allowed >> matrix.MODULE_IDS[m] & 1]
        return mods
    keys_of = {e["id"]: {coverage.key(m, e): m for m in runs(e)} for e in entries}
    uncovered = set()
    holders = {}
    for eid, ks in keys_of.items():
        for k in ks:
            uncovered.add(k)
            holders.setdefault(k, []).append(eid)
    chosen = {}            # entry id -> set of modules

    dirty = set()

    def add(eid, m):
        mods = chosen.setdefault(eid, set())
        if m in mods:
            return
        mods.add(m)
        dirty.add(eid)
        for c in coverage.COMPANIONS.get(m, ()):
            if c in runs(by_id[eid]):
                add(eid, c)
        ref = by_id[eid].get("twin_of")
        if ref in by_id and m in runs(by_id[ref]):
            add(ref, m)

    order = list(by_id)    # stable: matrix order breaks ties
    left = {eid: sum(1 for k in keys_of[eid] if k in uncovered) for eid in order}

    def cover(k):
        if k in uncovered:
            uncovered.discard(k)
            for h in holders[k]:
                left[h] -= 1

    while uncovered:
        best, best_n = None, 0
        for eid in order:
            n = left[eid]
            # an entry already booting costs no new launch: prefer it
            if n and eid in chosen:
                n += 1000
            if n > best_n:
                best, best_n = eid, n
        for k, m in keys_of[best].items():
            if k in uncovered:
                add(best, m)
        # what the entry runs now, and what the closure added elsewhere
        while dirty:
            eid = dirty.pop()
            mods = chosen[eid]
            for k, m in keys_of[eid].items():
                if m in mods:
                    cover(k)

    plan = []
    for e in entries:
        mods = chosen.get(e["id"])
        if not mods:
            continue
        p = copy.deepcopy(e)
        ordered = [m for m in runs(e) if m in mods]
        p.setdefault("params", {})["modules"] = matrix.module_mask(ordered)
        p["disk_groups"] = [g for g in coverage.GROUPS if any(coverage.MODULE_GROUP[m] == g for m in ordered)]
        p["plan_modules"] = ordered
        plan.append(p)
    info = {
        "keys": len(holders),
        "entries": len(entries),
        "plan_entries": len(plan),
        "keys_plan": len(covered(plan)),
        "module_runs_old": sum(len(v) for v in keys_of.values()),
        "module_runs_plan": sum(len(p["plan_modules"]) for p in plan),
        "launches_old": sum(len(e.get("disk_groups") or coverage.GROUPS) for e in entries),
        "launches_plan": sum(len(p["disk_groups"]) for p in plan),
        "cost_old_s": round(sum(entry_cost(e, runs(e)) for e in entries if runs(e))),
        "cost_plan_s": round(sum(entry_cost(p, p["plan_modules"]) for p in plan)),
        # with pacing per module (--module-warp auto)
        "cost_plan_module_warp_s": round(sum(entry_cost(p, p["plan_modules"], p.get("module_warp")) for p in plan)),
    }
    return plan, info


def covered(plan):
    """The coverage keys a plan runs."""
    return {coverage.key(m, p) for p in plan for m in p["plan_modules"]}
