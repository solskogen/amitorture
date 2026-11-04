/* network: exact-width board register access and small text helpers.
 *
 * Board registers are accessed with inline MOVE.B/MOVE.W/MOVE.L so gcc
 * cannot turn a store into CLR (a read-modify-write on the 68000: a stray
 * read of a FIFO port pops a byte) or merge/split accesses. */
#ifndef NETWORK_HW_H
#define NETWORK_HW_H

#include <exec/types.h>

static inline UBYTE es_rb(volatile UBYTE *p)
{
    UBYTE v;
    __asm__ volatile("move.b %1,%0" : "=d"(v) : "m"(*p) : "memory");
    return v;
}
static inline void es_wb(volatile UBYTE *p, UBYTE v)
{
    __asm__ volatile("move.b %1,%0" : "=m"(*p) : "d"(v) : "memory");
}
static inline UWORD es_rw(volatile UWORD *p)
{
    UWORD v;
    __asm__ volatile("move.w %1,%0" : "=d"(v) : "m"(*p) : "memory");
    return v;
}
static inline void es_ww(volatile UWORD *p, UWORD v)
{
    __asm__ volatile("move.w %1,%0" : "=m"(*p) : "d"(v) : "memory");
}
static inline ULONG es_rl(volatile ULONG *p)
{
    ULONG v;
    __asm__ volatile("move.l %1,%0" : "=d"(v) : "m"(*p) : "memory");
    return v;
}
static inline void es_wl(volatile ULONG *p, ULONG v)
{
    __asm__ volatile("move.l %1,%0" : "=m"(*p) : "d"(v) : "memory");
}

/* ---- a tiny string builder for "in"/"seq" descriptions */
static char es_sb[200];
static char *es_sp;
static void __attribute__((unused)) sb_reset(void)
{
    es_sp = es_sb;
    *es_sp = 0;
}
static void __attribute__((unused)) sb_s(const char *s)
{
    while (*s && es_sp < es_sb + sizeof(es_sb) - 1)
        *es_sp++ = *s++;
    *es_sp = 0;
}
static void __attribute__((unused)) sb_h(ULONG v, int digits)
{
    static const char hx[] = "0123456789abcdef";
    int i;
    if (es_sp >= es_sb + sizeof(es_sb) - 2 - digits)
        return;
    *es_sp++ = '$';
    for (i = digits - 1; i >= 0; i--)
        *es_sp++ = hx[(v >> (i * 4)) & 15];
    *es_sp = 0;
}
static void __attribute__((unused)) sb_d(ULONG v)
{
    char t[12];
    char *e = amt_utoa(t, v);
    (void)e;
    sb_s(t);
}

/* case id: <prefix>_<suffix> */
static char es_idb[64];
static __attribute__((unused)) const char *es_cid(const char *p, const char *s)
{
    char *d = es_idb;
    while (*p && d < es_idb + 30)
        *d++ = *p++;
    *d++ = '_';
    while (*s && d < es_idb + sizeof(es_idb) - 1)
        *d++ = *s++;
    *d = 0;
    return es_idb;
}

#endif
