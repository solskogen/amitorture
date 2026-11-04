/* amitorture OS takeover: save and restore the hardware state the tests touch.
 *
 * Sequence (main.c): Forbid(); Disable(); takeover_save(); takeover_take();
 * ... tests ...; takeover_restore(); Enable(); Permit();
 *
 * Saved: INTENA, DMACON, ADKCON (read through the R registers, written back
 * with SET/CLR = 0x8000), and exception vectors 2-63 at the VBR (that range
 * includes the interrupt autovectors 25-31). The VBR is read with MOVEC only
 * when AttnFlags says 68010 or better. */
#include <proto/exec.h>
#include "rt.h"
#include "mod.h"

static volatile ULONG *vec_base(const struct amt_saved *s)
{
    return (volatile ULONG *)s->vbr;
}

void takeover_save(struct amt_saved *s)
{
    volatile ULONG *v;
    int i;
    s->intena = CREG(R_INTENAR);
    s->dmacon = CREG(R_DMACONR);
    s->adkcon = CREG(R_ADKCONR);
    /* MOVEC VBR when exec flags a 68010+ or the probe found one (an exec
     * that does not flag the CPU, review L3) */
    s->vbr = ((SysBase->AttnFlags & AFF_68010) || amt_probe_result.cpu_family >= 10) ? amt_get_vbr() : 0;
    v = vec_base(s);
    for (i = 2; i < 64; i++)
        s->vec[i] = v[i];
}

void takeover_take(void)
{
    CREG(R_INTENA) = 0x7fff;     /* all interrupts off */
    CREG(R_DMACON) = 0x7fff;     /* all DMA off */
}

void takeover_restore(const struct amt_saved *s)
{
    volatile ULONG *v = vec_base(s);
    int i;
    CREG(R_INTENA) = 0x7fff;
    CREG(R_DMACON) = 0x7fff;
    for (i = 2; i < 64; i++)
        v[i] = s->vec[i];
    CREG(R_ADKCON) = 0x7fff;
    CREG(R_ADKCON) = 0x8000 | s->adkcon;
    /* INTREQ is left alone: requests raised while we owned the machine
     * (VBL, CIA) are serviced by the OS after Enable(). Clearing them would
     * lose CIA interrupts, whose ICR stays set until the OS reads it. */
    CREG(R_DMACON) = 0x8000 | (s->dmacon & 0x07ff);
    CREG(R_INTENA) = 0x8000 | (s->intena & 0x7fff);
}
