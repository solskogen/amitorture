/* SPDX-License-Identifier: 0BSD
 *
 * The kernels of one set, and the set's one exported symbol. Which kernels
 * a set has follows from its build:
 *   KSET_INT    the integer kernels (k000, k020)
 *   KSET_FLOAT  the float kernels (k000 soft float, k020f FPU, k020s soft)
 */
#include "bench.h"
#include "kset.h"

void k_arena_init(void *base, ULONG size);
ULONG k_arena_mark(void);
void k_arena_release(ULONG mark);
ULONG k_arena_high(void);
int k_arena_failed(void);

extern const struct Bench bench_dhry, bench_scene_2d, bench_zlib_deflate, bench_zlib_inflate,
    bench_ftgrays, bench_memcpy_large, bench_aesgcm, bench_chacha20poly1305,
    bench_nfs3_project, bench_nfs3_clip;

static const struct Bench *const list[] = {
#ifdef KSET_INT
    &bench_dhry,
    &bench_scene_2d,
    &bench_zlib_deflate,
    &bench_zlib_inflate,
    &bench_ftgrays,
    &bench_memcpy_large,
    &bench_aesgcm,
    &bench_chacha20poly1305,
#endif
#ifdef KSET_FLOAT
    &bench_nfs3_project,
    &bench_nfs3_clip,
#endif
};

const struct kset_api kset = {
    KSET_VERSION, sizeof(list) / sizeof(list[0]), list,
    k_arena_init, k_arena_mark, k_arena_release, k_arena_high, k_arena_failed,
};
