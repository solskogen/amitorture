/* amitorture bench harness (docs/BENCH.md).
 *
 * A block is a fixed amount of work, run(count, ctx), that returns a
 * checksum folded over its results. The harness finds how much work makes
 * one repetition long enough to time (or takes the count from the work
 * plan), runs timed repetitions until the fastest two agree, checks the
 * results and reports `bench` records (docs/schema.md).
 *
 * The guest uses only its own clock (amt_ticks(): colour clocks from the
 * beam counter), serial output and the mailbox. When the run parameters say
 * a host follows the mailbox (flag `host`), the harness also asks the host,
 * through `bench` requests (CONTRACT §13), whether a sizing run was long
 * enough and whether another repetition is needed, so a host that times
 * the blocks with its own clock gets the repetitions it needs. Without a
 * host (real hardware, FPGA) nothing waits on anyone.
 */
#ifndef AMT_BENCH_HARNESS_H
#define AMT_BENCH_HARNESS_H

#include "mod.h"

typedef ULONG (*bh_fn)(ULONG count, void *ctx);

#define BH_NOSUM  0x0001   /* the result reads hardware that changes: not compared */
#define BH_FIXED  0x0002   /* its own schedule: count 1, one run, no sizing or warm-up */
#define BH_FRAME  0x0004   /* one count = one frame: the host measures its cost per emulated second */
#define BH_HOST   0x0008   /* only the host's clock says anything: skipped without a host */

struct bh_blk {
    const char *name;      /* "blk" in records */
    const char *tier;      /* "micro" | "kernel" | "frame" | "thread" */
    UWORD id;              /* plan id: unique on the disk, stable across builds */
    UWORD flags;           /* BH_* */
    bh_fn run;
    void *ctx;
    /* optional: called after the first and the last repetition with the
     * checksum run() returned; checks the work (1 = right) and may replace
     * the checksum. NULL: the checksums of all repetitions must agree. */
    int (*verify)(void *ctx, ULONG *sum);
    ULONG per;             /* operations per count (for rates) */
    const char *unit;      /* what an operation is */
    const char *mem;       /* where its data lives ("chip", "fast", "slow", "rom") or NULL */
    const char *kset;      /* kernel set, or NULL */
};

#define BH_MAX_REPS 12     /* repetitions before a block is reported unconfirmed */

/* Once per module, in the bare phase: measures the clock's resolution
 * (first call only), reads the work plan, and enables the 68060 FPU. */
void bh_begin(void);
/* Restores what bh_begin changed. */
void bh_end(void);

/* Run one block. Returns 1 if it ran and its results were right. */
int bh_run(const struct bh_blk *b);
void bh_skip(const struct bh_blk *b, const char *why);
/* from inside bh_run (verify): a remark for the block's end record ("note") */
void bh_note(const char *s);

/* The result of a block that ran earlier on this boot (kset NULL: any):
 * 0 none, 1 ok, 2 unconfirmed, 3 fail; n = count, best = colour clocks for
 * n counts of per operations each. For bench_score. */
int bh_result(const char *name, const char *kset, ULONG *n, ULONG *best, ULONG *per);

/* mailbox case number while a block's timed work runs:
 * BH_POS_RUN | id << 8 | sequence (1-255, per timed run) */
#define BH_POS_RUN   0x10000UL
#define BH_POS_QUIET 0x20000UL     /* flood_recovery: its quiet phases */
ULONG bh_pos(void);                /* the current position, for blocks that change it */
void  bh_mark(ULONG pos);          /* set the mailbox position only (bh_pos() unchanged) */

/* harness_asm.s */
ULONG bh_pcr_get(void);
void  bh_pcr_set(ULONG);
void  bh_fpu_reset(void);
ULONG bh_fpcr_get(void);           /* FPU present only */
void  bh_fpcr_set(ULONG);

/* The work plan (CONTRACT §15): counts frozen by a baseline, patched into
 * the disk by the runner. Empty on a fresh disk: every block sizes itself. */
#define BH_PLAN_MAX 120
#define BH_PLAN_EXCLUSIVE 0x0001   /* header: blocks not listed are skipped */
#define BH_PLAN_SKIP      0x0001   /* entry: skip this block */
struct bh_plan_entry {
    UWORD id;
    UWORD flags;
    ULONG count;                   /* 0: size it */
};
struct bh_plan {
    char  magic[4];                /* "PLAN" */
    UWORD version;                 /* 1 */
    UWORD n;
    UWORD flags;
    UWORD pad;
    ULONG crc;                     /* crc32 of entries[0..n) */
    struct bh_plan_entry e[BH_PLAN_MAX];
};
extern struct bh_plan amt_plan;

#endif
