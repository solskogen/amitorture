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
ULONG bm_movec_vbr(ULONG, void *);
ULONG bm_pmove_tc(ULONG, void *);
ULONG bm_move16(ULONG, void *);
ULONG bm_movec_pcr(ULONG, void *);
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
ULONG bm_fsave_frestore(ULONG, void *);

/* when a block runs */
#define M_020     0x0001   /* 68020 and up */
#define M_FPU     0x0002   /* needs an FPU */
#define M_NO060   0x0004   /* unimplemented integer instruction on the 68060 */
#define M_FPSP    0x0008   /* FPSP-emulated on the 68040 and 68060 */
#define M_FPSP040 0x0010   /* FPSP-emulated on the 68040 only */
#define M_RMW     0x0020   /* indivisible read-modify-write: not in chip-bus RAM */
#define M_HALF    0x0040   /* body = 32 pairs of (reload, instruction): 512 per unit */
#define M_010     0x0080   /* 68010 and up */
#define M_030MMU  0x0100   /* 68030 with its integrated MMU; not EC030 or 68851 */
#define M_040     0x0200   /* 68040/68060 instruction set */
#define M_060     0x0400   /* 68060 only */
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
};

/* Plan ids are part of the baseline format: never reuse or renumber one. */
static const struct bm_def defs[] = {
    { "empty",          1, M_EMPTY,                 bm_empty },
    { "add_l",          2, 0,                       bm_add_l },
    { "div_w",          3, 0,                       bm_div_w },
    { "abcd_sbcd",      4, 0,                       bm_abcd_sbcd },
    { "roxl_roxr",      5, 0,                       bm_roxl_roxr },
    { "movem",          6, 0,                       bm_movem },
    { "movep",          7, M_NO060,                 bm_movep },
    { "trap_rte",       8, 0,                       bm_trap_rte },
    { "custom_read",    9, M_NOSUM,                 bm_custom_read },
    { "cia_read",      10, M_NOSUM,                 bm_cia_read },
    { "movec_vbr",     11, M_010,                   bm_movec_vbr },
    { "mul_l",         12, M_020,                   bm_mul_l },
    { "mul_l64",       13, M_020 | M_NO060,         bm_mul_l64 },
    { "div_l",         14, M_020,                   bm_div_l },
    { "div_l64",       15, M_020 | M_NO060,         bm_div_l64 },
    { "bfextu",        16, M_020,                   bm_bfextu },
    { "bfffo",         17, M_020,                   bm_bfffo },
    { "cas",           18, M_020 | M_RMW,           bm_cas },
    { "cas2",          19, M_020 | M_RMW | M_NO060, bm_cas2 },
    { "chk2",          20, M_020 | M_NO060,         bm_chk2 },
    { "pack_unpk",     21, M_020,                   bm_pack_unpk },
    { "pmove_tc",      22, M_030MMU,                bm_pmove_tc },
    { "move16",        23, M_040,                   bm_move16 },
    { "movec_pcr",     24, M_060,                   bm_movec_pcr },
    { "mem_read_chip", 25, M_CHIP,                  bm_mem_read },
    { "mem_read_fast", 26, M_FAST,                  bm_mem_read },
    { "mem_read_rom",  27, M_ROM,                   bm_mem_read },
    { "fadd",          32, M_FPU,                   bm_fadd },
    { "fmul",          33, M_FPU,                   bm_fmul },
    { "fdiv",          34, M_FPU,                   bm_fdiv },
    { "fsqrt",         35, M_FPU,                   bm_fsqrt },
    { "frem",          36, M_FPU | M_FPSP | M_HALF, bm_frem },
    { "fscale",        37, M_FPU | M_FPSP,          bm_fscale },
    { "fintrz",        38, M_FPU | M_FPSP040,       bm_fintrz },
    { "fgetexp",       39, M_FPU | M_FPSP,          bm_fgetexp },
    { "fsin",          40, M_FPU | M_FPSP,          bm_fsin },
    { "fatan",         41, M_FPU | M_FPSP,          bm_fatan },
    { "fetox",         42, M_FPU | M_FPSP,          bm_fetox },
    { "flogn",         43, M_FPU | M_FPSP,          bm_flogn },
    { "fmovecr",       44, M_FPU | M_FPSP,          bm_fmovecr },
    { "fmove_p_in",    45, M_FPU | M_FPSP,          bm_fmove_p_in },
    { "fmove_p_out",   46, M_FPU | M_FPSP,          bm_fmove_p_out },
    { "fmovem",        47, M_FPU,                   bm_fmovem },
    { "fsave_frestore", 48, M_FPU,                  bm_fsave_frestore },
};
#define N_DEFS (sizeof(defs) / sizeof(defs[0]))

/* why a block does not run on this machine, or 0 */
static const char *skip_why(const struct bm_def *d)
{
    int cpu = AMT_CPU(), fpu = AMT_FPU();
    if ((d->flags & M_010) && cpu < AMT_CPU_68010)
        return "requires 68010 or later (MOVEC VBR)";
    if ((d->flags & M_030MMU) && (cpu != AMT_CPU_68030 || !AMT_MMU()))
        return "requires a 68030 with its integrated MMU (not 68020, EC030 or 68040/68060)";
    if ((d->flags & M_040) && cpu < AMT_CPU_68EC040)
        return "requires 68040 or 68060 (MOVE16)";
    if ((d->flags & M_060) && cpu < AMT_CPU_68EC060)
        return "requires 68060 (MOVEC PCR)";
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
    if (d->flags & (M_CHIP | M_FAST | M_ROM))
        return BM_READ_BYTES;
    if (d->flags & M_EMPTY)
        return 16;
    return (d->flags & M_HALF) ? 512 : 1024;
}

/* chip RAM read with bitplane, copper and sprite DMA off (xsysinfo does the
 * same), so the result doesn't depend on the screen; restored afterwards */
static ULONG read_chip(ULONG units, void *mem)
{
    UWORD on = (UWORD)(CREG(R_DMACONR) & 0x01a0);
    ULONG r;
    CREG(R_DMACON) = 0x01a0;
    r = bm_mem_read(units, mem);
    CREG(R_DMACON) = (UWORD)(0x8000 | on);
    return r;
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
        b.per = per_unit(d);
        b.unit = (d->flags & (M_CHIP | M_FAST | M_ROM)) ? "byte" : "insn";
        b.mem = pool_kind();
        b.kset = 0;
        if (d->flags & M_CHIP) {
            b.run = read_chip;
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
