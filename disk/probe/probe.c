/* amitorture machine probe (CONTRACT.md section 8).
 *
 * Runs with exec alive, before the takeover.  The CPU/FPU/MMU tests are
 * architectural: each one executes an instruction (or register write and
 * read-back) whose behaviour differs between the Motorola variants, in
 * supervisor mode under Disable(), with our own vectors at VBR catching the
 * exceptions (probe_asm.s).  AttnFlags is recorded but does not decide.
 * See README.md for the decision table and the PRM references.
 *
 * Nothing is written outside our own BSS/stack except the vector table
 * entries (saved and restored around each test) and the CPU control
 * registers being tested, which are restored to the values read before.
 * The 68020 address-alias test writes only to two blocks it allocated. */
#include <exec/types.h>
#include <exec/execbase.h>
#include <exec/memory.h>
#include <exec/resident.h>
#include <libraries/configvars.h>
#include <proto/exec.h>

#define __NOLIBBASE__
#define EXPANSION_BASE_NAME amt_expbase
#include <proto/expansion.h>

#include "probe.h"

static struct Library *amt_expbase;

/* probe_asm.s */
extern ULONG amt_probe_sv(ULONG id, ULONG *io, ULONG vbr, ULONG install);
extern ULONG amt_probe_trappc;

/* test ids, the order of test_table in probe_asm.s */
enum {
    T_VBR, T_MOVECCR, T_CACR, T_SCALE, T_CACRW, T_PCR, T_PCRW, T_ITT0, T_CAAR,
    T_TC4, T_TC4W, T_PMOVE_RD, T_PMOVE_TT, T_PMOVE_W, T_MMU030U, T_FPU,
    T_FSAVE, T_NOP
};

#define V_ILLEGAL 4
#define V_LINEF   11

static ULONG io[128];               /* test in/out area (FSAVE needs 2x256) */
static ULONG mmu_tab[16 + 4];       /* 68030 table, 16-byte aligned inside */
static ULONG cur_vbr;
static struct amt_probe *P;

/* ------------------------------------------------------------------ */
/* "how" strings, built in P->how_pool                                 */

static char *hb_p;
static char *hb_end;
static int hb_ok;

static void how_begin(void)
{
    hb_p = P->how_pool + P->how_used;
    hb_end = P->how_pool + AMT_PROBE_HOW_POOL - 1;
    hb_ok = P->n_how < AMT_PROBE_HOW_MAX && hb_p < hb_end;
}

static void how_s(const char *s)
{
    if (!hb_ok)
        return;
    while (*s) {
        if (hb_p >= hb_end) {
            hb_ok = 0;
            return;
        }
        *hb_p++ = *s++;
    }
}

static void how_x(ULONG v, int digits)
{
    char t[9];
    int i;
    if (digits < 1 || digits > 8)
        digits = 8;
    for (i = digits - 1; i >= 0; i--) {
        t[i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    }
    t[digits] = 0;
    how_s(t);
}

static void how_d(LONG v)
{
    char t[12];
    int i = 11;
    ULONG u = v < 0 ? (ULONG)-v : (ULONG)v;
    t[i] = 0;
    do {
        t[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u && i > 1);
    if (v < 0)
        t[--i] = '-';
    how_s(t + i);
}

static void how_end(void)
{
    char *start = P->how_pool + P->how_used;
    if (!hb_ok)
        return;
    *hb_p++ = 0;
    P->how[P->n_how++] = start;
    P->how_used = hb_p - P->how_pool;
}

static void how(const char *s)
{
    how_begin();
    how_s(s);
    how_end();
}

static void how_hex(const char *s, ULONG v, int digits)
{
    how_begin();
    how_s(s);
    how_x(v, digits);
    how_end();
}

/* ------------------------------------------------------------------ */
/* running a test                                                       */

/* Run test id with our vectors at cur_vbr.  Returns 0 if it completed,
 * else the vector number of the exception that stopped it.  Exceptions
 * other than illegal instruction / line-F are unexpected for every test:
 * they are recorded ("trap:<tag> v=<n> pc=<pc>") and count as "absent". */
static ULONG run_at(ULONG id, const char *tag, ULONG vbr)
{
    ULONG v = amt_probe_sv(id, io, vbr, 1);
    if (v && v != V_ILLEGAL && v != V_LINEF) {
        P->traps++;
        how_begin();
        how_s("trap:");
        how_s(tag);
        how_s(" v=");
        how_d((LONG)v);
        how_s(" pc=");
        how_x(amt_probe_trappc, 8);
        how_end();
    }
    return v;
}

static ULONG run(ULONG id, const char *tag)
{
    return run_at(id, tag, cur_vbr);
}

static void io_clear(void)
{
    int i;
    for (i = 0; i < 128; i++)
        io[i] = 0;
}

/* ------------------------------------------------------------------ */
/* CPU / FPU / MMU (called under Disable())                             */

static void probe_mmu030(ULONG tc)
{
    ULONG t, tt0, tt1;
    ULONG *tab;
    int i, used;

    if (tc & 0x80000000UL) {
        P->mmu = 1;
        how_hex("tc030:e=1 tc=", tc, 8);
        return;
    }
    io_clear();
    t = run(T_PMOVE_TT, "pmove-tt");
    tt0 = io[0];
    tt1 = io[1];
    if (!t && ((tt0 | tt1) & 0x8000)) {
        how_hex("tt030:active tt0=", tt0, 8);
        how("tt030:disabled for the walk test");
    }
    if (!t) {
        /* identity map, 16 early-termination page descriptors (DT=1) */
        tab = (ULONG *)(((ULONG)mmu_tab + 15) & ~15UL);
        for (i = 0; i < 16; i++)
            tab[i] = ((ULONG)i << 28) | 1;
        io_clear();
        io[0] = (ULONG)tab;
        io[1] = 0x80000002UL;           /* CRP: L/U=1 (lower limit 0), DT=2 */
        io[2] = (ULONG)tab;
        io[3] = 0x80f04445UL;           /* E, PS=32K, IS=0, TIA-D=4,4,4,5 */
        io[6] = tc;
        t = run(T_MMU030U, "mmu030-walk");
        used = 0;
        for (i = 0; i < 16; i++)
            if (tab[i] & 8)
                used++;
        if (t == 56) {
            P->mmu = 1;
            how("mmu030:config-exception");
        } else if (!t) {
            P->mmu = used ? 1 : 0;
            how_begin();
            how_s(used ? "mmu030:walk u=" : "mmu030:no-walk u=");
            how_d(used);
            how_end();
        }
        if (!io[7])
            how("crp:dt=0 not restored");
        if (t != 56 && t)
            goto readback;
        return;
    }
readback:
    io_clear();
    io[0] = tc;
    io[1] = tc == 0x00f04445UL ? 0x00e04446UL : 0x00f04445UL;
    t = run(T_PMOVE_W, "pmove-tc-w");
    if (!t && io[2] == io[1]) {
        P->mmu = 1;
        how("tc030:rw");
    } else {
        P->mmu = 0;
        how_hex("tc030:ro rb=", io[2], 8);
    }
}

static void probe_mmu040(void)
{
    ULONG t, tc, urp;

    io_clear();
    t = run(T_TC4, "movec-tc");
    if (t) {
        how("movec-tc:illegal");
        P->mmu = 0;
        return;
    }
    tc = io[0];
    urp = io[1];
    if (tc & 0x8000) {
        P->mmu = 1;
        how_hex("tc:e=1 tc=", tc, 4);
        return;
    }
    io_clear();
    io[0] = tc;
    io[1] = tc ^ 0x4000;                /* toggle P (page size), E stays 0 */
    io[3] = urp;
    io[4] = urp == 0xa5a5a400UL ? 0x5a5a5a00UL : 0xa5a5a400UL;
    t = run(T_TC4W, "movec-tc-w");
    if (t) {
        P->mmu = 0;
        how("tc:write-trap");
        return;
    }
    P->mmu = ((io[2] ^ io[1]) & 0x4000) == 0;
    how_begin();
    how_s(P->mmu ? "tc:p-rw" : "tc:p-ro");
    how_s(io[5] == io[4] ? " urp:rw" : " urp:ro");
    how_end();
}

static void probe_cpu(UWORD attn)
{
    ULONG t, cacr = 0, pcr = 0, frame = 0;
    int fam, fpu_ok = 0, pcr_tmp = 0, fsz;

    /* 1. VBR.  Our vectors go to address 0 for this one test: on a 68000
     * (no VBR) the MOVEC traps through them; on a 68010+ it cannot trap
     * and simply reads the VBR. */
    io_clear();
    t = run_at(T_VBR, "movec-vbr", 0);
    if (t) {
        fam = 0;
        cur_vbr = 0;
        how(t == V_ILLEGAL ? "movec-vbr:illegal" : "movec-vbr:trap");
    } else {
        fam = 10;
        cur_vbr = io[0];
        how_hex("movec-vbr:ok vbr=", cur_vbr, 8);
    }
    P->vbr = cur_vbr;

    /* cross-check 68000 vs 68010+: MOVE CCR,Dn is illegal on the 68000 */
    io_clear();
    t = run(T_MOVECCR, "move-ccr");
    how(t ? "move-ccr:illegal" : "move-ccr:ok");
    if ((t != 0) != (fam == 0))
        how("move-ccr:disagrees");

    /* 2. 68010 vs 68020+: CACR does not exist on the 68010 */
    if (fam) {
        io_clear();
        t = run(T_CACR, "movec-cacr");
        if (t) {
            how("movec-cacr:illegal");
        } else {
            fam = 20;
            cacr = io[0];
            how_hex("movec-cacr:ok cacr=", cacr, 8);
        }
    }
    P->cacr = cacr;

    /* cross-check: index scale factor is ignored before the 68020 */
    io_clear();
    t = run(T_SCALE, "scale");
    if (!t) {
        how(io[0] == 4 ? "scale:x4" : io[0] == 1 ? "scale:x1" : "scale:?");
        if ((io[0] == 4) != (fam >= 20))
            how("scale:disagrees");
    }

    if (fam >= 20) {
        /* 3. 68060: PCR exists only there */
        io_clear();
        t = run(T_PCR, "movec-pcr");
        if (!t) {
            fam = 60;
            pcr = io[0];
            P->pcr = pcr;
            P->cpu_rev = (pcr >> 8) & 0xff;
            how_begin();
            how_s("pcr:ok id=");
            how_x(pcr >> 16, 4);
            how_s(" rev=");
            how_d((LONG)P->cpu_rev);
            how_s(pcr & 2 ? " dfp=1" : " dfp=0");
            how_end();
        } else {
            how("movec-pcr:illegal");
            /* 4. 68040: ITT0/DTT0 exist on 040/060, not on 020/030 */
            io_clear();
            t = run(T_ITT0, "movec-itt0");
            if (!t) {
                fam = 40;
                how("movec-itt0:ok");
            } else {
                how("movec-itt0:illegal");
            }
        }

        /* 5. CACR bit 13 (030 WA, 060 FIC) and bit 9 (030 FD) read back.
         * Neither enables a cache; the CACR is restored right away. */
        io_clear();
        io[0] = cacr;
        io[1] = 0x2200;
        t = run(T_CACRW, "cacr-w");
        if (!t) {
            ULONG rb = io[2] & 0x2200;
            how_hex("cacr-w:2200 rb=", rb, 4);
            if (fam == 20 && rb)
                fam = 30;
            if (fam == 20 && !rb)
                how("cacr:020");
            else if (fam == 30)
                how(rb == 0x2200 ? "cacr:030 wa+fd" : "cacr:030 partial");
            else if (fam == 40 && rb)
                how("cacr:040 unexpected");
            else if (fam == 60 && rb != 0x2000)
                how("cacr:060 unexpected");
        }

        /* cross-check: CAAR exists on 020/030 only */
        if (fam <= 40) {
            io_clear();
            t = run(T_CAAR, "movec-caar");
            how(t ? "movec-caar:illegal" : "movec-caar:ok");
            if ((t != 0) != (fam == 40))
                how("caar:disagrees");
        }
    }
    P->cpu_family = fam;

    /* FPU.  On the 68060 the boot code may have disabled the FPU with
     * PCR.DFP; clear it for the test and put it back afterwards. */
    if (fam == 60 && (pcr & 2)) {
        io_clear();
        io[1] = pcr & ~2UL;
        t = run(T_PCRW, "pcr-w");
        if (!t && !(io[2] & 2))
            pcr_tmp = 1;
        else
            how("pcr:dfp stuck");
    }
    io_clear();
    t = run(T_FPU, "fpu");
    if (!t) {
        fpu_ok = 1;
        io_clear();
        t = run(T_FSAVE, "fsave");
        if (!t) {
            frame = io[64];
            how_hex("fsave:", frame, 8);
        }
    } else {
        how(t == V_LINEF ? "fpu:linef" : "fpu:trap");
    }
    if (pcr_tmp) {
        io_clear();
        io[1] = pcr;
        run(T_PCRW, "pcr-restore");
        P->fpu_dis = (UBYTE)fpu_ok;
    }
    if (fpu_ok) {
        P->fpu_frame = frame;
        if (fam == 40) {
            P->fpu_id = AMT_FPU_68040;
        } else if (fam == 60) {
            P->fpu_id = AMT_FPU_68060;
        } else {
            fsz = (frame >> 16) & 0xff;
            P->fpu_id = fsz == 0x18 ? AMT_FPU_68881 : AMT_FPU_68882;
            how(fsz == 0x18 ? "fsave:0x18 68881" : fsz == 0x38 ? "fsave:0x38 68882"
                                                              : "fsave:size? 68882");
        }
    }

    /* MMU */
    if (fam == 20 || fam == 30) {
        io_clear();
        t = run(T_PMOVE_RD, "pmove-tc");
        if (t) {
            P->mmu = 0;
            how(t == V_LINEF ? "pmove-tc:linef" : "pmove-tc:trap");
        } else if (fam == 20) {
            P->mmu = 1;                 /* 68851 */
            how_hex("pmove-tc:ok 68851 tc=", io[0], 8);
        } else {
            how_hex("pmove-tc:ok tc=", io[0], 8);
            probe_mmu030(io[0]);
        }
    } else if (fam >= 40) {
        probe_mmu040();
    }

    (void)attn;
}

/* 68020 vs 68EC020: does the address bus have 32 bits?  Only testable if
 * exec has memory above 16 MB.  Under Forbid() we look for a free chunk H
 * in high memory whose low 24 address bits fall in a free chunk L below
 * 16 MB, and AllocAbs() both.  With a 24-bit bus a write to H lands in L,
 * which we own too, so nothing outside our two blocks can be touched. */
#define ALIAS_SZ 64

static int alias_pick(ULONG *lo_out, ULONG *hi_out)
{
    struct MemHeader *hm, *lm;
    struct MemChunk *hc, *lc;

    for (hm = (struct MemHeader *)SysBase->MemList.lh_Head; hm->mh_Node.ln_Succ;
         hm = (struct MemHeader *)hm->mh_Node.ln_Succ) {
        if ((ULONG)hm->mh_Upper <= 0x01000000UL)
            continue;
        for (hc = hm->mh_First; hc; hc = hc->mc_Next) {
            ULONG hs = (ULONG)hc, he = hs + hc->mc_Bytes, base;
            if (hs < 0x01000000UL)
                hs = 0x01000000UL;
            if (he <= hs)
                continue;
            base = hs & 0xff000000UL;
            if (he - base > 0x01000000UL)
                he = base + 0x01000000UL;
            for (lm = (struct MemHeader *)SysBase->MemList.lh_Head; lm->mh_Node.ln_Succ;
                 lm = (struct MemHeader *)lm->mh_Node.ln_Succ) {
                if ((ULONG)lm->mh_Upper > 0x01000000UL)
                    continue;
                for (lc = lm->mh_First; lc; lc = lc->mc_Next) {
                    ULONG ls = (ULONG)lc, le = ls + lc->mc_Bytes;
                    ULONG s = hs - base, e = he - base;
                    if (ls > s)
                        s = ls;
                    if (le < e)
                        e = le;
                    s = (s + 7) & ~7UL;
                    if (s + ALIAS_SZ <= e) {
                        *lo_out = s;
                        *hi_out = base + s;
                        return 1;
                    }
                }
            }
        }
    }
    return 0;
}

/* Fallback when exec lists no memory above 16 MB (AROS adds Zorro III RAM
 * after the bootblock runs): if expansion.library knows a configured RAM
 * board above 16 MB, write a pattern into a chip block we own and only READ
 * the board address with the same low 24 bits.  A 24-bit bus shows our
 * pattern there; a 32-bit bus shows the board's own (unowned) RAM, which is
 * never written.  Returns -1 if the test can't be done, else 0/1. */
static int addr24_board_alias(void)
{
    struct ConfigDev *cd = 0;
    ULONG hb = 0, hsize = 0;
    volatile ULONG *lo, *hi;
    int i, seen = 0, rounds = 0;

    amt_expbase = OpenLibrary((CONST_STRPTR)"expansion.library", 33);
    if (!amt_expbase)
        return -1;
    while ((cd = FindConfigDev(cd, -1, -1)) != 0) {
        ULONG a = (ULONG)cd->cd_BoardAddr;
        if (a >= 0x01000000UL && (cd->cd_Rom.er_Type & ERTF_MEMLIST)) {
            hb = a;
            hsize = cd->cd_BoardSize;
            break;
        }
    }
    CloseLibrary(amt_expbase);
    amt_expbase = 0;
    if (!hb)
        return -1;
    lo = (volatile ULONG *)AllocMem(ALIAS_SZ, MEMF_CHIP | MEMF_CLEAR);
    if (!lo)
        return -1;
    if (((ULONG)lo & 0x00ffffffUL) + ALIAS_SZ > hsize ||
        ((hb & 0x00ffffffUL) != 0 && ((ULONG)lo & 0x00ffffffUL) < (hb & 0x00ffffffUL))) {
        FreeMem((APTR)lo, ALIAS_SZ);
        return -1;
    }
    hi = (volatile ULONG *)((hb & 0xff000000UL) | ((ULONG)lo & 0x00ffffffUL));
    for (rounds = 0; rounds < 2; rounds++) {
        int match = 1;
        for (i = 0; i < ALIAS_SZ / 4; i++)
            lo[i] = (0x5a17c0deUL ^ (ULONG)i * 0x9e3779b9UL) + (ULONG)rounds;
        for (i = 0; i < ALIAS_SZ / 4; i++)
            if (hi[i] != lo[i])
                match = 0;
        seen += match;
    }
    FreeMem((APTR)lo, ALIAS_SZ);
    how_hex(seen == 2 ? "addr24:board-read-alias yes hi=" : "addr24:board-read-alias no hi=", (ULONG)hi, 8);
    if (seen == 1)
        how("addr24:board-read-alias inconsistent");
    return seen == 2;
}

static void probe_addr24(void)
{
    struct MemHeader *mh;
    volatile ULONG *lo = 0, *hi = 0;
    ULONG la = 0, ha = 0;
    int high = 0;

    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head; mh->mh_Node.ln_Succ;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ)
        if ((ULONG)mh->mh_Upper > 0x01000000UL)
            high = 1;
    if (high && alias_pick(&la, &ha)) {
        lo = (volatile ULONG *)AllocAbs(ALIAS_SZ, (APTR)la);
        hi = (volatile ULONG *)AllocAbs(ALIAS_SZ, (APTR)ha);
        if (lo != (volatile ULONG *)la || hi != (volatile ULONG *)ha) {
            if (lo)
                FreeMem((APTR)lo, ALIAS_SZ);
            if (hi)
                FreeMem((APTR)hi, ALIAS_SZ);
            lo = hi = 0;
        }
    }
    Permit();

    if (!high) {
        int r = addr24_board_alias();
        if (r >= 0) {
            P->addr24 = (UBYTE)r;
        } else {
            P->addr24 = 1;
            how("addr24:no-highmem assume 24");
        }
    } else if (!hi) {
        P->addr24 = 0;
        how("addr24:highmem, no alias pair, assume 32");
    } else {
        int alias;
        lo[0] = 0x4c4f574dUL;           /* "LOWM" */
        hi[0] = 0x48494748UL;           /* "HIGH" */
        alias = lo[0] == 0x48494748UL;
        lo[1] = 0x12345678UL;
        hi[1] = 0x9abcdef0UL;
        if (alias != (lo[1] == 0x9abcdef0UL))
            how("alias:inconsistent");
        P->addr24 = (UBYTE)alias;
        how_hex(alias ? "alias:yes hi=" : "alias:no hi=", (ULONG)hi, 8);
        FreeMem((APTR)hi, ALIAS_SZ);
        FreeMem((APTR)lo, ALIAS_SZ);
    }
}

static const char *const cpu_names[] = {
    "68000", "68010", "68EC020", "68020", "68EC030", "68030",
    "68EC040", "68LC040", "68040", "68EC060", "68LC060", "68060"
};

static void name_cpu(void)
{
    int id;
    switch (P->cpu_family) {
    case 0:  id = AMT_CPU_68000; break;
    case 10: id = AMT_CPU_68010; break;
    case 20: id = P->addr24 ? AMT_CPU_68EC020 : AMT_CPU_68020; break;
    case 30: id = P->mmu ? AMT_CPU_68030 : AMT_CPU_68EC030; break;
    case 40:
        id = P->fpu_id ? AMT_CPU_68040 : P->mmu ? AMT_CPU_68LC040 : AMT_CPU_68EC040;
        if (P->fpu_id && !P->mmu)
            how("cpu:040 fpu without mmu");
        break;
    default: {
        ULONG pid = P->pcr >> 16;
        if (pid == 0x0430 || P->fpu_id)
            id = AMT_CPU_68060;
        else
            id = P->mmu ? AMT_CPU_68LC060 : AMT_CPU_68EC060;
        if ((pid == 0x0430) != (P->fpu_id != 0))
            how("cpu:060 pcr id vs fpu disagree");
        break;
    }
    }
    P->cpu_id = id;
    P->cpu = cpu_names[id];
    switch (P->fpu_id) {
    case AMT_FPU_68881: P->fpu = "68881"; break;
    case AMT_FPU_68882: P->fpu = "68882"; break;
    case AMT_FPU_68040: P->fpu = "68040"; break;
    case AMT_FPU_68060: P->fpu = "68060"; break;
    default:            P->fpu = "none"; break;
    }
}

/* ------------------------------------------------------------------ */
/* chipset, exec, memory, boards                                         */

#define VPOSR    (*(volatile UWORD *)0xdff004UL)
#define DENISEID (*(volatile UWORD *)0xdff07cUL)

static void probe_chips(void)
{
    UWORD d, v;
    ULONG a;
    int i, stable = 1, lisa, ecsd, alice, ecsa;

    a = (VPOSR >> 8) & 0x7f;
    d = DENISEID;
    for (i = 0; i < 8; i++) {
        v = DENISEID;
        if (v != d)
            stable = 0;
    }
    lisa = stable && (d & 0xff) == 0xf8;
    ecsd = stable && (d & 0xff) == 0xfc;
    alice = (a & 0x2e) == 0x22;
    ecsa = (a & 0x20) != 0;

    P->agnus_id = a;
    /* OCS Denise has no DENISEID register: the read floats (open bus), so
     * any value seen there means nothing and must not leak into the record */
    P->denise_id = (ecsd || lisa) ? d : 0xffff;
    P->ecs_agnus = (UBYTE)(ecsa || alice);
    P->ecs_denise = (UBYTE)(ecsd || lisa);
    P->chipset = (lisa || alice) ? "AGA" : (ecsa || ecsd) ? "ECS" : "OCS";
    P->pal = (a & 0x10) ? 0 : 1;

    how_begin();
    how_s("agnus:");
    how_x(a, 2);
    how_s(alice ? " alice" : ecsa ? " ecs" : " ocs");
    how_s(P->pal ? " pal" : " ntsc");
    how_end();
    how_begin();
    if (ecsd || lisa) {
        how_s("denise:");
        how_x(d, 4);
        how_s(lisa ? " lisa" : " ecs");
    } else {
        how_s("denise:ocs (no DENISEID register, the read floats)");
    }
    how_end();
    how_begin();
    how_s("vblank:");
    how_d((LONG)SysBase->VBlankFrequency);
    if ((SysBase->VBlankFrequency == 60) == P->pal)
        how_s(" disagrees");
    how_end();
}

static void probe_exec(void)
{
    struct Resident *rt;
    ULONG r, b;
    P->kick_ver = SysBase->LibNode.lib_Version;
    P->kick_rev = SysBase->SoftVer;

    /* ROM base: the start of the ROM that holds exec's resident tag.
     * 256 KB ROMs start with $1111, 512 KB ones with $1114. */
    rt = FindResident((CONST_STRPTR)"exec.library");
    r = rt ? (ULONG)rt : (ULONG)SysBase->LibNode.lib_IdString;
    b = r & ~0x3ffffUL;
    if (*(volatile UWORD *)b == 0x1111) {
        P->rom_base = b;
        how("rom:1111 256k");
        return;
    }
    b = r & ~0x7ffffUL;
    P->rom_base = b;
    if (*(volatile UWORD *)b == 0x1114 || *(volatile UWORD *)b == 0x1111)
        how("rom:1114 512k");
    else
        how_hex("rom:nomagic ", *(volatile UWORD *)b, 4);
}

static void probe_mem(void)
{
    struct MemHeader *mh;
    Forbid();
    for (mh = (struct MemHeader *)SysBase->MemList.lh_Head;
         mh->mh_Node.ln_Succ && P->n_mem < AMT_PROBE_MEM_MAX;
         mh = (struct MemHeader *)mh->mh_Node.ln_Succ) {
        struct amt_probe_mem *m = &P->mem[P->n_mem++];
        m->name = mh->mh_Node.ln_Name;
        m->lower = (ULONG)mh->mh_Lower;
        m->upper = (ULONG)mh->mh_Upper;
        m->attr = mh->mh_Attributes;
        m->free = mh->mh_Free;
    }
    Permit();
}

static void probe_boards(void)
{
    struct ConfigDev *cd = 0;
    amt_expbase = OpenLibrary((CONST_STRPTR)"expansion.library", 33);
    if (!amt_expbase) {
        how("expansion:none");
        return;
    }
    while ((cd = FindConfigDev(cd, -1, -1)) != 0 && P->n_boards < AMT_PROBE_BOARDS_MAX) {
        struct amt_probe_board *b = &P->boards[P->n_boards++];
        b->man = cd->cd_Rom.er_Manufacturer;
        b->prod = cd->cd_Rom.er_Product;
        b->addr = (ULONG)cd->cd_BoardAddr;
        b->size = cd->cd_BoardSize;
        b->flags = cd->cd_Flags;
        b->ertype = cd->cd_Rom.er_Type;
    }
    CloseLibrary(amt_expbase);
    amt_expbase = 0;
}

/* ------------------------------------------------------------------ */

void amt_probe(struct amt_probe *p)
{
    UBYTE *z = (UBYTE *)p;
    ULONG i;
    UWORD attn;

    for (i = 0; i < sizeof(*p); i++)
        z[i] = 0;
    P = p;
    p->cpu_rev = -1;
    attn = SysBase->AttnFlags;
    p->exec_attn = attn;
    how_hex("attn:", attn, 4);

    Disable();
    probe_cpu(attn);
    Enable();

    if (p->cpu_family == 20)
        probe_addr24();
    else {
        p->addr24 = p->cpu_family < 20;
        how(p->addr24 ? "addr24:000/010" : "addr24:32-bit cpu");
    }
    name_cpu();

    probe_chips();
    probe_exec();
    probe_mem();
    probe_boards();
}

/* ------------------------------------------------------------------ */
/* JSON (same keys and order as the hello.probe object in rt/main.c)    */

struct jw {
    char *p, *end;
    int ok;
    int first;
};

static void jc(struct jw *w, char c)
{
    if (w->p < w->end)
        *w->p++ = c;
    else
        w->ok = 0;
}

static void jraw(struct jw *w, const char *s)
{
    while (*s)
        jc(w, *s++);
}

static void jqstr(struct jw *w, const char *s)
{
    jc(w, '"');
    if (s) {
        for (; *s; s++) {
            UBYTE c = (UBYTE)*s;
            if (c == '"' || c == '\\') {
                jc(w, '\\');
                jc(w, (char)c);
            } else if (c < 0x20 || c > 0x7e) {
                jraw(w, "\\u00");
                jc(w, "0123456789abcdef"[c >> 4]);
                jc(w, "0123456789abcdef"[c & 15]);
            } else {
                jc(w, (char)c);
            }
        }
    }
    jc(w, '"');
}

static void jkey(struct jw *w, const char *k)
{
    if (!w->first)
        jc(w, ',');
    w->first = 0;
    if (k) {
        jqstr(w, k);
        jc(w, ':');
    }
}

static void jstr(struct jw *w, const char *k, const char *v)
{
    jkey(w, k);
    jqstr(w, v);
}

static void jhex(struct jw *w, const char *k, ULONG v, int digits)
{
    int i;
    char t[9];
    for (i = digits - 1; i >= 0; i--) {
        t[i] = "0123456789abcdef"[v & 15];
        v >>= 4;
    }
    t[digits] = 0;
    jkey(w, k);
    jc(w, '"');
    jraw(w, t);
    jc(w, '"');
}

static void jint(struct jw *w, const char *k, LONG v)
{
    char t[12];
    int i = 11;
    ULONG u = v < 0 ? (ULONG)-v : (ULONG)v;
    t[i] = 0;
    do {
        t[--i] = (char)('0' + u % 10);
        u /= 10;
    } while (u && i > 1);
    if (v < 0)
        t[--i] = '-';
    jkey(w, k);
    jraw(w, t + i);
}

static void jopen(struct jw *w, const char *k, char c)
{
    jkey(w, k);
    jc(w, c);
    w->first = 1;
}

static void jclose(struct jw *w, char c)
{
    jc(w, c);
    w->first = 0;
}

int amt_probe_json(const struct amt_probe *p, char *buf, int len)
{
    struct jw w;
    char vs[16];
    LONG i, n;

    if (len <= 0)
        return -1;
    w.p = buf;
    w.end = buf + len - 1;
    w.ok = 1;
    w.first = 1;

    jc(&w, '{');
    jstr(&w, "cpu", p->cpu);
    jint(&w, "cpu_rev", p->cpu_rev);
    jstr(&w, "fpu", p->fpu);
    jhex(&w, "fpu_frame", p->fpu_frame, 8);
    jint(&w, "mmu", p->mmu);
    jint(&w, "addr24", p->addr24);
    jhex(&w, "vbr", p->vbr, 8);
    jhex(&w, "cacr", p->cacr, 8);
    jopen(&w, "how", '[');
    n = p->n_how > AMT_PROBE_HOW_MAX ? AMT_PROBE_HOW_MAX : p->n_how;
    for (i = 0; i < n; i++)
        jstr(&w, 0, p->how[i]);
    jclose(&w, ']');
    jhex(&w, "agnus_id", p->agnus_id, 4);
    jhex(&w, "denise_id", p->denise_id, 4);
    jstr(&w, "chipset", p->chipset);
    jint(&w, "ecs_agnus", p->ecs_agnus);
    jint(&w, "ecs_denise", p->ecs_denise);
    jstr(&w, "video", p->pal ? "PAL" : "NTSC");
    jhex(&w, "exec_attn", p->exec_attn, 4);
    {
        /* "version.revision" */
        char *q = vs + sizeof(vs) - 1;
        UWORD v = p->kick_rev;
        *q = 0;
        do { *--q = (char)('0' + v % 10); v /= 10; } while (v);
        *--q = '.';
        v = p->kick_ver;
        do { *--q = (char)('0' + v % 10); v /= 10; } while (v);
        jstr(&w, "kick", q);
    }
    jhex(&w, "rom_base", p->rom_base, 8);
    jopen(&w, "mem", '[');
    n = p->n_mem > AMT_PROBE_MEM_MAX ? AMT_PROBE_MEM_MAX : p->n_mem;
    for (i = 0; i < n; i++) {
        const struct amt_probe_mem *m = &p->mem[i];
        jopen(&w, 0, '{');
        jstr(&w, "name", m->name ? m->name : "");
        jhex(&w, "lower", m->lower, 8);
        jhex(&w, "upper", m->upper, 8);
        jhex(&w, "attr", m->attr, 4);
        jint(&w, "free", (LONG)m->free);
        jclose(&w, '}');
    }
    jclose(&w, ']');
    jopen(&w, "boards", '[');
    n = p->n_boards > AMT_PROBE_BOARDS_MAX ? AMT_PROBE_BOARDS_MAX : p->n_boards;
    for (i = 0; i < n; i++) {
        const struct amt_probe_board *b = &p->boards[i];
        jopen(&w, 0, '{');
        jint(&w, "man", (LONG)b->man);
        jint(&w, "prod", (LONG)b->prod);
        jhex(&w, "addr", b->addr, 8);
        jint(&w, "size", (LONG)b->size);
        jint(&w, "flags", (LONG)b->flags);
        jclose(&w, '}');
    }
    jclose(&w, ']');
    jc(&w, '}');
    *w.p = 0;
    return w.ok ? (int)(w.p - buf) : -1;
}
