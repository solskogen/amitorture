"""Command line: python3 -m amitorture {roms,matrix,run,baseline,check,repro,import} ..."""
from __future__ import annotations

import argparse
import json
import os
import sys

from . import matrix, romid
from .emulator import RunnerError

DEFAULT_WORKDIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "work"))
EXIT_OK, EXIT_PROBLEMS, EXIT_ERROR, EXIT_ISOLATION = 0, 1, 2, 3
EXIT_INTERRUPTED = 130          # 128 + SIGINT, as shells report Ctrl+C


def _install_interrupt_handler():
    """First Ctrl+C (or SIGTERM): stop every emulator, start no new one, keep
    what finished (`run` writes a partial report). Second: kill and quit."""
    import signal
    from . import emulator

    def handler(signum, frame):
        if emulator.INTERRUPT.is_set():
            emulator.interrupt(signal.SIGKILL)
            print("\namitorture: second interrupt: emulators killed, quitting now (temp files under "
                  "WORKDIR/tmp are left behind)", file=sys.stderr, flush=True)
            os._exit(EXIT_INTERRUPTED)
        emulator.interrupt()
        print("\namitorture: interrupted: stopping the emulators and writing what finished "
              "(press Ctrl+C again to quit at once)", file=sys.stderr, flush=True)
        raise KeyboardInterrupt

    signal.signal(signal.SIGINT, handler)
    signal.signal(signal.SIGTERM, handler)


def _setup_tmp(workdir):
    """Our own temp files (mkadf, subprocesses) go under the work dir, never /tmp."""
    t = os.path.join(os.path.abspath(workdir), "tmp")
    os.makedirs(t, exist_ok=True)
    os.environ["TMPDIR"] = t
    import tempfile
    tempfile.tempdir = t


def cmd_roms(a):
    found = romid.scan_dir(a.kickstarts, recursive=a.recursive)
    if a.json:
        print(json.dumps(found, indent=1))
        return EXIT_OK
    if not found:
        print(f"no files in {a.kickstarts}")
        return EXIT_OK
    w = max(len(r["file"]) for r in found)
    for r in found:
        rom = r.get("rom") or {}
        what = rom.get("name") or (f"header {r['header']['version']}" + (" (AROS)" if r["header"].get("aros") else "")
                                   if r.get("header") else "")
        extra = []
        if rom:
            extra.append(f"id {rom['id']}")
            extra.append(rom.get("category", ""))
        if r.get("doubled"):
            extra.append("256K doubled")
        print(f"{r['file']:<{w}}  {r.get('status', '?'):<17} {r.get('crc32') or '':8}  {what}"
              + (f"  [{', '.join(e for e in extra if e)}]" if extra else ""))
    rs = romid.RomSet(found)
    print()
    print(f"matrix {a.matrix}:")
    for e in matrix.expand(a.matrix):
        print(f"  {e['id']:<40} {rom_line(e, rs)}")
    return EXIT_OK


def rom_line(e, rs):
    if "builtin" in e["rom"]:
        return f"uses {e['rom']['builtin']} (built in)"
    r = rs.find(e["rom"]["ids"])
    if not r:
        return f"SKIP: needs {e['rom']['what']}"
    s = f"uses {r['file']} ({r['rom']['name']})"
    if e.get("ext_rom"):
        x = rs.find(e["ext_rom"]["ids"])
        s += f" + {x['file']}" if x else f"; SKIP: needs {e['ext_rom']['what']}"
    for br in e.get("board_roms") or []:
        x = rs.find(br["ids"])
        s += f" + {x['file']}" if x else f"; SKIP: needs {br['what']}"
    return s


def cmd_matrix(a):
    entries = matrix.expand(a.matrix)
    if a.config:
        pool = {e["id"]: e for e in entries}
        for eid, e in matrix.all_entries().items():
            pool.setdefault(eid, e)
        entries = matrix.select(list(pool.values()), a.config)
    if a.validate:
        return validate(a, entries)
    if a.json:
        print(json.dumps(entries, indent=1, default=lambda o: sorted(o) if isinstance(o, set) else str(o)))
        return EXIT_OK
    for e in entries:
        tw = f"  [{e['twin_kind']} twin of {e['twin_of']}]" if e.get("twin_kind") else ""
        print(f"{e['id']:<40} {e['title']}{tw}")
        if a.verbose:
            for k, v in e["options"].items():
                print(f"    {k}={v}")
    print(f"({len(entries)} entries, matrix {a.matrix}; matrices: "
          + ", ".join(f"{n} {len(matrix.expand(n))}" for n in matrix.MATRICES) + ")", file=sys.stderr)
    return EXIT_OK


def validate(a, entries):
    """--dump-config every entry: which options Amiberry changes (no emulation)."""
    from . import amiberry_cfg, emulator
    from .runner import _safe_rmtree
    binary = emulator.find_binary(a.amiberry)
    work = os.path.abspath(a.workdir)
    tmp = os.path.join(work, "tmp", f"validate-{os.getpid()}")
    os.makedirs(tmp, exist_ok=True)
    rs = romid.RomSet(romid.scan_dir(a.kickstarts)) if a.kickstarts else None
    out = []
    try:
        env = emulator.isolated_env(os.path.join(tmp, "home"))
        for e in entries:
            rom = ""
            if rs is not None and "ids" in e["rom"]:
                r = rs.find(e["rom"]["ids"])
                rom = r["path"] if r else ""
            elif "builtin" in e["rom"]:
                rom = e["rom"]["builtin"]
            _, text = amiberry_cfg.build(e, rom_path=rom or "", floppy="", serial_port=None, media_dir=tmp)
            p = os.path.join(tmp, "v.uae")
            with open(p, "w", encoding="utf-8") as fh:
                fh.write(text)
            v = amiberry_cfg.verify(binary, p, env)
            d = list(v.get("differences", []))
            out.append({"id": e["id"], "adjusted": d, "unknown": v.get("unknown_options"),
                        "messages": v.get("fixup_messages")})
            if d or v.get("unknown_options"):
                print(f"{e['id']}: ADJUSTED " + "; ".join(f"{x['option']} {x['wrote']}->{x['resolved']}"
                                                         + (f" ({' / '.join(x['because'])})" if x['because'] else "")
                                                         for x in d)
                      + (f" unknown: {v['unknown_options']}" if v.get("unknown_options") else ""))
            elif a.verbose:
                print(f"{e['id']}: ok")
    finally:
        if not a.keep:
            _safe_rmtree(tmp, os.path.join(work, "tmp"))
    n = sum(1 for x in out if x["adjusted"] or x["unknown"])
    print(f"{len(out)} entries validated, {n} adjusted by the emulator")
    if a.json:
        print(json.dumps(out, indent=1))
    return EXIT_OK


def parse_params(a):
    p = {"mode": a.mode, "seed": str(a.seed), "end_action": a.end_action}
    for item in a.param or []:
        if "=" not in item:
            raise RunnerError(f"--param wants key=value, got {item!r}")
        k, v = item.split("=", 1)
        p[k] = v
    return p


def parse_sets(a):
    out = {}
    for item in getattr(a, "set", None) or []:
        if "=" not in item:
            raise RunnerError(f"--set wants option=value, got {item!r}")
        k, v = item.split("=", 1)
        out[k.strip()] = v.strip()
    return out


def make_options(a, binary):
    from . import runner
    if not a.kickstarts or not os.path.isdir(a.kickstarts):
        raise RunnerError(f"--kickstarts {a.kickstarts}: not a directory")
    return runner.Options(
        binary=binary, kickstarts=a.kickstarts, workdir=a.workdir, configs=a.config, matrix=a.matrix,
        sets=parse_sets(a), timeout=a.timeout, hang=a.hang, adf=a.adf, disk_set=getattr(a, "disk_set", None),
        no_disk=getattr(a, "no_disk", False) or getattr(a, "selftest", False), selftest=getattr(a, "selftest", False),
        params=parse_params(a), verbose=not a.quiet, keep=a.keep, jobs=a.jobs,
        narrow=not getattr(a, "no_narrow", False), narrow_budget=getattr(a, "narrow_budget", 0.0),
        narrow_reruns=getattr(a, "narrow_reruns", 3), narrow_binaries=getattr(a, "narrow_binary", None),
        baseline=getattr(a, "baseline", None), amiberry_src=a.amiberry_src, repro=not getattr(a, "no_repro", False),
        xfail_path=a.xfail, audio=getattr(a, "audio", "auto"), ports=parse_ports(getattr(a, "ports", None)),
        show=_check_show(a))


def _check_show(a):
    """--show: find the desktop session now, not at the first launch."""
    if not getattr(a, "show", False):
        return False
    from . import emulator
    emulator.display_env()
    return True


def parse_ports(spec):
    """--ports LO-HI (or $AMT_PORTS): the serial port range for emulator launches."""
    spec = spec or os.environ.get("AMT_PORTS")
    if not spec:
        return None
    try:
        lo, hi = (int(x) for x in spec.split("-", 1))
    except ValueError:
        raise RunnerError(f"--ports wants LO-HI, got {spec!r}")
    if not 1024 <= lo <= hi <= 65535:
        raise RunnerError(f"--ports {spec}: not a valid range")
    return lo, hi


def cmd_run(a):
    from . import emulator, runner
    opts = make_options(a, emulator.find_binary(a.amiberry))
    outdir, summ = runner.run(opts)
    print(f"{summ['verdict']}: {os.path.join(outdir, 'REPORT.md')}")
    if summ.get("interrupted"):
        return EXIT_INTERRUPTED
    if not summ["isolation"]["ok"]:
        return EXIT_ISOLATION
    return EXIT_OK if summ["verdict"].startswith("PASS") else EXIT_PROBLEMS


def cmd_baseline(a):
    from . import emulator, regress
    opts = make_options(a, emulator.find_binary(a.amiberry))
    path, doc = regress.cmd_baseline(opts, a.name, a.repeat)
    m = doc["meta"]
    print(f"baseline {a.name}: {len(doc['entries'])} entries x {a.repeat} runs, {m['unstable_cases']} unstable "
          f"case(s): {path} (summary: {os.path.join(os.path.dirname(path), 'BASELINE.md')})")
    return EXIT_OK


def cmd_check(a):
    from . import baseline as B
    from . import emulator, regress
    if a.rediff:
        doc = regress.rediff(os.path.abspath(a.rediff), a.workdir)
        v = regress.check_verdict(doc)
        print(f"{v}: {os.path.join(os.path.abspath(a.rediff), 'REPORT.md')}")
        return EXIT_OK if v.startswith("NO REGRESSIONS") else EXIT_PROBLEMS
    if not a.amiberry or not a.baseline:
        raise RunnerError("check needs --amiberry BIN... and --baseline NAME (or --rediff CHECK_DIR)")
    bins = [emulator.find_binary(b) for b in a.amiberry]
    base = B.load(a.workdir, a.baseline)
    if base is None:
        raise RunnerError(f"no baseline {a.baseline!r} in {B.baseline_dir(a.workdir, a.baseline)}")
    meta = base["meta"]
    if not a.kickstarts:
        a.kickstarts = meta.get("kickstarts")
    md = meta.get("disk") or {}
    if a.adf is None and a.disk_set is None and md.get("path"):
        if md.get("set") and os.path.isdir(md["path"]):
            a.disk_set = md["path"]          # same disk set as the baseline
        elif os.path.isfile(md["path"]):
            a.adf = md["path"]
    # same run parameters as the baseline unless given explicitly
    bp = meta.get("params") or {}
    if a.mode is None:
        a.mode = bp.get("mode", "full")
    if a.seed is None:
        a.seed = int(bp.get("seed", 1))
    if a.end_action is None:
        a.end_action = bp.get("end_action", "halt")
    if not a.param:
        # the baseline's other --param values too (e.g. flags=noscreen changes
        # the DMA state every module starts with)
        a.param = [f"{k}={v}" for k, v in bp.items() if k not in ("mode", "seed", "end_action")]
    opts = make_options(a, bins[0])
    outdir, doc = regress.cmd_check(opts, a.baseline, bins, a.repeat)
    v = regress.check_verdict(doc)
    print(f"{v}: {os.path.join(outdir, 'REPORT.md')}")
    return EXIT_OK if v.startswith("NO REGRESSIONS") else EXIT_PROBLEMS


def cmd_repro(a):
    from . import regress
    _setup_tmp(os.path.join(os.path.abspath(a.pack), "work"))
    return regress.cmd_repro(a.pack, binary=a.amiberry, gdb=a.gdb, keep=a.keep, timeout=a.timeout, verbose=a.verbose,
                             show=_check_show(a))


def cmd_report(a):
    from . import analysis
    summ = analysis.rebuild(os.path.abspath(a.results), repro=not a.no_repro, xfail_path=a.xfail)
    print(f"{summ['verdict']}: {os.path.join(os.path.abspath(a.results), 'REPORT.md')}")
    return EXIT_OK if summ["verdict"].startswith("PASS") else EXIT_PROBLEMS


def cmd_import(a):
    from . import report, runner
    label = a.label or ("import-" + runner.new_run_id())
    outdir = os.path.abspath(a.out or os.path.join(a.workdir, "results", label))
    summ = report.import_files(a.files, outdir, label)
    print(f"{summ['verdict']}: {os.path.join(outdir, 'REPORT.md')}")
    return EXIT_OK if summ["verdict"].startswith("PASS") else EXIT_PROBLEMS


def _common(p, kick_required=True, param_defaults=True):
    p.add_argument("--kickstarts", required=kick_required, metavar="DIR", help="ROM directory (read only)")
    p.add_argument("--workdir", default=DEFAULT_WORKDIR, metavar="DIR", help=f"default {DEFAULT_WORKDIR}")
    p.add_argument("--matrix", default="smoke", choices=matrix.MATRICES, help="named matrix (default smoke)")
    p.add_argument("--config", action="append", metavar="NAME", help="entry id or glob, any matrix (repeatable)")
    p.add_argument("--set", action="append", metavar="OPTION=VALUE",
                   help="override an Amiberry option on every selected entry (repeatable)")
    p.add_argument("--timeout", type=float, default=600.0, metavar="S",
                   help="per disk launch (default 600: the slowest launch, the core disk of a cycle-exact A500 at "
                        "floppy_speed=100, took 243 s; the CDTV core disk 225 s)")
    p.add_argument("--hang", type=float, default=120.0, metavar="S",
                   help="heartbeat unchanged this long = hang (default 120: OS-phase modules doing floppy I/O "
                        "do not tick the heartbeat)")
    p.add_argument("--disk-set", metavar="DIR",
                   help="disk set: every *.adf in DIR runs per entry, in name order (default disk/build/set if "
                        "it exists and --adf is not given)")
    p.add_argument("--adf", metavar="PATH", help="a single test disk instead of the set "
                                                 "(default disk/build/amitorture.adf when there is no set)")
    p.add_argument("--mode", default="full" if param_defaults else None, choices=["full", "quick", "soak"])
    p.add_argument("--seed", type=int, default=1 if param_defaults else None)
    p.add_argument("--end-action", default="halt" if param_defaults else None, choices=["halt", "reboot", "return"])
    p.add_argument("--param", action="append", metavar="KEY=VALUE", help="extra mkadf.py --param")
    p.add_argument("--jobs", type=int, default=1, metavar="N", help="entries run in parallel (default 1)")
    p.add_argument("--keep", action="store_true", help="keep the per-run temp dir under WORKDIR/tmp")
    p.add_argument("--show", action="store_true",
                   help="show the emulator windows on your desktop (one per running emulator, --jobs N gives N); "
                        "isolation is kept, timing values may differ from a headless run")
    p.add_argument("--amiberry-src", metavar="DIR", help="Amiberry src/ for code pointers (default: next to the binary)")
    p.add_argument("--xfail", metavar="FILE", help="xfail registry (default runner/amitorture/xfail.json)")
    p.add_argument("--audio", default="auto", choices=["auto", "on", "off"],
                   help="capture the emulator's audio with SDL's disk driver (auto: only disks that send "
                        "audio_mark requests)")
    p.add_argument("--ports", metavar="LO-HI",
                   help="serial port range for the emulators (default $AMT_PORTS or 23540-23599)")
    p.add_argument("-q", "--quiet", action="store_true")


def build_parser():
    ap = argparse.ArgumentParser(prog="amitorture", description="Amiga emulator torture-test runner")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("roms", help="identify ROM files by CRC32/SHA1 and show which matrix entries can run")
    p.add_argument("--kickstarts", required=True, metavar="DIR")
    p.add_argument("--matrix", default="smoke", choices=matrix.MATRICES)
    p.add_argument("--recursive", action="store_true")
    p.add_argument("--json", action="store_true")
    p.set_defaults(fn=cmd_roms)

    p = sub.add_parser("matrix", help="list a matrix; --validate checks every config with --dump-config")
    p.add_argument("--matrix", default="smoke", choices=matrix.MATRICES)
    p.add_argument("--config", action="append", metavar="NAME")
    p.add_argument("--validate", action="store_true", help="run amiberry --dump-config on every entry")
    p.add_argument("--amiberry", metavar="PATH")
    p.add_argument("--kickstarts", metavar="DIR")
    p.add_argument("--workdir", default=DEFAULT_WORKDIR, metavar="DIR")
    p.add_argument("--keep", action="store_true")
    p.add_argument("--json", action="store_true")
    p.add_argument("-v", "--verbose", action="store_true")
    p.set_defaults(fn=cmd_matrix)

    p = sub.add_parser("run", help="run the test disk on every entry, then parity, narrowing, report")
    p.add_argument("--amiberry", metavar="PATH", help="Amiberry binary (default: 'amiberry' on PATH; never built)")
    _common(p)
    p.add_argument("--no-disk", action="store_true", help="no disk in DF0: checks launch/IPC/serial/scan only")
    p.add_argument("--selftest", action="store_true",
                   help="--no-disk plus a synthetic mailbox written over IPC to check scan + ring reading")
    p.add_argument("--no-narrow", action="store_true", help="do not rerun failures on neighbouring configs")
    p.add_argument("--narrow-budget", type=float, default=7200.0, metavar="S",
                   help="wall-time cap for all narrowing launches (default 7200; 0 = off)")
    p.add_argument("--narrow-reruns", type=int, default=3, metavar="N",
                   help="reruns of each failing config for the deterministic/intermittent label (default 3)")
    p.add_argument("--narrow-binary", action="append", metavar="PATH",
                   help="other Amiberry binary to try a failure on (repeatable)")
    p.add_argument("--baseline", metavar="NAME", help="annotate problems with this baseline's history")
    p.add_argument("--no-repro", action="store_true", help="do not write repro packs")
    p.set_defaults(fn=cmd_run)

    p = sub.add_parser("baseline", help="run each entry N times and store fingerprints, timing and perf bands")
    p.add_argument("--amiberry", metavar="PATH")
    _common(p)
    p.add_argument("--name", required=True, metavar="BASELINE")
    p.add_argument("--repeat", type=int, default=3, metavar="N", help="runs per entry (default 3)")
    p.set_defaults(fn=cmd_baseline)

    p = sub.add_parser("check", help="rerun a baseline's entries on one or more binaries and diff fingerprints")
    p.add_argument("--amiberry", nargs="+", metavar="BIN",
                   help="binaries, in order (the first one where each fingerprint changed is reported)")
    p.add_argument("--baseline", metavar="NAME")
    p.add_argument("--rediff", metavar="CHECK_DIR",
                   help="recompute an earlier check's diff from its saved runs (no emulator)")
    _common(p, kick_required=False, param_defaults=False)
    p.add_argument("--repeat", type=int, default=1, metavar="N",
                   help="runs per entry and binary (>1 also detects cases that became unstable)")
    p.set_defaults(fn=cmd_check)

    p = sub.add_parser("repro", help="rerun a repro pack (repro/<problem>/) and print the case result")
    p.add_argument("pack", metavar="PACKDIR")
    p.add_argument("--amiberry", metavar="PATH", help="binary (default $AMIBERRY or the one recorded in pack.json)")
    p.add_argument("--gdb", action="store_true", help="enable Amiberry's GDB server (see gdb.cmd)")
    p.add_argument("--show", action="store_true", help="show the emulator window on your desktop")
    p.add_argument("--keep", action="store_true")
    p.add_argument("--timeout", type=float, default=300.0)
    p.add_argument("-v", "--verbose", action="store_true")
    p.set_defaults(fn=cmd_repro)

    p = sub.add_parser("report", help="rebuild REPORT.md (parity, xfail, problems, pointers, repro packs) of a "
                                      "results dir from its saved files, without the emulator")
    p.add_argument("results", metavar="RESULTS_DIR")
    p.add_argument("--no-repro", action="store_true", help="keep the existing repro packs untouched")
    p.add_argument("--xfail", metavar="FILE", help="xfail registry (default runner/amitorture/xfail.json)")
    p.add_argument("--workdir", default=DEFAULT_WORKDIR, metavar="DIR")
    p.set_defaults(fn=cmd_report)

    p = sub.add_parser("import", help="build a report from serial captures / mailbox dumps made elsewhere")
    p.add_argument("files", nargs="+")
    p.add_argument("--out", metavar="DIR", help="results dir (default WORKDIR/results/<label>)")
    p.add_argument("--workdir", default=DEFAULT_WORKDIR, metavar="DIR")
    p.add_argument("--label")
    p.set_defaults(fn=cmd_import)
    return ap


def main(argv=None):
    from .emulator import Interrupted
    a = build_parser().parse_args(argv)
    _install_interrupt_handler()
    try:
        if a.cmd != "repro" and getattr(a, "workdir", None):
            _setup_tmp(a.workdir)
        return a.fn(a)
    except RunnerError as e:
        print(f"amitorture: error: {e}", file=sys.stderr)
        return EXIT_ERROR
    except (KeyboardInterrupt, Interrupted):
        from . import emulator
        emulator.interrupt()
        emulator.stop_all()
        print("amitorture: interrupted; every emulator it started has been stopped"
              + ("; no baseline was written" if a.cmd == "baseline" else ""), file=sys.stderr)
        return EXIT_INTERRUPTED
