/* 8520 CIA access and save/restore, shared by the cia and interrupts
 * modules (header only, so each module builds on its own).
 *
 * Register access goes through inline asm MOVE.B on purpose: gcc may emit
 * CLR.B or a read-modify-write for a plain C store, and on the 68000 CLR.B
 * reads the location first. A stray read of ICR clears the pending
 * interrupt flags, and a stray read of TODHI latches the TOD clock.
 *
 * The 8520 has write-only state we cannot read back:
 *   - the ICR mask: taken from ciaX.resource AbleICR(0), the OS's own copy;
 *   - the timer latches: a stopped timer has counter == latch after a
 *     one-shot underflow (the latch is reloaded on every underflow), so the
 *     counter is used; a running continuous timer is sampled until it
 *     wraps and the highest value seen right after the wrap is used;
 *   - the TOD alarm: the OS value is unknown. When the TOD is restored
 *     (save_tod), the alarm is set to TOD-1, just behind the restored
 *     clock, so it matches again only after a full 24-bit wrap (CIA-B,
 *     HSYNC: ~18 minutes; CIA-A, 50/60 Hz: ~93 hours) instead of at
 *     whatever test value a module left.
 * OS interrupt events that were pending when we drained ICR are re-posted
 * at restore time for TA/TB (a 1-count one-shot) and ALRM (alarm = TOD+1),
 * when the OS had them enabled in the mask. SP/FLG events cannot be
 * re-posted. A keyboard byte seen while we own CIA-A is acknowledged
 * (handshake) so the keyboard does not hang; the key itself is lost. */
#ifndef CIA_HW_H
#define CIA_HW_H

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/nodes.h>
#include <exec/lists.h>
#include "mod.h"

#define CIAA_BASE ((volatile UBYTE *)0xbfe001)
#define CIAB_BASE ((volatile UBYTE *)0xbfd000)

#define C_PRA    0x000
#define C_PRB    0x100
#define C_DDRA   0x200
#define C_DDRB   0x300
#define C_TALO   0x400
#define C_TAHI   0x500
#define C_TBLO   0x600
#define C_TBHI   0x700
#define C_TODLO  0x800
#define C_TODMID 0x900
#define C_TODHI  0xa00
#define C_SDR    0xc00
#define C_ICR    0xd00
#define C_CRA    0xe00
#define C_CRB    0xf00

/* CRA/CRB bits */
#define CR_START   0x01
#define CR_PBON    0x02
#define CR_OUTMODE 0x04
#define CR_RUNMODE 0x08   /* 1 = one-shot */
#define CR_LOAD    0x10   /* force load strobe */
#define CR_INMODE  0x20   /* CRA: count CNT; CRB: INMODE bit 0 */
#define CR_SPMODE  0x40   /* CRA: serial port output; CRB: INMODE bit 1 */
#define CR_ALARM   0x80   /* CRB: TOD writes go to the alarm */

/* ICR bits */
#define ICR_TA   0x01
#define ICR_TB   0x02
#define ICR_ALRM 0x04
#define ICR_SP   0x08
#define ICR_FLG  0x10
#define ICR_IR   0x80

static inline UBYTE cia_rd(volatile UBYTE *c, UWORD reg)
{
    UBYTE v;
    __asm__ volatile("move.b %1,%0" : "=d"(v) : "m"(c[reg]) : "memory");
    return v;
}

static inline void cia_wr(volatile UBYTE *c, UWORD reg, UBYTE v)
{
    __asm__ volatile("move.b %1,%0" : "=m"(c[reg]) : "d"(v) : "memory");
}

/* a running timer: read hi, lo, hi until the two hi reads agree */
static inline UWORD cia_timer(volatile UBYTE *c, UWORD lo, UWORD hi)
{
    UBYTE h1, l, h2;
    do {
        h1 = cia_rd(c, hi);
        l = cia_rd(c, lo);
        h2 = cia_rd(c, hi);
    } while (h1 != h2);
    return (UWORD)((h1 << 8) | l);
}

/* Colour clocks since takeover, like amt_ticks(), but (a) corrected for
 * the window between the beam wrapping to line 0 and the VERTB handler
 * counting the frame (amt_ticks() goes back by a whole frame there, and
 * an unsigned "now - t0 > timeout" then expires at once), and (b) clamped
 * to be monotonic. Needs VERTB serviced at least once per frame. */
/* Beam position from one VHPOSR read, with V8-V10 from VPOSR read before
 * and after it: V8 lives in another register, so reading VPOSR and VHPOSR
 * once each tears when the beam crosses line 255->256 (V8=0 with VHPOSR's
 * V7-V0 already 0: line 0) or 312->0 (line 256). Retried until both VPOSR
 * reads agree, so the pair belongs to the VHPOSR sample. */
static ULONG __attribute__((unused)) cia_beam(ULONG *hp)
{
    UWORD p1, hv, p2;
    do {
        p1 = CREG(R_VPOSR);
        hv = CREG(R_VHPOSR);
        p2 = CREG(R_VPOSR);
    } while ((p1 ^ p2) & 7);
    if (hp)
        *hp = hv & 0xff;
    return ((ULONG)(p1 & 7) << 8) | (hv >> 8);
}

static ULONG cia_last_now __attribute__((unused));
static ULONG __attribute__((unused)) cia_now(void)
{
    ULONG f1, f2, v, h, t;
    UWORD rq;
    ULONG lines = amt_probe_result.pal ? 313 : 263;
    do {
        f1 = amt_frames_now();
        rq = CREG(R_INTREQR);
        v = cia_beam(&h);
        f2 = amt_frames_now();
    } while (f1 != f2);
    if ((rq & 0x0020) && v < lines / 2)
        f1++;
    t = (f1 * lines + v) * 227UL + h;
    if ((LONG)(t - cia_last_now) < 0)
        t = cia_last_now;
    else
        cia_last_now = t;
    return t;
}

/* keyboard bytes acknowledged while a module owned CIA-A */
static ULONG cia_kbd_acks __attribute__((unused));

static void __attribute__((unused)) cia_wait_cc(ULONG cc)
{
    ULONG t0 = cia_now();
    while (cia_now() - t0 < cc)
        ;
}

/* Keyboard handshake: KDAT low for >= 85 us by putting the serial port
 * in output mode (HRM, keyboard chapter). 3 lines (~190 us). */
static void __attribute__((unused)) cia_kbd_handshake(void)
{
    UBYTE cra;
    (void)cia_rd(CIAA_BASE, C_SDR);
    cra = cia_rd(CIAA_BASE, C_CRA) & (UBYTE)~CR_LOAD;
    cia_wr(CIAA_BASE, C_CRA, cra | CR_SPMODE);
    cia_wait_cc(3 * 227);
    cra = cia_rd(CIAA_BASE, C_CRA) & (UBYTE)~CR_LOAD;
    cia_wr(CIAA_BASE, C_CRA, cra & (UBYTE)~CR_SPMODE);
    cia_kbd_acks++;
}

/* ICR read for test code: on CIA-A, a keyboard byte (SP with the serial
 * port in input mode) is acknowledged at once. */
static UBYTE __attribute__((unused)) cia_icr(volatile UBYTE *c)
{
    UBYTE v = cia_rd(c, C_ICR);
    if (c == CIAA_BASE && (v & ICR_SP) && !(cia_rd(c, C_CRA) & CR_SPMODE))
        cia_kbd_handshake();
    return v;
}

/* 1 if INTREQ `bit` (PORTS $0008 / EXTER $2000) comes back right after
 * being cleared while both CIA interrupt masks are clear and ICR drained
 * (call after cia_save() of both CIAs): another device holds INT2/INT6
 * asserted (CD32 Akiko, an IDE/Zorro board...), so that request bit cannot
 * be controlled by the tests. */
static int __attribute__((unused)) cia_int_line_stuck(UWORD bit)
{
    int i, n = 0;
    for (i = 0; i < 3; i++) {
        CREG(R_INTREQ) = bit;
        CREG(R_INTREQ) = bit;
        cia_wait_cc(2 * 227);
        if (CREG(R_INTREQR) & bit)
            n++;
    }
    CREG(R_INTREQ) = bit;
    return n != 0;
}

/* The same check later in a module, after a case that compares PORTS/EXTER
 * failed: masks both CIAs' ICR and drains them first (a keyboard byte is
 * acknowledged), and keeps the INTENA bit off meanwhile so no handler runs.
 * 1 = a device started holding the line after the module-start check (an
 * IDE drive finishing a command, a network board...): the failed case is
 * untestable, not a fail. */
static int __attribute__((unused)) cia_int_line_recheck(UWORD bit)
{
    UWORD ena = CREG(R_INTENAR) & bit;
    int r;
    CREG(R_INTENA) = bit;
    cia_wr(CIAA_BASE, C_ICR, 0x7f);
    cia_wr(CIAB_BASE, C_ICR, 0x7f);
    (void)cia_icr(CIAA_BASE);
    (void)cia_icr(CIAB_BASE);
    r = cia_int_line_stuck(bit);
    if (ena)
        CREG(R_INTENA) = 0x8000 | ena;
    return r;
}

#define CIA_STUCK_LATE_WHY_PORTS "the case failed, and a re-check then found INT2 (INTREQ PORTS) held " \
    "asserted with both CIA interrupt masks clear and ICR drained: another device (IDE/Zorro board, " \
    "network card...) started asserting level 2 during the module, so the result is not attributable"
#define CIA_STUCK_LATE_WHY_EXTER "the case failed, and a re-check then found INT6 (INTREQ EXTER) held " \
    "asserted with both CIA interrupt masks clear and ICR drained: another device (Zorro board...) " \
    "started asserting level 6 during the module, so the result is not attributable"

#define CIA_STUCK_WHY_PORTS "INT2 (INTREQ PORTS) is held asserted by another device with both CIA " \
    "interrupt masks clear (e.g. CD32 Akiko, IDE/Zorro board): level-2 requests cannot be controlled"
#define CIA_STUCK_WHY_EXTER "INT6 (INTREQ EXTER) is held asserted by another device with both CIA " \
    "interrupt masks clear (e.g. CD32 Akiko, Zorro board): level-6 requests cannot be controlled"

/* ---- OS resource lookup (exec is frozen, Disable()d: just walk the list) */
static int cia_streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

static void *cia_find_resource(const char *name)
{
    struct Node *n;
    for (n = SysBase->ResourceList.lh_Head; n && n->ln_Succ; n = n->ln_Succ)
        if (n->ln_Name && cia_streq(n->ln_Name, name))
            return n;
    return 0;
}

/* cia.resource AbleICR(mask) (LVO -18): with mask 0 it returns the current
 * enable mask without changing it. Its Disable()/Enable() pair nests inside
 * the runtime's Disable(), but the DISABLE half writes INTENA=$4000, so
 * INTEN is set again afterwards. */
static UBYTE cia_able_icr(void *res, UBYTE mask)
{
    register ULONG d0 __asm("d0") = mask;
    register void *a6 __asm("a6") = res;
    __asm__ volatile("jsr -18(%%a6)"
                     : "+d"(d0)
                     : "a"(a6)
                     : "d1", "a0", "a1", "memory", "cc");
    *(volatile UWORD *)0xdff09a = 0xc000;
    return (UBYTE)d0;
}

/* ---- save / restore */
struct cia_saved {
    volatile UBYTE *c;
    UBYTE cra, crb;        /* CR as found (after one-shots finished) */
    UBYTE osmask;          /* OS ICR enable mask */
    UBYTE pending;         /* OS events drained from ICR at save */
    UBYTE have_res;
    UBYTE tod_saved;
    UWORD la, lb;          /* latch values (see above) */
    ULONG tod;             /* TOD at save */
    ULONG frames0, ticks0; /* time of the TOD save */
};

static UWORD cia_latch_estimate(volatile UBYTE *c, UWORD lo, UWORD hi, UWORD cr)
{
    UBYTE crv = cia_rd(c, cr);
    ULONG t0;
    UWORD prev, v, best = 0;
    int after = 0;
    if (!(crv & CR_START))
        return cia_timer(c, lo, hi);
    if (crv & CR_RUNMODE) {
        /* one-shot running: let it finish (<= 65536 E = 93 ms) */
        t0 = cia_now();
        while ((cia_rd(c, cr) & CR_START) && cia_now() - t0 < 600000UL)
            ;
        if (!(cia_rd(c, cr) & CR_START))
            return cia_timer(c, lo, hi);
    }
    /* continuous: watch for a wrap, then keep the highest of a few samples */
    t0 = cia_now();
    prev = cia_timer(c, lo, hi);
    while (cia_now() - t0 < 1200000UL) {
        v = cia_timer(c, lo, hi);
        if (v > prev)
            after = 1;
        if (after) {
            if (v > best)
                best = v;
            if (++after > 4)
                break;
        }
        prev = v;
    }
    return after ? best : prev;
}

static ULONG cia_tod_read(volatile UBYTE *c)
{
    ULONG h, m, l;
    h = cia_rd(c, C_TODHI);
    m = cia_rd(c, C_TODMID);
    l = cia_rd(c, C_TODLO);
    return (h << 16) | (m << 8) | l;
}

static void cia_tod_write(volatile UBYTE *c, ULONG v)
{
    cia_wr(c, C_TODHI, (UBYTE)(v >> 16));
    cia_wr(c, C_TODMID, (UBYTE)(v >> 8));
    cia_wr(c, C_TODLO, (UBYTE)v);
}

static void __attribute__((unused)) cia_alarm_write(volatile UBYTE *c, ULONG v)
{
    UBYTE crb = cia_rd(c, C_CRB) & (UBYTE)~CR_LOAD;
    cia_wr(c, C_CRB, crb | CR_ALARM);
    cia_tod_write(c, v);
    cia_wr(c, C_CRB, crb & (UBYTE)~CR_ALARM);
}

static void cia_save(struct cia_saved *s, volatile UBYTE *c, int save_tod)
{
    void *res = cia_find_resource(c == CIAA_BASE ? "ciaa.resource" : "ciab.resource");
    s->c = c;
    s->have_res = res != 0;
    s->osmask = res ? (cia_able_icr(res, 0) & 0x1f) : 0;
    s->la = cia_latch_estimate(c, C_TALO, C_TAHI, C_CRA);
    s->lb = cia_latch_estimate(c, C_TBLO, C_TBHI, C_CRB);
    s->cra = cia_rd(c, C_CRA) & (UBYTE)~CR_LOAD;
    s->crb = cia_rd(c, C_CRB) & (UBYTE)~CR_LOAD;
    cia_wr(c, C_ICR, 0x7f);
    s->pending = cia_rd(c, C_ICR) & 0x1f;
    if (c == CIAA_BASE && (s->pending & ICR_SP) && !(s->cra & CR_SPMODE))
        cia_kbd_handshake();
    s->tod_saved = save_tod;
    if (save_tod) {
        /* TOD reads need CRB.ALARM = 0 to be plain reads */
        if (s->crb & CR_ALARM)
            cia_wr(c, C_CRB, s->crb & (UBYTE)~CR_ALARM);
        s->frames0 = amt_frames_now();
        s->ticks0 = cia_now();
        s->tod = cia_tod_read(c);
    }
    /* both timers stopped, continuous, phi2, TOD write mode, SP input */
    cia_wr(c, C_CRA, 0);
    cia_wr(c, C_CRB, 0);
    (void)cia_icr(c);
}

/* re-post a timer event: 1-count one-shot, wait for it */
static void cia_repost_timer(volatile UBYTE *c, UWORD lo, UWORD hi, UWORD cr)
{
    ULONG t0;
    cia_wr(c, cr, 0);
    cia_wr(c, lo, 1);
    cia_wr(c, hi, 0);
    cia_wr(c, cr, CR_RUNMODE | CR_LOAD | CR_START);
    t0 = cia_now();
    while ((cia_rd(c, cr) & CR_START) && cia_now() - t0 < 10000)
        ;
    cia_wr(c, cr, 0);
}

static void cia_restore(struct cia_saved *s)
{
    volatile UBYTE *c = s->c;
    UBYTE ev = s->pending & s->osmask;
    cia_wr(c, C_CRA, 0);
    cia_wr(c, C_CRB, 0);
    cia_wr(c, C_ICR, 0x7f);
    if (s->tod_saved) {
        ULONG el;
        if (c == CIAA_BASE)
            el = amt_frames_now() - s->frames0;
        else
            el = (cia_now() - s->ticks0) / 227;
        cia_tod_write(c, (s->tod + el) & 0xffffff);
    }
    if (ev & ICR_TA)
        cia_repost_timer(c, C_TALO, C_TAHI, C_CRA);
    if (ev & ICR_TB)
        cia_repost_timer(c, C_TBLO, C_TBHI, C_CRB);
    if (ev & ICR_ALRM) {
        /* alarm one tick ahead, then wait two ticks (no ICR read) */
        cia_alarm_write(c, (cia_tod_read(c) + 1) & 0xffffff);
        cia_wait_cc(c == CIAA_BASE ? 2 * 71051UL : 3 * 227UL);
    }
    if (s->tod_saved)   /* park the alarm just behind the clock (see top) */
        cia_alarm_write(c, (cia_tod_read(c) - 1) & 0xffffff);
    /* stopped + continuous: a high-byte write loads the counter as well */
    cia_wr(c, C_TALO, (UBYTE)s->la);
    cia_wr(c, C_TAHI, (UBYTE)(s->la >> 8));
    cia_wr(c, C_TBLO, (UBYTE)s->lb);
    cia_wr(c, C_TBHI, (UBYTE)(s->lb >> 8));
    if (s->osmask)
        cia_wr(c, C_ICR, 0x80 | s->osmask);
    cia_wr(c, C_CRA, s->cra);
    cia_wr(c, C_CRB, s->crb);
}

#endif
