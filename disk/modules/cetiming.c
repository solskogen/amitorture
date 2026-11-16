/* cetiming: cycle-level CPU and chip-bus timing (CONTRACT.md section 10, id 27).
 *
 * Small fixed loops (cetiming_asm.s), copied to chip RAM and timed with
 * CIA-B timer A in E-clock ticks. Each loop runs with two iteration counts
 * and the difference of the two times gives the cost of one iteration
 * without the fixed start/stop overhead. Results are `perf` records
 * (ops = extra iterations, ticks = extra E ticks); the host compares them
 * with real-hardware references on cycle-exact entries (host.cet_<name>
 * pseudo cases, runner cases.py). The guest never judges speed: it checks
 * only what is deterministic (loop layout, results, timer range, beam sync).
 *
 * The chip-write rows run with bitplane and/or sprite DMA on, synchronised
 * to the same beam line each run (display window, sprite range), to measure
 * the CPU's share of the chip bus.
 *
 * Hardware touched: CIA-B timer A (cia_save/cia_restore), BPLCON0, DDFSTRT/
 * STOP, DIWSTRT/STOP, BPL1MOD/2MOD, BPLxPT, SPRxPT, DMACON (BPLEN, SPREN),
 * CACR (020/030 CI) or CPUSHA (040/060) after copying the code. */
#include "mod.h"
#include "cia_hw.h"

AMT_MODULE(cetiming, 27, AMT_PH_BARE, AMT_MF_OWNS_DISPLAY, 4096, 0);

/* cetiming_asm.s: the blob and its labels (only addresses are used) */
extern const UBYTE cet_blob_start[], cet_blob_end[];
extern const UBYTE cet_chip_read[], cet_chip_write_a2[], cet_chip_write_a0[], cet_move_reg[], cet_lsl[],
    cet_mulu[], cet_dbra[], cet_move_pair[], cet_move_pair_dep[];
extern const UBYTE cet_l_chip_read[], cet_l_chip_write_a2[], cet_l_chip_write_a0[], cet_l_move_reg[],
    cet_l_lsl[], cet_l_mulu[], cet_l_dbra[], cet_l_move_pair[], cet_l_move_pair_dep[];
/* cetiming_040.s */
void cet_cacr_or(ULONG bits);
void cet_cpusha(void);

struct cet_blk {
    ULONG tgt, d1, d2, d3, d5;          /* inputs (tgt: chip word address) */
    ULONG o_d0, o_d1, o_d3, o_d5;       /* outputs */
};
typedef ULONG (*cet_fn)(ULONG count, struct cet_blk *b);

#define CUST(off) (*(volatile UWORD *)(0xdff000UL + (off)))
#define CUSTL(off) (*(volatile ULONG *)(0xdff000UL + (off)))
#define R_DMACON  0x096
#define R_DIWSTRT 0x08e
#define R_DIWSTOP 0x090
#define R_DDFSTRT 0x092
#define R_DDFSTOP 0x094
#define R_BPL1PT  0x0e0
#define R_BPLCON0 0x100
#define R_BPL1MOD 0x108
#define R_BPL2MOD 0x10a
#define R_SPR0PT  0x120
#define R_SPR0CTL 0x142

#define IN_D1 0x12345a3cUL
#define IN_D2 0x7777c35aUL
#define IN_D3 0x00000081UL
#define IN_D5 0x00001234UL
#define TGT_INIT 0xa5c3

/* loops: entry, loop label, offset of the loop body mod 4 (the 68020's
 * cached dbra costs one clock more when its opcode word is longword
 * aligned; the references were measured with these alignments) */
enum { F_CHIP_READ, F_CW_A2, F_CW_A0, F_MOVE_REG, F_LSL, F_MULU, F_DBRA, F_PAIR, F_PAIR_DEP, F_N };
static const struct {
    const char *name;
    const UBYTE *entry, *loop;
    UBYTE mod4;
} cet_loops[F_N] = {
    {"chip_read", cet_chip_read, cet_l_chip_read, 0},
    {"chip_write_a2", cet_chip_write_a2, cet_l_chip_write_a2, 2},
    {"chip_write_a0", cet_chip_write_a0, cet_l_chip_write_a0, 0},
    {"move_reg", cet_move_reg, cet_l_move_reg, 2},
    {"lsl", cet_lsl, cet_l_lsl, 2},
    {"mulu", cet_mulu, cet_l_mulu, 2},
    {"dbra", cet_dbra, cet_l_dbra, 0},
    {"move_pair", cet_move_pair, cet_l_move_pair, 0},
    {"move_pair_dep", cet_move_pair_dep, cet_l_move_pair_dep, 2},
};

/* display DMA during a row */
enum { D_NONE, D_SYNC, D_6PL, D_SPR, D_6PL_SPR, D_3PL };

/* the measured rows; `name` is the perf record name the host looks up */
static const struct {
    const char *name;
    UBYTE fn, dma;
} cet_rows[] = {
    {"chip_read", F_CHIP_READ, D_NONE},
    {"chip_write", F_CW_A2, D_NONE},
    {"move_reg", F_MOVE_REG, D_NONE},
    {"lsl_l_8", F_LSL, D_NONE},
    {"mulu_imm", F_MULU, D_NONE},
    {"dbra", F_DBRA, D_NONE},
    {"move_pair", F_PAIR, D_NONE},
    {"move_pair_dep", F_PAIR_DEP, D_NONE},
    {"chip_write_nodma", F_CW_A0, D_SYNC},
    {"chip_write_6pl", F_CW_A2, D_6PL},
    {"chip_write_spr", F_CW_A0, D_SPR},
    {"chip_write_6pl_spr", F_CW_A2, D_6PL_SPR},
    {"chip_write_3pl", F_CW_A2, D_3PL},
};
#define NROWS (sizeof(cet_rows) / sizeof(cet_rows[0]))

/* iteration counts: the difference (4096 / 1024) is what is reported. The
 * largest run must stay inside the 16-bit timer: 4608 x mulu on a 68000 is
 * about 32000 ticks. */
#define CPU_N1 512
#define CPU_N2 4608
#define DMA_N1 256
#define DMA_N2 1280
#define SYNC_LINE 96        /* inside the display window and the sprites' lines */
#define SPR_VSTART 0x50
#define SPR_VSTOP 0xb4

static UBYTE *P;            /* chip pool: code, then sprite data, then the target word */
static UWORD *spr;
static volatile UWORD *tgt;
static struct cia_saved cs;
static UBYTE overflow_row = 0xff, sync_row = 0xff;
static UWORD sync_vpos;

static cet_fn fn_at(int f)
{
    return (cet_fn)(P + (cet_loops[f].entry - cet_blob_start));
}

/* one timed run at SR=$2700; returns elapsed E ticks, or -1 if the timer ran out */
static long run_once(int f, ULONG n, struct cet_blk *b)
{
    UWORD sr = amt_get_sr();
    ULONG left;
    UBYTE icr;
    cia_wr(CIAB_BASE, C_CRA, 0x00);         /* stopped, continuous: TAHI write loads */
    cia_wr(CIAB_BASE, C_TALO, 0xff);
    cia_wr(CIAB_BASE, C_TAHI, 0xff);
    (void)cia_rd(CIAB_BASE, C_ICR);
    b->tgt = (ULONG)tgt;
    b->d1 = IN_D1;
    b->d2 = IN_D2;
    b->d3 = IN_D3;
    b->d5 = IN_D5;
    amt_set_sr(0x2700);
    left = fn_at(f)(n - 1, b);
    amt_set_sr(sr);
    icr = cia_rd(CIAB_BASE, C_ICR);
    cia_wr(CIAB_BASE, C_CRA, 0x00);
    if (icr & 0x01)
        return -1;
    return 0xffffUL - (left & 0xffff);
}

static void dma_setup(int dma)
{
    if (dma == D_6PL || dma == D_6PL_SPR || dma == D_3PL) {
        CUST(R_BPLCON0) = dma == D_3PL ? 0x3200 : 0x6200;
        CUST(R_DDFSTRT) = 0x0038;
        CUST(R_DDFSTOP) = 0x00d0;
        CUST(R_DIWSTRT) = 0x2c81;
        CUST(R_DIWSTOP) = 0x2cc1;
        CUST(R_BPL1MOD) = 0;
        CUST(R_BPL2MOD) = 0;
    }
}

/* every bitplane at the pool (only read), every sprite at the one sprite block */
static void dma_pointers(int dma)
{
    int i;
    if (dma == D_6PL || dma == D_6PL_SPR || dma == D_3PL)
        for (i = 0; i < 6; i++)
            CUSTL(R_BPL1PT + 4 * i) = (ULONG)P;
    if (dma == D_SPR || dma == D_6PL_SPR)
        for (i = 0; i < 8; i++)
            CUSTL(R_SPR0PT + 4 * i) = (ULONG)spr;
}

static void dma_off(void)
{
    int i;
    CUST(R_DMACON) = 0x0320;
    CUST(R_BPLCON0) = 0x0200;
    for (i = 0; i < 8; i++)
        CUST(R_SPR0CTL + 8 * i) = 0;
}

static UWORD vpos(void)
{
    return (UWORD)(amt_beam() >> 8);
}

/* frame top (a line below 20 reached from a later line), pointers reloaded,
 * then SYNC_LINE. Runs at SR=$2000 (watchdog live); bounded by the frame
 * counter. Returns 0 if the beam did not get there. */
static int beam_sync(int dma)
{
    ULONG f0 = amt_frames_now();
    while (vpos() < 20)
        if (amt_frames_now() - f0 > 4)
            return 0;
    while (vpos() >= 20)
        if (amt_frames_now() - f0 > 4)
            return 0;
    dma_pointers(dma);
    while (vpos() < SYNC_LINE)
        if (amt_frames_now() - f0 > 4)
            return 0;
    return 1;
}

static int row_run(int r, ULONG n, struct cet_blk *b, long *best)
{
    int dma = cet_rows[r].dma;
    long t;
    if (dma != D_NONE && !beam_sync(dma)) {
        if (sync_row == 0xff) {
            sync_row = r;
            sync_vpos = vpos();
        }
        return 0;
    }
    t = run_once(cet_rows[r].fn, n, b);
    if (t < 0) {
        if (overflow_row == 0xff)
            overflow_row = r;
        return 0;
    }
    if (*best < 0 || t < *best)
        *best = t;
    return 1;
}

/* result checks (the last N2 run of each row); 0 if fine */
static ULONG mulu_seq(ULONG d5, ULONG iters)
{
    while (iters--)
        d5 = (d5 & 0xffff) * 0x5555UL;
    return d5;
}

static UBYTE res_row = 0xff;
static const char *res_what;
static ULONG res_expect, res_got;

static void check_result(int r, ULONG n, const struct cet_blk *b)
{
    ULONG e = 0, g = 0;
    const char *what = 0;
    switch (cet_rows[r].fn) {
    case F_CHIP_READ:
        what = "d0", e = TGT_INIT, g = b->o_d0;
        break;
    case F_CW_A2:
    case F_CW_A0:
        what = "target", e = IN_D1 & 0xffff, g = *tgt;
        break;
    case F_MOVE_REG:
        what = "d0", e = IN_D2 & 0xffff, g = b->o_d0;
        break;
    case F_LSL:
        what = "d3", e = 0, g = b->o_d3;     /* 8 bits per iteration, n >= 4 */
        break;
    case F_MULU:
        what = "d5", e = mulu_seq(IN_D5, n), g = b->o_d5;
        break;
    case F_PAIR:
        what = "d1", e = (IN_D1 & 0xffff0000UL) | (IN_D3 & 0xffff), g = b->o_d1;
        if (g == e)
            what = "d0", e = IN_D2 & 0xffff, g = b->o_d0;
        break;
    case F_PAIR_DEP:
        what = "d1", e = (IN_D1 & 0xffff0000UL) | (IN_D2 & 0xffff), g = b->o_d1;
        break;
    default:
        return;
    }
    if (e != g && res_row == 0xff) {
        res_row = r;
        res_what = what;
        res_expect = e;
        res_got = g;
    }
}

static void layout_case(void)
{
    int f, bad = -1;
    for (f = 0; f < F_N; f++)
        if (((cet_loops[f].loop - cet_blob_start) & 3) != cet_loops[f].mod4) {
            bad = f;
            break;
        }
    if (bad < 0) {
        amt_case("layout", AMT_ST_PASS, 0);
        return;
    }
    amt_case_open("layout", AMT_ST_FAIL);
    json_obj("expect");
    json_int("mod4", cet_loops[bad].mod4);
    json_obj_end();
    json_obj("got");
    json_int("mod4", (cet_loops[bad].loop - cet_blob_start) & 3);
    json_obj_end();
    json_str("why", "a loop body is not at the longword alignment of the reference (cetiming_asm.s CET_ALIGN)");
    json_obj("in");
    json_str("loop", cet_loops[bad].name);
    json_int("offset", cet_loops[bad].loop - cet_blob_start);
    json_obj_end();
    amt_case_close();
}

void amt_run_cetiming(void)
{
    ULONG size = cet_blob_end - cet_blob_start;
    struct cet_blk b;
    ULONG i;
    unsigned r;
    int k;

    P = (UBYTE *)amt_chip_pool();
    for (i = 0; i < size; i++)
        P[i] = cet_blob_start[i];
    spr = (UWORD *)(P + ((size + 15) & ~15UL));
    spr[0] = (SPR_VSTART << 8) | 0x40;
    spr[1] = SPR_VSTOP << 8;
    for (i = 0; i < 2 * (SPR_VSTOP - SPR_VSTART); i++)
        spr[2 + i] = 0x5555;
    spr[2 + i] = 0;
    spr[3 + i] = 0;
    tgt = spr + 4 + i;
    if (AMT_CPU() >= AMT_CPU_68EC040)
        cet_cpusha();
    else if (AMT_CPU() >= AMT_CPU_68EC020)
        cet_cacr_or(0x8);

    layout_case();

    cia_save(&cs, CIAB_BASE, 0);
    for (r = 0; r < NROWS; r++) {
        int dma = cet_rows[r].dma;
        ULONG n1 = dma == D_NONE ? CPU_N1 : DMA_N1, n2 = dma == D_NONE ? CPU_N2 : DMA_N2;
        long t1 = -1, t2 = -1;
        int ok = 1;
        if (dma > D_SYNC) {
            dma_setup(dma);
            CUST(R_DMACON) = dma == D_SPR ? 0x8220 : dma == D_6PL_SPR ? 0x8320 : 0x8300;
        }
        *tgt = TGT_INIT;
        row_run(r, n1, &b, &t1);              /* warm-up (caches, first sprite fetch) */
        t1 = -1;
        for (k = 0; k < 3 && ok; k++) {
            *tgt = TGT_INIT;
            ok = row_run(r, n1, &b, &t1);
            *tgt = TGT_INIT;
            ok = ok && row_run(r, n2, &b, &t2);
        }
        if (ok)
            check_result(r, n2, &b);
        if (dma > D_SYNC)
            dma_off();
        if (ok)
            amt_perf_hz(cet_rows[r].name, n2 - n1, t2 > t1 ? (ULONG)(t2 - t1) : 0, amt_eclk_hz(), "eclock");
        amt_watchdog_kick();
    }
    cia_restore(&cs);

    if (overflow_row == 0xff) {
        amt_case("timer_no_overflow", AMT_ST_PASS, 0);
    } else {
        amt_case_open("timer_no_overflow", AMT_ST_FAIL);
        json_obj("expect");
        json_bool("underflow", 0);
        json_obj_end();
        json_obj("got");
        json_bool("underflow", 1);
        json_obj_end();
        json_str("why", "a timed run took longer than 65535 E ticks (CIA-B timer A one-shot underflowed)");
        json_obj("in");
        json_str("row", cet_rows[overflow_row].name);
        json_obj_end();
        amt_case_close();
    }

    if (sync_row == 0xff) {
        amt_case("beam_sync", AMT_ST_PASS, 0);
    } else {
        amt_case_open("beam_sync", AMT_ST_FAIL);
        json_obj("expect");
        json_int("vpos_min", SYNC_LINE);
        json_obj_end();
        json_obj("got");
        json_int("vpos", sync_vpos);
        json_obj_end();
        json_str("why", "the beam did not reach the measurement line within 4 frames (beam or VERTB stuck)");
        json_obj("in");
        json_str("row", cet_rows[sync_row].name);
        json_obj_end();
        amt_case_close();
    }

    if (res_row == 0xff) {
        amt_case("loop_results", AMT_ST_PASS, 0);
    } else {
        amt_case_open("loop_results", AMT_ST_FAIL);
        json_obj("expect");
        json_hex(res_what, res_expect);
        json_obj_end();
        json_obj("got");
        json_hex(res_what, res_got);
        json_obj_end();
        json_str("why", "a timed loop computed a wrong result (MC68000 PRM: MOVE, LSL, MULU, DBcc)");
        json_obj("in");
        json_str("row", cet_rows[res_row].name);
        json_hex("d1", IN_D1);
        json_hex("d2", IN_D2);
        json_hex("d3", IN_D3);
        json_hex("d5", IN_D5);
        json_obj_end();
        amt_case_close();
    }
}
