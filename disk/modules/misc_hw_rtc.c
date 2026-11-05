/* misc_hw part: battery-backed clock at $DC0000.
 *
 * Two chips are used by Commodore at the same address, both 4-bit wide with
 * one register per longword (register n at $DC0000 + 4n, data on D3-D0 of
 * the byte at +3; the upper nibble is open bus and masked off):
 *   - OKI MSM6242B (A500+, A2000, CDTV; Amiberry "MSM6242B"): S1 S10 MI1
 *     MI10 H1 H10 D1 D10 MO1 MO10 Y1 Y10 W, CD (HOLD b0, BUSY b1, IRQ FLAG b2,
 *     30-s ADJ b3), CE (MASK b0, ITRPT/STND b1, t0 b2, t1 b3), CF (REST b0,
 *     STOP b1, 24/12 b2, TEST b3). PM bit = H10 bit 2. Leap year = year
 *     digits divisible by 4. (OKI MSM6242B data sheet.)
 *   - Ricoh RP5C01 / RF5C01A (A3000, A4000): mode 00: S1 S10 MI1 MI10 H1 H10
 *     W D1 D10 MO1 MO10 Y1 Y10; mode 01: alarm, 12/24 select (reg A bit 0,
 *     1 = 24 h), leap-year counter (reg B bits 1-0, 0 = leap year); modes
 *     10/11: two 13 x 4-bit RAM blocks. MODE reg D (bits 1-0 mode, b2 ALARM
 *     EN, b3 TIMER EN), TEST reg E and RESET reg F (b0 alarm reset, b1
 *     reset of the divider stages for seconds and below, b2 16 Hz, b3 1 Hz
 *     output) are write-only. PM bit = H10 bit 1. (Ricoh RP5C01 data sheet.)
 *
 * Detection is read-only: all 16 registers are read and must decode as a
 * valid date in one of the two layouts; MODE/CD bit 3 tells them apart
 * (TIMER EN is 1 on a running RP5C01, 30-s ADJ is 0 on an idle MSM6242B).
 *
 * The time is saved at a second edge and restored at the end, adjusted for
 * the elapsed time (colour-clock beam counter), so the machine leaves with
 * the right time to within one frame. The RP5C01 RAM is saved and restored;
 * the control registers are restored (the RP5C01 RESET/TEST registers are
 * left at 0, their normal value). */
#include "misc_hw.h"

#define RTC_BASE 0xdc0000UL

#define CHIP_NONE  0
#define CHIP_MSM   1
#define CHIP_RIC   2

/* logical digit order */
enum { S1, S10, MI1, MI10, H1, H10, D1, D10, MO1, MO10, Y1, Y10, WD, NDIG };
static const UBYTE msm_ix[NDIG] = { 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12 };
static const UBYTE ric_ix[NDIG] = { 0, 1, 2, 3, 4, 5, 7, 8, 9, 10, 11, 12, 6 };

/* MSM6242B */
#define CD_HOLD  1
#define CD_BUSY  2
#define CD_ADJ   8
#define CF_REST  1
#define CF_STOP  2
#define CF_24H   4
#define CF_TEST  8
/* RP5C01 */
#define MD_ALARM 4
#define MD_TIMER 8
#define RS_SEC   2

struct rt {
    UBYTE s, mi, h, d, mo, y, w;   /* h 0-23 */
    UBYTE leap;                    /* RP5C01 leap-year counter */
};

static int chip;
static const UBYTE *ix;
static UBYTE pm_bit;
static int cur24;
static UBYTE ric_md;               /* RP5C01 MODE as last written */

static struct {
    UBYTE cd, ce, cf;              /* MSM */
    UBYTE md, sel, leap;           /* RP5C01 */
    UBYTE ram[26];
    int is24;
    int have_t0, running, ram_saved;
    struct rt t0;
    ULONG tk0;
} sv;

/* ------------------------------------------------------------ access */
static UBYTE rr(int r) { return hw_rd8(RTC_BASE + 4UL * r + 3) & 15; }
static void rw(int r, UBYTE v) { hw_wr8(RTC_BASE + 4UL * r + 3, v & 15); }

static void ric_mode(UBYTE m) { ric_md = m & 15; rw(13, ric_md); }
static void ric_bank(int b) { ric_mode((ric_md & 0x0c) | (b & 3)); }

static int is_leap(const struct rt *t)
{
    return chip == CHIP_RIC ? (t->leap & 3) == 0 : (t->y % 4) == 0;
}

static int mdays(const struct rt *t)
{
    static const UBYTE n[13] = { 0, 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    if (t->mo == 2 && is_leap(t))
        return 29;
    return t->mo <= 12 ? n[t->mo] : 31;
}

static void add_secs(struct rt *t, ULONG n)
{
    while (n--) {
        if (++t->s < 60)
            continue;
        t->s = 0;
        if (++t->mi < 60)
            continue;
        t->mi = 0;
        if (++t->h < 24)
            continue;
        t->h = 0;
        t->w = (UBYTE)((t->w + 1) % 7);
        if (++t->d <= mdays(t))
            continue;
        t->d = 1;
        if (++t->mo <= 12)
            continue;
        t->mo = 1;
        t->y = (UBYTE)((t->y + 1) % 100);
        t->leap = (UBYTE)((t->leap + 1) & 3);
    }
}

static void encode(const struct rt *t, int is24, UBYTE *g)
{
    UBYTE h = t->h, hh;
    g[S1] = t->s % 10;  g[S10] = t->s / 10;
    g[MI1] = t->mi % 10; g[MI10] = t->mi / 10;
    if (is24) {
        g[H1] = h % 10; g[H10] = h / 10;
    } else {
        hh = h % 12;
        if (!hh)
            hh = 12;
        g[H1] = hh % 10;
        g[H10] = (UBYTE)(hh / 10) | (h >= 12 ? pm_bit : 0);
    }
    g[D1] = t->d % 10;  g[D10] = t->d / 10;
    g[MO1] = t->mo % 10; g[MO10] = t->mo / 10;
    g[Y1] = t->y % 10;  g[Y10] = t->y / 10;
    g[WD] = t->w;
}

/* raw digits -> time; 1 if every digit is in range and the date exists */
static int decode(const UBYTE *g, int is24, UBYTE leap, struct rt *t)
{
    int ok = 1, i;
    t->leap = leap;
    for (i = 0; i < NDIG; i++)
        if (g[i] > 9)
            ok = 0;
    if (g[S10] > 5 || g[MI10] > 5 || g[D10] > 3 || g[MO10] > 1 || g[WD] > 6)
        ok = 0;
    t->s = g[S10] * 10 + g[S1];
    t->mi = g[MI10] * 10 + g[MI1];
    if (is24) {
        if (g[H10] > 2)
            ok = 0;
        t->h = (g[H10] & 3) * 10 + g[H1];
        if (t->h > 23)
            ok = 0;
    } else {
        /* twelve o'clock in 12-hour mode (review M13):
         *   MSM6242B: "12" (12, 1 .. 11). The OKI sheet only says H10 is
         *     0-1 with the PM/AM bit; the register-compatible Epson
         *     RTC-72421 manual (ETM17E-02, H10 register) gives the range
         *     "12:00 to 11:59, a.m. and p.m.", so hour 0 is invalid.
         *   RP5C01: the Ricoh sheet (EK-086-9804, 12/24 select register)
         *     gives no hour range; emulators of the chip count AM 00-11 /
         *     PM 20-31. Undefined by the sheet, so both "12" and "0" are
         *     accepted (CONTRACT section 10 rule 1). */
        UBYTE hh = (g[H10] & 1) * 10 + g[H1];
        if ((g[H10] & ~(pm_bit | 1)) || hh < (chip == CHIP_RIC ? 0 : 1) || hh > 12)
            ok = 0;
        t->h = (UBYTE)(hh % 12 + ((g[H10] & pm_bit) ? 12 : 0));
    }
    t->d = g[D10] * 10 + g[D1];
    t->mo = g[MO10] * 10 + g[MO1];
    t->y = g[Y10] * 10 + g[Y1];
    t->w = g[WD];
    if (t->s > 59 || t->mi > 59 || t->mo < 1 || t->mo > 12 || t->d < 1 || t->d > mdays(t))
        ok = 0;
    return ok;
}

static int same(const struct rt *a, const struct rt *b, int with_leap)
{
    return a->s == b->s && a->mi == b->mi && a->h == b->h && a->d == b->d && a->mo == b->mo &&
           a->y == b->y && a->w == b->w && (!with_leap || a->leap == b->leap);
}

/* MSM HOLD procedure (data sheet): HOLD=1, then BUSY must be 0, else drop
 * HOLD, wait and retry */
static void msm_lock(void)
{
    int n = 5;
    rw(13, CD_HOLD);
    while ((rr(13) & CD_BUSY) && n--) {
        rw(13, 0);
        mh_wait_cck(mh_cck_ms(1));
        rw(13, CD_HOLD);
    }
}
static void msm_unlock(void) { rw(13, 0); }

static void rd_digits_raw(UBYTE *g)
{
    int i;
    for (i = 0; i < NDIG; i++)
        g[i] = rr(ix[i]);
}

static void read_digits(UBYTE *g)
{
    int i, n;
    if (chip == CHIP_MSM) {
        msm_lock();
        rd_digits_raw(g);
        msm_unlock();
        return;
    }
    if (ric_md & 3)
        ric_bank(0);
    for (n = 0; n < 5; n++) {
        UBYTE h[NDIG];
        rd_digits_raw(g);
        rd_digits_raw(h);
        for (i = 0; i < NDIG && g[i] == h[i]; i++)
            ;
        if (i == NDIG)
            return;
    }
}

static UBYTE ric_leap_read(void)
{
    UBYTE v;
    ric_bank(1);
    v = rr(11) & 3;
    ric_bank(0);
    return v;
}

static int read_time(struct rt *t, UBYTE *g)
{
    UBYTE leap;
    read_digits(g);
    leap = chip == CHIP_RIC ? ric_leap_read() : 0;
    return decode(g, cur24, leap, t);
}

/* stop / start the clock; stopping also resets the sub-second divider */
static void clk_stop(void)
{
    if (chip == CHIP_MSM) {
        rw(15, (cur24 ? CF_24H : 0) | CF_STOP | CF_REST);
    } else {
        ric_mode(ric_md & MD_ALARM);
        rw(15, RS_SEC);
    }
}
static void clk_start(void)
{
    if (chip == CHIP_MSM) {
        rw(15, cur24 ? CF_24H : 0);
    } else {
        ric_mode((ric_md & MD_ALARM) | MD_TIMER);
        rw(15, 0);
    }
}

/* only while stopped */
static void set24(int is24)
{
    if (chip == CHIP_MSM) {
        /* change the 24/12 bit with REST = 1 (data sheet) */
        rw(15, (cur24 ? CF_24H : 0) | CF_STOP | CF_REST);
        rw(15, (is24 ? CF_24H : 0) | CF_STOP | CF_REST);
    } else {
        ric_bank(1);
        rw(10, is24 ? 1 : 0);
        ric_bank(0);
    }
    cur24 = is24;
}

/* only while stopped */
static void write_time(const struct rt *t)
{
    UBYTE g[NDIG];
    int i;
    encode(t, cur24, g);
    if (chip == CHIP_RIC) {
        ric_bank(1);
        rw(11, t->leap & 3);
        ric_bank(0);
    }
    for (i = 0; i < NDIG; i++)
        rw(ix[i], g[i]);
}

/* wait for the seconds digit to change; 1 on success */
static int wait_edge(ULONG timeout_ms)
{
    ULONG t0 = amt_ticks(), lim = mh_cck_ms(timeout_ms);
    UBYTE a, b, s0;
    if (chip == CHIP_RIC && (ric_md & 3))
        ric_bank(0);
    do {
        a = rr(0);
        b = rr(0);
    } while (a != b);
    s0 = a;
    while (amt_ticks() - t0 < lim) {
        a = rr(0);
        b = rr(0);
        if (a == b && a != s0)
            return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ reporting */
static char *fmt2(char *d, UBYTE v)
{
    *d++ = (char)('0' + (v / 10) % 10);
    *d++ = (char)('0' + v % 10);
    *d = 0;
    return d;
}

static char tbuf[48];
static const char *fmt_time(const struct rt *t)
{
    char *d = tbuf;
    *d++ = 'y'; d = fmt2(d, t->y);
    d = mh_cpy(d, "-"); d = fmt2(d, t->mo);
    d = mh_cpy(d, "-"); d = fmt2(d, t->d);
    d = mh_cpy(d, " "); d = fmt2(d, t->h);
    d = mh_cpy(d, ":"); d = fmt2(d, t->mi);
    d = mh_cpy(d, ":"); d = fmt2(d, t->s);
    d = mh_cpy(d, " w"); *d++ = (char)('0' + t->w);
    if (chip == CHIP_RIC) {
        d = mh_cpy(d, " leap"); *d++ = (char)('0' + (t->leap & 3));
    }
    *d = 0;
    return tbuf;
}

static void json_digits(const char *k, const UBYTE *g)
{
    char b[NDIG + 1];
    int i;
    for (i = 0; i < NDIG; i++)
        b[i] = "0123456789abcdef"[g[i] & 15];
    b[NDIG] = 0;
    json_str(k, b);
}

static void case_time(const char *id, int ok, const struct rt *exp, const struct rt *got,
                      const UBYTE *g, const char *why, const char *in)
{
    if (ok) {
        amt_case(id, AMT_ST_PASS, 0);
        return;
    }
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_str("time", fmt_time(exp));
    json_obj_end();
    json_obj("got");
    if (got)
        json_str("time", fmt_time(got));
    if (g)
        json_digits("digits", g);
    json_obj_end();
    json_str("why", why);
    json_obj("in");
    json_str("chip", chip == CHIP_MSM ? "MSM6242B" : "RP5C01");
    json_int("mode24", cur24);
    if (in)
        json_str("set", in);
    json_obj_end();
    json_obj("ctx");
    json_str("digit_order", "S1 S10 MI1 MI10 H1 H10 D1 D10 MO1 MO10 Y1 Y10 W");
    json_obj_end();
    amt_case_close();
}

/* ------------------------------------------------------------ detection */
static int layout_ok(const UBYTE *v, const UBYTE *map, UBYTE pmb, int is24)
{
    UBYTE g[NDIG];
    struct rt t;
    int i;
    UBYTE save_pm = pm_bit;
    int saved_chip = chip, ok;
    for (i = 0; i < NDIG; i++)
        g[i] = v[map[i]];
    /* lenient on the 10-year digit only: rtc_digits_valid reports it */
    if (g[Y10] > 9)
        g[Y10] = 0;
    pm_bit = pmb;
    chip = map == ric_ix ? CHIP_RIC : CHIP_MSM;
    ok = decode(g, is24, 0, &t);
    pm_bit = save_pm;
    chip = saved_chip;
    return ok;
}

static int orig_md = -1;

static int detect(char *how)
{
    UBYTE v[16], w[16];
    int i, diff = 0, stable = 1;
    for (i = 0; i < 16; i++)
        v[i] = rr(i);
    mh_wait_cck(mh_cck_ms(2));
    for (i = 0; i < 16; i++) {
        w[i] = rr(i);
        if (i > 1 && w[i] != v[i])      /* seconds may tick */
            stable = 0;
        if (v[i] != v[0])
            diff = 1;
    }
    for (i = 0; i < 16; i++)
        *how++ = "0123456789abcdef"[v[i]];
    *how = 0;
    if (!diff || !stable)
        return CHIP_NONE;
    if ((v[13] & 0x0b) == (MD_TIMER | 1) && v[10] <= 1 && v[11] <= 3) {
        /* RP5C01 left in mode 01 (Kickstart 3.1 battclock does that): the
         * 12/24 select and leap counter read sane, so this is the chip;
         * select mode 00 to confirm the time layout, else put MODE back */
        rw(13, v[13] & 0x0c);
        for (i = 0; i < 16; i++)
            w[i] = rr(i);
        w[13] = v[13];
        if (layout_ok(w, ric_ix, 2, v[10] & 1)) {
            orig_md = v[13];
            return CHIP_RIC;
        }
        rw(13, v[13]);
        return CHIP_NONE;
    }
    if ((v[13] & 0x0b) == MD_TIMER) {
        /* RP5C01 in mode 00, timer running: 12/24 select is in mode 01 and
         * cannot be read without a write, so accept either hour format */
        if (layout_ok(v, ric_ix, 2, 1) || layout_ok(v, ric_ix, 2, 0))
            return CHIP_RIC;
    } else if (!(v[13] & CD_ADJ)) {
        if (layout_ok(v, msm_ix, 4, (v[15] & CF_24H) != 0))
            return CHIP_MSM;
    }
    return CHIP_NONE;
}

/* ------------------------------------------------------------ save/restore */
static void save_state(void)
{
    UBYTE g[NDIG];
    int i;
    if (chip == CHIP_MSM) {
        sv.cd = rr(13) & (CD_HOLD | 4);
        sv.ce = rr(14);
        sv.cf = rr(15);
        sv.is24 = (sv.cf & CF_24H) != 0;
        msm_unlock();
    } else {
        sv.md = orig_md >= 0 ? (UBYTE)orig_md : rr(13);
        ric_md = rr(13);
        ric_bank(1);
        sv.sel = rr(10);
        sv.leap = rr(11) & 3;
        for (i = 0; i < 13; i++) {
            ric_bank(2);
            sv.ram[i] = rr(i);
            ric_bank(3);
            sv.ram[13 + i] = rr(i);
        }
        sv.ram_saved = 1;
        ric_bank(0);
        sv.is24 = sv.sel & 1;
    }
    cur24 = sv.is24;
    sv.running = wait_edge(2500);
    sv.tk0 = amt_ticks();
    read_time(&sv.t0, g);
    sv.have_t0 = 1;
}

static void restore_state(void)
{
    int i;
    if (!sv.have_t0)
        return;
    clk_stop();
    if (cur24 != sv.is24)
        set24(sv.is24);
    if (chip == CHIP_RIC && sv.ram_saved) {
        for (i = 0; i < 13; i++) {
            ric_bank(2);
            rw(i, sv.ram[i]);
            ric_bank(3);
            rw(i, sv.ram[13 + i]);
        }
        ric_bank(0);
    }
    if (sv.running) {
        struct rt t = sv.t0;
        ULONG n = mh_ms_since(sv.tk0) / 1000 + 2;
        ULONG lim = n * amt_cck_hz();
        add_secs(&t, n);
        write_time(&t);
        while (amt_ticks() - sv.tk0 < lim)
            ;
        clk_start();
    } else {
        struct rt t = sv.t0;
        t.leap = sv.leap;
        write_time(&t);
    }
    if (chip == CHIP_MSM) {
        rw(14, sv.ce);
        rw(15, sv.cf);
        rw(13, sv.cd & CD_HOLD);
    } else {
        ric_bank(1);
        rw(10, sv.sel);
        ric_bank(0);
        rw(15, 0);
        ric_mode(sv.md);
    }
}

/* ------------------------------------------------------------ cases */
static int wr_ok;

static void case_edge_timeout(const char *id, const char *why)
{
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_int("seconds_edges_min", 1);
    json_int("within_ms", 2500);
    json_obj_end();
    json_obj("got");
    json_int("seconds_edges", 0);
    json_obj_end();
    json_str("why", why);
    amt_case_close();
}

static void t_digits_counting(void)
{
    UBYTE g[NDIG];
    struct rt t;
    int ok = read_time(&t, g);
    if (ok)
        amt_case("rtc_digits_valid", AMT_ST_PASS, 0);
    else {
        amt_case_open("rtc_digits_valid", AMT_ST_FAIL);
        json_obj("expect");
        json_str("digits", "BCD in range: S10,MI10<=5, H 0-23 (24 h) or 1-12+PM, D 1-days of month, MO 1-12, W 0-6");
        json_obj_end();
        json_obj("got");
        json_digits("digits", g);
        json_obj_end();
        json_str("why", "the running clock shows a time that does not exist");
        amt_case_close();
    }
    if (sv.running)
        amt_case("rtc_counting", AMT_ST_PASS, 0);
    else
        case_edge_timeout("rtc_counting", "the seconds digit did not change within 2.5 s: clock stopped or not counting");
}

static void t_rate(void)
{
    ULONG t0, ms;
    int k;
    if (!sv.running) {
        amt_case("rtc_rate", AMT_ST_UNTESTABLE, "clock not counting (see rtc_counting)");
        return;
    }
    if (!wait_edge(2500)) {
        case_edge_timeout("rtc_rate", "no second edge within 2.5 s");
        return;
    }
    t0 = amt_ticks();
    for (k = 0; k < 3; k++)
        if (!wait_edge(2500))
            break;
    ms = mh_ms_since(t0);
    amt_case_open("rtc_rate", (k == 3 && ms >= 2850 && ms <= 3150) ? AMT_ST_PASS : AMT_ST_FAIL);
    if (!(k == 3 && ms >= 2850 && ms <= 3150)) {
        json_obj("expect");
        json_str("ms_for_3_seconds", "2850-3150");
        json_obj_end();
        json_obj("got");
        json_int("edges", k);
        json_obj_end();
        json_str("why", "three seconds of the RTC do not take three seconds of the colour clock (+-5%)");
    }
    json_obj("timing");
    json_int("ms_for_3_seconds", (long)ms);
    json_obj_end();
    amt_case_close();
}

/* MSM6242B: HOLD=1 inhibits the 1 Hz carry into S1 (registers stay
 * readable and constant); a carry that falls inside the hold is applied
 * when HOLD returns to 0 (hold < 1 s). The prescaler keeps running, so the
 * next carry keeps its phase. */
static void t_hold(void)
{
    UBYTE a[NDIG], b[NDIG], c[NDIG];
    struct rt ta, tb, tc, te;
    ULONG e0, ms2 = 0;
    int ok_ab, ok_c, ok_ph, i;
    if (!sv.running) {
        amt_case("rtc_hold", AMT_ST_UNTESTABLE, "clock not counting (see rtc_counting)");
        return;
    }
    if (!wait_edge(2500)) {
        case_edge_timeout("rtc_hold", "no second edge within 2.5 s");
        return;
    }
    e0 = amt_ticks();
    while (amt_ticks() - e0 < mh_cck_ms(450))
        ;
    rw(13, CD_HOLD);
    for (i = 0; i < 5 && (rr(13) & CD_BUSY); i++)
        mh_wait_cck(mh_cck_ms(1));
    rd_digits_raw(a);
    while (amt_ticks() - e0 < mh_cck_ms(1300))
        ;
    rd_digits_raw(b);
    rw(13, 0);
    mh_wait_cck(mh_cck_ms(60));
    read_digits(c);
    if (wait_edge(1500))
        ms2 = mh_ms_since(e0);
    decode(a, cur24, 0, &ta);
    decode(b, cur24, 0, &tb);
    decode(c, cur24, 0, &tc);
    te = ta;
    add_secs(&te, 1);
    ok_ab = 1;
    for (i = 0; i < NDIG; i++)
        if (a[i] != b[i])
            ok_ab = 0;
    ok_c = same(&tc, &te, 0);
    ok_ph = ms2 >= 1850 && ms2 <= 2150;
    if (ok_ab && ok_c && ok_ph) {
        amt_case_open("rtc_hold", AMT_ST_PASS);
    } else {
        amt_case_open("rtc_hold", AMT_ST_FAIL);
        json_obj("expect");
        json_digits("held_later", a);
        json_str("after_release", fmt_time(&te));
        json_str("next_edge_ms", "1850-2150");
        json_obj_end();
        json_obj("got");
        json_digits("held_later", b);
        json_digits("after_release", c);
        json_obj_end();
        json_str("why", !ok_ab ? "registers changed while HOLD=1 (a 1 Hz carry passed through the hold)"
                 : !ok_c ? "the carry that fell inside the hold was not applied (exactly once) when HOLD returned to 0"
                 : "the next carry after the hold is not at its old phase (prescaler stopped or reset by HOLD)");
        json_obj("in");
        json_str("procedure", "edge+450ms HOLD=1, read; edge+1300ms read, HOLD=0; +60ms read; wait next edge");
        json_obj_end();
    }
    json_obj("timing");
    json_int("next_edge_ms", (long)ms2);
    json_obj_end();
    amt_case_close();
}

/* MSM6242B: BUSY (CD bit 1) is 1 while the counters carry, once a second */
static void t_busy(void)
{
    ULONG t0 = amt_ticks(), lim = mh_cck_ms(1200), n = 0, busy = 0;
    if (!sv.running) {
        amt_case("rtc_busy", AMT_ST_UNTESTABLE, "clock not counting (see rtc_counting)");
        return;
    }
    rw(13, 0);
    while (amt_ticks() - t0 < lim) {
        if (rr(13) & CD_BUSY)
            busy++;
        n++;
    }
    if (busy)
        amt_case("rtc_busy", AMT_ST_PASS, 0);
    else {
        amt_case_open("rtc_busy", AMT_ST_FAIL);
        json_obj("expect");
        json_str("busy_samples", ">0 in 1.2 s");
        json_obj_end();
        json_obj("got");
        json_int("busy_samples", (long)busy);
        json_obj_end();
        json_str("why", "the running MSM6242B did not assert BUSY during a full 1 Hz carry period");
        json_obj("timing");
        json_int("polls", (long)n);
        json_obj_end();
        amt_case_close();
    }
}

static void t_msm_ctrl(void)
{
    static const UBYTE pat[] = { 1, 3, 5, 7, 9, 11, 13, 15 };
    UBYTE got = 0;
    int i, bad = -1;
    /* CE: MASK=1 in every pattern so STD.P stays inactive */
    for (i = 0; i < 8; i++) {
        rw(14, pat[i]);
        got = rr(14);
        if (got != pat[i]) {
            bad = i;
            break;
        }
    }
    rw(14, sv.ce);
    if (bad < 0)
        amt_case("rtc_ce_rw", AMT_ST_PASS, 0);
    else {
        amt_case_open("rtc_ce_rw", AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("ce", pat[bad], 1);
        json_obj_end();
        json_obj("got");
        json_hexn("ce", got, 1);
        json_obj_end();
        json_str("why", "control register E (MASK, ITRPT/STND, t0, t1) is read/write");
        amt_case_close();
    }
    if (!(sv.cf & CF_TEST))
        amt_case("rtc_cf_test_clear", AMT_ST_PASS, 0);
    else
        amt_check32("rtc_cf_test_clear", "cf", sv.cf & ~CF_TEST, sv.cf,
                    "CF TEST bit is set: the chip is in test mode (fast counting); it must be 0 in normal use");
}

static void t_ric_ram(void)
{
    UBYTE got = 0, exp = 0;
    int i, b, bad = -1, badb = 0;
    for (b = 2; b <= 3 && bad < 0; b++) {
        ric_bank(b);
        for (i = 0; i < 13; i++)
            rw(i, (UBYTE)(i * 5 + b * 3 + 1));
        for (i = 0; i < 13; i++) {
            exp = (UBYTE)((i * 5 + b * 3 + 1) & 15);
            got = rr(i);
            if (got != exp) {
                bad = i;
                badb = b;
                break;
            }
        }
        for (i = 0; i < 13 && bad < 0; i++)
            rw(i, (UBYTE)~(i * 5 + b * 3 + 1));
        for (i = 0; i < 13 && bad < 0; i++) {
            exp = (UBYTE)(~(i * 5 + b * 3 + 1) & 15);
            got = rr(i);
            if (got != exp) {
                bad = i;
                badb = b;
            }
        }
    }
    /* restore the RAM now (also done again at the end) */
    for (i = 0; i < 13; i++) {
        ric_bank(2);
        rw(i, sv.ram[i]);
        ric_bank(3);
        rw(i, sv.ram[13 + i]);
    }
    ric_bank(0);
    if (bad < 0)
        amt_case("rtc_ram_rw", AMT_ST_PASS, 0);
    else {
        amt_case_open("rtc_ram_rw", AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("nibble", exp, 1);
        json_obj_end();
        json_obj("got");
        json_hexn("nibble", got, 1);
        json_obj_end();
        json_str("why", "RP5C01 RAM block (mode 10/11, 13 x 4 bit) does not keep what was written");
        json_obj("in");
        json_int("mode", badb);
        json_int("reg", bad);
        json_obj_end();
        amt_case_close();
    }
}

static void t_ric_mode1(void)
{
    UBYTE got[4];
    int i, ok = 1;
    ric_bank(1);
    for (i = 0; i < 4; i++) {
        rw(11, (UBYTE)i);
        got[i] = rr(11) & 3;
        if (got[i] != i)
            ok = 0;
    }
    rw(11, sv.leap);
    ric_bank(0);
    if (ok)
        amt_case("rtc_leap_counter_rw", AMT_ST_PASS, 0);
    else {
        amt_case_open("rtc_leap_counter_rw", AMT_ST_FAIL);
        json_obj("expect");
        json_str("leap", "0123");
        json_obj_end();
        json_obj("got");
        json_hexn("leap0", got[0], 1); json_hexn("leap1", got[1], 1);
        json_hexn("leap2", got[2], 1); json_hexn("leap3", got[3], 1);
        json_obj_end();
        json_str("why", "mode 01 register B (leap-year counter, bits 1-0) is read/write");
        amt_case_close();
    }
    if ((ric_md & 3) == 0 && (rr(13) & 3) == 0)
        amt_case("rtc_mode_select", AMT_ST_PASS, 0);
    else
        amt_check32("rtc_mode_select", "mode", ric_md, rr(13), "MODE register bits 1-0 did not read back the selected mode");
}

/* stop: the counters do not move while stopped (MSM CF STOP, RP5C01 TIMER EN=0) */
static void t_stop(void)
{
    UBYTE a[NDIG], b[NDIG];
    UBYTE leap;
    struct rt ta, tb;
    int i, ok = 1;
    if (!sv.running) {
        amt_case("rtc_stop", AMT_ST_UNTESTABLE, "clock not counting (see rtc_counting)");
        return;
    }
    clk_stop();
    leap = chip == CHIP_RIC ? ric_leap_read() : 0;
    rd_digits_raw(a);
    mh_wait_ms(1500);
    rd_digits_raw(b);
    for (i = 0; i < NDIG; i++)
        if (a[i] != b[i])
            ok = 0;
    clk_start();
    if (ok) {
        amt_case("rtc_stop", AMT_ST_PASS, 0);
        return;
    }
    decode(a, cur24, leap, &ta);
    decode(b, cur24, leap, &tb);
    case_time("rtc_stop", 0, &ta, &tb, b,
              chip == CHIP_MSM ? "the counters advanced for 1.5 s with CF STOP=1 (and REST=1)"
                               : "the counters advanced for 1.5 s with MODE TIMER EN=0 (and the second divider reset)",
              0);
}

static const struct rt T_A = { 42, 37, 13, 14, 7, 95, 5, 3 };

static void t_write_readback(void)
{
    UBYTE g[NDIG];
    struct rt t;
    clk_stop();
    if (cur24 != 1)
        set24(1);
    write_time(&T_A);
    read_time(&t, g);
    wr_ok = same(&t, &T_A, chip == CHIP_RIC);
    case_time("rtc_write_readback", wr_ok, &T_A, &t, g,
              "time written with the clock stopped did not read back (the counters are read/write)", 0);
}

/* set t (24 h unless mode12), start, wait for the first carry, compare */
static void roll(const char *id, const struct rt *t, int mode12, const char *why)
{
    UBYTE g[NDIG];
    struct rt e = *t, got;
    int edge;
    if (!wr_ok) {
        amt_case(id, AMT_ST_UNTESTABLE, "depends on rtc_write_readback, which failed: the counters did not take the written time");
        return;
    }
    clk_stop();
    if (cur24 != !mode12)
        set24(!mode12);
    write_time(t);
    clk_start();
    edge = wait_edge(2500);
    read_time(&got, g);
    clk_stop();
    add_secs(&e, 1);
    if (!edge) {
        case_edge_timeout(id, "no carry within 2.5 s after starting the clock");
        return;
    }
    case_time(id, same(&got, &e, chip == CHIP_RIC), &e, &got, g, why, fmt_time(t));
}

static void t_rollovers(void)
{
    struct rt t;
    UBYTE g[NDIG];

    /* start: the first carry comes after starting, from the written time */
    t = T_A;
    roll("rtc_start_counts", &t, 0, "after start the first carry must advance the written time by exactly one second");
    t.s = 59;
    roll("rtc_roll_minute", &t, 0, "59 s carries into the minutes");
    t.mi = 59;
    roll("rtc_roll_hour", &t, 0, "59:59 carries into the hours");
    t.h = 23; t.d = 30; t.mo = 4; t.y = 23; t.w = 6; t.leap = 3;
    roll("rtc_roll_day_month", &t, 0, "April 30 23:59:59 -> May 1, day of week 6 -> 0");
    t.d = 28; t.mo = 2; t.y = 24; t.w = 3; t.leap = 0;
    roll("rtc_roll_feb28_leap", &t, 0, "leap year (MSM: year digits divisible by 4; RP5C01: leap counter 0): Feb 28 -> Feb 29");
    t.d = 29;
    roll("rtc_roll_feb29_leap", &t, 0, "leap year: Feb 29 -> Mar 1");
    t.d = 28; t.y = 23; t.leap = 3;
    roll("rtc_roll_feb28_common", &t, 0, "common year (MSM: year 23; RP5C01: leap counter 3): Feb 28 -> Mar 1");
    t.d = 31; t.mo = 12; t.y = 99; t.leap = 3;
    roll("rtc_roll_year", &t, 0, "Dec 31 23:59:59 of year 99 -> year 00 Jan 1 (RP5C01: leap counter 3 -> 0)");

    /* 12-hour mode */
    if (!wr_ok) {
        amt_case("rtc_12h_readback", AMT_ST_UNTESTABLE, "depends on rtc_write_readback, which failed: the counters did not take the written time");
    } else {
        struct rt got;
        UBYTE e[NDIG];
        int i, ok = 1;
        t = T_A;
        t.h = 15;
        clk_stop();
        set24(0);
        write_time(&t);
        read_digits(g);
        encode(&t, 0, e);
        for (i = 0; i < NDIG; i++)
            if (g[i] != e[i])
                ok = 0;
        decode(g, 0, t.leap, &got);
        case_time("rtc_12h_readback", ok, &t, &got, g,
                  "15:37:42 written in 12-hour mode as 3 PM (H10 = PM bit | 0, H1 = 3) did not read back", 0);
    }
    t = T_A;
    t.h = 11; t.mi = 59; t.s = 59;
    roll("rtc_roll_12h_noon", &t, 1, chip == CHIP_RIC
         ? "12-hour mode: 11:59:59 AM -> 12:00:00 PM (H10 = PM|1, H1 = 2, or PM|0, H1 = 0: the RP5C01 sheet leaves the form open)"
         : "12-hour mode: 11:59:59 AM -> 12:00:00 PM (H10 = PM|1, H1 = 2)");
    t.h = 23; t.d = 14;
    roll("rtc_roll_12h_midnight", &t, 1, "12-hour mode: 11:59:59 PM -> 12:00:00 AM of the next day");
    clk_stop();
    set24(1);
}

/* MSM6242B 30-second adjust: CD bit 3 = 1 rounds the seconds to the
 * nearest minute (00-29 -> 00, 30-59 -> next minute) and clears itself */
static void adj(const char *id, UBYTE s, const char *why)
{
    struct rt t = T_A, e, got;
    UBYTE g[NDIG];
    if (!wr_ok) {
        amt_case(id, AMT_ST_UNTESTABLE, "depends on rtc_write_readback, which failed: the counters did not take the written time");
        return;
    }
    t.s = s;
    clk_stop();
    write_time(&t);
    clk_start();
    mh_wait_cck(mh_cck_ms(20));
    rw(13, CD_ADJ);
    mh_wait_cck(mh_cck_ms(5));
    read_time(&got, g);
    clk_stop();
    e = t;
    e.s = 0;
    if (s >= 30)
        add_secs(&e, 60);
    case_time(id, same(&got, &e, 0) && !(rr(13) & CD_ADJ), &e, &got, g, why, fmt_time(&t));
}

static void t_restore_check(void)
{
    UBYTE g[NDIG];
    struct rt e0, e1, e2, got;
    ULONG s;
    if (!sv.running) {
        amt_case("rtc_restored", AMT_ST_SKIP, "the clock was not counting at start; its digits were written back unchanged");
        return;
    }
    if (!wr_ok) {
        amt_case("rtc_restored", AMT_ST_UNTESTABLE,
                 "the counters ignore writes (see rtc_write_readback), so nothing was restored");
        return;
    }
    if (chip == CHIP_MSM)
        msm_unlock();
    if (!wait_edge(2500)) {
        case_edge_timeout("rtc_restored", "after restoring, the clock does not count");
        return;
    }
    s = mh_ms_since(sv.tk0) / 1000;
    read_time(&got, g);
    e0 = sv.t0;
    add_secs(&e0, s ? s - 1 : 0);
    e1 = e0; add_secs(&e1, 1);
    e2 = e1; add_secs(&e2, 1);
    if (same(&got, &e0, 0) || same(&got, &e1, 0) || same(&got, &e2, 0)) {
        amt_case("rtc_restored", AMT_ST_PASS, 0);
        return;
    }
    amt_case_open("rtc_restored", AMT_ST_FAIL);
    json_str("why", "the clock does not show the saved time plus the elapsed time (+-1 s): the machine is left with a wrong time");
    json_obj("expect");
    json_str("time", fmt_time(&e1));
    json_obj_end();
    json_obj("got");
    json_str("time", fmt_time(&got));
    json_obj_end();
    amt_case_close();
}

static void rtc_body(void)
{
    t_digits_counting();
    t_rate();
    if (chip == CHIP_MSM) {
        t_msm_ctrl();
        t_hold();
        t_busy();
    } else {
        t_ric_ram();
        t_ric_mode1();
    }
    t_stop();
    t_write_readback();
    t_rollovers();
    if (chip == CHIP_MSM) {
        adj("rtc_30s_adj_down", 20, "30-second adjust at 20 s: seconds -> 00, same minute");
        adj("rtc_30s_adj_up", 40, "30-second adjust at 40 s: seconds -> 00 of the next minute");
    }
}

void mh_rtc(void)
{
    char how[20];
    int code;
    chip = detect(how);
    if (chip == CHIP_NONE) {
        amt_case_open("rtc_present", AMT_ST_SKIP);
        json_str("why", "no MSM6242B or RP5C01 found at $DC0000 (read-only check: the 16 registers do not decode as a running clock)");
        amt_case_close();
        return;
    }
    ix = chip == CHIP_MSM ? msm_ix : ric_ix;
    pm_bit = chip == CHIP_MSM ? 4 : 2;
    amt_case_open("rtc_present", AMT_ST_PASS);
    json_obj("ctx");
    json_str("chip", chip == CHIP_MSM ? "MSM6242B" : "RP5C01");
    json_obj_end();
    amt_case_close();
    sv.have_t0 = sv.ram_saved = 0;
    code = amt_try(save_state);
    if (!code)
        code = amt_try(rtc_body);
    amt_try(restore_state);
    if (!code)
        amt_try(t_restore_check);
    /* the check reads the time (MSM: HOLD, RP5C01: mode 00): put the saved
     * control state back once more */
    if (sv.have_t0) {
        if (chip == CHIP_MSM)
            rw(13, sv.cd & CD_HOLD);
        else
            ric_mode(sv.md);
    }
    if (code) {
        amt_case_open("rtc_aborted", AMT_ST_FAIL);
        json_str("why", "unexpected exception in the RTC tests (the time was restored)");
        json_obj("got");
        json_int("vector", amt_last_exc.vector);
        json_hex("pc", amt_last_exc.pc);
        json_obj_end();
        amt_case_close();
    }
}
