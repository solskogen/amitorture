/* misc_hw: shared helpers of the misc_hw module (id 19, bare phase) and its
 * parts misc_hw_rtc.c, misc_hw_gayle.c, misc_hw_cd.c. The OS-phase module
 * misc_hw_os (id 24) is self-contained and does not use this header.
 *
 * Hardware access goes through inline asm on purpose: gcc may turn a plain C
 * store into CLR (which reads first on the 68000) or a read-modify-write,
 * and several registers here have read side effects (the Gayle ID counter,
 * IDE status clears INTRQ, DMAC ISTR clears its flags). */
#ifndef MISC_HW_H
#define MISC_HW_H

#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include "mod.h"

static inline UBYTE hw_rd8(ULONG a)
{
    UBYTE v;
    __asm__ volatile("move.b (%1),%0" : "=d"(v) : "a"(a) : "memory");
    return v;
}
static inline void hw_wr8(ULONG a, UBYTE v)
{
    __asm__ volatile("move.b %1,(%0)" : : "a"(a), "d"(v) : "memory");
}
static inline UWORD hw_rd16(ULONG a)
{
    UWORD v;
    __asm__ volatile("move.w (%1),%0" : "=d"(v) : "a"(a) : "memory");
    return v;
}
static inline void hw_wr16(ULONG a, UWORD v)
{
    __asm__ volatile("move.w %1,(%0)" : : "a"(a), "d"(v) : "memory");
}
static inline ULONG hw_rd32(ULONG a)
{
    ULONG v;
    __asm__ volatile("move.l (%1),%0" : "=d"(v) : "a"(a) : "memory");
    return v;
}
static inline void hw_wr32(ULONG a, ULONG v)
{
    __asm__ volatile("move.l %1,(%0)" : : "a"(a), "d"(v) : "memory");
}

/* misc_hw.c */
void  mh_wait_cck(ULONG cck);             /* busy wait, colour clocks */
void  mh_wait_ms(ULONG ms);
ULONG mh_ms_since(ULONG t0);              /* amt_ticks() based */
ULONG mh_cck_ms(ULONG ms);                /* ms -> colour clocks */
const char *mh_id(const char *a, const char *b);   /* "a_b" in a static buffer */
char *mh_hex(char *d, ULONG v, int digits);
char *mh_cpy(char *d, const char *s);
int   mh_is_aros(void);
/* exec lists, walked read-only (bare phase: no exec calls) */
void *mh_find_name(struct List *l, const char *name);
int   mh_ram_overlaps(ULONG lo, ULONG hi);  /* any MemHeader in [lo,hi) */
/* try a read that may bus-error: 1 and *v on success */
int   mh_try_rd16(ULONG a, UWORD *v);

/* parts */
void mh_rtc(void);       /* misc_hw_rtc.c */
void mh_gayle(void);     /* misc_hw_gayle.c */
void mh_akiko(void);     /* misc_hw_cd.c */
void mh_cdtv(void);      /* misc_hw_cd.c */

#endif
