/* misc_hw_os: Zorro autoconfig consistency (CONTRACT.md section 10, module
 * id 24, OS phase; the OS-phase half of misc_hw, id 19).
 *
 * Walks expansion.library's ConfigDev list (FindConfigDev, read only) and
 * exec's MemList, and checks every board against the AutoConfig rules
 * (libraries/configregs.h, Zorro III technical specification):
 *   - er_Type bits 7-6: 11 = Zorro II, 10 = Zorro III (others are ignored by
 *     the OS and must not be in the list)
 *   - er_Flags: Zorro II: EXTENDED (b5), ZORRO_III (b4) and the sub-size
 *     (b3-0) must be 0; Zorro III: ZORRO_III must be 1
 *   - size code (er_Type b2-0): Zorro II/III 0 = 8 MB, 1-7 = 64 KB << (n-1);
 *     Zorro III with EXTENDED: 16 MB << n (7 reserved). cd_BoardSize must be
 *     that physical size, or the Zorro III sub-size (logical) size
 *   - address: Zorro II boards in the 8 MB space $200000-$9FFFFF or the I/O
 *     space $E90000-$EFFFFF; Zorro III boards in $10000000-$7FFFFFFF (or in a
 *     Zorro II space); aligned to their size, counted from $200000 in the
 *     Zorro II 8 MB space (expansion.library places a 4 MB board at $200000)
 *   - no two configured boards overlap; every MemHeader lies inside one
 *     ERTF_MEMLIST board or outside all boards; every configured MEMLIST board
 *     without CDF_BADMEMORY has its RAM in the MemList
 *   - ERTF_DIAGVALID: er_InitDiagVec is inside the board
 * Shut-up boards (CDF_SHUTUP) have no address and are only type-checked. */
#include <proto/exec.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <libraries/configvars.h>
#include <libraries/configregs.h>
#define __NOLIBBASE__
#define EXPANSION_BASE_NAME mho_expbase
#include <proto/expansion.h>
#include "mod.h"

AMT_MODULE(misc_hw_os, 24, AMT_PH_OS, 0, 0, 0);

static struct Library *mho_expbase;

#define MAXB 32

struct bd {
    ULONG addr, size, phys, lsize;    /* lsize: Z3 sub-size, 0 = none/auto */
    UWORD man, diag;
    UBYTE type, flags, cdf, prod, z3, sub, res03;
};
static struct bd b[MAXB];
static int nb;

#define MAXM 16
static ULONG mlo[MAXM], mhi[MAXM];
static int nm;

static void board_obj(const struct bd *x)
{
    json_obj(0);
    json_int("man", x->man);
    json_int("prod", x->prod);
    json_hexn("type", x->type, 2);
    json_hexn("flags", x->flags, 2);
    json_hexn("cd_flags", x->cdf, 2);
    json_hex("addr", x->addr);
    json_hex("size", x->size);
    json_hex("phys", x->phys);
    json_obj_end();
}

/* one case over all boards: bad[] marks the offenders */
static void report(const char *id, const UBYTE *bad, const char *rule, const char *why)
{
    int i, n = 0;
    for (i = 0; i < nb; i++)
        n += bad[i] != 0;
    if (!n) {
        amt_case(id, AMT_ST_PASS, 0);
        return;
    }
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    json_str("rule", rule);
    json_obj_end();
    json_obj("got");
    json_arr("boards");
    for (i = 0; i < nb; i++)
        if (bad[i])
            board_obj(&b[i]);
    json_arr_end();
    json_obj_end();
    json_str("why", why);
    amt_case_close();
}

static ULONG phys_size(const struct bd *x)
{
    ULONG c = x->type & ERT_MEMMASK;
    if (x->z3 && (x->flags & ERFF_EXTENDED))
        return c == 7 ? 0 : 0x1000000UL << c;
    return c ? 0x10000UL << (c - 1) : 0x800000UL;
}

/* Zorro III sub-size: 0 = same as physical, 1 = automatically sized,
 * 2-8 = 64 KB..4 MB, 9-13 = 6, 8, 10, 12, 14 MB, 14-15 reserved */
static ULONG sub_size(UBYTE ss)
{
    if (ss >= 2 && ss <= 8)
        return 0x10000UL << (ss - 2);
    if (ss >= 9 && ss <= 13)
        return (ULONG)(6 + 2 * (ss - 9)) << 20;
    return 0;
}

static int in_range(ULONG a, ULONG s, ULONG lo, ULONG hi)
{
    return a >= lo && s <= hi - lo && a - lo <= hi - lo - s;
}

/* AROS ROM? Its exec IdString is plain "exec 51.10 (date)" (no "AROS" in
 * it), so look for the aros.library resident module, which only the AROS
 * ROM has (read-only FindResident). The IdString test stays as a fallback
 * for builds that do name AROS there. */
static int is_aros(void)
{
    const char *s = (const char *)SysBase->LibNode.lib_IdString;
    int i;
    if (FindResident((CONST_STRPTR)"aros.library"))
        return 1;
    for (i = 0; s && s[i] && i < 80; i++)
        if (s[i] == 'A' && s[i + 1] == 'R' && s[i + 2] == 'O' && s[i + 3] == 'S')
            return 1;
    return 0;
}

static void collect(void)
{
    struct ConfigDev *cd = 0;
    struct Node *n;
    nb = nm = 0;
    while ((cd = FindConfigDev(cd, -1, -1)) != 0 && nb < MAXB) {
        struct bd *x = &b[nb++];
        x->addr = (ULONG)cd->cd_BoardAddr;
        x->size = cd->cd_BoardSize;
        x->type = cd->cd_Rom.er_Type;
        x->flags = cd->cd_Rom.er_Flags;
        x->prod = cd->cd_Rom.er_Product;
        x->res03 = cd->cd_Rom.er_Reserved03;
        x->man = cd->cd_Rom.er_Manufacturer;
        x->diag = cd->cd_Rom.er_InitDiagVec;
        x->cdf = cd->cd_Flags;
        x->z3 = (x->type & ERT_TYPEMASK) == ERT_ZORROIII;
        x->sub = x->flags & ERT_Z3_SSMASK;
        x->phys = phys_size(x);
        x->lsize = x->z3 ? sub_size(x->sub) : 0;
    }
    Forbid();
    for (n = SysBase->MemList.lh_Head; n->ln_Succ && nm < MAXM; n = n->ln_Succ) {
        struct MemHeader *mh = (struct MemHeader *)n;
        /* the region, not the node: AROS keeps headers for the ROM (MEMF_KICK)
         * in chip RAM, far from the memory they describe */
        mlo[nm] = (ULONG)mh->mh_Lower;
        mhi[nm] = (ULONG)mh->mh_Upper;
        nm++;
    }
    Permit();
}

void amt_run_misc_hw_os(void)
{
    UBYTE bad[MAXB];
    int i, j, aros;

    mho_expbase = OpenLibrary((CONST_STRPTR)"expansion.library", 33);
    if (!mho_expbase) {
        amt_case("zorro_boards", AMT_ST_SKIP, "expansion.library V33+ not available");
        return;
    }
    collect();
    CloseLibrary(mho_expbase);
    mho_expbase = 0;
    if (!nb) {
        amt_case("zorro_boards", AMT_ST_SKIP, "no autoconfig boards in the ConfigDev list");
        return;
    }
    aros = is_aros();

    amt_case_open("zorro_boards", AMT_ST_PASS);
    json_obj("ctx");
    json_arr("boards");
    for (i = 0; i < nb; i++)
        board_obj(&b[i]);
    json_arr_end();
    json_arr("mem");
    for (i = 0; i < nm; i++) {
        json_obj(0);
        json_hex("lo", mlo[i]);
        json_hex("hi", mhi[i]);
        json_obj_end();
    }
    json_arr_end();
    json_obj_end();
    amt_case_close();

    for (i = 0; i < nb; i++)
        bad[i] = (b[i].type & ERT_TYPEMASK) != ERT_ZORROII && (b[i].type & ERT_TYPEMASK) != ERT_ZORROIII;
    report("zorro_type", bad, "er_Type bits 7-6 = 11 (Zorro II) or 10 (Zorro III)",
           "a board in the ConfigDev list has an old-style/invalid type code");

    for (i = 0; i < nb; i++)
        bad[i] = b[i].z3 ? !(b[i].flags & ERFF_ZORRO_III)
                         : (b[i].flags & (ERFF_EXTENDED | ERFF_ZORRO_III | ERT_Z3_SSMASK)) != 0;
    report("zorro_flags", bad, "Zorro II: er_Flags bits 5-0 = 0; Zorro III: er_Flags bit 4 (ZORRO_III) = 1",
           "er_Flags contradicts the board type (configregs.h ERFF_EXTENDED/ERFF_ZORRO_III/sub-size)");

    for (i = 0; i < nb; i++)
        bad[i] = b[i].res03 != 0;
    report("zorro_reserved03", bad, "er_Reserved03 = 0 ($FF as read from the inverted board ROM)",
           "the reserved ExpansionRom byte 3 is not zero");

    for (i = 0; i < nb; i++)
        bad[i] = b[i].man == 0 || b[i].man == 0xffff;
    report("zorro_manufacturer", bad, "er_Manufacturer is an assigned id (not 0, not $FFFF)",
           "invalid manufacturer id (the board ROM reads as all 0 or all 1: bus or decode problem)");

    for (i = 0; i < nb; i++) {
        const struct bd *x = &b[i];
        /* A Zorro III board with a size code from the Zorro II table (no
         * EXTENDED, < 16 MB) may also be recorded as 16 MB: Kickstart 3.x
         * expansion.library gives every Zorro III board at least a 16 MB
         * slot and stores the slot size in cd_BoardSize (seen with KS 40.68
         * on Piccolo/Spectrum/Retina Z3, er_Type $8E/$8F/$87, er_Flags $10).
         * That is OS address-space policy, not the board's decode. */
        int z3slot = x->z3 && !(x->flags & ERFF_EXTENDED) && !x->lsize &&
                     x->phys && x->phys < 0x1000000UL && x->size == 0x1000000UL;
        int ok = x->phys != 0 && (x->size == x->phys || (x->lsize && x->size == x->lsize) || z3slot ||
                                  (x->z3 && x->sub == 1 && x->size && x->size <= x->phys && !(x->size & 0xffff)));
        bad[i] = !ok;
    }
    report("zorro_size", bad, "cd_BoardSize = size from er_Type bits 2-0 (Zorro III EXTENDED table if set), or the Zorro III sub-size, "
           "or 16 MB for a Zorro III board below 16 MB (expansion.library's minimum Zorro III slot)",
           "the board size the OS configured does not match the ExpansionRom size code");

    for (i = 0; i < nb; i++) {
        const struct bd *x = &b[i];
        int ok = 1;
        if (!(x->cdf & CDF_SHUTUP)) {
            int z2mem = in_range(x->addr, x->size, 0x200000UL, 0xa00000UL);
            int z2io = in_range(x->addr, x->size, 0xe90000UL, 0xf00000UL);
            if (x->z3)
                ok = in_range(x->addr, x->size, 0x10000000UL, 0x80000000UL) || z2mem || z2io;
            else
                ok = z2mem || z2io;
        }
        bad[i] = !ok;
    }
    report("zorro_address_range", bad,
           "Zorro II: $200000-$9FFFFF or $E90000-$EFFFFF; Zorro III: $10000000-$7FFFFFFF (or a Zorro II space)",
           "a configured board lies outside the address space of its bus type");

    for (i = 0; i < nb; i++) {
        const struct bd *x = &b[i];
        ULONG al = x->phys;
        bad[i] = 0;
        if (x->cdf & CDF_SHUTUP)
            continue;
        /* aligned to the size relative to the start of the space: the
         * Zorro II 8 MB space starts at $200000 (expansion.library puts a
         * 4 MB board at $200000) */
        if (!x->z3 && x->addr >= 0x200000UL && x->addr < 0xa00000UL)
            bad[i] = al && ((x->addr - 0x200000UL) & (al - 1));
        else if (al && (x->addr & (al - 1)))
            bad[i] = 1;
    }
    report("zorro_alignment", bad, "board address is a multiple of its (physical) size, counted from $200000 in the Zorro II 8 MB space",
           "a board was configured at an address its size code cannot decode");

    for (i = 0; i < nb; i++)
        bad[i] = 0;
    for (i = 0; i < nb; i++)
        for (j = i + 1; j < nb; j++) {
            const struct bd *x = &b[i], *y = &b[j];
            if ((x->cdf | y->cdf) & CDF_SHUTUP)
                continue;
            if (x->addr < y->addr + y->size && y->addr < x->addr + x->size)
                bad[i] = bad[j] = 1;
        }
    report("zorro_no_overlap", bad, "configured boards do not overlap",
           "two configured boards share addresses");

    /* memory nodes vs boards */
    for (i = 0; i < nb; i++)
        bad[i] = 0;
    for (j = 0; j < nm; j++) {
        int inside = -1;
        for (i = 0; i < nb; i++) {
            const struct bd *x = &b[i];
            if (x->cdf & CDF_SHUTUP)
                continue;
            if (mlo[j] >= x->addr && mhi[j] <= x->addr + x->size && (x->type & ERTF_MEMLIST))
                inside = i;
        }
        for (i = 0; i < nb; i++) {
            const struct bd *x = &b[i];
            if ((x->cdf & CDF_SHUTUP) || i == inside)
                continue;
            if (mlo[j] < x->addr + x->size && x->addr < mhi[j])
                bad[i] = 1;
        }
    }
    report("zorro_mem_overlap", bad,
           "each MemHeader lies inside one ERTF_MEMLIST board or outside all boards",
           "a memory list node overlaps a board that does not own it (I/O board or another board)");

    {
        int any = 0, missing = 0;
        for (i = 0; i < nb; i++) {
            const struct bd *x = &b[i];
            bad[i] = 0;
            if ((x->cdf & (CDF_SHUTUP | CDF_BADMEMORY)) || !(x->type & ERTF_MEMLIST))
                continue;
            any = 1;
            for (j = 0; j < nm; j++)
                if (mlo[j] >= x->addr && mhi[j] <= x->addr + x->size)
                    break;
            if (j == nm) {
                bad[i] = 1;
                missing = 1;
            }
        }
        if (!any)
            amt_case("zorro_memlist", AMT_ST_SKIP, "no configured ERTF_MEMLIST board");
        else if (missing && aros)
            amt_case("zorro_memlist", AMT_ST_SKIP,
                     "AROS ROM: its Zorro III RAM board is configured (ConfigDev present, cd_Flags CONFIGME still set) "
                     "but not in exec's MemList when the bootblock runs; Kickstart links it during autoconfig. "
                     "OS policy, not a hardware property, so a missing MemHeader is not tested here");
        else
            report("zorro_memlist", bad, "an ERTF_MEMLIST board (not CDF_BADMEMORY) has its RAM in exec's MemList",
                   "expansion did not link the board's RAM: the board failed its memory test or its size is wrong");
    }

    for (i = 0; i < nb; i++) {
        const struct bd *x = &b[i];
        bad[i] = (x->type & ERTF_DIAGVALID) && !(x->cdf & CDF_SHUTUP) && x->diag >= x->size;
    }
    report("zorro_diag_vector", bad, "ERTF_DIAGVALID: er_InitDiagVec < cd_BoardSize",
           "the DiagArea offset points outside the board");
}
