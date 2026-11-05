#!/usr/bin/env python3
"""Smoke test of the amitorture disk in Amiberry, for one configuration.

    tests/smoke_disk.py [--amiberry BIN] [--adf disk/build/set/amitorture-1-core.adf] [--rom PATH|aros] [--model A1200]
                        [--cpu 68020] [--chipset aga] [--port 23101] [--timeout 120]
                        [--serial-out FILE] [-s option=value ...]
                        [--media CONTROLLER [--plain] [--size-mb N] [--no-adf]]
                        [--hdf FILE --controller CONTROLLER]

--media creates signed test-owned media (runner/amitorture/media.py, CONTRACT
section 11) in the run's temp dir: an ADF in DF1 and, for a controller
(gayle, a4000ide, a3000, a2091, a4091, uae; "none" for the ADF only), an RDB
hardfile (or --plain without RDB). --hdf attaches an existing hardfile.

It boots the ADF headless in a fresh, isolated home, reads the records over the
TCP serial port and checks them (CRC suffix, JSON, seq, hello first, done
last), then finds the mailbox in chip RAM over the IPC socket (READ_MEM) and
checks its header and ring against the serial bytes, and that the machine is
still alive after `done` (end_action 0: state 4, exec still dispatching).

Exit status 0 when every check passes.
"""

import argparse
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import tempfile
import threading
import time
import zlib

AMIBERRY = None    # --amiberry, else $AMIBERRY, else `amiberry` on PATH (set in main)
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LINE_RE = re.compile(rb"^(\{.*\}) \*([0-9a-f]{8})$")
MAGIC = b"AMITORTURE-MBOX!"
HELLO_FIELDS = ("build", "mode", "seed", "exec", "cpu", "fpu", "mmu", "probe")
PROBE_FIELDS = ("cpu", "cpu_rev", "fpu", "fpu_frame", "mmu", "addr24", "vbr", "cacr", "how",
                "agnus_id", "denise_id", "chipset", "ecs_agnus", "ecs_denise", "video",
                "exec_attn", "kick", "rom_base", "mem", "boards")
DONE_FIELDS = ("pass", "fail", "skip", "untestable", "aborted", "records")


class Emulator:
    def __init__(self, args, work):
        self.output = []
        env = dict(os.environ)
        for var, sub in (("HOME", ""), ("XDG_CONFIG_HOME", "config"), ("XDG_DATA_HOME", "data"),
                         ("XDG_RUNTIME_DIR", "run"), ("AMIBERRY_HOME_DIR", "amiberry")):
            path = os.path.join(work, sub)
            os.makedirs(path, mode=0o700, exist_ok=True)
            env[var] = path
        # SDL2 and SDL3 spellings: an inherited SDL_VIDEO_DRIVER (SDL3)
        # would otherwise override the offscreen driver.
        for k in [k for k in env if k.startswith("SDL_")]:
            del env[k]
        env["SDL_AUDIODRIVER"] = env["SDL_AUDIO_DRIVER"] = "dummy"
        for k in ("WAYLAND_DISPLAY", "DISPLAY", "XAUTHORITY"):
            env.pop(k, None)
        if args.show:
            # the user's own display settings; the isolated dirs stay
            # (runner/amitorture/emulator.py display_env)
            sys.path.insert(0, os.path.join(ROOT, "runner"))
            from amitorture.emulator import display_env
            env.update(display_env())
        else:
            env["SDL_VIDEODRIVER"] = env["SDL_VIDEO_DRIVER"] = "offscreen"
        adf = os.path.join(work, "disk.adf")       # Amiberry may write to the disk
        shutil.copyfile(args.adf, adf)
        opts = [f"floppy0={adf}", f"serial_port=TCP://127.0.0.1:{args.port}/wait",
                "ipc_disable_logging=true", "floppy_speed=100"]
        if args.rom != "aros":
            opts.append(f"kickstart_rom_file={args.rom}")
        if args.cpu:
            opts.append(f"cpu_model={args.cpu}")
        if args.chipset:
            opts.append(f"chipset={args.chipset}")
        opts += media_options(args, work)
        opts += args.s or []
        cmd = [AMIBERRY, "--model", args.model]
        for o in opts:
            cmd += ["-s", o]
        cmd += ["-G", "--log"]
        self.cmd = cmd
        self.proc = subprocess.Popen(cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     stdin=subprocess.DEVNULL)
        try:
            threading.Thread(target=self._drain, daemon=True).start()
        except BaseException:
            self.kill()
            raise

    def _drain(self):
        for raw in self.proc.stdout:
            self.output.append(raw.decode(errors="replace"))

    def endpoint(self, timeout=30):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline and self.proc.poll() is None:
            for line in list(self.output):
                m = re.search(r"IPC: Listening on (\S+)", line)
                if m:
                    return m.group(1)
            time.sleep(0.05)
        return None

    def kill(self):
        if self.proc.poll() is None:
            self.proc.kill()
            self.proc.wait()


class Screenshots:
    """Saves the emulator's screen every `every` seconds (IPC SCREENSHOT)."""

    def __init__(self, emu, outdir, every):
        self.emu, self.outdir, self.every = emu, os.path.abspath(outdir), every
        os.makedirs(self.outdir, exist_ok=True)
        self.n = 0
        self.done = threading.Event()
        self.thread = threading.Thread(target=self._run, daemon=True)
        self.thread.start()

    def shot(self, ipc):
        path = os.path.join(self.outdir, f"shot-{self.n:03d}.png")
        try:
            r = ipc.request(f"SCREENSHOT\t{path}")
        except OSError as e:
            print(f"info screenshot failed: {e}")
            return
        if r.startswith("OK"):
            print(f"info screenshot {path} at {time.monotonic() - self.t0:.0f} s")
            self.n += 1

    def _run(self):
        self.t0 = time.monotonic()
        ep = self.emu.endpoint(timeout=60)
        if not ep:
            return
        ipc = Ipc(ep)
        while not self.done.wait(self.every):
            self.shot(ipc)
        self.shot(ipc)

    def stop(self):
        self.done.set()
        self.thread.join(timeout=30)


def media_options(args, work):
    """Amiberry options for --media / --hdf (test-owned media, CONTRACT section 11)."""
    if not args.media and not args.hdf:
        return []
    sys.path.insert(0, os.path.join(ROOT, "runner"))
    from amitorture import media
    if args.hdf:
        lines = media.config_lines(args.controller or args.media, os.path.abspath(args.hdf), None, args.board_rom)
    else:
        c = None if args.media == "none" else args.media
        r = media.make_set(os.path.join(work, "media"), c, size_mb=args.size_mb, rdb=not args.plain,
                           adf=not args.no_adf, rom_path=args.board_rom, partition=args.partition)
        lines = r["lines"]
        for m in r["media"]:
            print(f"info media {m['kind']} {m['blocks']} blocks: {m['path']}")
    for line in lines:
        print(f"info media option {line}")
    return lines


class Ipc:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        self.sock.connect(path)
        self.sock.settimeout(10)
        self.buf = b""

    def request(self, line):
        self.sock.sendall((line + "\n").encode())
        while b"\n" not in self.buf:
            chunk = self.sock.recv(65536)
            if not chunk:
                raise OSError("IPC socket closed")
            self.buf += chunk
        reply, self.buf = self.buf.split(b"\n", 1)
        return reply.decode(errors="replace")

    def read(self, addr, width=4):
        r = self.request(f"READ_MEM\t{addr:#x}\t{width}")
        if not r.startswith("OK\t"):
            raise OSError(f"READ_MEM {addr:#x}: {r}")
        return int(r.split("\t")[1])

    def read_bytes(self, addr, n):
        out = bytearray()
        for a in range(addr, addr + n, 4):
            out += self.read(a).to_bytes(4, "big")
        return bytes(out[:n])


def read_serial(port, timeout, connect_timeout=60):
    deadline = time.monotonic() + connect_timeout
    while True:
        try:
            s = socket.create_connection(("127.0.0.1", port), timeout=1)
            break
        except OSError:
            if time.monotonic() > deadline:
                raise
            time.sleep(0.1)
    s.settimeout(0.5)
    data = b""
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        try:
            chunk = s.recv(65536)
        except socket.timeout:
            continue
        if not chunk:
            break
        data += chunk
        last = data.rstrip(b"\n").rsplit(b"\n", 1)[-1]
        if data.endswith(b"\n") and b'"t":"done"' in last:
            break
    return s, data


def strip_preamble(data):
    """Drop what the ROM printed on the serial port before the first record
    (AROS prints its ROMInfo lines there). Returns (preamble, records)."""
    i = 0 if data.startswith(b'{"v":') else data.find(b'\n{"v":') + 1
    if i <= 0 and not data.startswith(b'{"v":'):
        return data, b""
    return data[:i], data[i:]


def check_records(data, fail):
    records = []
    for n, raw in enumerate(data.split(b"\n")[:-1]):
        m = LINE_RE.match(raw)
        if not m:
            fail(f"line {n} is not '<json> *<crc>': {raw[:120]!r}")
            continue
        body, crc = m.group(1), m.group(2).decode()
        if f"{zlib.crc32(body) & 0xffffffff:08x}" != crc:
            fail(f"line {n}: CRC {crc} does not match")
        try:
            rec = json.loads(body)
        except ValueError as e:
            fail(f"line {n}: bad JSON ({e})")
            continue
        if rec.get("v") != 1 or rec.get("seq") != n or "t" not in rec:
            fail(f"line {n}: bad v/seq/t: {body[:80]!r}")
        records.append(rec)
    if not data.endswith(b"\n"):
        fail(f"serial output ends mid-line: {data[-80:]!r}")
    if not records:
        fail("no records on serial")
        return records
    hello, done = records[0], records[-1]
    if hello.get("t") != "hello":
        fail("first record is not hello")
    else:
        for f in HELLO_FIELDS:
            if f not in hello:
                fail(f"hello lacks {f}")
        for f in PROBE_FIELDS:
            if f not in hello.get("probe", {}):
                fail(f"hello.probe lacks {f}")
    if done.get("t") != "done":
        fail("last record is not done")
    else:
        for f in DONE_FIELDS:
            if f not in done:
                fail(f"done lacks {f}")
        if done.get("records") != len(records):
            fail(f"done.records = {done.get('records')}, received {len(records)}")
        if done.get("fail"):
            fail(f"done reports {done['fail']} failures")
        if done.get("aborted"):
            fail(f"done reports {done['aborted']} aborted modules")
        counts = {st: 0 for st in ("pass", "fail", "skip", "untestable")}
        for r in records:
            if r.get("t") == "case":
                st = r.get("st")
                if st not in counts:
                    fail(f"case {r.get('mod')}.{r.get('case')} has invalid status {st!r}")
                else:
                    counts[st] += 1
        if counts["fail"]:
            fail(f"case records report {counts['fail']} failures")
        for st, count in counts.items():
            if done.get(st) != count:
                fail(f"done.{st} = {done.get(st)}, case records report {count}")
    return records


def find_mailbox(ipc, chip_top):
    first = int.from_bytes(MAGIC[:4], "big")
    for addr in range(0, chip_top, 16):
        if ipc.read(addr) == first and ipc.read_bytes(addr, 16) == MAGIC:
            return addr
    return None


def main():
    ap = argparse.ArgumentParser(description="amitorture disk smoke test (one config)")
    ap.add_argument("--adf", default=os.path.join(ROOT, "disk", "build", "set", "amitorture-1-core.adf"),
                    help="ADF to boot (default: the core disk of the set; `make -C disk` builds the set)")
    ap.add_argument("--amiberry", default=os.environ.get("AMIBERRY") or shutil.which("amiberry"),
                    help="Amiberry binary (default: $AMIBERRY, else 'amiberry' on PATH; never built)")
    ap.add_argument("--rom", default="aros", help="Kickstart ROM path, or 'aros' for Amiberry's AROS ROM")
    ap.add_argument("--model", default="A500")
    ap.add_argument("--cpu")
    ap.add_argument("--chipset")
    ap.add_argument("--port", type=int, default=23101)
    ap.add_argument("--timeout", type=float, default=120, help="seconds to wait for done")
    ap.add_argument("--serial-out", help="save the raw serial bytes here")
    ap.add_argument("--keep", action="store_true", help="keep the temp dir (emulator log in it)")
    ap.add_argument("-s", action="append", metavar="OPTION=VALUE", help="extra Amiberry option")
    ap.add_argument("--media", help="create test-owned media for this controller (or 'none': DF1 ADF only)")
    ap.add_argument("--hdf", help="attach this (signed) hardfile with --controller")
    ap.add_argument("--controller", help="controller for --hdf")
    ap.add_argument("--plain", action="store_true", help="--media: hardfile without RDB")
    ap.add_argument("--partition", action="store_true", help="--media: RDB with one non-mountable partition")
    ap.add_argument("--size-mb", type=float, default=8, help="--media: hardfile size")
    ap.add_argument("--no-adf", action="store_true", help="--media: no DF1 ADF")
    ap.add_argument("--board-rom", help="--media/--hdf: A2091/A4091 boot ROM (default: from ~/Amiberry/ROMs)")
    ap.add_argument("--show", action="store_true", help="show the emulator window on your desktop")
    ap.add_argument("--screenshots", metavar="DIR",
                    help="save the emulator's screen to DIR/shot-NNN.png every --shot-every seconds "
                         "(the disk's status screen), and once more after done")
    ap.add_argument("--shot-every", type=float, default=5.0, metavar="S")
    args = ap.parse_args()
    global AMIBERRY
    if not args.amiberry or not os.access(args.amiberry, os.X_OK):
        sys.exit("no Amiberry binary: pass --amiberry PATH, set $AMIBERRY, or put 'amiberry' on PATH")
    AMIBERRY = os.path.abspath(args.amiberry)
    if not 23101 <= args.port <= 23999:
        sys.exit("use ports 23101-23999 (your assigned range)")

    failures = []

    def fail(msg):
        failures.append(msg)
        print(f"FAIL {msg}")

    emu = shots = ser = None
    try:
        for attempt in range(3):
            # Keep temp files off a potentially small /tmp tmpfs.
            tmproot = os.path.join(ROOT, "work", "tmp")
            os.makedirs(tmproot, exist_ok=True)
            work = tempfile.mkdtemp(prefix="amt-smoke-", dir=tmproot)
            emu = Emulator(args, work)
            time.sleep(2)
            if emu.proc.poll() is None or "SDL could not initialize" not in "".join(emu.output):
                break
            print(f"info emulator did not start (SDL), retry {attempt + 1}")
            emu.kill()
            if attempt == 2:
                fail("SDL could not initialize after three attempts")
                return finish(failures, emu, work, args)
            shutil.rmtree(work)
            emu = None
        t0 = time.monotonic()
        shots = Screenshots(emu, args.screenshots, args.shot_every) if args.screenshots else None
        try:
            ser, data = read_serial(args.port, args.timeout)
        except OSError as e:
            fail(f"serial: {e}")
            data, ser = b"", None
        elapsed = time.monotonic() - t0
        if args.serial_out:
            with open(args.serial_out, "wb") as f:
                f.write(data)
        pre, data = strip_preamble(data)
        if pre:
            n_pre = pre.count(b"\n")
            print(f"info {n_pre} line(s) of ROM output before the first record (ignored)")
        sys.stdout.write(data.decode(errors="replace"))
        records = check_records(data, fail)
        print(f"info {len(records)} records in {elapsed:.1f}s")

        ep = emu.endpoint()
        if not ep:
            fail("IPC endpoint never announced")
            return finish(failures, emu, work, args)
        ipc = Ipc(ep)
        time.sleep(0.5)            # let state 2 -> 4 land
        chip_top = 0x200000
        mb = find_mailbox(ipc, chip_top)
        if mb is None:
            fail("mailbox magic not found in chip RAM")
            return finish(failures, emu, work, args)
        ver, state, beat, written, ring_size = (ipc.read(mb + o) for o in (16, 20, 24, 28, 32))
        print(f"info mailbox at {mb:#08x}: version {ver} state {state} heartbeat {beat} "
              f"written {written} ring_size {ring_size}")
        if ver != 1:
            fail(f"mailbox version {ver}")
        if state != 4:
            fail(f"mailbox state {state}, expected 4 (halted after done)")
        if written != len(data):
            fail(f"mailbox written {written} != {len(data)} serial bytes")
        if beat < len(records):
            fail(f"heartbeat {beat} < {len(records)} records")
        if ring_size and written:
            raw = ipc.read_bytes(mb + 48, ring_size)
            if written <= ring_size:
                ring = raw[:written]
            else:
                start = written % ring_size
                ring = raw[start:] + raw[:start]
            n = len(ring)
            if ring != data[-n:]:
                fail("mailbox ring differs from the serial bytes")
            else:
                print(f"info mailbox ring matches the last {n} serial bytes")

        # still alive after done: emulator up, exec still dispatching
        sysbase = ipc.read(4)
        idle1, disp1 = ipc.read(sysbase + 280), ipc.read(sysbase + 284)
        time.sleep(2)
        idle2, disp2 = ipc.read(sysbase + 280), ipc.read(sysbase + 284)
        status = ipc.request("GET_STATUS")
        if emu.proc.poll() is not None or not status.startswith("OK"):
            fail(f"emulator not alive after done: {status}")
        elif (idle1, disp1) == (idle2, disp2):
            fail(f"exec looks stopped after done (IdleCount {idle1}, DispCount {disp1} unchanged)")
        else:
            print(f"info alive after done: IdleCount {idle1}->{idle2} DispCount {disp1}->{disp2}")
        if ipc.read(mb + 24) != beat:
            print("info heartbeat still moving after done")
    finally:
        try:
            if ser:
                ser.close()
            if shots:
                shots.stop()
        finally:
            if emu:
                emu.kill()
    return finish(failures, emu, work, args)


def finish(failures, emu, work, args):
    emu.kill()
    with open(os.path.join(work, "amiberry.log"), "w") as f:
        f.write("".join(emu.output))
    if failures:
        print(f"smoke test FAILED ({len(failures)}): emulator log in {work}/amiberry.log")
        return 1
    if not args.keep:
        shutil.rmtree(work, ignore_errors=True)
    else:
        print(f"info kept {work}")
    print("smoke test passed")
    return 0


if __name__ == "__main__":
    # at most nproc emulators machine-wide (tools/amislot.py)
    sys.path.insert(0, os.path.join(ROOT, "tools"))
    from amislot import emulator_slot
    try:
        with emulator_slot():
            rc = main()       # main's finally stops the emulator on Ctrl+C
    except KeyboardInterrupt:
        print("smoke_disk: interrupted, emulator stopped", file=sys.stderr)
        rc = 130
    sys.exit(rc)
