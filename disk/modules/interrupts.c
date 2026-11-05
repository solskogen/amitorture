/* interrupts: Paula interrupt levels and the 680x0 interrupt logic
 * (CONTRACT.md section 10, module id 6).
 *
 * Expectations: Amiga Hardware Reference Manual (INTENA/INTREQ, interrupt
 * levels), M68000 PRM section 6 (exception processing, interrupts, STOP,
 * MOVE to SR), MC68010/020/030/040 user manuals (format $0 frame with the
 * vector offset). Paula INTREQ bit -> level (HRM table 7-1):
 *   0 TBE, 1 DSKBLK, 2 SOFT: 1   3 PORTS: 2   4 COPER, 5 VERTB, 6 BLIT: 3
 *   7-10 AUD0-3: 4   11 RBF, 12 DSKSYN: 5   13 EXTER: 6   14 INTEN master
 *   15 SET/CLR. Autovector = 24 + level.
 *
 * Handlers live in interrupts_asm.s. VERTB stays alive the whole time (it
 * is chained to the runtime's level-3 handler), so the frame counter and
 * the watchdog keep working. Tests whose log must be exact run inside a
 * "beam window" right after a VBL, so no real VERTB comes in between.
 * CIA-A timer A is used as an interrupt source and CIA-B timers A+B as a
 * 32-bit E-clock reference; both CIAs are saved/restored (cia_hw.h). */
#include "mod.h"
#include "cia_hw.h"
#include "interrupts_clock.h"
#include "hardware_audit.h"

AMT_MODULE(interrupts, 6, AMT_PH_BARE, AMT_MF_OWNS_DISPLAY, 256, 0);

#define MS(n) ((ULONG)(n) * 3547UL)

/* ---- asm (interrupts_asm.s) */
struct ir_ent {
    UWORD tag, sr, ssr, fmt;
    ULONG spc;
    UWORD intreqr, snap;
    ULONG d0;
    UWORD icr, seq;
    ULONG pad[2];
};
void ir_h1(void), ir_h2(void), ir_h3(void), ir_h4(void), ir_h5(void), ir_h6(void);
void ir_snap2(void), ir_st1(void), ir_st2(void), ir_st3(void), ir_lat2(void), ir_lat3(void);
void ir_settle(void);
void ir_do_stop2000(void), ir_do_stop2100(void), ir_do_movesr(void), ir_do_andisr(void);
void ir_do_ccr(void), ir_do_chain(void);
void ir_do_movem(const ULONG *a, const ULONG *b);
extern char ir_after_stop2000[], ir_after_stop2100[], ir_after_movesr[], ir_after_andisr[];
extern char ir_after_ccr[];
extern char ir_chain_start[], ir_chain_end[], ir_chain_t1[], ir_chain_t2[], ir_chain_t3[];
extern char ir_movem_start[], ir_movem_end[], ir_movem_t1[], ir_movem_t2[];
extern struct ir_ent ir_log[64];
extern struct ir_ent *volatile ir_logp;
extern volatile UWORD ir_trig[8], ir_fmt, ir_logvertb, ir_seq, ir_snapdone, ir_snapsr, ir_lat_n;
extern volatile ULONG ir_old27, ir_vertb_n, ir_phantom, ir_snappc, ir_snapregs[15];
extern volatile ULONG ir_st_soft, ir_st_cia, ir_st_l2, ir_st_vb;
extern volatile ULONG ir_lat_buf[64];
extern volatile UBYTE *volatile ir_lat_p;
extern volatile UWORD ir_lat_overflow;

#define SOFT  0x0004
#define PORTS 0x0008
#define COPER 0x0010
#define VERTB 0x0020
#define AUD0  0x0080
#define AUD1  0x0100
#define RBF   0x0800
#define DSKSYN 0x1000
#define EXTER 0x2000

static struct cia_saved sav_a, sav_b;
static UWORD stuck;          /* PORTS/EXTER held by another device */
extern volatile ULONG ir_st_other, ir_runaway;
extern volatile UBYTE ir_kbd_pend;

/* a keyboard byte consumed by a handler's ICR read: give the handshake now
 * (needs VERTB serviced: call at SR=$2000, between cases) */
static void ir_kbd(void)
{
    if (ir_kbd_pend) {
        ir_kbd_pend = 0;
        cia_kbd_handshake();
    }
}

/* A failed case that may depend on PORTS/EXTER: re-check whether another
 * device started holding INT2/INT6 after the module-start check (cia_hw.h).
 * If so the case is untestable, and every later case needing that line
 * too. Returns 1 when it reported the case. */
static int late_stuck(const char *id)
{
    UWORD nb = 0;
    if (!(stuck & PORTS) && cia_int_line_recheck(PORTS))
        nb |= PORTS;
    if (!(stuck & EXTER) && cia_int_line_recheck(EXTER))
        nb |= EXTER;
    if (!nb)
        return 0;
    stuck |= nb;
    amt_case(id, AMT_ST_UNTESTABLE, (nb & PORTS) ? CIA_STUCK_LATE_WHY_PORTS : CIA_STUCK_LATE_WHY_EXTER);
    return 1;
}

/* Cases that need a controllable PORTS/EXTER line report untestable when
 * another device holds it (detected once at module start). */
static int need(const char *id, UWORD bits)
{
    if (!(stuck & bits))
        return 1;
    amt_case(id, AMT_ST_UNTESTABLE, (stuck & bits & PORTS) ? CIA_STUCK_WHY_PORTS : CIA_STUCK_WHY_EXTER);
    return 0;
}
static amt_vec_fn oldvec[8];
static const amt_vec_fn myvec[7] = { 0, ir_h1, ir_h2, ir_h3, ir_h4, ir_h5, ir_h6 };
static int copper_used;

/* one representative source per level */
static const UWORD lvl_bit[7] = { 0, SOFT, PORTS, COPER, AUD0, RBF, EXTER };

/* ------------------------------------------------------------ helpers */
static ULONG beam_v(void)
{
    return amt_beam() >> 8;
}

/* wait until the serial shift register is empty (TSRE), so TBE stays quiet */
static void ser_idle(void)
{
    ULONG t0 = cia_now();
    while (!(CREG(R_SERDATR) & 0x1000) && cia_now() - t0 < MS(10))
        ;
}

/* right after a VBL, at line >= 20: ~240-290 lines before the next VERTB */
static void wait_window(void)
{
    ULONG f0 = amt_frames_now(), t0 = cia_now();
    while (amt_frames_now() == f0 && cia_now() - t0 < MS(60))
        ;
    t0 = cia_now();
    while (beam_v() < 20 && cia_now() - t0 < MS(10))
        ;
}

static void log_reset(void)
{
    int i;
    ir_logp = ir_log;
    ir_seq = 0;
    ir_phantom = 0;
    for (i = 0; i < 8; i++)
        ir_trig[i] = 0;
    ir_logvertb = 0;
}

static int nlog(void) { return (int)(ir_logp - ir_log); }

static int nentries(void)
{
    int i, n = 0;
    for (i = 0; i < nlog(); i++)
        if (!(ir_log[i].tag & 0x80))
            n++;
    return n;
}

/* clean state: mask 7, only INTEN|VERTB enabled, no requests but VERTB */
static void env(void)
{
    ir_kbd();
    ser_idle();
    amt_set_sr(0x2700);
    CREG(R_INTENA) = 0x7fdf;
    CREG(R_INTREQ) = 0x7fdf;
    CREG(R_INTREQ) = 0x7fdf;
    CREG(R_INTENA) = 0xc020;
    log_reset();
    amt_set_sr(0x2000);
}

static void env_end(void)
{
    amt_set_sr(0x2700);
    CREG(R_INTENA) = 0x3fdf;
    CREG(R_INTREQ) = 0x3fdf;
    CREG(R_INTREQ) = 0x3fdf;
    CREG(R_INTENA) = 0xc020;
    ir_logvertb = 0;
    amt_set_sr(0x2000);
    ir_kbd();
}

static void dump_log(const char *key)
{
    int i;
    json_arr(key);
    for (i = 0; i < nlog() && i < 12; i++) {
        struct ir_ent *e = &ir_log[i];
        json_obj(NULL);
        json_hexn("tag", e->tag, 2);
        json_hexn("sr", e->sr, 4);
        if (!(e->tag & 0x80)) {
            json_hexn("ssr", e->ssr, 4);
            json_hexn("spc", e->spc, 8);
            json_hexn("snap", e->snap, 4);
            if (ir_fmt)
                json_hexn("fmt", e->fmt, 4);
            if ((e->tag & 7) == 2 || (e->tag & 7) == 6)
                json_hexn("icr", e->icr, 2);
        }
        json_obj_end();
    }
    json_arr_end();
}

static void dump_tags(const char *key, const UBYTE *t, int n)
{
    int i;
    json_arr(key);
    for (i = 0; i < n; i++)
        json_hexn(NULL, t[i], 2);
    json_arr_end();
}

static int tags_match(const UBYTE *t, int n)
{
    int i;
    if (nlog() != n)
        return 0;
    for (i = 0; i < n; i++)
        if (ir_log[i].tag != t[i])
            return 0;
    return 1;
}

static void fail_log(const char *id, const char *why, const UBYTE *exp, int n)
{
    if (late_stuck(id))
        return;
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    dump_tags("tags", exp, n);
    json_obj_end();
    json_obj("got");
    dump_log("log");
    json_int("phantom", (long)ir_phantom);
    json_obj_end();
    json_str("why", why);
    json_str("tag_note", "tag = level on handler entry, level|$80 on exit");
    amt_case_close();
}

/* CIA-A timer A one-shot with force load */
static void ciaa_ta_fire(UWORD latch)
{
    cia_wr(CIAA_BASE, C_CRA, 0);
    cia_wr(CIAA_BASE, C_TALO, (UBYTE)latch);
    cia_wr(CIAA_BASE, C_TAHI, (UBYTE)(latch >> 8));
    cia_wr(CIAA_BASE, C_CRA, CR_RUNMODE | CR_LOAD | CR_START);
}

static int wait_entries(int n, ULONG to)
{
    ULONG t0 = cia_now();
    while (nentries() < n)
        if (cia_now() - t0 > to)
            return 0;
    return 1;
}

/* ------------------------------------------------------------ map */
static const struct { const char *id; UBYTE bit, lvl; const char *name; } map[] = {
    { "map_tbe", 0, 1, "TBE" },       { "map_dskblk", 1, 1, "DSKBLK" },
    { "map_softint", 2, 1, "SOFT" },  { "map_ports", 3, 2, "PORTS" },
    { "map_coper", 4, 3, "COPER" },   { "map_vertb", 5, 3, "VERTB" },
    { "map_blit", 6, 3, "BLIT" },     { "map_aud0", 7, 4, "AUD0" },
    { "map_aud1", 8, 4, "AUD1" },     { "map_aud2", 9, 4, "AUD2" },
    { "map_aud3", 10, 4, "AUD3" },    { "map_rbf", 11, 5, "RBF" },
    { "map_dsksyn", 12, 5, "DSKSYN" },{ "map_exter", 13, 6, "EXTER" },
};

static void map_tests(void)
{
    int i, fmt_bad = -1;
    UWORD fmt_got = 0;
    for (i = 0; i < 14; i++) {
        UWORD m = (UWORD)(1u << map[i].bit);
        UBYTE lvl = map[i].lvl;
        struct ir_ent *e = ir_log;
        int n, ok;
        if (!need(map[i].id, m & (PORTS | EXTER)))
            continue;
        env();
        wait_window();
        amt_set_sr(0x2700);
        if (m == VERTB)
            ir_logvertb = 1;
        CREG(R_INTREQ) = m;
        CREG(R_INTREQ) = m;
        CREG(R_INTENA) = 0x8000 | m;
        CREG(R_INTREQ) = 0x8000 | m;
        ir_settle();
        amt_set_sr(0x2000);
        ir_settle();
        ir_settle();
        n = nentries();
        if (m != VERTB)
            CREG(R_INTENA) = m;
        ok = n == 1 && e->tag == lvl && (e->snap & m) &&
             (e->sr & 0xff00) == (0x2000 | (lvl << 8)) && (e->ssr & 0xff00) == 0x2000;
        if (ok && ir_fmt && e->fmt != (UWORD)((24 + lvl) * 4) && fmt_bad < 0) {
            fmt_bad = i;
            fmt_got = e->fmt;
        }
        if (ok)
            amt_case(map[i].id, AMT_ST_PASS, 0);
        else if (!late_stuck(map[i].id)) {
            amt_case_open(map[i].id, AMT_ST_FAIL);
            json_obj("expect");
            json_int("entries", 1);
            json_int("level", lvl);
            json_int("vector", 24 + lvl);
            json_hexn("sr_in_handler", 0x2000 | (lvl << 8), 4);
            json_hexn("stacked_sr", 0x2000, 4);
            json_obj_end();
            json_obj("got");
            json_int("entries", n);
            dump_log("log");
            json_int("phantom", (long)ir_phantom);
            json_obj_end();
            json_obj("in");
            json_str("source", map[i].name);
            json_hexn("intena", 0xc000 | m, 4);
            json_hexn("intreq", 0x8000 | m, 4);
            json_hexn("sr_before", 0x2000, 4);
            json_obj_end();
            json_str("why", "an INTREQ SET write of this bit with INTEN and the bit enabled must cause "
                     "exactly one interrupt at the HRM level, through autovector 24+level, with SR "
                     "S=1 T=0 mask=level in the handler and the old SR stacked (compared bits $FF00)");
            amt_case_close();
        }
        env_end();
    }
    if (!ir_fmt)
        amt_case("autovector_format_word", AMT_ST_SKIP, "68000: no format/vector word in the frame");
    else if (fmt_bad < 0)
        amt_case("autovector_format_word", AMT_ST_PASS, 0);
    else {
        amt_case_open("autovector_format_word", AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("fmt", (24 + map[fmt_bad].lvl) * 4, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("fmt", fmt_got, 4);
        json_obj_end();
        json_obj("in");
        json_str("source", map[fmt_bad].name);
        json_obj_end();
        json_str("why", "68010+: an autovectored interrupt stacks a format $0 frame whose vector "
                 "offset is (24+level)*4");
        amt_case_close();
    }
}

/* ------------------------------------------------------------ enables */
static void enable_tests(void)
{
    int n1, n2;
    UWORD rq;

    /* INTEN master */
    env();
    wait_window();
    amt_set_sr(0x2700);
    CREG(R_INTENA) = 0x4000;
    CREG(R_INTENA) = 0x8000 | SOFT;
    CREG(R_INTREQ) = 0x8000 | SOFT;
    ir_settle();
    amt_set_sr(0x2000);
    ir_settle();
    n1 = nentries();
    CREG(R_INTENA) = 0xc000;
    ir_settle();
    ir_settle();
    n2 = nentries();
    env_end();
    if (n1 == 0 && n2 == 1)
        amt_case("inten_master", AMT_ST_PASS, 0);
    else if (!late_stuck("inten_master")) {
        amt_case_open("inten_master", AMT_ST_FAIL);
        json_obj("expect");
        json_int("taken_inten_off", 0);
        json_int("taken_after_inten_on", 1);
        json_obj_end();
        json_obj("got");
        json_int("taken_inten_off", n1);
        json_int("taken_after_inten_on", n2);
        json_obj_end();
        json_str("why", "INTENA bit 14 (INTEN) is the master enable: with it clear no Paula interrupt "
                 "reaches the CPU; setting it lets the pending SOFT request through");
        amt_case_close();
    }

    /* source enable bit */
    env();
    wait_window();
    amt_set_sr(0x2700);
    CREG(R_INTREQ) = 0x8000 | SOFT;
    ir_settle();
    amt_set_sr(0x2000);
    ir_settle();
    n1 = nentries();
    rq = CREG(R_INTREQR) & SOFT;
    CREG(R_INTENA) = 0x8000 | SOFT;
    ir_settle();
    ir_settle();
    n2 = nentries();
    env_end();
    if (n1 == 0 && rq && n2 == 1)
        amt_case("intena_source_off", AMT_ST_PASS, 0);
    else if (!late_stuck("intena_source_off")) {
        amt_case_open("intena_source_off", AMT_ST_FAIL);
        json_obj("expect");
        json_int("taken_disabled", 0);
        json_hexn("intreqr_soft", SOFT, 4);
        json_int("taken_after_enable", 1);
        json_obj_end();
        json_obj("got");
        json_int("taken_disabled", n1);
        json_hexn("intreqr_soft", rq, 4);
        json_int("taken_after_enable", n2);
        json_obj_end();
        json_str("why", "a request whose INTENA bit is clear stays pending in INTREQR and is taken "
                 "as soon as the bit is enabled");
        amt_case_close();
    }
}

/* ------------------------------------------------------------ SR mask */
static void mask_tests(void)
{
    static const char *const ids[7] = { 0, "sr_mask_l1", "sr_mask_l2", "sr_mask_l3",
                                        "sr_mask_l4", "sr_mask_l5", "sr_mask_l6" };
    int l, m;
    for (l = 1; l <= 6; l++) {
        UWORD b = lvl_bit[l];
        UBYTE taken = 0, after = 0, exp = (UBYTE)((1u << l) - 1);
        if (!need(ids[l], b & (PORTS | EXTER)))
            continue;
        env();
        wait_window();
        for (m = 0; m <= 7; m++) {
            amt_set_sr(0x2700);
            log_reset();
            CREG(R_INTENA) = 0x8000 | b;
            CREG(R_INTREQ) = 0x8000 | b;
            ir_settle();
            amt_set_sr((UWORD)(0x2000 | (m << 8)));
            ir_settle();
            ir_settle();
            if (nentries() > 0)
                taken |= (UBYTE)(1u << m);
            amt_set_sr(0x2000);
            ir_settle();
            ir_settle();
            if (nentries() == 1)
                after |= (UBYTE)(1u << m);
            CREG(R_INTENA) = b;
        }
        env_end();
        if (taken == exp && after == 0xff)
            amt_case(ids[l], AMT_ST_PASS, 0);
        else if (!late_stuck(ids[l])) {
            amt_case_open(ids[l], AMT_ST_FAIL);
            json_obj("expect");
            json_hexn("taken_at_mask", exp, 2);
            json_hexn("taken_once_after_mask0", 0xff, 2);
            json_obj_end();
            json_obj("got");
            json_hexn("taken_at_mask", taken, 2);
            json_hexn("taken_once_after_mask0", after, 2);
            json_obj_end();
            json_obj("in");
            json_int("level", l);
            json_hexn("intreq_bit", b, 4);
            json_str("bits", "bit m = SR mask m (0-7)");
            json_obj_end();
            json_str("why", "M68000 PRM 6.3.2: a level-n request is taken only when n > the SR "
                     "interrupt mask (mask 7 blocks levels 1-6); a masked request stays pending and "
                     "is taken exactly once when the mask is lowered");
            amt_case_close();
        }
    }
}

/* ------------------------------------------------------------ priority / nesting */
static void prio_tests(void)
{
    static const UBYTE exp[12] = { 6, 0x86, 5, 0x85, 4, 0x84, 3, 0x83, 2, 0x82, 1, 0x81 };
    UWORD all = SOFT | PORTS | COPER | AUD0 | RBF | EXTER;
    int i, same = 1;

    if (!need("priority_order", PORTS | EXTER)) {
        need("priority_back_to_back_pc", PORTS | EXTER);
        goto nesting;
    }
    env();
    wait_window();
    amt_set_sr(0x2700);
    CREG(R_INTENA) = 0x8000 | all;
    CREG(R_INTREQ) = 0x8000 | all;
    ir_settle();
    amt_set_sr(0x2000);
    ir_settle();
    ir_settle();
    CREG(R_INTENA) = all;
    if (tags_match(exp, 12))
        amt_case("priority_order", AMT_ST_PASS, 0);
    else
        fail_log("priority_order", "requests pending on all six levels with mask 7, then mask "
                 "lowered to 0: the highest level is taken first, each handler runs to completion "
                 "(RTE) before the next lower one (M68000 PRM 6.3.2)", exp, 12);
    for (i = 2; i < nlog(); i += 2)
        if (ir_log[i].spc != ir_log[0].spc)
            same = 0;
    if (nlog() == 12 && same)
        amt_case("priority_back_to_back_pc", AMT_ST_PASS, 0);
    else if (!late_stuck("priority_back_to_back_pc")) {
        amt_case_open("priority_back_to_back_pc", AMT_ST_FAIL);
        json_obj("expect");
        json_str("spc", "all six stacked PCs equal");
        json_obj_end();
        json_obj("got");
        dump_log("log");
        json_obj_end();
        json_str("why", "after each RTE the next pending interrupt is taken at that instruction "
                 "boundary, before any instruction of the interrupted code runs");
        amt_case_close();
    }
    env_end();

nesting:
    /* higher preempts lower: SOFT handler raises AUD0 (level 4) */
    {
        static const UBYTE e1[4] = { 1, 4, 0x84, 0x81 };
        env();
        wait_window();
        CREG(R_INTENA) = 0x8000 | SOFT | AUD0;
        ir_trig[1] = 0x8000 | AUD0;
        CREG(R_INTREQ) = 0x8000 | SOFT;
        ir_settle();
        ir_settle();
        ir_settle();
        if (tags_match(e1, 4) && (ir_log[1].ssr & 0x0700) == 0x0100)
            amt_case("nest_higher_preempts", AMT_ST_PASS, 0);
        else
            fail_log("nest_higher_preempts", "a level-4 request raised inside the level-1 handler "
                     "(mask 1) must interrupt it at once; the level-4 frame stacks mask 1", e1, 4);
        env_end();
    }
    /* lower waits: AUD0 handler raises SOFT */
    {
        static const UBYTE e2[4] = { 4, 0x84, 1, 0x81 };
        env();
        wait_window();
        CREG(R_INTENA) = 0x8000 | SOFT | AUD0;
        ir_trig[4] = 0x8000 | SOFT;
        CREG(R_INTREQ) = 0x8000 | AUD0;
        ir_settle();
        ir_settle();
        ir_settle();
        if (tags_match(e2, 4))
            amt_case("nest_lower_waits", AMT_ST_PASS, 0);
        else
            fail_log("nest_lower_waits", "a level-1 request raised inside the level-4 handler must "
                     "wait until that handler returns", e2, 4);
        env_end();
    }
    /* same level waits: AUD0 handler raises AUD1 */
    {
        static const UBYTE e3[4] = { 4, 0x84, 4, 0x84 };
        env();
        wait_window();
        CREG(R_INTENA) = 0x8000 | AUD0 | AUD1;
        ir_trig[4] = 0x8000 | AUD1;
        CREG(R_INTREQ) = 0x8000 | AUD0;
        ir_settle();
        ir_settle();
        ir_settle();
        if (tags_match(e3, 4) && (ir_log[2].snap & AUD1))
            amt_case("nest_same_level_waits", AMT_ST_PASS, 0);
        else
            fail_log("nest_same_level_waits", "a request of the same level (AUD1 inside the AUD0 "
                     "handler) is not taken while the mask equals its level (n > mask is required)",
                     e3, 4);
        env_end();
    }
    /* three deep: 1 -> 3 (COPER) -> 5 (DSKSYN) */
    {
        static const UBYTE e4[6] = { 1, 3, 5, 0x85, 0x83, 0x81 };
        env();
        wait_window();
        CREG(R_INTENA) = 0x8000 | SOFT | COPER | DSKSYN;
        ir_trig[1] = 0x8000 | COPER;
        ir_trig[3] = 0x8000 | DSKSYN;
        CREG(R_INTREQ) = 0x8000 | SOFT;
        ir_settle();
        ir_settle();
        ir_settle();
        if (tags_match(e4, 6) && (ir_log[1].ssr & 0x0700) == 0x0100 &&
            (ir_log[2].ssr & 0x0700) == 0x0300)
            amt_case("nest_three_deep", AMT_ST_PASS, 0);
        else
            fail_log("nest_three_deep", "level 1 handler raises level 3, whose handler raises "
                     "level 5: each preempts the one below, stacking masks 1 and 3", e4, 6);
        env_end();
    }
}

/* ------------------------------------------------------------ mask lowering */
static void pc_case(const char *id, int lvl, ULONG want, ULONG want_ssr, const char *why)
{
    struct ir_ent *e = ir_log;
    int n = nentries();
    if (n == 1 && e->tag == lvl && e->spc == want && e->ssr == want_ssr)
        amt_case(id, AMT_ST_PASS, 0);
    else if (!late_stuck(id)) {
        amt_case_open(id, AMT_ST_FAIL);
        json_obj("expect");
        json_int("level", lvl);
        json_hexn("spc", want, 8);
        json_hexn("ssr", want_ssr, 4);
        json_obj_end();
        json_obj("got");
        json_int("entries", n);
        dump_log("log");
        if (n >= 1)
            json_int("spc_minus_expected", (long)(e->spc - want));
        json_obj_end();
        json_str("why", why);
        amt_case_close();
    }
}

static void lower_tests(void)
{
    struct ir_ent *e = ir_log;
    env();
    wait_window();
    CREG(R_INTENA) = 0x8000 | SOFT;
    ir_do_movesr();
    CREG(R_INTENA) = SOFT;
    pc_case("movesr_lower_taken_next", 1, (ULONG)ir_after_movesr, 0x2000,
            "SOFT pending under mask 7; MOVE #$2000,SR lowers the mask: the interrupt is taken at the "
            "next instruction boundary, stacking the PC of the instruction after the MOVE");
    env_end();

    env();
    wait_window();
    CREG(R_INTENA) = 0x8000 | SOFT;
    ir_do_andisr();
    CREG(R_INTENA) = SOFT;
    pc_case("andisr_lower_taken_next", 1, (ULONG)ir_after_andisr, 0x2000,
            "SOFT pending under mask 7; ANDI #$F8FF,SR lowers the mask: the interrupt is taken "
            "before the next instruction");
    env_end();

    env();
    wait_window();
    CREG(R_INTENA) = 0x8000 | AUD0;
    ir_do_ccr();
    CREG(R_INTENA) = AUD0;
    pc_case("stacked_sr_is_pre_interrupt", 4, (ULONG)ir_after_ccr, 0x2315,
            "level 4 taken when MOVE #$2315,SR lowers the mask to 3: the frame holds the SR before "
            "the exception ($2315, CCR included) and the PC after the MOVE");
    if (nentries() == 1 && e->sr == 0x2415)
        amt_case("handler_sr_new_mask", AMT_ST_PASS, 0);
    else if (!late_stuck("handler_sr_new_mask")) {
        amt_case_open("handler_sr_new_mask", AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("sr", 0x2415, 4);
        json_obj_end();
        json_obj("got");
        dump_log("log");
        json_obj_end();
        json_str("why", "M68000 PRM 6.3.2: interrupt processing sets S, clears T, sets the mask to "
                 "the level (4) and leaves the CCR alone: $2315 -> $2415 in the handler");
        amt_case_close();
    }
    env_end();
}

/* ------------------------------------------------------------ registers */
static void reg_tests(void)
{
    static const UWORD pats[] = { 0x0001, 0x0002, 0x0004, 0x0008, 0x0010, 0x0020, 0x0040, 0x0080,
                                  0x0100, 0x0200, 0x0400, 0x0800, 0x1000, 0x2000, 0x4000,
                                  0x5555, 0x2aaa, 0x7fff };
    int i, bad = -1, step = -1;
    UWORD g = 0, w = 0, r[5];

    /* INTENAR */
    env();
    wait_window();
    amt_set_sr(0x2700);
    for (i = 0; i < (int)(sizeof(pats) / 2) && bad < 0; i++) {
        CREG(R_INTENA) = 0x7fff;
        CREG(R_INTENA) = 0x8000 | pats[i];
        g = CREG(R_INTENAR) & 0x7fff;
        if (g != pats[i])
            bad = i;
    }
    CREG(R_INTENA) = 0x7fff;
    CREG(R_INTENA) = 0x8000 | 0x00f0;
    r[0] = CREG(R_INTENAR) & 0x7fff;
    CREG(R_INTENA) = 0x8000 | 0x0f00;
    r[1] = CREG(R_INTENAR) & 0x7fff;
    CREG(R_INTENA) = 0x0030;
    r[2] = CREG(R_INTENAR) & 0x7fff;
    CREG(R_INTENA) = 0x8000;
    r[3] = CREG(R_INTENAR) & 0x7fff;
    CREG(R_INTENA) = 0x0000;
    r[4] = CREG(R_INTENAR) & 0x7fff;
    CREG(R_INTENA) = 0x7fff;
    CREG(R_INTENA) = 0xc020;
    amt_set_sr(0x2000);
    if (bad < 0)
        amt_case("intenar_readback", AMT_ST_PASS, 0);
    else
        amt_check32("intenar_readback", "intenar", pats[bad], g,
                    "INTENAR must read back the bits set with INTENA = $8000|bits (bit 15 masked)");
    {
        static const UWORD ex[5] = { 0x00f0, 0x0ff0, 0x0fc0, 0x0fc0, 0x0fc0 };
        for (i = 0; i < 5; i++)
            if (r[i] != ex[i] && step < 0)
                step = i;
        if (step < 0)
            amt_case("intena_setclr", AMT_ST_PASS, 0);
        else {
            amt_case_open("intena_setclr", AMT_ST_FAIL);
            json_obj("expect");
            json_arr("intenar");
            for (i = 0; i < 5; i++)
                json_hexn(NULL, ex[i], 4);
            json_arr_end();
            json_obj_end();
            json_obj("got");
            json_arr("intenar");
            for (i = 0; i < 5; i++)
                json_hexn(NULL, r[i], 4);
            json_arr_end();
            json_obj_end();
            json_obj("in");
            json_str("writes", "$7FFF, $80F0, $8F00, $0030, $8000, $0000");
            json_obj_end();
            json_str("why", "HRM: bit 15 SET/CLR: 1 sets the bits given, 0 clears them; bits written "
                     "as 0 are unaffected either way");
            amt_case_close();
        }
    }

    /* INTREQR (bit 14 excluded: not a request bit) */
    bad = -1;
    step = -1;
    env();
    wait_window();
    amt_set_sr(0x2700);
    CREG(R_INTENA) = 0x7fff;          /* nothing taken; VERTB off for a moment */
    for (i = 0; i < (int)(sizeof(pats) / 2) && bad < 0; i++) {
        w = pats[i] & 0x3fff;
        if (!w)
            continue;
        CREG(R_INTREQ) = 0x3fff;
        CREG(R_INTREQ) = 0x8000 | w;
        g = CREG(R_INTREQR) & 0x3fff & ~stuck;
        w &= ~stuck;
        if (g != w)
            bad = i;
    }
    CREG(R_INTREQ) = 0x3fff;
    CREG(R_INTREQ) = 0x8000 | 0x00f0;
    r[0] = CREG(R_INTREQR) & 0x3fff & ~stuck;
    CREG(R_INTREQ) = 0x8000 | 0x0f00;
    r[1] = CREG(R_INTREQR) & 0x3fff & ~stuck;
    CREG(R_INTREQ) = 0x0030;
    r[2] = CREG(R_INTREQR) & 0x3fff & ~stuck;
    CREG(R_INTREQ) = 0x8000;
    r[3] = CREG(R_INTREQR) & 0x3fff & ~stuck;
    CREG(R_INTREQ) = 0x0000;
    r[4] = CREG(R_INTREQR) & 0x3fff & ~stuck;
    CREG(R_INTREQ) = 0x3fff;
    CREG(R_INTREQ) = 0x3fff;
    CREG(R_INTENA) = 0xc020;
    amt_set_sr(0x2000);
    if (bad < 0)
        amt_case("intreqr_readback", AMT_ST_PASS, 0);
    else if (!late_stuck("intreqr_readback"))
        amt_check32("intreqr_readback", "intreqr", w, g,
                    "INTREQR (bits 0-13) must read back the bits set with INTREQ = $8000|bits");
    {
        static const UWORD ex[5] = { 0x00f0, 0x0ff0, 0x0fc0, 0x0fc0, 0x0fc0 };
        for (i = 0; i < 5; i++)
            if (r[i] != ex[i] && step < 0)
                step = i;
        if (step < 0)
            amt_case("intreq_setclr", AMT_ST_PASS, 0);
        else if (!late_stuck("intreq_setclr")) {
            amt_case_open("intreq_setclr", AMT_ST_FAIL);
            json_obj("expect");
            json_arr("intreqr");
            for (i = 0; i < 5; i++)
                json_hexn(NULL, ex[i], 4);
            json_arr_end();
            json_obj_end();
            json_obj("got");
            json_arr("intreqr");
            for (i = 0; i < 5; i++)
                json_hexn(NULL, r[i], 4);
            json_arr_end();
            json_obj_end();
            json_obj("in");
            json_str("writes", "$3FFF, $80F0, $8F00, $0030, $8000, $0000");
            json_obj_end();
            json_str("why", "HRM: INTREQ bit 15 SET/CLR semantics as for INTENA");
            amt_case_close();
        }
    }
}

/* ------------------------------------------------------------ CIA sources */
static void cia_irq_tests(void)
{
    struct ir_ent *e = ir_log;
    int got;

    if (!need("cia_a_timer_level2", PORTS))
        goto ciab;
    env();
    wait_window();
    cia_wr(CIAA_BASE, C_ICR, 0x7f);
    (void)cia_icr(CIAA_BASE);
    cia_wr(CIAA_BASE, C_ICR, 0x81);
    CREG(R_INTREQ) = PORTS;
    CREG(R_INTENA) = 0x8000 | PORTS;
    ciaa_ta_fire(0x0100);
    got = wait_entries(1, MS(5));
    ir_settle();
    CREG(R_INTENA) = PORTS;
    cia_wr(CIAA_BASE, C_ICR, 0x7f);
    cia_wr(CIAA_BASE, C_CRA, 0);
    if (got && nentries() == 1 && e->tag == 2 && (e->icr & 0x83) == 0x81)
        amt_case("cia_a_timer_level2", AMT_ST_PASS, 0);
    else if (!late_stuck("cia_a_timer_level2")) {
        amt_case_open("cia_a_timer_level2", AMT_ST_FAIL);
        json_obj("expect");
        json_int("level", 2);
        json_hexn("icr_in_handler", 0x81, 2);
        json_obj_end();
        json_obj("got");
        json_int("entries", nentries());
        dump_log("log");
        json_obj_end();
        json_obj("in");
        json_hexn("ciaa_icr_mask", 0x81, 2);
        json_int("ta_latch", 0x100);
        json_obj_end();
        json_str("why", "a CIA-A timer A underflow enabled in its ICR mask must raise INT2 (PORTS) and "
                 "be taken once at level 2; the handler reads ICR = $81");
        amt_case_close();
    }
    env_end();

ciab:
    if (!need("cia_b_timer_level6", EXTER))
        return;
    env();
    wait_window();
    cia_wr(CIAB_BASE, C_ICR, 0x7f);
    (void)cia_icr(CIAB_BASE);
    cia_wr(CIAB_BASE, C_ICR, 0x81);
    CREG(R_INTREQ) = EXTER;
    CREG(R_INTENA) = 0x8000 | EXTER;
    cia_wr(CIAB_BASE, C_CRA, 0);
    cia_wr(CIAB_BASE, C_TALO, 0x00);
    cia_wr(CIAB_BASE, C_TAHI, 0x01);
    cia_wr(CIAB_BASE, C_CRA, CR_RUNMODE | CR_LOAD | CR_START);
    got = wait_entries(1, MS(5));
    ir_settle();
    CREG(R_INTENA) = EXTER;
    cia_wr(CIAB_BASE, C_ICR, 0x7f);
    cia_wr(CIAB_BASE, C_CRA, 0);
    if (got && nentries() == 1 && e->tag == 6 && (e->icr & 0x83) == 0x81)
        amt_case("cia_b_timer_level6", AMT_ST_PASS, 0);
    else if (!late_stuck("cia_b_timer_level6")) {
        amt_case_open("cia_b_timer_level6", AMT_ST_FAIL);
        json_obj("expect");
        json_int("level", 6);
        json_hexn("icr_in_handler", 0x81, 2);
        json_obj_end();
        json_obj("got");
        json_int("entries", nentries());
        dump_log("log");
        json_obj_end();
        json_obj("in");
        json_hexn("ciab_icr_mask", 0x81, 2);
        json_int("ta_latch", 0x100);
        json_obj_end();
        json_str("why", "a CIA-B timer A underflow enabled in its ICR mask must raise INT6 (EXTER) and "
                 "be taken once at level 6; the handler reads ICR = $81");
        amt_case_close();
    }
    env_end();
}

/* ------------------------------------------------------------ STOP */
static void stop_tests(void)
{
    struct ir_ent *e = ir_log;
    ULONG t0, t1;
    int ok;

    /* woken by a CIA-A timer (level 2); VERTB logged as the fallback waker */
    if (!need("stop_wakes_pc_after", PORTS))
        goto pending;
    env();
    wait_window();
    ir_logvertb = 1;
    cia_wr(CIAA_BASE, C_ICR, 0x7f);
    (void)cia_icr(CIAA_BASE);
    cia_wr(CIAA_BASE, C_ICR, 0x81);
    CREG(R_INTENA) = 0x8000 | PORTS;
    t0 = cia_now();
    ciaa_ta_fire(2000);
    ir_do_stop2000();
    t1 = cia_now();
    CREG(R_INTENA) = PORTS;
    cia_wr(CIAA_BASE, C_ICR, 0x7f);
    ir_logvertb = 0;
    ok = nentries() >= 1 && e->tag == 2 && e->spc == (ULONG)ir_after_stop2000 &&
         e->ssr == 0x2000 && (e->icr & 0x81) == 0x81;
    if (ok) {
        amt_case_open("stop_wakes_pc_after", AMT_ST_PASS);
        json_obj("timing");
        json_int("stopped_cc", (long)(t1 - t0));
        json_int("timer_cc", 2001 * 5);
        json_obj_end();
        amt_case_close();
    } else if (!late_stuck("stop_wakes_pc_after")) {
        amt_case_open("stop_wakes_pc_after", AMT_ST_FAIL);
        json_obj("expect");
        json_int("first_level", 2);
        json_hexn("spc", (ULONG)ir_after_stop2000, 8);
        json_hexn("ssr", 0x2000, 4);
        json_obj_end();
        json_obj("got");
        dump_log("log");
        json_int("elapsed_cc", (long)(t1 - t0));
        json_obj_end();
        json_obj("in");
        json_str("insn", "STOP #$2000");
        json_str("waker", "CIA-A TA one-shot, 2001 E clocks (VERTB at level 3 as a fallback)");
        json_obj_end();
        json_str("why", "M68000 PRM STOP: loads SR and stops until an interrupt above the new mask; "
                 "the interrupt frame holds the PC of the instruction after STOP and the SR "
                 "loaded by STOP");
        amt_case_close();
    }
    env_end();

pending:
    /* already pending: taken right away */
    env();
    amt_set_sr(0x2700);
    CREG(R_INTENA) = 0x8000 | SOFT;
    CREG(R_INTREQ) = 0x8000 | SOFT;
    ir_settle();
    ir_do_stop2000();
    CREG(R_INTENA) = SOFT;
    pc_case("stop_pending_taken_at_once", 1, (ULONG)ir_after_stop2000, 0x2000,
            "SOFT pending when STOP #$2000 lowers the mask: taken at once, stacked PC after STOP");
    env_end();

    /* blocked level stays stopped until a higher one */
    {
        static const UBYTE ex[4] = { 2, 0x82, 1, 0x81 };
        int n_stop;
        if (!need("stop_masked_level_waits", PORTS))
            return;
        env();
        wait_window();
        ir_logvertb = 1;
        amt_set_sr(0x2700);
        CREG(R_INTENA) = 0x8000 | SOFT | PORTS;
        CREG(R_INTREQ) = 0x8000 | SOFT;
        cia_wr(CIAA_BASE, C_ICR, 0x7f);
        (void)cia_icr(CIAA_BASE);
        cia_wr(CIAA_BASE, C_ICR, 0x81);
        ciaa_ta_fire(2000);
        ir_do_stop2100();
        n_stop = nlog();
        amt_set_sr(0x2000);
        ir_settle();
        ir_settle();
        CREG(R_INTENA) = SOFT | PORTS;
        cia_wr(CIAA_BASE, C_ICR, 0x7f);
        ir_logvertb = 0;
        if (tags_match(ex, 4) && n_stop == 2 && ir_log[0].spc == (ULONG)ir_after_stop2100 &&
            ir_log[0].ssr == 0x2100)
            amt_case("stop_masked_level_waits", AMT_ST_PASS, 0);
        else if (!late_stuck("stop_masked_level_waits")) {
            amt_case_open("stop_masked_level_waits", AMT_ST_FAIL);
            json_obj("expect");
            dump_tags("tags", ex, 4);
            json_int("records_before_mask0", 2);
            json_hexn("spc", (ULONG)ir_after_stop2100, 8);
            json_hexn("ssr", 0x2100, 4);
            json_obj_end();
            json_obj("got");
            dump_log("log");
            json_int("records_before_mask0", n_stop);
            json_obj_end();
            json_obj("in");
            json_str("insn", "STOP #$2100 with SOFT (level 1) pending");
            json_str("waker", "CIA-A TA one-shot 2001 E (level 2); VERTB (level 3) as a fallback");
            json_obj_end();
            json_str("why", "a pending level-1 request does not end STOP #$2100 (1 is not > 1); the "
                     "level-2 timer does, and SOFT is taken only when the mask drops to 0");
            amt_case_close();
        }
        env_end();
    }
}

/* ------------------------------------------------------------ boundaries */
static void chain_tests(void)
{
    int trials = AMT_QUICK() ? 3 : 8, t, bad = -1;
    ULONG bpc = 0, bd0 = 0, blatch = 0;
    UWORD latch;
    amt_vec_fn old;

    if (!need("irq_between_instructions", PORTS)) {
        need("irq_after_movem", PORTS);
        return;
    }
    old = amt_vec_set(26, ir_snap2);
    env();
    cia_wr(CIAA_BASE, C_ICR, 0x7f);
    (void)cia_icr(CIAA_BASE);
    cia_wr(CIAA_BASE, C_ICR, 0x81);
    CREG(R_INTENA) = 0x8000 | PORTS;
    for (t = 0; t < trials; t++) {
        ULONG p, d0, s = (ULONG)ir_chain_start;
        int ok;
        latch = (UWORD)(200 + amt_rand() % 1500);
        ir_snapdone = 0;
        CREG(R_INTENA) = 0x8000 | PORTS;
        ciaa_ta_fire(latch);
        ir_do_chain();
        p = ir_snappc;
        d0 = ir_snapregs[0];
        if (p >= s && p < (ULONG)ir_chain_end)
            ok = !((p - s) & 1) && d0 == (p - s) / 2;
        else if (p == (ULONG)ir_chain_end || p == (ULONG)ir_chain_t1 || p == (ULONG)ir_chain_t2)
            ok = d0 == 2000;
        else if (p == (ULONG)ir_chain_t3)
            ok = d0 == 0;
        else
            ok = 0;
        if (!ok && bad < 0) {
            bad = t;
            bpc = p;
            bd0 = d0;
            blatch = latch;
        }
    }
    CREG(R_INTENA) = PORTS;
    cia_wr(CIAA_BASE, C_ICR, 0x7f);
    if (bad < 0)
        amt_case("irq_between_instructions", AMT_ST_PASS, 0);
    else if (!late_stuck("irq_between_instructions")) {
        amt_case_open("irq_between_instructions", AMT_ST_FAIL);
        json_obj("expect");
        json_str("rule", "stacked PC on an ADDQ boundary and D0 == (PC - start)/2");
        json_obj_end();
        json_obj("got");
        json_hex("spc_minus_start", bpc - (ULONG)ir_chain_start);
        json_hex("d0", bd0);
        json_obj_end();
        json_obj("in");
        json_int("trial", bad);
        json_int("ta_latch", (long)blatch);
        json_str("code", "moveq #0,d0; 2000 x addq.l #1,d0 (2 bytes each); tst.w; bne; moveq #0,d0; bra");
        json_obj_end();
        json_str("why", "interrupts are taken between instructions: the registers at the interrupt "
                 "must match the instructions completed before the stacked PC");
        amt_case_close();
    }

    /* MOVEM */
    {
        static ULONG ba[13], bb[13];
        int i, j, mism = -1;
        for (i = 0; i < 13; i++) {
            ba[i] = 0xa5000000UL | ((ULONG)i << 16) | (ULONG)(i * 0x101);
            bb[i] = ~ba[i];
        }
        bad = -1;
        CREG(R_INTENA) = 0x8000 | PORTS;
        cia_wr(CIAA_BASE, C_ICR, 0x81);
        for (t = 0; t < trials; t++) {
            ULONG p = 0, s = (ULONG)ir_movem_start;
            const ULONG *want = 0;
            latch = (UWORD)(200 + amt_rand() % 1500);
            ir_snapdone = 0;
            CREG(R_INTENA) = 0x8000 | PORTS;
            ciaa_ta_fire(latch);
            ir_do_movem(ba, bb);
            p = ir_snappc;
            if (p >= s && p < (ULONG)ir_movem_end && !((p - s) & 3)) {
                ULONG k = (p - s) / 4;
                want = (k == 0 || ((k - 1) & 1)) ? bb : ba;
            } else if (p == (ULONG)ir_movem_end || p == (ULONG)ir_movem_t1 ||
                       p == (ULONG)ir_movem_t2)
                want = bb;
            mism = -1;
            if (want) {
                for (j = 0; j < 8 && mism < 0; j++)
                    if (ir_snapregs[j] != want[j])
                        mism = j;
                for (j = 0; j < 5 && mism < 0; j++)
                    if (ir_snapregs[10 + j] != want[8 + j])
                        mism = 8 + j;
            }
            if ((!want || mism >= 0) && bad < 0) {
                bad = t;
                bpc = p;
                blatch = latch;
                break;
            }
        }
        CREG(R_INTENA) = PORTS;
        cia_wr(CIAA_BASE, C_ICR, 0x7f);
        if (bad < 0)
            amt_case("irq_after_movem", AMT_ST_PASS, 0);
        else if (!late_stuck("irq_after_movem")) {
            amt_case_open("irq_after_movem", AMT_ST_FAIL);
            json_obj("expect");
            json_str("rule", "stacked PC on a MOVEM boundary; d0-d7/a2-a6 all from the buffer of the "
                     "last MOVEM before it");
            json_obj_end();
            json_obj("got");
            json_hex("spc_minus_start", bpc - (ULONG)ir_movem_start);
            json_arr("regs_d0_d7_a0_a6");
            for (j = 0; j < 15; j++)
                json_hex(NULL, ir_snapregs[j]);
            json_arr_end();
            json_int("first_bad_reg", mism);
            json_obj_end();
            json_obj("in");
            json_int("trial", bad);
            json_int("ta_latch", (long)blatch);
            json_hex("buf_a0", ba[0]);
            json_hex("buf_b0", bb[0]);
            json_str("code", "movem.l (a1); then 128 x { movem.l (a0),d0-d7/a2-a6; movem.l (a1),... } "
                     "(4 bytes each), loop");
            json_obj_end();
            json_str("why", "an interrupt arriving during MOVEM is taken after the instruction "
                     "completes: no partial register transfer");
            amt_case_close();
        }
    }
    env_end();
    amt_vec_set(26, old);
}

/* ------------------------------------------------------------ latency */
/* Upper bound for request -> handler entry: 2 raster lines (454 colour
 * clocks; 1 E clock = 5 cck, both derived from the same 7.09/7.16 MHz
 * clock on every model). Worst case on the slowest machine, a 68000 at
 * 7 MHz (1 cck = 2 CPU clocks), by MC68000UM instruction and exception
 * timings: the runtime's VERTB handler (rt/bare.s, level 3, VERTB stays
 * enabled) may have just started, ~590 clocks with its exception and RTE;
 * then the level 2/3 exception (44) and the handler up to its first
 * counter read (~32, plus up to ~20 of E-clock sync for the CIA read): about
 * 690 clocks = 345 cck = 1.5 lines. Without a VERTB in the way the latency
 * is the current instruction plus ~100 clocks, well under a line. Faster
 * CPUs only shorten it. A handler entered after 2 lines means the
 * interrupt was held back by something other than the CPU finishing its
 * work (interrupt sampling, IPL or INTREQ propagation delayed). */
#define LAT_MAX_CCK (2UL * 227UL)
static ULONG eb_read(void);

static void latency_cia(void)
{
    int n = AMT_QUICK() ? 8 : 32, i, got = 0, torn = 0, lost_first = -1, late, ambiguous = 0;
    ULONG hist[16], mn = 0xffff, mx = 0, sum = 0;
    amt_vec_fn old;
    const UWORD L = 4095;

    if (!need("latency_cia_ta_to_handler", PORTS))
        return;
    old = amt_vec_set(26, ir_lat2);

    for (i = 0; i < 16; i++)
        hist[i] = 0;
    env();
    ir_lat_p = (volatile UBYTE *)ir_lat_buf;
    ir_lat_n = 0;
    ir_lat_overflow = 0;
    cia_wr(CIAB_BASE, C_CRA, 0);
    cia_wr(CIAB_BASE, C_CRB, 0);
    cia_wr(CIAB_BASE, C_TALO, 0xff);
    cia_wr(CIAB_BASE, C_TAHI, 0xff);
    cia_wr(CIAB_BASE, C_TBLO, 0xff);
    cia_wr(CIAB_BASE, C_TBHI, 0xff);
    cia_wr(CIAB_BASE, C_CRB, CR_SPMODE | CR_START);
    cia_wr(CIAB_BASE, C_CRA, CR_START | CR_LOAD);
    cia_wr(CIAA_BASE, C_ICR, 0x7f);
    (void)cia_icr(CIAA_BASE);
    cia_wr(CIAA_BASE, C_ICR, 0x81);
    CREG(R_INTENA) = 0x8000 | PORTS;
    for (i = 0; i < n; i++) {
        ULONG t0;
        ULONG independent;
        UWORD n0 = ir_lat_n;
        cia_wait_cc(5 * (amt_rand() % 300));
        cia_wr(CIAA_BASE, C_CRA, 0);
        cia_wr(CIAA_BASE, C_TALO, (UBYTE)L);
        cia_wr(CIAA_BASE, C_TAHI, (UBYTE)(L >> 8));
        CREG(R_INTENA) = 0x8000 | PORTS;
        independent = eb_read();
        cia_wr(CIAA_BASE, C_CRA, CR_START | CR_LOAD);
        t0 = cia_now();
        while (ir_lat_n == n0 && cia_now() - t0 < MS(20))
            ;
        if (eb_read() - independent >= 2UL * (L + 1UL))
            ambiguous++;
        if (ir_lat_n == n0 && lost_first < 0)
            lost_first = i;
    }
    CREG(R_INTENA) = PORTS;
    cia_wr(CIAA_BASE, C_CRA, 0);
    cia_wr(CIAA_BASE, C_ICR, 0x7f);
    got = ir_lat_n;
    for (i = 0; i < got && i < 64; i++) {
        volatile UBYTE *s = (volatile UBYTE *)&ir_lat_buf[i];
        ULONG c, lat;
        if (s[0] != s[2]) {
            torn++;
            continue;
        }
        c = ((ULONG)s[0] << 8) | s[1];
        lat = L - c;
        if (lat < mn) mn = lat;
        if (lat > mx) mx = lat;
        sum += lat;
        hist[lat > 15 ? 15 : lat]++;
    }
    env_end();
    amt_vec_set(26, old);
    if (got == n && !hw_latency_observation_valid(got, n, torn, ambiguous, ir_lat_overflow) &&
        !ir_lat_overflow) {
        amt_case("latency_cia_ta_to_handler", AMT_ST_UNTESTABLE,
                 "torn timer samples or independent elapsed-time bound cannot exclude a full timer reload");
        return;
    }
    if (got != n && late_stuck("latency_cia_ta_to_handler"))
        return;
    late = got - torn > 0 && mx * 5 > LAT_MAX_CCK;
    amt_case_open("latency_cia_ta_to_handler",
                  hw_latency_observation_valid(got, n, torn, ambiguous, ir_lat_overflow) && !late
                  ? AMT_ST_PASS : AMT_ST_FAIL);
    if (got != n || late || ir_lat_overflow) {
        json_obj("expect");
        json_int("samples", n);
        json_int("max_cck", (long)LAT_MAX_CCK);
        json_obj_end();
        json_obj("got");
        json_int("samples", got);
        json_int("first_lost", lost_first);
        json_int("max_cck", (long)(mx * 5));
        json_obj_end();
        json_str("why", got != n
                 ? "CIA-A timer A interrupts (level 2) were lost: not every underflow reached "
                   "the handler within 20 ms"
                 : "a CIA-A timer A interrupt (level 2) reached the handler more than 2 raster "
                   "lines (454 cck = 90.8 E clocks) after the underflow; the worst case on a "
                   "7 MHz 68000 (a VERTB handler in the way, the exception, the handler's "
                   "prologue) is about 1.5 lines (HRM 7 interrupts, MC68000UM exception timing)");
    }
    json_obj("timing");
    json_str("unit", "E clocks from TA underflow to the handler's first counter read");
    json_int("n", got - torn);
    json_int("min", (long)mn);
    json_int("max", (long)mx);
    json_int("sum", (long)sum);
    json_arr("hist_0_15plus");
    for (i = 0; i < 16; i++)
        json_int(NULL, (long)hist[i]);
    json_arr_end();
    json_obj_end();
    amt_case_close();
}

static void *find_lib(const char *name)
{
    struct Node *n;
    for (n = SysBase->LibList.lh_Head; n && n->ln_Succ; n = n->ln_Succ)
        if (n->ln_Name && cia_streq(n->ln_Name, name))
            return n;
    return 0;
}

static void latency_coper(void)
{
    int n = AMT_QUICK() ? 4 : 16, i, got, late;
    volatile UWORD *cl = (volatile UWORD *)amt_chip_pool();
    ULONG mn = 0xffffffffUL, mx = 0, sum = 0, t0;
    amt_vec_fn old = amt_vec_set(27, ir_lat3);
    const UWORD V = 0x80;

    cl[0] = (UWORD)((V << 8) | 0x01);  /* WAIT line V, h 0 */
    cl[1] = 0xfffe;
    cl[2] = 0x009c;                    /* MOVE INTREQ */
    cl[3] = 0x8000 | COPER;
    cl[4] = 0xffff;                    /* end */
    cl[5] = 0xfffe;
    env();
    ir_lat_p = (volatile UBYTE *)ir_lat_buf;
    ir_lat_n = 0;
    ir_lat_overflow = 0;
    copper_used = 1;
    CREG(R_INTENA) = 0x8000 | COPER;
    wait_window();
    *(volatile ULONG *)0xdff080 = (ULONG)cl;
    CREG(0x088) = 0;                   /* COPJMP1 */
    CREG(R_DMACON) = 0x8280;           /* DMAEN | COPEN */
    t0 = cia_now();
    while (ir_lat_n < n && cia_now() - t0 < MS(20) * (ULONG)(n + 4))
        ;
    CREG(R_DMACON) = 0x0080;
    CREG(R_INTENA) = COPER;
    got = ir_lat_n;
    for (i = 0; i < got && i < 64; i++) {
        UWORD vh = (UWORD)(ir_lat_buf[i] >> 16);
        ULONG lat = (ULONG)((vh >> 8) - V) * 227UL + (vh & 0xff);
        if (lat < mn) mn = lat;
        if (lat > mx) mx = lat;
        sum += lat;
    }
    env_end();
    amt_vec_set(27, old);
    late = got > 0 && mx > LAT_MAX_CCK;
    amt_case_open("latency_coper_to_handler", got >= n && !late && !ir_lat_overflow ? AMT_ST_PASS : AMT_ST_FAIL);
    if (got < n || late || ir_lat_overflow) {
        json_obj("expect");
        json_int("samples", n);
        json_int("max_cck", (long)LAT_MAX_CCK);
        json_obj_end();
        json_obj("got");
        json_int("samples", got);
        json_int("overflow", ir_lat_overflow);
        json_int("max_cck", (long)mx);
        json_obj_end();
        json_obj("in");
        json_str("copper", "WAIT $8001,$FFFE; MOVE #$8010,INTREQ; END");
        json_obj_end();
        json_str("why", got < n
                 ? "a copper MOVE to INTREQ (COPER, level 3) once per frame did not reach the "
                   "handler every frame"
                 : "a copper COPER interrupt (level 3) reached the handler more than 2 raster "
                   "lines (454 cck) after the copper WAIT position (line $80, h 0), or before "
                   "it (a huge value); the copper MOVE follows the WAIT within a few cck and "
                   "the CPU side takes well under a line on a 7 MHz 68000 (HRM 2 copper, "
                   "7 interrupts, MC68000UM exception timing)");
    }
    json_obj("timing");
    json_str("unit", "colour clocks from the copper WAIT position (line $80, h 0) to VHPOSR read "
             "at handler entry");
    json_int("n", got);
    json_int("min", (long)mn);
    json_int("max", (long)mx);
    json_int("sum", (long)sum);
    json_obj_end();
    amt_case_close();
}

/* ------------------------------------------------------------ storm */
/* CIA-B TA ($FFFF, continuous) + TB counting TA underflows: 32-bit E clock */
static ULONG eb_read(void)
{
    UWORD b1, a, b2;
    for (;;) {
        b1 = cia_timer(CIAB_BASE, C_TBLO, C_TBHI);
        a = cia_timer(CIAB_BASE, C_TALO, C_TAHI);
        b2 = cia_timer(CIAB_BASE, C_TBLO, C_TBHI);
        if (b1 == b2 && a < 0xfff0 && a > 0x0010)
            break;
    }
    return ((ULONG)(0xffff - b1) << 16) | (ULONG)(0xffff - a);
}

static int storm_clock_rec(const struct ir_clock *clock)
{
    amt_case_open("storm_reference_clock", clock->invalid ? AMT_ST_FAIL : AMT_ST_PASS);
    if (clock->invalid) {
        json_obj("expect");
        json_str("clock", "nondecreasing coherent CIA-B E-clock samples");
        json_obj_end();
        json_obj("got");
        json_str("clock", "went backwards");
        json_obj_end();
        json_str("why", "CIA-B TA counts E clocks continuously and TB counts TA underflows; "
                 "neither timer is reloaded during the storm. A backward coherent sample "
                 "invalidates the elapsed-time reference, not the interrupt counts");
    }
    json_obj("timing");
    json_hexn("start_e", clock->start, 8);
    json_hexn("previous_e", clock->previous, 8);
    json_hexn("sample_e", clock->sample, 8);
    json_int("elapsed_e", (long)clock->elapsed);
    json_obj_end();
    amt_case_close();
    return !clock->invalid;
}

static int frame_lines(void)
{
    ULONG maxv = 0, v, previous = 0, before = 0, now, begin, start, budget = 1000000;
    int endpoint = 0;
    cia_wr(CIAB_BASE, C_CRA, 0);
    cia_wr(CIAB_BASE, C_CRB, 0);
    cia_wr(CIAB_BASE, C_TALO, 0xff);
    cia_wr(CIAB_BASE, C_TAHI, 0xff);
    cia_wr(CIAB_BASE, C_TBLO, 0xff);
    cia_wr(CIAB_BASE, C_TBHI, 0xff);
    cia_wr(CIAB_BASE, C_CRB, CR_SPMODE | CR_START);
    cia_wr(CIAB_BASE, C_CRA, CR_START | CR_LOAD);
    wait_window();
    start = eb_read();
    before = cia_timer(CIAB_BASE, C_TALO, C_TAHI);
    while (--budget) {
        begin = cia_timer(CIAB_BASE, C_TALO, C_TAHI);
        v = beam_v();
        now = cia_timer(CIAB_BASE, C_TALO, C_TAHI);
        if (v < previous && maxv > 200) {
            endpoint = hw_frame_endpoint(maxv, previous, v, (UWORD)(before - now));
            break;
        }
        if (v > maxv && v < 1000)
            maxv = v;
        previous = v;
        before = begin;
    }
    /* A complete 16-bit wrap must not alias the short per-pair bracket. */
    if (eb_read() - start >= 65536UL)
        endpoint = 0;
    cia_wr(CIAB_BASE, C_CRA, 0);
    cia_wr(CIAB_BASE, C_CRB, 0);
    return endpoint ? (int)maxv + 1 : 0;
}

static void storm_rec(const char *id, ULONG exp, ULONG got, ULONG tol, const char *why,
                      ULONG el, ULONG per, ULONG lines2, ULONG issued, ULONG tmo)
{
    long d = (long)got - (long)exp;
    int ok = d >= -(long)tol && d <= (long)tol;
    if (!ok && late_stuck(id))
        return;
    amt_case_open(id, ok ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!ok) {
        /* the counts depend on the elapsed time: only the bound and the
         * verdict are fingerprinted, the numbers are under timing */
        json_obj("expect");
        json_int("tolerance", (long)tol);
        json_str("count", "within tolerance of the expected count (timing.expected)");
        json_obj_end();
        json_obj("got");
        json_str("count", d < 0 ? "too few" : "too many");
        json_obj("timing");
        json_int("count", (long)got);
        json_int("diff", d);
        json_obj_end();
        json_obj_end();
        json_str("why", why);
    }
    json_obj("timing");
    json_int("expected", (long)exp);
    json_int("got", (long)got);
    json_int("elapsed_e", (long)el);
    json_int("cia_period_e", (long)per);
    json_int("lines_2frames", (long)lines2);
    json_int("soft_issued", (long)issued);
    json_int("soft_timeouts", (long)tmo);
    json_int("cia_entries", (long)ir_st_cia);
    json_int("l2_entries", (long)ir_st_l2);
    json_int("l2_without_ta", (long)ir_st_other);
    json_int("vertb", (long)ir_st_vb);
    json_int("soft", (long)ir_st_soft);
    json_obj_end();
    amt_case_close();
}

static void storm(void)
{
    const UWORD P = 141;                       /* 142 E = 200 us */
    ULONG secs = AMT_QUICK() ? 1 : 5;
    ULONG ehz = amt_probe_result.pal ? 709379UL : 715909UL;
    ULONG target = secs * ehz, el, issued = 0, tmo = 0;
    ULONG cia_exp, vb_exp, lines2, cc4;
    struct ir_clock clock;
    amt_vec_fn o25, o26, o27;

    if (!need("storm_cia_timer", PORTS)) {
        need("storm_reference_clock", PORTS);
        need("storm_softint", PORTS);
        need("storm_vertb", PORTS);
        return;
    }
    {
        ULONG a = (ULONG)frame_lines(), b = (ULONG)frame_lines();
        lines2 = a && b ? a + b : 0;
    }
    env();
    /* reference clock */
    cia_wr(CIAB_BASE, C_ICR, 0x7f);
    cia_wr(CIAB_BASE, C_CRA, 0);
    cia_wr(CIAB_BASE, C_CRB, 0);
    cia_wr(CIAB_BASE, C_TALO, 0xff);
    cia_wr(CIAB_BASE, C_TAHI, 0xff);
    cia_wr(CIAB_BASE, C_TBLO, 0xff);
    cia_wr(CIAB_BASE, C_TBHI, 0xff);
    cia_wr(CIAB_BASE, C_CRB, CR_SPMODE | CR_START);
    cia_wr(CIAB_BASE, C_CRA, CR_START);
    /* CIA-A timer A, continuous */
    cia_wr(CIAA_BASE, C_ICR, 0x7f);
    (void)cia_icr(CIAA_BASE);
    cia_wr(CIAA_BASE, C_CRA, 0);
    cia_wr(CIAA_BASE, C_TALO, (UBYTE)P);
    cia_wr(CIAA_BASE, C_TAHI, (UBYTE)(P >> 8));
    cia_wr(CIAA_BASE, C_ICR, 0x81);
    ir_st_soft = ir_st_cia = ir_st_l2 = ir_st_vb = ir_st_other = 0;
    o25 = amt_vec_set(25, ir_st1);
    o26 = amt_vec_set(26, ir_st2);
    o27 = amt_vec_set(27, ir_st3);
    CREG(R_INTENA) = 0x8000 | SOFT | PORTS;
    cia_wait_cc(1000);
    ir_clock_init(&clock, eb_read());
    cia_wr(CIAA_BASE, C_CRA, CR_START | CR_LOAD);
    for (;;) {
        ULONG s0, te = eb_read();
        if (!ir_clock_sample(&clock, te) || clock.elapsed >= target)
            break;
        s0 = ir_st_soft;
        CREG(R_INTREQ) = 0x8000 | SOFT;
        issued++;
        while (ir_st_soft == s0) {
            ULONG now = eb_read();
            if (!ir_clock_sample(&clock, now))
                break;
            if (now - te > 3000) {
                tmo++;
                break;
            }
        }
        if (clock.invalid)
            break;
        if ((issued & 1023) == 0)
            amt_watchdog_kick();
        ir_kbd();                       /* a key event seen by ir_st2 */
    }
    cia_wr(CIAA_BASE, C_CRA, 0);
    ir_clock_sample(&clock, eb_read());
    ir_settle();
    CREG(R_INTENA) = SOFT | PORTS;
    cia_wr(CIAA_BASE, C_ICR, 0x7f);
    (void)cia_icr(CIAA_BASE);
    cia_wr(CIAB_BASE, C_CRA, 0);
    cia_wr(CIAB_BASE, C_CRB, 0);
    amt_vec_set(25, o25);
    amt_vec_set(26, o26);
    amt_vec_set(27, o27);
    env_end();

    if (!storm_clock_rec(&clock)) {
        const char *why = "storm_reference_clock failed: elapsed time is invalid; "
            "interrupt delivery cannot be judged from this aborted storm";
        amt_case("storm_cia_timer", AMT_ST_UNTESTABLE, why);
        amt_case("storm_softint", AMT_ST_UNTESTABLE, why);
        amt_case("storm_vertb", AMT_ST_UNTESTABLE, why);
        return;
    }
    el = clock.elapsed;
    cia_exp = el / (P + 1);
    /* frame length in colour clocks x4: lines x 227 (PAL) or 227.5 (NTSC) */
    cc4 = lines2 * (amt_probe_result.pal ? 454UL : 455UL);
    vb_exp = cc4 ? (el * 20UL) / cc4 : 0;
    storm_rec("storm_cia_timer", cia_exp, ir_st_cia, 1,
              "CIA-A timer A interrupts counted by the level-2 handler differ by more than 1 from "
              "elapsed/period (elapsed from the CIA-B E-clock counter): interrupts lost or doubled",
              el, P + 1, lines2, issued, tmo);
    storm_rec("storm_softint", issued, ir_st_soft, 0,
              "every SOFTINT written to INTREQ (one at a time, each waited for) must be taken exactly "
              "once; a timeout means a lost request",
              el, P + 1, lines2, issued, tmo);
    if (!lines2)
        amt_case("storm_vertb", AMT_ST_UNTESTABLE,
                 "beam sampling did not cover the terminal line and wrap within a sub-line E-clock interval");
    else storm_rec("storm_vertb", vb_exp, ir_st_vb, 1,
              "VERTB interrupts differ by more than 1 from elapsed time / frame length (lines measured "
              "from the beam): lost or extra vertical-blank interrupts",
              el, P + 1, lines2, issued, tmo);
}

/* ------------------------------------------------------------ main */
static void body(void)
{
    map_tests();
    enable_tests();
    mask_tests();
    prio_tests();
    lower_tests();
    reg_tests();
    cia_irq_tests();
    stop_tests();
    chain_tests();
    latency_cia();
    latency_coper();
    storm();
}

/* The runtime's level-3 dispatcher (where VERTB is chained to) clears
 * INTREQR & INTENAR, i.e. pending requests of every level, when no
 * level-3 hook is set. With this no-op hook it leaves them alone. */
static void l3_noop(ULONG intreq) { (void)intreq; }

void amt_run_interrupts(void)
{
    int code, i;
    amt_irq_hook[3] = l3_noop;
    cia_save(&sav_a, CIAA_BASE, 0);
    cia_save(&sav_b, CIAB_BASE, 0);
    stuck = 0;
    if (cia_int_line_stuck(PORTS))
        stuck |= PORTS;
    if (cia_int_line_stuck(EXTER))
        stuck |= EXTER;
    ir_runaway = 0;
    ir_kbd_pend = 0;
    if (stuck)
        amt_log(stuck == (PORTS | EXTER) ? "interrupts: INT2 and INT6 held by another device"
                : stuck == PORTS ? "interrupts: INT2 (PORTS) held by another device"
                                 : "interrupts: INT6 (EXTER) held by another device");
    ir_fmt = AMT_CPU() >= AMT_CPU_68010;
    for (i = 1; i <= 6; i++)
        oldvec[i] = amt_vec_set(24 + i, myvec[i]);
    ir_old27 = (ULONG)oldvec[3];
    code = amt_try(body);
    if (ir_runaway)
        amt_log("interrupts: a source that would not clear was disabled by the handler guard");
    amt_set_sr(0x2700);
    CREG(R_INTENA) = 0x3fdf;
    CREG(R_DMACON) = 0x0080;
    for (i = 1; i <= 6; i++)
        amt_vec_set(24 + i, oldvec[i]);
    amt_irq_hook[3] = 0;
    CREG(R_INTREQ) = 0x3fdf;
    CREG(R_INTREQ) = 0x3fdf;
    CREG(R_INTENA) = 0xc020;
    amt_set_sr(0x2000);
    if (copper_used) {
        UBYTE *gfx = find_lib("graphics.library");
        ULONG ci = gfx ? *(ULONG *)(gfx + 0x26) : 0;   /* GfxBase->copinit */
        if (ci)
            *(volatile ULONG *)0xdff080 = ci;
    }
    ir_kbd();
    cia_restore(&sav_b);
    cia_restore(&sav_a);
    if (code)
        amt_abort(code);
}
