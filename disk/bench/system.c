/* bench_system: what an emulator spends besides executing instructions
 * (docs/BENCH.md "Frame tier" and "Thread tier"). One count of a frame
 * block is one frame; the guest's clock can only say that a frame took a
 * frame, so these blocks need a host that times them with its own clock
 * (BH_HOST), as the cost of one emulated second.
 *
 *   frame:  stop          the CPU idle in STOP, woken by the VBL: the
 *                         emulator's own cost (chipset, display, pacing)
 *           vbl_poll      busy-waiting on the beam, as games do
 *           chipset_load  STOP with a busy display: 6 planes, 8 sprites,
 *                         a copper list rewriting colours on every line
 *   thread: stop_timer    STOP woken 1500 times a second by a CIA timer,
 *                         as an idle AmigaOS: the cost of waking the CPU
 *           flood_recovery  a CIA write flood, then quiet integer work: a
 *                         threaded CPU must come back after the flood
 * Ported from the accuracy disks' former hostspeed module. */
#include "harness.h"
#include "screen.h"

#define SY_CHIP 16384
AMT_MODULE(bench_system, 42, AMT_PH_BARE, AMT_MF_LONG, SY_CHIP, 4096);

ULONG bm_stop(ULONG, void *);
ULONG bm_vbl_poll(ULONG, void *);
ULONG bm_cia_write(ULONG, void *);
ULONG bm_add_l(ULONG, void *);

/* chipset_load: STOP (one unit = one frame, as "stop") with the display
 * chipset as busy as a game screen: 6 lowres bitplanes (every plane
 * repeats one line, modulo -40), all 8 sprites over 200 lines, a copper
 * list that rewrites 8 colours on every line from $2C to $FF, and
 * collision detection on for every sprite and plane. What the host spends
 * per emulated second here, minus "stop", is the cost of emulating that
 * display. Built once in the module's chip pool; the status screen comes
 * back after the block (screen_bare). */
#define LOAD_FIRST  0x2c
#define LOAD_LAST   0xff
#define LOAD_SPR_H  200
#define LOAD_COLS   8
#define LOAD_SPR_WORDS (2 + 2 * LOAD_SPR_H + 2)

static UWORD *load_cop;

static UWORD *load_move(UWORD *c, UWORD reg, UWORD val)
{
    *c++ = reg;
    *c++ = val;
    return c;
}

static void load_build(void)
{
    UBYTE *chip = amt_chip_pool();
    UWORD *line = (UWORD *)chip;                    /* 64 bytes: each plane fetches 40 */
    UWORD *spr = (UWORD *)(chip + 64);
    UWORD *c = spr + 8 * LOAD_SPR_WORDS;
    int aga = amt_probe_result.chipset && amt_probe_result.chipset[0] == 'A';
    UWORD vstop = LOAD_FIRST + LOAD_SPR_H;
    int i, y;
    load_cop = c;
    for (i = 0; i < 32; i++)
        line[i] = (UWORD)(0x0ff0 ^ (i * 0x1357));
    for (i = 0; i < 8; i++) {
        UWORD *s = spr + i * LOAD_SPR_WORDS;
        UWORD h = (UWORD)(0x60 + 0x10 * i);         /* HSTART / 2: overlapping, so they collide */
        s[0] = (UWORD)((LOAD_FIRST << 8) | h);
        s[1] = (UWORD)(((vstop & 0xff) << 8) | ((vstop & 0x100) ? 2 : 0));
        for (y = 0; y < LOAD_SPR_H; y++) {
            s[2 + 2 * y] = (UWORD)(0xffff >> (y & 15));
            s[3 + 2 * y] = (UWORD)(0x8001 | (y << 4));
        }
        s[2 + 2 * LOAD_SPR_H] = 0;
        s[3 + 2 * LOAD_SPR_H] = 0;
    }
    c = load_move(c, 0x08e, (LOAD_FIRST << 8) | 0x81);     /* DIWSTRT */
    c = load_move(c, 0x090, 0x2cc1);                        /* DIWSTOP */
    c = load_move(c, 0x092, 0x0038);                        /* DDFSTRT */
    c = load_move(c, 0x094, 0x00d0);                        /* DDFSTOP */
    c = load_move(c, 0x100, 0x6200);                        /* BPLCON0: 6 planes, COLOR */
    c = load_move(c, 0x102, 0x0000);                        /* BPLCON1 */
    c = load_move(c, 0x104, 0x0024);                        /* BPLCON2 */
    if (aga) {
        c = load_move(c, 0x106, 0x0c00);                    /* BPLCON3 */
        c = load_move(c, 0x10c, 0x0011);                    /* BPLCON4 */
        c = load_move(c, 0x1fc, 0x0000);                    /* FMODE */
    }
    c = load_move(c, 0x108, (UWORD)-40);                    /* BPL1MOD: repeat the line */
    c = load_move(c, 0x10a, (UWORD)-40);                    /* BPL2MOD */
    for (i = 0; i < 6; i++) {
        ULONG a = (ULONG)line + 2 * i;
        c = load_move(c, (UWORD)(0x0e0 + 4 * i), (UWORD)(a >> 16));
        c = load_move(c, (UWORD)(0x0e2 + 4 * i), (UWORD)a);
    }
    for (i = 0; i < 8; i++) {
        ULONG a = (ULONG)(spr + i * LOAD_SPR_WORDS);
        c = load_move(c, (UWORD)(0x120 + 4 * i), (UWORD)(a >> 16));
        c = load_move(c, (UWORD)(0x122 + 4 * i), (UWORD)a);
    }
    for (y = LOAD_FIRST; y <= LOAD_LAST; y++) {
        *c++ = (UWORD)((y << 8) | 0x07);                    /* WAIT line y */
        *c++ = 0xfffe;
        for (i = 0; i < LOAD_COLS; i++)
            c = load_move(c, (UWORD)(0x182 + 2 * i), (UWORD)((y * 0x0013 + i * 0x0247) & 0x0fff));
    }
    *c++ = 0xffff;                                          /* wait forever */
    *c++ = 0xfffe;
}

static ULONG sy_chipset_load(ULONG units, void *mem)
{
    ULONG r;
    if (!load_cop)
        load_build();
    CREG(0x098) = 0xffc0;                                   /* CLXCON: every sprite and plane */
    CREG(0x080) = (UWORD)((ULONG)load_cop >> 16);           /* COP1LC */
    CREG(0x082) = (UWORD)(ULONG)load_cop;
    CREG(0x088) = 0;                                        /* COPJMP1 (no blit runs here) */
    CREG(R_DMACON) = 0x83a0;                                /* SET | DMAEN | BPLEN | COPEN | SPREN */
    r = bm_stop(units, mem);
    CREG(R_DMACON) = 0x01a0;                                /* BPLEN | COPEN | SPREN off */
    CREG(0x098) = 0;
    (void)CREG(0x00e);                                      /* CLXDAT: clear */
    screen_bare(1);
    return r;
}
/* A CIA write flood: BSET/BCLR on the power LED bit of CIA-A PRA,
 * Kickstart 1.3's Alert pattern: back-to-back CIA writes. With a threaded
 * CPU each one is a hand-off to the thread that runs the chipset, which
 * must still keep real time. The LED bit is put back as it was. */
static ULONG sy_cia_write_led(ULONG units, void *mem)
{
    volatile UBYTE *pra = (volatile UBYTE *)0xbfe001;
    UBYTE led = (UBYTE)(*pra & 2);
    ULONG r = bm_cia_write(units, mem);
    if (led)
        *pra |= 2;
    else
        *pra &= (UBYTE)~2;
    return r;
}

/* stop_timer: as "stop" (one unit = one frame, the CPU idle in STOP), but
 * CIA-B timer A also wakes the CPU, ST_TIMER_HZ times a second, as an idle
 * AmigaOS is woken by its timers (more than a thousand times a second on
 * Workbench 3.2). What the host spends per emulated second here, over
 * "stop", is the cost of waking an idle CPU: with a threaded CPU, how long
 * it spins before it sleeps. The timer and its interrupt are switched off
 * again after every call. */
#define ST_TIMER_HZ 1500
#define CIAB_TALO (*(volatile UBYTE *)0xbfd400)
#define CIAB_TAHI (*(volatile UBYTE *)0xbfd500)
#define CIAB_ICR  (*(volatile UBYTE *)0xbfdd00)
#define CIAB_CRA  (*(volatile UBYTE *)0xbfde00)
#define ST_INT_EXTER 0x2000

static void st_hook(ULONG pending)
{
    (void)pending;
    (void)CIAB_ICR;                         /* read: acknowledges timer A */
    CREG(R_INTREQ) = ST_INT_EXTER;
}

static ULONG sy_stop_timer(ULONG units, void *mem)
{
    UWORD latch = (UWORD)(amt_eclk_hz() / ST_TIMER_HZ);
    ULONG end;
    (void)mem;
    CIAB_CRA = 0x00;                        /* timer A stopped */
    CIAB_ICR = 0x7f;                        /* every CIA-B source off */
    (void)CIAB_ICR;
    CIAB_TALO = (UBYTE)latch;
    CIAB_TAHI = (UBYTE)(latch >> 8);
    CREG(R_INTREQ) = ST_INT_EXTER;
    amt_irq_hook[6] = st_hook;
    CIAB_ICR = 0x81;                        /* SET | TA */
    CREG(R_INTENA) = 0x8000 | ST_INT_EXTER;
    CIAB_CRA = 0x11;                        /* LOAD | START, continuous */
    end = amt_frames_now() + units;
    while ((LONG)(amt_frames_now() - end) < 0)
        __asm__ volatile("stop #0x2000" ::: "memory");
    CIAB_CRA = 0x00;
    CIAB_ICR = 0x7f;
    (void)CIAB_ICR;
    CREG(R_INTENA) = ST_INT_EXTER;
    CREG(R_INTREQ) = ST_INT_EXTER;
    amt_irq_hook[6] = 0;
    return 0;
}

/* flood_recovery: FR_CYCLES times a flood of CIA writes for FR_FLOOD_FRAMES
 * (sy_cia_write_led: BSET/BCLR on the power LED bit, the costliest CIA write for
 * the thread that serves it), then FR_QUIET_FRAMES of plain integer work
 * (add_l). A
 * threaded CPU that loses the flood to the host is moved to the main thread
 * by the emulator; it must come back for the quiet work, not sit out a long
 * hold (docs/BENCH.md, "Thread tier"). The mailbox position says
 * which phase runs: the block's own during floods, BH_POS_QUIET during the
 * quiet work, so the runner can measure the CPU thread in each. */
#define FR_CYCLES       6
#define FR_FLOOD_FRAMES 50
#define FR_QUIET_FRAMES 150

static ULONG sy_flood_recovery(ULONG units, void *mem)
{
    int c;
    ULONG end;
    (void)units;
    for (c = 0; c < FR_CYCLES; c++) {
        amt_watchdog_kick();
        bh_mark(bh_pos());
        end = amt_frames_now() + FR_FLOOD_FRAMES;
        while ((LONG)(amt_frames_now() - end) < 0)
            sy_cia_write_led(16, mem);
        bh_mark(BH_POS_QUIET);
        end = amt_frames_now() + FR_QUIET_FRAMES;
        while ((LONG)(amt_frames_now() - end) < 0)
            bm_add_l(64, mem);
    }
    bh_mark(bh_pos());
    return 0;
}

struct sy_def {
    const char *name, *tier;
    UWORD id, flags;
    ULONG per;
    const char *unit;
    bh_fn fn;
};

/* Plan ids are part of the baseline format: never reuse or renumber one.
 * Retired: 210 (cia_write: only the E-clock bus wait, as cia_read). */
static const struct sy_def defs[] = {
    { "stop",           "frame",  200, BH_FRAME | BH_HOST | BH_NOSUM, 1,    "frame", bm_stop },
    { "vbl_poll",       "frame",  201, BH_FRAME | BH_HOST | BH_NOSUM, 1,    "frame", bm_vbl_poll },
    { "chipset_load",   "frame",  202, BH_FRAME | BH_HOST | BH_NOSUM, 1,    "frame", sy_chipset_load },
    { "stop_timer",     "thread", 211, BH_FRAME | BH_HOST | BH_NOSUM, 1,    "frame", sy_stop_timer },
    { "flood_recovery", "thread", 212, BH_FIXED | BH_HOST | BH_NOSUM, 1,    "run",   sy_flood_recovery },
};

void amt_run_bench_system(void)
{
    struct bh_blk b;
    unsigned i;

    bh_begin();
    for (i = 0; i < sizeof defs / sizeof defs[0]; i++) {
        const struct sy_def *d = &defs[i];
        b.name = d->name;
        b.tier = d->tier;
        b.id = d->id;
        b.flags = d->flags;
        b.run = d->fn;
        b.ctx = amt_any_pool();
        b.verify = 0;
        b.per = d->per;
        b.unit = d->unit;
        b.mem = 0;
        b.kset = 0;
        bh_run(&b);
    }
    bh_end();
}
