"""The `baseline`, `check` and `repro` commands (see baseline.py for the
data model and diff rules, problems.py for repro packs)."""
from __future__ import annotations

import copy
import json
import os

from . import baseline as B
from . import cases as casemod
from . import matrix
from . import runner as R
from . import xfail as xfailmod
from .emulator import IsolationError, RunnerError


def _strip(entry):
    e = {k: (sorted(v) if isinstance(v, set) else v) for k, v in entry.items()}
    return e


def _collect(opts, entries, outdir, repeat, label, binary=None, run_id=None):
    """Runs `entries` `repeat` times. Returns ({eid: {entry, statuses, runs, meta}}, contexts' environments)."""
    per = {e["id"]: {"entry": _strip(e), "statuses": [], "runs": [], "meta": {}, "results": []} for e in entries}
    envs = []
    o = copy.copy(opts)
    if binary:
        o.binary = binary
    for i in range(repeat):
        rdir = R.unique_dir(os.path.join(outdir, f"run{i + 1}"))
        ctx = R.Context(o, rdir, run_id=f"{run_id or R.new_run_id()}-r{i + 1}", label=label)
        try:
            R.log(o, f"{label}: repetition {i + 1}/{repeat}, {len(entries)} entries, binary {o.binary}")
            results = ctx.run_entries(entries)
        finally:
            ctx.close()
        envs.append(ctx.environment)
        from . import report
        try:
            report.write_all(rdir, ctx.environment, results, source=label)
        except Exception as ex:   # the fingerprints matter more than this repetition's report
            import traceback
            R.log(o, f"{label}: REPORT.md for {rdir} failed: {type(ex).__name__}: {ex}")
            with open(os.path.join(rdir, "report-error.txt"), "w", encoding="utf-8") as fh:
                fh.write(traceback.format_exc())
        if not ctx.environment.get("isolation", {}).get("ok", True):
            raise IsolationError(f"{label}: the isolation guard recorded changes to the user's files")
        for r in results:
            d = per[r["id"]]
            d["statuses"].append(r["status"])
            d["runs"].append(casemod.extract(r))
            d["meta"] = B.entry_meta(r, ctx.environment)
            d["results"].append({"status": r["status"], "reason": r.get("reason"), "adjusted": r.get("adjusted"),
                                 "raw": os.path.join(os.path.basename(rdir), r.get("raw") or "")})
    return per, envs


def _publish_baseline(bdir):
    names = ("runs", "baseline.json", "BASELINE.md")
    for name in names:
        if not os.path.exists(os.path.join(bdir, name + ".new")):
            raise RunnerError(f"baseline preparation is missing {name}.new")
    existed = {name: os.path.exists(os.path.join(bdir, name)) for name in names}
    try:
        for name in names:
            path = os.path.join(bdir, name)
            if existed[name]:
                if os.path.isdir(path):
                    R._safe_rmtree(path + ".previous", bdir)
                os.replace(path, path + ".previous")
            os.replace(path + ".new", path)
    except BaseException:
        # A consumed .new identifies an installed artifact even if interrupted.
        for name in reversed(names):
            path = os.path.join(bdir, name)
            if not os.path.exists(path + ".new"):
                if os.path.isdir(path):
                    R._safe_rmtree(path, bdir)
                elif os.path.exists(path):
                    os.unlink(path)
            if existed[name] and not os.path.exists(path) and os.path.exists(path + ".previous"):
                os.replace(path + ".previous", path)
        raise


def cmd_baseline(opts, name, repeat):
    if repeat < 1:
        raise RunnerError("--repeat must be at least 1")
    entries = R.select_entries(opts)
    bdir = B.baseline_dir(opts.workdir, name)
    runs_dir = os.path.join(bdir, "runs")
    new_runs = runs_dir + ".new"
    existed = os.path.isdir(bdir)
    os.makedirs(bdir, exist_ok=True)
    R._safe_rmtree(new_runs, bdir)
    # Prepare every artifact before publishing; failed publication rolls back.
    try:
        per, envs = _collect(opts, entries, new_runs, repeat, "baseline")
        env = envs[0]
        meta = {"name": name, "created": B.now_iso(), "repeat": repeat, "matrix": opts.matrix, "configs": opts.configs,
                "sets": opts.sets, "params": opts.params, "warp": getattr(opts, "warp", None),
                "binary": {k: env["amiberry"].get(k) for k in ("path", "sha256", "version", "size", "mtime")},
                "disk": {k: (env.get("disk_source") or {}).get(k) for k in ("path", "build_id", "sha256", "set")},
                "kickstarts": env.get("kickstarts_dir"), "amitorture": env.get("amitorture"), "host": env.get("host"),
                "roms": {eid: {k: d["meta"].get(k) for k in ("rom", "rom_name", "rom_crc32", "ext_rom")}
                         for eid, d in per.items()}}
        doc = B.build({eid: {k: d[k] for k in ("entry", "statuses", "runs", "meta")} for eid, d in per.items()}, meta)
        for eid, d in per.items():
            doc["entries"][eid]["runs_raw"] = d["results"]
        with open(os.path.join(bdir, "baseline.json.new"), "w", encoding="utf-8") as fh:
            json.dump(doc, fh, indent=1, default=B._json_default)
        with open(os.path.join(bdir, "BASELINE.md.new"), "w", encoding="utf-8") as fh:
            fh.write(render_baseline(doc))
        _publish_baseline(bdir)
    except BaseException:
        R._safe_rmtree(new_runs, bdir)
        for name_ in ("baseline.json.new", "BASELINE.md.new"):
            path = os.path.join(bdir, name_)
            if os.path.exists(path):
                os.unlink(path)
        if not existed:
            R._safe_rmtree(bdir, B.baseline_dir(opts.workdir, ""))
        raise
    return os.path.join(bdir, "baseline.json"), doc


def render_baseline(doc):
    m = doc["meta"]
    L = [f"# amitorture baseline `{m['name']}`", "",
         f"Created {m['created']}: {m['repeat']} run(s) of each entry of matrix `{m['matrix']}`"
         + (f" (--config {m['configs']})" if m.get("configs") else "") + (f" --set {m['sets']}" if m.get("sets") else "")
         + f" on `{m['binary'].get('version')}` (sha256 `{m['binary'].get('sha256')}`), disk build "
           f"`{m['disk'].get('build_id')}`. Data: baseline.json; raw runs: runs/run<N>/.", "",
         f"Unstable cases (fingerprint differs between identical runs, excluded from `check`): {m['unstable_cases']}",
         "Stability uses only the runs that completed (pass/fail); runs that hung, crashed or timed out are "
         "listed as excluded and reported by `check` as entry-level events.", "",
         "| Entry | Statuses | Complete runs | Cases | Unstable | Perf values | ROM | Adjusted by emulator |",
         "|---|---|---|---|---|---|---|---|"]
    for eid, e in doc["entries"].items():
        uns = [k for k, c in e["cases"].items() if not c["stable"]]
        adj = (e.get("meta") or {}).get("adjusted")
        L.append(f"| {eid} | {', '.join(e['statuses'])} | {e.get('complete_runs')} | {len(e['cases'])} | {len(uns)} | "
                 f"{len(e['perf'])} | "
                 f"{(e.get('meta') or {}).get('rom_name') or (e.get('meta') or {}).get('rom') or ''} | "
                 f"{'; '.join(d['option'] + ' ' + str(d['wrote']) + '->' + str(d['resolved']) for d in adj) if adj else ''} |")
    L.append("")
    for eid, e in doc["entries"].items():
        uns = {k: c for k, c in e["cases"].items() if not c["stable"]}
        if uns:
            L.append(f"## Unstable on {eid}")
            L.append("")
            for k, c in uns.items():
                L.append(f"- `{k}`: present {c['present']}, st {c['st']}, {len(c.get('variants', []))} fingerprint variants")
            L.append("")
    perf = [(eid, k, b) for eid, e in doc["entries"].items() for k, b in e["perf"].items()]
    if perf:
        L.append("## Perf bands (ticks/op, mean ± max(k·σ, pct·mean, abs))")
        L.append("")
        L.append("| Entry | Perf | mean | σ | band | n |")
        L.append("|---|---|---|---|---|---|")
        for eid, k, b in perf:
            if b.get("mean") is None:
                continue
            L.append(f"| {eid} | {k} | {b['mean']:.4g} | {b['sd']:.3g} | [{b['lo']:.4g}, {b['hi']:.4g}] | {b['n']} |")
        L.append("")
    return "\n".join(L)


def cmd_check(opts, name, binaries, repeat):
    if repeat < 1:
        raise RunnerError("--repeat must be at least 1")
    base = B.load(opts.workdir, name)
    if base is None:
        raise RunnerError(f"no baseline {name!r} in {B.baseline_dir(opts.workdir, name)} (make one with `baseline`)")
    if getattr(opts, "warp", None) is None and "warp" not in opts.sets:
        opts = copy.copy(opts)
        opts.warp = base["meta"].get("warp")
    entries = []
    for eid, be in base["entries"].items():
        e = dict(be["entry"])
        e["matrices"] = set(e.get("matrices") or [])
        e["options"] = dict(e["options"])
        entries.append(e)
    if opts.configs:
        entries = matrix.select(entries, opts.configs)
        if not entries:
            raise RunnerError(f"--config {opts.configs}: no entry of baseline {name} matches")
        keep = {e["id"] for e in entries}
        base = dict(base, entries={k: v for k, v in base["entries"].items() if k in keep})
    matrix.apply_overrides(entries, opts.sets)
    run_id = R.new_run_id()
    outdir = R.unique_dir(os.path.join(os.path.abspath(opts.workdir), "results", f"check-{run_id}"))
    registry = xfailmod.load(opts.xfail_path)
    per_binary = []
    envs = {}
    for i, b in enumerate(binaries):
        label = f"#{i + 1} {os.path.basename(b)}" if len(binaries) > 1 else os.path.basename(b)
        per, envl = _collect(opts, entries, os.path.join(outdir, f"bin{i + 1}"), repeat, "check", binary=b,
                             run_id=run_id)
        d = B.diff(base, {eid: {k: x[k] for k in ("entry", "statuses", "runs", "meta")} for eid, x in per.items()},
                   registry)
        d["binary"] = {k: envl[0]["amiberry"].get(k) for k in ("path", "sha256", "version")}
        d["label"] = label
        d["raw"] = {eid: x["results"] for eid, x in per.items()}
        per_binary.append((label, d))
        envs[label] = envl[0]
    first = B.first_changes(per_binary) if len(per_binary) > 1 else []
    doc = {"baseline": name, "baseline_meta": base["meta"], "repeat": repeat, "sets": opts.sets,
           "warp": getattr(opts, "warp", None),
           "entry_ids": [e["id"] for e in entries],
           "binaries": [{"label": lb, **d["binary"], "counts": d["counts"], "regressions": d["regressions"]}
                        for lb, d in per_binary],
           "per_binary": [{"label": lb, "findings": d["findings"], "entries": d["entries"]} for lb, d in per_binary],
           "first_changes": first}
    with open(os.path.join(outdir, "check.json"), "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=1, default=lambda o: sorted(o) if isinstance(o, set) else str(o))
    with open(os.path.join(outdir, "REPORT.md"), "w", encoding="utf-8") as fh:
        fh.write(render_check(doc))
    return outdir, doc


CHECK_ORDER = ["entry_status", "missing_entry", "new_failure", "regression", "became_unstable", "missing",
               "perf_missing", "timing_missing", "perf_out_of_band", "timing_out_of_band",
               "fixed", "new", "perf_new", "new_entry", "unstable_excluded"]


def check_verdict(doc):
    regs = sum(b["regressions"] for b in doc["binaries"])
    fixed = sum(b["counts"].get("fixed", 0) for b in doc["binaries"])
    if regs:
        return f"REGRESSIONS ({regs} finding(s) over {len(doc['binaries'])} binar{'y' if len(doc['binaries']) == 1 else 'ies'})"
    return "NO REGRESSIONS" + (f" ({fixed} fixed)" if fixed else "")


def render_check(doc):
    from .report import _cell, _j
    bm = doc["baseline_meta"]
    L = [f"# amitorture check against baseline `{doc['baseline']}`: {check_verdict(doc)}", "",
         f"Baseline: {bm.get('repeat')} run(s) per entry of `{bm.get('matrix')}` on `{bm['binary'].get('version')}` "
         f"(sha256 `{bm['binary'].get('sha256')}`), disk `{bm['disk'].get('build_id')}`, created {bm.get('created')}. "
         f"Check: {doc['repeat']} run(s) per entry" + (f", --set {doc['sets']}" if doc.get("sets") else "")
         + ". Data: check.json; raw runs: bin<N>/run<M>/.", "",
         "A fingerprint is a case record minus seq and timing fields; a regression is a stable baseline fingerprint "
         "that changed. Each finding names the entry's config changes vs the baseline, so a changed option is "
         "pinned to the cases it changed.", "", "## Binaries", "",
         "| # | Binary | sha256 | Version | Regressions | Counts |", "|---|---|---|---|---|---|"]
    for i, b in enumerate(doc["binaries"]):
        L.append(f"| {i + 1} | `{_cell(b.get('path'))}` | `{_cell((b.get('sha256') or '')[:16])}` | {_cell(b.get('version'))} "
                 f"| {b['regressions']} | {_cell(', '.join(f'{k} {v}' for k, v in sorted(b['counts'].items())))} |")
    L.append("")
    if doc.get("first_changes"):
        L.append("## First binary where each fingerprint changed")
        L.append("")
        L.append("| Entry | Case / perf | Kind | First changed on | Changed on |")
        L.append("|---|---|---|---|---|")
        for f in doc["first_changes"]:
            L.append(f"| {f['entry']} | `{_cell(f['key'])}` | {f['kind']} | {_cell(f['first_binary'])} | "
                     f"{_cell(', '.join(f['on']))} |")
        L.append("")
    for pb in doc["per_binary"]:
        L.append(f"## Findings on {pb['label']}")
        L.append("")
        cfg = {eid: e for eid, e in pb["entries"].items() if e.get("config_changes") or e.get("meta_changes")}
        if cfg:
            L.append("Entries whose setup differs from the baseline:")
            L.append("")
            for eid, e in cfg.items():
                parts = [f"`{d['option']}` {d['baseline']} -> {d['now']}" for d in e.get("config_changes", [])]
                parts += [f"{d['what']} {d['baseline']} -> {d['now']}" for d in e.get("meta_changes", [])]
                L.append(f"- `{eid}`: " + "; ".join(parts))
            L.append("")
        fs = sorted(pb["findings"], key=lambda f: (CHECK_ORDER.index(f["kind"]) if f["kind"] in CHECK_ORDER else 99,
                                                   f["entry"], f.get("case_key") or ""))
        if not fs:
            L.append("No differences.")
            L.append("")
        for f in fs:
            head = f"- **{f['kind']}** `{f['entry']}`" + (f" `{f['case_key']}`" if f.get("case_key") else "") + \
                   (f" `{f['perf']}`" if f.get("perf") else "") + f": {_cell(f.get('why'))}"
            L.append(head)
            if f.get("config_changes"):
                L.append("  - config vs baseline: " + ", ".join(f"`{d['option']}` {d['baseline']} -> {d['now']}"
                                                              for d in f["config_changes"]))
            if f.get("paths"):
                L.append("  - differs in: " + "; ".join(f"`{p['path']}` {_j(p['baseline'], 80)} -> {_j(p['now'], 80)}"
                                                     for p in f["paths"][:10]))
            if f["kind"] in ("regression", "fixed"):
                L.append(f"  - baseline: `{_j(f['baseline'], 700)}`")
                L.append(f"  - now: `{_j(f['now'], 700)}`")
            elif f["kind"] == "new_failure":
                L.append(f"  - now: `{_j(f['now'], 700)}`")
            if f.get("xfail"):
                L.append(f"  - xfail: {f['xfail'].get('ref')}: {f['xfail'].get('why')}")
            if f.get("band"):
                L.append(f"  - band: `{_j(f['band'])}`, value {f.get('value')}")
        L.append("")
    return "\n".join(L)


# ---------------------------------------------------------------------------
# repro

def cmd_repro(packdir, binary=None, gdb=False, keep=False, timeout=300.0, verbose=False, show=False):
    from . import problems
    packdir = os.path.abspath(packdir)
    with open(os.path.join(packdir, "pack.json"), encoding="utf-8") as fh:
        pack = json.load(fh)
    binary = binary or os.environ.get("AMIBERRY") or pack["binary"]
    from . import emulator
    binary = emulator.find_binary(binary)
    entry = problems.entry_from_pack(pack)
    entry["id"] = f"{entry['id']}~repro"
    work = os.path.join(packdir, "work")
    adf = os.path.join(packdir, "case.adf")
    opts = R.Options(binary=binary, kickstarts=pack.get("kickstarts") or os.path.dirname(
        (pack.get("rom") or {}).get("path") or "."), workdir=work, timeout=timeout, adf=adf, verbose=verbose,
        keep=keep, params={}, narrow=False, repro=False, media=True, disk_set=None, show=show)
    entry["params"] = {}     # the pack's ADF already has its parameters baked in
    outdir = R.unique_dir(os.path.join(work, "results", R.new_run_id()))
    ctx = R.Context(opts, outdir, label="repro")
    extra = {"debugging_features": "gdbserver", "gdb_port": str(pack.get("gdb_port") or problems.GDB_PORT)} \
        if gdb and pack.get("gdb_port") else None
    if gdb:
        print(f"GDB server on 127.0.0.1:{pack.get('gdb_port')}: m68k-amigaos-gdb -x {packdir}/gdb.cmd", flush=True)
    try:
        res = ctx.run_entries([entry], extra_options=extra)[0]
    finally:
        ctx.close()
    if not ctx.environment.get("isolation", {}).get("ok", True):
        raise IsolationError("repro: the isolation guard recorded changes to the user's files")
    fp = casemod.extract(res)
    key = pack.get("case_key")
    c = fp["cases"].get(key)
    if c is None:
        print(f"{key}: not reported (entry {res['status']}: {res.get('reason')}); raw: {outdir}")
        return 2
    rec = dict(c["rec"])
    print(json.dumps(rec, separators=(",", ":")))
    if pack.get("kind") == "engine_mismatch":
        ref = pack.get("ref")
        same = ref is not None and casemod.canonical(casemod.strip_timing(ref)[0]) == c["fp"]
        print(f"{key}: {'matches' if same else 'DIFFERS FROM'} the reference entry {pack.get('ref_entry')}")
        return 0 if same else 1
    print(f"{key}: {c['st']}")
    return 0 if c["st"] == "pass" else 1


def rediff(check_dir, workdir):
    """Recomputes a check's diff from its saved raw runs (bin<N>/run<M>/raw)
    with the current rules, and rewrites check.json and REPORT.md."""
    from . import analysis
    with open(os.path.join(check_dir, "check.json"), encoding="utf-8") as fh:
        old = json.load(fh)
    base = B.load(workdir, old["baseline"])
    if base is None:
        raise RunnerError(f"baseline {old['baseline']} not found")
    registry = xfailmod.load()
    per_binary = []
    for i, b in enumerate(old["binaries"]):
        bdir = os.path.join(check_dir, f"bin{i + 1}")
        per = {}
        for run in sorted(os.listdir(bdir)) if os.path.isdir(bdir) else []:
            rdir = os.path.join(bdir, run)
            env = {}
            try:
                with open(os.path.join(rdir, "environment.json"), encoding="utf-8") as fh:
                    env = json.load(fh)
            except OSError:
                pass
            if not env.get("isolation", {}).get("ok", True):
                raise IsolationError(f"saved run {rdir} recorded an isolation violation")
            for r in analysis.load_results(rdir):
                be = (base["entries"].get(r["id"]) or {}).get("entry") or {"id": r["id"]}
                e = dict(be, options=dict(r.get("options") or be.get("options") or {}))
                d = per.setdefault(r["id"], {"entry": e, "statuses": [], "runs": [], "meta": {}})
                d["statuses"].append(r["status"])
                d["runs"].append(casemod.extract(r))
                d["meta"] = B.entry_meta(r, env)
        keep = old.get("entry_ids")
        if keep is None:
            saved = old["per_binary"][i]
            keep = set(saved.get("entries") or {})
            keep.update(f["entry"] for f in saved.get("findings", []) if f["kind"] == "missing_entry")
            if not keep:
                keep = set(base["entries"])
        if set(keep) - set(base["entries"]):
            raise RunnerError("the baseline no longer contains all entries of the saved check")
        bsub = dict(base, entries={k: v for k, v in base["entries"].items() if k in keep})
        dd = B.diff(bsub, per, registry)
        per_binary.append((b["label"], dd, b))
    doc = dict(old)
    doc["binaries"] = [dict({k: b.get(k) for k in ("label", "path", "sha256", "version")}, counts=dd["counts"],
                            regressions=dd["regressions"]) for lb, dd, b in per_binary]
    doc["per_binary"] = [{"label": lb, "findings": dd["findings"], "entries": dd["entries"]} for lb, dd, b in per_binary]
    doc["first_changes"] = B.first_changes([(lb, dd) for lb, dd, b in per_binary]) if len(per_binary) > 1 else []
    doc["rediffed"] = B.now_iso()
    with open(os.path.join(check_dir, "check.json"), "w", encoding="utf-8") as fh:
        json.dump(doc, fh, indent=1, default=lambda o: sorted(o) if isinstance(o, set) else str(o))
    with open(os.path.join(check_dir, "REPORT.md"), "w", encoding="utf-8") as fh:
        fh.write(render_check(doc))
    return doc
