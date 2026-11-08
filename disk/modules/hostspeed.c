/* hostspeed: how fast the emulator runs each costly CPU/FPU instruction
 * (CONTRACT.md section 10, id 26; docs/cases/hostspeed.md).
 *
 * For each instruction class the module runs one timed block: the
 * instruction unrolled 64 times inside a DBF loop (hostspeed_asm.s), for a
 * fixed amount of EMULATED time (HS_BLOCK_MS, sized from a short trial run
 * measured with amt_ticks()). While a block runs, the mailbox position
 * (mbox_set_pos: module 26, case HS_POS0 + block index) says which block
 * it is. The guest cannot see host time; the runner can: it reads the
 * mailbox heartbeat (one increment per frame while no record is sent) and
 * stamps every read with its own clock, and so gets the emulator's frame
 * rate and the emulator's CPU time per block (runner/amitorture/cases.py,
 * pseudo cases host.speed_<insn>).
 *
 * Records (not cases: the verdict needs the host's clock, docs/schema.md):
 *   {"t":"speed","mod":"hostspeed","ev":"begin","insn":..,"cls":..,"pos":..}
 *   {"t":"speed","mod":"hostspeed","ev":"end","insn":..,"pos":..,"n":<instructions>,
 *    "timing":{"cck","empty_cck","frames","units"}}
 *   {"t":"speed","mod":"hostspeed","ev":"skip","insn":..,"why":..}
 *   benchmark-only builds, at the end (guest-timed scores, see below):
 *   {"t":"speed","mod":"hostspeed","ev":"score","insn":"int"|"fpu","blocks":..,"x100"|"ops":..}
 * Which blocks run depends only on the CPU/FPU variant (amt_probe_result):
 * the instructions the part implements in hardware. What the 68060 traps
 * as unimplemented integer instructions (MOVEP, CAS2, CHK2, 64-bit
 * MUL/DIV) and what the 68040/68060 FPSP would emulate (transcendentals,
 * FMOD/FREM/FSCALE/FGETEXP/FGETMAN, FMOVECR, packed decimal; FINT/FINTRZ
 * on the 68040) are skipped, as are CAS/CAS2 without non-chip RAM (chip
 * RAM has no indivisible read-modify-write cycle).
 *
 * The block sizes (n, cck) depend on the engine's emulated timing, so they
 * differ between JIT and interpreter; they are in speed records only,
 * which parity and fingerprints never compare. The module reports no
 * cases. */
#include "mod.h"
#ifdef AMT_BENCHMARK_ONLY
#include "screen.h"
#endif

AMT_MODULE(hostspeed, 26, AMT_PH_BARE, 0, 0, 4096);

#define HS_BLOCK_MS   1000     /* emulated time per timed block */
#define HS_POS0       1000     /* mailbox case number of block 0 */

typedef ULONG (*hs_fn)(ULONG units, void *mem);

/* hostspeed_asm.s */
ULONG hs_stop(ULONG, void *);
ULONG hs_empty(ULONG, void *);
ULONG hs_add_l(ULONG, void *);
ULONG hs_div_w(ULONG, void *);
ULONG hs_abcd_sbcd(ULONG, void *);
ULONG hs_roxl_roxr(ULONG, void *);
ULONG hs_movem(ULONG, void *);
ULONG hs_movep(ULONG, void *);
ULONG hs_trap_rte(ULONG, void *);
void  hs_rte(void);
ULONG hs_mul_l(ULONG, void *);
ULONG hs_mul_l64(ULONG, void *);
ULONG hs_div_l(ULONG, void *);
ULONG hs_div_l64(ULONG, void *);
ULONG hs_bfextu(ULONG, void *);
ULONG hs_bfffo(ULONG, void *);
ULONG hs_cas(ULONG, void *);
ULONG hs_cas2(ULONG, void *);
ULONG hs_chk2(ULONG, void *);
ULONG hs_pack_unpk(ULONG, void *);
ULONG hs_ram_read(ULONG, void *);
ULONG hs_custom_read(ULONG, void *);
ULONG hs_cia_read(ULONG, void *);
ULONG hs_movec_vbr(ULONG, void *);
ULONG hs_pmove_tc(ULONG, void *);
ULONG hs_move16(ULONG, void *);
ULONG hs_movec_pcr(ULONG, void *);
void  hs_fpu_reset(void);
ULONG hs_pcr_get(void);
void  hs_pcr_set(ULONG);
ULONG hs_fadd(ULONG, void *);
ULONG hs_render(ULONG, void *);
ULONG hs_fmul(ULONG, void *);
ULONG hs_fdiv(ULONG, void *);
ULONG hs_fsqrt(ULONG, void *);
ULONG hs_fmod(ULONG, void *);
ULONG hs_frem(ULONG, void *);
ULONG hs_fscale(ULONG, void *);
ULONG hs_fint(ULONG, void *);
ULONG hs_fintrz(ULONG, void *);
ULONG hs_fgetexp(ULONG, void *);
ULONG hs_fgetman(ULONG, void *);
ULONG hs_fsin(ULONG, void *);
ULONG hs_fcos(ULONG, void *);
ULONG hs_ftan(ULONG, void *);
ULONG hs_fsincos(ULONG, void *);
ULONG hs_fasin(ULONG, void *);
ULONG hs_facos(ULONG, void *);
ULONG hs_fatan(ULONG, void *);
ULONG hs_fatanh(ULONG, void *);
ULONG hs_fsinh(ULONG, void *);
ULONG hs_fcosh(ULONG, void *);
ULONG hs_ftanh(ULONG, void *);
ULONG hs_fetox(ULONG, void *);
ULONG hs_fetoxm1(ULONG, void *);
ULONG hs_ftwotox(ULONG, void *);
ULONG hs_ftentox(ULONG, void *);
ULONG hs_flogn(ULONG, void *);
ULONG hs_flognp1(ULONG, void *);
ULONG hs_flog2(ULONG, void *);
ULONG hs_flog10(ULONG, void *);
ULONG hs_fmovecr(ULONG, void *);
ULONG hs_fmove_p_in(ULONG, void *);
ULONG hs_fmove_p_out(ULONG, void *);
ULONG hs_fmovem(ULONG, void *);
ULONG hs_fsave_frestore(ULONG, void *);

/* flags: when a block runs */
#define B_020     0x0001   /* 68020 and up */
#define B_FPU     0x0002   /* needs an FPU */
#define B_NO060   0x0004   /* unimplemented integer instruction on the 68060 */
#define B_FPSP    0x0008   /* FPSP-emulated on the 68040 and 68060 */
#define B_FPSP040 0x0010   /* FPSP-emulated on the 68040 only */
#define B_RMW     0x0020   /* indivisible read-modify-write: not in chip RAM */
#define B_HALF    0x0040   /* body = 32 pairs of (reload, instruction): 512 per unit */
#define B_010     0x0080   /* 68010 and up */
#define B_030MMU  0x0100   /* 68030 with its integrated MMU; not EC030 or 68851 */
#define B_040     0x0200   /* 68040/68060 instruction set */
#define B_060     0x0400   /* 68060 only */

struct hs_blk {
    const char *insn;      /* host.speed_<insn> */
    hs_fn fn;
    UWORD flags;
};

/* The order is the report's. "stop" first: the CPU idle in STOP, woken by
 * the VBL once per frame, so what the host spends per emulated second there
 * is the emulator's own cost (chipset, display, frame pacing), which the
 * host cost per instruction leaves out. "empty" and "empty_end": the loop
 * alone, its own cost per iteration, and the run's noise and drift
 * references; add_l and fadd: the class references the host cost is
 * compared with. New blocks follow empty_end to preserve existing mailbox
 * positions; empty_final brackets those additions. */
static const struct hs_blk blocks[] = {
    { "stop",           hs_stop,           0 },
    { "empty",          hs_empty,          0 },
    { "add_l",          hs_add_l,          0 },
    { "div_w",          hs_div_w,          0 },
    { "abcd_sbcd",      hs_abcd_sbcd,      0 },
    { "roxl_roxr",      hs_roxl_roxr,      0 },
    { "movem",          hs_movem,          0 },
    { "movep",          hs_movep,          B_NO060 },
    { "trap_rte",       hs_trap_rte,       0 },
    { "mul_l",          hs_mul_l,          B_020 },
    { "mul_l64",        hs_mul_l64,        B_020 | B_NO060 },
    { "div_l",          hs_div_l,          B_020 },
    { "div_l64",        hs_div_l64,        B_020 | B_NO060 },
    { "bfextu",         hs_bfextu,         B_020 },
    { "bfffo",          hs_bfffo,          B_020 },
    { "cas",            hs_cas,            B_020 | B_RMW },
    { "cas2",           hs_cas2,           B_020 | B_RMW | B_NO060 },
    { "chk2",           hs_chk2,           B_020 | B_NO060 },
    { "pack_unpk",      hs_pack_unpk,      B_020 },
    { "fadd",           hs_fadd,           B_FPU },
    { "fmul",           hs_fmul,           B_FPU },
    { "fdiv",           hs_fdiv,           B_FPU },
    { "fsqrt",          hs_fsqrt,          B_FPU },
    { "fmod",           hs_fmod,           B_FPU | B_FPSP | B_HALF },
    { "frem",           hs_frem,           B_FPU | B_FPSP | B_HALF },
    { "fscale",         hs_fscale,         B_FPU | B_FPSP },
    { "fint",           hs_fint,           B_FPU | B_FPSP040 },
    { "fintrz",         hs_fintrz,         B_FPU | B_FPSP040 },
    { "fgetexp",        hs_fgetexp,        B_FPU | B_FPSP },
    { "fgetman",        hs_fgetman,        B_FPU | B_FPSP },
    { "fsin",           hs_fsin,           B_FPU | B_FPSP },
    { "fcos",           hs_fcos,           B_FPU | B_FPSP },
    { "ftan",           hs_ftan,           B_FPU | B_FPSP },
    { "fsincos",        hs_fsincos,        B_FPU | B_FPSP },
    { "fasin",          hs_fasin,          B_FPU | B_FPSP },
    { "facos",          hs_facos,          B_FPU | B_FPSP },
    { "fatan",          hs_fatan,          B_FPU | B_FPSP },
    { "fatanh",         hs_fatanh,         B_FPU | B_FPSP },
    { "fsinh",          hs_fsinh,          B_FPU | B_FPSP },
    { "fcosh",          hs_fcosh,          B_FPU | B_FPSP },
    { "ftanh",          hs_ftanh,          B_FPU | B_FPSP },
    { "fetox",          hs_fetox,          B_FPU | B_FPSP },
    { "fetoxm1",        hs_fetoxm1,        B_FPU | B_FPSP },
    { "ftwotox",        hs_ftwotox,        B_FPU | B_FPSP },
    { "ftentox",        hs_ftentox,        B_FPU | B_FPSP },
    { "flogn",          hs_flogn,          B_FPU | B_FPSP },
    { "flognp1",        hs_flognp1,        B_FPU | B_FPSP },
    { "flog2",          hs_flog2,          B_FPU | B_FPSP },
    { "flog10",         hs_flog10,         B_FPU | B_FPSP },
    { "fmovecr",        hs_fmovecr,        B_FPU | B_FPSP },
    { "fmove_p_in",     hs_fmove_p_in,     B_FPU | B_FPSP },
    { "fmove_p_out",    hs_fmove_p_out,    B_FPU | B_FPSP },
    { "fmovem",         hs_fmovem,         B_FPU },
    { "fsave_frestore", hs_fsave_frestore, B_FPU },
    { "render",         hs_render,         B_FPU },
    { "empty_end",      hs_empty,          0 },
    { "ram_read",       hs_ram_read,       0 },
    { "custom_read",    hs_custom_read,    0 },
    { "cia_read",       hs_cia_read,       0 },
    { "movec_vbr",      hs_movec_vbr,      B_010 },
    { "pmove_tc",       hs_pmove_tc,       B_030MMU },
    { "move16",         hs_move16,         B_040 },
    { "movec_pcr",      hs_movec_pcr,      B_060 },
    { "empty_final",    hs_empty,          0 },
};
#define N_BLOCKS (sizeof(blocks) / sizeof(blocks[0]))

static void *hs_mem;

/* why a block does not run on this machine, or 0 */
static const char *skip_why(const struct hs_blk *b)
{
    int cpu = AMT_CPU(), fpu = AMT_FPU();
    if ((b->flags & B_010) && cpu < AMT_CPU_68010)
        return "requires 68010 or later (MOVEC VBR)";
    if ((b->flags & B_030MMU) && (cpu != AMT_CPU_68030 || !AMT_MMU()))
        return "requires a 68030 with its integrated MMU (not 68020, EC030 or 68040/68060)";
    if ((b->flags & B_040) && cpu < AMT_CPU_68EC040)
        return "requires 68040 or 68060 (MOVE16)";
    if ((b->flags & B_060) && cpu < AMT_CPU_68EC060)
        return "requires 68060 (MOVEC PCR)";
    if ((b->flags & B_020) && cpu < AMT_CPU_68EC020)
        return "68020+ instruction: illegal on the 68000/68010";
    if ((b->flags & B_NO060) && cpu >= AMT_CPU_68EC060)
        return "68060: unimplemented integer instruction (trap 61, emulated by the 060 ISP on real hardware)";
    if ((b->flags & B_FPU) && fpu == AMT_FPU_NONE)
        return "no FPU";
    if ((b->flags & B_FPSP) && (fpu == AMT_FPU_68040 || fpu == AMT_FPU_68060))
        return "68040/68060: not implemented by the FPU (emulated by the FPSP on real hardware)";
    if ((b->flags & B_FPSP040) && fpu == AMT_FPU_68040)
        return "68040: not implemented by the FPU (emulated by the FPSP on real hardware)";
    if ((b->flags & B_RMW) && amt_any_pool_on_chip_bus())
        return "chip-bus RAM (chip or $C00000-$DBFFFF slow RAM) has no indivisible read-modify-write cycle";
    return 0;
}

/* instructions per unit ("stop": STOPs = frames, "empty": DBF iterations) */
static ULONG per_unit(const struct hs_blk *b)
{
    if (b->fn == hs_stop)
        return 1;
    if (b->fn == hs_empty)
        return 16;
    return (b->flags & B_HALF) ? 512 : 1024;
}

/* emulated colour clocks of fn(units) */
static ULONG time_units(hs_fn fn, ULONG units)
{
    ULONG t0 = amt_ticks();
    fn(units, hs_mem);
    return amt_ticks() - t0;
}

/* units for about `target` colour clocks: warm up (the JIT translates a
 * block after a few runs), then double a trial run until it takes at least
 * 1/64 of the target, and scale. */
static ULONG size_units(const struct hs_blk *b, ULONG target)
{
    hs_fn fn = b->fn;
    ULONG u = 1, t, r;
    ULONG max_units = 0x7fffffffUL / per_unit(b);
    fn(1, hs_mem);
    fn(1, hs_mem);
    fn(1, hs_mem);
    fn(1, hs_mem);
    for (;;) {
        t = time_units(fn, u);
        if (t >= target / 64 || u >= 0x00100000UL)
            break;
        u <<= 1;
    }
    if (t == 0)
        t = 1;
    r = (target << 3) / t;          /* target < 2^28: no overflow */
    if (r > 0x7ff)
        r = 0x7ff;
    u = (u * r) >> 3;               /* u <= 2^20, r < 2^11 */
    if (u > max_units)             /* keep n below 2^31 without shortening */
        u = max_units;             /* fast reference loops unnecessarily */
    return u ? u : 1;
}

static void emit(const struct hs_blk *b, ULONG idx, const char *ev)
{
    json_begin("speed");
    json_str("mod", "hostspeed");
    json_str("ev", ev);
    json_str("insn", b->insn);
    json_str("cls", (b->flags & B_FPU) ? "fpu" : "int");
    json_int("pos", (long)(HS_POS0 + idx));
#ifdef AMT_BENCHMARK_ONLY
    json_int("benchmark", 1);
#endif
}

static void run_block(const struct hs_blk *b, ULONG idx, ULONG target, ULONG *n_out, ULONG *net_out)
{
    ULONG units, empty, f0, f1, t0, t1;
    amt_watchdog_kick();
    units = size_units(b, target);
    empty = time_units(hs_empty, units);
    emit(b, idx, "begin");
    json_end();
    mbox_set_pos(26, HS_POS0 + idx);
    f0 = amt_frames_now();
    t0 = amt_ticks();
    b->fn(units, hs_mem);
    t1 = amt_ticks();
    f1 = amt_frames_now();
    mbox_set_pos(26, 0);
    emit(b, idx, "end");
    json_int("n", (long)(units * per_unit(b)));
    json_obj("timing");
    json_int("cck", (long)(t1 - t0));
    json_int("empty_cck", (long)empty);
    json_int("frames", (long)(f1 - f0));
    json_int("units", (long)units);
    json_obj_end();
    json_end();
    *n_out = units * per_unit(b);
    *net_out = (t1 - t0) > empty ? (t1 - t0) - empty : 0;
}

#ifdef AMT_BENCHMARK_ONLY
/* Benchmark-only disk: progress and two scores on the status screen, from
 * the guest's own clock (colour clocks). On a real Amiga or an FPGA that is
 * real time; in an emulator it is emulated time (the runner measures the
 * host's speed instead, docs/cases/hostspeed.md).
 *
 * Integer score: geometric mean over the blocks every CPU runs of the net
 * operations per second (loop overhead subtracted), relative to a PAL A500
 * (68000 at 7.09 MHz, 512 KB chip + 512 KB slow RAM, measured with this
 * code on Amiberry's cycle-exact core) = 1.00. FPU score: geometric mean of
 * the net operations per second of the FPU blocks every FPU implements in
 * hardware (no A500 reference exists for those). */
static const struct { hs_fn fn; ULONG ref; } int_ref[] = {
    { hs_add_l,     879525 },
    { hs_div_w,     54618 },
    { hs_abcd_sbcd, 1135077 },
    { hs_roxl_roxr, 318708 },
    { hs_movem,     71759 },
    { hs_trap_rte,  128194 },
};
static const hs_fn fpu_set[] = { hs_fadd, hs_fmul, hs_fdiv, hs_fsqrt, hs_fmovem };

/* 2^(2^-(k+1)) in Q30 */
static const ULONG pow2_frac[16] = {
    0x5a82799aUL, 0x4c1bf829UL, 0x45cae0f2UL, 0x42d561b4UL,
    0x4166c34cUL, 0x40b268faUL, 0x4058f6a8UL, 0x402c6be9UL,
    0x4016321bUL, 0x400b1818UL, 0x40058bceUL, 0x4002c5d8UL,
    0x400162e8UL, 0x4000b173UL, 0x400058b9UL, 0x40002c5dUL,
};

/* log2(x) in Q16, x > 0 */
static LONG log2_q16(ULONG x)
{
    LONG r = 31;
    ULONG bit;
    unsigned long long y;
    while (!(x & 0x80000000UL)) {
        x <<= 1;
        r--;
    }
    r <<= 16;
    y = x;                              /* [1, 2) in Q31 */
    for (bit = 0x8000; bit; bit >>= 1) {
        y = (y * y) >> 31;
        if (y >= 0x100000000ULL) {
            y >>= 1;
            r |= (LONG)bit;
        }
    }
    return r;
}

/* scale * 2^(v / 65536), saturated to 32 bits */
static ULONG exp2_q16(LONG v, ULONG scale)
{
    LONG i = v >> 16;
    ULONG f = (ULONG)v & 0xffff;
    unsigned long long m = 1ULL << 30;
    int k, shift;
    for (k = 0; k < 16; k++)
        if (f & (0x8000UL >> k))
            m = (m * pow2_frac[k]) >> 30;
    m *= scale;                         /* < 2^31 * 2^32 */
    shift = 30 - (int)i;
    if (shift >= 64)
        return 0;
    if (shift < 0)
        return 0xffffffffUL;
    m >>= shift;
    return m > 0xffffffffULL ? 0xffffffffUL : (ULONG)m;
}

static ULONG net_ops(ULONG n, ULONG net_cck)
{
    unsigned long long o;
    if (!net_cck || !n)
        return 0;
    o = (unsigned long long)n * amt_cck_hz() / net_cck;
    return o > 0xffffffffULL ? 0xffffffffUL : (ULONG)o;
}

static LONG int_log, fpu_log;
static int int_n, fpu_n;

static void score_add(const struct hs_blk *b, ULONG ops)
{
    unsigned i;
    if (!ops)
        return;
    for (i = 0; i < sizeof int_ref / sizeof int_ref[0]; i++)
        if (int_ref[i].fn == b->fn) {
            int_log += log2_q16(ops) - log2_q16(int_ref[i].ref);
            int_n++;
        }
    for (i = 0; i < sizeof fpu_set / sizeof fpu_set[0]; i++)
        if (fpu_set[i] == b->fn) {
            fpu_log += log2_q16(ops);
            fpu_n++;
        }
}

/* "12.34" from hundredths */
static char *fmt_x100(char *p, ULONG v)
{
    p = amt_utoa(p, v / 100);
    *p++ = '.';
    *p++ = (char)('0' + (v / 10) % 10);
    *p++ = (char)('0' + v % 10);
    *p = 0;
    return p;
}

/* "1.23M ops/s" */
static void fmt_ops(char *p, ULONG ops)
{
    if (ops >= 1000000000UL) {
        p = fmt_x100(p, ops / 10000000UL);
        *p++ = 'G';
    } else if (ops >= 1000000UL) {
        p = fmt_x100(p, ops / 10000UL);
        *p++ = 'M';
    } else if (ops >= 1000UL) {
        p = fmt_x100(p, ops / 10UL);
        *p++ = 'k';
    } else
        p = amt_utoa(p, ops);
    p[0] = ' ', p[1] = 'o', p[2] = 'p', p[3] = 's', p[4] = '/', p[5] = 's', p[6] = 0;
}

/* "Benchmark 12/48: " and "About 1:05 left." */
static void bench_progress(const struct hs_blk *b, ULONG done, ULONG total, ULONG frames)
{
    char a[24], t[12], *p;
    ULONG left, hz = amt_secs(1);
    p = amt_utoa(a, done + 1);
    *p++ = '/';
    p = amt_utoa(p, total);
    *p++ = ':';
    *p++ = ' ';
    *p = 0;
    screen_bench(0, "Benchmark ", a, b->insn);
    /* about 1.2 s per block until there is a measurement */
    left = done ? frames * (total - done) / done / hz : total * 6 / 5;
    p = amt_utoa(t, left / 60);
    *p++ = ':';
    *p++ = (char)('0' + (left % 60) / 10);
    *p++ = (char)('0' + left % 10);
    *p = 0;
    screen_bench(1, "About ", t, " left.");
}

static void bench_finish(ULONG total, int host)
{
    char a[24];
    ULONG v;
    amt_utoa(a, total);
    screen_bench(0, "Benchmark finished: ", a, " blocks.");
    json_begin("speed");
    json_str("mod", "hostspeed");
    json_str("ev", "score");
    json_str("insn", "int");
    json_int("benchmark", 1);
    json_int("blocks", int_n);
    if (int_n) {
        v = exp2_q16(int_log / int_n, 100);
        json_int("x100", (long)(v > 0x7fffffffUL ? 0x7fffffffUL : v));
        json_str("ref", "A500");
        fmt_x100(a, v);
        screen_bench(1, "Integer: ", a, " (A500 = 1.00)");
    } else
        screen_bench(1, "Integer: not measured", 0, 0);
    json_end();
    if (fpu_n) {
        v = exp2_q16(fpu_log / fpu_n, 1);
        json_begin("speed");
        json_str("mod", "hostspeed");
        json_str("ev", "score");
        json_str("insn", "fpu");
        json_int("benchmark", 1);
        json_int("blocks", fpu_n);
        json_int("ops", (long)(v > 0x7fffffffUL ? 0x7fffffffUL : v));
        json_end();
        fmt_ops(a, v);
        screen_bench(2, "FPU:     ", a, 0);
    } else
        screen_bench(2, AMT_FPU() == AMT_FPU_NONE ? "FPU:     none" : "FPU:     not measured", 0, 0);
    if (host) {
        screen_bench(3, "Emulated time: host speed is in the", 0, 0);
        screen_bench(4, "runner's report.", 0, 0);
        /* the report keeps a picture of the result (raw/<entry>/shot-*) */
        amt_request("screenshot", "benchmark", "{\"frames\":2}", amt_secs(10));
    } else
        screen_bench(3, "Per-block details: serial port.", 0, 0);
}
#endif

void amt_run_hostspeed(void)
{
    ULONG i, target, pcr = 0, n, net;
    int fpu = AMT_FPU() != AMT_FPU_NONE;
    int is060 = AMT_CPU() >= AMT_CPU_68EC060;
    int host;
    amt_vec_fn old_trap;
#ifdef AMT_BENCHMARK_ONLY
    ULONG total = 0, done = 0, f0;
    for (i = 0; i < N_BLOCKS; i++)
        if (!skip_why(&blocks[i]))
            total++;
    screen_bench(0, "Benchmark: waiting for a host (5 s)", 0, 0);
#endif

    hs_mem = amt_any_pool();
    target = (amt_cck_hz() / 1000) * HS_BLOCK_MS;
    /* A host that measures the blocks reads the mailbox; it acknowledges
     * this request once it follows the mailbox, so the first block is
     * sampled from its start. Without such a host (real hardware, serial
     * only) the blocks run anyway after the timeout. */
    host = amt_request("wait", "hostspeed", 0, amt_secs(5));
    (void)host;
#ifdef AMT_BENCHMARK_ONLY
    f0 = amt_frames_now();
#endif
    old_trap = amt_vec_set(32, hs_rte);
    if (fpu && is060) {
        pcr = hs_pcr_get();
        if (pcr & 2)
            hs_pcr_set(pcr & ~2UL);   /* PCR.DFP: enable the FPU */
    }
    for (i = 0; i < N_BLOCKS; i++) {
        const struct hs_blk *b = &blocks[i];
        const char *why = skip_why(b);
        if (why) {
            emit(b, i, "skip");
            json_str("why", why);
            json_end();
            continue;
        }
#ifdef AMT_BENCHMARK_ONLY
        bench_progress(b, done, total, amt_frames_now() - f0);
#endif
        run_block(b, i, target, &n, &net);
#ifdef AMT_BENCHMARK_ONLY
        score_add(b, net_ops(n, net));
        done++;
#endif
    }
    if (fpu) {
        hs_fpu_reset();
        if (is060 && (pcr & 2))
            hs_pcr_set(pcr);
    }
    amt_vec_set(32, old_trap);
#ifdef AMT_BENCHMARK_ONLY
    bench_finish(total, host);
#endif
}
