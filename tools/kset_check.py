#!/usr/bin/env python3
"""Check GCC assembly output of a bench kernel set for instructions its
target CPUs cannot execute (see docs/BENCH.md, "Kernel sets").

  k000          68000 instructions and addressing modes only.
  k020, k020s   68020+ integer code that the 68060 runs without trapping to
                emulation software: no MOVEP, CAS2, CHK2, CMP2, no 64-bit
                MULx.L/DIVx.L, and no FPU instructions at all.
  k020f         as k020, plus FPU instructions the 68040 and 68060 both
                implement in hardware.

Usage: kset_check.py --set SET file.s...   (exit status 1 on a violation)
"""

import argparse
import re
import sys

M68000 = {
    "abcd", "add", "adda", "addi", "addq", "addx", "and", "andi", "asl", "asr",
    "bchg", "bclr", "bset", "btst", "chk", "clr", "cmp", "cmpa", "cmpi", "cmpm",
    "divs", "divu", "eor", "eori", "exg", "ext", "illegal", "jmp", "jsr", "lea",
    "link", "lsl", "lsr", "move", "movea", "movem", "movep", "moveq", "muls",
    "mulu", "nbcd", "neg", "negx", "nop", "not", "or", "ori", "pea", "rol", "ror",
    "roxl", "roxr", "rtr", "rts", "sbcd", "sub", "suba", "subi", "subq", "subx",
    "swap", "tas", "trap", "trapv", "tst", "unlk",
    "jra", "jbra", "jbsr", "bra", "bsr",
}
CC = ("t", "f", "hi", "ls", "cc", "cs", "ne", "eq", "vc", "vs", "pl", "mi",
      "ge", "lt", "gt", "le", "hs", "lo")
for _c in CC:
    M68000.update({"b" + _c, "j" + _c, "jb" + _c, "db" + _c, "s" + _c})
M68000.add("dbra")

# Word-only on the 68000 (the .l forms are 68020+).
WORD_ONLY = {"muls", "mulu", "divs", "divu", "link"}

INT_060_FORBIDDEN = {"movep", "cas2", "chk2", "cmp2"}
FPU_FORBIDDEN = {
    # transcendentals and friends, emulated on both the 040 and 060
    "facos", "fasin", "fatan", "fatanh", "fcos", "fcosh", "fetox", "fetoxm1",
    "fgetexp", "fgetman", "fint", "fintrz", "flog10", "flog2", "flogn",
    "flognp1", "fmod", "frem", "fscale", "fsgldiv", "fsglmul", "fsin",
    "fsincos", "fsinh", "ftan", "ftanh", "ftentox", "ftwotox", "fmovecr",
}
FPU_FORBIDDEN_PREFIX = ("fs", "fdb", "ftrap")   # FScc, FDBcc, FTRAPcc (060)
FPU_ALLOWED_FS = {"fsub", "fsqrt", "fsadd", "fssub", "fsmul", "fsdiv",
                  "fsabs", "fsneg", "fsmove", "fssqrt", "fsave"}

SETS = ("k000", "k020", "k020f", "k020s")


def instructions(text):
    """Yield (line number, mnemonic, size, operands) for each instruction."""
    for n, line in enumerate(text.splitlines(), 1):
        line = line.split("|", 1)[0].split(";", 1)[0]
        if not line[:1].isspace():
            line = line.split(":", 1)[1] if ":" in line else ""
        parts = line.split(None, 1)
        if not parts or parts[0].startswith(".") or parts[0].endswith(":"):
            continue
        op = parts[0].lower()
        mnem, _, size = op.partition(".")
        yield n, mnem, size, parts[1].strip() if len(parts) > 1 else ""


def problems(text, kset):
    out = []
    for n, mnem, size, args in instructions(text):
        def bad(why):
            out.append(f"{n}: {mnem}{'.' + size if size else ''} {args}: {why}")
        if kset == "k000":
            if mnem not in M68000:
                bad("not a 68000 instruction")
            elif mnem in WORD_ONLY and size not in ("", "w"):
                bad("68020+ form")
            elif mnem == "ext" and size == "b":
                bad("68020+ form")
            elif "[" in args or re.search(r"\*\s*[248]", args):
                bad("68020+ addressing mode")
            continue
        if mnem in INT_060_FORBIDDEN:
            bad("not implemented on the 68060")
        elif mnem in ("muls", "mulu") and size == "l" and ":" in args:
            bad("64-bit multiply, not implemented on the 68060")
        elif mnem in ("divs", "divu") and size == "l" and ":" in args:
            bad("64-bit divide, not implemented on the 68060")
        elif mnem.startswith("f"):
            if kset != "k020f":
                bad("FPU instruction in an integer set")
            elif mnem in FPU_FORBIDDEN:
                bad("emulated on the 68040/68060")
            elif mnem.startswith(FPU_FORBIDDEN_PREFIX) and mnem not in FPU_ALLOWED_FS:
                bad("emulated on the 68060")
            elif size == "p":
                bad("packed decimal, emulated on the 68040/68060")
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--set", required=True, choices=SETS)
    ap.add_argument("files", nargs="+")
    a = ap.parse_args(argv)
    status = 0
    for path in a.files:
        with open(path) as f:
            for p in problems(f.read(), a.set):
                print(f"{path}:{p}", file=sys.stderr)
                status = 1
    return status


if __name__ == "__main__":
    sys.exit(main())
