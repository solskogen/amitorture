/* cia: 8520 CIA-A and CIA-B timers, ICR, TOD, serial port (CONTRACT.md
 * section 10, module id 7).
 *
 * Expectations: Amiga Hardware Reference Manual, appendix F (8520), and
 * the MOS/CSG 8520 data sheet. Differences from the 6526 that matter here:
 * the TOD is a 24-bit binary counter (no BCD, no AM/PM, no 50/60 Hz
 * prescaler: it counts pulses on its TOD pin), and CRA bit 7 is unused.
 * CIA-A TOD counts the 50/60 Hz tick (VSYNC on most Amigas), CIA-B TOD
 * counts HSYNC. The E clock is CPU clock / 10 (709379 Hz PAL); one E clock
 * is exactly 5 colour clocks.
 *
 * Safety: PBON is never set (CIA-A PB is the parallel port, CIA-B PB the
 * floppy control lines), CIA-B SPMODE/CNT are never used (parallel BUSY/
 * POUT), PRA/PRB/DDR are never written. CIA-A SPMODE is only set for the
 * short serial-port output test, with $FF in SDR. Everything else is saved
 * and restored by cia_hw.h. */
#include "mod.h"
#include "cia_hw.h"

AMT_MODULE(cia, 7, AMT_PH_BARE, 0, 0, 0);

#define MS(n)   ((ULONG)(n) * 3547UL)    /* colour clocks */
#define E2CC(n) ((ULONG)(n) * 5UL)

#define INTB_PORTS 0x0008
#define INTB_EXTER 0x2000

struct tm {
    volatile UBYTE *c;
    UWORD lo, hi, cr;
    UBYTE bit;      /* ICR bit */
    UBYTE is_b;     /* timer B */
    const char *pfx;
};

static const struct tm tms[4] = {
    { CIAA_BASE, C_TALO, C_TAHI, C_CRA, ICR_TA, 0, "ciaa_ta" },
    { CIAA_BASE, C_TBLO, C_TBHI, C_CRB, ICR_TB, 1, "ciaa_tb" },
    { CIAB_BASE, C_TALO, C_TAHI, C_CRA, ICR_TA, 0, "ciab_ta" },
    { CIAB_BASE, C_TBLO, C_TBHI, C_CRB, ICR_TB, 1, "ciab_tb" },
};

static struct cia_saved sav_a, sav_b;
static UWORD stuck;     /* PORTS/EXTER held by another device */

/* ------------------------------------------------------------ helpers */
static char idb[64];
static const char *cid(const char *p, const char *s)
{
    char *d = idb;
    while (*p)
        *d++ = *p++;
    *d++ = '_';
    while (*s && d < idb + sizeof(idb) - 1)
        *d++ = *s++;
    *d = 0;
    return idb;
}

static void tstop(const struct tm *t) { cia_wr(t->c, t->cr, 0); }
/* with the timer stopped in continuous mode the high-byte write also
 * loads the counter */
static void tload(const struct tm *t, UWORD v)
{
    cia_wr(t->c, t->lo, (UBYTE)v);
    cia_wr(t->c, t->hi, (UBYTE)(v >> 8));
}
static UWORD tread(const struct tm *t) { return cia_timer(t->c, t->lo, t->hi); }

static int twait_stop(const struct tm *t, ULONG to)
{
    ULONG t0 = cia_now();
    while (cia_rd(t->c, t->cr) & CR_START)
        if (cia_now() - t0 > to)
            return 0;
    return 1;
}

/* poll ICR until one of `bits` is seen (or timeout); returns all bits seen */
static UBYTE icr_wait(volatile UBYTE *c, UBYTE bits, ULONG to)
{
    UBYTE acc = 0;
    ULONG t0 = cia_now();
    do {
        acc |= cia_icr(c);
        if (acc & bits)
            break;
    } while (cia_now() - t0 < to);
    return acc;
}

static void wait_e(ULONG n) { cia_wait_cc(E2CC(n)); }

/* The CIA asserts /IRQ about one E clock after the ICR flag (8520), and
 * Paula needs a moment more to latch INT2/INT6 into INTREQ; no latency is
 * specified (HRM appendix F). Poll INTREQR for up to IRQ_WAIT_E E clocks
 * before deciding, so the verdict does not depend on how fast the CPU gets
 * from the timer read to the INTREQR read. */
#define IRQ_WAIT_E 20
static UWORD intreq_wait(UWORD ib)
{
    UWORD r;
    ULONG t0 = cia_now();
    do {
        r = CREG(R_INTREQR) & ib;
        if (r)
            break;
    } while (cia_now() - t0 < E2CC(IRQ_WAIT_E));
    return r;
}

/* a failed PORTS/EXTER comparison: untestable if another device started
 * holding the line after the module-start check (cia_hw.h) */
static int late_stuck(const char *id, UWORD ib)
{
    if ((stuck & ib) || !cia_int_line_recheck(ib))
        return 0;
    stuck |= ib;
    amt_case(id, AMT_ST_UNTESTABLE, ib == INTB_EXTER ? CIA_STUCK_LATE_WHY_EXTER : CIA_STUCK_LATE_WHY_PORTS);
    return 1;
}

/* one-shot with force load: exactly one underflow after latch+1 E clocks */
static void fire(const struct tm *t, UWORD latch)
{
    tstop(t);
    tload(t, latch);
    cia_wr(t->c, t->cr, CR_RUNMODE | CR_LOAD | CR_START);
    twait_stop(t, MS(5));
}

static void rep(const char *id, int ok, const char *why, const char *k, int dg,
                ULONG e, ULONG g, const char *ik, ULONG iv)
{
    if (ok) {
        amt_case(id, AMT_ST_PASS, 0);
        return;
    }
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_hexn(k, e, dg);
    json_obj_end();
    json_obj("got");
    json_hexn(k, g, dg);
    json_obj_end();
    json_str("why", why);
    if (ik) {
        json_obj("in");
        json_hexn(ik, iv, 4);
        json_obj_end();
    }
    amt_case_close();
}

/* ------------------------------------------------------------ timers */
static void t_load(const struct tm *t)
{
    static const UWORD fixed[] = { 0x0000, 0x0001, 0x00ff, 0x0100, 0x8000, 0xffff };
    UWORD vals[10];
    int n = 0, i, nr = AMT_QUICK() ? 1 : 4, bad = -1;
    UWORD got = 0;
    for (i = 0; i < 6; i++)
        vals[n++] = fixed[i];
    for (i = 0; i < nr; i++)
        vals[n++] = (UWORD)amt_rand();
    tstop(t);
    for (i = 0; i < n; i++) {
        UWORD g;
        tload(t, vals[i]);
        g = tread(t);
        if (g != vals[i] && bad < 0) {
            bad = i;
            got = g;
        }
    }
    rep(cid(t->pfx, "stopped_hi_write_loads"), bad < 0,
        "timer stopped (CR=$00): writing latch low then high must load the counter "
        "(8520: the latch goes to the counter when the high byte is written while stopped)",
        "counter", 4, bad < 0 ? 0 : vals[bad], got, "latch", bad < 0 ? 0 : vals[bad]);
}

static void t_lo_only(const struct tm *t)
{
    UWORD g1, g2;
    /* The 8520 transfers the latch to the counter in the E cycles after
     * the high-byte write; Amiberry's cycle-accurate CIA (cia.cpp
     * CIA_thi_write, loaddelay bits 1 and 2) loads it again one E clock
     * later, so a low-byte write that a fast CPU issues within 2 E clocks
     * changes the value loaded. The data sheet doesn't specify that
     * latency, so let the load finish before each next access. */
    tstop(t);
    tload(t, 0x1234);
    wait_e(4);
    cia_wr(t->c, t->lo, 0x56);
    wait_e(4);
    g1 = tread(t);
    cia_wr(t->c, t->hi, 0x9a);
    wait_e(4);
    g2 = tread(t);
    if (g1 == 0x1234 && g2 == 0x9a56) {
        amt_case(cid(t->pfx, "lo_write_no_load"), AMT_ST_PASS, 0);
        return;
    }
    amt_case_open(cid(t->pfx, "lo_write_no_load"), AMT_ST_FAIL);
    json_obj("expect");
    json_hexn("after_lo", 0x1234, 4);
    json_hexn("after_hi", 0x9a56, 4);
    json_obj_end();
    json_obj("got");
    json_hexn("after_lo", g1, 4);
    json_hexn("after_hi", g2, 4);
    json_obj_end();
    json_str("why", "stopped timer loaded with $1234; a low-byte write ($56) must only change the "
             "latch, and the following high-byte write ($9A) loads latch $9A56 into the counter");
    amt_case_close();
}

static void t_oneshot(const struct tm *t)
{
    UBYTE cr1, crv, acc, acc2;
    UWORD c1, c2, cnt;
    int started;

    tstop(t);
    (void)cia_icr(t->c);
    cia_wr(t->c, t->cr, CR_RUNMODE);          /* one-shot, START=0 */
    cia_wr(t->c, t->lo, 0x00);
    cia_wr(t->c, t->hi, 0x20);                /* 8520: loads and starts */
    cr1 = cia_rd(t->c, t->cr);
    c1 = tread(t);
    wait_e(300);
    c2 = tread(t);
    started = (cr1 & CR_START) && c2 < c1 && c1 <= 0x2000;
    if (started)
        amt_case(cid(t->pfx, "oneshot_hi_write_starts"), AMT_ST_PASS, 0);
    else {
        amt_case_open(cid(t->pfx, "oneshot_hi_write_starts"), AMT_ST_FAIL);
        json_obj("expect");
        json_str("cr_start", "1");
        json_str("counter", "decreasing from <= $2000");
        json_obj_end();
        json_obj("got");
        json_hexn("cr", cr1, 2);
        json_hexn("c1", c1, 4);
        json_hexn("c2_after_300e", c2, 4);
        json_obj_end();
        json_str("why", "8520 data sheet / HRM: in one-shot mode a write to the timer high byte "
                 "transfers the latch to the counter and starts the timer regardless of START");
        json_obj("in");
        json_hexn("cr_written", CR_RUNMODE, 2);
        json_hexn("latch", 0x2000, 4);
        json_obj_end();
        amt_case_close();
        cia_wr(t->c, t->cr, CR_RUNMODE | CR_LOAD | CR_START);
    }
    acc = icr_wait(t->c, t->bit, MS(30));
    rep(cid(t->pfx, "oneshot_underflow_icr"), (acc & t->bit) != 0,
        "one-shot timer with latch $2000 (8193 E clocks, 11.5 ms) did not set its ICR flag within 30 ms",
        "icr_bits", 2, t->bit, acc, 0, 0);
    twait_stop(t, MS(5));
    crv = cia_rd(t->c, t->cr);
    cnt = tread(t);
    if (!(crv & CR_START) && cnt == 0x2000)
        amt_case(cid(t->pfx, "oneshot_stops_reloads"), AMT_ST_PASS, 0);
    else {
        amt_case_open(cid(t->pfx, "oneshot_stops_reloads"), AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("cr_start", 0, 1);
        json_hexn("counter", 0x2000, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("cr", crv, 2);
        json_hexn("counter", cnt, 4);
        json_obj_end();
        json_str("why", "after a one-shot underflow the 8520 clears START and reloads the latch "
                 "into the counter (the latch is reloaded on every underflow)");
        amt_case_close();
    }
    wait_e(9000);
    acc2 = cia_icr(t->c);
    rep(cid(t->pfx, "oneshot_single_event"), !(acc2 & t->bit),
        "a one-shot timer underflowed again after it stopped (second ICR flag 9000 E clocks later)",
        "icr", 2, 0, acc2 & t->bit, 0, 0);
    tstop(t);
}

static void t_continuous(const struct tm *t)
{
    int n = 0;
    ULONG t0;
    UBYTE crv;
    UWORD cnt;
    tstop(t);
    tload(t, 0x0800);
    (void)cia_icr(t->c);
    cia_wr(t->c, t->cr, CR_START);
    t0 = cia_now();
    while (n < 3 && cia_now() - t0 < MS(40))
        if (cia_icr(t->c) & t->bit)
            n++;
    crv = cia_rd(t->c, t->cr);
    cnt = tread(t);
    tstop(t);
    if (n >= 3 && (crv & CR_START) && cnt <= 0x0800)
        amt_case(cid(t->pfx, "continuous_repeats"), AMT_ST_PASS, 0);
    else {
        amt_case_open(cid(t->pfx, "continuous_repeats"), AMT_ST_FAIL);
        json_obj("expect");
        json_int("events", 3);
        json_hexn("cr_start", 1, 1);
        json_str("counter", "<= $0800");
        json_obj_end();
        json_obj("got");
        json_int("events", n);
        json_hexn("cr", crv, 2);
        json_hexn("counter", cnt, 4);
        json_obj_end();
        json_str("why", "continuous mode (RUNMODE=0) with latch $0800: three underflows expected "
                 "within 40 ms (2049 E clocks each), START stays set, counter reloads from the latch");
        amt_case_close();
    }
}

static void t_stop_hold(const struct tm *t)
{
    UWORD c1, c2, c3;
    tstop(t);
    tload(t, 0xffff);
    cia_wr(t->c, t->cr, CR_START);
    wait_e(500);
    cia_wr(t->c, t->cr, 0);
    c1 = tread(t);
    wait_e(1000);
    c2 = tread(t);
    if (c1 == c2 && c1 < 0xffff)
        amt_case(cid(t->pfx, "start_stop_holds"), AMT_ST_PASS, 0);
    else {
        amt_case_open(cid(t->pfx, "start_stop_holds"), AMT_ST_FAIL);
        json_obj("expect");
        json_str("c1", "< $ffff (it ran 500 E)");
        json_str("c2", "== c1");
        json_obj_end();
        json_obj("got");
        json_hexn("c1", c1, 4);
        json_hexn("c2", c2, 4);
        json_obj_end();
        json_str("why", "clearing START must stop the counter and hold its value");
        amt_case_close();
    }
    cia_wr(t->c, t->cr, CR_START);
    wait_e(300);
    c3 = tread(t);
    tstop(t);
    rep(cid(t->pfx, "restart_resumes"), c3 < c2 && c2 - c3 < 2000,
        "setting START again must resume from the held count (no reload): expect a value a "
        "little below the held count after 300 E clocks",
        "counter", 4, c2, c3, "held", c2);
}

static void t_force_load(const struct tm *t)
{
    UWORD cnt0, cnt1, cb, ca;
    UBYTE crv;
    /* stopped, latch $ffff, counter below it (t_stop_hold left it so) */
    tstop(t);
    tload(t, 0xffff);
    cia_wr(t->c, t->cr, CR_START);
    wait_e(700);
    tstop(t);
    cnt0 = tread(t);
    cia_wr(t->c, t->cr, CR_LOAD);
    crv = cia_rd(t->c, t->cr);
    cnt1 = tread(t);
    if (cnt1 == 0xffff && cnt0 != 0xffff)
        amt_case(cid(t->pfx, "force_load_stopped"), AMT_ST_PASS, 0);
    else {
        amt_case_open(cid(t->pfx, "force_load_stopped"), AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("counter", 0xffff, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("before", cnt0, 4);
        json_hexn("counter", cnt1, 4);
        json_obj_end();
        json_str("why", "writing CR with LOAD (bit 4) set must force-load the latch ($FFFF) into "
                 "the stopped counter");
        amt_case_close();
    }
    rep(cid(t->pfx, "load_strobe_reads_0"), (crv & (CR_LOAD | CR_START)) == 0,
        "LOAD is a strobe: it always reads back 0 (and LOAD alone must not start the timer)",
        "cr", 2, 0, crv, "written", CR_LOAD);
    cia_wr(t->c, t->cr, CR_START);
    wait_e(2000);
    cb = tread(t);
    cia_wr(t->c, t->cr, CR_START | CR_LOAD);
    ca = tread(t);
    tstop(t);
    rep(cid(t->pfx, "force_load_running"), ca > cb && ca > 0xffff - 500,
        "force load while running must reload the counter from the latch ($FFFF) and keep counting",
        "counter", 4, 0xffff, ca, "before", cb);
}

static void t_hi_running(const struct tm *t)
{
    UWORD c0, c1, c2;
    UBYTE acc;
    ULONG t0, polls = 0, el;
    tstop(t);
    tload(t, 0x8000);
    (void)cia_icr(t->c);
    cia_wr(t->c, t->cr, CR_START);
    t0 = cia_now();
    while ((c0 = tread(t)) >= 0x7000 && cia_now() - t0 < MS(30))
        polls++;
    el = cia_now() - t0;
    cia_wr(t->c, t->lo, 0xff);
    cia_wr(t->c, t->hi, 0xff);
    c1 = tread(t);
    if (c1 < 0x7000)
        amt_case(cid(t->pfx, "running_hi_write_no_reload"), AMT_ST_PASS, 0);
    else {
        amt_case_open(cid(t->pfx, "running_hi_write_no_reload"), AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("counter_below", 0x7000, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("counter", c1, 4);
        json_obj_end();
        json_obj("in");
        json_hexn("latch_start", 0x8000, 4);
        json_hexn("poll_last", c0, 4);   /* the read that ended the poll */
        json_int("polls", (long)polls);
        json_int("poll_cck", (long)el);  /* beam time of the poll (timeout 30 ms) */
        json_obj_end();
        json_str("why", "continuous timer running below $7000: writing latch $FFFF must not reload the counter "
                 "(only underflow, force load, or a high-byte write while stopped do)");
        amt_case_close();
    }
    acc = icr_wait(t->c, t->bit, MS(80));
    c2 = tread(t);
    tstop(t);
    if ((acc & t->bit) && c2 > 0x8000)
        amt_case(cid(t->pfx, "new_latch_on_underflow"), AMT_ST_PASS, 0);
    else {
        amt_case_open(cid(t->pfx, "new_latch_on_underflow"), AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("icr_flag", 1, 1);
        json_str("counter", "> $8000 (reloaded from new latch $FFFF)");
        json_obj_end();
        json_obj("got");
        json_hexn("icr", acc, 2);
        json_hexn("counter", c2, 4);
        json_obj_end();
        json_str("why", "the latch written while running must be loaded at the next underflow");
        amt_case_close();
    }
}

static void t_inmode_cnt(const struct tm *t)
{
    UWORD g;
    tstop(t);
    tload(t, 0x1000);
    cia_wr(t->c, t->cr, CR_START | CR_INMODE);
    wait_e(1000);
    g = tread(t);
    tstop(t);
    rep(cid(t->pfx, "inmode_cnt_no_e_clock"), g == 0x1000,
        "timer A with INMODE=1 counts CNT edges only; with no CNT activity it must not count E clocks "
        "(CIA-A CNT is the keyboard clock, CIA-B CNT the parallel POUT line)",
        "counter", 4, 0x1000, g, 0, 0);
}

static void t_cr_readback(const struct tm *t)
{
    static const UBYTE pa[] = { 0x00, 0x04, 0x08, 0x0c, 0x20, 0x24, 0x28, 0x2c };
    static const UBYTE pb[] = { 0x00, 0x04, 0x08, 0x0c, 0x20, 0x40, 0x60, 0x48, 0x6c,
                                0x80, 0x88, 0xa4, 0xe8 };
    const UBYTE *p = t->is_b ? pb : pa;
    int n = t->is_b ? (int)sizeof(pb) : (int)sizeof(pa), i, bad = -1;
    UBYTE mask = t->is_b ? 0xff : 0x7f;   /* CRA bit 7 unused on the 8520 */
    UBYTE g = 0, w = 0;
    tstop(t);
    for (i = 0; i < n && bad < 0; i++) {
        UBYTE r;
        cia_wr(t->c, t->cr, p[i]);
        r = cia_rd(t->c, t->cr) & mask;
        if (r != p[i]) {
            bad = i; g = r; w = p[i];
            break;
        }
        cia_wr(t->c, t->cr, p[i] | CR_LOAD);
        r = cia_rd(t->c, t->cr) & mask;
        if (r != p[i]) {
            bad = i; g = r; w = p[i] | CR_LOAD;
        }
    }
    tstop(t);
    if (bad < 0)
        amt_case(cid(t->pfx, "cr_readback"), AMT_ST_PASS, 0);
    else {
        amt_case_open(cid(t->pfx, "cr_readback"), AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("cr", w & (UBYTE)~CR_LOAD, 2);
        json_obj_end();
        json_obj("got");
        json_hexn("cr", g, 2);
        json_obj_end();
        json_obj("in");
        json_hexn("written", w, 2);
        json_hexn("compare_mask", mask, 2);
        json_obj_end();
        json_str("why", "control register bits read back as written, except LOAD (strobe, reads 0); "
                 "START=0 and PBON/SPMODE(CRA) never set in this test");
        amt_case_close();
    }
}

static void timer_tests(const struct tm *t)
{
    t_load(t);
    t_lo_only(t);
    t_oneshot(t);
    t_continuous(t);
    t_stop_hold(t);
    t_force_load(t);
    t_hi_running(t);
    if (!t->is_b)
        t_inmode_cnt(t);
    t_cr_readback(t);
}

/* timer B in INMODE=10: counts timer A underflows */
static void cascade_tests(int b)
{
    const struct tm *ta = &tms[b * 2], *tb = &tms[b * 2 + 1];
    const char *pfx = b ? "ciab" : "ciaa";
    UWORD g0, g1;
    ULONG k, i;
    int nmarks = AMT_QUICK() ? 4 : 8, ntb = 0, ok, lb = AMT_QUICK() ? 3 : 7;
    ULONG marks[10], ta_n = 0, t0;

    tstop(ta);
    tstop(tb);
    tload(tb, 0x1000);
    cia_wr(tb->c, tb->cr, CR_SPMODE | CR_START);    /* INMODE=10: TA underflows */
    wait_e(500);
    g0 = tread(tb);
    tload(ta, 15);
    k = 5 + amt_rand() % (AMT_QUICK() ? 8 : 36);
    for (i = 0; i < k; i++) {
        cia_wr(ta->c, ta->cr, CR_RUNMODE | CR_LOAD | CR_START);
        twait_stop(ta, MS(2));
    }
    /* The data sheet gives no delay between a TA underflow and TB counting
     * it, and TA's START clears at the underflow itself. On real hardware
     * every CIA access is synchronised to its own E cycle, so the TB read
     * comes at least one E clock after the CRA read that saw START=0; a
     * non-cycle-exact emulated CPU can do both within one E clock (Amiberry
     * passes the count through a 2-E-clock input pipe, cia.cpp
     * process_pipe). Wait a few E clocks so the last underflow has been
     * counted whatever the CPU speed. */
    wait_e(8);
    g1 = tread(tb);
    tstop(ta);
    tstop(tb);
    if (g0 == 0x1000 && g1 == 0x1000 - k)
        amt_case(cid(pfx, "tb_counts_ta_underflows"), AMT_ST_PASS, 0);
    else {
        amt_case_open(cid(pfx, "tb_counts_ta_underflows"), AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("tb_idle", 0x1000, 4);
        json_hexn("tb_after", 0x1000 - k, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("tb_idle", g0, 4);
        json_hexn("tb_after", g1, 4);
        json_obj_end();
        json_obj("in");
        json_int("ta_underflows", (long)k);
        json_hexn("tb_latch", 0x1000, 4);
        json_hexn("crb", CR_SPMODE | CR_START, 2);
        json_obj_end();
        json_str("why", "CRB INMODE=10: timer B decrements once per timer A underflow and never on "
                 "E clocks; K one-shot runs of timer A must leave TB = latch - K");
        amt_case_close();
    }

    /* exact ratio in continuous mode */
    tload(ta, 1023);
    tload(tb, (UWORD)lb);
    (void)cia_icr(ta->c);
    cia_wr(tb->c, tb->cr, CR_SPMODE | CR_START);
    cia_wr(ta->c, ta->cr, CR_START);
    t0 = cia_now();
    while (ntb < nmarks + 1 && cia_now() - t0 < MS(400)) {
        UBYTE v = cia_icr(ta->c);
        if (v & ICR_TA)
            ta_n++;
        if (v & ICR_TB)
            marks[ntb++] = ta_n;
    }
    tstop(ta);
    tstop(tb);
    ok = ntb == nmarks + 1;
    for (i = 1; ok && i < (ULONG)ntb; i++)
        if (marks[i] - marks[i - 1] != (ULONG)lb + 1)
            ok = 0;
    if (ok)
        amt_case(cid(pfx, "tb_ta_ratio_exact"), AMT_ST_PASS, 0);
    else {
        amt_case_open(cid(pfx, "tb_ta_ratio_exact"), AMT_ST_FAIL);
        json_obj("expect");
        json_int("tb_events", nmarks + 1);
        json_int("ta_per_tb", lb + 1);
        json_obj_end();
        json_obj("got");
        json_int("tb_events", ntb);
        json_arr("ta_count_at_tb");
        for (i = 0; i < (ULONG)ntb; i++)
            json_int(NULL, (long)marks[i]);
        json_arr_end();
        json_obj_end();
        json_obj("in");
        json_int("ta_latch", 1023);
        json_int("tb_latch", lb);
        json_obj_end();
        json_str("why", "TA continuous, TB continuous counting TA underflows: every TB underflow "
                 "must come exactly latch_b+1 TA underflows after the previous one");
        amt_case_close();
    }
}

/* ------------------------------------------------------------ ICR */
static void icr_tests(int b)
{
    const struct tm *ta = &tms[b * 2], *tb = &tms[b * 2 + 1];
    volatile UBYTE *c = ta->c;
    const char *pfx = b ? "ciab" : "ciaa";
    UWORD ib = b ? INTB_EXTER : INTB_PORTS;
    UBYTE v1, v2, v3, v;
    UWORD r1, r2, r3;

    cia_wr(c, C_ICR, 0x7f);
    (void)cia_icr(c);
    fire(ta, 0x40);
    v1 = cia_icr(c);
    v2 = cia_icr(c);
    if ((v1 & 0x87) == 0x01 && (v2 & 0x87) == 0)
        amt_case(cid(pfx, "icr_read_clears"), AMT_ST_PASS, 0);
    else {
        amt_case_open(cid(pfx, "icr_read_clears"), AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("first", 0x01, 2);
        json_hexn("second", 0x00, 2);
        json_obj_end();
        json_obj("got");
        json_hexn("first", v1, 2);
        json_hexn("second", v2, 2);
        json_obj_end();
        json_str("why", "mask clear, one TA underflow: the first ICR read returns the TA flag with IR=0, "
                 "and the read clears it (compared bits: $87)");
        amt_case_close();
    }

    /* IR bit and the interrupt line to Paula */
    cia_wr(c, C_ICR, 0x81);
    CREG(R_INTREQ) = ib;
    CREG(R_INTREQ) = ib;
    fire(ta, 0x40);
    r1 = intreq_wait(ib);
    v = cia_icr(c);
    /* /IRQ is released by the read; give the release the same margin
     * before clearing INTREQ, so only a line that stays asserted can set
     * the bit again */
    wait_e(IRQ_WAIT_E);
    CREG(R_INTREQ) = ib;
    CREG(R_INTREQ) = ib;
    r2 = CREG(R_INTREQR) & ib;
    rep(cid(pfx, "icr_ir_bit_set"), (v & 0x87) == 0x81,
        "TA enabled in the mask ($81): after the underflow ICR reads $81 (IR bit 7 + TA)",
        "icr", 2, 0x81, v, 0, 0);
    if (stuck & ib) {
        const char *w = b ? CIA_STUCK_WHY_EXTER : CIA_STUCK_WHY_PORTS;
        amt_case(cid(pfx, "icr_irq_to_paula"), AMT_ST_UNTESTABLE, w);
        amt_case(cid(pfx, "icr_read_releases_irq"), AMT_ST_UNTESTABLE, w);
        amt_case(cid(pfx, "icr_intreq_clear_while_asserted"), AMT_ST_UNTESTABLE, w);
        goto setclr;
    }
    rep(cid(pfx, "icr_irq_to_paula"), r1 != 0,
        b ? "an enabled CIA-B interrupt must set INTREQ EXTER (bit 13, level 6) within 20 E clocks "
            "of the underflow"
          : "an enabled CIA-A interrupt must set INTREQ PORTS (bit 3, level 2) within 20 E clocks "
            "of the underflow",
        "intreqr_bit", 4, ib, r1, 0, 0);
    if (r2 == 0 || !late_stuck(cid(pfx, "icr_read_releases_irq"), ib))
        rep(cid(pfx, "icr_read_releases_irq"), r2 == 0,
            "after the ICR read (and 20 E clocks) the CIA has released /IRQ, so clearing the "
            "INTREQ bit must stick",
            "intreqr_bit", 4, 0, r2, 0, 0);
    /* clearing INTREQ while the CIA still asserts: either is allowed. The
     * line is first seen in INTREQ (bounded wait), then cleared, then read
     * after a few E clocks, so the value does not depend on CPU speed; it
     * stays an undef_ field all the same (not fingerprinted). */
    fire(ta, 0x40);
    (void)intreq_wait(ib);
    CREG(R_INTREQ) = ib;
    CREG(R_INTREQ) = ib;
    wait_e(4);
    r3 = CREG(R_INTREQR) & ib;
    (void)cia_icr(c);
    wait_e(IRQ_WAIT_E);
    CREG(R_INTREQ) = ib;
    CREG(R_INTREQ) = ib;
    amt_case_open(cid(pfx, "icr_intreq_clear_while_asserted"), AMT_ST_PASS);
    json_obj("ctx");
    json_int("undef_intreq_relatched", r3 != 0);
    json_str("note", "HRM does not say whether Paula's INT2/INT6 inputs re-set INTREQ while the line "
             "stays asserted; both results accepted");
    json_obj_end();
    amt_case_close();

setclr:
    /* mask set/clear with bit 7 */
    cia_wr(c, C_ICR, 0x7f);
    cia_wr(c, C_ICR, 0x81);
    cia_wr(c, C_ICR, 0x82);
    cia_wr(c, C_ICR, 0x01);       /* mask = TB */
    (void)cia_icr(c);
    fire(ta, 0x40);
    v1 = cia_icr(c);
    fire(tb, 0x40);
    v2 = cia_icr(c);
    cia_wr(c, C_ICR, 0x7f);
    fire(tb, 0x40);
    v3 = cia_icr(c);
    if ((v1 & 0x87) == 0x01 && (v2 & 0x87) == 0x82 && (v3 & 0x87) == 0x02)
        amt_case(cid(pfx, "icr_mask_setclr"), AMT_ST_PASS, 0);
    else {
        amt_case_open(cid(pfx, "icr_mask_setclr"), AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("ta_event", 0x01, 2);
        json_hexn("tb_event", 0x82, 2);
        json_hexn("tb_after_7f", 0x02, 2);
        json_obj_end();
        json_obj("got");
        json_hexn("ta_event", v1, 2);
        json_hexn("tb_event", v2, 2);
        json_hexn("tb_after_7f", v3, 2);
        json_obj_end();
        json_obj("in");
        json_str("writes", "$7F,$81,$82,$01 (mask=TB), then $7F");
        json_obj_end();
        json_str("why", "ICR write: bit 7=1 sets the mask bits given, bit 7=0 clears them, other mask bits "
                 "unchanged; IR (bit 7 of the data) only for enabled sources");
        amt_case_close();
    }

    /* two flags at once */
    cia_wr(c, C_ICR, 0x83);
    (void)cia_icr(c);
    fire(ta, 0x40);
    fire(tb, 0x40);
    v = cia_icr(c);
    cia_wr(c, C_ICR, 0x7f);
    rep(cid(pfx, "icr_two_flags"), (v & 0x87) == 0x83,
        "TA and TB both underflowed with both enabled: one ICR read returns $83",
        "icr", 2, 0x83, v, 0, 0);

    /* enabling the mask after the event: either is allowed */
    (void)cia_icr(c);
    CREG(R_INTREQ) = ib;
    fire(ta, 0x40);
    cia_wr(c, C_ICR, 0x81);
    r1 = intreq_wait(ib);
    v = cia_icr(c);
    cia_wr(c, C_ICR, 0x7f);
    wait_e(IRQ_WAIT_E);
    CREG(R_INTREQ) = ib;
    CREG(R_INTREQ) = ib;
    if (v & 0x01) {
        amt_case_open(cid(pfx, "icr_mask_after_event"), AMT_ST_PASS);
        json_obj("ctx");
        json_hexn("undef_icr", v, 2);
        json_int("undef_intreq", r1 != 0);
        json_str("note", "data sheet does not define IR when the mask is set after the flag; "
                 "IR=0 or 1 accepted, the TA flag itself must be there");
        json_obj_end();
        amt_case_close();
    } else
        rep(cid(pfx, "icr_mask_after_event"), 0,
            "the TA flag must stay in ICR until read, also when the mask is set afterwards",
            "icr_ta", 2, 1, v, 0, 0);
}

/* ------------------------------------------------------------ TOD */
/* Tick stamps: raster lines (CIA-B, HSYNC) or frames (CIA-A, 50/60 Hz).
 * Every tolerance is derived from stamps taken around the TOD accesses,
 * so a slow CPU (several lines per poll on a 68000) widens the window
 * instead of failing. */

/* (frame, line) stamp. The frame number is the VERTB count, plus one when
 * VERTB is pending early in a frame (not serviced yet). That alone is not
 * enough: the beam counter wraps to line 0 a few colour clocks before
 * INTREQ VERTB becomes visible (seen in Amiberry: V=0 H=2 with VERTB still
 * clear; the HRM gives no exact position), and a fast CPU can sample in
 * between, which made the stamp drop by a whole frame. So the stamp also
 * follows the beam: it never goes back, and a big drop of V within the
 * same frame number means the beam wrapped before VERTB showed up. Stamps
 * are taken often enough (polling loops, or a few CIA accesses apart) that
 * consecutive ones are less than a frame apart. */
static ULONG nlines;
static ULONG st_fe, st_v;
static int st_init;

static void beam_stamp(ULONG *fe, ULONG *line)
{
    ULONG f1, f2, v, f;
    UWORD rq;
    do {
        f1 = amt_frames_now();
        rq = CREG(R_INTREQR);
        v = cia_beam(0);   /* torn V8/V7-V0 looked like a wrap (+1 frame) */
        f2 = amt_frames_now();
    } while (f1 != f2);
    f = f1 + (((rq & 0x0020) && v < 100) ? 1 : 0);
    if (st_init) {
        if ((LONG)(f - st_fe) < 0)
            f = st_fe;
        if (f == st_fe && v + 100 < st_v)
            f++;
    }
    st_init = 1;
    st_fe = f;
    st_v = v;
    *fe = f;
    *line = v;
}

static ULONG line_now(void)
{
    ULONG f, v;
    beam_stamp(&f, &v);
    return (ULONG)(UWORD)f * (UWORD)nlines + v;
}

static ULONG frame_now(void)
{
    ULONG f, v;
    beam_stamp(&f, &v);
    return f;
}

static ULONG tk(int b) { return b ? line_now() : frame_now(); }

static void tod_wait(int b, ULONG n)
{
    ULONG t0 = tk(b), c0 = cia_now();
    while (tk(b) - t0 < n && cia_now() - c0 < MS(30) * n)
        ;
}

#define D24(a, b) (((a) - (b)) & 0xffffffUL)

static void tod_dcase(const char *id, ULONG d, ULONG lo, ULONG hi, const char *why,
                      ULONG v0, ULONG v1, int b)
{
    if (d >= lo && d <= hi) {
        amt_case(id, AMT_ST_PASS, 0);
        return;
    }
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_int("delta_min", (long)lo);
    json_int("delta_max", (long)hi);
    json_str("unit", b ? "raster lines" : "frames");
    json_obj_end();
    json_obj("got");
    json_int("delta", (long)d);
    json_hexn("v0", v0, 6);
    json_hexn("v1", v1, 6);
    json_obj_end();
    json_str("why", why);
    json_str("window_note", "min/max come from beam stamps taken around the TOD accesses");
    amt_case_close();
}

static void tod_tests(int b)
{
    volatile UBYTE *c = b ? CIAB_BASE : CIAA_BASE;
    const char *pfx = b ? "ciab" : "ciaa";
    ULONG x0, x1, x2, x3, h, m, l, d, n, t0, alarm, sp, s0, s1, s2;
    ULONG W = b ? 10 : 3;
    ULONG to_tick = b ? MS(2) : MS(30);       /* one tick, generous */
    UBYTE v;
    static const struct { const char *id; ULONG x; } steps[] = {
        { "tod_binary_lo_09", 0x000009 },
        { "tod_binary_lo_carry", 0x0000ff },
        { "tod_binary_mid_carry", 0x00ffff },
        { "tod_rollover_24bit", 0xffffff },
    };
    int i;

    nlines = amt_probe_result.pal ? 313 : 263;
    cia_wr(c, C_CRB, 0);
    cia_wr(c, C_ICR, 0x7f);

    /* rate */
    n = b ? (AMT_QUICK() ? 200 : 600) : (AMT_QUICK() ? 5 : 15);
    tod_wait(b, 1);
    sp = tk(b);
    x0 = cia_tod_read(c);
    s0 = tk(b);
    tod_wait(b, n);
    s1 = tk(b);
    x1 = cia_tod_read(c);
    s2 = tk(b);
    d = D24(x1, x0);
    tod_dcase(cid(pfx, b ? "tod_counts_hsync" : "tod_counts_vsync"), d, s1 - s0 - 1, s2 - sp + 1,
              b ? "CIA-B TOD counts HSYNC: one count per raster line"
                : "CIA-A TOD counts the 50/60 Hz tick (VSYNC on most Amigas, else the power line; "
                  "the same rate within +-1 over this window): one count per frame",
              x0, x1, b);

    /* latch on reading the high byte, held until the low byte is read */
    sp = tk(b);
    h = cia_rd(c, C_TODHI);
    s0 = tk(b);
    tod_wait(b, W);
    m = cia_rd(c, C_TODMID);
    l = cia_rd(c, C_TODLO);
    x0 = (h << 16) | (m << 8) | l;
    x1 = cia_tod_read(c);
    s2 = tk(b);
    tod_dcase(cid(pfx, "tod_latch_hi_read"), D24(x1, x0), W - 1, s2 - sp + 1,
              "reading TODHI latches all three bytes until TODLO is read: MID/LO read W ticks later "
              "still give the old value, so a fresh read differs by >= W-1 (no latch: ~0)", x0, x1, b);
    sp = tk(b);
    x0 = cia_rd(c, C_TODLO);
    s0 = tk(b);
    tod_wait(b, W);
    x1 = cia_rd(c, C_TODLO);
    s2 = tk(b);
    tod_dcase(cid(pfx, "tod_lo_read_unlatched"), (x1 - x0) & 0xff, W - 1, s2 - sp + 1,
              "after TODLO has been read the latch is released: TODLO read alone follows the "
              "counter (W ticks waited)", x0, x1, b);

    /* write order: high stops, low starts */
    cia_wr(c, C_TODHI, 0x12);
    cia_wr(c, C_TODMID, 0x34);
    x1 = cia_tod_read(c);
    tod_wait(b, W);
    x2 = cia_tod_read(c);
    if (x1 == x2 && (x1 >> 8) == 0x1234)
        amt_case(cid(pfx, "tod_write_hi_stops"), AMT_ST_PASS, 0);
    else {
        amt_case_open(cid(pfx, "tod_write_hi_stops"), AMT_ST_FAIL);
        json_obj("expect");
        json_str("v", "$1234xx, unchanged over W ticks");
        json_obj_end();
        json_obj("got");
        json_hexn("v0", x1, 6);
        json_hexn("v1", x2, 6);
        json_int("w", (long)W);
        json_obj_end();
        json_str("why", "writing TODHI stops the TOD clock until TODLO is written");
        amt_case_close();
    }
    sp = tk(b);
    cia_wr(c, C_TODLO, 0x56);
    s0 = tk(b);
    tod_wait(b, W);
    s1 = tk(b);
    x3 = cia_tod_read(c);
    s2 = tk(b);
    /* +1 on the maximum: the 8520 data sheet does not say whether a TOD
     * pulse that arrived just before the TODLO write (while the clock was
     * halted) but is counted a little after it (the TOD input is
     * synchronised and delayed inside the chip) increments the restarted
     * clock. Amiberry counts it (cia.cpp CIA_tod_check after setting todon:
     * the HSYNC of the current line is applied at the restart), so the
     * restarted clock may be one ahead of the window. */
    tod_dcase(cid(pfx, "tod_write_lo_starts"), D24(x3, 0x123456), s1 - s0 - 1, s2 - sp + 2,
              "writing TODLO restarts the TOD clock from the written value ($123456); "
              "max = ticks in the stamp window + 1, + 1 for a pulse already in flight at the write (not specified)",
              0x123456, x3, b);

    /* binary counter (8520), not BCD (6526) */
    for (i = 0; i < 4; i++) {
        ULONG g, x = steps[i].x;
        sp = tk(b);
        cia_tod_write(c, x);
        t0 = cia_now();
        do
            g = cia_tod_read(c);
        while (g == x && cia_now() - t0 < 4 * to_tick);
        /* 8520 TODMED carry glitch: a read during the few cycles of a carry
         * across a 12-bit boundary can see a transient value (e.g. $2F ->
         * $20 -> $30 in TODMED). Use the value once two consecutive reads
         * agree. */
        if (g != x) {
            int k;
            ULONG g2;
            for (k = 0; k < 16; k++) {
                g2 = cia_tod_read(c);
                if (g2 == g)
                    break;
                g = g2;
            }
        }
        s2 = tk(b);
        d = D24(g, x);
        if (d >= 1 && d <= s2 - sp + 1)
            amt_case(cid(pfx, steps[i].id), AMT_ST_PASS, 0);
        else {
            amt_case_open(cid(pfx, steps[i].id), AMT_ST_FAIL);
            json_obj("expect");
            json_hexn("tod", (x + 1) & 0xffffff, 6);
            json_int("binary_delta_max", (long)(s2 - sp + 1));
            json_obj_end();
            json_obj("got");
            json_hexn("tod", g, 6);
            json_int("binary_delta", (long)d);
            json_obj_end();
            json_obj("in");
            json_hexn("written", x, 6);
            json_obj_end();
            json_str("why", "the 8520 TOD is a 24-bit binary counter: the first value seen after "
                     "writing X is X+k mod 2^24 with k the ticks elapsed (a BCD 6526 would show "
                     "$10 after $09)");
            amt_case_close();
        }
    }

    /* alarm */
    alarm = 0x000200;
    sp = tk(b);
    x0 = cia_tod_read(c);
    cia_alarm_write(c, alarm);
    x1 = cia_tod_read(c);
    s2 = tk(b);
    if (D24(x1, x0) <= s2 - sp + 1 && x1 != alarm)
        amt_case(cid(pfx, "tod_alarm_write_keeps_tod"), AMT_ST_PASS, 0);
    else
        tod_dcase(cid(pfx, "tod_alarm_write_keeps_tod"), D24(x1, x0), 0, s2 - sp + 1,
                  "with CRB bit 7 (ALARM) set, TOD writes go to the alarm; the TOD keeps its value "
                  "and keeps counting", x0, x1, b);

    (void)cia_icr(c);
    cia_tod_write(c, alarm - 3);
    t0 = cia_now();
    v = 0;
    x2 = 0;
    s1 = 0;
    /* the flag may have been set just after the previous poll's ICR read
     * (an interrupt or a stall can fall between two polls), so the window
     * starts at the stamp taken before the previous ICR read */
    s0 = tk(b);
    while (cia_now() - t0 < 8 * to_tick) {
        ULONG sn = tk(b);
        v = cia_icr(c);
        x2 = cia_tod_read(c);
        s1 = tk(b);
        if (v & ICR_ALRM)
            break;
        s0 = sn;
    }
    if ((v & ICR_ALRM) && D24(x2, alarm) <= s1 - s0 + 1)
        amt_case(cid(pfx, "tod_alarm_fires"), AMT_ST_PASS, 0);
    else {
        amt_case_open(cid(pfx, "tod_alarm_fires"), AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("icr_alrm", ICR_ALRM, 2);
        json_hexn("tod_at_flag", alarm, 6);
        json_int("tod_late_max", (long)(s1 - s0 + 1));
        json_obj_end();
        json_obj("got");
        json_hexn("icr", v, 2);
        json_hexn("tod_at_flag", x2, 6);
        json_obj_end();
        json_obj("in");
        json_hexn("alarm", alarm, 6);
        json_hexn("tod_start", alarm - 3, 6);
        json_obj_end();
        json_str("why", "ALRM (ICR bit 2) must be set when the TOD reaches the alarm value, and not "
                 "before (TOD read right after the flag = alarm, + the ticks of one poll)");
        amt_case_close();
    }

    cia_wr(c, C_ICR, 0x84);
    (void)cia_icr(c);
    cia_tod_write(c, alarm - 2);
    v = icr_wait(c, ICR_ALRM, 6 * to_tick);
    x3 = cia_tod_read(c);
    cia_wr(c, C_ICR, 0x7f);
    if ((v & 0x87) == 0x84)
        amt_case(cid(pfx, "tod_alarm_ir"), AMT_ST_PASS, 0);
    else {
        amt_case_open(cid(pfx, "tod_alarm_ir"), AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("icr", 0x84, 2);
        json_obj_end();
        json_obj("got");
        json_hexn("icr", v, 2);
        json_hexn("tod_after_wait", x3, 6);
        json_obj_end();
        json_obj("in");
        json_hexn("alarm", alarm, 6);
        json_hexn("tod_start", alarm - 2, 6);
        json_hexn("icr_mask_write", 0x84, 2);
        json_obj_end();
        json_str("why", "ALRM enabled in the mask: when the TOD reaches the alarm, ICR reads $84 "
                 "(IR + ALRM)");
        amt_case_close();
    }

    (void)cia_icr(c);
    cia_tod_write(c, alarm + 1);
    tod_wait(b, 4);
    v = cia_icr(c);
    rep(cid(pfx, "tod_alarm_past_no_fire"), !(v & ICR_ALRM),
        "the alarm fires only on an exact match: TOD started at alarm+1 must not set ALRM",
        "icr_alrm", 2, 0, v & ICR_ALRM, 0, 0);

    /* the same alarm many times, at changing tick phases */
    {
        int nrep = AMT_QUICK() ? 4 : 12, k, missed = 0;
        UWORD missmask = 0;
        ULONG tod_end[16];
        for (k = 0; k < nrep; k++) {
            ULONG a = 0x000300 + (ULONG)k * 0x10;
            UBYTE acc = 0;
            cia_alarm_write(c, a);
            (void)cia_icr(c);
            cia_tod_write(c, a - 2);
            t0 = cia_now();
            do {
                acc |= cia_icr(c);
                x0 = cia_tod_read(c);
            } while (D24(x0, a) > 0x800000UL || D24(x0, a) < 2 ? cia_now() - t0 < 8 * to_tick : 0);
            acc |= cia_icr(c);
            tod_end[k] = x0;
            if (!(acc & ICR_ALRM)) {
                missed++;
                missmask |= (UWORD)(1u << k);
            }
            if (b)
                tod_wait(b, (ULONG)(k % 5));   /* vary the phase */
        }
        if (!missed)
            amt_case(cid(pfx, "tod_alarm_repeat"), AMT_ST_PASS, 0);
        else {
            amt_case_open(cid(pfx, "tod_alarm_repeat"), AMT_ST_FAIL);
            json_obj("expect");
            json_int("fired", nrep);
            json_obj_end();
            json_obj("got");
            json_int("fired", nrep - missed);
            json_hexn("missed_mask", missmask, 4);
            json_arr("tod_after");
            for (k = 0; k < nrep; k++)
                json_hexn(NULL, tod_end[k], 6);
            json_arr_end();
            json_obj_end();
            json_obj("in");
            json_str("alarms", "$000300 + 16*k, TOD started at alarm-2, mask off, polled until "
                     "TOD >= alarm+2");
            json_obj_end();
            json_str("why", "every time the TOD counts through the alarm value ALRM must be set "
                     "in ICR; a miss is a lost alarm interrupt");
            amt_case_close();
        }
    }
}

/* ------------------------------------------------------------ SDR */
static void sdr_test(void)
{
    const struct tm *ta = &tms[0];
    UBYTE v = 0, acc = 0;
    ULONG t0, t1;
    tstop(ta);
    (void)cia_icr(CIAA_BASE);      /* acknowledges a pending keyboard byte */
    tload(ta, 3);
    cia_wr(CIAA_BASE, C_CRA, CR_SPMODE | CR_START);
    t0 = cia_now();
    cia_wr(CIAA_BASE, C_SDR, 0xff);   /* $FF: KDAT stays high (idle) */
    do {
        v = cia_rd(CIAA_BASE, C_ICR);
        acc |= v;
    } while (!(acc & ICR_SP) && cia_now() - t0 < MS(20));
    t1 = cia_now();
    cia_wr(CIAA_BASE, C_CRA, 0);      /* back to input: keyboard listens again */
    if (acc & ICR_SP) {
        amt_case_open("ciaa_sdr_output_shift", AMT_ST_PASS);
        json_obj("timing");
        json_int("cc", (long)(t1 - t0));
        json_int("expect_e", 16 * 4);
        json_obj_end();
        amt_case_close();
    } else {
        amt_case_open("ciaa_sdr_output_shift", AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("icr_sp", ICR_SP, 2);
        json_obj_end();
        json_obj("got");
        json_hexn("icr", acc, 2);
        json_obj_end();
        json_obj("in");
        json_hexn("cra", CR_SPMODE | CR_START, 2);
        json_int("ta_latch", 3);
        json_hexn("sdr", 0xff, 2);
        json_obj_end();
        json_str("why", "serial port in output mode, clocked by timer A underflows (CNT = TA/2): "
                 "8 bits shift out in 16 TA underflows (64 E clocks) and set ICR SP; not seen in 20 ms");
        amt_case_close();
    }
}

/* ------------------------------------------------------------ E clock */
static void eclock_perf(int b)
{
    const struct tm *ta = &tms[b * 2];
    ULONG t0, t1;
    tstop(ta);
    tload(ta, 0xffff);
    t0 = cia_now();
    cia_wr(ta->c, ta->cr, CR_RUNMODE | CR_LOAD | CR_START);
    twait_stop(ta, MS(200));
    t1 = cia_now();
    tstop(ta);
    /* 65536 E clocks; expected 5 colour clocks per E clock */
    amt_perf(b ? "ciab_ta_eclock_65536" : "ciaa_ta_eclock_65536", 65536, t1 - t0);
}

/* ------------------------------------------------------------ main */
static char *hx2(char *d, UBYTE v)
{
    static const char hd[] = "0123456789abcdef";
    *d++ = hd[v >> 4];
    *d++ = hd[v & 15];
    return d;
}

static char *put(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    return d;
}

static void log_env(void)
{
    char buf[128], *d = buf;
    d = put(d, "cia env: ciaa cra=");
    d = hx2(d, sav_a.cra);
    d = put(d, " crb=");
    d = hx2(d, sav_a.crb);
    d = put(d, " osmask=");
    d = hx2(d, sav_a.osmask);
    d = put(d, sav_a.have_res ? " res" : " nores");
    d = put(d, "; ciab cra=");
    d = hx2(d, sav_b.cra);
    d = put(d, " crb=");
    d = hx2(d, sav_b.crb);
    d = put(d, " osmask=");
    d = hx2(d, sav_b.osmask);
    d = put(d, sav_b.have_res ? " res" : " nores");
    *d = 0;
    amt_log(buf);
}

static void body(void)
{
    int i, b;
    for (i = 0; i < 4; i++)
        timer_tests(&tms[i]);
    for (b = 0; b < 2; b++) {
        cascade_tests(b);
        icr_tests(b);
        tod_tests(b);
    }
    sdr_test();
    eclock_perf(0);
    eclock_perf(1);
}

void amt_run_cia(void)
{
    int code;
    cia_save(&sav_a, CIAA_BASE, 1);
    cia_save(&sav_b, CIAB_BASE, 1);
    stuck = 0;
    if (cia_int_line_stuck(INTB_PORTS))
        stuck |= INTB_PORTS;
    if (cia_int_line_stuck(INTB_EXTER))
        stuck |= INTB_EXTER;
    log_env();
    code = amt_try(body);       /* restore the CIAs even after an abort */
    cia_restore(&sav_b);
    cia_restore(&sav_a);
    if (code)
        amt_abort(code);
}
