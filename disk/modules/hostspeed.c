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

AMT_MODULE(hostspeed, 26, AMT_PH_BARE, 0, 0, 1024);

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
void  hs_fpu_reset(void);
ULONG hs_pcr_get(void);
void  hs_pcr_set(ULONG);
ULONG hs_fadd(ULONG, void *);
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
 * reference at both ends; add_l and fadd: the class references the host
 * cost is compared with. */
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
    { "empty_end",      hs_empty,          0 },
};
#define N_BLOCKS (sizeof(blocks) / sizeof(blocks[0]))

static void *hs_mem;

/* why a block does not run on this machine, or 0 */
static const char *skip_why(const struct hs_blk *b)
{
    int cpu = AMT_CPU(), fpu = AMT_FPU();
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
    if ((b->flags & B_RMW) && amt_any_pool_is_chip())
        return "no non-chip RAM: chip RAM has no indivisible read-modify-write cycle";
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
static ULONG size_units(hs_fn fn, ULONG target)
{
    ULONG u = 1, t, r;
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
    if (u > 0x1fffffUL)             /* n = units x 1024 must stay below 2^31 (a */
        u = 0x1fffffUL;             /* JIT at cpu_speed=max): a shorter block */
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
}

static void run_block(const struct hs_blk *b, ULONG idx, ULONG target)
{
    ULONG units, empty, f0, f1, t0, t1;
    amt_watchdog_kick();
    units = size_units(b->fn, target);
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
}

void amt_run_hostspeed(void)
{
    ULONG i, target, pcr = 0;
    int fpu = AMT_FPU() != AMT_FPU_NONE;
    int is060 = AMT_CPU() >= AMT_CPU_68EC060;
    amt_vec_fn old_trap;

    hs_mem = amt_any_pool();
    target = (amt_cck_hz() / 1000) * HS_BLOCK_MS;
    /* A host that measures the blocks reads the mailbox; it acknowledges
     * this request once it follows the mailbox, so the first block is
     * sampled from its start. Without such a host (real hardware, serial
     * only) the blocks run anyway after the timeout. */
    amt_request("wait", "hostspeed", 0, amt_secs(5));
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
        run_block(b, i, target);
    }
    if (fpu) {
        hs_fpu_reset();
        if (is060 && (pcr & 2))
            hs_pcr_set(pcr);
    }
    amt_vec_set(32, old_trap);
}
