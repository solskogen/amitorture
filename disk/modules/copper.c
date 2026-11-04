/* copper: the Copper instruction set, comparisons, jumps, danger bit,
 * interrupts and timing (CONTRACT.md section 10, module id 13, disk "chip").
 *
 * Expectations: Amiga Hardware Reference Manual (HRM) chapter 2
 * "Coprocessor Hardware" (MOVE/WAIT/SKIP, comparison enables, BFD, COP1LC/
 * COP2LC/COPJMP, restart at vertical blank, COPCON danger bit, the line-255
 * wait), chapter 7 (INTREQ COPER), appendix C (ECS: danger bit covers all
 * registers). See docs/cases/copper.md.
 *
 * Every pass/fail observation is state that persists until the CPU reads it
 * at a point where nothing can still change it, so the records do not depend
 * on how closely the CPU core follows the beam:
 *   - markers: INTREQ bits nothing else sets while their interrupts/DMA are
 *     off (SOFT, DSKBLK, COPER, BLIT where no blit runs), read lines later;
 *   - release positions: the copper list (restarted every frame) enables one
 *     bitplane of an all-ones playfield with MOVE BPLCON0 where the WAIT
 *     under test releases; one-line DMA sprites just before and after that
 *     position collide or not, and CLXDAT is read at the vertical blank
 *     after a complete frame (as in the display module);
 *   - a copper-programmed blit, JOY0DAT via JOYTEST, DMACONR/ADKCONR, AGA
 *     colour read-back, COPER interrupt counts per frame.
 * Beam-polled latencies are reported in "timing" objects only. */
#include "mod.h"
#include "display_hw.h"

AMT_MODULE(copper, 13, AMT_PH_BARE, AMT_MF_LONG | AMT_MF_OWNS_DISPLAY, 24576, 0);

#define OFF_L1    0x0000
#define OFF_L2    0x0400
#define OFF_L3    0x0800
#define OFF_IDLE  0x0c00
#define OFF_BLIT  0x1000            /* blit target (128 bytes reused) */
#define OFF_DW    0x5000            /* danger blit target word */
#define OFF_LX    0x5400            /* composed release-oracle list */
#define OFF_PL    0x5800            /* one all-ones playfield row (40 bytes) */
#define OFF_SP    0x5840            /* 4 probe sprite lists + empty list */

#define MK_A   0x0004               /* SOFT */
#define MK_B   0x0002               /* DSKBLK (disk DMA is off) */
#define MK_C   0x0010               /* COPER (its interrupt is off) */
#define MK_D   0x0040               /* BLIT (only where no blit runs) */
#define MK_ALL (MK_A | MK_B | MK_C | MK_D)
#define COPER  0x0010

static UBYTE *pool;
static int aga, ecsa, pal;
static int host_ok = 1;

/* ------------------------------------------------------------ list builder */
static UWORD *cw;
static UWORD *lst(ULONG off) { return cw = (UWORD *)(pool + off); }
static void cmove(UWORD reg, UWORD val) { *cw++ = reg & 0x1fe; *cw++ = val; }
static void cwaitx(UWORD v, UWORD h, UWORD ve, UWORD he, int bfd, int skip)
{
    *cw++ = (UWORD)(((v & 0xff) << 8) | (h & 0xfe) | 1);
    *cw++ = (UWORD)((bfd ? 0x8000 : 0) | ((ve & 0x7f) << 8) | (he & 0xfe) | (skip ? 1 : 0));
}
static void cwait(UWORD v, UWORD h) { cwaitx(v, h, 0x7f, 0xfe, 1, 0); }
static void cskip(UWORD v, UWORD h) { cwaitx(v, h, 0x7f, 0xfe, 1, 1); }
static void cend(void) { *cw++ = 0xffff; *cw++ = 0xfffe; }
static void cmark(UWORD m) { cmove(INTREQ, (UWORD)(0x8000 | m)); }
static void crel(void) { cmove(BPLCON0, 0x1200); }    /* release oracle: 1 plane on */
static int list_words(ULONG off) { return (int)(cw - (UWORD *)(pool + off)); }

static void cop_start(ULONG off)
{
    CUL(COP1LCH) = (ULONG)(pool + off);
    CU(COPJMP1) = 0;
    CU(DMACON) = DMAF_SET | DMAF_DMAEN | DMAF_COPEN;
}

static void cop_idle(void)
{
    CUL(COP1LCH) = (ULONG)(pool + OFF_IDLE);
    CU(COPJMP1) = 0;
}

static void cop_off(void)
{
    cop_idle();
    CU(DMACON) = DMAF_COPEN;
}

static void mk_clear(void)
{
    CU(INTREQ) = MK_ALL;
    CU(INTREQ) = MK_ALL;
}

static UWORD mk_get(void) { return CU(INTREQR) & MK_ALL; }

/* ------------------------------------------------------------ beam */
static int wait_vbl(void)
{
    ULONG f = amt_frames_now();
    UWORD last = hw_vline(), v;
    int wraps = 0;
    while (amt_frames_now() == f) {
        v = hw_vline();
        if (v < last && ++wraps > 3)
            return 0;
        last = v;
    }
    return 1;
}

static void wait_line(UWORD y)
{
    while (hw_vline() < y)
        ;
}

/* right after a VBL, with interrupts masked, at line y */
static void sync_line(UWORD y)
{
    wait_vbl();
    amt_set_sr(0x2700);
    wait_line(y);
}

static void unsync(void) { amt_set_sr(0x2000); }

/* timing only: poll INTREQR for mask until the beam reaches `limit`; lo/hi
 * bracket the moment it appeared (lo 0: the first read already had it) */
static int poll(UWORD mask, ULONG limit, ULONG *lo, ULONG *hi)
{
    ULONG p, last = hw_beam(), l = 0;
    for (;;) {
        p = hw_beam_fix(hw_beam(), last);
        if (CU(INTREQR) & mask)
            break;
        if (p < last || p >= limit)
            return 0;
        l = p;
        last = p;
    }
    *lo = l;
    *hi = hw_beam_fix(hw_beam(), p);
    return 1;
}

static ULONG pos(UWORD v, UWORD h) { return ((ULONG)v << 8) | h; }

static void json_pos(const char *key, ULONG p)
{
    json_obj(key);
    json_int("v", (long)(p >> 8));
    json_hexn("h", p & 0xff, 2);
    json_obj_end();
}

static void json_timing(ULONG e, int found, ULONG lo, ULONG hi)
{
    json_obj("timing");
    json_str("unit", "colour clocks from the reference position to the beam reads around the "
             "marker (lo before, hi after); 227 per line");
    json_int("found", found);
    if (found) {
        if (lo)
            json_int("lo", (long)hw_lin(lo) - (long)hw_lin(e));
        else
            json_str("lo", "first read saw it");
        json_int("hi", (long)hw_lin(hi) - (long)hw_lin(e));
    }
    json_obj_end();
}

static void json_list(const char *key, ULONG off, int nwords)
{
    UWORD *p = (UWORD *)(pool + off);
    int i;
    json_arr(key);
    for (i = 0; i < nwords; i++)
        json_hexn(NULL, p[i], 4);
    json_arr_end();
}

/* markers after a copper run started by the CPU on line `start`; read
 * `lines` lines later (any later is as good: markers persist) */
static UWORD run_markers(ULONG off, UWORD start, UWORD lines, ULONG *lo, ULONG *hi, int *found,
                         UWORD tmask)
{
    UWORD g;
    mk_clear();
    sync_line(start);
    cop_start(off);
    *found = tmask ? poll(tmask, pos((UWORD)(start + lines), 0), lo, hi) : 0;
    wait_line((UWORD)(start + lines));
    g = mk_get();
    cop_idle();
    unsync();
    return g;
}

/* ------------------------------------------------------------ release oracle */
/* The composed list (restarted every VBL): display setup with no planes,
 * WAIT for the start line, then the test body from OFF_L1 in which crel()
 * marks the release. Probes (one-line sprites, $FFFF): sprite 0 on line V-1
 * late in the line, sprite 2 on line V well after the release H, sprite 4
 * on line V before it (when H leaves room). CLXCON plane 1 = 1: the bits
 * say on which side of the release the plane was on. */
static void spr_one(int k, UWORD y, UWORD h)
{
    UWORD *q = (UWORD *)(pool + OFF_SP + 16 * k);
    if (!h) {
        q[0] = q[1] = 0;
        return;
    }
    q[0] = (UWORD)(((y & 0xff) << 8) | ((h >> 1) & 0xff));
    q[1] = (UWORD)((((y + 1) & 0xff) << 8) | (h & 1) | ((y & 0x100) ? 4 : 0) |
                   (((y + 1) & 0x100) ? 2 : 0));
    q[2] = 0xffff;
    q[3] = 0;
    q[4] = q[5] = 0;
}

static void lx_build(UWORD start, int nbody)
{
    UWORD *b = (UWORD *)(pool + OFF_L1);
    UWORD *pl = (UWORD *)(pool + OFF_PL);
    int i;
    for (i = 0; i < 20; i++)
        pl[i] = 0xffff;
    lst(OFF_LX);
    cmove(BPLCON0, 0x0200);
    cmove(BPLCON1, 0);
    cmove(BPLCON2, 0);
    cmove(DIWSTRT, 0x2c81);
    cmove(DIWSTOP, 0x2cc1);          /* VSTOP $12C: lines $2C-$12B */
    cmove(DDFSTRT, 0x38);
    cmove(DDFSTOP, 0xd0);
    cmove(BPL1MOD, (UWORD)-40);
    cmove(BPL2MOD, (UWORD)-40);
    if (aga)
        cmove(FMODE, 0);
    cmove(BPL1PTH, (UWORD)((ULONG)pl >> 16));
    cmove(BPL1PTH + 2, (UWORD)(ULONG)pl);
    for (i = 0; i < 8; i++) {
        ULONG sp = (ULONG)(pool + OFF_SP + 16 * ((i & 1) ? 4 : i / 2));
        cmove((UWORD)(SPR0PTH + 4 * i), (UWORD)(sp >> 16));
        cmove((UWORD)(SPR0PTH + 4 * i + 2), (UWORD)sp);
    }
    if (start)
        cwait(start, 0x00);
    for (i = 0; i < nbody; i++)
        *cw++ = b[i];
}

/* clean collision read: whole frames only (see display module) */
static UWORD clean_clx(UWORD clx)
{
    UWORD r;
    int i;
    wait_vbl();
    CU(CLXCON) = clx;
    for (i = 0; i < 3; i++) {           /* discard late-delivered frames */
        wait_vbl();
        (void)CU(CLXDAT);
    }
    wait_vbl();
    wait_vbl();
    r = CU(CLXDAT) & 0x001e;
    return r;
}

/* run the oracle for release position e (V, H in colour clocks); returns
 * bit 0 = probe on line V-1 (H $1A0) hit, bit 1 = probe after the release
 * hit, bit 2 = probe before it (same line) hit, bit 3 = probe at the very
 * end of line V-1 (H $1B0, last 16 pixels of the window) hit */
static UWORD oracle(UWORD start, ULONG e)
{
    UWORD v = (UWORD)(e >> 8), h = (UWORD)(e & 0xff), ha, hb = 0, r;
    int nb = list_words(OFF_L1);
    ha = (UWORD)(2 * h + 0x70);
    if (ha < 0x90)
        ha = 0x90;
    if (2 * h >= 0x88 + 0x18)
        hb = (UWORD)(2 * h - 0x18);
    spr_one(0, (UWORD)(v - 1), 0x1a0);
    spr_one(1, v, ha);
    spr_one(2, v, hb);
    spr_one(3, (UWORD)(v - 1), 0x1b0);
    spr_one(4, 0, 0);
    lx_build(start, nb);
    CU(DMACON) = DMAF_SET | DMAF_DMAEN | DMAF_BPLEN | DMAF_SPREN;
    cop_start(OFF_LX);
    wait_vbl();
    r = (UWORD)((clean_clx(0x0041) >> 1) & 15);
    cop_idle();
    CU(DMACON) = DMAF_BPLEN | DMAF_SPREN;
    CU(CLXCON) = 0;
    return r;
}

static void oracle_case(const char *id, UWORD start, ULONG e, const char *why)
{
    UWORD got, hb, h = (UWORD)(e & 0xff);
    int nb = list_words(OFF_L1), ok;
    hb = (2 * h >= 0x88 + 0x18) ? 1 : 0;
    got = oracle(start, e);
    if (!hb)
        got &= 0xb;
    ok = got == 2;
    amt_case_open(id, ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_pos("release", e);
        json_int("before_line_hit", 0);
        json_int("before_line_end_hit", 0);
        json_int("after_hit", 1);
        if (hb)
            json_int("before_h_hit", 0);
        json_obj_end();
        json_obj("got");
        json_int("before_line_hit", got & 1);
        json_int("before_line_end_hit", (got >> 3) & 1);
        json_int("after_hit", (got >> 1) & 1);
        if (hb)
            json_int("before_h_hit", (got >> 2) & 1);
        json_obj_end();
        json_obj("in");
        json_list("body", OFF_L1, nb);
        json_int("start_line", start);
        json_str("oracle", "list restarted every frame: no planes, WAIT start line, body; the MOVE "
                 "#$1200,BPLCON0 in the body (release point) turns on an all-ones plane; one-line "
                 "sprites on line V-1 (H $1A0 and $1B0), on line V at H 2*h+$70 (min $90) and, if room, at "
                 "2*h-$18; CLXDAT plane-1 bits read at the VBL after a complete frame");
        json_obj_end();
        json_str("why", why);
    }
    amt_case_close();
}

/* ------------------------------------------------------------ MOVE */
static void move_tests(void)
{
    ULONG lo = 0, hi = 0, e;
    UWORD g, adk0, a, d;
    int found;
    /* basic */
    lst(OFF_L1);
    cmark(MK_A);
    cend();
    mk_clear();
    sync_line(0x40);
    cop_start(OFF_L1);
    e = hw_beam();
    found = poll(MK_A, pos(0x48, 0), &lo, &hi);
    wait_line(0x48);
    g = mk_get();
    cop_idle();
    unsync();
    amt_case_open("move_intreq", g == MK_A ? AMT_ST_PASS : AMT_ST_FAIL);
    if (g != MK_A) {
        json_obj("expect");
        json_hexn("markers", MK_A, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("markers", g, 4);
        json_obj_end();
        json_obj("in");
        json_list("list", OFF_L1, list_words(OFF_L1));
        json_str("start", "CPU COP1LC + COPJMP1 on line $40, markers read from line $48 on");
        json_obj_end();
        json_str("why", "a copper MOVE to INTREQ ($09C) with bit 15 set sets that request bit (HRM 2: "
                 "MOVE; HRM 7: INTREQ)");
    }
    json_timing(e, found, lo, hi);
    amt_case_close();
    /* order: later MOVEs win */
    lst(OFF_L1);
    cmove(INTREQ, 0x8000 | MK_A | MK_B);
    cmove(INTREQ, MK_A);
    cmark(MK_C);
    cmark(MK_D);
    cend();
    g = run_markers(OFF_L1, 0x40, 8, &lo, &hi, &found, 0);
    amt_case_open("move_sequence", g == (MK_B | MK_C | MK_D) ? AMT_ST_PASS : AMT_ST_FAIL);
    if (g != (MK_B | MK_C | MK_D)) {
        json_obj("expect");
        json_hexn("intreqr_markers", MK_B | MK_C | MK_D, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("intreqr_markers", g, 4);
        json_obj_end();
        json_obj("in");
        json_list("list", OFF_L1, list_words(OFF_L1));
        json_obj_end();
        json_str("why", "MOVEs execute in list order: set A|B, clear A, set C, set D leaves B|C|D "
                 "(HRM 2: MOVE, INTREQ SET/CLR)");
    }
    amt_case_close();
    /* register classes: DMACON (BLTPRI) and ADKCON (audio modulation bits) */
    adk0 = CU(ADKCONR);
    lst(OFF_L1);
    cmove(DMACON, DMAF_SET | DMAF_BLTPRI);
    cmove(ADKCON, 0x00ff);
    cmove(ADKCON, 0x80a5);
    cmark(MK_A);
    cend();
    mk_clear();
    sync_line(0x40);
    cop_start(OFF_L1);
    wait_line(0x48);
    g = mk_get();
    d = CU(DMACONR) & DMAF_BLTPRI;
    a = CU(ADKCONR) & 0x00ff;
    cop_idle();
    unsync();
    CU(DMACON) = DMAF_BLTPRI;
    CU(ADKCON) = 0x00ff;
    CU(ADKCON) = (UWORD)(0x8000 | (adk0 & 0x00ff));
    found = g == MK_A;
    amt_case_open("move_dmacon_adkcon", (found && d && a == 0xa5) ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!(found && d && a == 0xa5)) {
        json_obj("expect");
        json_hexn("dmaconr_bltpri", DMAF_BLTPRI, 4);
        json_hexn("adkconr_low", 0xa5, 2);
        json_hexn("markers", MK_A, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("dmaconr_bltpri", d, 4);
        json_hexn("adkconr_low", a, 2);
        json_hexn("markers", g, 4);
        json_obj_end();
        json_obj("in");
        json_list("list", OFF_L1, list_words(OFF_L1));
        json_obj_end();
        json_str("why", "DMACON ($096) and ADKCON ($09E) are above $80, so the copper may write them "
                 "without the danger bit; DMACONR/ADKCONR read the result (HRM 2, appendix A)");
    }
    amt_case_close();
}

/* ------------------------------------------------------------ WAIT */
static void wait_tests(void)
{
    UWORD k = pal ? 0x10 : 0x02;

    lst(OFF_L1);
    cwait(0x80, 0x00);
    crel();
    cend();
    oracle_case("wait_v", 0x40, pos(0x80, 0),
                "WAIT $8001,$FFFE releases when the beam reaches line $80 (HRM 2: WAIT)");

    lst(OFF_L1);
    cwait(0x80, 0x80);
    crel();
    cend();
    oracle_case("wait_vh", 0x40, pos(0x80, 0x80),
                "WAIT $8081,$FFFE releases at line $80, horizontal position $80 (HRM 2: WAIT)");

    /* only V3-V0 (and V7) compared: from line $40 the low bits reach 5 at $45 */
    lst(OFF_L1);
    cwaitx(0x05, 0x00, 0x0f, 0x00, 1, 0);
    crel();
    cend();
    oracle_case("wait_v_masked", 0x40, pos(0x45, 0),
                "WAIT VP=$05 with VE=$0F compares V7 and V3-V0 only: started on line $40 it "
                "releases on line $45 (HRM 2: comparison enable bits)");

    /* V7 is always compared */
    lst(OFF_L1);
    cwaitx(0x80, 0x00, 0x00, 0x00, 1, 0);
    crel();
    cend();
    oracle_case("wait_v7_always_compared", 0x40, pos(0x80, 0),
                "the vertical comparison bit V7 has no enable bit: WAIT VP=$80 with VE=0 still "
                "waits for line $80 (HRM 2: VE covers V6-V0 only)");

    /* horizontal only, same line; the second one is already true */
    lst(OFF_L1);
    cwait(0x60, 0x10);
    cwaitx(0x00, 0xa0, 0x00, 0xfe, 1, 0);
    cmark(MK_A);
    cwaitx(0x00, 0x40, 0x00, 0xfe, 1, 0);
    crel();
    cend();
    oracle_case("wait_h_masked_same_line", 0x40, pos(0x60, 0xa0),
                "with VE=0 only V7 and H are compared: WAIT H=$A0 on line $60 releases at $A0 of "
                "that line; a following WAIT H=$40 is already satisfied and releases at once, not "
                "on the next line (HRM 2: comparison enable bits)");

    /* horizontal positions beyond the end of the line */
    lst(OFF_L1);
    cwait(0x80, 0xe4);
    crel();
    cend();
    oracle_case("wait_h_past_line_end_e4", 0x40, pos(0x81, 0),
                "the beam never reaches H $E4 (a line is $00-$E2/$E3), so WAIT $80E5,$FFFE "
                "releases when V becomes $81 (HRM 2: horizontal positions)");
    lst(OFF_L1);
    cwait(0x80, 0xfe);
    crel();
    cend();
    oracle_case("wait_h_past_line_end_fe", 0x40, pos(0x81, 0),
                "WAIT $80FF,$FFFE releases at the start of line $81 (HRM 2: horizontal positions)");

    /* line 256+: the $FFDF wait first */
    lst(OFF_L1);
    cwait(0xff, 0xde);
    cwait(k, 0x00);
    crel();
    cend();
    oracle_case("wait_line_256_wrap", 0x40, pos((UWORD)(0x100 + k), 0),
                "the copper compares V7-V0 only; WAIT $FFDF,$FFFE then WAIT $xx01,$FFFE waits for "
                "line 256+$xx (HRM 2: the line 255 wait)");

    /* without the end-of-line wait, the second WAIT is compared on line 255 */
    lst(OFF_L1);
    cwait(0xff, 0x06);
    cwait(0x08, 0x00);
    crel();
    cend();
    oracle_case("wait_after_255_8bit_compare", 0x40, pos(0xff, 0x06),
                "a WAIT for line $08 evaluated on line 255 ($FF >= $08) releases at once: the "
                "copper vertical compare is 8 bits (HRM 2: the line 255 wait)");
}

/* ------------------------------------------------------------ SKIP */
static void skip_tests(void)
{
    static const struct { UWORD v, h, ve, he; UWORD exp; const char *what; } sk[] = {
        { 0x50, 0x00, 0x7f, 0xfe, MK_B, "SKIP $5001 on line $60: true, skips MOVE A" },
        { 0x70, 0x00, 0x7f, 0xfe, MK_A | MK_B, "SKIP $7001 on line $60: false" },
        { 0x05, 0x00, 0x0f, 0x00, MK_A | MK_B, "SKIP VP=$05 VE=$0F on line $60 (low bits 0): false" },
        { 0x00, 0x08, 0x00, 0xfe, MK_B, "SKIP H=$08 VE=0 after a WAIT for H $10: true" },
        { 0x60, 0x10, 0x7f, 0xfe, MK_B, "SKIP $6011 right after WAIT $6011: true (>=)" },
    };
    UWORD got[5];
    ULONG lo, hi;
    int i, ok = 1, bad = -1, f;
    for (i = 0; i < 5; i++) {
        lst(OFF_L1);
        cwait(0x60, 0x10);
        cwaitx(sk[i].v, sk[i].h, sk[i].ve, sk[i].he, 1, 1);
        cmark(MK_A);
        cmark(MK_B);
        cend();
        got[i] = run_markers(OFF_L1, 0x40, 0x28, &lo, &hi, &f, 0);
        if (got[i] != sk[i].exp && ok) {
            ok = 0;
            bad = i;
        }
    }
    amt_case_open("skip_compare", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_hexn("markers", sk[bad].exp, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("markers", got[bad], 4);
        json_obj_end();
        json_obj("in");
        json_str("list", "WAIT $6011,$FFFE; SKIP (below); MOVE A; MOVE B; END");
        json_hexn("skip_w1", (sk[bad].v << 8) | sk[bad].h | 1, 4);
        json_hexn("skip_w2", 0x8000 | (sk[bad].ve << 8) | sk[bad].he | 1, 4);
        json_str("case", sk[bad].what);
        json_obj_end();
        json_str("why", "SKIP skips the next instruction when the beam is at or past its (masked) "
                 "position, with the same comparison as WAIT (HRM 2: SKIP)");
    }
    amt_case_close();

    /* SKIP over a WAIT that would block for the rest of the frame */
    {
        UWORD g;
        lst(OFF_L1);
        cwait(0x60, 0x10);
        cskip(0x50, 0x00);
        cwait(0xff, 0xfe);
        cmark(MK_C);
        cend();
        g = run_markers(OFF_L1, 0x40, 0x30, &lo, &hi, &f, 0);
        amt_case_open("skip_over_wait", g == MK_C ? AMT_ST_PASS : AMT_ST_FAIL);
        if (g != MK_C) {
            json_obj("expect");
            json_hexn("markers", MK_C, 4);
            json_obj_end();
            json_obj("got");
            json_hexn("markers", g, 4);
            json_obj_end();
            json_obj("in");
            json_list("list", OFF_L1, list_words(OFF_L1));
            json_str("read", "markers read on line $70");
            json_obj_end();
            json_str("why", "a true SKIP skips the following instruction whatever it is, here a "
                     "WAIT $FFFF,$FFFE; the MOVE after it runs at once (HRM 2: SKIP)");
        }
        amt_case_close();
    }
}

/* ------------------------------------------------------------ jumps, restart */
static void jump_tests(void)
{
    ULONG lo, hi;
    UWORD g, g2;
    int f, ok, t;

    /* copper strobes COPJMP2 */
    lst(OFF_L2);
    cmark(MK_B);
    cend();
    lst(OFF_L1);
    cmark(MK_A);
    cmove(COPJMP2, 0);
    cmark(MK_C);
    cend();
    CUL(COP2LCH) = (ULONG)(pool + OFF_L2);
    g = run_markers(OFF_L1, 0x40, 8, &lo, &hi, &f, 0);
    ok = g == (MK_A | MK_B);
    amt_case_open("copjmp2_by_copper", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_hexn("markers", MK_A | MK_B, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("markers", g, 4);
        json_obj_end();
        json_obj("in");
        json_str("list1", "MOVE A; MOVE #0,COPJMP2; MOVE C; END");
        json_str("list2", "MOVE B; END (COP2LC)");
        json_obj_end();
        json_str("why", "a write to COPJMP2 loads the copper PC from COP2LC at once: B runs, the "
                 "MOVE C after the strobe never does (HRM 2: jump strobes)");
    }
    amt_case_close();

    /* CPU strobes COPJMP2 while the copper waits */
    lst(OFF_L1);
    cmark(MK_A);
    cend();
    mk_clear();
    sync_line(0x40);
    cop_start(OFF_L1);
    wait_line(0x48);
    g = mk_get();
    CU(COPJMP2) = 0;
    wait_line(0x50);
    g2 = mk_get();
    cop_idle();
    unsync();
    ok = g == MK_A && g2 == (MK_A | MK_B);
    amt_case_open("copjmp2_by_cpu", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_hexn("before", MK_A, 4);
        json_hexn("after", MK_A | MK_B, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("before", g, 4);
        json_hexn("after", g2, 4);
        json_obj_end();
        json_str("why", "the CPU writing COPJMP2 restarts the copper at COP2LC (list2: MOVE B) even "
                 "while it waits at the end of its list (HRM 2: jump strobes)");
    }
    amt_case_close();

    /* COP1LC written by the copper: no jump; used at the next vertical blank.
     * list3 sets C, then D only if it runs before line 0 h $30 */
    lst(OFF_L3);
    cmark(MK_C);
    cskip(0x00, 0x30);
    cmark(MK_D);
    cend();
    lst(OFF_L1);
    cmove(COP1LCH, (UWORD)((ULONG)(pool + OFF_L3) >> 16));
    cmove(COP1LCL, (UWORD)(ULONG)(pool + OFF_L3));
    cmark(MK_A);
    cend();
    g = g2 = 0;
    for (t = 0; t < 4; t++) {
        UWORD last, v, lim = pal ? 0x130 : 0x100;
        int valid;
        mk_clear();
        sync_line(0x40);
        cop_start(OFF_L1);
        wait_line(lim);
        g = mk_get();
        valid = hw_vline() >= lim;       /* still in the same frame */
        last = hw_vline();
        for (;;) {                      /* to the next frame */
            v = hw_vline();
            if (v < last && last - v > 100)
                break;
            last = v;
        }
        wait_line(0x10);
        g2 = mk_get();
        cop_idle();
        unsync();
        if (valid)
            break;
    }
    ok = g == MK_A;
    amt_case_open("cop1lc_write_no_jump", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_hexn("markers_before_vbl", MK_A, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("markers_before_vbl", g, 4);
        json_obj_end();
        json_obj("in");
        json_str("list1", "MOVE COP1LCH/L = list3; MOVE A; END");
        json_str("list3", "MOVE C; SKIP $0031,$FFFF; MOVE D; END");
        json_obj_end();
        json_str("why", "writing COP1LC does not move the copper PC (only COPJMP1 or vertical "
                 "blank does): A runs, list3 does not, until the frame ends (HRM 2)");
    }
    amt_case_close();
    ok = g2 == (MK_A | MK_C | MK_D);
    amt_case_open("vbl_restart_cop1lc", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_hexn("markers_next_frame", MK_A | MK_C | MK_D, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("markers_next_frame", g2, 4);
        json_obj_end();
        json_obj("in");
        json_str("list3", "MOVE C; SKIP $0031,$FFFF; MOVE D; END (D only if run before line 0 h $30)");
        json_obj_end();
        json_str("why", "at the start of vertical blank (line 0) the copper restarts at COP1LC, "
                 "here the list3 the copper itself stored, so C and D are set early in line 0 "
                 "(HRM 2: vertical blank restart)");
    }
    amt_case_close();

    /* loop with COPJMP2 until SKIP is true; the oracle list jumps to list2 */
    lst(OFF_L2);
    cmark(MK_A);
    cskip(0x90, 0x00);
    cmove(COPJMP2, 0);
    crel();
    cend();
    CUL(COP2LCH) = (ULONG)(pool + OFF_L2);
    lst(OFF_L1);
    cmove(COPJMP2, 0);
    cend();
    oracle_case("skip_copjmp2_loop", 0x60, pos(0x90, 0),
                "list2 (MOVE A; SKIP $9001,$FFFF; MOVE #0,COPJMP2; release) loops through COPJMP2 "
                "until the SKIP becomes true on line $90, then falls through to the release "
                "(HRM 2: SKIP loops)");
}

/* ------------------------------------------------------------ COPEN */
static void copen_test(void)
{
    UWORD g = 0, g2 = 0;
    int t, valid = 0, ok;
    lst(OFF_L1);
    cwait(0x80, 0x00);
    cmark(MK_A);
    cend();
    for (t = 0; t < 4 && !valid; t++) {
        mk_clear();
        sync_line(0x40);
        cop_start(OFF_L1);
        wait_line(0x60);
        CU(DMACON) = DMAF_COPEN;        /* copper DMA off while it waits */
        valid = hw_vline() < 0x78;      /* the switch-off came well before $80 */
        wait_line(0x90);
        g = mk_get();
        CU(DMACON) = DMAF_SET | DMAF_COPEN;
        wait_line(0x98);
        g2 = mk_get();
        cop_idle();
        unsync();
    }
    ok = !g && g2 == MK_A;
    amt_case_open("copen_pause_resume", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_hexn("markers_while_off", 0, 4);
        json_hexn("markers_after_on", MK_A, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("markers_while_off", g, 4);
        json_hexn("markers_after_on", g2, 4);
        json_obj_end();
        json_obj("in");
        json_str("seq", "WAIT $8001,$FFFE; MOVE A started on line $40; COPEN off on line $60, read "
                 "on line $90, on again, read 8 lines later");
        json_obj_end();
        json_str("why", "with COPEN clear the copper does not run (A stays clear past line $80); "
                 "turned on again it continues its WAIT, which is already true (HRM 2, DMACON)");
    }
    json_obj("timing");
    json_int("attempts", t);
    json_obj_end();
    amt_case_close();
}

/* ------------------------------------------------------------ BFD */
static int blit_busy(void)
{
    (void)CU(DMACONR);
    return (CU(DMACONR) & DMAF_BBUSY) != 0;
}

static void blit_wait(void)
{
    ULONG f0 = amt_frames_now();
    while (blit_busy() && amt_frames_now() - f0 < 200)
        ;
}

/* D only, D = 1, 64 words x 1024 lines over the same 128 bytes (modulo
 * -128): about two frames */
static void blit_start_long(void)
{
    CU(DMACON) = DMAF_SET | DMAF_DMAEN | DMAF_BLTEN;
    blit_wait();
    CU(BLTCON0) = 0x01ff;
    CU(BLTCON1) = 0;
    CU(BLTAFWM) = 0xffff;
    CU(BLTALWM) = 0xffff;
    CUL(BLTDPTH) = (ULONG)(pool + OFF_BLIT);
    CU(BLTDMOD) = (UWORD)-128;
    CU(BLTSIZE) = 0;                    /* 1024 lines x 64 words */
}

/* poll a marker for up to n frames (no beam timing involved) */
static int wait_marker(UWORD m, ULONG n)
{
    ULONG f0 = amt_frames_now();
    while (!(CU(INTREQR) & m))
        if (amt_frames_now() - f0 > n)
            return 0;
    return 1;
}

static void bfd_tests(void)
{
    int bfd;
    for (bfd = 0; bfd < 2; bfd++) {
        ULONG f0, frames;
        int f, busy0, busy_at, ok;
        lst(OFF_L1);
        cwaitx(0x40, 0x00, 0x7f, 0xfe, bfd, 0);
        cmark(MK_A);
        cend();
        mk_clear();
        wait_vbl();
        wait_line(0x50);
        blit_start_long();
        busy0 = blit_busy();
        f0 = amt_frames_now();
        cop_start(OFF_L1);
        f = wait_marker(MK_A, 20);
        busy_at = blit_busy();
        frames = amt_frames_now() - f0;
        cop_idle();
        blit_wait();
        CU(DMACON) = DMAF_BLTEN;
        ok = busy0 && f && (bfd ? busy_at : !busy_at);
        amt_case_open(bfd ? "wait_bfd1_ignores_blitter" : "wait_bfd0_waits_blitter",
                      ok ? AMT_ST_PASS : AMT_ST_FAIL);
        if (!ok) {
            json_obj("expect");
            json_int("blitter_busy_at_start", 1);
            json_int("found", 1);
            json_int("blitter_busy_at_marker", bfd);
            json_obj_end();
            json_obj("got");
            json_int("blitter_busy_at_start", busy0);
            json_int("found", f);
            json_int("blitter_busy_at_marker", busy_at);
            json_obj_end();
            json_obj("in");
            json_str("blit", "D only, minterm $FF, 64 words x 1024 lines (modulo -128), about two "
                     "frames, started on line $50");
            json_hexn("wait_w2", bfd ? 0xfffe : 0x7ffe, 4);
            json_str("list", "WAIT $4001 (already passed); MOVE A; END");
            json_obj_end();
            json_str("why", bfd ? "with BFD=1 the WAIT ignores the blitter: A is set while the blit "
                     "still runs (HRM 2: blitter-finished disable)"
                     : "with BFD=0 a WAIT also waits for the blitter to finish: A must not be set "
                     "while DMACONR BBUSY is 1 (HRM 2: blitter-finished disable)");
        }
        json_obj("timing");
        json_str("unit", "frames from copper start until the CPU saw the marker");
        json_int("frames", (long)frames);
        json_obj_end();
        amt_case_close();
    }
}

/* ------------------------------------------------------------ danger bit */
static void danger_blit_list(int with_b)
{
    ULONG dw = (ULONG)(pool + OFF_DW);
    lst(OFF_L1);
    if (with_b)
        cmark(MK_B);
    cmove(BLTCON0, 0x01ff);
    cmove(BLTCON1, 0x0000);
    cmove(BLTDPTH, (UWORD)(dw >> 16));
    cmove(BLTDPTL, (UWORD)dw);
    cmove(BLTDMOD, 0);
    cmove(BLTSIZE, (1 << 6) | 1);
    cmark(MK_A);
    cend();
}

static void danger_tests(void)
{
    volatile UWORD *dw = (volatile UWORD *)(pool + OFF_DW);
    UWORD g, g2, joy0, joyw, j1;
    int f, ok;

    /* CDANG clear: blitter registers stop the copper */
    *dw = 0;
    CU(DMACON) = DMAF_SET | DMAF_DMAEN | DMAF_BLTEN;
    CU(COPCON) = 0;
    danger_blit_list(1);
    mk_clear();
    sync_line(0x40);
    cop_start(OFF_L1);
    wait_line(0x50);
    g = mk_get();
    unsync();
    /* following frames: restarted at COP1LC, B again, stops again */
    CU(INTREQ) = MK_B;
    wait_vbl();
    wait_vbl();
    wait_line(0x20);
    g2 = mk_get();
    cop_idle();
    blit_wait();
    ok = g == MK_B && g2 == MK_B && *dw == 0;
    amt_case_open("danger_off_blitter_stops", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_hexn("markers", MK_B, 4);
        json_hexn("markers_next_frame", MK_B, 4);
        json_hexn("dest", 0, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("markers", g, 4);
        json_hexn("markers_next_frame", g2, 4);
        json_hexn("dest", *dw, 4);
        json_obj_end();
        json_obj("in");
        json_hexn("copcon", 0, 4);
        json_list("list", OFF_L1, list_words(OFF_L1));
        json_obj_end();
        json_str("why", "without COPCON CDANG the copper may not write registers below $80: the MOVE "
                 "to BLTCON0 ($040) stops it (no blit, MOVE A never runs); it starts again at "
                 "COP1LC on the next vertical blank (MOVE B again) (HRM 2: danger bit)");
    }
    amt_case_close();

    /* CDANG set: the copper starts a blit */
    *dw = 0;
    CU(COPCON) = 0x0002;
    danger_blit_list(0);
    mk_clear();
    sync_line(0x40);
    cop_start(OFF_L1);
    wait_line(0x50);
    g = mk_get();
    cop_idle();
    unsync();
    blit_wait();
    CU(COPCON) = 0;
    ok = (g & MK_A) && *dw == 0xffff;
    amt_case_open("danger_on_blitter_runs", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_int("a", 1);
        json_hexn("dest", 0xffff, 4);
        json_obj_end();
        json_obj("got");
        json_int("a", (g & MK_A) != 0);
        json_hexn("dest", *dw, 4);
        json_obj_end();
        json_obj("in");
        json_hexn("copcon", 2, 4);
        json_list("list", OFF_L1, list_words(OFF_L1));
        json_obj_end();
        json_str("why", "with COPCON CDANG the copper may write the blitter registers $040-$07E: it "
                 "programs and starts a 1-word D-only minterm $FF blit (HRM 2: danger bit)");
    }
    amt_case_close();
    CU(DMACON) = DMAF_BLTEN;

    /* registers below $40: JOYTEST ($036) */
    joy0 = CU(JOY0DAT);
    joyw = (UWORD)(joy0 ^ 0x5454);
    {
        int cd;
        for (cd = 0; cd < 2; cd++) {
            int expect_write = cd && ecsa;
            UWORD jr;
            lst(OFF_L1);
            cmove(JOYTEST, joyw);
            cmark(MK_A);
            cend();
            CU(COPCON) = cd ? 0x0002 : 0;
            mk_clear();
            sync_line(0x40);
            cop_start(OFF_L1);
            wait_line(0x50);
            f = (mk_get() & MK_A) != 0;
            jr = CU(JOY0DAT);
            j1 = CU(JOY1DAT);
            cop_idle();
            unsync();
            CU(COPCON) = 0;
            CU(JOYTEST) = joy0;          /* put the port 0 counters back */
            ok = expect_write ? (f && (jr & 0xfcfc) == (joyw & 0xfcfc))
                              : (!f && (jr & 0xfcfc) == (joy0 & 0xfcfc));
            amt_case_open(cd ? "danger_on_below_40" : "danger_off_below_40",
                          ok ? AMT_ST_PASS : AMT_ST_FAIL);
            if (!ok) {
                json_obj("expect");
                json_int("a", expect_write);
                json_hexn("joy0dat_fcfc", (expect_write ? joyw : joy0) & 0xfcfc, 4);
                json_obj_end();
                json_obj("got");
                json_int("a", f);
                json_hexn("joy0dat_fcfc", jr & 0xfcfc, 4);
                json_obj_end();
                json_obj("in");
                json_hexn("copcon", cd ? 2 : 0, 4);
                json_hexn("joytest", joyw, 4);
                json_hexn("joy0dat_before", joy0, 4);
                json_str("agnus", ecsa ? "ECS/AGA" : "OCS");
                json_obj_end();
                json_str("why", !cd ? "without CDANG a copper MOVE to JOYTEST ($036) stops the copper "
                         "(HRM 2: danger bit)"
                         : ecsa ? "ECS Agnus with CDANG lets the copper write every register: the MOVE "
                         "to JOYTEST loads bits 15-10/7-2 of the mouse counters (HRM appendix C: COPCON)"
                         : "OCS Agnus: even with CDANG the copper may not write below $40; the MOVE to "
                         "JOYTEST stops it (HRM 2: danger bit)");
            }
            json_obj("ctx");
            json_hexn("undef_joy1dat_after", j1, 4);   /* moves with a port-1 device */
            json_obj_end();
            amt_case_close();
        }
    }
}

/* ------------------------------------------------------------ COPER */
static volatile UWORD co_n, co_pos[16], co_sr[16], co_badsr;

static void coper_hook(ULONG pend)
{
    if (pend & COPER) {
        UWORD p = CU(VHPOSR), sr = amt_get_sr();
        CU(INTREQ) = COPER;
        CU(INTREQ) = COPER;
        if (co_n < 16) {
            co_pos[co_n] = p;
            co_sr[co_n] = sr;
        }
        if ((sr & 0x0700) != 0x0300)
            co_badsr++;
        co_n++;
    }
}

static void coper_test(void)
{
    int n = AMT_QUICK() ? 3 : 8, i, ok;
    long mn = 99999, mx = -1;
    UWORD c0, c1;
    lst(OFF_L1);
    cwait(0x80, 0x00);
    cmove(INTREQ, 0x8000 | COPER);
    cend();
    co_n = 0;
    co_badsr = 0;
    mk_clear();
    amt_irq_hook[3] = coper_hook;
    CU(INTENA) = 0x8000 | COPER;
    cop_start(OFF_L1);
    wait_vbl();
    wait_vbl();
    c0 = co_n;
    for (i = 0; i < n; i++)
        wait_vbl();
    c1 = co_n;
    CU(INTENA) = COPER;
    cop_idle();
    amt_irq_hook[3] = 0;
    mk_clear();
    ok = (UWORD)(c1 - c0) == (UWORD)n && !co_badsr;
    for (i = 0; i < 16 && i < co_n; i++) {
        long lat = (long)((co_pos[i] >> 8) - 0x80) * 227 + (co_pos[i] & 0xff);
        if (lat < mn)
            mn = lat;
        if (lat > mx)
            mx = lat;
    }
    amt_case_open("coper_interrupt", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_int("interrupts", n);
        json_int("bad_sr", 0);
        json_obj_end();
        json_obj("got");
        json_int("interrupts", (UWORD)(c1 - c0));
        json_int("bad_sr", co_badsr);
        json_obj_end();
        json_obj("in");
        json_str("list", "WAIT $8001,$FFFE; MOVE #$8010,INTREQ; END; INTENA COPER on; interrupts "
                 "counted over n whole frames (VBL to VBL)");
        json_obj_end();
        json_str("why", "the copper's INTREQ write raises COPER, a level 3 interrupt, once per frame; "
                 "the handler runs with SR interrupt mask 3 (HRM 2, 7)");
    }
    json_obj("timing");
    json_str("unit", "colour clocks from line $80 h 0 to VHPOSR read in the level-3 C hook");
    json_int("min", mn);
    json_int("max", mx);
    json_obj_end();
    amt_case_close();
}

/* ------------------------------------------------------------ colours */
static int shot(const char *tag)
{
    if (!host_ok)
        return -1;
    if (amt_request("screenshot", tag, "{\"frames\":2}", amt_secs(10)))
        return 1;
    host_ok = 0;
    return 0;
}

static void colour_test(void)
{
    static const UWORD bar[8] = { 0x0f00, 0x0f80, 0x0ff0, 0x00f0, 0x00ff, 0x000f, 0x080f, 0x0fff };
    UWORD rd = 0, g = 0;
    int i, v, r, ok, t;
    lst(OFF_L1);
    cmove(BPLCON0, 0x0200);
    for (v = 0x60; v < 0xc0; v += 0x10)
        for (i = 0; i < 8; i++) {
            cwait((UWORD)(v + i * 2), (UWORD)(0x40 + i * 0x14));
            cmove(COLOR00, bar[(i + v / 0x10) & 7]);
            cwaitx(0, (UWORD)(0x48 + i * 0x14), 0, 0xfe, 1, 0);
            cmove(COLOR00, 0x0000);
        }
    cmark(MK_A);
    cmove(COLOR00, 0x0123);
    cend();
    for (t = 0; t < 4; t++) {
        int valid;
        mk_clear();
        sync_line(0x40);
        cop_start(OFF_L1);
        wait_line(0xd0);
        g = mk_get();
        if (aga) {
            CU(BPLCON3) = 0x0c00;
            CU(BPLCON2) = 0x0100;       /* RDRAM */
            rd = CU(COLOR00) & 0x0fff;
            CU(BPLCON2) = 0x0000;
        }
        /* valid while still after the list end, before the next frame's bars */
        valid = hw_vline() >= 0xd0;
        unsync();
        if (valid)
            break;
    }
    r = shot("copper_colour_bars");
    cop_idle();
    ok = (g & MK_A) && (!aga || rd == 0x0123);
    amt_case_open("colour_writes_midline", ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        json_obj("expect");
        json_int("a", 1);
        if (aga)
            json_hexn("color00_readback", 0x0123, 4);
        json_obj_end();
        json_obj("got");
        json_int("a", (g & MK_A) != 0);
        if (aga)
            json_hexn("color00_readback", rd, 4);
        json_obj_end();
        json_str("why", "COLOR00 ($180) is a normal copper target: 96 mid-line WAIT/MOVE pairs run and "
                 "the list goes on to MOVE A; AGA reads the last value back with BPLCON2 RDRAM "
                 "(HRM 2; AGA documentation)");
    }
    json_str("pattern", "COLOR00 bars: 8 per line group at H $40+$14n, lines $60-$BF");
    json_str("shot", r > 0 ? "acked" : r == 0 ? "not_acked" : "no_host");
    amt_case_close();
}

/* ------------------------------------------------------------ timing */
static ULONG cpu_reads(UWORD y0, UWORD y1)
{
    volatile UWORD *p = (volatile UWORD *)(pool + OFF_DW + 16);
    ULONG n = 0;
    wait_line(y0);
    while (hw_vline() < y1) {
        (void)p[0]; (void)p[0]; (void)p[0]; (void)p[0];
        n += 4;
    }
    return n;
}

static void timing_tests(void)
{
    ULONG lo = 0, hi = 0, idle, busy;
    UWORD g;
    int f, i;
    /* 64 MOVEs after a WAIT */
    lst(OFF_L1);
    cwait(0x80, 0x08);
    for (i = 0; i < 64; i++)
        cmove(COLOR00 + 2, (UWORD)i);
    cmark(MK_A);
    cend();
    g = run_markers(OFF_L1, 0x40, 0x50, &lo, &hi, &f, MK_A);
    amt_case_open("timing_move_rate", (g & MK_A) ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!(g & MK_A)) {
        json_obj("expect");
        json_int("a", 1);
        json_obj_end();
        json_obj("got");
        json_int("a", 0);
        json_obj_end();
        json_str("why", "WAIT $8009 then 65 MOVEs must complete (read on line $90)");
    }
    json_obj("timing");
    json_str("unit", "colour clocks from WAIT $8009 to the 65th MOVE (64 COLOR01 + INTREQ); "
             "a MOVE takes 2 copper memory cycles (4 cck) with no other DMA (HRM 2)");
    json_int("found", f);
    if (f) {
        json_int("lo", lo ? (long)hw_lin(lo) - (long)hw_lin(pos(0x80, 0x08)) : -1);
        json_int("hi", (long)hw_lin(hi) - (long)hw_lin(pos(0x80, 0x08)));
    }
    json_obj_end();
    amt_case_close();
    /* CPU chip reads with the copper idle vs running a MOVE loop */
    lst(OFF_L2);
    for (i = 0; i < 32; i++)
        cmove(COLOR00 + 2, (UWORD)i);
    cskip(0xa0, 0x00);
    cmove(COPJMP2, 0);
    cend();
    lst(OFF_L1);
    cwait(0x80, 0x00);
    cmove(COPJMP2, 0);
    cend();
    CUL(COP2LCH) = (ULONG)(pool + OFF_L2);
    sync_line(0x40);
    cop_start(OFF_L1);
    idle = cpu_reads(0x50, 0x70);
    busy = cpu_reads(0x80, 0xa0);
    cop_idle();
    unsync();
    amt_case_open("timing_cpu_vs_copper", AMT_ST_PASS);
    json_obj("timing");
    json_str("unit", "CPU word reads of chip RAM in 32 lines, copper waiting vs copper running MOVEs "
             "back to back (it takes every other memory cycle)");
    json_int("reads_copper_idle", (long)idle);
    json_int("reads_copper_busy", (long)busy);
    json_obj_end();
    amt_case_close();
}

/* ------------------------------------------------------------ main */
static void body(void)
{
    move_tests();
    wait_tests();
    skip_tests();
    jump_tests();
    copen_test();
    bfd_tests();
    danger_tests();
    coper_test();
    colour_test();
    timing_tests();
}

void amt_run_copper(void)
{
    int code;
    UWORD adk = CU(ADKCONR);
    pool = (UBYTE *)amt_chip_pool();
    aga = amt_probe_result.chipset && amt_probe_result.chipset[0] == 'A';
    ecsa = amt_probe_result.ecs_agnus || aga;
    pal = amt_probe_result.pal;
    lst(OFF_IDLE);
    cend();
    code = amt_try(body);
    amt_set_sr(0x2700);
    cop_off();
    amt_irq_hook[3] = 0;
    CU(COPCON) = 0;
    CU(DMACON) = DMAF_BLTEN | DMAF_BLTPRI | DMAF_BPLEN | DMAF_SPREN;
    CU(CLXCON) = 0;
    CU(BPLCON0) = 0x0200;
    CU(ADKCON) = 0x00ff;
    CU(ADKCON) = (UWORD)(0x8000 | (adk & 0x00ff));
    CU(INTENA) = COPER;
    CU(INTREQ) = MK_ALL;
    CU(INTREQ) = MK_ALL;
    CU(COLOR00) = 0x0000;
    hw_restore_copper();
    amt_set_sr(0x2000);
    if (code)
        amt_abort(code);
}
