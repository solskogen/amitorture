/* amitorture probe result (CONTRACT.md section 8).
 *
 * void amt_probe(struct amt_probe *p) runs with exec alive, before takeover.
 * It identifies the CPU (68000 ... 68060, no 68080), FPU and MMU with
 * architectural tests run in supervisor mode under Disable() with its own
 * trap vectors (see probe_asm.s and README.md), and reads the chipset,
 * exec and expansion information.
 *
 * The runtime (disk/rt/main.c) reads the fields of the first block by name
 * for the hello record; the fields after "probe extensions" are extra.
 * Strings are static, point into exec structures, or point into how_pool
 * inside the struct itself. */
#ifndef AMT_PROBE_H
#define AMT_PROBE_H

#include <exec/types.h>

#define AMT_PROBE_HOW_MAX    48
#define AMT_PROBE_HOW_POOL   1024
#define AMT_PROBE_MEM_MAX    12
#define AMT_PROBE_BOARDS_MAX 16

/* cpu_id values, ordered so that e.g. cpu_id >= AMT_CPU_68EC020 means
 * "020 instruction set or better" */
#define AMT_CPU_68000   0
#define AMT_CPU_68010   1
#define AMT_CPU_68EC020 2
#define AMT_CPU_68020   3
#define AMT_CPU_68EC030 4
#define AMT_CPU_68030   5
#define AMT_CPU_68EC040 6
#define AMT_CPU_68LC040 7
#define AMT_CPU_68040   8
#define AMT_CPU_68EC060 9
#define AMT_CPU_68LC060 10
#define AMT_CPU_68060   11

/* fpu_id values */
#define AMT_FPU_NONE    0
#define AMT_FPU_68881   68881
#define AMT_FPU_68882   68882
#define AMT_FPU_68040   68040
#define AMT_FPU_68060   68060

struct amt_probe_mem {
    const char *name;     /* MemHeader ln_Name (may be NULL) */
    ULONG lower, upper;   /* mh_Lower, mh_Upper */
    ULONG attr;           /* mh_Attributes */
    ULONG free;           /* mh_Free */
};

struct ConfigDev;
struct Library;
struct List;

struct amt_probe_board {
    ULONG man;            /* er_Manufacturer */
    ULONG prod;           /* er_Product */
    ULONG addr;           /* cd_BoardAddr */
    ULONG size;           /* cd_BoardSize */
    ULONG flags;          /* cd_Flags (CDF_SHUTUP 1, CDF_CONFIGME 2, CDF_BADMEMORY 4) */
    ULONG ertype;         /* cd_Rom.er_Type */
    ULONG serial;         /* private er_SerialNumber identity snapshot */
    struct ConfigDev *config_dev; /* private provenance; not part of hello JSON */
};

struct amt_probe {
    const char *cpu;      /* "68000" ... "68060", see section 8 */
    LONG  cpu_rev;        /* 68060: PCR revision; -1 otherwise */
    const char *fpu;      /* "none|68881|68882|68040|68060" */
    ULONG fpu_frame;      /* first longword of the FSAVE idle frame
                             (68881/2: version<<24|size<<16), 0 if no FPU */
    UBYTE mmu;            /* 0/1 */
    UBYTE addr24;         /* 0/1: 24-bit address bus */
    UBYTE ecs_agnus;      /* 0/1 (also 1 for AGA Alice) */
    UBYTE ecs_denise;     /* 0/1 (also 1 for AGA Lisa) */
    UBYTE pal;            /* 1 = PAL, 0 = NTSC */
    UBYTE pad[3];
    ULONG vbr;
    ULONG cacr;           /* CACR as found (0 on 68000/68010) */
    ULONG agnus_id;       /* VPOSR bits 14-8 */
    ULONG denise_id;      /* DENISEID ($DFF07C) as read; 0xffff if unstable */
    const char *chipset;  /* "OCS|ECS|AGA" */
    ULONG exec_attn;      /* SysBase->AttnFlags */
    UWORD kick_ver, kick_rev;   /* exec lib_Version, SoftVer */
    ULONG rom_base;
    LONG  n_how;
    const char *how[AMT_PROBE_HOW_MAX];
    LONG  n_mem;
    struct amt_probe_mem mem[AMT_PROBE_MEM_MAX];
    LONG  n_boards;
    struct amt_probe_board boards[AMT_PROBE_BOARDS_MAX];

    /* --- probe extensions (not in the JSON object) --- */
    LONG  cpu_id;         /* AMT_CPU_* */
    LONG  cpu_family;     /* 0, 10, 20, 30, 40, 60 */
    LONG  fpu_id;         /* AMT_FPU_* */
    UBYTE fpu_dis;        /* 68060: FPU present but PCR.DFP was set when the
                             probe ran; it is left set, so clear PCR bit 1
                             before running FPU code */
    UBYTE pad2[3];
    ULONG pcr;            /* 68060 PCR as found, else 0 */
    ULONG traps;          /* number of unexpected exceptions caught */
    LONG  how_used;
    char  how_pool[AMT_PROBE_HOW_POOL];
    struct Library *board_library; /* private open reference for HALT/REBOOT */
    struct List *board_list;       /* live list header, never a cached member */
};

void amt_probe(struct amt_probe *p);
/* Runtime opt-in: retain expansion.library/list provenance only for terminal
 * HALT/REBOOT runs. Ordinary standalone/RETURN probing retains no references. */
void amt_probe_with_claims(struct amt_probe *p, int retain_boards);
/* OS phase only, before any claims. Idempotent; also used on final RETURN. */
void amt_probe_release_boards(struct amt_probe *p);

/* Serialise p as the section 8 JSON object (compact, ASCII, no newline).
 * Writes a NUL-terminated string to buf and returns its length, or -1 if it
 * did not fit in len bytes (buf then holds a truncated, NUL-terminated
 * prefix, unless len <= 0). */
int amt_probe_json(const struct amt_probe *p, char *buf, int len);

#endif
