/* paula module: shared declarations for paula.c (audio) and paula_disk.c
 * (disk controller). See paula.c for the overview. */
#ifndef PAULA_H
#define PAULA_H

#include "mod.h"

/* ---- custom registers */
#define R_DSKPTH   0x020
#define R_DSKLEN   0x024
#define R_DSKBYTR  0x01a
#define R_DSKSYNC  0x07e
#define AUD_BASE   0x0a0
#define AUDLC(c)   (*(volatile ULONG *)(CUSTOM_BASE + AUD_BASE + (c) * 16))
#define AUDLEN(c)  CREG(AUD_BASE + 4 + (c) * 16)
#define AUDPER(c)  CREG(AUD_BASE + 6 + (c) * 16)
#define AUDVOL(c)  CREG(AUD_BASE + 8 + (c) * 16)
#define AUDDAT(c)  CREG(AUD_BASE + 10 + (c) * 16)
#define AUDREG(c, o) ((volatile UWORD *)(CUSTOM_BASE + AUD_BASE + (o) + (c) * 16))

#define INT_DSKBLK 0x0002
#define INT_VERTB  0x0020
#define INT_AUD(c) (0x0080 << (c))
#define INT_DSKSYN 0x1000

/* ---- CIA (byte access through MOVE.B: see cia_hw.h for why) */
#define CIAA ((volatile UBYTE *)0xbfe001)
#define CIAB ((volatile UBYTE *)0xbfd000)
#define C_PRA  0x000
#define C_PRB  0x100
#define C_DDRA 0x200
#define C_DDRB 0x300

static inline UBYTE pa_cia_rd(volatile UBYTE *c, UWORD reg)
{
    UBYTE v;
    __asm__ volatile("move.b %1,%0" : "=d"(v) : "m"(c[reg]) : "memory");
    return v;
}

static inline void pa_cia_wr(volatile UBYTE *c, UWORD reg, UBYTE v)
{
    __asm__ volatile("move.b %1,%0" : "=m"(c[reg]) : "d"(v) : "memory");
}

/* ---- time: colour clocks from the beam + VBL count, corrected for a frame
 * wrap the VERTB handler has not counted yet (as cia_hw.h cia_now) */
ULONG pa_now(void);                 /* monotonic, main code only */
ULONG pa_now_raw(void);             /* no clamp, usable in interrupt hooks */
void  pa_wait_cc(ULONG cc);
#define PA_MS(n) ((ULONG)(n) * 3547UL)

/* ---- shared chip buffers (paula.c) */
extern UBYTE *pa_chip;              /* amt_chip_pool() */
#define PA_AUDBUF_OFF   0           /* 1024 bytes of audio data */
#define PA_RDBUF_OFF    1024        /* raw read buffer, PA_RDWORDS words */
#define PA_RDWORDS      7000
#define PA_WRBUF_OFF    (1024 + PA_RDWORDS * 2)
#define PA_WRWORDS      6200
#define PA_CHIP_NEED    (PA_WRBUF_OFF + PA_WRWORDS * 2 + 64)

/* ---- small helpers */
char *pa_cat(char *d, const char *s);
const char *pa_id(const char *a, int n, const char *b);   /* a<n>b, static buffer */

void pa_audio_tests(void);
void pa_disk_tests(void);
void pa_disk_restore(void);         /* every path, also after an abort */

#endif
