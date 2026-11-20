/* bench_micro: the micro tier, one instruction class or memory path per
 * block (docs/BENCH.md "Micro tier"). The blocks are in micro_asm.s; which
 * of them run depends only on the CPU/FPU the probe found: the
 * instructions the part implements in hardware. What the 68060 traps as
 * unimplemented integer instructions (MOVEP, CAS2, CHK2, 64-bit MUL/DIV)
 * and what the 68040/68060 FPSP would emulate (transcendentals, FREM,
 * FSCALE, FGETEXP, FMOVECR, packed decimal; FINTRZ on the 68040) are
 * skipped, as are CAS/CAS2 in chip-bus RAM (no indivisible
 * read-modify-write cycle there). */
#include "harness.h"

#define BM_MEM_BYTES  4096
#define BM_READ_BYTES 65536UL
AMT_MODULE(bench_micro, 40, AMT_PH_BARE, AMT_MF_LONG, BM_READ_BYTES, BM_MEM_BYTES + BM_READ_BYTES);

ULONG bm_mem_read(ULONG, void *);
ULONG bm_empty(ULONG, void *);
ULONG bm_add_l(ULONG, void *);
ULONG bm_div_w(ULONG, void *);
ULONG bm_abcd_sbcd(ULONG, void *);
ULONG bm_roxl_roxr(ULONG, void *);
ULONG bm_movem(ULONG, void *);
ULONG bm_movep(ULONG, void *);
ULONG bm_trap_rte(ULONG, void *);
void  bm_rte(void);
ULONG bm_mul_l(ULONG, void *);
ULONG bm_mul_l64(ULONG, void *);
ULONG bm_div_l(ULONG, void *);
ULONG bm_div_l64(ULONG, void *);
ULONG bm_bfextu(ULONG, void *);
ULONG bm_bfffo(ULONG, void *);
ULONG bm_cas(ULONG, void *);
ULONG bm_cas2(ULONG, void *);
ULONG bm_chk2(ULONG, void *);
ULONG bm_pack_unpk(ULONG, void *);
ULONG bm_custom_read(ULONG, void *);
ULONG bm_cia_read(ULONG, void *);
ULONG bm_move16(ULONG, void *);
ULONG bm_fadd(ULONG, void *);
ULONG bm_fmul(ULONG, void *);
ULONG bm_fdiv(ULONG, void *);
ULONG bm_fsqrt(ULONG, void *);
ULONG bm_frem(ULONG, void *);
ULONG bm_fscale(ULONG, void *);
ULONG bm_fintrz(ULONG, void *);
ULONG bm_fgetexp(ULONG, void *);
ULONG bm_fsin(ULONG, void *);
ULONG bm_fatan(ULONG, void *);
ULONG bm_fetox(ULONG, void *);
ULONG bm_flogn(ULONG, void *);
ULONG bm_fmovecr(ULONG, void *);
ULONG bm_fmove_p_in(ULONG, void *);
ULONG bm_fmove_p_out(ULONG, void *);
ULONG bm_fmovem(ULONG, void *);
ULONG bm_mem_write(ULONG, void *);
ULONG bm_mem_copy(ULONG, void *);
ULONG bm_dispatch(ULONG, void *);
ULONG bm_smc_patch(ULONG, void *);
ULONG bm_unaligned_l(ULONG, void *);
ULONG amt_cs_cacr_get(void);
void  amt_cs_cacr_set(ULONG);
void  amt_cs_cpusha(void);

/* how bm_smc_patch makes the CPU see its patch: 0 nothing (68000/68010),
 * 1 CACR clear-instruction-cache (68020/68030), 2 CPUSHA BC (68040/68060) */
ULONG bm_flush_mode;

/* when a block runs */
#define M_020     0x0001   /* 68020 and up */
#define M_FPU     0x0002   /* needs an FPU */
#define M_NO060   0x0004   /* unimplemented integer instruction on the 68060 */
#define M_FPSP    0x0008   /* FPSP-emulated on the 68040 and 68060 */
#define M_FPSP040 0x0010   /* FPSP-emulated on the 68040 only */
#define M_RMW     0x0020   /* indivisible read-modify-write: not in chip-bus RAM */
#define M_HALF    0x0040   /* body = 32 pairs of (reload, instruction): 512 per unit */
#define M_WRITE   0x0080   /* with M_CHIP/M_FAST: memory write, 64 KB per unit */
#define M_COPY    0x0100   /* with M_CHIP/M_FAST: memory copy, 32 KB per unit */
#define M_NOICACHE 0x0400  /* run with the instruction cache off (68020+) */
#define M_040     0x0200   /* 68040/68060 instruction set */
#define M_NOSUM   0x0800   /* reads hardware registers that change */
#define M_EMPTY   0x1000   /* the loop alone: 16 per unit */
#define M_CHIP    0x2000   /* memory read, chip RAM, display DMA off */
#define M_FAST    0x4000   /* memory read, fast RAM */
#define M_ROM     0x8000   /* memory read, Kickstart ROM */

struct bm_def {
    const char *name;
    UWORD id;
    UWORD flags;
    bh_fn fn;
    UWORD check;           /* V_*: the checksum the block must return */
};

/* expected checksums (micro_asm.s): wrong arithmetic fails the block */
#define V_NONE  0
#define V_ADD   1          /* d0 = 1024 per unit, d1 = 1 */
#define V_MUL   2          /* x * a * a^-1 = x */
#define V_DIVW  3          /* the dividend is a fixed point */
#define V_FADD  4          /* fp0 = 1.5 + 512 per unit */
#define V_FMUL  5          /* fp0 = 1.5 * 2 * 0.5 */
#define V_SMC   6          /* d0 = the patched immediates, d1 = 63 per unit */

/* Plan ids are part of the baseline format: never reuse or renumber one.
 * Retired: 11 movec_vbr, 22 pmove_tc, 24 movec_pcr, 48 fsave_frestore. */
static const struct bm_def defs[] = {
    { "empty",          1, M_EMPTY,                 bm_empty, V_NONE },
    { "add_l",          2, 0,                       bm_add_l, V_ADD },
    { "div_w",          3, 0,                       bm_div_w, V_DIVW },
    { "abcd_sbcd",      4, 0,                       bm_abcd_sbcd, V_NONE },
    { "roxl_roxr",      5, 0,                       bm_roxl_roxr, V_NONE },
    { "movem",          6, 0,                       bm_movem, V_NONE },
    { "movep",          7, M_NO060,                 bm_movep, V_NONE },
    { "trap_rte",       8, 0,                       bm_trap_rte, V_NONE },
    { "custom_read",    9, M_NOSUM,                 bm_custom_read, V_NONE },
    { "cia_read",      10, M_NOSUM,                 bm_cia_read, V_NONE },
    { "mul_l",         12, M_020,                   bm_mul_l, V_MUL },
    { "mul_l64",       13, M_020 | M_NO060,         bm_mul_l64, V_NONE },
    { "div_l",         14, M_020,                   bm_div_l, V_NONE },
    { "div_l64",       15, M_020 | M_NO060,         bm_div_l64, V_NONE },
    { "bfextu",        16, M_020,                   bm_bfextu, V_NONE },
    { "bfffo",         17, M_020,                   bm_bfffo, V_NONE },
    { "cas",           18, M_020 | M_RMW,           bm_cas, V_NONE },
    { "cas2",          19, M_020 | M_RMW | M_NO060, bm_cas2, V_NONE },
    { "chk2",          20, M_020 | M_NO060,         bm_chk2, V_NONE },
    { "pack_unpk",     21, M_020,                   bm_pack_unpk, V_NONE },
    { "move16",        23, M_040,                   bm_move16, V_NONE },
    { "mem_read_chip", 25, M_CHIP,                  bm_mem_read, V_NONE },
    { "mem_read_fast", 26, M_FAST,                  bm_mem_read, V_NONE },
    { "mem_read_rom",  27, M_ROM,                   bm_mem_read, V_NONE },
    { "mem_copy_chip", 30, M_CHIP | M_COPY,         bm_mem_copy, V_NONE },
    { "mem_copy_fast", 31, M_FAST | M_COPY,         bm_mem_copy, V_NONE },
    { "mem_write_chip",28, M_CHIP | M_WRITE,        bm_mem_write, V_NONE },
    { "mem_write_fast",29, M_FAST | M_WRITE,        bm_mem_write, V_NONE },
    { "fadd",          32, M_FPU,                   bm_fadd, V_FADD },
    { "fmul",          33, M_FPU,                   bm_fmul, V_FMUL },
    { "fdiv",          34, M_FPU,                   bm_fdiv, V_NONE },
    { "fsqrt",         35, M_FPU,                   bm_fsqrt, V_NONE },
    { "frem",          36, M_FPU | M_FPSP | M_HALF, bm_frem, V_NONE },
    { "fscale",        37, M_FPU | M_FPSP,          bm_fscale, V_NONE },
    { "fintrz",        38, M_FPU | M_FPSP040,       bm_fintrz, V_NONE },
    { "fgetexp",       39, M_FPU | M_FPSP,          bm_fgetexp, V_NONE },
    { "fsin",          40, M_FPU | M_FPSP,          bm_fsin, V_NONE },
    { "fatan",         41, M_FPU | M_FPSP,          bm_fatan, V_NONE },
    { "fetox",         42, M_FPU | M_FPSP,          bm_fetox, V_NONE },
    { "flogn",         43, M_FPU | M_FPSP,          bm_flogn, V_NONE },
    { "fmovecr",       44, M_FPU | M_FPSP,          bm_fmovecr, V_NONE },
    { "fmove_p_in",    45, M_FPU | M_FPSP,          bm_fmove_p_in, V_NONE },
    { "fmove_p_out",   46, M_FPU | M_FPSP,          bm_fmove_p_out, V_NONE },
    { "fmovem",        47, M_FPU,                   bm_fmovem, V_NONE },
    { "unaligned_l",   49, M_020,                   bm_unaligned_l, V_NONE },
    { "dispatch",      50, 0,                       bm_dispatch, V_NONE },
    { "smc_patch",     51, 0,                       bm_smc_patch, V_SMC },
    { "add_l_nocache", 52, M_020 | M_NOICACHE,      bm_add_l, V_ADD },
};
#define N_DEFS (sizeof(defs) / sizeof(defs[0]))

/* why a block does not run on this machine, or 0 */
static const char *skip_why(const struct bm_def *d)
{
    int cpu = AMT_CPU(), fpu = AMT_FPU();
    if ((d->flags & M_040) && cpu < AMT_CPU_68EC040)
        return "requires 68040 or 68060 (MOVE16)";
    if ((d->flags & M_020) && cpu < AMT_CPU_68EC020)
        return "68020+ instruction: illegal on the 68000/68010";
    if ((d->flags & M_NO060) && cpu >= AMT_CPU_68EC060)
        return "68060: unimplemented integer instruction (trap 61, emulated by the 060 ISP on real hardware)";
    if ((d->flags & M_FPU) && fpu == AMT_FPU_NONE)
        return "no FPU";
    if ((d->flags & M_FPSP) && (fpu == AMT_FPU_68040 || fpu == AMT_FPU_68060))
        return "68040/68060: not implemented by the FPU (emulated by the FPSP on real hardware)";
    if ((d->flags & M_FPSP040) && fpu == AMT_FPU_68040)
        return "68040: not implemented by the FPU (emulated by the FPSP on real hardware)";
    if ((d->flags & M_RMW) && amt_any_pool_on_chip_bus())
        return "chip-bus RAM (chip or $C00000-$DBFFFF slow RAM) has no indivisible read-modify-write cycle";
    if ((d->flags & M_FAST) && amt_any_pool_on_chip_bus())
        return "no fast RAM";
    return 0;
}

static ULONG per_unit(const struct bm_def *d)
{
    if (d->flags & M_COPY)
        return BM_READ_BYTES / 2;
    if (d->flags & (M_CHIP | M_FAST | M_ROM))
        return BM_READ_BYTES;
    if (d->flags & M_EMPTY)
        return 16;
    if (d->fn == bm_smc_patch)
        return 1;                   /* one patch + flush + 64 instructions */
    return (d->flags & M_HALF) ? 512 : 1024;
}

/* FP0-FP3 of bm_fset as singles: 0.5, 2.0, 7.25 */
#define F_REST (0x3f000000UL + 0x40000000UL + 0x40e80000UL)

/* the IEEE single nearest (ties to even) to n / 2, n > 0 */
static ULONG single_half(ULONG n)
{
    int e = 31;
    ULONG m, rest;
    while (!(n & 0x80000000UL)) {
        n <<= 1;
        e--;
    }
    m = n >> 8;                     /* 24 bits, leading 1 */
    rest = n & 0xff;
    if (rest > 0x80 || (rest == 0x80 && (m & 1)))
        m++;
    if (m & 0x01000000UL) {
        m >>= 1;
        e++;
    }
    return ((ULONG)(e - 1 + 127) << 23) | (m & 0x007fffffUL);
}

/* the block under test, and the count of its last run (for check()) */
static const struct bm_def *cur;
static ULONG cur_count;

static ULONG counted(ULONG units, void *mem)
{
    cur_count = units;
    return cur->fn(units, mem);
}

/* the same, with the instruction cache off: CACR EI (68020/68030) or IE
 * (68040/68060, after pushing the caches) cleared for the timed run */
static ULONG nocache(ULONG units, void *mem)
{
    ULONG cacr = amt_cs_cacr_get(), r;
    if (AMT_CPU() >= AMT_CPU_68EC040) {
        amt_cs_cpusha();
        amt_cs_cacr_set(cacr & ~0x00008000UL);
    } else
        amt_cs_cacr_set(cacr & ~0x00000001UL);
    r = counted(units, mem);
    amt_cs_cacr_set(cacr);
    return r;
}

static int check(void *ctx, ULONG *sum)
{
    ULONG want;
    (void)ctx;
    switch (cur->check) {
    case V_ADD:  want = cur_count * 1024 + 1; break;
    case V_MUL:  want = 0x12345678UL + 0x2545f491UL + 0x41444c71UL; break;
    case V_DIVW: want = 0x03e80bb8UL + 21846; break;
    case V_FADD:
        if (cur_count >= 0x400000UL)
            return 1;               /* 2 * fp0 no longer fits 32 bits */
        want = single_half(3 + 1024 * cur_count) + F_REST;
        break;
    case V_FMUL: want = 0x3fc00000UL + F_REST; break;
    case V_SMC:
        want = (cur_count & 1) ? (cur_count + 1) / 2 * cur_count
                               : cur_count / 2 * (cur_count + 1);
        want += 63 * cur_count;
        break;
    default:     return 1;
    }
    return *sum == want;
}

static void fill(ULONG *p, ULONG bytes, ULONG seed)
{
    ULONG i;
    for (i = 0; i < bytes / 4; i++)
        p[i] = seed += 0x9e3779b9UL;
}

static const char *pool_kind(void)
{
    if (amt_any_pool_is_chip())
        return "chip";
    return amt_any_pool_on_chip_bus() ? "slow" : "fast";
}

void amt_run_bench_micro(void)
{
    UBYTE *any = amt_any_pool();
    UBYTE *chip = amt_chip_pool();
    amt_vec_fn old_trap;
    struct bh_blk b;
    unsigned i;

    bh_begin();
    fill((ULONG *)chip, BM_READ_BYTES, 1);
    fill((ULONG *)(any + BM_MEM_BYTES), BM_READ_BYTES, 1);
    old_trap = amt_vec_set(32, bm_rte);
    bm_flush_mode = AMT_CPU() >= AMT_CPU_68EC040 ? 2 : AMT_CPU() >= AMT_CPU_68EC020 ? 1 : 0;
    for (i = 0; i < N_DEFS; i++) {
        const struct bm_def *d = &defs[i];
        const char *why = skip_why(d);
        b.name = d->name;
        b.tier = "micro";
        b.id = d->id;
        b.flags = (d->flags & M_NOSUM) ? BH_NOSUM : 0;
        b.run = d->fn;
        b.ctx = any;
        b.verify = 0;
        if (d->check || (d->flags & M_NOICACHE)) {
            cur = d;
            b.run = (d->flags & M_NOICACHE) ? nocache : counted;
            b.verify = d->check ? check : 0;
        }
        b.per = per_unit(d);
        b.unit = (d->flags & (M_CHIP | M_FAST | M_ROM)) ? "byte" : "insn";
        if (d->fn == bm_smc_patch)
            b.unit = "patch";
        else if (d->fn == bm_dispatch)
            b.unit = "dispatch";
        b.mem = pool_kind();
        b.kset = 0;
        if (d->flags & M_CHIP) {
            b.ctx = chip;
            b.mem = "chip";
        } else if (d->flags & M_FAST)
            b.ctx = any + BM_MEM_BYTES;
        else if (d->flags & M_ROM) {
            b.ctx = (void *)(amt_probe_result.rom_base ? amt_probe_result.rom_base : 0xf80000UL);
            b.mem = "rom";
        }
        if (why)
            bh_skip(&b, why);
        else
            bh_run(&b);
    }
    amt_vec_set(32, old_trap);
    bh_end();
}
