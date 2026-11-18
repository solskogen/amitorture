/* SPDX-License-Identifier: 0BSD
 *
 * The freestanding C library the kernels link against: memory and string
 * functions, benchwork's checksum, and an arena allocator in place of
 * AllocVec(). Compiled once per kernel set, with that set's CPU flags, so
 * memcpy-large measures a memcpy compiled for the set it runs in.
 *
 * The arena is a bump allocator. bench_free() only gives memory back when
 * it is the most recent block; the harness takes a mark after setup() and
 * releases to it after every loop, so a loop's allocations (zlib's state
 * and window) land at the same addresses every time.
 */
#include <exec/types.h>
#include <stddef.h>
/* not <string.h>: libnix defines some of these inline there */

#include "bench.h"

/* keep GCC from turning these loops into calls to themselves */
#define NOLIB __attribute__((optimize("no-tree-loop-distribute-patterns")))

static UBYTE *arena_base;
static ULONG arena_size, arena_top, arena_peak;
static int arena_oom;

void k_arena_init(void *base, ULONG size)
{
    arena_base = base;
    arena_size = size;
    arena_top = arena_peak = 0;
    arena_oom = 0;
}

ULONG k_arena_mark(void) { return arena_top; }
void k_arena_release(ULONG mark) { if (mark <= arena_top) arena_top = mark; }
ULONG k_arena_high(void) { return arena_peak; }
int k_arena_failed(void) { return arena_oom; }

/* each block has a 16-byte header (its size in the last longword), so
 * blocks stay 16-byte aligned and the most recent one can be freed */
void *bench_alloc(ULONG size, const char *what)
{
    ULONG need = (size + 16 + 15) & ~15UL;
    UBYTE *b;
    (void)what;
    if (!arena_base || need < size || need > arena_size - arena_top) {
        arena_oom = 1;
        return NULL;
    }
    b = arena_base + arena_top + 16;
    arena_top += need;
    if (arena_top > arena_peak)
        arena_peak = arena_top;
    ((ULONG *)b)[-1] = need;
    return b;
}

void bench_free(void *p)
{
    UBYTE *b = p;
    ULONG need;
    if (!b)
        return;
    need = ((ULONG *)b)[-1];
    if (b - 16 + need == arena_base + arena_top)
        arena_top -= need;
}

void *bench_zalloc(void *opaque, unsigned items, unsigned size)
{
    (void)opaque;
    return bench_alloc((ULONG)items * size, "library buffer");
}

void bench_zfree(void *opaque, void *address)
{
    (void)opaque;
    bench_free(address);
}

/* FNV-1a over big-endian 32-bit words (any alignment), then the tail
 * bytes: a quarter of byte-wise FNV's multiplies, which are calls on the
 * 68000. Not benchwork's value, which is byte-wise. */
ULONG checksum(ULONG seed, const void *data, ULONG len)
{
    const UBYTE *p = data;
    ULONG h = seed ? seed : 2166136261UL;
    for (; len >= 4; len -= 4, p += 4)
        h = (h ^ ((ULONG)p[0] << 24 | (ULONG)p[1] << 16 | (ULONG)p[2] << 8 | p[3]))
            * 16777619UL;
    while (len--)
        h = (h ^ *p++) * 16777619UL;
    return h;
}

/* newlib's C memcpy, in short: longword moves, four at a time, when both
 * pointers are even (the 68000 needs no more than that), bytes otherwise */
NOLIB void *memcpy(void *restrict dst, const void *restrict src, size_t n)
{
    UBYTE *d = dst;
    const UBYTE *s = src;
    if (n >= 16 && !(((ULONG)d | (ULONG)s) & 1)) {
        ULONG *dl = (ULONG *)d;
        const ULONG *sl = (const ULONG *)s;
        while (n >= 16) {
            *dl++ = *sl++;
            *dl++ = *sl++;
            *dl++ = *sl++;
            *dl++ = *sl++;
            n -= 16;
        }
        while (n >= 4) {
            *dl++ = *sl++;
            n -= 4;
        }
        d = (UBYTE *)dl;
        s = (const UBYTE *)sl;
    }
    while (n--)
        *d++ = *s++;
    return dst;
}

NOLIB void *memmove(void *dst, const void *src, size_t n)
{
    UBYTE *d = dst;
    const UBYTE *s = src;
    if (d <= s || d >= s + n)
        return memcpy(dst, src, n);
    d += n;
    s += n;
    while (n--)
        *--d = *--s;
    return dst;
}

NOLIB void *memset(void *dst, int c, size_t n)
{
    UBYTE *d = dst;
    while (n--)
        *d++ = (UBYTE)c;
    return dst;
}

NOLIB int memcmp(const void *a, const void *b, size_t n)
{
    const UBYTE *p = a, *q = b;
    for (; n; n--, p++, q++)
        if (*p != *q)
            return *p - *q;
    return 0;
}

NOLIB size_t strlen(const char *s)
{
    const char *p = s;
    while (*p)
        p++;
    return p - s;
}

NOLIB char *strcpy(char *restrict d, const char *restrict s)
{
    char *r = d;
    while ((*d++ = *s++))
        ;
    return r;
}

NOLIB int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b)
        a++, b++;
    return (UBYTE)*a - (UBYTE)*b;
}

/* Dhrystone's two records */
void *malloc(size_t n)
{
    return bench_alloc(n, "malloc");
}

void free(void *p)
{
    bench_free(p);
}
