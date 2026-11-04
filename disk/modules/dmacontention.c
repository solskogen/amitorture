/* dmacontention (module 23): the CPU running from chip RAM while other DMA
 * channels run at full load.
 *
 * Each case runs one DMA scenario, then a fixed integer workload (dc_loop,
 * copied into the chip pool so that its instruction fetches and data
 * accesses all use the chip bus) and checks:
 *   - the workload result (computed on the host for DC_ITERS iterations):
 *     DMA must be invisible to the CPU's results;
 *   - blitter: every blit restarted from the BLIT interrupt completed, each
 *     raised exactly one interrupt, and the destination equals the minterm
 *     D = A^B^C of the sources (HRM ch.6); with BLTPRI=0 the CPU makes
 *     progress while the blitter is busy (HRM: the blitter gives the CPU a
 *     cycle after it has waited 3);
 *   - copper: one COPER interrupt (MOVE to INTREQ at the end of the list)
 *     per frame (the copper restarts at COP1LC every vertical blank);
 *   - audio: one interrupt per channel per 2*AUDxLEN*AUDxPER colour clocks
 *     (HRM ch.5), counted against the beam-derived elapsed time;
 *   - disk (DF0, DMA read only): DSKBLK once, and the first sector found
 *     after the sync word has valid AmigaDOS header/data checksums. DF0 is
 *     read only when the machine booted through trackdisk.device, and only
 *     after a reference read without any other DMA load gave a valid
 *     AmigaDOS sector; otherwise (no disk, a PC/NDOS/unformatted disk) the
 *     disk part is untestable, never a fail.
 * Timing values (CPU colour clocks, beam samples, counts) go in "timing"
 * fields and perf records. Where the HRM gives an exact figure it is in
 * timing.hrm_* next to the measurement; the host compares those for
 * cycle-exact configs only (a non-cycle-exact emulator need not match).
 */
#include <exec/devices.h>
#include "mod.h"

AMT_MODULE(dmacontention, 23, AMT_PH_BARE, AMT_MF_OWNS_DISPLAY, 36864, 0);

void *memset(void *, int, unsigned long);

/* dmacontention_asm.s */
extern const UBYTE dc_code[], dc_code_end[];
extern const ULONG dc_loop_off, dc_sample_off;
/* dmacontention_040.s */
void dc_cacr_or(ULONG bits);
void dc_cpusha(void);
ULONG dc_cacr_get(void);
void dc_cacr_set(ULONG v);

#define DC_ITERS 16384UL
#define DC_FRAMES 8           /* repeat the workload until at least this many frames passed */
#define EXP_X    0xa0b43eccUL
#define EXP_Y    0x7b083c68UL
#define EXP_SUM  0x3a343295UL

/* chip pool layout */
#define O_CODE   0x0000
#define O_RES    0x0100
#define O_LOOP   0x0200
#define O_PLANE  0x0600
#define O_AUD    0x0700
#define O_BROW   0x0800
#define O_CROW   0x0840
#define O_A      0x1000
#define O_D      0x3000
#define O_COP    0x5000
#define O_BIG    0x6800
#define BIG_SIZE 0x2000

#define BLT_W    32           /* words per line */
#define BLT_H    128

#define CUST(o)  (*(volatile UWORD *)(0xdff000UL + (o)))
#define CUSTL(o) (*(volatile ULONG *)(0xdff000UL + (o)))
#define CIAA_PRA (*(volatile UBYTE *)0xbfe001UL)
#define CIAB_PRB (*(volatile UBYTE *)0xbfd100UL)
#define CIAB_DDRB (*(volatile UBYTE *)0xbfd300UL)

/* scenario flags */
#define F_BLT    0x01
#define F_NASTY  0x02
#define F_COP    0x04
#define F_AUD    0x08
#define F_DSK    0x10
#define F_SAMPLE 0x20         /* sample DMACONR (blitter) or VHPOSR (bitplanes) */

struct scen {
    const char *id;
    UWORD flags;
    UWORD bpl;                /* 0 none, 1 hires 4, 2 lores 6, 3 AGA 8 planes FMODE=3 */
};
static const struct scen scens[] = {
    { "baseline", 0, 0 },
    { "blitter", F_BLT | F_SAMPLE, 0 },
    { "blitter_nasty", F_BLT | F_NASTY | F_SAMPLE, 0 },
    { "copper", F_COP, 0 },
    { "bitplanes_hires4", F_SAMPLE, 1 },
    { "bitplanes_lores6", F_SAMPLE, 2 },
    { "bitplanes_aga8", F_SAMPLE, 3 },
    { "audio", F_AUD, 0 },
    { "disk", F_DSK, 0 },
    { "all", F_BLT | F_COP | F_AUD | F_DSK, 1 },
#ifdef DC_DEBUG
    { "dbg_dsk_blt", F_BLT | F_DSK, 0 },
    { "dbg_dsk_aud", F_AUD | F_DSK, 0 },
    { "dbg_dsk_cop", F_COP | F_DSK, 0 },
    { "dbg_dsk_bpl", F_DSK, 1 },
    { "dbg_dsk_cop_bpl", F_COP | F_DSK, 1 },
#endif
};
#define NSCEN (sizeof scens / sizeof scens[0])

static UBYTE *P;
static int fam;
static volatile ULONG blt_started, blt_done, cop_n, dsk_n;
static volatile ULONG aud_n[4];
static volatile UWORD blt_stop;
static ULONG blt_max;            /* restart limit: nasty blits back to back lock the CPU out */
static UWORD blt_con0;
static const UWORD aud_per[4] = { 200, 210, 220, 230 };
#define AUD_LEN 16

static void (*loop_fn)(ULONG *, ULONG, ULONG *);
static void (*sample_fn)(UWORD *, ULONG, volatile UWORD *);

/* ------------------------------------------------------------ helpers */
static void cache_push(void)
{
    if (fam == 2)
        dc_cacr_or(0x0008);
    else if (fam == 3)
        dc_cacr_or(0x0808);
    else if (fam == 4)
        dc_cpusha();
}

static ULONG xs(ULONG x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/* HALF colour clocks from the beam + VBL count, like amt_ticks() but
 * (a) corrected for a beam wrap whose VERTB has not been counted yet (read
 * at SR=$2700 amt_ticks() is then a frame short; same correction as
 * cia_hw.h cia_now()) and (b) with NTSC lines at their 227.5 cck average
 * (amt_ticks() counts 227: 0.2 % short, 9 interrupts over a 7 s A500 run).
 * Only differences are used (mod 2^32). */
static ULONG dc_now2(void)
{
    ULONG f1, f2, v, h;
    UWORD rq;
    ULONG lines = amt_probe_result.pal ? 313 : 263;
    do {
        f1 = amt_frames_now();
        rq = CUST(R_INTREQR);
        {
            ULONG b = amt_beam();
            v = b >> 8;
            h = b & 0xff;
        }
        f2 = amt_frames_now();
    } while (f1 != f2);
    if ((rq & 0x0020) && v < lines / 2)
        f1++;
    return (f1 * lines + v) * (amt_probe_result.pal ? 454UL : 455UL) + 2 * h;
}

static int wait_frames_until(volatile ULONG *v, ULONG target, ULONG frames)
{
    ULONG f0 = amt_frames_now();
    while (*v < target) {
        if (amt_frames_now() - f0 > frames)
            return 0;
        amt_watchdog_kick();
    }
    return 1;
}

static int blt_idle(ULONG frames)
{
    ULONG f0 = amt_frames_now();
    (void)CUST(R_DMACONR);
    while (CUST(R_DMACONR) & 0x4000) {
        if (amt_frames_now() - f0 > frames)
            return 0;
    }
    return 1;
}

static void blt_start(void)
{
    ULONG b = (ULONG)P;
    CUST(0x040) = blt_con0;            /* BLTCON0 */
    CUST(0x042) = 0;                   /* BLTCON1 */
    CUST(0x044) = 0xffff;
    CUST(0x046) = 0xffff;
    CUSTL(0x048) = b + O_CROW;         /* BLTCPT */
    CUSTL(0x04c) = b + O_BROW;         /* BLTBPT */
    CUSTL(0x050) = b + O_A;            /* BLTAPT */
    CUSTL(0x054) = b + O_D;            /* BLTDPT */
    CUST(0x060) = (UWORD)(-2 * BLT_W); /* BLTCMOD: C repeats one row */
    CUST(0x062) = (UWORD)(-2 * BLT_W); /* BLTBMOD: B repeats one row */
    CUST(0x064) = 0;
    CUST(0x066) = 0;
    blt_started++;
    CUST(0x058) = (UWORD)((BLT_H << 6) | BLT_W);
}

static void hook3(ULONG pend)
{
    if (pend & 0x0010) {
        CUST(R_INTREQ) = 0x0010;
        cop_n++;
    }
    if (pend & 0x0040) {
        CUST(R_INTREQ) = 0x0040;
        blt_done++;
        if (!blt_stop && blt_started < blt_max)
            blt_start();
    }
}

static void hook4(ULONG pend)
{
    int i;
    CUST(R_INTREQ) = (UWORD)(pend & 0x0780);
    for (i = 0; i < 4; i++)
        if (pend & (0x0080UL << i))
            aud_n[i]++;
}

static void hook1(ULONG pend)
{
    if (pend & 0x0002) {
        CUST(R_INTREQ) = 0x0002;
        dsk_n++;
    }
}

/* ------------------------------------------------------------ display */
static UWORD *cop_build(int planes, int moves)
{
    UWORD *p = (UWORD *)(P + O_COP);
    ULONG pl = (ULONG)P + O_PLANE;
    int i;
    for (i = 0; i < planes; i++) {
        *p++ = (UWORD)(0x0e0 + 4 * i);
        *p++ = (UWORD)(pl >> 16);
        *p++ = (UWORD)(0x0e2 + 4 * i);
        *p++ = (UWORD)pl;
    }
    if (moves) {
        *p++ = 0x3001;
        *p++ = 0xfffe;
        for (i = 0; i < moves; i++) {
            *p++ = 0x0180;
            *p++ = (i & 1) ? 0x0000 : 0x0111;
        }
    }
    *p++ = 0x009c;
    *p++ = 0x8010;                     /* INTREQ = SET | COPER */
    *p++ = 0xffff;
    *p++ = 0xfffe;
    return p;
}

static void bpl_setup(int mode)
{
    CUST(0x08e) = 0x2c81;              /* DIWSTRT */
    CUST(0x090) = 0x2cc1;              /* DIWSTOP */
    CUST(0x102) = 0;                   /* BPLCON1 */
    CUST(0x104) = 0;                   /* BPLCON2 */
    if (mode == 1) {
        CUST(0x092) = 0x003c;
        CUST(0x094) = 0x00d4;
        CUST(0x108) = (UWORD)-80;
        CUST(0x10a) = (UWORD)-80;
        CUST(0x100) = 0xc200;          /* HIRES, 4 planes, COLOR */
    } else if (mode == 2) {
        CUST(0x092) = 0x0038;
        CUST(0x094) = 0x00d0;
        CUST(0x108) = (UWORD)-40;
        CUST(0x10a) = (UWORD)-40;
        CUST(0x100) = 0x6200;          /* lores, 6 planes, COLOR */
    } else if (mode == 3) {
        CUST(0x1fc) = 0x0003;          /* FMODE: 4x bitplane fetch width */
        CUST(0x092) = 0x0038;
        CUST(0x094) = 0x00d0;
        CUST(0x108) = 0;
        CUST(0x10a) = 0;
        CUST(0x100) = 0x0210;          /* lores, 8 planes (BPU3), COLOR */
    }
}

static void *find_lib(const char *name)
{
    struct Node *n;
    for (n = SysBase->LibList.lh_Head; n && n->ln_Succ; n = n->ln_Succ) {
        const char *a = n->ln_Name, *b = name;
        if (!a)
            continue;
        while (*a && *a == *b)
            a++, b++;
        if (*a == *b)
            return n;
    }
    return 0;
}

static void display_restore(void)
{
    UBYTE *gfx = find_lib("graphics.library");
    ULONG ci = gfx ? *(ULONG *)(gfx + 0x26) : 0;   /* GfxBase->copinit */
    CUST(0x100) = 0x0200;
    if (amt_probe_result.chipset && amt_probe_result.chipset[0] == 'A')
        CUST(0x1fc) = 0;
    if (ci)
        CUSTL(0x080) = ci;
}

/* ------------------------------------------------------------ disk */
struct dsk_res {
    int state;          /* 0 ok, 1 untestable, 2 fail */
    const char *why;
    ULONG info, hsum_exp, hsum_got, dsum_exp, dsum_got, skipped;
};

static UBYTE prb_saved;
static UWORD adk_saved;
static int dsk_saved;

static int dsk_start(struct dsk_res *r)
{
    ULONG f0;
    UWORD *buf = (UWORD *)(P + O_BIG);
    int i;
    if (CIAB_DDRB != 0xff) {
        r->state = 1;
        r->why = "CIA-B port B (drive control) is not all outputs";
        return 0;
    }
    prb_saved = CIAB_PRB;
    adk_saved = CUST(R_ADKCONR);
    dsk_saved = 1;
    CIAB_PRB = 0xff;                   /* all deselected, /MTR high */
    CIAB_PRB = 0x7f;                   /* /MTR low */
    CIAB_PRB = 0x77;                   /* /SEL0 low: DF0 latches motor on */
    f0 = amt_frames_now();
    while (CIAA_PRA & 0x20) {          /* /DSKRDY */
        if (amt_frames_now() - f0 > amt_secs(2)) {
            r->state = 1;
            r->why = "DF0 did not report ready within 2 s after motor on (no drive?)";
            return 0;
        }
        amt_watchdog_kick();
    }
    if (!(CIAA_PRA & 0x04)) {          /* /DSKCHANGE low: no disk */
        r->state = 1;
        r->why = "no disk in DF0 (/DSKCHANGE low)";
        return 0;
    }
    for (i = 0; i < 640; i++)
        buf[i] = 0;
    return 1;
}

/* arm the read DMA: called with DSKBLK already enabled in INTENA */
static void dsk_go(void)
{
    UWORD *buf = (UWORD *)(P + O_BIG);
    CUST(0x09e) = 0x7f00;              /* ADKCON: clear precomp/MFMPREC/WORDSYNC/MSBSYNC/FAST */
    CUST(0x09e) = 0x9500;              /* SET MFMPREC WORDSYNC FAST (MFM read) */
    CUST(0x07e) = 0x4489;              /* DSKSYNC */
    CUSTL(0x020) = (ULONG)buf;         /* DSKPT */
    CUST(0x024) = 0x4000;              /* DSKLEN off, write bit clear */
    CUST(R_DMACON) = 0x8210;           /* DMAEN|DSKEN first (HRM ch.8), then DSKLEN twice */
    CUST(0x024) = 0x8000 | 600;        /* DMA on, 600 words, read */
    CUST(0x024) = 0x8000 | 600;        /* (HRM: written twice) */
}

static ULONG mfm(ULONG odd, ULONG even)
{
    return ((odd & 0x55555555UL) << 1) | (even & 0x55555555UL);
}

static ULONG lw(const UWORD *p)
{
    return ((ULONG)p[0] << 16) | p[1];
}

static void dsk_check(struct dsk_res *r)
{
    const UWORD *w = (const UWORD *)(P + O_BIG);
    ULONG hs = 0, ds = 0;
    int i, k = 0;
    while (k < 8 && w[k] == 0x4489)
        k++;
    r->skipped = (ULONG)k;
    w += k;
    /* AmigaDOS sector: info (odd,even), label 4+4, header sum, data sum, 512 bytes odd then even */
    r->info = mfm(lw(w), lw(w + 2));
    for (i = 0; i < 10; i++)
        hs ^= lw(w + 2 * i);
    hs &= 0x55555555UL;
    r->hsum_exp = hs;
    r->hsum_got = mfm(lw(w + 20), lw(w + 22));
    for (i = 0; i < 256; i++)
        ds ^= lw(w + 28 + 2 * i);
    ds &= 0x55555555UL;
    r->dsum_exp = ds;
    r->dsum_got = mfm(lw(w + 24), lw(w + 26));
    if ((r->info >> 24) != 0xff || ((r->info >> 8) & 0xff) > 21 || r->hsum_exp != r->hsum_got ||
        r->dsum_exp != r->dsum_got) {
        r->state = 2;
        r->why = "disk DMA data: the first sector after the sync word has a bad format byte or checksum "
                 "(AmigaDOS MFM sector layout)";
    }
}

static void dsk_stop(void)
{
    if (!dsk_saved)
        return;
    dsk_saved = 0;
    CUST(0x024) = 0x4000;
    CIAB_PRB = 0xff;
    CIAB_PRB = 0xf7;                   /* select DF0 with /MTR high: motor off */
    CIAB_PRB = 0xff;
    CIAB_PRB = prb_saved;
    CUST(0x09e) = 0x7f00;
    CUST(0x09e) = (UWORD)(0x8000 | (adk_saved & 0x7f00));
}

/* DF0 usable for the disk scenarios? Decided once, by a reference read with
 * no other DMA running: -1 not yet, 0 no (dsk_why), 1 yes. */
static int dsk_ok = -1;
static const char *dsk_why;

static int streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void dsk_reference(void)
{
    struct dsk_res dr;
    const char *bn = 0;
    memset(&dr, 0, sizeof dr);
    dsk_ok = 0;
    if (amt_bootio && amt_bootio->io_Device)
        bn = amt_bootio->io_Device->dd_Library.lib_Node.ln_Name;
    if (!bn || !streq(bn, "trackdisk.device")) {
        dsk_why = "not booted from a floppy through trackdisk.device (CD/HD/CLI start): DF0 is not "
                  "known to hold an AmigaDOS disk, so it is not read";
        return;
    }
    if (!dsk_start(&dr)) {
        dsk_why = dr.why;
        dsk_stop();
        return;
    }
    dsk_n = 0;
    amt_irq_hook[1] = hook1;
    CUST(R_INTREQ) = 0x0002;
    CUST(R_INTENA) = 0x8002;
    dsk_go();
    wait_frames_until(&dsk_n, 1, amt_secs(2));
    CUST(R_INTENA) = 0x0002;
    CUST(0x024) = 0x4000;
    CUST(R_DMACON) = 0x0010;
    CUST(R_INTREQ) = 0x0002;
    amt_irq_hook[1] = 0;
    if (dsk_n != 1)
        dsk_why = "reference read of DF0 with no other DMA running gave no DSKBLK within 2 s: no "
                  "$4489 sync on the track under the head, so DF0 holds no AmigaDOS track";
    else {
        cache_push();
        dsk_check(&dr);
        if (dr.state == 2)
            dsk_why = "reference read of DF0 with no other DMA running did not give a valid AmigaDOS "
                      "sector after the sync word: DF0 holds a non-AmigaDOS (PC, NDOS, unformatted) disk";
        else
            dsk_ok = 1;
    }
    dsk_stop();
    dsk_n = 0;
}

/* ------------------------------------------------------------ scenarios */
struct samp {
    ULONG n, busy;                     /* DMACONR samples: total, with BBUSY */
    ULONG in_fetch, out_fetch, lines;  /* VHPOSR samples on lines $40-$F0 */
};

static void analyse_vh(struct samp *s, ULONG n)
{
    const UWORD *b = (const UWORD *)(P + O_BIG);
    ULONG i, lo = 0x1ff, hi = 0;
    s->in_fetch = s->out_fetch = 0;
    for (i = 0; i < n; i++) {
        UWORD v = b[i] >> 8, h = b[i] & 0xff;
        if (v < 0x40 || v > 0xf0)
            continue;
        if (v < lo)
            lo = v;
        if (v > hi)
            hi = v;
        if (h >= 0x44 && h <= 0xd0)
            s->in_fetch++;
        else
            s->out_fetch++;
    }
    s->lines = hi >= lo ? hi - lo + 1 : 0;
}

static void prep_blit(void)
{
    UWORD *a = (UWORD *)(P + O_A), *b = (UWORD *)(P + O_BROW), *c = (UWORD *)(P + O_CROW);
    UWORD *d = (UWORD *)(P + O_D);
    ULONG x = 0x2545f491UL;
    int i;
    for (i = 0; i < BLT_W * BLT_H; i++) {
        x = xs(x);
        a[i] = (UWORD)(x >> 16);
        d[i] = 0;
    }
    for (i = 0; i < BLT_W; i++) {
        x = xs(x);
        b[i] = (UWORD)(x >> 16);
        c[i] = (UWORD)x;
    }
}

/* first wrong destination word (D = A ^ B ^ C, HRM minterm $96), or -1 */
static long check_blit(UWORD *exp, UWORD *got, ULONG *crc_e, ULONG *crc_g)
{
    const UWORD *a = (const UWORD *)(P + O_A), *b = (const UWORD *)(P + O_BROW), *c = (const UWORD *)(P + O_CROW);
    const UWORD *d = (const UWORD *)(P + O_D);
    long bad = -1;
    ULONG ce = 0, cg = 0;
    int i;
    for (i = 0; i < BLT_W * BLT_H; i++) {
        UWORD e = a[i] ^ b[i % BLT_W] ^ c[i % BLT_W];
        UBYTE be[2], bg[2];
        be[0] = (UBYTE)(e >> 8);
        be[1] = (UBYTE)e;
        bg[0] = (UBYTE)(d[i] >> 8);
        bg[1] = (UBYTE)d[i];
        ce = crc32_update(ce, be, 2);
        cg = crc32_update(cg, bg, 2);
        if (bad < 0 && d[i] != e) {
            bad = i;
            *exp = e;
            *got = d[i];
        }
    }
    *crc_e = ce;
    *crc_g = cg;
    return bad;
}

struct result {
    const char *why;
    int st;
    ULONG e1, g1, e2, g2, e3, g3;
    const char *k1, *k2, *k3;
};

static void fail_once(struct result *r, const char *why, const char *k1, ULONG e1, ULONG g1)
{
    if (r->st != AMT_ST_PASS)
        return;
    r->st = AMT_ST_FAIL;
    r->why = why;
    r->k1 = k1;
    r->e1 = e1;
    r->g1 = g1;
    r->k2 = r->k3 = 0;
}

static void run_scen(const struct scen *s)
{
    ULONG res[3], t0, t1, f0, f1, ta0 = 0, ta1 = 0, fc0 = 0, fc1 = 0, i;
    struct samp sm;
    struct dsk_res dr;
    struct result r;
    UWORD dma = 0, inten = 0;
    int dsk_on = 0, blit_hung = 0;
    ULONG *lb = (ULONG *)(P + O_LOOP);
    UWORD be = 0, bg = 0;
    ULONG crc_e = 0, crc_g = 0, cop_snap = 0, aud_snap[4] = { 0, 0, 0, 0 };
    ULONG aud_exp[4] = { 0, 0, 0, 0 };
    ULONG reps = 0, cacr0 = 0, dsk_last = 0;
    UWORD dskbytr = 0, dsk_intreq = 0, dsk_intena = 0;
    long bad = -1;

    if (s->bpl == 3 && !(amt_probe_result.chipset && amt_probe_result.chipset[0] == 'A')) {
        amt_case(s->id, AMT_ST_SKIP, "AGA chipset needed (8 bitplanes, FMODE)");
        return;
    }
    memset(&r, 0, sizeof r);
    memset(&dr, 0, sizeof dr);
    r.st = AMT_ST_PASS;
    sm.n = sm.busy = sm.in_fetch = sm.out_fetch = sm.lines = 0;
    blt_started = blt_done = cop_n = dsk_n = 0;
    for (i = 0; i < 4; i++)
        aud_n[i] = 0;
    blt_stop = 0;
    for (i = 0; i < 256; i++)
        lb[i] = 0;
    if (s->flags & F_BLT)
        prep_blit();
    if (s->flags & F_DSK) {
        if (dsk_ok < 0)
            dsk_reference();
        if (!dsk_ok) {
            dr.why = dsk_why;
            dsk_on = 0;
        } else
            dsk_on = dsk_start(&dr);
        if (!dsk_on) {
            dsk_stop();
            if (s->flags == F_DSK) {
                amt_case(s->id, AMT_ST_UNTESTABLE, dr.why);
                return;
            }
        } else {
            dma |= 0x0010;             /* DSKEN */
            inten |= 0x0002;           /* DSKBLK */
        }
    }
    cache_push();

    amt_irq_hook[3] = hook3;
    amt_irq_hook[4] = hook4;
    amt_irq_hook[1] = hook1;
    if (s->flags & (F_COP)) {
        cop_build(s->bpl ? (s->bpl == 1 ? 4 : s->bpl == 2 ? 6 : 8) : 0, 1200);
        inten |= 0x0010;
    } else if (s->bpl) {
        cop_build(s->bpl == 1 ? 4 : s->bpl == 2 ? 6 : 8, 0);
        inten |= 0x0010;
    }
    if (s->bpl) {
        bpl_setup(s->bpl);
        dma |= 0x0100;                 /* BPLEN */
    }
    if (s->flags & F_AUD) {
        UBYTE *smp = P + O_AUD;
        for (i = 0; i < 4 * 2 * AUD_LEN; i++)
            smp[i] = (UBYTE)((i & 8) ? 0x40 : 0xc0);
        for (i = 0; i < 4; i++) {
            ULONG o = 0x0a0 + 0x10 * i;
            CUSTL(o) = (ULONG)(smp + 2 * AUD_LEN * i);
            CUST(o + 4) = AUD_LEN;
            CUST(o + 6) = aud_per[i];
            CUST(o + 8) = 0;           /* volume 0: silent */
        }
        inten |= 0x0780;
    }
    if (s->flags & F_BLT) {
        blt_con0 = 0x0f96;             /* USEA|USEB|USEC|USED, D = A^B^C */
        blt_max = (s->flags & F_NASTY) ? 16 : 100000;
        inten |= 0x0040;
    }
    cache_push();                      /* copper list, samples, blit sources out to chip RAM */
    CUST(R_INTREQ) = 0x07d2;
    CUST(R_INTENA) = (UWORD)(0x8000 | inten);
    if (s->flags & (F_COP) || s->bpl) {
        CUSTL(0x080) = (ULONG)P + O_COP;
        CUST(0x088) = 0;               /* COPJMP1 */
        dma |= 0x0080;                 /* COPEN */
        fc0 = amt_frames_now();
    }
    if (s->flags & F_AUD) {
        dma |= 0x000f;
        ta0 = dc_now2();
    }
    if (s->flags & F_BLT)
        dma |= 0x0040 | ((s->flags & F_NASTY) ? 0x0400 : 0);
    if (dsk_on)
        dsk_go();
    CUST(R_DMACON) = (UWORD)(0x8200 | dma);
    if (s->flags & F_BLT)
        blt_start();

    if (s->flags & F_SAMPLE) {
        if (s->flags & F_BLT) {
            sample_fn((UWORD *)(P + O_BIG), BIG_SIZE / 32, (volatile UWORD *)(0xdff000UL + R_DMACONR));
            sm.n = BIG_SIZE / 2;
            for (i = 0; i < sm.n; i++)
                if (((UWORD *)(P + O_BIG))[i] & 0x4000)
                    sm.busy++;
        } else {
            /* wait for line $38 of a fresh frame, then sample VHPOSR */
            ULONG f = amt_frames_now();
            while (amt_frames_now() == f)
                ;
            while ((CUST(R_VHPOSR) >> 8) < 0x38)
                ;
            sample_fn((UWORD *)(P + O_BIG), BIG_SIZE / 32, (volatile UWORD *)(0xdff000UL + R_VHPOSR));
            analyse_vh(&sm, BIG_SIZE / 2);
        }
    }

    /* caches off while the workload runs, so that it really uses the chip bus */
    if (fam) {
        cache_push();
        cacr0 = dc_cacr_get();
        dc_cacr_set(fam >= 4 ? cacr0 & ~0x80008000UL : cacr0 & ~0x0101UL);
    }
    f0 = amt_frames_now();
    t0 = amt_ticks();
    reps = 0;
    do {
        for (i = 0; i < 256; i++)
            lb[i] = 0;
        loop_fn(lb, DC_ITERS, res);
        reps++;
        if (res[0] != EXP_X || res[1] != EXP_Y || res[2] != EXP_SUM)
            break;
    } while (amt_frames_now() - f0 < DC_FRAMES && reps < 100000);
    t1 = amt_ticks();
    f1 = amt_frames_now();
    if (fam) {
        dc_cacr_set(cacr0);
        cache_push();
    }

    {
        UWORD sr = amt_get_sr();
        amt_set_sr(0x2700);
        if (s->flags & F_AUD)
            ta1 = dc_now2();
        if (fc0)
            fc1 = amt_frames_now();
        cop_snap = cop_n;
        for (i = 0; i < 4; i++)
            aud_snap[i] = aud_n[i];
        amt_set_sr(sr);
    }
    blt_stop = 1;
    if (s->flags & F_BLT) {
        if (!blt_idle(100))
            blit_hung = 1;
        else
            wait_frames_until(&blt_done, blt_started, 10);
    }
    if (dsk_on) {
        const UWORD *w = (const UWORD *)(P + O_BIG);
        wait_frames_until(&dsk_n, 1, amt_secs(2));
        dskbytr = CUST(0x01a);
        dsk_intreq = CUST(R_INTREQR);
        dsk_intena = CUST(R_INTENAR);
        cache_push();
        for (i = 0; i < 640; i++)
            if (w[i])
                dsk_last = i;
    }
    {
        CUST(R_INTENA) = (UWORD)inten;
        CUST(R_DMACON) = (UWORD)(dma | 0x0400);
        CUST(R_INTREQ) = 0x07d2;
        if (dsk_on)
            dsk_stop();
        for (i = 0; i < 4; i++)
            CUST(0x0a8 + 0x10 * i) = 0;
        if (s->bpl || (s->flags & F_COP))
            display_restore();
        amt_irq_hook[1] = amt_irq_hook[3] = amt_irq_hook[4] = 0;

        /* ---- checks */
        if (res[0] != EXP_X || res[1] != EXP_Y || res[2] != EXP_SUM) {
            r.st = AMT_ST_FAIL;
            r.why = "the CPU workload in chip RAM gave a wrong result while DMA was running: DMA must be "
                    "invisible to the CPU's computation (got = the first wrong repetition)";
            r.k1 = "x"; r.e1 = EXP_X; r.g1 = res[0];
            r.k2 = "y"; r.e2 = EXP_Y; r.g2 = res[1];
            r.k3 = "sum"; r.e3 = EXP_SUM; r.g3 = res[2];
        }
        if (s->flags & F_BLT) {
            if (blit_hung)
                fail_once(&r, "the blitter never finished (BBUSY stuck for 2 s)", "bbusy", 0, 1);
            else if (blt_done != blt_started)
                fail_once(&r, "a blit completed without its BLIT interrupt, or an interrupt was lost "
                              "(HRM: BLIT is requested at the end of every blit)", "blit_irqs", blt_started, blt_done);
            else {
                cache_push();
                bad = check_blit(&be, &bg, &crc_e, &crc_g);
                if (bad >= 0)
                    fail_once(&r, "blitter destination differs from D = A^B^C (minterm $96, no shift, B/C "
                                  "modulo -64 repeating one row, HRM ch.6)", "dest_crc", crc_e, crc_g);
            }
            /* samples_busy (CPU progress while BBUSY) is a timing figure: an
             * immediate blitter or a slow CPU sees none without breaking
             * any HRM rule, so it is reported as timing.samples_busy with
             * timing.hrm_samples_busy_min, never as a fail */
        }
        if (fc0) {
            ULONG fr = fc1 - fc0;
            if (cop_snap + 1 < fr || cop_snap > fr + 1)
                fail_once(&r, "copper list ending in MOVE INTREQ,#$8010 must raise exactly one COPER "
                              "interrupt per frame (copper restarts at COP1LC every vertical blank), +-1; "
                              "the counts are timing.coper / timing.coper_frames",
                          "coper_irqs_within_1", 1, 0);
        }
        if (s->flags & F_AUD) {
            for (i = 0; i < 4; i++) {
                ULONG per = 2UL * AUD_LEN * aud_per[i];
                ULONG e = (ta1 - ta0) / (2 * per) + 1;
                aud_exp[i] = e;
                if ((aud_snap[i] + 3 < e || aud_snap[i] > e + 3) && r.st == AMT_ST_PASS)
                    fail_once(&r, "audio interrupts: one per 2*AUDxLEN*AUDxPER colour clocks per channel "
                                  "(HRM ch.5), +-3 for start and the beam clock; the counts are "
                                  "timing.aud / timing.aud_expected", "aud_irqs_within_3", 1, 0);
            }
        }
        if (dsk_on) {
            if (dsk_n != 1)
                fail_once(&r, "disk DMA read: exactly one DSKBLK interrupt expected when the DSKLEN word "
                              "count is done (HRM ch.8)", "dskblk", 1, dsk_n);
            else {
                cache_push();
                dsk_check(&dr);
                if (dr.state == 2)
                    fail_once(&r, dr.why, "data_sum", dr.dsum_exp, dr.dsum_got);
            }
        }

        amt_perf_hz(s->id, DC_ITERS * reps, t1 - t0, amt_cck_hz(), "cck");
        amt_case_open(s->id, r.st);
        if (r.st != AMT_ST_PASS) {
            json_str("why", r.why);
            json_obj("expect");
            json_hex(r.k1, r.e1);
            if (r.k2) json_hex(r.k2, r.e2);
            if (r.k3) json_hex(r.k3, r.e3);
            json_obj_end();
            json_obj("got");
            json_hex(r.k1, r.g1);
            if (r.k2) json_hex(r.k2, r.g2);
            if (r.k3) json_hex(r.k3, r.g3);
            json_obj_end();
            json_obj("ctx");
            json_hex("chip_pool", (ULONG)P);
            json_str("cpu", amt_probe_result.cpu);
            json_str("chipset", amt_probe_result.chipset);
            json_hexn("dmacon_set", (UWORD)(0x8200 | dma), 4);
            json_hexn("intena_set", (UWORD)(0x8000 | inten), 4);
            json_int("workload_reps", (long)reps);
            if (r.k1 && r.k1[0] == 'x' && !r.k1[1])
                json_str("workload", "16384 x {rol.l #5,d0; add.l d1,d0; eor.l d0,d1; move.l d0,(a0,d2.w); "
                                     "d2=(d2+$1c)&$3fc; add.l (a0,d2.w),d3} from d0=$12345678 d1=$9abcdef0 "
                                     "d2=d3=0 over a zeroed 1 KB chip buffer; x,y,sum = d0,d1,d3");
            if (bad >= 0) {
                const UWORD *a = (const UWORD *)(P + O_A), *bb = (const UWORD *)(P + O_BROW);
                const UWORD *cc = (const UWORD *)(P + O_CROW);
                json_int("first_bad_word", bad);
                json_int("line", bad / BLT_W);
                json_int("word", bad % BLT_W);
                json_hex("d_addr", (ULONG)P + O_D + 2 * (ULONG)bad);
                json_hexn("a", a[bad], 4);
                json_hexn("b", bb[bad % BLT_W], 4);
                json_hexn("c", cc[bad % BLT_W], 4);
                json_hexn("word_exp", be, 4);
                json_hexn("word_got", bg, 4);
                json_str("rule", "D = A ^ B ^ C (minterm $96); A advances (modulo 0), B and C repeat one "
                                 "32-word row (modulo -64)");
                json_int("blits_started", (long)blt_started);
            }
            if (dsk_on) {
                json_hex("sector_info", dr.info);
                json_hex("hdr_sum_exp", dr.hsum_exp);
                json_hex("hdr_sum_got", dr.hsum_got);
                json_int("sync_skipped", (long)dr.skipped);
                json_hexn("dskbytr", dskbytr, 4);
                json_hexn("intreqr", dsk_intreq, 4);
                json_hexn("intenar", dsk_intena, 4);
                json_int("last_nonzero_word", (long)dsk_last);
                json_arr("raw");
                for (i = 0; i < 16; i++)
                    json_hexn(NULL, ((UWORD *)(P + O_BIG))[i], 4);
                json_arr_end();
            }
            json_obj_end();
        }
        json_obj("in");
        json_int("iters", (long)DC_ITERS);
        if (s->flags & F_BLT) {
            json_hexn("bltcon0", blt_con0, 4);
            json_int("blt_words", BLT_W);
            json_int("blt_lines", BLT_H);
            json_int("nasty", (s->flags & F_NASTY) != 0);
        }
        if (s->bpl)
            json_str("bitplanes", s->bpl == 1 ? "hires 4 planes" : s->bpl == 2 ? "lores 6 planes"
                                                                   : "lores 8 planes FMODE=3");
        if (s->flags & F_DSK)
            json_str("disk", dsk_on ? "DF0 DMA read, 600 words" : dr.why);
        json_obj_end();
        json_obj("timing");
        json_int("cpu_cck", (long)(t1 - t0));
        json_int("reps", (long)reps);
        json_int("frames", (long)(f1 - f0));
        if (s->flags & F_BLT) {
            json_int("blits", (long)blt_done);
            if (s->flags & F_SAMPLE) {
                json_int("samples", (long)sm.n);
                json_int("samples_busy", (long)sm.busy);
                /* nasty: the CPU gets no chip-bus cycle while the ABCD blit
                 * runs, but BBUSY is set by the BLTSIZE write, before the
                 * blitter's first DMA cycle (start-up latency not given by
                 * the HRM), so a CPU that gets from BLTSIZE/RTE to the
                 * DMACONR read inside that window sees BBUSY once per start
                 * (the end too, at most). Bound only, not an exact HRM
                 * figure: an exact `hrm_samples_busy` = 0 was reported as an
                 * hrm_timing_mismatch on 68020/040/060 CE (1, 24, 24). */
                if (s->flags & F_NASTY)
                    json_int("hrm_samples_busy_max", 2L * (long)blt_done);
                else
                    json_int("hrm_samples_busy_min", 1);
            }
        }
        if (fc0) {
            json_int("coper", (long)cop_snap);
            json_int("coper_frames", (long)(fc1 - fc0));
        }
        if (s->flags & F_AUD) {
            json_arr("aud");
            for (i = 0; i < 4; i++)
                json_int(NULL, (long)aud_snap[i]);
            json_arr_end();
            json_arr("aud_expected");
            for (i = 0; i < 4; i++)
                json_int(NULL, (long)aud_exp[i]);
            json_arr_end();
        }
        if (s->bpl && (s->flags & F_SAMPLE)) {
            json_int("lines", (long)sm.lines);
            json_int("in_fetch", (long)sm.in_fetch);
            json_int("out_fetch", (long)sm.out_fetch);
            if (s->bpl == 1)
                json_int("hrm_in_fetch", 0);
        }
        json_obj_end();
        amt_case_close();
    }
}

void amt_run_dmacontention(void)
{
    unsigned i;
    ULONG n = (ULONG)(dc_code_end - dc_code);
    P = (UBYTE *)amt_chip_pool();
    switch (AMT_CPU()) {
    case AMT_CPU_68000: case AMT_CPU_68010: fam = 0; break;
    case AMT_CPU_68EC020: case AMT_CPU_68020: fam = 2; break;
    case AMT_CPU_68EC030: case AMT_CPU_68030: fam = 3; break;
    default: fam = 4; break;
    }
    if (n > O_RES - O_CODE) {
        amt_case("setup", AMT_ST_FAIL, "dmacontention code does not fit its chip pool slot");
        return;
    }
    for (i = 0; i < n; i++)
        P[O_CODE + i] = dc_code[i];
    cache_push();
    loop_fn = (void (*)(ULONG *, ULONG, ULONG *))(P + O_CODE + dc_loop_off);
    sample_fn = (void (*)(UWORD *, ULONG, volatile UWORD *))(P + O_CODE + dc_sample_off);
    for (i = 0; i < NSCEN; i++) {
        run_scen(&scens[i]);
        amt_watchdog_kick();
    }
}
