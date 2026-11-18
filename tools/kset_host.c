/* SPDX-License-Identifier: 0BSD
 *
 * Host driver for tools/kset_expect.py: runs every kernel of a kernel set
 * built natively (32-bit, SSE floats) once, the way the bench harness does,
 * and prints "name checksum" per kernel. The kernels are portable C, so
 * these are the checksums the Amiga build must produce.
 */
#include <stdio.h>

#include "bench.h"
#include "kset.h"

extern const struct kset_api kset;

static unsigned char arena[8 << 20] __attribute__((aligned(16)));

int main(void)
{
    int i, rc = 0;

    for (i = 0; i < kset.n; i++) {
        const struct Bench *b = kset.benches[i];
        void *state;
        const void *result;
        ULONG mark, check = 0;

        kset.arena_init(arena, sizeof(arena));
        state = b->setup();
        if (!state) {
            fprintf(stderr, "%s: setup failed\n", b->name);
            rc = 1;
            continue;
        }
        mark = kset.arena_mark();
        result = b->run(state);
        kset.arena_release(mark);
        if (!result || !b->verify(state, result, &check)) {
            fprintf(stderr, "%s: failed\n", b->name);
            rc = 1;
        }
        printf("%s %08lx\n", b->name, (unsigned long)check);
        if (b->teardown)
            b->teardown(state);
    }
    return rc;
}
