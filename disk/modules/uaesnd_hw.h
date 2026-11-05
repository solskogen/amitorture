/* uaesnd (the UAE sound board, manufacturer 6502, product 2): register
 * offsets and the sample-set writer, shared by expsound (the board's own
 * tests) and paula (a Paula channel mixed with a uaesnd stream). The
 * interface is specified in the comment block of Amiberry's sndboard.cpp.
 * Board access goes through the exact-width accessors of expsound_hw.h. */
#ifndef UAESND_HW_H
#define UAESND_HW_H

#include "expsound_hw.h"

#define UAESND_MAN        6502
#define UAESND_PROD       2

#define UAESND_RAM_OFFSET 0x0090   /* .L board RAM offset */
#define UAESND_RAM_SIZE   0x0094   /* .L board RAM size */
#define UAESND_ALLOC      0x00e0   /* .L allocated streams mask: a stream plays only once allocated */
#define UAESND_ENABLE     0x00f0   /* .L stream enable mask */
#define UAESND_STREAM0    0x0100   /* stream 1 register block */
#define UAESND_S_SETPTR   0x0080   /* .L sample set pointer: non-zero starts, 0 stops */
#define UAESND_S_STATUS   0x0093   /* .B bit 0: playing */

/* The RAM offset and size the specification gives: 32 KB at $8000 on the
 * Z2 board, 8 MB at $800000 on the Z3 board. */
static inline ULONG uaesnd_ram_expected(ULONG board_size)
{
    return board_size > 0x10000 ? 0x800000UL : 0x8000UL;
}

/* Writes a 48-byte sample set (8-bit signed mono, volume 8192/32768, no
 * next set) at ram + so and returns its address. */
static inline ULONG uaesnd_set_write(ULONG ram, ULONG so, ULONG data, LONG len, ULONG freq, ULONG rep,
                                     LONG replen, UWORD repcnt, UWORD flags, UBYTE intr)
{
    volatile UBYTE *p = (volatile UBYTE *)(ram + so);
    int i;
    for (i = 0; i < 48; i += 4)
        es_wl((volatile ULONG *)(p + i), 0);
    es_ww((volatile UWORD *)(p + 0), flags);
    es_ww((volatile UWORD *)(p + 2), 0);
    es_wl((volatile ULONG *)(p + 4), data);
    es_wl((volatile ULONG *)(p + 8), (ULONG)len);
    es_wl((volatile ULONG *)(p + 12), freq);
    es_wl((volatile ULONG *)(p + 16), rep);
    es_wl((volatile ULONG *)(p + 20), (ULONG)replen);
    es_ww((volatile UWORD *)(p + 24), repcnt);
    es_ww((volatile UWORD *)(p + 26), 0x2000);   /* volume 8192/32768 */
    es_wl((volatile ULONG *)(p + 28), 0);         /* next: none */
    es_wb(p + 32, 1);                             /* mono */
    es_wb(p + 33, 0x40);                          /* 8-bit signed */
    es_wb(p + 34, intr);
    return ram + so;
}

#endif
