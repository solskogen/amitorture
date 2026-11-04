#!/usr/bin/env python3
"""Host-side blitter model and case-table generator for the amitorture
`blitter` module (disk/modules/blitter.c).

The model follows the Amiga Hardware Reference Manual, chapter 6 (Blitter):
area mode (minterms, A/B barrel shifters, first/last word masks, modulos,
ascending/descending, inclusive/exclusive fill with FCI, the zero flag) and
line mode (octants, SIGN-driven Bresenham stepping, SING, texture).

    gen_blitter.py                 write disk/modules/blitter_tab.c (and docs table)
    gen_blitter.py --list          list the case ids
    gen_blitter.py --dump ID       print the case registers, the fills and the
                                   expected destination window (hex words), to
                                   compare with a failing record from the guest

Each case describes one blit inside a 128 KB chip pool (offsets relative to
amt_chip_pool()). The guest fills the listed regions with xorshift32 words
from the given seeds, runs a priming blit (A and B from a zero word, so the
A/B "previous word" holding registers are zero whether or not the hardware
clears them at blit start), runs the blit and CRCs the destination window,
which includes guard bytes on both sides. The model does the same on a
bytearray and also tracks which bytes are defined, so a case that reads or
CRCs undefined memory fails at generation time.
"""
import argparse
import os
import sys
import zlib

MUTATE = set()           # --mutate: deliberately wrong model rules (self-test)
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
POOL = 128 * 1024
GUARD = 8                 # guard bytes on each side of every destination window
PRIME_ZERO = 0x0000       # 4 zero bytes the priming blit reads (A and B)
PRIME_SCRATCH = 0x0010    # priming blit's 1-word D target

# BLTCON0
USEA, USEB, USEC, USED = 0x800, 0x400, 0x200, 0x100
# BLTCON1
LINE, DESC, FCI, IFE, EFE = 0x01, 0x02, 0x04, 0x08, 0x10
SING, SIGN = 0x02, 0x40
SUD, SUL, AUL = 0x10, 0x08, 0x04

# case flags (shared with blitter.c)
BF_LINE = 0x0001      # line mode: apt is the raw error term, not a pool offset
BF_BIG = 0x0002       # ECS: start through BLTSIZV/BLTSIZH
BF_ECS = 0x0004       # needs ECS Agnus (skip on OCS)
BF_NOZERO = 0x0008    # do not check BZERO
BF_CON0L = 0x0010     # ECS: write BLTCON0L = aux after BLTCON0
BF_SPECIAL = 0x0020   # only run by a special test (irq, bbusy, nasty)
BF_HOT = 0x0040       # also run under amt_hot()


def s16(v):
    v &= 0xffff
    return v - 0x10000 if v & 0x8000 else v


def fill_words(seed, n):
    """Guest fill rule (blitter.c fill_region): an odd seed gives xorshift32
    words (the upper halves); an even seed gives n copies of seed >> 16."""
    if seed & 1:
        return xorshift_words(seed, n)
    return [seed >> 16 & 0xffff] * n


def xorshift_words(seed, n):
    x = seed & 0xffffffff
    out = []
    for _ in range(n):
        x ^= (x << 13) & 0xffffffff
        x ^= x >> 17
        x ^= (x << 5) & 0xffffffff
        out.append(x >> 16)
    return out


def minterm(lf, a, b, c):
    d = 0
    for i in range(8):
        if lf >> i & 1:
            d |= (a if i & 4 else ~a) & (b if i & 2 else ~b) & (c if i & 1 else ~c)
    return d & 0xffff


class Mem:
    def __init__(self):
        self.b = bytearray(POOL)
        self.valid = bytearray(POOL)

    def rw(self, off, what):
        if not (0 <= off < POOL - 1):
            raise ValueError(f"{what}: read outside the pool at {off:#x}")
        if not (self.valid[off] and self.valid[off + 1]):
            raise ValueError(f"{what}: read of undefined memory at {off:#x}")
        return self.b[off] << 8 | self.b[off + 1]

    def ww(self, off, v, what):
        if not (0 <= off < POOL - 1):
            raise ValueError(f"{what}: write outside the pool at {off:#x}")
        self.b[off] = v >> 8 & 0xff
        self.b[off + 1] = v & 0xff
        self.valid[off] = self.valid[off + 1] = 1

    def fill(self, off, words, seed):
        for i, w in enumerate(fill_words(seed, words)):
            self.ww(off + 2 * i, w, "fill")


class Case:
    """One blit. Pointers are pool offsets (apt is the error term in line mode)."""

    def __init__(self, cid, **k):
        self.id = cid
        self.con0 = k.get("con0", 0)
        self.con1 = k.get("con1", 0)
        self.afwm = k.get("afwm", 0xffff)
        self.alwm = k.get("alwm", 0xffff)
        self.adat = k.get("adat", 0)
        self.bdat = k.get("bdat", 0)
        self.cdat = k.get("cdat", 0)
        self.amod = k.get("amod", 0)
        self.bmod = k.get("bmod", 0)
        self.cmod = k.get("cmod", 0)
        self.dmod = k.get("dmod", 0)
        self.apt = k.get("apt", 0)
        self.bpt = k.get("bpt", 0)
        self.cpt = k.get("cpt", 0)
        self.dpt = k.get("dpt", 0)
        self.w = k["w"]
        self.h = k["h"]
        self.flags = k.get("flags", 0)
        self.aux = k.get("aux", 0)
        self.fills = list(k.get("fills") or [])   # (off, words, seed)
        self.window = k.get("window")             # (off, len) or None: derived
        self.group = k.get("group", "")
        self.note = k.get("note", "")
        self.seed_base = k.get("seed", 0)
        self.zero_alt = None      # BZERO under the other readings of the HRM (run_*)

    # --- size encoding -------------------------------------------------
    def size_regs(self):
        if self.flags & BF_BIG:
            return 0, self.h & 0x7fff, self.w & 0x7ff
        assert 1 <= self.w <= 64 and 1 <= self.h <= 1024, self.id
        return ((self.h & 1023) << 6) | (self.w & 63), 0, 0

    # --- model ---------------------------------------------------------
    def effective_con0(self):
        if self.flags & BF_CON0L:
            return (self.con0 & 0xff00) | (self.aux & 0xff)
        return self.con0

    def run(self, mem):
        if self.flags & BF_LINE:
            return self.run_line(mem)
        return self.run_area(mem)

    def run_area(self, mem):
        con0 = self.effective_con0()
        con1 = self.con1
        ash, bsh = con0 >> 12, con1 >> 12
        lf = con0 & 0xff
        desc = bool(con1 & DESC)
        ife, efe = bool(con1 & IFE), bool(con1 & EFE)
        assert not (ife and efe), self.id
        assert not ((ife or efe) and not desc), self.id + ": fill needs descending mode"
        step = -2 if desc else 2
        sg = -1 if desc else 1
        apt, bpt, cpt, dpt = self.apt, self.bpt, self.cpt, self.dpt
        adat, bdat, cdat = self.adat, self.bdat, self.cdat
        aold = bold = 0
        zero = True
        zero_pre_fill = True
        for _row in range(self.h):
            fc = 1 if con1 & FCI else 0
            for col in range(self.w):
                if con0 & USEA:
                    adat = mem.rw(apt, "A")
                    apt += step
                if con0 & USEB:
                    bdat = mem.rw(bpt, "B")
                    bpt += step
                if con0 & USEC:
                    cdat = mem.rw(cpt, "C")
                    cpt += step
                a = adat
                if col == 0:
                    a &= self.afwm
                if col == self.w - 1:
                    a &= self.alwm
                if desc:
                    ah = (((a << 16) | aold) >> (16 - ash)) & 0xffff
                    bh = (((bdat << 16) | bold) >> (16 - bsh)) & 0xffff
                else:
                    ah = (((aold << 16) | a) >> ash) & 0xffff
                    bh = (((bold << 16) | bdat) >> bsh) & 0xffff
                aold, bold = (adat if "aold_unmasked" in MUTATE else a), bdat
                d = minterm(lf, ah, bh, cdat)
                if d:
                    zero_pre_fill = False
                if ife or efe:
                    out = 0
                    for bit in range(16):
                        ib = d >> bit & 1
                        ob = (ib | fc) if (ife or "fill_excl_as_incl" in MUTATE) else (ib ^ fc)
                        fc ^= ib
                        out |= ob << bit
                    d = out
                if d:
                    zero = False
                if con0 & USED:
                    mem.ww(dpt, d, "D")
                    dpt += step
            if con0 & USEA:
                apt += sg * s16(self.amod & ~1)
            if con0 & USEB:
                bpt += sg * s16(self.bmod & ~1)
            if con0 & USEC:
                cpt += sg * s16(self.cmod & ~1)
            if con0 & USED:
                dpt += sg * s16(self.dmod & ~1)
        # HRM 6: BZERO is set when every word the blit produced was zero. The
        # block diagram puts the zero detector on the D output, after the fill
        # circuit; the other reading (minterm output, before the fill) is
        # recorded so that evaluate() checks BZERO only where both agree.
        if ife or efe:
            self.zero_alt = [zero_pre_fill]
        return zero

    def run_line(self, mem):
        con0, con1 = self.con0, self.con1
        ash, bsh = con0 >> 12, con1 >> 12
        lf = con0 & 0xff
        # Only minterms whose not-A half is "D = C" are used, so the value of
        # the B (texture) hold register outside the pixel's bit cannot matter.
        assert lf & 0x0f == 0x0a, self.id
        assert self.adat == 0x8000 and self.afwm == 0xffff and self.alwm == 0xffff
        err = s16(self.apt)
        sign = bool(con1 & SIGN)
        assert sign == (err < 0), self.id
        ptr = self.cpt
        assert self.dpt == self.cpt
        onedot = False
        # BZERO (HRM 6: every D word the blit produced was zero). Which words a
        # SING step that draws no dot "produces" is not documented: the D words
        # written (zero), the minterm with the dot also on suppressed steps
        # (zero_dot), or C unchanged on them (zero_c). evaluate() checks BZERO
        # only where all three agree.
        zero = zero_dot = zero_c = True
        for _ in range(self.h):
            draw = not (con1 & SING and onedot)
            onedot = "sing_ignored" not in MUTATE
            c = mem.rw(ptr, "C")
            tbit = (self.bdat >> bsh) & 1
            d = minterm(lf, 0x8000 >> ash, 0xffff if tbit else 0, c)
            if d:
                zero_dot = False
            if draw:
                mem.ww(ptr, d, "D")
                if d:
                    zero = zero_c = False
            elif c:
                zero_c = False
            bsh = (bsh + (1 if "texture_up" in MUTATE else -1)) & 15
            # minor step when SIGN is clear, then the major step
            dx = dy = 0
            if con1 & SUD:          # x major
                dx = -1 if con1 & AUL else 1
                if not sign:
                    dy = -1 if con1 & SUL else 1
            else:                   # y major
                dy = -1 if con1 & AUL else 1
                if not sign:
                    dx = -1 if con1 & SUL else 1
            err = s16(err + (s16(self.amod) if not sign else s16(self.bmod)))
            sign = err < 0
            if dy:
                ptr += dy * s16(self.cmod)
                onedot = False
            if dx > 0:
                ash += 1
                if ash == 16:
                    ash = 0
                    ptr += 2
            elif dx < 0:
                ash -= 1
                if ash < 0:
                    ash = 15
                    ptr -= 2
        self.zero_alt = [zero_dot, zero_c]
        return zero

    # --- derived layout ------------------------------------------------
    def spans(self):
        """Byte spans [lo, hi) touched per channel (area mode), by pointer walk."""
        con0 = self.effective_con0()
        desc = bool(self.con1 & DESC)
        step = -2 if desc else 2
        sg = -1 if desc else 1
        res = {}
        for ch, use, pt, mod in (("A", USEA, self.apt, self.amod), ("B", USEB, self.bpt, self.bmod),
                                 ("C", USEC, self.cpt, self.cmod), ("D", None, self.dpt, self.dmod)):
            if use is not None and not con0 & use:
                continue
            lo, hi = None, None
            p = pt
            for _ in range(self.h):
                first, last = p, p + step * (self.w - 1)
                a, b = min(first, last), max(first, last) + 2
                lo = a if lo is None else min(lo, a)
                hi = b if hi is None else max(hi, b)
                p += step * self.w + sg * s16(mod & ~1)
            res[ch] = (lo, hi)
        return res

    def finish_layout(self, seeds):
        """Fill the source spans and the destination window from seeds."""
        if self.window is None:
            lo, hi = self.spans()["D"]
            self.window = (lo - GUARD, hi - lo + 2 * GUARD)
        if not self.fills:
            sp = self.spans() if not self.flags & BF_LINE else {}
            for ch in "ABC":
                if ch in sp:
                    lo, hi = sp[ch]
                    self.fills.append((lo, (hi - lo) // 2, next(seeds)))
            wo, wl = self.window
            self.fills.append((wo, wl // 2, next(seeds)))

    def evaluate(self):
        mem = Mem()
        for off, words, seed in self.fills:
            mem.fill(off, words, seed)
        zero = self.run(mem)
        wo, wl = self.window
        if not all(mem.valid[wo:wo + wl]):
            raise ValueError(f"{self.id}: undefined bytes in the CRC window")
        self.expect = bytes(mem.b[wo:wo + wl])
        self.crc = zlib.crc32(self.expect) & 0xffffffff
        ambiguous = self.zero_alt is not None and any(z != zero for z in self.zero_alt)
        self.zero = 2 if (self.flags & BF_NOZERO or ambiguous) else (1 if zero else 0)
        return self


# ---------------------------------------------------------------- cases
def seedgen(start):
    s = start
    while True:
        s = (s * 1103515245 + 12345) & 0xffffffff
        yield s | 1


A_BASE, B_BASE, C_BASE, D_BASE = 0x0100, 0x4100, 0x8100, 0xC100


def area(cid, con0, con1=0, w=4, h=4, amod=0, bmod=0, cmod=0, dmod=0, **k):
    """An area blit with the four channels in their own regions."""
    desc = bool(con1 & DESC)
    def start(base, mod):
        if not desc:
            return base + GUARD + 64
        # last word of the last row
        return base + GUARD + 64 + (h * (2 * w + s16(mod)) - s16(mod)) - 2
    apt = k.pop("apt", start(A_BASE, amod))
    bpt = k.pop("bpt", start(B_BASE, bmod))
    cpt = k.pop("cpt", start(C_BASE, cmod))
    dpt = k.pop("dpt", start(D_BASE, dmod))
    return Case(cid, con0=con0, con1=con1, w=w, h=h, amod=amod, bmod=bmod, cmod=cmod, dmod=dmod,
                apt=apt, bpt=bpt, cpt=cpt, dpt=dpt, **k)


def line_case(cid, x1, y1, x2, y2, lf=0xca, sing=False, tex=0xffff, bsh=None, group="line",
              note="", flags=0):
    PW, PH = 40, 100                # plane: 320 x 100 pixels
    plane = 0x2000 + GUARD
    dx, dy = x2 - x1, y2 - y1
    adx, ady = abs(dx), abs(dy)
    xmaj = adx >= ady
    dmax, dmin = (adx, ady) if xmaj else (ady, adx)
    if xmaj:
        oct_ = SUD | (AUL if dx < 0 else 0) | (SUL if dy < 0 else 0)
    else:
        oct_ = (AUL if dy < 0 else 0) | (SUL if dx < 0 else 0)
    err = 4 * dmin - 2 * dmax
    if bsh is None:
        bsh = x1 & 15
    con0 = ((x1 & 15) << 12) | USEA | USEC | USED | lf
    con1 = (bsh << 12) | oct_ | (SING if sing else 0) | (SIGN if err < 0 else 0) | LINE
    ptr = plane + y1 * PW + 2 * (x1 >> 4)
    assert 0 <= x1 < 320 and 0 <= x2 < 320 and 0 <= y1 < PH and 0 <= y2 < PH
    return Case(cid, con0=con0, con1=con1, w=2, h=dmax + 1, adat=0x8000, bdat=tex,
                amod=(4 * (dmin - dmax)) & 0xffff, bmod=(4 * dmin) & 0xffff, cmod=PW, dmod=PW,
                apt=err & 0xffff, cpt=ptr, dpt=ptr, flags=BF_LINE | flags,
                fills=[(plane - GUARD, (PW * PH + 2 * GUARD) // 2, None)],
                window=(plane - GUARD, PW * PH + 2 * GUARD), group=group,
                note=note or f"({x1},{y1})-({x2},{y2}) dmax={dmax} dmin={dmin} oct={oct_:#04x}"
                f"{' SING' if sing else ''} tex={tex:04x} bsh={bsh} lf={lf:02x}")


def build_cases():
    cases = []
    ABCD = USEA | USEB | USEC | USED

    # 1. all 256 minterms, A/B/C -> D, 4x4 words, no shift
    for lf in range(256):
        cases.append(area(f"minterm_{lf:02x}", ABCD | lf, w=4, h=4, amod=2, bmod=4, cmod=6, dmod=2,
                          group="minterm", flags=BF_HOT if lf == 0xe4 else 0))

    # 2. A and B shifts 0..15, ascending and descending (ASH=n, BSH=15-n),
    #    D = AB + !AC ($CA) so both shifted sources and C reach D; 5x4 words
    for desc in (0, 1):
        for n in range(16):
            cases.append(area(f"shift_{'desc' if desc else 'asc'}_{n:02d}",
                              (n << 12) | ABCD | 0xca, ((15 - n) << 12) | (DESC if desc else 0),
                              w=5, h=4, amod=2, bmod=0, cmod=4, dmod=2, group="shift",
                              flags=BF_HOT if (desc and n == 7) else 0))
    # B shift alone (A disabled, ADAT=$FFFF so D = B) for every BSH, ascending
    for n in range(16):
        cases.append(area(f"shift_b_only_{n:02d}", USEB | USED | 0xcc, n << 12,
                          w=3, h=3, bmod=2, dmod=0, adat=0xffff, group="shift"))

    # 3. first/last word masks
    mk = [("mask_w1", 1, 0, 0x0ff0, 0x3c3f), ("mask_w2", 2, 0, 0xf00f, 0x0ff0),
          ("mask_w5", 5, 0, 0x7fff, 0xfffe), ("mask_w5_zero_first", 5, 0, 0x0000, 0xffff),
          ("mask_w5_zero_last", 5, 0, 0xffff, 0x0000),
          ("mask_w1_desc", 1, 1, 0x0ff0, 0x3c3f), ("mask_w5_desc", 5, 1, 0x00ff, 0xff00)]
    for cid, w, desc, f, l in mk:
        cases.append(area(cid, ABCD | 0xca, DESC if desc else 0, w=w, h=4, amod=2, bmod=2, cmod=2,
                          dmod=2, afwm=f, alwm=l, group="mask"))
    # masks seen through the A shifter: masked bits shift into the next word
    # and (via the last word) into the next line's first word
    for desc in (0, 1):
        cases.append(area(f"mask_shift_{'desc' if desc else 'asc'}", (5 << 12) | ABCD | 0xca,
                          (11 << 12) | (DESC if desc else 0), w=4, h=5, amod=0, bmod=2, cmod=2,
                          dmod=2, afwm=0xf0f0, alwm=0x3ffc, group="mask"))
    # the HRM "extra word, last word mask 0" shift idiom
    cases.append(area("mask_extra_word_shift", (9 << 12) | USEA | USEC | USED | 0xfa, 0,
                      w=4, h=6, amod=-2, cmod=0, dmod=0, afwm=0xffff, alwm=0x0000, group="mask"))
    # A disabled, ADAT=$FFFF: the masks alone shape a rectangle (D = A xor C).
    # Not stated by the HRM (its masking text says "the A source data", not
    # "DMA"). Evidence: WinUAE/Amiberry blitter.cpp masks the A data with
    # FWM/LWM whether it was fetched or preloaded (blitter_doblit: bltadat =
    # blt_info.bltadat when A is off, then &= blit_masktable[i];
    # blitter_loadadat: bltadat &= bltafwm/bltalwm). Not hardware-confirmed.
    cases.append(area("mask_adat_rect", USEC | USED | 0x5a, 0, w=6, h=5, cmod=4, dmod=4,
                      adat=0xffff, afwm=0x07ff, alwm=0xffe0, group="mask"))

    # 4. modulos
    cases.append(area("mod_positive", ABCD | 0xd8, 0, w=3, h=6, amod=10, bmod=4, cmod=8, dmod=14,
                      group="mod"))
    cases.append(area("mod_negative_a_repeat_row", USEA | USEC | USED | 0x6a, 0, w=4, h=6,
                      amod=-8, cmod=2, dmod=2, group="mod"))
    cases.append(area("mod_negative_b", USEA | USEB | USED | 0xc0 | 0x30, (3 << 12), w=4, h=5,
                      amod=0, bmod=-12, dmod=0, group="mod"))
    cases.append(area("mod_negative_d_upward", USEA | USED | 0xf0, 0, w=3, h=6, amod=0,
                      dmod=-12, dpt=D_BASE + 0x200, group="mod"))
    cases.append(area("mod_desc_positive", ABCD | 0x96, (2 << 12) | DESC, w=3, h=5, amod=6,
                      bmod=2, cmod=4, dmod=8, group="mod"))
    cases.append(area("mod_desc_negative", USEA | USED | 0xf0, (6 << 12) | DESC, w=3, h=4,
                      amod=-6, dmod=4, group="mod"))
    # in-place C=D (same pointer and modulo), and an overlapping descending move
    cases.append(Case("mod_inplace_c_eq_d", con0=(3 << 12) | USEA | USEC | USED | 0x5a, w=5, h=6,
                      amod=4, cmod=6, dmod=6, apt=A_BASE + 64, cpt=D_BASE + 64, dpt=D_BASE + 64,
                      group="mod"))
    dlast = D_BASE + 64 + 6 * 20 - 2
    cases.append(Case("mod_overlap_desc_move", con0=(4 << 12) | USEA | USED | 0xf0, con1=DESC,
                      w=8, h=6, amod=4, dmod=4, apt=dlast, dpt=dlast + 22,
                      window=(D_BASE + 64 - GUARD, 6 * 20 + 24 + 2 * GUARD),
                      fills=[(D_BASE + 64 - GUARD, (6 * 20 + 24 + 2 * GUARD) // 2, None)],
                      group="mod"))

    # 5. channel enable combinations (A,B,C data registers preloaded for
    #    the disabled channels, no shift); D = A xor B xor C
    for m in range(16):
        con0 = (USEA if m & 8 else 0) | (USEB if m & 4 else 0) | (USEC if m & 2 else 0) | \
               (USED if m & 1 else 0)
        name = "".join(ch for ch, bit in (("a", 8), ("b", 4), ("c", 2), ("d", 1)) if m & bit) or "none"
        cases.append(area(f"use_{name}", con0 | 0x96, 0, w=3, h=3, amod=2, bmod=2, cmod=2, dmod=2,
                          adat=0x1234, bdat=0xa5c3, cdat=0x0ff0, group="use"))

    # 6. zero flag (BZERO), with and without D
    for d in (0, 1):
        suf = "d" if d else "nod"
        dd = USED if d else 0
        # A and B read the same words: A and not B is zero everywhere
        cases.append(area(f"zero_{suf}_same_src", USEA | USEB | dd | 0x30, 0, w=4, h=4,
                          amod=2, bmod=2, dmod=2, bpt=A_BASE + GUARD + 64, group="zero"))
        # ... unless B is shifted against A
        cases.append(area(f"zero_{suf}_shifted_src", USEA | USEB | dd | 0x30, 1 << 12, w=4, h=4,
                          amod=2, bmod=2, dmod=2, bpt=A_BASE + GUARD + 64, group="zero"))
        # data outside the masks only: zero
        cases.append(area(f"zero_{suf}_masked", USEA | dd | 0xf0, 0, w=1, h=8, amod=0, dmod=0,
                          afwm=0x0000, alwm=0xffff, group="zero"))
        # minterm 0 and minterm $ff
        cases.append(area(f"zero_{suf}_lf00", USEA | USEB | USEC | dd | 0x00, 0, w=2, h=2,
                          group="zero"))
        cases.append(area(f"zero_{suf}_lfff", USEA | USEB | USEC | dd | 0xff, 0, w=2, h=2,
                          group="zero"))
        # a single non-zero bit in the last word of the last line
        cases.append(area(f"zero_{suf}_last_word", USEC | dd | 0x0a, 0, w=4, h=3, cmod=0, dmod=0,
                          fills=None, group="zero"))

    # 7. fill (descending only), sparse edges D = A&B&C
    for kind, bit in (("incl", IFE), ("excl", EFE)):
        for fci in (0, 1):
            cases.append(area(f"fill_{kind}_fci{fci}", ABCD | 0x80, DESC | bit | (FCI if fci else 0),
                              w=4, h=8, amod=2, bmod=2, cmod=2, dmod=2, group="fill",
                              flags=BF_HOT if (kind == "excl" and fci == 0) else 0))
        cases.append(area(f"fill_{kind}_shift", (6 << 12) | ABCD | 0x80, (3 << 12) | DESC | bit,
                          w=3, h=6, amod=0, bmod=2, cmod=4, dmod=4, afwm=0x3fff, alwm=0xfff8,
                          group="fill"))
        # in place, A only: D = fill(A) written over itself (the polygon idiom)
        pt = D_BASE + 64 + 10 * 12 - 2
        cases.append(Case(f"fill_{kind}_inplace", con0=USEA | USED | 0xf0, con1=DESC | bit,
                          w=6, h=10, apt=pt, dpt=pt, group="fill",
                          window=(D_BASE + 64 - GUARD, 120 + 2 * GUARD),
                          fills=[(D_BASE + 64 - GUARD, (120 + 2 * GUARD) // 2, None)]))
        cases[-1].sparse = True

    # 8. line mode
    oct_pts = {      # end point offsets per octant (HRM octant numbering), three slopes each;
        # the middle one has dmin = dmax/2, so the error term hits exactly 0
        "o1": [(80, 7), (80, 40), (45, 44)],      # dx>0 dy>0, x major
        "o2": [(7, 45), (22, 44), (1, 45)],       # dx>0 dy>0, y major
        "o3": [(-7, 45), (-22, 44), (-44, 45)],   # dx<0 dy>0, y major
        "o4": [(-80, 7), (-80, 40), (-80, 1)],    # dx<0 dy>0, x major
        "o5": [(-80, -7), (-80, -40), (-45, -44)],
        "o6": [(-7, -45), (-22, -44), (-1, -45)],
        "o7": [(7, -45), (22, -44), (44, -45)],
        "o8": [(80, -7), (80, -40), (80, -1)],
    }
    oct_code = {"o1": SUD, "o2": 0, "o3": SUL, "o4": SUD | AUL, "o5": SUD | SUL | AUL,
                "o6": SUL | AUL, "o7": AUL, "o8": SUD | SUL}   # hardware/blit.h OCTANTn
    x0, y0 = 150, 49
    for o, pts in oct_pts.items():
        for i, (dx, dy) in enumerate(pts):
            c = line_case(f"line_{o}_{i}", x0 + 3 * i, y0 - i, x0 + 3 * i + dx, y0 - i + dy,
                          group="line", flags=BF_HOT if (o == "o5" and i == 1) else 0)
            assert c.con1 & 0x1c == oct_code[o], c.id
            cases.append(c)
    cases.append(line_case("line_horizontal", 20, 10, 300, 10))
    cases.append(line_case("line_horizontal_left", 300, 12, 21, 12))
    cases.append(line_case("line_vertical", 77, 2, 77, 97))
    cases.append(line_case("line_vertical_up", 78, 97, 78, 2))
    cases.append(line_case("line_diag", 10, 5, 99, 94))
    cases.append(line_case("line_single_pixel", 123, 45, 123, 45))
    cases.append(line_case("line_two_pixels", 15, 45, 16, 45))
    cases.append(line_case("line_long", 0, 0, 319, 99))
    # SING (one dot per horizontal line), every octant, XOR minterm
    for o, pts in oct_pts.items():
        dx, dy = pts[0]
        cases.append(line_case(f"line_sing_{o}", x0, y0, x0 + dx, y0 + dy, lf=0x4a, sing=True,
                               group="line_sing"))
    # texture (BLTBDAT pattern, BSH = start bit)
    for i, (tex, bsh, p) in enumerate([(0xf0f0, 15, (10, 20, 200, 40)), (0xaaaa, 0, (10, 30, 200, 80)),
                                       (0x8001, 7, (300, 90, 20, 70)), (0x1234, 3, (100, 90, 130, 5)),
                                       (0xff00, 12, (250, 5, 40, 30)), (0x00ff, 9, (5, 95, 90, 2))]):
        cases.append(line_case(f"line_texture_{i}", *p, tex=tex, bsh=bsh, group="line_tex"))
    # other minterms with D=C outside the pixel: set, clear, XOR, inverted texture
    for lf in (0xfa, 0x0a, 0x4a, 0x2a, 0x8a, 0xba):
        cases.append(line_case(f"line_lf_{lf:02x}", 30, 80, 280, 20, lf=lf, tex=0xc3a5, bsh=11,
                               group="line_lf"))

    # 9. sizes
    cases.append(area("size_1x1", ABCD | 0xca, (2 << 12), w=1, h=1, group="size"))
    cases.append(Case("size_1x1024", con0=(5 << 12) | USEA | USEC | USED | 0x6a, w=1, h=1024,
                      amod=0, cmod=0, dmod=0, apt=0x0100 + 64, cpt=0x1000 + 64, dpt=0x2000 + 64,
                      group="size"))
    cases.append(Case("size_64x16", con0=(13 << 12) | ABCD | 0xb8, con1=(5 << 12), w=64, h=16,
                      amod=2, bmod=0, cmod=4, dmod=6, apt=0x0100 + 64, bpt=0x1000 + 64,
                      cpt=0x2000 + 64, dpt=0x3000 + 64, group="size", afwm=0x7fff, alwm=0xfff0))
    cases.append(Case("size_64x1_desc", con0=(10 << 12) | USEA | USEB | USED | 0xc6,
                      con1=(6 << 12) | DESC, w=64, h=1, apt=0x0100 + 200, bpt=0x1000 + 200,
                      dpt=0x2000 + 200, group="size"))
    # ECS big blits through BLTSIZV/BLTSIZH
    cases.append(Case("big_sizv_1100x2", con0=(3 << 12) | USEA | USEC | USED | 0x6a, w=2, h=1100,
                      amod=0, cmod=0, dmod=0, apt=0x0100 + 64, cpt=0x2000 + 64, dpt=0x4000 + 64,
                      flags=BF_BIG | BF_ECS, group="big"))
    cases.append(Case("big_sizh_100x20", con0=(7 << 12) | ABCD | 0xca, con1=(1 << 12), w=100,
                      h=20, amod=0, bmod=2, cmod=0, dmod=4, apt=0x0100 + 64, bpt=0x2000 + 64,
                      cpt=0x4000 + 64, dpt=0x6000 + 64, flags=BF_BIG | BF_ECS, group="big",
                      afwm=0x0fff, alwm=0xff80))
    cases.append(Case("big_sizh_200x3_desc", con0=(9 << 12) | USEA | USEB | USED | 0xe2,
                      con1=(4 << 12) | DESC, w=200, h=3, amod=2, bmod=0, dmod=2,
                      apt=0x0100 + 64 + 3 * 402 - 2 - 2, bpt=0x2000 + 64 + 3 * 400 - 2,
                      dpt=0x4000 + 64 + 3 * 402 - 2 - 2, flags=BF_BIG | BF_ECS, group="big"))
    cases.append(area("big_small_via_sizv", ABCD | 0x1e, (2 << 12), w=4, h=4, amod=2, bmod=2,
                      cmod=2, dmod=2, flags=BF_BIG | BF_ECS, group="big"))
    cases.append(area("ecs_bltcon0l", ABCD | 0x00, 0, w=4, h=3, amod=2, bmod=2, cmod=2, dmod=2,
                      flags=BF_CON0L | BF_ECS, aux=0xd2, group="big"))

    # 10. bases for the special tests (nasty, interrupt, BBUSY)
    cases.append(Case("nasty_base", con0=(1 << 12) | ABCD | 0x96, con1=(2 << 12), w=32, h=32,
                      amod=0, bmod=0, cmod=0, dmod=0, apt=0x0100 + 64, bpt=0x1000 + 64,
                      cpt=0x2000 + 64, dpt=0x3000 + 64, flags=BF_SPECIAL, group="special"))
    cases.append(area("irq_base", ABCD | 0x5c, (4 << 12), w=8, h=8, amod=2, bmod=2, cmod=2, dmod=2,
                      flags=BF_SPECIAL, group="special"))
    cases.append(Case("bbusy_base", con0=ABCD | 0xca, w=64, h=64, apt=0x0100 + 64,
                      bpt=0x3000 + 64, cpt=0x6000 + 64, dpt=0x9000 + 64,
                      flags=BF_SPECIAL, group="special"))

    seeds = seedgen(0x5eed0001)
    for c in cases:
        if c.fills is None:
            c.fills = []
        c.fills = [(o, n, s if s is not None else next(seeds)) for o, n, s in c.fills]
        c.finish_layout(seeds)
    ids = [c.id for c in cases]
    assert len(ids) == len(set(ids)), "duplicate case id"
    return cases


def fixups(cases):
    """Hand-made fill patterns (even seeds are constants, see fill_words)."""
    for c in cases:
        if getattr(c, "sparse", False):
            # in-place fill: zero rows with two single-bit edge words each
            wo, wl = c.window
            base = wo + GUARD
            fills = [(wo, wl // 2, 0)]
            for r in range(c.h):
                for pos in ((r * 37 + 5) % (16 * c.w), (r * 53 + 50) % (16 * c.w)):
                    off = base + r * 2 * c.w + 2 * (pos // 16)
                    fills.append((off, 1, (0x8000 >> (pos % 16)) << 16))
            c.fills = fills
        if c.id.endswith("_last_word"):
            # C is zero except one bit in the last word of the last line
            wo, wl = c.window
            last = c.cpt + 2 * (c.w * c.h - 1)
            c.fills = [(wo, wl // 2, 0x55550000), (c.cpt, c.w * c.h, 0), (last, 1, 0x00400000)]


# ---------------------------------------------------------------- output
def emit_c(cases, path):
    fills = []
    out = ["/* generated by tools/gen_blitter.py - do not edit */",
           '#include "blitter.h"', ""]
    rows = []
    for c in cases:
        c.fill0 = len(fills)
        fills.extend(c.fills)
        size, sizv, sizh = c.size_regs()
        apt = c.apt
        rows.append(f'    {{ "{c.id}", 0x{c.con0:04x}, 0x{c.con1:04x}, 0x{c.afwm:04x}, 0x{c.alwm:04x}, '
                    f'0x{c.adat:04x}, 0x{c.bdat:04x}, 0x{c.cdat:04x}, 0x{c.aux:04x}, '
                    f'{s16(c.amod)}, {s16(c.bmod)}, {s16(c.cmod)}, {s16(c.dmod)}, '
                    f'0x{apt & 0xffffffff:05x}, 0x{c.bpt:05x}, 0x{c.cpt:05x}, 0x{c.dpt:05x}, '
                    f'0x{size:04x}, 0x{sizv:04x}, 0x{sizh:04x}, 0x{c.flags:04x}, '
                    f'{c.fill0}, {len(c.fills)}, 0x{c.window[0]:05x}, {c.window[1]}, '
                    f'0x{c.crc:08x}UL, {c.zero}, {c.w}, {c.h} }},')
    out.append(f"const struct blt_fill blt_fills[{len(fills)}] = {{")
    for o, n, s in fills:
        out.append(f"    {{ 0x{o:05x}, {n}, 0x{s:08x}UL }},")
    out.append("};")
    out.append("")
    out.append(f"const struct blt_case blt_cases[{len(cases)}] = {{")
    out.extend(rows)
    out.append("};")
    out.append(f"const int blt_ncases = {len(cases)};")
    with open(path, "w") as f:
        f.write("\n".join(out) + "\n")


def dump(c):
    print(f"case {c.id} ({c.group}) {c.note}")
    size, sizv, sizh = c.size_regs()
    print(f"  BLTCON0={c.con0:04x} BLTCON1={c.con1:04x} AFWM={c.afwm:04x} ALWM={c.alwm:04x}"
          f" aux(BLTCON0L)={c.aux:04x}")
    print(f"  ADAT={c.adat:04x} BDAT={c.bdat:04x} CDAT={c.cdat:04x}")
    print(f"  MOD A={s16(c.amod)} B={s16(c.bmod)} C={s16(c.cmod)} D={s16(c.dmod)}")
    print(f"  PT (pool offsets; A is the error term in line mode) A={c.apt:#x} B={c.bpt:#x}"
          f" C={c.cpt:#x} D={c.dpt:#x}")
    print(f"  size {c.w} words x {c.h} rows: BLTSIZE={size:04x} SIZV={sizv:04x} SIZH={sizh:04x}"
          f" flags={c.flags:#x}")
    for o, n, s in c.fills:
        print(f"  fill {o:#07x} {n} words seed {s:08x}")
    print(f"  window {c.window[0]:#07x} +{c.window[1]}  crc {c.crc:08x}  zero {c.zero}")
    e = c.expect
    for i in range(0, len(e), 32):
        print(f"  {c.window[0] + i:05x}: " + " ".join(e[j:j + 2].hex() for j in range(i, min(i + 32, len(e)), 2)))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.join(ROOT, "disk", "modules", "blitter_tab.c"))
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--dump")
    ap.add_argument("--mutate", action="append", default=[],
                    help="self-test: aold_unmasked, fill_excl_as_incl, texture_up, sing_ignored")
    a = ap.parse_args()
    MUTATE.update(a.mutate)
    cases = build_cases()
    fixups(cases)
    for c in cases:
        c.evaluate()
    if a.list:
        for c in cases:
            print(f"{c.id:28s} {c.group:10s} crc={c.crc:08x} zero={c.zero}")
        return
    if a.dump:
        for c in cases:
            if c.id == a.dump:
                dump(c)
                return
        sys.exit(f"no case {a.dump}")
    emit_c(cases, a.out)
    print(f"{len(cases)} cases -> {a.out}")


if __name__ == "__main__":
    main()
