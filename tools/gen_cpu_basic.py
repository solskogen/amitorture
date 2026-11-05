#!/usr/bin/env python3
"""Generator for the cpu_basic module (module id 2) of amitorture.

usage: tools/gen_cpu_basic.py [--asm disk/modules/cpu_basic_tab.s] [--doc docs/cases/cpu_basic.md]

It holds an independent Python reference model of the 680x0 integer
instructions (written from the M68000 Family Programmer's Reference Manual,
"PRM", and the MC68020/030/040/060 user manuals) and emits:

  * disk/modules/cpu_basic_tab.s: one code snippet per instruction form
    ("template") plus a table of test vectors: the input registers/CCR/memory
    and the architecturally expected outputs, with a per-vector mask of the
    defined CCR bits;
  * docs/cases/cpu_basic.md: one line per case.

The guest (cpu_basic.c + cpu_basic_exec.s) loads the inputs, runs the snippet,
and compares every register, the CCR (masked) and a memory window with the
table. Nothing in this file looks at what an emulator does.

Slot names (inputs and outputs of a template):
  d0..d7, a0..a6   registers; a suffix '@' means the value is an offset from M
                   (the base of the module's memory window in the any-pool),
                   '^' means it is an offset from the template's anchor label
  ccr              input: CCR value; output: (value, mask of defined bits)
  m<off>.<b|w|l>   memory at M+off (off signed), optional '@'/'^' as above
Registers that are not outputs must keep their input (or background) value;
window bytes that are not outputs must keep their initial value.
"""
import argparse
import itertools
import re
import os
import sys

M32 = 0xFFFFFFFF
X, N, Z, V, C = 0x10, 0x08, 0x04, 0x02, 0x01
SZ = {'b': 1, 'w': 2, 'l': 4}
SN = {1: 'b', 2: 'w', 4: 'l'}

# CPU ids as in disk/probe/probe.h (AMT_CPU_*)
CPUS = ['68000', '68010', '68EC020', '68020', '68EC030', '68030',
        '68EC040', '68LC040', '68040', '68EC060', '68LC060', '68060']
ALL = 0xFFF
C010 = 0xFFE            # 68010 and later
C020 = 0xFFC            # 68020 and later
C000_010 = 0x003
NO060 = 0x1FF           # everything but the 68060 family
C020N60 = C020 & NO060  # 68020/030/040 (all variants)
C060 = 0xE00


def msk(s):
    return (1 << (8 * s)) - 1


def msb(s):
    return 1 << (8 * s - 1)


def sx(v, s):
    v &= msk(s)
    return v - (1 << (8 * s)) if v & msb(s) else v


def u32(v):
    return v & M32


def nz(r, s):
    r &= msk(s)
    return (N if r & msb(s) else 0) | (Z if r == 0 else 0)


def setlo(full, val, s):
    m = msk(s)
    return (full & ~m & M32) | (val & m)


def bg(off):
    """background byte of the memory window at M+off (cpu_basic.c uses the same)"""
    return (off * 0x3B + 0x5A) & 0xFF


# --------------------------------------------------------------------------
# slots

class Slot:
    def __init__(self, name):
        self.name = name
        base = name
        self.rel = 0
        if name.endswith('@'):
            self.rel, base = 1, name[:-1]
        elif name.endswith('^'):
            self.rel, base = 2, name[:-1]
        self.base = base
        if base == 'ccr':
            self.kind, code = 'ccr', 15
        elif base[0] in 'da' and len(base) == 2 and base[1].isdigit():
            self.kind = 'reg'
            code = int(base[1]) + (8 if base[0] == 'a' else 0)
            assert code < 15, name
        elif base[0] == 'm':
            o, s = base[1:].split('.')
            self.kind = 'mem'
            self.off = int(o)
            self.size = SZ[s]
            assert -1024 <= self.off < 1024
            code = 0x8000 | ({1: 0, 2: 1, 4: 2}[self.size] << 11) | (self.off & 0x7FF)
        else:
            raise ValueError(name)
        if self.rel == 1:
            code |= 0x4000
        elif self.rel == 2:
            code |= 0x2000
        self.code = code


class Mem:
    """byte memory of the window, offsets relative to M"""
    def __init__(self, d):
        self.b = {}
        for k, v in d.items():
            s = Slot(k)
            if s.kind == 'mem':
                self.wr(s.off, s.size, v)

    def rb(self, o):
        return self.b.get(o, bg(o))

    def rd(self, o, s):
        v = 0
        for i in range(s):
            v = (v << 8) | self.rb(o + i)
        return v

    def wr(self, o, s, v):
        for i in range(s):
            self.b[o + i] = (v >> (8 * (s - 1 - i))) & 0xFF

    def slot(self, name):
        s = Slot(name)
        return self.rd(s.off, s.size)


# --------------------------------------------------------------------------
# templates and cases

cases = []
templates = []


class Case:
    def __init__(self, cid, rule, fail, intent):
        self.id, self.rule, self.fail, self.intent = cid, rule, fail, intent
        self.tpls = []


cur_case = None


def case(cid, intent, rule, fail="emulation of this instruction family is wrong"):
    global cur_case
    for c in cases:
        assert c.id != cid, cid
    cur_case = Case(cid, rule, fail, intent)
    cases.append(cur_case)


class Tpl:
    pass


def tpl(asm, ins, outs, fn, inputs, cpu=ALL, text=None, win=None):
    """asm: snippet body (lines; 'ANCHOR' is replaced by the anchor label).
    fn(d) -> dict with every output slot (ccr as (value, mask))."""
    t = Tpl()
    t.asm = [l for l in (asm.split('\n') if isinstance(asm, str) else asm) if l.strip()]
    t.ins = [Slot(s) for s in ins]
    t.outs = [Slot(s) for s in outs]
    t.cpu = cpu
    t.anchor = any('ANCHOR' in l for l in t.asm)
    if text is None:
        text = ' / '.join(l.strip() for l in t.asm if not l.strip().startswith('.')
                          and not l.strip().startswith('ANCHOR'))
    t.text = text
    t.vecs = []
    for iv in inputs:
        assert len(iv) == len(ins), (text, iv)
        d = dict(zip(ins, iv))
        o = fn(d)
        ov = []
        for s in outs:
            v = o[s]
            if s == 'ccr':
                val, m = v
                v = ((m & 0x1F) << 8) | (val & m & 0x1F)
            ov.append(u32(v))
        t.vecs.append([u32(v) for v in iv] + ov)
    lo, hi = None, None
    for s in t.ins + t.outs:
        if s.kind == 'mem':
            a, b = s.off, s.off + s.size
            lo = a if lo is None else min(lo, a)
            hi = b if hi is None else max(hi, b)
    if win:
        t.win = win
    elif lo is not None:
        lo -= 4
        hi += 4
        t.win = (lo, hi - lo)
    else:
        t.win = (0, 0)
    assert t.win[1] <= 256, text
    assert t.vecs, text
    t.case = cur_case
    cur_case.tpls.append(t)
    templates.append(t)
    return t


def ccr_out(val, m=0x1F):
    return (val & 0x1F, m)


# --------------------------------------------------------------------------
# operand sets

OPS = {1: [0x00, 0x01, 0x7F, 0x80, 0xFF, 0x55, 0xAA, 0xFE],
       2: [0x0000, 0x0001, 0x007F, 0x0080, 0x7FFF, 0x8000, 0xFFFF, 0x5555, 0xAAAA, 0xFFFE],
       4: [0x00000000, 0x00000001, 0xFFFFFFFF, 0x0000007F, 0x00000080, 0x00007FFF,
           0x00008000, 0x7FFFFFFF, 0x80000000, 0x55555555, 0xAAAAAAAA, 0x0000FFFF]}
SMALL = {1: [0x00, 0x01, 0x7F, 0x80, 0xFF, 0xAA],
         2: [0x0000, 0x0001, 0x7FFF, 0x8000, 0xFFFF, 0x5555],
         4: [0x00000000, 0x00000001, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xAAAAAAAA]}
HI = {1: [0x00000000, 0xFFFFFF00, 0x5A3C9600],
      2: [0x00000000, 0xFFFF0000, 0x5A3C0000],
      4: [0, 0, 0]}
# CCR inputs for instructions that do not read the CCR: cycled so every
# defined flag is seen both set and clear on input, and X both ways
ALT = [0x00, 0x1F, 0x10, 0x0F, 0x15, 0x0A]
# X/Z input combinations (with the other flags both ways) for ADDX & co
XZ = [0x00, 0x04, 0x10, 0x14, 0x0B, 0x0F, 0x1B, 0x1F]


def rv(v, s, k):
    """register value carrying the operand in its low s bytes"""
    return HI[s][k % 3] | (v & msk(s))


def pairs(s, a=None, b=None):
    a = a or OPS[s]
    b = b or OPS[s]
    return [(rv(x, s, i), rv(y, s, i + 1)) for i, (x, y) in enumerate(itertools.product(a, b))]


def with_alt(rows, alt=ALT):
    return [tuple(r) + (alt[i % len(alt)],) for i, r in enumerate(rows)]


def with_xz(rows):
    out = []
    for i, r in enumerate(rows):
        for j in range(4):
            out.append(tuple(r) + (XZ[j + 4 * ((i + j) & 1)],))
    return out


def singles(s, ops=None):
    ops = ops or OPS[s]
    return [(rv(x, s, i),) for i, x in enumerate(ops)]


# --------------------------------------------------------------------------
# reference semantics

def f_add(s, d, src, xin=0, zin=1, ext=False):
    m = msk(s)
    d &= m
    src &= m
    r = (d + src + xin) & m
    c = (d + src + xin) > m
    v = ((~(d ^ src)) & (d ^ r)) & msb(s)
    if ext:
        z = Z if (r == 0 and zin) else 0
    else:
        z = Z if r == 0 else 0
    f = (N if r & msb(s) else 0) | z | (V if v else 0) | ((C | X) if c else 0)
    return r, f


def f_sub(s, d, src, xin=0, zin=1, ext=False):
    m = msk(s)
    d &= m
    src &= m
    r = (d - src - xin) & m
    c = (src + xin) > d
    v = ((d ^ src) & (d ^ r)) & msb(s)
    if ext:
        z = Z if (r == 0 and zin) else 0
    else:
        z = Z if r == 0 else 0
    f = (N if r & msb(s) else 0) | z | (V if v else 0) | ((C | X) if c else 0)
    return r, f


def f_logic(s, r):
    return nz(r, s)     # V=C=0, X unchanged


def cond(cc, ccr):
    n, z, v, c = bool(ccr & N), bool(ccr & Z), bool(ccr & V), bool(ccr & C)
    return [True, False, not c and not z, c or z, not c, c, not z, z,
            not v, v, not n, n, n == v, n != v, n == v and not z, z or n != v][cc]


CCN = ['t', 'f', 'hi', 'ls', 'cc', 'cs', 'ne', 'eq', 'vc', 'vs', 'pl', 'mi', 'ge', 'lt', 'gt', 'le']


def f_shift(op, s, val, cnt, xin):
    """returns (result, ccr) for ASL/ASR/LSL/LSR/ROL/ROR/ROXL/ROXR by cnt
    (cnt already reduced modulo 64 for register counts)"""
    m = msk(s)
    hb = msb(s)
    r = val & m
    xin = 1 if xin else 0
    if cnt == 0:
        if op in ('roxl', 'roxr'):
            return r, nz(r, s) | (C if xin else 0) | (X if xin else 0)
        return r, nz(r, s) | (X if xin else 0)
    c = 0
    v = 0
    x = xin
    for _ in range(cnt):
        if op in ('asl', 'lsl'):
            c = 1 if r & hb else 0
            nr = (r << 1) & m
            if op == 'asl' and ((nr ^ r) & hb):
                v = 1
            r = nr
        elif op == 'asr':
            c = r & 1
            r = (r >> 1) | (r & hb)
        elif op == 'lsr':
            c = r & 1
            r >>= 1
        elif op == 'rol':
            c = 1 if r & hb else 0
            r = ((r << 1) | c) & m
        elif op == 'ror':
            c = r & 1
            r = (r >> 1) | (hb if c else 0)
        elif op == 'roxl':
            c = 1 if r & hb else 0
            r = ((r << 1) | x) & m
            x = c
        elif op == 'roxr':
            c = r & 1
            r = (r >> 1) | (hb if x else 0)
            x = c
    f = nz(r, s) | (V if v else 0) | (C if c else 0)
    if op in ('rol', 'ror'):
        f |= X if xin else 0
    else:
        f |= X if c else 0
    return r, f


def bcd2i(b):
    return (b >> 4) * 10 + (b & 15)


def i2bcd(i):
    return ((i // 10) << 4) | (i % 10)


# --------------------------------------------------------------------------
# families

def gen_move():
    case('move', "MOVE.B/.W/.L Dn,Dn for boundary operands",
         "PRM MOVE: N,Z from the moved data, V=C=0, X unaffected; only the low byte/word of a data-register destination changes")
    for s in (1, 2, 4):
        sn = SN[s]
        rows = with_alt([(r[0], r[1]) for r in pairs(s, SMALL[s], OPS[s])])
        tpl(f"\tmove.{sn}\td1,d0", ['d0', 'd1', 'ccr'], ['d0', 'ccr'],
            lambda d, s=s: {'d0': setlo(d['d0'], d['d1'], s),
                            'ccr': ccr_out((d['ccr'] & X) | nz(d['d1'], s))}, rows)
    case('movea', "MOVEA.W/.L to an address register",
         "PRM MOVEA: word source is sign-extended to 32 bits; condition codes unaffected")
    for s in (2, 4):
        rows = with_alt(singles(s))
        tpl(f"\tmovea.{SN[s]}\td1,a0", ['d1', 'ccr'], ['a0', 'ccr'],
            lambda d, s=s: {'a0': u32(sx(d['d1'], s)), 'ccr': ccr_out(d['ccr'])}, rows)
    case('moveq', "MOVEQ #imm,Dn for 0,1,-1,$7F,$80",
         "PRM MOVEQ: 8-bit immediate sign-extended to 32 bits; N,Z set, V=C=0, X unaffected")
    for k in (0, 1, -1, 127, -128, 0x55):
        tpl(f"\tmoveq\t#{k},d0", ['d0', 'ccr'], ['d0', 'ccr'],
            lambda d, k=k: {'d0': u32(k), 'ccr': ccr_out((d['ccr'] & X) | nz(u32(k), 4))},
            [(0x12345678, 0x00), (0xFFFFFFFF, 0x1F)])
    case('move_ccr_sr', "MOVE to CCR, MOVE from SR, MOVE from CCR (010+), MOVE to SR",
         "PRM MOVE to CCR: low 5 bits of the word source go to XNZVC, the upper byte is ignored; "
         "MOVE from SR stores S/IPL/CCR (unimplemented bits read 0); MOVE from CCR (68010+) zero-extends the CCR to a word")
    rows = [((0xA5E0 | c) if c & 1 else (0x5A00 | c), ALT[c % 6]) for c in range(32)]
    tpl("\tmove.w\td0,ccr", ['d0', 'ccr'], ['ccr'],
        lambda d: {'ccr': ccr_out(d['d0'] & 0x1F)}, rows)
    rows = [(0x87654321, c) for c in range(32)]
    tpl("\tmove.w\tsr,d0", ['d0', 'ccr'], ['d0', 'ccr'],
        lambda d: {'d0': setlo(d['d0'], 0x2000 | d['ccr'], 2), 'ccr': ccr_out(d['ccr'])}, rows)
    tpl("\tmove.w\tccr,d0", ['d0', 'ccr'], ['d0', 'ccr'],
        lambda d: {'d0': setlo(d['d0'], d['ccr'], 2), 'ccr': ccr_out(d['ccr'])}, rows, cpu=C010)
    rows = [(0x2300 | 0x08E0 | c, ALT[c % 6]) for c in range(32)]
    rows = [(a, 0x5A5A0000 | a, c) for a, c in rows]
    tpl("\tmove.w\td0,sr\n\tmove.w\tsr,d1\n\tmove.w\t#$2000,sr", ['d0', 'd1', 'ccr'], ['d1', 'ccr'],
        lambda d: {'d1': setlo(d['d1'], 0x2300 | (d['d0'] & 0x1F), 2), 'ccr': ccr_out(0)}, rows,
        text="move.w d0,sr (d0=$2Bxx: unimplemented bits 11,7-5 set) / move.w sr,d1 / move.w #$2000,sr")


def gen_imm_ccr():
    case('logic_imm_ccr', "ANDI/ORI/EORI #imm,CCR for all 32 CCR inputs",
         "PRM ANDI/ORI/EORI to CCR: the low 5 bits of the immediate byte combine with XNZVC; upper bits have no effect")
    for op, imms, f in (('andi', (0x00, 0x1E, 0xEF, 0xF5, 0xFF), lambda a, b: a & b),
                        ('ori', (0x00, 0x01, 0x10, 0x0A, 0xE0), lambda a, b: a | b),
                        ('eori', (0x00, 0x1F, 0x05, 0xF0, 0xFF), lambda a, b: a ^ b)):
        for k in imms:
            tpl(f"\t{op}.b\t#${k:02x},ccr", ['ccr'], ['ccr'],
                lambda d, k=k, f=f: {'ccr': ccr_out(f(d['ccr'], k))}, [(c,) for c in range(32)])
    case('logic_imm_sr', "ANDI/ORI/EORI #imm,SR (supervisor): CCR and interrupt mask bits",
         "PRM ANDI/ORI/EORI to SR: the whole SR is combined with the immediate word (S kept set here); "
         "read back with MOVE from SR")
    seq = [("ori.w\t#$0000,sr", lambda c: 0x2000 | c),
           ("ori.w\t#$001f,sr", lambda c: 0x201F),
           ("ori.w\t#$0300,sr", lambda c: 0x2300 | c),
           ("ori.w\t#$0700,sr\n\tandi.w\t#$faf5,sr", lambda c: 0x2200 | (c & 0x15)),
           ("andi.w\t#$ffe0,sr", lambda c: 0x2000),
           ("andi.w\t#$ffff,sr", lambda c: 0x2000 | c),
           ("eori.w\t#$0500,sr", lambda c: 0x2500 | c),
           ("eori.w\t#$001f,sr", lambda c: 0x2000 | (c ^ 0x1F))]
    for a, f in seq:
        tpl(f"\t{a}\n\tmove.w\tsr,d1\n\tmove.w\t#$2000,sr", ['d1', 'ccr'], ['d1', 'ccr'],
            lambda d, f=f: {'d1': setlo(d['d1'], f(d['ccr']), 2), 'ccr': ccr_out(0)},
            [(0x11110000 | c * 0x101, c) for c in (0x00, 0x1F, 0x0A, 0x15, 0x10, 0x04)])


def gen_movem():
    case('movem_l', "MOVEM.L register<->memory, all modes (An), -(An), (An)+, d16(An), d16(PC)",
         "PRM MOVEM: -(An) stores in reverse order (A7..D0) and An = final address; (An)+ loads D0..A7 and An = final address; "
         "registers are transferred in mask order; condition codes unaffected")
    regs = ['d0', 'd1', 'd2', 'd3', 'a1', 'a2']
    vals = [[0x01234567, 0x89ABCDEF, 0xFFFFFFFF, 0x00000000, 0x7FFFFFFF, 0x80000000],
            [0x55555555, 0xAAAAAAAA, 0x00000001, 0xFFFF0000, 0x0000FFFF, 0x12345678]]

    def st(off, rl, s=4):
        def f(d):
            mem = Mem(d)
            o = {}
            for i, r in enumerate(rl):
                mem.wr(off + i * s, s, d[r])
            for i in range(len(rl)):
                o[f'm{off + i * s}.{SN[s]}'] = mem.rd(off + i * s, s)
            o['ccr'] = ccr_out(d['ccr'])
            return o
        return f
    tpl("\tmovem.l\td0-d3/a1-a2,(a0)", regs + ['a0@', 'ccr'], [f'm{4 * i}.l' for i in range(6)] + ['ccr'],
        st(0, regs), [tuple(v) + (0, c) for v, c in zip(vals, (0x00, 0x1F))])
    tpl("\tmovem.l\td0-d3/a1-a2,-6(a0)", regs + ['a0@', 'ccr'], [f'm{4 * i - 6}.l' for i in range(6)] + ['ccr'],
        st(-6, regs), [tuple(v) + (0, c) for v, c in zip(vals, (0x0A, 0x15))])

    def stpre(d):
        mem = Mem(d)
        o = {}
        a = d['a0@']
        for r in reversed(['d0', 'd1', 'd2', 'a1']):
            a -= 4
            mem.wr(a, 4, d[r])
        for i in range(4):
            o[f'm{24 - 16 + 4 * i}.l'] = mem.rd(24 - 16 + 4 * i, 4)
        o['a0@'] = a
        o['ccr'] = ccr_out(d['ccr'])
        return o
    tpl("\tmovem.l\td0-d2/a1,-(a0)", ['d0', 'd1', 'd2', 'a1', 'a0@', 'ccr'],
        [f'm{8 + 4 * i}.l' for i in range(4)] + ['a0@', 'ccr'], stpre,
        [(vals[0][0], vals[0][1], vals[0][2], vals[0][4], 24, 0x00),
         (vals[1][0], vals[1][1], vals[1][2], vals[1][4], 24, 0x1F)])

    def ld(off, rl, s=4, post=False, base='a0@', pc=None):
        def f(d):
            mem = Mem(d)
            o = {}
            for i, r in enumerate(rl):
                v = pc[i] if pc else mem.rd(off + i * s, s)
                o[r] = u32(sx(v, s))
            if post:
                o[base] = d[base] + s * len(rl)
            o['ccr'] = ccr_out(d['ccr'])
            return o
        return f
    memrows = [(0x89ABCDEF, 0x01234567, 0x80000000, 0x7FFFFFFF, 0xFFFFFFFF, 0x00008000),
               (0x00000000, 0x00000001, 0xFFFF7FFF, 0x00010000, 0xAAAAAAAA, 0x55555555)]
    tpl("\tmovem.l\t(a0)+,d0-d2/a1", [f'm{4 * i}.l' for i in range(4)] + ['a0@', 'ccr'],
        ['d0', 'd1', 'd2', 'a1', 'a0@', 'ccr'], ld(0, ['d0', 'd1', 'd2', 'a1'], post=True),
        [r[:4] + (0, c) for r, c in zip(memrows, (0x00, 0x1F))])
    tpl("\tmovem.l\t8(a0),d4-d6/a3", [f'm{8 + 4 * i}.l' for i in range(4)] + ['a0@', 'ccr'],
        ['d4', 'd5', 'd6', 'a3', 'ccr'], ld(8, ['d4', 'd5', 'd6', 'a3']),
        [r[:4] + (0, c) for r, c in zip(memrows, (0x0A, 0x15))])
    tpl("\tmovem.l\t(a0),d0/d7/a6", [f'm{4 * i}.l' for i in range(3)] + ['a0@', 'ccr'],
        ['d0', 'd7', 'a6', 'ccr'], ld(0, ['d0', 'd7', 'a6']),
        [r[:3] + (0, c) for r, c in zip(memrows, (0x00, 0x1F))])
    k = [0xDEADBEEF, 0x00000000, 0x80000001, 0x7FFFFFFE]
    tpl("\tmovem.l\t.t(pc),d0-d3\n\trts\n\tcnop\t0,4\n.t:\tdc.l\t$deadbeef,$00000000,$80000001,$7ffffffe",
        ['ccr'], ['d0', 'd1', 'd2', 'd3', 'ccr'], ld(0, ['d0', 'd1', 'd2', 'd3'], pc=k),
        [(0x00,), (0x1F,)], text="movem.l .t(pc),d0-d3 (.t: $deadbeef,0,$80000001,$7ffffffe)")
    tpl("\tmovem.l\t(a0)+,d0/a0", ['m0.l', 'm4.l', 'a0@', 'ccr'], ['d0', 'a0@', 'ccr'],
        lambda d: {'d0': d['m0.l'], 'a0@': d['a0@'] + 8, 'ccr': ccr_out(d['ccr'])},
        [(0x11223344, 0x55667788, 0, 0x00), (0xFFFFFFFF, 0x00000000, 0, 0x1F)],
        text="movem.l (a0)+,d0/a0 (base in list: memory value ignored, a0 = incremented address)")

    case('movem_w', "MOVEM.W register<->memory (word loads are sign-extended into the whole register)",
         "PRM MOVEM: word transfers to registers are sign-extended to 32 bits, data registers included")
    tpl("\tmovem.w\td0-d2/a1,(a0)", ['d0', 'd1', 'd2', 'a1', 'a0@', 'ccr'],
        [f'm{2 * i}.w' for i in range(4)] + ['ccr'], st(0, ['d0', 'd1', 'd2', 'a1'], 2),
        [(0x12348001, 0xFFFF7FFF, 0x0000FFFF, 0xAAAA5555, 0, 0x00),
         (0x00000000, 0x80000000, 0x00008000, 0x00010001, 0, 0x1F)])

    def stpw(d):
        mem = Mem(d)
        a = d['a0@']
        for r in reversed(['d0', 'd1', 'a1']):
            a -= 2
            mem.wr(a, 2, d[r])
        return {'m10.w': mem.rd(10, 2), 'm12.w': mem.rd(12, 2), 'm14.w': mem.rd(14, 2),
                'a0@': a, 'ccr': ccr_out(d['ccr'])}
    tpl("\tmovem.w\td0-d1/a1,-(a0)", ['d0', 'd1', 'a1', 'a0@', 'ccr'], ['m10.w', 'm12.w', 'm14.w', 'a0@', 'ccr'],
        stpw, [(0x12348001, 0xFFFF7FFF, 0x0000FFFF, 16, 0x0A), (0, 0x8000, 0x10001, 16, 0x15)])
    wrows = [(0x8001, 0x7FFF, 0xFFFF, 0x0000), (0x0080, 0x8000, 0x0001, 0xFFFE)]
    tpl("\tmovem.w\t(a0)+,d0-d2/a1", [f'm{2 * i}.w' for i in range(4)] + ['a0@', 'ccr'],
        ['d0', 'd1', 'd2', 'a1', 'a0@', 'ccr'], ld(0, ['d0', 'd1', 'd2', 'a1'], s=2, post=True),
        [r + (0, c) for r, c in zip(wrows, (0x00, 0x1F))])
    tpl("\tmovem.w\t-4(a0),d5/a4", ['m-4.w', 'm-2.w', 'a0@', 'ccr'], ['d5', 'a4', 'ccr'],
        ld(-4, ['d5', 'a4'], s=2), [r[:2] + (0, c) for r, c in zip(wrows, (0x0A, 0x15))])

    # -(An) with An in the list: CPU-dependent value stored
    for cid, cpu, note in (('movem_predec_base_000', C000_010,
                            "68000/68010 store the initial value of the address register"),
                           ('movem_predec_base_020', C020,
                            "68020/030/040 (and 060) store the initial value decremented by the operand size")):
        case(cid, "MOVEM.L d0/a0,-(a0): the address register itself is in the list",
             "PRM MOVEM (Notes): " + note)
        if True:
                tpl("\tmovem.l\td0/a0,-(a0)", ['d0', 'a0@', 'ccr'], ['m12.l@', 'm8.l', 'a0@', 'ccr'],
                    lambda d, cpu=cpu: {'m12.l@': d['a0@'] - (0 if cpu == C000_010 else 4),
                                        'm8.l': d['d0'], 'a0@': d['a0@'] - 8, 'ccr': ccr_out(d['ccr'])},
                    [(0x12345678, 16, 0x00), (0xFFFFFFFF, 16, 0x1F)], cpu=cpu)


def gen_movep():
    case('movep', "MOVEP.W/.L both directions, even and odd addresses (not on 68060: unimplemented there)",
         "PRM MOVEP: bytes go to/come from every second address starting at d16(Ay), high-order byte first; "
         "register-destination word form leaves the upper word; no condition codes affected")

    def wr(sz, disp):
        def f(d):
            mem = Mem(d)
            o = {}
            for i in range(sz):
                mem.wr(disp + 2 * i, 1, d['d0'] >> (8 * (sz - 1 - i)))
            for i in range(sz):
                o[f'm{disp + 2 * i}.b'] = mem.rb(disp + 2 * i)
            o['ccr'] = ccr_out(d['ccr'])
            return o
        return f

    def rd(sz, disp):
        def f(d):
            mem = Mem(d)
            v = 0
            for i in range(sz):
                v = (v << 8) | mem.rb(disp + 2 * i)
            return {'d1': setlo(d['d1'], v, sz), 'ccr': ccr_out(d['ccr'])}
        return f
    for sz, disp in ((2, 0), (2, 1), (4, 0), (4, 1), (4, -7)):
        sn = SN[sz]
        rows = [(0x12345678, 0, 0x00), (0x89ABCDEF, 0, 0x1F), (0x00FF00FF, 0, 0x0A)]
        tpl(f"\tmovep.{sn}\td0,{disp}(a0)", ['d0', 'a0@', 'ccr'],
            [f'm{disp + 2 * i}.b' for i in range(sz)] + ['ccr'], wr(sz, disp), rows, cpu=NO060)
        ins = [f'm{disp + 2 * i}.b' for i in range(sz)]
        rows = [tuple((0x11 * (i + 1) + 0x80 * (j & 1)) & 0xFF for i in range(sz)) + (0xA5A5A5A5, 0, ALT[j])
                for j in range(3)]
        tpl(f"\tmovep.{sn}\t{disp}(a0),d1", ins + ['d1', 'a0@', 'ccr'], ['d1', 'ccr'], rd(sz, disp),
            rows, cpu=NO060)


def gen_addsub():
    for op, f in (('add', f_add), ('sub', f_sub)):
        R = op.upper()
        case(f'{op}_dn', f"{R}.B/.W/.L Dn,Dn over boundary operand pairs",
             f"PRM {R}: {'X=C=carry' if op == 'add' else 'X=C=borrow'}, V=signed overflow, N,Z from the result; "
             "upper bits of the destination register kept for .B/.W")
        for s in (1, 2, 4):
            tpl(f"\t{op}.{SN[s]}\td1,d0", ['d0', 'd1', 'ccr'], ['d0', 'ccr'],
                lambda d, s=s, f=f: (lambda r: {'d0': setlo(d['d0'], r[0], s), 'ccr': ccr_out(r[1])})(
                    f(s, d['d0'], d['d1'])), with_alt(pairs(s)))
        case(f'{op}_ea', f"{R} with memory, immediate and address-register operands; {R}A",
             f"PRM {R}/{R}A: as {op}_dn for <ea>,Dn and Dn,<ea>; {R}A sign-extends a word source and leaves the CCR")
        prs = with_alt(pairs(4, SMALL[4], SMALL[4]))[::5]
        tpl(f"\t{op}.l\t(a0),d0", ['d0', 'm0.l', 'a0@', 'ccr'], ['d0', 'ccr'],
            lambda d, f=f: (lambda r: {'d0': r[0], 'ccr': ccr_out(r[1])})(f(4, d['d0'], d['m0.l'])),
            [(a, b, 0, c) for a, b, c in prs])
        tpl(f"\t{op}.w\td0,(a0)+", ['d0', 'm0.w', 'a0@', 'ccr'], ['m0.w', 'a0@', 'ccr'],
            lambda d, f=f: (lambda r: {'m0.w': r[0], 'a0@': d['a0@'] + 2, 'ccr': ccr_out(r[1])})(
                f(2, d['m0.w'], d['d0'])),
            [(a, b & 0xFFFF, 0, c) for a, b, c in with_alt(pairs(2, SMALL[2], SMALL[2]))[::5]])
        tpl(f"\t{op}.b\t-(a0),d0", ['d0', 'm3.b', 'a0@', 'ccr'], ['d0', 'a0@', 'ccr'],
            lambda d, f=f: (lambda r: {'d0': setlo(d['d0'], r[0], 1), 'a0@': d['a0@'] - 1,
                                       'ccr': ccr_out(r[1])})(f(1, d['d0'], d['m3.b'])),
            [(a, b & 0xFF, 4, c) for a, b, c in with_alt(pairs(1, SMALL[1], SMALL[1]))[::5]])
        tpl(f"\t{op}.l\td0,6(a0,d1.w)", ['d0', 'd1', 'm10.l', 'a0@', 'ccr'], ['m10.l', 'ccr'],
            lambda d, f=f: (lambda r: {'m10.l': r[0], 'ccr': ccr_out(r[1])})(f(4, d['m10.l'], d['d0'])),
            [(a, 0xFFFF0004, b, 0, c) for a, b, c in prs])
        tpl(f"\t{op}.l\t#$80000001,d0", ['d0', 'ccr'], ['d0', 'ccr'],
            lambda d, f=f: (lambda r: {'d0': r[0], 'ccr': ccr_out(r[1])})(f(4, d['d0'], 0x80000001)),
            with_alt(singles(4)), text=f"{op}.l #$80000001,d0 (<ea>=#imm form, not {op}i)")
        tpl(f"\t{op}.w\ta1,d0", ['d0', 'a1', 'ccr'], ['d0', 'ccr'],
            lambda d, f=f: (lambda r: {'d0': setlo(d['d0'], r[0], 2), 'ccr': ccr_out(r[1])})(
                f(2, d['d0'], d['a1'])), with_alt(pairs(2, SMALL[2], SMALL[2]))[::3])
        for s in (2, 4):
            tpl(f"\t{op}a.{SN[s]}\td1,a0", ['a0', 'd1', 'ccr'], ['a0', 'ccr'],
                lambda d, s=s, op=op: {'a0': u32(d['a0'] + sx(d['d1'], s)) if op == 'add'
                                       else u32(d['a0'] - sx(d['d1'], s)), 'ccr': ccr_out(d['ccr'])},
                with_alt([(a, b) for a, b in pairs(4, SMALL[4], OPS[4])][::2]))
        tpl(f"\t{op}a.w\t(a1)+,a0", ['a0', 'm0.w', 'a1@', 'ccr'], ['a0', 'a1@', 'ccr'],
            lambda d, op=op: {'a0': u32(d['a0'] + (1 if op == 'add' else -1) * sx(d['m0.w'], 2)),
                              'a1@': d['a1@'] + 2, 'ccr': ccr_out(d['ccr'])},
            [(0x00010000, 0x8000, 0, 0x1F), (0xFFFFFFFF, 0x7FFF, 0, 0x00), (0x7FFFFFFF, 0x0001, 0, 0x0A)])
        tpl(f"\t{op}a.l\t#$80000000,a0", ['a0', 'ccr'], ['a0', 'ccr'],
            lambda d: {'a0': u32(d['a0'] + 0x80000000), 'ccr': ccr_out(d['ccr'])},
            with_alt(singles(4)[:4]))

        case(f'{op}i', f"{R}I #imm to Dn and to memory",
             f"PRM {R}I: flags as {R}; immediate operand of the operation size")
        for s, imm in ((1, 0x80), (1, 0x01), (2, 0x7FFF), (2, 0xFFFF), (4, 0x80000000), (4, 0x00000001)):
            tpl(f"\t{op}i.{SN[s]}\t#${imm:x},d0", ['d0', 'ccr'], ['d0', 'ccr'],
                lambda d, s=s, imm=imm, f=f: (lambda r: {'d0': setlo(d['d0'], r[0], s), 'ccr': ccr_out(r[1])})(
                    f(s, d['d0'], imm)), with_alt(singles(s)))
        tpl(f"\t{op}i.l\t#$7fffffff,(a0)", ['m0.l', 'a0@', 'ccr'], ['m0.l', 'ccr'],
            lambda d, f=f: (lambda r: {'m0.l': r[0], 'ccr': ccr_out(r[1])})(f(4, d['m0.l'], 0x7FFFFFFF)),
            [(v, 0, c) for (v,), c in zip(singles(4, SMALL[4]), ALT)])
        tpl(f"\t{op}i.b\t#$ff,-(a0)", ['m1.b', 'a0@', 'ccr'], ['m1.b', 'a0@', 'ccr'],
            lambda d, f=f: (lambda r: {'m1.b': r[0], 'a0@': d['a0@'] - 1, 'ccr': ccr_out(r[1])})(
                f(1, d['m1.b'], 0xFF)), [(v, 2, c) for v, c in zip(SMALL[1], ALT)])

        case(f'{op}q', f"{R}Q #1..8 to Dn, An and memory",
             f"PRM {R}Q: flags as {R} for Dn/memory; with an address register the whole 32-bit register "
             "changes (word size too) and the CCR is unaffected")
        for s, k in ((1, 1), (1, 8), (2, 1), (2, 8), (4, 1), (4, 8), (4, 5)):
            tpl(f"\t{op}q.{SN[s]}\t#{k},d0", ['d0', 'ccr'], ['d0', 'ccr'],
                lambda d, s=s, k=k, f=f: (lambda r: {'d0': setlo(d['d0'], r[0], s), 'ccr': ccr_out(r[1])})(
                    f(s, d['d0'], k)), with_alt(singles(s)))
        for s, k in ((2, 1), (2, 8), (4, 3)):
            tpl(f"\t{op}q.{SN[s]}\t#{k},a0", ['a0', 'ccr'], ['a0', 'ccr'],
                lambda d, k=k, op=op: {'a0': u32(d['a0'] + (k if op == 'add' else -k)), 'ccr': ccr_out(d['ccr'])},
                with_alt([(0x0000FFFF,), (0xFFFFFFFF,), (0x00000000,), (0x7FFFFFFF,), (0x80000000,), (0x0001FFFE,)]),
                text=f"{op}q.{SN[s]} #{k},a0 (whole register, CCR unaffected)")
        tpl(f"\t{op}q.w\t#2,4(a0)", ['m4.w', 'a0@', 'ccr'], ['m4.w', 'ccr'],
            lambda d, f=f: (lambda r: {'m4.w': r[0], 'ccr': ccr_out(r[1])})(f(2, d['m4.w'], 2)),
            [(v, 0, c) for v, c in zip(SMALL[2], ALT)])

        case(f'{op}x', f"{R}X Dn,Dn and -(An),-(An) for every X/Z input",
             f"PRM {R}X: the X bit is {'added' if op == 'add' else 'subtracted'}; Z is cleared if the result is "
             "non-zero and unchanged otherwise; X=C, V=signed overflow")
        for s in (1, 2, 4):
            ops = SMALL[s] + [msk(s) - 1]
            tpl(f"\t{op}x.{SN[s]}\td1,d0", ['d0', 'd1', 'ccr'], ['d0', 'ccr'],
                lambda d, s=s, f=f: (lambda r: {'d0': setlo(d['d0'], r[0], s), 'ccr': ccr_out(r[1])})(
                    f(s, d['d0'], d['d1'], 1 if d['ccr'] & X else 0, 1 if d['ccr'] & Z else 0, True)),
                with_xz(pairs(s, ops, ops)))
        for s in (1, 2, 4):
            sn = SN[s]
            rows = [(a & msk(s), b & msk(s), 8, 16, c) for a, b, c in with_xz(pairs(s, SMALL[s], SMALL[s]))[::7]]
            tpl(f"\t{op}x.{sn}\t-(a1),-(a0)", [f'm{8 - s}.{sn}', f'm{16 - s}.{sn}', 'a1@', 'a0@', 'ccr'],
                [f'm{8 - s}.{sn}', 'a1@', 'a0@', 'ccr'],
                lambda d, s=s, sn=sn, f=f: (lambda r: {f'm{8 - s}.{sn}': r[0], 'a1@': 16 - s, 'a0@': 8 - s,
                                                       'ccr': ccr_out(r[1])})(
                    f(s, d[f'm{8 - s}.{sn}'], d[f'm{16 - s}.{sn}'], 1 if d['ccr'] & X else 0,
                      1 if d['ccr'] & Z else 0, True)),
                [(b, a, 16, 8, c) for a, b, x, y, c in rows])
        tpl(f"\t{op}x.w\t-(a0),-(a0)", ['m4.w', 'm6.w', 'a0@', 'ccr'], ['m4.w', 'a0@', 'ccr'],
            lambda d, f=f: (lambda r: {'m4.w': r[0], 'a0@': 4, 'ccr': ccr_out(r[1])})(
                f(2, d['m4.w'], d['m6.w'], 1 if d['ccr'] & X else 0, 1 if d['ccr'] & Z else 0, True)),
            [(a & 0xFFFF, b & 0xFFFF, 8, c) for a, b, c in with_xz(pairs(2, SMALL[2], SMALL[2]))[::9]],
            text=f"{op}x.w -(a0),-(a0) (same register: source at a0-2, destination at a0-4)")


def gen_neg():
    case('neg_negx', "NEG, NEGX (every X/Z input), NOT on Dn and memory",
         "PRM NEG: 0-dst, X=C=(dst!=0), V=(dst==MSB only); NEGX: 0-dst-X, Z cleared if non-zero else unchanged; "
         "NOT: one's complement, N,Z set, V=C=0, X unaffected")
    for s in (1, 2, 4):
        sn = SN[s]
        tpl(f"\tneg.{sn}\td0", ['d0', 'ccr'], ['d0', 'ccr'],
            lambda d, s=s: (lambda r: {'d0': setlo(d['d0'], r[0], s), 'ccr': ccr_out(r[1])})(f_sub(s, 0, d['d0'])),
            with_alt(singles(s)))
        tpl(f"\tnegx.{sn}\td0", ['d0', 'ccr'], ['d0', 'ccr'],
            lambda d, s=s: (lambda r: {'d0': setlo(d['d0'], r[0], s), 'ccr': ccr_out(r[1])})(
                f_sub(s, 0, d['d0'], 1 if d['ccr'] & X else 0, 1 if d['ccr'] & Z else 0, True)),
            with_xz(singles(s)))
        tpl(f"\tnot.{sn}\td0", ['d0', 'ccr'], ['d0', 'ccr'],
            lambda d, s=s: {'d0': setlo(d['d0'], ~d['d0'], s),
                            'ccr': ccr_out((d['ccr'] & X) | nz(~d['d0'], s))}, with_alt(singles(s)))
    tpl("\tneg.l\t(a0)", ['m0.l', 'a0@', 'ccr'], ['m0.l', 'ccr'],
        lambda d: (lambda r: {'m0.l': r[0], 'ccr': ccr_out(r[1])})(f_sub(4, 0, d['m0.l'])),
        [(v, 0, c) for v, c in zip(SMALL[4], ALT)])
    tpl("\tnegx.w\t(a0)+", ['m0.w', 'a0@', 'ccr'], ['m0.w', 'a0@', 'ccr'],
        lambda d: (lambda r: {'m0.w': r[0], 'a0@': 2, 'ccr': ccr_out(r[1])})(
            f_sub(2, 0, d['m0.w'], 1 if d['ccr'] & X else 0, 1 if d['ccr'] & Z else 0, True)),
        [(v, 0, c) for v, c in zip(SMALL[2] * 2, XZ)])
    tpl("\tnot.b\t-(a0)", ['m1.b', 'a0@', 'ccr'], ['m1.b', 'a0@', 'ccr'],
        lambda d: {'m1.b': ~d['m1.b'] & 0xFF, 'a0@': 1, 'ccr': ccr_out((d['ccr'] & X) | nz(~d['m1.b'], 1))},
        [(v, 2, c) for v, c in zip(SMALL[1], ALT)])


def gen_cmp():
    case('cmp', "CMP.B/.W/.L Dn,Dn over boundary pairs; CMP <ea>,Dn",
         "PRM CMP: flags of dst-src (N,Z,V,C); X unaffected; no register changes")
    for s in (1, 2, 4):
        tpl(f"\tcmp.{SN[s]}\td1,d0", ['d0', 'd1', 'ccr'], ['ccr'],
            lambda d, s=s: {'ccr': ccr_out((d['ccr'] & X) | (f_sub(s, d['d0'], d['d1'])[1] & 0xF))},
            with_alt(pairs(s)))
    prs = with_alt(pairs(4, SMALL[4], SMALL[4]))[::4]
    tpl("\tcmp.l\t(a0)+,d0", ['d0', 'm0.l', 'a0@', 'ccr'], ['a0@', 'ccr'],
        lambda d: {'a0@': 4, 'ccr': ccr_out((d['ccr'] & X) | (f_sub(4, d['d0'], d['m0.l'])[1] & 0xF))},
        [(a, b, 0, c) for a, b, c in prs])
    tpl("\tcmp.w\ta1,d0", ['d0', 'a1', 'ccr'], ['ccr'],
        lambda d: {'ccr': ccr_out((d['ccr'] & X) | (f_sub(2, d['d0'], d['a1'])[1] & 0xF))},
        with_alt(pairs(2, SMALL[2], SMALL[2]))[::3])
    case('cmpa', "CMPA.W/.L",
         "PRM CMPA: a word source is sign-extended and the comparison is always 32-bit; X unaffected")
    for s in (2, 4):
        tpl(f"\tcmpa.{SN[s]}\td1,a0", ['a0', 'd1', 'ccr'], ['ccr'],
            lambda d, s=s: {'ccr': ccr_out((d['ccr'] & X) | (f_sub(4, d['a0'], u32(sx(d['d1'], s)))[1] & 0xF))},
            with_alt(pairs(4, SMALL[4] + [0xFFFF8000, 0x00007FFF], OPS[4])))
    case('cmpi', "CMPI #imm to Dn and memory; CMPI to d16(PC) (68020+)",
         "PRM CMPI: flags of dst-imm; X unaffected; PC-relative destination allowed from the 68020 on")
    for s, imm in ((1, 0x80), (2, 0x7FFF), (4, 0x80000000), (4, 0)):
        tpl(f"\tcmpi.{SN[s]}\t#${imm:x},d0", ['d0', 'ccr'], ['ccr'],
            lambda d, s=s, imm=imm: {'ccr': ccr_out((d['ccr'] & X) | (f_sub(s, d['d0'], imm)[1] & 0xF))},
            with_alt(singles(s)))
    tpl("\tcmpi.w\t#$8000,2(a0)", ['m2.w', 'a0@', 'ccr'], ['ccr'],
        lambda d: {'ccr': ccr_out((d['ccr'] & X) | (f_sub(2, d['m2.w'], 0x8000)[1] & 0xF))},
        [(v, 0, c) for v, c in zip(SMALL[2], ALT)])
    for k, imm in enumerate((0x12345678, 0x12345679, 0x80000000)):
        tpl(f"\tcmpi.l\t#${imm:x},.t(pc)\n\trts\n.t:\tdc.l\t$12345678", ['ccr'], ['ccr'],
            lambda d, imm=imm: {'ccr': ccr_out((d['ccr'] & X) | (f_sub(4, 0x12345678, imm)[1] & 0xF))},
            [(0x00,), (0x1F,)], cpu=C020, text=f"cmpi.l #${imm:x},.t(pc) (.t: dc.l $12345678)")
    case('cmpm', "CMPM.B/.W/.L (Ay)+,(Ax)+, including Ax=Ay",
         "PRM CMPM: flags of (Ax)-(Ay); both registers post-incremented by the size; X unaffected")
    for s in (1, 2, 4):
        sn = SN[s]
        rows = [(a & msk(s), b & msk(s), 0, 8, c) for a, b, c in with_alt(pairs(s, SMALL[s], SMALL[s]))[::3]]
        tpl(f"\tcmpm.{sn}\t(a1)+,(a0)+", [f'm0.{sn}', f'm8.{sn}', 'a1@', 'a0@', 'ccr'], ['a1@', 'a0@', 'ccr'],
            lambda d, s=s, sn=sn: {'a1@': s, 'a0@': 8 + s,
                                   'ccr': ccr_out((d['ccr'] & X) | (f_sub(s, d[f'm8.{sn}'], d[f'm0.{sn}'])[1] & 0xF))},
            rows)
    tpl("\tcmpm.l\t(a0)+,(a0)+", ['m0.l', 'm4.l', 'a0@', 'ccr'], ['a0@', 'ccr'],
        lambda d: {'a0@': 8, 'ccr': ccr_out((d['ccr'] & X) | (f_sub(4, d['m4.l'], d['m0.l'])[1] & 0xF))},
        [(a, b, 0, c) for a, b, c in with_alt(pairs(4, SMALL[4], SMALL[4]))[::4]],
        text="cmpm.l (a0)+,(a0)+ (compares (a0+4) with (a0))")


def in_bounds(s, lo, hi, r, areg):
    """CMP2/CHK2 per PRM: Z if Rn equals a bound, C if out of bounds; signed or
    unsigned comparison chosen by the bounds (lower <= upper as given).
    The vectors only use bound pairs whose ordering is the same signed and
    unsigned, or signed pairs that straddle zero (lower < 0 <= upper)."""
    if areg:
        lo, hi, r, s = u32(sx(lo, s)), u32(sx(hi, s)), u32(r), 4
    m = msk(s)
    lo, hi, r = lo & m, hi & m, r & m
    if lo <= hi:
        out = r < lo or r > hi
    else:   # signed pair straddling zero
        out = sx(r, s) < sx(lo, s) or sx(r, s) > sx(hi, s)
    z = r == lo or r == hi
    return (Z if z else 0) | (C if out else 0)


def gen_cmp2_chk():
    case('cmp2_chk2', "CMP2/CHK2 .B/.W/.L against bound pairs in memory (68020-68040; unimplemented on 68060); "
                      "CHK2 only with in-bounds values (out-of-bounds traps: exceptions module)",
         "MC68020UM/PRM CMP2: Z=Rn equals either bound, C=Rn out of bounds; N,V undefined (masked); "
         "address-register form sign-extends the bounds and compares 32 bits")
    for s in (1, 2, 4):
        sn = SN[s]
        m = msk(s)
        bounds = [(0x10, 0x20), (m - 0x20, m - 0x10), ((-0x10) & m, 0x10), (0, m >> 1)]
        rows = []
        k = 0
        for lo, hi in bounds:
            for r in (lo, hi, lo - 1, hi + 1, (lo + hi) // 2 if lo <= hi else 0, 0, m):
                rows.append((lo, hi, rv(r & m, s, k), ALT[k % 6]))
                k += 1
        tpl(f"\tcmp2.{sn}\t(a0),d0", [f'm0.{sn}', f'm{s}.{sn}', 'd0', 'a0@', 'ccr'], ['ccr'],
            lambda d, s=s, sn=sn: {'ccr': ccr_out((d['ccr'] & X) | in_bounds(s, d[f'm0.{sn}'], d[f'm{s}.{sn}'],
                                                                             d['d0'], False), X | Z | C)},
            [(a, b, r, 0, c) for a, b, r, c in rows], cpu=C020N60)
        inrows = [(a, b, r, 0, c) for a, b, r, c in rows if not in_bounds(s, a, b, r, False) & C]
        tpl(f"\tchk2.{sn}\t(a0),d0", [f'm0.{sn}', f'm{s}.{sn}', 'd0', 'a0@', 'ccr'], ['ccr'],
            lambda d, s=s, sn=sn: {'ccr': ccr_out((d['ccr'] & X) | in_bounds(s, d[f'm0.{sn}'], d[f'm{s}.{sn}'],
                                                                             d['d0'], False), X | Z | C)},
            inrows, cpu=C020N60)
    rows = [(0x8000, 0x7FFF, 0xFFFF8000), (0x8000, 0x7FFF, 0x00008000), (0xFFF0, 0x0010, 0x00000000),
            (0xFFF0, 0x0010, 0x0000FFF0), (0x0010, 0x0020, 0x00010015), (0x0010, 0x0020, 0x00000020)]
    tpl("\tcmp2.w\t(a0),a1", ['m0.w', 'm2.w', 'a1', 'a0@', 'ccr'], ['ccr'],
        lambda d: {'ccr': ccr_out((d['ccr'] & X) | in_bounds(2, d['m0.w'], d['m2.w'], d['a1'], True), X | Z | C)},
        [(a, b, r, 0, ALT[i % 6]) for i, (a, b, r) in enumerate(rows)], cpu=C020N60,
        text="cmp2.w (a0),a1 (bounds sign-extended, 32-bit compare)")

    case('chk', "CHK.W (and CHK.L on 68020+) with in-range values (no trap)",
         "PRM CHK: no exception if 0 <= Dn <= bound; N,Z,V,C undefined then (masked), X unaffected, no register changes")
    rows = [(0x0000, 0x0000), (0x1230005, 0x0005), (0x7FFF, 0x7FFF), (0xFFFF0000, 0x7FFF), (0x0001, 0x8001 ^ 0x8000)]
    tpl("\tchk.w\td1,d0", ['d0', 'd1', 'ccr'], ['ccr'], lambda d: {'ccr': ccr_out(d['ccr'] & X, X)},
        [(a, b, ALT[i]) for i, (a, b) in enumerate(rows)])
    rows = [(0, 0), (5, 0x7FFFFFFF), (0x7FFFFFFF, 0x7FFFFFFF), (0x10000, 0x10000)]
    tpl("\tchk.l\td1,d0", ['d0', 'd1', 'ccr'], ['ccr'], lambda d: {'ccr': ccr_out(d['ccr'] & X, X)},
        [(a, b, ALT[i]) for i, (a, b) in enumerate(rows)], cpu=C020)


def gen_logic():
    for op, f in (('and', lambda a, b: a & b), ('or', lambda a, b: a | b), ('eor', lambda a, b: a ^ b)):
        R = op.upper()
        case(f'{op}', f"{R}.B/.W/.L Dn,Dn over boundary pairs; {R} with memory; {R}I to Dn and memory",
             f"PRM {R}/{R}I: N,Z from the result, V=C=0, X unaffected")
        for s in (1, 2, 4):
            tpl(f"\t{op}.{SN[s]}\td1,d0", ['d0', 'd1', 'ccr'], ['d0', 'ccr'],
                lambda d, s=s, f=f: {'d0': setlo(d['d0'], f(d['d0'], d['d1']), s),
                                     'ccr': ccr_out((d['ccr'] & X) | nz(f(d['d0'], d['d1']), s))},
                with_alt(pairs(s)))
        prs = with_alt(pairs(4, SMALL[4], SMALL[4]))[::5]
        if op != 'eor':
            tpl(f"\t{op}.l\t(a0),d0", ['d0', 'm0.l', 'a0@', 'ccr'], ['d0', 'ccr'],
                lambda d, f=f: {'d0': f(d['d0'], d['m0.l']), 'ccr': ccr_out((d['ccr'] & X) | nz(f(d['d0'], d['m0.l']), 4))},
                [(a, b, 0, c) for a, b, c in prs])
        tpl(f"\t{op}.w\td0,-(a0)", ['d0', 'm2.w', 'a0@', 'ccr'], ['m2.w', 'a0@', 'ccr'],
            lambda d, f=f: {'m2.w': f(d['d0'], d['m2.w']) & 0xFFFF, 'a0@': 2,
                            'ccr': ccr_out((d['ccr'] & X) | nz(f(d['d0'], d['m2.w']), 2))},
            [(a, b & 0xFFFF, 4, c) for a, b, c in prs])
        for s, imm in ((1, 0x81), (2, 0x8001), (4, 0x80000001), (4, 0xFFFFFFFF)):
            tpl(f"\t{op}i.{SN[s]}\t#${imm:x},d0", ['d0', 'ccr'], ['d0', 'ccr'],
                lambda d, s=s, imm=imm, f=f: {'d0': setlo(d['d0'], f(d['d0'], imm), s),
                                              'ccr': ccr_out((d['ccr'] & X) | nz(f(d['d0'], imm), s))},
                with_alt(singles(s)))
        tpl(f"\t{op}i.b\t#$0f,1(a0)", ['m1.b', 'a0@', 'ccr'], ['m1.b', 'ccr'],
            lambda d, f=f: {'m1.b': f(d['m1.b'], 0x0F) & 0xFF, 'ccr': ccr_out((d['ccr'] & X) | nz(f(d['m1.b'], 0x0F), 1))},
            [(v, 0, c) for v, c in zip(SMALL[1], ALT)])


SHIFTS = ['asl', 'asr', 'lsl', 'lsr', 'rol', 'ror', 'roxl', 'roxr']
SH_OPS = {1: [0x01, 0x80, 0xFF, 0x55, 0xC1, 0x00],
          2: [0x0001, 0x8000, 0xFFFF, 0x5555, 0xC001, 0x4000],
          4: [0x00000001, 0x80000000, 0xFFFFFFFF, 0x55555555, 0xC0000001, 0x40000000]}
SH_CNT = [0, 1, 2, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 0x101, 0xFFFFFFC1]


def gen_shifts():
    for op in SHIFTS:
        R = op.upper()
        case(f'{op}', f"{R}.B/.W/.L by register counts {{0,1,2,7,8,9,15,16,17,31,32,33,63,64,..}} and by #1/#8; "
                      f"{R} <ea> (word, by 1)",
             f"PRM {R}: register count taken modulo 64; " +
             {'asl': "C=X=last bit out, V=MSB changed at any time during the shift; count 0: V=C=0, X unaffected",
              'asr': "sign bit replicated, C=X=last bit out, V=0; count 0: C=0, X unaffected",
              'lsl': "C=X=last bit out, V=0; count 0: C=0, X unaffected",
              'lsr': "C=X=last bit out, V=0; count 0: C=0, X unaffected",
              'rol': "C=last bit rotated out, X unaffected, V=0; count 0: C=0",
              'ror': "C=last bit rotated out, X unaffected, V=0; count 0: C=0",
              'roxl': "rotate through X: C=X=last bit out, V=0; count 0: C=X, X unaffected",
              'roxr': "rotate through X: C=X=last bit out, V=0; count 0: C=X, X unaffected"}[op])
        for s in (1, 2, 4):
            sn = SN[s]
            rows = []
            k = 0
            for v in SH_OPS[s]:
                for c in SH_CNT:
                    xs = (0x00, 0x10) if op in ('roxl', 'roxr') else (ALT[k % 6],)
                    for x in xs:
                        if op in ('roxl', 'roxr'):
                            x |= ALT[k % 6] & 0x0F
                        rows.append((rv(v, s, k), c, x))
                        k += 1
            tpl(f"\t{op}.{sn}\td1,d0", ['d0', 'd1', 'ccr'], ['d0', 'ccr'],
                lambda d, s=s, op=op: (lambda r: {'d0': setlo(d['d0'], r[0], s), 'ccr': ccr_out(r[1])})(
                    f_shift(op, s, d['d0'], d['d1'] & 63, d['ccr'] & X)), rows)
            for k2 in (1, 8):
                rows = []
                for i, v in enumerate(SH_OPS[s]):
                    for x in ((0x00, 0x1F) if op in ('roxl', 'roxr') else (ALT[i % 6],)):
                        rows.append((rv(v, s, i), x))
                tpl(f"\t{op}.{sn}\t#{k2},d0", ['d0', 'ccr'], ['d0', 'ccr'],
                    lambda d, s=s, op=op, k2=k2: (lambda r: {'d0': setlo(d['d0'], r[0], s), 'ccr': ccr_out(r[1])})(
                        f_shift(op, s, d['d0'], k2, d['ccr'] & X)), rows)
        rows = []
        for i, v in enumerate(SH_OPS[2]):
            for x in (0x00, 0x1F):
                rows.append((v, 0, x ^ (0x0A if i & 1 else 0)))
        tpl(f"\t{op}.w\t(a0)+", ['m0.w', 'a0@', 'ccr'], ['m0.w', 'a0@', 'ccr'],
            lambda d, op=op: (lambda r: {'m0.w': r[0], 'a0@': 2, 'ccr': ccr_out(r[1])})(
                f_shift(op, 2, d['m0.w'], 1, d['ccr'] & X)), rows)


def gen_bitops():
    for op in ('btst', 'bchg', 'bclr', 'bset'):
        R = op.upper()
        case(op, f"{R} Dn/#imm with a data-register destination (bit mod 32) and a memory byte (bit mod 8)",
             f"PRM {R}: Z = NOT(tested bit) before the change; other condition codes unaffected; "
             "bit number modulo 32 for Dn, modulo 8 for memory")

        def apply(v, b, op=op):
            if op == 'bchg':
                return v ^ (1 << b)
            if op == 'bclr':
                return v & ~(1 << b)
            if op == 'bset':
                return v | (1 << b)
            return v
        bits = [0, 1, 7, 8, 15, 16, 31, 32, 33, 63, 0xFFFFFFFF, 0x80000007]
        datas = [0x00000000, 0xFFFFFFFF, 0x80000001, 0x5555AAAA]
        rows = [(dv, b, ALT[i % 6]) for i, (dv, b) in enumerate(itertools.product(datas, bits))]

        def fr(d, op=op):
            b = d['d1'] & 31
            o = {'ccr': ccr_out((d['ccr'] & ~Z) | (0 if (d['d0'] >> b) & 1 else Z))}
            if op != 'btst':
                o['d0'] = u32(apply(d['d0'], b))
            return o
        outs = ['ccr'] if op == 'btst' else ['d0', 'ccr']
        tpl(f"\t{op}\td1,d0", ['d0', 'd1', 'ccr'], outs, fr, rows)
        for b in (0, 5, 31, 39):
            rows = [(dv, ALT[i % 6]) for i, dv in enumerate(datas)]
            tpl(f"\t{op}\t#{b},d0", ['d0', 'ccr'], outs,
                lambda d, b=b, op=op: dict([('ccr', ccr_out((d['ccr'] & ~Z) | (0 if (d['d0'] >> (b & 31)) & 1 else Z)))] +
                                           ([] if op == 'btst' else [('d0', u32(apply(d['d0'], b & 31)))])), rows)
        mrows = [(mv, b, 0, ALT[i % 6]) for i, (mv, b) in enumerate(itertools.product((0x00, 0xFF, 0x81, 0x5A), (0, 3, 7, 8, 15, 0xFFFFFFFF)))]

        def fm(d, op=op):
            b = d['d1'] & 7
            o = {'ccr': ccr_out((d['ccr'] & ~Z) | (0 if (d['m1.b'] >> b) & 1 else Z))}
            if op != 'btst':
                o['m1.b'] = apply(d['m1.b'], b) & 0xFF
            return o
        tpl(f"\t{op}\td1,1(a0)", ['m1.b', 'd1', 'a0@', 'ccr'], ['ccr'] if op == 'btst' else ['m1.b', 'ccr'], fm, mrows)
        for b in (2, 7, 9):
            tpl(f"\t{op}\t#{b},(a0)+", ['m0.b', 'a0@', 'ccr'], ['a0@', 'ccr'] if op == 'btst' else ['m0.b', 'a0@', 'ccr'],
                lambda d, b=b, op=op: dict([('a0@', 1), ('ccr', ccr_out((d['ccr'] & ~Z) | (0 if (d['m0.b'] >> (b & 7)) & 1 else Z)))] +
                                           ([] if op == 'btst' else [('m0.b', apply(d['m0.b'], b & 7) & 0xFF)])),
                [(mv, 0, ALT[i]) for i, mv in enumerate((0x00, 0xFF, 0x84, 0x7B))])
    tpl("\tbtst\td1,#$a5", ['d1', 'ccr'], ['ccr'],
        lambda d: {'ccr': ccr_out((d['ccr'] & ~Z) | (0 if (0xA5 >> (d['d1'] & 7)) & 1 else Z))},
        [(b, ALT[b % 6]) for b in range(9)], text="btst d1,#$a5 (immediate data destination, bit mod 8)")
    tpl("\tbtst\t#3,.t(pc)\n\trts\n.t:\tdc.b\t$08,$00", ['ccr'], ['ccr'],
        lambda d: {'ccr': ccr_out(d['ccr'] & ~Z)}, [(0x00,), (0x1F,)], text="btst #3,.t(pc) (.t: dc.b $08)")


def bf_model(op, get, put, o, w, dn=0):
    """bit field op on an abstract field (get(i)/put(i,b) for field bit i, 0 = MSB)."""
    w = w or 32
    v = 0
    for i in range(w):
        v = (v << 1) | get(i)
    if op == 'bfins':
        ins = dn & ((1 << w) - 1)
        for i in range(w):
            put(i, (ins >> (w - 1 - i)) & 1)
        f = (N if ins >> (w - 1) & 1 else 0) | (Z if ins == 0 else 0)
        return None, f
    f = (N if v >> (w - 1) & 1 else 0) | (Z if v == 0 else 0)
    res = None
    if op == 'bfextu':
        res = v
    elif op == 'bfexts':
        res = u32(v - (1 << w) if v >> (w - 1) & 1 else v)
    elif op == 'bfffo':
        p = w
        for i in range(w):
            if (v >> (w - 1 - i)) & 1:
                p = i
                break
        res = u32(o + p)
    elif op in ('bfchg', 'bfclr', 'bfset'):
        for i in range(w):
            b = (v >> (w - 1 - i)) & 1
            put(i, b ^ 1 if op == 'bfchg' else (0 if op == 'bfclr' else 1))
    return res, f


BFOPS = ['bftst', 'bfextu', 'bfexts', 'bfffo', 'bfchg', 'bfclr', 'bfset', 'bfins']


def gen_bitfield():
    for op in BFOPS:
        R = op.upper()
        case(op, f"{R} (68020+) on a data register (offset mod 32, wrap-around) and on memory "
                 "(signed register offsets, fields spanning up to 5 bytes)",
             f"MC68020UM/PRM {R}: field from offset (bit 0 = MSB) and width (0 means 32); N=MSB of the field, Z=field is 0 "
             f"{'(of the inserted value)' if op == 'bfins' else '(before any change)'}, V=C=0, X unaffected"
             + ("; BFFFO returns offset + index of the first 1 (offset + width if none)" if op == 'bfffo' else ""))
        has_d = op in ('bfextu', 'bfexts', 'bfffo')
        modifies = op in ('bfchg', 'bfclr', 'bfset', 'bfins')
        # --- register form, register offset/width
        datas = [0x80000001, 0x0F0F0F0F, 0xFFFFFFFF, 0x00000000, 0x12345678]
        offs = [0, 1, 7, 8, 24, 31] + ([] if op == 'bfffo' else [33, 0xFFFFFFFF])
        wids = [1, 8, 16, 31, 0, 33]
        rows = []
        k = 0
        for o in offs:
            for w in wids:
                dv = datas[k % len(datas)]
                rows.append((dv, o, w, 0x5A5AA5A5 ^ (k * 0x01010101) & M32, ALT[k % 6]))
                k += 1

        def freg(d, op=op, has_d=has_d, modifies=modifies):
            reg = [d['d0']]
            o = d['d1']
            off = o & 31
            w = d['d2'] & 31

            def get(i):
                return (reg[0] >> (31 - ((off + i) & 31))) & 1

            def put(i, b):
                p = 31 - ((off + i) & 31)
                reg[0] = (reg[0] & ~(1 << p)) | (b << p)
            res, f = bf_model(op, get, put, o if op == 'bfffo' else off, w, d['d3'])
            out = {'ccr': ccr_out((d['ccr'] & X) | f)}
            if has_d:
                out['d3'] = res
            if modifies:
                out['d0'] = u32(reg[0])
            return out
        outs = (['d3'] if has_d else []) + (['d0'] if modifies else []) + ['ccr']
        if op == 'bfins':
            asm = "\tbfins\td3,d0{d1:d2}"
        elif has_d:
            asm = f"\t{op}\td0{{d1:d2}},d3"
        else:
            asm = f"\t{op}\td0{{d1:d2}}"
        tpl(asm, ['d0', 'd1', 'd2', 'd3', 'ccr'], outs, freg, rows, cpu=C020)
        # --- memory form, register offset/width
        moffs = [-17, -9, -1, 0, 1, 7, 13, 31, 40]
        mwids = [1, 8, 9, 17, 0]
        pats = [(0x80000001, 0xF00FF00F, 0x00FF00FF, 0x55AA55AA, 0x80000001),
                (0xFFFFFFFF, 0x00000000, 0xFFFFFFFF, 0x00000000, 0xFFFFFFFF),
                (0x12345678, 0x9ABCDEF0, 0x0F1E2D3C, 0x4B5A6978, 0x8796A5B4)]
        rows = []
        k = 0
        for o in moffs:
            for w in mwids:
                p = pats[k % 3]
                base = 8 + (k & 1)
                rows.append(p + (u32(o), w, 0xC3A5965A ^ (k * 0x11111111) & M32, base, ALT[k % 6]))
                k += 1
        mslots = ['m0.l', 'm4.l', 'm8.l', 'm12.l', 'm16.l']

        def fmem(d, op=op, has_d=has_d, modifies=modifies):
            mem = Mem(d)
            base = d['a0@']
            o = sx(d['d1'], 4)
            w = d['d2'] & 31

            def addr(i):
                bit = o + i
                return base + (bit >> 3), 7 - (bit & 7)

            def get(i):
                a, b = addr(i)
                return (mem.rb(a) >> b) & 1

            def put(i, v):
                a, b = addr(i)
                mem.b[a] = (mem.rb(a) & ~(1 << b)) | (v << b)
            res, f = bf_model(op, get, put, o, w, d['d3'])
            out = {'ccr': ccr_out((d['ccr'] & X) | f)}
            if has_d:
                out['d3'] = res
            if modifies:
                for s in mslots:
                    out[s] = mem.slot(s)
            return out
        outs = (['d3'] if has_d else []) + (mslots if modifies else []) + ['ccr']
        if op == 'bfins':
            asm = "\tbfins\td3,(a0){d1:d2}"
        elif has_d:
            asm = f"\t{op}\t(a0){{d1:d2}},d3"
        else:
            asm = f"\t{op}\t(a0){{d1:d2}}"
        tpl(asm, mslots + ['d1', 'd2', 'd3', 'a0@', 'ccr'], outs, fmem, rows, cpu=C020)
        # --- immediate offset/width forms
        for (o, w, mem_form) in ((4, 8, False), (28, 8, False), (0, 32, False), (5, 32, True), (31, 2, True)):
            ws = str(w)
            if mem_form:
                ea = f"2(a0){{{o}:{ws}}}"
            else:
                ea = f"d0{{{o}:{ws}}}"
            if op == 'bfins':
                asm = f"\tbfins\td3,{ea}"
            elif has_d:
                asm = f"\t{op}\t{ea},d3"
            else:
                asm = f"\t{op}\t{ea}"
            if mem_form:
                def fi(d, op=op, o=o, w=w, has_d=has_d, modifies=modifies):
                    dd = dict(d)
                    dd['d1'] = o
                    dd['d2'] = w & 31
                    dd['a0@'] = d['a0@'] + 2
                    r = fmem(dd, op, has_d, modifies)
                    return r
                rows = [p + (0x0000A5A5 * (i + 1) & M32, 0, ALT[i]) for i, p in enumerate(pats)]
                tpl(asm, mslots + ['d3', 'a0@', 'ccr'], outs, fi, rows, cpu=C020)
            else:
                def fi(d, op=op, o=o, w=w, has_d=has_d, modifies=modifies):
                    dd = dict(d)
                    dd['d1'] = o
                    dd['d2'] = w & 31
                    return freg(dd, op, has_d, modifies)
                routs = (['d3'] if has_d else []) + (['d0'] if modifies else []) + ['ccr']
                rows = [(dv, 0xA5A5A5A5 ^ i, ALT[i]) for i, dv in enumerate(datas)]
                tpl(asm, ['d0', 'd3', 'ccr'], routs, fi, rows, cpu=C020)


def gen_muldiv():
    case('mul_w', "MULU.W/MULS.W Dn,Dn over boundary pairs, and from memory",
         "PRM MULU/MULS: 16x16->32 product in Dn; N,Z from the 32-bit result, V=C=0, X unaffected")
    for op in ('mulu', 'muls'):
        def f(d, op=op):
            a, b = d['d0'] & 0xFFFF, d['d1'] & 0xFFFF
            r = u32(a * b) if op == 'mulu' else u32(sx(a, 2) * sx(b, 2))
            return {'d0': r, 'ccr': ccr_out((d['ccr'] & X) | nz(r, 4))}
        tpl(f"\t{op}.w\td1,d0", ['d0', 'd1', 'ccr'], ['d0', 'ccr'], f, with_alt(pairs(2)))
        tpl(f"\t{op}.w\t(a0),d0", ['d0', 'm0.w', 'a0@', 'ccr'], ['d0', 'ccr'],
            lambda d, f=f: f({'d0': d['d0'], 'd1': d['m0.w'], 'ccr': d['ccr']}),
            [(a, b & 0xFFFF, 0, c) for a, b, c in with_alt(pairs(2, SMALL[2], SMALL[2]))[::5]])

    DVD = [0, 1, 0x7FFF, 0x8000, 0xFFFF, 0x10000, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x12345678, 0xFFFF8000, 100]
    DVS = [1, 2, 3, 0x10, 0x7FFF, 0x8000, 0xFFFE, 0xFFFF]
    case('div_w', "DIVU.W/DIVS.W Dn,Dn over boundary pairs including overflow (no zero divisor: exceptions module)",
         "PRM DIVU/DIVS: 32/16 -> remainder:quotient in Dn, remainder has the dividend's sign; N,Z from the "
         "quotient, V=C=0; on overflow V=1, C=0, Dn unchanged and N,Z undefined (masked); X unaffected")
    for op in ('divu', 'divs'):
        def f(d, op=op):
            a = d['d0']
            b = d['d1'] & 0xFFFF
            if op == 'divu':
                q, r = a // b, a % b
                ovf = q > 0xFFFF
            else:
                sa, sb = sx(a, 4), sx(b, 2)
                q = abs(sa) // abs(sb)
                if (sa < 0) != (sb < 0):
                    q = -q
                r = sa - q * sb
                ovf = q < -0x8000 or q > 0x7FFF
            if ovf:
                return {'d0': a, 'ccr': ccr_out((d['ccr'] & X) | V, X | V | C)}
            q &= 0xFFFF
            return {'d0': ((r & 0xFFFF) << 16) | q, 'ccr': ccr_out((d['ccr'] & X) | nz(q, 2))}
        rows = with_alt([(a, rv(b, 2, i)) for i, (a, b) in enumerate(itertools.product(DVD, DVS))])
        tpl(f"\t{op}.w\td1,d0", ['d0', 'd1', 'ccr'], ['d0', 'ccr'], f, rows)
        tpl(f"\t{op}.w\t#$0007,d0", ['d0', 'ccr'], ['d0', 'ccr'],
            lambda d, f=f: f({'d0': d['d0'], 'd1': 7, 'ccr': d['ccr']}), with_alt([(a,) for a in DVD]))

    L = [0, 1, 2, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x10000, 0xFFFF, 0x12345678, 0xFFFF0000, 0x00000003]
    case('mul_l', "MULU.L/MULS.L 32x32->32 (68020+) over boundary pairs",
         "MC68020UM/PRM MULU.L/MULS.L: low 32 bits of the product in Dl; V=1 if the product does not fit in 32 "
         "bits (unsigned/signed); N,Z from the 32-bit result; C=0, X unaffected")
    for op in ('mulu', 'muls'):
        def f(d, op=op):
            a, b = d['d0'], d['d1']
            p = a * b if op == 'mulu' else sx(a, 4) * sx(b, 4)
            r = u32(p)
            ovf = (p > M32) if op == 'mulu' else not (-0x80000000 <= p <= 0x7FFFFFFF)
            return {'d0': r, 'ccr': ccr_out((d['ccr'] & X) | nz(r, 4) | (V if ovf else 0))}
        tpl(f"\t{op}.l\td1,d0", ['d0', 'd1', 'ccr'], ['d0', 'ccr'], f, with_alt(pairs(4, L, L)), cpu=C020)
    case('mul_l_64', "MULU.L/MULS.L 32x32->64 Dh:Dl (68020/030/040; unimplemented on 68060)",
         "MC68020UM/PRM MULU.L/MULS.L 64-bit form: Dh:Dl = product; N=bit 63, Z=64-bit result is 0, V=C=0, X unaffected")
    for op in ('mulu', 'muls'):
        def f(d, op=op):
            a, b = d['d0'], d['d1']
            p = a * b if op == 'mulu' else sx(a, 4) * sx(b, 4)
            p &= (1 << 64) - 1
            return {'d0': p & M32, 'd2': p >> 32,
                    'ccr': ccr_out((d['ccr'] & X) | (N if p >> 63 else 0) | (Z if p == 0 else 0))}
        tpl(f"\t{op}.l\td1,d2:d0", ['d0', 'd1', 'd2', 'ccr'], ['d0', 'd2', 'ccr'], f,
            [(a, b, 0xA5A5A5A5, c) for a, b, c in with_alt(pairs(4, L, L))], cpu=C020N60)

    case('div_l', "DIVU.L/DIVS.L 32/32 (quotient only) and DIVUL.L/DIVSL.L 32/32 -> Dr:Dq (68020+)",
         "MC68020UM/PRM DIVU.L/DIVS.L: quotient truncated toward zero, remainder sign of the dividend; "
         "N,Z from the quotient, V=C=0; overflow (only $80000000/-1 signed) sets V, C=0, operands unchanged, N,Z undefined (masked)")
    LD = [0, 1, 7, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x12345678, 0xFFFFFFF9, 0x10000]
    LS = [1, 2, 7, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0xFFFFFFF9, 0x10000]

    def div32(op, a, b):
        if op == 'divu':
            return a // b, a % b, False
        sa, sb = sx(a, 4), sx(b, 4)
        q = abs(sa) // abs(sb)
        if (sa < 0) != (sb < 0):
            q = -q
        r = sa - q * sb
        return q, r, not (-0x80000000 <= q <= 0x7FFFFFFF)
    for op in ('divu', 'divs'):
        def fq(d, op=op):
            q, r, ovf = div32(op, d['d0'], d['d1'])
            if ovf:
                return {'d0': d['d0'], 'ccr': ccr_out((d['ccr'] & X) | V, X | V | C)}
            return {'d0': u32(q), 'ccr': ccr_out((d['ccr'] & X) | nz(u32(q), 4))}
        rows = with_alt([(a, b) for a, b in itertools.product(LD, LS)])
        tpl(f"\t{op}.l\td1,d0", ['d0', 'd1', 'ccr'], ['d0', 'ccr'], fq, rows, cpu=C020)

        def fqr(d, op=op):
            q, r, ovf = div32(op, d['d0'], d['d1'])
            if ovf:
                return {'d0': d['d0'], 'd2': d['d2'], 'ccr': ccr_out((d['ccr'] & X) | V, X | V | C)}
            return {'d0': u32(q), 'd2': u32(r), 'ccr': ccr_out((d['ccr'] & X) | nz(u32(q), 4))}
        rows = [(a, b, 0x5A5A5A5A, c) for a, b, c in with_alt([(a, b) for a, b in itertools.product(LD, LS)])]
        tpl(f"\t{op}l.l\td1,d2:d0", ['d0', 'd1', 'd2', 'ccr'], ['d0', 'd2', 'ccr'], fqr, rows, cpu=C020)

    case('div_l_64', "DIVU.L/DIVS.L 64/32 Dr:Dq (68020/030/040; unimplemented on 68060), including overflow",
         "MC68020UM/PRM DIVU.L/DIVS.L 64-bit form: Dr:Dq / src -> remainder in Dr, quotient in Dq; overflow if the "
         "quotient does not fit in 32 bits: V=1, C=0, operands unchanged, N,Z undefined (masked)")
    D64 = [(0, 0), (0, 0xFFFFFFFF), (1, 0), (0x7FFFFFFF, 0xFFFFFFFF), (0xFFFFFFFF, 0xFFFFFFFF),
           (0x80000000, 0), (0x00000001, 0x00000001), (0xFFFFFFFF, 0x80000000), (0x00000000, 0x80000000),
           (0x12345678, 0x9ABCDEF0)]
    S64 = [1, 2, 0xFFFFFFFF, 0x7FFFFFFF, 0x80000000, 0x10000, 0x12345679, 7]
    for op in ('divu', 'divs'):
        def f(d, op=op):
            a = (d['d2'] << 32) | d['d0']
            b = d['d1']
            if op == 'divu':
                q, r = a // b, a % b
                ovf = q > M32
            else:
                sa = a - (1 << 64) if a >> 63 else a
                sb = sx(b, 4)
                q = abs(sa) // abs(sb)
                if (sa < 0) != (sb < 0):
                    q = -q
                r = sa - q * sb
                ovf = not (-0x80000000 <= q <= 0x7FFFFFFF)
            if ovf:
                return {'d0': d['d0'], 'd2': d['d2'], 'ccr': ccr_out((d['ccr'] & X) | V, X | V | C)}
            return {'d0': u32(q), 'd2': u32(r), 'ccr': ccr_out((d['ccr'] & X) | nz(u32(q), 4))}
        rows = [(lo, b, hi, ALT[i % 6]) for i, ((hi, lo), b) in enumerate(itertools.product(D64, S64))]
        tpl(f"\t{op}.l\td1,d2:d0", ['d0', 'd1', 'd2', 'ccr'], ['d0', 'd2', 'ccr'], f, rows, cpu=C020N60)


BCDV = [0x00, 0x01, 0x09, 0x10, 0x49, 0x50, 0x99]


def gen_bcd():
    case('abcd_sbcd', "ABCD/SBCD Dy,Dx and -(Ay),-(Ax) over valid BCD pairs for every X/Z input",
         "PRM ABCD/SBCD: decimal dst+src+X / dst-src-X, C=X=decimal carry/borrow, Z cleared if non-zero else unchanged; "
         "N and V undefined (masked)")
    for op in ('abcd', 'sbcd'):
        def f(s_, d_, ccr, op=op):
            x = 1 if ccr & X else 0
            if op == 'abcd':
                r = bcd2i(d_) + bcd2i(s_) + x
                c = r >= 100
                r %= 100
            else:
                r = bcd2i(d_) - bcd2i(s_) - x
                c = r < 0
                r %= 100
            rb = i2bcd(r)
            z = Z if (rb == 0 and ccr & Z) else 0
            return rb, z | ((C | X) if c else 0)
        rows = with_xz([(rv(a, 1, i), rv(b, 1, i + 1)) for i, (a, b) in enumerate(itertools.product(BCDV, BCDV))])
        tpl(f"\t{op}\td1,d0", ['d0', 'd1', 'ccr'], ['d0', 'ccr'],
            lambda d, f=f: (lambda r: {'d0': setlo(d['d0'], r[0], 1), 'ccr': ccr_out(r[1], X | Z | C)})(
                f(d['d1'] & 0xFF, d['d0'] & 0xFF, d['ccr'])), rows)
        rows = [(a & 0xFF, b & 0xFF, 8, 4, c) for a, b, c in rows[::11]]
        tpl(f"\t{op}\t-(a1),-(a0)", ['m7.b', 'm3.b', 'a1@', 'a0@', 'ccr'], ['m3.b', 'a1@', 'a0@', 'ccr'],
            lambda d, f=f: (lambda r: {'m3.b': r[0], 'a1@': 7, 'a0@': 3, 'ccr': ccr_out(r[1], X | Z | C)})(
                f(d['m7.b'], d['m3.b'], d['ccr'])), rows)
    case('nbcd', "NBCD Dn and memory over valid BCD values for every X/Z input",
         "PRM NBCD: 0-dst-X in decimal, C=X=borrow, Z cleared if non-zero else unchanged; N,V undefined (masked)")

    def fn(v, ccr):
        x = 1 if ccr & X else 0
        r = 0 - bcd2i(v) - x
        c = r < 0
        rb = i2bcd(r % 100)
        return rb, (Z if (rb == 0 and ccr & Z) else 0) | ((C | X) if c else 0)
    tpl("\tnbcd\td0", ['d0', 'ccr'], ['d0', 'ccr'],
        lambda d: (lambda r: {'d0': setlo(d['d0'], r[0], 1), 'ccr': ccr_out(r[1], X | Z | C)})(fn(d['d0'] & 0xFF, d['ccr'])),
        with_xz([(rv(b, 1, i),) for i, b in enumerate(BCDV)]))
    tpl("\tnbcd\t(a0)", ['m0.b', 'a0@', 'ccr'], ['m0.b', 'ccr'],
        lambda d: (lambda r: {'m0.b': r[0], 'ccr': ccr_out(r[1], X | Z | C)})(fn(d['m0.b'], d['ccr'])),
        [(b, 0, c) for b, c in zip(BCDV + BCDV[:1], XZ)])
    case('pack_unpk', "PACK/UNPK (68020+) register and -(An) forms with adjustments",
         "MC68020UM/PRM PACK: (src word + adj) -> bits 11-8 and 3-0 packed into one byte; UNPK: byte -> "
         "nibbles in bits 11-8/3-0 + adj; condition codes unaffected")

    def pk(w, adj):
        t = (w + adj) & 0xFFFF
        return ((t >> 4) & 0xF0) | (t & 0x0F)

    def up(b, adj):
        return ((((b & 0xF0) << 4) | (b & 0x0F)) + adj) & 0xFFFF
    W = [0x0000, 0x0F0F, 0x3132, 0x3930, 0xFFFF, 0x1234]
    for adj in (0x0000, 0xCFD0, 0x0101):
        tpl(f"\tpack\td0,d1,#${adj:04x}", ['d0', 'd1', 'ccr'], ['d1', 'ccr'],
            lambda d, adj=adj: {'d1': setlo(d['d1'], pk(d['d0'] & 0xFFFF, adj), 1), 'ccr': ccr_out(d['ccr'])},
            [(0xABCD0000 | w, 0x12345678, ALT[i]) for i, w in enumerate(W)], cpu=C020)
        tpl(f"\tunpk\td0,d1,#${adj:04x}", ['d0', 'd1', 'ccr'], ['d1', 'ccr'],
            lambda d, adj=adj: {'d1': setlo(d['d1'], up(d['d0'] & 0xFF, adj), 2), 'ccr': ccr_out(d['ccr'])},
            [(0xABCDEF00 | (w & 0xFF), 0x12345678, ALT[i]) for i, w in enumerate(W)], cpu=C020)
    tpl("\tpack\t-(a0),-(a1),#$3030", ['m2.w', 'a0@', 'a1@', 'ccr'], ['m9.b', 'a0@', 'a1@', 'ccr'],
        lambda d: {'m9.b': pk(d['m2.w'], 0x3030), 'a0@': 2, 'a1@': 9, 'ccr': ccr_out(d['ccr'])},
        [(w, 4, 10, ALT[i]) for i, w in enumerate(W)], cpu=C020)
    tpl("\tunpk\t-(a0),-(a1),#$3030", ['m3.b', 'a0@', 'a1@', 'ccr'], ['m8.w', 'a0@', 'a1@', 'ccr'],
        lambda d: {'m8.w': up(d['m3.b'], 0x3030), 'a0@': 3, 'a1@': 8, 'ccr': ccr_out(d['ccr'])},
        [(w & 0xFF, 4, 10, ALT[i]) for i, w in enumerate(W)], cpu=C020)


def gen_misc():
    case('ext_swap', "EXT.W, EXT.L, EXTB.L (68020+), SWAP",
         "PRM EXT/EXTB/SWAP: sign extension byte->word, word->long, byte->long; SWAP exchanges the halves; "
         "N,Z from the (32-bit for SWAP) result, V=C=0, X unaffected")
    for ins, fn, cpu in (("ext.w", lambda v: setlo(v, sx(v, 1), 2), ALL),
                         ("ext.l", lambda v: u32(sx(v, 2)), ALL),
                         ("extb.l", lambda v: u32(sx(v, 1)), C020),
                         ("swap", lambda v: ((v << 16) | (v >> 16)) & M32, ALL)):
        s = 2 if ins == 'ext.w' else 4
        vals = [0x00000000, 0x0000007F, 0x00000080, 0x12345680, 0x00007FFF, 0x00008000, 0xFFFF0000, 0x0000FFFF, 0x7F00FF01, 0xFFFFFFFF]
        tpl(f"\t{ins}\td0", ['d0', 'ccr'], ['d0', 'ccr'],
            lambda d, fn=fn, s=s: {'d0': fn(d['d0']), 'ccr': ccr_out((d['ccr'] & X) | nz(fn(d['d0']), s))},
            with_alt([(v,) for v in vals]), cpu=cpu)
    case('exg', "EXG Dx,Dy / Ax,Ay / Dx,Ay",
         "PRM EXG: exchanges two 32-bit registers; condition codes unaffected")
    for a, b in (('d1', 'd6'), ('a1', 'a5'), ('d2', 'a3')):
        tpl(f"\texg\t{a},{b}", [a, b, 'ccr'], [a, b, 'ccr'],
            lambda d, a=a, b=b: {a: d[b], b: d[a], 'ccr': ccr_out(d['ccr'])},
            [(0x12345678, 0x9ABCDEF0, 0x00), (0xFFFFFFFF, 0, 0x1F)])
    case('clr_tst', "CLR.B/.W/.L Dn and memory; TST.B/.W/.L Dn and memory; TST An / #imm / d16(PC) (68020+)",
         "PRM CLR: operand <- 0, N=0 Z=1 V=C=0, X unaffected; TST: N,Z from the operand, V=C=0, X unaffected")
    for s in (1, 2, 4):
        sn = SN[s]
        tpl(f"\tclr.{sn}\td0", ['d0', 'ccr'], ['d0', 'ccr'],
            lambda d, s=s: {'d0': setlo(d['d0'], 0, s), 'ccr': ccr_out((d['ccr'] & X) | Z)},
            with_alt([(0xFFFFFFFF,), (0x12345678,), (0,), (0x80808080,)]))
        tpl(f"\tclr.{sn}\t(a0)+", [f'm0.{sn}', 'a0@', 'ccr'], [f'm0.{sn}', 'a0@', 'ccr'],
            lambda d, s=s, sn=sn: {f'm0.{sn}': 0, 'a0@': s, 'ccr': ccr_out((d['ccr'] & X) | Z)},
            [(msk(s), 0, 0x0B), (0x5A5A5A5A & msk(s), 0, 0x1B)])
        tpl(f"\ttst.{sn}\td0", ['d0', 'ccr'], ['ccr'],
            lambda d, s=s: {'ccr': ccr_out((d['ccr'] & X) | nz(d['d0'], s))}, with_alt(singles(s)))
        tpl(f"\ttst.{sn}\t-(a0)", [f'm{4 - s}.{sn}', 'a0@', 'ccr'], ['a0@', 'ccr'],
            lambda d, s=s, sn=sn: {'a0@': 4 - s, 'ccr': ccr_out((d['ccr'] & X) | nz(d[f'm{4 - s}.{sn}'], s))},
            [(v, 4, c) for v, c in zip(SMALL[s], ALT)])
    for s in (2, 4):
        tpl(f"\ttst.{SN[s]}\ta0", ['a0', 'ccr'], ['ccr'],
            lambda d, s=s: {'ccr': ccr_out((d['ccr'] & X) | nz(d['a0'], s))},
            with_alt([(0,), (0x8000,), (0x00010000,), (0xFFFF0000,), (0x80000000,), (1,)]), cpu=C020)
    for imm in (0, 0x80000000, 0x00000001):
        tpl(f"\ttst.l\t#${imm:x}", ['ccr'], ['ccr'], lambda d, imm=imm: {'ccr': ccr_out((d['ccr'] & X) | nz(imm, 4))},
            [(0x00,), (0x1F,)], cpu=C020)
    tpl("\ttst.w\t.t(pc)\n\trts\n.t:\tdc.w\t$8000", ['ccr'], ['ccr'],
        lambda d: {'ccr': ccr_out((d['ccr'] & X) | N)}, [(0x00,), (0x1F,)], cpu=C020,
        text="tst.w .t(pc) (.t: dc.w $8000)")
    case('tas', "TAS Dn and TAS on a memory byte in the any-pool (the memory form is not run when the pool is on the chip bus: chip or $C00000 slow RAM)",
         "PRM TAS: N,Z from the byte before, V=C=0, X unaffected; bit 7 of the operand is set")
    vals = [0x00, 0x01, 0x7F, 0x80, 0xFF, 0x55]
    tpl("\ttas\td0", ['d0', 'ccr'], ['d0', 'ccr'],
        lambda d: {'d0': d['d0'] | 0x80, 'ccr': ccr_out((d['ccr'] & X) | nz(d['d0'], 1))},
        with_alt([(rv(v, 1, i),) for i, v in enumerate(vals)]))
    tpl("\ttas\t1(a0)", ['m1.b', 'a0@', 'ccr'], ['m1.b', 'ccr'],
        lambda d: {'m1.b': d['m1.b'] | 0x80, 'ccr': ccr_out((d['ccr'] & X) | nz(d['m1.b'], 1))},
        [(v, 0, ALT[i]) for i, v in enumerate(vals)])


def gen_scc_bcc():
    nzvc = list(range(16))
    case('scc', "Scc for all 16 conditions and all 16 NZVC inputs (plus X), register and memory",
         "PRM Scc: destination byte = $FF if the condition is true else $00; condition codes unaffected; "
         "conditions per PRM Table 3-19")
    for cc in range(16):
        tpl(f"\ts{CCN[cc]}\td0", ['d0', 'ccr'], ['d0', 'ccr'],
            lambda d, cc=cc: {'d0': setlo(d['d0'], 0xFF if cond(cc, d['ccr']) else 0, 1), 'ccr': ccr_out(d['ccr'])},
            [(0x12345678 ^ (i * 0x11), c | (X if i & 4 else 0)) for i, c in enumerate(nzvc)])
    tpl("\tsne\t(a0)+", ['m0.b', 'a0@', 'ccr'], ['m0.b', 'a0@', 'ccr'],
        lambda d: {'m0.b': 0xFF if not d['ccr'] & Z else 0, 'a0@': 1, 'ccr': ccr_out(d['ccr'])},
        [(0x5A, 0, c) for c in (0x00, 0x04, 0x1B, 0x1F)])
    case('bcc', "Bcc.S/.W for all 14 conditions x 16 NZVC inputs, backward Bcc.W, Bcc.L (68020+)",
         "PRM Bcc: branch taken iff the condition holds; condition codes unaffected")
    rows = [(0x100, c | (X if c & 2 else 0)) for c in nzvc]
    for cc in range(2, 16):
        for sz in ('s', 'w'):
            tpl(f"\tb{CCN[cc]}.{sz}\t.t\n\tlea\t1(a1),a1\n\trts\n.t:\tlea\t2(a1),a1", ['a1', 'ccr'], ['a1', 'ccr'],
                lambda d, cc=cc: {'a1': d['a1'] + (2 if cond(cc, d['ccr']) else 1), 'ccr': ccr_out(d['ccr'])}, rows)
    for cc in (6, 7, 12, 15):
        tpl(f"\tbra.s\t.f\n.b:\tlea\t2(a1),a1\n\trts\n.f:\tb{CCN[cc]}.w\t.b\n\tlea\t1(a1),a1",
            ['a1', 'ccr'], ['a1', 'ccr'],
            lambda d, cc=cc: {'a1': d['a1'] + (2 if cond(cc, d['ccr']) else 1), 'ccr': ccr_out(d['ccr'])}, rows)
        tpl(f"\tb{CCN[cc]}.l\t.t\n\tlea\t1(a1),a1\n\trts\n.t:\tlea\t2(a1),a1", ['a1', 'ccr'], ['a1', 'ccr'],
            lambda d, cc=cc: {'a1': d['a1'] + (2 if cond(cc, d['ccr']) else 1), 'ccr': ccr_out(d['ccr'])}, rows,
            cpu=C020)
    case('dbcc', "DBcc for all 16 conditions x 16 NZVC inputs, counter 0, 1, $8000, $FFFF",
         "PRM DBcc: if the condition is true, fall through; else Dn.W-1 -> Dn.W and branch unless it became -1; "
         "upper word of Dn and the condition codes unaffected")
    for cc in range(16):
        rows = []
        for i, c in enumerate(nzvc):
            cnt = [0x0000, 0x0001, 0x8000, 0xFFFF][i & 3]
            rows.append((0xA5A50000 | cnt, 0x100, c | (X if i & 8 else 0)))
        name = 'dbra' if cc == 1 else f'db{CCN[cc]}'

        def f(d, cc=cc):
            if cond(cc, d['ccr']):
                return {'d0': d['d0'], 'a1': d['a1'] + 1, 'ccr': ccr_out(d['ccr'])}
            w = (d['d0'] - 1) & 0xFFFF
            return {'d0': setlo(d['d0'], w, 2), 'a1': d['a1'] + (1 if w == 0xFFFF else 2), 'ccr': ccr_out(d['ccr'])}
        tpl(f"\t{name}\td0,.t\n\tlea\t1(a1),a1\n\trts\n.t:\tlea\t2(a1),a1", ['d0', 'a1', 'ccr'], ['d0', 'a1', 'ccr'],
            f, rows)


def gen_flow2():
    case('rts_rtd_rtr', "RTS, RTD #d (68010+), RTR (all 32 CCR values)",
         "PRM RTS: PC <- (SP)+; RTD: PC <- (SP)+, SP += d; RTR: CCR <- (SP)+ (low byte), PC <- (SP)+")
    tpl("\tpea\tANCHOR(pc)\n\trts\n\tlea\t1(a1),a1\n\trts\nANCHOR:\tlea\t2(a1),a1", ['a1', 'ccr'], ['a1', 'ccr'],
        lambda d: {'a1': d['a1'] + 2, 'ccr': ccr_out(d['ccr'])}, [(0, 0), (7, 0x1F)])
    tpl("\tmovea.l\tsp,a4\n\tsubq.l\t#8,sp\n\tpea\t.t(pc)\n\trtd\t#8\n\tlea\t1(a1),a1\n\trts\n.t:\tsuba.l\tsp,a4\n\tlea\t2(a1),a1",
        ['a1', 'ccr'], ['a1', 'a4', 'ccr'],
        lambda d: {'a1': d['a1'] + 2, 'a4': 0, 'ccr': ccr_out(d['ccr'])}, [(0, 0), (7, 0x1F)], cpu=C010,
        text="movea.l sp,a4 / subq.l #8,sp / pea .t(pc) / rtd #8 / .t: suba.l sp,a4 (0) / a1+2")
    tpl("\tpea\t.t(pc)\n\tmove.w\td0,-(sp)\n\trtr\n\tlea\t1(a1),a1\n\trts\n.t:\tlea\t2(a1),a1",
        ['d0', 'a1', 'ccr'], ['a1', 'ccr'],
        lambda d: {'a1': d['a1'] + 2, 'ccr': ccr_out(d['d0'] & 0x1F)},
        [((0xFF00 if c & 1 else 0x0000) | (0xE0 if c & 2 else 0) | c, 0, ALT[c % 6]) for c in range(32)],
        text="pea .t(pc) / move.w d0,-(sp) / rtr / .t: (CCR = d0 & $1F)")
    case('link_unlk', "LINK.W, LINK.L (68020+), UNLK",
         "PRM LINK: SP-4 -> SP, An -> (SP), SP -> An, SP + d -> SP; UNLK: An -> SP, (SP)+ -> An; CCR unaffected")
    for ins, cpu in (("link.w\ta3,#-20", ALL), ("link.l\ta3,#-20", C020)):
        tpl(f"\tmovea.l\tsp,a4\n\t{ins}\n\tmovea.l\tsp,a2\n\tsuba.l\ta4,a2\n\tmovea.l\ta3,a1\n\tsuba.l\ta4,a1"
            "\n\tmovea.l\t(a3),a5\n\tunlk\ta3\n\tsuba.l\tsp,a4",
            ['a3', 'ccr'], ['a1', 'a2', 'a4', 'a5', 'ccr'],
            lambda d: {'a1': u32(-4), 'a2': u32(-24), 'a4': 0, 'a5': d['a3'], 'ccr': ccr_out(d['ccr'])},
            [(0x12345678, 0x00), (0xFFFFFFFF, 0x1F)], cpu=cpu,
            text=f"movea.l sp,a4 / {ins.replace(chr(9), ' ')} / a2=sp-a4 (-24) / a1=a3-a4 (-4) / a5=(a3) / unlk a3 / a4-=sp (0)")
    case('lea_pea', "LEA and PEA through (An), d16(An), d8(An,Xn), abs.W, abs.L",
         "PRM LEA/PEA: the effective address itself (no memory access) -> An / pushed on the stack; CCR unaffected")
    for ea, ins, f, rows in (
            ("(a0)", ['a0@'], lambda d: d['a0@'], [(0,), (12,)]),
            ("-32768(a0)", ['a0@'], lambda d: d['a0@'] - 32768, [(0,)]),
            ("32767(a0)", ['a0@'], lambda d: d['a0@'] + 32767, [(0,)]),
            ("-128(a0,d1.w)", ['a0@', 'd1'], lambda d: d['a0@'] - 128 + sx(d['d1'], 2), [(0, 0xFFFF8000), (0, 0x00017FFF)]),
            ("127(a0,a2.l)", ['a0@', 'a2'], lambda d: d['a0@'] + 127 + d['a2'], [(0, 0xFFFFFF00), (0, 0x00010000)])):
        tpl(f"\tlea\t{ea},a1", ins + ['ccr'], ['a1@', 'ccr'],
            lambda d, f=f: {'a1@': u32(f(d)), 'ccr': ccr_out(d['ccr'])}, [r + (ALT[i],) for i, r in enumerate(rows)])
        tpl(f"\tpea\t{ea}\n\tmovea.l\t(sp)+,a1", ins + ['ccr'], ['a1@', 'ccr'],
            lambda d, f=f: {'a1@': u32(f(d)), 'ccr': ccr_out(d['ccr'])}, [r + (ALT[i + 1],) for i, r in enumerate(rows)])
    for absv in (0x1234, 0x7FFC, 0x8000, 0xFFFE):
        tpl(f"\tlea\t{sx(absv, 2)}.w,a1", ['ccr'], ['a1', 'ccr'],
            lambda d, absv=absv: {'a1': u32(sx(absv, 2)), 'ccr': ccr_out(d['ccr'])}, [(0x00,), (0x1F,)],
            text=f"lea ${absv:x}.w,a1 (sign-extended absolute short)")
    tpl("\tpea\t-32764.w\n\tmovea.l\t(sp)+,a1", ['ccr'], ['a1', 'ccr'],
        lambda d: {'a1': 0xFFFF8004, 'ccr': ccr_out(d['ccr'])}, [(0x00,), (0x1F,)])
    tpl("\tlea\t$12345678,a1", ['ccr'], ['a1', 'ccr'],
        lambda d: {'a1': 0x12345678, 'ccr': ccr_out(d['ccr'])}, [(0x00,), (0x1F,)],
        text="lea $12345678,a1 (absolute long)")


def gen_cas():
    case('cas', "CAS.B/.W/.L Dc,Du,(An) equal and not equal (68020+, aligned)",
         "MC68020UM/PRM CAS: flags of <ea>-Dc (as CMP); if Z, Du -> <ea>, else <ea> -> Dc; X unaffected")
    for s in (1, 2, 4):
        sn = SN[s]
        m = msk(s)
        vals = SMALL[s]
        rows = []
        k = 0
        for mv in vals:
            for dc in (mv, (mv + 1) & m, (mv ^ msb(s)) & m):
                rows.append((rv(dc, s, k), rv(0x5A5A5A5A & m, s, k + 1), mv, 0, ALT[k % 6]))
                k += 1

        def f(d, s=s, sn=sn):
            mv = d[f'm0.{sn}']
            dc = d['d0'] & msk(s)
            fl = f_sub(s, mv, dc)[1] & 0xF
            o = {'ccr': ccr_out((d['ccr'] & X) | fl)}
            if fl & Z:
                o[f'm0.{sn}'] = d['d1'] & msk(s)
                o['d0'] = d['d0']
            else:
                o[f'm0.{sn}'] = mv
                o['d0'] = setlo(d['d0'], mv, s)
            return o
        tpl(f"\tcas.{sn}\td0,d1,(a0)", ['d0', 'd1', f'm0.{sn}', 'a0@', 'ccr'], ['d0', f'm0.{sn}', 'ccr'], f, rows,
            cpu=C020)
    case('cas2', "CAS2.W/.L (68020-68040; unimplemented on 68060): both equal, first differs, second differs",
         "MC68020UM/PRM CAS2: compare (Rn1) with Dc1 then (Rn2) with Dc2; if both equal, Du1/Du2 are stored; "
         "else both memory operands -> Dc1/Dc2; flags from the first compare that differed, else from the second")
    for s in (2, 4):
        sn = SN[s]
        m = msk(s)
        rows = []
        k = 0
        for m1, m2 in ((0x1234 & m, 0x8000 & m), (m, 0), (msb(s), msb(s) - 1)):
            for dc1, dc2 in ((m1, m2), ((m1 + 1) & m, m2), (m1, (m2 - 1) & m), ((m1 ^ m) & m, (m2 ^ 1) & m)):
                rows.append((rv(dc1, s, k), rv(dc2, s, k + 1), rv(0xAAAAAAAA & m, s, k),
                             rv(0x55555555 & m, s, k + 2), m1, m2, 0, 8, ALT[k % 6]))
                k += 1

        def f(d, s=s, sn=sn):
            m1, m2 = d[f'm0.{sn}'], d[f'm8.{sn}']
            f1 = f_sub(s, m1, d['d0'])[1] & 0xF
            o = {}
            if f1 & Z:
                f2 = f_sub(s, m2, d['d1'])[1] & 0xF
                fl = f2
                eq = bool(f2 & Z)
            else:
                fl, eq = f1, False
            if eq:
                o[f'm0.{sn}'] = d['d2'] & msk(s)
                o[f'm8.{sn}'] = d['d3'] & msk(s)
                o['d0'], o['d1'] = d['d0'], d['d1']
            else:
                o[f'm0.{sn}'], o[f'm8.{sn}'] = m1, m2
                o['d0'] = setlo(d['d0'], m1, s)
                o['d1'] = setlo(d['d1'], m2, s)
            o['ccr'] = ccr_out((d['ccr'] & X) | fl)
            return o
        tpl(f"\tcas2.{sn}\td0:d1,d2:d3,(a0):(d4)",
            ['d0', 'd1', 'd2', 'd3', f'm0.{sn}', f'm8.{sn}', 'a0@', 'd4@', 'ccr'],
            ['d0', 'd1', f'm0.{sn}', f'm8.{sn}', 'ccr'], f, rows, cpu=C020N60,
            text=f"cas2.{sn} d0:d1,d2:d3,(a0):(d4) (second address in a data register)")


# --------------------------------------------------------------------------
# addressing-mode matrix (raw-encoded where the exact extension words matter)

def hw(v):
    return f"${v & 0xFFFF:04x}"


def full_ext(da, reg, wl, scale, bs, is_, bdsz, iis):
    return ((da << 15) | (reg << 12) | (wl << 11) | (scale << 9) | 0x100 | (bs << 7) | (is_ << 6) |
            (bdsz << 4) | iis)


def disp_words(v, sz):
    if sz == 1:
        return []
    if sz == 2:
        return [v & 0xFFFF]
    return [(v >> 16) & 0xFFFF, v & 0xFFFF]


def gen_ea():
    case('ea_data', "MOVE.L read/write through every 68000 data addressing mode (incl. A7 byte +/-2, "
                    "abs.W/abs.L, PC-relative, immediate, same register as source and destination)",
         "PRM 2.2 addressing modes: (An), (An)+, -(An), d16(An), d8(An,Xn.W/.L), abs.W (sign-extended), abs.L, "
         "d16(PC), d8(PC,Xn), #imm; byte (An)+/-(An) on A7 steps by 2")
    v = [0x12345678, 0x80000001]
    tpl("\tmove.l\t(a0),d0", ['m0.l', 'a0@', 'ccr'], ['d0', 'ccr'],
        lambda d: {'d0': d['m0.l'], 'ccr': ccr_out((d['ccr'] & X) | nz(d['m0.l'], 4))},
        [(v[0], 0, 0x1F), (v[1], 0, 0x00)])
    tpl("\tmove.l\t(a0)+,d0", ['m0.l', 'a0@', 'ccr'], ['d0', 'a0@', 'ccr'],
        lambda d: {'d0': d['m0.l'], 'a0@': 4, 'ccr': ccr_out((d['ccr'] & X) | nz(d['m0.l'], 4))},
        [(v[0], 0, 0x1F), (0, 0, 0x00)])
    tpl("\tmove.l\t-(a0),d0", ['m0.l', 'a0@', 'ccr'], ['d0', 'a0@', 'ccr'],
        lambda d: {'d0': d['m0.l'], 'a0@': 0, 'ccr': ccr_out((d['ccr'] & X) | nz(d['m0.l'], 4))},
        [(v[0], 4, 0x1F), (v[1], 4, 0x00)])
    for disp in (6, -6, 32767, -32768):
        off = 32 if disp in (6, -6) else 0
        mo = 32 + disp if disp in (6, -6) else None
        if mo is None:
            continue
        tpl(f"\tmove.l\t{disp}(a0),d0", [f'm{mo}.l', 'a0@', 'ccr'], ['d0', 'ccr'],
            lambda d, mo=mo: {'d0': d[f'm{mo}.l'], 'ccr': ccr_out((d['ccr'] & X) | nz(d[f'm{mo}.l'], 4))},
            [(v[0], off, 0x1F), (v[1], off, 0x00)])
    for ea, regs, rows, mo in (("-2(a0,d1.w)", ['d1'], [(0xFFFF0006,), (0x00000006,)], 36),
                               ("4(a0,d1.l)", ['d1'], [(u32(-8),), (u32(-8),)], 28),
                               ("-128(a0,a2.w)", ['a2'], [(0x5A5A0080,), (0x00000080,)], 32),
                               ("127(a0,a2.l)", ['a2'], [(u32(-127 + 4),), (u32(-123),)], 36)):
        tpl(f"\tmove.l\t{ea},d0", [f'm{mo}.l', 'a0@'] + regs + ['ccr'], ['d0', 'ccr'],
            lambda d, mo=mo: {'d0': d[f'm{mo}.l'], 'ccr': ccr_out((d['ccr'] & X) | nz(d[f'm{mo}.l'], 4))},
            [(v[i], 32) + r + (ALT[i],) for i, r in enumerate(rows)])
    # writes
    tpl("\tmove.l\td0,(a0)+", ['d0', 'a0@', 'ccr'], ['m0.l', 'a0@', 'ccr'],
        lambda d: {'m0.l': d['d0'], 'a0@': 4, 'ccr': ccr_out((d['ccr'] & X) | nz(d['d0'], 4))},
        [(v[0], 0, 0x1F), (0, 0, 0x00)])
    tpl("\tmove.w\td0,-(a0)", ['d0', 'a0@', 'ccr'], ['m2.w', 'a0@', 'ccr'],
        lambda d: {'m2.w': d['d0'] & 0xFFFF, 'a0@': 2, 'ccr': ccr_out((d['ccr'] & X) | nz(d['d0'], 2))},
        [(v[0], 4, 0x1F), (0xFFFF8000, 4, 0x00)])
    tpl("\tmove.b\td0,-3(a0,d1.w)", ['d0', 'd1', 'a0@', 'ccr'], ['m5.b', 'ccr'],
        lambda d: {'m5.b': d['d0'] & 0xFF, 'ccr': ccr_out((d['ccr'] & X) | nz(d['d0'], 1))},
        [(0x12345680, 0xFFFF0008, 0, 0x1F), (0x00000000, 0x00000008, 0, 0x00)])
    tpl("\tmove.l\t(a0)+,(a0)+", ['m0.l', 'a0@', 'ccr'], ['m4.l', 'a0@', 'ccr'],
        lambda d: {'m4.l': d['m0.l'], 'a0@': 8, 'ccr': ccr_out((d['ccr'] & X) | nz(d['m0.l'], 4))},
        [(v[0], 0, 0x1F), (v[1], 0, 0x00)], text="move.l (a0)+,(a0)+ (copies (a0) to (a0+4))")
    tpl("\tmove.w\t-(a0),-(a0)", ['m6.w', 'a0@', 'ccr'], ['m4.w', 'a0@', 'ccr'],
        lambda d: {'m4.w': d['m6.w'], 'a0@': 4, 'ccr': ccr_out((d['ccr'] & X) | nz(d['m6.w'], 2))},
        [(0x8000, 8, 0x1F), (0x0001, 8, 0x00)], text="move.w -(a0),-(a0) (copies (a0-2) to (a0-4))")
    tpl("\tmove.b\t1(a0),2(a1)", ['m1.b', 'a0@', 'a1@', 'ccr'], ['m10.b', 'ccr'],
        lambda d: {'m10.b': d['m1.b'], 'ccr': ccr_out((d['ccr'] & X) | nz(d['m1.b'], 1))},
        [(0x80, 0, 8, 0x1F), (0x00, 0, 8, 0x00)])
    # A7 byte steps
    tpl("\tmovea.l\tsp,a4\n\tmove.b\td0,-(sp)\n\tmovea.l\tsp,a2\n\tsuba.l\ta4,a2\n\tmove.b\t(sp)+,d1\n\tsuba.l\tsp,a4",
        ['d0', 'd1', 'ccr'], ['a2', 'a4', 'd1', 'ccr'],
        lambda d: {'a2': u32(-2), 'a4': 0, 'd1': setlo(d['d1'], d['d0'], 1),
                   'ccr': ccr_out((d['ccr'] & X) | nz(d['d0'], 1))},
        [(0x000000A5, 0xFFFFFFFF, 0x1F), (0x12345600, 0, 0x00)],
        text="move.b d0,-(sp) (sp-=2) / move.b (sp)+,d1 (sp+=2)")
    # absolute and PC-relative
    tpl("\tmove.l\t$4.w,d0\n\tmovea.w\t#4,a2\n\tsub.l\t(a2),d0", ['ccr'], ['d0', 'a2', 'ccr'],
        lambda d: {'d0': 0, 'a2': 4, 'ccr': ccr_out(Z)},
        [(0x00,), (0x1F,)], text="move.l $4.w,d0 / movea.w #4,a2 / sub.l (a2),d0 (= 0: abs.W read of location 4)")
    # abs.W against an independently known value: ExecBase->ChkBase (offset 38)
    # is the one's complement of SysBase (exec/execbase.h)
    tpl("\tmove.l\ta2,d2\n\tmovea.l\t$4.w,a2\n\tmove.l\t38(a2),d0\n\tadd.l\ta2,d0\n\tnot.l\td0\n\tmovea.l\td2,a2",
        ['a2@', 'ccr'], ['d0', 'd2@', 'a2@', 'ccr'],
        lambda d: {'d0': 0, 'd2@': d['a2@'], 'a2@': d['a2@'], 'ccr': ccr_out(Z)},   # ADD: X=C=0 (no carry); NOT: Z
        [(0, 0x00), (0x40, 0x1F)],
        text="movea.l $4.w,a2 / move.l 38(a2),d0 / add.l a2,d0 / not.l d0 (= 0: SysBase + ExecBase->ChkBase = $FFFFFFFF)")
    tpl("\tmove.l\td0,cb_absvar\n\tmove.w\tcb_absvar+2,d1\n\tmove.l\tcb_absvar,d2", ['d0', 'd1', 'ccr'],
        ['d1', 'd2', 'ccr'],
        lambda d: {'d1': setlo(d['d1'], d['d0'], 2), 'd2': d['d0'], 'ccr': ccr_out((d['ccr'] & X) | nz(d['d0'], 4))},
        [(0x89ABCDEF, 0x11111111, 0x1F), (0x00010000, 0xFFFFFFFF, 0x00)],
        text="move.l d0,abs.L / move.w abs.L+2,d1 / move.l abs.L,d2 (abs.L in module BSS)")
    tpl("\tmove.l\t.t(pc),d0\n\tmove.w\t.t+4(pc),d1\n\trts\n.t:\tdc.l\t$fedcba98\n\tdc.w\t$0000", ['d1', 'ccr'],
        ['d0', 'd1', 'ccr'], lambda d: {'d0': 0xFEDCBA98, 'd1': setlo(d['d1'], 0, 2), 'ccr': ccr_out((d['ccr'] & X) | Z)},
        [(0xFFFFFFFF, 0x1F), (0x12345678, 0x00)], text="move.l .t(pc),d0 / move.w .t+4(pc),d1 (.t: $fedcba98,$0000)")
    tpl("\tmove.w\t.t(pc,d1.w),d0\n\trts\n.t:\tdc.w\t$1111,$8222,$3333", ['d0', 'd1', 'ccr'], ['d0', 'ccr'],
        lambda d: (lambda w: {'d0': setlo(d['d0'], w, 2), 'ccr': ccr_out((d['ccr'] & X) | nz(w, 2))})(
            [0x1111, 0x8222, 0x3333][sx(d['d1'], 2) // 2]),
        [(0, 0xFFFF0002, 0x00), (0xFFFFFFFF, 0x4), (0x12345678, 0, 0x1F)][::1] if False else
        [(0, 0xFFFF0002, 0x00), (0xFFFFFFFF, 0x00000004, 0x1F), (0x12345678, 0, 0x0A)],
        text="move.w .t(pc,d1.w),d0 (.t: $1111,$8222,$3333)")
    tpl("\tmove.l\t#$80000000,d0\n\tmove.w\t#$0000,d1\n\tmove.b\t#$7f,d2", ['d1', 'd2', 'ccr'], ['d0', 'd1', 'd2', 'ccr'],
        lambda d: {'d0': 0x80000000, 'd1': setlo(d['d1'], 0, 2), 'd2': setlo(d['d2'], 0x7F, 1),
                   'ccr': ccr_out(d['ccr'] & X)}, [(0xFFFFFFFF, 0xFFFFFFFF, 0x1F), (0, 0, 0x00)])

    case('ea_scale', "Brief extension word with scale x1/x2/x4/x8 (68020+), Dn.W and An.L index, negative d8",
         "MC68020UM 2.x: d8(An,Xn.SIZE*SCALE): index sign-extended if .W, multiplied by the scale")
    for scale in range(4):
        for xr, wl, xv in (('d1', 0, 0xFFFF0003), ('a2', 1, u32(-3)), ('d1', 1, 0x00000005)):
            ireg = int(xr[1]) + (8 if xr[0] == 'a' else 0)
            ext = ((ireg >> 3) << 15) | ((ireg & 7) << 12) | (wl << 11) | (scale << 9) | (u32(-16) & 0xFF)
            words = [0x43F0, ext]     # lea (d8,a0,Xn),a1
            idx = (sx(xv, 4) if wl else sx(xv, 2)) << scale
            tpl("\tdc.w\t" + ",".join(hw(w) for w in words), ['a0@', xr, 'ccr'], ['a1@', 'ccr'],
                lambda d, idx=idx: {'a1@': u32(d['a0@'] - 16 + idx), 'ccr': ccr_out(d['ccr'])},
                [(64, xv, 0x00), (128, xv, 0x1F)], cpu=C020,
                text=f"lea (-16,a0,{xr}.{'l' if wl else 'w'}*{1 << scale}),a1 [ext {ext:04x}]")
    # the same through a memory read
    tpl("\tdc.w\t$2030,$1408", ['m40.l', 'd1', 'a0@', 'ccr'], ['d0', 'ccr'],
        lambda d: {'d0': d['m40.l'], 'ccr': ccr_out((d['ccr'] & X) | nz(d['m40.l'], 4))},
        [(0x89ABCDEF, 0xFFFF0008, 0, 0x00), (0, 0x12340008, 0, 0x1F)], cpu=C020,
        text="move.l (8,a0,d1.w*4),d0 [2030 1408]")

    case('ea_full', "Full extension word (68020+): base/index suppress, null/word/long base displacement, "
                    "memory indirect pre- and post-indexed with null/word/long outer displacement, An and PC base",
         "MC68020UM 2.x full format: EA = bd + base + index (no indirect); pre-indexed: ([bd+base+index]) + od; "
         "post-indexed: ([bd+base]) + index + od; IS=1 suppresses the index, BS=1 the base register")
    gen_ea_full()


def gen_ea_full():
    # LEA (full format, An base): opcode $43f0 = lea (ext,a0),a1; PC base: $43fb.
    # index register d1 (Dn) or a2 (An); base a0 = M+64.
    cfgs = []
    for bdsz, bd in ((1, 0), (2, 0x0010), (2, u32(-0x20)), (3, 0x00000030), (3, u32(-0x18000))):
        for bs in (0, 1):
            for is_ in (0, 1):
                cfgs.append((bdsz, bd, bs, is_, 0, 0))
    # memory indirect, An base
    for iis in (1, 2, 3, 5, 6, 7):
        for is_ in ((0, 1) if iis < 4 else (0,)):
            for bdsz, bd in ((1, 0), (2, 0x0008), (3, 0x00000010)):
                od = {1: 0, 2: 0, 3: 0}[iis & 3] if (iis & 3) == 1 else (u32(-6) if (iis & 3) == 2 else 0x00000044)
                cfgs.append((bdsz, bd, 0, is_, iis, od))
    k = 0
    for bdsz, bd, bs, is_, iis, od in cfgs:
        scale = k & 3
        idxreg, wl, xv = (('d1', 0, 0xFFFF0006), ('a2', 1, 0x00000003), ('d1', 1, u32(-2)), ('a2', 0, 0x12340004))[k % 4]
        ireg = int(idxreg[1]) + (8 if idxreg[0] == 'a' else 0)
        ext = full_ext(ireg >> 3, ireg & 7, wl, scale, bs, is_, bdsz, iis)
        words = [0x43F0, ext] + disp_words(bd, bdsz) + (disp_words(od, iis & 3) if iis else [])
        idx = 0 if is_ else ((sx(xv, 4) if wl else sx(xv, 2)) << scale)
        ins = ['a0@', idxreg]
        rows = [64, xv]
        if bs:
            base_rel = False
        else:
            base_rel = True
        text = _full_text(bdsz, bd, bs, is_, iis, od, idxreg, wl, scale)
        if iis == 0:
            if base_rel:
                out = 'a1@'
                fn = (lambda d, bd=bd, idx=idx: {'a1@': u32(d['a0@'] + sx(bd, 4) + idx), 'ccr': ccr_out(d['ccr'])})
            else:
                out = 'a1'
                fn = (lambda d, bd=bd, idx=idx: {'a1': u32(sx(bd, 4) + idx), 'ccr': ccr_out(d['ccr'])})
            tpl("\tdc.w\t" + ",".join(hw(w) for w in words), ins + ['ccr'], [out, 'ccr'], fn,
                [tuple(rows) + (ALT[k % 6],)], cpu=C020, text=text + " [" + " ".join(f"{w:04x}" for w in words) + "]")
        else:
            # pointer location: base + bd (+ index if pre-indexed)
            pre = (iis < 4) and not is_
            ploc = 64 + sx(bd, 4) + (idx if pre else 0)
            assert -200 < ploc < 400, (ploc, text)
            pslot = f'm{ploc}.l@'
            ptr = 0x100 + 4 * (k % 8)        # pointer value: M + $100 + ..
            add = sx(od, 4) + (0 if (pre or is_) else idx)
            tpl("\tdc.w\t" + ",".join(hw(w) for w in words), ins + [pslot, 'ccr'], ['a1@', 'ccr'],
                lambda d, add=add, pslot=pslot: {'a1@': u32(d[pslot] + add), 'ccr': ccr_out(d['ccr'])},
                [tuple(rows) + (ptr, ALT[k % 6])], cpu=C020,
                text=text + " [" + " ".join(f"{w:04x}" for w in words) + "]")
        k += 1
    # data read through memory indirect post-indexed: move.l ([8,a0],d1.w*2,4),d0
    ext = full_ext(0, 1, 0, 1, 0, 0, 2, 6)
    words = [0x2030, ext, 0x0008, 0x0004]
    tpl("\tdc.w\t" + ",".join(hw(w) for w in words), ['a0@', 'd1', 'm72.l@', 'm176.l', 'ccr'], ['d0', 'ccr'],
        lambda d: {'d0': d['m176.l'], 'ccr': ccr_out((d['ccr'] & X) | nz(d['m176.l'], 4))},
        [(64, 0xFFFF0006, 160, 0x8000ABCD, 0x00), (64, 0x00000006, 160, 0x00000000, 0x1F)], cpu=C020,
        text="move.l ([8,a0],d1.w*2,4),d0 [" + " ".join(f"{w:04x}" for w in words) + "]")
    # PC-relative full format: lea (bd,pc,d1.l*4),a1 with the anchor at the extension word
    for bdsz, bd, bs in ((2, 0x0100, 0), (3, u32(-0x40), 0), (2, 0x0100, 1), (1, 0, 0)):
        ext = full_ext(0, 1, 1, 2, bs, 0, bdsz, 0)
        words = disp_words(bd, bdsz)
        asm = "\tdc.w\t$43fb\nANCHOR:\tdc.w\t" + ",".join(hw(w) for w in [ext] + words)
        out = 'a1' if bs else 'a1^'
        tpl(asm, ['d1', 'ccr'], [out, 'ccr'],
            lambda d, bd=bd, out=out: {out: u32(sx(bd, 4) + (d['d1'] << 2)), 'ccr': ccr_out(d['ccr'])},
            [(3, 0x00), (u32(-5), 0x1F)], cpu=C020,
            text=f"lea ({'' if bdsz == 1 else hex(sx(bd, 4))},{'zpc' if bs else 'pc'},d1.l*4),a1 [43fb {ext:04x}"
                 + "".join(f" {w:04x}" for w in words) + "]")
    # PC memory indirect: lea ([bd,pc]),a1 reading a pointer stored in the code
    ext = full_ext(0, 0, 0, 0, 0, 1, 2, 1)
    asm = ("\tdc.w\t$43fb\nANCHOR:\tdc.w\t" + hw(ext) + ",.p-ANCHOR\n\tbra.s\t.e\n\tcnop\t0,2\n.p:\tdc.l\t$00c0ffee\n.e:")
    tpl(asm, ['ccr'], ['a1', 'ccr'], lambda d: {'a1': 0x00C0FFEE, 'ccr': ccr_out(d['ccr'])}, [(0x00,), (0x1F,)],
        cpu=C020, text=f"lea ([.p,pc]),a1 (.p: dc.l $00c0ffee) [43fb {ext:04x} ....]")


def _full_text(bdsz, bd, bs, is_, iis, od, idxreg, wl, scale):
    b = '' if bdsz == 1 else (f"{sx(bd, 4):#x}" + ('.l' if bdsz == 3 else '.w'))
    base = 'za0' if bs else 'a0'
    ix = '' if is_ else f"{idxreg}.{'l' if wl else 'w'}*{1 << scale}"
    o = '' if (iis & 3) in (0, 1) else (f"{sx(od, 4):#x}" + ('.l' if (iis & 3) == 3 else '.w'))
    if iis == 0:
        parts = [p for p in (b, base, ix) if p]
        return f"lea ({','.join(parts)}),a1"
    if iis < 4 and not is_:
        inner = ','.join(p for p in (b, base, ix) if p)
        return f"lea ([{inner}]{',' + o if o else ''}),a1 (pre-indexed)"
    if is_:
        inner = ','.join(p for p in (b, base) if p)
        return f"lea ([{inner}]{',' + o if o else ''}),a1 (index suppressed)"
    inner = ','.join(p for p in (b, base) if p)
    return f"lea ([{inner}],{ix}{',' + o if o else ''}),a1 (post-indexed)"


# --------------------------------------------------------------------------
# JMP/JSR (written plainly; replaces the draft in gen_flow)

def gen_jmp():
    case('jmp_jsr', "JMP/JSR through (An), d16(An), d8(An,Xn), abs.L, d16(PC), d8(PC,Xn); JSR return address",
         "PRM JMP/JSR: PC <- effective address; JSR pushes the address of the instruction after it; CCR unaffected")
    # landing pad at ANCHOR: +2 then +4 (entering at ANCHOR+4 skips the +2)
    pad = "ANCHOR:\tlea\t2(a1),a1\n\tlea\t4(a1),a1\n\trts"
    forms = [("(a0)", ['a0^'], [(0,)]),
             ("8(a0)", ['a0^'], [(u32(-8),)]),
             ("-4(a0,d1.w)", ['a0^', 'd1'], [(0, 0xFFFF0004), (u32(-4), 0x12340008)]),
             ("ANCHOR", [], [()]),
             ("ANCHOR(pc)", [], [()]),
             ("ANCHOR(pc,d1.l)", ['d1'], [(0,), (4,)])]
    for ins in ('jmp', 'jsr'):
        for ea, ins_, rows in forms:
            def entry(d, ea=ea):
                if ea == '(a0)':
                    e = d['a0^']
                elif ea == '8(a0)':
                    e = d['a0^'] + 8
                elif ea == '-4(a0,d1.w)':
                    e = d['a0^'] - 4 + sx(d['d1'], 2)
                elif 'd1.l' in ea:
                    e = d['d1']
                else:
                    e = 0
                return u32(e)

            def f(d, ins=ins, entry=entry):
                e = entry(d)
                inc = (6 if e == 0 else 4) + (1 if ins == 'jsr' else 0)
                o = {'a1': d['a1'] + inc, 'ccr': ccr_out(d['ccr'])}
                if ins == 'jsr':
                    o['a2'] = 0
                return o
            if ins == 'jmp':
                asm = f"\tjmp\t{ea}\n\tlea\t16(a1),a1\n\trts\n{pad}"
                outs = ['a1', 'ccr']
            else:
                # a2 = return address (read at the target) - address of .r = 0; a3 is restored
                asm = (f"\tjsr\t{ea}\n.r:\tlea\t1(a1),a1\n\tlea\t.r(pc),a3\n\tsuba.l\ta3,a2\n\tmovea.l\ta4,a3\n\trts\n"
                       f"ANCHOR:\tmovea.l\t(sp),a2\n\tlea\t2(a1),a1\n\tlea\t4(a1),a1\n\trts")
                outs = ['a1', 'a2', 'ccr']
            if ins == 'jsr' and ea.endswith('(pc,d1.l)'):
                # entering at ANCHOR+4 would skip the return-address read: use offsets 0 only
                rows = [(0,)]
            if ins == 'jsr':
                # entering past ANCHOR skips "movea.l (sp),a2" (+2 bytes) - keep JSR entries at ANCHOR
                rows = [r for r in rows if entry(dict(zip(ins_, r), a1=0, ccr=0)) == 0] or rows[:1]
                ins2 = ins_ + ['a4']
                rows = [r + (0x13572468,) for r in rows]

                def f2(d, f=f):
                    o = f(d)
                    o['a3'] = d['a4']
                    return o
                tpl(asm, ins2 + ['a1', 'ccr'], outs + ['a3'], f2,
                    [r + (0x100, ALT[i]) for i, r in enumerate(rows)],
                    text=f"jsr {ea} (target reads the return address)")
            else:
                tpl(asm, ins_ + ['a1', 'ccr'], outs, f, [r + (0x100, ALT[i]) for i, r in enumerate(rows)],
                    text=f"jmp {ea}")
    # 68020 memory indirect JMP: jmp ([0,a0]) with the target pointer in memory
    ext = full_ext(0, 0, 0, 1, 0, 1, 1, 1)
    tpl(f"\tdc.w\t$4ef0,{hw(ext)}\n\tlea\t16(a1),a1\n\trts\n{pad}", ['a0@', 'm0.l^', 'a1', 'ccr'], ['a1', 'ccr'],
        lambda d: {'a1': d['a1'] + 6, 'ccr': ccr_out(d['ccr'])}, [(0, 0, 0x100, 0x00), (0, 0, 0x200, 0x1F)],
        cpu=C020, text=f"jmp ([a0]) (pointer to the target in memory) [4ef0 {ext:04x}]")


# --------------------------------------------------------------------------
# emitter

def build():
    gen_move()
    gen_imm_ccr()
    gen_movem()
    gen_movep()
    gen_addsub()
    gen_neg()
    gen_cmp()
    gen_cmp2_chk()
    gen_logic()
    gen_shifts()
    gen_bitops()
    gen_bitfield()
    gen_muldiv()
    gen_bcd()
    gen_misc()
    gen_scc_bcc()
    gen_flow_clean()
    gen_jmp()
    gen_flow2()
    gen_cas()
    gen_ea()


def gen_flow_clean():
    case('bra_bsr', "BRA.S/.W/.L, BSR.S/.W/.L (.L 68020+): target and pushed return address",
         "PRM BRA/BSR: PC + 2 + displacement; BSR pushes the address of the next instruction; CCR unaffected")
    for sz, cpu in (('s', ALL), ('w', ALL), ('l', C020)):
        tpl(f"\tbra.{sz}\t.t\n\tlea\t1(a1),a1\n\trts\n.t:\tlea\t2(a1),a1", ['a1', 'ccr'], ['a1', 'ccr'],
            lambda d: {'a1': d['a1'] + 2, 'ccr': ccr_out(d['ccr'])}, [(0, 0x00), (5, 0x1F)], cpu=cpu)
        tpl(f"\tbsr.{sz}\t.sub\nANCHOR:\tlea\t1(a1),a1\n\trts\n.sub:\tmovea.l\t(sp),a2\n\tlea\t2(a1),a1\n\trts",
            ['a1', 'ccr'], ['a1', 'a2^', 'ccr'],
            lambda d: {'a1': d['a1'] + 3, 'a2^': 0, 'ccr': ccr_out(d['ccr'])}, [(0, 0x00), (5, 0x1F)], cpu=cpu,
            text=f"bsr.{sz} .s / ANCHOR: a1+1 / rts / .s: movea.l (sp),a2 (return address = ANCHOR) / a1+2 / rts")


FAIL = {
    'move': "flag computation of MOVE (V/C not cleared, X touched) or a sized write clobbering the upper register bits",
    'movea': "missing sign extension of MOVEA.W, or MOVEA wrongly setting flags",
    'moveq': "sign extension or flag bug in MOVEQ",
    'move_ccr_sr': "SR/CCR transfer masks wrong (upper byte not ignored, unimplemented SR bits readable), or MOVE from CCR missing on 68010+",
    'logic_imm_ccr': "ANDI/ORI/EORI to CCR combine the wrong bits",
    'logic_imm_sr': "ANDI/ORI/EORI to SR mishandle the interrupt mask or the CCR",
    'movem_l': "MOVEM register order, address update, or the base-in-list rule for (An)+ is wrong",
    'movem_w': "MOVEM.W to registers not sign-extending (data registers included)",
    'movem_predec_base_000': "the 68000/010 value of An stored by MOVEM -(An) is emulated like a 68020 (or vice versa)",
    'movem_predec_base_020': "the 68020+ value of An stored by MOVEM -(An) is emulated like a 68000 (or vice versa)",
    'movep': "MOVEP byte lane / ordering bug, or the upper word of Dn changed by MOVEP.W",
    'cmp2_chk2': "CMP2/CHK2 bound comparison (signedness, An sign-extension, Z on equal bound) wrong",
    'chk': "CHK trapping on an in-range value or changing X/registers",
    'mul_w': "16x16 multiply sign handling or flags", 'div_w': "DIVU/DIVS quotient/remainder sign, overflow detection, or the destination changed on overflow",
    'mul_l': "32-bit multiply overflow (V) detection or flags", 'mul_l_64': "64-bit product register assignment (Dh/Dl) or N/Z of the 64-bit result",
    'div_l': "32-bit divide rounding/remainder sign, the Dr:Dq assignment, or MIN/-1 overflow",
    'div_l_64': "64/32 divide overflow detection or register assignment",
    'abcd_sbcd': "decimal adjust, decimal carry/borrow, or the sticky Z rule", 'nbcd': "NBCD decimal adjust or sticky Z",
    'pack_unpk': "PACK/UNPK nibble placement, adjustment, or -(An) byte order",
    'ext_swap': "sign extension width or SWAP flags", 'exg': "EXG register selection (opmode) wrong",
    'clr_tst': "CLR/TST flags, or TST An/#imm/PC-relative not accepted on 68020+",
    'tas': "TAS flags taken after setting bit 7, or the byte not written",
    'scc': "condition evaluation (PRM Table 3-19) wrong for some NZVC combination",
    'bcc': "condition evaluation or branch displacement (.S/.W/.L, backward) wrong",
    'dbcc': "DBcc counter handling (word only, -1 termination) or condition evaluation wrong",
    'bra_bsr': "branch displacement or the return address pushed by BSR wrong",
    'jmp_jsr': "effective address computation of a control mode, or the JSR return address",
    'rts_rtd_rtr': "stack handling of RTS/RTD/RTR, or RTR not restoring exactly the low 5 CCR bits",
    'link_unlk': "LINK/UNLK frame pointer or stack arithmetic (LINK.L displacement on 68020+)",
    'lea_pea': "effective address arithmetic (sign extension of d16/d8/abs.W/index.W)",
    'cas': "CAS compare direction (ea - Dc), or the update/load selection", 'cas2': "CAS2 dual compare order, flags source, or load-back of both Dc registers",
    'ea_data': "an addressing mode computing the wrong address or the wrong register update (A7 byte step, same-register source/destination)",
    'ea_scale': "the 68020 index scale factor or index sign extension",
    'ea_full': "68020 full-format extension word decoding: BS/IS, bd/od size, pre- vs post-indexing, PC base",
}


def esc(s):
    return s.replace('"', "'").replace('\t', ' ')


def emit(asm_path, doc_path):
    out = []
    w = out.append
    w("; generated by tools/gen_cpu_basic.py - do not edit")
    w("; cpu_basic snippets and test vectors (see the generator for the format)")
    w("\tmachine\t68020")
    w("\topt\to-")
    w("\txdef\t_cb_cases")
    w("\txdef\t_cb_ncases")
    w("\txdef\t_cb_tpls")
    w("\txdef\t_cb_ntpls")
    w("\txdef\t_cb_pool")
    w("\tsection\t.text,code")
    for i, t in enumerate(templates):
        w(f"cbs_{i}:")
        for l in t.asm:
            l = l.replace('ANCHOR', f'cba_{i}')
            # snippet-local labels become unique global ones (the anchor label would end a local scope)
            l = re.sub(r'(?<![\w)])\.(t2|t|b|f|r|sub|p|e)\b', lambda m: f'cbl_{i}_{m.group(1)}', l)
            w(l)
        w(f"cbe_{i}:")
        w("\trts")
    w("\tsection\t.data,data")
    w("\teven")
    w("_cb_ncases:\tdc.w\t%d" % len(cases))
    w("_cb_ntpls:\tdc.w\t%d" % len(templates))
    w("_cb_cases:")
    ti = 0
    for ci, c in enumerate(cases):
        w(f"\tdc.l\tcbcn_{ci},cbcr_{ci}")
        w(f"\tdc.w\t{ti},{len(c.tpls)}")
        ti += len(c.tpls)
    w("_cb_tpls:")
    for i, t in enumerate(templates):
        nw = f"(cbe_{i}-cbs_{i})/2"
        w(f"\tdc.l\tcbs_{i},cbs_{i}")
        w(f"\tdc.w\t{nw},{len(t.vecs)}")
        w(f"\tdc.l\tcbt_{i},{('cba_%d' % i) if t.anchor else '0'},cbsl_{i},cbv_{i}")
        w(f"\tdc.w\t${t.cpu:04x}")
        w(f"\tdc.b\t{len(t.ins)},{len(t.outs)}")
        w(f"\tdc.w\t{t.win[0]},{t.win[1]},0,0")
    for i, t in enumerate(templates):
        w(f"cbsl_{i}:\tdc.w\t" + ",".join(f"${s.code:04x}" for s in t.ins + t.outs))
    # vector values are indices into one pool of distinct 32-bit values
    pool = sorted(set(x for t in templates for v in t.vecs for x in v))
    assert len(pool) < 65536
    pidx = {x: i for i, x in enumerate(pool)}
    for i, t in enumerate(templates):
        w(f"cbv_{i}:")
        for v in t.vecs:
            w("\tdc.w\t" + ",".join(str(pidx[x]) for x in v))
    w("\tcnop\t0,4")
    w("_cb_pool:")
    for k in range(0, len(pool), 8):
        w("\tdc.l\t" + ",".join(f"${x:08x}" for x in pool[k:k + 8]))
    for i, t in enumerate(templates):
        w(f"cbt_{i}:\tdc.b\t\"{esc(t.text)}\",0")
    for ci, c in enumerate(cases):
        w(f"cbcn_{ci}:\tdc.b\t\"{c.id}\",0")
        w(f"cbcr_{ci}:\tdc.b\t\"{esc(c.rule)}\",0")
    w("\tsection\t.bss,bss")
    w("\tcnop\t0,4")
    w("cb_absvar:\tds.l\t2")
    with open(asm_path, 'w') as f:
        f.write("\n".join(out) + "\n")

    d = []
    d.append("# cpu_basic (module 2): integer instruction set")
    d.append("")
    d.append("Generated by `tools/gen_cpu_basic.py`, which also holds the reference model. Each case first "
             "compares its templates' vectors with the table (quick mode samples every fourth vector). Each "
             "subsequent hot round executes **every supported snippet**, using vector "
             "`round mod snippet_vector_count`, and checks the result against the independent table, "
             "including round 0. There are at least `amt_hot_reps()+1` rounds and enough rounds for every "
             "vector of every snippet to be checked after four warm-up executions. Thus quick/full "
             "`ea_full` cover all 53 supported snippets, not just 19 in quick mode. These counts establish "
             "execution coverage, not actual JIT activation or the cause of an emulator failure. A failure lists the "
             "instruction text and opcode words, the inputs, the expected and actual outputs and the mask of "
             "defined CCR bits (`ccr_mask`; undefined flags are masked out). Register values marked `@m` are "
             "offsets from the module's memory window base M, `@anchor` offsets from a label in the snippet.")
    d.append("")
    d.append("Native regression checks (no emulator): `TMPDIR=$PWD/work/tmp PYTHONDONTWRITEBYTECODE=1 "
             "python3 -B -m unittest discover -s runner/tests -t runner -p test_cpu_audit_fixes.py -v`. "
             "These compile production CPU helpers/callbacks, check the generated `ea_full` snippet "
             "counts, and reject old-code mutations.")
    d.append("")
    d.append("Cases whose templates all need a CPU the machine lacks report `skip`; the CPUs a template runs on are "
             "fixed in the generator (e.g. MOVEP, CMP2/CHK2, CAS2, 64-bit MUL/DIV are not run on the 68060, "
             "where they are unimplemented and trap: exceptions module).")
    d.append("")
    d.append("| case | intent | rule (manual) | vectors | CPUs | a failure usually means |")
    d.append("|---|---|---|---|---|---|")
    for c in cases:
        nv = sum(len(t.vecs) for t in c.tpls)
        m = 0
        for t in c.tpls:
            m |= t.cpu
        cpus = cpu_desc(m)
        fail = FAIL.get(c.id)
        if fail is None:
            base = c.id.split('_')[0].rstrip('iq') if c.id in ('addi', 'addq', 'subi', 'subq') else c.id.split('_')[0]
            fail = {'add': "ADD-family carry/overflow/X computation or a sized write clobbering upper bits",
                    'sub': "SUB-family borrow/overflow/X computation or a sized write clobbering upper bits",
                    'addx': "X not added or the sticky Z rule not applied", 'subx': "X not subtracted or the sticky Z rule not applied",
                    'neg': "NEG/NEGX/NOT flag computation or the sticky Z rule of NEGX",
                    'cmp': "CMP-family flags (borrow/overflow) or X modified by a compare",
                    'cmpa': "CMPA.W source not sign-extended or compared at the wrong size",
                    'cmpi': "CMPI flags, or CMPI to (d16,PC) not allowed on 68020+",
                    'cmpm': "CMPM operand order or post-increment",
                    'and': "logical result/flags wrong or X touched", 'or': "logical result/flags wrong or X touched",
                    'eor': "logical result/flags wrong or X touched",
                    'btst': "bit number modulo (32 for Dn, 8 for memory) or Z/other flags", 'bchg': "bit number modulo or Z/other flags",
                    'bclr': "bit number modulo or Z/other flags", 'bset': "bit number modulo or Z/other flags",
                    }.get(base)
            if c.id in SHIFTS:
                fail = "shift/rotate count handling (modulo 64, count 0, count >= size) or the C/X/V rules"
            if c.id in BFOPS:
                fail = "bit field offset/width decoding (wrap-around in Dn, signed memory offsets, width 0 = 32) or N/Z"
            if fail is None:
                fail = c.fail
        d.append(f"| `{c.id}` | {c.intent} | {c.rule} | {nv} in {len(c.tpls)} forms | {cpus} | {fail} |")
    with open(doc_path, 'w') as f:
        f.write("\n".join(d) + "\n")


def cpu_desc(m):
    if m == ALL:
        return "all"
    if m == C010:
        return "68010+"
    if m == C020:
        return "68020+"
    if m == C020N60:
        return "68020-68040"
    if m == NO060:
        return "68000-68040"
    if m == C000_010:
        return "68000/68010"
    if m == ALL & ~0:
        return "all"
    return ",".join(CPUS[i] for i in range(12) if m >> i & 1)


def main():
    ap = argparse.ArgumentParser()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument('--asm', default=os.path.join(root, 'disk', 'modules', 'cpu_basic_tab.s'))
    ap.add_argument('--doc', default=os.path.join(root, 'docs', 'cases', 'cpu_basic.md'))
    a = ap.parse_args()
    build()
    os.makedirs(os.path.dirname(a.doc), exist_ok=True)
    emit(a.asm, a.doc)
    nv = sum(len(t.vecs) for t in templates)
    words = sum(len(t.vecs) * (len(t.ins) + len(t.outs)) for t in templates)
    print(f"{len(cases)} cases, {len(templates)} templates, {nv} vectors, {words * 4} bytes of vector data",
          file=sys.stderr)


if __name__ == '__main__':
    main()
