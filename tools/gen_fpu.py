#!/usr/bin/env python3
"""Generate disk/modules/fpu_gen.s: the fpu module's operation stubs, its
predicate stubs and its test-vector tables with exact expected results.

    python3 tools/gen_fpu.py [-o disk/modules/fpu_gen.s]

Everything is computed with integers / fractions.Fraction (and decimal for the
transcendental references); the host's float is never used for a value.

Reference rules (MC68881/MC68882 User's Manual, 2nd ed., sections 2-4 and the
per-instruction tables; MC68040 UM section 9; MC68060 UM section 6):
  * extended format: 1 sign, 15-bit exponent (bias 16383), 64-bit mantissa with
    an explicit integer bit; value = mant * 2^(exp-16383-63) for exp 0..7FFE
    (Motorola extended has Emin = -16383 at biased exponent 0); exp 7FFF:
    infinity if the fraction (bits 62-0) is 0, else NaN (bit 62 = 1: quiet);
  * results are rounded to the FPCR rounding precision (X 64 / S 24 / D 53
    mantissa bits; the exponent range stays extended for register results) in
    the FPCR mode (RN ties-to-even, RZ, RM, RP); memory results are rounded to
    the destination format and its exponent range;
  * FPSR: CC N Z I NAN (31-24), quotient (23-16), EXC BSUN SNAN OPERR OVFL UNFL
    DZ INEX2 INEX1 (15-8), AEXC IOP OVFL UNFL DZ INEX (7-0) with
    IOP|=SNAN|OPERR|BSUN, OVFL|=OVFL, UNFL|=UNFL&INEX2, DZ|=DZ,
    INEX|=INEX1|INEX2|OVFL;
  * OPERR default result: the default NaN $7FFF FFFFFFFF FFFFFFFF; a NaN
    operand propagates (quieted, bit 62 set); with two NaNs the destination's.
Where the manuals leave something open (tininess boundary, N on a compare of
zeros, sign of FNEG/FABS of a NaN, the integer value stored on an integer
overflow, the j bit of a converted NaN), the vector masks it out.
"""

import argparse
import decimal
import math
import os
from fractions import Fraction as Fr

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# FPU applicability bits (fpu.c: FM_881 ...)
M881, M882, M040, M060 = 1, 2, 4, 8
M6888X = M881 | M882
MALL = M6888X | M040 | M060
M4060 = M040 | M060

# FPSR bits
CC_N, CC_Z, CC_I, CC_NAN = 1 << 27, 1 << 26, 1 << 25, 1 << 24
BSUN, SNAN, OPERR, OVFL, UNFL, DZ, INEX2, INEX1 = [1 << b for b in range(15, 7, -1)]
A_IOP, A_OVFL, A_UNFL, A_DZ, A_INEX = 0x80, 0x40, 0x20, 0x10, 0x08

RN, RZ, RM, RP = 0, 1, 2, 3
PX, PS, PD = 0, 1, 2
MODE_NAMES = ["rn", "rz", "rm", "rp"]
PREC_NAMES = ["x", "s", "d"]
PREC_BITS = {PX: 64, PS: 24, PD: 53}


def fpcr(mode=RN, prec=PX, enables=0):
    return (enables << 8) | (prec << 6) | (mode << 4)


# ------------------------------------------------------------------ values
class V:
    """kind: 'z' zero, 'i' infinity, 'n' NaN, 'f' finite nonzero."""
    __slots__ = ("k", "s", "m", "nm")

    def __init__(self, k, s=0, m=None, nm=0):
        self.k, self.s, self.m, self.nm = k, s, m, nm   # m: Fraction (f); nm: NaN mantissa

    def neg(self):
        return V(self.k, self.s ^ 1, self.m, self.nm)

    def val(self):
        return -self.m if self.s else self.m

    def snan(self):
        return self.k == "n" and not (self.nm >> 62) & 1


def fin(x):
    """a finite operand; values that are not extended-representable are
    rounded to the nearest extended value first (RN), so 1/3 means X(1/3)"""
    x = Fr(x)
    if x == 0:
        return V("z", 0)
    v = V("f", 1 if x < 0 else 0, abs(x))
    e = ilog2(v.m)
    q = v.m / Fr(2) ** (max(e, -16383) - 63)
    if q.denominator != 1:
        v, _ = round_real(v.s, Exact(v.m), 64, -16383, 16383, 0)
    return v


def xval(high, low):
    """the extended value with sign/exponent word `high` and mantissa `low` (normal)"""
    assert low >> 63 and 0 < high & 0x7fff < 0x7fff
    v = Fr(low) * Fr(2) ** ((high & 0x7fff) - 16383 - 63)
    return -v if high & 0x8000 else v


ZERO, NZERO = V("z", 0), V("z", 1)
INF, NINF = V("i", 0), V("i", 1)
QNAN = V("n", 0, nm=0xC000000000000000)
NQNAN = V("n", 1, nm=0xC000000000000000)
SNANV = V("n", 0, nm=0xA000000000000000)       # j=1, bit 62 = 0, nonzero fraction
DNAN = V("n", 0, nm=0xFFFFFFFFFFFFFFFF)        # default NaN


def quiet(v):
    return V("n", v.s, nm=v.nm | (1 << 62))


def ilog2(x):
    """floor(log2(x)) for a positive Fraction."""
    n, d = x.numerator, x.denominator
    e = n.bit_length() - d.bit_length()
    if e >= 0:
        if n < (d << e):
            e -= 1
    else:
        if (n << -e) < d:
            e -= 1
    return e


# ------------------------------------------------------------------ rounding
class Exact:
    """an exact positive rational"""
    def __init__(self, x):
        self.x = Fr(x)

    def floor_scaled(self, s):          # floor(x * 2^s), exact?
        n, d = self.x.numerator, self.x.denominator
        if s >= 0:
            n <<= s
        else:
            d <<= -s
        return n // d, n % d == 0

    def log2(self):
        return ilog2(self.x)


class Sqrt:
    """sqrt of a positive rational"""
    def __init__(self, x):
        self.x = Fr(x)

    def floor_scaled(self, s):          # floor(sqrt(x) * 2^s)
        n, d = self.x.numerator, self.x.denominator
        if s >= 0:
            n <<= 2 * s
        else:
            d <<= -2 * s
        q, r = divmod(n, d)
        import math
        t = math.isqrt(q)
        return t, r == 0 and t * t == q

    def log2(self):
        return ilog2(self.x) // 2


class Approx:
    """a transcendental value known to ~256 bits: treated as never exact"""
    def __init__(self, x):
        self.x = Fr(x)

    def floor_scaled(self, s):
        q, _ = Exact(self.x).floor_scaled(s)
        return q, False

    def log2(self):
        return ilog2(self.x)




def round_real(sign, r, p, emin, emax, mode):
    """Round sign*r (r an Exact/Sqrt/Approx, positive) to p mantissa bits with
    exponents emin..emax (denormals below emin).  Returns (V, flags) with flags
    a set of 'inex', 'ovfl', 'unfl', 'tiny'."""
    e = r.log2()
    qe = max(e, emin) - (p - 1)
    t, ex = r.floor_scaled(-qe + 2)
    m, rb = t >> 2, t & 3
    inexact = rb != 0 or not ex
    half = rb >= 2
    rest = (rb & 1) or not ex
    up = False
    if mode == RN:
        up = half and (rest or (m & 1))
    elif mode == RP:
        up = inexact and not sign
    elif mode == RM:
        up = inexact and sign
    if up:
        m += 1
    flags = set()
    if inexact:
        flags.add("inex")
    if e < emin:
        flags.add("tiny")
        if inexact:
            flags.add("unfl")
    if m and ilog2(Fr(m) * Fr(2) ** qe) > emax:
        flags |= {"ovfl", "inex"}
        maxv = Fr((1 << p) - 1) * Fr(2) ** (emax - p + 1)
        to_inf = mode == RN or (mode == RP and not sign) or (mode == RM and sign)
        return (V("i", sign) if to_inf else V("f", sign, maxv)), flags
    if m == 0:
        return V("z", sign), flags
    return V("f", sign, Fr(m) * Fr(2) ** qe), flags


X_EMIN, X_EMAX = -16383, 16383
S_EMIN, S_EMAX = -126, 127
D_EMIN, D_EMAX = -1022, 1023


def round_reg(sign, r, mode, prec):
    """register result: FPCR precision mantissa, extended exponent range"""
    return round_real(sign, r, PREC_BITS[prec], X_EMIN, X_EMAX, mode)


# ------------------------------------------------------------------ encodings
def x_enc(v):
    """V -> (long0, long1, long2) extended memory format"""
    if v.k == "z":
        return (v.s << 31, 0, 0)
    if v.k == "i":
        return ((v.s << 31) | (0x7FFF << 16), 0, 0)
    if v.k == "n":
        return ((v.s << 31) | (0x7FFF << 16), v.nm >> 32, v.nm & 0xFFFFFFFF)
    e = ilog2(v.m)
    if e >= X_EMIN:
        be = e + 16383
        mant = v.m / Fr(2) ** (e - 63)
    else:
        be = 0
        mant = v.m / Fr(2) ** (X_EMIN - 63)
    assert mant.denominator == 1 and mant < 2 ** 64, (v.m, e)
    mant = int(mant)
    assert 0 <= be <= 0x7FFE
    return ((v.s << 31) | (be << 16), mant >> 32, mant & 0xFFFFFFFF)


def x_raw(sign, be, mant):
    return ((sign << 31) | (be << 16), mant >> 32, mant & 0xFFFFFFFF)


def x_dec(l0, l1, l2):
    s, be, mant = l0 >> 31, (l0 >> 16) & 0x7FFF, (l1 << 32) | l2
    if be == 0x7FFF:
        if mant & ((1 << 63) - 1) == 0:
            return V("i", s)
        return V("n", s, nm=mant)
    if mant == 0:
        return V("z", s)
    return V("f", s, Fr(mant) * Fr(2) ** (be - 16383 - 63))


def fmt_enc(v, ebits, fbits):
    """IEEE single/double encoding of a representable V (for S/D outputs)"""
    bias = (1 << (ebits - 1)) - 1
    emaxb = (1 << ebits) - 1
    if v.k == "z":
        return v.s << (ebits + fbits)
    if v.k == "i":
        return (v.s << (ebits + fbits)) | (emaxb << fbits)
    if v.k == "n":
        frac = (v.nm >> (63 - fbits)) & ((1 << fbits) - 1)
        return (v.s << (ebits + fbits)) | (emaxb << fbits) | frac
    e = ilog2(v.m)
    emin = 1 - bias
    if e >= emin:
        mant = v.m / Fr(2) ** (e - fbits)
        be = e + bias
        assert mant.denominator == 1 and (1 << fbits) <= mant < (2 << fbits)
        return (v.s << (ebits + fbits)) | (be << fbits) | (int(mant) - (1 << fbits))
    mant = v.m / Fr(2) ** (emin - fbits)
    assert mant.denominator == 1 and mant < (1 << fbits)
    return (v.s << (ebits + fbits)) | int(mant)


def fmt_dec(bits, ebits, fbits):
    bias = (1 << (ebits - 1)) - 1
    s = bits >> (ebits + fbits)
    be = (bits >> fbits) & ((1 << ebits) - 1)
    f = bits & ((1 << fbits) - 1)
    if be == (1 << ebits) - 1:
        if f == 0:
            return V("i", s)
        # X NaN: j=1, fraction left-justified (the j bit is masked in the checks)
        return V("n", s, nm=(1 << 63) | (f << (63 - fbits)))
    if be == 0:
        if f == 0:
            return V("z", s)
        return V("f", s, Fr(f) * Fr(2) ** (1 - bias - fbits))
    return V("f", s, Fr((1 << fbits) | f) * Fr(2) ** (be - bias - fbits))


def s_enc(v):
    return fmt_enc(v, 8, 23)


def d_enc(v):
    return fmt_enc(v, 11, 52)


# ------------------------------------------------------------------ packed decimal
def p_enc_digits(sm, se, exp, digits17, yy=0):
    """digits17: string of 17 decimal digits (integer digit + 16 fraction)"""
    assert len(digits17) == 17 and 0 <= exp <= 999
    e = "%03d" % exp
    l0 = (sm << 31) | (se << 30) | (yy << 28) | (int(e[0]) << 24) | (int(e[1]) << 20) | (int(e[2]) << 16) | int(digits17[0])
    frac = digits17[1:]
    l1 = int(frac[:8], 16)
    l2 = int(frac[8:], 16)
    return (l0, l1, l2)


def p_value(sm, se, exp, digits17):
    m = Fr(int(digits17)) / Fr(10) ** 16
    v = m * Fr(10) ** (-exp if se else exp)
    return -v if sm else v


def p_out(v, k, mode):
    """FMOVE.P out with a static k-factor k>0 of a finite V; returns
    (packed longs, inexact)"""
    if v.k == "z":
        return p_enc_digits(v.s, 0, 0, "0" * 17), False
    x = v.m
    # exponent E with 1 <= x/10^E < 10
    E = 0
    while x / Fr(10) ** E >= 10:
        E += 1
    while x / Fr(10) ** E < 1:
        E -= 1
    q = x / Fr(10) ** (E - k + 1)
    n = q.numerator // q.denominator
    rem = q - n
    inexact = rem != 0
    if inexact:
        if mode == RN:
            if rem > Fr(1, 2) or (rem == Fr(1, 2) and n & 1):
                n += 1
        elif mode == RP and not v.s:
            n += 1
        elif mode == RM and v.s:
            n += 1
    if n == 10 ** k:
        n //= 10
        E += 1
    digits = str(n).ljust(17, "0")
    return p_enc_digits(v.s, 1 if E < 0 else 0, abs(E), digits), inexact


# ------------------------------------------------------------------ FPSR
def cc_of(v):
    c = CC_N if v.s else 0
    if v.k == "z":
        c |= CC_Z
    elif v.k == "i":
        c |= CC_I
    elif v.k == "n":
        c |= CC_NAN
    return c


def aexc(exc):
    a = 0
    if exc & (BSUN | SNAN | OPERR):
        a |= A_IOP
    if exc & OVFL:
        a |= A_OVFL
    if (exc & UNFL) and (exc & INEX2):
        a |= A_UNFL
    if exc & DZ:
        a |= A_DZ
    if exc & (INEX1 | INEX2 | OVFL):
        a |= A_INEX
    return a


def exc_from_flags(flags, inex_bit=INEX2):
    e = 0
    if "inex" in flags:
        e |= inex_bit
    if "ovfl" in flags:
        e |= OVFL
    if "unfl" in flags:
        e |= UNFL
    return e


# ------------------------------------------------------------------ operations
class R:
    """operation result: register value (or memory longs), exception bits,
    quotient byte, masks of don't-care bits"""
    def __init__(self, v=None, exc=0, quot=0, mem=None, cc=None, fpsr_dc=0, res_dc=(0, 0, 0), v2=None):
        self.v, self.exc, self.quot, self.mem, self.cc = v, exc, quot, mem, cc
        self.fpsr_dc, self.res_dc, self.v2 = fpsr_dc, res_dc, v2


def nan_dyadic(src, dst):
    exc = SNAN if (src.snan() or dst.snan()) else 0
    return R(quiet(dst if dst.k == "n" else src), exc)


def operr():
    return R(DNAN, OPERR)


def rnd(sign, r, mode, prec):
    v, fl = round_reg(sign, r, mode, prec)
    exc = exc_from_flags(fl)
    dc = UNFL if ("tiny" in fl and "inex" not in fl) else 0
    if "ovfl" in fl:
        dc |= INEX2          # INEX2 with OVFL: not specified consistently, not checked
    return R(v, exc, fpsr_dc=dc)


def from_exact(x, mode, prec, zero_sign=0):
    x = Fr(x)
    if x == 0:
        return R(V("z", zero_sign))
    return rnd(1 if x < 0 else 0, Exact(abs(x)), mode, prec)


def op_add(src, dst, mode, prec, sub=False):
    if sub:
        src = src.neg() if src.k != "n" else src
    if src.k == "n" or dst.k == "n":
        return nan_dyadic(src, dst)
    if src.k == "i" and dst.k == "i":
        if src.s != dst.s:
            return operr()
        # FSUB of opposite infinities: dst - src = dst (an infinity of dst's
        # sign) on every FPU (MC68881/882 UM FSUB/FADD special-case tables,
        # 68040UM/68060UM). Checked strictly on the 6888x too: the earlier
        # relaxation there came from an emulator's behaviour (Amiberry
        # softfloat faddsub_swap_inf), not from the manual (CONTRACT 10 rule 1).
        return R(dst)
    if src.k == "i":
        return R(src)
    if dst.k == "i":
        return R(dst)
    if src.k == "z" and dst.k == "z":
        if src.s == dst.s:
            return R(V("z", src.s))
        return R(V("z", 1 if mode == RM else 0))
    x = (src.val() if src.k == "f" else 0) + (dst.val() if dst.k == "f" else 0)
    return from_exact(x, mode, prec, zero_sign=1 if mode == RM else 0)


def op_mul(src, dst, mode, prec):
    if src.k == "n" or dst.k == "n":
        return nan_dyadic(src, dst)
    s = src.s ^ dst.s
    if (src.k == "i" and dst.k == "z") or (src.k == "z" and dst.k == "i"):
        return operr()
    if src.k == "i" or dst.k == "i":
        return R(V("i", s))
    if src.k == "z" or dst.k == "z":
        return R(V("z", s))
    return rnd(s, Exact(src.m * dst.m), mode, prec)


def op_div(src, dst, mode, prec):          # dst / src
    if src.k == "n" or dst.k == "n":
        return nan_dyadic(src, dst)
    s = src.s ^ dst.s
    if (src.k == "z" and dst.k == "z") or (src.k == "i" and dst.k == "i"):
        return operr()
    if dst.k == "i":
        return R(V("i", s))
    if src.k == "i":
        return R(V("z", s))
    if src.k == "z":
        return R(V("i", s), DZ)
    if dst.k == "z":
        return R(V("z", s))
    return rnd(s, Exact(dst.m / src.m), mode, prec)


def op_sqrt(src, mode, prec):
    if src.k == "n":
        return R(quiet(src), SNAN if src.snan() else 0)
    if src.k == "z":
        return R(src)
    if src.s:
        return operr()
    if src.k == "i":
        return R(src)
    return rnd(0, Sqrt(src.m), mode, prec)


def op_move(src, mode, prec):
    if src.k == "n":
        return R(quiet(src), SNAN if src.snan() else 0)
    if src.k in "zi":
        return R(src)
    return rnd(src.s, Exact(src.m), mode, prec)


def op_abs(src, mode, prec):
    if src.k == "n":   # sign of the NaN result: not checked
        return R(quiet(src), SNAN if src.snan() else 0, fpsr_dc=CC_N, res_dc=(0x80000000, 0, 0))
    return op_move(V(src.k, 0, src.m), mode, prec)


def op_neg(src, mode, prec):
    if src.k == "n":
        return R(quiet(src), SNAN if src.snan() else 0, fpsr_dc=CC_N, res_dc=(0x80000000, 0, 0))
    return op_move(src.neg(), mode, prec)


def round_int(x, mode):
    """round a Fraction to an integer in the FPCR mode"""
    n = x.numerator // x.denominator          # floor
    rem = x - n
    if rem == 0:
        return n
    if mode == RZ:
        return n if x > 0 else n + 1
    if mode == RM:
        return n
    if mode == RP:
        return n + 1
    if rem > Fr(1, 2) or (rem == Fr(1, 2) and n & 1):
        return n + 1
    return n


def op_int(src, mode, prec, rz=False):
    if src.k == "n":
        return R(quiet(src), SNAN if src.snan() else 0)
    if src.k in "zi":
        return R(src)
    x = src.val()
    n = round_int(x, RZ if rz else mode)
    exc = INEX2 if n != x else 0
    if n == 0:
        return R(V("z", src.s), exc)
    r = rnd(1 if n < 0 else 0, Exact(abs(n)), mode, prec)
    r.exc |= exc
    return r


def op_getexp(src, mode, prec):
    if src.k == "n":
        return R(quiet(src), SNAN if src.snan() else 0)
    if src.k == "i":
        return operr()
    if src.k == "z":
        return R(src)
    return from_exact(ilog2(src.m), mode, prec)


def op_getman(src, mode, prec):
    if src.k == "n":
        return R(quiet(src), SNAN if src.snan() else 0)
    if src.k == "i":
        return operr()
    if src.k == "z":
        return R(src)
    m = src.m / Fr(2) ** ilog2(src.m)
    return rnd(src.s, Exact(m), mode, prec)


def op_scale(src, dst, mode, prec):
    if src.k == "n" or dst.k == "n":
        return nan_dyadic(src, dst)
    if src.k == "i":
        return operr()
    if dst.k in "zi":
        return R(dst)
    n = 0 if src.k == "z" else int(src.val())     # truncated toward zero
    return rnd(dst.s, Exact(dst.m * Fr(2) ** n), mode, prec)


def op_modrem(src, dst, mode, prec, rem):
    if src.k == "n" or dst.k == "n":
        r = nan_dyadic(src, dst)
        r.fpsr_dc |= 0x00FF0000
        return r
    if src.k == "z" or dst.k == "i":
        r = operr()
        r.fpsr_dc |= 0x00FF0000
        return r
    qs = src.s ^ dst.s
    if src.k == "i" or dst.k == "z":
        return R(dst, fpsr_dc=0x00800000)       # sign of a zero quotient: not checked
    q = dst.m / src.m
    n = round_int(q, RN) if rem else q.numerator // q.denominator
    r = dst.m - n * src.m
    quot = (qs << 7) | (n & 0x7F)
    if r == 0:
        return R(V("z", dst.s), quot=quot)
    res = rnd(dst.s ^ (1 if r < 0 else 0), Exact(abs(r)), mode, prec)
    res.quot = quot
    return res


def op_cmp(src, dst, mode, prec):
    """CC of dst - src; the result register is unchanged"""
    if src.k == "n" or dst.k == "n":
        return R(dst, SNAN if (src.snan() or dst.snan()) else 0, cc=CC_NAN, fpsr_dc=CC_N | CC_I)

    def key(v):
        if v.k == "i":
            return (2 if not v.s else -2, 0)
        if v.k == "z":
            return (0, 0)
        return (0, v.val())
    a, b = key(dst), key(src)
    if a == b:
        cc = CC_Z
        dc = CC_I
        if dst.k == "z":
            dc |= CC_N            # N on zero compares: not specified
        elif dst.k == "i" and dst.s:
            cc |= CC_N            # -Inf == -Inf: N and Z (equal finite: +0, Z only)
        return R(dst, cc=cc, fpsr_dc=dc)
    return R(dst, cc=CC_N if a < b else 0, fpsr_dc=CC_I)


def op_tst(src, mode, prec):
    return R(None, SNAN if src.snan() else 0, cc=cc_of(src))


def op_sgl(src, dst, mode, prec, div):
    if src.k == "f" and dst.k == "f":
        if div:
            x = dst.m / src.m
        else:
            # FSGLMUL discards both input mantissas' low bits before multiplying,
            # independently of FPCR rounding mode and without narrowing exponents.
            def trunc(m):
                unit = Fr(2) ** (ilog2(m) - 23)
                return (m // unit) * unit
            x = trunc(dst.m) * trunc(src.m)
        return rnd(src.s ^ dst.s, Exact(x), mode, PS)
    return (op_div if div else op_mul)(src, dst, mode, PX)


# transcendentals with decimal (precision 90 digits ~ 300 bits)
D = decimal.Decimal
CTX = decimal.Context(prec=90)


def d_pi():
    def atan_inv(n):
        x = D(1) / n
        x2 = x * x
        s, t, k = D(0), x, 1
        while True:
            term = t / k
            if abs(term) < D(10) ** -95:
                break
            s += term if (k // 2) % 2 == 0 else -term
            t *= x2
            k += 2
        return s
    with decimal.localcontext(CTX):
        return 16 * atan_inv(D(5)) - 4 * atan_inv(D(239))


def d_sincos(x):
    with decimal.localcontext(CTX):
        pi = d_pi()
        k = (x / (pi / 2)).to_integral_value(rounding=decimal.ROUND_HALF_EVEN)
        r = x - k * pi / 2
        s, c, t, n = D(0), D(0), D(1), 0
        while True:
            if abs(t) < D(10) ** -95 and n > 4:
                break
            if n % 2 == 0:
                c += t if (n // 2) % 2 == 0 else -t
            else:
                s += t if (n // 2) % 2 == 0 else -t
            n += 1
            t = t * r / n
        k = int(k) % 4
        return [(s, c), (c, -s), (-s, -c), (-c, s)][k]


def d_atan(x):
    with decimal.localcontext(CTX):
        neg = x < 0
        x = abs(x)
        halvings = 0
        while x > D("0.1"):
            x = x / (1 + (1 + x * x).sqrt())
            halvings += 1
        s, t, k, x2 = D(0), x, 1, x * x
        while abs(t / k) > D(10) ** -95:
            s += t / k if (k // 2) % 2 == 0 else -t / k
            t *= x2
            k += 2
        s *= 2 ** halvings
        return -s if neg else s


def to_frac(d):
    return Fr(d)


def trans(name, x):
    """returns a decimal of the exact mathematical function value at Fraction x"""
    with decimal.localcontext(CTX):
        dx = D(x.numerator) / D(x.denominator)
        if name == "fsin":
            return d_sincos(dx)[0]
        if name == "fcos":
            return d_sincos(dx)[1]
        if name == "ftan":
            s, c = d_sincos(dx)
            return s / c
        if name == "fetox":
            return dx.exp()
        if name == "ftwotox":
            return (dx * D(2).ln()).exp()
        if name == "ftentox":
            return (dx * D(10).ln()).exp()
        if name == "flogn":
            return dx.ln()
        if name == "flog2":
            return dx.ln() / D(2).ln()
        if name == "flog10":
            return dx.ln() / D(10).ln()
        if name == "fatan":
            return d_atan(dx)
        if name == "fsinh":
            return (dx.exp() - (-dx).exp()) / 2
        if name == "fcosh":
            return (dx.exp() + (-dx).exp()) / 2
        if name == "ftanh":
            a, b = dx.exp(), (-dx).exp()
            return (a - b) / (a + b)
        if name == "fasin":
            return d_atan(dx / (1 - dx * dx).sqrt())
        if name == "facos":
            return d_pi() / 2 - d_atan(dx / (1 - dx * dx).sqrt())
        if name == "fatanh":
            return ((1 + dx).ln() - (1 - dx).ln()) / 2
        if name == "fetoxm1":
            return dx.exp() - 1       # |x| >= 2^-70: 90 digits keep about 70 after the cancellation
        if name == "flognp1":
            return (1 + dx).ln()
    raise ValueError(name)


def op_trans(name, src, mode, prec):
    if src.k == "n":
        return R(quiet(src), SNAN if src.snan() else 0)
    if name in ("fasin", "facos", "fatanh", "fetoxm1", "flognp1"):
        r = op_trans_more(name, src, mode, prec)
        if r is not None:
            return r
    if src.k == "z":
        if name in ("fsin", "ftan", "fatan", "fsinh", "ftanh"):
            return R(src)
        if name in ("fcos", "fetox", "ftwotox", "ftentox", "fcosh"):
            return R(fin(1))
        if name in ("flogn", "flog2", "flog10"):
            return R(NINF, DZ)
    if src.k == "i":
        if name in ("fsin", "fcos", "ftan"):
            return operr()
        if name in ("fetox", "ftwotox", "ftentox"):
            return R(ZERO if src.s else INF)
        if name in ("flogn", "flog2", "flog10"):
            return operr() if src.s else R(INF)
        if name == "fatan":
            with decimal.localcontext(CTX):
                d = d_pi() / 2
            r = rnd(src.s, Approx(abs(to_frac(d))), mode, prec)
            return r
    if name in ("flogn", "flog2", "flog10") and src.s:
        return operr()
    x = src.val()
    if name in ("flogn", "flog2", "flog10") and x == 1:
        return R(ZERO, fpsr_dc=INEX2)     # exact +0; INEX2 not specified for it
    d = trans(name, x)
    f = to_frac(d)
    return rnd(1 if f < 0 else 0, Approx(abs(f)), mode, prec)


def op_trans_more(name, src, mode, prec):
    """special operands of FASIN, FACOS, FATANH, FETOXM1, FLOGNP1 (MC68881/882 UM
    section 4 per-instruction tables); None: an ordinary argument"""
    def approx(sign, d):
        return rnd(sign, Approx(abs(to_frac(d))), mode, prec)
    with decimal.localcontext(CTX):
        half_pi = d_pi() / 2
    if src.k == "z":
        if name == "facos":
            return approx(0, half_pi)               # acos(+-0) = pi/2
        return R(src)                               # asin, atanh, etoxm1, lognp1 of +-0: +-0
    if src.k == "i":
        if name == "fetoxm1":
            return R(fin(-1) if src.s else INF)     # e^-inf - 1 = -1, e^+inf - 1 = +inf
        if name == "flognp1" and not src.s:
            return R(INF)
        return operr()                              # asin, acos, atanh of +-inf; lognp1(-inf)
    x = src.val()
    if name in ("fasin", "facos"):
        if abs(x) > 1:
            return operr()
        if abs(x) == 1:
            if name == "fasin":
                return approx(src.s, half_pi)       # asin(+-1) = +-pi/2
            if src.s:
                with decimal.localcontext(CTX):
                    return approx(0, d_pi())        # acos(-1) = pi
            return R(ZERO, fpsr_dc=INEX2)           # acos(1) = +0 exact; INEX2 not specified for it
    if name == "fatanh":
        if abs(x) > 1:
            return operr()
        if abs(x) == 1:
            return R(NINF if src.s else INF, DZ)    # atanh(+-1) = +-inf, DZ
    if name == "flognp1":
        if x < -1:
            return operr()
        if x == -1:
            return R(NINF, DZ)                      # log(0) = -inf, DZ
    return None


def op_sincos(src, mode, prec):
    """FSINCOS fp1,fp2:fp0 -> sin in fp0, cos in fp2"""
    s = op_trans("fsin", src, mode, prec)
    c = op_trans("fcos", src, mode, prec)
    s.v2 = c.v
    s.exc |= c.exc
    return s


# ------------------------------------------------------------------ op table
# name, asm (stub body; a2 = source memory, a1 = output memory; fp1 = source,
# fp0 = destination/result, fp2 = second result), kind ('reg' result in fp0,
# 'mem' result written to (a1)), FPU mask
OPS = []
OPIDX = {}


def defop(name, asm, kind, mask):
    OPIDX[name] = len(OPS)
    OPS.append((name, asm, kind, mask))


for o in ("fadd", "fsub", "fmul", "fdiv", "fcmp", "fsglmul", "fsgldiv"):
    defop(o, [f"{o}.x\tfp1,fp0"], "reg", MALL)
for o in ("fmod", "frem", "fscale"):
    defop(o, [f"{o}.x\tfp1,fp0"], "reg", M6888X)
for o in ("fsadd", "fdadd", "fssub", "fdsub", "fsmul", "fdmul", "fsdiv", "fddiv"):
    defop(o, [f"{o}.x\tfp1,fp0"], "reg", M4060)
defop("fmove", ["fmove.x\tfp1,fp0"], "reg", MALL)
for o in ("fsqrt", "fabs", "fneg"):
    defop(o, [f"{o}.x\tfp1,fp0"], "reg", MALL)
for o in ("fsmove", "fdmove", "fssqrt", "fdsqrt", "fsabs", "fdabs", "fsneg", "fdneg"):
    defop(o, [f"{o}.x\tfp1,fp0"], "reg", M4060)
for o in ("fint", "fintrz"):
    defop(o, [f"{o}.x\tfp1,fp0"], "reg", M6888X | M060)
for o in ("fgetexp", "fgetman"):
    defop(o, [f"{o}.x\tfp1,fp0"], "reg", M6888X)
defop("ftst", ["ftst.x\tfp1"], "reg", MALL)
TRANS = ("fsin", "fcos", "ftan", "fetox", "ftwotox", "ftentox", "flogn", "flog2", "flog10",
         "fatan", "fsinh", "fcosh", "ftanh", "fasin", "facos", "fatanh", "fetoxm1", "flognp1")
for o in TRANS:
    defop(o, [f"{o}.x\tfp1,fp0"], "reg", M6888X)
defop("fsincos", ["fsincos.x\tfp1,fp2:fp0"], "reg", M6888X)
for sz in "bwlsdx":
    defop(f"fmove_{sz}_in", [f"fmove.{sz}\t(a2),fp0"], "reg", MALL)
defop("fmove_p_in", ["fmove.p\t(a2),fp0"], "reg", M6888X)
for sz in "bwlsdx":
    defop(f"fmove_{sz}_out", [f"fmove.{sz}\tfp0,(a1)"], "mem", MALL)
PKS = (1, 3, 5, 17)
for k in PKS:
    defop(f"fmove_p_out_k{k}", [f"fmove.p\tfp0,(a1){{#{k}}}"], "mem", M6888X)
defop("fmove_x_inout", ["fmove.x\t(a2),fp0", "fmove.x\tfp0,(a1)"], "mem", MALL)
CR = {0x00: "pi", 0x0B: "log10(2)", 0x0C: "e", 0x0D: "log2(e)", 0x0E: "log10(e)", 0x0F: "0.0",
      0x30: "ln(2)", 0x31: "ln(10)", 0x32: "1e0", 0x33: "1e1", 0x34: "1e2", 0x35: "1e4", 0x36: "1e8",
      0x37: "1e16", 0x38: "1e32", 0x39: "1e64", 0x3A: "1e128", 0x3B: "1e256", 0x3C: "1e512",
      0x3D: "1e1024", 0x3E: "1e2048", 0x3F: "1e4096"}
for off in CR:
    defop(f"fmovecr_{off:02x}", [f"fmovecr\t#${off:02x},fp0"], "reg", M6888X)


# ------------------------------------------------------------------ vectors
GROUPS = []     # (case id, [vector dicts])
CUR = None


def group(name):
    global CUR
    CUR = (name, [])
    GROUPS.append(CUR)


def desc_v(v):
    if v is None:
        return "-"
    if v.k == "z":
        return "-0" if v.s else "+0"
    if v.k == "i":
        return "-inf" if v.s else "+inf"
    if v.k == "n":
        return ("-" if v.s else "") + ("snan" if v.snan() else "qnan")
    x = v.val()
    if x.denominator == 1 and abs(x) < 10 ** 12:
        return str(x)
    e = ilog2(abs(x))
    m = abs(x) / Fr(2) ** e
    if m == 1:
        return f"{'-' if x < 0 else ''}2^{e}"
    if m.denominator < 2 ** 70 and x.denominator < 10 ** 6 and abs(x.numerator) < 10 ** 6:
        return f"{x.numerator}/{x.denominator}"
    return f"{'-' if x < 0 else ''}m*2^{e}"


def vec(op, mode=RN, prec=PX, src=None, dst=None, srcraw=None, dstraw=None, res=None,
        mem=None, exc=0, quot=0, cc=None, fpsr_dc=0, res_dc=(0, 0, 0), res2=None,
        mask=MALL, tol=0, note="", enables=0):
    name, asm, kind, omask = OPS[OPIDX[op]]
    mask &= omask
    if exc & (OVFL | UNFL) or fpsr_dc & UNFL:
        # 040/060 with no FPSP: overflow and underflow are nonmaskable (the
        # FPSP builds the default result), so these vectors are 6888x only;
        # fpu_asm.s tests the traps (exc_*_disabled_4060)
        mask &= M6888X
    if not mask:
        return
    s = srcraw if srcraw is not None else (x_enc(src) if src is not None else (0, 0, 0))
    d = dstraw if dstraw is not None else (x_enc(dst) if dst is not None else (0, 0, 0))
    if kind == "reg":
        rr = x_enc(res) if res is not None else d
    else:
        rr = tuple(mem) + (0,) * (3 - len(mem))
    r2 = x_enc(res2) if res2 is not None else (0, 0, 0)
    if cc is None:
        cc = cc_of(res) if (kind == "reg" and res is not None) else 0
    if op == "fmove_x_inout":
        # FMOVE.X in sets CC and EXC; the FMOVE.X out after it clears the
        # exception status byte again (it is cleared at the start of every
        # FPU arithmetic instruction) but keeps CC and the accrued byte
        v_in = x_dec(*s)
        cc = cc_of(quiet(v_in) if v_in.k == "n" else v_in)
        fpsr = cc | aexc(exc)
    else:
        fpsr = cc | ((quot & 0xFF) << 16) | exc | aexc(exc)
    dcmask = fpsr_dc
    if tol:
        dcmask |= INEX2                 # inexact results: INEX2 not checked
    if dcmask & (UNFL | INEX2):        # accrued bits that depend on don't-care bits
        dcmask |= A_UNFL | A_INEX
    rmask = tuple((~m) & 0xFFFFFFFF for m in res_dc)
    if not note:
        note = ""
    why = f"{op} src={desc_v(src) if srcraw is None else 'raw'} dst={desc_v(dst) if dstraw is None else 'raw'} {MODE_NAMES[mode]}.{PREC_NAMES[prec]}"
    if note:
        why += ": " + note
    CUR[1].append(dict(op=OPIDX[op], mask=mask, tol=tol, fpcr=fpcr(mode, prec, enables), src=s, dst=d,
                       res=rr, fpsr=fpsr, fpsr_mask=(~dcmask) & 0xFFFFFFFF, res2=r2, rmask=rmask, why=why))


def emit_reg(op, fn, src, dst=None, mode=RN, prec=PX, mask=MALL, note="", tol=0, enables=0):
    """run a reference op and add the vector"""
    if dst is None:
        r = fn(src, mode, prec)
    else:
        r = fn(src, dst, mode, prec)
    if dst is None:
        dst_init = fin(7) if op != "ftst" else None
    else:
        dst_init = dst
    res = r.v if r.v is not None else dst_init
    if op == "ftst":
        dst_init = fin(7)
        res = dst_init
    vec(op, mode, prec, src=src, dst=dst_init, res=res, exc=r.exc, quot=r.quot,
        cc=r.cc if r.cc is not None else cc_of(res), fpsr_dc=r.fpsr_dc, res_dc=r.res_dc,
        res2=r.v2, mask=mask, note=note, tol=tol, enables=enables)


MODES = (RN, RZ, RM, RP)
PRECS = (PX, PS, PD)
P2 = lambda e: Fr(2) ** e                      # noqa: E731
XMAX = (Fr(2 ** 64 - 1)) * P2(16383 - 63)
XMAXV = fin(XMAX)

# ---- conversions in -----------------------------------------------------------
group("conv_in_b")
for b in (0, 1, 0x7F, 0x80, 0xFF):
    x = b - 256 if b & 0x80 else b
    vec("fmove_b_in", srcraw=(b << 24, 0, 0), dst=fin(7), res=fin(x), note=f"byte {b:02x} sign-extended")
group("conv_in_w")
for w in (0, 1, 0x7FFF, 0x8000, 0xFFFF):
    x = w - 65536 if w & 0x8000 else w
    vec("fmove_w_in", srcraw=(w << 16, 0, 0), dst=fin(7), res=fin(x), note=f"word {w:04x}")
group("conv_in_l")
for l in (0, 1, 0x7FFFFFFF, 0x80000000, 0xFFFFFFFF, 0x01000001, 0x01000003, 0xFEFFFFFF):
    x = l - 2 ** 32 if l & 0x80000000 else l
    for mode in MODES:
        for prec in PRECS:
            if (mode, prec) != (RN, PX) and abs(x) < 2 ** 24:
                continue
            r = from_exact(x, mode, prec)
            vec("fmove_l_in", mode, prec, srcraw=(l, 0, 0), dst=fin(7), res=r.v, exc=r.exc,
                note=f"long {l:08x} rounded to the FPCR precision")
group("conv_in_s")
for bits in (0x00000000, 0x80000000, 0x3F800000, 0xBFC00000, 0x7F7FFFFF, 0x00800000, 0x7F800000, 0xFF800000):
    v = fmt_dec(bits, 8, 23)
    vec("fmove_s_in", srcraw=(bits, 0, 0), dst=fin(7), res=v, note=f"single {bits:08x}")
vec("fmove_s_in", srcraw=(0x7FC00000, 0, 0), dst=fin(7), res=V("n", 0, nm=0xC000000000000000),
    res_dc=(0, 0x80000000, 0), note="single QNaN -> extended NaN, fraction left-justified (j don't care)")
vec("fmove_s_in", srcraw=(0x7F800001, 0, 0), dst=fin(7), res=V("n", 0, nm=0xC000010000000000),
    exc=SNAN, res_dc=(0, 0x80000000, 0), note="single SNaN: SNAN set, result quieted")
for bits in (0x00000001, 0x807FFFFF):
    v = fmt_dec(bits, 8, 23)
    vec("fmove_s_in", srcraw=(bits, 0, 0), dst=fin(7), res=v, mask=M6888X,
        note=f"single denormal {bits:08x} converts exactly (6888x; 040/060 trap instead)")
group("conv_in_d")
for hi, lo in ((0, 0), (0x3FF00000, 0), (0xC0090000, 0), (0x7FEFFFFF, 0xFFFFFFFF), (0x00100000, 0),
               (0x7FF00000, 0), (0xFFF00000, 0), (0x3FB99999, 0x9999999A)):
    v = fmt_dec((hi << 32) | lo, 11, 52)
    for mode in MODES:
        for prec in PRECS:
            if (mode, prec) != (RN, PX) and hi != 0x3FB99999:
                continue
            r = op_move(v, mode, prec)
            vec("fmove_d_in", mode, prec, srcraw=(hi, lo, 0), dst=fin(7), res=r.v, exc=r.exc,
                note=f"double {hi:08x}{lo:08x}")
vec("fmove_d_in", srcraw=(0x7FF00000, 1, 0), dst=fin(7), res=V("n", 0, nm=0xC000000000000800),
    exc=SNAN, res_dc=(0, 0x80000000, 0), note="double SNaN: SNAN set, result quieted")
vec("fmove_d_in", srcraw=(0x00000000, 1, 0), dst=fin(7), res=fmt_dec(1, 11, 52), mask=M6888X,
    note="double denormal converts exactly (6888x)")
group("conv_in_x")
for v in (ZERO, NZERO, INF, NINF, fin(1), fin(-3), XMAXV, QNAN, NQNAN, DNAN):
    vec("fmove_x_in", src=None, srcraw=x_enc(v), dst=fin(7), res=v, note="extended " + desc_v(v))
vec("fmove_x_in", srcraw=x_enc(SNANV), dst=fin(7), res=quiet(SNANV), exc=SNAN,
    note="extended SNaN: SNAN set, result quieted (bit 62 set)")
vx = fin(1 + P2(-40) + P2(-63))
for mode in MODES:
    for prec in (PS, PD):
        r = op_move(vx, mode, prec)
        vec("fmove_x_in", mode, prec, srcraw=x_enc(vx), dst=fin(7), res=r.v, exc=r.exc,
            note="1+2^-40+2^-63 rounded to the FPCR precision")
vec("fmove_x_in", srcraw=x_raw(0, 0x3FFF, 0x4000000000000000), dst=fin(7), res=fin(Fr(1, 2)), mask=M6888X,
    note="unnormal $3FFF 40000000 00000000 is normalized to 0.5 (6888x)")
vec("fmove_x_in", srcraw=x_raw(0, 0, 1), dst=fin(7), res=x_dec(0, 0, 1), mask=M6888X, fpsr_dc=UNFL,
    note="extended denormal (6888x)")
vec("fmove_x_in", srcraw=x_raw(0, 0, 0), dst=fin(7), res=ZERO, note="+0")
group("conv_in_p")
for (sm, se, e, dg, val) in ((0, 0, 0, "1" + "0" * 16, 1), (1, 0, 1, "1" + "25" + "0" * 14, Fr(-25, 2)),
                             (0, 1, 1, "25" + "0" * 15, Fr(1, 4)), (0, 0, 10, "1" + "0" * 16, 10 ** 10),
                             (0, 0, 0, "0" * 17, 0), (1, 0, 0, "0" * 17, 0)):
    raw = p_enc_digits(sm, se, e, dg)
    res = fin(val) if val != 0 else V("z", sm)
    vec("fmove_p_in", srcraw=raw, dst=fin(7), res=res, note=f"packed {raw[0]:08x} {raw[1]:08x} {raw[2]:08x}")
raw = p_enc_digits(0, 1, 1, "1" + "0" * 16)
for mode in MODES:
    r = from_exact(Fr(1, 10), mode, PX)
    vec("fmove_p_in", mode, PX, srcraw=raw, dst=fin(7), res=r.v, exc=INEX1, fpsr_dc=INEX2,
        tol=1, note="packed 1E-1: inexact decimal conversion sets INEX1 (within 1 ulp)")
vec("fmove_p_in", srcraw=(0x7FFF0000, 0, 0), dst=fin(7), res=INF, note="packed +infinity")

# ---- conversions out -----------------------------------------------------------
INT_SRC = (Fr(5, 2), Fr(-5, 2), Fr(7, 2), Fr(-1, 2), Fr(1, 2), Fr(9999, 4000), Fr(-9999, 4000), Fr(100))


def int_out_group(sz, bits):
    lo, hi = -(1 << (bits - 1)), (1 << (bits - 1)) - 1
    group(f"conv_out_{sz}")
    for x in INT_SRC + (Fr(hi), Fr(lo)):
        for mode in MODES:
            n = round_int(x, mode)
            exc = INEX2 if n != x else 0
            word = n & ((1 << bits) - 1)
            mem = (word << (32 - bits),)
            vec(f"fmove_{sz}_out", mode, PX, dst=fin(x), mem=mem, exc=exc,
                note=f"rounded to integer in the FPCR mode = {n}")
    for v in (ZERO, NZERO):
        vec(f"fmove_{sz}_out", dst=v, mem=(0,), note="zero")
    for v, note in ((fin(hi + 1), "integer overflow"), (fin(lo - 1), "integer overflow"), (INF, "infinity"),
                    (QNAN, "NaN")):
        vec(f"fmove_{sz}_out", dst=v, mem=(0,), exc=OPERR, fpsr_dc=INEX2,
            res_dc=(0xFFFFFFFF, 0, 0), mask=M6888X, note=f"{note}: OPERR (stored value not checked)")


int_out_group("b", 8)
int_out_group("w", 16)
int_out_group("l", 32)


def float_out_group(sz, enc, ebits, fbits, emin, emax):
    group(f"conv_out_{sz}")
    p = fbits + 1
    srcs = [fin(1), fin(-2), fin(Fr(1, 3)), fin(Fr(-2, 3)), fin(1 + P2(-p)), fin(1 + P2(-p) + P2(-p - 5)),
            fin(1 + 3 * P2(-p)), fin(P2(emax + 1)), fin(-P2(emax + 1)), XMAXV,
            fin(P2(emin - 1)), fin(-P2(emin - p - 3)), fin(P2(emin) * 3 / 4 + P2(emin - p - 5)), fin(P2(-16000))]
    for v in srcs:
        for mode in MODES:
            r, fl = round_real(v.s, Exact(v.m), p, emin, emax, mode)
            exc = exc_from_flags(fl)
            m6 = M6888X if ("tiny" in fl) else MALL
            dc = UNFL if ("tiny" in fl and "inex" not in fl) else 0
            if "ovfl" in fl:
                dc |= INEX2
            bitsv = enc(r)
            mem = (bitsv,) if sz == "s" else (bitsv >> 32, bitsv & 0xFFFFFFFF)
            vec(f"fmove_{sz}_out", mode, PX, dst=v, mem=mem, exc=exc, mask=m6, fpsr_dc=dc,
                note="rounded to the destination format and range; CC unchanged")
    for v in (ZERO, NZERO, INF, NINF, QNAN, NQNAN):
        bitsv = enc(v)
        mem = (bitsv,) if sz == "s" else (bitsv >> 32, bitsv & 0xFFFFFFFF)
        vec(f"fmove_{sz}_out", dst=v, mem=mem, note="special value")
    q = quiet(SNANV)
    bitsv = enc(q)
    mem = (bitsv,) if sz == "s" else (bitsv >> 32, bitsv & 0xFFFFFFFF)
    vec(f"fmove_{sz}_out", dst=SNANV, mem=mem, exc=SNAN, note="SNaN register: SNAN set, quiet NaN stored")


float_out_group("s", s_enc, 8, 23, S_EMIN, S_EMAX)
float_out_group("d", d_enc, 11, 52, D_EMIN, D_EMAX)

# ---- exceptions enabled, none raised ---------------------------------------------
# Every FPU exception but BSUN enabled in the FPCR, operations that raise
# none: the results are the plain ones, nothing traps. An emulator may take
# a slower path whenever an exception is enabled (Amiberry's JIT hands such
# instructions to the interpreter handler); the vectors run in the hot loop
# as all of them do, so that path runs compiled.
group("exc_enabled_quiet")
QUIET = 0x7F                                   # SNAN OPERR OVFL UNFL DZ INEX2 INEX1
for op, fn, src, dst in (("fadd", op_add, fin(Fr(3, 2)), fin(Fr(9, 4))),
                         ("fsub", lambda s, d, m, p: op_add(s, d, m, p, sub=True), fin(Fr(3, 2)), fin(Fr(9, 4))),
                         ("fmul", op_mul, fin(Fr(3, 2)), fin(Fr(9, 4))),
                         ("fdiv", op_div, fin(Fr(3, 2)), fin(Fr(9, 4))),
                         ("fcmp", op_cmp, fin(Fr(3, 2)), fin(Fr(9, 4)))):
    emit_reg(op, fn, src, dst, enables=QUIET, note="exceptions enabled, none raised")
for op, fn, src in (("fsqrt", op_sqrt, fin(Fr(9, 4))), ("fmove", op_move, fin(Fr(3, 2))),
                    ("fabs", op_abs, fin(Fr(-3, 2))), ("fneg", op_neg, fin(Fr(3, 2))),
                    ("ftst", op_tst, fin(Fr(-3, 2)))):
    emit_reg(op, fn, src, None, enables=QUIET, note="exceptions enabled, none raised")
vec("fmove_l_in", srcraw=(0xFFFFFFF9, 0, 0), dst=fin(7), res=fin(-7), enables=QUIET, note="exceptions enabled")
vec("fmove_w_in", srcraw=(0x00070000, 0, 0), dst=fin(1), res=fin(7), enables=QUIET, note="exceptions enabled")
vec("fmove_b_in", srcraw=(0xF9000000, 0, 0), dst=fin(1), res=fin(-7), enables=QUIET, note="exceptions enabled")
vec("fmove_s_in", srcraw=(s_enc(fin(Fr(3, 2))), 0, 0), dst=fin(7), res=fin(Fr(3, 2)), enables=QUIET,
    note="exceptions enabled")
_d = d_enc(fin(Fr(-9, 4)))
vec("fmove_d_in", srcraw=(_d >> 32, _d & 0xFFFFFFFF, 0), dst=fin(7), res=fin(Fr(-9, 4)), enables=QUIET,
    note="exceptions enabled")
vec("fmove_x_in", srcraw=x_enc(fin(Fr(5, 8))), dst=fin(7), res=fin(Fr(5, 8)), enables=QUIET, note="exceptions enabled")
vec("fmove_l_out", dst=fin(-12), mem=(0xFFFFFFF4,), enables=QUIET, note="exceptions enabled, CC unchanged")
vec("fmove_s_out", dst=fin(Fr(3, 2)), mem=(s_enc(fin(Fr(3, 2))),), enables=QUIET, note="exceptions enabled")
_d = d_enc(fin(Fr(-9, 4)))
vec("fmove_d_out", dst=fin(Fr(-9, 4)), mem=(_d >> 32, _d & 0xFFFFFFFF), enables=QUIET, note="exceptions enabled")

group("conv_out_x")
for v in (ZERO, NZERO, INF, NINF, fin(1), fin(1 + P2(-60)), XMAXV, QNAN, DNAN):
    for prec in (PX, PS, PD):
        if prec != PX and v.k != "f":
            continue
        vec("fmove_x_out", RN, prec, dst=v, mem=x_enc(v), note="extended destination: not rounded to the FPCR precision")
vec("fmove_x_out", dst=SNANV, mem=x_enc(quiet(SNANV)), exc=SNAN, note="SNaN register: SNAN set, quiet NaN stored")

group("conv_out_p")
for x, k in ((Fr(1), 1), (Fr(1), 17), (Fr(25, 2), 3), (Fr(25, 2), 17), (Fr(-25, 2), 3), (Fr(1, 4), 5),
             (Fr(123456), 3), (Fr(123456), 17), (Fr(10) ** 10, 1)):
    for mode in MODES:
        if mode != RN and x.denominator == 1 and x < 1000:
            continue
        mem, inex = p_out(fin(x), k, mode)
        vec(f"fmove_p_out_k{k}", mode, PX, dst=fin(x), mem=mem, exc=INEX2 if inex else 0,
            note=f"packed out, k={k}")
v13 = from_exact(Fr(1, 3), RN, PX).v
for mode in MODES:
    mem, inex = p_out(v13, 5, mode)
    vec("fmove_p_out_k5", mode, PX, dst=v13, mem=mem, exc=INEX2, note="packed out of X(1/3), k=5")
vec("fmove_p_out_k3", dst=ZERO, mem=p_enc_digits(0, 0, 0, "0" * 17), note="packed out of +0")

# ---- special values, FMOVE.X in -> FMOVE.X out ---------------------------------
group("inf_roundtrip")
for v in (INF, NINF, QNAN, NQNAN, DNAN, V("n", 1, nm=0xFFFFFFFFFFFFFFFF)):
    vec("fmove_x_inout", srcraw=x_enc(v), dst=fin(7), mem=x_enc(v),
        note="FMOVE.X in then FMOVE.X out must keep every bit")
group("special_x_inout")
for v in (ZERO, NZERO, fin(1), XMAXV, fin(-XMAX), fin(P2(-16382)), fin(Fr(3, 2) * P2(-16382))):
    vec("fmove_x_inout", srcraw=x_enc(v), dst=fin(7), mem=x_enc(v), note="bit-exact round trip " + desc_v(v))
vec("fmove_x_inout", srcraw=x_enc(SNANV), dst=fin(7), mem=x_enc(quiet(SNANV)), exc=SNAN,
    note="SNaN in: SNAN set, quieted")
for raw, note in ((x_raw(0, 0, 1), "smallest denormal"), (x_raw(1, 0, 0x4000000000000000), "negative denormal"),
                  (x_raw(0, 0, 0x7FFFFFFFFFFFFFFF), "largest denormal")):
    vec("fmove_x_inout", srcraw=raw, dst=fin(7), mem=raw, mask=M6888X, fpsr_dc=UNFL,
        note=note + " survives FMOVE.X in/out (6888x)")
vec("fmove_x_inout", srcraw=x_raw(0, 0x3FFF, 0x4000000000000000), dst=fin(7), mem=x_raw(0, 0x3FFE, 1 << 63),
    mask=M6888X, note="unnormal is normalized on FMOVE.X in (6888x)")

# ---- FCMP / FTST -----------------------------------------------------------------
group("fcmp_classes")
CMPV = (ZERO, NZERO, fin(1), fin(-1), fin(2), INF, NINF, QNAN)
for a in CMPV:
    for b in CMPV:
        emit_reg("fcmp", op_cmp, b, a, note="CC of dst-src; register unchanged")
emit_reg("fcmp", op_cmp, SNANV, fin(1), note="SNaN operand sets SNAN")
group("ftst_classes")
for v in (ZERO, NZERO, fin(1), fin(-1), INF, NINF, QNAN, NQNAN, XMAXV, fin(P2(-16382))):
    emit_reg("ftst", op_tst, v, note="CC of the operand")

# ---- arithmetic ----------------------------------------------------------------------
SPECIALS = (ZERO, NZERO, INF, NINF, QNAN, SNANV, fin(1), fin(-3))


def dyadic_group(name, fn, extra, mask=MALL, specials=True):
    group(name)
    if specials:
        for a in SPECIALS:
            for b in SPECIALS:
                emit_reg(name[6:], fn, b, a, mask=mask)
    for (src, dst, modes, precs, note) in extra:
        for mode in modes:
            for prec in precs:
                emit_reg(name[6:], fn, src, dst, mode, prec, mask=mask, note=note)


ALLM, ALLP = MODES, PRECS
dyadic_group("arith_fadd", op_add, [
    (fin(P2(-64)), fin(1), ALLM, (PX,), "tie at half an X ulp -> even"),
    (fin(P2(-64)), fin(1 + P2(-63)), ALLM, (PX,), "tie, odd lsb -> up in RN"),
    (fin(P2(-24)), fin(1), ALLM, (PS,), "tie at half an S ulp"),
    (fin(P2(-53)), fin(1), ALLM, (PD,), "tie at half a D ulp"),
    (fin(3 * P2(-66)), fin(1), ALLM, (PX,), "below half"),
    (fin(-1), fin(1), ALLM, (PX,), "x + -x: +0, but -0 in RM"),
    (XMAXV, XMAXV, ALLM, (PX,), "overflow"),
    (fin(-XMAX), fin(-XMAX), ALLM, (PX,), "negative overflow"),
    (fin(Fr(1, 3)), fin(Fr(1, 3)), ALLM, ALLP, ""),
])
dyadic_group("arith_fsub", lambda s, d, m, p: op_add(s, d, m, p, sub=True), [
    (fin(P2(-65)), fin(1), ALLM, (PX,), "1-2^-65: tie on a mantissa of all ones"),
    (fin(1), fin(1), ALLM, (PX,), "x - x: +0, but -0 in RM"),
    (fin(-XMAX), XMAXV, ALLM, (PX,), "overflow"),
    (fin(Fr(2, 3)), fin(1), ALLM, ALLP, ""),
])
dyadic_group("arith_fmul", op_mul, [
    (fin(1 + P2(-32)), fin(1 + P2(-32)), ALLM, (PX,), "exact product 1+2^-31+2^-64 is an X tie"),
    (fin(1 + P2(-12)), fin(1 + P2(-12)), ALLM, (PS,), "S tie"),
    (fin(Fr(1, 3)), fin(3), ALLM, ALLP, "X(1/3)*3"),
    (fin(P2(16000)), fin(P2(16000)), ALLM, (PX,), "overflow"),
    (fin(-P2(16000)), fin(P2(16000)), ALLM, (PX,), "negative overflow"),
])
for mode in MODES:     # deep underflow (6888x): zero or the smallest denormal
    emit_reg("fmul", op_mul, fin(P2(-10000)), fin(P2(-10000)), mode, PX, mask=M6888X, note="deep underflow")
    emit_reg("fmul", op_mul, fin(-P2(-10000)), fin(P2(-10000)), mode, PX, mask=M6888X, note="deep underflow")
dyadic_group("arith_fdiv", op_div, [
    (fin(3), fin(1), ALLM, ALLP, "1/3"),
    (fin(3), fin(-1), ALLM, ALLP, "-1/3"),
    (fin(3), fin(2), ALLM, ALLP, "2/3"),
    (fin(10), fin(1), ALLM, ALLP, "1/10"),
    (fin(7), fin(-22), ALLM, (PX,), "-22/7"),
    (fin(P2(-16000)), fin(P2(16000)), ALLM, (PX,), "overflow"),
    (fin(4), fin(1), (RN,), (PX,), "exact"),
])


def monadic_group(name, fn, srcs, mask=MALL, modes=(RN,), precs=(PX,), tol=0):
    group(name)
    op = name.split("_", 1)[1]
    for src in srcs:
        for mode in modes:
            for prec in precs:
                emit_reg(op, fn, src, None, mode, prec, mask=mask, tol=tol)


MSPEC = (ZERO, NZERO, INF, NINF, QNAN, SNANV)
monadic_group("arith_fsqrt", op_sqrt, MSPEC + (fin(-1), fin(4), fin(Fr(9, 4))))
for mode in MODES:
    for prec in PRECS:
        for x in (2, 3, Fr(1, 2)):
            emit_reg("fsqrt", op_sqrt, fin(x), None, mode, prec, note="correctly rounded square root")
monadic_group("arith_fmove", op_move, MSPEC)
for mode in MODES:
    for prec in PRECS:
        for x in (1 + P2(-24) + P2(-60), -(1 + P2(-53) + P2(-60)), 1 + P2(-24), 1 + 3 * P2(-25), Fr(1, 3)):
            v = rnd(1 if x < 0 else 0, Exact(abs(x)), RN, PX).v
            emit_reg("fmove", op_move, v, None, mode, prec, note="FMOVE.X FPm,FP0 rounds to the FPCR precision")
monadic_group("arith_fabs", op_abs, MSPEC + (fin(-1), fin(5)))
emit_reg("fabs", op_abs, fin(-(1 + P2(-30))), None, RN, PS, note="FABS rounds to the FPCR precision")
monadic_group("arith_fneg", op_neg, MSPEC + (fin(-1), fin(5)))
emit_reg("fneg", op_neg, fin(1 + P2(-30)), None, RZ, PS, note="FNEG rounds to the FPCR precision")
INTS = (Fr(5, 2), Fr(7, 2), Fr(-5, 2), Fr(1, 2), Fr(-1, 2), Fr(27, 10), Fr(-27, 10), P2(70) + 1, Fr(3, 4))
monadic_group("arith_fint", op_int, MSPEC + tuple(fin(x) for x in INTS), mask=M6888X | M060, modes=MODES)
monadic_group("arith_fintrz", lambda s, m, p: op_int(s, m, p, rz=True), MSPEC + tuple(fin(x) for x in INTS),
              mask=M6888X | M060, modes=(RN, RP))
monadic_group("arith_fgetexp", op_getexp, MSPEC + (fin(8), fin(Fr(3, 4)), fin(-1), XMAXV, fin(P2(-16000))),
              mask=M6888X)
monadic_group("arith_fgetman", op_getman, MSPEC + (fin(12), fin(Fr(-3, 4)), XMAXV, fin(P2(-16000) * 3)),
              mask=M6888X)
dyadic_group("arith_fscale", op_scale, [
    (fin(3), fin(Fr(3, 2)), (RN,), (PX,), "1.5 * 2^3"),
    (fin(-1), fin(Fr(3, 2)), (RN,), (PX,), ""),
    (fin(Fr(27, 10)), fin(1), (RN,), (PX,), "scale factor truncated to 2"),
    (fin(Fr(-27, 10)), fin(1), (RN,), (PX,), "scale factor truncated to -2"),
    (fin(20000), fin(1), MODES, (PX,), "overflow"),
], mask=M6888X)


def modrem_group(name, rem):
    group(name)
    for a in SPECIALS:
        for b in SPECIALS:
            emit_reg(name[6:], lambda s, d, m, p: op_modrem(s, d, m, p, rem), b, a, mask=M6888X)
    for dst, src in ((7, 2), (-7, 2), (7, -2), (-7, -2), (5, 2), (4, 2), (-4, 2), (Fr(11, 2), Fr(3, 2)),
                     (P2(70), 3), (P2(70) + 1, 7), (1, 3), (2, 3), (Fr(3, 2), 1), (Fr(5, 2), 1)):
        emit_reg(name[6:], lambda s, d, m, p: op_modrem(s, d, m, p, rem), fin(src), fin(dst), mask=M6888X,
                 note="quotient byte = sign + low 7 bits of the quotient")


modrem_group("arith_fmod", False)
modrem_group("arith_frem", True)

group("arith_fsglmul")
for mode in MODES:
    for src, dst in ((1 + P2(-24), 1), (1, 1 + P2(-24)),
                     (-(1 + P2(-24)), 1), (1 + P2(-24), 1 + P2(-24))):
        for prec in PRECS:
            emit_reg("fsglmul", lambda s, d, m, p: op_sgl(s, d, m, p, False), fin(src), fin(dst), mode, prec,
                     note="both input mantissas truncated to 24 bits before multiplication")
    emit_reg("fsglmul", lambda s, d, m, p: op_sgl(s, d, m, p, False), fin(1 + P2(-23)), fin(1 + P2(-23)), mode, PX,
             note="product rounded to single precision, any FPCR precision")
    emit_reg("fsglmul", lambda s, d, m, p: op_sgl(s, d, m, p, False), fin(3), fin(Fr(1, 3) * 0 + 5), mode, PD)
    emit_reg("fsglmul", lambda s, d, m, p: op_sgl(s, d, m, p, False), fin(P2(200) * 3), fin(P2(200) * 5 + P2(180)),
             mode, PX, note="extended exponent range is kept")
group("arith_fsgldiv")
for mode in MODES:
    emit_reg("fsgldiv", lambda s, d, m, p: op_sgl(s, d, m, p, True), fin(3), fin(1), mode, PX,
             note="1/3 rounded to single precision")
    emit_reg("fsgldiv", lambda s, d, m, p: op_sgl(s, d, m, p, True), fin(3), fin(-2), mode, PD)
    emit_reg("fsgldiv", lambda s, d, m, p: op_sgl(s, d, m, p, True), fin(P2(-300) * 3), fin(P2(300)), mode, PX,
             note="extended exponent range is kept")
emit_reg("fsgldiv", lambda s, d, m, p: op_sgl(s, d, m, p, True), ZERO, fin(1), note="divide by zero")

group("arith_fsd_variants")     # 040/060 only
SD = (("fsadd", op_add, PS), ("fdadd", op_add, PD), ("fssub", lambda s, d, m, p: op_add(s, d, m, p, True), PS),
      ("fdsub", lambda s, d, m, p: op_add(s, d, m, p, True), PD), ("fsmul", op_mul, PS), ("fdmul", op_mul, PD),
      ("fsdiv", op_div, PS), ("fddiv", op_div, PD))
X13 = rnd(0, Exact(Fr(1, 3)), RN, PX).v
for op, fn, pr in SD:
    a, b = (fin(3), fin(1)) if "div" in op else (X13, fin(1))
    for mode in MODES:
        for fp in (PX, PD if pr == PS else PS):
            emit_reg(op, lambda s, d, m, p, fn=fn, pr=pr: fn(s, d, m, pr), a, b, mode, fp, mask=M4060,
                     note=f"rounded to {PREC_NAMES[pr]} whatever the FPCR precision")
    emit_reg(op, lambda s, d, m, p, fn=fn, pr=pr: fn(s, d, m, pr), INF, NINF, RN, PX, mask=M4060)
for op, fn, pr in (("fsmove", op_move, PS), ("fdmove", op_move, PD), ("fssqrt", op_sqrt, PS),
                   ("fdsqrt", op_sqrt, PD), ("fsabs", op_abs, PS), ("fdabs", op_abs, PD),
                   ("fsneg", op_neg, PS), ("fdneg", op_neg, PD)):
    for mode in MODES:
        x = rnd(0, Sqrt(Fr(2)), RN, PX).v if "sqrt" not in op else fin(2)
        if "neg" in op or "abs" in op:
            x = x.neg()
        emit_reg(op, lambda s, m, p, fn=fn, pr=pr: fn(s, m, pr), x, None, mode, PX, mask=M4060,
                 note=f"rounded to {PREC_NAMES[pr]}")

# ---- transcendentals (6888x) ---------------------------------------------------------------
# Arguments for the tolerance vectors. The MC68881/882 UM gives no accuracy
# bound in ulps for the transcendentals, so TOL is not a manual figure. It is
# applied only to well-conditioned arguments: relative condition number
# kappa = |x f'(x) / f(x)| <= KAPPA_MAX (checked below). An error of e (relative)
# in the argument, e.g. from reducing x against an approximate pi/2 or ln 2
# (|k| * error of the constant, about kappa-scaled), then moves the result by
# about kappa*e, so an implementation that is accurate to a fraction of an
# extended ulp internally stays within TOL. Badly conditioned arguments
# (ftan(-1.5): kappa 21; fcos(-1.5): 21; fsin(10): 15; fetox(-20): 20, ...)
# were replaced: there an accurate 6888x may legitimately miss TOL.
TARGS = {
    "fsin": (Fr(1, 2), Fr(1), Fr(-3, 2), Fr(11), Fr(1, 1000)),
    "fcos": (Fr(1, 2), Fr(1), Fr(-3), Fr(19, 2)),
    "ftan": (Fr(1, 2), Fr(1), Fr(-3, 4)),
    "fetox": (Fr(1), Fr(-1), Fr(1, 2), Fr(3), Fr(-2)),
    "ftwotox": (Fr(1, 2), Fr(-3, 2), Fr(3)),
    "ftentox": (Fr(1, 2), Fr(1)),
    "flogn": (Fr(2), Fr(10), Fr(1, 2), Fr(1000)),
    "flog2": (Fr(3), Fr(1, 3), Fr(1024)),
    "flog10": (Fr(2), Fr(1000), Fr(1, 7)),
    "fatan": (Fr(1, 2), Fr(1), Fr(-2), Fr(100)),
    "fsinh": (Fr(1, 2), Fr(-2)),
    "fcosh": (Fr(1, 2), Fr(2)),
    "ftanh": (Fr(1, 2), Fr(-1)),
    # the last of each: well-conditioned arguments where softfloat's FPSP
    # algorithms are 3-6 ulp off (round to nearest, extended)
    "fasin": (Fr(1, 2), Fr(-1, 4), Fr(9, 10), xval(0xbffd, 0xf10fbd9574635182)),
    "facos": (Fr(1, 2), Fr(-3, 4), Fr(1, 100), xval(0x3ffe, 0x8e9f4554a24fdc05)),
    "fatanh": (Fr(1, 2), Fr(-1, 10), Fr(3, 4), xval(0xbffa, 0xb6062affd80ab526), xval(0xbffb, 0xa2084c694feb58be)),
    "fetoxm1": (Fr(1, 2), Fr(-1), Fr(2), Fr(1, 1000), Fr(-30)),
    "flognp1": (Fr(1, 2), Fr(-1, 2), Fr(10), Fr(1, 1000), xval(0xbffc, 0x9f6919d6d28876fd)),
}
TSPEC = {
    "fsin": (ZERO, NZERO, INF, QNAN), "fcos": (ZERO, INF), "ftan": (ZERO, NZERO, NINF),
    "fetox": (ZERO, INF, NINF, QNAN), "ftwotox": (ZERO, NINF), "ftentox": (ZERO, INF),
    "flogn": (ZERO, NZERO, INF, NINF, fin(-1), fin(1)), "flog2": (ZERO, fin(1)), "flog10": (fin(1), fin(-2)),
    "fatan": (ZERO, NZERO, INF, NINF), "fsinh": (ZERO,), "fcosh": (ZERO,), "ftanh": (NZERO,),
    "fasin": (ZERO, NZERO, fin(1), fin(-1), fin(2), INF), "facos": (ZERO, fin(1), fin(-1), fin(-2), NINF),
    "fatanh": (ZERO, NZERO, fin(1), fin(-1), fin(2), INF), "fetoxm1": (ZERO, NZERO, INF, NINF),
    "flognp1": (ZERO, NZERO, fin(-1), fin(-2), INF, NINF),
}
TOL = 2
KAPPA_MAX = 3
_KAPPA = {
    "fsin": lambda x: x * math.cos(x) / math.sin(x), "fcos": lambda x: -x * math.sin(x) / math.cos(x),
    "ftan": lambda x: x / (math.sin(x) * math.cos(x)), "fetox": lambda x: x, "ftwotox": lambda x: x * math.log(2),
    "ftentox": lambda x: x * math.log(10), "flogn": lambda x: 1 / math.log(x), "flog2": lambda x: 1 / math.log(x),
    "flog10": lambda x: 1 / math.log(x), "fatan": lambda x: x / ((1 + x * x) * math.atan(x)),
    "fsinh": lambda x: x / math.tanh(x), "fcosh": lambda x: x * math.tanh(x),
    "ftanh": lambda x: x / (math.sinh(x) * math.cosh(x)),
    "fasin": lambda x: x / (math.sqrt(1 - x * x) * math.asin(x)),
    "facos": lambda x: -x / (math.sqrt(1 - x * x) * math.acos(x)),
    "fatanh": lambda x: x / ((1 - x * x) * math.atanh(x)), "fetoxm1": lambda x: x * math.exp(x) / math.expm1(x),
    "flognp1": lambda x: x / ((1 + x) * math.log1p(x)),
}
# the other rounding modes and precisions, at the first argument of each:
# RZ/RM/RP extended, and RN single and double (the result must be rounded to
# that precision; the tolerance is then in its ulps)
TMODES = ((RZ, PX), (RM, PX), (RP, PX), (RN, PS), (RN, PD))


def kappa_ok(name, x):
    """host-side guard (floats are fine here: it only selects arguments)"""
    return abs(_KAPPA[name](float(x))) <= KAPPA_MAX


for name in TRANS:
    group(f"trans_{name}")
    for x in TARGS[name]:
        assert kappa_ok(name, x), (name, x, "badly conditioned argument for a TOL vector")
        emit_reg(name, lambda s, m, p, n=name: op_trans(n, s, m, p), fin(x), None, RN, PX, mask=M6888X, tol=TOL,
                 note=f"within {TOL} ulp of the exact value")
    for mode, prec in TMODES:
        emit_reg(name, lambda s, m, p, n=name: op_trans(n, s, m, p), fin(TARGS[name][0]), None, mode, prec,
                 mask=M6888X, tol=TOL, note=f"within {TOL} ulp ({PREC_NAMES[prec]}) of the exact value")
    for v in TSPEC[name]:
        r = op_trans(name, v, RN, PX)
        exact_res = r.v.k != "f" or v.k == "z" or (name.startswith("flog") and v.k == "f" and v.m == 1)
        emit_reg(name, lambda s, m, p, n=name: op_trans(n, s, m, p), v, None, RN, PX, mask=M6888X,
                 tol=0 if exact_res else TOL, note="special operand")
group("trans_fsincos")
for x in (Fr(1, 2), Fr(-1), Fr(-12, 5)):
    assert kappa_ok("fsin", x) and kappa_ok("fcos", x), (x, "badly conditioned fsincos argument")
    emit_reg("fsincos", op_sincos, fin(x), None, RN, PX, mask=M6888X, tol=TOL, note="sin in FP0, cos in FP2")
for mode, prec in TMODES:
    emit_reg("fsincos", op_sincos, fin(Fr(1, 2)), None, mode, prec, mask=M6888X, tol=TOL,
             note=f"sin in FP0, cos in FP2, within {TOL} ulp ({PREC_NAMES[prec]})")
emit_reg("fsincos", op_sincos, ZERO, None, RN, PX, mask=M6888X, note="sin(+0)=+0, cos(+0)=1")
emit_reg("fsincos", op_sincos, INF, None, RN, PX, mask=M6888X, note="infinity: OPERR, both NaN")

group("trans_fmovecr")
with decimal.localcontext(CTX):
    crv = {0x00: d_pi(), 0x0B: D(2).log10(), 0x0C: D(1).exp(), 0x0D: 1 / D(2).ln(), 0x0E: D(1).exp().log10(),
           0x30: D(2).ln(), 0x31: D(10).ln()}
for off in CR:
    if off == 0x0F:
        vec(f"fmovecr_{off:02x}", dst=fin(7), res=ZERO, mask=M6888X, note="constant 0.0")
        continue
    if off in crv:
        val, exact = Approx(to_frac(crv[off])), False
    else:
        n = {0x32: 0, 0x33: 1, 0x34: 2, 0x35: 4, 0x36: 8, 0x37: 16, 0x38: 32, 0x39: 64, 0x3A: 128, 0x3B: 256,
             0x3C: 512, 0x3D: 1024, 0x3E: 2048, 0x3F: 4096}[off]
        val, exact = Exact(Fr(10) ** n), True
    r = rnd(0, val, RN, PX)
    exact = exact and not r.exc
    vec(f"fmovecr_{off:02x}", dst=fin(7), res=r.v, exc=r.exc, fpsr_dc=0 if exact else INEX2, mask=M6888X,
        tol=0 if exact else 1, note=f"ROM constant {CR[off]} (RN, X)")
for mode in MODES:
    r = rnd(0, Approx(to_frac(crv[0])), mode, PX)
    vec("fmovecr_00", mode, PX, dst=fin(7), res=r.v, exc=r.exc, mask=M6888X, note="pi rounded in the FPCR mode")
for prec in (PS, PD):
    r = rnd(0, Approx(to_frac(crv[0])), RN, prec)
    vec("fmovecr_00", RN, prec, dst=fin(7), res=r.v, exc=r.exc, mask=M6888X, note="pi rounded to the FPCR precision")


# ------------------------------------------------------------------ predicates
def pred_true(p, n, z, nan):
    p &= 15
    return [False, z, not (nan or z or n), z or not (nan or n), n and not (nan or z), z or (n and not nan),
            not (nan or z), not nan, nan, nan or z, nan or not (n or z), nan or z or not n, nan or (n and not z),
            nan or z or n, not z, True][p]


# ------------------------------------------------------------------ output
def asm_str(s):
    s = s.replace('"', "'")
    return '\tdc.b\t"' + s + '",0'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("-o", default=os.path.join(ROOT, "disk", "modules", "fpu_gen.s"))
    a = ap.parse_args()
    out = []
    w = out.append
    w("; GENERATED by tools/gen_fpu.py - do not edit")
    w("; fpu module: operation stubs, predicate stubs, test vectors")
    w("\tmachine\t68040")
    w("\tfpu\t1")
    w("\txdef\t_fpu_optab\n\txdef\t_fpu_opnames\n\txdef\t_fpu_groups\n\txdef\t_fpu_n_groups")
    w("\txdef\t_fpu_predtab\n\txdef\t_fpu_pred_truth\n\txdef\t_fpu_trapcnt")
    w("\tsection\t.text,code")
    for i, (name, asm, kind, mask) in enumerate(OPS):
        w(f"op_{i}:\t; {name}")
        for line in asm:
            w("\t" + line)
        w(f"\tmoveq\t#{1 if kind == 'reg' else 0},d1")
        w("\trts")
    # predicate stubs: d0 = FPSR in; out d0 = result, d1 = FPSR after
    # (families 4/5: FScc/FBcc with the integer CCR checked, see below)
    for fam in range(4):
        for p in range(32):
            lab = f"pr_{fam}_{p}"
            w(f"{lab}:")
            w("\tfmove.l\td0,fpsr")
            if fam == 0:      # FBcc.W
                w("\tmoveq\t#0,d0")
                w(f"\tdc.w\t$f280+{p}")
                w(f"\tdc.w\t{lab}_t-*")
                w(f"\tbra.s\t{lab}_d")
                w(f"{lab}_t:\tmoveq\t#1,d0")
                w(f"{lab}_d:\tfmove.l\tfpsr,d1")
                w("\trts")
            elif fam == 1:    # FScc D0: only $FF (1) or $00 (0) in the low byte, upper bytes kept; else 2
                w("\tmove.l\t#$a5a5a500,d0")
                w(f"\tdc.w\t$f240,{p}")
                w("\tfmove.l\tfpsr,d1")
                w("\tcmp.l\t#$a5a5a5ff,d0")
                w(f"\tbeq.s\t{lab}_t")
                w("\tcmp.l\t#$a5a5a500,d0")
                w(f"\tbeq.s\t{lab}_f")
                w("\tmoveq\t#2,d0")
                w("\trts")
                w(f"{lab}_t:\tmoveq\t#1,d0")
                w("\trts")
                w(f"{lab}_f:\tmoveq\t#0,d0")
                w("\trts")
            elif fam == 2:    # FDBcc D0 (count 1): fall through = condition true
                w("\tmoveq\t#1,d0")
                w(f"\tdc.w\t$f248,{p}")
                w(f"\tdc.w\t{lab}_b-*")
                w("\tfmove.l\tfpsr,d1")
                w("\tcmp.w\t#1,d0")
                w(f"\tbne.s\t{lab}_x")
                w("\tmoveq\t#1,d0")
                w("\trts")
                w(f"{lab}_b:\tfmove.l\tfpsr,d1")
                w("\ttst.w\td0")
                w(f"\tbne.s\t{lab}_x")
                w("\tmoveq\t#0,d0")
                w("\trts")
                w(f"{lab}_x:\tmoveq\t#2,d0")
                w("\trts")
            else:             # FTRAPcc (no operand): vector 7 counts
                w("\tclr.l\t_fpu_trapcnt")
                w(f"\tdc.w\t$f27c,{p}")
                w("\tfmove.l\tfpsr,d1")
                w("\tmove.l\t_fpu_trapcnt,d0")
                w("\trts")
    # CCR stubs (pred_ccr): FScc D2 (family 4) and FBcc.W (family 5) do not
    # change the integer CCR. Each runs twice, with CCR $15 (X Z C) and $0A
    # (N V) before it; MOVE CCR,Dn ($42c0+n, 68010+) right after it, before
    # anything else can touch the flags (FBcc: on the taken and on the
    # untaken path, i.e. after the end of a JIT block). Out d0 =
    # CCR1 << 8 | CCR2, $150a when kept; d1 = FPSR after.
    for fam in (4, 5):
        for p in range(32):
            lab = f"pr_{fam}_{p}"
            w(f"{lab}:")
            w("\tfmove.l\td0,fpsr")
            w("\tmoveq\t#0,d0")
            w("\tmoveq\t#0,d2")
            for n, (ccr, reg) in enumerate(((0x15, 0), (0x0A, 2))):
                w(f"\tmove.w\t#${ccr:02x},ccr")
                if fam == 4:
                    w(f"\tdc.w\t$f242,{p}\t\t; FScc D2")
                    w(f"\tdc.w\t$42c{reg}\t\t; move ccr,d{reg}")
                else:
                    w(f"\tdc.w\t$f280+{p}\t\t; FBcc.W")
                    w(f"\tdc.w\t{lab}_t{n}-*")
                    w(f"\tdc.w\t$42c{reg}\t\t; untaken: move ccr,d{reg}")
                    w(f"\tbra.s\t{lab}_n{n}")
                    w(f"{lab}_t{n}:\tdc.w\t$42c{reg}\t; taken: move ccr,d{reg}")
                    w(f"{lab}_n{n}:")
            w("\tfmove.l\tfpsr,d1")
            w("\tlsl.w\t#8,d0")
            w("\tor.w\td2,d0")
            w("\trts")
    w("\tsection\t.data,data")
    w("_fpu_optab:")
    for i in range(len(OPS)):
        w(f"\tdc.l\top_{i}")
    w("_fpu_opnames:")
    for i in range(len(OPS)):
        w(f"\tdc.l\topn_{i}")
    w("_fpu_predtab:")
    for fam in range(6):
        for p in range(32):
            w(f"\tdc.l\tpr_{fam}_{p}")
    w("_fpu_pred_truth:")
    for p in range(32):
        m = 0
        for i in range(16):        # i = FPSR CC nibble N Z I NAN
            n, z, nan = (i >> 3) & 1, (i >> 2) & 1, i & 1
            if pred_true(p, n, z, nan):
                m |= 1 << i
        w(f"\tdc.w\t${m:04x}")
    w("_fpu_trapcnt:\tdc.l\t0")
    # vectors: 80 bytes each
    w("_fpu_n_groups:\tdc.l\t%d" % len(GROUPS))
    w("_fpu_groups:")
    for gi, (gname, vecs) in enumerate(GROUPS):
        m = 0
        for v in vecs:
            m |= v["mask"]
        w(f"\tdc.l\tgn_{gi},gv_{gi},{len(vecs)},{m}")
    nvec = 0
    for gi, (gname, vecs) in enumerate(GROUPS):
        w("\tcnop\t0,4")
        w(f"gv_{gi}:")
        for vi, v in enumerate(vecs):
            nvec += 1
            w(f"\tdc.w\t{v['op']},{v['mask'] | (v['tol'] << 8)}")
            w(f"\tdc.l\t${v['fpcr']:08x}")
            w("\tdc.l\t" + ",".join(f"${x:08x}" for x in v["src"]))
            w("\tdc.l\t" + ",".join(f"${x:08x}" for x in v["dst"]))
            w("\tdc.l\t" + ",".join(f"${x:08x}" for x in v["res"]))
            w(f"\tdc.l\t${v['fpsr']:08x},${v['fpsr_mask']:08x}")
            w("\tdc.l\t" + ",".join(f"${x:08x}" for x in v["res2"]))
            w("\tdc.l\t" + ",".join(f"${x:08x}" for x in v["rmask"]))
            w(f"\tdc.l\twy_{gi}_{vi}")
    for gi, (gname, vecs) in enumerate(GROUPS):
        w(f"gn_{gi}:" + asm_str(gname)[0:])
        for vi, v in enumerate(vecs):
            w(f"wy_{gi}_{vi}:" + asm_str(v["why"]))
    for i, (name, asm, kind, mask) in enumerate(OPS):
        w(f"opn_{i}:" + asm_str(name))
    with open(a.o, "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"{a.o}: {len(OPS)} ops, {len(GROUPS)} groups, {nvec} vectors")


if __name__ == "__main__":
    main()
