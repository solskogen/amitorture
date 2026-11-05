#ifndef HARDWARE_AUDIT_H
#define HARDWARE_AUDIT_H

/* Validate in blocks, before converting an untrusted descriptor to bytes. */
static int __attribute__((unused)) hw_owned_domain(ULONG total, ULONG first, ULONG bytes)
{
    return total >= 4 && total <= bytes / 512 &&
           first < total - 1;
}

static int __attribute__((unused)) hw_modulator_period(LONG t)
{
    return (t >= 2800 && t <= 3600) || (t >= 6000 && t <= 6800);
}

static int __attribute__((unused)) hw_stable_complete(ULONG now, ULONG since, int matching)
{
    return matching && now - since >= 2;
}

static int __attribute__((unused)) hw_owner_match(
    ULONG total, ULONG first, ULONG bsize, ULONG kind,
    ULONG total2, ULONG first2, ULONG bsize2, ULONG kind2)
{
    return total == total2 && first == first2 && bsize == 512 && bsize2 == 512 &&
           kind >= 1 && kind <= 4 && kind == kind2;
}

static int __attribute__((unused)) hw_latency_observation_valid(
    int got, int required, int torn, int ambiguous, int overflow)
{
    return required > 0 && got == required && !torn && !ambiguous && !overflow;
}

/* A sticky marker sampled only after completion cannot establish ordering. */
static int __attribute__((unused)) hw_bfd_order(int marker, int busy_after, int saw_done_clear)
{
    if (!marker)
        return 0;
    return busy_after ? -1 : saw_done_clear ? 1 : 0;
}

static int __attribute__((unused)) hw_keyboard_window(ULONG start, ULONG now)
{
    return now - start < 100UL * 3547UL;
}

static int __attribute__((unused)) hw_frame_endpoint(ULONG maxv, ULONG previous,
                                                     ULONG v, ULONG elapsed)
{
    return maxv > 200 && previous == maxv && v < previous && elapsed < 40;
}

#endif
