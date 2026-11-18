/* SPDX-License-Identifier: 0BSD
 *
 * IEEE 754 single precision in software, for the kernel sets that run
 * without an FPU (k000 on every CPU, k020s on an 020/030 without an FPU and
 * on an LC/EC 040/060). libnix's soft float calls mathieeesingbas.library,
 * which the bench can't: it runs after the takeover, and must not depend on
 * the OS. These are the libgcc entry points GCC calls, correctly rounded to
 * nearest-even, with subnormals, as SSE and the 6888x in single precision
 * do, so a float kernel gives the same checksum with and without an FPU.
 *
 * No 64-bit arithmetic: the 24x24-bit product is built from 16-bit
 * multiplies and the quotient by long division, so nothing here calls
 * further libgcc helpers. NaN results are quiet, with the first NaN
 * operand's payload (SSE's rule); the kernels never produce one.
 *
 * Mantissas are kept with three extra bits below the last place (guard,
 * round, sticky): a normal value's leading 1 is at bit 26.
 */
typedef unsigned int u32;
typedef int s32;

#ifdef SOFTFLOAT_TEST
#define SF(name) sf_##name
#else
#define SF(name) __##name
#endif

#define SIGN 0x80000000u
#define EXP(x) (((x) >> 23) & 0xff)
#define MANT(x) ((x) & 0x7fffffu)
#define QNAN 0x7fc00000u

static int is_nan(u32 x) { return (x & 0x7fffffffu) > 0x7f800000u; }

/* shift right by n, folding the bits shifted out into bit 0 */
static u32 shr_sticky(u32 m, s32 n)
{
    if (n <= 0)
        return m;
    if (n >= 32)
        return m != 0;
    return (m >> n) | ((m & ((1u << n) - 1)) != 0);
}

/* m has its leading 1 at bit 26 (or lower only for a subnormal with e == 1)
 * and three extra bits; e is the biased exponent, possibly <= 0 */
static u32 round_pack(u32 sign, s32 e, u32 m)
{
    u32 low;
    if (e <= 0) {
        m = shr_sticky(m, 1 - e);
        e = 1;
    }
    low = m & 7;
    m >>= 3;
    if (low > 4 || (low == 4 && (m & 1)))
        m++;
    if (m >= (1u << 24)) {
        m >>= 1;
        e++;
    }
    if (e >= 255)
        return sign | 0x7f800000u;
    return sign | ((m >> 23) ? (u32)e << 23 : 0) | MANT(m);
}

/* exponent and 24-bit mantissa of a finite nonzero value, normalized so the
 * leading 1 is at bit 23 (subnormals get an exponent below 1) */
static u32 unpack(u32 x, s32 *e)
{
    u32 m = MANT(x);
    s32 ex = EXP(x);
    if (ex) {
        *e = ex;
        return m | (1u << 23);
    }
    ex = 1;
    while (!(m & (1u << 23))) {
        m <<= 1;
        ex--;
    }
    *e = ex;
    return m;
}

static u32 nan2(u32 a, u32 b) { return (is_nan(a) ? a : b) | 0x400000u; }

static u32 add_bits(u32 a, u32 b)
{
    u32 sa, sb, ma, mb, m, t;
    s32 ea, eb, d;

    if (is_nan(a) || is_nan(b))
        return nan2(a, b);
    if ((a & 0x7fffffffu) < (b & 0x7fffffffu)) {
        t = a;
        a = b;
        b = t;
    }
    sa = a & SIGN;
    sb = b & SIGN;
    if (EXP(a) == 255) {
        if (EXP(b) == 255 && sa != sb)
            return 0xffc00000u;      /* inf - inf: SSE's default NaN */
        return a;
    }
    if (!(b & 0x7fffffffu))
        return (a & 0x7fffffffu) ? a : (sa & sb);   /* -0 + -0 = -0, else +0 */
    ea = EXP(a) ? EXP(a) : 1;
    eb = EXP(b) ? EXP(b) : 1;
    ma = (MANT(a) | (EXP(a) ? 1u << 23 : 0)) << 3;
    mb = (MANT(b) | (EXP(b) ? 1u << 23 : 0)) << 3;
    d = ea - eb;
    mb = shr_sticky(mb, d);
    if (sa == sb) {
        m = ma + mb;
        if (m >= (1u << 27)) {
            m = shr_sticky(m, 1);
            ea++;
        }
    } else {
        m = ma - mb;
        if (!m)
            return 0;                /* x - x = +0 when rounding to nearest */
        while (m < (1u << 26) && ea > 1) {
            m <<= 1;
            ea--;
        }
    }
    return round_pack(sa, ea, m);
}

static u32 mul_bits(u32 a, u32 b)
{
    u32 sign = (a ^ b) & SIGN, ma, mb, a1, a0, b1, b0, lo, hi, mid, t, m;
    s32 ea, eb, e, s;

    if (is_nan(a) || is_nan(b))
        return nan2(a, b);
    if (EXP(a) == 255 || EXP(b) == 255) {
        if (!(a & 0x7fffffffu) || !(b & 0x7fffffffu))
            return 0xffc00000u;      /* inf * 0 */
        return sign | 0x7f800000u;
    }
    if (!(a & 0x7fffffffu) || !(b & 0x7fffffffu))
        return sign;
    ma = unpack(a, &ea);
    mb = unpack(b, &eb);
    /* 48-bit product hi:lo from 16-bit halves */
    a1 = ma >> 16;
    a0 = ma & 0xffff;
    b1 = mb >> 16;
    b0 = mb & 0xffff;
    lo = a0 * b0;
    mid = a1 * b0 + a0 * b1;
    hi = a1 * b1 + (mid >> 16);
    t = lo + (mid << 16);
    hi += t < lo;
    lo = t;
    /* product in [2^46, 2^48): bring its leading 1 to bit 26 */
    e = ea + eb - 127;
    if (hi >= (1u << 15)) {
        s = 21;
        e++;
    } else
        s = 20;
    m = (hi << (32 - s)) | (lo >> s) | ((lo & ((1u << s) - 1)) != 0);
    return round_pack(sign, e, m);
}

static u32 div_bits(u32 a, u32 b)
{
    u32 sign = (a ^ b) & SIGN, ma, mb, q = 0;
    s32 ea, eb, e, i;

    if (is_nan(a) || is_nan(b))
        return nan2(a, b);
    if (EXP(a) == 255) {
        if (EXP(b) == 255)
            return 0xffc00000u;      /* inf / inf */
        return sign | 0x7f800000u;
    }
    if (EXP(b) == 255)
        return sign;
    if (!(b & 0x7fffffffu))
        return (a & 0x7fffffffu) ? sign | 0x7f800000u : 0xffc00000u;
    if (!(a & 0x7fffffffu))
        return sign;
    ma = unpack(a, &ea);
    mb = unpack(b, &eb);
    e = ea - eb + 127;
    if (ma < mb) {
        ma <<= 1;
        e--;
    }
    /* 27 quotient bits, the first always 1 */
    for (i = 0; i < 27; i++) {
        q <<= 1;
        if (ma >= mb) {
            ma -= mb;
            q |= 1;
        }
        ma <<= 1;
    }
    return round_pack(sign, e, q | (ma != 0));
}

/* -2 unordered, else -1, 0, 1 */
static int cmp_bits(u32 a, u32 b)
{
    s32 x, y;
    if (is_nan(a) || is_nan(b))
        return -2;
    if (!((a | b) & 0x7fffffffu))
        return 0;
    x = (a & SIGN) ? -(s32)(a & 0x7fffffffu) : (s32)a;
    y = (b & SIGN) ? -(s32)(b & 0x7fffffffu) : (s32)b;
    return x < y ? -1 : x > y;
}

static u32 from_u32(u32 sign, u32 u)
{
    s32 e = 127 + 31;
    if (!u)
        return 0;
    while (!(u & SIGN)) {
        u <<= 1;
        e--;
    }
    /* leading 1 at bit 31: bring it to bit 26 */
    return round_pack(sign, e, shr_sticky(u, 5));
}

/* truncate toward zero, saturating as the 68881's FMOVE.L does */
static s32 to_s32(u32 a)
{
    s32 e = EXP(a);
    u32 m;
    if (is_nan(a))
        return 0x7fffffff;
    if (e < 127)
        return 0;
    if (e >= 127 + 31)
        return (a & SIGN) ? (s32)0x80000000u : 0x7fffffff;
    m = MANT(a) | (1u << 23);
    m = e >= 150 ? m << (e - 150) : m >> (150 - e);
    return (a & SIGN) ? -(s32)m : (s32)m;
}

union fbits { float f; u32 u; };
union dbits { double d; u32 w[2]; };

static u32 fb(float f) { union fbits x; x.f = f; return x.u; }
static float bf(u32 u) { union fbits x; x.u = u; return x.f; }

float SF(addsf3)(float a, float b) { return bf(add_bits(fb(a), fb(b))); }
float SF(subsf3)(float a, float b) { return bf(add_bits(fb(a), fb(b) ^ SIGN)); }
float SF(mulsf3)(float a, float b) { return bf(mul_bits(fb(a), fb(b))); }
float SF(divsf3)(float a, float b) { return bf(div_bits(fb(a), fb(b))); }
float SF(negsf2)(float a) { return bf(fb(a) ^ SIGN); }

/* libgcc's comparison results, including what each returns when unordered */
int SF(eqsf2)(float a, float b) { return cmp_bits(fb(a), fb(b)) != 0; }
int SF(nesf2)(float a, float b) { return cmp_bits(fb(a), fb(b)) != 0; }
int SF(ltsf2)(float a, float b) { int c = cmp_bits(fb(a), fb(b)); return c == -2 ? 1 : c; }
int SF(lesf2)(float a, float b) { int c = cmp_bits(fb(a), fb(b)); return c == -2 ? 1 : c; }
int SF(cmpsf2)(float a, float b) { int c = cmp_bits(fb(a), fb(b)); return c == -2 ? 1 : c; }
int SF(gtsf2)(float a, float b) { int c = cmp_bits(fb(a), fb(b)); return c == -2 ? -1 : c; }
int SF(gesf2)(float a, float b) { int c = cmp_bits(fb(a), fb(b)); return c == -2 ? -1 : c; }
int SF(unordsf2)(float a, float b) { return cmp_bits(fb(a), fb(b)) == -2; }

float SF(floatunsisf)(u32 u) { return bf(from_u32(0, u)); }
float SF(floatsisf)(s32 i) { return bf(i < 0 ? from_u32(SIGN, -(u32)i) : from_u32(0, (u32)i)); }
s32 SF(fixsfsi)(float a) { return to_s32(fb(a)); }
u32 SF(fixunssfsi)(float a)
{
    u32 x = fb(a);
    s32 e = EXP(x);
    u32 m;
    if ((x & SIGN) || e < 127)
        return 0;
    if (e >= 127 + 32)
        return 0xffffffffu;
    m = MANT(x) | (1u << 23);
    return e >= 150 ? m << (e - 150) : m >> (150 - e);
}

/* exact: every single is a double */
double SF(extendsfdf2)(float a)
{
    union dbits r;
    u32 x = fb(a), sign = x & SIGN, m = MANT(x);
    s32 e = EXP(x);
    if (e == 255) {
        r.w[0] = sign | 0x7ff00000u | (m >> 3);
        r.w[1] = m << 29;
    } else if (!e && !m) {
        r.w[0] = sign;
        r.w[1] = 0;
    } else {
        if (!e) {
            e = 1;
            while (!(m & (1u << 23))) {
                m <<= 1;
                e--;
            }
            m &= 0x7fffffu;
        }
        r.w[0] = sign | ((u32)(e - 127 + 1023) << 20) | (m >> 3);
        r.w[1] = m << 29;
    }
#ifdef SOFTFLOAT_TEST
    if (1) {   /* the host is little-endian: swap the words */
        u32 t = r.w[0];
        r.w[0] = r.w[1];
        r.w[1] = t;
    }
#endif
    return r.d;
}
