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
Softfloat twins are compared the same way but reported as
`fpu_mode_difference` (informational): native FPU on this host is 64-bit,
softfloat is 80-bit extended, so differences there are expected.
"""
from __future__ import annotations

from . import cases as casemod
from . import matrix

ENFORCED = {"jit", "threaded"}
RAN = {"pass", "fail", "incomplete"}


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
    ref_fp = ref_fp or casemod.extract(ref_res)
    twin_fp = twin_fp or casemod.extract(twin_res)
    opt_diff = matrix.option_diff(ref_res.get("options") or {}, twin_res.get("options") or {})
    differing = [{"option": k, "ref": a, "twin": b} for k, a, b in opt_diff]
    items = []
    rc = {k: v for k, v in ref_fp["cases"].items() if not v.get("engine_dependent")}
    tc = {k: v for k, v in twin_fp["cases"].items() if not v.get("engine_dependent")}
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
        if a["fp"] != b["fp"]:
            paths = casemod.diff_paths(a["rec"], b["rec"])
            what = ", ".join(p for p, _, _ in paths[:6]) + (" ..." if len(paths) > 6 else "")
            if a.get("excluded") or b.get("excluded"):
                base["not_compared_fields"] = {"ref": a.get("excluded"), "twin": b.get("excluded"),
                                               "note": "allowed-result markers, excluded from the fingerprint"}
            items.append(dict(base, subkind="value", mod=a["mod"], case=a["case"], ref=a["rec"], twin=b["rec"],
                              paths=[{"path": p, "ref": x, "twin": y} for p, x, y in paths[:40]],
                              why=f"{key}: {twin_res['id']} differs from {ref_res['id']} in {what} "
                                  f"(the entries differ only in {', '.join(d['option'] for d in differing) or '?'})"))
    info.update(status="compared", compared=len(set(rc) & set(tc)), mismatches=len(items))
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
