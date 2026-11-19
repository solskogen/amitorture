/* bench_score: the summary at the end of the bench disk (docs/BENCH.md
 * "Score"). It runs nothing itself: it reads the results of the blocks
 * that ran before it (bh_result), turns them into absolute rates with the
 * guest's own clock, reports them as one `score` record and shows them on
 * the screen. No reference machine: every number is a rate (DMIPS = VAX
 * 11/780 Dhrystones, 1757 per second), a bandwidth or a time. */
#include "harness.h"
#include "screen.h"

AMT_MODULE(bench_score, 44, AMT_PH_BARE, 0, 0, 0);

typedef unsigned long long U64;

/* operations per second from a result, 0 if there is none */
static U64 rate(const char *name, const char *kset, int *st)
{
    ULONG n, best, per;
    U64 ops;
    *st = bh_result(name, kset, &n, &best, &per);
    if ((*st != 1 && *st != 2) || !best)
        return 0;
    ops = (U64)per * n;
    return ops / best * amt_cck_hz() + ops % best * amt_cck_hz() / best;
}

/* tenths of a ns per operation, 0 if there is none */
static ULONG ns10(const char *name)
{
    ULONG n, best, per;
    U64 ops;
    int st = bh_result(name, 0, &n, &best, &per);
    if ((st != 1 && st != 2) || !n || !per)
        return 0;
    ops = (U64)per * n;
    return (ULONG)((U64)best * 1000000000ULL / amt_cck_hz() * 10 / ops);
}

/* v / 100 with two decimals, v / 10 with one */
static char *fix(char *d, ULONG v, int dec)
{
    ULONG s = dec == 2 ? 100 : 10, f = v % s;
    d = amt_utoa(d, v / s);
    *d++ = '.';
    if (dec == 2)
        *d++ = (char)('0' + f / 10);
    *d++ = (char)('0' + f % 10);
    *d = 0;
    return d;
}
static char *str(char *d, const char *s)
{
    while (*s)
        *d++ = *s++;
    *d = 0;
    return d;
}

static char l0[48], l1[48], l2[48], l3[48];

void amt_run_bench_score(void)
{
    U64 dh, fa, fm, r;
    ULONG dmips = 0, mflops = 0, mbs[3], off, hit, walk, flt, pfa, pt;
    long hit_pct = 0;
    int st, sa, sm, i, have_hit;
    const char *kset = "k020";
    static const char *const memn[3] = { "mem_read_chip", "mem_read_fast", "mem_read_rom" };
    static const char *const memk[3] = { "chip_mbs_x100", "fast_mbs_x100", "rom_mbs_x100" };
    static const char *const meml[3] = { " chip ", " fast ", " rom " };
    char *p;

    dh = rate("dhrystone", kset, &st);
    if (!dh) {
        kset = "k000";
        dh = rate("dhrystone", kset, &st);
    }
    if (dh)
        dmips = (ULONG)(dh * 100 / 1757);
    fa = rate("fadd", 0, &sa);
    fm = rate("fmul", 0, &sm);
    if (fa && fm)
        mflops = (ULONG)(2 * fa * fm / (fa + fm) / 10000);
    for (i = 0; i < 3; i++) {
        r = rate(memn[i], 0, &st);
        mbs[i] = (ULONG)(r / 10000);
    }
    off = ns10("mmu_off");
    hit = ns10("mmu_hit");
    walk = ns10("mmu_walk");
    flt = ns10("mmu_fault");
    pfa = ns10("mmu_pflusha");
    pt = ns10("mmu_ptest");
    if (!pt)
        pt = ns10("mmu_plpa");
    have_hit = off && hit;
    if (have_hit)
        hit_pct = ((long)hit - (long)off) * 1000 / (long)off;

    json_begin("bench");
    json_str("ev", "score");
    if (dmips) {
        json_int("dmips_x100", (long)dmips);
        json_str("dhry_kset", kset);
    }
    if (mflops)
        json_int("mflops_x100", (long)mflops);
    for (i = 0; i < 3; i++)
        if (mbs[i])
            json_int(memk[i], (long)mbs[i]);
    if (have_hit)
        json_int("mmu_hit_pct_x10", hit_pct);
    if (walk && hit)
        json_int("mmu_miss_ns_x10", (long)walk - (long)hit);
    if (flt)
        json_int("mmu_fault_ns", (long)(flt / 10));
    if (pfa)
        json_int("mmu_pflusha_ns_x10", (long)pfa);
    if (pt)
        json_int("mmu_ptest_ns_x10", (long)pt);
    json_end();

    p = str(l0, "DMIPS ");
    p = dmips ? fix(p, dmips, 2) : str(p, "?");
    p = str(p, "  MFLOPS ");
    if (mflops)
        fix(p, mflops, 2);
    else
        str(p, AMT_FPU() == AMT_FPU_NONE ? "none" : "?");
    p = str(l1, "MB/s");
    for (i = 0; i < 3; i++) {
        p = str(p, meml[i]);
        p = mbs[i] ? fix(p, mbs[i] / 10, 1) : str(p, i == 1 && amt_any_pool_on_chip_bus() ? "-" : "?");
    }
    if (!off) {
        str(l2, "MMU none or skipped");
        l3[0] = 0;
    } else {
        p = str(l2, "MMU hit ");
        if (have_hit) {
            if (hit_pct < 0) {
                *p++ = '-';
                hit_pct = -hit_pct;
            } else
                *p++ = '+';
            p = fix(p, (ULONG)hit_pct, 1);
            p = str(p, "%");
        } else
            p = str(p, "?");
        p = str(p, " miss ");
        p = walk > hit && hit ? amt_utoa(p, (walk - hit + 5) / 10) : str(p, "?");
        p = str(p, "ns fault ");
        p = flt ? fix(p, (flt + 500) / 1000, 1) : str(p, "?");
        str(p, "us");
        p = str(l3, "    pflusha ");
        p = pfa ? amt_utoa(p, (pfa + 5) / 10) : str(p, "?");
        p = str(p, "ns ptest ");
        p = pt ? amt_utoa(p, (pt + 5) / 10) : str(p, "?");
        str(p, "ns");
    }
    screen_bench(0, l0, 0, 0);
    screen_bench(1, l1, 0, 0);
    screen_bench(2, l2, 0, 0);
    if (l3[0])
        screen_bench(3, l3, 0, 0);
}
