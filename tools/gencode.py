#!/usr/bin/env python3
"""gencode.py: seeded random code blocks for the dispatch module (id 22).

usage: tools/gencode.py [--asm disk/modules/dispatch_tab.s] [--doc docs/cases/dispatch.md]
       tools/gencode.py --trace <case> <block>     (print one block and its expected trace)

For each CPU class (c000: 68000 ISA; c020: 68020/030/040 ISA; c040: the
68040/68060 integer ISA, i.e. c020 minus the instructions the 68060 does not
implement) and each seed group, this generates blocks of random but valid
integer code: ALU ops over all the addressing modes of the class, MOVEM,
bit fields, MUL/DIV, BCD, CCR transfers, and control flow (forward Bcc/BRA,
DBcc loops, BSR/JSR/RTS/RTD subroutines, LINK/UNLK frames, stack push/pop).

Every candidate instruction is executed on a reference model while the block
is generated, and kept only if it is valid in the state it will see (memory
accesses inside the window, no address errors on c000, no traps, no reads of
undefined flags or uninitialised stack). The model uses the reference
semantics of tools/gen_cpu_basic.py (f_add, f_sub, f_shift, cond, bf_model:
written from the M68000 PRM and the 68020/030/040/060 user manuals).

The guest (disk/modules/dispatch.c) builds the same initial state from the
block's seed, runs the block, and compares registers, CCR (masked to the
defined flags) and the memory window with the expectation. On a mismatch it
re-runs the block with the first opcode word of instruction k replaced by
TRAP #15, for every k in first-arrival order, to find the first checkpoint
whose state differs.

Machine model of a block (offsets are relative to M, the window base in the
module's any-pool):
  M+$000..$0FF  data window (checked)
  M+$100..$7FF  stack (block SP starts at $7FC; $7FC holds the harness return
                address; interrupts may use the stack below SP)
  d0-d7, a4, a5 data registers; a0-a3, a6, a7 pointers (values M-relative)

Expected-state stream per block (bytes; multi-byte values big-endian):
  u32 seed; u8 nstatic; u8 nentries; u8 woff[nstatic] (word offsets of the
  static instructions from the block start); then nentries entries in
  first-arrival order. Entry = the state on the first arrival at static
  instruction k (k=$FF: the final state after the block returned), as a
  delta from the previous entry (the first one: from the initial state):
    u8 hdr: bit7 k/pred bytes follow (else k = previous k + 1 and
            pred = previous k), bit6 d-register mask byte follows, bit5
            a-register mask byte follows, bit4 window runs follow, bit3
            defined-flags mask byte follows, bit2 CCR byte follows
    [u8 k, u8 pred]   pred: the instruction executed just before ($FF none)
    [u8 dmask] [u8 amask]  bit n: register dn / an changed
    u32 value per changed register, d0..d7 then a0..a7 (pointers M-relative)
    [u8 defmask] [u8 ccr]  (ccr holds 0 in undefined flags)
    [u8 nruns, nruns x (u8 off, u8 len, len bytes)]  changed window bytes
Instruction texts (dst_<n>): per static instruction, its disassembly (branch
targets as @<index>) byte-pair encoded with the global table _ds_bpe (code c
expands to the two codes at 2c, 2c+1; (0,0) = literal byte), 0-terminated.
"""
import argparse
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_cpu_basic as cb  # noqa: E402
from gen_cpu_basic import X, N, Z, V, C, msk, sx, u32, nz, setlo, f_add, f_sub, f_shift, cond, bf_model  # noqa: E402

M32 = 0xFFFFFFFF
WIN = 256
STK_LO = 0x700          # lowest address the block's own stack use may reach
SP0 = 0x7FC
STK_TOP = 0x800
SC = {1: 'b', 2: 'w', 4: 'l'}

CLASSES = [
    ('c000', "68000 ISA (runs on every CPU; no MOVEP, which the 68060 does not implement)", 0xFFF),
    ('c020', "68020/030 ISA: 020 indexed modes with scaled index, brief format and full format (bd,An,Xn) (the memory-indirect and PC-relative 020 modes are covered by cpu_basic only), bit fields, 32/64-bit MUL/DIV, EXTB, PACK/UNPK, "
             "MOVE from CCR, RTD, LINK.L, Bcc.L, misaligned accesses (68020/030/040)", 0x1FC),
    ('c040', "68040/68060 integer ISA: the c020 set minus the instructions the 68060 does not implement "
             "(64-bit MUL/DIV, MOVEP, CAS2, CHK2/CMP2) (68040/68060)", 0xFC0),
]
GROUPS = 3
NBLK = 48           # blocks per (class, group), full mode
NBLK_QUICK = 16     # quick mode: the first NBLK_QUICK blocks, same case ids
TARGET = (9, 16)    # static instructions per block (main part)

ROLE = ['D'] * 8 + ['M', 'M', 'M', 'M', 'D', 'D', 'M', 'M']
PREGS = [8, 9, 10, 11, 14]
DAREGS = [12, 13]
CC_NEED = [0, 0, C | Z, C | Z, C, C, Z, Z, V, V, N, N, N | V, N | V, N | V | Z, N | V | Z]
CCN = cb.CCN


def rn(r):
    if r == 15:
        return 'sp'
    return 'd%d' % r if r < 8 else 'a%d' % (r - 8)


class Bad(Exception):
    pass


class Label:
    n = 0

    def __init__(self):
        Label.n += 1
        self.name = 'L%d' % Label.n
        self.ins = None


END = object()


def resolve(t):
    while isinstance(t, Label):
        t = t.ins
    return t


class Ins:
    def __init__(self, text, ex):
        self.text = text
        self.ex = ex
        self.next = None
        self.idx = None
        self.ref = None


# --------------------------------------------------------------------------
# machine state

class St:
    __slots__ = ('r', 'ccr', 'undef', 'win', 'stk', 'cls')

    def copy(self):
        s = St()
        s.r = list(self.r)
        s.ccr, s.undef, s.cls = self.ccr, self.undef, self.cls
        s.win = bytearray(self.win)
        s.stk = dict(self.stk)
        return s

    def snap(self):
        regs = tuple(u32(v) for v in self.r)
        return regs, self.ccr & ~self.undef & 0x1F, ~self.undef & 0x1F, bytes(self.win)


def xs(x):
    x ^= (x << 13) & M32
    x ^= x >> 17
    x ^= (x << 5) & M32
    return x


def init_state(seed, cls):
    """same formula as ds_init() in disk/modules/dispatch.c"""
    x = seed
    vals = []
    for _ in range(16 + 64):
        x = xs(x)
        vals.append(x)
    s = St()
    s.cls = cls
    s.r = [0] * 16
    for i in range(8):
        s.r[i] = vals[i]
    s.r[12], s.r[13] = vals[8], vals[9]
    for j, i in enumerate(PREGS):
        s.r[i] = 0x30 + ((vals[10 + j] >> 16) & 0x9E)
    s.r[15] = SP0
    s.ccr = vals[15] & 0x1F
    s.undef = 0
    s.win = bytearray()
    for i in range(64):
        s.win += vals[16 + i].to_bytes(4, 'big')
    s.stk = {SP0 + i: ('H', None, i) for i in range(4)}
    return s


def setcc(st, v, m):
    st.ccr = (st.ccr & ~m) | (v & m)
    st.undef &= ~m


def need(st, m):
    if st.undef & m:
        raise Bad('reads undefined flags')


def set_ptr(st, r, rel):
    rel = sx(rel & M32, 4)
    if r == 15:
        if rel > st.r[15]:
            for a in [a for a in st.stk if a < rel]:
                del st.stk[a]
    st.r[r] = rel


def set_reg(st, r, v):
    """whole-register write of a data value (roles enforced)"""
    if ROLE[r] != 'D':
        raise AssertionError('data value into pointer register %s' % rn(r))
    st.r[r] = u32(v)


def chk_addr(st, a, s):
    if st.cls == 0 and s > 1 and (a & 1):
        raise Bad('odd address on 68000')
    if 0 <= a and a + s <= WIN:
        return 0
    if a >= st.r[15] and a >= STK_LO and a + s <= SP0:
        return 1
    raise Bad('outside the window')


def mrd(st, a, s):
    if chk_addr(st, a, s) == 0:
        return int.from_bytes(st.win[a:a + s], 'big')
    v = 0
    for i in range(s):
        e = st.stk.get(a + i)
        if not e or e[0] != 'D':
            raise Bad('uninitialised stack')
        v = (v << 8) | e[1]
    return v


def mwr(st, a, s, v):
    if chk_addr(st, a, s) == 0:
        st.win[a:a + s] = (v & msk(s)).to_bytes(s, 'big')
        return
    for i in range(s):
        st.stk[a + i] = ('D', (v >> (8 * (s - 1 - i))) & 0xFF)


def push_tag(st, tag, val):
    a = st.r[15] - 4
    if a < STK_LO:
        raise Bad('stack too deep')
    set_ptr(st, 15, a)
    for i in range(4):
        st.stk[a + i] = (tag, val, i)


def pop_tag(st, tag):
    a = st.r[15]
    es = [st.stk.get(a + i) for i in range(4)]
    if any(e is None or len(e) != 3 for e in es):
        raise Bad('pop of non-tagged stack')
    if es[0][0] == 'H' and tag == 'R':
        set_ptr(st, 15, a + 4)
        return END
    if any(e[0] != tag or e[1] is not es[0][1] and e[1] != es[0][1] or e[2] != i for i, e in enumerate(es)):
        raise Bad('stack tag mismatch')
    set_ptr(st, 15, a + 4)
    return es[0][1]


# --------------------------------------------------------------------------
# effective addresses

class EA:
    def __init__(self, mode, reg=0, disp=0, x=0, xl=False, scale=1, bdl=False, imm=0):
        self.mode, self.reg, self.disp, self.x, self.xl, self.scale, self.bdl, self.imm = \
            mode, reg, disp, x, xl, scale, bdl, imm

    def t(self, s):
        m, r = self.mode, rn(self.reg)
        if m == 'dn' or m == 'an':
            return r
        if m == 'ind':
            return '(%s)' % r
        if m == 'pi':
            return '(%s)+' % r
        if m == 'pd':
            return '-(%s)' % r
        if m == 'd16':
            return '%d(%s)' % (self.disp, r)
        xs_ = '%s.%s%s' % (rn(self.x), 'l' if self.xl else 'w', ('*%d' % self.scale) if self.scale > 1 else '')
        if m == 'd8x':
            return '%d(%s,%s)' % (self.disp, r, xs_)
        if m == 'bdx':
            return '(%d.%s,%s,%s)' % (self.disp, 'l' if self.bdl else 'w', r, xs_)
        if m == 'imm':
            return '#$%x' % (self.imm & msk(s))
        raise ValueError(m)

    def is_mem(self):
        return self.mode not in ('dn', 'an', 'imm')

    def writes_reg(self):
        return self.mode in ('pi', 'pd')


def xval(st, ea):
    if ROLE[ea.x] != 'D':
        raise AssertionError('pointer as index')
    v = st.r[ea.x]
    v = sx(v, 4) if ea.xl else sx(v & 0xFFFF, 2)
    return v * ea.scale


def ea_calc(st, ea):
    """address of a control-mode EA without side effects (LEA, bitfields, ...);
    returns (value, role of the base)"""
    b = st.r[ea.reg]
    if ea.mode == 'ind':
        return b, ROLE[ea.reg]
    if ea.mode == 'd16':
        return b + ea.disp, ROLE[ea.reg]
    if ea.mode in ('d8x', 'bdx'):
        return b + ea.disp + xval(st, ea), ROLE[ea.reg]
    raise Bad('not a control mode')


def loc(st, ea, s):
    """resolve an operand: ('d', n) / ('a', n) / ('i', v) / ('m', addr); applies (An)+/-(An)"""
    m = ea.mode
    if m == 'dn':
        return ('d', ea.reg)
    if m == 'an':
        return ('a', ea.reg)
    if m == 'imm':
        return ('i', ea.imm & msk(s))
    r = ea.reg
    if ROLE[r] != 'M':
        raise AssertionError('memory access through a data address register')
    step = 2 if (r == 15 and s == 1) else s
    b = st.r[r]
    if m == 'pi':
        set_ptr(st, r, b + step)
        return ('m', b)
    if m == 'pd':
        set_ptr(st, r, b - step)
        return ('m', b - step)
    a, _ = ea_calc(st, ea)
    return ('m', a)


def lrd(st, lc, s):
    k, v = lc
    if k == 'd':
        return st.r[v] & msk(s)
    if k == 'a':
        if ROLE[v] != 'D' or s == 1:
            raise AssertionError('pointer register read as data')
        return st.r[v] & msk(s)
    if k == 'i':
        return v
    return mrd(st, v, s)


def lwr(st, lc, s, val):
    k, v = lc
    if k == 'd':
        st.r[v] = setlo(st.r[v], val, s)
    elif k == 'm':
        mwr(st, v, s, val)
    else:
        raise AssertionError('bad destination')


# --------------------------------------------------------------------------
# generator context

SPEC = {1: [0, 1, 0x7F, 0x80, 0xFF, 0xFE],
        2: [0, 1, 0x7FFF, 0x8000, 0xFFFF, 0xFFFE, 0x00FF],
        4: [0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0xFFFFFFFE, 0x0000FFFF, 0x00010000]}


class Gen:
    def __init__(self, rng, cls):
        self.rng = rng
        self.cls = cls
        self.cur = []
        self.reserved = set()
        self.subs = []

    def ch(self, seq):
        return self.rng.choice(seq)

    def p(self, x):
        return self.rng.random() < x

    def imm(self, s):
        return self.ch(SPEC[s]) if self.p(0.35) else self.rng.getrandbits(8 * s)

    def size(self, allow_b=True):
        return self.ch([1, 2, 4, 4] if allow_b else [2, 4, 4])

    def emit(self, st, text, ex):
        ins = Ins(text, ex)
        ex(st)
        self.cur.append(ins)
        return ins

    def ddst(self):
        c = [r for r in range(8) if r not in self.reserved]
        return self.ch(c)

    def dreg(self):
        return self.rng.randrange(8)

    def pdst(self):
        c = [r for r in PREGS if r not in self.reserved]
        return self.ch(c)

    # ---- operands
    def target(self, st, s):
        """a window address for an access of size s"""
        t = self.rng.randrange(0, WIN - s + 1)
        if s > 1 and (self.cls == 0 or not self.p(0.3)):
            t &= ~1
        return t

    def index_for(self, st, need_lo, need_hi, scale_ok):
        """pick an index register (and scale) whose contribution lies in
        [need_lo, need_hi]; may emit a MOVEQ to make one"""
        cands = []
        for x in list(range(8)) + DAREGS:
            for xl in (False, True):
                v = st.r[x]
                v = sx(v, 4) if xl else sx(v & 0xFFFF, 2)
                for sc in ((1, 2, 4, 8) if scale_ok else (1,)):
                    if need_lo <= v * sc <= need_hi:
                        cands.append((x, xl, sc))
        if cands and self.p(0.8):
            return self.ch(cands)
        x = self.ddst()
        v = self.rng.randint(-24, 24)
        self.emit(st, '\tmoveq\t#%d,%s' % (v, rn(x)), lambda st, x=x, v=v: ex_moveq(st, x, v))
        xl = self.p(0.5)
        sc = self.ch((1, 2, 4)) if scale_ok else 1
        if not need_lo <= v * sc <= need_hi:
            sc = 1
        if not need_lo <= v * sc <= need_hi:
            raise Bad('no index')
        return (x, xl, sc)

    def mem(self, st, s, modes=None, ctrl=False):
        """a memory operand inside the window (may emit an index setup)"""
        c020 = self.cls > 0
        if modes is None:
            modes = ['ind', 'd16', 'd8x'] + ([] if ctrl else ['pi', 'pd', 'pi', 'pd']) + (['bdx'] if c020 else [])
        m = self.ch(modes)
        regs = PREGS if m not in ('pi', 'pd') else [r for r in PREGS if r not in self.reserved]
        r = self.ch(regs)
        b = st.r[r]
        t = self.target(st, s)
        if m in ('ind', 'pi'):
            if not 0 <= b <= WIN - s:
                raise Bad('pointer out of the window')
            return EA(m, r)
        if m == 'pd':
            if not 0 <= b - s <= WIN - s:
                raise Bad('pointer out of the window')
            return EA(m, r)
        if m == 'd16':
            d = t - b
            if not -32768 <= d <= 32767:
                raise Bad('d16 range')
            return EA(m, r, disp=d)
        if m == 'd8x':
            lo, hi = t - b - 127, t - b + 128
            x, xl, sc = self.index_for(st, lo, hi, c020)
            d = t - b - xval(st, EA('d8x', r, x=x, xl=xl, scale=sc))
            if not -128 <= d <= 127:
                raise Bad('d8 range')
            return EA(m, r, disp=d, x=x, xl=xl, scale=sc)
        # bdx: (bd.w/bd.l, An, Xn.size*scale)
        bdl = self.p(0.4)
        lim = 0x7FFFFFFF if bdl else 32767
        x, xl, sc = self.index_for(st, -0x7FFF, 0x7FFF, True)
        d = t - b - xval(st, EA('bdx', r, x=x, xl=xl, scale=sc))
        if not -lim - 1 <= d <= lim:
            raise Bad('bd range')
        return EA(m, r, disp=d, x=x, xl=xl, scale=sc, bdl=bdl)

    def src(self, st, s, imm=True, an=True, memw=4):
        """a data source operand"""
        opts = [('dn', 4), ('mem', memw)]
        if imm:
            opts.append(('imm', 2))
        if an and s > 1:
            opts.append(('an', 1))
        k = self.rng.choices([o for o, _ in opts], [w for _, w in opts])[0]
        if k == 'dn':
            return EA('dn', self.dreg())
        if k == 'an':
            return EA('an', self.ch(DAREGS))
        if k == 'imm':
            return EA('imm', imm=self.imm(s))
        return self.mem(st, s)

    def dst(self, st, s, mem_w=0.5):
        if self.p(mem_w):
            return self.mem(st, s)
        return EA('dn', self.ddst())


def ex_moveq(st, x, v):
    set_reg(st, x, u32(v))
    setcc(st, nz(u32(v), 4), N | Z | V | C)


# --------------------------------------------------------------------------
# straight-line instruction generators: g(G, st) emits one instruction
# (plus setup instructions) and executes it on st

def g_move(G, st):
    s = G.size()
    a = G.src(st, s)
    b = G.dst(st, s)

    def ex(st, a=a, b=b, s=s):
        v = lrd(st, loc(st, a, s), s)
        lwr(st, loc(st, b, s), s, v)
        setcc(st, nz(v, s), N | Z | V | C)
    G.emit(st, '\tmove.%s\t%s,%s' % (SC[s], a.t(s), b.t(s)), ex)


def g_moveq(G, st):
    x = G.ddst()
    v = G.ch([0, 1, -1, 127, -128, 0x55]) if G.p(0.4) else G.rng.randint(-128, 127)
    G.emit(st, '\tmoveq\t#%d,%s' % (v, rn(x)), lambda st, x=x, v=v: ex_moveq(st, x, v))


def g_move_imm(G, st):
    s = G.ch([2, 4])
    x = G.ddst()
    v = G.imm(s)
    a = EA('imm', imm=v)

    def ex(st, x=x, s=s, v=v):
        st.r[x] = setlo(st.r[x], v, s)
        setcc(st, nz(v, s), N | Z | V | C)
    G.emit(st, '\tmove.%s\t%s,%s' % (SC[s], a.t(s), rn(x)), ex)


def g_movea(G, st):
    if G.p(0.35):
        p = G.ch(PREGS)
        q = G.pdst()
        if p == q:
            raise Bad('same')

        def ex(st, p=p, q=q):
            set_ptr(st, q, st.r[p])
        G.emit(st, '\tmovea.l\t%s,%s' % (rn(p), rn(q)), ex)
        return
    s = G.ch([2, 4])
    a = G.src(st, s)
    q = G.ch(DAREGS)

    def ex(st, a=a, q=q, s=s):
        v = lrd(st, loc(st, a, s), s)
        set_reg(st, q, sx(v, s))
    G.emit(st, '\tmovea.%s\t%s,%s' % (SC[s], a.t(s), rn(q)), ex)


def g_lea(G, st):
    c020 = G.cls > 0
    if G.p(0.25):
        # data arithmetic through LEA on a4/a5
        b = G.ch(DAREGS)
        q = G.ch(DAREGS)
        m = G.ch(['d16', 'd8x'] + (['bdx'] if c020 else []))
        if m == 'd16':
            ea = EA('d16', b, disp=G.rng.randint(-32768, 32767))
        else:
            x = G.ch(list(range(8)) + DAREGS)
            ea = EA(m, b, disp=G.rng.randint(-128, 127) if m == 'd8x' else G.rng.randint(-100000, 100000),
                    x=x, xl=G.p(0.5), scale=G.ch((1, 2, 4, 8)) if c020 else 1, bdl=True)

        def ex(st, ea=ea, q=q):
            v, _ = ea_calc(st, ea)
            set_reg(st, q, v)
        G.emit(st, '\tlea\t%s,%s' % (ea.t(4), rn(q)), ex)
        return
    q = G.pdst()
    ea = G.mem(st, 1, modes=['d16', 'd16', 'd8x', 'ind'] + (['bdx'] if c020 else []), ctrl=True)

    def ex(st, ea=ea, q=q):
        v, _ = ea_calc(st, ea)
        set_ptr(st, q, v)
    G.emit(st, '\tlea\t%s,%s' % (ea.t(4), rn(q)), ex)


def g_repoint(G, st):
    """bring a pointer that drifted out of the window back"""
    bad = [r for r in PREGS if not 0 <= st.r[r] < WIN and r not in G.reserved]
    if not bad:
        raise Bad('nothing to repoint')
    q = G.ch(bad)
    good = [r for r in PREGS if 0 <= st.r[r] < WIN]
    t = G.target(st, 4)
    if good and G.p(0.7):
        p = G.ch(good)
        d = t - st.r[p]
        G.emit(st, '\tlea\t%d(%s),%s' % (d, rn(p), rn(q)), lambda st, p=p, q=q, d=d: set_ptr(st, q, st.r[p] + d))
    else:
        d = t - st.r[q]
        if not -32768 <= d <= 32767:
            raise Bad('far')
        G.emit(st, '\tadda.w\t#%d,%s' % (d, rn(q)), lambda st, q=q, d=d: set_ptr(st, q, st.r[q] + d))


def g_exg(G, st):
    k = G.rng.randrange(3)
    if k == 0:
        a, b = G.ddst(), G.ddst()
    elif k == 1:
        a, b = G.ddst(), G.ch(DAREGS)
    else:
        c = [r for r in PREGS if r not in G.reserved]
        a, b = G.ch(c), G.ch(c)

    def ex(st, a=a, b=b):
        st.r[a], st.r[b] = st.r[b], st.r[a]
    G.emit(st, '\texg\t%s,%s' % (rn(a), rn(b)), ex)


def g_swap_ext(G, st):
    x = G.ddst()
    k = G.ch(['swap', 'ext.w', 'ext.l'] + (['extb.l'] if G.cls else []))

    def ex(st, x=x, k=k):
        v = st.r[x]
        if k == 'swap':
            r, s = u32((v << 16) | (v >> 16)), 4
            st.r[x] = r
        elif k == 'ext.w':
            r, s = sx(v, 1) & 0xFFFF, 2
            st.r[x] = setlo(v, r, 2)
        elif k == 'ext.l':
            r, s = u32(sx(v, 2)), 4
            st.r[x] = r
        else:
            r, s = u32(sx(v, 1)), 4
            st.r[x] = r
        setcc(st, nz(r, s), N | Z | V | C)
    G.emit(st, '\t%s\t%s' % (k, rn(x)), ex)


def g_alu(G, st):
    op = G.ch(['add', 'sub', 'and', 'or', 'cmp', 'add', 'sub', 'eor'])
    s = G.size()
    x = G.ddst()
    if op == 'eor' or (op != 'cmp' and G.p(0.4)):
        # Dn,<ea>
        if op == 'eor' and G.p(0.4):
            b = EA('dn', G.ddst())
        else:
            b = G.mem(st, s)
        a = EA('dn', G.dreg())
    else:
        a = G.src(st, s, an=(op in ('add', 'sub', 'cmp')))
        b = EA('dn', x)

    def ex(st, a=a, b=b, s=s, op=op):
        va = lrd(st, loc(st, a, s), s)
        lb = loc(st, b, s)
        vb = lrd(st, lb, s)
        if op == 'add':
            r, f = f_add(s, vb, va)
            m = 0x1F
        elif op in ('sub', 'cmp'):
            r, f = f_sub(s, vb, va)
            m = 0x1F if op == 'sub' else N | Z | V | C
        else:
            r = {'and': vb & va, 'or': vb | va, 'eor': vb ^ va}[op]
            f, m = nz(r, s), N | Z | V | C
        if op != 'cmp':
            lwr(st, lb, s, r)
        setcc(st, f, m)
    G.emit(st, '\t%s.%s\t%s,%s' % (op, SC[s], a.t(s), b.t(s)), ex)


def g_alu_imm(G, st):
    op = G.ch(['addi', 'subi', 'andi', 'ori', 'eori', 'cmpi'])
    s = G.size()
    v = G.imm(s)
    b = G.dst(st, s)

    def ex(st, b=b, s=s, op=op, v=v):
        lb = loc(st, b, s)
        vb = lrd(st, lb, s)
        if op == 'addi':
            r, f = f_add(s, vb, v)
            m = 0x1F
        elif op in ('subi', 'cmpi'):
            r, f = f_sub(s, vb, v)
            m = 0x1F if op == 'subi' else N | Z | V | C
        else:
            r = {'andi': vb & v, 'ori': vb | v, 'eori': vb ^ v}[op]
            f, m = nz(r, s), N | Z | V | C
        if op != 'cmpi':
            lwr(st, lb, s, r)
        setcc(st, f, m)
    G.emit(st, '\t%s.%s\t#$%x,%s' % (op, SC[s], v, b.t(s)), ex)


def g_addq(G, st):
    op = G.ch(['addq', 'subq'])
    q = G.rng.randint(1, 8)
    k = G.rng.randrange(4)
    if k == 0:
        s = G.ch([2, 4])
        r = G.ch(DAREGS + [r for r in PREGS if r not in G.reserved])

        def ex(st, r=r, q=q, op=op):
            d = q if op == 'addq' else -q
            if ROLE[r] == 'M':
                set_ptr(st, r, st.r[r] + d)
            else:
                set_reg(st, r, st.r[r] + d)
        G.emit(st, '\t%s.%s\t#%d,%s' % (op, SC[s], q, rn(r)), ex)
        return
    s = G.size()
    b = G.dst(st, s)

    def ex(st, b=b, s=s, q=q, op=op):
        lb = loc(st, b, s)
        vb = lrd(st, lb, s)
        r, f = (f_add if op == 'addq' else f_sub)(s, vb, q)
        lwr(st, lb, s, r)
        setcc(st, f, 0x1F)
    G.emit(st, '\t%s.%s\t#%d,%s' % (op, SC[s], q, b.t(s)), ex)


def g_addx(G, st):
    op = G.ch(['addx', 'subx'])
    s = G.size()
    if G.p(0.6):
        a, b = EA('dn', G.dreg()), EA('dn', G.ddst())
    else:
        c = [r for r in PREGS if r not in G.reserved]
        a, b = EA('pd', G.ch(c)), EA('pd', G.ch(c))

    def ex(st, a=a, b=b, s=s, op=op):
        need(st, X)
        va = lrd(st, loc(st, a, s), s)
        lb = loc(st, b, s)
        vb = lrd(st, lb, s)
        xin = 1 if st.ccr & X else 0
        zin = 1 if st.ccr & Z else 0
        r, f = (f_add if op == 'addx' else f_sub)(s, vb, va, xin, zin, ext=True)
        lwr(st, lb, s, r)
        setcc(st, f, N | V | C | X | (Z if r & msk(s) else 0))
    G.emit(st, '\t%s.%s\t%s,%s' % (op, SC[s], a.t(s), b.t(s)), ex)


def g_cmpm(G, st):
    s = G.size()
    c = [r for r in PREGS if r not in G.reserved]
    ra, rb = G.ch(c), G.ch(c)

    def ex(st, ra=ra, rb=rb, s=s):
        va = lrd(st, loc(st, EA('pi', ra), s), s)
        vb = lrd(st, loc(st, EA('pi', rb), s), s)
        r, f = f_sub(s, vb, va)
        setcc(st, f, N | Z | V | C)
    G.emit(st, '\tcmpm.%s\t(%s)+,(%s)+' % (SC[s], rn(ra), rn(rb)), ex)


def g_adda(G, st):
    op = G.ch(['adda', 'suba', 'cmpa'])
    s = G.ch([2, 4])
    if op == 'cmpa' and G.p(0.3):
        p, q = G.ch(PREGS), G.ch(PREGS)

        def ex(st, p=p, q=q):
            if not (0 <= st.r[p] < 0x1000 and 0 <= st.r[q] < 0x1000):
                raise Bad('pointer compare range')
            r, f = f_sub(4, st.r[q], st.r[p])
            setcc(st, f, N | Z | V | C)
        G.emit(st, '\tcmpa.l\t%s,%s' % (rn(p), rn(q)), ex)
        return
    a = G.src(st, s)
    if op == 'cmpa':
        q = G.ch(DAREGS)
    else:
        q = G.ch(DAREGS + [r for r in PREGS if r not in G.reserved])
        if ROLE[q] == 'M' and a.mode not in ('imm',) and G.p(0.6):
            a = EA('imm', imm=G.rng.randint(-40, 40) & 0xFFFF if s == 2 else u32(G.rng.randint(-40, 40)))

    def ex(st, a=a, q=q, s=s, op=op):
        v = sx(lrd(st, loc(st, a, s), s), s)
        if op == 'cmpa':
            r, f = f_sub(4, st.r[q], u32(v))
            setcc(st, f, N | Z | V | C)
            return
        d = v if op == 'adda' else -v
        if ROLE[q] == 'M':
            set_ptr(st, q, st.r[q] + d)
        else:
            set_reg(st, q, st.r[q] + d)
    G.emit(st, '\t%s.%s\t%s,%s' % (op, SC[s], a.t(s), rn(q)), ex)


def g_unary(G, st):
    op = G.ch(['neg', 'negx', 'not', 'clr', 'tst', 'tst'])
    s = G.size()
    if op == 'tst' and G.cls and G.p(0.25):
        b = G.ch([EA('an', G.ch(DAREGS)), EA('imm', imm=G.imm(4))])
        if b.mode == 'an' and s == 1:
            s = 2
    else:
        b = G.dst(st, s)

    def ex(st, b=b, s=s, op=op):
        lb = loc(st, b, s)
        v = lrd(st, lb, s)
        if op == 'neg':
            r, f = f_sub(s, 0, v)
            m = 0x1F
        elif op == 'negx':
            need(st, X)
            r, f = f_sub(s, 0, v, 1 if st.ccr & X else 0, 1 if st.ccr & Z else 0, ext=True)
            m = N | V | C | X | (Z if r & msk(s) else 0)
        elif op == 'not':
            r, f, m = ~v & msk(s), nz(~v, s), N | Z | V | C
        elif op == 'clr':
            r, f, m = 0, Z, N | Z | V | C
        else:
            r, f, m = None, nz(v, s), N | Z | V | C
        if r is not None:
            lwr(st, lb, s, r)
        setcc(st, f, m)
    G.emit(st, '\t%s.%s\t%s' % (op, SC[s], b.t(s)), ex)


def g_shift(G, st):
    op = G.ch(['asl', 'asr', 'lsl', 'lsr', 'rol', 'ror', 'roxl', 'roxr'])
    k = G.rng.randrange(5)
    if k == 4:
        s = 2
        b = G.mem(st, 2)
        cnt = None
        txt = '\t%s.w\t%s' % (op, b.t(2))
    else:
        s = G.size()
        b = EA('dn', G.ddst())
        if k < 2:
            cnt = G.rng.randint(1, 8)
            txt = '\t%s.%s\t#%d,%s' % (op, SC[s], cnt, b.t(s))
        else:
            cnt = G.dreg()
            if G.p(0.5) and cnt not in G.reserved and cnt != b.reg:
                v = G.ch([0, 1, 7, 8, 15, 16, 17, 31, 32, 33, 63, 64])
                G.emit(st, '\tmoveq\t#%d,%s' % (v, rn(cnt)), lambda st, x=cnt, v=v: ex_moveq(st, x, v))
            txt = '\t%s.%s\t%s,%s' % (op, SC[s], rn(cnt), b.t(s))
            cnt = ('r', cnt)

    def ex(st, b=b, s=s, op=op, cnt=cnt):
        if op in ('roxl', 'roxr'):
            need(st, X)
        if cnt is None:
            n = 1
        elif isinstance(cnt, tuple):
            n = st.r[cnt[1]] & 63
        else:
            n = cnt
        lb = loc(st, b, s)
        v = lrd(st, lb, s)
        r, f = f_shift(op, s, v, n, st.ccr & X)
        lwr(st, lb, s, r)
        setcc(st, f, (N | Z | V | C) if (n == 0 or op in ('rol', 'ror')) else 0x1F)
    G.emit(st, txt, ex)


def g_bit(G, st):
    op = G.ch(['btst', 'bchg', 'bclr', 'bset'])
    reg = G.p(0.5)
    if reg:
        b = EA('dn', G.ddst() if op != 'btst' else G.dreg())
        s = 4
    else:
        b = G.mem(st, 1)
        s = 1
    if G.p(0.5):
        n = G.rng.randrange(32 if reg else 8)
        if G.p(0.2):
            n += G.ch([32, 8, 64])     # immediate bit numbers are taken modulo the size
        nt = '#%d' % n
        nb = ('i', n)
    else:
        nb = ('r', G.dreg())
        nt = rn(nb[1])

    def ex(st, b=b, s=s, op=op, nb=nb):
        n = nb[1] if nb[0] == 'i' else st.r[nb[1]]
        n &= (31 if s == 4 else 7)
        lb = loc(st, b, s)
        v = lrd(st, lb, s)
        bit = (v >> n) & 1
        if op == 'bchg':
            lwr(st, lb, s, v ^ (1 << n))
        elif op == 'bclr':
            lwr(st, lb, s, v & ~(1 << n))
        elif op == 'bset':
            lwr(st, lb, s, v | (1 << n))
        setcc(st, 0 if bit else Z, Z)
    if nb[0] == 'i' and (nb[1] > 255):
        raise Bad('imm bit')
    G.emit(st, '\t%s\t%s,%s' % (op, nt, b.t(s)), ex)


def g_scc(G, st):
    cc = G.rng.randrange(16)
    b = G.dst(st, 1)

    def ex(st, b=b, cc=cc):
        need(st, CC_NEED[cc])
        lwr(st, loc(st, b, 1), 1, 0xFF if cond(cc, st.ccr) else 0)
    G.emit(st, '\ts%s\t%s' % (CCN[cc], b.t(1)), ex)


def g_mulw(G, st):
    op = G.ch(['mulu', 'muls'])
    a = G.src(st, 2, an=False)
    x = G.ddst()

    def ex(st, a=a, x=x, op=op):
        v = lrd(st, loc(st, a, 2), 2)
        d = st.r[x] & 0xFFFF
        r = u32(v * d) if op == 'mulu' else u32(sx(v, 2) * sx(d, 2))
        st.r[x] = r
        setcc(st, nz(r, 4), N | Z | V | C)
    G.emit(st, '\t%s.w\t%s,%s' % (op, a.t(2), rn(x)), ex)


def g_divw(G, st):
    op = G.ch(['divu', 'divs'])
    a = G.src(st, 2, an=False)
    x = G.ddst()
    if a.mode == 'imm' and a.imm == 0:
        a.imm = G.ch([1, 3, 7, 0xFFFF, 0x8000, 100])

    def ex(st, a=a, x=x, op=op):
        b = lrd(st, loc(st, a, 2), 2)
        if b == 0:
            raise Bad('divide by zero')
        d = st.r[x]
        if op == 'divu':
            q, r = d // b, d % b
            ovf = q > 0xFFFF
        else:
            sa, sb = sx(d, 4), sx(b, 2)
            q = abs(sa) // abs(sb)
            if (sa < 0) != (sb < 0):
                q = -q
            r = sa - q * sb
            ovf = not -0x8000 <= q <= 0x7FFF
        if ovf:
            setcc(st, V, V | C)
            st.undef |= N | Z
            return
        q &= 0xFFFF
        st.r[x] = ((r & 0xFFFF) << 16) | q
        setcc(st, nz(q, 2), N | Z | V | C)
    G.emit(st, '\t%s.w\t%s,%s' % (op, a.t(2), rn(x)), ex)


def bcd_ok(v):
    return (v & 0xF) <= 9 and (v >> 4) <= 9


def g_bcd(G, st):
    op = G.ch(['abcd', 'sbcd', 'nbcd'])
    if op == 'nbcd':
        b = G.dst(st, 1)
        a = None
    elif G.p(0.6):
        a, b = EA('dn', G.dreg()), EA('dn', G.ddst())
    else:
        c = [r for r in PREGS if r not in G.reserved]
        a, b = EA('pd', G.ch(c)), EA('pd', G.ch(c))

    def ex(st, a=a, b=b, op=op):
        need(st, X)
        x = 1 if st.ccr & X else 0
        if op == 'nbcd':
            lb = loc(st, b, 1)
            vs, vd = lrd(st, lb, 1), 0
        else:
            vs = lrd(st, loc(st, a, 1), 1)
            lb = loc(st, b, 1)
            vd = lrd(st, lb, 1)
        if not (bcd_ok(vs) and bcd_ok(vd)):
            raise Bad('not BCD')
        if op == 'abcd':
            r = cb.bcd2i(vd) + cb.bcd2i(vs) + x
            c = r >= 100
        else:
            r = cb.bcd2i(vd) - cb.bcd2i(vs) - x
            c = r < 0
        rb = cb.i2bcd(r % 100)
        lwr(st, lb, 1, rb)
        setcc(st, (C | X) if c else 0, C | X | (Z if rb else 0))
        st.undef |= N | V
    if op == 'nbcd':
        G.emit(st, '\tnbcd\t%s' % b.t(1), ex)
    else:
        G.emit(st, '\t%s\t%s,%s' % (op, a.t(1), b.t(1)), ex)


def g_movem(G, st):
    s = G.ch([2, 4])
    pool = list(range(8)) + DAREGS
    n = G.rng.randint(1, 5)
    regs = sorted(G.rng.sample(pool, n))
    load = G.p(0.5)
    if load:
        regs = [r for r in regs if r not in G.reserved]
        if not regs:
            raise Bad('empty')
        m = G.ch(['ind', 'pi', 'pi', 'd16', 'd8x'])
    else:
        m = G.ch(['ind', 'pd', 'pd', 'd16', 'd8x'])
    span = len(regs) * s
    rlist = '/'.join(rn(r) for r in regs)
    if m == 'pd':
        c = [r for r in PREGS if r not in G.reserved]
        ea = EA('pd', G.ch(c))
    elif m == 'pi':
        c = [r for r in PREGS if r not in G.reserved]
        ea = EA('pi', G.ch(c))
    else:
        ea = G.mem(st, s, modes=[m], ctrl=True)

    def ex(st, ea=ea, s=s, regs=regs, load=load, span=span):
        if ea.mode == 'pd':
            b = st.r[ea.reg] - span
        elif ea.mode == 'pi':
            b = st.r[ea.reg]
        else:
            b, _ = ea_calc(st, ea)
        chk_addr(st, b, 1)
        chk_addr(st, b + span - 1, 1)
        if load and st.cls == 0 and not 0 <= b + span + 2 <= WIN + 0x40:
            raise Bad('68000 extra read')
        for i, r in enumerate(regs):
            a = b + i * s
            if load:
                set_reg(st, r, sx(mrd(st, a, s), s))
            else:
                mwr(st, a, s, st.r[r])
        if ea.mode == 'pd':
            set_ptr(st, ea.reg, b)
        elif ea.mode == 'pi':
            set_ptr(st, ea.reg, b + span)
    if load:
        G.emit(st, '\tmovem.%s\t%s,%s' % (SC[s], ea.t(s), rlist), ex)
    else:
        G.emit(st, '\tmovem.%s\t%s,%s' % (SC[s], rlist, ea.t(s)), ex)


def g_ccr(G, st):
    k = G.rng.randrange(5 if G.cls else 4)
    if k == 0:
        a = G.src(st, 2, an=False)

        def ex(st, a=a):
            v = lrd(st, loc(st, a, 2), 2)
            setcc(st, v, 0x1F)
        G.emit(st, '\tmove.w\t%s,ccr' % a.t(2), ex)
    elif k < 4:
        op = ['andi', 'ori', 'eori'][k - 1]
        v = G.rng.getrandbits(8) if G.p(0.3) else G.rng.getrandbits(5)

        def ex(st, op=op, v=v):
            c = st.ccr
            if op == 'andi':
                st.ccr, st.undef = c & v & 0x1F, st.undef & v
            elif op == 'ori':
                st.ccr, st.undef = (c | v) & 0x1F, st.undef & ~v & 0x1F
            else:
                st.ccr = (c ^ v) & 0x1F
        G.emit(st, '\t%s.b\t#$%02x,ccr' % (op, v), ex)
    else:
        b = G.dst(st, 2)

        def ex(st, b=b):
            need(st, 0x1F)
            lwr(st, loc(st, b, 2), 2, st.ccr)
        G.emit(st, '\tmove.w\tccr,%s' % b.t(2), ex)


# ---- 68020+ generators

def g_mull(G, st):
    op = G.ch(['mulu', 'muls'])
    a = G.src(st, 4, an=False)
    lo = G.ddst()
    if G.cls == 1 and G.p(0.4):
        hi = G.ddst()
        if hi == lo:
            raise Bad('same')

        def ex(st, a=a, lo=lo, hi=hi, op=op):
            v = lrd(st, loc(st, a, 4), 4)
            d = st.r[lo]
            p = v * d if op == 'mulu' else sx(v, 4) * sx(d, 4)
            p &= (1 << 64) - 1
            st.r[lo], st.r[hi] = p & M32, p >> 32
            setcc(st, (N if p >> 63 else 0) | (Z if p == 0 else 0), N | Z | V | C)
        G.emit(st, '\t%s.l\t%s,%s:%s' % (op, a.t(4), rn(hi), rn(lo)), ex)
        return

    def ex(st, a=a, lo=lo, op=op):
        v = lrd(st, loc(st, a, 4), 4)
        d = st.r[lo]
        p = v * d if op == 'mulu' else sx(v, 4) * sx(d, 4)
        r = u32(p)
        ovf = (p > M32) if op == 'mulu' else not -0x80000000 <= p <= 0x7FFFFFFF
        st.r[lo] = r
        setcc(st, nz(r, 4) | (V if ovf else 0), N | Z | V | C)
    G.emit(st, '\t%s.l\t%s,%s' % (op, a.t(4), rn(lo)), ex)


def div32(op, a, b):
    if op == 'u':
        return a // b, a % b, False
    sa, sb = sx(a, 4), sx(b, 4)
    q = abs(sa) // abs(sb)
    if (sa < 0) != (sb < 0):
        q = -q
    return q, sa - q * sb, not -0x80000000 <= q <= 0x7FFFFFFF


def g_divl(G, st):
    sg = G.ch(['u', 's'])
    a = G.src(st, 4, an=False)
    if a.mode == 'imm' and a.imm == 0:
        a.imm = 7
    q = G.ddst()
    form = G.ch(['q', 'rq'] + (['64'] if G.cls == 1 else []))
    r = None
    if form != 'q':
        r = G.ddst()
        if r == q:
            raise Bad('same')

    def ex(st, a=a, q=q, r=r, form=form, sg=sg):
        b = lrd(st, loc(st, a, 4), 4)
        if b == 0:
            raise Bad('divide by zero')
        if form == '64':
            d = (st.r[r] << 32) | st.r[q]
            if sg == 'u':
                qq, rr = d // b, d % b
                ovf = qq > M32
            else:
                sd = d - (1 << 64) if d >> 63 else d
                sb = sx(b, 4)
                qq = abs(sd) // abs(sb)
                if (sd < 0) != (sb < 0):
                    qq = -qq
                rr = sd - qq * sb
                ovf = not -0x80000000 <= qq <= 0x7FFFFFFF
        else:
            qq, rr, ovf = div32(sg, st.r[q], b)
        if ovf:
            setcc(st, V, V | C)
            st.undef |= N | Z
            return
        st.r[q] = u32(qq)
        if r is not None:
            st.r[r] = u32(rr)
        setcc(st, nz(u32(qq), 4), N | Z | V | C)
    mn = 'div%s%s.l' % (sg, 'l' if form == 'rq' else '')
    if form == 'q':
        G.emit(st, '\t%s\t%s,%s' % (mn, a.t(4), rn(q)), ex)
    else:
        G.emit(st, '\t%s\t%s,%s:%s' % (mn, a.t(4), rn(r), rn(q)), ex)


BFOPS = ['bftst', 'bfextu', 'bfexts', 'bfffo', 'bfchg', 'bfclr', 'bfset', 'bfins']


def g_bitfield(G, st):
    op = G.ch(BFOPS)
    reg = G.p(0.45)
    if reg:
        b = EA('dn', G.ddst() if op in ('bfchg', 'bfclr', 'bfset', 'bfins') else G.dreg())
    else:
        b = G.mem(st, 1, modes=['ind', 'd16', 'd8x', 'bdx'], ctrl=True)
    if G.p(0.5):
        o = ('i', G.rng.randrange(32))
    else:
        x = G.dreg()
        o = ('r', x)
        if G.p(0.6) and x not in G.reserved:
            v = G.rng.randint(0, 31) if (reg and op == 'bfffo') else G.rng.randint(-40, 60)
            G.emit(st, '\tmoveq\t#%d,%s' % (v, rn(x)), lambda st, x=x, v=v: ex_moveq(st, x, v))
    if G.p(0.5):
        w = ('i', G.rng.randrange(32))
    else:
        w = ('r', G.dreg())
    dn = G.ddst() if op in ('bfextu', 'bfexts', 'bfffo') else (G.dreg() if op == 'bfins' else None)

    def ex(st, b=b, o=o, w=w, dn=dn, op=op, reg=reg):
        ov = o[1] if o[0] == 'i' else st.r[o[1]]
        wv = (w[1] if w[0] == 'i' else st.r[w[1]]) & 31
        wv = wv or 32
        if reg:
            if op == 'bfffo' and o[0] == 'r' and ov > 31:
                raise Bad('bfffo register offset > 31')
            off = ov & 31
            cell = [st.r[b.reg]]

            def get(i):
                return (cell[0] >> (31 - ((off + i) & 31))) & 1

            def put(i, bit):
                p = 31 - ((off + i) & 31)
                cell[0] = (cell[0] & ~(1 << p)) | (bit << p)
        else:
            so = sx(ov, 4) if o[0] == 'r' else ov
            base, _ = ea_calc(st, b)
            ba = base * 8 + so
            lo, hi = ba >> 3, (ba + wv - 1) >> 3
            chk_addr(st, lo, 1)
            chk_addr(st, hi, 1)
            off = so

            def get(i):
                a = ba + i
                return (st.win[a >> 3] >> (7 - (a & 7))) & 1

            def put(i, bit):
                a = ba + i
                bb = st.win[a >> 3]
                p = 7 - (a & 7)
                st.win[a >> 3] = (bb & ~(1 << p)) | (bit << p)
        res, f = bf_model(op, get, put, off if not reg else (ov if op == 'bfffo' else off), wv,
                          st.r[dn] if dn is not None else 0)
        if reg and op in ('bfchg', 'bfclr', 'bfset', 'bfins'):
            st.r[b.reg] = u32(cell[0])
        if op in ('bfextu', 'bfexts', 'bfffo'):
            st.r[dn] = u32(res)
        setcc(st, f, N | Z | V | C)
    fld = '{%s:%s}' % (str(o[1]) if o[0] == 'i' else rn(o[1]), str(w[1]) if w[0] == 'i' else rn(w[1]))
    if op == 'bfins':
        txt = '\tbfins\t%s,%s%s' % (rn(dn), b.t(1), fld)
    elif dn is not None:
        txt = '\t%s\t%s%s,%s' % (op, b.t(1), fld, rn(dn))
    else:
        txt = '\t%s\t%s%s' % (op, b.t(1), fld)
    G.emit(st, txt, ex)


def g_pack(G, st):
    op = G.ch(['pack', 'unpk'])
    adj = G.ch([0, 0x3030, 0xCFD0, 0x0101, G.rng.getrandbits(16)])
    if G.p(0.6):
        a, b = EA('dn', G.dreg()), EA('dn', G.ddst())
    else:
        c = [r for r in PREGS if r not in G.reserved]
        a, b = EA('pd', G.ch(c)), EA('pd', G.ch(c))

    def ex(st, a=a, b=b, op=op, adj=adj):
        if op == 'pack':
            if a.mode == 'dn':
                w = st.r[a.reg] & 0xFFFF
            else:
                lo_ = lrd(st, loc(st, a, 1), 1)
                hi_ = lrd(st, loc(st, a, 1), 1)
                w = (hi_ << 8) | lo_
            t = (w + adj) & 0xFFFF
            r = ((t >> 4) & 0xF0) | (t & 0x0F)
            lwr(st, loc(st, b, 1), 1, r)
        else:
            v = lrd(st, loc(st, a, 1), 1)
            r = ((((v & 0xF0) << 4) | (v & 0x0F)) + adj) & 0xFFFF
            if b.mode == 'dn':
                st.r[b.reg] = setlo(st.r[b.reg], r, 2)
            else:
                lwr(st, loc(st, b, 1), 1, r & 0xFF)
                lwr(st, loc(st, b, 1), 1, r >> 8)
    G.emit(st, '\t%s\t%s,%s,#$%04x' % (op, a.t(1), b.t(1), adj), ex)



# --------------------------------------------------------------------------
# items

COMMON = [(g_move, 12), (g_moveq, 4), (g_move_imm, 2), (g_movea, 3), (g_lea, 3), (g_exg, 1), (g_swap_ext, 2),
          (g_alu, 12), (g_alu_imm, 6), (g_addq, 4), (g_addx, 3), (g_cmpm, 1), (g_adda, 3), (g_unary, 4),
          (g_shift, 6), (g_bit, 4), (g_scc, 2), (g_mulw, 2), (g_divw, 2), (g_bcd, 2), (g_movem, 3), (g_ccr, 2)]
C020 = [(g_mull, 3), (g_divl, 3), (g_bitfield, 6), (g_pack, 1)]
ITEM_END = object()


def attempt(G, st, g):
    """run generator g on st; on Bad restore st and the current item list"""
    sv_cur, sv = list(G.cur), st.copy()
    try:
        g(G, st)
        return True
    except Bad:
        G.cur = sv_cur
        st.r, st.ccr, st.undef, st.win, st.stk = sv.r, sv.ccr, sv.undef, sv.win, sv.stk
        return False


def straight(G, st):
    tab = COMMON + (C020 if G.cls else [])
    if any(not 0 <= st.r[r] < WIN for r in PREGS):
        tab = tab + [(g_repoint, 6)]
    for _ in range(40):
        g = G.rng.choices([t for t, _ in tab], [w for _, w in tab])[0]
        if attempt(G, st, g):
            return
    raise Bad('no straight instruction')


def link(ins_list, end):
    for a, b in zip(ins_list, ins_list[1:]):
        a.next = b
    ins_list[-1].next = end


def run(st, start, stop, limit=3000, trace=None):
    ins = resolve(start)
    n = 0
    while ins is not stop:
        if ins is END or ins is ITEM_END or ins is None:
            raise Bad('left the code')
        if trace is not None:
            trace(ins, st)
        t = ins.ex(st)
        ins = resolve(ins.next if t is None else t)
        n += 1
        if n > limit:
            raise Bad('too long')
    return n


def body(G, st, lo, hi):
    G.cur = []
    for _ in range(G.rng.randint(lo, hi)):
        straight(G, st)
    return G.cur


def item_skip(G, st):
    lbl = Label()
    ok = [c for c in range(2, 16) if not st.undef & CC_NEED[c]]
    cc = 0 if (G.p(0.15) or not ok) else G.ch(ok)
    sz = G.ch(['s', 's', 'w'] + (['l'] if G.cls else []))
    inner = body(G, st.copy(), 1, 3)

    def ex(st, cc=cc, lbl=lbl):
        need(st, CC_NEED[cc])
        return lbl if (cc == 0 or cond(cc, st.ccr)) else None
    br = Ins('\tb%s.%s\t%s' % ('ra' if cc == 0 else CCN[cc], sz, lbl.name), ex)
    br.ref = lbl
    return [br] + inner, [lbl]


def item_loop(G, st):
    c = G.ddst()
    n = G.ch([0, 1, 2, 3, 5, 7, 9, 12])
    init = Ins('\tmoveq\t#%d,%s' % (n, rn(c)), lambda st, x=c, v=n: ex_moveq(st, x, v))
    scratch = st.copy()
    init.ex(scratch)
    G.reserved.add(c)
    inner = body(G, scratch, 1, 3)
    G.reserved.discard(c)
    cc = 1 if G.p(0.7) else G.ch(range(2, 16))
    top = Label()
    top.ins = inner[0]

    def ex(st, cc=cc, c=c, top=top):
        if cc != 1:
            need(st, CC_NEED[cc])
            if cond(cc, st.ccr):
                return None
        v = (st.r[c] - 1) & 0xFFFF
        st.r[c] = (st.r[c] & 0xFFFF0000) | v
        return top if v != 0xFFFF else None
    db = Ins('\tdb%s\t%s,%s' % (CCN[cc], rn(c), top.name), ex)
    db.ref = top
    return [init] + inner + [db], []


class Sub:
    def __init__(self, lbl, arg, code):
        self.lbl, self.arg, self.code = lbl, arg, code


def ex_rts(st, extra=0):
    r = pop_tag(st, 'R')
    if r is END:
        if extra:
            raise Bad('rtd to the harness')
        return END
    if extra:
        set_ptr(st, 15, st.r[15] + extra)
    return r


def ex_push_d(st, x):
    """MOVE.L Dx,-(SP): PRM MOVE sets N,Z from the data, clears V,C"""
    a = st.r[15] - 4
    if a < STK_LO:
        raise Bad('deep')
    set_ptr(st, 15, a)
    mwr(st, a, 4, st.r[x])
    setcc(st, nz(st.r[x], 4), N | Z | V | C)


def g_arg(G, st):
    x = G.ddst()

    def ex(st, x=x):
        v = mrd(st, st.r[15] + 4, 4)
        set_reg(st, x, v)
        setcc(st, nz(v, 4), N | Z | V | C)
    G.emit(st, '\tmove.l\t4(sp),%s' % rn(x), ex)


def new_sub(G, st, arg):
    """st: state at the subroutine entry (return address pushed)"""
    lbl = Label()
    G.cur = []
    if arg and not attempt(G, st, g_arg):
        raise Bad('arg')
    for _ in range(G.rng.randint(1, 4)):
        straight(G, st)
    code = G.cur
    if arg and G.cls:
        code.append(Ins('\trtd\t#4', lambda st: ex_rts(st, 4)))
    else:
        code.append(Ins('\trts', lambda st: ex_rts(st)))
    lbl.ins = code[0]
    link(code, None)
    return Sub(lbl, arg, code)


def item_call(G, st):
    pre = []
    fresh = not G.subs or G.p(0.6)
    if fresh:
        if len(G.subs) >= 3:
            raise Bad('enough subroutines')
        arg = G.p(0.3)
    else:
        sub = G.ch(G.subs)
        arg = sub.arg
    if arg:
        x = G.dreg()
        pre.append(Ins('\tmove.l\t%s,-(sp)' % rn(x), lambda st, x=x: ex_push_d(st, x)))
    if fresh:
        scratch = st.copy()
        for i in pre:
            i.ex(scratch)
        push_tag(scratch, 'R', None)
        sub = new_sub(G, scratch, arg)
    form = G.ch(['bsr.w', 'bsr.w', 'jsr'] + (['bsr.l'] if G.cls else []))
    txt = ('\tjsr\t%s(pc)' % sub.lbl.name) if form == 'jsr' else ('\t%s\t%s' % (form, sub.lbl.name))
    call = Ins(txt, None)

    def ex(st, call=call, sub=sub):
        push_tag(st, 'R', call.next)
        return sub.lbl
    call.ex = ex
    call.ref = sub.lbl
    post = []
    if arg and not G.cls:
        post.append(Ins('\taddq.l\t#4,sp', lambda st: set_ptr(st, 15, st.r[15] + 4)))
    return pre + [call] + post, [], (sub if fresh else None)


def item_stack(G, st):
    """push / body / pop on the block stack (a byte push moves SP by 2)"""
    if G.p(0.4):
        s = G.ch([1, 2, 4])
        x, y = G.dreg(), G.ddst()

        def exa(st, x=x, s=s):
            lwr(st, loc(st, EA('pd', 15), s), s, st.r[x])
            setcc(st, nz(st.r[x], s), N | Z | V | C)

        def exb(st, y=y, s=s):
            v = lrd(st, loc(st, EA('pi', 15), s), s)
            st.r[y] = setlo(st.r[y], v, s)
            setcc(st, nz(v, s), N | Z | V | C)
        a = Ins('\tmove.%s\t%s,-(sp)' % (SC[s], rn(x)), exa)
        b = Ins('\tmove.%s\t(sp)+,%s' % (SC[s], rn(y)), exb)
    else:
        s = G.ch([2, 4])
        regs = sorted(G.rng.sample(list(range(8)) + DAREGS, G.rng.randint(2, 4)))
        rl = '/'.join(rn(r) for r in regs)

        def exa(st, regs=regs, s=s):
            b = st.r[15] - len(regs) * s
            if b < STK_LO:
                raise Bad('deep')
            set_ptr(st, 15, b)
            for i, r in enumerate(regs):
                mwr(st, b + i * s, s, st.r[r])

        def exb(st, regs=regs, s=s):
            b = st.r[15]
            for i, r in enumerate(regs):
                set_reg(st, r, sx(mrd(st, b + i * s, s), s))
            set_ptr(st, 15, b + len(regs) * s)
        a = Ins('\tmovem.%s\t%s,-(sp)' % (SC[s], rl), exa)
        b = Ins('\tmovem.%s\t(sp)+,%s' % (SC[s], rl), exb)
    scratch = st.copy()
    a.ex(scratch)
    inner = body(G, scratch, 1, 3)
    return [a] + inner + [b], []


def item_link(G, st):
    d = G.ch([0, 4, 8, 12, 16])
    lng = G.cls and G.p(0.3)

    def exl(st, d=d):
        push_tag(st, 'P', st.r[14])
        set_ptr(st, 14, st.r[15])
        if st.r[15] - d < STK_LO:
            raise Bad('deep')
        set_ptr(st, 15, st.r[15] - d)

    def exu(st):
        set_ptr(st, 15, st.r[14])
        v = pop_tag(st, 'P')
        if v is END:
            raise Bad('unlk')
        set_ptr(st, 14, v)
    a = Ins('\tlink.%s\ta6,#%d' % ('l' if lng else 'w', -d), exl)
    b = Ins('\tunlk\ta6', exu)
    scratch = st.copy()
    exl(scratch)
    G.reserved.add(14)
    G.cur = []

    def local(G, st, store):
        s = G.ch([1, 2, 4])
        o = -G.rng.randrange(s, d + 1)
        if s > 1 and (G.cls == 0 or G.p(0.7)):
            o &= ~1
        if o + s > 0:
            raise Bad('frame')
        if store:
            x = G.dreg()

            def ex(st, x=x, o=o, s=s):
                mwr(st, st.r[14] + o, s, st.r[x])
                setcc(st, nz(st.r[x], s), N | Z | V | C)
            G.emit(st, '\tmove.%s\t%s,%d(a6)' % (SC[s], rn(x), o), ex)
        else:
            y = G.ddst()

            def ex(st, y=y, o=o, s=s):
                v = mrd(st, st.r[14] + o, s)
                r, f = f_add(s, st.r[y], v)
                st.r[y] = setlo(st.r[y], r, s)
                setcc(st, f, 0x1F)
            G.emit(st, '\tadd.%s\t%d(a6),%s' % (SC[s], o, rn(y)), ex)
    for _ in range(G.rng.randint(1, 4)):
        k = G.rng.randrange(3)
        if d and k == 0:
            attempt(G, scratch, lambda G, st: local(G, st, True))
        elif d and k == 1:
            attempt(G, scratch, lambda G, st: local(G, st, False))
        else:
            straight(G, scratch)
    G.reserved.discard(14)
    return [a] + G.cur + [b], []


ITEMS = [('straight', 8), ('skip', 3), ('loop', 4), ('call', 2), ('stack', 1), ('link', 1)]


def gen_block(cls, seed):
    rng = random.Random(seed)
    G = Gen(rng, cls)
    st0 = init_state(seed, cls)
    st = st0.copy()
    main = []
    pend = []
    target = rng.randint(*TARGET)
    tries = 0
    while len(main) < target and tries < 400:
        tries += 1
        kind = rng.choices([k for k, _ in ITEMS], [w for _, w in ITEMS])[0]
        G.cur = []
        G.reserved = set()
        new = None
        try:
            if kind == 'straight':
                ins = body(G, st.copy(), 1, 3)
                lbls = []
            elif kind == 'skip':
                ins, lbls = item_skip(G, st)
            elif kind == 'loop':
                ins, lbls = item_loop(G, st)
            elif kind == 'call':
                ins, lbls, new = item_call(G, st)
            elif kind == 'stack':
                ins, lbls = item_stack(G, st)
            else:
                ins, lbls = item_link(G, st)
            if not ins:
                raise Bad('empty')
            end = Label()
            end.ins = ITEM_END
            link(ins, end)
            for lb in lbls:
                lb.ins = end
            scratch = st.copy()
            run(scratch, ins[0], ITEM_END)
        except Bad:
            continue
        for p in pend:
            p.ins = ins[0]
        pend = [end]
        main.extend(ins)
        if new:
            G.subs.append(new)
        st = scratch
    fin = Ins('\trts', lambda st: ex_rts(st))
    for p in pend:
        p.ins = fin
    main.append(fin)
    prog = main + [i for s in G.subs for i in s.code]
    for k, i in enumerate(prog):
        i.idx = k
    return prog, st0


def simulate(prog, st0, trace=None):
    """run the whole block; returns the checkpoints [(k, pred, snap)] in
    first-arrival order, then (0xFF, last, final)"""
    st = st0.copy()
    seen = set()
    order = []
    last = [None]

    def tr(ins, st):
        if ins.idx not in seen:
            seen.add(ins.idx)
            order.append((ins.idx, 0xFF if last[0] is None else last[0], st.snap()))
        if trace:
            trace(ins, st)
        last[0] = ins.idx
    n = run(st, prog[0], END, limit=5000, trace=tr)
    order.append((0xFF, last[0], st.snap()))
    if st.r[15] != STK_TOP:
        raise AssertionError('stack not balanced')
    return order, n


def diff_runs(a, b):
    """runs (off, len) where b differs from a; gaps of up to 2 equal bytes are merged"""
    runs = []
    i = 0
    while i < WIN:
        if a[i] == b[i]:
            i += 1
            continue
        j = i + 1
        last = i
        while j < WIN and j - last <= 3:
            if a[j] != b[j]:
                last = j
            j += 1
        runs.append((i, last + 1 - i))
        i = last + 1
    return runs


def encode(seed, order, st0snap, nstatic):
    out = bytearray(seed.to_bytes(4, 'big'))
    out += bytes([nstatic, len(order)])
    prev = st0snap
    pk = None
    for k, pred, snap in order:
        regs, ccr, dm, win = snap
        pregs, pccr, pdm, pwin = prev
        dmask = sum(1 << i for i in range(8) if regs[i] != pregs[i])
        amask = sum(1 << i for i in range(8) if regs[8 + i] != pregs[8 + i])
        runs = diff_runs(pwin, win)
        implicit = pk is not None and pk != 0xFF and k == pk + 1 and pred == pk
        hdr = ((0 if implicit else 0x80) | (0x40 if dmask else 0) | (0x20 if amask else 0)
               | (0x10 if runs else 0) | (0x08 if dm != pdm else 0) | (0x04 if ccr != pccr else 0))
        e = bytearray([hdr])
        if not implicit:
            e += bytes([k, pred])
        if dmask:
            e.append(dmask)
        if amask:
            e.append(amask)
        for i in range(16):
            if regs[i] != pregs[i]:
                e += regs[i].to_bytes(4, 'big')
        if dm != pdm:
            e.append(dm)
        if ccr != pccr:
            e.append(ccr)
        if runs:
            e.append(len(runs))
            for o, ln in runs:
                e += bytes([o, ln]) + win[o:o + ln]
        out += e
        prev = snap
        pk = k
    return bytes(out)


def block_seed(ci, g, b):
    return (((ci + 1) * 0x01000193 + g * 0x9E3779B1 + b * 0x85EBCA6B + 0x2545F491) & M32) | 1


def make_block(ci, g, b):
    seed = block_seed(ci, g, b)
    for attempt_ in range(20):
        Label.n = 0
        s = seed if attempt_ == 0 else (xs(seed + attempt_) | 1)
        prog, st0 = gen_block(ci, s)
        order, n = simulate(prog, st0)
        if len(prog) <= 120 and n <= 1500:
            return s, prog, st0, order, n
    raise AssertionError('could not make a block')


# --------------------------------------------------------------------------
# output

CLASSES_IDX = {c[0]: i for i, c in enumerate(CLASSES)}


def labels_of(prog):
    """{instruction index: [label names]} for every label a branch references"""
    out = {}
    for i in prog:
        lb = getattr(i, 'ref', None)
        if lb is not None:
            t = resolve(lb)
            out.setdefault(t.idx, [])
            if lb.name not in out[t.idx]:
                out[t.idx].append(lb.name)
    return out


def block_asm(bi, prog):
    lines = ['dsc_%d:' % bi]
    lo = labels_of(prog)
    for i in prog:
        lines.append('dsk_%d_%d:' % (bi, i.idx))
        for name in lo.get(i.idx, []):
            lines.append('%s_%d:' % (name, bi))
        lines.append(relabel(i.text, bi))
    lines.append('dse_%d:' % bi)
    return lines


def relabel(text, bi):
    import re
    return re.sub(r'\b(L\d+)\b', lambda m: '%s_%d' % (m.group(1), bi), text)


def all_blocks():
    for ci, (cname, cdesc, cmask) in enumerate(CLASSES):
        for g in range(GROUPS):
            for b in range(NBLK):
                yield ci, g, b


def ins_text(ins):
    """disassembly text of an instruction for the guest's failure records:
    branch labels become '@<static index of the target>'"""
    import re
    t = ins.text.strip().replace('\t', ' ', 1).replace('\t', '')
    if ins.ref is not None:
        t = re.sub(r'\bL\d+\b', '@%d' % resolve(ins.ref).idx, t)
    return t


def bpe_train(texts):
    """byte-pair encoding over all instruction texts: returns (rules, encoded).
    rules[code] = (a, b); 0 terminates each text and is never a code"""
    data = [list(t.encode('ascii')) for t in texts]
    used = set(b for t in data for b in t)
    free = [c for c in range(1, 256) if c not in used]
    rules = {}
    for code in free:
        cnt = {}
        for t in data:
            for a, b in zip(t, t[1:]):
                cnt[(a, b)] = cnt.get((a, b), 0) + 1
        if not cnt:
            break
        pair = max(sorted(cnt), key=lambda k: cnt[k])
        if cnt[pair] < 4:
            break
        rules[code] = pair
        nd = []
        for t in data:
            o = []
            i = 0
            while i < len(t):
                if i + 1 < len(t) and (t[i], t[i + 1]) == pair:
                    o.append(code)
                    i += 2
                else:
                    o.append(t[i])
                    i += 1
            nd.append(o)
        data = nd
    return rules, data


def emit(asm_path, doc_path):
    lines = ['; generated by tools/gencode.py - do not edit',
             '; dispatch blocks and expected-state streams (format: see the generator)',
             '\tmachine\t68020', '\topt\to-',
             '\txdef\t_ds_cases', '\txdef\t_ds_ncases', '\txdef\t_ds_blocks', '\txdef\t_ds_nblocks',
             '\txdef\t_ds_bpe',
             '\tsection\t.text,code']
    stats = {'ins': 0, 'dyn': 0, 'info': 0, 'code': 0, 'text': 0}
    made = [make_block(ci, g, b) for ci, g, b in all_blocks()]
    texts = [ins_text(i) for m in made for i in m[1]]
    rules, enc = bpe_train(texts)
    ei = 0
    bi = 0
    for seed, prog, st0, order, n in made:
        lines += block_asm(bi, prog)
        lines.append('\tif\t(dse_%d-dsc_%d)>510' % (bi, bi))
        lines.append('\tfail\t"dispatch block %d too long"' % bi)
        lines.append('\tendif')
        info = encode(seed, order, st0.snap(), len(prog))
        lines.append('dsi_%d:' % bi)
        lines.append('\tdc.b\t' + ','.join('$%02x' % v for v in info[:6]))
        for k in range(0, len(prog), 16):
            lines.append('\tdc.b\t' + ','.join('(dsk_%d_%d-dsc_%d)/2' % (bi, i.idx, bi) for i in prog[k:k + 16]))
        rest = info[6:]
        for k in range(0, len(rest), 24):
            lines.append('\tdc.b\t' + ','.join('$%02x' % v for v in rest[k:k + 24]))
        lines.append('dst_%d:' % bi)
        for i in prog:
            e = enc[ei] + [0]
            ei += 1
            lines.append('\tdc.b\t' + ','.join('$%02x' % v for v in e))
            stats['text'] += len(e)
        lines.append('\teven')
        stats['ins'] += len(prog)
        stats['dyn'] += n
        stats['info'] += len(info) + len(prog)
        bi += 1
    nblocks = bi
    lines.append('\tcnop\t0,4')
    lines.append('_ds_blocks:')
    for k in range(nblocks):
        lines.append('\tdc.l\tdsc_%d,dsi_%d,dst_%d' % (k, k, k))
    lines.append('_ds_nblocks:\tdc.w\t%d' % nblocks)
    lines.append('\tcnop\t0,4')
    lines.append('_ds_cases:')
    cases = []
    for ci, (cname, cdesc, cmask) in enumerate(CLASSES):
        for g in range(GROUPS):
            cases.append((cname, g, (ci * GROUPS + g) * NBLK, cdesc, cmask))
            lines.append('\tdc.l\tdsn_%s_%d' % (cname, g))
            lines.append('\tdc.w\t%d,%d,%d,%d,$%03x,0' % (ci, (ci * GROUPS + g) * NBLK, NBLK, NBLK_QUICK, cmask))
    lines.append('_ds_ncases:\tdc.w\t%d' % len(cases))
    lines.append('; byte-pair rules for the instruction texts: code c expands to the pair at 2*c (0,0: literal)')
    lines.append('_ds_bpe:')
    for c in range(0, 256, 16):
        lines.append('\tdc.b\t' + ','.join('$%02x,$%02x' % rules.get(k, (0, 0)) for k in range(c, c + 16)))
    for cname, g, first, cdesc, cmask in cases:
        lines.append('dsn_%s_%d:\tdc.b\t"%s_s%d",0' % (cname, g, cname, g + 1))
    lines.append('\teven')
    with open(asm_path, 'w') as f:
        f.write('\n'.join(lines) + '\n')
    write_doc(doc_path, cases, stats, nblocks)
    print('%d cases, %d blocks, %d static instructions (%.1f/block), %d executed, %d bytes of state streams, '
          '%d bytes of instruction text'
          % (len(cases), nblocks, stats['ins'], stats['ins'] / nblocks, stats['dyn'], stats['info'], stats['text']),
          file=sys.stderr)


def cpu_list(m):
    return ','.join(cb.CPUS[i] for i in range(12) if m >> i & 1)


def write_doc(path, cases, stats, nblocks):
    d = ['# dispatch (module 22): seeded random code blocks', '',
         'Generated by `tools/gencode.py`, which builds the blocks and computes the expected state of each '
         'one on the host with a reference model of the integer instructions (the semantics of '
         '`tools/gen_cpu_basic.py`: M68000 PRM, MC68020/030/040/060 user manuals).', '',
         'Each case is one (CPU class, seed group): %d blocks in full mode, the first %d in quick mode (same '
         'ids). The guest builds the initial state of a block from its seed (registers, CCR, a 256-byte data '
         'window), runs it (supervisor mode, interrupt mask 0, its own stack above the window), and compares '
         'd0-d7/a0-a7 (pointers a0-a3/a6/a7 as offsets from the window base M), the CCR (only the flags the '
         'manual defines at that point: a DIV overflow leaves N,Z undefined, ABCD/SBCD/NBCD leave N,V '
         'undefined) and the window with the expectation. Then `amt_hot()` re-runs block `i mod n` for '
         'i = 0..amt_hot_reps() and compares a hash of the result with the expected one (interpreter vs '
         'JIT-compiled execution).' % (NBLK, NBLK_QUICK), '',
         'On a mismatch the guest bisects in-guest: it copies the block to a buffer, replaces the first opcode '
         'word of static instruction k by TRAP #15, runs it, and compares the state captured by the trap with '
         'the expected state at the first arrival at k, for each k in first-arrival order. For a failure seen '
         'only on a repeated (hot) run, each truncated copy is run many times so that a JIT compiles it too. '
         'The record is self-contained: `expect`/`got` are the full register files (d0-d7, a0-a7, ccr, '
         'ccr_mask) at the first arrival at k, plus the window bytes that differ (offset:byte); `ctx.culprit` '
         'and `ctx.k` give index, disassembly text and opcode words of the instruction executed just before '
         'k (the one that went wrong) and of k; `ctx.before` is the expected state at the first arrival at the '
         'culprit; `ctx.block_text` lists every instruction of the block (branch targets as @index); '
         '`ctx.window_base` is M (a0-a3/a6/a7 are M-relative). `tools/gencode.py --trace <case> <block>` '
         'prints the block source and the full expected trace (the state before every executed instruction).', '',
         'Totals: %d blocks, %d static instructions, %d executed instructions.'
         % (nblocks, stats['ins'], stats['dyn']), '',
         '| case | intent | rule (manual) | CPUs | a failure usually means |', '|---|---|---|---|---|']
    for cname, g, first, cdesc, cmask in cases:
        d.append('| `%s_s%d` | %s; seed group %d: %d blocks of random valid code with Bcc/BRA, DBcc loops, '
                 'BSR/JSR/RTS%s, LINK/UNLK, stack push/pop and memory operands in a scratch window | '
                 'M68000 PRM (MC68020UM for c020/c040) instruction semantics: every register, the defined CCR '
                 'flags and the window after the block (and at each checkpoint) equal the reference model | %s | '
                 'an instruction computes a wrong result or flag, an addressing mode computes a wrong address '
                 'or register update, or the JIT and the interpreter diverge on a sequence (see `culprit`) |'
                 % (cname, g + 1, cdesc, g + 1, NBLK, '/RTD' if cname != 'c000' else '', cpu_list(cmask)))
    with open(path, 'w') as f:
        f.write('\n'.join(d) + '\n')


def fmt_state(snap, rel):
    regs, ccr, dm, win = snap
    out = []
    for i in range(16):
        v = regs[i]
        out.append('%s=%08x%s' % (rn(i), v, '@M' if ROLE[i] == 'M' else ''))
    out.append('ccr=%02x/%02x' % (ccr, dm))
    return ' '.join(out)


def do_trace(case, blk):
    cname, g = case.split('_s')
    ci = CLASSES_IDX[cname]
    g = int(g) - 1
    seed, prog, st0, order, n = make_block(ci, g, blk)
    bi = (ci * GROUPS + g) * NBLK + blk
    print('; case %s block %d (global block %d), seed %08x, %d static, %d executed' % (case, blk, bi, seed,
                                                                                     len(prog), n))
    print('\n'.join(block_asm(bi, prog)))
    print('; initial window:')
    for k in range(0, WIN, 32):
        print(';  %02x: %s' % (k, st0.win[k:k + 32].hex()))
    print('; trace (state before each executed instruction):')

    def tr(ins, st):
        print('[%3d] %-36s %s' % (ins.idx, ins.text.strip().replace('\t', ' '), fmt_state(st.snap(), True)))
    order, n = simulate(prog, st0, trace=tr)
    fin = order[-1][2]
    print('; final: %s' % fmt_state(fin, True))
    for k in range(0, WIN, 32):
        print(';  %02x: %s' % (k, fin[3][k:k + 32].hex()))
    print('; checkpoints (first-arrival order): ' + ' '.join('%d<-%s' % (k, p) for k, p, _ in order))


def main():
    ap = argparse.ArgumentParser()
    root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    ap.add_argument('--asm', default=os.path.join(root, 'disk', 'modules', 'dispatch_tab.s'))
    ap.add_argument('--doc', default=os.path.join(root, 'docs', 'cases', 'dispatch.md'))
    ap.add_argument('--trace', nargs=2, metavar=('CASE', 'BLOCK'))
    a = ap.parse_args()
    if a.trace:
        do_trace(a.trace[0], int(a.trace[1]))
        return
    emit(a.asm, a.doc)


if __name__ == '__main__':
    main()
