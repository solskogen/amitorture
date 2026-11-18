// SPDX-License-Identifier: 0BSD
//
// Dhrystone 2.1, the classic integer benchmark, in the form xSysInfo uses:
// the measurement loop is Dhry_Run() and the timing is ours. Dhrystone is
// synthetic, but it is the number every Amiga owner already knows for their
// machine, so a compiler that moves it is immediately noticed.
//
// Dhrystone's ground rules ask for separate compilation of dhry_1.c and
// dhry_2.c and no procedure merging; the Makefile builds them as separate
// objects and leaves LTO off.

#include "bench.h"
#include "third_party/dhry/dhry.h"

// dhry_1.c's globals, which dhry.h does not declare.
extern Rec_Pointer Ptr_Glob;
extern int Int_Glob;
extern char Ch_1_Glob, Ch_2_Glob;
extern int Arr_2_Glob[50][50];

// Dhrystone runs per loop (ksize.h).
#define RUNS KS_DHRY_RUNS

// Dhrystone keeps its state in dhry_1.c's globals, so there is no state of
// our own to hand around: the hooks get one of those globals instead.
static void *dhry_setup(void) {
    return Dhry_Initialize() ? &Int_Glob : NULL;
}

// Proc_8() accumulates into Arr_2_Glob, so the records and arrays are reset
// before every run (a 10 KB memset).
static const void *dhry_run(void *state) {
    if (!Dhry_Initialize())
        return NULL;
    Dhry_Run(RUNS);
    return state;
}

// The globals Dhrystone leaves behind are a function of the run count, so
// they double as the check value.
// The ints by value, so the check doesn't depend on byte order.
static ULONG check_int(ULONG h, int v) {
    UBYTE b[4];

    b[0] = (UBYTE)((ULONG)v >> 24);
    b[1] = (UBYTE)((ULONG)v >> 16);
    b[2] = (UBYTE)((ULONG)v >> 8);
    b[3] = (UBYTE)v;
    return checksum(h, b, 4);
}

static bool dhry_verify(void *state, const void *result, ULONG *check) {
    (void)state;
    (void)result;
    ULONG h = check_int(0, Int_Glob);
    int i, j;

    h = checksum(h, &Ch_1_Glob, sizeof(Ch_1_Glob));
    h = checksum(h, &Ch_2_Glob, sizeof(Ch_2_Glob));
    for (i = 0; i < 50; i++)
        for (j = 0; j < 50; j++)
            h = check_int(h, Arr_2_Glob[i][j]);
    *check = checksum(h, Ptr_Glob->variant.var_1.Str_Comp,
                      sizeof(Ptr_Glob->variant.var_1.Str_Comp));
    return true;
}

const struct Bench bench_dhry = {
    /* .name= */ "dhrystone",
    /* .desc= */ "Dhrystone 2.1",
    /* .group= */ GROUP_MEMORY,
    /* .unit= */ "Dhry",
    /* .per_loop= */ RUNS,
    /* .setup= */ dhry_setup,
    /* .run= */ dhry_run,
    /* .verify= */ dhry_verify,
    /* .teardown= */ NULL,
};
