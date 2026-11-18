#!/usr/bin/env python3
"""Run bench kernel sets under Musashi (the `machine68k` Python package) to
get each kernel's checksum, cycles per loop and arena use, independently of
any Amiga emulator. Used to fill in disk/bench/kernels/kexpect.h and to size
the inputs in ksize.h (see docs/BENCH.md).

  kset_sim.py [--cpu 68000|68020|68030|68040] [--loops N] [--json] SET.o...

SET.o is a partially linked kernel set from disk/bench/kernels.mk. It is
linked into a hunk executable, loaded flat at 0x1000 and its kset_api is
driven exactly like the bench harness does: setup, run (N loops, arena
released after each), verify, teardown. Checksums are compared with
kexpect.h (tools/kset_expect.py). Musashi's FPU ignores the FPCR rounding
precision, so k020f's float kernels differ there; that is not a failure.
"""

import argparse
import json
import os
import re
import struct
import subprocess
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import mkadf  # noqa: E402

TOOLCHAIN = os.environ.get("AMIGA_TOOLCHAIN", "/opt/amiga/bin")
LOAD = 0x1000
STACK = 16 * 1024
GROUPS = ["compression", "graphics", "text", "crypto", "memory"]
KEXPECT = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                       "disk", "bench", "kernels", "kexpect.h")


def load_expect(path=KEXPECT):
    """{set: {kernel: checksum}} from kexpect.h."""
    out, cur = {}, None
    with open(path) as f:
        for line in f:
            m = re.match(r"static const struct kexpect kexpect_(\w+)\[\]", line)
            if m:
                cur = out.setdefault(m.group(1), {})
            m = re.match(r'\s*\{"([^"]+)", 0x([0-9a-f]{8})UL\}', line)
            if m and cur is not None:
                cur[m.group(1)] = m.group(2)
    return out


class Crash(Exception):
    pass


def link(obj):
    fd, exe = tempfile.mkstemp(suffix=".exe")
    os.close(fd)
    subprocess.run([os.path.join(TOOLCHAIN, "m68k-amigaos-ld"), "-o", exe, obj],
                   check=True)
    with open(exe, "rb") as f:
        blob = f.read()
    os.unlink(exe)
    return mkadf.parse_hunks(blob)


class Sim:
    def __init__(self, obj, cpu, ram_kib):
        import machine68k as m
        self.m = m
        self.mc = m.Machine(getattr(m.CPUType, "M" + cpu), ram_kib)
        self.mem, self.cpu = self.mc.mem, self.mc.cpu
        self.ram = ram_kib * 1024
        hunks = link(obj)
        addr = LOAD
        for h in hunks:
            h.base = addr
            addr = mkadf.align(addr + h.alloc, 16)
        for h in hunks:
            img = bytearray(h.data)
            for off, tgt in h.relocs:
                v = struct.unpack_from(">I", img, off)[0] + hunks[tgt].base
                struct.pack_into(">I", img, off, v & 0xFFFFFFFF)
            if img:
                self.mem.w_block(h.base, bytes(img))
        self.image_end = addr
        sym = None
        for h in hunks:
            for name, off in h.symbols.items():
                if name.endswith("_kset"):
                    sym = h.base + off
        if sym is None:
            raise SystemExit(f"{obj}: no *_kset symbol")
        self.kset = sym
        self.result = None

        def done(op, pc):
            return self.mc.create_execute_end("ret")

        def crash(op, pc):
            self.crashed = True
            return self.mc.create_execute_end("crash")
        t_ret = self.mc.traps.alloc(done)
        t_crash = self.mc.traps.alloc(crash)
        self.ret_addr = 0x400
        self.mem.w16(self.ret_addr, 0xA000 | t_ret)
        self.mem.w16(0x402, 0xA000 | t_crash)
        for v in range(2, 64):
            self.mem.w32(v * 4, 0x402)
        self.stack_top = self.ram - 16
        self.mem.w32(0, self.stack_top)
        self.mem.w32(4, self.ret_addr)
        self.cpu.pulse_reset()

    def call(self, fn, *args):
        sp = self.stack_top
        for a in reversed(args):
            sp -= 4
            self.mem.w32(sp, a & 0xFFFFFFFF)
        sp -= 4
        self.mem.w32(sp, self.ret_addr)
        self.cpu.w_sp(sp)
        self.cpu.w_pc(fn)
        self.crashed = False
        cycles = 0
        while True:
            r = self.mc.execute(100_000_000)
            cycles += r.cycles
            if r.result is not None:
                break
        if self.crashed:
            raise Crash(f"exception at pc~{self.cpu.r_pc():#x}")
        return self.cpu.r_reg(0), cycles

    def r32(self, a):
        return self.mem.r32(a)

    def cstr(self, a):
        return self.mem.r_cstr(a) if a else ""


def run_set(obj, cpu, loops, ram_kib):
    s = Sim(obj, cpu, ram_kib)
    k = s.kset
    version, n = s.mem.r16(k), s.mem.r16(k + 2)
    benches = s.r32(k + 4)
    init, mark, release, high, failed = (s.r32(k + 8 + 4 * i) for i in range(5))
    arena = mkadf.align(s.image_end, 16)
    arena_size = s.stack_top - STACK - arena
    out = []
    for i in range(n):
        b = s.r32(benches + 4 * i)
        name = s.cstr(s.r32(b))
        unit = s.cstr(s.r32(b + 12))
        group = s.r32(b + 8)
        setup, run, verify, teardown = (s.r32(b + 24 + 4 * j) for j in range(4))
        rec = {"set": os.path.basename(obj)[:-2], "cpu": cpu, "name": name,
               "group": GROUPS[group] if group < len(GROUPS) else group, "unit": unit}
        try:
            s.call(init, arena, arena_size)
            m0, _ = s.call(mark)
            state, c_setup = s.call(setup)
            if not state:
                rec["error"] = "setup failed"
                out.append(rec)
                continue
            m1, _ = s.call(mark)
            cyc = []
            res = 0
            for _ in range(loops):
                res, c = s.call(run, state)
                cyc.append(c)
                s.call(release, m1)
            chk_addr = s.stack_top - STACK // 2
            s.mem.w32(chk_addr, 0)
            ok, c_verify = s.call(verify, state, res, chk_addr)
            rec.update({
                "ok": bool(ok & 0xFF), "check": f"{s.r32(chk_addr):08x}",
                "cycles": cyc, "setup_cycles": c_setup, "verify_cycles": c_verify,
                "setup_bytes": m1 - m0,
            })
            if teardown:
                s.call(teardown, state)
            hi, _ = s.call(high)
            oom, _ = s.call(failed)
            rec["arena_peak"] = hi
            rec["oom"] = bool(oom)
        except Crash as e:
            rec["error"] = str(e)
        out.append(rec)
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--cpu", default="68000", choices=["68000", "68020", "68030", "68040"])
    ap.add_argument("--loops", type=int, default=2)
    ap.add_argument("--ram", type=int, default=8192, help="RAM in KiB")
    ap.add_argument("--json", action="store_true")
    ap.add_argument("sets", nargs="+")
    a = ap.parse_args(argv)
    expect = load_expect() if os.path.exists(KEXPECT) else {}
    allrecs = []
    for obj in a.sets:
        recs = run_set(obj, a.cpu, a.loops, a.ram)
        for r in recs:
            want = expect.get(r["set"], {}).get(r["name"])
            if "check" in r and want is not None:
                r["expect"] = want
                r["match"] = want == r["check"]
        allrecs += recs
        if a.json:
            continue
        for r in recs:
            if "error" in r:
                print(f"{r['set']:6} {r['name']:20} ERROR {r['error']}")
                continue
            match = {True: "=kexpect", False: "!=kexpect", None: ""}[r.get("match")]
            print(f"{r['set']:6} {r['name']:20} {'ok ' if r['ok'] else 'BAD'} {r['check']}{match:>10} "
                  f"loop {r['cycles'][-1]:>10} cyc  setup {r['setup_cycles']:>10}  "
                  f"verify {r['verify_cycles']:>9}  peak {r['arena_peak']:>7}"
                  f"{'  OOM' if r['oom'] else ''}")
    if a.json:
        json.dump(allrecs, sys.stdout, indent=1)
    # Musashi's FPU ignores the FPCR rounding precision, so k020f's float
    # kernels don't give the IEEE single results there; they're not counted.
    def good(r):
        return r.get("ok") and (r.get("match") is not False or r["set"] == "k020f")
    return 0 if all(good(r) for r in allrecs) else 1


if __name__ == "__main__":
    sys.exit(main())
