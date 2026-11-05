/* memory / memory_os (modules 21 and 25): shared declarations.
 *
 * memory_os (AMT_PH_OS, runs before the bare phase) walks ExecBase->MemList
 * and allocates, in every memory region, 2 KB test blocks centred on a
 * 64 KB boundary, on an odd 32 KB boundary and (when the region spans one)
 * on a 16 MB boundary, with AllocAbs() at addresses it found free in the
 * region's own free list (falls back to one plain Allocate() block, without
 * a boundary, if no free boundary exists). memory (AMT_PH_BARE) tests them.
 * The blocks remain available through the bare phase and are freed on CLI
 * return. Default halt intentionally retains them with the live mailbox.
 */
#ifndef AMT_MEMORY_H
#define AMT_MEMORY_H

#include <exec/types.h>

#define MEM_MAXREG   12
#define MEM_HALF     1024           /* bytes on each side of the boundary */
#define MEM_BLK      (2 * MEM_HALF)

/* boundary kinds */
#define MB_64K   0
#define MB_32K   1
#define MB_16M   2
#define MB_PLAIN 3                  /* no boundary: fallback block */

struct mem_reg {
    char  kind[12];                 /* chip, slow, z2, z3, mbram, hifast, other (+"2".. if repeated) */
    ULONG lower, upper, attr;
    UBYTE chipbus;                  /* 1: chip or slow (Agnus-arbitrated) RAM: no RMW;
                                       2: non-autoconfig RAM at $600000-$9FFFFF (PCMCIA):
                                       RMW and MOVE16 bursts not guaranteed */
    UBYTE nb;                       /* number of blocks */
    UBYTE pad[2];
    ULONG blk[4];                   /* block start (boundary - MEM_HALF), 0 = none, by MB_* */
    ULONG plain_alloc;              /* original, possibly unaligned Allocate() result */
    struct MemHeader *plain_owner;
};

extern struct mem_reg mem_regs[MEM_MAXREG];
extern LONG mem_nreg;
extern LONG mem_os_ran;             /* memory_os ran (and filled mem_regs) */

#endif
