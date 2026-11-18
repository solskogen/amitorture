/* amitorture bench harness: sizing, timed repetitions, the stopping rule
 * and the bench records (harness.h, docs/BENCH.md). */
#include "harness.h"

/* the work plan: in initialised data so that mkadf finds it in the payload
 * (symbol _amt_plan) and the runner can patch it (CONTRACT §15) */
struct bh_plan amt_plan = { { 'P', 'L', 'A', 'N' }, 1, 0, 0, 0, 0, { { 0, 0, 0 } } };

static int inited, plan_ok, host;
static ULONG res_ticks;        /* the clock's resolution, colour clocks */
static ULONG floor_ticks;      /* 100 ms: shortest repetition */
static ULONG min_ticks;        /* repetition length the guest's own clock needs */
static ULONG seq, cur_pos, pcr_saved;
static UBYTE fpu_on;

ULONG bh_pos(void) { return cur_pos; }

void bh_mark(ULONG pos)
{
    mbox_set_pos(amt_mbox ? amt_mbox->cur_mod : 0, pos);
}

static void set_pos(ULONG pos)
{
    cur_pos = pos;
    bh_mark(pos);
}

/* the smallest step of the clock, read back to back: the read itself on a
 * clock that ticks with the beam (a few colour clocks on a fast CPU), a
 * frame on a clock that only moves between frames */
static ULONG measure_res(void)
{
    ULONG t = amt_ticks(), start = t, best = 0xffffffffUL, steps = 0;
    ULONG limit = (ULONG)(amt_probe_result.pal ? 313 : 263) * 227UL * 12;
    while (steps < 64 && amt_ticks() - start < limit) {
        ULONG u = amt_ticks();
        if (u != t) {
            if (u - t < best)
                best = u - t;
            steps++;
            t = u;
        }
    }
    return best == 0xffffffffUL ? limit : best;
}

static int plan_valid(void)
{
    if (amt_plan.magic[0] != 'P' || amt_plan.magic[1] != 'L' || amt_plan.magic[2] != 'A' ||
        amt_plan.magic[3] != 'N' || amt_plan.version != 1 || amt_plan.n > BH_PLAN_MAX)
        return 0;
    return crc32_update(0, amt_plan.e, amt_plan.n * sizeof amt_plan.e[0]) == amt_plan.crc;
}

static const struct bh_plan_entry *plan_find(UWORD id)
{
    int i;
    if (!plan_ok)
        return 0;
    for (i = 0; i < amt_plan.n; i++)
        if (amt_plan.e[i].id == id)
            return &amt_plan.e[i];
    return 0;
}

void bh_begin(void)
{
    if (!inited) {
        inited = 1;
        host = (amt_par.flags & AMT_F_HOST) && amt_mbox;
        res_ticks = measure_res();
        floor_ticks = amt_cck_hz() / 10;
        min_ticks = res_ticks > floor_ticks / 400 ? res_ticks * 400 : floor_ticks;
        plan_ok = plan_valid();
        json_begin("bench");
        json_str("ev", "env");
        json_int("tick_hz", (long)amt_cck_hz());
        json_int("res_ticks", (long)res_ticks);
        json_int("min_ticks", (long)min_ticks);
        json_int("host", host);
        json_int("plan", plan_ok ? amt_plan.n : -1);
        if (plan_ok && (amt_plan.flags & BH_PLAN_EXCLUSIVE))
            json_int("plan_exclusive", 1);
        json_end();
    }
    /* 68060: the probe leaves PCR.DFP as it found it; FPU code needs it clear */
    if (AMT_FPU() != AMT_FPU_NONE && AMT_CPU() >= AMT_CPU_68EC060) {
        pcr_saved = bh_pcr_get();
        if (pcr_saved & 2)
            bh_pcr_set(pcr_saved & ~2UL);
        fpu_on = 1;
    } else if (AMT_FPU() != AMT_FPU_NONE)
        fpu_on = 1;
}

void bh_end(void)
{
    if (fpu_on) {
        bh_fpu_reset();
        if (AMT_CPU() >= AMT_CPU_68EC060 && (pcr_saved & 2))
            bh_pcr_set(pcr_saved);
        fpu_on = 0;
    }
    set_pos(0);
}

static void rec(const struct bh_blk *b, const char *ev)
{
    json_begin("bench");
    json_str("ev", ev);
    json_str("blk", b->name);
}

void bh_skip(const struct bh_blk *b, const char *why)
{
    rec(b, "skip");
    json_str("tier", b->tier);
    json_int("id", b->id);
    json_str("why", why);
    json_end();
}

static void wait_frame(void)
{
    ULONG f = amt_frames_now();
    while (amt_frames_now() == f)
        ;
}

/* one timed run, starting at vertical blank so that every repetition sees
 * the display and refresh DMA from the same point of the frame */
static ULONG timed(const struct bh_blk *b, ULONG count, const char *phase, ULONG *sum)
{
    ULONG t0, t1, s;
    if (++seq > 255)
        seq = 1;
    amt_watchdog_kick();
    wait_frame();
    set_pos(BH_POS_RUN | ((ULONG)b->id << 8) | seq);
    t0 = amt_ticks();
    s = b->run(count, b->ctx);
    t1 = amt_ticks();
    set_pos(0);
    amt_watchdog_kick();
    if (sum)
        *sum = s;
    rec(b, "run");
    json_str("ph", phase);
    json_int("n", (long)count);
    json_int("seq", (long)seq);
    json_obj("timing");
    json_int("t", (long)(t1 - t0));
    json_obj_end();
    json_end();
    return t1 - t0;
}

/* 1: the host wants it (a longer sizing run, another repetition);
 * -1: it doesn't; 0: no host, or it didn't answer */
static int ask(const struct bh_blk *b, const char *what, ULONG count, ULONG reps)
{
    char args[56], *p = args;
    if (!host)
        return 0;
    *p++ = '{';
    p[0] = '"', p[1] = 'n', p[2] = '"', p[3] = ':';
    p = amt_utoa(p + 4, count);
    *p++ = ',';
    p[0] = '"', p[1] = 'r', p[2] = 'e', p[3] = 'p', p[4] = 's', p[5] = '"', p[6] = ':';
    p = amt_utoa(p + 7, reps);
    p[0] = ',', p[1] = '"', p[2] = 'i', p[3] = 'd', p[4] = '"', p[5] = ':';
    p = amt_utoa(p + 6, b->id);
    *p++ = '}';
    *p = 0;
    return amt_request3(what, b->name, args, amt_secs(10));
}

/* the two fastest repetitions agree within 0.25% */
static int confirmed(const ULONG *t, int n, ULONG *best, ULONG *second)
{
    ULONG a = 0xffffffffUL, c = 0xffffffffUL;
    int i;
    for (i = 0; i < n; i++) {
        if (t[i] < a) {
            c = a;
            a = t[i];
        } else if (t[i] < c)
            c = t[i];
    }
    *best = a;
    *second = c;
    return n >= 2 && c - a <= a / 400;
}

int bh_run(const struct bh_blk *b)
{
    const struct bh_plan_entry *pe = plan_find(b->id);
    ULONG t[BH_MAX_REPS], cold = 0, count, prev = 0, sum0 = 0, sum1 = 0, best, second;
    int n = 0, ok = 1, nonlinear = 0, conf, a;
    const char *why = 0;

    if (plan_ok && ((pe && (pe->flags & BH_PLAN_SKIP)) || (!pe && (amt_plan.flags & BH_PLAN_EXCLUSIVE)))) {
        bh_skip(b, "not in this run's work plan");
        return 0;
    }
    if ((b->flags & BH_HOST) && !host) {
        bh_skip(b, "measures the host's cost per emulated frame: needs a host following the mailbox");
        return 0;
    }
    count = pe ? pe->count : 0;
    rec(b, "begin");
    json_str("tier", b->tier);
    json_int("id", b->id);
    json_str("unit", b->unit);
    json_int("per", (long)b->per);
    if (b->mem)
        json_str("mem", b->mem);
    if (b->kset)
        json_str("kset", b->kset);
    if (count)
        json_int("plan", (long)count);
    json_end();

    if (b->flags & BH_FIXED) {
        count = 1;
        t[n++] = timed(b, 1, "rep", &sum0);
        sum1 = sum0;
        if (b->verify && !b->verify(b->ctx, &sum0)) {
            ok = 0;
            why = "wrong result";
        }
        best = second = t[0];
        conf = 1;
        goto done;
    }

    /* warm-up: translation (JIT), caches; its time is reported as "cold" */
    cold = timed(b, 1, "cold", 0);

    if (!count) {
        /* double until a run is long enough to time: 100 ms of the guest's
         * clock and, with a host, as long as the host asks for; without
         * one, 400 x the guest clock's resolution */
        for (count = 1;; count <<= 1) {
            ULONG tt = timed(b, count, "size", &sum0);
            if (tt >= floor_ticks || count >= 0x40000000UL) {
                a = ask(b, "bench_longer", count, 0);
                if (count >= 0x40000000UL || a < 0 || (a == 0 && tt >= min_ticks)) {
                    t[n++] = tt;
                    /* time(n) / time(n/2) should be 2: the clock follows the work */
                    if (prev >= floor_ticks / 2 && (tt < prev / 25 * 49 || tt > prev / 25 * 51))
                        nonlinear = 1;
                    break;
                }
            }
            prev = tt;
        }
    } else
        t[n++] = timed(b, count, "rep", &sum0);

    if (b->verify && !b->verify(b->ctx, &sum0)) {
        ok = 0;
        why = "wrong result";
    }
    for (;;) {
        conf = confirmed(t, n, &best, &second);
        if (n >= BH_MAX_REPS)
            break;
        if (n >= 2) {
            a = ask(b, "bench_more", count, (ULONG)n);
            if (a < 0 || (a == 0 && conf))
                break;
        }
        t[n++] = timed(b, count, "rep", &sum1);
    }
    if (b->verify) {
        if (!b->verify(b->ctx, &sum1) && ok) {
            ok = 0;
            why = "wrong result";
        }
    } else if (!(b->flags & BH_NOSUM) && sum1 != sum0) {
        ok = 0;
        why = "the repetitions disagree";
    }

done:
    rec(b, "end");
    json_str("st", !ok ? "fail" : conf ? "ok" : "unconfirmed");
    if (why)
        json_str("why", why);
    json_int("n", (long)count);
    json_int("reps", n);
    if (!(b->flags & BH_NOSUM)) {
        json_hex("sum", sum0);
        if (sum1 != sum0)
            json_hex("sum_last", sum1);
    }
    if (nonlinear)
        json_int("nonlinear", 1);
    json_obj("timing");
    json_int("best", (long)best);
    json_int("second", (long)second);
    if (cold)
        json_int("cold", (long)cold);
    json_obj_end();
    json_end();
    return ok;
}
