/* mmu (module id 20): the paged MMU of the 68030 and the 68040/68060
 * (full and LC parts; EC parts have none: one skip case).
 *
 * Every test builds its own translation tables in the any-pool, enables
 * translation only around the test (mm_run in mmu_asm.s, SR=$2700) and
 * switches it off again afterwards; the original TC/TT/CRP/SRP (030) and
 * TC/URP/SRP/ITTn/DTTn (040/060) are saved at the start and restored and
 * compared at the end (case regs_restored). If the OS already had the MMU
 * on (TC.E set when the module starts) nothing is run: its tables are
 * unknown, so its configuration could not be put back reliably.
 *
 * Memory map while a test runs:
 *   - 68030: every logical address outside the test window is identity
 *     mapped with early-termination descriptors (cache inhibited);
 *   - 68040/68060: identity pages (cache inhibited, serialized) for chip
 *     RAM, the CIAs, the custom chips, the ROM, the payload, the stack, the
 *     vector table and the pool; everything else is invalid;
 *   - the test window: a free 32 MB logical range whose pages map onto data
 *     pages in the pool (or, read-only, onto the ROM). Writes only ever go
 *     to the pool: logical pages that are not inside the pool are only
 *     written after basic_translate has shown that the MMU translates.
 * The data pages hold a marker per longword: (physical address ^ MKX), so a
 * value read through a translation says which physical address it came
 * from.
 *
 * Expectations: MC68030UM section 9 (PMMU), MC68040UM section 3 (MMU) and
 * 8 (access error frame), MC68060UM section 4 (MMU) and 8 (access error,
 * FSLW). Docs: docs/cases/mmu.md. */
#include "mod.h"
#include "cpu_checks.h"

#define POOL_SIZE  59392UL
#define T_SIZE     (16UL * 1024)     /* tables */
#define U_TOP      (18UL * 1024)     /* user stack top (below: user stack) */

AMT_MODULE(mmu, 20, AMT_PH_BARE, 0, 0, 59392);

/* ------------------------------------------------------------ asm side */
struct mm_regs {
    ULONG tc, crp[2], srp[2], tt0, tt1, urp, itt0, itt1, cacr, flags, mmusr;
};
struct mlog { UWORD vec, hsr; ULONG ssp; UWORD f[46]; };
extern struct mm_regs mm_cfg, mm_orig, mm_now;
extern volatile UWORD mm_fam, mm_vec, mm_nlog, mm_active;
extern volatile ULONG mm_in[15], mm_out[15], mm_old[64], mm_usp, p30_an;
extern ULONG (*volatile mm_hook)(ULONG idx);
extern volatile struct mlog mm_log[6];
extern const ULONG mm_stub_table[62];
ULONG mm_run(void (*fn)(void));
void mm_save_orig(void);
void mm_restore_orig(void);
void mm_read_now(void);
void mm_pflusha(void);
void mm_nop(void);
ULONG p30_ptest(ULONG addr, ULONG fc, ULONG kind);
void p30_pload(ULONG addr, ULONG fc, ULONG w);
void p30_pflush(ULONG kind, ULONG fc, ULONG addr);
void p30_dclr(void);
ULONG p40_ptest(ULONG addr, ULONG fc, ULONG w);
void p40_pflush(ULONG kind, ULONG fc, ULONG addr);
ULONG p60_plpa(ULONG addr, ULONG fc, ULONG w);
void p40_cpusha_dc(void);
void p40_cinvl_dc(ULONG pa);
void p40_cpushl_dc(ULONG pa);
ULONG mm_moves_get(ULONG addr, ULONG fc);
void mm_moves_put(ULONG addr, ULONG val, ULONG fc, ULONG size);
#define SN(n) extern UBYTE sn_##n[], sn_##n##_at[]
SN(rd); SN(wr); SN(addq); SN(rdpi); SN(wrpi); SN(urd); SN(uwr); SN(moves_wr);
SN(plpar); SN(plpaw); SN(cfg_rp); SN(cfg_tc); SN(tas);
extern UBYTE sn_load_rp30[];

#define A(x) ((ULONG)(x))
#define VL(a) (*(volatile ULONG *)(a))

/* ------------------------------------------------------------ variant */
static int fam;                   /* 3 = 68030, 4 = 68040/LC040, 6 = 68060/LC060 */
#define IS30 (fam == 3)
#define IS40 (fam == 4)
#define IS60 (fam == 6)
static int broken;                /* basic_translate failed: skip the rest */
static ULONG pbase, tlo, thi, tp, dlo, dhi;
static ULONG win, rom, chip_top;
static int t_over;                /* table area overflow */

#define MKX 0x5aa5c33cUL
static ULONG mk(ULONG pa) { return pa ^ MKX; }
static void fill_data(void)
{
    ULONG a;
    for (a = dlo; a < dhi; a += 4)
        VL(a) = mk(a);
}
static ULONG dpage(ULONG ps, int k) { return ((dlo + ps - 1) & ~(ps - 1)) + (ULONG)k * ps; }
static int dcount(ULONG ps) { return (int)((dhi - dpage(ps, 0)) / ps); }

/* ranges that must stay identity mapped while a test runs */
struct rng { ULONG lo, hi; };
static struct rng need[10];
static int nneed;
static void need_add(ULONG lo, ULONG hi)
{
    if (nneed < 10) {
        need[nneed].lo = lo & ~0x1fffUL;
        need[nneed].hi = (hi + 0x1fff) & ~0x1fffUL;
        nneed++;
    }
}
static int overlaps(ULONG lo, ULONG len)
{
    int i;
    for (i = 0; i < nneed; i++)
        if (need[i].lo < lo + len && lo < need[i].hi)
            return 1;
    return 0;
}

/* ------------------------------------------------------------ tables */
static void tb_reset(void) { tp = tlo; t_over = 0; }
static ULONG talloc(ULONG size, ULONG align)
{
    ULONG a = (tp + align - 1) & ~(align - 1), i;
    if (a + size > thi) {
        t_over = 1;
        a = tlo;                  /* keeps the pointers in the pool */
    } else
        tp = a + size;
    for (i = 0; i < size; i += 4)
        VL(a + i) = 0;
    return a;
}

/* --- 68030 --- */
struct c30 { UBYTE ps, is, ti[4], nl; };
static struct c30 C3;
static int L8;                    /* long (8-byte) descriptors in every table */
static ULONG r30;                 /* level-A table */
#define INV30 0x494e5600UL        /* DT=0: deliberately invalid (not filled) */
#define D_WP  0x04UL
#define D_U   0x08UL
#define D_M   0x10UL
#define D_CI  0x40UL
#define D_S30 0x100UL             /* 030 long format only */
#define D_S40 0x80UL
#define D_G   0x400UL
#define CM_WT 0x00UL
#define CM_CB 0x20UL
#define CM_CI 0x40UL
#define CM_CIN 0x60UL

static const struct c30 c30_def = { 12, 0, { 8, 6, 6, 0 }, 3 };

static int inwin(ULONG la) { return la - win < 0x02000000UL; }
static int sh30(int lev)
{
    int s = 32 - C3.is, i;
    for (i = 0; i <= lev; i++)
        s -= C3.ti[i];
    return s;
}
static ULONG ix30(ULONG la, int lev) { return (la >> sh30(lev)) & ((1UL << C3.ti[lev]) - 1); }
static ULONG *ent30(ULONG t, ULONG i) { return (ULONG *)(t + i * (L8 ? 8 : 4)); }
static ULONG tnew30(int lev, int inv)
{
    ULONG n = 1UL << C3.ti[lev], t = talloc(n * (L8 ? 8 : 4), 16), i;
    if (inv)
        for (i = 0; i < n; i++)
            ent30(t, i)[0] = INV30;
    return t;
}
static void td30(ULONG *e, ULONG t)
{
    if (L8) {
        e[0] = 0x7fff0000UL | 0xfc00UL | 3;
        e[1] = t;
    } else
        e[0] = t | 2;
}
static ULONG tad30(ULONG *e) { return (L8 ? e[1] : e[0]) & ~0xfUL; }
static void pd30(ULONG *e, ULONG pa, ULONG fl)
{
    if (L8) {
        e[0] = 0x7fff0000UL | 0xfc00UL | (fl & 0x15c) | 1;
        e[1] = pa & ~0xffUL;
    } else
        e[0] = (pa & ~0xffUL) | (fl & 0x5c) | 1;
}
/* descriptor slot for la at level lev; creates the tables above it */
static ULONG *slot30(ULONG la, int lev)
{
    ULONG t = r30;
    int l;
    for (l = 0;; l++) {
        ULONG *e = ent30(t, ix30(la, l));
        ULONG dt = e[0] & 3;
        if (l == lev)
            return e;
        if (dt >= 2) {
            t = tad30(e);
            continue;
        }
        t = tnew30(l + 1, inwin(la));
        td30(e, t);
    }
}
static void fill30(ULONG t, int lev, ULONG base)
{
    ULONG n = 1UL << C3.ti[lev], i;
    for (i = 0; i < n; i++) {
        ULONG *e = ent30(t, i), la = base + (i << sh30(lev));
        if (e[0] == 0) {
            if (inwin(la))
                e[0] = INV30;
            else
                pd30(e, la, D_CI);
        } else if ((e[0] & 3) >= 2 && lev < C3.nl - 1)
            fill30(tad30(e), lev + 1, la);
    }
}
static ULONG tc30(void)
{
    return 0x80000000UL | ((ULONG)C3.ps << 20) | ((ULONG)C3.is << 16) | ((ULONG)C3.ti[0] << 12) |
           ((ULONG)C3.ti[1] << 8) | ((ULONG)C3.ti[2] << 4) | C3.ti[3];
}
static void rp30(ULONG *rp, ULONG t)
{
    rp[0] = 0x7fff0000UL | (L8 ? 3 : 2);
    rp[1] = t;
}

/* --- 68040/68060 --- */
static int P8;                    /* 8 KB pages */
static ULONG r40;                 /* root table */
#define PS40 (P8 ? 8192UL : 4096UL)
static ULONG *slot40r(ULONG root, ULONG la)
{
    ULONG *re = (ULONG *)root + (la >> 25), *pe, pt;
    if (!(*re & 2))
        *re = talloc(512, 512) | 2;
    pt = *re & ~0x1ffUL;
    pe = (ULONG *)pt + ((la >> 18) & 127);
    if (!(*pe & 2))
        *pe = talloc(P8 ? 128 : 256, P8 ? 128 : 256) | 2;
    pt = *pe & (P8 ? ~0x7fUL : ~0xffUL);
    return (ULONG *)pt + (P8 ? ((la >> 13) & 31) : ((la >> 12) & 63));
}
static void id40(ULONG root)
{
    int i;
    ULONG a;
    for (i = 0; i < nneed; i++)
        for (a = need[i].lo & ~(PS40 - 1); a < need[i].hi; a += PS40) {
            ULONG *s = slot40r(root, a);
            if (!*s)
                *s = a | CM_CI | 1;
        }
}

/* --- generic (both) --- */
static ULONG gps;                 /* page size of the current tables */
static void gb_begin(ULONG ps)
{
    tb_reset();
    gps = ps;
    mm_cfg.tt0 = mm_cfg.tt1 = mm_cfg.itt0 = mm_cfg.itt1 = 0;
    mm_cfg.cacr = 0;
    mm_cfg.flags = 0;
    if (IS30) {
        if (ps == 4096)
            C3 = c30_def;
        else {
            int p = 0, r;
            while ((1UL << p) < ps)
                p++;
            r = 24 - p;
            C3.ps = (UBYTE)p;
            C3.is = 0;
            C3.ti[0] = 8;
            C3.ti[1] = (UBYTE)((r + 1) / 2);
            C3.ti[2] = (UBYTE)(r - (r + 1) / 2);
            C3.ti[3] = 0;
            C3.nl = 3;
        }
        L8 = 0;
        r30 = tnew30(0, 0);
    } else {
        P8 = ps == 8192;
        r40 = talloc(512, 512);
    }
}
static void gb_begin30(const struct c30 *c, int l8)
{
    tb_reset();
    C3 = *c;
    L8 = l8;
    gps = 1UL << c->ps;
    mm_cfg.tt0 = mm_cfg.tt1 = 0;
    mm_cfg.cacr = 0;
    mm_cfg.flags = 0;
    r30 = tnew30(0, 0);
}
static ULONG *gb_slot(ULONG la)
{
    return IS30 ? slot30(la, C3.nl - 1) : slot40r(r40, la);
}
/* fl: D_WP, D_CI/CM_*, D_U, D_M, D_G (040/060), "S" via gb_super */
static void gb_map(ULONG la, ULONG pa, ULONG fl)
{
    ULONG *s = gb_slot(la);
    if (IS30)
        pd30(s, pa, fl);
    else
        *s = (pa & ~(PS40 - 1)) | (fl & 0x7fc) | 1;
}
static ULONG D_S(void) { return IS30 ? D_S30 : D_S40; }
static void gb_inv(ULONG la)
{
    ULONG *s = gb_slot(la);
    s[0] = INV30;
    if (IS30 && L8)
        s[1] = 0;
}
static ULONG gb_pdval(ULONG pa, ULONG fl)       /* page descriptor word 0 value */
{
    if (IS30)
        return L8 ? (0x7fff0000UL | 0xfc00UL | (fl & 0x15c) | 1) : ((pa & ~0xffUL) | (fl & 0x5c) | 1);
    return (pa & ~(PS40 - 1)) | (fl & 0x7fc) | 1;
}
static void gb_end(void)
{
    if (IS30) {
        fill30(r30, 0, 0);
        mm_cfg.tc = tc30();
        rp30(mm_cfg.crp, r30);
        rp30(mm_cfg.srp, r30);
    } else {
        id40(r40);
        mm_cfg.tc = 0x8000UL | (P8 ? 0x4000UL : 0);
        mm_cfg.urp = mm_cfg.srp[0] = r40;
    }
}
static ULONG LW(int i) { return win + (ULONG)i * gps; }

/* ------------------------------------------------------------ run + log */
static ULONG fl32(volatile UWORD *f, int w) { return ((ULONG)f[w] << 16) | f[w + 1]; }
static int nwords(int i)
{
    int fmt = mm_log[i].f[3] >> 12;
    switch (fmt) {
    case 0: return 4;
    case 2: return 6;
    case 4: return 8;
    case 7: return 30;
    case 0xa: return 16;
    case 0xb: return 46;
    }
    return 12;
}
/* 68030 instruction-stream fault: SSW DF=0 with FB or FC set. The data cycle
 * fault address (+$10) is valid only for DF=1; the address of the faulted
 * prefetch is the stage B address (+$24, long format $B frame only), stage C
 * being stage B - 2 (MC68030UM 8.2.1, figures 8-7/8-8 and the SSW table). */
static int ifetch30(int i)
{
    volatile UWORD *f = mm_log[i].f;
    return IS30 && !(f[5] & 0x0100) && (f[5] & 0xc000);
}
static ULONG fa_of(int i)
{
    volatile UWORD *f = mm_log[i].f;
    if (IS30) {
        if (ifetch30(i) && (f[3] >> 12) == 0xb)
            return fl32(f, 18) - ((f[5] & 0x8000) ? 2 : 0);
        return fl32(f, 8);
    }
    if (IS40)
        return fl32(f, 10);
    return fl32(f, 4);
}

static void in_clear(void)
{
    int i;
    for (i = 0; i < 15; i++)
        mm_in[i] = 0;
}

/* --- fix-and-resume hook --- */
struct fixe { ULONG lo, hi; ULONG *d; ULONG v; int ifetch; };
static struct fixe fx[3];
static int nfx;
static volatile ULONG nest_addr, nest_val;
static void fix_add(ULONG lo, ULONG len, ULONG *d, ULONG v)
{
    fx[nfx].lo = lo;
    fx[nfx].hi = lo + len;
    fx[nfx].d = d;
    fx[nfx].v = v;
    fx[nfx].ifetch = 0;
    nfx++;
}
/* a fix for an instruction-fetch fault: on the 030 it matches only a fault
 * whose SSW says FB/FC (and, in a short format $A frame, which carries no
 * stage B address, it matches on that alone) */
static void fix_add_ifetch(ULONG lo, ULONG len, ULONG *d, ULONG v)
{
    fix_add(lo, len, d, v);
    fx[nfx - 1].ifetch = 1;
}
static int wb_done;
static void wb040(ULONG idx)
{
    static const UBYTE sw[3] = { 9, 8, 7 };       /* WB1S, WB2S, WB3S */
    static const UBYTE aw[3] = { 20, 16, 12 };    /* WB1A, WB2A, WB3A */
    static const UBYTE dw[3] = { 22, 18, 14 };    /* WB1D, WB2D, WB3D */
    volatile UWORD *fr = (volatile UWORD *)mm_log[idx].ssp;
    int n;
    for (n = 0; n < 3; n++) {
        UWORD s = fr[sw[n]];
        ULONG sz = (s >> 5) & 3;
        if (!(s & 0x80) || sz == 3)
            continue;
        {
            ULONG addr = fl32(fr, aw[n]), data = fl32(fr, dw[n]);
            if (n == 0)
                data = cpu_wb1_data(data, addr, sz);
            mm_moves_put(addr, data, s & 7, sz);
        }
        fr[sw[n]] = s & ~0x80;
        wb_done++;
    }
}
static ULONG hook_fix(ULONG idx)
{
    ULONG fa;
    int i;
    if (mm_log[idx].vec != 2 || idx >= 4)
        return 0;
    fa = fa_of((int)idx);
    if (nest_addr && idx == 0)
        nest_val = VL(nest_addr);           /* faults again (nested) */
    /* a misaligned access may report the operand start (040 SSW MA / 060
     * FSLW MA) while the page that faulted is the next one: match any fix
     * whose page overlaps the 8 bytes from the fault address */
    for (i = 0; i < nfx; i++) {
        if (IS30 && fx[i].ifetch != ifetch30((int)idx))
            continue;
        if ((IS30 && fx[i].ifetch && (mm_log[idx].f[3] >> 12) == 0xa) ||
            (fa < fx[i].hi && fa + 8 > fx[i].lo)) {
            *fx[i].d = fx[i].v;
            break;
        }
    }
    if (i == nfx)
        return 0;
    mm_pflusha();
    if (IS40)
        wb040(idx);
    return 1;
}
static void hook_set(int on)
{
    mm_hook = on ? hook_fix : 0;
    nfx = 0;
    nest_addr = 0;
    nest_val = 0;
    wb_done = 0;
}

/* --- op lists run with the MMU on --- */
#define O_R    0      /* r = *(long *)a */
#define O_W    1      /* *(long *)a = v */
#define O_RB   2      /* r = *(byte *)a */
#define O_MR   3      /* r = MOVES.L (a) with SFC = fc */
#define O_MW   4      /* MOVES.L v,(a) with DFC = fc */
#define O_FL   5      /* flush kind v (030 p30_pflush / 040 p40_pflush), fc, a */
#define O_PT   6      /* 030: r = PTEST kind v; 040: r = PTEST (v = write); 060: r = PLPA */
#define O_PL   7      /* 030 PLOAD (v = write) */
#define O_CALL 8      /* r = ((ULONG (*)(void))a)() */
#define O_C    9      /* cache: v=0 CPUSHA DC, 1 CINVL DC,(a), 2 CPUSHL DC,(a), 3 030 CD */
#define O_NOP  10
struct op { ULONG a, v, r, r2; UBYTE k, fc; };
#define MAXOPS 500
static struct op ops[MAXOPS];
static int nops;
static int op(int k, ULONG a, ULONG v, int fc)
{
    struct op *o = &ops[nops < MAXOPS ? nops : MAXOPS - 1];
    o->k = (UBYTE)k;
    o->a = a;
    o->v = v;
    o->fc = (UBYTE)fc;
    o->r = o->r2 = 0xdeadbeefUL;
    return nops < MAXOPS ? nops++ : nops - 1;
}
static void b_ops(void)
{
    int i;
    for (i = 0; i < nops; i++) {
        struct op *o = &ops[i];
        switch (o->k) {
        case O_R: o->r = VL(o->a); break;
        case O_W: VL(o->a) = o->v; break;
        case O_RB: o->r = *(volatile UBYTE *)o->a; break;
        case O_MR: o->r = mm_moves_get(o->a, o->fc); break;
        case O_MW: mm_moves_put(o->a, o->v, o->fc, 0); break;
        case O_FL:
            if (IS30)
                p30_pflush(o->v, o->fc, o->a);
            else
                p40_pflush(o->v, o->fc, o->a);
            break;
        case O_PT:
            if (IS30) {
                o->r = p30_ptest(o->a, o->fc, o->v);
                o->r2 = p30_an;
            } else if (IS40)
                o->r = p40_ptest(o->a, o->fc, o->v);
            else
                o->r = p60_plpa(o->a, o->fc, o->v);
            break;
        case O_PL: p30_pload(o->a, o->fc, o->v); break;
        case O_CALL: o->r = ((ULONG (*)(void))o->a)(); break;
        case O_C:
            if (o->v == 0)
                p40_cpusha_dc();
            else if (o->v == 1)
                p40_cinvl_dc(o->a);
            else if (o->v == 2)
                p40_cpushl_dc(o->a);
            else
                p30_dclr();
            break;
        default: mm_nop(); break;
        }
    }
}
/* --- inputs of the last run, for self-contained failure records --- */
struct snap { ULONG la, at, b0, b1; UBYTE lev, ok; };
static struct snap sn[8];
static int nsn;
static int run_ops_last;          /* the last run was an op list */
static ULONG run_fn;              /* else: the snippet */

/* descriptor slot for la without creating tables; *lev = level found */
static ULONG *find_desc(ULONG la, int *lev)
{
    if (IS30) {
        ULONG t = r30;
        int l;
        for (l = 0;; l++) {
            ULONG *e = ent30(t, ix30(la, l));
            *lev = l;
            if (l == C3.nl - 1 || (e[0] & 3) < 2)
                return e;
            t = tad30(e);
        }
    } else {
        ULONG root = (mm_cfg.srp[0] & ~0x1ffUL), *re, *pe, pt;
        *lev = 0;
        if (!root)
            return 0;
        re = (ULONG *)root + (la >> 25);
        if (!(*re & 2))
            return re;
        *lev = 1;
        pe = (ULONG *)(*re & ~0x1ffUL) + ((la >> 18) & 127);
        if (!(*pe & 2))
            return pe;
        *lev = 2;
        pt = *pe & (P8 ? ~0x7fUL : ~0xffUL);
        return (ULONG *)pt + (P8 ? ((la >> 13) & 31) : ((la >> 12) & 63));
    }
}
static void snap_add(ULONG la)
{
    int i, lev;
    ULONG *e, pg = la & ~(gps ? gps - 1 : 0xfffUL);
    if (nsn >= 8)
        return;
    for (i = 0; i < nsn; i++)
        if (sn[i].la == pg)
            return;
    e = find_desc(pg, &lev);
    if (!e)
        return;
    sn[nsn].la = pg;
    sn[nsn].at = A(e);
    sn[nsn].b0 = e[0];
    sn[nsn].b1 = (IS30 && L8) ? e[1] : 0;
    sn[nsn].lev = (UBYTE)lev;
    sn[nsn].ok = 1;
    nsn++;
}
static ULONG RUN(void (*fn)(void))
{
    int i;
    nsn = 0;
    run_ops_last = fn == b_ops;
    run_fn = A(fn);
    if (!(mm_cfg.flags & 1)) {
        if (run_ops_last) {
            for (i = 0; i < nops; i++)
                if (ops[i].k != O_FL && ops[i].k != O_C && ops[i].k != O_NOP &&
                    !(ops[i].k == O_W && ops[i].a >= tlo && ops[i].a < thi))
                    snap_add(ops[i].a);
        } else {
            snap_add(mm_in[8]);
            if (nest_addr)
                snap_add(nest_addr);
        }
        snap_add(win);
    }
    return mm_run(fn);
}

/* the instruction a snippet tests: text, opcode words, FC of the access */
struct sninfo { UBYTE *fn, *at; const char *txt; UBYTE fc; };
static const struct sninfo sninfo[] = {
    { sn_rd, sn_rd_at, "MOVE.L (A0),D0", 5 },
    { sn_wr, sn_wr_at, "MOVE.L D1,(A0)", 5 },
    { sn_addq, sn_addq_at, "ADDQ.L #1,(A0)", 5 },
    { sn_rdpi, sn_rdpi_at, "MOVE.L (A0)+,D0", 5 },
    { sn_wrpi, sn_wrpi_at, "MOVE.L D1,(A0)+", 5 },
    { sn_urd, sn_urd_at, "user mode: MOVE.L (A0),D0 then TRAP #0", 1 },
    { sn_uwr, sn_uwr_at, "user mode: MOVE.L D1,(A0) then TRAP #0", 1 },
    { sn_moves_wr, sn_moves_wr_at, "MOVEC D2,DFC; MOVES.L D1,(A0)", 0xff },
    { sn_plpar, sn_plpar_at, "MOVEC D2,DFC; PLPAR (A0)", 0xff },
    { sn_plpaw, sn_plpaw_at, "MOVEC D2,DFC; PLPAW (A0)", 0xff },
    { sn_cfg_rp, sn_cfg_rp_at, "PMOVE (A0),CRP (MMU off)", 7 },
    { sn_cfg_tc, sn_cfg_tc_at, "PMOVE (A1),TT0; PMOVE (A0),TC", 7 },
    { sn_tas, sn_tas_at, "TAS (A0)", 5 },
};

/* text of an op (what the MMU-on body executed) */
static const char *op_txt(const struct op *o)
{
    static const char *const pt30[16] = {
        "PTESTR D1,(A0),#0", "PTESTW D1,(A0),#0", "PTESTR D1,(A0),#1,A1", "PTESTW D1,(A0),#1,A1",
        "PTESTR D1,(A0),#2,A1", "PTESTW D1,(A0),#2,A1", "PTESTR D1,(A0),#3,A1", "PTESTW D1,(A0),#3,A1",
        "PTESTR D1,(A0),#4,A1", "PTESTW D1,(A0),#4,A1", "PTESTR D1,(A0),#5,A1", "PTESTW D1,(A0),#5,A1",
        "PTESTR D1,(A0),#6,A1", "PTESTW D1,(A0),#6,A1", "PTESTR D1,(A0),#7,A1", "PTESTW D1,(A0),#7,A1" };
    static const char *const fl30[5] = { "PFLUSHA", "PFLUSH D1,#7", "PFLUSH D1,#4",
                                         "PFLUSH D1,#7,(A0)", "PFLUSH D1,#0" };
    static const char *const fl40[4] = { "PFLUSHA", "PFLUSHAN", "PFLUSH (A0)", "PFLUSHN (A0)" };
    static const char *const cc[4] = { "CPUSHA DC", "CINVL DC,(A0)", "CPUSHL DC,(A0)",
                                       "MOVEC CACR: set CD (030 clear data cache)" };
    switch (o->k) {
    case O_R: return "MOVE.L (A0),D0";
    case O_W: return "MOVE.L D1,(A0)";
    case O_RB: return "MOVE.B (A0),D0";
    case O_MR: return "MOVEC D0,SFC; MOVES.L (A0),D0";
    case O_MW: return "MOVEC D0,DFC; MOVES.L D1,(A0)";
    case O_FL: return IS30 ? fl30[o->v < 5 ? o->v : 0] : fl40[o->v & 3];
    case O_PT: return IS30 ? pt30[o->v & 15] : IS40 ? (o->v ? "MOVEC D0,DFC; PTESTW (A0); MOVEC MMUSR,D0" : "MOVEC D0,DFC; PTESTR (A0); MOVEC MMUSR,D0")
                                                    : (o->v ? "MOVEC D0,DFC; PLPAW (A0)" : "MOVEC D0,DFC; PLPAR (A0)");
    case O_PL: return o->v ? "PLOADW D1,(A0)" : "PLOADR D1,(A0)";
    case O_CALL: return "JSR (A0)";
    case O_C: return cc[o->v & 3];
    }
    return "NOP";
}
/* opcode words of an op's MMU instruction (0 = plain access) */
static int op_words(const struct op *o, UWORD *w)
{
    if (o->k == O_PT) {
        if (IS30) {
            ULONG lv = o->v >> 1, r = !(o->v & 1);
            w[0] = 0xf010;
            w[1] = (UWORD)(0x8000 | (lv << 10) | (r << 9) | (lv ? 0x0120 : 0) | 0x09);
            return 2;
        }
        w[0] = IS40 ? (o->v ? 0xf548 : 0xf568) : (o->v ? 0xf588 : 0xf5c8);
        return 1;
    }
    if (o->k == O_PL) {
        w[0] = 0xf010;
        w[1] = o->v ? 0x2009 : 0x2209;
        return 2;
    }
    if (o->k == O_FL) {
        if (IS30) {
            static const UWORD x[5] = { 0x2400, 0x30e9, 0x3089, 0x38e9, 0x3009 };
            w[0] = o->v == 3 ? 0xf010 : 0xf000;
            w[1] = x[o->v < 5 ? o->v : 0];
            return 2;
        }
        w[0] = (UWORD)(0xf500 | ((o->v == 0 ? 3 : o->v == 1 ? 2 : o->v == 2 ? 1 : 0) << 3));
        return 1;
    }
    if (o->k == O_MR || o->k == O_MW) {
        w[0] = 0x0e90;
        w[1] = o->k == O_MW ? 0x1800 : 0x0000;
        return 2;
    }
    return 0;
}

static void emit_inputs(void)
{
    int i, j;
    UWORD w[3];
    json_obj("mmu");
    json_hex("tc", mm_cfg.tc);
    if (mm_cfg.flags & 1)
        json_str("note", "MMU left off for this run (TC written only by the snippet)");
    if (IS30) {
        json_hex("crp_hi", mm_cfg.crp[0]);
        json_hex("crp_lo", mm_cfg.crp[1]);
        json_hex("srp_hi", mm_cfg.srp[0]);
        json_hex("srp_lo", mm_cfg.srp[1]);
        json_hex("tt0", mm_cfg.tt0);
        json_hex("tt1", mm_cfg.tt1);
        json_str("layout", L8 ? "long (8-byte) descriptors in every table"
                              : "short (4-byte) descriptors");
    } else {
        json_hex("urp", mm_cfg.urp);
        json_hex("srp", mm_cfg.srp[0]);
        json_hex("dtt0", mm_cfg.tt0);
        json_hex("dtt1", mm_cfg.tt1);
        json_hex("itt0", mm_cfg.itt0);
        json_hex("itt1", mm_cfg.itt1);
    }
    if (mm_cfg.cacr)
        json_hex("cacr_or", mm_cfg.cacr);
    json_obj_end();
    if (nsn) {
        json_arr("desc");
        for (i = 0; i < nsn; i++) {
            ULONG *e = (ULONG *)sn[i].at;
            json_obj(NULL);
            json_hex("la", sn[i].la);
            json_int("level", sn[i].lev);
            json_hex("at", sn[i].at);
            json_hex("before", sn[i].b0);
            if (IS30 && L8)
                json_hex("before_w1", sn[i].b1);
            json_hex("after", e[0]);
            json_obj_end();
        }
        json_arr_end();
        json_str("desc_note", IS30 ? "030 descriptor of each page at the level the walk ends (0 = A); "
                                     "DT 1-0: 0 invalid, 1 page/early termination, 2/3 table/indirect"
                                   : "040/060 descriptor of each page (level 0 root, 1 pointer, 2 page); "
                                     "PDT 1-0: 01/11 resident, 10 indirect, 00 invalid");
    }
    if (run_ops_last) {
        json_arr("ops");
        for (i = 0; i < nops && i < 24; i++) {
            const struct op *o = &ops[i];
            int n = op_words(o, w);
            json_obj(NULL);
            json_str("insn", op_txt(o));
            if (n) {
                json_arr("opcode");
                for (j = 0; j < n; j++)
                    json_hexn(NULL, w[j], 4);
                json_arr_end();
            }
            json_hex("a0", o->a);
            if (o->k == O_MR || o->k == O_MW || o->k == O_PT || o->k == O_PL ||
                (o->k == O_FL && IS30))
                json_int(IS30 && (o->k == O_PT || o->k == O_PL || o->k == O_FL) ? "d1_fc" : "fc", o->fc);
            if (o->k == O_W || o->k == O_MW)
                json_hex("d1", o->v);
            if (o->k == O_R || o->k == O_RB || o->k == O_MR || o->k == O_PT || o->k == O_CALL)
                json_hex("result", o->r);
            if (IS30 && o->k == O_PT && (o->v >> 1))
                json_hex("a1", o->r2);
            json_obj_end();
        }
        json_arr_end();
        if (nops > 24)
            json_int("ops_total", nops);
        json_str("ops_note", "executed in order with the MMU on, SR=$2700; result = D0 (MMUSR for "
                             "PTEST, physical address for PLPA)");
    } else {
        for (i = 0; i < (int)(sizeof sninfo / sizeof sninfo[0]); i++)
            if (A(sninfo[i].fn) == run_fn) {
                const UWORD *p = (const UWORD *)sninfo[i].at;
                json_obj("insn");
                json_str("text", sninfo[i].txt);
                json_arr("opcode");
                for (j = 0; j < 3; j++)
                    json_hexn(NULL, p[j], 4);
                json_arr_end();
                json_hex("a0", mm_in[8]);
                json_hex("d1", mm_in[1]);
                json_int("fc", sninfo[i].fc == 0xff ? (long)mm_in[2] : sninfo[i].fc);
                if (sninfo[i].fc == 0xff)
                    json_hex("d2", mm_in[2]);
                if (sninfo[i].fc == 1)
                    json_hex("usp", mm_usp);
                json_obj_end();
            }
        if (nfx) {
            json_arr("hook_fixes");
            for (i = 0; i < nfx; i++) {
                json_obj(NULL);
                json_hex("page", fx[i].lo);
                json_hex("desc_at", A(fx[i].d));
                json_hex("new_desc", fx[i].v);
                json_obj_end();
            }
            json_arr_end();
            json_str("hook", "on vector 2 the handler writes new_desc for the page overlapping "
                             "FA..FA+7, PFLUSHA (040: then performs the valid writebacks with "
                             "MOVES) and RTEs; any other exception ends the run");
        }
        if (nest_addr)
            json_hex("nested_read_in_handler", nest_addr);
    }
}

static ULONG run_ops(void)
{
    in_clear();
    return RUN(b_ops);
}

/* ------------------------------------------------------------ reporting */
#define NF 14
struct fld { const char *k; ULONG e, e2, g; UBYTE alt; };
static struct fld fe[NF];
static int nfe;
struct kv { const char *k; ULONG v; };
static struct kv kin[NF], kcx[NF];
static int nin, ncx;
static const char *why;
static ULONG last_vec;

static void rs(const char *w)
{
    nfe = nin = ncx = 0;
    why = w;
    last_vec = 0;
}
static int ck(const char *k, ULONG e, ULONG g)
{
    if (e == g)
        return 1;
    if (nfe < NF) {
        fe[nfe].k = k;
        fe[nfe].e = e;
        fe[nfe].g = g;
        fe[nfe].alt = 0;
        nfe++;
    }
    return 0;
}
static int ck2(const char *k, ULONG e, ULONG e2, ULONG g)
{
    if (e == g || e2 == g)
        return 1;
    if (nfe < NF) {
        fe[nfe].k = k;
        fe[nfe].e = e;
        fe[nfe].e2 = e2;
        fe[nfe].g = g;
        fe[nfe].alt = 1;
        nfe++;
    }
    return 0;
}
static void ci(const char *k, ULONG v) { if (nin < NF) { kin[nin].k = k; kin[nin].v = v; nin++; } }
static void cx(const char *k, ULONG v) { if (ncx < NF) { kcx[ncx].k = k; kcx[ncx].v = v; ncx++; } }

static void hexstr(char *p, ULONG v)
{
    int i;
    for (i = 7; i >= 0; i--) {
        int d = (int)(v >> (i * 4)) & 15;
        *p++ = (char)(d < 10 ? '0' + d : 'a' + d - 10);
    }
    *p = 0;
}

static void rend(const char *id)
{
    int i, j;
    if (t_over && !nfe)
        ck("table_area_overflow", 0, 1);
    if (!nfe) {
        amt_case(id, AMT_ST_PASS, 0);
        return;
    }
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    for (i = 0; i < nfe; i++) {
        if (fe[i].alt) {
            char b[20];
            hexstr(b, fe[i].e);
            b[8] = '|';
            hexstr(b + 9, fe[i].e2);
            json_str(fe[i].k, b);
        } else
            json_hex(fe[i].k, fe[i].e);
    }
    json_obj_end();
    json_obj("got");
    for (i = 0; i < nfe; i++)
        json_hex(fe[i].k, fe[i].g);
    /* observed values that were not compared exactly (full MMUSR, ...) */
    for (i = 0; i < ncx; i++)
        json_hex(kcx[i].k, kcx[i].v);
    if (mm_nlog) {
        json_int("exc0_vec", mm_log[0].vec);
        json_arr("exc0_frame");
        for (j = 0; j < nwords(0); j++)
            json_hexn(NULL, mm_log[0].f[j], 4);
        json_arr_end();
    }
    json_obj_end();
    json_str("why", why);
    json_obj("in");
    for (i = 0; i < nin; i++)
        json_hex(kin[i].k, kin[i].v);
    emit_inputs();
    json_obj_end();
    json_obj("ctx");
    json_str("cpu", amt_probe_result.cpu);
    json_str("markers", "data pages hold (physical address ^ 5aa5c33c) per longword");
    json_hex("window", win);
    json_hex("pool", pbase);
    json_hex("tc", mm_cfg.tc);
    json_int("vec", (long)last_vec);
    if (mm_nlog) {
        json_arr("exc");
        for (i = 0; i < mm_nlog && i < 6; i++) {
            json_obj(NULL);
            json_int("vec", mm_log[i].vec);
            json_hexn("handler_sr", mm_log[i].hsr, 4);
            json_arr("frame");
            for (j = 0; j < nwords(i); j++)
                json_hexn(NULL, mm_log[i].f[j], 4);
            json_arr_end();
            json_obj_end();
        }
        json_arr_end();
    }
    json_obj_end();
    amt_case_close();
}
static void skip(const char *id, const char *w) { amt_case(id, AMT_ST_SKIP, w); }
static int pre(const char *id)
{
    if (broken) {
        skip(id, "basic_translate failed: the MMU does not translate, so the other MMU cases cannot run");
        return 1;
    }
    return 0;
}

/* ------------------------------------------------------------ fault checks */
#define K_INV 0
#define K_WP  1
#define K_SUP 2
#define K_TTW 3     /* 040/060 transparent translation write protect */

/* checks the access fault logged at index i: data access of a longword at
 * addr, write or read, user or supervisor, cause kind; data = the value of
 * a write; pc = address of the faulting instruction (060: stacked PC) */
static void chk_fault(int i, ULONG addr, int write, int user, int kind, ULONG data, ULONG pc)
{
    volatile UWORD *f = mm_log[i].f;
    ULONG fc = user ? 1 : 5;
    cx("fault_kind", (ULONG)kind);
    if (!ck("vec", 2, mm_log[i].vec))
        return;
    cx("undef_stacked_pc_off", fl32(f, 1) - pc);
    if (IS30) {
        UWORD fmt = f[3] >> 12;
        ck2("format", 0xa, 0xb, fmt);
        ck("vector_offset", 0x008, f[3] & 0xfff);
        ck("fault_addr", addr, fl32(f, 8));
        /* SSW: DF (bit 8) = 1 data cycle fault (rerun on RTE), RM = 0,
         * RW (bit 6) 1 = read, SIZE (5-4) 00 = long, FC (2-0) */
        ck("ssw_df_rm_rw_size_fc", 0x0100 | (write ? 0 : 0x40) | fc, f[5] & 0x01f7);
        if (write)
            ck("data_output_buffer", data, fl32(f, 12));
    } else if (IS40) {
        UWORD ssw = f[6];
        int n, found = 0;
        ck("format_vector", 0x7008, f[3]);
        ck("fault_addr", addr, fl32(f, 10));
        /* SSW: CP CU CT CM (15-12) = 0, MA = 0, ATC (bit 10), LK = 0, RW (bit 8)
         * 1 = read, SIZE (6-5) 00 = long, TT (4-3) 00 = normal, TM (2-0) = FC */
        if (kind == K_TTW)
            ck("ssw_rw_size_tt_tm", fc, ssw & 0x017f);
        else
            ck("ssw", 0x0400 | (write ? 0 : 0x100) | fc, ssw & 0xff7f);
        if (write) {
            static const UBYTE sw[3] = { 9, 8, 7 }, aw[3] = { 20, 16, 12 }, dw[3] = { 22, 18, 14 };
            for (n = 0; n < 3; n++)
                if ((f[sw[n]] & 0x80) && fl32(f, aw[n]) == addr) {
                    found = 1;
                    ck("wb_data", data, fl32(f, dw[n]));
                    ck("wb_status_size_tt_tm", 0x80 | fc, f[sw[n]] & 0xff);
                }
            ck("wb_slot_with_fault_addr", 1, (ULONG)found);
        }
    } else {
        ULONG fslw = fl32(f, 6), want;
        ck("format_vector", 0x4008, f[3]);
        ck("fault_addr", addr, fl32(f, 4));
        ck("pc", pc, fl32(f, 1));
        /* FSLW: RW (24-23) 01 write / 10 read, SIZE (22-21) 00 long, TT 00,
         * TM (18-16) = FC, PF (9) / SP (8) / WP (7), TTR (3); others 0
         * (bit 2 BPE not checked) */
        want = (write ? 0x00800000UL : 0x01000000UL) | (fc << 16);
        want |= kind == K_INV ? 0x200 : kind == K_SUP ? 0x100 : kind == K_WP ? 0x80 : 0x08;
        /* a TTR write-protect fault must set TTR; whether WP is set as well
         * is not spelled out (MC68060UM 8.4.1 FSLW), so WP is not compared */
        ck("fslw", want, fslw & ~(kind == K_TTW ? 0x84UL : 4UL));
    }
}

/* ------------------------------------------------------------ cases */
static const char W_BASIC[] =
    "a test page mapped to another physical page must read and write that physical page "
    "(MC68030UM 9.1/9.5, MC68040UM 3.1, MC68060UM 4.1); everything else identity mapped";

static int no_mmu;
/* any U bit set on the path of window page 0 (root/level A, pointer/level B,
 * page descriptor)? */
static int walked(void)
{
    ULONG *e;
    if (IS30) {
        e = ent30(r30, ix30(win, 0));
        if (e[0] & D_U)
            return 1;
        if ((e[0] & 3) < 2)
            return 0;
        e = ent30(tad30(e), ix30(win, 1));
        if (e[0] & D_U)
            return 1;
        return (*slot30(win, 2) & D_U) != 0;
    }
    e = (ULONG *)r40 + (win >> 25);
    if (*e & D_U)
        return 1;
    e = (ULONG *)(*e & ~0x1ffUL) + ((win >> 18) & 127);
    if (*e & D_U)
        return 1;
    return (*slot40r(r40, win) & D_U) != 0;
}

static int pfx(const char *s, const char *p)
{
    while (*p)
        if (*s++ != *p++)
            return 0;
    return 1;
}
/* the probe ran its own table walk and saw U bits set (probe.c probe_mmu030) */
static int probe_saw_walk(void)
{
    int i;
    for (i = 0; i < amt_probe_result.n_how && i < AMT_PROBE_HOW_MAX; i++)
        if (amt_probe_result.how[i] && pfx(amt_probe_result.how[i], "mmu030:walk u="))
            return 1;
    return 0;
}
static int is_mmu_how(const char *h)
{
    return h && (pfx(h, "mmu030:") || pfx(h, "tc030:") || pfx(h, "tt030:") ||
                 pfx(h, "tc:") || pfx(h, "movec-tc") || pfx(h, "pmove-tc") || pfx(h, "crp:"));
}

static int in_basic;     /* t_basic running (EC030 F-line detection) */
static void t_basic(void)
{
    ULONG v, P0, P1;
    in_basic = 1;
    rs(W_BASIC);
    gb_begin(4096);
    P0 = dpage(4096, 0);
    P1 = dpage(4096, 1);
    gb_map(LW(0), P1, D_CI);
    gb_map(LW(1), P0, D_CI);
    gb_end();
    fill_data();
    nops = 0;
    op(O_R, LW(0) + 0x10, 0, 0);
    op(O_R, LW(1) + 0xff0, 0, 0);
    op(O_RB, LW(0) + 0x7ff, 0, 0);
    v = run_ops();
    last_vec = v;
    ci("l0", LW(0));
    ci("p0", P1);
    ci("l1", LW(1));
    ci("p1", P0);
    ck("vec", 0, v);
    ck("read_l0_10", mk(P1 + 0x10), ops[0].r);
    ck("read_l1_ff0", mk(P0 + 0xff0), ops[1].r);
    ck("read_byte_l0_7ff", mk(P1 + 0x7fc) & 0xff, ops[2].r);
    if (nfe && !walked() && (ops[0].r ^ MKX) - dlo >= dhi - dlo &&
        (ops[1].r ^ MKX) - dlo >= dhi - dlo) {
        /* TC.E=1 was accepted but the MMU never walked the tables. If the
         * probe itself saw a table walk (030 "mmu030:walk u=N"), this CPU
         * does have a paged MMU and it is broken: a fail of basic_translate.
         * Otherwise the probe's MMU decision rested on weaker evidence (TC
         * reads back, a configuration exception, a 68851 PMOVE) and an EC
         * part or an emulator without MMU translation cannot be told apart
         * from a broken MMU: untestable, with the probe's evidence. */
        if (!probe_saw_walk()) {
            int i;
            no_mmu = 1;
            amt_case_open("module", AMT_ST_UNTESTABLE);
            json_str("why", "TC.E=1 was accepted but no table walk happened (no U bit set in any "
                            "descriptor of the test page's path) and the reads did not reach any "
                            "test page. The probe saw no table walk of its own (its MMU decision "
                            "came from a register read-back), so this is either an EC part "
                            "without a paged MMU (Amiberry: no mmu_model, or mmu_model=68ec0x0) "
                            "or a completely broken MMU; the module cannot tell which");
            json_obj("ctx");
            json_str("probe_cpu", amt_probe_result.cpu);
            json_hex("got_read", ops[0].r);
            json_arr("probe_how");
            for (i = 0; i < amt_probe_result.n_how && i < AMT_PROBE_HOW_MAX; i++)
                if (is_mmu_how(amt_probe_result.how[i]))
                    json_str(NULL, amt_probe_result.how[i]);
            json_arr_end();
            json_obj_end();
            amt_case_close();
            in_basic = 0;
            return;
        }
        cx("probe_saw_walk", 1);
        ck("table_walk_seen", 1, 0);
    }
    if (!nfe) {
        nops = 0;
        op(O_W, LW(0) + 0x20, 0x13579bdfUL, 0);
        op(O_R, LW(0) + 0x20, 0, 0);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("readback", 0x13579bdfUL, ops[1].r);
        ck("phys_p0_20", 0x13579bdfUL, VL(P1 + 0x20));
        ck("phys_other_20", mk(P0 + 0x20), VL(P0 + 0x20));
    }
    if (nfe)
        broken = 1;
    in_basic = 0;
    rend("basic_translate");
}

static const char W_PS[] =
    "page size: two logical pages mapped crossed onto two physical pages; reads at the start, "
    "middle and end of each page and a write near the end must hit the right physical page "
    "(030 TC.PS 256 B-32 KB, MC68030UM 9.6.1; 040/060 TC.P 4/8 KB, MC68040UM 3.1.2)";

static void t_ps(const char *id, ULONG ps)
{
    ULONG P0, P1, v, e[6];
    int i, ro = ps >= 16384 || dcount(ps) < 2;
    if (pre(id))
        return;
    rs(W_PS);
    gb_begin(ps);
    if (ro) {
        P0 = rom;
        P1 = rom + ps;
    } else {
        P0 = dpage(ps, 0);
        P1 = dpage(ps, 1);
    }
    gb_map(LW(0), P1, D_CI);
    gb_map(LW(1), P0, D_CI);
    gb_end();
    fill_data();
    nops = 0;
    op(O_R, LW(0), 0, 0);
    op(O_R, LW(0) + ps / 2, 0, 0);
    op(O_R, LW(0) + ps - 4, 0, 0);
    op(O_R, LW(1), 0, 0);
    op(O_R, LW(1) + ps - 4, 0, 0);
    op(O_RB, LW(0) + ps - 1, 0, 0);
    e[0] = VL(P1);
    e[1] = VL(P1 + ps / 2);
    e[2] = VL(P1 + ps - 4);
    e[3] = VL(P0);
    e[4] = VL(P0 + ps - 4);
    e[5] = *(volatile UBYTE *)(P1 + ps - 1);
    if (!ro)
        op(O_W, LW(0) + ps - 8, 0x2468ace1UL, 0);
    v = run_ops();
    last_vec = v;
    ci("page_size", ps);
    ci("p_l0", P1);
    ci("p_l1", P0);
    ci("read_only_rom", (ULONG)ro);
    ck("vec", 0, v);
    for (i = 0; i < 6; i++) {
        static const char *const k[6] = { "l0_0", "l0_half", "l0_end", "l1_0", "l1_end", "l0_last_byte" };
        ck(k[i], e[i], ops[i].r);
    }
    if (!ro) {
        ck("phys_written", 0x2468ace1UL, VL(P1 + ps - 8));
        ck("phys_other", mk(P0 + ps - 8), VL(P0 + ps - 8));
    }
    rend(id);
}

/* --- faults --- */
static const char W_INVF[] =
    "read of a page with an invalid descriptor: access fault, vector 2. 030: format $A/$B, SSW "
    "DF=1 RW=1 FC=5 size long, fault address (MC68030UM 8.2.1, 9.5.3); 040: format $7, SSW ATC=1 "
    "RW=1 TM=5, FA (MC68040UM 8.2.1/8.4.5); 060: format $4, FSLW PF=1 RW=read TM=5, PC = the "
    "faulting instruction (MC68060UM 8.2.1, 8.4.1)";
static const char W_INVR[] =
    "after an invalid-page read fault the handler validates the descriptor, flushes the ATC and "
    "RTEs: the read is rerun (030 DF) or the instruction restarted (040/060) and completes once: "
    "MOVE.L (A0)+,D0 gives the data and A0+4, one fault (MC68030UM 8.2.1, MC68040UM 8.4.6, "
    "MC68060UM 8.4.1)";
static const char W_WPR[] =
    "a write-protected page can be read without a fault (MC68030UM 9.5.3.2 WP, MC68040UM 3.2.2 W)";
static const char W_WPF[] =
    "write to a write-protected page: access fault, vector 2, the write does not reach memory. "
    "030: format $A/$B, SSW DF=1 RW=0 FC=5, data output buffer = data; 040: format $7, SSW ATC=1 "
    "RW=0, the pending write in a writeback slot (address = FA, data); 060: format $4, FSLW WP=1 "
    "RW=write, PC = the faulting instruction (MC68030UM 8.2.1, MC68040UM 8.4.5, MC68060UM 8.4.1)";
static const char W_WPRS[] =
    "after a write-protect fault the handler clears WP, flushes the ATC (040: completes the "
    "pending writebacks) and RTEs: the write happens exactly once and execution continues: "
    "MOVE.L D1,(A0)+ leaves A0+4 (030 reruns the data cycle, 040 does not retry writes, 060 "
    "restarts) (MC68030UM 8.2.1, MC68040UM 8.4.6, MC68060UM 8.4.1)";
static const char W_RMW[] =
    "ADDQ.L #1,(A0) on a write-protected page: after the fix and RTE the memory is incremented "
    "exactly once (030 rerun of the write, 040 software writeback and no restart, 060 restart "
    "with the write not done) (MC68030UM 8.2.1, MC68040UM 8.4.6, MC68060UM 8.4.1)";
static const char W_SUPF[] =
    "user-mode read of a supervisor-only page (030 long-format S bit, 040/060 S bit): access "
    "fault, vector 2, frame from the supervisor stack, FC/TM = 1 (user data), 060 FSLW SP=1 "
    "(MC68030UM 9.5.3.2, MC68040UM 3.2.2, MC68060UM 4.2.2)";
static const char W_SUPR[] =
    "after the supervisor-only fault the handler clears S and RTEs: the user read completes and "
    "the user code continues to its TRAP #0 (MC68030UM 8.2.1, MC68040UM 8.4.6, MC68060UM 8.4.1)";
static const char W_SUPOK[] =
    "a supervisor access to a supervisor-only page is allowed (MC68030UM 9.5.3.2, MC68040UM 3.2.2)";

static ULONG P0g, P1g;            /* data pages of the fault tests */

static void fault_tables(ULONG fl0, int inv0, int l8)
{
    if (IS30) {
        gb_begin30(&c30_def, l8);
    } else
        gb_begin(4096);
    P0g = dpage(4096, 0);
    P1g = dpage(4096, 1);
    if (inv0)
        gb_inv(LW(0));
    else
        gb_map(LW(0), P0g, fl0);
    gb_map(LW(1), P1g, D_CI);
    gb_end();
    fill_data();
}

static ULONG run_sn(void *fn, ULONG a0, ULONG d1)
{
    in_clear();
    mm_in[8] = a0;
    mm_in[1] = d1;
    mm_usp = pbase + U_TOP;
    return RUN((void (*)(void))fn);
}

static void t_faults(void)
{
    ULONG v, a;
    /* invalid read: frame */
    if (!pre("invalid_read_fault")) {
        rs(W_INVF);
        fault_tables(0, 1, 0);
        hook_set(0);
        a = LW(0) + 0x40;
        v = run_sn(sn_rd, a, 0);
        last_vec = v;
        ci("addr", a);
        if (ck("vec", 2, v) && ck("faults", 1, mm_nlog))
            chk_fault(0, a, 0, 0, K_INV, 0, A(sn_rd_at));
        rend("invalid_read_fault");
    }
    if (!pre("invalid_read_resume")) {
        rs(W_INVR);
        fault_tables(0, 1, 0);
        hook_set(1);
        a = LW(0) + 0x40;
        fix_add(LW(0), 4096, gb_slot(LW(0)), gb_pdval(P0g, D_CI));
        if (IS30 && L8) { /* not used */ }
        v = run_sn(sn_rdpi, a, 0);
        last_vec = v;
        ci("addr", a);
        ck("vec", 0, v);
        ck("faults", 1, mm_nlog);
        ck("d0", mk(P0g + 0x40), mm_out[0]);
        ck("a0_off", 4, mm_out[8] - a);
        rend("invalid_read_resume");
    }
    if (!pre("wp_read_ok")) {
        rs(W_WPR);
        fault_tables(D_CI | D_WP, 0, 0);
        hook_set(0);
        nops = 0;
        op(O_R, LW(0) + 0x60, 0, 0);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("read", mk(P0g + 0x60), ops[0].r);
        rend("wp_read_ok");
    }
    if (!pre("wp_write_fault")) {
        rs(W_WPF);
        fault_tables(D_CI | D_WP, 0, 0);
        hook_set(0);
        a = LW(0) + 0x80;
        v = run_sn(sn_wr, a, 0xcafef00dUL);
        last_vec = v;
        ci("addr", a);
        ci("data", 0xcafef00dUL);
        if (ck("vec", 2, v) && ck("faults", 1, mm_nlog))
            chk_fault(0, a, 1, 0, K_WP, 0xcafef00dUL, A(sn_wr_at));
        ck("memory_unchanged", mk(P0g + 0x80), VL(P0g + 0x80));
        rend("wp_write_fault");
    }
    if (!pre("wp_write_resume")) {
        rs(W_WPRS);
        fault_tables(D_CI | D_WP, 0, 0);
        hook_set(1);
        a = LW(0) + 0x84;
        fix_add(LW(0), 4096, gb_slot(LW(0)), gb_pdval(P0g, D_CI));
        v = run_sn(sn_wrpi, a, 0x0badc0deUL);
        last_vec = v;
        ci("addr", a);
        ck("vec", 0, v);
        ck("faults", 1, mm_nlog);
        ck("memory", 0x0badc0deUL, VL(P0g + 0x84));
        ck("a0_off", 4, mm_out[8] - a);
        if (IS40)
            cx("undef_writebacks_done", (ULONG)wb_done);
        rend("wp_write_resume");
    }
    if (!pre("wp_rmw_resume")) {
        rs(W_RMW);
        fault_tables(D_CI | D_WP, 0, 0);
        hook_set(1);
        a = LW(0) + 0x90;
        fix_add(LW(0), 4096, gb_slot(LW(0)), gb_pdval(P0g, D_CI));
        v = run_sn(sn_addq, a, 0);
        last_vec = v;
        ci("addr", a);
        ck("vec", 0, v);
        ck("faults", 1, mm_nlog);
        ck("memory", mk(P0g + 0x90) + 1, VL(P0g + 0x90));
        if (IS40)
            cx("undef_writebacks_done", (ULONG)wb_done);
        rend("wp_rmw_resume");
    }
    /* supervisor-only pages (030: long-format tables) */
    if (!pre("super_user_fault")) {
        rs(W_SUPF);
        fault_tables(D_CI | D_S(), 0, 1);
        hook_set(0);
        a = LW(0) + 0xc0;
        v = run_sn(sn_urd, a, 0);
        last_vec = v;
        ci("addr", a);
        if (ck("vec", 2, v) && ck("faults", 1, mm_nlog))
            chk_fault(0, a, 0, 1, K_SUP, 0, A(sn_urd_at));
        if (mm_nlog)
            ck("stacked_sr_s", 0, mm_log[0].f[0] & 0x2000);
        rend("super_user_fault");
    }
    if (!pre("super_user_resume")) {
        rs(W_SUPR);
        fault_tables(D_CI | D_S(), 0, 1);
        hook_set(1);
        a = LW(0) + 0xc4;
        fix_add(LW(0), 4096, gb_slot(LW(0)), gb_pdval(P0g, D_CI));
        v = run_sn(sn_urd, a, 0);
        last_vec = v;
        ci("addr", a);
        ck("vec_trap0", 32, v);
        ck("events", 2, mm_nlog);
        if (mm_nlog >= 1)
            ck("first_vec", 2, mm_log[0].vec);
        ck("d0", mk(P0g + 0xc4), mm_out[0]);
        rend("super_user_resume");
    }
    if (!pre("super_access_ok")) {
        rs(W_SUPOK);
        fault_tables(D_CI | D_S(), 0, 1);
        hook_set(0);
        nops = 0;
        op(O_R, LW(0) + 0xc8, 0, 0);
        op(O_W, LW(0) + 0xcc, 0x600dd00dUL, 0);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("read", mk(P0g + 0xc8), ops[0].r);
        ck("memory", 0x600dd00dUL, VL(P0g + 0xcc));
        rend("super_access_ok");
    }
    hook_set(0);
}

/* --- misaligned access across a page boundary, instruction fetch --- */
static const char W_MIS[] =
    "a misaligned longword access whose second half lies on an invalid page: access fault "
    "(040 SSW MA=1, 060 FSLW MA=1: fault on a later part of a misaligned access); after the handler validates the page and RTEs the access completes exactly once with "
    "the right bytes on both pages (030 reruns the faulted cycle, 040 writebacks/restart, 060 "
    "restart) (MC68030UM 8.2.1, MC68040UM 8.4.6, MC68060UM 8.4.1)";
static const char W_IFF[] =
    "instruction fetch from an invalid page (JSR into it): access fault, vector 2; 030 SSW "
    "DF=0 with FB or FC set (MC68030UM 8.2.1); 040 SSW ATC=1, TM=6 (supervisor code), FA = "
    "the fetch address (MC68040UM 8.4.5); 060 FSLW IO=1 PF=1 RW=read TM=6, FA and PC = the "
    "fetch address (MC68060UM 8.4.1); after the fix and RTE the code runs";

static void t_misaligned(void)
{
    int w;
    ULONG v, a, P0 = dpage(4096, 0), P1 = dpage(4096, 1);
    for (w = 0; w < 2; w++) {
        const char *id = w ? "misaligned_write_resume" : "misaligned_read_resume";
        if (pre(id))
            continue;
        rs(W_MIS);
        gb_begin(4096);
        gb_map(LW(0), P0, D_CI);
        gb_inv(LW(1));
        gb_end();
        fill_data();
        hook_set(1);
        fix_add(LW(1), 4096, gb_slot(LW(1)), gb_pdval(P1, D_CI));
        a = LW(1) - 2;
        v = run_sn(w ? (void *)sn_wr : (void *)sn_rd, a, 0x89abcdefUL);
        last_vec = v;
        ci("addr", a);
        ck("vec", 0, v);
        ck("faults", 1, mm_nlog);
        if (mm_nlog) {
            volatile UWORD *f = mm_log[0].f;
            cx("undef_fault_addr_off", fa_of(0) - a);
            /* the fault is on the second part of the misaligned access */
            if (IS40)
                ck("ssw_ma", 0x0800, f[6] & 0x0800);
            else if (IS60)
                ck("fslw_ma", 0x08000000UL, fl32(f, 6) & 0x08000000UL);
        }
        if (w) {
            ck("page0_end", (mk(P0 + 0xffc) & 0xffff0000UL) | 0x89ab, VL(P0 + 0xffc));
            ck("page1_start", 0xcdef0000UL | (mk(P1) & 0xffff), VL(P1));
        } else
            ck("d0", (mk(P0 + 0xffc) << 16) | (mk(P1) >> 16), mm_out[0]);
        if (IS40)
            cx("undef_writebacks_done", (ULONG)wb_done);
        rend(id);
    }
    hook_set(0);
}

static void t_ifetch(void)
{
    ULONG v, a, P0 = dpage(4096, 0);
    if (pre("ifetch_fault_resume"))
        return;
    rs(W_IFF);
    gb_begin(4096);
    gb_inv(LW(0));
    gb_end();
    fill_data();
    VL(P0 + 0x600) = 0x70014e75UL;          /* moveq #1,d0; rts */
    hook_set(1);
    fix_add_ifetch(LW(0), 4096, gb_slot(LW(0)), gb_pdval(P0, D_CI));
    a = LW(0) + 0x600;
    nops = 0;
    op(O_CALL, a, 0, 0);
    v = run_ops();
    last_vec = v;
    ci("target", a);
    ck("vec", 0, v);
    ck("faults", 1, mm_nlog);
    ck("d0", 1, ops[0].r & 0xff);
    if (mm_nlog && mm_log[0].vec == 2) {
        volatile UWORD *f = mm_log[0].f;
        if (IS30) {
            ck2("format", 0xa, 0xb, f[3] >> 12);
            ck("ssw_df", 0, f[5] & 0x100);
            ck("ssw_fb_or_fc", 1, (f[5] & 0xc000) != 0);
            /* the long frame's stage B address (stage C = B-2) names the
             * faulted prefetch, which lies in the invalid page */
            if ((f[3] >> 12) == 0xb && (f[5] & 0xc000))
                ck("ifetch_addr_in_page", 1, fa_of(0) - LW(0) < 4096);
        } else if (IS40) {
            ck("format_vector", 0x7008, f[3]);
            ck("fault_addr", a, fl32(f, 10));
            ck("ssw_atc_rw_tm", 0x0506, f[6] & 0x0507);
        } else {
            ck("format_vector", 0x4008, f[3]);
            ck("fault_addr", a, fl32(f, 4));
            ck("pc", a, fl32(f, 1));
            ck("fslw", 0x01068200UL, fl32(f, 6) & 0x09878380UL);
        }
    }
    hook_set(0);
    rend("ifetch_fault_resume");
}

/* --- history bits --- */
static const char W_UREAD[] =
    "a read that walks the table sets U in the page descriptor and leaves M clear "
    "(MC68030UM 9.5.3.2, MC68040UM 3.2.2/3.2.3, MC68060UM 4.2.2)";
static const char W_MWR[] =
    "a write that walks the table sets U and M in the page descriptor (MC68030UM 9.5.3.2, "
    "MC68040UM 3.2.3, MC68060UM 4.2.3)";
static const char W_MAR[] =
    "a write to a page first loaded into the ATC by a read (entry M=0) must set M in the page "
    "descriptor in memory (MC68030UM 9.4.1/9.5.3.2, MC68040UM 3.2.3, MC68060UM 4.2.3)";
static const char W_UTAB[] =
    "the table walk sets U in the table (root/pointer) descriptors it used "
    "(MC68030UM 9.5.3.1, MC68040UM 3.2.2, MC68060UM 4.2.2)";

static void t_history(void)
{
    ULONG v, *s0, *s1, *s2, *ta = 0, *tb = 0;
    if (pre("used_bit_read")) {
        pre("modified_bit_write");
        pre("modified_after_read");
        pre("used_bit_tables");
        return;
    }
    gb_begin(4096);
    gb_map(LW(0), dpage(4096, 0), D_CI);
    gb_map(LW(1), dpage(4096, 1), D_CI);
    gb_map(LW(2), dpage(4096, 2), D_CI);
    s0 = gb_slot(LW(0));
    s1 = gb_slot(LW(1));
    s2 = gb_slot(LW(2));
    gb_end();
    if (IS30) {
        ta = ent30(r30, ix30(win, 0));
        tb = ent30(tad30(ta), ix30(win, 1));
    } else {
        ta = (ULONG *)r40 + (win >> 25);
        tb = (ULONG *)(*ta & ~0x1ffUL) + ((win >> 18) & 127);
    }
    fill_data();
    nops = 0;
    op(O_R, LW(0) + 4, 0, 0);
    op(O_W, LW(1) + 4, 1, 0);
    op(O_R, LW(2) + 4, 0, 0);
    op(O_W, LW(2) + 8, 2, 0);
    v = run_ops();

    rs(W_UREAD);
    last_vec = v;
    ck("vec", 0, v);
    ci("desc_before", gb_pdval(dpage(4096, 0), D_CI));
    ck("desc_u", D_U, *s0 & D_U);
    ck("desc_m", 0, *s0 & D_M);
    rend("used_bit_read");
    rs(W_MWR);
    last_vec = v;
    ck("vec", 0, v);
    ck("desc_um", D_U | D_M, *s1 & (D_U | D_M));
    rend("modified_bit_write");
    rs(W_MAR);
    last_vec = v;
    ck("vec", 0, v);
    ck("desc_um", D_U | D_M, *s2 & (D_U | D_M));
    rend("modified_after_read");
    rs(W_UTAB);
    last_vec = v;
    ck("vec", 0, v);
    ck("level_a_or_root_u", D_U, *ta & D_U);
    ck("level_b_or_pointer_u", D_U, *tb & D_U);
    rend("used_bit_tables");
}

/* --- ATC --- */
static const char W_STALE[] =
    "a descriptor changed without a flush: the old translation may still be in the ATC (either "
    "page is allowed); after PFLUSHA the new one must apply (MC68030UM 9.4/9.8 PFLUSHA, "
    "MC68040UM 3.3/3.4, MC68060UM 4.3)";
static const char W_PFP[] =
    "PFLUSH of one page (030 PFLUSH #5,#7,(A0); 040/060 PFLUSH (A0) with DFC=5): that page must "
    "use the new descriptor; another changed page may still use the old one "
    "(MC68030UM PFLUSH, MC68040UM 3.4 / M68000PRM PFLUSH)";
static const char W_GLOB[] =
    "PFLUSHAN / PFLUSHN do not flush global (G=1) entries, PFLUSHA flushes all: after PFLUSHAN "
    "the non-global page must use its new descriptor, the global one may keep the old one; "
    "after PFLUSHA both use the new one (MC68040UM 3.4, MC68060UM 4.3, M68000PRM PFLUSH)";

static void t_atc(void)
{
    ULONG v, P[4], *s0, *s1;
    int i;
    for (i = 0; i < 4; i++)
        P[i] = dpage(4096, i);
    if (!pre("atc_stale_flusha")) {
        rs(W_STALE);
        gb_begin(4096);
        gb_map(LW(0), P[0], D_CI);
        s0 = gb_slot(LW(0));
        gb_end();
        fill_data();
        nops = 0;
        op(O_R, LW(0) + 0x100, 0, 0);
        op(O_W, A(s0), gb_pdval(P[1], D_CI), 0);
        op(O_R, LW(0) + 0x100, 0, 0);
        op(O_FL, 0, 0, 5);
        op(O_R, LW(0) + 0x100, 0, 0);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("before", mk(P[0] + 0x100), ops[0].r);
        ck2("stale_or_new", mk(P[0] + 0x100), mk(P[1] + 0x100), ops[2].r);
        ck("after_pflusha", mk(P[1] + 0x100), ops[4].r);
        cx("undef_stale_kept", ops[2].r == mk(P[0] + 0x100));
        rend("atc_stale_flusha");
    }
    if (!pre("atc_pflush_page")) {
        rs(W_PFP);
        gb_begin(4096);
        gb_map(LW(0), P[0], D_CI);
        gb_map(LW(1), P[1], D_CI);
        s0 = gb_slot(LW(0));
        s1 = gb_slot(LW(1));
        gb_end();
        fill_data();
        nops = 0;
        op(O_R, LW(0) + 0x104, 0, 0);
        op(O_R, LW(1) + 0x104, 0, 0);
        op(O_W, A(s0), gb_pdval(P[2], D_CI), 0);
        op(O_W, A(s1), gb_pdval(P[3], D_CI), 0);
        op(O_FL, LW(0), IS30 ? 3 : 2, 5);
        op(O_R, LW(0) + 0x104, 0, 0);
        op(O_R, LW(1) + 0x104, 0, 0);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("flushed_page_new", mk(P[2] + 0x104), ops[5].r);
        ck2("other_page_old_or_new", mk(P[1] + 0x104), mk(P[3] + 0x104), ops[6].r);
        rend("atc_pflush_page");
    }
    if (!IS30 && !pre("pflushan_global")) {
        rs(W_GLOB);
        gb_begin(4096);
        gb_map(LW(0), P[0], D_CI | D_G);
        gb_map(LW(1), P[1], D_CI);
        s0 = gb_slot(LW(0));
        s1 = gb_slot(LW(1));
        gb_end();
        fill_data();
        nops = 0;
        op(O_R, LW(0) + 0x108, 0, 0);
        op(O_R, LW(1) + 0x108, 0, 0);
        op(O_W, A(s0), gb_pdval(P[2], D_CI | D_G), 0);
        op(O_W, A(s1), gb_pdval(P[3], D_CI), 0);
        op(O_FL, 0, 1, 5);                  /* PFLUSHAN */
        op(O_R, LW(0) + 0x108, 0, 0);
        op(O_R, LW(1) + 0x108, 0, 0);
        op(O_FL, 0, 0, 5);                  /* PFLUSHA */
        op(O_R, LW(0) + 0x108, 0, 0);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck2("global_after_pflushan", mk(P[0] + 0x108), mk(P[2] + 0x108), ops[5].r);
        ck("nonglobal_after_pflushan", mk(P[3] + 0x108), ops[6].r);
        ck("global_after_pflusha", mk(P[2] + 0x108), ops[8].r);
        cx("undef_global_kept_by_pflushan", ops[5].r == mk(P[0] + 0x108));
        rend("pflushan_global");
    }
    if (!IS30 && !pre("pflushn_page")) {
        rs(W_GLOB);
        gb_begin(4096);
        gb_map(LW(0), P[0], D_CI);
        s0 = gb_slot(LW(0));
        gb_end();
        fill_data();
        nops = 0;
        op(O_R, LW(0) + 0x10c, 0, 0);
        op(O_W, A(s0), gb_pdval(P[1], D_CI), 0);
        op(O_FL, LW(0), 3, 5);              /* PFLUSHN (A0) */
        op(O_R, LW(0) + 0x10c, 0, 0);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("nonglobal_after_pflushn", mk(P[1] + 0x10c), ops[3].r);
        rend("pflushn_page");
    }
}

static const char W_THR[] =
    "more pages than the ATC holds (030: 22 entries; 040/060: 64 data entries), each mapped to "
    "one of a few physical pages: every read and write through every page must use that page's "
    "translation while entries are replaced (MC68030UM 9.4, MC68040UM 3.3, MC68060UM 4.3)";

static void t_thrash(void)
{
    ULONG v, i, n, k, round, rounds, seed, bad = 0;
    ULONG P[16];
    static ULONG pi_of[200];
    if (pre("atc_thrash"))
        return;
    rs(W_THR);
    n = IS30 ? 48 : 160;
    k = 8;
    gb_begin(4096);
    for (i = 0; i < k; i++)
        P[i] = dpage(4096, (int)i);
    for (i = 0; i < n; i++) {
        pi_of[i] = (i * 3 + 1) % k;
        gb_map(LW((int)i), P[pi_of[i]], D_CI);
    }
    gb_end();
    fill_data();
    rounds = AMT_QUICK() ? 1 : 3;
    ci("pages", n);
    ci("phys_pages", k);
    for (round = 0; round < rounds && !bad; round++) {
        /* one run per round, so the ATC is never flushed in between:
         * sequential reads, writes, then scattered reads of every page */
        nops = 0;
        for (i = 0; i < n; i++) {
            ULONG off = ((i * 37 + 5 + round * 11) & 511) * 4;
            op(O_R, LW((int)i) + off, mk(P[pi_of[i]] + off), 0);
        }
        for (i = 0; i < n; i++)
            op(O_W, LW((int)i) + 3072 + (i & 255) * 4, 0x3c000000UL | (round << 16) | i, 0);
        seed = 0x1234567UL + round;
        for (i = 0; i < n; i++) {
            ULONG q;
            seed = seed * 1103515245UL + 12345;
            q = (seed >> 8) % n;
            op(O_R, LW((int)q) + 2048 + (q & 63) * 4, mk(P[pi_of[q]] + 2048 + (q & 63) * 4), 0);
        }
        v = run_ops();
        if (v) {
            last_vec = v;
            ck("vec", 0, v);
            bad = 1;
            break;
        }
        for (i = 0; i < (ULONG)nops; i++)
            if (ops[i].k == O_R && ops[i].r != ops[i].v) {
                ci("op_index", i);
                ci("op_addr", ops[i].a);
                ci("round", round);
                ck("read", ops[i].v, ops[i].r);
                cx("got_phys", ops[i].r ^ MKX);
                bad = 1;
                break;
            }
        if (!bad)
            for (i = 0; i < n; i++) {
                ULONG pa = P[pi_of[i]] + 3072 + (i & 255) * 4;
                if (VL(pa) != (0x3c000000UL | (round << 16) | i)) {
                    ci("logical_page", i);
                    ci("round", round);
                    ck("phys_written", 0x3c000000UL | (round << 16) | i, VL(pa));
                    bad = 1;
                    break;
                }
            }
    }
    rend("atc_thrash");
}

/* --- nested fault --- */
static const char W_NEST[] =
    "a fault inside the access-fault handler (nested): the handler reads another invalid page, "
    "the nested fault is taken and fixed, the handler's read completes, then the outer fault is "
    "fixed and the original read completes; both frames are on the supervisor stack with S=1 "
    "(MC68030UM 8.2.1, MC68040UM 8.4.6, MC68060UM 8.4.1)";

static void t_nested(void)
{
    ULONG v, a, b;
    if (pre("nested_fault"))
        return;
    rs(W_NEST);
    if (IS30)
        gb_begin30(&c30_def, 0);
    else
        gb_begin(4096);
    gb_inv(LW(0));
    gb_inv(LW(1));
    gb_end();
    fill_data();
    hook_set(1);
    a = LW(0) + 0x208;
    b = LW(1) + 0x20c;
    fix_add(LW(0), 4096, gb_slot(LW(0)), gb_pdval(dpage(4096, 0), D_CI));
    fix_add(LW(1), 4096, gb_slot(LW(1)), gb_pdval(dpage(4096, 1), D_CI));
    nest_addr = b;
    v = run_sn(sn_rd, a, 0);
    last_vec = v;
    ci("outer_addr", a);
    ci("inner_addr", b);
    ck("vec", 0, v);
    ck("faults", 2, mm_nlog);
    if (mm_nlog >= 2) {
        ck("outer_fault_addr", a, fa_of(0));
        ck("inner_fault_addr", b, fa_of(1));
        ck("inner_stacked_sr_s", 0x2000, mm_log[1].f[0] & 0x2000);
        ck("inner_handler_sr", 0x2700, mm_log[1].hsr & 0xf700);
    }
    ck("inner_value", mk(dpage(4096, 1) + 0x20c), nest_val);
    ck("outer_value", mk(dpage(4096, 0) + 0x208), mm_out[0]);
    hook_set(0);
    rend("nested_fault");
}

/* --- indirect descriptors --- */
static const char W_IND[] =
    "an indirect descriptor at the page level (030 DT=2 at the last level, 040/060 PDT=10) "
    "points to the page descriptor: the page translates through it, the walk sets U in the "
    "target page descriptor and leaves the indirect descriptor unchanged (MC68030UM 9.5.1.4, "
    "MC68040UM 3.2.2, MC68060UM 4.2.2)";

static void t_indirect(void)
{
    ULONG v, *s, pd, ival;
    if (pre("indirect"))
        return;
    rs(W_IND);
    gb_begin(4096);
    s = gb_slot(LW(0));
    pd = talloc(16, 16);
    VL(pd) = gb_pdval(dpage(4096, 1), D_CI);
    ival = pd | 2;
    s[0] = ival;
    gb_end();
    fill_data();
    nops = 0;
    op(O_R, LW(0) + 0x300, 0, 0);
    v = run_ops();
    last_vec = v;
    ci("indirect_desc", ival);
    ci("page_desc", gb_pdval(dpage(4096, 1), D_CI));
    ck("vec", 0, v);
    ck("read", mk(dpage(4096, 1) + 0x300), ops[0].r);
    ck("target_u", D_U, VL(pd) & D_U);
    ck("indirect_unchanged", ival, s[0]);
    rend("indirect");
}

/* --- root pointers --- */
static const char W_SRE[] =
    "030 TC.SRE=1: supervisor accesses use SRP, user accesses (MOVES with SFC=1) use CRP "
    "(MC68030UM 9.6.1, 9.5.2)";
static const char W_FCL[] =
    "030 TC.FCL=1: the root table is indexed by the function code first; FC 1 and FC 5 go "
    "through different level-A tables (MC68030UM 9.5.2.1, 9.6.1)";
static const char W_URP[] =
    "040/060: user accesses (MOVES with SFC=1) use URP, supervisor accesses SRP "
    "(MC68040UM 3.1.1, MC68060UM 4.1.1)";

static ULONG copy_tab(ULONG t, ULONG bytes, ULONG align)
{
    ULONG n = talloc(bytes, align), i;
    for (i = 0; i < bytes; i += 4)
        VL(n + i) = VL(t + i);
    return n;
}

static void t_roots(void)
{
    ULONG v, ru, rsup, P0 = dpage(4096, 0), P1 = dpage(4096, 1);
    if (IS30) {
        int which;
        for (which = 0; which < 2; which++) {
            const char *id = which ? "fcl" : "sre";
            if (pre(id))
                continue;
            rs(which ? W_FCL : W_SRE);
            gb_begin(4096);
            gb_map(LW(0), P0, D_CI);
            fill30(r30, 0, 0);
            ru = r30;
            /* second tree: copy of level A with the window entries fresh */
            rsup = copy_tab(ru, 256 * 4, 16);
            ent30(rsup, ix30(win, 0))[0] = 0;
            ent30(rsup, ix30(win + 0x01000000UL, 0))[0] = 0;
            r30 = rsup;
            gb_map(LW(0), P1, D_CI);
            fill30(r30, 0, 0);
            mm_cfg.tc = tc30();
            if (which == 0) {
                mm_cfg.tc |= 0x02000000UL;          /* SRE */
                rp30(mm_cfg.crp, ru);
                rp30(mm_cfg.srp, rsup);
            } else {
                ULONG ft = talloc(32, 16), f;
                for (f = 0; f < 8; f++)
                    VL(ft + f * 4) = (f == 1 ? ru : rsup) | 2;
                mm_cfg.tc |= 0x01000000UL;          /* FCL */
                mm_cfg.crp[0] = 0x7fff0002UL;
                mm_cfg.crp[1] = ft;
                mm_cfg.srp[0] = 0x7fff0002UL;
                mm_cfg.srp[1] = ft;
            }
            fill_data();
            nops = 0;
            op(O_R, LW(0) + 0x400, 0, 0);
            op(O_MR, LW(0) + 0x404, 0, 1);
            op(O_MR, LW(0) + 0x408, 0, 5);
            v = run_ops();
            last_vec = v;
            ck("vec", 0, v);
            ck("supervisor_read", mk(P1 + 0x400), ops[0].r);
            ck("user_moves_read", mk(P0 + 0x404), ops[1].r);
            ck("supervisor_moves_read", mk(P1 + 0x408), ops[2].r);
            rend(id);
        }
        return;
    }
    if (pre("urp_srp"))
        return;
    rs(W_URP);
    gb_begin(4096);
    gb_map(LW(0), P0, D_CI);
    id40(r40);
    ru = r40;
    rsup = copy_tab(ru, 512, 512);
    VL(rsup + (win >> 25) * 4) = 0;
    r40 = rsup;
    gb_map(LW(0), P1, D_CI);
    id40(r40);
    mm_cfg.tc = 0x8000UL;
    mm_cfg.urp = ru;
    mm_cfg.srp[0] = rsup;
    fill_data();
    nops = 0;
    op(O_R, LW(0) + 0x400, 0, 0);
    op(O_MR, LW(0) + 0x404, 0, 1);
    op(O_MR, LW(0) + 0x408, 0, 5);
    v = run_ops();
    last_vec = v;
    ck("vec", 0, v);
    ck("supervisor_read", mk(P1 + 0x400), ops[0].r);
    ck("user_moves_read", mk(P0 + 0x404), ops[1].r);
    ck("supervisor_moves_read", mk(P1 + 0x408), ops[2].r);
    rend("urp_srp");
}

/* --- transparent translation --- */
/* L = data page 0 used as a logical address (inside the pool), mapped by
 * the tables onto data page 1: a TT match gives page 0 (identity), the
 * tables give page 1. */
static ULONG TL, TP;
static void tt_tables(void)
{
    gb_begin(4096);
    TL = dpage(4096, 0);
    TP = dpage(4096, 1);
    gb_map(TL, TP, D_CI);
    gb_end();
    fill_data();
}
static ULONG tt30(ULONG base, ULONG mask, int rw, int rwm, ULONG fcb, ULONG fcm, int en)
{
    return (base & 0xff000000UL) | ((mask & 0xff) << 16) | (en ? 0x8000UL : 0) | 0x400 |
           (rw ? 0x200UL : 0) | (rwm ? 0x100UL : 0) | ((fcb & 7) << 4) | (fcm & 7);
}
static ULONG tt40(ULONG base, ULONG mask, int s, int w)
{
    return (base & 0xff000000UL) | ((mask & 0xff) << 16) | 0x8000UL | ((ULONG)s << 13) | CM_CI |
           (w ? 4UL : 0);
}

static const char W_TT30[] =
    "030 TT0/TT1: an access whose address bits 31-24 (with the mask) and FC (with the FC mask) "
    "match an enabled TTn is not translated (logical = physical); RWM=1 matches reads and "
    "writes, RWM=0 only the R/W given by RW; others go through the tables (MC68030UM 9.7)";
static const char W_TTRMW[] =
    "030 TTn and read-modify-write cycles (TAS): with RWM=0 neither the read nor the write of "
    "the locked cycle is transparent (TAS goes through the tables), with RWM=1 the whole TAS is "
    "transparent (MC68030UM 9.7.x RWM)";
static const char W_TT40[] =
    "040/060 DTTn: a data access whose address bits 31-24 (with the mask) match and whose "
    "privilege matches the S field (00 user, 01 supervisor, 1x both) is not translated; W=1 "
    "write-protects it (access fault); others go through the tables (MC68040UM 3.1.3, "
    "MC68060UM 4.1.3)";
static const char W_ITT[] =
    "040/060 ITTn match instruction fetches only and DTTn data accesses only: a JSR into a page "
    "that the tables map elsewhere runs the identity code when an ITT matches, the table code "
    "when only a DTT matches (MC68040UM 3.1.3, MC68060UM 4.1.3); 030 TTn with FC base 6 "
    "(supervisor program) matches the fetch (MC68030UM 9.7)";

/* expected result helpers: page 0 marker (TT) or page 1 marker (tables) */
#define VT(o) mk(TL + (o))
#define VP(o) mk(TP + (o))

static void tt_case(const char *id, const char *w, ULONG tt_0, ULONG tt_1,
                    int r1, int r5, int wr1)  /* expected: 1 = TT (identity), 0 = tables; wr1 -1 = no write */
{
    ULONG v;
    if (pre(id))
        return;
    rs(w);
    tt_tables();
    mm_cfg.tt0 = tt_0;
    mm_cfg.tt1 = tt_1;
    nops = 0;
    op(O_MR, TL + 0x500, 0, 1);
    op(O_MR, TL + 0x504, 0, 5);
    if (wr1 >= 0)
        op(O_MW, TL + 0x508, 0x77665544UL, 1);
    v = run_ops();
    last_vec = v;
    ci("tt0", tt_0);
    ci("tt1", tt_1);
    ci("logical", TL);
    ci("table_phys", TP);
    ck("vec", 0, v);
    ck("fc1_read", r1 ? VT(0x500) : VP(0x500), ops[0].r);
    ck("fc5_read", r5 ? VT(0x504) : VP(0x504), ops[1].r);
    if (wr1 >= 0) {
        ck("fc1_write_identity_page", wr1 ? 0x77665544UL : VT(0x508), VL(TL + 0x508));
        ck("fc1_write_table_page", wr1 ? VP(0x508) : 0x77665544UL, VL(TP + 0x508));
    }
    rend(id);
}

static const UWORD code_tab[3] = { 0x7001, 0x4e75, 0 };   /* moveq #1,d0; rts */
static const UWORD code_id[3] = { 0x7002, 0x4e75, 0 };    /* moveq #2,d0; rts */

static void itt_case(const char *id, ULONG itt0, ULONG itt1, ULONG dtt0, ULONG tt30v, ULONG expect)
{
    ULONG v;
    if (pre(id))
        return;
    rs(W_ITT);
    tt_tables();
    VL(TL + 0x600) = ((ULONG)code_id[0] << 16) | code_id[1];
    VL(TP + 0x600) = ((ULONG)code_tab[0] << 16) | code_tab[1];
    if (IS30)
        mm_cfg.tt1 = tt30v;
    else {
        mm_cfg.itt0 = itt0;
        mm_cfg.itt1 = itt1;
        mm_cfg.tt0 = dtt0;
    }
    nops = 0;
    op(O_CALL, TL + 0x600, 0, 0);
    v = run_ops();
    last_vec = v;
    ci("itt0_or_tt1", IS30 ? tt30v : itt0);
    ci("itt1", itt1);
    ci("dtt0", dtt0);
    ck("vec", 0, v);
    ck("d0", expect, ops[0].r & 0xff);
    rend(id);
}

static void t_tt(void)
{
    ULONG blk = dpage(4096, 0) & 0xff000000UL, v;
    if (IS30) {
        tt_case("tt0_fc", W_TT30, tt30(blk, 0, 0, 1, 1, 0, 1), 0, 1, 0, 1);
        tt_case("tt1_fc", W_TT30, 0, tt30(blk, 0, 0, 1, 1, 0, 1), 1, 0, 1);
        tt_case("tt_fc_mask", W_TT30, tt30(blk, 0, 0, 1, 1, 4, 1), 0, 1, 1, -1);
        tt_case("tt_rw_read", W_TT30, tt30(blk, 0, 1, 0, 1, 0, 1), 0, 1, 0, 0);
        tt_case("tt_rw_write", W_TT30, tt30(blk, 0, 0, 0, 1, 0, 1), 0, 0, 0, 1);
        tt_case("tt_addr_mask", W_TT30, tt30(blk ^ 0x01000000UL, 1, 0, 1, 1, 0, 1), 0, 1, 0, -1);
        tt_case("tt_addr_nomatch", W_TT30, tt30(blk ^ 0x01000000UL, 0, 0, 1, 1, 0, 1), 0, 0, 0, -1);
        tt_case("tt_disabled", W_TT30, tt30(blk, 0, 0, 1, 1, 0, 0), 0, 0, 0, -1);
        if (!pre("tt_rmw")) {
            rs(W_TTRMW);
            if (amt_any_pool_on_chip_bus())
                skip("tt_rmw", "the pool is on the chip bus (chip or $C00000-$DBFFFF slow RAM), which does not support the indivisible "
                     "read-modify-write cycle of TAS (HRM)");
            else {
                int rwm;
                for (rwm = 0; rwm < 2 && !nfe; rwm++) {
                    tt_tables();
                    mm_cfg.tt0 = tt30(blk, 0, 1, rwm, 5, 0, 1);
                    hook_set(0);
                    ci("logical_and_identity_phys", TL);
                    ci("table_phys", TP);
                    ci("identity_page_540_before", 0x11111111UL);
                    ci("table_page_540_before", 0x22222222UL);
                    VL(TL + 0x540) = 0x11111111UL;
                    VL(TP + 0x540) = 0x22222222UL;
                    v = run_sn(sn_tas, TL + 0x540, 0);
                    last_vec = v;
                    ci(rwm ? "tt0_rwm1" : "tt0_rwm0", mm_cfg.tt0);
                    ck("vec", 0, v);
                    if (rwm) {
                        ck("rwm1_identity_page", 0x91111111UL, VL(TL + 0x540));
                        ck("rwm1_table_page", 0x22222222UL, VL(TP + 0x540));
                    } else {
                        ck("rwm0_identity_page", 0x11111111UL, VL(TL + 0x540));
                        ck("rwm0_table_page", 0xa2222222UL, VL(TP + 0x540));
                    }
                }
                rend("tt_rmw");
            }
        }
        itt_case("tt_ifetch", 0, 0, 0, tt30(blk, 0, 0, 1, 6, 0, 1), 2);
        itt_case("tt_ifetch_nomatch", 0, 0, 0, tt30(blk, 0, 0, 1, 5, 0, 1), 1);
        if (!pre("ptest0_tt")) {
            rs("030 PTEST level 0 of an address matched by TT0 (FC 1): MMUSR T (bit 6) = 1 "
               "(MC68030UM 9.6.3 MMUSR, PTEST)");
            tt_tables();
            mm_cfg.tt0 = tt30(blk, 0, 0, 1, 1, 0, 1);
            nops = 0;
            op(O_PT, TL + 0x10, 0, 1);
            v = run_ops();
            last_vec = v;
            ck("vec", 0, v);
            ck("mmusr_t", 0x40, ops[0].r & 0x40);
            cx("mmusr", ops[0].r);
            rend("ptest0_tt");
        }
        return;
    }
    tt_case("dtt0_user", W_TT40, tt40(blk, 0, 0, 0), 0, 1, 0, 1);
    tt_case("dtt1_user", W_TT40, 0, tt40(blk, 0, 0, 0), 1, 0, 1);
    tt_case("dtt_super", W_TT40, tt40(blk, 0, 1, 0), 0, 0, 1, 0);
    tt_case("dtt_any", W_TT40, tt40(blk, 0, 2, 0), 0, 1, 1, -1);
    tt_case("dtt_addr_mask", W_TT40, tt40(blk ^ 0x01000000UL, 1, 0, 0), 0, 1, 0, -1);
    tt_case("dtt_addr_nomatch", W_TT40, tt40(blk ^ 0x01000000UL, 0, 0, 0), 0, 0, 0, -1);
    itt_case("itt0_fetch", tt40(blk, 0, 2, 0), 0, 0, 0, 2);
    itt_case("itt1_fetch", 0, tt40(blk, 0, 2, 0), 0, 0, 2);
    itt_case("dtt_not_ifetch", 0, 0, tt40(blk, 0, 2, 0), 0, 1);
    if (!pre("dtt_wp_fault")) {
        ULONG a;
        rs(W_TT40);
        tt_tables();
        mm_cfg.tt0 = tt40(blk, 0, 0, 1);
        hook_set(0);
        a = TL + 0x50c;
        in_clear();
        mm_in[8] = a;
        mm_in[1] = 0x11223344UL;
        mm_in[2] = 1;
        v = RUN((void (*)(void))sn_moves_wr);
        last_vec = v;
        ci("dtt0", mm_cfg.tt0);
        ci("addr", a);
        if (ck("vec", 2, v) && ck("faults", 1, mm_nlog))
            chk_fault(0, a, 1, 1, K_TTW, 0x11223344UL, A(sn_moves_wr_at));
        ck("identity_page_unchanged", VT(0x50c), VL(TL + 0x50c));
        ck("table_page_unchanged", VP(0x50c), VL(TP + 0x50c));
        rend("dtt_wp_fault");
    }
    if (IS40 && !pre("ptest040_tt")) {
        rs("040 PTESTR of an address matched by DTT0 (S=user, DFC=1): MMUSR T (bit 1) = 1 "
           "(MC68040UM 3.5 MMUSR, M68000PRM PTEST)");
        tt_tables();
        mm_cfg.tt0 = tt40(blk, 0, 0, 0);
        nops = 0;
        op(O_PT, TL + 0x10, 0, 1);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("mmusr_t", 2, ops[0].r & 2);
        cx("mmusr", ops[0].r);
        rend("ptest040_tt");
    }
    if (IS60 && !pre("plpa_tt")) {
        rs("060 PLPAR of an address matched by DTT0 (S=user, DFC=1): no translation, An "
           "unchanged (MC68060UM 4.1.3, M68000PRM PLPA)");
        tt_tables();
        mm_cfg.tt0 = tt40(blk, 0, 0, 0);
        nops = 0;
        op(O_PT, TL + 0x14, 0, 1);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("physical", TL + 0x14, ops[0].r);
        rend("plpa_tt");
    }
}

/* --- caches --- */
static const char W_CI30[] =
    "030 CI bit: with the data cache on, a write through a cache-inhibited alias reaches memory; "
    "a cacheable alias may still return the stale cached data until the data cache is cleared "
    "(CACR CD), then it must return the new data; a write through the cacheable alias is "
    "write-through, so the inhibited alias sees it at once (MC68030UM 6.1, 9.5.3.2 CI)";
static const char W_CB[] =
    "040/060 CM=01 copyback: a write through the copyback alias may stay in the data cache (the "
    "cache-inhibited alias may still read the old data) until CPUSHA DC; after the push the "
    "inhibited alias must read the new data (MC68040UM 4.3/4.4, MC68060UM 5.3)";
static const char W_WT[] =
    "040/060 CM=00 write-through: a write through the cacheable alias reaches memory, so after "
    "a NOP the cache-inhibited alias reads it (MC68040UM 4.3, MC68060UM 5.3)";
static const char W_STALE40[] =
    "040/060: a write through the cache-inhibited alias goes to memory; the cacheable alias may "
    "keep a stale line until CINVL DC invalidates it, then it reads the new data "
    "(MC68040UM 4.3/4.5, MC68060UM 5.3/5.5)";

static void t_cache(void)
{
    ULONG v, P0 = dpage(4096, 0);
    if (IS30) {
        if (pre("ci_coherency"))
            return;
        rs(W_CI30);
        gb_begin(4096);
        gb_map(LW(0), P0, 0);
        gb_map(LW(1), P0, D_CI);
        gb_end();
        mm_cfg.cacr = 0x0100;           /* ED */
        fill_data();
        nops = 0;
        op(O_R, LW(0) + 0x700, 0, 0);
        op(O_W, LW(1) + 0x700, 0xa1b2c3d4UL, 0);
        op(O_R, LW(0) + 0x700, 0, 0);
        op(O_C, 0, 3, 0);
        op(O_R, LW(0) + 0x700, 0, 0);
        op(O_W, LW(0) + 0x704, 0x5e6f7a8bUL, 0);
        op(O_R, LW(1) + 0x704, 0, 0);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck2("cacheable_before_clear", mk(P0 + 0x700), 0xa1b2c3d4UL, ops[2].r);
        ck("cacheable_after_clear", 0xa1b2c3d4UL, ops[4].r);
        ck("inhibited_after_wt_write", 0x5e6f7a8bUL, ops[6].r);
        cx("undef_stale_seen", ops[2].r == mk(P0 + 0x700));
        rend("ci_coherency");
        return;
    }
    if (!pre("cm_copyback_push")) {
        rs(W_CB);
        gb_begin(4096);
        gb_map(LW(0), P0, CM_CB);
        gb_map(LW(1), P0, CM_CI);
        gb_end();
        mm_cfg.cacr = 0x80000000UL;     /* DE (040) / EDC (060) */
        fill_data();
        nops = 0;
        op(O_R, LW(0) + 0x710, 0, 0);
        op(O_W, LW(0) + 0x710, 0x10203040UL, 0);
        op(O_NOP, 0, 0, 0);
        op(O_R, LW(1) + 0x710, 0, 0);
        op(O_C, 0, 0, 0);
        op(O_R, LW(1) + 0x710, 0, 0);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck2("inhibited_before_push", mk(P0 + 0x710), 0x10203040UL, ops[3].r);
        ck("inhibited_after_push", 0x10203040UL, ops[5].r);
        ck("memory_after", 0x10203040UL, VL(P0 + 0x710));
        cx("undef_old_seen_before_push", ops[3].r == mk(P0 + 0x710));
        rend("cm_copyback_push");
    }
    if (!pre("cm_writethrough")) {
        rs(W_WT);
        gb_begin(4096);
        gb_map(LW(0), P0, CM_WT);
        gb_map(LW(1), P0, CM_CI);
        gb_end();
        mm_cfg.cacr = 0x80000000UL;
        fill_data();
        nops = 0;
        op(O_R, LW(0) + 0x720, 0, 0);
        op(O_W, LW(0) + 0x720, 0x50607080UL, 0);
        op(O_NOP, 0, 0, 0);
        op(O_R, LW(1) + 0x720, 0, 0);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("inhibited_after_wt_write", 0x50607080UL, ops[3].r);
        rend("cm_writethrough");
    }
    if (!pre("cm_stale_cinv")) {
        rs(W_STALE40);
        gb_begin(4096);
        gb_map(LW(0), P0, CM_WT);
        gb_map(LW(1), P0, CM_CI);
        gb_end();
        mm_cfg.cacr = 0x80000000UL;
        fill_data();
        nops = 0;
        op(O_R, LW(0) + 0x730, 0, 0);
        op(O_W, LW(1) + 0x730, 0x90a0b0c0UL, 0);
        op(O_NOP, 0, 0, 0);
        op(O_R, LW(0) + 0x730, 0, 0);
        op(O_C, P0 + 0x730, 1, 0);
        op(O_R, LW(0) + 0x730, 0, 0);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck2("cacheable_before_cinv", mk(P0 + 0x730), 0x90a0b0c0UL, ops[3].r);
        ck("cacheable_after_cinv", 0x90a0b0c0UL, ops[5].r);
        cx("undef_stale_seen", ops[3].r == mk(P0 + 0x730));
        rend("cm_stale_cinv");
    }
}

/* --- 68030 only: early termination, levels, limits --- */
static const char W_ETA[] =
    "030 early-termination page descriptor at level A (16 MB): the window's level-A entry maps "
    "it onto physical $00000000-$00FFFFFF, so window+ROM offset reads the ROM; PTEST #7 stops "
    "with N=1 (MC68030UM 9.5.3.3, 9.6.3)";
static const char W_ETB[] =
    "030 early-termination page descriptor at level B (256 KB): window+offset reads the ROM at "
    "the same offset, the remaining index bits are passed through (MC68030UM 9.5.3.3)";
static const char W_LEV[] =
    "030 table layouts: the TC fields IS + PS + TIA + TIB + TIC + TID = 32 select 2, 3 or 4 table "
    "levels; the page translates and PTEST #7 reports N = the number of levels (MC68030UM 9.5, "
    "9.6.1, 9.6.3)";
static const char W_IS[] =
    "030 TC.IS=4: the top 4 logical address bits are ignored, so window and window & $0FFFFFFF "
    "read the same physical page (MC68030UM 9.6.1 IS)";

static void t_et(void)
{
    ULONG v, e[3], *s;
    if (!pre("early_term_a")) {
        rs(W_ETA);
        gb_begin30(&c30_def, 0);
        s = slot30(win, 0);
        pd30(s, 0, D_CI);
        gb_end();
        e[0] = VL(rom + 0x100);
        e[1] = VL(0xfffff0UL);
        nops = 0;
        op(O_R, win + rom + 0x100, 0, 0);
        op(O_R, win + 0xfffff0UL, 0, 0);
        op(O_PT, win + rom, 7 * 2, 5);
        v = run_ops();
        last_vec = v;
        ci("desc", *s);
        ck("vec", 0, v);
        ck("rom_100", e[0], ops[0].r);
        ck("rom_fffff0", e[1], ops[1].r);
        ck("ptest_n", 1, ops[2].r & 7);
        ck("ptest_bliswi", 0, ops[2].r & 0xec00);
        cx("mmusr", ops[2].r);
        rend("early_term_a");
    }
    if (!pre("early_term_b")) {
        rs(W_ETB);
        gb_begin30(&c30_def, 0);
        s = slot30(win, 1);
        pd30(s, rom, D_CI);
        gb_end();
        e[0] = VL(rom);
        e[1] = VL(rom + 0x1fffc);
        e[2] = VL(rom + 0x3fff0);
        nops = 0;
        op(O_R, win, 0, 0);
        op(O_R, win + 0x1fffc, 0, 0);
        op(O_R, win + 0x3fff0, 0, 0);
        op(O_PT, win + 0x3fff0, 7 * 2, 5);
        v = run_ops();
        last_vec = v;
        ci("desc", *s);
        ck("vec", 0, v);
        ck("rom_0", e[0], ops[0].r);
        ck("rom_1fffc", e[1], ops[1].r);
        ck("rom_3fff0", e[2], ops[2].r);
        ck("ptest_n", 2, ops[3].r & 7);
        cx("mmusr", ops[3].r);
        rend("early_term_b");
    }
}

static void lev_case(const char *id, const struct c30 *c, const char *w, int alias)
{
    ULONG v, ps = 1UL << c->ps, P0, P1, e0, e1;
    int ro;
    if (pre(id))
        return;
    rs(w);
    gb_begin30(c, 0);
    ro = ps >= 16384 || dcount(ps) < 2;
    P0 = ro ? rom : dpage(ps, 0);
    P1 = ro ? rom + ps : dpage(ps, 1);
    gb_map(LW(0), P1, D_CI);
    gb_map(LW(1), P0, D_CI);
    gb_end();
    fill_data();
    e0 = VL(P1 + 0x24);
    e1 = VL(P0 + ps - 0x10);
    nops = 0;
    op(O_R, LW(0) + 0x24, 0, 0);
    op(O_R, LW(1) + ps - 0x10, 0, 0);
    op(O_PT, LW(0), 7 * 2, 5);
    if (alias)
        op(O_R, (LW(0) & 0x0fffffffUL) + 0x24, 0, 0);
    v = run_ops();
    last_vec = v;
    ci("tc", mm_cfg.tc);
    ck("vec", 0, v);
    ck("l0", e0, ops[0].r);
    ck("l1", e1, ops[1].r);
    ck("ptest_n", c->nl, ops[2].r & 7);
    ck("ptest_flags", 0, ops[2].r & 0xec00);
    if (alias)
        ck("alias_read", e0, ops[3].r);
    cx("mmusr", ops[2].r);
    rend(id);
}

static const char W_LIM[] =
    "030 long-format table descriptor limit: L/U=0 upper limit (index > LIMIT) or L/U=1 lower "
    "limit (index < LIMIT) makes the next index out of bounds: the access faults (bus error) "
    "and PTEST sets MMUSR L and I; in-bounds indexes translate (MC68030UM 9.5.1.2, 9.6.3)";
static const char W_LIMR[] =
    "030 CRP limit: with an upper limit equal to the window's level-A index, the next level-A "
    "entry is out of bounds and faults (MC68030UM 9.5.1.1, 9.5.1.2)";
static const char W_LIMET[] =
    "030 long-format early-termination page descriptor: its limit checks the next (level-B) "
    "index: B index 0 translates, B index 62 > LIMIT 31 faults (MC68030UM 9.5.1.2, 9.5.3.3)";

static void lim_one(const char *id, int lower)
{
    ULONG v, *ea, P0 = dpage(4096, 0), P1 = dpage(4096, 1), a_ok, a_bad;
    if (pre(id))
        return;
    rs(W_LIM);
    gb_begin30(&c30_def, 1);
    gb_map(win, P0, D_CI);                     /* B index 0 */
    gb_map(win + 0x40000UL, P1, D_CI);         /* B index 1 */
    ea = slot30(win, 0);
    ea[0] = (lower ? 0x80000000UL | (1UL << 16) : 0) | 0xfc00UL | 3;
    gb_end();
    fill_data();
    a_ok = lower ? win + 0x40010UL : win + 0x10;
    a_bad = lower ? win + 0x10 : win + 0x40010UL;
    nops = 0;
    op(O_R, a_ok, 0, 0);
    op(O_PT, a_bad, 7 * 2, 5);
    op(O_PT, a_ok, 7 * 2, 5);
    v = run_ops();
    last_vec = v;
    ci("level_a_desc_w0", ea[0]);
    ck("vec", 0, v);
    ck("in_bounds_read", lower ? mk(P1 + 0x10) : mk(P0 + 0x10), ops[0].r);
    ck("ptest_out_l_i", 0x4400, ops[1].r & 0x4400);
    ck("ptest_in_l_i", 0, ops[2].r & 0x4400);
    cx("mmusr_out", ops[1].r);
    cx("mmusr_in", ops[2].r);
    hook_set(0);
    v = run_sn(sn_rd, a_bad, 0);
    ck("out_of_bounds_vec", 2, v);
    if (v == 2)
        ck("fault_addr", a_bad, fa_of(0));
    rend(id);
}

static void t_limits(void)
{
    ULONG v, P0 = dpage(4096, 0), P1 = dpage(4096, 1), *s;
    int i, ok;
    lim_one("limit_upper", 0);
    lim_one("limit_lower", 1);
    if (!pre("limit_root")) {
        ULONG widx = win >> 24;
        ok = 1;
        for (i = 0; i < nneed; i++)
            if ((need[i].hi - 1) >> 24 > widx)
                ok = 0;
        if (!ok)
            skip("limit_root", "an identity-mapped range lies above the test window");
        else {
            rs(W_LIMR);
            gb_begin30(&c30_def, 0);
            gb_map(win, P0, D_CI);
            gb_map(win + 0x01000000UL, P1, D_CI);
            gb_end();
            mm_cfg.crp[0] = (widx << 16) | 2;
            mm_cfg.srp[0] = mm_cfg.crp[0];
            fill_data();
            nops = 0;
            op(O_R, win + 0x20, 0, 0);
            op(O_PT, win + 0x01000020UL, 7 * 2, 5);
            v = run_ops();
            last_vec = v;
            ci("crp_w0", mm_cfg.crp[0]);
            ck("vec", 0, v);
            ck("in_bounds_read", mk(P0 + 0x20), ops[0].r);
            ck("ptest_out_l_i", 0x4400, ops[1].r & 0x4400);
            cx("mmusr_out", ops[1].r);
            hook_set(0);
            v = run_sn(sn_rd, win + 0x01000020UL, 0);
            ck("out_of_bounds_vec", 2, v);
            rend("limit_root");
        }
    }
    if (!pre("limit_early_term")) {
        rs(W_LIMET);
        gb_begin30(&c30_def, 1);
        s = slot30(win, 0);
        s[0] = (31UL << 16) | 0xfc00UL | D_CI | 1;
        s[1] = 0;
        gb_end();
        nops = 0;
        op(O_R, win + 0x100, 0, 0);
        op(O_PT, win + rom, 7 * 2, 5);
        v = run_ops();
        last_vec = v;
        ci("desc_w0", s[0]);
        ck("vec", 0, v);
        ck("in_bounds_read", VL(0x100), ops[0].r);
        ck("ptest_out_l_i", 0x4400, ops[1].r & 0x4400);
        cx("mmusr_out", ops[1].r);
        hook_set(0);
        v = run_sn(sn_rd, win + rom, 0);
        ck("out_of_bounds_vec", 2, v);
        rend("limit_early_term");
    }
}

/* --- 68030 PTEST / PLOAD / PFLUSH --- */
#define M_B 0x8000
#define M_L 0x4000
#define M_S 0x2000
#define M_W 0x0800
#define M_I 0x0400
#define M_M 0x0200
#define M_T 0x0040
static const char W_PL[] =
    "030 PLOADR: after PFLUSHA PTEST level 0 finds no ATC entry (MMUSR I=1); PLOADR loads one "
    "(I=0) and sets U in the page descriptor (MC68030UM 9.6.3, PLOAD/PTEST)";
static const char W_PT0O[] =
    "030 PTEST level 0 searches the ATC by page: an address inside a loaded page (not its "
    "first byte) finds the entry (MMUSR I=0) (MC68030UM 9.4, PTEST)";
static const char W_PLW[] =
    "030 PLOADW: the table search is done as for a write: U and M are set in the page "
    "descriptor (MC68030UM PLOAD: history bits updated as for the access type)";
static const char W_PLO[] =
    "030 PLOADR #5 creates the entry for FC 5 only: an ATC entry of the same page for FC 1 "
    "(loaded by a MOVES read) stays valid (PTEST #0 FC 1: I=0); ATC entries are tagged by FC "
    "(MC68030UM 9.4, PLOAD)";
static const char W_PFFC[] =
    "030 PFLUSH FC,#mask: flushes the ATC entries whose FC matches in the mask bits (1 = "
    "compared), others stay (PTEST level 0: I=1 flushed, I=0 kept) (MC68030UM PFLUSH)";
static const char W_PFEA[] =
    "030 PFLUSH FC,#mask,<ea>: flushes only the entry for that page and FC (MC68030UM PFLUSH)";
static const char W_PFA[] =
    "030 PFLUSHA: flushes every ATC entry (MC68030UM PFLUSH)";
static const char W_PTV[] =
    "030 PTESTR #7 of a valid page: MMUSR B,L,S,W,I,M,T = 0, N = 3 levels, An = address of the "
    "page descriptor; PTEST does not change the U bit and does not load the ATC "
    "(MC68030UM 9.6.3, PTEST)";
static const char W_PTI[] =
    "030 PTESTR #7 of an invalid page descriptor: MMUSR I=1, N=3, An = address of that "
    "descriptor (MC68030UM PTEST)";
static const char W_PTW[] =
    "030 PTESTW #7: WP set in the page descriptor or in a table descriptor on the path gives "
    "MMUSR W=1 (MC68030UM 9.5.3.2, PTEST)";
static const char W_PTS[] =
    "030 PTESTR #7 with FC 1 of a long-format page with S=1: MMUSR S=1; with FC 5: S=0 "
    "(MC68030UM PTEST)";
static const char W_PTM[] =
    "030 PTESTR #7 of a page whose descriptor has M=1: MMUSR M=1 (MC68030UM PTEST)";
static const char W_PT1[] =
    "030 PTESTR #1: the search stops after level A: N=1, An = address of the level-A "
    "descriptor (MC68030UM PTEST)";

#define PK(level, w) ((ULONG)(level) * 2 + (w))

static void t_ptest30(void)
{
    ULONG v, *s0, *s1, *s2, P0 = dpage(4096, 0), P1 = dpage(4096, 1);
    if (!pre("pload_ptest0")) {
        rs(W_PL);
        gb_begin(4096);
        gb_map(LW(0), P0, D_CI);
        s0 = gb_slot(LW(0));
        gb_end();
        nops = 0;
        op(O_FL, 0, 0, 0);
        op(O_PT, LW(0), PK(0, 0), 5);
        op(O_PL, LW(0), 0, 5);
        op(O_PT, LW(0), PK(0, 0), 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("before_pload_i", M_I, ops[1].r & M_I);
        ck("after_pload_i", 0, ops[3].r & M_I);
        ck("desc_u", D_U, *s0 & D_U);
        cx("mmusr_before", ops[1].r);
        cx("mmusr_after", ops[3].r);
        rend("pload_ptest0");
    }
    if (!pre("ptest0_offset")) {
        rs(W_PT0O);
        gb_begin(4096);
        gb_map(LW(0), P0, D_CI);
        gb_end();
        nops = 0;
        op(O_FL, 0, 0, 0);
        op(O_PL, LW(0), 0, 5);
        op(O_PT, LW(0) + 0x234, PK(0, 0), 5);
        op(O_PT, LW(0), PK(0, 0), 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("offset_i", 0, ops[2].r & M_I);
        ck("page_start_i", 0, ops[3].r & M_I);
        cx("mmusr_offset", ops[2].r);
        rend("ptest0_offset");
    }
    if (!pre("pload_w_m")) {
        rs(W_PLW);
        gb_begin(4096);
        gb_map(LW(1), P1, D_CI);
        s1 = gb_slot(LW(1));
        gb_end();
        nops = 0;
        op(O_FL, 0, 0, 0);
        op(O_PL, LW(1), 1, 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("desc_um", D_U | D_M, *s1 & (D_U | D_M));
        rend("pload_w_m");
    }
    if (!pre("pload_other_fc")) {
        rs(W_PLO);
        gb_begin(4096);
        gb_map(LW(0), P0, D_CI);
        gb_end();
        nops = 0;
        op(O_FL, 0, 0, 0);
        op(O_MR, LW(0), 0, 1);              /* ATC entry (FC 1, page 0) */
        op(O_PT, LW(0), PK(0, 0), 1);
        op(O_PL, LW(0), 0, 5);              /* PLOADR #5 for the same page */
        op(O_PT, LW(0), PK(0, 0), 1);
        op(O_PT, LW(0), PK(0, 0), 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("fc1_before_i", 0, ops[2].r & M_I);
        ck("fc1_after_pload5_i", 0, ops[4].r & M_I);
        ck("fc5_loaded_i", 0, ops[5].r & M_I);
        rend("pload_other_fc");
    }
    if (!pre("pflush_fc")) {
        rs(W_PFFC);
        gb_begin(4096);
        gb_map(LW(0), P0, D_CI);
        gb_end();
        nops = 0;
        op(O_FL, 0, 0, 0);
        op(O_MR, LW(0), 0, 1);              /* ATC entry (FC 1, page 0) */
        op(O_MR, LW(0), 0, 5);              /* ATC entry (FC 5, page 0) */
        op(O_PT, LW(0), PK(0, 0), 1);
        op(O_PT, LW(0), PK(0, 0), 5);
        op(O_FL, 0, 1, 1);                  /* PFLUSH #1,#7 */
        op(O_PT, LW(0), PK(0, 0), 1);
        op(O_PT, LW(0), PK(0, 0), 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("loaded_fc1_i", 0, ops[3].r & M_I);
        ck("loaded_fc5_i", 0, ops[4].r & M_I);
        ck("fc1_flushed_i", M_I, ops[6].r & M_I);
        ck("fc5_kept_i", 0, ops[7].r & M_I);
        rend("pflush_fc");
    }
    if (!pre("pflush_fc_mask")) {
        rs(W_PFFC);
        gb_begin(4096);
        gb_map(LW(0), P0, D_CI);
        gb_end();
        nops = 0;
        op(O_FL, 0, 0, 0);
        op(O_MR, LW(0), 0, 1);
        op(O_MR, LW(0), 0, 5);
        op(O_FL, 0, 2, 4);                  /* PFLUSH #4,#4: all supervisor */
        op(O_PT, LW(0), PK(0, 0), 1);
        op(O_PT, LW(0), PK(0, 0), 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("fc1_kept_i", 0, ops[4].r & M_I);
        ck("fc5_flushed_i", M_I, ops[5].r & M_I);
        rend("pflush_fc_mask");
    }
    if (!pre("pflush_fc_ea")) {
        rs(W_PFEA);
        gb_begin(4096);
        gb_map(LW(0), P0, D_CI);
        gb_map(LW(1), P1, D_CI);
        gb_end();
        nops = 0;
        op(O_FL, 0, 0, 0);
        op(O_MR, LW(0), 0, 5);
        op(O_MR, LW(1), 0, 5);
        op(O_MR, LW(0), 0, 1);
        op(O_FL, LW(0), 3, 5);              /* PFLUSH #5,#7,(a0) */
        op(O_PT, LW(0), PK(0, 0), 5);
        op(O_PT, LW(1), PK(0, 0), 5);
        op(O_PT, LW(0), PK(0, 0), 1);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("page_fc5_flushed_i", M_I, ops[5].r & M_I);
        ck("other_page_kept_i", 0, ops[6].r & M_I);
        ck("page_fc1_kept_i", 0, ops[7].r & M_I);
        rend("pflush_fc_ea");
    }
    if (!pre("pflusha_ptest")) {
        rs(W_PFA);
        gb_begin(4096);
        gb_map(LW(0), P0, D_CI);
        gb_map(LW(1), P1, D_CI);
        gb_end();
        nops = 0;
        op(O_MR, LW(0), 0, 1);
        op(O_MR, LW(1), 0, 5);
        op(O_FL, 0, 0, 0);
        op(O_PT, LW(0), PK(0, 0), 1);
        op(O_PT, LW(1), PK(0, 0), 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("fc1_i", M_I, ops[3].r & M_I);
        ck("fc5_i", M_I, ops[4].r & M_I);
        rend("pflusha_ptest");
    }
    if (!pre("ptestr_valid")) {
        rs(W_PTV);
        gb_begin(4096);
        gb_map(LW(0), P0, D_CI);
        s0 = gb_slot(LW(0));
        gb_end();
        nops = 0;
        op(O_FL, 0, 0, 0);
        op(O_PT, LW(0) + 0x10, PK(7, 0), 5);
        op(O_PT, LW(0), PK(0, 0), 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("mmusr", 3, ops[1].r & 0xee47);
        ck("an_desc_addr", A(s0), ops[1].r2);
        ck("desc_u_unchanged", 0, *s0 & D_U);
        ck("atc_not_loaded_i", M_I, ops[2].r & M_I);
        rend("ptestr_valid");
    }
    if (!pre("ptestr_invalid")) {
        rs(W_PTI);
        gb_begin(4096);
        gb_inv(LW(0));
        s0 = gb_slot(LW(0));
        gb_end();
        nops = 0;
        op(O_PT, LW(0), PK(7, 0), 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("mmusr_i", M_I, ops[0].r & (M_I | M_B | M_L));
        ck("mmusr_n", 3, ops[0].r & 7);
        ck("an_desc_addr", A(s0), ops[0].r2);
        rend("ptestr_invalid");
    }
    if (!pre("ptestw_wp")) {
        rs(W_PTW);
        gb_begin(4096);
        gb_map(win, P0, D_CI | D_WP);           /* page WP, B index 0 */
        gb_map(win + 0x40000UL, P1, D_CI);      /* B index 1: WP in the B entry */
        s1 = slot30(win + 0x40000UL, 1);
        s1[0] |= D_WP;
        gb_map(win + 0x80000UL, P1, D_CI);      /* B index 2: no WP */
        gb_end();
        nops = 0;
        op(O_PT, win, PK(7, 1), 5);
        op(O_PT, win + 0x40000UL, PK(7, 1), 5);
        op(O_PT, win + 0x80000UL, PK(7, 1), 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("page_wp_w", M_W, ops[0].r & (M_W | M_I));
        ck("table_wp_w", M_W, ops[1].r & (M_W | M_I));
        ck("no_wp_w", 0, ops[2].r & (M_W | M_I));
        rend("ptestw_wp");
    }
    if (!pre("ptestr_super_user")) {
        rs(W_PTS);
        gb_begin30(&c30_def, 1);
        gb_map(LW(0), P0, D_CI | D_S30);
        gb_end();
        nops = 0;
        op(O_PT, LW(0), PK(7, 0), 1);
        op(O_PT, LW(0), PK(7, 0), 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("fc1_s", M_S, ops[0].r & M_S);
        ck("fc5_s", 0, ops[1].r & M_S);
        cx("mmusr_fc1", ops[0].r);
        rend("ptestr_super_user");
    }
    if (!pre("ptest_modified")) {
        rs(W_PTM);
        gb_begin(4096);
        gb_map(LW(2), P1, D_CI | D_U | D_M);
        gb_end();
        nops = 0;
        op(O_PT, LW(2), PK(7, 0), 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("mmusr_m", M_M, ops[0].r & (M_M | M_I));
        rend("ptest_modified");
    }
    if (!pre("ptest_level1")) {
        rs(W_PT1);
        gb_begin(4096);
        gb_map(LW(0), P0, D_CI);
        s2 = slot30(LW(0), 0);
        gb_end();
        nops = 0;
        op(O_PT, LW(0), PK(1, 0), 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("mmusr_n", 1, ops[0].r & 7);
        ck("an_desc_addr", A(s2), ops[0].r2);
        rend("ptest_level1");
    }
}

static const char W_CFG[] =
    "030 MMU configuration exception: loading CRP with DT=0 (invalid), or TC with E=1 and "
    "IS+PS+TIA..TID not 32, takes vector 56 (MC68030UM 9.6.1, 9.6.2, 8.1.x)";

static void t_cfg30(void)
{
    static ULONG rpbad[2], tcbad, ttall;
    ULONG v;
    if (!pre("config_exc_tc")) {
        rs(W_CFG);
        mm_cfg.flags = 1;
        hook_set(0);
        tcbad = 0x80c08000UL;               /* E, PS=12, TIA=8: sum 20 */
        ttall = 0x00ff8507UL;               /* TT0: every address and FC, CI */
        in_clear();
        mm_in[8] = A(&tcbad);
        mm_in[9] = A(&ttall);
        v = RUN((void (*)(void))sn_cfg_tc);
        last_vec = v;
        ci("tc", tcbad);
        ck("vec", 56, v);
        mm_cfg.flags = 0;
        rend("config_exc_tc");
    }
    if (!pre("config_exc_rp")) {
        rs(W_CFG);
        mm_cfg.flags = 1;
        hook_set(0);
        rpbad[0] = 0x7fff0000UL;            /* DT = 0 */
        rpbad[1] = tlo;
        in_clear();
        mm_in[8] = A(rpbad);
        v = RUN((void (*)(void))sn_cfg_rp);
        last_vec = v;
        ci("crp_w0", rpbad[0]);
        ck("vec", 56, v);
        mm_cfg.flags = 0;
        rend("config_exc_rp");
    }
}

/* --- 68040 PTEST --- */
static const char W_P40[] =
    "040 PTESTR/PTESTW (A0) with DFC: MMUSR = physical page (31-12), B, G, U1/U0, S, CM, M, W, "
    "T, R from the page descriptor and the walk (MC68040UM 3.5, M68000PRM PTEST)";

static void t_ptest40(void)
{
    ULONG v, P0 = dpage(4096, 0), P1 = dpage(4096, 1);
    if (!pre("ptest040_valid")) {
        rs(W_P40);
        gb_begin(4096);
        gb_map(LW(0), P0, D_G | 0x100 | CM_CI);
        gb_end();
        nops = 0;
        op(O_FL, 0, 0, 5);
        op(O_PT, LW(0) + 0x44, 0, 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("mmusr", (P0 & 0xfffff000UL) | D_G | 0x100 | CM_CI | 1, ops[1].r & 0xfffffff7UL);
        rend("ptest040_valid");
    }
    if (!pre("ptest040_wp")) {
        rs(W_P40);
        gb_begin(4096);
        gb_map(LW(0), P0, CM_CI | D_WP);
        gb_end();
        nops = 0;
        op(O_PT, LW(0), 1, 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("mmusr_w_r", 5, ops[0].r & 5);
        cx("mmusr", ops[0].r);
        rend("ptest040_wp");
    }
    if (!pre("ptest040_invalid")) {
        rs(W_P40);
        gb_begin(4096);
        gb_inv(LW(0));
        gb_end();
        nops = 0;
        op(O_PT, LW(0), 0, 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("mmusr_b_t_r", 0, ops[0].r & 0x803);
        cx("mmusr", ops[0].r);
        rend("ptest040_invalid");
    }
    if (!pre("ptest040_super")) {
        rs(W_P40);
        gb_begin(4096);
        gb_map(LW(0), P0, CM_CI | D_S40);
        gb_end();
        nops = 0;
        op(O_PT, LW(0), 0, 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("mmusr_s_r", 0x81, ops[0].r & 0x81);
        cx("mmusr", ops[0].r);
        rend("ptest040_super");
    }
    if (!pre("ptest040_modified")) {
        rs(W_P40);
        gb_begin(4096);
        gb_map(LW(1), P1, CM_CI);
        gb_end();
        nops = 0;
        op(O_W, LW(1) + 8, 5, 0);
        op(O_PT, LW(1), 0, 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("mmusr_m_r", 0x11, ops[1].r & 0x11);
        cx("mmusr", ops[1].r);
        rend("ptest040_modified");
    }
}

/* --- 68060 PLPA --- */
static const char W_PLPA[] =
    "060 PLPAR/PLPAW (A0) with DFC: A0 = the physical address of the logical address "
    "(MC68060UM 4.4, M68000PRM PLPA)";
static const char W_PLPAF[] =
    "060 PLPA of an invalid page (PLPAR) or a write-protected page (PLPAW): access error, "
    "vector 2, format $4, FA = the logical address, FSLW PF (invalid) / WP, PC = the PLPA "
    "(MC68060UM 4.4, 8.4.1)";

static void t_plpa(void)
{
    ULONG v, a, P0 = dpage(4096, 0);
    int w;
    for (w = 0; w < 2; w++) {
        const char *id = w ? "plpaw_valid" : "plpar_valid";
        if (pre(id))
            continue;
        rs(W_PLPA);
        gb_begin(4096);
        gb_map(LW(0), P0, CM_CI);
        gb_end();
        nops = 0;
        op(O_PT, LW(0) + 0x123, (ULONG)w, 5);
        v = run_ops();
        last_vec = v;
        ck("vec", 0, v);
        ck("physical", P0 + 0x123, ops[0].r);
        rend(id);
    }
    for (w = 0; w < 2; w++) {
        const char *id = w ? "plpaw_wp_fault" : "plpar_invalid_fault";
        if (pre(id))
            continue;
        rs(W_PLPAF);
        gb_begin(4096);
        if (w)
            gb_map(LW(0), P0, CM_CI | D_WP);
        else
            gb_inv(LW(0));
        gb_end();
        hook_set(0);
        a = LW(0) + 0x88;
        in_clear();
        mm_in[8] = a;
        mm_in[2] = 5;
        v = RUN((void (*)(void))(w ? sn_plpaw : sn_plpar));
        last_vec = v;
        ci("addr", a);
        if (ck("vec", 2, v) && ck("faults", 1, mm_nlog)) {
            volatile UWORD *f = mm_log[0].f;
            ck("format_vector", 0x4008, f[3]);
            ck("fault_addr", a, fl32(f, 4));
            ck("pc", w ? A(sn_plpaw_at) : A(sn_plpar_at), fl32(f, 1));
            ck("fslw_pf_wp", w ? 0x80 : 0x200, fl32(f, 6) & 0x380);
            cx("fslw", fl32(f, 6));
        }
        rend(id);
    }
}

/* --- final register check --- */
static void t_restored(void)
{
    rs("the module restores every MMU register it changed (TC, TT0/TT1, CRP/SRP on the 030; TC, "
       "URP, SRP, ITT0/1, DTT0/1 on the 040/060) to the value it found");
    mm_read_now();
    ck("tc", mm_orig.tc, mm_now.tc);
    ck("tt0_or_dtt0", mm_orig.tt0, mm_now.tt0);
    ck("tt1_or_dtt1", mm_orig.tt1, mm_now.tt1);
    if (IS30) {
        if (mm_orig.crp[0] & 3) {
            ck("crp_hi", mm_orig.crp[0], mm_now.crp[0]);
            ck("crp_lo", mm_orig.crp[1], mm_now.crp[1]);
        } else
            cx("crp_orig_dt0_hi", mm_orig.crp[0]);
        if (mm_orig.srp[0] & 3) {
            ck("srp_hi", mm_orig.srp[0], mm_now.srp[0]);
            ck("srp_lo", mm_orig.srp[1], mm_now.srp[1]);
        }
    } else {
        ck("urp", mm_orig.urp, mm_now.urp);
        ck("srp", mm_orig.srp[0], mm_now.srp[0]);
        ck("itt0", mm_orig.itt0, mm_now.itt0);
        ck("itt1", mm_orig.itt1, mm_now.itt1);
    }
    ck("cacr", mm_orig.cacr, mm_now.cacr);
    rend("regs_restored");
}

/* ------------------------------------------------------------ driver */
static void run_all(void)
{
    t_basic();
    if (no_mmu)
        return;
    if (IS30) {
        static const ULONG ps[8] = { 256, 512, 1024, 2048, 4096, 8192, 16384, 32768 };
        static const char *const ids[8] = { "ps_256", "ps_512", "ps_1k", "ps_2k", "ps_4k",
                                            "ps_8k", "ps_16k", "ps_32k" };
        int i;
        for (i = 0; i < 8; i++)
            t_ps(ids[i], ps[i]);
    } else {
        t_ps("ps_4k", 4096);
        t_ps("ps_8k", 8192);
    }
    t_faults();
    t_misaligned();
    t_ifetch();
    t_history();
    t_atc();
    t_thrash();
    t_nested();
    t_indirect();
    t_roots();
    t_tt();
    t_cache();
    if (IS30) {
        static const struct c30 l2 = { 15, 0, { 9, 8, 0, 0 }, 2 };
        static const struct c30 l4 = { 12, 0, { 8, 4, 4, 4 }, 4 };
        static const struct c30 lis = { 12, 4, { 4, 6, 6, 0 }, 3 };
        int i, ok = 1;
        t_et();
        lev_case("levels_2", &l2, W_LEV, 0);
        lev_case("levels_4", &l4, W_LEV, 0);
        for (i = 0; i < nneed; i++)
            if (need[i].hi > 0x10000000UL)
                ok = 0;
        if (overlaps(win & 0x0fffffffUL, 0x02000000UL))
            ok = 0;
        if (ok)
            lev_case("levels_is", &lis, W_IS, 1);
        else if (!pre("levels_is"))
            skip("levels_is", "memory the test needs lies above 256 MB (or aliases the window) "
                 "and would be remapped by IS=4");
        t_limits();
        t_ptest30();
        t_cfg30();
    } else if (IS40)
        t_ptest40();
    else
        t_plpa();
}

static void restore_rp30(void)
{
    /* CRP/SRP were changed; put back the originals that are valid (DT != 0).
     * A DT=0 original cannot be loaded without a configuration exception. */
    if ((mm_orig.crp[0] & 3) || (mm_orig.srp[0] & 3)) {
        static ULONG crp[2], srp[2];
        crp[0] = (mm_orig.crp[0] & 3) ? mm_orig.crp[0] : mm_cfg.crp[0];
        crp[1] = (mm_orig.crp[0] & 3) ? mm_orig.crp[1] : mm_cfg.crp[1];
        srp[0] = (mm_orig.srp[0] & 3) ? mm_orig.srp[0] : mm_cfg.srp[0];
        srp[1] = (mm_orig.srp[0] & 3) ? mm_orig.srp[1] : mm_cfg.srp[1];
        if (crp[0] & 3 && srp[0] & 3) {
            mm_cfg.flags = 1;
            hook_set(0);
            in_clear();
            mm_in[8] = A(crp);
            mm_in[9] = A(srp);
            mm_run((void (*)(void))sn_load_rp30);
            mm_cfg.flags = 0;
        }
    }
}

void amt_run_mmu(void)
{
    LONG c = AMT_CPU();
    int i, code;
    ULONG sp, tc_e;
    static const ULONG wins[4] = { 0xae000000UL, 0x6e000000UL, 0x2e000000UL, 0xee000000UL };

    if (c == AMT_CPU_68030)
        fam = 3;
    else if (c == AMT_CPU_68040 || c == AMT_CPU_68LC040)
        fam = 4;
    else if (c == AMT_CPU_68060 || c == AMT_CPU_68LC060)
        fam = 6;
    else
        fam = 0;
    if (!fam || !AMT_MMU()) {
        amt_case("module", AMT_ST_SKIP,
                 c == AMT_CPU_68EC030 || c == AMT_CPU_68EC040 || c == AMT_CPU_68EC060
                     ? "EC variant: no paged MMU"
                     : (c == AMT_CPU_68020 && AMT_MMU()) ? "68020 + 68851 is not covered"
                                                          : "no 68030/040/060 MMU in this machine");
        return;
    }
    mm_fam = (UWORD)fam;
    for (i = 2; i < 64; i++)
        if (i < 24 || i >= 32)
            mm_old[i] = A(amt_vec_set(i, (amt_vec_fn)mm_stub_table[i - 2]));
    mm_save_orig();
    tc_e = IS30 ? (mm_orig.tc & 0x80000000UL) : (mm_orig.tc & 0x8000UL);
    if (tc_e) {
        amt_case_open("module", AMT_ST_SKIP);
        json_str("why", "the MMU was already enabled (TC.E=1) when the module started: the OS "
                        "tables are unknown, so its configuration could not be restored after "
                        "testing");
        json_obj("ctx");
        json_hex("tc", mm_orig.tc);
        json_obj_end();
        amt_case_close();
        return;
    }

    pbase = A(amt_any_pool());
    tlo = (pbase + 511) & ~511UL;
    thi = pbase + T_SIZE;
    dlo = pbase + U_TOP;
    dhi = pbase + POOL_SIZE;
    rom = amt_probe_result.rom_base & ~0x3ffffUL;
    if (rom < 0xf80000UL)
        rom = 0xf80000UL;
    chip_top = 0x200000UL;
    for (i = 0; i < amt_probe_result.n_mem; i++)
        if ((amt_probe_result.mem[i].attr & 2) && amt_probe_result.mem[i].upper > chip_top &&
            amt_probe_result.mem[i].upper <= 0x00a00000UL)
            chip_top = (amt_probe_result.mem[i].upper + 0x3ffff) & ~0x3ffffUL;

    nneed = 0;
    need_add(0, chip_top);
    need_add(0xbf0000UL, 0xc00000UL);
    need_add(0xdf0000UL, 0xe00000UL);
    need_add(rom, 0x1000000UL);
    need_add(A(amt_base), A(amt_base) + amt_hdr->alloc_size);
    sp = A(&sp);
    need_add(sp - 0xa000UL, sp + 0x2000UL);
    need_add(A(amt_vbr()), A(amt_vbr()) + 0x400);
    need_add(pbase, pbase + POOL_SIZE);
    win = 0;
    for (i = 0; i < 4; i++)
        if (!overlaps(wins[i], 0x02000000UL)) {
            win = wins[i];
            break;
        }
    if (!win) {
        amt_case("module", AMT_ST_SKIP, "no free 32 MB logical range for the test window");
        return;
    }

    broken = 0;
    no_mmu = 0;
    in_basic = 0;
    code = amt_try(run_all);
    mm_active = 0;
    mm_hook = 0;
    if (code == AMT_ABORT_UNEXPECTED && amt_last_exc.vector == 11 && IS30 && in_basic) {
        /* an F-line trap from PMOVE CRP/SRP, PFLUSHA or PTEST while setting
         * up basic_translate: a 68EC030 (no paged MMU) that the probe took
         * for a 68030, e.g. because TT0/TT1 were active (68EC030UM: only
         * the TT registers exist). The restore may trap the same way. */
        amt_try(mm_restore_orig);
        amt_case_open("module", AMT_ST_SKIP);
        json_str("why", "an MMU instruction took the F-line trap (vector 11) while setting up "
                        "basic_translate: this behaves as a 68EC030 without a paged MMU");
        json_obj("ctx");
        json_str("probe_cpu", amt_probe_result.cpu);
        json_obj_end();
        amt_case_close();
        return;
    }
    mm_restore_orig();
    if (IS30)
        restore_rp30();
    if (code) {
        amt_case_open("internal_abort", AMT_ST_FAIL);
        json_str("why", "the module was aborted by an exception outside a test window (a "
                        "module bug or an emulator crash in MMU code)");
        json_obj("got");
        json_int("code", code);
        json_int("vector", amt_last_exc.vector);
        json_hex("pc", amt_last_exc.pc);
        json_obj_end();
        json_obj("expect");
        json_int("code", 0);
        json_obj_end();
        amt_case_close();
    }
    if (!no_mmu)
        t_restored();
}
