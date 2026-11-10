"""compare: the hostspeed benchmarks of two Amiberry binaries, A/B.

Runs the same entries (normally matrix benchmarks) on binary A and binary B,
`repeat` times, one launch at a time, alternating which binary goes first
(A B, B A, A B, ...) so a slow drift of the host (thermal, another process)
does not favour either. Optionally pins the runner and every emulator to a
CPU set. Each launch is a normal `run` (its own results dir, REPORT.md and
summary.json); compare then reads summary.json["hostspeed"] and, per entry
and block, compares B with A:

  * instruction blocks: host ops/s (higher is better);
  * frame blocks (cases.FRAME_BLOCKS: stop, vbl_poll, chipset_load), where
    the CPU idles for a number of frames: total host CPU seconds per
    emulated second (lower is better).

Verdict per block, with `threshold` percent:
  slower / faster  the medians differ by more than the threshold and the
                   two sets of repeats do not overlap (B's best is worse
                   than A's worst, or B's worst better than A's best);
  borderline       the medians differ by more than the threshold but the
                   repeats overlap: rerun with more repeats;
  same             within the threshold.
Writes COMPARE.md and compare.json (binaries with sha256, disk sha256, host
and CPU frequency policy, load, affinity, every command). Exit status 1 if
any block is slower.
"""
from __future__ import annotations

import hashlib
import json
import os
import statistics
import subprocess
import sys
import time

from . import cases as casemod
from . import envinfo

RUN_DIR = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))


def parse_cpus(spec):
    """'2-5,8' -> {2, 3, 4, 5, 8}"""
    out = set()
    for part in str(spec).split(","):
        part = part.strip()
        if not part:
            continue
        if "-" in part:
            lo, hi = part.split("-", 1)
            out.update(range(int(lo), int(hi) + 1))
        else:
            out.add(int(part))
    if not out:
        raise ValueError(f"empty CPU set {spec!r}")
    return out


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as fh:
        for chunk in iter(lambda: fh.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def metric(block):
    """(value, higher_is_better, unit) of a measured block, or None."""
    if block.get("st") not in ("measured", "pass"):
        return None
    if block.get("insn") in casemod.FRAME_BLOCKS:
        v = block.get("cpu_total_per_emu_s")
        return None if v is None else (v, False, "total CPU/s")
    v = block.get("ops_per_host_s")
    return None if v is None else (v, True, "host ops/s")


def collect(summary):
    """{(entry, disk, insn): (value, higher_is_better, unit)} from summary.json."""
    out = {}
    for e in summary.get("hostspeed") or []:
        for b in e.get("blocks") or []:
            m = metric(b)
            if m is not None:
                out[(e["entry"], e.get("disk"), b["insn"])] = m
    return out


def verdict(a, b, higher, threshold):
    """a, b: the repeats of A and B. Returns (verdict, change_pct), where
    change_pct > 0 means B is better."""
    ma, mb = statistics.median(a), statistics.median(b)
    if ma <= 0 or mb <= 0:
        return "same", 0.0
    change = (mb / ma - 1) * 100 if higher else (ma / mb - 1) * 100
    if abs(change) <= threshold:
        return "same", change
    if higher:
        worse, better = max(b) < min(a), min(b) > max(a)
    else:
        worse, better = min(b) > max(a), max(b) < min(a)
    if change < 0 and worse:
        return "slower", change
    if change > 0 and better:
        return "faster", change
    return "borderline", change


def analyse(runs, labels, threshold):
    """runs: [{"label", "values": {key: (v, higher, unit)}}]. Returns rows."""
    series = {}
    for r in runs:
        for key, (v, higher, unit) in r["values"].items():
            s = series.setdefault(key, {"higher": higher, "unit": unit, labels[0]: [], labels[1]: []})
            s[r["label"]].append(v)
    rows = []
    for (entry, disk, insn), s in sorted(series.items(), key=lambda kv: (kv[0][0], str(kv[0][1]), kv[0][2])):
        a, b = s[labels[0]], s[labels[1]]
        row = {"entry": entry, "disk": disk, "insn": insn, "unit": s["unit"], "higher_is_better": s["higher"],
               "a": a, "b": b}
        if not a or not b:
            row.update(verdict="incomplete", change_pct=None)
        else:
            v, change = verdict(a, b, s["higher"], threshold)
            row.update(verdict=v, change_pct=round(change, 2),
                       median_a=statistics.median(a), median_b=statistics.median(b))
        rows.append(row)
    return rows


def _fmt(v):
    return "-" if v is None else f"{v:.4g}"


def markdown(manifest, rows):
    la, lb = manifest["labels"]
    counts = {}
    for r in rows:
        counts[r["verdict"]] = counts.get(r["verdict"], 0) + 1
    L = [f"# amitorture compare: {la} vs {lb}", "",
         f"- A `{la}`: `{manifest['binaries'][la]['path']}` (sha256 {manifest['binaries'][la]['sha256'][:16]})",
         f"- B `{lb}`: `{manifest['binaries'][lb]['path']}` (sha256 {manifest['binaries'][lb]['sha256'][:16]})",
         f"- repeats: {manifest['repeat']}, alternating order; threshold {manifest['threshold']}%; "
         f"CPU affinity {manifest['affinity']}",
         f"- host: {casemod.host_desc(manifest['host'])}",
         f"- verdicts: " + ", ".join(f"{k} {v}" for k, v in sorted(counts.items())), "",
         "Change > 0: B is better (more host ops/s; for the frame blocks stop/vbl_poll/chipset_load, less total "
         "host CPU per emulated second). **slower**/**faster**: beyond the threshold and the repeats do not "
         "overlap; borderline: beyond the threshold but they overlap (rerun with more repeats).", "",
         f"| entry | block | measure | {la} | {lb} | change | verdict |", "|---|---|---|---|---|---|---|"]
    for r in rows:
        v = r["verdict"]
        v = f"**{v}**" if v in ("slower", "faster") else v
        ch = "-" if r.get("change_pct") is None else f"{r['change_pct']:+.1f}%"
        name = r["entry"] + (f" [{r['disk']}]" if r.get("disk") else "")
        L.append(f"| {name} | {r['insn']} | {r['unit']} | {' / '.join(_fmt(x) for x in r['a'])} | "
                 f"{' / '.join(_fmt(x) for x in r['b'])} | {ch} | {v} |")
    L.extend(["", "## Launches", ""])
    for run in manifest["runs"]:
        L.append(f"- repeat {run['repeat']} `{run['label']}`: {run['verdict']}, {run['seconds']:.0f} s, "
                 f"load {run['loadavg_start'][0]:.2f} -> {run['loadavg_end'][0]:.2f}: {run['result']}")
    return "\n".join(L) + "\n"


def run(a):
    """The `compare` command."""
    from .emulator import RunnerError
    if len(a.amiberry or []) != 2:
        raise RunnerError("compare needs exactly two --amiberry binaries (A, then B)")
    paths = [os.path.abspath(os.path.expanduser(p)) for p in a.amiberry]
    for p in paths:
        if not os.access(p, os.X_OK):
            raise RunnerError(f"not an executable: {p}")
    labels = list(a.label or []) or ["A", "B"]
    if len(labels) != 2 or labels[0] == labels[1]:
        raise RunnerError("--label: give two different names, one per binary")
    if a.repeat < 1:
        raise RunnerError("--repeat must be at least 1")
    if a.pin:
        try:
            os.sched_setaffinity(0, parse_cpus(a.pin))
        except (ValueError, OSError) as e:
            raise RunnerError(f"--pin {a.pin}: {e}")
    adf = a.adf or (os.path.join(RUN_DIR, "..", "disk", "build", "benchmarks", "amitorture.adf")
                    if a.matrix == "benchmarks" else None)
    adf = adf and os.path.abspath(os.path.normpath(adf))
    if adf and not os.path.isfile(adf):
        raise RunnerError(f"no test disk at {adf}: build it (make -C disk benchmarks) or pass --adf")
    workdir = os.path.abspath(a.workdir)
    out = os.path.join(workdir, "compare", time.strftime("%Y%m%d-%H%M%SZ", time.gmtime()))
    os.makedirs(out, exist_ok=True)
    disk_sha = sha256(adf) if adf else None
    manifest = {"labels": labels, "repeat": a.repeat, "threshold": a.threshold, "matrix": a.matrix,
                "config": a.config or [], "set": a.set or [], "adf": adf, "adf_sha256": disk_sha,
                "affinity": sorted(os.sched_getaffinity(0)), "host": envinfo.host_brief(),
                "binaries": {lab: {"path": p, "sha256": sha256(p)} for lab, p in zip(labels, paths)},
                "runs": []}
    by_label = dict(zip(labels, paths))
    runs = []

    def save(rows=None):
        doc = dict(manifest, rows=rows)
        with open(os.path.join(out, "compare.json"), "w", encoding="utf-8") as fh:
            json.dump(doc, fh, indent=1)

    for rep in range(1, a.repeat + 1):
        order = labels if rep % 2 else labels[::-1]
        for lab in order:
            if (adf and sha256(adf) != disk_sha) or sha256(by_label[lab]) != manifest["binaries"][lab]["sha256"]:
                raise RunnerError("the test disk or a binary changed during the comparison")
            cmd = [sys.executable, "-m", "amitorture", "run", "--amiberry", by_label[lab],
                   "--matrix", a.matrix, "--workdir", workdir, "--jobs", "1", "--warp", "off",
                   "--no-narrow", "--no-repro", "-q"]
            if adf:
                cmd += ["--adf", adf]
            if a.kickstarts:
                cmd += ["--kickstarts", a.kickstarts]
            for c in a.config or []:
                cmd += ["--config", c]
            for s in a.set or []:
                cmd += ["--set", s]
            results = os.path.join(workdir, "results")
            before = set(os.listdir(results)) if os.path.isdir(results) else set()
            log = os.path.join(out, f"{rep}-{lab}.log")
            load0, t0 = os.getloadavg(), time.monotonic()
            if not a.quiet:
                print(f"[compare] repeat {rep}/{a.repeat}: {lab}", flush=True)
            with open(log, "w", encoding="utf-8") as fh:
                rc = subprocess.run(cmd, cwd=RUN_DIR, stdout=fh, stderr=subprocess.STDOUT).returncode
            created = sorted(set(os.listdir(results)) - before) if os.path.isdir(results) else []
            if len(created) != 1:
                raise RunnerError(f"expected one new results dir from the run, got {created}: see {log}")
            res = os.path.join(results, created[0])
            try:
                with open(os.path.join(res, "summary.json"), encoding="utf-8") as fh:
                    summary = json.load(fh)
            except (OSError, ValueError) as e:
                raise RunnerError(f"no summary.json in {res} ({e}): see {log}")
            values = collect(summary)
            runs.append({"label": lab, "values": values})
            manifest["runs"].append({"repeat": rep, "label": lab, "command": cmd, "returncode": rc, "log": log,
                                     "result": res, "verdict": summary.get("verdict"),
                                     "seconds": round(time.monotonic() - t0, 1), "loadavg_start": load0,
                                     "loadavg_end": os.getloadavg(), "blocks": len(values)})
            save()
    rows = analyse(runs, labels, a.threshold)
    save(rows)
    with open(os.path.join(out, "COMPARE.md"), "w", encoding="utf-8") as fh:
        fh.write(markdown(manifest, rows))
    slower = [r for r in rows if r["verdict"] == "slower"]
    print(f"{'SLOWER' if slower else 'OK'}: {len(slower)} slower, "
          f"{sum(r['verdict'] == 'faster' for r in rows)} faster, "
          f"{sum(r['verdict'] == 'borderline' for r in rows)} borderline of {len(rows)} blocks: "
          f"{os.path.join(out, 'COMPARE.md')}")
    return 1 if slower else 0
