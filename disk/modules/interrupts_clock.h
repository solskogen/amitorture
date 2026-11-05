#ifndef INTERRUPTS_CLOCK_H
#define INTERRUPTS_CLOCK_H

struct ir_clock {
    ULONG start, previous, sample, elapsed;
    int invalid;
};

static void ir_clock_init(struct ir_clock *c, ULONG now)
{
    c->start = c->previous = c->sample = now;
    c->elapsed = 0;
    c->invalid = 0;
}

/* The storm lasts at most five seconds, far below half a 32-bit E-clock
 * wrap. Equal samples and a genuine wrap are valid; a backward sample is
 * not elapsed time and must never reach an unsigned timeout comparison. */
static int ir_clock_sample(struct ir_clock *c, ULONG now)
{
    if (c->invalid)
        return 0;
    c->sample = now;
    if ((LONG)(now - c->previous) < 0) {
        c->invalid = 1;
        return 0;
    }
    c->previous = now;
    c->elapsed = now - c->start;
    return 1;
}

#endif
