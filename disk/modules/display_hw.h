/* Custom chip register offsets and small helpers shared by the display
 * and copper modules (header only, so each module builds on its own).
 * Register names and offsets: Amiga Hardware Reference Manual appendix A
 * (OCS), appendix C (ECS), and the AGA register documentation. */
#ifndef DISPLAY_HW_H
#define DISPLAY_HW_H

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/nodes.h>
#include <exec/lists.h>
#include "mod.h"

#define CU(o)  (*(volatile UWORD *)(0xdff000UL + (o)))
#define CUL(o) (*(volatile ULONG *)(0xdff000UL + (o)))

#define DMACONR  0x002
#define VPOSR    0x004
#define VHPOSR   0x006
#define JOY0DAT  0x00a
#define JOY1DAT  0x00c
#define CLXDAT   0x00e
#define ADKCONR  0x010
#define INTENAR  0x01c
#define INTREQR  0x01e
#define COPCON   0x02e
#define JOYTEST  0x036
#define BLTCON0  0x040
#define BLTCON1  0x042
#define BLTAFWM  0x044
#define BLTALWM  0x046
#define BLTDPTH  0x054
#define BLTDPTL  0x056
#define BLTSIZE  0x058
#define BLTDMOD  0x066
#define DENISEID 0x07c
#define COP1LCH  0x080
#define COP1LCL  0x082
#define COP2LCH  0x084
#define COP2LCL  0x086
#define COPJMP1  0x088
#define COPJMP2  0x08a
#define DIWSTRT  0x08e
#define DIWSTOP  0x090
#define DDFSTRT  0x092
#define DDFSTOP  0x094
#define DMACON   0x096
#define CLXCON   0x098
#define INTENA   0x09a
#define INTREQ   0x09c
#define ADKCON   0x09e
#define BPL1PTH  0x0e0
#define BPLCON0  0x100
#define BPLCON1  0x102
#define BPLCON2  0x104
#define BPLCON3  0x106
#define BPL1MOD  0x108
#define BPL2MOD  0x10a
#define BPLCON4  0x10c
#define CLXCON2  0x10e
#define SPR0PTH  0x120
#define SPR0POS  0x140
#define SPR0CTL  0x142
#define SPR0DATA 0x144
#define SPR0DATB 0x146
#define COLOR00  0x180
#define DIWHIGH  0x1e4
#define FMODE    0x1fc

/* DMACON */
#define DMAF_SET    0x8000
#define DMAF_BBUSY  0x4000
#define DMAF_BLTPRI 0x0400
#define DMAF_DMAEN  0x0200
#define DMAF_BPLEN  0x0100
#define DMAF_COPEN  0x0080
#define DMAF_BLTEN  0x0040
#define DMAF_SPREN  0x0020

/* exec is frozen and Disable()d in the bare phase: walk the list */
static int __attribute__((unused)) hw_streq(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return *a == *b;
}

/* point the copper back at graphics.library's copinit list (GfxBase+$26),
 * so the OS display comes back after the module */
static void __attribute__((unused)) hw_restore_copper(void)
{
    struct Node *n;
    for (n = SysBase->LibList.lh_Head; n && n->ln_Succ; n = n->ln_Succ)
        if (n->ln_Name && hw_streq(n->ln_Name, "graphics.library")) {
            ULONG ci = *(ULONG *)((UBYTE *)n + 0x26);
            if (ci)
                CUL(COP1LCH) = ci;
            return;
        }
}

/* beam position as (V << 8) | H, V with VPOSR bits 2-0 (V10-V8).
 * VPOSR is read before and after VHPOSR so a V8 change between the two
 * halves is never mixed. Note: at the start of a line H can wrap to 0
 * a little before V counts up (seen as (V, small H) after (V, large H));
 * hw_beam_fix() corrects for that against an earlier sample. */
static inline ULONG __attribute__((unused)) hw_beam(void)
{
    UWORD a, b, vh;
    ULONG polls = 4096;
    do {
        a = CU(VPOSR) & 7;
        vh = CU(VHPOSR);
        b = CU(VPOSR) & 7;
    } while (a != b && --polls);
    if (!polls) {
        amt_set_sr(0x2000);
        amt_abort(AMT_ABORT_WATCHDOG);
    }
    return ((ULONG)a << 16) | vh;
}

/* a sample taken after `prev`: same V but smaller H means the line
 * already ended */
static inline ULONG __attribute__((unused)) hw_beam_fix(ULONG p, ULONG prev)
{
    if ((p >> 8) == (prev >> 8) && (p & 0xff) < (prev & 0xff))
        p += 0x100;
    return p;
}

static inline UWORD __attribute__((unused)) hw_vline(void)
{
    ULONG x = CUL(VPOSR);
    return (UWORD)(((x >> 16) & 7) << 8 | ((x >> 8) & 0xff));
}

/* beam position -> colour clocks from line 0 (227 per line; NTSC long
 * lines are 228, which only matters for slack calculations) */
static inline ULONG __attribute__((unused)) hw_lin(ULONG pos)
{
    return (pos >> 8) * 227UL + (pos & 0xff);
}

#endif
