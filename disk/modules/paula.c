/* paula: audio DMA/interrupts/modulation and the disk controller
 * (CONTRACT.md section 10, module id 14, bare phase, disk group "chip").
 *
 * Expectations come from the Amiga Hardware Reference Manual (HRM):
 *   chapter 5 "Audio hardware" (AUDxLC/LEN/PER/VOL/DAT, DMA and interrupt
 *   timing, double buffering, direct (non-DMA) output, ADKCON attach bits,
 *   minimum period, the audio state diagram: states 000 idle, 001/101 DMA
 *   start, 010/011 high/low byte output; "napnav", AUDxIP = INTREQ bit),
 *   chapter 6 "Blitter"/"DMA time slot allocation" (one audio DMA slot per
 *   channel per scan line), chapter 7 (INTREQ levels: AUDx = 4, DSKBLK = 1,
 *   DSKSYN = 5), chapter 8 "Disk controller" (DSKLEN double write, DSKPT,
 *   DSKSYNC + WORDSYNC, ADKCON disk bits, DSKBYTR, drive control lines on
 *   CIA-B PRB, status lines on CIA-A PRA, drive identification), appendix
 *   "8520 CIA" (CIA-A PRA bit 1 = power LED / audio filter), and the
 *   AmigaDOS/trackdisk MFM track format (11 sectors of 544 MFM words:
 *   $AAAA $AAAA $4489 $4489, odd/even encoded info long $FF/track/sector/
 *   sectors-to-gap, 16-byte label, header and data checksums = XOR of the
 *   MFM longs AND $55555555, 512 data bytes).
 *
 * Timing: interrupt times are logged by the level-4 handler in paula_asm.s
 * from VPOSR/VHPOSR + the runtime VBL counter, in half colour clocks (PAL
 * line 227 cck, NTSC lines alternate 227/228 -> 227.5). Measured values
 * go in "timing" objects only; pass/fail is on HRM bounds (normally +-1
 * sample period around the nominal value), so a record is the same in
 * every correct emulator/configuration, apart from "timing".
 *
 * Audio data and disk buffers are in the chip pool. The module restores
 * ADKCON, the audio registers it used (volume 0, DMA off, requests clear),
 * CIA-A PRA bit 1, and the drive lines (see paula_disk.c). The serial UART
 * is the output channel and is not tested (UARTBRK is never set).
 * With a uaesnd board present, uaesnd_mono_keeps_right claims it, plays a
 * silent mono stream next to a Paula tone, and stops the stream again. */
#include "paula.h"
#include "hardware_audit.h"
#include "uaesnd_hw.h"

AMT_MODULE(paula, 14, AMT_PH_BARE, AMT_MF_OWNS_DISPLAY, 28672, 0);

UBYTE *pa_chip;

/* ---- asm (paula_asm.s) */
struct pa_ent {
    ULONG frames;
    UWORD vposr, vhposr, intreqr, pend;
};
extern struct pa_ent pa_log[];
extern char pa_log_end[];
extern struct pa_ent *volatile pa_logp;
extern volatile UWORD pa_n, pa_stop_at, pa_feed_left;
extern volatile UWORD pa_sample_invalid;
extern const UWORD *volatile pa_feed_p;
extern volatile UWORD *volatile pa_feed_a;
extern volatile UWORD pa_act[16];
void pa_l4(void);
void pa_stamp(struct pa_ent *e);
void pa_stamp_write(struct pa_ent *e, volatile UWORD *reg, ULONG val);

/* ------------------------------------------------------------ helpers */
char *pa_cat(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    *d = 0;
    return d;
}

const char *pa_id(const char *a, int n, const char *b)
{
    static char buf[48];
    char *p = pa_cat(buf, a);
    p = amt_utoa(p, (ULONG)n);
    pa_cat(p, b);
    return buf;
}

static ULONG now_calc(void)
{
    ULONG f1, f2, v, h;
    UWORD rq;
    ULONG lines = amt_probe_result.pal ? 313 : 263;
    do {
        f1 = amt_frames_now();
        UWORD p1, hv, p2;
        rq = CREG(R_INTREQR);
        /* VPOSR (V8-V10) before and after VHPOSR: a single pair read across
         * line 255->256 gives line 0 (as in paula_asm.s) */
        do {
            p1 = CREG(R_VPOSR);
            hv = CREG(R_VHPOSR);
            p2 = CREG(R_VPOSR);
        } while ((p1 ^ p2) & 7);
        v = ((ULONG)(p1 & 7) << 8) | (hv >> 8);
        h = hv & 0xff;
        f2 = amt_frames_now();
    } while (f1 != f2);
    if ((rq & INT_VERTB) && v < lines / 2)
        f1++;
    return (f1 * lines + v) * 227UL + h;
}

ULONG pa_now_raw(void) { return now_calc(); }

ULONG pa_now(void)
{
    static ULONG last;
    ULONG t = now_calc();
    if ((LONG)(t - last) < 0)
        t = last;
    return last = t;
}

void pa_wait_cc(ULONG cc)
{
    ULONG t0 = pa_now();
    while (pa_now() - t0 < cc)
        ;
}

/* ---- interrupt-time base: half colour clocks since tb_f0 */
static ULONG tb_f0, tb_lines, tb_line2;

static void tb_init(void)
{
    tb_lines = amt_probe_result.pal ? 313 : 263;
    if (!(CREG(R_VPOSR) & 0x8000))
        tb_lines--;
    tb_line2 = amt_probe_result.pal ? 454 : 455;
    tb_f0 = amt_frames_now() - 1;
}

static ULONG ent_t2(const struct pa_ent *e)
{
    ULONG v = ((ULONG)(e->vposr & 7) << 8) | (e->vhposr >> 8);
    ULONG h = e->vhposr & 0xff;
    ULONG f = e->frames - tb_f0;
    if ((e->intreqr & INT_VERTB) && v < tb_lines / 2)
        f++;
    return (f * tb_lines + v) * tb_line2 + 2 * h;
}

static ULONG cck_per_frame(void) { return amt_probe_result.pal ? 71051UL : 59659UL; }

/* ------------------------------------------------------------ checks
 * One case = a list of bounded measurements. "in" lists the bounds (fixed),
 * "timing" the measured values (vary), a failure names the first bad one. */
struct chk {
    const char *what;
    UWORD a, b;           /* parameters, named by the case (pa_ka/pa_kb) */
    LONG got, lo, hi;
    int alternatives;
};
#define MAXCHK 40
static struct chk chks[MAXCHK];
static int nchk;
static const char *pa_ka, *pa_kb;

static void chk_begin(const char *ka, const char *kb)
{
    nchk = 0;
    pa_ka = ka;
    pa_kb = kb;
}

static void chk_add(const char *what, UWORD a, UWORD b, LONG got, LONG lo, LONG hi)
{
    if (nchk < MAXCHK) {
        struct chk *c = &chks[nchk++];
        c->what = what;
        c->a = a;
        c->b = b;
        c->got = got;
        c->lo = lo;
        c->hi = hi;
        c->alternatives = 0;
    }
}

static void chk_modulator(LONG got)
{
    chk_add("modulator_iv", 8, 400, got, 2800, 3600);
    chks[nchk - 1].alternatives = 1;
}

static void chk_fields(const struct chk *c, int with_got)
{
    json_str("what", c->what);
    if (pa_ka)
        json_int(pa_ka, c->a);
    if (pa_kb)
        json_int(pa_kb, c->b);
    json_int("lo", c->lo);
    json_int("hi", c->hi);
    if (c->alternatives) {
        json_int("alternative_lo", 6000);
        json_int("alternative_hi", 6800);
    }
    if (with_got)
        json_int("got", c->got);
}

static void chk_one(const struct chk *c)
{
    json_obj(NULL);
    chk_fields(c, 0);
    json_obj_end();
}

/* extra: optional callback adding fields to "in" */
static int chk_report(const char *id, const char *why, void (*extra)(void))
{
    int i, bad = -1;
    if (pa_sample_invalid) {
        amt_case(id, AMT_ST_UNTESTABLE, "bounded beam sampling could not obtain a coherent audio timestamp");
        return 0;
    }
    for (i = 0; i < nchk; i++)
        if (chks[i].alternatives ? !hw_modulator_period(chks[i].got) :
            (chks[i].got < chks[i].lo || chks[i].got > chks[i].hi)) {
            bad = i;
            break;
        }
    amt_case_open(id, bad < 0 ? AMT_ST_PASS : AMT_ST_FAIL);
    json_obj("in");
    json_str("unit", "cck");
    if (extra)
        extra();
    json_arr("bounds");
    for (i = 0; i < nchk; i++)
        chk_one(&chks[i]);
    json_arr_end();
    json_obj_end();
    json_obj("timing");
    json_arr("got");
    for (i = 0; i < nchk; i++)
        json_int(NULL, chks[i].got);
    json_arr_end();
    json_obj_end();
    if (bad >= 0) {
        json_obj("expect");
        json_str("what", chks[bad].what);
        json_int("lo", chks[bad].lo);
        json_int("hi", chks[bad].hi);
        json_obj_end();
        /* the measured value varies run to run: only whether it is below,
         * above or missing is fingerprinted; the value is under timing */
        json_obj("got");
        chk_fields(&chks[bad], 0);
        json_str("result", chks[bad].got < 0 ? "timeout" : chks[bad].got < chks[bad].lo ? "below" : "above");
        json_obj("timing");
        json_int("got", chks[bad].got);
        json_obj_end();
        json_obj_end();
        json_str("why", why);
        json_str("note", "result timeout = the interrupt never came; got.timing.got = the measured value");
    }
    amt_case_close();
    return bad < 0;
}

/* ------------------------------------------------------------ audio basics */
#define ZBUF_OFF 512          /* 512 zero bytes: silent sample data */
#define MODBUF_OFF 128        /* 64 words of modulation data */
#define TONE_OFF 0            /* 64 bytes of tone data */
#define FEED_OFF 256          /* manual-mode words */

static ULONG zbuf(void) { return (ULONG)(pa_chip + PA_AUDBUF_OFF + ZBUF_OFF); }
static UWORD *modbuf(void) { return (UWORD *)(pa_chip + PA_AUDBUF_OFF + MODBUF_OFF); }

static amt_vec_fn old28;

static void log_reset(void)
{
    int i;
    pa_logp = pa_log;
    pa_n = 0;
    pa_stop_at = 0;
    pa_feed_left = 0;
    for (i = 0; i < 16; i++)
        pa_act[i] = 0;
}

static void act_set(int i, UWORD n, volatile UWORD *reg, UWORD v)
{
    pa_act[i * 4 + 1] = (UWORD)((ULONG)reg >> 16);
    pa_act[i * 4 + 2] = (UWORD)(ULONG)reg;
    pa_act[i * 4 + 3] = v;
    pa_act[i * 4] = n;
}

static int nlog(void) { return (int)(pa_logp - pa_log); }

static int ch_count(int c)
{
    int i, n = 0;
    for (i = 0; i < nlog(); i++)
        if (pa_log[i].pend & INT_AUD(c))
            n++;
    return n;
}

/* entry times (cck since the test's time base) of channel c */
static int ch_times(int c, ULONG *t, int max)
{
    int i, n = 0;
    for (i = 0; i < nlog() && n < max; i++)
        if (pa_log[i].pend & INT_AUD(c))
            t[n++] = ent_t2(&pa_log[i]) / 2;
    return n;
}

static int wait_count(int c, int n, ULONG frames)
{
    ULONG f0 = amt_frames_now();
    while (ch_count(c) < n) {
        if (amt_frames_now() - f0 > frames)
            return 0;
        __asm__ volatile("stop #0x2000" ::: "memory");
    }
    return 1;
}

/* all channels idle, no requests, attach bits clear, volume 0 */
static void aud_quiet(void)
{
    int c;
    CREG(R_DMACON) = 0x000f;
    CREG(R_INTENA) = 0x0780;
    for (c = 0; c < 4; c++)
        AUDVOL(c) = 0;
    CREG(R_ADKCON) = 0x00ff;
    /* a channel whose DMA went off runs on in direct mode until it finds
     * its request pending (state diagram 011 -> 000): leave the requests
     * alone long enough for that (2 words of the longest period used) */
    pa_wait_cc(PA_MS(4));
    CREG(R_INTREQ) = 0x0780;
    CREG(R_INTREQ) = 0x0780;
}

static void ch_setup(int c, ULONG lc, UWORD len, UWORD per, UWORD vol)
{
    AUDLC(c) = lc;
    AUDLEN(c) = len;
    AUDPER(c) = per;
    AUDVOL(c) = vol;
}

static LONG iv(const ULONG *t, int n, int k)
{
    if (k + 1 >= n)
        return -1;
    return (LONG)(t[k + 1] - t[k]);
}

/* frames needed for `cc` colour clocks, with margin */
static ULONG fr_for(ULONG cc) { return cc / cck_per_frame() + 4; }

/* ------------------------------------------------------------ cases */
static void t_dmaconr(void)
{
    UWORD a, b;
    CREG(R_DMACON) = 0x0200;              /* DMAEN off: no DMA can happen */
    CREG(R_DMACON) = 0x800f;
    a = CREG(R_DMACONR) & 0x020f;
    CREG(R_DMACON) = 0x000f;
    b = CREG(R_DMACONR) & 0x020f;
    if (a == 0x000f && b == 0)
        amt_case("dmaconr_aud_bits", AMT_ST_PASS, 0);
    else {
        amt_case_open("dmaconr_aud_bits", AMT_ST_FAIL);
        json_obj("expect");
        json_hexn("after_set", 0x000f, 4);
        json_hexn("after_clr", 0, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("after_set", a, 4);
        json_hexn("after_clr", b, 4);
        json_obj_end();
        json_str("why", "DMACON SET/CLR of AUD0EN-AUD3EN (bits 0-3) must read back in DMACONR "
                 "(masked $020F, DMAEN kept off)");
        amt_case_close();
    }
}

static const struct { UWORD len, per; } dma_sets[3] = { { 16, 124 }, { 32, 200 }, { 64, 428 } };

static void t_dma_interval(int c, struct chk *startc, int *nstart)
{
    int s, s0 = AMT_QUICK() ? 1 : 0, s1 = AMT_QUICK() ? 2 : 3;
    ULONG t[8];
    struct pa_ent on;
    for (s = s0; s < s1; s++) {
        UWORD L = dma_sets[s].len, P = dma_sets[s].per;
        ULONG blk = 2UL * L * P;
        int n, k;
        aud_quiet();
        log_reset();
        tb_init();
        ch_setup(c, zbuf(), L, P, 0);
        CREG(R_INTENA) = 0x8000 | INT_AUD(c);
        pa_stamp_write(&on, &CREG(R_DMACON), 0x8200 | (1u << c));
        wait_count(c, 6, fr_for(7 * blk));
        CREG(R_DMACON) = 1u << c;
        CREG(R_INTENA) = INT_AUD(c);
        n = ch_times(c, t, 6);
        for (k = 1; k < 5; k++)
            chk_add(k == 1 ? "iv2" : k == 2 ? "iv3" : k == 3 ? "iv4" : "iv5", L, P,
                    iv(t, n, k), (LONG)(blk - P), (LONG)(blk + P));
        /* start: first interrupt after DMA on, first block */
        if (*nstart < 6) {
            struct chk *x = &startc[(*nstart)++];
            x->what = "first_irq";
            x->a = L;
            x->b = P;
            x->got = n > 0 ? (LONG)(t[0] - ent_t2(&on) / 2) : -1;
            x->lo = 0;
            x->hi = 3 * 228;
            x = &startc[(*nstart)++];
            x->what = "iv1";
            x->a = L;
            x->b = P;
            x->got = iv(t, n, 0);
            x->lo = (LONG)(2UL * (L - 1) * P - P);
            x->hi = (LONG)(blk + P);
        }
    }
    aud_quiet();
}

static void t_dma_all(void)
{
    static struct chk st[6];
    int c, ns, i;
    for (c = 0; c < 4; c++) {
        ns = 0;
        chk_begin("len", "per");
        t_dma_interval(c, st, &ns);
        chk_report(pa_id("aud", c, "_dma_interval"),
                   "audio DMA: after the start-up block, AUDx interrupts must come every "
                   "AUDxLEN words x 2 samples x AUDxPER colour clocks (HRM ch.5: the interrupt "
                   "marks the reload of LC/LEN at each block), +-1 sample period", 0);
        chk_begin("len", "per");
        for (i = 0; i < ns; i++)
            chk_add(st[i].what, st[i].a, st[i].b, st[i].got, st[i].lo, st[i].hi);
        chk_report(pa_id("aud", c, "_dma_start"),
                   "audio DMA start (HRM ch.5 state diagram 000->001->101): the first AUDx "
                   "interrupt comes when the first data word has been fetched, at most ~2 scan "
                   "lines after DMACON SET DMAEN|AUDxEN; the first block then lasts about "
                   "(LEN-1)..LEN words (two words are prefetched before output starts)", 0);
    }
}

/* LEN written in the handler applies to the block after the current one */
static void t_double_buffer(void)
{
    ULONG t[8];
    int n;
    const UWORD L1 = 32, L2 = 16, P = 200;
    aud_quiet();
    log_reset();
    tb_init();
    ch_setup(0, zbuf(), L1, P, 0);
    act_set(0, 3, &AUDLEN(0), L2);
    CREG(R_INTENA) = 0x8000 | INT_AUD(0);
    CREG(R_DMACON) = 0x8201;
    wait_count(0, 6, fr_for(8UL * 2 * L1 * P));
    CREG(R_DMACON) = 0x0001;
    n = ch_times(0, t, 6);
    aud_quiet();
    chk_begin("len", "per");
    chk_add("iv3_old_len", L1, P, iv(t, n, 2), 2L * L1 * P - P, 2L * L1 * P + P);
    chk_add("iv4_new_len", L2, P, iv(t, n, 3), 2L * L2 * P - P, 2L * L2 * P + P);
    chk_add("iv5_new_len", L2, P, iv(t, n, 4), 2L * L2 * P - P, 2L * L2 * P + P);
    chk_report("aud_double_buffer_len",
               "AUD0LEN written in the handler of interrupt 3 (32 -> 16 words) must not change "
               "the block in progress (its length was loaded at that block's start, which the "
               "interrupt signals) and must apply from the next block (HRM ch.5, double "
               "buffering: new LC/LEN written after the interrupt are used for the next block)", 0);
}

/* PER is not double-buffered: it applies from the next sample */
static void t_per_change(void)
{
    ULONG t[8];
    int n;
    const UWORD L = 32, Po = 200, Pn = 300;
    aud_quiet();
    log_reset();
    tb_init();
    ch_setup(1, zbuf(), L, Po, 0);
    act_set(0, 3, &AUDPER(1), Pn);
    CREG(R_INTENA) = 0x8000 | INT_AUD(1);
    CREG(R_DMACON) = 0x8202;
    wait_count(1, 6, fr_for(8UL * 2 * L * Pn));
    CREG(R_DMACON) = 0x0002;
    n = ch_times(1, t, 6);
    aud_quiet();
    chk_begin("len", "per");
    chk_add("iv3_changed", L, Pn, iv(t, n, 2), (LONG)Po + (2L * L - 1) * Pn - Pn,
            (LONG)Po + (2L * L - 1) * Pn + Pn);
    chk_add("iv4", L, Pn, iv(t, n, 3), 2L * L * Pn - Pn, 2L * L * Pn + Pn);
    chk_add("iv5", L, Pn, iv(t, n, 4), 2L * L * Pn - Pn, 2L * L * Pn + Pn);
    chk_report("aud_per_change",
               "AUD1PER written in the handler of interrupt 3 (200 -> 300) is used from the "
               "next sample on (the period counter reloads from AUDxPER at the end of every "
               "sample, HRM ch.5), so the block in progress already runs at the new period "
               "(1 old + 63 new samples), unlike LEN which waits for the next block", 0);
}

/* period below 124: the DMA can only fetch one word per channel per line */
static void t_min_period(void)
{
    static const UWORD pers[2] = { 64, 100 };
    ULONG t[8];
    int n, i, k;
    const UWORD L = 32;
    chk_begin("len", "per");
    for (i = 0; i < 2; i++) {
        UWORD P = pers[i];
        aud_quiet();
        log_reset();
        tb_init();
        ch_setup(2, zbuf(), L, P, 0);
        CREG(R_INTENA) = 0x8000 | INT_AUD(2);
        CREG(R_DMACON) = 0x8204;
        wait_count(2, 5, fr_for(6UL * L * 228));
        CREG(R_DMACON) = 0x0004;
        n = ch_times(2, t, 5);
        for (k = 1; k < 4; k++)
            chk_add(k == 1 ? "iv2" : k == 2 ? "iv3" : "iv4", L, P, iv(t, n, k),
                    (LONG)(L - 1) * 227, (LONG)(L + 1) * 228);
    }
    aud_quiet();
    chk_report("aud_min_period",
               "AUDxPER below the minimum of 124 (HRM ch.5): audio DMA gets one slot per channel "
               "per scan line (HRM DMA time slot allocation), so a channel can fetch at most "
               "one word (2 samples) per line; a block of LEN words then takes LEN lines "
               "(227.5 cck each), not LEN*2*PER", 0);
}

/* direct (non-DMA) output: one interrupt per data word */
static UWORD man_k;
static void man_extra(void) { json_int("words", man_k); }

static void t_manual(void)
{
    ULONG t[16];
    int n, k;
    const UWORD P = 300, K = 8;
    UWORD *feed = (UWORD *)(pa_chip + PA_AUDBUF_OFF + FEED_OFF);
    struct pa_ent on;
    int cnt, after;
    for (k = 0; k < K; k++)
        feed[k] = 0;
    aud_quiet();
    log_reset();
    tb_init();
    AUDPER(2) = P;
    AUDVOL(2) = 0;
    pa_feed_p = feed + 1;
    pa_feed_a = &AUDDAT(2);
    pa_feed_left = K - 1;
    pa_stop_at = K;
    CREG(R_INTENA) = 0x8000 | INT_AUD(2);
    pa_stamp_write(&on, &AUDDAT(2), feed[0]);
    wait_count(2, K, fr_for(4UL * K * P));
    pa_wait_cc(8UL * P);
    cnt = ch_count(2);
    n = ch_times(2, t, K);
    chk_begin("words", "per");
    chk_add("count", K, P, cnt, K, K);
    chk_add("first_irq", K, P, n > 0 ? (LONG)(t[0] - ent_t2(&on) / 2) : -1, 0, P);
    for (k = 0; k + 1 < K; k++)
        chk_add("iv", (UWORD)(k + 1), P, iv(t, n, k), 2L * P - P, 2L * P + P);
    man_k = K;
    chk_report("aud_manual_words",
               "direct mode (AUDxEN off, HRM ch.5 'playing without DMA'; state diagram "
               "000->010 on an AUDxDAT write with the request clear): the interrupt comes when "
               "the word is taken for output, at once for the first write, then every 2 samples "
               "(2*PER cck) while the handler writes the next word", man_extra);

    /* the last request was left pending (INTENA cleared in the handler):
     * the channel must go idle (011 -> 000: !AUDxON and AUDxIP) */
    CREG(R_INTREQ) = INT_AUD(2);
    CREG(R_INTREQ) = INT_AUD(2);
    CREG(R_INTENA) = 0x8000 | INT_AUD(2);
    pa_wait_cc(10UL * P);
    after = ch_count(2) - cnt;
    CREG(R_INTENA) = INT_AUD(2);
    aud_quiet();
    chk_begin("words", "per");
    chk_add("irqs_after_idle", K, P, after, 0, 0);
    chk_report("aud_manual_idle",
               "direct mode: when the word ends while the channel's request is still pending "
               "(AUDxIP), the channel goes idle (HRM ch.5 state diagram 011->000); clearing the "
               "request afterwards must not raise another interrupt", man_extra);
}

/* DMA off in the middle of a block: the channel continues in direct mode
 * (011 -> 010 while !AUDxIP), one interrupt per word, until a request is
 * left pending */
static void t_dma_off(void)
{
    ULONG t[16];
    int n, k, after, cnt;
    const UWORD L = 8, P = 300;
    aud_quiet();
    log_reset();
    tb_init();
    ch_setup(1, zbuf(), L, P, 0);
    pa_stop_at = 8;
    act_set(0, 3, &CREG(R_DMACON), 0x0002);   /* DMA off in the handler of irq 3 */
    CREG(R_INTENA) = 0x8000 | INT_AUD(1);
    CREG(R_DMACON) = 0x8202;
    wait_count(1, 8, fr_for(6UL * 2 * L * P));
    pa_wait_cc(8UL * P);
    cnt = ch_count(1);
    n = ch_times(1, t, 8);
    CREG(R_INTREQ) = INT_AUD(1);
    CREG(R_INTREQ) = INT_AUD(1);
    CREG(R_INTENA) = 0x8000 | INT_AUD(1);
    pa_wait_cc(10UL * P);
    after = ch_count(1) - cnt;
    CREG(R_INTENA) = INT_AUD(1);
    aud_quiet();
    chk_begin("len", "per");
    chk_add("count", L, P, cnt, 8, 8);
    chk_add("iv3_off", L, P, iv(t, n, 2), 2L * P - P, 2L * P + P);
    for (k = 3; k < 7; k++)
        chk_add("iv_direct", L, P, iv(t, n, k), 2L * P - P, 2L * P + P);
    chk_add("irqs_after_idle", L, P, after, 0, 0);
    chk_report("aud_dma_off_direct",
               "AUD1EN cleared in the handler of interrupt 3 (DMA block 8 words x 2 x 300; the "
               "interrupt comes as the block's last word starts): per the HRM ch.5 audio state "
               "diagram the channel then runs on in direct mode (011->010 while !AUDxON and the "
               "request is clear, with AUDxIR), so the next interrupt comes when the current "
               "word ends (2*PER after interrupt 3) and then one every word (2*PER) while the "
               "handler clears it; it goes idle (011->000) once interrupt 8 is left pending", 0);
}

/* ---- ADKCON attach (modulation) */
static UWORD mod_cfg[4];
static void mod_extra(void)
{
    json_hexn("adkcon", mod_cfg[0], 4);
    json_int("modulator", mod_cfg[1]);
    json_hexn("data0", mod_cfg[2], 4);
    json_hexn("data1", mod_cfg[3], 4);
}

/* run modulator m (8 words, PER 400) attached to m+1 (16 words, PER 150);
 * t0/t1: interrupt times of m and m+1 */
static void mod_run(int m, UWORD adk, UWORD d0, UWORD d1, ULONG *t0, int *n0, ULONG *t1, int *n1)
{
    UWORD *mb = modbuf();
    int i, c = m + 1;
    for (i = 0; i < 8; i++)
        mb[i] = (i & 1) ? d1 : d0;
    aud_quiet();
    log_reset();
    tb_init();
    ch_setup(m, (ULONG)mb, 8, 400, 0);
    if (c < 4)
        ch_setup(c, zbuf(), 16, 150, 0);
    CREG(R_ADKCON) = 0x8000 | adk;
    CREG(R_INTENA) = 0x8000 | INT_AUD(m) | (c < 4 ? INT_AUD(c) : 0);
    CREG(R_DMACON) = 0x8200 | (1u << m) | (c < 4 ? (1u << c) : 0);
    if (c < 4)
        wait_count(c, 6, fr_for(8UL * 2 * 16 * 400));
    wait_count(m, 6, fr_for(8UL * 2 * 8 * 400));
    CREG(R_DMACON) = 0x000f;
    *n0 = ch_times(m, t0, 6);
    *n1 = c < 4 ? ch_times(c, t1, 6) : 0;
    aud_quiet();
    mod_cfg[0] = adk;
    mod_cfg[1] = (UWORD)m;
    mod_cfg[2] = d0;
    mod_cfg[3] = d1;
}

static void t_modulation(void)
{
    ULONG t0[8], t1[8];
    int n0, n1, m, k;
    const UWORD Pm = 300;

    /* period modulation 0->1, 1->2, 2->3: the modulated channel plays at the
     * period given by the modulator's data words */
    for (m = 0; m < 3; m++) {
        mod_run(m, (UWORD)(0x10 << m), Pm, Pm, t0, &n0, t1, &n1);
        chk_begin("len", "per");
        for (k = 2; k < 5; k++)
            chk_add("modulated_iv", 16, Pm, iv(t1, n1, k), 2L * 16 * Pm - Pm, 2L * 16 * Pm + Pm);
        if (m == 0)
            for (k = 2; k < 5; k++)
                chk_modulator(iv(t0, n0, k));
        chk_report(m == 0 ? "adk_pmod_01" : m == 1 ? "adk_pmod_12" : "adk_pmod_23",
                   "ADKCON USExPy (attach period, HRM ch.5 'joining tracks'): the modulator's "
                   "data words go to the next channel's period register, so the modulated "
                   "channel (16 words, own PER 150) must run at the data period 300: "
                   "2*16*300 cck per block, +-1 period. The modulator's own block time is "
                   "accepted as 8*400 (one word per period, HRM text) or 2*8*400 (one DMA "
                   "word per 2 periods, HRM state diagram 'napnav')", mod_extra);
    }

    /* volume modulation only: the period of the modulated channel is unchanged */
    mod_run(0, 0x0001, 0x0020, 0x0020, t0, &n0, t1, &n1);
    chk_begin("len", "per");
    for (k = 2; k < 5; k++)
        chk_add("modulated_iv", 16, 150, iv(t1, n1, k), 2L * 16 * 150 - 150, 2L * 16 * 150 + 150);
    for (k = 2; k < 5; k++)
        chk_modulator(iv(t0, n0, k));
    chk_report("adk_vmod_01",
               "ADKCON USE0V1 (attach volume only): channel 1's period stays its own (150), "
               "2*16*150 cck per block; the modulator's block time is accepted as 8*400 or "
               "2*8*400 (see adk_pmod_01)", mod_extra);

    /* both: words alternate volume, period */
    mod_run(0, 0x0011, 0x0020, Pm, t0, &n0, t1, &n1);
    chk_begin("len", "per");
    for (k = 2; k < 5; k++)
        chk_add("modulated_iv", 16, Pm, iv(t1, n1, k), 2L * 16 * Pm - Pm, 2L * 16 * Pm + Pm);
    for (k = 2; k < 5; k++)
        chk_add("modulator_iv", 8, 400, iv(t0, n0, k), 8L * 400 - 400, 8L * 400 + 400);
    chk_report("adk_pvmod_01",
               "ADKCON USE0V1|USE0P1: the modulator's words alternate volume, period (HRM "
               "ch.5), one word per modulator period, so channel 0's 8-word block lasts "
               "8*400 cck and channel 1 plays at the period words (300): 2*16*300 per block",
               mod_extra);

    /* channel 3 attached to nothing */
    mod_run(3, 0x0080, 0x0000, 0x0000, t0, &n0, t1, &n1);
    chk_begin("len", "per");
    for (k = 2; k < 5; k++)
        chk_modulator(iv(t0, n0, k));
    chk_report("adk_use3pn",
               "ADKCON USE3PN: channel 3 modulates nothing (its output is off); its DMA and "
               "interrupts go on, block time accepted as 8*400 or 2*8*400 (see adk_pmod_01)",
               mod_extra);
}

/* AUDx request bit with the interrupt disabled: polled */
static void t_intreq_poll(void)
{
    struct pa_ent e[6];
    ULONG t[6];
    int n = 0, k;
    const UWORD L = 16, P = 200;
    ULONG f0, lim = fr_for(8UL * 2 * L * P);
    volatile UWORD *rq = &CREG(R_INTREQR);
    aud_quiet();
    tb_init();
    ch_setup(3, zbuf(), L, P, 0);
    CREG(R_DMACON) = 0x8208;
    f0 = amt_frames_now();
    while (n < 6 && amt_frames_now() - f0 < lim) {
        if (*rq & INT_AUD(3)) {
            pa_stamp(&e[n]);
            CREG(R_INTREQ) = INT_AUD(3);
            CREG(R_INTREQ) = INT_AUD(3);
            n++;
        }
    }
    for (k = 0; k < n; k++)
        t[k] = ent_t2(&e[k]) / 2;
    CREG(R_DMACON) = 0x0008;
    aud_quiet();
    chk_begin("len", "per");
    chk_add("requests", L, P, n, 6, 6);
    for (k = 1; k < 5; k++)
        chk_add("iv", L, P, iv(t, n, k), 2L * L * P - P, 2L * L * P + P);
    chk_report("aud_intreq_poll",
               "with INTENA AUD3 off, the AUD3 request bit (INTREQR bit 10) must still be "
               "set at every block start (HRM ch.7: INTREQ records requests regardless of "
               "INTENA), every 2*LEN*PER cck", 0);
}

/* ADKCON read back, every bit except UARTBRK (bit 11: the serial output) */
static void t_adkcon(void)
{
    static const UWORD pats[] = { 0x76ff, 0x5455, 0x22aa };
    int b, bad = -1;
    UWORD want = 0, got = 0;
    for (b = 0; b < 15 && bad < 0; b++) {
        UWORD m = (UWORD)(1u << b);
        if (b == 11)
            continue;
        CREG(R_ADKCON) = 0x77ff;
        CREG(R_ADKCON) = 0x8000 | m;
        got = CREG(R_ADKCONR) & 0x77ff;
        if (got != m) {
            bad = b;
            want = m;
            break;
        }
        CREG(R_ADKCON) = m;
        got = CREG(R_ADKCONR) & 0x77ff;
        if (got != 0) {
            bad = b;
            want = 0;
        }
    }
    for (b = 0; b < 3 && bad < 0; b++) {
        CREG(R_ADKCON) = 0x77ff;
        CREG(R_ADKCON) = 0x8000 | pats[b];
        got = CREG(R_ADKCONR) & 0x77ff;
        if (got != pats[b]) {
            bad = 100 + b;
            want = pats[b];
        }
    }
    CREG(R_ADKCON) = 0x77ff;
    if (bad < 0)
        amt_case("adkcon_readback", AMT_ST_PASS, 0);
    else {
        amt_case_open("adkcon_readback", AMT_ST_FAIL);
        json_obj("in");
        json_int("step", bad);
        json_str("step_note", "0-14: that bit set then cleared alone; 100+: pattern");
        json_obj_end();
        json_obj("expect");
        json_hexn("adkconr", want, 4);
        json_obj_end();
        json_obj("got");
        json_hexn("adkconr", got, 4);
        json_obj_end();
        json_str("why", "ADKCON (SET/CLR) bits 0-10 and 12-14 (audio attach USExVy/USExPy, FAST, "
                 "MSBSYNC, WORDSYNC, MFMPREC, PRECOMP0/1) must read back in ADKCONR, mask "
                 "$77FF (UARTBRK not tested: it would stop the serial output)");
        amt_case_close();
    }
}

/* CIA-A PRA bit 1: power LED / audio low-pass filter (0 = LED bright, filter on) */
static void t_led(void)
{
    UBYTE ddra = pa_cia_rd(CIAA, C_DDRA), pra0, a, b;
    if (!(ddra & 0x02)) {
        amt_case_open("filter_led", AMT_ST_UNTESTABLE);
        json_str("why", "CIA-A DDRA bit 1 is an input here (the OS sets $03); not changed");
        json_obj("got");
        json_hexn("ddra", ddra, 2);
        json_obj_end();
        amt_case_close();
        return;
    }
    pra0 = pa_cia_rd(CIAA, C_PRA);
    /* write bit 1 only: bit 0 (OVL) is written back as found, bits 2-7 are
     * inputs (the written value only goes to their output latch) */
    pa_cia_wr(CIAA, C_PRA, (UBYTE)(pra0 & ~0x02));
    a = pa_cia_rd(CIAA, C_PRA) & 0x02;
    pa_cia_wr(CIAA, C_PRA, (UBYTE)(pra0 | 0x02));
    b = pa_cia_rd(CIAA, C_PRA) & 0x02;
    pa_cia_wr(CIAA, C_PRA, pra0);
    if (a == 0 && b == 0x02)
        amt_case("filter_led", AMT_ST_PASS, 0);
    else {
        amt_case_open("filter_led", AMT_ST_FAIL);
        json_obj("expect");
        json_int("bit1_after_0", 0);
        json_int("bit1_after_1", 1);
        json_obj_end();
        json_obj("got");
        json_int("bit1_after_0", a ? 1 : 0);
        json_int("bit1_after_1", b ? 1 : 0);
        json_hexn("ddra", ddra, 2);
        json_obj_end();
        json_str("why", "CIA-A PRA bit 1 (/LED, audio filter) is an output (DDRA bit 1 = 1) and "
                 "must read back what was written (8520: an output pin reads its latch)");
        amt_case_close();
    }
}

/* ---- host-analysed tones (CONTRACT section 13, audio_mark) */
static const signed char sine32[32] = {
    0, 19, 37, 53, 68, 80, 89, 94, 96, 94, 89, 80, 68, 53, 37, 19,
    0, -19, -37, -53, -68, -80, -89, -94, -96, -94, -89, -80, -68, -53, -37, -19
};
static const struct { UWORD per, vol; UBYTE sine; const char *side; } tones[4] = {
    { 254, 64, 0, "left" }, { 226, 48, 1, "right" }, { 202, 32, 0, "right" }, { 170, 64, 1, "left" }
};

static char *jkv(char *p, const char *k, ULONG v, int last)
{
    p = pa_cat(p, "\"");
    p = pa_cat(p, k);
    p = pa_cat(p, "\":");
    p = amt_utoa(p, v);
    if (!last)
        p = pa_cat(p, ",");
    return p;
}

static void t_host_tones(void)
{
    static char args[320];
    int c, i, alive = 1;
    UBYTE *tb = pa_chip + PA_AUDBUF_OFF + TONE_OFF;
    ULONG clk = amt_cck_hz();
    for (c = 0; c < 4; c++) {
        const char *id = pa_id("host_tone_aud", c, "");
        static char idc[24];
        char *p;
        ULONG fm = (ULONG)(((unsigned long long)clk * 1000ULL) / ((ULONG)tones[c].per * 32UL));
        int a1 = 0, a2 = 0;
        pa_cat(idc, id);
        if (!alive) {
            amt_case(idc, AMT_ST_UNTESTABLE, "host did not act on request audio_mark");
            continue;
        }
        for (i = 0; i < 32; i++)
            tb[i] = tones[c].sine ? (UBYTE)sine32[i] : (UBYTE)(i < 16 ? 96 : -96);
        p = pa_cat(args, "{\"edge\":\"start\",");
        p = jkv(p, "ch", (ULONG)c, 0);
        p = jkv(p, "period", tones[c].per, 0);
        p = jkv(p, "vol", tones[c].vol, 0);
        p = jkv(p, "samples", 32, 0);
        p = jkv(p, "amp", 96, 0);
        p = jkv(p, "clock", clk, 0);
        p = jkv(p, "freq_mhz", fm, 0);
        p = jkv(p, "ms", 500, 0);
        p = pa_cat(p, "\"wave\":\"");
        p = pa_cat(p, tones[c].sine ? "sine" : "square");
        p = pa_cat(p, "\",\"side\":\"");
        p = pa_cat(p, tones[c].side);
        pa_cat(p, "\"}");
        aud_quiet();
        ch_setup(c, (ULONG)tb, 16, tones[c].per, tones[c].vol);
        a1 = amt_request("audio_mark", idc + 5, args, amt_secs(10));
        if (a1) {
            CREG(R_DMACON) = 0x8200 | (1u << c);
            pa_wait_cc(PA_MS(500));
            CREG(R_DMACON) = 1u << c;
            AUDVOL(c) = 0;
            p = pa_cat(args, "{\"edge\":\"stop\",\"expect_hz\":");
            p = amt_utoa(p, fm / 1000);
            *p++ = '.';
            *p++ = (char)('0' + (fm / 100) % 10);
            *p++ = (char)('0' + (fm / 10) % 10);
            *p++ = (char)('0' + fm % 10);
            pa_cat(p, ",\"tol_pct\":2}");
            a2 = amt_request("audio_mark", idc + 5, args, amt_secs(10));
        }
        aud_quiet();
        if (!a1 || !a2) {
            alive = 0;
            amt_case_open(idc, AMT_ST_UNTESTABLE);
            json_str("why", "host did not act on request audio_mark");
            json_int("start_acked", a1);
            amt_case_close();
            continue;
        }
        amt_case_open(idc, AMT_ST_PASS);
        json_obj("in");
        json_int("ch", c);
        json_int("period", tones[c].per);
        json_int("vol", tones[c].vol);
        json_str("wave", tones[c].sine ? "sine" : "square");
        json_int("samples_per_cycle", 32);
        json_int("freq_mhz", (long)fm);
        json_str("side", tones[c].side);
        json_obj_end();
        json_str("note", "informational: played for 0.5 s between audio_mark start/stop; the host "
                 "record of these requests has the analysis (frequency = clock/(PER*32), "
                 "channels 0+3 left, 1+2 right, amplitude ~ VOL/64)");
        amt_case_close();
    }
}

/* ---- a uaesnd mono stream must not take Paula off the right output.
 * A silent, endlessly repeating mono stream (35.B = 0: left and right) runs
 * on the uaesnd board while channel 1 (right output) plays a square tone
 * between audio_mark start/stop (side right): the runner judges the right
 * output as host.tone_uaesnd_mono_paula. Amiberry's mixer replaced the
 * right side with the left mix while a one-channel stream played. Needs a
 * uaesnd board in amt_probe_result.boards (claimed); skip without one.
 * Note: here the stream takes a mixer path on which the old bug does not
 * show (it passes on Amiberry bcea436dd too); in expsound's sequence it
 * failed. The case still checks that Paula keeps its side next to a mono
 * stream. */
#define UMONO_ID "uaesnd_mono_keeps_right"
static ULONG umono_base;             /* claimed board while the stream may run, else 0 */

static void umono_stop(void)
{
    ULONG t0;
    if (!umono_base)
        return;
    es_wl((volatile ULONG *)(umono_base + UAESND_STREAM0 + UAESND_S_SETPTR), 0);
    es_wl((volatile ULONG *)(umono_base + UAESND_ENABLE), 0);
    t0 = amt_frames_now();
    while ((es_rb((volatile UBYTE *)(umono_base + UAESND_STREAM0 + UAESND_S_STATUS)) & 1) &&
           amt_frames_now() - t0 < 25)
        ;
    umono_base = 0;
}

static void t_uaesnd_mono_mix(void)
{
    static char args[200];
    const struct amt_probe_board *bd = 0;
    UBYTE *tb = pa_chip + PA_AUDBUF_OFF + TONE_OFF;
    const UWORD per = 254, vol = 64;
    ULONG clk = amt_cck_hz(), fm, roff, rsize, ram, set, want;
    int k, a1 = 0, a2 = 0;
    char *p;
    for (k = 0; k < amt_probe_result.n_boards && k < AMT_PROBE_BOARDS_MAX; k++)
        if (amt_probe_result.boards[k].man == UAESND_MAN && amt_probe_result.boards[k].prod == UAESND_PROD) {
            bd = &amt_probe_result.boards[k];
            break;
        }
    if (!bd) {
        amt_case(UMONO_ID, AMT_ST_SKIP, "no uaesnd board");
        return;
    }
    if (!amt_board_ok(bd) || bd->size < 0x10000) {
        amt_case(UMONO_ID, AMT_ST_UNTESTABLE, "uaesnd board fails amt_board_ok or is smaller than its aperture");
        return;
    }
    if (!amt_board_claim(bd)) {
        amt_case(UMONO_ID, AMT_ST_UNTESTABLE, "no exclusive live ConfigDev claim of the uaesnd board");
        return;
    }
    roff = es_rl((volatile ULONG *)(bd->addr + UAESND_RAM_OFFSET));
    rsize = es_rl((volatile ULONG *)(bd->addr + UAESND_RAM_SIZE));
    want = uaesnd_ram_expected(bd->size);
    if (roff != want || rsize != want || roff + rsize > bd->size) {
        amt_case(UMONO_ID, AMT_ST_UNTESTABLE, "uaesnd RAM offset/size not as specified: not written");
        return;
    }
    ram = bd->addr + roff;
    for (k = 0; k < 2205; k++)
        es_wb((volatile UBYTE *)(ram + 0x3000 + k), 0);
    set = uaesnd_set_write(ram, 0x400, ram + 0x3000, 2205, 22050UL << 8, ram + 0x3000, 2205, 0xffff, 0, 0);
    umono_base = bd->addr;
    es_wl((volatile ULONG *)(bd->addr + UAESND_ENABLE), 1);
    es_wl((volatile ULONG *)(bd->addr + UAESND_STREAM0 + UAESND_S_SETPTR), set);
    pa_wait_cc(PA_MS(100));

    for (k = 0; k < 32; k++)
        tb[k] = (UBYTE)(k < 16 ? 96 : -96);
    fm = (ULONG)(((unsigned long long)clk * 1000ULL) / ((ULONG)per * 32UL));
    p = pa_cat(args, "{\"edge\":\"start\",");
    p = jkv(p, "ch", 1, 0);
    p = jkv(p, "period", per, 0);
    p = jkv(p, "vol", vol, 0);
    p = jkv(p, "samples", 32, 0);
    p = jkv(p, "clock", clk, 0);
    p = jkv(p, "freq_mhz", fm, 0);
    p = jkv(p, "tol_pct", 2, 0);
    p = jkv(p, "ms", 500, 0);
    pa_cat(p, "\"wave\":\"square\",\"side\":\"right\"}");
    aud_quiet();
    ch_setup(1, (ULONG)tb, 16, per, vol);
    a1 = amt_request("audio_mark", "uaesnd_mono_paula", args, amt_secs(10));
    if (a1) {
        CREG(R_DMACON) = 0x8202;              /* DMAEN | AUD1EN */
        pa_wait_cc(PA_MS(500));
        CREG(R_DMACON) = 0x0002;
        AUDVOL(1) = 0;
        a2 = amt_request("audio_mark", "uaesnd_mono_paula", "{\"edge\":\"stop\"}", amt_secs(10));
    }
    aud_quiet();
    umono_stop();
    if (!a1 || !a2) {
        amt_case_open(UMONO_ID, AMT_ST_UNTESTABLE);
        json_str("why", "host did not act on request audio_mark");
        json_int("start_acked", a1);
        amt_case_close();
        return;
    }
    amt_case_open(UMONO_ID, AMT_ST_PASS);
    json_obj("in");
    json_hex("board", bd->addr);
    json_str("stream", "silent 8-bit signed mono, repeat -1, 35.B = 0 (left and right)");
    json_int("ch", 1);
    json_int("period", per);
    json_int("freq_mhz", (long)fm);
    json_str("side", "right");
    json_obj_end();
    json_str("note", "informational: the runner judges the capture as host.tone_uaesnd_mono_paula "
             "(the Paula tone must be on the right output while the mono stream plays)");
    amt_case_close();
}

void pa_audio_tests(void)
{
    old28 = amt_vec_set(28, pa_l4);
    t_dmaconr();
    t_adkcon();
    t_dma_all();
    t_double_buffer();
    t_per_change();
    t_min_period();
    t_manual();
    t_dma_off();
    t_modulation();
    t_intreq_poll();
    t_led();
    aud_quiet();
    amt_vec_set(28, old28);
}

/* Each part runs under amt_try, so an unexpected exception, a watchdog
 * expiry or an amt_abort still gets the hardware restored: audio quiet,
 * and for the disk part pa_disk_restore() (DSKLEN off, head back, motors
 * off, drives deselected, PRB as found). After an abort the remaining
 * parts are not run; the abort is passed on to the runtime at the end. */
void amt_run_paula(void)
{
    pa_sample_invalid = 0;
    UWORD adk0 = CREG(R_ADKCONR);
    int i, code;
    pa_chip = amt_chip_pool();
    for (i = 0; i < 1024; i++)
        pa_chip[PA_AUDBUF_OFF + i] = 0;
    old28 = 0;
    code = amt_try(pa_audio_tests);
    aud_quiet();
    if (code && old28)
        amt_vec_set(28, old28);
    if (!code)
        code = amt_try(pa_disk_tests);
    pa_disk_restore();
    if (!code) {
        old28 = amt_vec_set(28, pa_l4);
        code = amt_try(t_host_tones);
        amt_vec_set(28, old28);
    }
    aud_quiet();
    if (!code)
        code = amt_try(t_uaesnd_mono_mix);
    aud_quiet();
    umono_stop();
    CREG(R_ADKCON) = 0x7fff;
    CREG(R_ADKCON) = 0x8000 | (adk0 & 0x7fff);
    if (code) {
        amt_watchdog_kick();
        amt_abort(code);
    }
}
