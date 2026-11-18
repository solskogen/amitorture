/* SPDX-License-Identifier: 0BSD
 *
 * What a kernel provides and what the kernel set gives it: benchwork's
 * bench.h (src/bench.h), cut down to what runs on the bench disk. The
 * kernels are freestanding: no OS calls, memory comes from kshim.c's arena.
 */
#ifndef BENCHWORK_BENCH_H
#define BENCHWORK_BENCH_H

#include <exec/types.h>
#include <stddef.h>

#include "compiler.h"
#include "ksize.h"

enum BenchGroup {
    GROUP_COMPRESSION,
    GROUP_GRAPHICS,
    GROUP_TEXT,
    GROUP_CRYPTO,
    GROUP_MEMORY,
    NGROUPS
};

struct Bench {
    const char *name;
    const char *desc;
    enum BenchGroup group;
    const char *unit;
    double per_loop;
    /* untimed, once: prepare the input; NULL if it can't run */
    void *(*setup)(void);
    /* one loop over the work, timed; where its output is, or NULL */
    const void *(*run)(void *state);
    /* untimed, after a repetition: check the output, reduce it to a checksum */
    bool (*verify)(void *state, const void *result, ULONG *check);
    /* release what setup() allocated; may be NULL */
    void (*teardown)(void *state);
};

/* 32-bit FNV-1a over big-endian words, then tail bytes; seeded so that
 * several buffers can be chained */
ULONG checksum(ULONG seed, const void *data, ULONG len);

void *bench_alloc(ULONG size, const char *what);
void bench_free(void *p);
void *bench_zalloc(void *opaque, unsigned items, unsigned size);
void bench_zfree(void *opaque, void *address);

#endif
