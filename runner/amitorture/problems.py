"""Problems (grouped failures) and repro packs.

Grouping: case failures and engine mismatches that share a module and a
narrowing signature (the set of settings that flip them, plus the
determinism label) become one problem: 400 failing ADDX cases that all flip
on comp_nf are one problem, not 400. Without narrowing data the signature is
the set of entries the case fails on. Other kinds (crash, hang, probe
mismatch, config not applied ...) are grouped as before (report._key).

Repro pack, repro/<problem>/ :
  case.adf      the test disk with only the module enabled (modules=<mask>)
                and the run's seed/mode baked in (CONTRACT §4). The disk has
                no per-case switch: the module's other cases run too, and
                run.sh prints the one that matters.
  config.uae    the failing entry's config, standalone (absolute ROM path in
                the kickstarts dir, case.adf in DF0, serial TCP without
                /wait, GDB server on: debugging_features=gdbserver, unless
                the CPU is threaded, where Amiberry refuses it)
  pack.json     everything the runner needs to rerun it (entry, ROM, case)
  run.sh        reruns it isolated and prints the case result line
  gdb.cmd       m68k-amigaos-gdb commands for Amiberry's GDB server
  host-gdb.cmd  host gdb: breakpoints on the suggested emulator functions
"""
from __future__ import annotations

import json
import os
import re
import shlex
import stat

from . import amiberry_cfg, codemap, matrix

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
RUNNER_DIR = os.path.join(REPO, "runner")
GDB_PORT = 23597          # inside this runner's range (23540-23599)
REPRO_SERIAL_PORT = 23598
GROUPED = ("case_fail", "engine_mismatch")


# ---------------------------------------------------------------------------
# docs/cases/<module>.md

_doc_cache = {}


def case_doc(mod, case, repo=REPO):
    """The line(s) of docs/cases/<mod>.md that describe `case` (intent, rule,
    what a failure means), or None."""
    if mod not in _doc_cache:
        p = os.path.join(repo, "docs", "cases", f"{mod}.md")
        try:
            with open(p, encoding="utf-8") as fh:
                _doc_cache[mod] = (p, fh.read().splitlines())
        except OSError:
            _doc_cache[mod] = (p, None)
    path, lines = _doc_cache[mod]
    if not lines or not case:
        return None
    rx = re.compile(r"(^|[`|\s*#\[(])" + re.escape(case) + r"($|[`|\s*:)\],.])")
    hits = []
    for i, line in enumerate(lines):
        if rx.search(line):
            hits.append((i, line.strip()))
    if not hits:
        # abbreviated ids in a table row: `unimpl060_movep_w`, `_movep_l`, ...
        parts = case.split("_")
        for k in range(1, len(parts)):
            prefix, suffix = "_".join(parts[:k]), "_" + "_".join(parts[k:])
            for i, line in enumerate(lines):
                if f"`{suffix}`" in line and f"`{prefix}_" in line:
                    hits.append((i, line.strip()))
                    break
            if hits:
                break
    if not hits:
        return {"file": os.path.relpath(path, repo), "line": None, "text": None}
    i, text = hits[0]
    # a heading followed by text: take the paragraph under it
    if text.startswith("#"):
        para = []
        for line in lines[i + 1:i + 8]:
            if line.startswith("#"):
                break
            if line.strip():
                para.append(line.strip())
        text = text + " " + " ".join(para)
    return {"file": os.path.relpath(path, repo), "line": i + 1, "text": text[:1500]}


# ---------------------------------------------------------------------------
# grouping

def _narrow_for(narrowing, entry, key):
    if not narrowing:
        return None
    for g in narrowing.get("groups", []):
        if g["entry"] == entry and key in g.get("cases", {}):
            return g["cases"][key]
    return None


def _fields(item):
    """The field names a failure is about: expect/got keys (engine mismatch:
    the differing paths' first component)."""
    out = set()
    for k in ("expect", "got"):
        v = item.get(k)
        if isinstance(v, dict):
            out |= {str(x) for x in v}
        elif v is not None:
            out.add(f"<{k}>")
    for p in item.get("paths") or []:
        out.add(str(p.get("path", "")).split(".")[0].split("[")[0])
    return out


def _components(members):
    """Connected components of members (key, its, nar, fields) over shared fields."""
    comps = []
    for m in members:
        hit = [c for c in comps if any(m[3] & x[3] for x in c) or (not m[3] and not any(x[3] for x in c))]
        merged = [m]
        for c in hit:
            merged += c
            comps.remove(c)
        comps.append(merged)
    for c in comps:
        c.sort(key=lambda x: x[0])
    comps.sort(key=lambda c: c[0][0])
    return comps


def group(items, results, narrowing=None):
    """items: failure items (report.collect_failures + parity). Returns problems."""
    from . import report
    by_case = {}
    others = {}
    # an engine mismatch on a case that also fails on that entry is the same
    # finding: it stays in failures.jsonl but is folded into the case_fail problem
    # (or into its xfail when the failure is a known bug)
    failing = {(i["entry"], i.get("case_key")) for i in items if i["kind"] == "case_fail"}
    folded = {}
    for it in items:
        if it["kind"] == "engine_mismatch" and (it["entry"], it.get("case_key")) in failing:
            it["folded_into_case_fail"] = True
            folded.setdefault((it["entry"], it.get("case_key")), []).append(it["id"])
    for it in items:
        if it.get("xfail") or it.get("folded_into_case_fail"):
            continue
        if it["kind"] in GROUPED:
            key = it.get("case_key") or f"{it.get('mod')}.{it.get('case')}"
            by_case.setdefault((it["kind"], key), []).append(it)
        else:
            others.setdefault(report._key(it), []).append(it)
    # Cause signature: cases share a problem only when they fail on the same
    # entries, have the same narrowing outcome, and their expect/got fields
    # overlap (connected components over shared field names). Different
    # field sets on the same module are usually different bugs (an MMU frame
    # address next to a 68060 trap vector).
    pre = {}
    for (kind, key), its in by_case.items():
        entries = sorted({i["entry"] for i in its})
        nar = None
        for e in entries:
            nar = _narrow_for(narrowing, e, key)
            if nar:
                break
        if nar and (nar["flips"] or nar["holds"]):
            sig = ("narrowed", nar["determinism"], tuple(nar["signature"]))
        else:
            sig = ("entries", tuple(entries))
        mod = its[0].get("mod")
        fields = set()
        for i in its:
            fields |= _fields(i)
        pre.setdefault((kind, mod, sig, tuple(entries)), []).append((key, its, nar, fields))
    buckets = {}
    for (kind, mod, sig, ents), members in pre.items():
        for n, comp in enumerate(_components(members)):
            fl = sorted(set().union(*(m[3] for m in comp)))
            bkey = (kind, mod, sig, ents, n)
            b = buckets.setdefault(bkey, {"kind": kind, "mod": mod,
                                          "signature": (*sig, ("entries", ents), ("fields", tuple(fl))),
                                          "cases": {}, "narrowing": comp[0][2]})
            for key, its, _, _ in comp:
                b["cases"][key] = its
    problems = []
    for (kind, mod, sig, _ents, _n), b in buckets.items():
        all_items = [i for its in b["cases"].values() for i in its]
        entries = sorted({i["entry"] for i in all_items})
        rep = sorted(all_items, key=lambda i: (i["entry"], i.get("case_key") or ""))[0]
        n = len(b["cases"])
        nar = b["narrowing"]
        sev = "low" if kind == "fpu_mode_difference" else "normal"
        fl = list(b["signature"][-1][1])
        about = (" [" + ", ".join(fl[:3]) + (" …" if len(fl) > 3 else "") + "]") if fl and n > 1 else ""
        if kind == "case_fail":
            title = (f"{rep.get('mod')}.{rep.get('case')} fails" if n == 1 else f"{n} {mod} cases fail{about}")
        else:
            opts = ", ".join(sorted({d["option"] for i in all_items for d in i.get("differing_options", [])}))
            title = (f"{rep.get('mod')}.{rep.get('case')} differs between engines" if n == 1 else
                     f"{n} {mod} cases differ between engines") + (f" ({opts})" if opts else "")
        if nar:
            title += f": {nar['statement'].split(';')[0]}"
        elif len(entries) < len([r for r in results if r["status"] not in ("skip",) and not r.get("narrow")]):
            title += f" on {len(entries)} entr{'y' if len(entries) == 1 else 'ies'}"
        also = sorted({fid for i in all_items for fid in folded.get((i["entry"], i.get("case_key")), [])})
        if also:
            title += " (also an engine mismatch vs the reference)"
        problems.append({"kind": kind, "mod": mod, "title": title, "severity": sev,
                         "signature": list(map(str, b["signature"])), "fields": fl,
                         "engine_mismatch_also": also,
                         "entries": entries, "case_keys": sorted(b["cases"]), "cases": n,
                         "failures": [i["id"] for i in all_items], "representative": rep["id"], "narrowing": nar,
                         "narrowing_by_case": {k: _narrow_for(narrowing, rep["entry"], k) for k in sorted(b["cases"])}
                         if narrowing else {}})
    for k, its in others.items():
        entries = sorted({i["entry"] for i in its})
        low = all(i.get("confidence") == "low" for i in its) or k[0] in ("fpu_mode_difference", "config_not_applied")
        if k[0] == "probe_mismatch" and len(k) > 2:
            title = f"probe sees {k[1]}={k[2]} where the config asked otherwise"
        elif k[0] == "config_not_applied":
            title = "Amiberry changed or ignored config options (--dump-config)"
        elif k[0] == "thread_speed":
            insns = sorted({str(i.get("case", "")).replace("speed_", "") for i in its})
            title = (f"threaded CPU behind its unthreaded twin ({', '.join(insns)}) on {len(entries)} "
                     f"entr{'y' if len(entries) == 1 else 'ies'}")
        else:
            title = f"{k[0]} on {len(entries)} entr{'y' if len(entries) == 1 else 'ies'}"
        problems.append({"kind": k[0], "mod": its[0].get("mod"), "title": title, "severity": "low" if low else "normal",
                         "signature": list(map(str, k)), "entries": entries, "case_keys": [], "cases": 0,
                         "failures": [i["id"] for i in its], "representative": its[0]["id"], "narrowing": None,
                         "narrowing_by_case": {}})
    order = {"crash": 0, "hang": 1, "case_fail": 2, "engine_mismatch": 3, "timeout": 4, "incomplete": 5}
    problems.sort(key=lambda p: (p["severity"] == "low", order.get(p["kind"], 9), -len(p["failures"]), p["title"]))
    for n, p in enumerate(problems):
        p["id"] = f"P{n + 1:02d}"
    return problems


# ---------------------------------------------------------------------------
# repro packs

def _write(path, text, exe=False):
    with open(path, "w", encoding="utf-8") as fh:
        fh.write(text)
    if exe:
        os.chmod(path, os.stat(path).st_mode | stat.S_IXUSR | stat.S_IXGRP | stat.S_IXOTH)


def gdb_arch(cpu_model):
    return {"68000": "m68k", "68010": "m68k", "68020": "m68k:68020", "68030": "m68k:68030",
            "68040": "m68k:68040", "68060": "m68k:68060"}.get(str(cpu_model), "m68k")


def _pc_of(item):
    for src in (item.get("ctx"), (item.get("record") or {}).get("ctx"), (item.get("twin") or {}).get("ctx"),
                (item.get("got") if isinstance(item.get("got"), dict) else None)):
        if isinstance(src, dict):
            for k in ("pc", "case_pc", "insn_pc", "fault_pc"):
                v = src.get(k)
                if isinstance(v, str) and re.fullmatch(r"[0-9a-fA-F]{1,8}", v):
                    return int(v, 16)
    return None


def write_pack(outdir, problem, item, entry, *, rom, ext_rom, adf_src, params, module_id, binary, kickstarts,
               amiberry_src, pointers, bake, warp=None):
    """Writes repro/<problem id>/. Returns its info dict (relative paths)."""
    if warp is not None:
        entry = dict(entry, options=dict(entry["options"], warp="true" if warp["effective"] else "false"))
    d = os.path.join(outdir, "repro", problem["id"])
    os.makedirs(d, exist_ok=True)
    info = {"dir": f"repro/{problem['id']}/", "files": []}
    p = dict(params)
    if module_id is not None:
        p["modules"] = f"{1 << module_id:#x}"
    adf = os.path.join(d, "case.adf")
    try:
        bake(adf_src, adf, p)
        info["files"].append("case.adf")
    except Exception as e:  # keep the rest of the pack
        info["adf_error"] = str(e)
    threaded = entry["options"].get("cpu_threaded") == "true"
    gdb_opts = {} if threaded else {"debugging_features": "gdbserver", "gdb_port": str(GDB_PORT)}
    rom_path = rom.get("path") if rom else (entry["rom"].get("builtin") or "")
    _, text = amiberry_cfg.build(entry, rom_path=rom_path, floppy=adf, serial_port=None,
                                 media_dir=os.path.join(d, "media"), ext_rom_path=(ext_rom or {}).get("path"),
                                 extra=dict(gdb_opts, serial_port=f"TCP://127.0.0.1:{REPRO_SERIAL_PORT}"))
    _write(os.path.join(d, "config.uae"), text)
    info["files"].append("config.uae")
    pack = {"problem": problem["id"], "title": problem["title"], "case_key": item.get("case_key"),
            "mod": item.get("mod"), "case": item.get("case"), "kind": item["kind"], "module_id": module_id,
            "params": p, "entry": _jsonable_entry(entry), "rom": rom, "ext_rom": ext_rom, "kickstarts": kickstarts,
            "binary": binary, "amiberry_src": amiberry_src, "expect": item.get("expect"), "got": item.get("got"),
            "ref_entry": item.get("ref_entry"), "ref": item.get("ref") if item["kind"] == "engine_mismatch" else None,
            "gdb_port": None if threaded else GDB_PORT, "warp": warp}
    _write(os.path.join(d, "pack.json"), json.dumps(pack, indent=1))
    info["files"].append("pack.json")
    _write(os.path.join(d, "run.sh"), f"""#!/bin/sh
# Reruns {problem['id']} ({item.get('case_key')}) on entry {entry['id']} and prints the case result line.
# Isolated like a normal run: HOME/XDG/TMPDIR inside ./work, SDL offscreen, serial on a free port.
# Exit status: 0 the case passes (or matches the reference, engine mismatch), 1 it still fails, 2 error.
# Usage: ./run.sh [--amiberry BIN] [--gdb] [--keep]     (default binary: $AMIBERRY or the one recorded)
here=$(cd "$(dirname "$0")" && pwd)
export TMPDIR="$here/work/tmp"; mkdir -p "$TMPDIR"
cd {shlex.quote(RUNNER_DIR)} && exec python3 -m amitorture repro "$here" "$@"
""", exe=True)
    info["files"].append("run.sh")
    pc = _pc_of(item)
    arch = gdb_arch(entry["options"].get("cpu_model"))
    gl = [f"# m68k-amigaos-gdb -x gdb.cmd",
          f"# Guest-side debugging of {item.get('case_key')} through Amiberry's GDB server.",
          "# 1. start the emulator on this pack with the server on:  ./run.sh --gdb   (or amiberry --config config.uae)",
          "#    (config.uae has debugging_features=gdbserver gdb_port=%d; Amiberry refuses it with cpu_threaded)"
          % GDB_PORT,
          f"set architecture {arch}",
          "# big endian: needed with a multiarch host gdb, harmless with m68k-amigaos-gdb",
          "set endian big",
          "set pagination off",
          f"target remote 127.0.0.1:{GDB_PORT}"]
    if pc is not None:
        gl += [f"# the case reported pc={pc:08x} in ctx (absolute: same config + seed load the payload at the same "
               "address)", f"break *{pc:#010x}", "continue", "x/6i $pc", "info registers", "stepi", "info registers",
               "x/6i $pc"]
    else:
        gl += ["# the case did not report a pc in ctx: break on the exception vectors or step from the module",
               "# start. Example: catch the next exception through the VBR table (vector 4 = illegal):",
               "#   x/64wx 0        (vector table at VBR, 0 on a 68000)",
               "continue", "info registers", "x/8i $pc"]
    _write(os.path.join(d, "gdb.cmd"), "\n".join(gl) + "\n")
    info["files"].append("gdb.cmd")
    hl = ["# gdb -x host-gdb.cmd      (run from this directory; host-side debugging of the emulator)",
          f"file {binary}",
          f"set environment HOME={d}/work/home", f"set environment XDG_CONFIG_HOME={d}/work/home/.config",
          f"set environment XDG_DATA_HOME={d}/work/home/.local/share",
          "# XDG_RUNTIME_DIR is short on purpose: AF_UNIX socket paths are limited to 107 bytes "
          "(create it with mode 700 first)",
          f"set environment XDG_RUNTIME_DIR={os.path.join(REPO, 'work', 'tmp', 'hostgdb-run')}",
          f"set environment AMIBERRY_HOME_DIR={d}/work/home/Amiberry", f"set environment TMPDIR={d}/work/tmp",
          "set environment SDL_VIDEO_DRIVER=offscreen", "set environment SDL_VIDEODRIVER=offscreen",
          "set environment SDL_AUDIO_DRIVER=dummy", "set environment SDL_AUDIODRIVER=dummy",
          "# config.uae's serial port is a TCP listener without /wait: connect with e.g. "
          f"`nc 127.0.0.1 {REPRO_SERIAL_PORT}` to see the records",
          f"set args -G --log --config {d}/config.uae",
          "set breakpoint pending on"]
    funcs = []
    for ptr in pointers or []:
        f = ptr.get("function")
        if f and f not in funcs:
            funcs.append(f)
            hl.append(f"# {ptr.get('why')} ({ptr.get('file')}{':' + str(ptr['line']) if ptr.get('line') else ''}, "
                      f"{ptr.get('confidence')})")
            hl.append(f"break {f}")
    if not funcs:
        hl.append("# no specific emulator function suggested for this problem")
    hl += ["run", "bt"]
    _write(os.path.join(d, "host-gdb.cmd"), "\n".join(hl) + "\n")
    info["files"].append("host-gdb.cmd")
    os.makedirs(os.path.join(d, "work", "tmp"), exist_ok=True)
    info["run"] = f"repro/{problem['id']}/run.sh"
    return info


def _jsonable_entry(e):
    out = {}
    for k, v in e.items():
        out[k] = sorted(v) if isinstance(v, set) else v
    return out


def entry_from_pack(pack):
    e = dict(pack["entry"])
    e["matrices"] = set(e.get("matrices") or [])
    e["params"] = dict(pack.get("params") or {})
    return e


def summarize_cfg(entry):
    return matrix.summary_line(entry["options"])


def pointers_for(item, result, src):
    opts = dict(result.get("options") or {})
    opts.update({k: v for k, v in (result.get("resolved_options") or {}).items()})
    return codemap.pointers(item, opts, src)
