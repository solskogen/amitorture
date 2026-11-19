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
static int tod_ok;             /* CIA-B's TOD counts lines: lost frames are put back */
static ULONG frame_cck;        /* colour clocks per frame, as amt_ticks() counts them */
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

/* CIA-B's TOD counter: horizontal syncs, counted by the CIA itself.
 * Reading the high byte latches all three until the low byte is read. */
static ULONG tod_lines(void)
{
    ULONG h = *(volatile UBYTE *)0xbfda00;
    ULONG m = *(volatile UBYTE *)0xbfd900;
    ULONG l = *(volatile UBYTE *)0xbfd800;
    return (h << 16) | (m << 8) | l;
}

static ULONG tod_d = 0xffffffffUL, tod_v;  /* the last check's counts */

/* Does the TOD counter count the beam's lines? Compared with the beam
 * alone over 150 lines of one frame (lines 20-60 to 170-210), so the check
 * depends neither on the VERTB interrupt that amt_ticks() counts frames
 * with nor on when it is served. */
static int tod_try(void)
{
    ULONG v0, v, l0, l1, d;
    do
        v0 = amt_beam() >> 8;
    while (v0 < 20 || v0 > 60);
    l0 = tod_lines();
    do {
        v = amt_beam() >> 8;
        if (v < v0)
            return 0;
    } while (v < v0 + 150);
    l1 = tod_lines();
    d = (l1 - l0) & 0xffffffUL;
    tod_d = d;
    tod_v = v - v0;
    return d + 2 >= v - v0 && d <= v - v0 + 2;
}

static int tod_check(void)
{
    int i;
    for (i = 0; i < 4; i++) {
        if (tod_try())
            return 1;
        /* stopped: a write to the high byte stops the counter until the
         * low byte is written (unless CRB bit 7 selects the alarm) */
        if (i == 1 && tod_d == 0 && !(*(volatile UBYTE *)0xbfdf00 & 0x80)) {
            UBYTE lo = *(volatile UBYTE *)0xbfd800;
            *(volatile UBYTE *)0xbfd800 = lo;
        }
    }
    return 0;
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
        frame_cck = (amt_probe_result.pal ? 313UL : 263UL) * 227UL;
        tod_ok = tod_check();
        floor_ticks = amt_cck_hz() / 10;
        min_ticks = res_ticks > floor_ticks / 400 ? res_ticks * 400 : floor_ticks;
        plan_ok = plan_valid();
        json_begin("bench");
        json_str("ev", "env");
        json_int("tick_hz", (long)amt_cck_hz());
        json_int("res_ticks", (long)res_ticks);
        json_int("min_ticks", (long)min_ticks);
        json_int("tod", tod_ok);
        if (!tod_ok) {
            /* why not: TOD and beam lines over the last check (-1: none) */
            json_int("tod_lines", (long)tod_d);
            json_int("tod_beam", (long)tod_v);
        }
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
    ULONG t0, t1, l0 = 0, l1 = 0, s, d, lost = 0;
    if (++seq > 255)
        seq = 1;
    amt_watchdog_kick();
    wait_frame();
    /* the last record before the run: a host that stamps the arrival of
     * serial bytes has the run's start in its newline, and its end in the
     * first byte of the run record (nothing is sent in between) */
    json_begin("bench");
    json_str("ev", "go");
    json_int("id", b->id);
    json_int("rs", (long)seq);
    json_end();
    set_pos(BH_POS_RUN | ((ULONG)b->id << 8) | seq);
    t0 = amt_ticks();
    if (tod_ok)
        l0 = tod_lines();
    s = b->run(count, b->ctx);
    t1 = amt_ticks();
    if (tod_ok)
        l1 = tod_lines();
    d = t1 - t0;
    /* amt_ticks() counts frames in the VERTB handler; a machine that serves
     * that interrupt more than a frame late loses whole frames. The TOD
     * line count says how many: put them back. */
    if (tod_ok) {
        ULONG cck = ((l1 - l0) & 0xffffffUL) * 227UL;
        if (cck > d + frame_cck / 2) {
            lost = (cck - d + frame_cck / 2) / frame_cck;
            d += lost * frame_cck;
        }
    }
    set_pos(0);
    amt_watchdog_kick();
    if (sum)
        *sum = s;
    rec(b, "run");
    json_str("ph", phase);
    json_int("n", (long)count);
    json_int("rs", (long)seq);
    if (lost)
        json_int("lost_frames", (long)lost);
    json_obj("timing");
    json_int("t", (long)d);
    json_obj_end();
    json_end();
    return d;
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

static const char *note;

void bh_note(const char *s)
{
    note = s;
}

int bh_run(const struct bh_blk *b)
{
    const struct bh_plan_entry *pe = plan_find(b->id);
    ULONG t[BH_MAX_REPS], cold = 0, count, prev = 0, prev2 = 0, sum0 = 0, sum1 = 0, best, second;
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
            /* the clock follows the work: doubling the count doubles the
             * time it adds (a fixed cost per run, as the frame blocks' start
             * inside a frame, adds nothing), judged at every doubling long
             * enough to time. Shorter runs are noise: one can be faster
             * than the run before it. The slack, 2 % plus 1/16 frame, covers
             * per-frame costs (interrupts) that a run picks up depending on
             * where in the frame it starts; a lost frame is far outside it. */
            if (count >= 4 && prev >= prev2 && prev - prev2 >= floor_ticks / 8 &&
                prev - prev2 >= res_ticks * 100) {
                ULONG d1 = prev - prev2, d2 = tt - prev, slack = d1 / 25 + frame_cck / 16;
                if (tt < prev || d2 + slack < 2 * d1 || d2 > 2 * d1 + slack)
                    nonlinear = 1;
            } else if (prev >= floor_ticks / 8 && (prev2 > prev || tt < prev))
                nonlinear = 1;
            if (tt >= floor_ticks || count >= 0x40000000UL) {
                a = ask(b, "bench_longer", count, 0);
                if (count >= 0x40000000UL || a < 0 || (a == 0 && tt >= min_ticks)) {
                    t[n++] = tt;
                    break;
                }
            }
            prev2 = prev;
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
    /* without a host the guest's clock is the only one: a clock that did
     * not follow the work leaves the time unconfirmed */
    json_str("st", !ok ? "fail" : conf && !(nonlinear && !host) ? "ok" : "unconfirmed");
    if (why)
        json_str("why", why);
    if (note)
        json_str("note", note);
    note = 0;
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
