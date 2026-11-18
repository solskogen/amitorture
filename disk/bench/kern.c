/* bench_kernels: the kernel tier, real workloads from benchwork
 * (docs/BENCH.md "Kernel tier"). Every machine runs the 68000 set (k000);
 * a 68020 or later also runs k020 and, with an FPU, k020f, else k020s.
 * A kernel's memory comes from an arena in the module's any-pool (fast RAM
 * when there is some); its checksum after the first and the last
 * repetition must be the one the host computed when the disk was built
 * (kexpect.h). */
#include "harness.h"
#include "kernels/kset.h"
#include "kernels/kexpect.h"

/* bench.h, without its includes: the kernel sets are compiled separately */
struct Bench {
    const char *name;
    const char *desc;
    int group;
    const char *unit;
    double per_loop;
    void *(*setup)(void);
    const void *(*run)(void *state);
    UBYTE (*verify)(void *state, const void *result, ULONG *check);
    void (*teardown)(void *state);
};

#define KN_ARENA (160UL * 1024)   /* the largest kernel needs about 100 KB */
AMT_MODULE(bench_kernels, 41, AMT_PH_BARE, AMT_MF_LONG, 0, KN_ARENA);

extern const struct kset_api k000_kset, k020_kset, k020f_kset, k020s_kset;

struct kn_set {
    const char *name;
    const struct kset_api *api;
    const struct kexpect *expect;
    UWORD n_expect;
    UWORD id_base;          /* plan ids: id_base + index in the set */
};

/* Plan ids are part of the baseline format: kernels are only ever added at
 * the end of a set, and a set's id range (32) never moves. */
static const struct kn_set sets[] = {
    { "k000",  &k000_kset,  kexpect_k000,  sizeof kexpect_k000 / sizeof kexpect_k000[0],  64 },
    { "k020",  &k020_kset,  kexpect_k020,  sizeof kexpect_k020 / sizeof kexpect_k020[0],  96 },
    { "k020f", &k020f_kset, kexpect_k020f, sizeof kexpect_k020f / sizeof kexpect_k020f[0], 128 },
    { "k020s", &k020s_kset, kexpect_k020s, sizeof kexpect_k020s / sizeof kexpect_k020s[0], 160 },
};

struct kn_ctx {
    const struct kset_api *api;
    const struct Bench *b;
    const struct kexpect *expect;   /* NULL: none for this kernel */
    void *state;
    const void *result;
    ULONG mark;                     /* the arena after setup() */
};

static ULONG kn_run(ULONG count, void *p)
{
    struct kn_ctx *c = p;
    const void *r = 0;
    while (count--) {
        c->api->arena_release(c->mark);
        r = c->b->run(c->state);
    }
    c->result = r;
    return (ULONG)r;
}

static int kn_verify(void *p, ULONG *sum)
{
    struct kn_ctx *c = p;
    ULONG check = 0;
    int ok = c->result && !c->api->arena_failed() && c->b->verify(c->state, c->result, &check);
    *sum = check;
    return ok && c->expect && c->expect->check == check;
}

static int same(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* (ULONG) of a non-negative IEEE double, without floating point */
static ULONG dtoul(const double *d)
{
    const ULONG *w = (const ULONG *)d;
    int e = (int)((w[0] >> 20) & 0x7ff) - 1023;
    ULONG hi = (w[0] & 0xfffff) | 0x100000, lo = w[1];
    if ((w[0] & 0x80000000UL) || e < 0)
        return 0;
    if (e > 31)
        return 0xffffffffUL;
    /* value = hi:lo (53 bits) >> (52 - e) */
    if (e >= 20)
        return (hi << (e - 20)) | (e > 20 ? lo >> (52 - e) : 0);
    return hi >> (20 - e);
}

static const char *pool_kind(void)
{
    if (amt_any_pool_is_chip())
        return "chip";
    return amt_any_pool_on_chip_bus() ? "slow" : "fast";
}

static void run_set(const struct kn_set *s)
{
    const struct kset_api *api = s->api;
    struct kn_ctx c;
    struct bh_blk blk;
    unsigned i;

    for (i = 0; i < api->n; i++) {
        const struct Bench *b = (const struct Bench *)api->benches[i];
        c.api = api;
        c.b = b;
        c.expect = i < s->n_expect && same(s->expect[i].name, b->name) ? &s->expect[i] : 0;
        c.result = 0;
        blk.name = b->name;
        blk.tier = "kernel";
        blk.id = (UWORD)(s->id_base + i);
        blk.flags = 0;
        blk.run = kn_run;
        blk.ctx = &c;
        blk.verify = kn_verify;
        blk.per = dtoul(&b->per_loop);
        blk.unit = b->unit;
        blk.mem = pool_kind();
        blk.kset = s->name;

        api->arena_init(amt_any_pool(), KN_ARENA);
        c.state = b->setup();
        if (!c.state || api->arena_failed()) {
            bh_skip(&blk, "setup failed: the kernel's input doesn't fit its arena");
            continue;
        }
        if (!c.expect) {
            bh_skip(&blk, "no expected checksum for this kernel (kexpect.h is stale)");
            continue;
        }
        c.mark = api->arena_mark();
        bh_run(&blk);
        if (b->teardown)
            b->teardown(c.state);
    }
}

void amt_run_bench_kernels(void)
{
    int cpu020 = AMT_CPU() >= AMT_CPU_68EC020;
    int fpu = AMT_FPU() != AMT_FPU_NONE;
    unsigned i;

    bh_begin();
    for (i = 0; i < sizeof sets / sizeof sets[0]; i++) {
        const struct kn_set *s = &sets[i];
        if (s->api->version != KSET_VERSION)
            continue;
        if (i > 0 && !cpu020)
            continue;
        if ((s->api == &k020f_kset && !fpu) || (s->api == &k020s_kset && fpu))
            continue;
        if (s->api == &k020f_kset) {
            /* round every result to single precision, as the reference
             * (tools/kset_expect.py: x87 at single precision) does
             * (rounding precision bits 7-6 = 01, round to nearest) */
            ULONG fpcr = bh_fpcr_get();
            bh_fpcr_set((fpcr & ~0xf0UL) | 0x40);
            run_set(s);
            bh_fpcr_set(fpcr);
        } else
            run_set(s);
    }
    bh_end();
}
