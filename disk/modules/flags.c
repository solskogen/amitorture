/* flags: condition codes as consumers see them (CONTRACT.md section 10, id 3).
 *
 * Where JIT flag optimisation and lazy flag evaluation break: flags produced
 * several instructions before they are consumed (non-flag instructions,
 * branches, BSR/RTS, DBcc loops, TRAP/RTE, an interrupt in between), X-flag
 * chains (ADDX/SUBX/NEGX/ROXL/ROXR, the ADDX/SUBX/NEGX Z rule), X vs C
 * divergence, MOVE to/from CCR/SR, ORI/ANDI/EORI to CCR, and every Scc/Bcc/
 * DBcc condition over all 32 XNZVC values.
 *
 * All instruction sequences are in flags_asm.s (68000 ISA) and flags_010.s
 * (MOVE from CCR). Expected values come from the M68000 Programmer's
 * Reference Manual (PRM) rules, modelled in C below; every table case runs
 * once against the model and then again under amt_hot(), so an interpreted
 * pass and later (possibly compiled) passes must agree. */
#include "mod.h"

AMT_MODULE(flags, 3, AMT_PH_BARE, 0, 0, 0);

#define CF 0x01
#define VF 0x02
#define ZF 0x04
#define NF 0x08
#define XF 0x10

/* ------------------------------------------------------------ asm */
void fl_scc_table(UBYTE *taken, UWORD *sr);
void fl_bcc_table(UBYTE *taken, UWORD *sr, ULONG wide);
void fl_dbcc_table(UBYTE *taken, UWORD *sr, ULONG *cnt);
void fl_ccrimm_table(UWORD *sr);
void fl_moveccr_table(UWORD *sr, const UWORD *memsrc);
void fl_movesr_table(UWORD *sr);
void fl_preserve_table(UWORD *sr);
void fl_xc_table(UWORD *sr, UBYTE *xr, UBYTE *xa, const UBYTE *pre, ULONG npre);
ULONG fl_shift(ULONG val, ULONG cnt, ULONG ccr, ULONG which);
extern volatile UWORD fl_shift_sr;
ULONG fl_addx_r4(ULONG *dst, const ULONG *src, ULONG ccr);
ULONG fl_subx_r4(ULONG *dst, const ULONG *src, ULONG ccr);
ULONG fl_negx_r4(ULONG *dst, const ULONG *src, ULONG ccr);
typedef ULONG (*xmem_fn)(void *dst_end, const void *src_end, ULONG n, ULONG ccr);
ULONG fl_xmem_addx_l(void *, const void *, ULONG, ULONG);
ULONG fl_xmem_addx_w(void *, const void *, ULONG, ULONG);
ULONG fl_xmem_addx_b(void *, const void *, ULONG, ULONG);
ULONG fl_xmem_subx_l(void *, const void *, ULONG, ULONG);
ULONG fl_xmem_subx_w(void *, const void *, ULONG, ULONG);
ULONG fl_xmem_subx_b(void *, const void *, ULONG, ULONG);
ULONG fl_xmem_negx_l(void *, const void *, ULONG, ULONG);
ULONG fl_xmem_roxlw(void *, const void *, ULONG, ULONG);
ULONG fl_xmem_roxrw(void *, const void *, ULONG, ULONG);
ULONG fl_xmem_roxll(void *, const void *, ULONG, ULONG);
ULONG fl_xmem_roxrl(void *, const void *, ULONG, ULONG);
void fl_trap_table(UWORD *out);
void fl_trap1_h(void);
void fl_trap2_h(void);
void fl_trap3_h(void);
void fl_trapv_h(void);
extern volatile ULONG fl_trapv_count;
typedef ULONG (*fg_fn)(ULONG a, ULONG b, ULONG ccr, UBYTE *out);
extern fg_fn fg_tab[];
void fl_movefromccr_table(UWORD *out);   /* flags_010.s, 68010+ */

/* ------------------------------------------------------------ PRM model */
static ULONG szmask(int sz) { return sz == 1 ? 0xffUL : sz == 2 ? 0xffffUL : 0xffffffffUL; }
static ULONG szsign(int sz) { return sz == 1 ? 0x80UL : sz == 2 ? 0x8000UL : 0x80000000UL; }

static int nz(ULONG rm, int sz)
{
    return (rm & szsign(sz) ? NF : 0) | (rm ? 0 : ZF);
}

/* PRM 3.x condition table (Table 3-19) */
static int cond_true(int cc, int f)
{
    int c = f & 1, v = (f >> 1) & 1, z = (f >> 2) & 1, n = (f >> 3) & 1;
    switch (cc) {
    case 0: return 1;               /* T  */
    case 1: return 0;               /* F  */
    case 2: return !c && !z;        /* HI */
    case 3: return c || z;          /* LS */
    case 4: return !c;              /* CC */
    case 5: return c;               /* CS */
    case 6: return !z;              /* NE */
    case 7: return z;               /* EQ */
    case 8: return !v;              /* VC */
    case 9: return v;               /* VS */
    case 10: return !n;             /* PL */
    case 11: return n;              /* MI */
    case 12: return n == v;         /* GE */
    case 13: return n != v;         /* LT */
    case 14: return !z && n == v;   /* GT */
    default: return z || n != v;    /* LE */
    }
}
static const char *const cc_names[16] = {
    "t", "f", "hi", "ls", "cc", "cs", "ne", "eq",
    "vc", "vs", "pl", "mi", "ge", "lt", "gt", "le"
};

/* d + s + x (ADD/ADDX). isx: Z cleared if nonzero, else unchanged. Returns
 * new XNZVC (X = C); *rm = masked result. */
static int m_add(ULONG s, ULONG d, int x, int sz, int f, int isx, ULONG *rm)
{
    ULONG m = szmask(sz), sg = szsign(sz);
    ULONG sm = s & m, dm = d & m, r = (sm + dm + (ULONG)x) & m;
    int C = (((sm & dm) | (~r & dm) | (sm & ~r)) & sg) != 0;
    int V = (((sm & dm & ~r) | (~sm & ~dm & r)) & sg) != 0;
    int Z = isx ? (r ? 0 : (f & ZF) != 0) : r == 0;
    *rm = r;
    return (C ? CF | XF : 0) | (V ? VF : 0) | (Z ? ZF : 0) | (r & sg ? NF : 0);
}

/* d - s - x (SUB/SUBX/NEG/NEGX/CMP) */
static int m_sub(ULONG s, ULONG d, int x, int sz, int f, int isx, ULONG *rm)
{
    ULONG m = szmask(sz), sg = szsign(sz);
    ULONG sm = s & m, dm = d & m, r = (dm - sm - (ULONG)x) & m;
    int C = (((sm & ~dm) | (r & ~dm) | (sm & r)) & sg) != 0;
    int V = (((~sm & dm & ~r) | (sm & ~dm & r)) & sg) != 0;
    int Z = isx ? (r ? 0 : (f & ZF) != 0) : r == 0;
    *rm = r;
    return (C ? CF | XF : 0) | (V ? VF : 0) | (Z ? ZF : 0) | (r & sg ? NF : 0);
}

enum { K_ROXL, K_ROXR, K_ROL, K_ROR, K_LSL, K_LSR, K_ASL, K_ASR };
static const char *const k_names[8] = { "roxl", "roxr", "rol", "ror", "lsl", "lsr", "asl", "asr" };

/* PRM 4: ROXd/ROd/LSd/ASd, bit by bit. Count 0: ROXd sets C = X, the others
 * clear C; X unchanged. ROd never changes X. ASL sets V if the MSB changes
 * at any time during the shift. */
static int m_shift(int k, int sz, ULONG val, int cnt, int f, ULONG *rm)
{
    ULONG m = szmask(sz), sg = szsign(sz), v = val & m;
    int x = (f & XF) != 0, c = 0, vf = 0, i, out = 0;
    for (i = 0; i < cnt; i++) {
        switch (k) {
        case K_ROXL: out = (v & sg) != 0; v = ((v << 1) | (ULONG)x) & m; x = out; break;
        case K_ROXR: out = (int)(v & 1); v = (v >> 1) | (x ? sg : 0); x = out; break;
        case K_ROL:  out = (v & sg) != 0; v = ((v << 1) | (ULONG)out) & m; break;
        case K_ROR:  out = (int)(v & 1); v = (v >> 1) | (out ? sg : 0); break;
        case K_LSL:  out = (v & sg) != 0; v = (v << 1) & m; x = out; break;
        case K_LSR:  out = (int)(v & 1); v >>= 1; x = out; break;
        case K_ASL:  out = (v & sg) != 0; v = (v << 1) & m;
                     if (((v & sg) != 0) != out) vf = 1;
                     x = out; break;
        default:     out = (int)(v & 1); v = (v >> 1) | (v & sg); x = out; break;
        }
        c = out;
    }
    if (cnt == 0)
        c = (k == K_ROXL || k == K_ROXR) ? x : 0;
    *rm = v;
    return (x ? XF : 0) | (c ? CF : 0) | (vf ? VF : 0) | nz(v, sz);
}

static UWORD sr_hi;   /* SR & $FF00 in the bare phase: the contract value $2000 */

/* ------------------------------------------------------------ table runner */
#define MAXW 4096
static ULONG got[MAXW];
static UBYTE bbuf[1024];
static UWORD wbuf[1280];
static ULONG lbuf[1024];

struct tcase {
    const char *id;
    const char *why;
    ULONG n, w;
    const char *const *names;
    ULONG (*run)(void);
    void (*model)(ULONG i, ULONG *e);
    void (*in)(ULONG i);
};

static const struct tcase *cur_tc;
static ULONG tramp(ULONG it) { (void)it; return cur_tc->run(); }

/* cheap order-sensitive hash of got[] for the repetition check (equality
 * is all that matters; crc32 is too slow on a 68000 for these tables) */
static ULONG crc_got(ULONG words)
{
    ULONG h = 0x811c9dc5UL, i;
    for (i = 0; i < words; i++)
        h = ((h << 5) | (h >> 27)) ^ (got[i] + i);
    return h;
}

/* repetitions for amt_hot: about 4 x amt_hot_reps() entry executions per
 * case (the table code runs n times per pass anyway), at least 4 passes */
static ULONG reps_for(ULONG n)
{
    ULONG base = amt_hot_reps();
    ULONG r = base * 4 / n;
    if (r > base)
        r = base;
    if (r < 4)
        r = 4;
    return r;
}

static long first_bad(const struct tcase *t, ULONG *nbad, ULONG *ebad)
{
    ULONG i, j, e[12];
    long fb = -1;
    *nbad = 0;
    for (i = 0; i < t->n; i++) {
        t->model(i, e);
        for (j = 0; j < t->w; j++)
            if (got[i * t->w + j] != e[j])
                break;
        if (j < t->w) {
            if (fb < 0) {
                fb = (long)i;
                for (j = 0; j < t->w; j++)
                    ebad[j] = e[j];
            }
            (*nbad)++;
        }
    }
    return fb;
}

static void run_tcase(const struct tcase *t)
{
    ULONG nbad = 0, first = 0, bad = 0, e[12], j;
    long fb, hot = -1;

    cur_tc = t;
    t->run();
    fb = first_bad(t, &nbad, e);
    if (fb < 0) {
        hot = amt_hot(tramp, reps_for(t->n), &first, &bad);
        if (hot < 0) {
            amt_case(t->id, AMT_ST_PASS, 0);
            return;
        }
        /* got[] now holds the diverging pass */
        fb = first_bad(t, &nbad, e);
    }
    amt_case_open(t->id, AMT_ST_FAIL);
    if (fb >= 0) {
        json_obj("expect");
        for (j = 0; j < t->w; j++)
            json_hex(t->names[j], e[j]);
        json_obj_end();
        json_obj("got");
        for (j = 0; j < t->w; j++)
            json_hex(t->names[j], got[(ULONG)fb * t->w + j]);
        json_obj_end();
        json_obj("in");
        json_int("entry", fb);
        t->in((ULONG)fb);
        json_obj_end();
    } else {
        json_obj("expect");
        json_hex("crc", first);
        json_obj_end();
        json_obj("got");
        json_hex("crc", bad);
        json_obj_end();
    }
    json_str("why", t->why);
    json_obj("ctx");
    json_int("entries", (long)t->n);
    json_int("mismatches", (long)nbad);
    if (hot >= 0) {
        json_str("pass", "repetition");
        json_int("hot_iter", hot);
    } else
        json_str("pass", "first");
    json_hexn("sr_hi", sr_hi, 4);
    json_obj_end();
    amt_case_close();
}

static void in_ccr(ULONG ccr)
{
    json_hexn("ccr", ccr, 2);
}

/* ------------------------------------------------------------ Scc / Bcc / DBcc */
static const char *const nm_tsr[] = { "taken", "sr" };
static const char *const nm_tsrc[] = { "taken", "sr", "d2" };

static ULONG run_scc(void)
{
    ULONG i;
    fl_scc_table(bbuf, wbuf);
    for (i = 0; i < 512; i++) {
        got[2 * i] = bbuf[i];
        got[2 * i + 1] = wbuf[i];
    }
    return crc_got(1024);
}
static void model_scc(ULONG i, ULONG *e)
{
    ULONG ccr = i >> 4, cc = i & 15;
    e[0] = cond_true((int)cc, (int)ccr) ? 0xff : 0;
    e[1] = sr_hi | ccr;
}
static void in_scc(ULONG i)
{
    in_ccr(i >> 4);
    json_str("insn", "Scc (after MOVE to CCR; earlier Scc in between)");
    json_str("cond", cc_names[i & 15]);
}

static ULONG bcc_wide;
static ULONG run_bcc(void)
{
    ULONG i;
    fl_bcc_table(bbuf, wbuf, bcc_wide);
    for (i = 0; i < 512; i++) {
        got[2 * i] = bbuf[i];
        got[2 * i + 1] = wbuf[i];
    }
    return crc_got(1024);
}
static void model_bcc(ULONG i, ULONG *e)
{
    ULONG ccr = i >> 4, cc = i & 15;
    e[0] = cc == 1 ? 0 : cond_true((int)cc, (int)ccr) ? 0xff : 0;
    e[1] = sr_hi | ccr;
}
static void in_bcc(ULONG i)
{
    in_ccr(i >> 4);
    json_str("insn", (i & 15) == 0 ? "BRA" : "Bcc (one MOVE to CCR, then 16 branches)");
    json_str("cond", cc_names[i & 15]);
    json_str("size", bcc_wide ? "w" : "s");
}

static ULONG run_dbcc(void)
{
    ULONG i;
    fl_dbcc_table(bbuf, wbuf, lbuf);
    for (i = 0; i < 1024; i++) {
        got[3 * i] = bbuf[i];
        got[3 * i + 1] = wbuf[i];
        got[3 * i + 2] = lbuf[i];
    }
    return crc_got(3072);
}
/* PRM DBcc: if cc true, no operation; else Dn.w - 1, branch unless -1 */
static void model_dbcc(ULONG i, ULONG *e)
{
    ULONG start = (i >> 9) ? 0x12340000UL : 0x12340001UL;
    ULONG ccr = (i >> 4) & 31, cc = i & 15;
    if (cond_true((int)cc, (int)ccr)) {
        e[0] = 0;
        e[2] = start;
    } else {
        ULONG w = (start - 1) & 0xffff;
        e[0] = w == 0xffff ? 0 : 0xff;
        e[2] = (start & 0xffff0000UL) | w;
    }
    e[1] = sr_hi | ccr;
}
static void in_dbcc(ULONG i)
{
    in_ccr((i >> 4) & 31);
    json_str("insn", "DBcc d2,target");
    json_str("cond", cc_names[i & 15]);
    json_hex("d2", (i >> 9) ? 0x12340000UL : 0x12340001UL);
}

/* ------------------------------------------------------------ ORI/ANDI/EORI to CCR */
static const UBYTE ccr_imm[12] = { 0x00, 0x01, 0x02, 0x04, 0x08, 0x10, 0x1f, 0xff, 0xe0, 0x15, 0x0a, 0x1b };
static const char *const ccrop_names[3] = { "ORI #imm,CCR", "ANDI #imm,CCR", "EORI #imm,CCR" };
static const char *const nm_sr[] = { "sr" };

static ULONG run_ccrimm(void)
{
    ULONG i;
    fl_ccrimm_table(wbuf);
    for (i = 0; i < 1152; i++)
        got[i] = wbuf[i];
    return crc_got(1152);
}
static void model_ccrimm(ULONG i, ULONG *e)
{
    ULONG v = i / 36, k = i % 36, op = k / 12, imm = ccr_imm[k % 12], r;
    r = op == 0 ? (v | imm) : op == 1 ? (v & imm) : (v ^ imm);
    e[0] = sr_hi | (r & 0x1f);
}
static void in_ccrimm(ULONG i)
{
    ULONG k = i % 36;
    in_ccr(i / 36);
    json_str("insn", ccrop_names[k / 12]);
    json_hexn("imm", ccr_imm[k % 12], 2);
}

/* ------------------------------------------------------------ MOVE to CCR */
static UWORD mccr_src[32];
static const UWORD mccr_imm[4] = { 0xffe0, 0x0015, 0xff1f, 0x800a };

static ULONG run_moveccr(void)
{
    ULONG i;
    fl_moveccr_table(wbuf, mccr_src);
    for (i = 0; i < 292; i++)
        got[i] = wbuf[i];
    return crc_got(292);
}
static ULONG mccr_srcword(ULONG i)
{
    return i < 256 ? (0xa500 | i) : i < 288 ? mccr_src[i - 256] : mccr_imm[i - 288];
}
/* PRM MOVE to CCR: the low byte of the source word goes to the CCR, the
 * upper byte is ignored; CCR bits 7-5 are always zero. */
static void model_moveccr(ULONG i, ULONG *e)
{
    e[0] = sr_hi | (mccr_srcword(i) & 0x1f);
}
static void in_moveccr(ULONG i)
{
    json_str("insn", i < 256 ? "MOVE.W Dn,CCR" : i < 288 ? "MOVE.W (An)+,CCR" : "MOVE.W #imm,CCR");
    json_hexn("src", mccr_srcword(i), 4);
}

/* ------------------------------------------------------------ MOVE to/from SR */
static const char *const nm_movesr[] = { "sr_mem", "sr_reg" };
static ULONG run_movesr(void)
{
    ULONG i;
    fl_movesr_table(wbuf);
    for (i = 0; i < 64; i++)
        got[i] = wbuf[i];
    return crc_got(64);
}
/* SR bits 11 and 7-5 are not implemented on any 680x0 and read as 0 */
static void model_movesr(ULONG i, ULONG *e)
{
    e[0] = e[1] = 0x2000 | i;
}
static void in_movesr(ULONG i)
{
    json_str("insn", "MOVE.W Dn,SR; MOVE.W SR,<ea> (mem and Dn)");
    json_hexn("written", 0x28e0 | i, 4);
}

/* ------------------------------------------------------------ MOVE from CCR (010+) */
static const char *const nm_mfccr[] = { "ccr_reg", "ccr_mem", "sr_after" };
static ULONG run_mfccr(void)
{
    ULONG i;
    fl_movefromccr_table(wbuf);
    for (i = 0; i < 96; i++)
        got[i] = wbuf[i];
    return crc_got(96);
}
static void model_mfccr(ULONG i, ULONG *e)
{
    e[0] = e[1] = i;           /* upper byte zero */
    e[2] = sr_hi | i;
}
static void in_mfccr(ULONG i)
{
    in_ccr(i);
    json_str("insn", "MOVE.W CCR,Dn; MOVE.W CCR,(An)+; MOVE.W SR,(An)+");
}

/* ------------------------------------------------------------ non-flag instructions */
#define FL_NPRES 33
static const char *const pres_names[FL_NPRES] = {
    "movea.l d4,a4", "movea.w d4,a4", "lea 4(a3),a4", "pea (a3)", "addq.l #4,sp",
    "adda.l d4,a4", "adda.w d4,a4", "suba.l d4,a4", "addq.w #1,a4", "subq.l #8,a4",
    "exg d4,d5", "exg a3,a4", "exg d6,a4", "movem.l d4-d5,-(sp)", "movem.l (sp)+,d4-d5",
    "link a6,#-8", "unlk a6", "move.l usp,a4", "move.l a2,usp", "nop",
    "bsr.w sub/rts", "jsr abs/rts", "bra.s", "jmp (a4)", "dbf d6 (d6=0, expires)",
    "dbf d6 (d6=5, taken)", "st d6", "move.w sr,d6", "movem.w d4,(a3)", "movem.l (a3),d6",
    "lea 2(a3,d5.w),a4", "swap d4 ($00018000)", "swap d5 (0)"
};
static ULONG run_preserve(void)
{
    ULONG i;
    fl_preserve_table(wbuf);
    for (i = 0; i < 32 * FL_NPRES; i++)
        got[i] = wbuf[i];
    return crc_got(32 * FL_NPRES);
}
/* PRM: none of these affect the condition codes, except SWAP (X not
 * affected, N/Z from the 32-bit result, V and C cleared). */
static void model_preserve(ULONG i, ULONG *e)
{
    ULONG v = i / FL_NPRES, k = i % FL_NPRES;
    if (k == 31)
        e[0] = sr_hi | (v & XF) | NF;
    else if (k == 32)
        e[0] = sr_hi | (v & XF) | ZF;
    else
        e[0] = sr_hi | v;
}
static void in_preserve(ULONG i)
{
    in_ccr(i / FL_NPRES);
    json_str("insn", pres_names[i % FL_NPRES]);
}

/* ------------------------------------------------------------ X vs C */
#define FL_NXC 32
static const UBYTE xc_pre[4] = { 0x00, 0x10, 0x0f, 0x1f };
/* type: S = compare 1-2 (N,C set), L = logical/move result r (N,Z from r,
 * V=C=0), B = bit test (only Z: Z = !bit), R = rotate without X (N,Z, C),
 * A = no flags. X is never affected by any of them (PRM). */
struct xcdef { const char *name; char type; ULONG r; UBYTE c; };
static const struct xcdef xc_defs[FL_NXC] = {
    { "cmp.l d5,d0 (1-2)", 'S', 0, 1 }, { "cmpi.l #2,d0 (1-2)", 'S', 0, 1 },
    { "cmpa.l a4,a3 (1-2)", 'S', 0, 1 }, { "cmpm.l (a6)+,(a5)+ (1-2)", 'S', 0, 1 },
    { "tst.l d0 (1)", 'L', 1, 0 }, { "and.l d5,d0 (1&2)", 'L', 0, 0 },
    { "or.l d5,d0", 'L', 3, 0 }, { "eor.l d5,d0", 'L', 3, 0 },
    { "not.l d0", 'L', 0xfffffffeUL, 0 }, { "clr.l d0", 'L', 0, 0 },
    { "move.l d5,d0", 'L', 2, 0 }, { "moveq #-1,d0", 'L', 0xffffffffUL, 0 },
    { "ext.l d0", 'L', 1, 0 }, { "swap d0 (1)", 'L', 0x10000UL, 0 },
    { "mulu.w d5,d0 (1*2)", 'L', 2, 0 }, { "muls.w d5,d0 (1*2)", 'L', 2, 0 },
    { "divu.w d5,d0 (1/2: q=0)", 'L', 0, 0 }, { "divs.w d5,d0 (1/2: q=0)", 'L', 0, 0 },
    { "btst #0,d0 (1)", 'B', 0, 0 }, { "bset #1,d0 (1)", 'B', 1, 0 },
    { "bchg #1,d0 (1)", 'B', 1, 0 }, { "bclr #0,d0 (1)", 'B', 0, 0 },
    { "rol.l #1,d6 ($80000001)", 'R', 3, 1 }, { "ror.l #1,d6 ($80000001)", 'R', 0xc0000000UL, 1 },
    { "rol.l d5,d6 (2, $80000001)", 'R', 6, 0 }, { "cmp.b d5,d0 (1-2)", 'S', 0, 1 },
    { "cmp.w d5,d0 (1-2)", 'S', 0, 1 }, { "cmpa.w a4,a3 (1-2)", 'S', 0, 1 },
    { "addq.l #1,a3", 'A', 0, 0 }, { "subq.w #1,a3", 'A', 0, 0 },
    { "ext.w d0 (1)", 'L', 1, 0 }, { "move.w d5,d0", 'L', 2, 0 },
};
static const char *const nm_xc[] = { "sr", "x_by_roxl", "x_by_addx" };
static ULONG run_xc(void)
{
    ULONG i;
    fl_xc_table(wbuf, bbuf, bbuf + 512, xc_pre, 4);
    for (i = 0; i < 4 * FL_NXC; i++) {
        got[3 * i] = wbuf[i];
        got[3 * i + 1] = bbuf[i];
        got[3 * i + 2] = bbuf[512 + i];
    }
    return crc_got(3 * 4 * FL_NXC);
}
static void model_xc(ULONG i, ULONG *e)
{
    int p = xc_pre[i / FL_NXC], f;
    const struct xcdef *d = &xc_defs[i % FL_NXC];
    switch (d->type) {
    case 'S': f = (p & XF) | NF | CF; break;
    case 'L': f = (p & XF) | nz(d->r, 4); break;
    case 'B': f = (p & ~ZF) | (d->r ? ZF : 0); break;
    case 'R': f = (p & XF) | nz(d->r, 4) | (d->c ? CF : 0); break;
    default:  f = p; break;
    }
    e[0] = sr_hi | (ULONG)f;
    e[1] = e[2] = (p & XF) ? 1 : 0;
}
static void in_xc(ULONG i)
{
    in_ccr(xc_pre[i / FL_NXC]);
    json_str("insn", xc_defs[i % FL_NXC].name);
    json_str("then", "MOVE SR / ROXL.L #1,d7(=0) / ADDX.L d7,d7(=0)");
}

/* ------------------------------------------------------------ shifts and rotates */
/* 64 and 65 act like 0 and 1: a register count is taken modulo 64 (PRM ASd/LSd/ROd/ROXd) */
#define SH_NC 16
static const UBYTE sh_cnt[SH_NC] = { 0, 1, 2, 3, 7, 8, 9, 15, 16, 17, 31, 32, 33, 63, 64, 65 };
static const ULONG sh_val[2] = { 0x80008081UL, 0x40004040UL };
static const UBYTE sh_pre[2] = { 0x00, 0x1f };
/* immediate and memory forms (which 24..33): kind, size, count */
struct shdef { UBYTE k, sz, cnt; const char *name; };
static const struct shdef sh_imm[10] = {
    { K_ROXL, 4, 1, "roxl.l #1,d0" }, { K_ROXL, 4, 8, "roxl.l #8,d0" },
    { K_ROXR, 2, 3, "roxr.w #3,d0" }, { K_ROXR, 1, 8, "roxr.b #8,d0" },
    { K_ROXL, 1, 5, "roxl.b #5,d0" }, { K_ASL, 2, 8, "asl.w #8,d0" },
    { K_LSR, 4, 4, "lsr.l #4,d0" }, { K_ROXR, 4, 1, "roxr.l #1,d0" },
    { K_ROXL, 2, 1, "roxl.w (a1)" }, { K_ROXR, 2, 1, "roxr.w (a1)" },
};
#define SH_NREG (24 * SH_NC * 2 * 2)
#define SH_N (SH_NREG + 10 * 2 * 2)
struct shin { ULONG which, k, sz, cnt, val, pre; };
static void sh_decode(ULONG i, struct shin *s)
{
    if (i < SH_NREG) {
        s->which = i / (SH_NC * 4);
        s->k = s->which / 3;
        s->sz = 1UL << (s->which % 3);
        s->cnt = sh_cnt[(i / 4) % SH_NC];
    } else {
        ULONG j = (i - SH_NREG) / 4;
        s->which = 24 + j;
        s->k = sh_imm[j].k;
        s->sz = sh_imm[j].sz;
        s->cnt = sh_imm[j].cnt;
    }
    s->val = sh_val[(i / 2) % 2];
    s->pre = sh_pre[i % 2];
    if (s->which >= 32)
        s->val &= 0xffff;
}
static const char *const nm_shift[] = { "result", "sr" };
static ULONG run_shift(void)
{
    ULONG which, c, v, pr, *g = got;
    for (which = 0; which < 24; which++)
        for (c = 0; c < SH_NC; c++)
            for (v = 0; v < 2; v++)
                for (pr = 0; pr < 2; pr++) {
                    *g++ = fl_shift(sh_val[v], sh_cnt[c], sh_pre[pr], which);
                    *g++ = fl_shift_sr;
                }
    for (which = 24; which < 34; which++)
        for (v = 0; v < 2; v++)
            for (pr = 0; pr < 2; pr++) {
                ULONG val = which >= 32 ? sh_val[v] & 0xffff : sh_val[v];
                *g++ = fl_shift(val, sh_imm[which - 24].cnt, sh_pre[pr], which);
                *g++ = fl_shift_sr;
            }
    return crc_got(2 * SH_N);
}
static void model_shift(ULONG i, ULONG *e)
{
    struct shin s;
    ULONG rm, m;
    int f;
    sh_decode(i, &s);
    f = m_shift((int)s.k, (int)s.sz, s.val, (int)(s.cnt & 63), (int)s.pre, &rm);
    m = szmask((int)s.sz);
    e[0] = (s.val & ~m) | rm;
    e[1] = sr_hi | (ULONG)f;
}
static void in_shift(ULONG i)
{
    struct shin s;
    char buf[16], *p = buf;
    const char *k;
    sh_decode(i, &s);
    in_ccr(s.pre);
    if (s.which < 24) {
        for (k = k_names[s.k]; *k; )
            *p++ = *k++;
        *p++ = '.';
        *p++ = s.sz == 1 ? 'b' : s.sz == 2 ? 'w' : 'l';
        *p = 0;
        json_str("insn", buf);
        json_str("form", "d1,d0 (register count, mod 64)");
    } else
        json_str("insn", sh_imm[s.which - 24].name);
    json_int("count", (long)s.cnt);
    json_hex("val", s.val);
}

/* ------------------------------------------------------------ multi-precision X chains */
#define MP_NV 7
static const UBYTE mp_pre[5] = { 0x00, 0x04, 0x10, 0x14, 0x1f };
static ULONG mp_rand[4][8];
/* element k (0 = most significant) of vector v for dst (s=0) or src (s=1) */
static ULONG mp_elem(int v, int s, int k, int n)
{
    switch (v) {
    case 0: return s ? (k == n - 1 ? 1 : 0) : 0xffffffffUL;
    case 1: return 0;
    case 2: return mp_rand[s][k];
    case 3: return mp_rand[2 + s][k];
    case 4: return mp_rand[0][k];
    case 5: return k == 0 ? 0x80000000UL : 0;
    default: return s ? 0xffffffffUL : 0;
    }
}

enum { MP_ADDX, MP_SUBX, MP_NEGX, MP_ROXL, MP_ROXR, MP_ROXL_MOVE, MP_ROXR_MOVE };
struct mpcase {
    const char *id, *insn;
    int op, sz, n, irq;
    xmem_fn mem;                 /* NULL: register form (r4) */
    ULONG (*reg)(ULONG *, const ULONG *, ULONG);
};
static const struct mpcase *cur_mp;
static ULONG mp_dmeml[8], mp_smeml[8];
#define mp_dmem ((UBYTE *)mp_dmeml)
#define mp_smem ((UBYTE *)mp_smeml)
static ULONG mp_triggers;
static const char *const nm_mp[] = { "r0", "r1", "r2", "r3", "r4", "r5", "r6", "r7", "sr" };

static void put_elem(UBYTE *p, int sz, ULONG v)
{
    if (sz == 4) *(ULONG *)p = v;
    else if (sz == 2) *(UWORD *)p = (UWORD)v;
    else *p = (UBYTE)v;
}
static ULONG get_elem(const UBYTE *p, int sz)
{
    return sz == 4 ? *(const ULONG *)p : sz == 2 ? *(const UWORD *)p : *p;
}

static ULONG mp_vd[MP_NV][8], mp_vs[MP_NV][8];   /* packed element images */
static void prep_mp(const struct mpcase *c)
{
    int v, k;
    for (v = 0; v < MP_NV; v++)
        for (k = 0; k < c->n; k++) {
            put_elem((UBYTE *)mp_vd[v] + k * c->sz, c->sz, mp_elem(v, 0, k, c->n));
            put_elem((UBYTE *)mp_vs[v] + k * c->sz, c->sz, mp_elem(v, 1, k, c->n));
        }
}
static ULONG run_mp(void)
{
    const struct mpcase *c = cur_mp;
    ULONG w = (ULONG)c->n + 1, sr, *g = got, bytes = (ULONG)(c->n * c->sz);
    int v, pr, k;
    UBYTE *dend = mp_dmem + bytes, *send = mp_smem + bytes;
    for (v = 0; v < MP_NV; v++)
        for (pr = 0; pr < 5; pr++) {
            ULONG pre = mp_pre[pr] | (c->irq ? 0x10000UL : 0);
            for (k = 0; k < 8; k++) {
                mp_dmeml[k] = mp_vd[v][k];
                mp_smeml[k] = mp_vs[v][k];
            }
            if (!c->mem)
                sr = c->reg(mp_dmeml, mp_smeml, pre);
            else if (c->op == MP_ROXR || c->op == MP_ROXR_MOVE)
                sr = c->mem(0, mp_dmem, (ULONG)c->n, pre);
            else
                sr = c->mem(dend, send, (ULONG)c->n, pre);
            if (c->irq)
                mp_triggers++;
            for (k = 0; k < c->n; k++)
                g[k] = c->sz == 4 ? mp_dmeml[k] : get_elem(mp_dmem + k * c->sz, c->sz);
            g[c->n] = sr;
            g += w;
        }
    return crc_got(MP_NV * 5 * w);
}
static void model_mp(ULONG i, ULONG *e)
{
    const struct mpcase *c = cur_mp;
    int v = (int)(i / 5), n = c->n, sz = c->sz, k;
    int f = mp_pre[i % 5];
    ULONG rm, m = szmask(sz);
    if (c->op == MP_ROXR || c->op == MP_ROXR_MOVE) {
        for (k = 0; k < n; k++) {
            f = m_shift(K_ROXR, sz, mp_elem(v, 0, k, n), 1, f, &rm);
            if (c->op == MP_ROXR_MOVE)
                f = (f & XF) | nz(rm, sz);       /* MOVE.L keeps X, clears V/C */
            e[k] = rm;
        }
    } else {
        for (k = n - 1; k >= 0; k--) {
            ULONG d = mp_elem(v, 0, k, n) & m, s = mp_elem(v, 1, k, n) & m;
            int x = (f & XF) != 0;
            switch (c->op) {
            case MP_ADDX: f = m_add(s, d, x, sz, f, 1, &rm); break;
            case MP_SUBX: f = m_sub(s, d, x, sz, f, 1, &rm); break;
            case MP_NEGX: f = m_sub(d, 0, x, sz, f, 1, &rm); break;
            default:
                f = m_shift(K_ROXL, sz, d, 1, f, &rm);
                if (c->op == MP_ROXL_MOVE)
                    f = (f & XF) | nz(rm, sz);
                break;
            }
            e[k] = rm;
        }
    }
    e[n] = sr_hi | (ULONG)f;
}
static void in_mp(ULONG i)
{
    const struct mpcase *c = cur_mp;
    int v = (int)(i / 5), k;
    in_ccr(mp_pre[i % 5]);
    json_str("insn", c->insn);
    json_int("elems", c->n);
    json_int("size", c->sz);
    json_arr("dst");
    for (k = 0; k < c->n; k++)
        json_hexn(0, mp_elem(v, 0, k, c->n) & szmask(c->sz), c->sz * 2);
    json_arr_end();
    if (c->op <= MP_SUBX) {
        json_arr("src");
        for (k = 0; k < c->n; k++)
            json_hexn(0, mp_elem(v, 1, k, c->n) & szmask(c->sz), c->sz * 2);
        json_arr_end();
    }
    json_str("order", "element 0 most significant");
}

static const struct mpcase mp_cases[] = {
    { "addx_reg4", "addx.l Dy,Dx x4 unrolled", MP_ADDX, 4, 4, 0, 0, fl_addx_r4 },
    { "subx_reg4", "subx.l Dy,Dx x4 unrolled", MP_SUBX, 4, 4, 0, 0, fl_subx_r4 },
    { "negx_reg4", "negx.l Dx x4 unrolled", MP_NEGX, 4, 4, 0, 0, fl_negx_r4 },
    { "addx_mem_l8", "addx.l -(a1),-(a0) dbf loop", MP_ADDX, 4, 8, 0, fl_xmem_addx_l, 0 },
    { "addx_mem_w8", "addx.w -(a1),-(a0) dbf loop", MP_ADDX, 2, 8, 0, fl_xmem_addx_w, 0 },
    { "addx_mem_b8", "addx.b -(a1),-(a0) dbf loop", MP_ADDX, 1, 8, 0, fl_xmem_addx_b, 0 },
    { "subx_mem_l8", "subx.l -(a1),-(a0) dbf loop", MP_SUBX, 4, 8, 0, fl_xmem_subx_l, 0 },
    { "subx_mem_w8", "subx.w -(a1),-(a0) dbf loop", MP_SUBX, 2, 8, 0, fl_xmem_subx_w, 0 },
    { "subx_mem_b8", "subx.b -(a1),-(a0) dbf loop", MP_SUBX, 1, 8, 0, fl_xmem_subx_b, 0 },
    { "negx_mem_l8", "negx.l -(a0) dbf loop", MP_NEGX, 4, 8, 0, fl_xmem_negx_l, 0 },
    { "roxl_chain_w8", "roxl.w -(a0) dbf loop", MP_ROXL, 2, 8, 0, fl_xmem_roxlw, 0 },
    { "roxr_chain_w8", "roxr.w (a1)+ dbf loop", MP_ROXR, 2, 8, 0, fl_xmem_roxrw, 0 },
    { "roxl_chain_l8", "move.l -(a0),d0; roxl.l #1,d0; move.l d0,(a0) dbf loop", MP_ROXL_MOVE, 4, 8, 0, fl_xmem_roxll, 0 },
    { "roxr_chain_l8", "move.l (a1),d0; roxr.l #1,d0; move.l d0,(a1)+ dbf loop", MP_ROXR_MOVE, 4, 8, 0, fl_xmem_roxrl, 0 },
    { "addx_mem_l8_irq", "addx.l -(a1),-(a0) dbf loop, SOFTINT raised just before", MP_ADDX, 4, 8, 1, fl_xmem_addx_l, 0 },
};

/* ------------------------------------------------------------ TRAP / RTE */
static const char *const nm_trap[] = { "sr_after_trap1", "sr_after_trap2", "sr_in_trap3", "sr_after_trapv" };
static ULONG run_trap(void)
{
    ULONG i;
    fl_trapv_count = 0;
    fl_trap_table(wbuf);
    for (i = 0; i < 128; i++)
        got[i] = wbuf[i];
    got[128] = fl_trapv_count;
    got[129] = got[130] = got[131] = 0;
    return crc_got(132);
}
/* PRM/UM exception processing: the SR is copied, S set, T cleared; the CCR
 * is unchanged on entry and RTE restores the stacked SR. TRAPV traps only
 * when V is set (vector 7). */
static void model_trap(ULONG i, ULONG *e)
{
    if (i == 32) {
        e[0] = 16;
        e[1] = e[2] = e[3] = 0;
        return;
    }
    e[0] = sr_hi | i;
    e[1] = sr_hi | (i ^ 0x1f);
    e[2] = sr_hi | i;
    e[3] = sr_hi | i;
}
static void in_trap(ULONG i)
{
    if (i == 32) {
        json_str("what", "TRAPV taken count over ccr 0..31 (V set in 16)");
        return;
    }
    in_ccr(i);
    json_str("trap1", "handler EORI #$1F,CCR; RTE");
    json_str("trap2", "handler EORI.W #$1F,(SP); RTE (stacked SR)");
    json_str("trap3", "handler MOVE SR,D5; RTE");
    json_str("trapv", "handler ADDQ.L #1,mem; RTE");
}

/* ------------------------------------------------------------ flags across gaps */
#define FG_NOPS 23
#define FG_NPAIRS 12
#define FG_NPRE 4
#define FG_N (FG_NOPS * FG_NPAIRS * FG_NPRE)
static const char *const fg_ops[FG_NOPS] = {
    "(none)", "add.l d1,d0", "add.w d1,d0", "add.b d1,d0", "sub.l d1,d0", "sub.w d1,d0",
    "sub.b d1,d0", "cmp.l d1,d0", "cmp.b d1,d0", "neg.l d0", "neg.w d0", "addx.l d1,d0",
    "subx.l d1,d0", "negx.l d0", "and.l d1,d0", "or.w d1,d0", "lsl.l #1,d0", "asr.l #1,d0",
    "asl.l #1,d0", "roxl.l #1,d0", "roxr.l #1,d0", "mulu.w d1,d0", "tst.l d0"
};
static const char *const fg_gaps[7] = {
    "none", "nonflag", "branch", "bsr", "dbcc", "trap", "irq"
};
static const char *const fg_gapdesc[7] = {
    "consumers directly after the op",
    "movea/lea/adda/suba/addq An/subq An/exg/movem/pea/link/unlk/nop/move usp/Scc between",
    "Bhi.s (taken or not), Bmi.w (taken or not), Bvs/Bcs/Beq/Bge chain between",
    "bsr.w and jsr to subroutines (lea/exg/movea/addq An) between",
    "dbf loop (4 passes) and a dbeq loop (exits at once if Z, else 8 passes) between",
    "two TRAP #1 (handler EORI #$1F,CCR; RTE) between",
    "SOFTINT raised (INTREQ via MOVEM), then a 48-pass dbf loop: the level 1 interrupt hits between"
};
static const UBYTE fg_pre[FG_NPRE] = { 0x00, 0x1f, 0x14, 0x0a };
static ULONG fg_a[FG_NPAIRS] = { 0, 1, 0xffffffffUL, 0x7fffffffUL, 0x80000000UL, 0x80000000UL,
                                 0x00007fffUL, 0x000000ffUL, 0x0000ffffUL, 0x12345678UL, 0, 0 };
static ULONG fg_b[FG_NPAIRS] = { 0, 1, 1, 1, 0x80000000UL, 1, 1, 1, 0x00010001UL, 0x9abcdef0UL, 0, 0 };
static int fg_gap;
static ULONG fg_calls;
static ULONG fg_outl[5];
#define fg_out ((UBYTE *)fg_outl)
static const char *const nm_fg[] = { "ccr_x_d6", "scc_mask", "d0" };

static ULONG run_fg(void)
{
    ULONG op, p, pr, j, mask, *g = got;
    fg_fn *tab = &fg_tab[fg_gap * FG_NOPS];
    for (op = 0; op < FG_NOPS; op++)
        for (p = 0; p < FG_NPAIRS; p++)
            for (pr = 0; pr < FG_NPRE; pr++) {
                *g++ = tab[op](fg_a[p], fg_b[p], fg_pre[pr], fg_out);
                mask = 0;
                for (j = 0; j < 16; j++) {
                    if (fg_out[j] == 0xff)
                        mask |= 1UL << j;
                    else if (fg_out[j] != 0)
                        mask |= 0x10000UL << j;
                }
                *g++ = mask;
                *g++ = fg_outl[4];
            }
    fg_calls += FG_N;
    return crc_got(3 * FG_N);
}
static int fg_model(int op, ULONG a, ULONG b, int f, ULONG *res)
{
    ULONG rm;
    int x = (f & XF) != 0, nf;
    switch (op) {
    case 0: *res = a; return f;
    case 1: nf = m_add(b, a, 0, 4, f, 0, &rm); *res = rm; return nf;
    case 2: nf = m_add(b, a, 0, 2, f, 0, &rm); *res = (a & 0xffff0000UL) | rm; return nf;
    case 3: nf = m_add(b, a, 0, 1, f, 0, &rm); *res = (a & 0xffffff00UL) | rm; return nf;
    case 4: nf = m_sub(b, a, 0, 4, f, 0, &rm); *res = rm; return nf;
    case 5: nf = m_sub(b, a, 0, 2, f, 0, &rm); *res = (a & 0xffff0000UL) | rm; return nf;
    case 6: nf = m_sub(b, a, 0, 1, f, 0, &rm); *res = (a & 0xffffff00UL) | rm; return nf;
    case 7: nf = m_sub(b, a, 0, 4, f, 0, &rm); *res = a; return (nf & ~XF) | (f & XF);
    case 8: nf = m_sub(b, a, 0, 1, f, 0, &rm); *res = a; return (nf & ~XF) | (f & XF);
    case 9: nf = m_sub(a, 0, 0, 4, f, 0, &rm); *res = rm; return nf;
    case 10: nf = m_sub(a, 0, 0, 2, f, 0, &rm); *res = (a & 0xffff0000UL) | rm; return nf;
    case 11: nf = m_add(b, a, x, 4, f, 1, &rm); *res = rm; return nf;
    case 12: nf = m_sub(b, a, x, 4, f, 1, &rm); *res = rm; return nf;
    case 13: nf = m_sub(a, 0, x, 4, f, 1, &rm); *res = rm; return nf;
    case 14: *res = a & b; return (f & XF) | nz(a & b, 4);
    case 15: rm = (a | b) & 0xffff; *res = (a & 0xffff0000UL) | rm; return (f & XF) | nz(rm, 2);
    case 16: nf = m_shift(K_LSL, 4, a, 1, f, &rm); *res = rm; return nf;
    case 17: nf = m_shift(K_ASR, 4, a, 1, f, &rm); *res = rm; return nf;
    case 18: nf = m_shift(K_ASL, 4, a, 1, f, &rm); *res = rm; return nf;
    case 19: nf = m_shift(K_ROXL, 4, a, 1, f, &rm); *res = rm; return nf;
    case 20: nf = m_shift(K_ROXR, 4, a, 1, f, &rm); *res = rm; return nf;
    case 21: rm = (a & 0xffff) * (b & 0xffff); *res = rm; return (f & XF) | nz(rm, 4);
    default: *res = a; return (f & XF) | nz(a, 4);
    }
}
static void model_fg(ULONG i, ULONG *e)
{
    ULONG op = i / (FG_NPAIRS * FG_NPRE), r = i % (FG_NPAIRS * FG_NPRE);
    ULONG p = r / FG_NPRE, pre = fg_pre[r % FG_NPRE], res, d6, mask = 0, j;
    int f = fg_model((int)op, fg_a[p], fg_b[p], (int)pre, &res);
    d6 = (fg_gap == 4 && !(f & ZF)) ? 0xff : 7;
    e[0] = (ULONG)(f & 0x1f) | (ULONG)(f & XF ? 0x100 : 0) | (d6 << 16);
    for (j = 0; j < 16; j++)
        if (cond_true((int)j, f))
            mask |= 1UL << j;
    e[1] = mask;
    e[2] = res;
}
static void in_fg(ULONG i)
{
    ULONG op = i / (FG_NPAIRS * FG_NPRE), r = i % (FG_NPAIRS * FG_NPRE);
    ULONG p = r / FG_NPRE;
    in_ccr(fg_pre[r % FG_NPRE]);
    json_str("op", fg_ops[op]);
    json_hex("d0", fg_a[p]);
    json_hex("d1", fg_b[p]);
    json_str("gap", fg_gapdesc[fg_gap]);
    json_str("layout", "ccr_x_d6 = SR&$1F | X(by ADDX)<<8 | d6.b<<16; scc_mask bit n = Scc cond n");
}

/* ------------------------------------------------------------ interrupt hook */
static volatile ULONG irq_count;
/* Called from the level 1 handler (and the level 3 one, see irq_on). The
 * re-read and clear run at IPL 7 so a VERTB preempting the level 1 handler
 * cannot acknowledge the same request a second time. */
static void soft_hook(ULONG req)
{
    UWORD sr;
    (void)req;
    sr = amt_get_sr();
    amt_set_sr(sr | 0x0700);
    if (CREG(R_INTREQR) & 0x0004) {
        CREG(R_INTREQ) = 0x0004;
        CREG(R_INTREQ) = 0x0004;
        irq_count++;
    }
    amt_set_sr(sr);
}
static void irq_on(void)
{
    irq_count = 0;
    amt_irq_hook[1] = soft_hook;
    /* the runtime's level 3 handler acknowledges every other pending bit
     * as spurious when no level 3 hook is set (bare.s isr_common), which
     * would swallow a SOFTINT that is pending while VERTB is serviced */
    amt_irq_hook[3] = soft_hook;
    CREG(R_INTREQ) = 0x0004;
    CREG(R_INTENA) = 0x8004;
}
static void irq_off(void)
{
    CREG(R_INTENA) = 0x0004;
    CREG(R_INTREQ) = 0x0004;
    CREG(R_INTREQ) = 0x0004;
    amt_irq_hook[1] = 0;
    amt_irq_hook[3] = 0;
}

/* ------------------------------------------------------------ body */
#define TC(id, why, n, w, names, pfx) \
    { id, why, n, w, names, run_##pfx, model_##pfx, in_##pfx }

void amt_run_flags(void)
{
    int i, k;
    struct tcase t;
    amt_vec_fn o1, o2, o3, o4;

    /* the contract value (CONTRACT section 10: the bare phase runs at SR =
     * $2000), not a read-back: a wrong upper SR byte must not agree with
     * itself in every expectation below */
    sr_hi = 0x2000;
    amt_check32("sr_upper_bare", "sr_hi", 0x2000, amt_get_sr() & 0xff00,
                "MOVE from SR in the bare phase must read S=1, T1=T0=M=0, I=0 and the reserved "
                "bits 0 ($2000: CONTRACT section 10; PRM 1.3.2 status register)");
    for (i = 0; i < 32; i++)
        mccr_src[i] = (UWORD)(((amt_rand() & 0xff) << 8) | (amt_rand() & 0xff));
    for (k = 0; k < 4; k++)
        for (i = 0; i < 8; i++)
            mp_rand[k][i] = amt_rand();
    fg_a[10] = amt_rand(); fg_b[10] = amt_rand();
    fg_a[11] = amt_rand(); fg_b[11] = amt_rand();

    {
        static const struct tcase tcs[] = {
            TC("scc_all", "Scc result (or SR after Scc) wrong for this condition and CCR (PRM Table 3-19 conditions; Scc does not affect the CCR)", 512, 2, nm_tsr, scc),
            TC("dbcc_all", "DBcc wrong: if cc is true no operation, else Dn.w-1 and branch unless -1; upper word and CCR unchanged (PRM DBcc)", 1024, 3, nm_tsrc, dbcc),
            TC("ccr_imm_ops", "ORI/ANDI/EORI to CCR result wrong: only bits 4-0 exist, others read 0 (PRM ORI/ANDI/EORI to CCR)", 1152, 1, nm_sr, ccrimm),
            TC("move_to_ccr", "MOVE to CCR: low byte of source to CCR (bits 7-5 zero), upper byte ignored, SR upper byte unchanged (PRM MOVE to CCR)", 292, 1, nm_sr, moveccr),
            TC("move_sr_reserved", "MOVE to/from SR: reserved SR bits 11 and 7-5 must read as 0 (PRM 1.3.2 status register; MOVE to/from SR)", 32, 2, nm_movesr, movesr),
            TC("preserve_nonflag", "instruction changed the CCR although the PRM says it does not (or SWAP set N/Z/V/C wrong)", 32 * FL_NPRES, 1, nm_sr, preserve),
            TC("x_vs_c", "an instruction that sets C (or none) changed X, or N/Z/V/C wrong; X must be unaffected and visible to ROXL/ADDX (PRM per-instruction condition codes)", 4 * FL_NXC, 3, nm_xc, xc),
            TC("shift_rotate_xc", "shift/rotate result or flags wrong: ROXd count 0 sets C=X, ROd never touches X, LSd/ASd set X=C, count 0 clears C, ASL V if MSB ever changes (PRM ASL/LSL/ROL/ROXL)", SH_N, 2, nm_shift, shift),
            TC("trap_rte_ccr", "CCR not preserved into a TRAP handler / restored by RTE, stacked SR change not applied, or TRAPV wrong (PRM 6.3 exception processing, RTE, TRAPV)", 33, 4, nm_trap, trap),
        };
        for (i = 0; i < (int)(sizeof(tcs) / sizeof(tcs[0])); i++) {
            if (i == 8) {
                o1 = amt_vec_set(33, fl_trap1_h);
                o2 = amt_vec_set(34, fl_trap2_h);
                o3 = amt_vec_set(35, fl_trap3_h);
                o4 = amt_vec_set(7, fl_trapv_h);
                run_tcase(&tcs[i]);
                amt_vec_set(33, o1);
                amt_vec_set(34, o2);
                amt_vec_set(35, o3);
                amt_vec_set(7, o4);
                continue;
            }
            run_tcase(&tcs[i]);
            if (i == 0) {
                t = (struct tcase)TC("bcc_all_short", "Bcc.s taken/not taken wrong for this condition and CCR, or a Bcc changed the CCR (PRM Bcc)", 512, 2, nm_tsr, bcc);
                bcc_wide = 0;
                run_tcase(&t);
                t.id = "bcc_all_word";
                t.why = "Bcc.w taken/not taken wrong for this condition and CCR, or a Bcc changed the CCR (PRM Bcc)";
                bcc_wide = 1;
                run_tcase(&t);
            }
        }
    }

    /* MOVE from CCR: 68010 and later only (PRM: not on the 68000) */
    if (AMT_CPU() >= AMT_CPU_68010) {
        t = (struct tcase)TC("move_from_ccr", "MOVE from CCR must give the CCR zero-extended (upper byte 0) and leave it unchanged (PRM MOVE from CCR, 68010+)", 32, 3, nm_mfccr, mfccr);
        run_tcase(&t);
    } else
        amt_case("move_from_ccr", AMT_ST_SKIP, "MOVE from CCR does not exist on the 68000");

    /* multi-precision X chains */
    for (i = 0; i < (int)(sizeof(mp_cases) / sizeof(mp_cases[0])); i++) {
        cur_mp = &mp_cases[i];
        t.id = cur_mp->id;
        t.why = "multi-precision chain wrong: X carries between elements, ADDX/SUBX/NEGX clear Z if nonzero and never set it, ROXd shifts X in/out (PRM ADDX/SUBX/NEGX/ROXd)";
        t.n = MP_NV * 5;
        t.w = (ULONG)cur_mp->n + 1;
        t.names = nm_mp;
        t.run = run_mp;
        t.model = model_mp;
        t.in = in_mp;
        prep_mp(cur_mp);
        if (cur_mp->n == 4) {
            static const char *const nm4[] = { "r0", "r1", "r2", "r3", "sr" };
            t.names = nm4;
        }
        if (cur_mp->irq) {
            mp_triggers = 0;
            irq_on();
            run_tcase(&t);
            /* the last interrupt is long delivered by now */
            k = 0;
            while (irq_count != mp_triggers && ++k < 10000)
                ;
            irq_off();
            amt_check32("addx_mem_l8_irq_delivered", "softints", mp_triggers, irq_count,
                        "each raised SOFTINT (INTREQ $8004, INTENA on, SR mask 0) must give exactly one level 1 interrupt (HRM interrupts)");
        } else
            run_tcase(&t);
    }

    /* flags across gaps */
    {
        static char ids[7][20];
        for (i = 0; i < 7; i++) {
            char *p = ids[i];
            const char *s = "across_", *g = fg_gaps[i];
            while (*s) *p++ = *s++;
            while (*g) *p++ = *g++;
            *p = 0;
            fg_gap = i;
            t.id = ids[i];
            t.why = "flags produced by the op were not the ones seen after the gap (Scc/MOVE SR/ADDX X), or the op result is wrong (PRM per-instruction condition codes; the gap instructions affect no flags)";
            t.n = FG_N;
            t.w = 3;
            t.names = nm_fg;
            t.run = run_fg;
            t.model = model_fg;
            t.in = in_fg;
            if (i == 5) {
                o1 = amt_vec_set(33, fl_trap1_h);
                run_tcase(&t);
                amt_vec_set(33, o1);
            } else if (i == 6) {
                fg_calls = 0;
                irq_on();
                run_tcase(&t);
                k = 0;
                while (irq_count != fg_calls && ++k < 10000)
                    ;
                irq_off();
                amt_check32("across_irq_delivered", "softints", fg_calls, irq_count,
                            "each raised SOFTINT (INTREQ $8004, INTENA on, SR mask 0) must give exactly one level 1 interrupt (HRM interrupts)");
            } else
                run_tcase(&t);
        }
    }
}
