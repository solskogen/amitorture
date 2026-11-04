/* cpu_basic (module 2): deterministic pass over the 680x0 integer ISA.
 *
 * The test material is generated on the host by tools/gen_cpu_basic.py
 * (a reference model of the instructions written from the M68000 PRM and
 * the 68020/030/040/060 user manuals) into cpu_basic_tab.s: one code
 * snippet per instruction form ("template") and its test vectors, i.e. the
 * input registers/CCR/memory and the expected outputs with the mask of the
 * CCR bits the manual defines for that input.
 *
 * For every vector we set a background state (all of d0-d7/a0-a6, the CCR,
 * a memory window in the any-pool), apply the inputs, run the snippet
 * through cb_exec (cpu_basic_exec.s) and compare all 15 registers, the
 * masked CCR, the window bytes and the stack pointer with the expectation.
 *
 * A case is one instruction family. It runs every vector once (verification
 * pass), then amt_hot() runs vector (i*7919 mod n) for i = 0..hot_reps() and
 * returns 0 for a matching vector, so a divergence after the snippets have
 * been executed many times (JIT-compiled) is reported with its iteration.
 */
#include "mod.h"

AMT_MODULE(cpu_basic, 2, AMT_PH_BARE, 0, 0, 4096);

struct cb_tpl {
    const UWORD *entry;
    const UWORD *op;
    UWORD opwords;
    UWORD nvec;
    const char *text;
    const UBYTE *anchor;
    const UWORD *slots;
    const UWORD *vec;
    UWORD cpumask;
    UBYTE nin, nout;
    WORD win_off;
    UWORD win_len;
    UWORD pad[2];
};

struct cb_case {
    const char *id;
    const char *rule;
    UWORD first, n;
};

/* cpu_basic_tab.s (generated) */
extern const struct cb_case cb_cases[];
extern const UWORD cb_ncases;
extern const struct cb_tpl cb_tpls[];
extern const UWORD cb_ntpls;
extern const ULONG cb_pool[];

/* cpu_basic_exec.s */
void cb_exec(void);
extern ULONG cb_in[15], cb_out[15];
extern UWORD cb_ccr, cb_osr;
extern const UWORD *cb_entry;
extern ULONG cb_isp, cb_osp;

#define WIN_MAX 256
#define M_OFFSET 1024           /* M = any pool + 1024; slots address M-1024..M+1023 */
#define NSAVE 3

static UBYTE *M;

/* background register values: data registers arbitrary, address registers
 * point into the any pool (outside every window) so a stray access stays in
 * our memory */
static const ULONG bg_d[8] = {
    0xD0D0D0D0UL, 0xD1D1D1D1UL, 0xD2D2D2D2UL, 0xD3D3D3D3UL,
    0xD4D4D4D4UL, 0xD5D5D5D5UL, 0xD6D6D6D6UL, 0xD7D7D7D7UL
};

static ULONG exp_r[15];
static UBYTE exp_ccr, exp_mask;
static UBYTE exp_w[WIN_MAX];

struct cb_fail {
    const struct cb_tpl *t;
    UWORD vi;
    UBYTE hot;
    UBYTE exp_ccr, exp_mask, got_ccr;
    ULONG iter;
    ULONG exp_r[15], got_r[15];
    ULONG sp_diff;
    UBYTE exp_w[WIN_MAX], got_w[WIN_MAX];
};
static struct cb_fail saved[NSAVE];
static int nsaved;
static int in_hot;
static ULONG hot_iter;

/* the snippet being run, for the report when it takes an exception */
static const struct cb_tpl *volatile cur_t;
static volatile UWORD cur_vi;

static int cpu_ok(const struct cb_tpl *t)
{
    return (t->cpumask >> AMT_CPU()) & 1;
}

static UBYTE bgbyte(int off)
{
    return (UBYTE)(off * 0x3B + 0x5A);
}

static int slot_size(UWORD code)
{
    return 1 << ((code >> 11) & 3);
}

static int slot_off(UWORD code)
{
    int o = code & 0x7ff;
    if (o & 0x400)
        o -= 0x800;
    return o;
}

static ULONG relv(const struct cb_tpl *t, UWORD code, ULONG v)
{
    if (code & 0x4000)
        v += (ULONG)M;
    else if (code & 0x2000)
        v += (ULONG)t->anchor;
    return v;
}

static void put_be(UBYTE *p, int size, ULONG v)
{
    while (size--) {
        p[size] = (UBYTE)v;
        v >>= 8;
    }
}

static ULONG get_be(const UBYTE *p, int size)
{
    ULONG v = 0;
    while (size--)
        v = (v << 8) | *p++;
    return v;
}

/* run vector vi of t; returns 0 if every output matches */
static int run_vec(const struct cb_tpl *t, UWORD vi)
{
    const UWORD *sl = t->slots;
    const UWORD *vv = t->vec + (ULONG)vi * (t->nin + t->nout);
    UBYTE *w = M + t->win_off;
    int wl = t->win_len;
    int i, bad = 0;
    ULONG spd;

    for (i = 0; i < 8; i++)
        cb_in[i] = bg_d[i];
    for (i = 0; i < 7; i++)
        cb_in[8 + i] = (ULONG)M + 0x700 + 0x10 * i;
    cb_ccr = 0;
    for (i = 0; i < wl; i++)
        w[i] = bgbyte(t->win_off + i);
    for (i = 0; i < t->nin; i++) {
        UWORD c = sl[i];
        ULONG v = relv(t, c, cb_pool[vv[i]]);
        if (c & 0x8000)
            put_be(M + slot_off(c), slot_size(c), v);
        else if ((c & 15) == 15)
            cb_ccr = (UWORD)(v & 0x1f);
        else
            cb_in[c & 15] = v;
    }
    for (i = 0; i < 15; i++)
        exp_r[i] = cb_in[i];
    exp_ccr = (UBYTE)cb_ccr;
    exp_mask = 0x1f;
    for (i = 0; i < wl; i++)
        exp_w[i] = w[i];
    for (i = 0; i < t->nout; i++) {
        UWORD c = sl[t->nin + i];
        ULONG v = cb_pool[vv[t->nin + i]];
        if (c & 0x8000) {
            put_be(exp_w + (slot_off(c) - t->win_off), slot_size(c), relv(t, c, v));
        } else if ((c & 15) == 15) {
            exp_mask = (UBYTE)(v >> 8);
            exp_ccr = (UBYTE)(v & 0x1f);
        } else {
            exp_r[c & 15] = relv(t, c, v);
        }
    }

    cb_entry = t->entry;
    cur_t = t;
    cur_vi = vi;
    cb_exec();

    for (i = 0; i < 15; i++)
        if (cb_out[i] != exp_r[i])
            bad = 1;
    if ((cb_osr ^ exp_ccr) & exp_mask)
        bad = 1;
    for (i = 0; i < wl; i++)
        if (w[i] != exp_w[i])
            bad = 1;
    spd = cb_osp - cb_isp;
    if (spd)
        bad = 1;
    if (bad && nsaved < NSAVE) {
        struct cb_fail *f = &saved[nsaved++];
        f->t = t;
        f->vi = vi;
        f->hot = (UBYTE)in_hot;
        f->iter = hot_iter;
        f->exp_ccr = exp_ccr;
        f->exp_mask = exp_mask;
        f->got_ccr = (UBYTE)(cb_osr & 0x1f);
        f->sp_diff = spd;
        for (i = 0; i < 15; i++) {
            f->exp_r[i] = exp_r[i];
            f->got_r[i] = cb_out[i];
        }
        for (i = 0; i < wl; i++) {
            f->exp_w[i] = exp_w[i];
            f->got_w[i] = w[i];
        }
    }
    return bad;
}

/* ------------------------------------------------------------ reporting */
static char sbuf[640];
static int slen;

static void sb_reset(void) { slen = 0; sbuf[0] = 0; }

static void sb_s(const char *s)
{
    while (*s && slen < (int)sizeof(sbuf) - 1)
        sbuf[slen++] = *s++;
    sbuf[slen] = 0;
}

static void sb_hex(ULONG v, int digits)
{
    static const char hx[] = "0123456789abcdef";
    while (digits--)
        if (slen < (int)sizeof(sbuf) - 1)
            sbuf[slen++] = hx[(v >> (digits * 4)) & 15];
    sbuf[slen] = 0;
}

static void sb_int(long v)
{
    char n[12];
    if (v < 0) {
        sb_s("-");
        v = -v;
    }
    amt_utoa(n, (ULONG)v);
    sb_s(n);
}

static const char *const rnames[15] = {
    "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
    "a0", "a1", "a2", "a3", "a4", "a5", "a6"
};

/* key for a slot: d0, a1@m, ccr, m+8.l, m-4.l@anchor */
static char kbuf[24];
static const char *slot_key(UWORD c)
{
    char *p = kbuf;
    const char *s;
    if (c & 0x8000) {
        int o = slot_off(c);
        char n[12];
        *p++ = 'm';
        *p++ = o < 0 ? '-' : '+';
        amt_utoa(n, (ULONG)(o < 0 ? -o : o));
        for (s = n; *s; )
            *p++ = *s++;
        *p++ = '.';
        *p++ = "bwl?"[(c >> 11) & 3];
    } else if ((c & 15) == 15) {
        s = "ccr";
        while (*s)
            *p++ = *s++;
    } else {
        s = rnames[c & 15];
        while (*s)
            *p++ = *s++;
    }
    s = (c & 0x4000) ? "@m" : (c & 0x2000) ? "@anchor" : "";
    while (*s)
        *p++ = *s++;
    *p = 0;
    return kbuf;
}

static ULONG unrel(const struct cb_tpl *t, UWORD c, ULONG v)
{
    if (c & 0x4000)
        v -= (ULONG)M;
    else if (c & 0x2000)
        v -= (ULONG)t->anchor;
    return v;
}

static int slot_digits(UWORD c)
{
    if (c & 0x8000)
        return slot_size(c) * 2;
    return 8;
}

static void emit_op(const struct cb_tpl *t)
{
    int i, n = t->opwords > 16 ? 16 : t->opwords;
    sb_reset();
    for (i = 0; i < n; i++) {
        if (i)
            sb_s(" ");
        sb_hex(t->op[i], 4);
    }
    if (t->opwords > n)
        sb_s(" ...");
    json_str("op", sbuf);
}

static void emit_inputs(const struct cb_tpl *t, UWORD vi)
{
    const UWORD *vv = t->vec + (ULONG)vi * (t->nin + t->nout);
    int i;
    json_obj("in");
    for (i = 0; i < t->nin; i++) {
        UWORD c = t->slots[i];
        if ((c & 0x8000) == 0 && (c & 15) == 15)
            json_hexn("ccr", cb_pool[vv[i]] & 0x1f, 2);
        else
            json_hexn(slot_key(c), cb_pool[vv[i]], slot_digits(c));
    }
    json_obj_end();
}

/* is register r an output slot of t? */
static int is_out_reg(const struct cb_tpl *t, int r)
{
    int i;
    for (i = 0; i < t->nout; i++) {
        UWORD c = t->slots[t->nin + i];
        if (!(c & 0x8000) && (c & 15) == r)
            return 1;
    }
    return 0;
}

/* expect (which=0) or got (1): every output slot, plus registers and window
 * bytes that differ although they are not outputs */
static void emit_state(const struct cb_fail *f, int which)
{
    const struct cb_tpl *t = f->t;
    const ULONG *r = which ? f->got_r : f->exp_r;
    const UBYTE *w = which ? f->got_w : f->exp_w;
    int i;
    json_obj(which ? "got" : "expect");
    for (i = 0; i < t->nout; i++) {
        UWORD c = t->slots[t->nin + i];
        if (c & 0x8000) {
            int sz = slot_size(c);
            ULONG v = get_be(w + slot_off(c) - t->win_off, sz);
            json_hexn(slot_key(c), sz == 4 ? unrel(t, c, v) : v, sz * 2);
        } else if ((c & 15) == 15) {
            json_hexn("ccr", which ? (f->got_ccr & f->exp_mask) : f->exp_ccr, 2);
        } else {
            json_hex(slot_key(c), unrel(t, c, r[c & 15]));
        }
    }
    for (i = 0; i < 15; i++)
        if (f->exp_r[i] != f->got_r[i] && !is_out_reg(t, i)) {
            if (i >= 8) {
                /* background address registers point at M+$700+$10*n:
                 * shown relative to M so records don't depend on layout */
                sb_reset();
                sb_s(rnames[i]);
                sb_s("@m");
                json_hex(sbuf, r[i] - (ULONG)M);
            } else
                json_hex(rnames[i], r[i]);
        }
    for (i = 0; i < t->win_len; i++)
        if (f->exp_w[i] != f->got_w[i]) {
            /* first differing window byte not already shown as a slot */
            sb_reset();
            sb_s("m");
            sb_s(t->win_off + i < 0 ? "-" : "+");
            sb_int(t->win_off + i < 0 ? -(t->win_off + i) : t->win_off + i);
            sb_s(".b(first diff)");
            json_hexn(sbuf, w[i], 2);
            break;
        }
    if (which && f->sp_diff)
        json_hex("sp_change", f->sp_diff);
    json_obj_end();
}

/* why: "<what differs> (<insn>)" */
static void why_diff(const struct cb_fail *f)
{
    const struct cb_tpl *t = f->t;
    int i, n = 0;
    for (i = 0; i < 15; i++)
        if (f->exp_r[i] != f->got_r[i]) {
            sb_s(n++ ? "," : "");
            sb_s(rnames[i]);
        }
    if ((f->exp_ccr ^ f->got_ccr) & f->exp_mask) {
        sb_s(n++ ? "," : "");
        sb_s("ccr(");
        if ((f->exp_ccr ^ f->got_ccr) & f->exp_mask & 0x10) sb_s("X");
        if ((f->exp_ccr ^ f->got_ccr) & f->exp_mask & 0x08) sb_s("N");
        if ((f->exp_ccr ^ f->got_ccr) & f->exp_mask & 0x04) sb_s("Z");
        if ((f->exp_ccr ^ f->got_ccr) & f->exp_mask & 0x02) sb_s("V");
        if ((f->exp_ccr ^ f->got_ccr) & f->exp_mask & 0x01) sb_s("C");
        sb_s(")");
    }
    for (i = 0; i < t->win_len; i++)
        if (f->exp_w[i] != f->got_w[i]) {
            sb_s(n++ ? "," : "");
            sb_s("memory");
            break;
        }
    if (f->sp_diff) {
        sb_s(n++ ? "," : "");
        sb_s("sp");
    }
    sb_s(" wrong after ");
    sb_s(t->text);
}

static void emit_fail_detail(const struct cb_fail *f)
{
    json_str("insn", f->t->text);
    emit_op(f->t);
    json_int("vec", f->vi);
    json_hexn("ccr_mask", f->exp_mask, 2);
    if (f->hot)
        json_int("hot_iter", (long)f->iter);
    emit_inputs(f->t, f->vi);
    emit_state(f, 0);
    emit_state(f, 1);
}

/* ------------------------------------------------------------ cases */
static const struct cb_case *ccase;
static ULONG ccase_total;
static int rmw_off;     /* this case's memory RMW forms are not run here */

/* a form with a memory operand (its text has an effective address in
 * parentheses); TAS Dn does no bus read-modify-write */
static int has_mem(const struct cb_tpl *t)
{
    const char *p;
    for (p = t->text; *p; p++)
        if (*p == '(')
            return 1;
    return 0;
}

static int form_ok(const struct cb_tpl *t)
{
    return cpu_ok(t) && !(rmw_off && has_mem(t));
}

/* the test pool is on the chip bus: chip RAM, or A500-style slow RAM at
 * $C00000-$DBFFFF, which is also accessed through Agnus' chip bus
 * arbitration (HRM: no indivisible read-modify-write cycle on that bus) */
static int pool_on_chip_bus(void)
{
    ULONG p = (ULONG)amt_any_pool();
    if (amt_any_pool_is_chip())
        return 1;
    return p >= 0x00c00000UL && p < 0x00dc0000UL;
}

static int vec_at(ULONG k, const struct cb_tpl **tp, UWORD *vi)
{
    int j;
    for (j = 0; j < ccase->n; j++) {
        const struct cb_tpl *t = &cb_tpls[ccase->first + j];
        if (!form_ok(t))
            continue;
        if (k < t->nvec) {
            *tp = t;
            *vi = (UWORD)k;
            return 1;
        }
        k -= t->nvec;
    }
    return 0;
}

/* Here the hot pass is coverage, not repetition: every vector of the case
 * once more after its snippet is warm (JIT-translated), up to 1000 vectors
 * as before, plus a margin past the translation threshold. A small case no
 * longer runs 1000 iterations over the same few vectors. */
static ULONG hot_reps(void)
{
    ULONG n = ccase_total < 1000UL ? ccase_total : 1000UL;
    ULONG r = n + 16;
    if (AMT_QUICK() || r < amt_hot_reps())
        r = amt_hot_reps();
    return r;
}

static ULONG hot_fn(ULONG i)
{
    const struct cb_tpl *t;
    UWORD vi;
    /* stride through the vectors (7919 is prime, so every vector is
     * reachable): with only amt_hot_reps() iterations, "i mod n" would never
     * hot-check the vectors past the first reps of a large case */
    ULONG k = (ccase_total % 7919UL) ? (i * 7919UL) % ccase_total : i % ccase_total;
    hot_iter = i;
    if (!vec_at(k, &t, &vi))
        return 0xffffffffUL;
    return run_vec(t, vi) ? k + 1 : 0;
}

static const char *const cpu_names[12] = {
    "68000", "68010", "68EC020", "68020", "68EC030", "68030",
    "68EC040", "68LC040", "68040", "68EC060", "68LC060", "68060"
};

/* TAS/CAS/CAS2 use the indivisible read-modify-write bus cycle */
static int is_rmw_case(const char *id)
{
    static const char *const rmw[] = { "tas", "cas", "cas2" };
    unsigned k;
    for (k = 0; k < sizeof rmw / sizeof rmw[0]; k++) {
        const char *a = id, *b = rmw[k];
        while (*a && *a == *b) { a++; b++; }
        if (!*a && !*b)
            return 1;
    }
    return 0;
}

static ULONG cs_nrun, cs_nfail, cs_first, cs_bad;
static long cs_hr;
static int cs_step;

static void case_body(void)
{
    const struct cb_case *c = ccase;
    int j;
    for (j = 0; j < c->n; j++) {
        const struct cb_tpl *t = &cb_tpls[c->first + j];
        UWORD v;
        if (!form_ok(t))
            continue;
        for (v = 0; v < t->nvec; v += cs_step) {
            cs_nrun++;
            if (run_vec(t, v))
                cs_nfail++;
        }
        amt_watchdog_kick();
    }
    /* hot loop: vector (i*7919 mod n), 0 = match */
    in_hot = 1;
    cs_hr = amt_hot(hot_fn, hot_reps(), &cs_first, &cs_bad);
    in_hot = 0;
}

/* the snippet of vector cur_vi of cur_t took an exception that the case does
 * not expect (no template of cpu_basic is meant to trap) */
static void report_abort(const struct cb_case *c, int code)
{
    const struct cb_tpl *t = cur_t;
    ULONG pc = amt_last_exc.pc;
    UWORD sr = amt_last_exc.sr, fv = amt_last_exc.frame[3];
    if (AMT_CPU() != AMT_CPU_68000) {
        /* 010+: every frame starts SR, PC, format/vector */
        sr = amt_last_exc.frame[0];
        pc = ((ULONG)amt_last_exc.frame[1] << 16) | amt_last_exc.frame[2];
    }
    amt_case_open(c->id, AMT_ST_FAIL);
    sb_reset();
    sb_s(c->rule);
    if (code == AMT_ABORT_UNEXPECTED) {
        sb_s(" -- the instruction took an unexpected exception (vector ");
        sb_int((long)amt_last_exc.vector);
        sb_s(")");
    } else {
        sb_s(" -- the case was aborted (watchdog or abort code)");
    }
    if (t) {
        sb_s(" in ");
        sb_s(t->text);
        sb_s(in_hot ? " during the hot loop" : "");
    }
    json_str("why", sbuf);
    if (t)
        emit_inputs(t, cur_vi);
    json_obj("expect");
    json_str("exception", "none");
    json_obj_end();
    json_obj("got");
    json_int("abort_code", code);
    if (code == AMT_ABORT_UNEXPECTED)
        json_int("vector", amt_last_exc.vector);
    json_obj_end();
    json_obj("ctx");
    if (t) {
        json_str("insn", t->text);
        emit_op(t);
        json_int("vec", cur_vi);
        if (in_hot)
            json_int("hot_iter", (long)hot_iter);
        /* the stacked PC as an offset from the snippet entry (68000 group 0
         * frames give an approximate PC) */
        json_hex("pc_off", pc - (ULONG)t->entry);
        if (AMT_CPU() != AMT_CPU_68000)
            json_hexn("format_vector", fv, 4);
    }
    json_hexn("sr", sr, 4);
    json_obj_end();
    amt_case_close();
}

static void run_case(const struct cb_case *c)
{
    int j, i, code;
    ULONG nrun = 0, nfail = 0, nskip_forms = 0, first, bad;
    long hr;
    UWORD mask = 0;
    int step = AMT_QUICK() ? 4 : 1;

    ccase = c;
    ccase_total = 0;
    nsaved = 0;
    in_hot = 0;
    hot_iter = 0;
    rmw_off = 0;
    for (j = 0; j < c->n; j++) {
        const struct cb_tpl *t = &cb_tpls[c->first + j];
        mask |= t->cpumask;
        if (cpu_ok(t))
            ccase_total += t->nvec;
        else
            nskip_forms++;
    }
    if (ccase_total && is_rmw_case(c->id) && pool_on_chip_bus()) {
        /* run only the forms without a memory operand (TAS Dn) */
        rmw_off = 1;
        ccase_total = 0;
        for (j = 0; j < c->n; j++) {
            const struct cb_tpl *t = &cb_tpls[c->first + j];
            if (form_ok(t))
                ccase_total += t->nvec;
            else if (cpu_ok(t))
                nskip_forms++;
        }
        if (!ccase_total) {
            amt_case(c->id, AMT_ST_SKIP, "TAS/CAS/CAS2 on memory need an indivisible read-modify-write "
                     "cycle, which the Amiga chip bus (chip RAM, and slow RAM at $C00000) does not "
                     "support (HRM: do not use TAS on chip memory; the same holds for CAS/CAS2), and "
                     "this machine has no other RAM for the test pool");
            return;
        }
    }
    if (!ccase_total) {
        sb_reset();
        sb_s("not applicable to the ");
        sb_s(cpu_names[AMT_CPU()]);
        sb_s(": every form of this case needs ");
        {
            int lo = -1, hi = -1;
            for (i = 0; i < 12; i++)
                if (mask >> i & 1) {
                    if (lo < 0)
                        lo = i;
                    hi = i;
                }
            sb_s(lo >= 0 ? cpu_names[lo] : "?");
            if (hi > lo) {
                sb_s(" .. ");
                sb_s(cpu_names[hi]);
            }
            sb_s(" (unimplemented or trapping elsewhere; see docs/cases/cpu_basic.md)");
        }
        amt_case(c->id, AMT_ST_SKIP, sbuf);
        return;
    }

    /* verification pass + hot loop, under amt_try(): an unexpected trap in
     * a snippet fails this case only and the module goes on with the next */
    cs_nrun = cs_nfail = 0;
    cs_first = cs_bad = 0;
    cs_hr = -1;
    cs_step = step;
    cur_t = 0;
    code = amt_try(case_body);
    if (code) {
        report_abort(c, code);
        in_hot = 0;
        return;
    }
    nrun = cs_nrun;
    nfail = cs_nfail;
    hr = cs_hr;
    first = cs_first;
    bad = cs_bad;

    if (!nfail && hr < 0 && first == 0) {
        amt_case(c->id, AMT_ST_PASS, 0);
        return;
    }
    amt_case_open(c->id, AMT_ST_FAIL);
    sb_reset();
    sb_s(c->rule);
    sb_s(" -- ");
    if (nfail) {
        sb_int((long)nfail);
        sb_s(" of ");
        sb_int((long)nrun);
        sb_s(" vectors wrong");
    } else {
        sb_s("all vectors right on the first pass, but a later repetition diverged (hot loop)");
    }
    if (nsaved) {
        sb_s("; first: ");
        why_diff(&saved[0]);
    }
    json_str("why", sbuf);
    if (nsaved) {
        emit_inputs(saved[0].t, saved[0].vi);
        emit_state(&saved[0], 0);
        emit_state(&saved[0], 1);
    } else {
        json_obj("expect");
        json_int("hot_result", 0);
        json_obj_end();
        json_obj("got");
        json_hex("hot_result", hr < 0 ? first : bad);
        json_obj_end();
    }
    json_obj("ctx");
    if (nsaved) {
        json_str("insn", saved[0].t->text);
        emit_op(saved[0].t);
        json_int("vec", saved[0].vi);
        json_hexn("ccr_mask", saved[0].exp_mask, 2);
        if (saved[0].hot)
            json_int("hot_iter", (long)saved[0].iter);
    }
    json_int("nfail", (long)nfail);
    json_int("nrun", (long)nrun);
    if (nskip_forms)
        json_int("forms_skipped_cpu", (long)nskip_forms);
    if (hr >= 0 || first) {
        json_obj("hot");
        json_int("iter", hr >= 0 ? hr : 0);
        json_hex("first", first);
        json_hex("bad", bad);
        json_obj_end();
    }
    json_obj_end();
    if (nsaved > 1) {
        json_arr("more");
        for (i = 1; i < nsaved; i++) {
            json_obj(NULL);
            emit_fail_detail(&saved[i]);
            json_obj_end();
        }
        json_arr_end();
    }
    amt_case_close();
}

void amt_run_cpu_basic(void)
{
    UWORD i;
    M = (UBYTE *)amt_any_pool() + M_OFFSET;
    for (i = 0; i < cb_ncases; i++)
        run_case(&cb_cases[i]);
}
