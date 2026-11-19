/* bench_mmu: the MMU tier (docs/BENCH.md "MMU tier"). On a 68030, 68040,
 * 68LC040, 68060 or 68LC060 whose MMU is off at the start, the module
 * builds its own identity-mapped tables (everything the run needs: chip
 * RAM, the chip registers, ROM, the payload, the stack, the vectors, the
 * pool) plus a 2 MB test window that maps onto four data pages, and times:
 *
 *   mmu_off      reads, MMU off (the baseline)
 *   mmu_hit      the same reads through the MMU, every page in the ATC
 *   mmu_walk     the same physical data over 256 logical pages: an ATC
 *                miss (a table walk) for every read
 *   mmu_fault    invalid page -> bus error -> handler validates it -> RTE
 *   mmu_pflusha  PFLUSHA (and the walks it causes for the loop's own code)
 *   mmu_ptest    PTESTR (68030, 68040) / mmu_plpa PLPAR (68060)
 *
 * The read blocks give the same checksum: they read the same physical
 * bytes. The original MMU registers are put back at the end. */
#include "harness.h"

#define POOL 40960UL
AMT_MODULE(bench_mmu, 43, AMT_PH_BARE, 0, 0, POOL);

#define A(x) ((ULONG)(x))
#define VL(a) (*(volatile ULONG *)(a))

extern UWORD bmm_fam, bmm_fexp;
extern ULONG bmm_tc, bmm_tt[2], bmm_crp[2], bmm_root, bmm_orig[11];
extern ULONG bmm_fdesc, bmm_finv, bmm_fval, bmm_fva, bmm_old2;
void  bmm_save(void);
void  bmm_setup(void);
void  bmm_restore(void);
ULONG bmm_read(ULONG count, ULONG base, ULONG step);
ULONG bmm_fault(ULONG count);
ULONG bmm_pflusha(ULONG count);
ULONG bmm_ptest(ULONG count, ULONG la);
void  bmm_berr(void);

#define WIN_LEN 0x200000UL         /* MB 0: read pages, MB 1: fault page */
#define NEAR    64UL               /* step: 4 pages, all in the ATC */
#define FAR     0x4040UL           /* step: 256 pages, every read misses */

static struct { ULONG lo, hi; } need[10];
static int nneed;
static ULONG tp, thi, t_over;
static ULONG P, W, tc_on, cm_mem, cm_p;

static void need_add(ULONG lo, ULONG hi)
{
    if (nneed < 10) {
        need[nneed].lo = lo & ~0xfffUL;
        need[nneed].hi = (hi + 0xfff) & ~0xfffUL;
        nneed++;
    }
}
static int overlaps(ULONG lo, ULONG len)
{
    int i;
    for (i = 0; i < nneed; i++)
        if (need[i].lo < lo + len && lo < need[i].hi)
            return 1;
    return 0;
}
static int chipreg(ULONG a)
{
    return a < 0x00200000UL || (a >= 0x00bf0000UL && a < 0x00c00000UL) ||
           (a >= 0x00df0000UL && a < 0x00e00000UL);
}

/* does a transparent translation register match supervisor data reads at a? */
static int fam_;
static int tt_hits(ULONG tt, ULONG a)
{
    ULONG base = tt >> 24, mask = (tt >> 16) & 0xff;
    if (!(tt & 0x8000UL) || (((a >> 24) ^ base) & ~mask & 0xff))
        return 0;
    if (fam_ == 3)                 /* R/W (read) or RWM, and FC 5 */
        return (tt & 0x300UL) && !((5 ^ (tt >> 4)) & ~tt & 7);
    return ((tt >> 13) & 3) != 0;  /* S: not user-only */
}
/* the cache mode bits an identity page gets: what the MMU-off access gets
 * (an original DTT that matches, the timed run may have it cleared), else
 * serialised for the chip registers and the default for memory */
static ULONG cm_of(ULONG a)
{
    int i;
    for (i = 5; i <= 6; i++)
        if (tt_hits(bmm_orig[i], a))
            return fam_ == 3 ? ((bmm_orig[i] & 0x400UL) ? 0x40 : 0) : bmm_orig[i] & 0x60;
    return fam_ == 3 ? 0 : chipreg(a) ? 0x40 : cm_mem;
}

static ULONG talloc(ULONG size, ULONG align)
{
    ULONG a = (tp + align - 1) & ~(align - 1), i;
    if (a + size > thi) {
        t_over = 1;
        return 0;
    }
    tp = a + size;
    for (i = 0; i < size; i += 4)
        VL(a + i) = 0;
    return a;
}
/* --- 68030: TC $80C04880 (4 KB pages, TIA 4, TIB 8, TIC 8) --- */
#define D_UM 0x18UL                /* U and M preset: the walker never writes */
static void build30(void)
{
    ULONG at = talloc(64, 16), bt = talloc(1024, 16), c1 = talloc(1024, 16), c2 = talloc(1024, 16);
    ULONG ia = W >> 28, ib = (W >> 20) & 0xff, i;
    if (t_over)
        return;
    for (i = 0; i < 16; i++)
        VL(at + 4 * i) = i << 28 | D_UM | 1;
    VL(at + 4 * ia) = bt | 8 | 2;
    for (i = 0; i < 256; i++)
        VL(bt + 4 * i) = ia << 28 | i << 20 | D_UM | 1;
    VL(bt + 4 * ib) = c1 | 8 | 2;
    VL(bt + 4 * (ib + 1)) = c2 | 8 | 2;
    for (i = 0; i < 256; i++)
        VL(c1 + 4 * i) = (P + (i & 3) * 4096) | cm_p | D_UM | 1;
    bmm_fdesc = c2;
    /* CI: the 68030's data cache is logical, a hit there would not fault */
    bmm_fval = P | 0x40 | D_UM | 1;
    bmm_finv = 0;
    bmm_crp[0] = 0x7fff0002UL;
    bmm_crp[1] = at;
    tc_on = 0x80c04880UL;
}

/* --- 68040/68060: 4 KB pages --- */
static ULONG *slot40(ULONG la)
{
    ULONG *re = (ULONG *)bmm_root + (la >> 25), *pe, t;
    if (!(*re & 2)) {
        if (!(t = talloc(512, 512)))
            return 0;
        *re = t | 8 | 2;
    }
    pe = (ULONG *)(*re & ~0x1ffUL) + ((la >> 18) & 127);
    if (!(*pe & 2)) {
        if (!(t = talloc(256, 256)))
            return 0;
        *pe = t | 8 | 2;
    }
    return (ULONG *)(*pe & ~0xffUL) + ((la >> 12) & 63);
}
static void build40(void)
{
    ULONG a, *s, k;
    int i;
    if (!(bmm_root = talloc(512, 512)))
        return;
    for (i = 0; i < nneed; i++)
        for (a = need[i].lo; a < need[i].hi; a += 4096) {
            if (!(s = slot40(a)))
                return;
            if (!*s)
                *s = a | cm_of(a) | D_UM | 1;
        }
    for (k = 0; k < 256; k++) {
        if (!(s = slot40(W + k * 4096)))
            return;
        *s = (P + (k & 3) * 4096) | cm_p | D_UM | 1;
    }
    if (!(s = slot40(W + 0x100000UL)))
        return;
    bmm_fdesc = A(s);
    bmm_fval = P | cm_p | D_UM | 1;
    bmm_finv = 0;
}

/* --- blocks --- */
struct rd { ULONG base, step, tc, count, expect; };
static struct rd r_off, r_hit, r_walk, r_flt, r_pf, r_pt;

static ULONG tt_on[2];
static void use(ULONG tc)
{
    bmm_tc = tc;
    bmm_tt[0] = tc ? tt_on[0] : bmm_orig[5];
    bmm_tt[1] = tc ? tt_on[1] : bmm_orig[6];
}

static ULONG run_read(ULONG count, void *ctx)
{
    struct rd *r = ctx;
    r->count = count;
    use(r->tc);
    return bmm_read(count, r->base, r->step);
}
static ULONG run_fault(ULONG count, void *ctx)
{
    struct rd *r = ctx;
    r->count = count;
    use(tc_on);
    return bmm_fault(count);
}
static ULONG run_pflusha(ULONG count, void *ctx)
{
    (void)ctx;
    use(tc_on);
    return bmm_pflusha(count);
}
static ULONG run_ptest(ULONG count, void *ctx)
{
    struct rd *r = ctx;
    use(tc_on);
    return bmm_ptest(count, r->base);
}
/* the reads' checksum: count x the sum of one count's physical words */
static int vf_count(void *ctx, ULONG *sum)
{
    struct rd *r = ctx;
    return *sum == r->count * r->expect;
}
static int vf_ptest(void *ctx, ULONG *sum)
{
    struct rd *r = ctx;
    return *sum == r->expect;
}

static void blk(struct bh_blk *b, const char *name, UWORD id, bh_fn fn, void *ctx,
                int (*vf)(void *, ULONG *), ULONG per, const char *unit, const char *mem)
{
    b->name = name;
    b->tier = "mmu";
    b->id = id;
    b->flags = 0;
    b->run = fn;
    b->ctx = ctx;
    b->verify = vf;
    b->per = per;
    b->unit = unit;
    b->mem = mem;
    b->kset = 0;
}

static struct bh_blk bl[7];
static void set_blocks(const char *mem)
{
    blk(&bl[0], "mmu_off", 220, run_read, &r_off, vf_count, 256, "read", mem);
    blk(&bl[1], "mmu_hit", 221, run_read, &r_hit, vf_count, 256, "read", mem);
    blk(&bl[2], "mmu_walk", 222, run_read, &r_walk, vf_count, 256, "read", mem);
    blk(&bl[3], "mmu_fault", 223, run_fault, &r_flt, vf_count, 1, "fault", mem);
    blk(&bl[4], "mmu_pflusha", 224, run_pflusha, &r_pf, 0, 16, "insn", mem);
    blk(&bl[5], "mmu_ptest", 225, run_ptest, &r_pt, vf_ptest, 16, "insn", mem);
    blk(&bl[6], "mmu_plpa", 226, run_ptest, &r_pt, vf_ptest, 16, "insn", mem);
}
static void skip_all(const char *why)
{
    int i;
    for (i = 0; i < 7; i++)
        bh_skip(&bl[i], why);
}

static int tried;
static void try_setup(void)
{
    bmm_save();
    tried = 1;
}

void amt_run_bench_mmu(void)
{
    static const ULONG wins[6] = { 0xae000000UL, 0x6e000000UL, 0x2e000000UL,
                                   0xee000000UL, 0x5e000000UL, 0x9e000000UL };
    LONG c = AMT_CPU();
    ULONG pool = A(amt_any_pool()), rom, chip_top, sp, sum, i, j, tlo;
    const char *mem = amt_any_pool_is_chip() ? "chip" : amt_any_pool_on_chip_bus() ? "slow" : "fast";
    amt_vec_fn old2;
    int fam, k;

    bh_begin();
    set_blocks(mem);
    fam = c == AMT_CPU_68030 ? 3 : (c == AMT_CPU_68040 || c == AMT_CPU_68LC040) ? 4
        : (c == AMT_CPU_68060 || c == AMT_CPU_68LC060) ? 6 : 0;
    if (!fam || !AMT_MMU()) {
        skip_all(c == AMT_CPU_68EC030 || c == AMT_CPU_68EC040 || c == AMT_CPU_68EC060
                     ? "EC variant: no paged MMU"
                 : (c == AMT_CPU_68020 && AMT_MMU()) ? "68020 + 68851 is not covered"
                                                     : "no 68030/040/060 MMU in this machine");
        bh_end();
        return;
    }
    bmm_fam = (UWORD)fam;
    fam_ = fam;
    tried = 0;
    if (amt_try(try_setup) || !tried) {
        skip_all("reading the MMU registers trapped: no paged MMU");
        bh_end();
        return;
    }
    if (fam == 3 ? (bmm_orig[0] & 0x80000000UL) : (bmm_orig[0] & 0x8000UL)) {
        skip_all("the MMU was already enabled (TC.E=1): its tables are not ours to change");
        bh_end();
        return;
    }

    tlo = (pool + 511) & ~511UL;
    tp = tlo;
    thi = tlo + 16384;
    t_over = 0;
    P = (thi + 4095) & ~4095UL;
    rom = amt_probe_result.rom_base & ~0x3ffffUL;
    if (rom < 0xf80000UL)
        rom = 0xf80000UL;
    chip_top = 0x200000UL;
    for (k = 0; k < amt_probe_result.n_mem; k++)
        if ((amt_probe_result.mem[k].attr & 2) && amt_probe_result.mem[k].upper > chip_top &&
            amt_probe_result.mem[k].upper <= 0x00a00000UL)
            chip_top = (amt_probe_result.mem[k].upper + 0x3ffff) & ~0x3ffffUL;
    nneed = 0;
    need_add(0, chip_top);
    need_add(0xbf0000UL, 0xc00000UL);
    need_add(0xdf0000UL, 0xe00000UL);
    need_add(rom, 0x1000000UL);
    need_add(A(amt_base), A(amt_base) + amt_hdr->alloc_size);
    need_add(A(amt_base2), A(amt_base2) + amt_hdr->data_alloc);
    sp = A(&sp);
    need_add(sp - 0xa000UL, sp + 0x2000UL);
    need_add(A(amt_vbr()), A(amt_vbr()) + 0x400);
    need_add(pool, pool + POOL);
    /* a window no transparent translation covers; else the first free one,
     * with the TTs that cover it off while the MMU is on */
    W = 0;
    for (k = 0; k < 6 && !W; k++)
        if (!overlaps(wins[k], WIN_LEN) && !tt_hits(bmm_orig[5], wins[k]) &&
            !tt_hits(bmm_orig[6], wins[k]))
            W = wins[k];
    for (k = 0; k < 6 && !W; k++)
        if (!overlaps(wins[k], WIN_LEN))
            W = wins[k];
    if (!W) {
        skip_all("no free 2 MB logical range for the test window");
        bh_end();
        return;
    }

    for (k = 0; k < 2; k++)
        tt_on[k] = tt_hits(bmm_orig[5 + k], W) ? 0 : bmm_orig[5 + k];
    for (i = 0; i < 4096; i++)
        VL(P + 4 * i) = 0x9e3779b9UL * (i + 1);
    /* the cache mode MMU-off memory accesses get without a TT: 68040
     * write-through, 68060 TC.DCO */
    cm_mem = fam == 6 ? ((bmm_orig[0] >> 9) & 3) << 5 : 0;
    cm_p = cm_of(P);
    if (fam == 3)
        build30();
    else {
        tc_on = fam == 6 ? 0x8000UL | (bmm_orig[0] & 0x07fcUL) : 0x8000UL;
        build40();
    }
    if (t_over) {
        skip_all("the translation tables did not fit in the pool");
        bh_end();
        return;
    }

    sum = 0;
    for (j = 0; j < 64; j++)
        for (i = 0; i < 4; i++)
            sum += VL(P + i * 0x1010 + j * 64);
    r_off.base = P;
    r_off.step = NEAR;
    r_off.tc = 0;
    r_off.expect = sum;
    r_hit = r_off;
    r_hit.base = W;
    r_hit.tc = tc_on;
    r_walk = r_hit;
    r_walk.step = FAR;
    r_flt.expect = VL(P);
    bmm_fva = W + 0x100000UL;
    r_pt.base = W;
    r_pt.expect = fam == 6 ? P : W;

    bmm_setup();
    old2 = amt_vec_set(2, bmm_berr);
    bmm_old2 = A(old2);
    for (k = 0; k < 5; k++)
        bh_run(&bl[k]);
    if (fam == 6) {
        bh_skip(&bl[5], "68060: no PTEST (PLPAR instead)");
        bh_run(&bl[6]);
    } else {
        bh_run(&bl[5]);
        bh_skip(&bl[6], "PLPAR is 68060 only");
    }
    amt_vec_set(2, old2);
    bmm_restore();
    bh_end();
}
