"""One run: every selected matrix entry, launched isolated, observed, stopped."""
from __future__ import annotations

import base64
import datetime
import json
import os
import shutil
import threading
import time

from . import adf as adfmod
from . import amiberry_cfg, emulator, envinfo, hostreq, isolation, mailbox, matrix, records, romid
from . import audio as audiomod
from . import cdimage
from .emulator import RunnerError


class Options:
    def __init__(self, **kw):
        self.binary = kw["binary"]
        self.kickstarts = kw["kickstarts"]
        self.workdir = kw["workdir"]
        self.configs = kw.get("configs") or []
        self.matrix = kw.get("matrix") or "smoke"
        self.sets = kw.get("sets") or {}
        self.timeout = kw.get("timeout", 300.0)
        self.hang = kw.get("hang", 30.0)
        self.adf = kw.get("adf")
        self.disk_set = kw.get("disk_set")
        self.no_disk = kw.get("no_disk", False)
        self.selftest = kw.get("selftest", False)
        self.params = kw.get("params") or {}
        self.boot_timeout = kw.get("boot_timeout", 60.0)
        self.verbose = kw.get("verbose", True)
        self.keep = kw.get("keep", False)
        self.jobs = max(1, int(kw.get("jobs", 1) or 1))
        self.narrow = kw.get("narrow", True)
        self.narrow_budget = kw.get("narrow_budget", 7200.0)
        self.narrow_reruns = kw.get("narrow_reruns", 3)
        self.narrow_binaries = kw.get("narrow_binaries") or []
        self.baseline = kw.get("baseline")
        self.amiberry_src = kw.get("amiberry_src")
        self.repro = kw.get("repro", True)
        self.xfail_path = kw.get("xfail_path")
        self.media = kw.get("media", True)
        self.audio = kw.get("audio", "auto")          # auto | on | off (SDL disk audio capture)
        self.warp = kw.get("warp")
        if self.warp not in (None, "off", "auto", "on"):
            raise RunnerError(f"unknown warp policy {self.warp!r}")
        if self.warp is not None and "warp" in self.sets:
            raise RunnerError("use --warp or --set warp=..., not both")
        self.ports = kw.get("ports")                  # (lo, hi) serial port range, None = emulator.PORT_RANGE
        self.show = kw.get("show", False)             # emulator windows on the desktop (emulator.display_env)


def log(opts, msg):
    if opts.verbose:
        print(f"[amitorture] {msg}", flush=True)


def new_run_id():
    return datetime.datetime.now(datetime.timezone.utc).strftime("%Y%m%d-%H%M%SZ")


def _safe_rmtree(path, inside):
    path, inside = os.path.abspath(path), os.path.abspath(inside)
    if not path.startswith(inside + os.sep):
        raise RunnerError(f"refusing to delete {path}: not inside {inside}")
    if os.path.exists(path):
        shutil.rmtree(path)


class EntryRun:
    """Observes one emulator instance; collects transports and decides when it ended."""

    POLL = 0.25
    POLL_BUSY = 0.01        # while host requests are coming in (they arrive in bursts: keys, mouse, marks)
    BUSY_WINDOW = 1.0       # seconds after the last request
    SCAN_INTERVAL = 0.5     # until the mailbox is found (quick low-chip scans, every 2nd one full)
    URGENT_SCAN = 0.25      # full scans this often while a request waits for the mailbox
    DONE_GRACE = 5.0

    def __init__(self, opts, entry, rawdir, rel="", audio=None, warp=False):
        self.opts, self.entry, self.rawdir, self.rel = opts, entry, rawdir, rel
        if warp:
            self.POLL = self.POLL_BUSY
        self.audio = audio          # audio.Capture (SDL disk driver) or None
        self.requests = None        # hostreq.Requests, once the IPC is up
        self._taken = {"serial": 0, "mailbox": 0}
        self.serial_times = []
        self._serial_bytes = 0
        self._serial_boot = 0
        self.lock = threading.Lock()
        self.serial_parser = records.StreamParser("serial")
        self.mbox_parser = records.StreamParser("mailbox")
        self.mbox_log = open(os.path.join(rawdir, "mailbox.jsonl"), "w", encoding="utf-8")
        self.t0 = time.monotonic()
        self.mb = {"found_at": None, "candidates": [], "scans": 0, "scan_reads": 0, "scan_seconds": 0.0,
                   "polls": 0, "lost_bytes": 0, "bytes": 0, "last_header": None, "vanished": 0}
        self.follower = None
        self.watch = mailbox.Watch()
        self.last_progress = time.monotonic()
        self.events = []
        self.pid = None             # the emulator's pid (thread CPU times for hostspeed)
        self.speed_samples = []     # hostspeed: [t, heartbeat, case, cpu_main_ns, cpu_total_ns, cpu_top_ns]

    def event(self, what, **kw):
        e = {"t": round(time.monotonic() - self.t0, 3), "event": what, **kw}
        self.events.append(e)
        self.mbox_log.write(json.dumps(e) + "\n") if what.startswith("mailbox") else None

    def on_serial(self, data):
        with self.lock:
            self.serial_parser.feed(data)
            self.last_progress = time.monotonic()
            # arrival time of every chunk (seconds since launch, bytes so far):
            # logmine.py maps emulator log lines to the module that ran then
            self._serial_bytes += len(data)
            self.serial_times.append([round(self.last_progress - self.t0, 3), self._serial_bytes])

    # -- mailbox ---------------------------------------------------------
    QUICK_TOP = 0x40000   # where exec's AllocMem usually puts the mailbox (CONTRACT §7 note)

    def scan(self, ipc, full=True):
        stats = {}
        t = time.monotonic()
        top = matrix.chip_bytes(self.entry)
        if not full:
            top = min(top, self.QUICK_TOP)
        found = mailbox.scan(ipc, 0, top, stats=stats)
        dt = time.monotonic() - t
        self.mb["scans"] += 1
        self.mb["scan_reads"] += stats.get("reads", 0)
        self.mb["scan_seconds"] = round(self.mb["scan_seconds"] + dt, 3)
        rec = {"t": round(time.monotonic() - self.t0, 3), "event": "scan", "range": [0, top],
               "reads": stats.get("reads", 0), "seconds": round(dt, 3), "found": [f"{a:08x}" for a in found]}
        self.mbox_log.write(json.dumps(rec) + "\n")
        self.mbox_log.flush()
        if not found:
            return None
        # Several hits (e.g. the magic string in the payload's data): take the
        # one with the most output, the first one on a tie.
        best, best_total = None, -1
        for a in found:
            h = mailbox.read_header(ipc, a)
            if h and h.total > best_total:
                best, best_total = a, h.total
        self.mb["candidates"] = [f"{a:08x}" for a in found]
        return best

    def speed_sample(self, h):
        """RingFollower callback, right after a mailbox header read: while a
        hostspeed block runs (module 26, case HS_POS0 + block), keep the
        heartbeat (one per frame: the block sends no records), the host
        clock and the emulator's thread CPU times of this instant
        (cases.speed_cases turns them into frame rate and CPU cost)."""
        if h.mod != SPEED_MOD_ID or not SPEED_POS0 <= h.case < SPEED_POS0 + 1000:
            return
        t = time.monotonic() - self.t0
        cpu = thread_cpu(self.pid) if self.pid else None
        self.speed_samples.append([round(t, 6), h.heartbeat, h.case] + (list(cpu) if cpu else [None, None, None]))

    def poll_mailbox(self, ipc):
        if self.follower.on_header is None:
            self.follower.on_header = self.speed_sample
        res = self.follower.poll()
        self.mb["polls"] += 1
        if res is None:
            self.mb["vanished"] += 1
            self.mbox_log.write(json.dumps({"t": round(time.monotonic() - self.t0, 3), "event": "vanished",
                                            "addr": f"{self.follower.addr:08x}"}) + "\n")
            self.follower = None
            with self.lock:
                self.mbox_parser.lose(0, self.mbox_parser.splitter.base, "mailbox vanished (reboot?)")
            return None
        h = res["header"]
        self.mb["last_header"] = h.as_dict()
        if res["data"] or res["lost"]:
            self.mbox_log.write(json.dumps({"t": round(time.monotonic() - self.t0, 3), "event": "data",
                                            "header": h.as_dict(), "lost": res["lost"], "lost_at": res["lost_at"],
                                            "data_b64": base64.b64encode(res["data"]).decode()}) + "\n")
            self.mbox_log.flush()
            with self.lock:
                if res["lost"]:
                    self.mbox_parser.lose(res["lost"], res["lost_at"], "ring overrun")
                self.mbox_parser.feed(res["data"])
            self.mb["bytes"] += len(res["data"])
            self.mb["lost_bytes"] += res["lost"]
            self.last_progress = time.monotonic()
        return h

    def harvest(self):
        """New request/capture records from both transports -> self.requests."""
        if self.requests is None:
            return
        with self.lock:
            for src, parser in (("serial", self.serial_parser), ("mailbox", self.mbox_parser)):
                new = parser.records[self._taken[src]:]
                self._taken[src] = len(parser.records)
                for r in new:
                    if not r.crc_ok or r.obj is None:
                        continue
                    if src == "serial" and r.boot > self._serial_boot:
                        self._serial_boot = r.boot
                        self.requests.new_boot()
                    if r.obj.get("t") in ("request", "capture"):
                        self.requests.offer(r.obj, src)

    def done_seen(self):
        with self.lock:
            s = any(r.type == "done" and r.crc_ok for r in self.serial_parser.records)
            m = any(r.type == "done" and r.crc_ok for r in self.mbox_parser.records)
        return s, m

    def any_records(self):
        with self.lock:
            return bool(self.serial_parser.records or self.mbox_parser.records)

    # -- selftest: a synthetic mailbox written over IPC -------------------
    def selftest(self, ipc):
        """Writes a fake mailbox into guest chip RAM (emulation paused) and
        lets the normal scan/follow path find and read it, in two steps so
        that the second one wraps the ring."""
        top = matrix.chip_bytes(self.entry)
        addr = (top - 0x4000) & ~15
        recs = selftest_records()
        part1, part2 = recs[:2], recs[2:]
        # Ring just big enough for step 2 alone: step 2 then wraps, but
        # nothing is overwritten before it was read (no loss expected).
        n1, n2 = len(b"".join(part1)), len(b"".join(part2))
        ring = (max(n1, n2) + 64 + 15) & ~15
        ipc.request("PAUSE")
        time.sleep(0.3)
        info = {"addr": f"{addr:08x}", "ring_size": ring, "records": len(recs)}
        try:
            data1 = b"".join(part1)
            img = mailbox.build_image(part1, ring_size=ring, state=1, heartbeat=1)
            bad = ipc.write_bytes(addr, img)
            if bad:
                raise RunnerError(f"WRITE_MEM failed: {bad[:3]}")
            found = self.scan(ipc)
            info["found"] = None if found is None else f"{found:08x}"
            if found != addr:
                info["error"] = "scan did not find the mailbox that was written"
                return info
            self.follower = mailbox.RingFollower(ipc, addr)
            self.mb["found_at"] = f"{addr:08x}"
            self.poll_mailbox(ipc)
            # step 2: append the rest so that the ring wraps.
            data2 = b"".join(part2)
            full = data1 + data2
            total = len(full)
            ringbuf = bytearray(ring)
            for i in range(max(0, total - ring), total):
                ringbuf[i % ring] = full[i]
            hdr = mailbox.MAGIC + __import__("struct").pack(">8I", 1, 2, 7, total, ring, 0, 0, 0)
            bad = ipc.write_bytes(addr, hdr + bytes(ringbuf))
            if bad:
                raise RunnerError(f"WRITE_MEM failed: {bad[:3]}")
            self.poll_mailbox(ipc)
            info["wrapped"] = total > ring
            info["total_bytes"] = total
            info["lost_bytes"] = self.mb["lost_bytes"]
            # request round trip (CONTRACT §13): the synthetic records hold a
            # "wait" request; the normal intake/service path must write its id
            # to mailbox+44, read back here over IPC.
            if self.requests is not None:
                self.harvest()
                made = self.requests.service(addr)
                ack = ipc.read_longs([addr + hostreq.ACK_OFFSET])[0]
                info["request_roundtrip"] = {"served": len(made), "acked": [h["acked"] for h in made],
                                             "host_ack_read_back": ack, "ok": ack == 1 and len(made) == 1}
                if not info["request_roundtrip"]["ok"]:
                    info["error"] = f"request round trip failed: {info['request_roundtrip']}"
        finally:
            # remove the magic so nothing else ever finds it, then resume.
            ipc.write_bytes(addr, bytes(16))
            ipc.request("RESUME")
            self.follower = None
        return info

    def close(self):
        with self.lock:
            self.serial_parser.close()
            self.mbox_parser.close()
            try:
                with open(os.path.join(self.rawdir, "serial-times.json"), "w", encoding="utf-8") as fh:
                    json.dump(self.serial_times, fh)
            except OSError:
                pass
        self.mbox_log.close()


# hostspeed (disk/modules/hostspeed.c): its module id and the mailbox case
# number of its block 0 while a timed block runs
SPEED_MOD_ID = 26
SPEED_POS0 = 1000


def thread_cpu(pid):
    """(main thread ns, all threads ns, busiest other thread ns) of CPU time
    so far, from /proc/<pid>/task/*/schedstat (first field: ns on the CPU),
    or None. The main thread (tid = pid) runs Amiberry's emulation."""
    base = f"/proc/{pid}/task"
    try:
        tids = os.listdir(base)
    except OSError:
        return None
    main = total = top = 0
    for tid in tids:
        try:
            with open(f"{base}/{tid}/schedstat", "rb") as fh:
                ns = int(fh.read().split()[0])
        except (OSError, ValueError, IndexError):
            continue
        total += ns
        if tid == str(pid):
            main = ns
        elif ns > top:
            top = ns
    return main, total, top


def selftest_records():
    seq = iter(range(1000))

    def r(t, **kw):
        return records.encode({"v": 1, "seq": next(seq), "t": t, **kw})
    probe = {"cpu": "68000", "cpu_rev": -1, "fpu": "none", "fpu_frame": 0, "mmu": 0, "addr24": 1, "vbr": "00000000",
             "cacr": "00000000", "how": ["selftest"], "agnus_id": "00", "denise_id": "ff", "chipset": "OCS",
             "ecs_agnus": 0, "ecs_denise": 0, "video": "PAL", "exec_attn": "0000", "kick": "0.0",
             "rom_base": "00f80000", "mem": [], "boards": []}
    out = [r("hello", build="host-selftest", mode=1, seed=1, exec="0.0", cpu="68000", fpu="none", mmu=0, probe=probe),
           r("mod", mod="selftest", ev="begin")]
    for i in range(12):
        out.append(r("case", mod="selftest", case=f"ring_{i:02d}", st="pass"))
    out.append(r("mod", mod="selftest", ev="end", **{"pass": 12, "fail": 0, "skip": 0}))
    out.append(r("request", id=1, what="wait", tag="selftest", args={}))
    n = len(out) + 1
    out.append(r("done", **{"pass": 12, "fail": 0, "skip": 0, "untestable": 0, "aborted": 0, "records": n}))
    return out


LAUNCH_ATTEMPTS = 3


def _launch_once(opts, entry, run, argv, env, home, rawdir, port, res):
    proc = emulator.Process(opts.binary, argv, env, os.path.join(rawdir, "amiberry.log"), cwd=home)
    run.pid = proc.proc.pid
    serial = emulator.SerialClient(port, os.path.join(rawdir, "serial.bin"), on_data=run.on_serial)
    ipc = None
    ended = None
    try:
        m, t = proc.wait_for(r"IPC: Listening on (\S+)", opts.boot_timeout)
        if not m:
            ended = "crash" if not proc.alive() else "error"
            res["reason"] = "Amiberry never reported its IPC endpoint ('IPC: Listening on ...')"
        else:
            endpoint = m.group(1)
            res["ipc"] = {"endpoint": endpoint, "reported_after_s": round(t, 3)}
            ipc = emulator.IPC(endpoint)
            res["ipc"]["version"] = ipc.request("GET_VERSION")
            verify_warp(ipc, res["warp"])
            run.requests = hostreq.Requests(
                ipc, rawdir, run.rel, options=entry["options"], audio=run.audio,
                frame_hz=60 if entry["options"].get("ntsc") == "true" else 50, t0=run.t0,
                log=(lambda m: log(opts, f"{entry['id']}: {m}")) if opts.verbose else None)
            ended = observe(opts, entry, run, proc, serial, ipc, res)
    except (OSError, RunnerError) as e:
        emulator.check_interrupt()          # the IPC broke because Ctrl+C stopped the emulator
        # A lost IPC connection usually means the emulator is dying: give its
        # crash handler time to finish and exit before classifying.
        t_end = time.monotonic() + 10.0
        while proc.alive() and time.monotonic() < t_end:
            time.sleep(0.2)
        crashed_in_log = proc.find(r"--- end exception ---|Fault Address|Segmentation fault|SIGSEGV|SIGBUS")[0]
        if not proc.alive() or crashed_in_log:
            ended = "crash"
            if crashed_in_log:
                res["reason"] = f"emulator crashed (its crash handler logged: {crashed_in_log.group(0)}); " \
                                f"the IPC connection then closed ({type(e).__name__}: {e})"
        else:
            ended = "error"
            res["reason"] = f"{type(e).__name__}: {e}"
    finally:
        exit_before_stop = proc.exit_info()
        stop = proc.stop(ipc)
        if ipc:
            ipc.close()
        serial.stop()
        run.close()
    return proc, serial, ipc, ended, exit_before_stop, stop


def _media_options(opts, ctx, entry, media_dir):
    """Test-owned media (ADF/HDF) from media.py, when that module exists.

    media.py belongs to the devio module. Expected API (any of these names):
    prepare / make_media / config_options / config_lines, called with the
    keyword arguments it accepts out of (entry, options, media_dir, workdir,
    dir, outdir, seed, params). It returns a dict of Amiberry options, a list
    of "key=value" lines, or {"options": {...}, ...}. Anything else, or an
    exception, is recorded and the run goes on without test media."""
    if not opts.media or opts.no_disk:
        return {}, None
    try:
        from . import media  # noqa: F401  (written by the devio agent)
    except ImportError as e:
        return {}, {"available": False, "note": f"runner/amitorture/media.py not present ({e}); no test media"}
    except Exception as e:  # a broken media.py must not stop the run
        return {}, {"available": False, "error": f"importing media.py: {type(e).__name__}: {e}"}
    import inspect
    fn = None
    for name in ("prepare", "make_media", "config_options", "config_lines"):
        if callable(getattr(media, name, None)):
            fn = getattr(media, name)
            break
    if fn is None:
        return {}, {"available": False, "error": "media.py has none of prepare/make_media/config_options/config_lines"}
    params = merge_params(opts.params, entry.get("params", {}))
    have = {"entry": entry, "options": entry["options"], "media_dir": media_dir, "workdir": media_dir,
            "dir": media_dir, "outdir": media_dir, "seed": int(params.get("seed", 1) or 1), "params": params}
    try:
        sig = inspect.signature(fn)
        kwargs = {k: v for k, v in have.items() if k in sig.parameters}
        if not kwargs and sig.parameters:
            out = fn(media_dir)
        else:
            out = fn(**kwargs)
    except Exception as e:
        return {}, {"available": True, "function": fn.__name__, "error": f"{type(e).__name__}: {e}"}
    info = {"available": True, "function": fn.__name__}
    optsd = {}
    if isinstance(out, tuple) and out:
        out, extra_info = out[0], out[1:]
        info["extra"] = [str(x)[:500] for x in extra_info]
    if isinstance(out, dict) and "options" in out and isinstance(out["options"], (dict, list)):
        info.update({k: v for k, v in out.items() if k != "options" and isinstance(v, (str, int, float, list, dict))})
        out = out["options"]
    if isinstance(out, dict):
        optsd = {str(k): str(v) for k, v in out.items()}
    elif isinstance(out, (list, tuple)):
        for line in out:
            if isinstance(line, str) and "=" in line:
                k, v = line.split("=", 1)
                optsd[k.strip()] = v.strip()
    else:
        info["error"] = f"unexpected return type {type(out).__name__}"
    info["options"] = optsd
    return optsd, info


def _copy_rom(ctx, rom):
    roms_dir = os.path.join(ctx["media"], "roms")
    os.makedirs(roms_dir, exist_ok=True)
    dst = os.path.join(roms_dir, rom["file"])
    with ctx["lock"]:
        if not os.path.exists(dst):
            shutil.copyfile(rom["path"], dst)
        if rom.get("key"):
            kd = os.path.join(roms_dir, "rom.key")
            if not os.path.exists(kd):
                shutil.copyfile(rom["key"], kd)
    return dst


def _rom_info(rom):
    return {"file": rom["file"], "source": rom["path"], "crc32": rom["crc32"], "sha1": rom["sha1"], **rom["rom"]}


# Emulator network backends with no real LAN behind them (user-mode NAT).
# On these the network module may broadcast its ARP request (run flag
# "net", AMT_F_NET; CONTRACT section 4): nothing outside the emulator sees it.
_EMU_NET_KEYS = ("a2065", "zz9000_net", "ne2000_pci", "ne2000_pcmcia")


FLAG_BITS = {"unsafe": 1, "noserial": 2, "nombox": 4, "net": 8, "noscreen": 16}   # tools/mkadf.py


def _add_flag(params, name):
    """OR one run-parameter flag into params["flags"], whether that is a
    number or a list of names (so a user's --param flags=... is kept)."""
    fl = params.get("flags", 0)
    try:
        params["flags"] = str((int(fl, 0) if isinstance(fl, str) else int(fl)) | FLAG_BITS[name])
    except ValueError:
        names = [x for x in str(fl).replace("+", ",").split(",") if x.strip()]
        if name not in names:
            names.append(name)
        params["flags"] = ",".join(names)
    return params


def _mask(v):
    return 0xFFFFFFFFFFFFFFFF if str(v).lower() == "all" else int(str(v), 0)


def merge_params(run, entry):
    """The run's parameters with the entry's on top, except that two module
    masks are ANDed: a board entry's own mask (matrix.py) and --param
    modules=... (or narrowing's one-module mask) both apply."""
    p = dict(run, **entry)
    if "modules" in run and "modules" in entry:
        try:
            p["modules"] = f"{_mask(run['modules']) & _mask(entry['modules']):#x}"
        except ValueError:
            pass
    return p


def _with_net_flag(params, options):
    if any(str(options.get(k, "")).strip().lower() == "slirp" for k in _EMU_NET_KEYS):
        _add_flag(params, "net")
    return params


def _with_entry_flags(params, entry):
    """Flags an entry needs on top of the run's (matrix.py "flags": e.g.
    "unsafe" where the board under test is known to be configured), plus the
    net flag for an emulator-only network backend."""
    for name in entry.get("flags") or ():
        _add_flag(params, name)
    return _with_net_flag(params, entry["options"])


def base_result(entry):
    return {"id": entry["id"], "title": entry["title"], "jit": entry.get("jit"), "twin_of": entry.get("twin_of"),
            "twin_kind": entry.get("twin_kind"), "twin_options": entry.get("twin_options", []),
            "machine": {k: entry.get(k) for k in ("cpu", "fpu", "chipset", "video", "model", "core")},
            "options": entry["options"], "overrides": entry.get("overrides", {}), "params": entry.get("params", {}),
            "narrow": entry.get("narrow"), "status": None, "reason": None}


# Disks whose modules test the floppy hardware itself and need real drive
# speed: chip (paula: raw disk DMA, DSKSYNC, motor/ready timing) and cpux
# (dmacontention: disk DMA under load). Turbo (floppy_speed=0, disk.cpp's
# turbo DMA path) breaks raw disk-DMA timing, but loads the payload through
# trackdisk in a fraction of the time (about 9 s of a disk's ~12 s start).
REAL_SPEED_GROUPS = ("chip", "cpux")
WARP_GROUPS = ("cpu", "fpu", "mmu")


def build_group(info):
    parts = (info.get("build_id") or "").split()
    if len(parts) >= 2 and "/" in parts[-2] and parts[-2].replace("/", "").isdigit():
        return parts[-1]
    return None


def disk_warp(opts, entry, info, capture=False, extra=None):
    """Choose pacing from the baked disk's build identity, never its filename."""
    group = build_group(info or {})
    mode = getattr(opts, "warp", None)
    configured = (extra or {}).get("warp", entry["options"].get("warp", "false"))
    if mode is None:
        value = str(configured).strip().lower()
        if value not in ("true", "yes", "1", "false", "no", "0"):
            raise RunnerError(f"invalid warp option {configured!r}")
        enabled = value in ("true", "yes", "1")
        mode = "config" if "warp" in entry["options"] or "warp" in (extra or {}) else "off"
        reason = "explicit config option" if mode == "config" else "normal pacing by default"
    elif mode == "auto":
        # JIT entries keep normal pacing: with warp the JIT runs these disks
        # slower (smoke, Pi 500: cpu+fpu+mmu 16-18 s -> 46-52 s per entry)
        jit = str(entry["options"].get("cachesize", "0")).strip() not in ("", "0")
        enabled = group in WARP_GROUPS and not capture and not opts.selftest and not jit
        reason = ("audio capture requires normal pacing" if capture else
                  "selftest requires normal pacing" if opts.selftest else
                  "JIT entry: warp slows the JIT down" if jit and group in WARP_GROUPS else
                  "functional set disk" if enabled else
                  "timing-sensitive or unclassified disk")
        if "warp" in (extra or {}):
            raise RunnerError("automatic warp cannot be overridden through extra options")
    else:
        enabled = mode == "on"
        reason = "forced by warp policy" + ("; timing-sensitive results may be invalid" if enabled else "")
    return {"requested": mode, "effective": enabled, "group": group, "reason": reason}


def verify_warp(ipc, policy):
    reply = ipc.request("GET_WARP")
    policy["reply"] = reply
    policy["verified"] = reply == f"OK\t{int(policy['effective'])}"
    if not policy["verified"]:
        raise RunnerError(f"warp state verification failed: expected {int(policy['effective'])}, got {reply!r}")


def adf_set_group(path):
    """The set group ("core", "exp", ...) a disk was built as, from its header
    build id ("<commit> <date> <n>/<m> <group>"), or None (the all-in-one
    ADF, an unreadable header, no file)."""
    try:
        bid = (adfmod.header_info(path) or {}).get("build_id") or ""
    except Exception:   # not an amitorture disk: real speed, as before
        return None
    return build_group({"build_id": bid})


def disk_floppy_speed(dname, entry, adf=None):
    """floppy_speed for one disk launch: "0" (turbo) for a set disk that does
    not test the floppy hardware, None (the entry's own setting, real speed)
    otherwise. A set disk run on its own (--adf, no dname) is recognised by
    its header, so it gets the same speed as in the set; the all-in-one ADF
    holds the floppy tests too and stays at real speed. An explicit
    floppy_speed (matrix entry or --set) always wins; narrowing's paula
    variant still forces 100 through extra_options."""
    if "floppy_speed" in (entry.get("options") or {}):
        return None
    if dname:
        group = dname.split("-", 1)[1] if "-" in dname else dname
    else:
        group = adf_set_group(adf) if adf else None
        if group is None:
            return None
    return None if group in REAL_SPEED_GROUPS else "0"


def safe_id(rid):
    return rid.replace("~", "_").replace("=", "-").replace("/", "_")


def _run_disk(opts, ctx, entry, binary=None, extra_options=None, disk=None):
    """One emulator launch of one entry with one disk ((name, path) of the
    disk set, or None for the single --adf / --no-disk). Returns a result
    dict as described in docs/schema.md (one per disk)."""
    binary = binary or opts.binary
    rid = entry["id"]
    safe = safe_id(rid)
    dname = disk[0] if disk else None
    dsuf = f".{dname}" if dname else ""
    res = base_result(entry)
    res["binary"] = binary
    res["disk_name"] = dname
    # ROM
    if "builtin" in entry["rom"]:
        rom_cfg = entry["rom"]["builtin"]
        res["rom"] = {"builtin": rom_cfg, "name": "AROS ROM built into the Amiberry binary"}
    else:
        rom = ctx["romset"].find(entry["rom"]["ids"])
        if rom is None:
            res["status"] = "skip"
            res["reason"] = f"no ROM: needs {entry['rom']['what']}; none found in {opts.kickstarts}"
            return res
        rom_cfg = _copy_rom(ctx, rom)
        res["rom"] = _rom_info(rom)
    ext_cfg = None
    if entry.get("ext_rom"):
        ext = ctx["romset"].find(entry["ext_rom"]["ids"])
        if ext is None:
            res["status"] = "skip"
            res["reason"] = f"no extended ROM: needs {entry['ext_rom']['what']}; none found in {opts.kickstarts}"
            return res
        ext_cfg = _copy_rom(ctx, ext)
        res["ext_rom"] = _rom_info(ext)
    board_opts = {}
    for br in entry.get("board_roms") or []:
        b = ctx["romset"].find(br["ids"])
        if b is None:
            res["status"] = "skip"
            res["reason"] = f"no board ROM: needs {br['what']}; none found in {opts.kickstarts}"
            return res
        board_opts[br["option"]] = _copy_rom(ctx, b)
        res.setdefault("board_roms", []).append(dict(_rom_info(b), option=br["option"]))
    # per-entry temp dir under the run's temp root in the work dir (short
    # name, see Context; entry.txt says which entry it is)
    with ctx["lock"]:
        ctx["seq"][0] += 1
        etmp = os.path.join(ctx["tmp"], f"e{ctx['seq'][0]:03d}")
    _safe_rmtree(etmp, ctx["tmp"])
    os.makedirs(etmp)
    with open(os.path.join(etmp, "entry.txt"), "w", encoding="utf-8") as fh:
        fh.write(rid + "\n")
    media_dir = os.path.join(etmp, "media")
    os.makedirs(media_dir, exist_ok=True)
    for mf in entry.get("media_files") or []:
        # board media the entry needs (e.g. a PCMCIA SRAM card image), zero-filled
        with open(os.path.join(media_dir, mf["name"]), "wb") as fh:
            fh.truncate(int(mf["size"]))
    params = _with_entry_flags(merge_params(opts.params, entry.get("params", {})), entry)
    # Disk
    if opts.no_disk:
        floppy = ""
        res["disk"] = None
    else:
        floppy = os.path.join(media_dir, f"{safe}{dsuf}.adf")
        res["disk"] = adfmod.bake(disk[1] if disk else ctx["adf_src"], floppy, params)
    cd_opts = {}
    if entry.get("boot") == "cd" and not opts.no_disk:
        # CD32/CDTV: boot from a CD made from the baked disk (cdimage.py,
        # CONTRACT §14); no floppy drive, as on the real machine.
        iso = os.path.join(media_dir, f"{safe}{dsuf}.iso")
        try:
            res["cd"] = cdimage.make_boot_iso(floppy, iso, roms=[ext_cfg, rom_cfg],
                                              machine=entry["options"].get("chipset_compatible", "CD32"))
        except cdimage.CdError as e:
            raise RunnerError(f"CD image for {rid}: {e}")
        res["cd"]["iso"] = os.path.basename(iso)
        cd_opts = {"cdimage0": f"{iso},image", "nr_floppies": "0", "floppy0type": "-1"}
        floppy = ""
    # Config
    media_opts, media_info = _media_options(opts, ctx, entry, media_dir)
    if cd_opts:
        # no floppy drive on a CD-booted machine: drop test floppies (DF1)
        dropped = sorted(k for k in media_opts if k.startswith("floppy") or k == "nr_floppies")
        media_opts = {k: v for k, v in media_opts.items() if k not in dropped}
        if dropped and media_info is not None:
            media_info["dropped_for_cd_boot"] = dropped
    if media_info is not None:
        res["media"] = media_info
    extra = dict(media_opts)
    extra.update(cd_opts)
    extra.update(board_opts)
    fs = disk_floppy_speed(dname, entry, adf=None if disk or opts.no_disk else ctx.get("adf_src"))
    if fs is not None:
        extra["floppy_speed"] = fs
        res["floppy_speed"] = fs
    if extra_options:
        extra.update(extra_options)
    src_disk = disk[1] if disk else (None if opts.no_disk else ctx.get("adf_src"))
    capture = audio_wanted(opts, entry, src_disk)
    res["warp"] = disk_warp(opts, entry, res.get("disk"), capture, extra)
    extra["warp"] = "true" if res["warp"]["effective"] else "false"
    port = emulator.pick_port(*(opts.ports or emulator.PORT_RANGE))
    cfg_entry = dict(entry, options={k: (v.replace("{media}", media_dir) if isinstance(v, str) else v)
                                     for k, v in entry["options"].items()})
    opts_written, text = amiberry_cfg.build(cfg_entry, rom_path=rom_cfg, floppy=floppy, serial_port=port,
                                            media_dir=media_dir, ext_rom_path=ext_cfg, extra=extra)
    conf = os.path.join(etmp, f"{safe}{dsuf}.uae")
    with open(conf, "w", encoding="utf-8") as fh:
        fh.write(text)
    cfgdir = os.path.join(ctx["results"], "configs")
    shutil.copyfile(conf, os.path.join(cfgdir, f"{safe}{dsuf}.uae"))
    home = os.path.join(etmp, "home")
    env = emulator.isolated_env(home, tmpdir=os.path.join(etmp, "tmp"), show=getattr(opts, "show", False))
    # Audio capture (SDL3 disk driver) only where the disk asks for audio
    # marks (or --audio on); the dummy driver otherwise.
    audio_path = None
    if capture:
        audio_path = os.path.join(etmp, "audio.raw")
        env.update(audiomod.env_for(audio_path))
    ver = amiberry_cfg.verify(binary, conf, env)
    with open(os.path.join(cfgdir, f"{safe}{dsuf}.resolved.uae"), "w", encoding="utf-8") as fh:
        fh.write(ver.get("resolved_text", ""))
    res["config"] = {"file": f"configs/{safe}{dsuf}.uae", "resolved": f"configs/{safe}{dsuf}.resolved.uae",
                     "dump_ok": ver.get("ok"), "differences": ver.get("differences", []),
                     "unknown_options": ver.get("unknown_options", []), "fixup_messages": ver.get("fixup_messages", []),
                     "error": ver.get("error"), "returncode": ver.get("returncode"), "stderr": ver.get("stderr"),
                     "serial_port": port}
    res["resolved_options"] = {k: v for k, v in (ver.get("resolved") or {}).items()
                               if k in entry["options"] or k in amiberry_cfg.DWRITE_DEFAULTS}
    # "config adjusted by emulator": the machine that ran is not the one designed
    adj = [d for d in ver.get("differences", []) if d["option"] not in ADJUST_IGNORE]
    if adj:
        res["adjusted"] = adj
    error = amiberry_cfg.verification_error(ver)
    if error:
        res["status"], res["reason"], res["ended"] = "error", error, "error"
        emulator.release_port(port)
        if not opts.keep:
            _safe_rmtree(etmp, ctx["tmp"])
        return res
    # Launch
    rel = f"raw/{safe}/{dname}/" if dname else f"raw/{safe}/"
    rawdir = os.path.join(ctx["results"], rel)
    os.makedirs(rawdir, exist_ok=True)
    res["raw"] = rel
    argv = ["-G", "--log", "--config", conf]
    res["command"] = {"argv": [binary, *argv], "env": {k: env[k] for k in (
        "HOME", "XDG_CONFIG_HOME", "XDG_DATA_HOME", "XDG_RUNTIME_DIR", "AMIBERRY_HOME_DIR", "TMPDIR",
        "SDL_VIDEO_DRIVER", "SDL_VIDEODRIVER", "SDL_AUDIO_DRIVER", "SDL_AUDIODRIVER",
        "SDL_AUDIO_DISK_OUTPUT_FILE", "WAYLAND_DISPLAY", "DISPLAY") if k in env}}
    eopts = opts if binary == opts.binary else _with_binary(opts, binary)
    t_start = time.time()
    attempts = []
    slot = emulator.emulator_slot()
    t_wait = time.time()
    res["emulator_slot"] = slot.__enter__()     # held until this emulator has exited
    res["slot_wait_s"] = round(time.time() - t_wait, 2)
    t_start = time.time()
    load_start = _loadavg()
    try:
        for attempt in range(1, LAUNCH_ATTEMPTS + 1):
            cap = None
            if audio_path:
                if os.path.exists(audio_path):
                    os.remove(audio_path)
                cap = audiomod.Capture(audio_path, rawdir, rel)
            run = EntryRun(eopts, entry, rawdir, rel, audio=cap, warp=res["warp"]["effective"])
            log(opts, f"{rid}{(' [' + dname + ']') if dname else ''}: launching ({entry['title']}), serial port {port}"
                + (f", attempt {attempt}" if attempt > 1 else "")
                + (f", binary {binary}" if binary != opts.binary else "")
                + f", warp {'on' if res['warp']['effective'] else 'off'} ({res['warp']['reason']})")
            proc, serial, ipc, ended, exit_before_stop, stop = _launch_once(eopts, entry, run, argv, env, home,
                                                                            rawdir, port, res)
            emulator.check_interrupt()      # stopped by Ctrl+C: not a result
            sdl_fail, _ = proc.find(r"SDL could not initialize")
            if ended == "crash" and sdl_fail and not run.any_records() and attempt < LAUNCH_ATTEMPTS:
                # Known start-up flake (display/SDL init), not a guest-visible failure: retry.
                attempts.append({"attempt": attempt, "why": sdl_fail.group(0), "exit": exit_before_stop,
                                 "log_tail": proc.tail(10)})
                for f in ("amiberry.log", "serial.bin", "mailbox.jsonl"):
                    p = os.path.join(rawdir, f)
                    if os.path.exists(p):
                        os.replace(p, os.path.join(rawdir, f"attempt{attempt}.{f}"))
                res["reason"] = None
                time.sleep(1.0)
                continue
            break
    finally:
        slot.__exit__(None, None, None)
        emulator.release_port(port)
    res["launch_attempts"] = attempts
    selftest_info = res.pop("_selftest", None)
    res["duration_s"] = round(time.time() - t_start, 2)
    res["started"] = datetime.datetime.fromtimestamp(t_start, datetime.timezone.utc).isoformat()
    res["process"] = {"pid": proc.proc.pid, "exit_before_stop": exit_before_stop, "stop_method": stop,
                      "exit": proc.exit_info(), "log": f"{rel}amiberry.log", "log_tail": proc.tail(80)}
    m_sig, _ = proc.find(r"si_signo = (\d+)")
    if m_sig:
        try:
            import signal as _signal
            res["process"]["crash_signal"] = f"{_signal.Signals(int(m_sig.group(1))).name} ({m_sig.group(1)})"
        except ValueError:
            res["process"]["crash_signal"] = m_sig.group(1)
    res["log_counts"] = log_counts(proc)
    with proc.lock:
        _lines = [line for _, line in proc.lines]
    eng = engine_from_log(_lines)
    res["engine"] = eng
    if eng is not None:
        known = {d["option"] for d in res.get("adjusted", [])}
        extra_adj = [d for d in engine_adjustments(entry["options"], eng) if d["option"] not in known]
        if extra_adj:
            res.setdefault("adjusted", []).extend(extra_adj)
            adj = res["adjusted"]
    m_acc, _ = proc.find(r"TCP: Serial connection accepted")
    res["serial"] = {"port": port, "connected_after_s": None if serial.connected_at is None else round(serial.connected_at, 3),
                     "accepted_in_log": bool(m_acc), "bytes": serial.bytes, "closed_by_peer": serial.closed_by_peer,
                     "error": serial.error, "raw": f"{rel}serial.bin"}
    res["mailbox"] = dict(run.mb, raw=f"{rel}mailbox.jsonl")
    if run.speed_samples:
        res["hostspeed"] = {"fields": ["t", "heartbeat", "case", "cpu_main_ns", "cpu_total_ns", "cpu_top_ns"],
                            "samples": run.speed_samples, "poll_s": run.POLL, "pos0": SPEED_POS0}
    res["host_context"] = dict(envinfo.host_brief(), jobs=opts.jobs, loadavg_start=load_start,
                               loadavg_end=_loadavg(), slot_wait_s=res.get("slot_wait_s"))
    if selftest_info is not None:
        res["selftest"] = selftest_info
    if ipc:
        res["ipc"]["requests"] = ipc.requests
    merged = records.merge({"serial": run.serial_parser.records, "mailbox": run.mbox_parser.records})
    host_recs = run.requests.host if run.requests else []
    if run.requests is not None:
        res["host_requests"] = dict(run.requests.summary(), raw=f"{rel}host.jsonl")
    if run.audio is not None:
        res["audio"] = run.audio.info()
        res["audio"]["file"] = "work/tmp (removed after the run unless --keep)" if not opts.keep else audio_path
    if host_recs:
        with open(os.path.join(rawdir, "host.jsonl"), "w", encoding="utf-8") as fh:
            for h in host_recs:
                fh.write(json.dumps(h) + "\n")
    hostreq.attach(merged, host_recs)
    res["records"] = summarize_records(merged, run)
    with open(os.path.join(rawdir, "records.jsonl"), "w", encoding="utf-8") as fh:
        for mrec in merged.records:
            fh.write(json.dumps(mrec) + "\n")
    res["ended"] = ended
    res["status"], why = classify(res, merged, ended)
    if res.get("reason") is None:
        res["reason"] = why
    res["_merged"] = merged
    res["modules"] = {m["rec"].get("mod"): m["rec"].get("id") for m in merged.records
                      if m["rec"].get("t") == "mod" and m["rec"].get("ev") == "begin"}
    if not opts.keep:
        _safe_rmtree(etmp, ctx["tmp"])
    log(opts, f"{rid}{(' [' + dname + ']') if dname else ''}: {res['status']} ({res['reason']}); "
        f"{len(merged.records)} records, stopped by {stop}"
        + (f"; CONFIG ADJUSTED BY EMULATOR: {', '.join(d['option'] for d in adj)}" if adj else ""))
    return res


def _loadavg():
    try:
        return [round(x, 2) for x in os.getloadavg()]
    except OSError:
        return None


def audio_wanted(opts, entry, disk_path):
    """--audio on/off, or auto: the entry asks for it (entry["audio"]), or the
    disk's payload contains an audio_mark request."""
    mode = getattr(opts, "audio", "auto") or "auto"
    if mode == "on":
        return True
    if mode == "off":
        return False
    if entry.get("audio"):
        return True
    return bool(disk_path) and audiomod.wants_audio(disk_path)


# entry status = the worst over its disks
STATUS_RANK = {"crash": 0, "hang": 1, "timeout": 2, "error": 3, "incomplete": 4, "fail": 5, "pass": 6, "skip": 7}


def entry_disks(ctx, entry):
    """[(name, path)] to run for this entry: the disk set (or the disks of it
    named in entry['disk_only']), [None] for a single --adf or --no-disk."""
    disks = ctx.get("disks")
    if not disks:
        return [None]
    only = entry.get("disk_only")
    if only:
        only = [only] if isinstance(only, str) else list(only)
        picked = [d for d in disks if d[0] in only]
        return picked or disks
    groups = entry.get("disk_groups")
    if groups:
        # hardware entries: only the disks of these groups ("<n>-<group>")
        return [d for d in disks if d[0].split("-", 1)[-1] in groups]
    return disks


def run_entry(opts, ctx, entry, binary=None, extra_options=None):
    """Runs one matrix entry: every disk of the set in name order, as
    separate sequential emulator launches (each with its own slot, baked
    disk and raw/<entry>/<disk>/), merged into one entry result."""
    disks = entry_disks(ctx, entry)
    if not disks:
        r = base_result(entry)
        r["status"] = "skip"
        r["reason"] = (f"the disk set has no disk of the group(s) {', '.join(entry.get('disk_groups') or [])} "
                       "this entry needs")
        return r
    subs = []
    for d in disks:
        r = _run_disk(opts, ctx, entry, binary=binary, extra_options=extra_options, disk=d)
        subs.append(r)
        if r["status"] == "skip" and "process" not in r:  # ROM missing: same for every disk
            break
    if disks == [None]:
        return subs[0]
    return combine(entry, subs)


def merge_disks(subs):
    """One Merged over the disks: each record keeps its per-disk boot and seq
    and gets "disk"; the boot number is offset by 1000 per disk so that
    (boot, seq) stays unique. Per-disk integrity stays in each sub-result."""
    out, gaps, bad, conflicts, schema, per_source, dups = [], [], [], [], [], {}, 0
    for k, sub in enumerate(subs):
        m = sub.get("_merged")
        if m is None:
            continue
        dn = sub.get("disk_name")
        for x in m.records:
            out.append(dict(x, boot=k * 1000 + x["boot"], disk=dn))
        gaps += [dict(g, boot=k * 1000 + g["boot"], disk=dn) for g in m.gaps]
        bad += [dict(b, disk=dn) for b in m.bad]
        conflicts += [dict(c, disk=dn) for c in m.conflicts]
        schema += [dict(sp, disk=dn) for sp in m.schema_problems]
        dups += m.duplicates
        for src, c in m.per_source.items():
            t = per_source.setdefault(src, {})
            for kk, v in c.items():
                t[kk] = t.get(kk, 0) + v
    return records.Merged(out, gaps, bad, conflicts, dups, schema, per_source)


def combine(entry, subs):
    """The entry result from its per-disk results (pure; unit-tested)."""
    first = next((s for s in subs if s.get("status") != "skip"), subs[0])
    res = {k: v for k, v in first.items() if not k.startswith("_") and k not in (
        "disk", "disk_name", "raw", "process", "serial", "mailbox", "ipc", "records", "ended", "hang",
        "launch_attempts", "media", "modules", "log_counts", "emulator_slot", "slot_wait_s", "host_requests",
        "audio", "warp")}
    res["disks"] = subs
    if any("warp" in s for s in subs):
        res["warp"] = {"disks": [dict(s["warp"], name=s.get("disk_name")) for s in subs if "warp" in s]}
    if all(s["status"] == "skip" for s in subs):
        res["status"], res["reason"] = "skip", subs[0].get("reason")
        return res
    worst = min(subs, key=lambda s: STATUS_RANK.get(s["status"], 3))
    res["status"] = worst["status"]
    bad = [s for s in subs if s["status"] != "pass"]
    res["reason"] = "; ".join(f"{s.get('disk_name')}: {s['status']} ({s.get('reason')})" for s in bad) \
        if bad else "done, no failures (every disk)"
    res["ended"] = worst.get("ended")
    res["raw"] = "raw/" + safe_id(entry["id"]) + "/"
    res["disk"] = {"set": [{"name": s.get("disk_name"), **{k: (s.get("disk") or {}).get(k) for k in (
        "path", "build_id", "sha256", "params")}} for s in subs]}
    res["duration_s"] = round(sum(s.get("duration_s") or 0 for s in subs), 2)
    res["modules"] = {}
    for s in subs:
        res["modules"].update(s.get("modules") or {})
    adj = []
    for s in subs:
        for d in s.get("adjusted") or []:
            if d not in adj:
                adj.append(d)
    if adj:
        res["adjusted"] = adj
    elif "adjusted" in res:
        del res["adjusted"]
    res["engine"] = next((s.get("engine") for s in subs if s.get("engine")), None)
    hr = [s["host_requests"] for s in subs if s.get("host_requests")]
    if hr:
        res["host_requests"] = {k: sum(h.get(k, 0) for h in hr) for k in ("requests", "acked", "not_acked",
                                                                          "captures", "unserved")}
    for k in ("process", "serial", "mailbox", "ipc", "hang"):
        if worst.get(k) is not None:
            res[k] = worst[k]
    merged = merge_disks(subs)
    res["_merged"] = merged
    hellos = [(s.get("disk_name"), (s.get("records") or {}).get("hello")) for s in subs]
    res["records"] = {"merged": len(merged.records),
                      "per_disk": {s.get("disk_name"): (s.get("records") or {}).get("merged") for s in subs},
                      "gaps": merged.gaps, "bad_count": len(merged.bad),
                      "hello": next((h for _, h in hellos if h), None),
                      "done": {s.get("disk_name"): (s.get("records") or {}).get("done") for s in subs}}
    res["probe_consistency"] = probe_consistency(hellos)
    return res


PROBE_VOLATILE = ("free",)


def _probe_view(hello):
    if not hello:
        return None
    p = dict(hello.get("probe") or {})
    p["mem"] = [{k: v for k, v in m.items() if k not in PROBE_VOLATILE} for m in p.get("mem") or []]
    # `how` is free-text evidence (it may quote open-bus reads such as an OCS
    # DENISEID); the machine is described by the other fields
    p.pop("how", None)
    return {"cpu": hello.get("cpu"), "fpu": hello.get("fpu"), "mmu": hello.get("mmu"), "exec": hello.get("exec"),
            "probe": p}


def probe_consistency(hellos):
    """Do the disks of one entry see the same machine? [(disk, hello)] ->
    {"ok", "reference_disk", "differences": [{disk, path, reference, got}]}.
    Free memory is ignored (each disk's payload has another size)."""
    from . import cases as casemod
    views = [(d, _probe_view(h)) for d, h in hellos if h]
    if len(views) < 2:
        return {"ok": True, "compared": len(views), "differences": []}
    ref_d, ref = views[0]
    diffs = []
    for d, v in views[1:]:
        for path, a, b in casemod.diff_paths(ref, v):
            diffs.append({"disk": d, "path": path, "reference": a, "got": b})
    return {"ok": not diffs, "compared": len(views), "reference_disk": ref_d, "differences": diffs[:50]}


# Differences that do not change the machine (only how Amiberry writes them back)
ADJUST_IGNORE = set()

# Log lines worth counting (JIT fallbacks, flushes, fault recovery)
LOG_PATTERNS = {
    "jit_fallback": r"(?i)jit.*(fallback|not compil|interpret)",
    "jit_flush": r"(?i)(flush_icache|cache flush|JIT: flush)",
    "jit_segv": r"(?i)(segv|sigsegv|fault recovery|caught (a )?fault)",
    "illegal": r"(?i)illegal instruction",
}


_ENGINE_RX = None


def engine_from_log(lines):
    """What actually ran, from build_cpufunctbl's log line (src/newcpu.cpp):
    'CPU=68040, FPU=68040 (host 64b), MMU=0, JIT=CPU/FPU=8192. fast' and the
    threaded-CPU lines ('uae_start_thread "cpu" ...' / '... running the CPU
    on the main thread.'). Returns a dict, or None without such a line."""
    import re as _re
    global _ENGINE_RX
    if _ENGINE_RX is None:
        _ENGINE_RX = _re.compile(r"CPU=(\d+), FPU=(\d+)(?: \(([^)]*)\))?, MMU=(\d+), JIT(?:=(CPU/FPU|CPU))?=(\d+)\.?\s*(.*)$")
    eng = None
    for line in lines:
        m = _ENGINE_RX.search(line)
        if m:
            eng = {"line": line[line.find("CPU="):].strip(), "cpu_model": m.group(1), "fpu_model": m.group(2),
                   "fpu_mode": m.group(3), "mmu_model": m.group(4), "jit_kind": m.group(5),
                   "cachesize": m.group(6), "mode": m.group(7).strip()}
    if eng is None:
        return None
    eng["cpu_thread_started"] = any('uae_start_thread "cpu"' in line for line in lines)
    eng["cpu_thread_refused"] = [line.strip() for line in lines if "running the CPU on the main thread" in line
                                 or "Threaded CPU mode is not compatible" in line][:3]
    eng["jit_refused"] = [line.strip() for line in lines if "JIT and cycle-exact can't" in line
                          or "JIT requires 68020" in line or "not JIT compatible" in line][:3]
    return eng


def engine_adjustments(options, eng):
    """Differences between the engine the config asked for and the one the
    emulator log says ran (JIT, threaded CPU, softfloat, CPU/FPU/MMU)."""
    if eng is None:
        return []
    out = []

    def add(option, wrote, resolved, because):
        out.append({"option": option, "wrote": wrote, "resolved": resolved, "because": because,
                    "source": "emulator log"})

    want_jit = int(options.get("cachesize", "0") or 0)
    got_jit = int(eng["cachesize"] or 0)
    if want_jit and not got_jit:
        add("cachesize", str(want_jit), "0 (JIT not active)", eng["jit_refused"] + [eng["line"]])
    elif not want_jit and got_jit:
        add("cachesize", "0", f"{got_jit} (JIT active)", [eng["line"]])
    if options.get("cpu_threaded") == "true" and (eng["cpu_thread_refused"] or not eng["cpu_thread_started"]):
        add("cpu_threaded", "true", "false (threaded CPU not active)",
            eng["cpu_thread_refused"] or ['no \'uae_start_thread "cpu"\' in the log'])
    if options.get("cpu_threaded", "false") != "true" and eng["cpu_thread_started"]:
        add("cpu_threaded", "false", "true (a CPU thread was started)", [])
    want_fpu = options.get("fpu_model", "0") or "0"
    if want_fpu != eng["fpu_model"]:
        add("fpu_model", want_fpu, eng["fpu_model"], [eng["line"]])
    sf = "softfloat" in (eng["fpu_mode"] or "")
    if eng["fpu_model"] != "0" and (options.get("fpu_softfloat") == "true") != sf:
        add("fpu_softfloat", options.get("fpu_softfloat", "false"), "true" if sf else "false", [eng["line"]])
    if options.get("cpu_model") and options["cpu_model"] != eng["cpu_model"]:
        add("cpu_model", options["cpu_model"], eng["cpu_model"], [eng["line"]])
    want_mmu = options.get("mmu_model", "0") or "0"
    want_mmu_n = "0" if want_mmu in ("0", "") else (want_mmu.replace("68ec0", "680"))
    if want_mmu_n != eng["mmu_model"]:
        add("mmu_model", want_mmu, eng["mmu_model"], [eng["line"]])
    return out


def log_counts(proc):
    import re as _re
    out = {}
    with proc.lock:
        lines = [line for _, line in proc.lines]
    for k, pat in LOG_PATTERNS.items():
        rx = _re.compile(pat)
        out[k] = sum(1 for line in lines if rx.search(line))
    return out


def _with_binary(opts, binary):
    import copy as _copy
    o = _copy.copy(opts)
    o.binary = binary
    return o


def observe(opts, entry, run, proc, serial, ipc, res):
    """The watch loop. Returns why it ended: done | hang | timeout | crash | selftest | error."""
    deadline = time.monotonic() + opts.timeout
    last_scan = 0.0
    n_scans = 0
    done_at = None
    # Host requests wait for the runner's next poll: at POLL (0.25 s) each
    # key event of input.kbd_keycode_set cost ~0.2 s of waiting. Right
    # after a request, poll fast, so a burst is served at once.
    last_req_n, last_req_t = 0, -1e9
    if opts.selftest:
        time.sleep(2.0)   # let Kickstart get going first
        res["_selftest"] = run.selftest(ipc)
        if "error" in res["_selftest"]:
            return "error"
    while True:
        emulator.check_interrupt()
        now = time.monotonic()
        if not proc.alive():
            emulator.check_interrupt()
            return "crash"
        run.harvest()
        urgent = run.requests is not None and run.requests.waiting() and now - last_scan >= run.URGENT_SCAN
        if run.follower is None and not opts.selftest and (urgent or now - last_scan >= run.SCAN_INTERVAL):
            last_scan = now
            n_scans += 1
            # Low chip RAM most times (fast), all of chip RAM every 2nd time
            # and whenever a request waits for the mailbox (its ack goes there).
            a = run.scan(ipc, full=urgent or (n_scans % 2 == 0))
            if a is not None:
                run.follower = mailbox.RingFollower(ipc, a)
                run.mb["found_at"] = f"{a:08x}"
                run.mb.setdefault("found_after_s", round(now - run.t0, 3))
                log(opts, f"{entry['id']}: mailbox at {a:08x}")
        h = None
        if run.follower is not None:
            try:
                h = run.poll_mailbox(ipc)
            except OSError:
                if not proc.alive():
                    return "crash"
                raise
        if run.requests is not None:
            run.harvest()
            if run.requests.waiting():
                try:
                    run.requests.service(run.follower.addr if run.follower is not None else None)
                except OSError:
                    if not proc.alive():
                        return "crash"
                    raise
                if run.follower is not None:
                    run.last_progress = time.monotonic()
        s_done, m_done = run.done_seen()
        finished_state = h is not None and h.state in (2, 3, 4)
        if (s_done or m_done or finished_state) and done_at is None:
            done_at = now
            if run.follower is None and not opts.selftest:
                # The guest finished before a periodic scan saw its mailbox:
                # it must exist now (unless the mailbox flag is off), so look
                # once more and read it to the end before stopping.
                last_scan = now
                a = run.scan(ipc)
                if a is not None:
                    run.follower = mailbox.RingFollower(ipc, a)
                    run.mb["found_at"] = f"{a:08x}"
                    run.mb["found_after_done"] = True
                    log(opts, f"{entry['id']}: mailbox at {a:08x} (found after done)")
                    continue
        if done_at is not None:
            # Wait for the slower transport to deliver the end too.
            serial_quiet = serial.last_data is None or now - serial.last_data > 1.0
            both = s_done and (m_done or run.follower is None)
            if both or (now - done_at > run.DONE_GRACE and serial_quiet) or opts.selftest:
                return "selftest" if opts.selftest else "done"
        if h is not None and h.state == 1:
            if run.watch.update(h.heartbeat) > opts.hang:
                res["hang"] = {"heartbeat": h.heartbeat, "unchanged_s": opts.hang, "mod": h.mod, "case": h.case,
                               "via": "mailbox heartbeat"}
                return "hang"
        elif run.follower is None and run.any_records() and now - run.last_progress > opts.hang:
            res["hang"] = {"unchanged_s": opts.hang, "via": "no new serial bytes (no mailbox)"}
            return "hang"
        if now > deadline:
            return "timeout"
        if run.requests is not None:
            n = run.requests.stats.get("requests", 0)
            if n != last_req_n:
                last_req_n, last_req_t = n, now
        time.sleep(run.POLL_BUSY if now - last_req_t < run.BUSY_WINDOW else run.POLL)


def summarize_records(merged, run):
    sp, mp = run.serial_parser, run.mbox_parser
    return records.summarize(merged) | {
        "noise": {"serial": {"lines": sp.noise_lines, "bytes": sp.noise_bytes, "samples": sp.noise_samples[:10]},
                  "mailbox": {"lines": mp.noise_lines, "bytes": mp.noise_bytes, "samples": mp.noise_samples[:10]}},
        "lost": {"serial": sp.lost, "mailbox": mp.lost},
    }


def classify(res, merged, ended):
    """Entry status from how it ended and what it said.

    pass       done record, 0 failed cases, records complete and consistent
    fail       at least one failed case (or done.fail > 0)
    crash      the emulator process died on its own
    hang       heartbeat / output stopped before done
    timeout    still running at --timeout without done
    incomplete done seen but records missing/corrupt, or no done but ended otherwise
    error      host-side problem (no IPC endpoint, ...)
    """
    cases = merged.by_type("case")
    failed = [c for c in cases if c.get("st") == "fail"]
    done = merged.by_type("done")
    integrity = []
    if merged.gaps:
        integrity.append(f"{sum(g['count'] for g in merged.gaps)} record(s) missing (seq gaps)")
    bad_lost = records.bad_unrecovered(merged)
    if bad_lost:
        integrity.append(f"{len(bad_lost)} corrupt line(s) (bad CRC/JSON) not recovered from another transport")
    if merged.conflicts:
        integrity.append(f"{len(merged.conflicts)} seq conflict(s) between transports")
    if merged.schema_problems:
        integrity.append(f"{len(merged.schema_problems)} record schema problem(s)")
    if ended != "selftest":
        integrity.extend(records.module_problems(merged))
    guest = hostreq.guest_records(merged.records)
    if done and guest and isinstance(done[-1].get("records"), int) and done[-1]["records"] != len(
            [m for m in guest if m["boot"] == guest[-1]["boot"]]):
        integrity.append(f"done says {done[-1]['records']} records, got "
                         f"{len([m for m in guest if m['boot'] == guest[-1]['boot']])}")
    if ended == "crash":
        ex = res.get("process", {}).get("exit_before_stop", {})
        how = ex.get("signal") or f"code {ex.get('returncode')}"
        sig = res.get("process", {}).get("crash_signal")
        if sig:
            how += f"; its crash handler reports signal {sig}"
        return "crash", f"Amiberry exited by itself ({how})"
    if ended == "error":
        return "error", res.get("reason") or "host-side error"
    if failed or (done and done[-1].get("fail", 0) > 0):
        return "fail", f"{len(failed)} failed case(s)" + ("; " + "; ".join(integrity) if integrity else "")
    if ended == "hang":
        return "hang", f"no progress for {res['hang']['unchanged_s']} s ({res['hang']['via']}) before done"
    if ended == "timeout":
        if not merged.records:
            return "timeout", "no records at all before --timeout (did the disk boot?)"
        return "timeout", "no done record before --timeout"
    if ended in ("done", "selftest"):
        if not done:
            return "incomplete", "mailbox state says finished but no done record arrived"
        if integrity:
            return "incomplete", "; ".join(integrity)
        if done[-1].get("aborted"):
            return "incomplete", f"done.aborted={done[-1]['aborted']}"
        hello = merged.by_type("hello")
        if ended != "selftest" and hello and hello[-1].get("modules") == {}:
            return "skip", "no compiled modules selected by the run mask"
        return "pass", "done, no failures" + (" (host selftest records, not a guest run)" if ended == "selftest" else "")
    return "error", f"ended: {ended}"


def resolve_disk_set(opts):
    """The disk-set directory to use: --disk-set, else (when no --adf is
    given) disk/build/set if it exists. None means the single --adf."""
    if getattr(opts, "disk_set", None):
        d = os.path.abspath(opts.disk_set)
        if not os.path.isdir(d):
            raise RunnerError(f"--disk-set {d}: not a directory")
        return d
    if opts.adf:
        return None
    return adfmod.DEFAULT_SET if os.path.isdir(adfmod.DEFAULT_SET) else None


def unique_dir(path):
    """path, or path-2, path-3 ... if it exists (two runs in one second)."""
    p, n = path, 1
    while os.path.exists(p):
        n += 1
        p = f"{path}-{n}"
    os.makedirs(p)
    return p


def entry_error(entry, ex):
    r = base_result(entry)
    r["status"], r["reason"] = "error", str(ex)
    return r


class Context:
    """Everything shared by the entries of one invocation: the results dir,
    the run's temp root under the work dir (removed at the end unless
    --keep), the ROM set, the test disk and the isolation guard."""

    def __init__(self, opts, results_dir, run_id=None, label="run"):
        self.opts = opts
        self.run_id = run_id or new_run_id()
        work = os.path.abspath(opts.workdir)
        opts.workdir = work
        self.work = work
        self.results = os.path.abspath(results_dir)
        # Short paths: Amiberry's IPC socket lives in <entry tmp>/home/run and
        # AF_UNIX paths are limited to 107 bytes.
        self.tmp_parent = os.path.join(work, "tmp")
        if len(self.tmp_parent) > 56:
            self.tmp_parent = os.path.join(adfmod.REPO, "work", "tmp")
        os.makedirs(self.tmp_parent, exist_ok=True)
        import tempfile
        self.tmp = tempfile.mkdtemp(prefix=f"{label[:3]}{os.getpid()}-", dir=self.tmp_parent)
        self.d = {"results": self.results, "tmp": self.tmp, "media": os.path.join(self.tmp, "shared"),
                  "lock": threading.Lock(), "seq": [0]}
        self.d["disks"] = None
        if not opts.no_disk:
            set_dir = resolve_disk_set(opts)
            if set_dir:
                self.d["disks"] = adfmod.disk_set(set_dir)
                if not self.d["disks"]:
                    raise RunnerError(f"--disk-set {set_dir}: no amitorture-*.adf in it")
                self.d["adf_src"] = self.d["disks"][0][1]
                self.d["disk_set"] = set_dir
            else:
                self.d["adf_src"] = os.path.abspath(opts.adf or adfmod.DEFAULT_ADF)
                if not os.path.isfile(self.d["adf_src"]):
                    raise RunnerError(f"test disk {self.d['adf_src']} not found (build it with `make -C disk set` "
                                      "or `make -C disk`, pass --disk-set DIR / --adf PATH, or use --no-disk)")
        for d in (self.tmp, self.d["media"], os.path.join(self.results, "configs"), os.path.join(self.results, "raw")):
            os.makedirs(d, exist_ok=True)
        watched = isolation.watched_paths(extra=[(os.path.dirname(opts.binary), False, "warn"),
                                                 (os.path.abspath(opts.kickstarts), False, "warn")])
        self.guard = isolation.Guard(watched).__enter__()
        probe_home = os.path.join(self.tmp, "probe-home")
        env = emulator.isolated_env(probe_home)
        self.environment = {
            "run_id": self.run_id,
            "amiberry": emulator.binary_info(opts.binary, env),
            "host": envinfo.host(),
            "amitorture": envinfo.git_rev(),
            "kickstarts_dir": os.path.abspath(opts.kickstarts),
            "roms": romid.scan_dir(opts.kickstarts),
            "disk_source": None if opts.no_disk else (
                {"set": self.d.get("disk_set"), "path": self.d.get("disk_set"),
                 "build_id": " | ".join(adfmod.header_info(p).get("build_id") or "?" for _, p in self.d["disks"]),
                 "disks": [dict(adfmod.header_info(p), name=n) for n, p in self.d["disks"]]}
                if self.d.get("disks") else adfmod.header_info(self.d["adf_src"])),
            "params": opts.params,
            "workdir": work,
            "tmpdir": self.tmp,
            "options": {"timeout": opts.timeout, "hang": opts.hang, "no_disk": opts.no_disk, "selftest": opts.selftest,
                        "configs": opts.configs, "matrix": opts.matrix, "sets": opts.sets, "jobs": opts.jobs,
                        "show": getattr(opts, "show", False), "warp": opts.warp},
        }
        self.d["romset"] = romid.RomSet(self.environment["roms"])
        self._binfo = {opts.binary: self.environment["amiberry"]}

    def binary_info(self, binary):
        if binary not in self._binfo:
            env = emulator.isolated_env(os.path.join(self.tmp, "probe-home"))
            self._binfo[binary] = emulator.binary_info(binary, env)
        return self._binfo[binary]

    def run_entries(self, entries, binary=None, extra_options=None, on_result=None):
        """Runs entries (opts.jobs at a time). Returns results in entry order."""
        opts = self.opts
        results = [None] * len(entries)
        self.results_so_far = results      # what a Ctrl+C keeps

        def one(i, e):
            try:
                r = run_entry(opts, self.d, e, binary=binary, extra_options=extra_options)
            except RunnerError as ex:
                r = entry_error(e, ex)
            except Exception as ex:   # a runner bug must not lose the other entries
                import traceback
                r = entry_error(e, f"runner exception {type(ex).__name__}: {ex}")
                r["traceback"] = traceback.format_exc()[-3000:]
            if r["status"] in ("skip", "error") and "process" not in r:
                log(opts, f"{e['id']}: {r['status']}: {r['reason']}")
            r["isolation_changes_so_far"] = len([c for c in self.guard.check() if c["severity"] == "fail"])
            results[i] = r
            if on_result:
                on_result(r)

        jobs = min(opts.jobs, emulator.max_slots())
        if jobs < opts.jobs and not getattr(self, "_jobs_capped", False):
            self._jobs_capped = True
            log(opts, f"--jobs {opts.jobs} capped to {jobs}: machine-wide emulator slots (tools/amislot.py)")
        if jobs <= 1 or len(entries) <= 1:
            for i, e in enumerate(entries):
                one(i, e)
        else:
            emulator.run_parallel(lambda ie: one(*ie), list(enumerate(entries)), jobs)
        return results

    def close(self):
        self.guard.__exit__(None, None, None)
        self.environment["isolation"] = self.guard.report()
        if not self.opts.keep:
            _safe_rmtree(self.tmp, self.tmp_parent)
        else:
            log(self.opts, f"--keep: temp files kept in {self.tmp}")
        if not self.guard.ok:
            log(self.opts, "ISOLATION VIOLATED: the user's Amiberry dirs changed during the run:")
            for c in self.guard.changes:
                if c["severity"] == "fail":
                    log(self.opts, f"  {c['change']}: {c['path']}")
            for u in self.guard.user_instances.values():
                log(self.opts, f"  likely cause: an Amiberry not started by amitorture ran on your own config "
                    f"(pid {u['pid']}, {u['first_seen']}-{u.get('last_seen')}): {u['cmd']}")
        return self.environment["isolation"]


def select_entries(opts):
    try:
        entries = matrix.expand(opts.matrix)
    except ValueError as e:
        raise RunnerError(str(e))
    if opts.configs:
        # --config may name entries of any matrix
        pool = {e["id"]: e for e in entries}
        for eid, e in matrix.all_entries().items():
            pool.setdefault(eid, e)
        entries = matrix.select(list(pool.values()), opts.configs)
        if not entries:
            raise RunnerError(f"--config {opts.configs}: no matrix entry matches; see `amitorture matrix --matrix full`")
    return matrix.apply_overrides(entries, opts.sets)


def run(opts):
    """The `run` command: runs the selected entries, then JIT/engine parity,
    xfail, narrowing of failures, problem grouping, repro packs and the
    report. Returns the results dir and the summary."""
    from . import analysis, report
    run_id = new_run_id()
    outdir = unique_dir(os.path.join(os.path.abspath(opts.workdir), "results", run_id))
    run_id = os.path.basename(outdir)
    entries = select_entries(opts)
    ctx = Context(opts, outdir, run_id)
    results, extra = None, None
    try:
        log(opts, f"run {run_id}: matrix {opts.matrix}, {len(entries)} entr{'y' if len(entries) == 1 else 'ies'}, "
            f"results in {outdir}")
        try:
            results = ctx.run_entries(entries)
            extra = analysis.analyze_run(opts, ctx, entries, results)
        except (KeyboardInterrupt, emulator.Interrupted):
            # Ctrl+C: keep what finished. Interrupted during the entries: the
            # finished entries; during the analysis (narrowing launches more
            # emulators): every entry, without parity/narrowing.
            emulator.interrupt()
            emulator.stop_all()
            phase = "entries" if results is None else "analysis"
            if results is None:
                results = [r for r in getattr(ctx, "results_so_far", []) if r is not None]
            extra = None
            done = {r["id"] for r in results}
            ctx.environment["interrupted"] = {
                "during": phase, "finished": len(results), "selected": len(entries),
                "not_run": [e["id"] for e in entries if e["id"] not in done]}
            log(opts, f"interrupted during the {phase}: writing a partial report of {len(results)} of "
                f"{len(entries)} entries")
    finally:
        iso = ctx.close()
    ctx.environment["isolation"] = iso
    summary = report.write_all(outdir, ctx.environment, results, analysis=extra)
    log(opts, f"report: {os.path.join(outdir, 'REPORT.md')}")
    return outdir, summary
