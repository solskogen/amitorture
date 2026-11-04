/* memory_os (module 25, AMT_PH_OS): allocates the per-region test blocks
 * for the memory module (21) while exec is alive. See memory.h. */
#include <proto/exec.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include "mod.h"
#include "memory.h"

AMT_MODULE(memory_os, 25, AMT_PH_OS, 0, 0, 0);

struct mem_reg mem_regs[MEM_MAXREG];
LONG mem_nreg;
LONG mem_os_ran;

static void scpy(char *d, const char *s)
{
    while ((*d++ = *s++) != 0)
        ;
}

/* region kind from the address map (and the autoconfig board list) */
static const char *kind_of(ULONG lo, ULONG attr, UBYTE *chipbus)
{
    LONG i;
    *chipbus = 0;
    if (!(attr & (MEMF_CHIP | MEMF_FAST)))
        return "rom";                  /* not RAM (AROS lists its ROMs as MemHeaders) */
    if ((attr & MEMF_CHIP) && lo < 0x00200000UL) {
        *chipbus = 1;
        return "chip";
    }
    if (lo >= 0x00c00000UL && lo < 0x00dc0000UL) {
        *chipbus = 1;
        return "slow";
    }
    for (i = 0; i < amt_probe_result.n_boards; i++) {
        const struct amt_probe_board *b = &amt_probe_result.boards[i];
        if (amt_board_ok(b) && lo >= b->addr && lo - b->addr < b->size)   /* review H1 */
            return b->addr < 0x01000000UL ? "z2" : "z3";
    }
    if (lo >= 0x00600000UL && lo < 0x00a00000UL) {
        /* no autoconfig board here: on the A600/A1200 this is the Gayle
         * PCMCIA window (card memory). Whether the card interface supports
         * an indivisible RMW cycle or a MOVE16 line burst is not specified
         * (HRM; 68040UM MOVE16), so those are skipped (the kind stays "z2":
         * it is part of the case ids) */
        *chipbus = 2;
        return "z2";
    }
    if (lo >= 0x00200000UL && lo < 0x00a00000UL)
        return "z2";
    if (lo >= 0x07000000UL && lo < 0x08000000UL)
        return "mbram";
    if (lo >= 0x08000000UL)
        return "hifast";
    return "other";
}

/* a free address B (B % step == rem) with [B-HALF, B+HALF) inside one free
 * chunk of mh; 0 if none */
static ULONG find_free(struct MemHeader *mh, ULONG step, ULONG rem)
{
    struct MemChunk *mc;
    for (mc = mh->mh_First; mc; mc = mc->mc_Next) {
        ULONG lo = (ULONG)mc + MEM_HALF, hi = (ULONG)mc + mc->mc_Bytes - MEM_HALF;
        ULONG b;
        if (mc->mc_Bytes < MEM_BLK + 16)
            continue;
        b = (lo & ~(step - 1)) + rem;
        while (b < lo && b + step > b)
            b += step;
        /* also keep away from the ends of the region */
        if (b >= lo && b <= hi && b - MEM_HALF >= (ULONG)mh->mh_Lower && b + MEM_HALF <= (ULONG)mh->mh_Upper)
            return b;
    }
    return 0;
}

void amt_run_memory_os(void)
{
    struct MemHeader *mh;
    static const ULONG step[3] = { 0x10000UL, 0x10000UL, 0x1000000UL };
    static const ULONG rem[3] = { 0, 0x8000UL, 0 };
    LONG n = 0, i, k;
    char id[24];

    mem_nreg = 0;
    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head; mh->mh_Node.ln_Succ && n < MEM_MAXREG;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ) {
        struct mem_reg *r = &mem_regs[n];
        const char *kd = kind_of((ULONG)mh->mh_Lower, mh->mh_Attributes, &r->chipbus);
        int dup = 0;
        for (i = 0; i < n; i++) {
            const char *a = mem_regs[i].kind, *b = kd;
            while (*b && *a == *b)
                a++, b++;
            if (!*b && (!*a || (*a >= '2' && *a <= '9')))
                dup++;
        }
        scpy(r->kind, kd);
        if (dup) {
            char *q = r->kind;
            while (*q)
                q++;
            q[0] = (char)('1' + dup);
            q[1] = 0;
        }
        r->lower = (ULONG)mh->mh_Lower;
        r->upper = (ULONG)mh->mh_Upper;
        r->attr = mh->mh_Attributes;
        r->nb = 0;
        for (k = 0; k < 4; k++)
            r->blk[k] = 0;
        for (k = 0; k < 3 && !(r->kind[0] == 'r' && r->kind[1] == 'o'); k++) {
            ULONG b = find_free(mh, step[k], rem[k]);
            if (b && AllocAbs(MEM_BLK, (APTR)(b - MEM_HALF))) {
                r->blk[k] = b - MEM_HALF;
                r->nb++;
            }
        }
        if (!r->blk[MB_64K] && !(r->kind[0] == 'r' && r->kind[1] == 'o')) {
            /* no free 64 KB boundary: one plain block from this region */
            UBYTE *p = Allocate(mh, MEM_BLK + 16);
            if (p) {
                r->blk[MB_PLAIN] = ((ULONG)p + 15) & ~15UL;
                r->nb++;
            }
        }
        n++;
    }
    Permit();
    mem_nreg = n;
    mem_os_ran = 1;

    for (i = 0; i < n; i++) {
        struct mem_reg *r = &mem_regs[i];
        char *q = id;
        const char *t = r->kind;
        while (*t)
            *q++ = *t++;
        scpy(q, "_alloc");
        if (r->kind[0] == 'r' && r->kind[1] == 'o') {
            amt_case(id, AMT_ST_SKIP, "this MemHeader is not RAM (neither MEMF_CHIP nor MEMF_FAST; "
                                      "AROS lists its ROMs in the MemList): not tested");
        } else if (r->blk[MB_64K] || r->blk[MB_PLAIN]) {
            amt_case_open(id, AMT_ST_PASS);
            json_obj("ctx");
            json_hex("lower", r->lower);
            json_hex("upper", r->upper);
            json_hexn("attr", r->attr, 4);
            /* which blocks could be allocated depends on the exec free list
             * at this point (payload size, boot path), not on the machine:
             * undef_* keys stay out of the fingerprint (schema 4.1) */
            json_int("undef_blocks", r->nb);
            json_int("undef_b64k", r->blk[MB_64K] != 0);
            json_int("undef_b32k", r->blk[MB_32K] != 0);
            json_int("undef_b16m", r->blk[MB_16M] != 0);
            json_obj_end();
            amt_case_close();
        } else {
            amt_case(id, AMT_ST_UNTESTABLE, "no free 2 KB block in this memory region");
        }
    }
}
