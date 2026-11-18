// SPDX-License-Identifier: 0BSD
//
// memcpy-large: block copies whose size the compiler can't see, so every
// one goes through the kernel set's memcpy (kshim.c, newlib's algorithm
// compiled for the set): the CPU's write path to RAM, reads and writes in
// longword bursts. From benchwork's memcpy-var-large; its constant-size
// and memmove variants measure how a compiler expands copies, which we
// compile once, so they are not on the bench disk.
//
// One loop copies the 32 KB source half of a 64 KB buffer to the other
// half in pieces of 200 bytes to 4 KB, then once in one piece. The checksum
// covers the destination, so a copy that moves the wrong bytes shows up as
// a different value rather than a different time. Each loop writes the
// same bytes, so the value doesn't depend on the loop count.

#include <exec/types.h>
#include <string.h>

#include "bench.h"

#define BUFSIZE (64UL * 1024)
#define HALF (BUFSIZE / 2)
#define REPS KS_MEMCPY_REPS

static const ULONG SIZES[] = {200, 256, 384, 512, 1024, 2048, 4096};
#define NSIZES (sizeof(SIZES) / sizeof(SIZES[0]))

static UBYTE *buf;

static void memcpy_teardown(void *state) {
    (void)state;
    bench_free(buf);
    buf = NULL;
}

static void *memcpy_setup(void) {
    ULONG i, x = 2463534242UL;

    buf = bench_alloc(BUFSIZE, "block move buffer");
    if (!buf)
        return NULL;
    // Xorshift, so the bytes are not all equal: a copy that drops or
    // repeats a run then shows up in the checksum.
    for (i = 0; i < BUFSIZE; i += 4) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        buf[i] = (UBYTE)(x >> 24);
        buf[i + 1] = (UBYTE)(x >> 16);
        buf[i + 2] = (UBYTE)(x >> 8);
        buf[i + 3] = (UBYTE)x;
    }
    return buf;
}

static const void *memcpy_run(void *state) {
    // read through a volatile pointer, so the sizes stay run-time values
    const volatile ULONG *vsizes = SIZES;
    volatile ULONG whole = HALF;
    UBYTE *src = state, *dst = src + HALF;
    ULONG r, off, i;

    for (r = 0; r < REPS; r++) {
        for (off = 0, i = 0; off < HALF; i = (i + 1) % NSIZES) {
            ULONG n = vsizes[i];
            if (n > HALF - off)
                n = HALF - off;
            memcpy(dst + off, src + off, n);
            off += n;
        }
        memcpy(dst, src, whole);
    }
    return dst;
}

static bool memcpy_verify(void *state, const void *result, ULONG *check) {
    (void)state;
    if (memcmp(result, state, HALF))
        return false;
    *check = checksum(0, result, HALF);
    return true;
}

const struct Bench bench_memcpy_large = {
    /* .name= */ "memcpy-large",
    /* .desc= */ "memcpy of 32 KB in 200-byte to 4 KB pieces, then whole",
    /* .group= */ GROUP_MEMORY,
    /* .unit= */ "KB",
    /* .per_loop= */ 2.0 * REPS * (HALF / 1024),
    /* .setup= */ memcpy_setup,
    /* .run= */ memcpy_run,
    /* .verify= */ memcpy_verify,
    /* .teardown= */ memcpy_teardown,
};
