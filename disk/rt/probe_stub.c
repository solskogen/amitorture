/* Temporary probe: fills struct amt_probe from exec only (AttnFlags, MemList).
 * The Makefile builds this file only when disk/probe/probe.c does not exist. */
#include <proto/exec.h>
#include <exec/memory.h>
#include "rt.h"
#include "../probe/probe.h"

void amt_probe(struct amt_probe *p)
{
    UWORD a = SysBase->AttnFlags;
    struct MemHeader *mh;
    ULONG id;

    p->cpu = (a & AFF_68060) ? "68060" : (a & AFF_68040) ? "68040" : (a & AFF_68030) ? "68030" :
             (a & AFF_68020) ? "68020" : (a & AFF_68010) ? "68010" : "68000";
    p->cpu_rev = -1;
    p->fpu = (a & (AFF_68040 | AFF_68060)) && (a & AFF_FPU40) ? ((a & AFF_68060) ? "68060" : "68040") :
             (a & AFF_68882) ? "68882" : (a & AFF_68881) ? "68881" : "none";
    p->fpu_frame = 0;
    p->mmu = 0;
    p->addr24 = (a & AFF_68020) ? 0 : 1;
    p->vbr = 0;
    p->cacr = 0;
    p->exec_attn = a;
    p->kick_ver = SysBase->LibNode.lib_Version;
    p->kick_rev = SysBase->SoftVer;
    p->rom_base = (ULONG)SysBase->LibNode.lib_IdString >= 0xfc0000UL && p->kick_ver < 36
                  ? 0xfc0000UL : ((ULONG)SysBase->LibNode.lib_IdString & 0xfff80000UL);

    id = CREG(R_VPOSR) >> 8 & 0x7f;
    p->agnus_id = id;
    p->ecs_agnus = (id & 0x20) ? 1 : 0;
    p->denise_id = 0xffff;   /* not probed by the stub */
    p->ecs_denise = 0;
    p->chipset = (id & 0x22) == 0x22 ? "AGA" : p->ecs_agnus ? "ECS" : "OCS";
    p->pal = SysBase->VBlankFrequency != 60;

    p->n_how = 0;
    p->how[p->n_how++] = "stub:attnflags";
    p->how[p->n_how++] = "stub:vposr";

    p->n_mem = 0;
    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head;
         mh->mh_Node.ln_Succ && p->n_mem < AMT_PROBE_MEM_MAX;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ) {
        struct amt_probe_mem *m = &p->mem[p->n_mem++];
        m->name = mh->mh_Node.ln_Name;
        m->lower = (ULONG)mh->mh_Lower;
        m->upper = (ULONG)mh->mh_Upper;
        m->attr = mh->mh_Attributes;
        m->free = mh->mh_Free;
    }
    Permit();
    p->n_boards = 0;
}
