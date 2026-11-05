"""Stability baselines and regression checks.

baseline: runs every entry N times (--repeat, default 3) with one binary and
stores, per entry, every case fingerprint (cases.py), every timing value and
every perf number, plus the binary sha256/version, the ROM ids and the disk
build id, in WORKDIR/baselines/<name>/baseline.json. A case whose
fingerprint differs between the identical runs (or that is missing from
some) is `unstable` and is not used for comparison. Perf/timing values get a
band: mean +- max(k*sigma, pct*|mean|, abs_min).

check: reruns the baseline's entries (as stored, plus --set overrides) on
one or more binaries and diffs fingerprint by fingerprint:
  fail_detail_changed  failed before and after with the same expect/got/why/in;
                    only ctx changed (e.g. addresses after a disk rebuild): informational
  regression        stable baseline fingerprint changed (pass->fail,
                    or any changed value; `transition` says which)
  fixed             baseline fail -> now pass (xfail ref if registered)
  new / missing     case only in the check / only in the baseline
  new_failure       unexpected failing case only in the check
  became_unstable   stable in the baseline, differs between check runs
  unstable_excluded unstable in the baseline: reported, not compared
  perf_out_of_band / timing_out_of_band (direction faster/slower)
  entry_status      the entry as a whole ended differently (crash, skip ...)
Every finding carries the entry's config changes vs the baseline, so a
deliberate --set shows up pinned to the cases it changed. With several
binaries, `first_changes` names the first binary where each fingerprint
changed (bisect without building).

`build()` and `diff()` are pure (unit-tested).
"""
from __future__ import annotations

import datetime
import json
import math
import os

from . import cases as casemod
from . import matrix
from . import xfail as xfailmod

BAND_K = 3.0
BAND_PCT = 0.05
BAND_ABS = 1.0
FORMAT = 1
REGRESSION_KINDS = {"regression", "missing", "became_unstable", "perf_out_of_band", "timing_out_of_band",
                    "entry_status", "missing_entry", "new_failure", "perf_missing", "timing_missing"}


def band(values, k=BAND_K, pct=BAND_PCT, abs_min=BAND_ABS):
    vals = [float(v) for v in values if isinstance(v, (int, float)) and not isinstance(v, bool)]
    if not vals:
        return None
    n = len(vals)
    mean = sum(vals) / n
    sd = math.sqrt(sum((v - mean) ** 2 for v in vals) / (n - 1)) if n > 1 else 0.0
    half = max(k * sd, pct * abs(mean), abs_min)
    return {"n": n, "mean": mean, "sd": sd, "lo": mean - half, "hi": mean + half, "values": vals,
            "k": k, "pct": pct, "abs_min": abs_min}


def stability(runs):
    """{case_key: {...}} over N fingerprint extracts of one entry."""
    keys = set()
    for r in runs:
        keys |= set(r["cases"])
    out = {}
    for key in sorted(keys):
        present = [r["cases"][key] for r in runs if key in r["cases"]]
        fps = sorted({c["fp"] for c in present})
        stable = len(present) == len(runs) and len(fps) == 1
        c0 = present[0]
        d = {"mod": c0["mod"], "case": c0["case"], "stable": stable, "present": f"{len(present)}/{len(runs)}",
             "st": [c["st"] for c in present], "fp": fps[0] if stable else None, "rec": c0["rec"]}
        if not stable:
            d["variants"] = fps
        out[key] = d
    return out


def timing_bands(runs, **bk):
    """{case_key: {path: band}} for numeric timing leaves present in the runs."""
    acc = {}
    for r in runs:
        for key, c in r["cases"].items():
            for path, v in (c.get("timing") or {}).items():
                for leaf, x in casemod.numeric_leaves(v, path).items():
                    acc.setdefault(key, {}).setdefault(leaf, []).append(x)
    return {k: {p: band(v, **bk) for p, v in d.items()} for k, d in acc.items()}


def perf_bands(runs, **bk):
    acc, ops = {}, {}
    for r in runs:
        for key, p in r["perf"].items():
            if p.get("value") is not None:
                acc.setdefault(key, []).append(p["value"])
            ops.setdefault(key, set()).add(p.get("ops"))
    out = {}
    for k, v in acc.items():
        o = sorted(x for x in ops[k] if isinstance(x, (int, float)) and x)
        # absolute floor = the measurement's resolution: 2 ticks over the whole run
        kw = dict(bk)
        kw.setdefault("abs_min", 2.0 / o[0] if o else BAND_ABS)
        out[k] = dict(band(v, **kw) or {}, ops=o, unit="ticks/op")
    return out


COMPLETE = ("pass", "fail")


def complete_runs(statuses, runs):
    """The runs that ended with a done record and complete records. A run
    that hung, crashed or timed out says nothing about case stability (it is
    an entry-level event, reported as such)."""
    return [r for st, r in zip(statuses, runs) if st in COMPLETE]


def build_entry(entry, statuses, runs, meta=None, **bk):
    good = complete_runs(statuses, runs)
    return {"entry": entry, "statuses": statuses, "cases": stability(good) if good else {},
            "timing": timing_bands(good, **bk), "perf": perf_bands(good, **bk), "meta": meta or {},
            "repeat": len(runs), "complete_runs": len(good),
            "excluded_runs": [i + 1 for i, st in enumerate(statuses) if st not in COMPLETE]}


def build(per_entry, meta, **bk):
    """per_entry: {eid: {"entry", "statuses", "runs", "meta"}} -> baseline doc."""
    ents = {eid: build_entry(d["entry"], d["statuses"], d["runs"], d.get("meta"), **bk) for eid, d in per_entry.items()}
    unstable = sum(1 for e in ents.values() for c in e["cases"].values() if not c["stable"])
    return {"format": FORMAT, "meta": dict(meta, unstable_cases=unstable), "entries": ents}


def _ran(status):
    return status in ("pass", "fail", "incomplete")


def config_changes(base_entry, now_entry):
    if base_entry is None or now_entry is None:
        return []
    return [{"option": k, "baseline": a, "now": b}
            for k, a, b in matrix.option_diff(base_entry.get("options") or {}, now_entry.get("options") or {})]


def _out_of_band(b, value):
    if b is None or value is None or "lo" not in b:
        return None
    if value < b["lo"]:
        return "lower"
    if value > b["hi"]:
        return "higher"
    return None


def diff(baseline, check, registry=None):
    """check: {eid: {"entry", "statuses", "runs", "meta"}} (same shape as
    build's input). Returns {"findings": [...], "counts": {...}, "entries": {...}}."""
    registry = registry or {}
    findings = []

    def add(kind, eid, **kw):
        findings.append({"kind": kind, "entry": eid, **kw})

    per_entry = {}
    for eid, b in baseline["entries"].items():
        c = check.get(eid)
        if c is None:
            add("missing_entry", eid, why="entry in the baseline was not run by the check")
            continue
        changes = config_changes(b["entry"], c["entry"])
        meta_changes = []
        for k in ("rom", "ext_rom", "disk_build", "binary_sha256", "warp"):
            if (b.get("meta") or {}).get(k) != (c.get("meta") or {}).get(k):
                meta_changes.append({"what": k, "baseline": (b.get("meta") or {}).get(k),
                                     "now": (c.get("meta") or {}).get(k)})
        per_entry[eid] = {"config_changes": changes, "meta_changes": meta_changes,
                          "baseline_status": b["statuses"], "status": c["statuses"]}
        ctx = {"config_changes": changes}
        abnormal = {"crash", "hang", "timeout", "error", "incomplete", "skip"}
        if set(b["statuses"]) & abnormal != set(c["statuses"]) & abnormal:
            add("entry_status", eid, baseline=b["statuses"], now=c["statuses"],
                why=f"entry ended {c['statuses']} (baseline {b['statuses']})", **ctx)
        good = complete_runs(c["statuses"], c["runs"])
        if not good:
            continue
        now = stability(good)
        opts = (c["entry"] or {}).get("options") or {}
        for key in sorted(set(b["cases"]) | set(now)):
            bc, nc = b["cases"].get(key), now.get(key)
            if bc is None:
                xf = xfailmod.match(registry, key, eid, opts) if "fail" in nc["st"] else None
                kind = "new_failure" if "fail" in nc["st"] and not xf else "new"
                item = dict(case_key=key, mod=nc["mod"], case=nc["case"],
                            now={"st": nc["st"], "rec": nc["rec"]}, **ctx)
                if xf:
                    item["xfail"] = xf
                add(kind, eid, why=f"case {key} is new (not in the baseline)"
                    + (" and unexpectedly fails" if kind == "new_failure" else ""), **item)
                continue
            if nc is None:
                add("missing", eid, case_key=key, mod=bc["mod"], case=bc["case"],
                    baseline={"st": bc["st"], "rec": bc["rec"]}, why=f"case {key} no longer reported", **ctx)
                continue
            if not bc["stable"]:
                add("unstable_excluded", eid, case_key=key, mod=bc["mod"], case=bc["case"],
                    baseline={"present": bc["present"], "variants": len(bc.get("variants", []))},
                    why=f"case {key} was unstable in the baseline ({bc['present']} runs, "
                        f"{len(bc.get('variants', []))} variants): not compared", **ctx)
                continue
            if not nc["stable"]:
                add("became_unstable", eid, case_key=key, mod=bc["mod"], case=bc["case"],
                    baseline={"st": bc["st"][0], "rec": bc["rec"]}, now={"present": nc["present"], "st": nc["st"],
                                                                         "variants": nc.get("variants")},
                    why=f"case {key} was stable in the baseline but differs between the check runs", **ctx)
                continue
            if bc["fp"] == nc["fp"]:
                continue
            # A baseline recorded under older exclusion rules may still carry
            # fields that are no longer fingerprinted: apply today's rules to
            # both sides before calling it a change.
            if not [p for p, _, _ in casemod.diff_paths(bc["rec"], nc["rec"])
                    if not casemod.is_excluded_path(p)]:
                continue
            bs, ns = bc["st"][0], nc["st"][0]
            xf = xfailmod.match(registry, key, eid, opts)
            paths = casemod.diff_paths(bc["rec"], nc["rec"])
            item = dict(case_key=key, mod=bc["mod"], case=bc["case"], transition=f"{bs}->{ns}",
                        baseline={"st": bs, "rec": bc["rec"]}, now={"st": ns, "rec": nc["rec"]},
                        paths=[{"path": p, "baseline": x, "now": y} for p, x, y in paths[:40]], **ctx)
            if xf:
                item["xfail"] = xf
            if bs == "fail" and ns == "pass":
                add("fixed", eid, why=f"case {key} passes now (failed in the baseline)"
                    + (f"; xfail {xf['ref']}" if xf else ""), **item)
            elif bs == ns == "fail" and xf:
                add("fail_detail_changed", eid, why=f"case {key} is a known bug ({xf['ref']}) and still fails; "
                    "its measured values changed (" + ", ".join(p for p, _, _ in paths[:5]) + ")", **item)
            elif bs == ns == "fail" and paths and all(p == "ctx" or p.startswith("ctx.") or p.startswith("ctx[")
                                                       for p, _, _ in paths):
                # Still failing the same way (same expect/got/why/in); only the
                # diagnostic context moved, e.g. absolute addresses after a
                # rebuild of the disk changed the memory layout. Reported, not
                # counted as a regression.
                add("fail_detail_changed", eid, why=f"case {key} still fails the same way; only ctx changed ("
                    + ", ".join(p for p, _, _ in paths[:5]) + ")", **item)
            else:
                what = ", ".join(p for p, _, _ in paths[:5])
                add("regression", eid, why=f"case {key} changed ({bs}->{ns}) in {what}"
                    + (f"; config differs from baseline: "
                       + ", ".join(f"{d['option']} {d['baseline']}->{d['now']}" for d in changes) if changes else ""),
                    **item)
        # perf and timing bands
        nperf = {}
        for r in good:
            for k, p in r["perf"].items():
                if p.get("value") is not None:
                    nperf.setdefault(k, []).append(p["value"])
        for k, bb in b["perf"].items():
            if k not in nperf:
                add("perf_missing", eid, perf=k, why=f"perf {k} no longer reported", **ctx)
                continue
            v = sum(nperf[k]) / len(nperf[k])
            d = _out_of_band(bb, v)
            if d:
                add("perf_out_of_band", eid, perf=k, direction=d, value=v,
                    band={x: bb.get(x) for x in ("mean", "sd", "lo", "hi", "n")},
                    why=f"perf {k} = {v:.4g} ticks/op, {'below' if d == 'lower' else 'above'} the baseline band "
                        f"[{bb['lo']:.4g}, {bb['hi']:.4g}] ({'faster' if d == 'lower' else 'slower'})", **ctx)
        for k in sorted(set(nperf) - set(b["perf"])):
            add("perf_new", eid, perf=k, why=f"perf {k} is new", **ctx)
        ntime = timing_bands(good)
        for key, paths_ in b["timing"].items():
            for path, bb in paths_.items():
                nb = (ntime.get(key) or {}).get(path)
                if nb is None:
                    add("timing_missing", eid, case_key=key, path=path,
                        why=f"timing {key} {path} no longer reported", **ctx)
                    continue
                d = _out_of_band(bb, nb["mean"])
                if d:
                    add("timing_out_of_band", eid, case_key=key, path=path, direction=d, value=nb["mean"],
                        band={x: bb.get(x) for x in ("mean", "sd", "lo", "hi", "n")},
                        why=f"timing {key} {path} = {nb['mean']:.4g}, {d} than the baseline band "
                            f"[{bb['lo']:.4g}, {bb['hi']:.4g}]", **ctx)
    for eid in sorted(set(check) - set(baseline["entries"])):
        add("new_entry", eid, why="entry not in the baseline")
    # Perf/timing on cpu_speed=max entries (fast core, JIT) follow the host's
    # speed and load: reported, but not counted as regressions.
    for f in findings:
        if f["kind"] in ("perf_out_of_band", "timing_out_of_band"):
            e = (check.get(f["entry"]) or {}).get("entry") or {}
            if str((e.get("options") or {}).get("cpu_speed", "real")) == "max":
                f["host_dependent"] = True
                f["why"] += " [cpu_speed=max: timing follows the host, informational]"
    counts = {}
    for f in findings:
        k = f["kind"] + (" (host-dependent)" if f.get("host_dependent") else "")
        counts[k] = counts.get(k, 0) + 1
    return {"findings": findings, "counts": counts, "entries": per_entry,
            "regressions": sum(1 for f in findings if f["kind"] in REGRESSION_KINDS and not f.get("host_dependent"))}


def finding_key(f):
    return (f["entry"], f.get("case_key") or f.get("perf") or f.get("path") or "", f["kind"] if f["kind"] in (
        "entry_status", "missing_entry", "new_entry") else "")


def first_changes(per_binary):  # noqa: C901
    """per_binary: [(label, diff result)] in the order given. For every
    fingerprint that changed on some binary: the first binary where it did."""
    seen = {}
    for i, (label, d) in enumerate(per_binary):
        for f in d["findings"]:
            if f["kind"] in ("unstable_excluded", "perf_new", "new_entry") or f.get("host_dependent"):
                continue
            k = finding_key(f)
            if k not in seen:
                seen[k] = {"entry": f["entry"], "key": k[1] or k[2], "first_binary": label, "index": i,
                           "kind": f["kind"], "why": f.get("why"),
                           "on": [lb for lb, dd in per_binary if any(finding_key(x) == k for x in dd["findings"])]}
    return sorted(seen.values(), key=lambda x: (x["index"], x["entry"], x["key"]))


# ---------------------------------------------------------------------------
# storage

def baseline_dir(workdir, name):
    return os.path.join(os.path.abspath(workdir), "baselines", name)


def save(workdir, name, doc):
    d = baseline_dir(workdir, name)
    os.makedirs(d, exist_ok=True)
    p = os.path.join(d, "baseline.json")
    with open(p + ".new", "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=1, default=_json_default)
    os.replace(p + ".new", p)
    return p


def load(workdir, name):
    p = os.path.join(baseline_dir(workdir, name), "baseline.json")
    if not os.path.isfile(p):
        return None
    with open(p, encoding="utf-8") as fh:
        return json.load(fh)


def _json_default(o):
    if isinstance(o, set):
        return sorted(o)
    raise TypeError(type(o).__name__)


def entry_meta(result, ctx_env):
    rom = result.get("rom") or {}
    ext = result.get("ext_rom") or {}
    disk = result.get("disk") or {}
    return {"rom": rom.get("id", rom.get("builtin")), "rom_name": rom.get("name"), "rom_crc32": rom.get("crc32"),
            "ext_rom": ext.get("id"),
            "disk_build": disk.get("build_id") if "set" not in disk else
            " | ".join(f"{d.get('name')}={d.get('build_id')}" for d in disk["set"]),
            "binary_sha256": ((ctx_env or {}).get("amiberry") or {}).get("sha256"),
            "adjusted": result.get("adjusted"), "resolved_options": result.get("resolved_options"),
            "warp": result.get("warp")}


def now_iso():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ")
