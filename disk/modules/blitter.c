/* blitter: area and line blits checked against a host model of the HRM
 * (Amiga Hardware Reference Manual, chapter 6). The cases and their expected
 * destination CRCs come from tools/gen_blitter.py (blitter_tab.c); see
 * docs/cases/blitter.md. A failing case can be expanded on the host with
 *     tools/gen_blitter.py --dump <case>
 * which prints the registers, the fill seeds and the expected window. */
#include "mod.h"
#include "blitter.h"

AMT_MODULE(blitter, 8, AMT_PH_BARE, AMT_MF_OWNS_DISPLAY, 131072, 0);

#define CUST(o)  (*(volatile UWORD *)(0xdff000UL + (o)))
#define CUSTL(o) (*(volatile ULONG *)(0xdff000UL + (o)))
#define BLTCON0  0x040
#define BLTCON1  0x042
#define BLTAFWM  0x044
#define BLTALWM  0x046
#define BLTCPT   0x048
#define BLTBPT   0x04c
#define BLTAPT   0x050
#define BLTDPT   0x054
#define BLTSIZE  0x058
#define BLTCON0L 0x05a
#define BLTSIZV  0x05c
#define BLTSIZH  0x05e
#define BLTCMOD  0x060
#define BLTBMOD  0x062
#define BLTAMOD  0x064
#define BLTDMOD  0x066
#define BLTCDAT  0x070
#define BLTBDAT  0x072
#define BLTADAT  0x074

#define DMAF_BBUSY  0x4000
#define DMAF_BZERO  0x2000
#define DMA_ON      0x8240     /* SET | DMAEN | BLTEN */
#define DMA_OFF     0x0640     /* BLTPRI | DMAEN | BLTEN */
#define DMAF_BLTPRI 0x0400
#define INTF_BLIT   0x0040

#define USEA 0x0800
#define USEB 0x0400
#define USEC 0x0200

#define WAIT_FRAMES 100        /* ~2 s: far longer than any blit here */

static UBYTE *pool;
static int dead;               /* a blit never finished: the rest is untestable */

/* ------------------------------------------------------------ helpers */
static void fill_region(const struct blt_fill *f)
{
    volatile UWORD *p = (volatile UWORD *)(pool + f->off);
    ULONG n = f->words, x = f->seed;
    if (x & 1) {
        while (n--) {
            x ^= x << 13;
            x ^= x >> 17;
            x ^= x << 5;
            *p++ = (UWORD)(x >> 16);
        }
    } else {
        UWORD v = (UWORD)(x >> 16);
        while (n--)
            *p++ = v;
    }
}

static int str_eq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static const struct blt_case *find_case(const char *id)
{
    int i;
    for (i = 0; i < blt_ncases; i++)
        if (str_eq(blt_cases[i].id, id))
            return &blt_cases[i];
    return 0;
}

/* HRM: test BBUSY twice (the first DMACONR read after starting a blit may
 * not show it yet on early Agnus). Returns 0 when done, -1 on timeout. */
static int blt_wait(void)
{
    ULONG f0 = amt_frames_now();
    (void)CUST(R_DMACONR);
    while (CUST(R_DMACONR) & DMAF_BBUSY)
        if (amt_frames_now() - f0 > WAIT_FRAMES)
            return -1;
    return 0;
}

/* A 1x1 blit with A and B from a zero word: afterwards the A and B
 * "previous word" holding registers are zero, whether the hardware clears
 * them at the start of a blit or keeps them from the last one. */
static int prime(void)
{
    ULONG base = (ULONG)pool;
    *(volatile ULONG *)pool = 0;
    CUST(R_DMACON) = DMA_ON;
    CUST(BLTCON0) = 0x0d00;
    CUST(BLTCON1) = 0;
    CUST(BLTAFWM) = 0xffff;
    CUST(BLTALWM) = 0xffff;
    CUSTL(BLTAPT) = base;
    CUSTL(BLTBPT) = base;
    CUSTL(BLTDPT) = base + 16;
    CUST(BLTAMOD) = 0;
    CUST(BLTBMOD) = 0;
    CUST(BLTDMOD) = 0;
    CUST(BLTSIZE) = (1 << 6) | 1;
    return blt_wait();
}

static int setup(const struct blt_case *c)
{
    UWORD i;
    for (i = 0; i < c->nfill; i++)
        fill_region(&blt_fills[c->fill0 + i]);
    return prime();
}

/* program the registers and start; DMA must be on */
static void go(const struct blt_case *c)
{
    ULONG base = (ULONG)pool;
    CUST(BLTCON0) = c->con0;
    CUST(BLTCON1) = c->con1;
    if (c->flags & BF_CON0L)
        CUST(BLTCON0L) = c->aux;
    CUST(BLTAFWM) = c->afwm;
    CUST(BLTALWM) = c->alwm;
    CUSTL(BLTAPT) = (c->flags & BF_LINE) ? c->apt : base + c->apt;
    CUSTL(BLTBPT) = base + c->bpt;
    CUSTL(BLTCPT) = base + c->cpt;
    CUSTL(BLTDPT) = base + c->dpt;
    CUST(BLTAMOD) = (UWORD)c->amod;
    CUST(BLTBMOD) = (UWORD)c->bmod;
    CUST(BLTCMOD) = (UWORD)c->cmod;
    CUST(BLTDMOD) = (UWORD)c->dmod;
    /* data registers only for channels without DMA (and BLTADAT in line
     * mode), after BLTCON1 because BLTBDAT is shifted when written */
    if (!(c->con0 & USEA) || (c->flags & BF_LINE))
        CUST(BLTADAT) = c->adat;
    if (!(c->con0 & USEB))
        CUST(BLTBDAT) = c->bdat;
    if (!(c->con0 & USEC))
        CUST(BLTCDAT) = c->cdat;
    if (c->flags & BF_BIG) {
        CUST(BLTSIZV) = c->sizv;
        CUST(BLTSIZH) = c->sizh;      /* starts the blit */
    } else {
        CUST(BLTSIZE) = c->size;      /* starts the blit */
    }
}

struct result {
    ULONG crc;
    UBYTE zero;
    UBYTE timeout;
};

static void finish(const struct blt_case *c, struct result *r)
{
    r->timeout = blt_wait() ? 1 : 0;
    r->zero = (CUST(R_DMACONR) & DMAF_BZERO) ? 1 : 0;
    CUST(R_DMACON) = DMA_OFF;
    r->crc = crc32_update(0, pool + c->win_off, c->win_len);
}

static void run_case(const struct blt_case *c, UWORD dmaextra, struct result *r)
{
    if (setup(c)) {
        r->timeout = 1;
        CUST(R_DMACON) = DMA_OFF;
        return;
    }
    CUST(R_DMACON) = DMA_ON | dmaextra;
    go(c);
    finish(c, r);
}

/* ------------------------------------------------------------ reporting */
static const char *why_for(const char *id)
{
    static const struct { const char *pfx, *why; } t[] = {
        { "minterm_", "D is not the BLTCON0 LF minterm of A, B and C (HRM 6: logic functions)" },
        { "shift_", "barrel shifter: A/B shifted by ASH/BSH with the bits of the previous word "
                    "(of the previous line at a line start) shifted in; left in descending mode (HRM 6: shifting)" },
        { "mask_", "BLTAFWM/BLTALWM not ANDed with the first/last A word of each line before the shift (HRM 6: masking)" },
        { "mod_", "modulos not added (ascending) or subtracted (descending) at the end of each line (HRM 6: modulos)" },
        { "use_", "channel enables: a disabled channel must use its preloaded data register, D off must not write (HRM 6)" },
        { "zero_", "destination / BZERO wrong for a blit that tests the zero flag (HRM 6: DMACONR BZERO)" },
        { "fill_", "area fill: inclusive/exclusive fill with FCI per line, right to left in descending mode (HRM 6: area fill)" },
        { "line_", "line mode: octant stepping by SIGN of the BLTAPTL error term, SING, texture (HRM 6: line mode)" },
        { "size_", "BLTSIZE: width 0 = 64 words, height 0 = 1024 lines (HRM 6: BLTSIZE)" },
        { "big_", "ECS BLTSIZV/BLTSIZH big blit (HRM appendix C: ECS blitter)" },
        { "ecs_", "ECS BLTCON0L must replace the minterm byte of BLTCON0 (HRM appendix C)" },
        { "hot_", "repeating the same blit gave a different destination (CPU fill/CRC path under the JIT or the blit itself)" },
        { "nasty_", "BLTPRI (blitter-nasty) must not change the result of a blit (HRM 6: blitter priority)" },
        { "irq_", "the blit itself was wrong in the interrupt test" },
    };
    unsigned i;
    for (i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
        const char *p = t[i].pfx, *s = id;
        while (*p && *p == *s)
            p++, s++;
        if (!*p)
            return t[i].why;
    }
    return "destination window differs from the HRM model";
}

static void emit_inputs(const struct blt_case *c)
{
    json_obj("in");
    json_hexn("bltcon0", c->con0, 4);
    json_hexn("bltcon1", c->con1, 4);
    if (c->flags & BF_CON0L)
        json_hexn("bltcon0l", c->aux, 4);
    json_hexn("afwm", c->afwm, 4);
    json_hexn("alwm", c->alwm, 4);
    json_hexn("adat", c->adat, 4);
    json_hexn("bdat", c->bdat, 4);
    json_hexn("cdat", c->cdat, 4);
    json_int("amod", c->amod);
    json_int("bmod", c->bmod);
    json_int("cmod", c->cmod);
    json_int("dmod", c->dmod);
    json_hexn((c->flags & BF_LINE) ? "aptl" : "apt", c->apt, 5);
    json_hexn("bpt", c->bpt, 5);
    json_hexn("cpt", c->cpt, 5);
    json_hexn("dpt", c->dpt, 5);
    if (c->flags & BF_BIG) {
        json_hexn("bltsizv", c->sizv, 4);
        json_hexn("bltsizh", c->sizh, 4);
    } else {
        json_hexn("bltsize", c->size, 4);
    }
    json_int("w", c->w);
    json_int("h", c->h);
    json_hexn("win", c->win_off, 5);
    json_int("win_len", (long)c->win_len);
    json_obj_end();
}

static void emit_ctx(const struct blt_case *c, ULONG pool_base)
{
    int i;
    const volatile UWORD *d = (const volatile UWORD *)(pool + (c->dpt & ~1UL));
    json_obj("ctx");
    json_str("pt_note", "pointers are offsets into the chip pool");
    json_hex("pool", pool_base);
    json_str("host", "tools/gen_blitter.py --dump <case> prints the expected window");
    json_arr("d_words_got");
    for (i = 0; i < 8; i++)
        json_hexn(NULL, d[i], 4);
    json_arr_end();
    json_obj_end();
}

static void report(const char *id, const struct blt_case *c, const struct result *r,
                   const char *why)
{
    int zero_ok = (c->zero == 2) || (c->zero == r->zero);
    if (r->timeout) {
        dead = 1;
        CUST(R_DMACON) = DMA_OFF;
        amt_case_open(id, AMT_ST_FAIL);
        json_obj("expect");
        json_str("bbusy", "clear within 2 s");
        json_obj_end();
        json_obj("got");
        json_str("bbusy", "still set");
        json_obj_end();
        json_str("why", "blit never finished: DMACONR BBUSY stayed set (HRM 6)");
        emit_inputs(c);
        amt_case_close();
        return;
    }
    if (r->crc == c->crc && zero_ok) {
        amt_case(id, AMT_ST_PASS, 0);
        return;
    }
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_hex("crc", c->crc);
    if (c->zero != 2)
        json_int("bzero", c->zero);
    json_obj_end();
    json_obj("got");
    json_hex("crc", r->crc);
    if (c->zero != 2)
        json_int("bzero", r->zero);
    json_obj_end();
    if (r->crc == c->crc)
        json_str("why", "destination correct but DMACONR BZERO wrong: it must be 1 exactly when "
                        "every D word the blit produced was zero, D enabled or not (HRM 6)");
    else
        json_str("why", why ? why : why_for(id));
    emit_inputs(c);
    emit_ctx(c, (ULONG)pool);
    amt_case_close();
}

static int untestable(const char *id)
{
    if (!dead)
        return 0;
    amt_case(id, AMT_ST_UNTESTABLE, "an earlier blit never finished (BBUSY stuck)");
    return 1;
}

static int needs_ecs(const char *id, const struct blt_case *c)
{
    if (!(c->flags & BF_ECS) || amt_probe_result.ecs_agnus)
        return 0;
    amt_case(id, AMT_ST_SKIP, "OCS Agnus has no BLTSIZV/BLTSIZH/BLTCON0L (HRM appendix C: ECS only)");
    return 1;
}

/* ------------------------------------------------------------ hot */
static const struct blt_case *hot_case;

static ULONG hot_fn(ULONG i)
{
    struct result r;
    (void)i;
    run_case(hot_case, 0, &r);
    if (r.timeout) {
        dead = 1;
        return 0xdeadbeefUL;
    }
    return r.crc ^ ((ULONG)r.zero << 31);
}

static void run_hot(const struct blt_case *c)
{
    char id[48];
    char *p = id;
    const char *s = "hot_";
    ULONG first = 0, bad = 0, reps, expect;
    long it;
    while (*s)
        *p++ = *s++;
    for (s = c->id; *s && p < id + sizeof(id) - 1; )
        *p++ = *s++;
    *p = 0;
    if (untestable(id))
        return;
    reps = amt_hot_reps() / 4;
    hot_case = c;
    it = amt_hot(hot_fn, reps, &first, &bad);
    expect = c->crc ^ ((c->zero == 1) ? 0x80000000UL : 0);
    if (it < 0 && first == expect) {
        amt_case(id, AMT_ST_PASS, 0);
        return;
    }
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_hex("crc_bz", expect);
    json_obj_end();
    json_obj("got");
    json_hex("first", first);
    if (it >= 0) {
        json_hex("diverged", bad);
        json_int("iteration", it);
    }
    json_obj_end();
    json_str("why", it >= 0 ? "the same blit repeated under amt_hot() gave a different "
                              "destination CRC/BZERO (bit 31 = BZERO) on a later iteration"
                            : "the blit under amt_hot() was consistent but wrong");
    json_int("reps", (long)reps);
    emit_inputs(c);
    amt_case_close();
}

/* ------------------------------------------------------------ special */
static void test_nasty(void)
{
    const struct blt_case *c = find_case("nasty_base");
    struct result r;
    if (!untestable("nasty_off")) {
        run_case(c, 0, &r);
        report("nasty_off", c, &r, 0);
    }
    if (!untestable("nasty_on")) {
        run_case(c, DMAF_BLTPRI, &r);
        report("nasty_on", c, &r, 0);
    }
}

static volatile ULONG irq_count, irq_req;

static void blit_hook(ULONG ir)
{
    if (ir & INTF_BLIT) {
        irq_count++;
        irq_req = ir;
        CUST(R_INTREQ) = INTF_BLIT;
        CUST(R_INTREQ) = INTF_BLIT;
    }
}

static void test_irq(void)
{
    const struct blt_case *c = find_case("irq_base");
    struct result r;
    UWORD before, after;
    ULONG f0;
    int i;

    /* polled: INTREQR BLIT is set when the blit finishes (INTENA off) */
    if (!untestable("irq_intreq_set")) {
        CUST(R_INTENA) = INTF_BLIT;
        if (setup(c)) {
            r.timeout = 1;
            report("irq_intreq_set", c, &r, 0);
            return;
        }
        CUST(R_INTREQ) = INTF_BLIT;
        CUST(R_INTREQ) = INTF_BLIT;
        before = CUST(R_INTREQR) & INTF_BLIT;
        CUST(R_DMACON) = DMA_ON;
        go(c);
        r.timeout = blt_wait() ? 1 : 0;
        for (i = 0; i < 64; i++)          /* allow the request to land */
            (void)CUST(R_DMACONR);
        after = CUST(R_INTREQR) & INTF_BLIT;
        r.zero = (CUST(R_DMACONR) & DMAF_BZERO) ? 1 : 0;
        CUST(R_DMACON) = DMA_OFF;
        CUST(R_INTREQ) = INTF_BLIT;
        r.crc = crc32_update(0, pool + c->win_off, c->win_len);
        if (!r.timeout && (before || !after)) {
            amt_case_open("irq_intreq_set", AMT_ST_FAIL);
            json_obj("expect");
            json_int("before", 0);
            json_int("after", 1);
            json_obj_end();
            json_obj("got");
            json_int("before", before ? 1 : 0);
            json_int("after", after ? 1 : 0);
            json_obj_end();
            json_str("why", "INTREQR bit 6 (BLIT) must be clear after INTREQ clear and set when "
                            "the blit finishes, even with INTENA BLIT off (HRM 6/7: blitter done interrupt)");
            emit_inputs(c);
            amt_case_close();
        } else {
            report("irq_intreq_set", c, &r, 0);
        }
    }

    /* level 3 interrupt through amt_irq_hook[3], exactly once per blit */
    if (!untestable("irq_level3")) {
        irq_count = 0;
        irq_req = 0;
        amt_irq_hook[3] = blit_hook;
        if (setup(c)) {
            r.timeout = 1;
            amt_irq_hook[3] = 0;
            report("irq_level3", c, &r, 0);
            return;
        }
        CUST(R_INTREQ) = INTF_BLIT;
        CUST(R_INTREQ) = INTF_BLIT;
        CUST(R_INTENA) = 0x8000 | INTF_BLIT;
        CUST(R_DMACON) = DMA_ON;
        go(c);
        f0 = amt_frames_now();
        while (!irq_count && amt_frames_now() - f0 < WAIT_FRAMES)
            ;
        finish(c, &r);
        f0 = amt_frames_now();
        while (amt_frames_now() - f0 < 2)    /* a second interrupt would land here */
            ;
        CUST(R_INTENA) = INTF_BLIT;
        CUST(R_INTREQ) = INTF_BLIT;
        amt_irq_hook[3] = 0;
        if (!r.timeout && (irq_count != 1 || !(irq_req & INTF_BLIT))) {
            amt_case_open("irq_level3", AMT_ST_FAIL);
            json_obj("expect");
            json_int("count", 1);
            json_hexn("intreq_blit", INTF_BLIT, 4);
            json_obj_end();
            json_obj("got");
            json_int("count", (long)irq_count);
            json_hexn("intreq_blit", irq_req & INTF_BLIT, 4);
            json_obj_end();
            json_str("why", "with INTENA INTEN|BLIT a finished blit must raise exactly one level-3 "
                            "interrupt with INTREQR BLIT set (HRM 6/7: BLIT, level 3)");
            emit_inputs(c);
            amt_case_close();
        } else {
            report("irq_level3", c, &r, 0);
        }
    }
}

static void test_bbusy(void)
{
    const struct blt_case *c = find_case("bbusy_base");
    struct result r;
    UWORD idle, r1, r2, d1, d2;
    ULONG t0, t1, f0;
    int i;

    if (untestable("bbusy_idle_clear") | untestable("bbusy_set_while_running") |
        untestable("bbusy_clear_when_done"))
        return;
    if (setup(c)) {
        r.timeout = 1;
        report("bbusy_idle_clear", c, &r, 0);
        return;
    }
    CUST(R_DMACON) = DMA_ON;
    idle = CUST(R_DMACONR) & DMAF_BBUSY;
    go(c);
    t0 = amt_ticks();
    r1 = CUST(R_DMACONR) & DMAF_BBUSY;
    r2 = CUST(R_DMACONR) & DMAF_BBUSY;
    f0 = amt_frames_now();
    r.timeout = 0;
    while (CUST(R_DMACONR) & DMAF_BBUSY)
        if (amt_frames_now() - f0 > WAIT_FRAMES) {
            r.timeout = 1;
            break;
        }
    t1 = amt_ticks();
    d1 = CUST(R_DMACONR) & DMAF_BBUSY;
    for (i = 0; i < 200; i++)
        (void)CUST(R_DMACONR);
    d2 = CUST(R_DMACONR) & DMAF_BBUSY;
    r.zero = (CUST(R_DMACONR) & DMAF_BZERO) ? 1 : 0;
    CUST(R_DMACON) = DMA_OFF;
    r.crc = crc32_update(0, pool + c->win_off, c->win_len);
    (void)r1;

    amt_check32("bbusy_idle_clear", "bbusy", 0, idle ? 1 : 0,
                "DMACONR BBUSY must be clear with no blit pending (HRM 6: BBUSY)");
    amt_check32("bbusy_set_while_running", "bbusy_second_read", 1, r2 ? 1 : 0,
                "a 64x64-word ABCD blit (~16000 colour clocks) must still be busy on the "
                "second DMACONR read after the BLTSIZE write (HRM 6: test BBUSY twice)");
    if (r.timeout) {
        report("bbusy_clear_when_done", c, &r, 0);
    } else if (d1 || d2) {
        amt_case_open("bbusy_clear_when_done", AMT_ST_FAIL);
        json_obj("expect");
        json_int("after", 0);
        json_int("later", 0);
        json_obj_end();
        json_obj("got");
        json_int("after", d1 ? 1 : 0);
        json_int("later", d2 ? 1 : 0);
        json_obj_end();
        json_str("why", "BBUSY must stay clear once the blit has finished (HRM 6)");
        amt_case_close();
    } else {
        report("bbusy_clear_when_done", c, &r,
               "the blit polled through BBUSY produced a wrong destination");
    }
    if (!r.timeout)
        amt_perf("blit_64x64_abcd", 4096, t1 - t0);
}

/* ------------------------------------------------------------ body */
void amt_run_blitter(void)
{
    int i;
    struct result r;
    pool = amt_chip_pool();
    dead = 0;

    for (i = 0; i < blt_ncases; i++) {
        const struct blt_case *c = &blt_cases[i];
        if (c->flags & BF_SPECIAL)
            continue;
        if (untestable(c->id) || needs_ecs(c->id, c))
            continue;
        run_case(c, 0, &r);
        report(c->id, c, &r, 0);
    }
    for (i = 0; i < blt_ncases; i++)
        if (blt_cases[i].flags & BF_HOT)
            run_hot(&blt_cases[i]);
    test_nasty();
    test_irq();
    test_bbusy();

    /* leave the blitter idle with DMA off */
    CUST(R_DMACON) = DMA_OFF;
    CUST(R_INTENA) = INTF_BLIT;
    CUST(R_INTREQ) = INTF_BLIT;
}
