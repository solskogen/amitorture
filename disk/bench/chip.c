/* bench_chip: the chipset tier (docs/BENCH.md "Chip tier"): what the
 * custom chips cost the CPU and how fast they work, timed on the guest's
 * colour clock like the micro tier. It runs after bench_score, so none of
 * it enters the score.
 *
 *   chip_lores6   the mem_read_chip loop with 6 lowres bitplanes on
 *   chip_hires4   the same with 4 hires bitplanes on
 *   copper_load   the same with the copper writing 40 colour registers on
 *                 every line from $2C to $FF (no bitplanes)
 *   irq_flood     a level-3 interrupt (COPER set through INTREQ) per unit:
 *                 request, entry, hook, RTE, the CPU sees the count change
 *   blit_copy     A to D copy of 32 KB per blit (64 words x 256 lines)
 *   blit_fill     D-only fill of the same area
 * Compare chip_lores6/chip_hires4/copper_load with micro mem_read_chip
 * (the same loop, display DMA off). The contention blocks point the copper
 * at their own list inside the timed run and turn their DMA off before
 * returning; the status screen comes back after the block (screen_bare). */
#include "harness.h"
#include "screen.h"

#define CH_READ  65536UL                /* bm_mem_read: 64 KB per unit */
#define CH_LINE  80                     /* one hires line; planes repeat it */
#define CH_COP   40960UL                /* the three lists, ~35 KB */
#define CH_CHIP  (CH_READ + CH_LINE + CH_COP)
AMT_MODULE(bench_chip, 45, AMT_PH_BARE, AMT_MF_LONG, CH_CHIP, 0);

ULONG bm_mem_read(ULONG, void *);

#define LOAD_FIRST 0x2c
#define LOAD_LAST  0xff
#define LOAD_MOVES 40

static UBYTE *chip;
static UWORD *cop_lores6, *cop_hires4, *cop_load;

static UWORD *cmove(UWORD *c, UWORD reg, UWORD val)
{
    *c++ = reg;
    *c++ = val;
    return c;
}

static UWORD *cop_end(UWORD *c)
{
    *c++ = 0xffff;
    *c++ = 0xfffe;
    return c;
}

/* a display of `planes` planes that all repeat one line (modulo -bytes) */
static UWORD *cop_planes(UWORD *c, int planes, int hires)
{
    int aga = amt_probe_result.chipset && amt_probe_result.chipset[0] == 'A';
    UWORD bytes = hires ? 80 : 40;
    ULONG line = (ULONG)(chip + CH_READ);
    int i;
    c = cmove(c, 0x08e, (LOAD_FIRST << 8) | 0x81);         /* DIWSTRT */
    c = cmove(c, 0x090, 0x2cc1);                            /* DIWSTOP */
    c = cmove(c, 0x092, hires ? 0x003c : 0x0038);           /* DDFSTRT */
    c = cmove(c, 0x094, hires ? 0x00d4 : 0x00d0);           /* DDFSTOP */
    c = cmove(c, 0x100, (UWORD)((hires ? 0x8000 : 0) | (planes << 12) | 0x0200));
    c = cmove(c, 0x102, 0x0000);                            /* BPLCON1 */
    if (aga) {
        c = cmove(c, 0x106, 0x0c00);                        /* BPLCON3 */
        c = cmove(c, 0x10c, 0x0011);                        /* BPLCON4 */
        c = cmove(c, 0x1fc, 0x0000);                        /* FMODE: OCS fetches */
    }
    c = cmove(c, 0x108, (UWORD)-bytes);                     /* BPL1MOD */
    c = cmove(c, 0x10a, (UWORD)-bytes);                     /* BPL2MOD */
    for (i = 0; i < planes; i++) {
        c = cmove(c, (UWORD)(0x0e0 + 4 * i), (UWORD)(line >> 16));
        c = cmove(c, (UWORD)(0x0e2 + 4 * i), (UWORD)line);
    }
    return c;
}

static void build(void)
{
    UWORD *c = (UWORD *)(chip + CH_READ + CH_LINE);
    ULONG *p = (ULONG *)chip, s = 7;
    int i, y;
    for (i = 0; i < (int)(CH_READ / 4); i++)
        p[i] = s += 0x9e3779b9UL;
    for (i = 0; i < CH_LINE / 2; i++)
        ((UWORD *)(chip + CH_READ))[i] = (UWORD)(0x0ff0 ^ (i * 0x1357));
    cop_lores6 = c;
    c = cop_end(cop_planes(c, 6, 0));
    cop_hires4 = c;
    c = cop_end(cop_planes(c, 4, 1));
    cop_load = c;
    c = cop_planes(c, 0, 0);
    for (y = LOAD_FIRST; y <= LOAD_LAST; y++) {
        *c++ = (UWORD)((y << 8) | 0x07);                    /* WAIT line y */
        *c++ = 0xfffe;
        for (i = 0; i < LOAD_MOVES; i++)
            c = cmove(c, (UWORD)(0x182 + 2 * (i % 31)), (UWORD)((y * 0x0013 + i * 0x0247) & 0x0fff));
    }
    cop_end(c);
}

static void blit_wait(void)
{
    (void)CREG(R_DMACONR);                  /* old Agnus: BBUSY one read late */
    while (CREG(R_DMACONR) & 0x4000)
        ;
}

static void cop_start(UWORD *list)
{
    CREG(0x080) = (UWORD)((ULONG)list >> 16);               /* COP1LC */
    CREG(0x082) = (UWORD)(ULONG)list;
    blit_wait();                                            /* no COPJMP1 during a blit */
    CREG(0x088) = 0;                                        /* COPJMP1 */
}

static ULONG contended(UWORD *list, ULONG units)
{
    ULONG r;
    cop_start(list);
    CREG(R_DMACON) = 0x8380;                                /* SET | DMAEN | BPLEN | COPEN */
    r = bm_mem_read(units, chip);
    CREG(R_DMACON) = 0x0180;
    return r;
}

static ULONG ch_lores6(ULONG units, void *ctx) { (void)ctx; return contended(cop_lores6, units); }
static ULONG ch_hires4(ULONG units, void *ctx) { (void)ctx; return contended(cop_hires4, units); }
static ULONG ch_copper(ULONG units, void *ctx) { (void)ctx; return contended(cop_load, units); }

/* irq_flood: the level-3 hook takes COPER, acknowledges it, counts */
static volatile ULONG irq_count;

static void irq_hook(ULONG pending)
{
    if (pending & 0x0010) {
        CREG(R_INTREQ) = 0x0010;
        CREG(R_INTREQ) = 0x0010;        /* twice: the first can be too early on a fast CPU */
        irq_count++;
    }
}

static ULONG ch_irq(ULONG units, void *ctx)
{
    ULONG n0 = irq_count, c, spin, lost = 0;
    (void)ctx;
    while (units--) {
        c = irq_count;
        CREG(R_INTREQ) = 0x8010;        /* SET | COPER */
        for (spin = 0; irq_count == c; )
            if (++spin == 1000000UL) {
                lost++;
                break;
            }
    }
    return irq_count - n0 + (lost << 16);
}

/* blitter: one 64 x 256 word blit (32 KB) per unit inside the read buffer */
#define BLIT_W 64
#define BLIT_H 256

static void blit(UWORD con0, ULONG src, ULONG dst)
{
    CREG(0x040) = con0;                                     /* BLTCON0 */
    CREG(0x042) = 0;                                        /* BLTCON1 */
    CREG(0x044) = 0xffff;                                   /* BLTAFWM */
    CREG(0x046) = 0xffff;                                   /* BLTALWM */
    CREG(0x064) = 0;                                        /* BLTAMOD */
    CREG(0x066) = 0;                                        /* BLTDMOD */
    CREG(0x050) = (UWORD)(src >> 16);                       /* BLTAPT */
    CREG(0x052) = (UWORD)src;
    CREG(0x054) = (UWORD)(dst >> 16);                       /* BLTDPT */
    CREG(0x056) = (UWORD)dst;
    CREG(0x058) = (BLIT_H << 6) | BLIT_W;                   /* BLTSIZE: go */
    blit_wait();
}

static ULONG blit_sum(ULONG dst)
{
    ULONG *d = (ULONG *)dst;
    return d[0] + d[BLIT_W * BLIT_H / 2 - 1] + d[1234];
}

static ULONG ch_blit_copy(ULONG units, void *ctx)
{
    ULONG src = (ULONG)chip, dst = src + 2 * BLIT_W * BLIT_H;
    (void)ctx;
    CREG(R_DMACON) = 0x8240;                                /* SET | DMAEN | BLTEN */
    blit_wait();
    while (units--)
        blit(0x09f0, src, dst);                             /* USEA | USED, D = A */
    return blit_sum(dst);
}

static ULONG ch_blit_fill(ULONG units, void *ctx)
{
    ULONG dst = (ULONG)chip + 2 * BLIT_W * BLIT_H;
    (void)ctx;
    CREG(R_DMACON) = 0x8240;
    blit_wait();
    while (units--)
        blit(0x01ff, 0, dst);                               /* USED, D = 1 */
    return blit_sum(dst);
}

struct ch_def {
    const char *name;
    UWORD id;
    bh_fn fn;
    ULONG per;
    const char *unit;
};

/* Plan ids share the 200-255 range with bench_system and bench_mmu: never
 * reuse or renumber one. */
static const struct ch_def defs[] = {
    { "chip_lores6", 203, ch_lores6,    CH_READ,                 "byte" },
    { "chip_hires4", 204, ch_hires4,    CH_READ,                 "byte" },
    { "copper_load", 205, ch_copper,    CH_READ,                 "byte" },
    { "irq_flood",   206, ch_irq,       1,                       "irq"  },
    { "blit_copy",   207, ch_blit_copy, 2UL * BLIT_W * BLIT_H,   "byte" },
    { "blit_fill",   208, ch_blit_fill, 2UL * BLIT_W * BLIT_H,   "byte" },
};

void amt_run_bench_chip(void)
{
    struct bh_blk b;
    UWORD blten = (UWORD)(CREG(R_DMACONR) & 0x0040);
    unsigned i;

    chip = amt_chip_pool();
    bh_begin();
    build();
    for (i = 0; i < sizeof defs / sizeof defs[0]; i++) {
        const struct ch_def *d = &defs[i];
        b.name = d->name;
        b.tier = "chip";
        b.id = d->id;
        b.flags = 0;
        b.run = d->fn;
        b.ctx = 0;
        b.verify = 0;
        b.per = d->per;
        b.unit = d->unit;
        b.mem = "chip";
        b.kset = 0;
        if (d->fn == ch_irq) {
            b.mem = 0;
            CREG(R_INTENA) = 0x0010;
            CREG(R_INTREQ) = 0x0010;
            amt_irq_hook[3] = irq_hook;
            CREG(R_INTENA) = 0x8010;                        /* SET | COPER */
        }
        bh_run(&b);
        if (d->fn == ch_irq) {
            CREG(R_INTENA) = 0x0010;
            CREG(R_INTREQ) = 0x0010;
            amt_irq_hook[3] = 0;
        }
        screen_bare(1);
    }
    blit_wait();
    if (!blten)
        CREG(R_DMACON) = 0x0040;
    bh_end();
}
