/* SPDX-License-Identifier: 0BSD
 *
 * The one symbol a kernel set exports into the bench payload. Each set is
 * compiled for its own CPU (k000: 68000; k020: the 68020-68060 common
 * subset, with or without an FPU), partially linked with its own libgcc
 * and shim, and every other symbol is made local, so the sets can't call
 * each other's code by accident. The harness renames `kset` per set.
 */
#ifndef AMT_KSET_H
#define AMT_KSET_H

#include <exec/types.h>

struct Bench;

struct kset_api {
    UWORD version;              /* KSET_VERSION */
    UWORD n;                    /* kernels in `benches` */
    const struct Bench *const *benches;
    /* the memory every bench_alloc() comes from; reset by the harness */
    void (*arena_init)(void *base, ULONG size);
    ULONG (*arena_mark)(void);
    void (*arena_release)(ULONG mark);
    ULONG (*arena_high)(void);  /* highest mark reached since arena_init */
    int (*arena_failed)(void);  /* an allocation didn't fit since arena_init */
};

#define KSET_VERSION 1

#endif
