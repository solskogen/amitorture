/* timing: a throughput and timing fingerprint (CONTRACT.md section 10, id 10).
 *
 * 1. Fixed CPU loops (timing_asm.s, timing_fpu.s), each run once to warm up
 *    and then three times measured, in CIA E-clock ticks (CIA-B timer A+B
 *    chained into a 32-bit down counter) and in colour clocks (amt_ticks()).
 *    They become `perf` records: no pass/fail on speed; the host compares
 *    bands and orderings. The loop results themselves are deterministic and
 *    ARE checked (loop_result_*).
 * 2. Deterministic sanity cases from the Amiga Hardware Reference Manual:
 *    E clock = colour clock / 5, VBL rate per CIA second, lines per frame
 *    (LOF, interlace alternation) and colour clocks per line (H wrap).
 *
 * Hardware touched: CIA-B timers A and B, saved and restored with
 * cia_save()/cia_restore() (cia_hw.h: latch estimate, ICR mask drained and
 * OS events re-posted), BPLCON0 (LACE; left at $0200 = no bitplanes,
 * colour on), INTREQ VERTB while polling at SR=$2700.
 *
 * Every poll loop that runs at SR=$2700 (the watchdog cannot fire there) is
 * bounded: by the other clock (beam loops by the CIA E clock, CIA loops by
 * the beam) and, if both clocks stand still, by a poll count. A loop that
 * hits its bound makes its case fail with "beam/VERTB/CIA stuck". */
#include <exec/execbase.h>
#include <exec/nodes.h>
#include "mod.h"
#include "cia_hw.h"

AMT_MODULE(timing, 10, AMT_PH_BARE, AMT_MF_OWNS_DISPLAY, 4096, 4096);

/* timing_asm.s / timing_fpu.s */
ULONG tm_dbra(ULONG outer);
ULONG tm_movereg(ULONG outer);
ULONG tm_movemem(ULONG outer, void *src, void *dst);
ULONG tm_mulu(ULONG outer);
ULONG tm_divu(ULONG outer);
ULONG tm_branch(ULONG outer);
ULONG tm_fpu(ULONG outer);
ULONG tm_hscan(ULONG outer);

/* CIA-B (8520), registers on the even byte lane */
#define CIAB(off) (*(volatile UBYTE *)(0xbfd000UL + (off)))

#define R_BPLCON0 0x100

static struct cia_saved tm_cs;   /* CIA-B as found (cia_hw.h) */
static long tm_hz;           /* E-clock Hz */

/* 32-bit down counter: timer B counts timer A underflows.
 * TB counts a TA underflow a little after TA has reloaded $FFFF (the 8520
 * data sheet gives no latency; Amiberry passes it through a 2-E-clock input
 * pipe, cia.cpp process_pipe). A fast CPU can do all seven reads inside
 * that window and get the old TB with the new TA, a value 65536 too high,
 * which made the E-clock bound of a poll loop expire at once ("beam stuck"
 * with negative e_ticks on JIT twins). So a read with TA within 16 E clocks
 * of the reload is repeated until TA has moved on (bounded, in case the CIA
 * stands still). */
static ULONG ec_read(void)
{
    UBYTE bh, bl, ah, al, ah2, bh2, bl2;
    UWORD n = 0;
    for (;;) {
        bh = CIAB(C_TBHI);
        bl = CIAB(C_TBLO);
        ah = CIAB(C_TAHI);
        al = CIAB(C_TALO);
        ah2 = CIAB(C_TAHI);
        bh2 = CIAB(C_TBHI);
        bl2 = CIAB(C_TBLO);
        if (ah != ah2 || bh != bh2 || bl != bl2)
            continue;
        if (ah == 0xff && al >= 0xf0 && ++n < 4000)
            continue;
        break;
    }
    return ((ULONG)bh << 24) | ((ULONG)bl << 16) | ((ULONG)ah << 8) | al;
}

static void ec_start(void)
{
    /* saves CR, latch estimate and the OS ICR mask; leaves both timers
     * stopped, continuous, and ICR masked and drained */
    cia_save(&tm_cs, CIAB_BASE, 0);
    CIAB(C_TALO) = 0xff;
    CIAB(C_TAHI) = 0xff;
    CIAB(C_TBLO) = 0xff;
    CIAB(C_TBHI) = 0xff;
    CIAB(C_CRA) = 0x10;                     /* force load */
    CIAB(C_CRB) = 0x40 | 0x10;              /* B counts A underflows */
    CIAB(C_CRB) = 0x40 | 0x01;              /* start B, then A */
    CIAB(C_CRA) = 0x01;
}

/* returns 1 if CRA/CRB read back as saved (a one-shot timer that was
 * running may have stopped by itself, so START is not compared then) */
static int ec_stop(void)
{
    UBYTE ma = (tm_cs.cra & 0x08) ? 0xee : 0xef, mb = (tm_cs.crb & 0x08) ? 0xee : 0xef;
    cia_restore(&tm_cs);
    return (CIAB(C_CRA) & ma) == (tm_cs.cra & ma) && (CIAB(C_CRB) & mb) == (tm_cs.crb & mb);
}

/* ---- bounds for the poll loops at SR=$2700
 * A loop expires when `elimit` E ticks have passed since tm_guard_init(),
 * or when neither the E counter nor the caller's progress value `v`
 * (beam line, VBL count...) changed over tm_stuck_polls polls (both clocks
 * stuck). The caller calls it every TM_GUARD_EVERY polls through a local
 * countdown (TM_GUARD), so a 68000 poll loop keeps its rate.
 *
 * The poll bound is only a last resort against an emulator whose beam AND
 * CIA both stopped for good. Nothing bounds how many instructions a CPU may
 * execute while chipset time stands still: Amiberry's non-cycle-exact
 * cores (JIT, cpu_speed=max) keep only the CPU running while they wait for
 * the host's frame time (events.cpp event_check_vsync), so a fast
 * emulated CPU can legitimately see both clocks frozen for millions of
 * polls. The bound is therefore generous, and larger for CPUs that can be
 * that fast (on a real 68000 4M polls are minutes). */
#define TM_GUARD_EVERY 256
static ULONG tm_stuck_polls = 4000000UL;
#define TM_STUCK_POLLS tm_stuck_polls
struct tm_guard {
    ULONG still, e0, elast, vlast, elimit;
};
#define TM_GUARD(cnt, g, v) (--(cnt) == 0 && ((cnt) = TM_GUARD_EVERY, tm_guard_expired((g), (v))))

static void tm_guard_init(struct tm_guard *g, ULONG elimit)
{
    g->still = 0;
    g->e0 = g->elast = ec_read();
    g->vlast = 0xffffffffUL;
    g->elimit = elimit;
}

/* which bound expired last (for the stuck_case record) */
static UBYTE tm_why;            /* 1 = E-clock limit, 2 = no clock moved */
static ULONG tm_why_e, tm_why_still;

static int tm_guard_expired(struct tm_guard *g, ULONG v)
{
    ULONG e = ec_read();
    /* (LONG) < 0: the counter read back above the start value, i.e. a TB/TA
     * read in the cascade window (see ec_read); not elapsed time */
    if ((LONG)(g->e0 - e) > 0 && g->e0 - e > g->elimit) {
        tm_why = 1;
        tm_why_e = g->e0 - e;
        tm_why_still = g->still;
        return 1;
    }
    if (e != g->elast || v != g->vlast) {
        g->still = 0;
        g->elast = e;
        g->vlast = v;
    } else if ((g->still += TM_GUARD_EVERY) > TM_STUCK_POLLS) {
        tm_why = 2;
        tm_why_e = g->e0 - e;
        tm_why_still = g->still;
        return 1;
    }
    return 0;
}

/* E ticks for n frames (generous: n frames take n*14188 E ticks PAL) */
#define TM_FRAMES_E(n) ((ULONG)(n) * 15000UL)

static void stuck_case(const char *id, const char *what, const char *why)
{
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_str("progress", what);
    json_obj_end();
    json_obj("got");
    json_str("progress", "none within the bound");
    json_obj_end();
    json_str("why", why);
    json_obj("timing");
    json_str("bound", tm_why == 1 ? "E-clock limit" : tm_why == 2 ? "no clock moved over the poll bound" : "other");
    json_int("e_ticks", (long)tm_why_e);
    json_int("still_polls", (long)tm_why_still);
    json_int("poll_bound", (long)TM_STUCK_POLLS);
    json_obj_end();
    amt_case_close();
    tm_why = 0;
}

#define WHY_BEAM_STUCK "beam stuck: the vertical beam counter (VPOSR/VHPOSR) did not wrap within the bound (CIA-B E clock and a poll count); the frame never ended (HRM ch.6 beam counter)"

/* vertical beam position (V10..V0), consistent across the VPOSR/VHPOSR
 * pair; *lof gets VPOSR bit 15 from the same read */
static ULONG beam_vl(UWORD *lof)
{
    UWORD a, b, a2;
    do {
        a = CREG(R_VPOSR);
        b = CREG(R_VHPOSR);
        a2 = CREG(R_VPOSR);
    } while ((a & 0x8007) != (a2 & 0x8007));
    if (lof)
        *lof = a2 & 0x8000;
    return ((ULONG)(a & 7) << 8) | (b >> 8);
}
#define beam_v() beam_vl(0)

/* ------------------------------------------------------------ perf loops */
struct loop {
    const char *name;
    ULONG outer;
    ULONG ops_per_outer;
    int kind;           /* 0 plain, 1 mem chip, 2 mem any */
    ULONG (*fn)(ULONG);
};

static ULONG run_loop(const struct loop *l, ULONG outer)
{
    UBYTE *p;
    if (l->kind == 1) {
        p = amt_chip_pool();
        return tm_movemem(outer, p, p + 2048);
    }
    if (l->kind == 2) {
        p = amt_any_pool();
        return tm_movemem(outer, p, p + 2048);
    }
    return l->fn(outer);
}

static void perf_loop(const struct loop *l)
{
    ULONG e0, e1, c0, c1, best_e = 0xffffffffUL, best_c = 0, e[3], c[3];
    int i;

    run_loop(l, 1);                     /* warm-up: caches, JIT */
    for (i = 0; i < 3; i++) {
        c0 = amt_ticks();
        e0 = ec_read();
        run_loop(l, l->outer);
        e1 = ec_read();
        c1 = amt_ticks();
        e[i] = e0 - e1;
        c[i] = c1 - c0;
        if (e[i] < best_e) {
            best_e = e[i];
            best_c = c[i];
        }
        amt_watchdog_kick();
    }
    json_begin("perf");
    json_str("mod", "timing");
    json_str("name", l->name);
    json_int("ops", (long)(l->outer * l->ops_per_outer));
    json_int("ticks", (long)best_e);
    json_int("tick_hz", tm_hz);
    json_int("cck", (long)best_c);
    json_int("cck_hz", (long)amt_eclock_hz());
    json_arr("runs_ticks");
    for (i = 0; i < 3; i++)
        json_int(NULL, (long)e[i]);
    json_arr_end();
    json_arr("runs_cck");
    for (i = 0; i < 3; i++)
        json_int(NULL, (long)c[i]);
    json_arr_end();
    json_end();
}

static ULONG model_branch(ULONG outer)
{
    ULONG s = 0x12345678UL, d = 0, o, i, c;
    for (o = 0; o < outer; o++)
        for (i = 0; i < 256; i++) {
            c = s & 1;
            s >>= 1;
            if (c) {
                s ^= 0x80200003UL;
                d += 3;
            }
            if (s & 4)
                d++;
            else
                d--;
        }
    return d;
}

static ULONG hot_outer;
static ULONG hot_mulu(ULONG i) { (void)i; return tm_mulu(hot_outer); }
static ULONG hot_divu(ULONG i) { (void)i; return tm_divu(hot_outer); }
static ULONG hot_branch(ULONG i) { (void)i; return tm_branch(hot_outer); }
static ULONG hot_fpu(ULONG i) { (void)i; return tm_fpu(hot_outer); }

static void check_loop(const char *id, ULONG (*fn)(ULONG), ULONG expect,
                       const char *what)
{
    ULONG first = 0, bad = 0;
    long r;
    hot_outer = 1;
    r = amt_hot(fn, AMT_QUICK() ? 20 : 100, &first, &bad);
    if (r < 0 && first == expect) {
        amt_case(id, AMT_ST_PASS, 0);
        return;
    }
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_hex("result", expect);
    json_obj_end();
    json_obj("got");
    json_hex("result", r < 0 ? first : bad);
    if (r >= 0) {
        json_hex("first", first);
        json_int("iteration", r);
    }
    json_obj_end();
    json_str("why", what);
    json_obj("in");
    json_int("outer", 1);
    json_obj_end();
    amt_case_close();
}

/* ------------------------------------------------------------ beam tests */
#define NFR 8

/* Frame lengths come from the copper, not from polling the beam: a CPU
 * poll of VPOSR may miss a whole line (a 68000 poll iteration is ~0.75
 * line; a non-cycle-exact core may not run the CPU for several lines), so
 * "highest line seen + 1" is not a reliable frame length. The copper is
 * beam-synchronous: it restarts at COP1LC every vertical blank (HRM ch.2),
 * waits for line L-1 and sets INTREQ DSKBLK, then for line L and sets SOFT
 * (L = 312 PAL / 262 NTSC; lines above 255 are reached through the $FFDF
 * wait). A frame of L lines (0..L-1) shows DSKBLK only, one of L+1 lines
 * (0..L) DSKBLK|SOFT. The CPU only has to notice each V wrap (V decreasing)
 * and read the two bits before line L-1 of the next frame; that is far
 * looser than seeing every line. At SR=$2700 the bits raise no interrupt;
 * they are cleared before the SR is lowered again. */
#define TM_COP_WORDS 12
static UWORD *tm_cop;
static UWORD tm_dmacon_cop;      /* COPEN as found */

static void tm_restore_copper(void)
{
    struct Node *n;
    CREG(R_DMACON) = 0x0080;                    /* copper off */
    CREG(R_INTREQ) = 0x0006;
    CREG(R_INTREQ) = 0x0006;
    for (n = SysBase->LibList.lh_Head; n && n->ln_Succ; n = n->ln_Succ) {
        const char *a = n->ln_Name, *b = "graphics.library";
        while (a && *a && *a == *b)
            a++, b++;
        if (a && *a == *b) {
            ULONG ci = *(ULONG *)((UBYTE *)n + 0x26);   /* GfxBase->copinit */
            if (ci)
                *(volatile ULONG *)(CUSTOM_BASE + 0x080) = ci;
            break;
        }
    }
    if (tm_dmacon_cop)
        CREG(R_DMACON) = 0x8000 | 0x0200 | 0x0080;
}

static void tm_start_copper(int pal)
{
    UWORD L = pal ? 312 : 262, *c;
    tm_cop = (UWORD *)((UBYTE *)amt_chip_pool() + 4096 - 2 * TM_COP_WORDS);
    c = tm_cop;
    *c++ = 0xffdf; *c++ = 0xfffe;                           /* line 255 */
    *c++ = (UWORD)(((L - 1 - 256) << 8) | 0x01); *c++ = 0xfffe;
    *c++ = R_INTREQ; *c++ = 0x8002;                         /* DSKBLK: line L-1 */
    *c++ = (UWORD)(((L - 256) << 8) | 0x01); *c++ = 0xfffe;
    *c++ = R_INTREQ; *c++ = 0x8004;                         /* SOFT: line L */
    *c++ = 0xffff; *c++ = 0xfffe;
    tm_dmacon_cop = CREG(R_DMACONR) & 0x0080;
    CREG(R_INTREQ) = 0x0006;
    *(volatile ULONG *)(CUSTOM_BASE + 0x080) = (ULONG)tm_cop;  /* COP1LC */
    CREG(0x088) = 0;                                        /* COPJMP1 */
    CREG(R_DMACON) = 0x8000 | 0x0200 | 0x0080;
}

/* sample `n` frames (wrap to wrap); lines[i] = frame length from the copper
 * marks (0 = neither mark: the copper did not get there), lof[i] = LOF bit
 * seen in that frame (bit 0: seen 1, bit 1: seen 0). Returns 0 if the beam
 * did not finish the frames within the bound (2n+4 frames of E clock).
 * Called at SR=$2700 with tm_start_copper() done. */
static int sample_frames(int n, UWORD *lines, UBYTE *lof, int pal)
{
    ULONG v, pv;
    UWORD p, m, L = pal ? 312 : 262;
    int i = -1;
    UWORD cnt = TM_GUARD_EVERY;
    struct tm_guard g;
    tm_guard_init(&g, TM_FRAMES_E(2 * n + 4));
    pv = beam_v();
    while (i < n) {
        v = beam_vl(&p);
        if (v < pv) {
            m = CREG(R_INTREQR) & 0x0006;
            CREG(R_INTREQ) = 0x0006;
            if (i >= 0)
                lines[i] = m == 0x0006 ? (UWORD)(L + 1) : m == 0x0002 ? L : 0;
            i++;
            if (i < n)
                lof[i] = 0;
        }
        if (i >= 0 && i < n)
            lof[i] |= p ? 1 : 2;
        if (TM_GUARD(cnt, &g, v))
            return 0;
        pv = v;
    }
    return 1;
}

static void lines_case(const char *id, const UWORD *lines, const UBYTE *lof,
                       int lace, int pal)
{
    int i, ok = 1;
    UWORD lng = pal ? 313 : 263, shrt = pal ? 312 : 262;
    UWORD exp[NFR];
    for (i = 0; i < NFR; i++) {
        /* the frame's length follows its LOF bit (HRM: VPOSR LOF) */
        exp[i] = (lof[i] == 1) ? lng : shrt;
        if (lof[i] == 3 || lines[i] != exp[i])
            ok = 0;
        if (lace && i > 0 && lof[i] == lof[i - 1])
            ok = 0;
        if (!lace && lof[i] != lof[0])
            ok = 0;
    }
    if (ok) {
        amt_case(id, AMT_ST_PASS, 0);
        return;
    }
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_str("rule", lace ? "LOF toggles every frame; long frame = LOF 1"
                          : "LOF constant; long frame = LOF 1");
    json_int("long", lng);
    json_int("short", shrt);
    json_arr("lines");
    for (i = 0; i < NFR; i++)
        json_int(NULL, exp[i]);
    json_arr_end();
    json_obj_end();
    json_obj("got");
    json_arr("lines");
    for (i = 0; i < NFR; i++)
        json_int(NULL, lines[i]);
    json_arr_end();
    json_arr("lof");
    for (i = 0; i < NFR; i++)
        json_int(NULL, lof[i] == 1 ? 1 : lof[i] == 2 ? 0 : -1);
    json_arr_end();
    json_obj_end();
    json_str("why", lace
        ? "interlace (BPLCON0 LACE): frames must alternate long/short (PAL 313/312, NTSC 263/262 lines), VPOSR LOF following (HRM ch.3 interlace, VPOSR)"
        : "non-interlaced: every frame has the length its LOF bit says (PAL 313/312, NTSC 263/262), and LOF does not toggle (HRM VPOSR/VPOSW)");
    json_str("method", "frame length from copper marks: INTREQ DSKBLK set by a copper WAIT on line 311/261, SOFT on line 312/262, read and cleared at each V wrap (0 = neither mark)");
    json_obj("ctx");
    json_int("pal", pal);
    json_hexn("bplcon0", lace ? 0x0204 : 0x0200, 4);
    json_obj_end();
    amt_case_close();
}

/* VHPOSR H sampling for cck_per_line: blocks of 1000 samples (tm_hscan)
 * until at least TM_HS_LINES beam lines have passed (counted from V between
 * blocks; a block lasts far less than a frame) and at least TM_HS_MIN_BLOCKS
 * blocks were taken. Covering whole frames, not a fixed sample count, makes
 * the coverage independent of how fast the CPU polls compared to the beam.
 * Returns 0 if the beam stood still (bound: CIA E clock / poll count). */
#define TM_HS_FRAMES     8
#define TM_HS_MIN_BLOCKS 100
static int hscan_frames(UWORD *hmax, UWORD *hmin, ULONG *blocks, ULONG *nlines)
{
    ULONG lpf = amt_probe_result.pal ? 313 : 263;
    ULONG want = TM_HS_FRAMES * lpf, done = 0, nb = 0, v, pv, r, e0, still = 0;
    UWORD mx = 0, mn = 0xff;
    pv = beam_v();
    e0 = ec_read();
    for (;;) {
        r = tm_hscan(1);
        nb++;
        if (((r >> 8) & 0xff) > mx)
            mx = (UWORD)((r >> 8) & 0xff);
        if ((r & 0xff) < mn)
            mn = (UWORD)(r & 0xff);
        v = beam_v();
        if (v != pv) {
            done += (v + lpf - pv) % lpf;
            still = 0;
        } else if (++still > TM_STUCK_POLLS / 1000) {
            /* no line change over the poll bound (in samples; see the
             * bounds comment: chipset time may stand still while a fast
             * CPU runs) */
            tm_why = 2;
            tm_why_still = still * 1000;
            tm_why_e = e0 - ec_read();
            break;
        }
        pv = v;
        if (done >= want && nb >= TM_HS_MIN_BLOCKS)
            break;
        if (e0 - ec_read() > TM_FRAMES_E(150)) {
            tm_why = 1;                 /* 3 s: beam far slower than the CIA */
            tm_why_e = e0 - ec_read();
            tm_why_still = still * 1000;
            break;
        }
    }
    *hmax = mx;
    *hmin = mn;
    *blocks = nb;
    *nlines = done;
    return done >= want;
}

static int hscan_status(UWORD hmax, UWORD hmin, UWORD expected_max)
{
    if (hmax > expected_max)
        return AMT_ST_FAIL;
    if (hmin == 0 && hmax >= expected_max - 1)
        return AMT_ST_PASS;
    return AMT_ST_UNTESTABLE;
}

static void beam_tests(void)
{
    UWORD lines[NFR];
    UBYTE lof[NFR];
    int pal = amt_probe_result.pal;
    UWORD hmax, hmin;
    ULONG n, blocks, nlines;
    UWORD exp_hmax = pal ? 0xe2 : 0xe3;
    int ok, status;

    amt_set_sr(0x2700);
    tm_start_copper(pal);

    /* interlace: LOF must alternate */
    CREG(R_BPLCON0) = 0x0204;
    ok = sample_frames(NFR, lines, lof, pal);
    /* leave interlace during a long frame so later frames stay long */
    n = 0;
    while (ok && !(CREG(R_VPOSR) & 0x8000) && n < 2000000UL)
        n++;
    CREG(R_BPLCON0) = 0x0200;
    tm_restore_copper();                /* no marks while the SR is low */
    amt_set_sr(0x2000);
    if (ok)
        lines_case("lines_per_frame_lace", lines, lof, 1, pal);
    else
        stuck_case("lines_per_frame_lace", "8 beam frames (V wraps)", WHY_BEAM_STUCK);
    amt_set_sr(0x2700);

    tm_start_copper(pal);
    ok = sample_frames(NFR, lines, lof, pal);
    tm_restore_copper();
    amt_set_sr(0x2000);
    if (ok)
        lines_case("lines_per_frame_nolace", lines, lof, 0, pal);
    else
        stuck_case("lines_per_frame_nolace", "8 beam frames (V wraps)", WHY_BEAM_STUCK);
    amt_set_sr(0x2700);

    /* colour clocks per line: the H counter runs 0..$E2 (227 cck, PAL) or
     * up to $E3 on NTSC long lines (227/228 alternating; HRM VHPOSR) */
    ok = hscan_frames(&hmax, &hmin, &blocks, &nlines);
    amt_set_sr(0x2000);
    if (!ok) {
        stuck_case("cck_per_line", "8 frames of beam lines while sampling VHPOSR", WHY_BEAM_STUCK);
        return;
    }
    /* Polls need not visit every H position, even across whole frames:
     * instruction/block timing can quantize every access to the same phases.
     * An out-of-range value is a failure; missing endpoints is insufficient
     * coverage, not evidence that the hardware counter missed them. */
    status = hscan_status(hmax, hmin, exp_hmax);
    if (status == AMT_ST_PASS) {
        amt_case_open("cck_per_line", AMT_ST_PASS);
        json_obj("timing");
        json_hexn("hmax_seen", hmax, 2);
        json_hexn("hmin_seen", hmin, 2);
        json_int("samples", (long)(blocks * 1000));
        json_int("lines", (long)nlines);
        json_obj_end();
        amt_case_close();
    } else {
        amt_case_open("cck_per_line", status);
        json_obj("expect");
        json_hexn("hmax", exp_hmax, 2);
        json_hexn("hmin", 0, 2);
        json_obj_end();
        json_obj("got");
        json_hexn("hmax", hmax, 2);
        json_hexn("hmin", hmin, 2);
        json_obj_end();
        json_str("why", hmax > exp_hmax
                 ? (pal ? "VHPOSR H exceeds $E2 on a PAL line (227 colour clocks; HRM ch.6 beam position)"
                        : "VHPOSR H exceeds $E3 on an NTSC long line (228 colour clocks; HRM, LOL)")
                 : "CPU polling did not cover both ends of the VHPOSR H range; access timing can "
                   "skip these positions even over whole frames, so the line-end check is inconclusive");
        json_obj("ctx");
        json_int("pal", pal);
        json_int("frames", TM_HS_FRAMES);
        json_obj_end();
        json_obj("timing");
        json_int("samples", (long)(blocks * 1000));
        json_int("lines", (long)nlines);
        json_obj_end();
        amt_case_close();
    }
}

/* E clock vs colour clock over K frames, and VBL per E-second.
 * The CPU notices a V wrap some time after it happened (one poll on a
 * 68000; on a non-cycle-exact core possibly several lines, when the CPU
 * did not run). That delay is read from the beam itself: right after the
 * E-counter read at a detected wrap, VPOSR/VHPOSR give how far into the
 * new frame the beam already is, and that many colour clocks are taken off
 * the E-clock interval. The result no longer depends on how fast the CPU
 * polls. */
static ULONG beam_cck2(int pal)
{
    UWORD a, b, a2;
    ULONG v;
    do {
        a = CREG(R_VPOSR);
        b = CREG(R_VHPOSR);
        a2 = CREG(R_VPOSR);
    } while ((a & 7) != (a2 & 7));
    v = ((ULONG)(a & 7) << 8) | (b >> 8);
    /* x2 colour clocks since line 0 (NTSC lines average 227.5) */
    return v * (pal ? 454UL : 455UL) + 2UL * (b & 0xff);
}

static void eclock_tests(void)
{
    int pal = amt_probe_result.pal;
    ULONG k = 10, fr, e0 = 0, e1 = 0, et, sum_lines = 0, v, pv, off0 = 0, off1 = 0;
    UWORD lof = 0, flof = 0;
    ULONG cck2, ecck2, diff, tol, vbl, sec, polls;
    int started = 0;
    long want = pal ? 50 : 60;
    int ok, stuck;
    UWORD cnt = TM_GUARD_EVERY;
    struct tm_guard g;

    amt_set_sr(0x2700);
    /* from one V wrap to the K-th next one; each frame counts the lines its
     * LOF bit says (HRM: long frame 313/263, short 312/262) */
    fr = 0;
    stuck = 0;
    tm_guard_init(&g, TM_FRAMES_E(2 * k + 4));
    pv = beam_v();
    for (;;) {
        v = beam_vl(&lof);
        if (v < pv) {
            if (!started) {
                e0 = ec_read();
                off0 = beam_cck2(pal);
                started = 1;
            } else {
                sum_lines += flof ? (pal ? 313 : 263) : (pal ? 312 : 262);
                if (++fr == k) {
                    e1 = ec_read();
                    off1 = beam_cck2(pal);
                    break;
                }
            }
        }
        flof = lof;
        pv = v;
        if (TM_GUARD(cnt, &g, v)) {
            stuck = 1;
            break;
        }
    }
    et = e0 - e1;
    if (stuck) {
        amt_set_sr(0x2000);
        stuck_case("eclock_cck_ratio", "10 beam frames (V wraps)", WHY_BEAM_STUCK);
    } else {
        /* expected colour clocks x2 (NTSC lines average 227.5) */
        cck2 = sum_lines * (pal ? 454UL : 455UL);
        /* E ticks x10 = cck x2 between the two reads; minus the beam's
         * lead at the second read, plus its lead at the first one */
        ecck2 = et * 10UL - off1 + off0;
        diff = ecck2 > cck2 ? ecck2 - cck2 : cck2 - ecck2;
        tol = cck2 / 1000 + 200;         /* 0.1% + read jitter */
        amt_set_sr(0x2000);

        amt_case_open("eclock_cck_ratio", diff <= tol ? AMT_ST_PASS : AMT_ST_FAIL);
        if (diff > tol) {
            json_obj("expect");
            json_int("cck_x2", (long)cck2);
            json_int("tol_x2", (long)tol);
            json_obj_end();
            json_obj("got");
            json_int("eclk_x10", (long)ecck2);
            json_obj_end();
            json_str("why", "CIA E clock must be exactly colour clock / 5 (709379 vs 3546895 Hz PAL, 715909 vs 3579545 NTSC; HRM appendix F, 8520 timers); measured over whole frames using the HRM line lengths, the CPU's wrap-detection delay taken off using the beam position");
            json_obj("in");
            json_int("frames", (long)k);
            json_int("pal", pal);
            json_obj_end();
        }
        json_obj("timing");
        json_int("frames", (long)k);
        json_int("lines", (long)sum_lines);
        json_int("eticks", (long)et);
        json_int("lead0_cck_x2", (long)off0);
        json_int("lead1_cck_x2", (long)off1);
        json_obj_end();
        amt_case_close();
    }

    /* VBL (VERTB) edges in one CIA second (tm_hz E ticks). The VERTB wait is
     * bounded by the E clock; the CIA second by the VBL count (3x the
     * expected rate means the CIA is far too slow or stuck) and, if VERTB
     * stops too, by the poll count. */
    amt_set_sr(0x2700);
    vbl = 0;
    stuck = 0;
    CREG(R_INTREQ) = 0x0020;
    tm_guard_init(&g, TM_FRAMES_E(4));
    cnt = TM_GUARD_EVERY;
    while (!(CREG(R_INTREQR) & 0x0020))
        if (TM_GUARD(cnt, &g, 0)) {
            stuck = 1;
            break;
        }
    CREG(R_INTREQ) = 0x0020;
    e0 = ec_read();
    polls = 0;
    while (!stuck) {
        if (CREG(R_INTREQR) & 0x0020) {
            CREG(R_INTREQ) = 0x0020;
            vbl++;
            polls = 0;
        }
        sec = e0 - ec_read();
        if (sec >= (ULONG)tm_hz)
            break;
        if ((long)vbl > 3 * want || ++polls > TM_STUCK_POLLS) {
            stuck = 2;
            break;
        }
    }
    CREG(R_INTREQ) = 0x0020;
    amt_set_sr(0x2000);

    if (stuck) {
        stuck_case("vbl_per_cia_second", stuck == 1 ? "a VERTB request within 4 frames of E clock"
                                                    : "one CIA second (E ticks) within 3 seconds of VBLs",
                   stuck == 1 ? "VERTB stuck: INTREQ VERTB was not set again within the bound (CIA-B E clock and a poll count) after being cleared (HRM ch.7 VERTB)"
                              : "CIA stuck: CIA-B timers A+B (E clock) did not count one second within 3 seconds of VERTB requests, or neither clock moved (HRM appendix F, 8520 timers)");
        return;
    }
    ok = (long)vbl >= want - 1 && (long)vbl <= want + 1;
    amt_case_open("vbl_per_cia_second", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_int("vbl", want);
        json_int("tol", 1);
        json_obj_end();
        json_obj("got");
        json_int("vbl", (long)vbl);
        json_obj_end();
        json_str("why", pal ? "PAL: 3546895/(227*313) = 49.92 frames per second (HRM); counted VERTB in 709379 CIA E ticks"
                            : "NTSC: 3579545/(227.5*263) = 59.83 frames per second (HRM); counted VERTB in 715909 CIA E ticks");
        json_obj("in");
        json_int("pal", pal);
        json_int("eticks", tm_hz);
        json_obj_end();
    }
    json_obj("timing");
    json_int("vbl", (long)vbl);
    json_obj_end();
    amt_case_close();
}

static const struct loop loops[] = {
    { "dbra",          100, 1000, 0, tm_dbra },
    { "move_reg",      250,  800, 0, tm_movereg },
    { "move_mem_chip", 100,  512, 1, 0 },
    { "move_mem_any",  100,  512, 2, 0 },
    { "mulu",           25,  800, 0, tm_mulu },
    { "divu",           10,  800, 0, tm_divu },
    { "branch",         64,  256, 0, tm_branch },
};
static const struct loop fpu_loop = { "fpu", 25, 800, 0, tm_fpu };

void amt_run_timing(void)
{
    unsigned i;
    int fpu_ok = AMT_FPU() && !amt_probe_result.fpu_dis;

    tm_hz = (long)(amt_eclock_hz() / 5);
    tm_stuck_polls = AMT_CPU() <= AMT_CPU_68010 ? 32000000UL : 512000000UL;
    ec_start();

    beam_tests();
    eclock_tests();

    for (i = 0; i < sizeof loops / sizeof loops[0]; i++)
        perf_loop(&loops[i]);
    if (fpu_ok)
        perf_loop(&fpu_loop);

    check_loop("loop_result_mulu", hot_mulu, 0x3333UL * 0x5555UL,
               "MULU.W $5555,$3333 must give $1110EEEF (PRM MULU)");
    check_loop("loop_result_divu", hot_divu,
               ((0x123456UL % 0x1234UL) << 16) | (0x123456UL / 0x1234UL),
               "DIVU.W #$1234 into $00123456: remainder $0056 : quotient $0100 (PRM DIVU)");
    check_loop("loop_result_branch", hot_branch, model_branch(1),
               "LFSR-driven Bcc/BTST/EOR loop result differs from the C model (PRM LSR/Bcc/BTST)");
    if (fpu_ok)
        check_loop("loop_result_fpu", hot_fpu, 2,
                   "x <- x*0.5 + 1.0 repeated 400 times in extended precision converges to exactly 2.0 (6888x/040/060 FMUL/FADD, round to nearest)");
    else
        amt_case("loop_result_fpu", AMT_ST_SKIP, AMT_FPU() ? "FPU disabled (68060 PCR.DFP)" : "no FPU");

    if (ec_stop())
        amt_case("cia_restore", AMT_ST_PASS, 0);
    else {
        amt_case_open("cia_restore", AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("cra", tm_cs.cra & 0xef, 2);
        json_hexn("crb", tm_cs.crb & 0xef, 2);
        json_obj_end();
        json_obj("got");
        json_hexn("cra", CIAB(C_CRA) & 0xef, 2);
        json_hexn("crb", CIAB(C_CRB) & 0xef, 2);
        json_obj_end();
        json_str("why", "CIA-B CRA/CRB did not read back as written (8520 control registers are read/write except the LOAD strobe, bit 4)");
        amt_case_close();
    }
}
