"""Engine parity: a twin (JIT, threaded CPU) must give the same results as
its reference entry, case by case.

For every (reference, twin) pair that both ran:
  * the case lists must be equal: a case missing on either side is an
    `engine_mismatch` (subkind missing_case);
  * every case record must be equal after removing timing fields
    (cases.strip_timing): a difference is an `engine_mismatch` with both
    values, the differing paths and the options the twins differ in;
  * perf records and timing fields are never compared here, nor are the
    pseudo cases marked engine_dependent (host.speed_*, cases.py): how fast
    the host runs an instruction is what tells the JIT and the interpreter
    apart, so their verdicts may differ by design.
Bounded Paula/H-counter sampling that was explicitly inconclusive is listed
as not comparable, not a match and not parity coverage. RTC wall-clock dates
are compared through their architectural assertions and relationships; raw
records remain intact. parity_observations documents the exact scoped rules.
Softfloat twins are compared the same way but reported as
`fpu_mode_difference` (informational): native FPU on this host is 64-bit,
softfloat is 80-bit extended, so differences there are expected.
"""
from __future__ import annotations

from . import cases as casemod
from . import amiberry_cfg, matrix, parity_observations

ENFORCED = {"jit", "threaded"}
RAN = {"pass", "fail", "incomplete"}


def _pair_problem(ref_res, twin_res, kind):
    for r in (ref_res, twin_res):
        if any((s.get("config") or {}).get("dump_ok") is False for s in (r.get("disks") or [r])):
            return "configuration verification failed"
    pacing = []
    for r in (ref_res, twin_res):
        pacing.append({s.get("disk_name"): (s.get("warp") or {}).get(
            "effective", str((s.get("resolved_options") or s.get("options") or {}).get(
                "warp", "false")).lower() in ("true", "yes", "1"))
            for s in (r.get("disks") or [r])})
    # a planned reference may run disks its twin does not: compare the disks both ran
    both = set(pacing[0]) & set(pacing[1])
    if {d: pacing[0][d] for d in both} != {d: pacing[1][d] for d in both}:
        return "entries have different per-disk warp modes"
    opts = [amiberry_cfg.COMMON | (r.get("options") or {}) | (r.get("resolved_options") or {})
            for r in (ref_res, twin_res)]
    ref, twin = opts
    allowed = {"jit": matrix.JIT_OPTION_KEYS, "threaded": {"cpu_threaded"},
               "softfloat": {"fpu_softfloat"}}.get(kind)
    if allowed is None:
        return f"unknown twin kind {kind!r}"
    if kind == "jit" and "cpu_threaded" in (twin_res.get("twin_options") or []):
        allowed = allowed | {"cpu_threaded"}
    if kind == "jit":
        try:
            valid = int(ref.get("cachesize", 0)) == 0 and int(twin.get("cachesize", 0)) > 0
        except (TypeError, ValueError):
            valid = False
        if not valid:
            return "JIT parity requires an interpreter reference (cachesize=0) and a JIT twin (cachesize>0)"
    else:
        axis = "cpu_threaded" if kind == "threaded" else "fpu_softfloat"
        if str(ref.get(axis, "false")).lower() not in ("false", "no", "0") or str(
                twin.get(axis, "false")).lower() not in ("true", "yes", "1"):
            return f"{kind} parity requires {axis}=false on the reference and true on the twin"
    other = [k for k, _, _ in matrix.option_diff(ref, twin) if k not in allowed]
    if other:
        return "entries differ outside the intended engine options: " + ", ".join(other)
    return None


def compare(ref_res, twin_res, ref_fp=None, twin_fp=None):
    """Returns (items, info). items: engine_mismatch / fpu_mode_difference
    dicts; info: {"compared": n, "status": "compared|not compared", "why"}."""
    kind = twin_res.get("twin_kind") or "jit"
    out_kind = "engine_mismatch" if kind in ENFORCED else "fpu_mode_difference"
    info = {"ref": ref_res["id"], "twin": twin_res["id"], "twin_kind": kind, "kind": out_kind}
    if ref_res.get("status") not in RAN or twin_res.get("status") not in RAN:
        info.update(status="not compared", compared=0,
                    why=f"reference {ref_res.get('status')}, twin {twin_res.get('status')}: both must have run")
        return [], info
    adj = [d for r in (ref_res, twin_res) for d in (r.get("adjusted") or [])]
    if adj:
        info.update(status="not compared", compared=0,
                    why="config adjusted by emulator (" + "; ".join(
                        f"{d['option']} {d['wrote']}->{d['resolved']}" for d in adj[:4])
                        + "): not a run of the designed engine pair")
        return [], info
    why = _pair_problem(ref_res, twin_res, kind)
    if why:
        info.update(status="not compared", compared=0, why=why)
        return [], info
    ref_fp = ref_fp or casemod.extract(ref_res)
    twin_fp = twin_fp or casemod.extract(twin_res)
    opt_diff = matrix.option_diff(ref_res.get("options") or {}, twin_res.get("options") or {})
    differing = [{"option": k, "ref": a, "twin": b} for k, a, b in opt_diff]
    items = []
    rc = {k: v for k, v in ref_fp["cases"].items() if not v.get("engine_dependent")}
    tc = {k: v for k, v in twin_fp["cases"].items() if not v.get("engine_dependent")}
    # planned runs (plan.py): compare the modules both were given
    planned = [set(r["plan_modules"]) for r in (ref_res, twin_res) if r.get("plan_modules")]
    if planned:
        common = set.intersection(*planned)
        rc = {k: v for k, v in rc.items() if v.get("mod") in common or v.get("mod") == "host"}
        tc = {k: v for k, v in tc.items() if v.get("mod") in common or v.get("mod") == "host"}
    not_comparable, observations = [], []
    compared = 0
    for key in sorted(set(rc) | set(tc)):
        a, b = rc.get(key), tc.get(key)
        base = {"kind": out_kind, "entry": twin_res["id"], "ref_entry": ref_res["id"], "twin_kind": kind,
                "case_key": key, "differing_options": differing}
        if a is None or b is None:
            have, lack = (ref_res["id"], twin_res["id"]) if b is None else (twin_res["id"], ref_res["id"])
            c = a or b
            items.append(dict(base, subkind="missing_case", mod=c["mod"], case=c["case"],
                              ref=None if a is None else a["rec"], twin=None if b is None else b["rec"],
                              why=f"case {key} reported on {have} but missing on {lack}"))
            continue
        compared += 1
        ar, ap = parity_observations.normalize(a, rc)
        br, bp = parity_observations.normalize(b, tc)
        if ap or bp:
            observations.append({"case_key": key, "ref_fields": ap, "twin_fields": bp,
                                 "why": "compare RTC assertions and clock relationships, not launch wall time"})
        if casemod.canonical(ar) != casemod.canonical(br):
            paths = casemod.diff_paths(ar, br)
            what = ", ".join(p for p, _, _ in paths[:6]) + (" ..." if len(paths) > 6 else "")
            if a.get("excluded") or b.get("excluded"):
                base["not_compared_fields"] = {"ref": a.get("excluded"), "twin": b.get("excluded"),
                                               "note": "allowed-result markers, excluded from the fingerprint"}
            if ap or bp:
                base.update(comparison_ref=ar, comparison_twin=br, observation_fields={"ref": ap, "twin": bp})
            items.append(dict(base, subkind="value", mod=a["mod"], case=a["case"], ref=a["rec"], twin=b["rec"],
                              paths=[{"path": p, "ref": x, "twin": y} for p, x, y in paths[:40]],
                              why=f"{key}: {twin_res['id']} differs from {ref_res['id']} in {what} "
                                  f"(the entries differ only in {', '.join(d['option'] for d in differing) or '?'})"))
    info.update(status="compared", compared=compared, mismatches=len(items),
                not_comparable=not_comparable, observations=observations)
    return items, info


def check_all(results, fps=None):
    """Parity over every (reference, twin) pair in a run."""
    by_id = {r["id"]: r for r in results}
    fps = fps or {}
    items, infos = [], []
    for r in results:
        ref = by_id.get(r.get("twin_of"))
        if ref is None:
            if r.get("twin_of"):
                infos.append({"ref": r["twin_of"], "twin": r["id"], "twin_kind": r.get("twin_kind"),
                              "status": "not compared", "why": "reference entry not in this run", "compared": 0})
            continue
        it, info = compare(ref, r, fps.get(ref["id"]), fps.get(r["id"]))
        items.extend(it)
        infos.append(info)
    return items, infos
