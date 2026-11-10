"""Command line: python3 -m amitorture {roms,matrix,run,baseline,check,compare,repro,import} ..."""
from __future__ import annotations

import argparse
import json
import os
import sys

from . import matrix, romid
from .emulator import RunnerError

CHECKOUT_WORKDIR = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", "..", "work"))
# $AMT_WORKDIR, else work/ in the checkout the runner runs from
DEFAULT_WORKDIR = os.path.abspath(os.path.expanduser(os.environ.get("AMT_WORKDIR") or CHECKOUT_WORKDIR))
WORKDIR_HELP = ("where everything amitorture writes goes: results/, baselines/, tmp/ (default: $AMT_WORKDIR, "
                f"else work/ in this checkout, now {DEFAULT_WORKDIR})")


def _default_kickstarts():
    """$AMT_KICKSTARTS, else Amiberry's usual ROM directory ~/Amiberry/ROMs when it exists (read only)."""
    env = os.environ.get("AMT_KICKSTARTS")
    if env:
        return os.path.abspath(os.path.expanduser(env))
    d = os.path.expanduser("~/Amiberry/ROMs")
    return d if os.path.isdir(d) else None


DEFAULT_KICKSTARTS = _default_kickstarts()
KICK_HELP = ("directory of Kickstart and board ROM files, read only; files are identified by checksum, not name "
             f"(default: $AMT_KICKSTARTS, else ~/Amiberry/ROMs if it exists{', now ' + DEFAULT_KICKSTARTS if DEFAULT_KICKSTARTS else ': not found'})")
MODE_HELP = ("how much each module does (default full): full = every case with all its inputs; quick = the same "
             "case ids with fewer inputs and iterations, faster but less thorough (meant for developing the disks). "
             "A baseline and its checks must use the same mode; check takes the baseline's")
SEED_HELP = ("seed of the random inputs the modules draw (CIA timer values, display colours and probe "
             "positions, devio data patterns, ...); the same seed gives the same records, so baselines and "
             "checks must use the same one (check takes the baseline's). Default 1")
END_HELP = ("what the disk does after its last module (default halt: the machine stays on the final status "
            "screen; the runner stops the emulator itself). reboot = ColdReboot; return = back to the "
            "Kickstart boot code")
PARAM_HELP = ("an extra run parameter baked into the disks (repeatable): "
              "flags=unsafe,noserial,nombox,net,noscreen,intcompat,fpucompat "
              "(comma-separated; intcompat=32, fpucompat=64 follow configured instruction policies, "
              "strict options clear stale compatibility flags; JIT uses both compatibility policies), "
              "baud=N (serial rate, default 115200), modules=MASK (bit n = module id n, "
              "docs/CONTRACT.md section 10; e.g. modules=0x1000 runs only display)")
REPRO_HELP = ("do not write repro packs. A repro pack is repro/<problem>/ in the results: a one-module disk with "
              "the case's parameters, the minimal Amiberry config, run.sh and gdb command files, so "
              "`amitorture repro <pack>` reruns just that failure in seconds (e.g. after a fix in Amiberry)")
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
    if not a.kickstarts:
        raise RunnerError("no ROM directory: pass --kickstarts DIR or set AMT_KICKSTARTS (~/Amiberry/ROMs does not exist)")
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
            error = amiberry_cfg.verification_error(v)
            out.append({"id": e["id"], "adjusted": d, "unknown": v.get("unknown_options"),
                        "messages": v.get("fixup_messages"), "ok": v.get("ok"), "error": error,
                        "returncode": v.get("returncode"), "stderr": v.get("stderr")})
            if error:
                print(f"{e['id']}: FAILED {error}")
            elif d:
                print(f"{e['id']}: ADJUSTED " + "; ".join(f"{x['option']} {x['wrote']}->{x['resolved']}"
                                                         + (f" ({' / '.join(x['because'])})" if x['because'] else "")
                                                         for x in d)
                      + (f" unknown: {v['unknown_options']}" if v.get("unknown_options") else ""))
            elif a.verbose:
                print(f"{e['id']}: ok")
    finally:
        if not a.keep:
            _safe_rmtree(tmp, os.path.join(work, "tmp"))
    failed = sum(1 for x in out if not x["ok"])
    n = sum(1 for x in out if x["ok"] and x["adjusted"])
    print(f"{len(out) - failed} entries validated, {n} adjusted by the emulator"
          + (f", {failed} verification(s) failed" if failed else ""))
    if a.json:
        print(json.dumps(out, indent=1))
    return EXIT_ERROR if failed else EXIT_OK


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
    if not a.kickstarts:
        raise RunnerError("no ROM directory: pass --kickstarts DIR or set AMT_KICKSTARTS (~/Amiberry/ROMs does not exist)")
    if not os.path.isdir(a.kickstarts):
        raise RunnerError(f"--kickstarts {a.kickstarts}: not a directory")
    return runner.Options(
        binary=binary, kickstarts=a.kickstarts, workdir=a.workdir, configs=a.config, matrix=a.matrix,
        sets=parse_sets(a), timeout=a.timeout, hang=a.hang, resets=max(0, getattr(a, "resets", 0) or 0), adf=a.adf, disk_set=getattr(a, "disk_set", None),
        no_disk=getattr(a, "no_disk", False) or getattr(a, "selftest", False), selftest=getattr(a, "selftest", False),
        params=parse_params(a), verbose=not a.quiet, keep=a.keep, jobs=a.jobs,
        narrow=not getattr(a, "no_narrow", False), narrow_budget=getattr(a, "narrow_budget", 0.0),
        narrow_reruns=getattr(a, "narrow_reruns", 3), narrow_binaries=getattr(a, "narrow_binary", None),
        baseline=getattr(a, "baseline", None), amiberry_src=a.amiberry_src, repro=not getattr(a, "no_repro", False),
        xfail_path=a.xfail, audio=getattr(a, "audio", "auto"), warp=getattr(a, "warp", None),
        ports=parse_ports(getattr(a, "ports", None)),
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
        a.kickstarts = meta.get("kickstarts") or DEFAULT_KICKSTARTS
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


def cmd_compare(a):
    from . import compare
    return compare.run(a)


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
    if kick_required:
        p.add_argument("--kickstarts", default=DEFAULT_KICKSTARTS, metavar="DIR", help=KICK_HELP)
    else:   # check: the baseline's ROM directory first
        p.add_argument("--kickstarts", metavar="DIR", help="ROM directory, read only (default: the baseline's, "
                                                            "else $AMT_KICKSTARTS or ~/Amiberry/ROMs)")
    p.add_argument("--workdir", default=DEFAULT_WORKDIR, metavar="DIR", help=WORKDIR_HELP)
    p.add_argument("--matrix", default="smoke", choices=matrix.MATRICES,
                   help="which machines to run (default smoke): smoke = 15 machines, covering CPU classes, chipsets "
                        "and strict instruction policies; standard = 145, every CPU/FPU variant, board and controller; "
                        "full = 543, also every JIT "
                        "sub-option, threaded and softfloat twin; benchmarks = 19, CPU-family benchmarks with "
                        "JIT and threaded off/on pairs. `amitorture matrix --matrix NAME` lists them")
    p.add_argument("--config", action="append", metavar="NAME",
                   help="run only these entries: an entry id or a glob such as 'a1200*', from any matrix "
                        "(repeatable; replaces --matrix)")
    p.add_argument("--set", action="append", metavar="OPTION=VALUE",
                   help="override an Amiberry config option on every selected entry, e.g. --set cpu_threaded=true "
                        "(repeatable)")
    p.add_argument("--timeout", type=float, default=900.0, metavar="S",
                   help="per disk launch (default 900: the slowest launch, the core disk of a cycle-exact A500 at "
                        "floppy_speed=100, took 441 s)")
    p.add_argument("--resets", type=int, default=0, metavar="N",
                   help="after the guest's done record, reset the emulator over IPC (RESET, a keyboard reset) and "
                        "run the disk again, N times per launch: catches state that only a reset sets up (e.g. "
                        "the JIT's opcode table). Each boot gets the full --timeout; every boot's cases count "
                        "towards the verdict, the case tables show the last boot (default 0)")
    p.add_argument("--hang", type=float, default=120.0, metavar="S",
                   help="heartbeat unchanged this long = hang (default 120: OS-phase modules doing floppy I/O "
                        "do not tick the heartbeat)")
    p.add_argument("--disk-set", metavar="DIR",
                   help="disk set: every *.adf in DIR runs per entry, in name order (default disk/build/set if "
                        "it exists and --adf is not given)")
    p.add_argument("--adf", metavar="PATH", help="a single test disk instead of the set "
                                                 "(default disk/build/amitorture.adf when there is no set; "
                                                 "matrix benchmarks uses disk/build/benchmarks/amitorture.adf)")
    p.add_argument("--mode", default="full" if param_defaults else None, choices=["full", "quick"], help=MODE_HELP)
    p.add_argument("--seed", type=int, default=1 if param_defaults else None, help=SEED_HELP)
    p.add_argument("--end-action", default="halt" if param_defaults else None, choices=["halt", "reboot", "return"],
                   help=END_HELP)
    p.add_argument("--param", action="append", metavar="KEY=VALUE", help=PARAM_HELP)
    p.add_argument("--jobs", type=int, default=1, metavar="N",
                   help="entries run in parallel (default 1), capped at the machine-wide emulator limit "
                        "(nproc, or $AMT_MAX_EMU)")
    p.add_argument("--keep", action="store_true",
                   help="keep the per-run temp dir under WORKDIR/tmp (emulator homes, generated configs and media) "
                        "for debugging; normally removed at the end")
    p.add_argument("--show", action="store_true",
                   help="show the emulator windows on your desktop (one per running emulator, --jobs N gives N); "
                        "isolation is kept, timing values may differ from a headless run")
    p.add_argument("--amiberry-src", metavar="DIR",
                   help="Amiberry's source tree, so the report can name the emulator files and functions behind a "
                        "failure (default: found next to the binary, or $AMIBERRY_SRC)")
    p.add_argument("--xfail", metavar="FILE",
                   help="registry of known emulator bugs: matching failures show as xfail, not as new problems "
                        "(default runner/amitorture/xfail.json)")
    p.add_argument("--audio", default="auto", choices=["auto", "on", "off"],
                   help="capture the emulator's audio with SDL's disk driver (auto: only disks that send "
                        "audio_mark requests)")
    p.add_argument("--warp", choices=["off", "auto", "on"],
                   help="off: normal pacing (default); auto: warp only CPU/FPU/MMU set disks, with normal "
                        "pacing for all other disks and audio captures; on: force warp everywhere "
                        "(experimental, invalidates audio/input/RTC/hostspeed tests). check inherits the "
                        "baseline's policy unless specified. Cannot combine with --set warp=...")
    p.add_argument("--ports", metavar="LO-HI",
                   help="serial port range for the emulators (default $AMT_PORTS or 23540-23599)")
    p.add_argument("-q", "--quiet", action="store_true", help="no progress lines, only the final verdict")


def build_parser():
    ap = argparse.ArgumentParser(prog="amitorture", description="Amiga emulator torture-test runner")
    sub = ap.add_subparsers(dest="cmd", required=True)

    p = sub.add_parser("roms", help="identify ROM files by CRC32/SHA1 and show which matrix entries can run")
    p.add_argument("--kickstarts", default=DEFAULT_KICKSTARTS, metavar="DIR", help=KICK_HELP)
    p.add_argument("--matrix", default="smoke", choices=matrix.MATRICES,
                   help="show which ROM each entry of this matrix would use (default smoke)")
    p.add_argument("--recursive", action="store_true", help="also scan subdirectories")
    p.add_argument("--json", action="store_true", help="machine-readable output")
    p.set_defaults(fn=cmd_roms)

    p = sub.add_parser("matrix", help="list a matrix; --validate checks every config with --dump-config")
    p.add_argument("--matrix", default="smoke", choices=matrix.MATRICES, help="which matrix to list (default smoke)")
    p.add_argument("--config", action="append", metavar="NAME", help="only entries matching this id or glob (repeatable)")
    p.add_argument("--validate", action="store_true",
                   help="write each entry's config and run `amiberry --dump-config` on it: shows options Amiberry "
                        "changes or ignores (needs --amiberry and a ROM directory)")
    p.add_argument("--amiberry", metavar="PATH", help="Amiberry binary for --validate (default: 'amiberry' on PATH)")
    p.add_argument("--kickstarts", default=DEFAULT_KICKSTARTS, metavar="DIR", help=KICK_HELP)
    p.add_argument("--workdir", default=DEFAULT_WORKDIR, metavar="DIR", help=WORKDIR_HELP)
    p.add_argument("--keep", action="store_true", help="keep the generated configs under WORKDIR/tmp")
    p.add_argument("--json", action="store_true", help="machine-readable output")
    p.add_argument("-v", "--verbose", action="store_true", help="every entry's options, or every validation result")
    p.set_defaults(fn=cmd_matrix)

    p = sub.add_parser("run", help="run the test disk on every entry, then parity, narrowing, report")
    p.add_argument("--amiberry", metavar="PATH", help="Amiberry binary (default: 'amiberry' on PATH; never built)")
    _common(p)
    p.add_argument("--no-disk", action="store_true",
                   help="boot without a test disk: only checks that Amiberry starts isolated, and its IPC socket, "
                        "serial port and memory reads work")
    p.add_argument("--selftest", action="store_true",
                   help="--no-disk plus a synthetic mailbox written over IPC, to check the runner reads the "
                        "guest's results correctly (about 30 s)")
    p.add_argument("--no-narrow", action="store_true",
                   help="skip narrowing. Narrowing reruns each failing module on neighbouring configs (JIT on/off, "
                        "each comp_* option, core mode, FPU mode, ...) to find the setting that flips the result")
    p.add_argument("--narrow-budget", type=float, default=7200.0, metavar="S",
                   help="wall-time cap for all narrowing reruns together (default 7200 s; 0 = no narrowing)")
    p.add_argument("--narrow-reruns", type=int, default=3, metavar="N",
                   help="reruns of each failing config, to tell a deterministic failure from an intermittent one "
                        "(default 3)")
    p.add_argument("--narrow-binary", action="append", metavar="PATH",
                   help="another Amiberry binary to try each failure on during narrowing, e.g. an older build "
                        "(repeatable)")
    p.add_argument("--baseline", metavar="NAME",
                   help="say in the report, per problem, whether the case passed in this baseline (no diff; "
                        "use `check` for that)")
    p.add_argument("--no-repro", action="store_true", help=REPRO_HELP)
    p.set_defaults(fn=cmd_run)

    p = sub.add_parser("baseline", help="run each entry N times and store fingerprints, timing and perf bands")
    p.add_argument("--amiberry", metavar="PATH", help="the Amiberry build you trust (default: 'amiberry' on PATH)")
    _common(p)
    p.add_argument("--name", required=True, metavar="BASELINE",
                   help="stored as WORKDIR/baselines/BASELINE; an existing one is replaced only when the new one "
                        "is complete")
    p.add_argument("--repeat", type=int, default=3, metavar="N",
                   help="runs per entry (default 3): cases that differ between identical runs are marked "
                        "unstable and not compared by check")
    p.set_defaults(fn=cmd_baseline)

    p = sub.add_parser("check", help="rerun a baseline's entries on one or more binaries and diff fingerprints")
    p.add_argument("--amiberry", nargs="+", metavar="BIN",
                   help="binaries, in order (the first one where each fingerprint changed is reported)")
    p.add_argument("--baseline", metavar="NAME", help="the baseline to compare with (WORKDIR/baselines/NAME)")
    p.add_argument("--rediff", metavar="CHECK_DIR",
                   help="recompute an earlier check's diff from its saved runs (no emulator)")
    _common(p, kick_required=False, param_defaults=False)
    p.add_argument("--repeat", type=int, default=1, metavar="N",
                   help="runs per entry and binary (>1 also detects cases that became unstable)")
    p.set_defaults(fn=cmd_check)

    p = sub.add_parser("compare", help="A/B: the hostspeed benchmarks of two binaries, alternating, repeated; "
                                       "per block slower/faster/borderline/same")
    p.add_argument("--amiberry", action="append", metavar="PATH", required=True,
                   help="give it twice: binary A (the reference), then binary B")
    p.add_argument("--label", action="append", metavar="NAME",
                   help="names for A and B in the report, in the same order (default A, B)")
    p.add_argument("--kickstarts", default=DEFAULT_KICKSTARTS, metavar="DIR", help=KICK_HELP)
    p.add_argument("--matrix", default="benchmarks", choices=matrix.MATRICES,
                   help="matrix of the entries (default benchmarks)")
    p.add_argument("--config", action="append", metavar="NAME",
                   help="only entries matching this id or glob (repeatable; default every entry)")
    p.add_argument("--set", action="append", metavar="OPTION=VALUE",
                   help="Amiberry option for every entry on both binaries (repeatable), e.g. cpu_threaded=true")
    p.add_argument("--adf", metavar="PATH",
                   help="test disk (default disk/build/benchmarks/amitorture.adf for matrix benchmarks)")
    p.add_argument("--repeat", type=int, default=3, metavar="N",
                   help="launches per binary (default 3); the order alternates: A B, B A, ...")
    p.add_argument("--threshold", type=float, default=2.0, metavar="PCT",
                   help="smallest change of the median that counts (default 2 percent)")
    p.add_argument("--pin", metavar="CPUS",
                   help="run the runner and every emulator on these host CPUs, e.g. 2-5 (sched_setaffinity)")
    p.add_argument("--workdir", default=DEFAULT_WORKDIR, metavar="DIR", help=WORKDIR_HELP)
    p.add_argument("-q", "--quiet", action="store_true", help="no progress lines, only the final verdict")
    p.set_defaults(fn=cmd_compare)

    p = sub.add_parser("repro", help="rerun a repro pack (repro/<problem>/) and print the case result")
    p.add_argument("pack", metavar="PACKDIR")
    p.add_argument("--amiberry", metavar="PATH", help="binary (default $AMIBERRY or the one recorded in pack.json)")
    p.add_argument("--gdb", action="store_true", help="enable Amiberry's GDB server (see gdb.cmd)")
    p.add_argument("--show", action="store_true", help="show the emulator window on your desktop")
    p.add_argument("--keep", action="store_true", help="keep the pack's work/ dir (emulator home, log)")
    p.add_argument("--timeout", type=float, default=300.0, metavar="S", help="limit for the launch (default 300 s)")
    p.add_argument("-v", "--verbose", action="store_true", help="progress lines and the emulator log path")
    p.set_defaults(fn=cmd_repro)

    p = sub.add_parser("report", help="rebuild REPORT.md (parity, xfail, problems, pointers, repro packs) of a "
                                      "results dir from its saved files, without the emulator")
    p.add_argument("results", metavar="RESULTS_DIR", help="a results dir written by run (work/results/<run-id>)")
    p.add_argument("--no-repro", action="store_true", help="keep the existing repro packs untouched")
    p.add_argument("--xfail", metavar="FILE",
                   help="registry of known emulator bugs (default runner/amitorture/xfail.json)")
    p.add_argument("--workdir", default=DEFAULT_WORKDIR, metavar="DIR", help=WORKDIR_HELP)
    p.set_defaults(fn=cmd_report)

    p = sub.add_parser("import", help="build a report from serial captures / mailbox dumps made elsewhere")
    p.add_argument("files", nargs="+",
                   help="serial captures (one per disk, or one long capture across all disks) and/or mailbox "
                        "dumps, all from one machine")
    p.add_argument("--out", metavar="DIR", help="results dir (default WORKDIR/results/<label>)")
    p.add_argument("--workdir", default=DEFAULT_WORKDIR, metavar="DIR", help=WORKDIR_HELP)
    p.add_argument("--label", help="name of the results dir (default import-<time>)")
    p.set_defaults(fn=cmd_import)
    return ap


def main(argv=None):
    from .emulator import Interrupted, IsolationError
    a = build_parser().parse_args(argv)
    _install_interrupt_handler()
    try:
        if a.cmd != "repro" and getattr(a, "workdir", None):
            _setup_tmp(a.workdir)
        return a.fn(a)
    except IsolationError as e:
        print(f"amitorture: isolation violated: {e}", file=sys.stderr)
        return EXIT_ISOLATION
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
