/* dispatch (module 22): seeded random code blocks.
 *
 * tools/gencode.py generates, per CPU class and seed group, blocks of random
 * but valid integer code (ALU over all addressing modes of the class, MOVEM,
 * bit fields, MUL/DIV, BCD, CCR transfers, Bcc/BRA, DBcc loops, BSR/JSR/
 * RTS/RTD, LINK/UNLK, stack push/pop) and computes on the host, with a
 * reference model of the instructions, the state at the first arrival at
 * every instruction and after the block (dispatch_tab.s; format in the
 * generator). Nothing here models an instruction.
 *
 * Per block: build the initial state from the seed (registers, CCR, a
 * 256-byte data window at M = any pool), run it (ds_run, dispatch_asm.s),
 * compare d0-d7/a0-a7 (pointers relative to M), the defined CCR flags and
 * the window. On a mismatch, bisect: run a copy of the block with the first
 * opcode word of instruction k replaced by TRAP #15, for every k in
 * first-arrival order, and report the first checkpoint whose state differs.
 * Then amt_hot() re-runs block (i mod n) and compares a hash of the result
 * (interpreter vs JIT-compiled code).
 */
#include "mod.h"

AMT_MODULE(dispatch, 22, AMT_PH_BARE, 0, 0, 4096);

void *memcpy(void *, const void *, unsigned long);

struct ds_case {
    const char *id;
    UWORD cls, first, n, nq, cpumask, pad;
};
struct ds_blk {
    const UWORD *code;
    const UBYTE *info;
    const UBYTE *text;      /* per instruction: byte-pair encoded text, 0-terminated */
};
extern const UBYTE ds_bpe[512];
extern const struct ds_case ds_cases[];
extern const UWORD ds_ncases;
extern const struct ds_blk ds_blocks[];
extern const UWORD ds_nblocks;

/* dispatch_asm.s */
void ds_run(void);
extern ULONG ds_in[16], ds_out[16];
extern UWORD ds_sr, ds_osr, ds_vec;
extern UWORD ds_frame[16];
extern const void *ds_entry;
extern const UBYTE ds_xstubs[];
/* dispatch_040.s */
void ds_cacr_or(ULONG bits);
void ds_cpusha(int is060);

#define WIN     256
#define SP0     0x7fc
#define CODEBUF 0x800
#define MAXBLK  64
#define TRAP15  0x4e4f
#define XSTUB   12          /* bytes per stub in ds_xstubs */

static const UBYTE pregs[5] = { 8, 9, 10, 11, 14 };
#define IS_PTR(r) ((r) >= 8 && (r) != 12 && (r) != 13)

struct st {
    ULONG r[16];
    UBYTE ccr, dm;          /* ccr has 0 in undefined flags; dm = defined mask */
    UBYTE win[WIN];
};

static UBYTE *M;
static ULONG exp_hash[MAXBLK];
static UBYTE exp_dm[MAXBLK];
static const struct ds_case *ccase;
static UWORD cur_blk;
static int fam;              /* 0: 68000/010, 2: 020/030, 3: 030, 4: 040, 6: 060 */
static struct st e_st, g_st, p_st, f_st;
static amt_vec_fn oldv[64];

/* ------------------------------------------------------------ state */
static ULONG xs(ULONG x)
{
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    return x;
}

/* same formula as init_state() in tools/gencode.py */
static void ds_init(ULONG seed, struct st *s)
{
    ULONG v[16], x = seed;
    int i;
    for (i = 0; i < 16; i++)
        v[i] = x = xs(x);
    for (i = 0; i < 8; i++)
        s->r[i] = v[i];
    s->r[12] = v[8];
    s->r[13] = v[9];
    for (i = 0; i < 5; i++)
        s->r[pregs[i]] = 0x30 + ((v[10 + i] >> 16) & 0x9e);
    s->r[15] = SP0;
    s->ccr = (UBYTE)(v[15] & 0x1f);
    s->dm = 0x1f;
    for (i = 0; i < WIN; i += 4) {
        x = xs(x);
        s->win[i] = (UBYTE)(x >> 24);
        s->win[i + 1] = (UBYTE)(x >> 16);
        s->win[i + 2] = (UBYTE)(x >> 8);
        s->win[i + 3] = (UBYTE)x;
    }
}

static ULONG rd32(const UBYTE *p)
{
    return ((ULONG)p[0] << 24) | ((ULONG)p[1] << 16) | ((ULONG)p[2] << 8) | p[3];
}

struct ent {
    UBYTE k, pred;
};

/* apply one stream entry to s; returns the next entry */
static const UBYTE *ds_apply(const UBYTE *p, struct st *s, struct ent *e, struct ent *prev)
{
    UBYTE hdr = *p++, dmask = 0, amask = 0;
    int i;
    if (hdr & 0x80) {
        e->k = *p++;
        e->pred = *p++;
    } else {
        e->k = prev->k + 1;
        e->pred = prev->k;
    }
    if (hdr & 0x40)
        dmask = *p++;
    if (hdr & 0x20)
        amask = *p++;
    for (i = 0; i < 8; i++)
        if (dmask & (1 << i)) {
            s->r[i] = rd32(p);
            p += 4;
        }
    for (i = 0; i < 8; i++)
        if (amask & (1 << i)) {
            s->r[8 + i] = rd32(p);
            p += 4;
        }
    if (hdr & 0x08)
        s->dm = *p++;
    if (hdr & 0x04)
        s->ccr = *p++;
    if (hdr & 0x10) {
        UBYTE n = *p++;
        while (n--) {
            UBYTE off = *p++, len = *p++;
            while (len--)
                s->win[off++] = *p++;
        }
    }
    return p;
}

struct binfo {
    ULONG seed;
    UBYTE nstatic, nent;
    const UBYTE *woff;
    const UBYTE *stream;
    ULONG codelen;
};

static void ds_info(UWORD b, struct binfo *bi)
{
    const UBYTE *p = ds_blocks[b].info;
    bi->seed = rd32(p);
    bi->nstatic = p[4];
    bi->nent = p[5];
    bi->woff = p + 6;
    bi->stream = p + 6 + bi->nstatic;
    bi->codelen = (ULONG)(p - (const UBYTE *)ds_blocks[b].code);
}

/* expected final state of block b */
static void ds_final(UWORD b, struct st *s)
{
    struct binfo bi;
    struct ent e = { 0xff, 0xff }, pe = { 0xff, 0xff };
    const UBYTE *p;
    int i;
    ds_info(b, &bi);
    ds_init(bi.seed, s);
    p = bi.stream;
    for (i = 0; i < bi.nent; i++) {
        p = ds_apply(p, s, &e, &pe);
        pe = e;
    }
}

static ULONG ds_hash(const struct st *s)
{
    ULONG h = 0x811c9dc5UL;
    int i;
    for (i = 0; i < 16; i++)
        h = ((h << 5) | (h >> 27)) ^ s->r[i];
    h = ((h << 5) | (h >> 27)) ^ ((ULONG)(s->ccr & s->dm) << 8 | s->dm);
    for (i = 0; i < WIN; i += 4)
        h = ((h << 5) | (h >> 27)) ^ rd32(s->win + i);
    return h;
}

/* ------------------------------------------------------------ running */
static void cache_flush(void)
{
    if (fam == 2)
        ds_cacr_or(0x0008);            /* CI */
    else if (fam == 3)
        ds_cacr_or(0x0808);            /* CI | CD */
    else if (fam >= 4)
        ds_cpusha(fam == 6);
}

/* Run block b (trunc < 0: as is; else a copy with a TRAP #15 at static
 * instruction trunc). Fills *g; returns the vector number that ended the
 * run (0 = normal return, 47 = the truncation trap, else an exception). */
static UWORD ds_exec(UWORD b, int trunc, struct st *g, int copy)
{
    struct st *s = &p_st;
    struct binfo bi;
    int i;
    ds_info(b, &bi);
    ds_init(bi.seed, s);
    for (i = 0; i < WIN; i++)
        M[i] = s->win[i];
    for (i = 0; i < 16; i++)
        ds_in[i] = IS_PTR(i) ? s->r[i] + (ULONG)M : s->r[i];
    ds_sr = 0x2000 | s->ccr;
    if (trunc >= 0) {
        UBYTE *dst = M + CODEBUF;
        if (copy) {
            const UBYTE *src = (const UBYTE *)ds_blocks[b].code;
            ULONG n = bi.codelen;
            for (i = 0; i < (int)n; i++)
                dst[i] = src[i];
            *(UWORD *)(dst + 2 * bi.woff[trunc]) = TRAP15;
            cache_flush();
        }
        ds_entry = dst;
    } else {
        ds_entry = ds_blocks[b].code;
    }
    ds_run();
    for (i = 0; i < 16; i++)
        g->r[i] = IS_PTR(i) ? ds_out[i] - (ULONG)M : ds_out[i];
    g->dm = 0x1f;
    if (ds_vec) {
        UWORD sr = AMT_CPU() == AMT_CPU_68000 &&
                   (ds_vec == 2 || ds_vec == 3) ? ds_frame[4] : ds_frame[0];
        int fsz = 6;
        if (AMT_CPU() != AMT_CPU_68000) {
            UWORD fmt = ds_frame[3] >> 12;
            fsz = fmt == 2 ? 12 : 8;
        } else if (ds_vec == 2 || ds_vec == 3) {
            fsz = 14;
        }
        g->ccr = (UBYTE)(sr & 0x1f);
        g->r[15] += fsz;
    } else {
        g->ccr = (UBYTE)(ds_osr & 0x1f);
    }
    for (i = 0; i < WIN; i++)
        g->win[i] = M[i];
    return ds_vec;
}

/* g matches e (CCR compared on e's defined flags)? */
static int same(const struct st *e, struct st *g)
{
    int i;
    g->dm = e->dm;
    g->ccr &= e->dm;
    for (i = 0; i < 16; i++)
        if (e->r[i] != g->r[i])
            return 0;
    if (e->ccr != g->ccr)
        return 0;
    for (i = 0; i < WIN; i++)
        if (e->win[i] != g->win[i])
            return 0;
    return 1;
}

static void install_vectors(void)
{
    int v, i = 0;
    /* 2-15 and 32-63 (FPU/MMU/unimplemented-integer 61 included, so a
     * wrong trap there is caught per block and bisected, too); 24-31 are
     * the interrupts */
    for (v = 2; v <= 63; v++) {
        if (v > 15 && v < 32)
            continue;
        oldv[v] = amt_vec_set(v, (amt_vec_fn)(ds_xstubs + XSTUB * i));
        i++;
    }
}

static void remove_vectors(void)
{
    int v;
    for (v = 2; v <= 63; v++) {
        if (v > 15 && v < 32)
            continue;
        amt_vec_set(v, oldv[v]);
    }
}

/* ------------------------------------------------------------ reporting */
static const char *const rnames[16] = {
    "d0", "d1", "d2", "d3", "d4", "d5", "d6", "d7",
    "a0", "a1", "a2", "a3", "a4", "a5", "a6", "a7"
};

static void ops(const char *key, UWORD b, UBYTE k)
{
    struct binfo bi;
    const UWORD *code = ds_blocks[b].code;
    int a, e, i;
    ds_info(b, &bi);
    json_arr(key);
    if (k < bi.nstatic) {
        a = bi.woff[k];
        e = k + 1 < bi.nstatic ? bi.woff[k + 1] : (int)(bi.codelen / 2);
        for (i = a; i < e && i < a + 8; i++)
            json_hexn(NULL, code[i], 4);
    }
    json_arr_end();
}

static void regs_obj(const char *key, const struct st *s, const struct st *other, int all)
{
    int i, n = 0;
    json_obj(key);
    for (i = 0; i < 16; i++)
        if (all || s->r[i] != other->r[i]) {
            json_hex(rnames[i], s->r[i]);
            n++;
        }
    if (all || s->ccr != other->ccr || n == 0)
        json_hexn("ccr", s->ccr, 2);
    if (all)
        json_hexn("ccr_mask", s->dm, 2);
    {
        int first = 1;
        for (i = 0; i < WIN; i++)
            if (s->win[i] != other->win[i]) {
                if (first) {
                    json_arr("win");
                    first = 0;
                }
                if (n++ < 40) {
                    char buf[8];
                    static const char hx[] = "0123456789abcdef";
                    buf[0] = hx[i >> 4];
                    buf[1] = hx[i & 15];
                    buf[2] = ':';
                    buf[3] = hx[s->win[i] >> 4];
                    buf[4] = hx[s->win[i] & 15];
                    buf[5] = 0;
                    json_str(NULL, buf);
                }
            }
        if (!first)
            json_arr_end();
    }
    json_obj_end();
}

struct fail {
    int have;
    UWORD blk, nfail;
    UBYTE k, pred;
    UBYTE hot, found;
    UWORD vec;
    ULONG iter, rep;
    struct st exp, got, before;
    UWORD frame[16];
};
static struct fail F;
static struct st hot_got;
static int hot_have;
static UWORD hot_vec;
static UWORD hot_frame[16];

/* instruction text k of block b (byte-pair decoded) into out */
static void ds_text(UWORD b, UBYTE k, char *out, int len)
{
    const UBYTE *p = ds_blocks[b].text;
    UBYTE stk[64];
    int sp, n = 0;
    while (k--)
        while (*p++)
            ;
    for (; *p; p++) {
        sp = 0;
        stk[sp++] = *p;
        while (sp) {
            UBYTE c = stk[--sp];
            if (ds_bpe[2 * c] && sp < 62) {
                stk[sp++] = ds_bpe[2 * c + 1];
                stk[sp++] = ds_bpe[2 * c];
            } else if (n < len - 1) {
                out[n++] = (char)c;
            }
        }
    }
    out[n] = 0;
}

/* expected state at the first arrival at static instruction k */
static void ds_state_at(UWORD b, UBYTE k, struct st *s)
{
    struct binfo bi;
    struct ent e = { 0xff, 0xff }, pe = { 0xff, 0xff };
    const UBYTE *p;
    int j;
    ds_info(b, &bi);
    ds_init(bi.seed, s);
    p = bi.stream;
    for (j = 0; j < bi.nent; j++) {
        p = ds_apply(p, s, &e, &pe);
        pe = e;
        if (e.k == k)
            return;
    }
}

/* Bisect block b: run it truncated (TRAP #15) at each checkpoint in
 * first-arrival order, reps times each (reps > 1: the truncated copy gets
 * compiled by a JIT like the original was), and find the first checkpoint
 * whose state differs from the expectation. */
static void bisect(UWORD b, ULONG reps)
{
    struct binfo bi;
    struct ent e = { 0xff, 0xff }, pe = { 0xff, 0xff };
    const UBYTE *p;
    int j;
    ULONG r;
    UWORD v;
    ds_info(b, &bi);
    ds_init(bi.seed, &e_st);
    p = bi.stream;
    for (j = 0; j < bi.nent; j++) {
        p = ds_apply(p, &e_st, &e, &pe);
        pe = e;
        for (r = 0; r < reps; r++) {
            if (e.k == 0xff)
                v = ds_exec(b, -1, &g_st, 0);
            else
                v = ds_exec(b, e.k, &g_st, r == 0);
            if ((e.k == 0xff ? v != 0 : v != 47) || !same(&e_st, &g_st)) {
                F.k = e.k;
                F.pred = e.pred;
                F.found = 1;
                F.rep = r;
                F.exp = e_st;
                F.got = g_st;
                F.vec = (e.k == 0xff ? v : (v == 47 ? 0 : v));
                if (F.vec)
                    memcpy(F.frame, ds_frame, sizeof F.frame);
                if (F.pred != 0xff)
                    ds_state_at(b, F.pred, &F.before);
                return;
            }
            if ((r & 31) == 31)
                amt_watchdog_kick();
        }
        amt_watchdog_kick();
    }
    F.found = 0;     /* the bisection did not reproduce it */
}

static void ins_obj(const char *key, UWORD b, UBYTE k)
{
    char t[64];
    json_obj(key);
    json_int("index", k == 0xff ? -1 : k);
    if (k != 0xff) {
        ds_text(b, k, t, sizeof t);
        json_str("text", t);
        ops("opcode", b, k);
    } else {
        json_str("text", "(end of block: returned to the harness)");
    }
    json_obj_end();
}

static void report(const struct ds_case *c, UWORD n, int st)
{
    struct binfo bi;
    char buf[80], *q;
    int i;
    if (st == AMT_ST_PASS) {
        amt_case(c->id, AMT_ST_PASS, 0);
        return;
    }
    ds_info(F.blk, &bi);
    amt_case_open(c->id, AMT_ST_FAIL);
    json_obj("in");
    json_int("block", (long)(F.blk - c->first));
    json_hex("seed", bi.seed);
    json_int("blocks", n);
    json_obj_end();
    if (!F.found)
        json_str("why", F.hot ? "the block gave a different final state on a repeated (hot) run than the reference "
                                "model and its first run (interpreter vs compiled code); truncated runs did not "
                                "reproduce it, so no culprit instruction"
                              : "final state differs from the reference model, but no truncated run reproduced a "
                                "difference (timing- or cache-dependent?)");
    else if (F.vec)
        json_str("why", "an exception was taken while executing the culprit instruction (ctx.culprit); the "
                        "reference model says it is valid in the state ctx.before");
    else if (F.k == 0xff)
        json_str("why", "state after the block differs from the reference model; every checkpoint matched, so the "
                        "divergence is in an instruction re-executed after its first arrival (loop/subroutine); "
                        "ctx.culprit is the last instruction executed");
    else
        json_str("why", F.hot ? "JIT/interpreter divergence: on a repeated run of the block truncated at ctx.k, the "
                                "state at the first arrival at ctx.k differs from the reference model; the culprit "
                                "instruction (ctx.culprit, executed just before) computed it wrongly from ctx.before"
                              : "the state at the first arrival at instruction ctx.k differs from the reference "
                                "model: the culprit instruction (ctx.culprit, executed just before) computed it "
                                "wrongly from ctx.before");
    /* full register files after; window bytes only where they differ */
    regs_obj("expect", &F.exp, &F.got, 1);
    regs_obj("got", &F.got, &F.exp, 1);
    json_obj("ctx");
    json_hex("window_base", (ULONG)M);
    json_str("regs", "a0-a3/a6/a7 are offsets from window_base; win entries are offset:byte; ccr compared on "
                     "ccr_mask (the defined flags)");
    json_int("failing_blocks", F.nfail);
    if (F.hot)
        json_int("hot_iter", (long)F.iter);
    if (F.found) {
        if (F.hot)
            json_int("bisect_rep", (long)F.rep);
        ins_obj("culprit", F.blk, F.pred);
        ins_obj("k", F.blk, F.k);
        if (F.pred != 0xff) {
            regs_obj("before", &F.before, &F.before, 1);
            json_str("before_is", "expected state at the first arrival at the culprit (that checkpoint matched, "
                                  "so got = expected there); if the culprit is in a loop it ran more than once "
                                  "before k");
        }
    }
    json_arr("block_text");
    for (i = 0; i < bi.nstatic; i++) {
        char t[64];
        q = amt_utoa(buf, (ULONG)i);
        *q++ = ':';
        *q++ = ' ';
        ds_text(F.blk, (UBYTE)i, t, sizeof t);
        {
            const char *u = t;
            while (*u && q < buf + sizeof buf - 1)
                *q++ = *u++;
        }
        *q = 0;
        json_str(NULL, buf);
    }
    json_arr_end();
    if (F.vec) {
        json_int("vector", F.vec);
        json_arr("frame");
        for (i = 0; i < 8; i++)
            json_hexn(NULL, F.frame[i], 4);
        json_arr_end();
    }
    json_obj_end();
    amt_case_close();
}

static ULONG hot_fn(ULONG i)
{
    UWORD n = AMT_QUICK() ? ccase->nq : ccase->n;
    UWORD j = (UWORD)(i % n);
    UWORD b = ccase->first + j;
    UWORD v;
    cur_blk = b;
    v = ds_exec(b, -1, &g_st, 0);
    g_st.dm = exp_dm[j];
    g_st.ccr &= exp_dm[j];
    if (v || ds_hash(&g_st) != exp_hash[j]) {
        if (!hot_have) {            /* keep the state of the first diverging run */
            hot_have = 1;
            hot_got = g_st;
            hot_vec = v;
            if (v)
                memcpy(hot_frame, ds_frame, sizeof hot_frame);
        }
        return j + 1;
    }
    return 0;
}

static void case_body(void)
{
    const struct ds_case *c = ccase;
    UWORD n = AMT_QUICK() ? c->nq : c->n;
    UWORD j;
    ULONG first, bad;
    long r;

    F.have = 0;
    F.nfail = 0;
    F.found = 0;
    for (j = 0; j < n; j++) {
        UWORD b = c->first + j, v;
        cur_blk = b;
        ds_final(b, &f_st);
        exp_dm[j] = f_st.dm;
        exp_hash[j] = ds_hash(&f_st);
        v = ds_exec(b, -1, &g_st, 0);
        if (v || !same(&f_st, &g_st)) {
            F.nfail++;
            if (!F.have) {
                F.have = 1;
                F.blk = b;
                F.hot = 0;
                F.exp = f_st;
                F.got = g_st;
                F.vec = v;
                if (v)
                    memcpy(F.frame, ds_frame, sizeof F.frame);
                bisect(b, 1);
            }
        }
#ifdef DS_CHECKALL
        else {
            /* self-check of the generated checkpoints: bisect every block */
            F.found = 0;
            bisect(b, 1);
            if (F.found && !F.have) {
                F.have = 1;
                F.blk = b;
                F.hot = 0;
                F.nfail++;
            }
        }
#endif
        amt_watchdog_kick();
    }
    if (F.have) {
        report(c, n, AMT_ST_FAIL);
        return;
    }
    hot_have = 0;
    r = amt_hot(hot_fn, amt_hot_reps(), &first, &bad);
    if (r >= 0) {
        UWORD b = c->first + (UWORD)(bad - 1);
        F.have = 1;
        F.hot = 1;
        F.blk = b;
        F.iter = (ULONG)r;
        F.nfail = 1;
        ds_final(b, &F.exp);
        F.got = hot_got;
        F.vec = hot_vec;
        same(&F.exp, &F.got);
        if (F.vec)
            memcpy(F.frame, hot_frame, sizeof F.frame);
        F.found = 0;
        bisect(b, amt_hot_reps() / 4 + 16);
        report(c, n, AMT_ST_FAIL);
        return;
    }
    report(c, n, AMT_ST_PASS);
}

void amt_run_dispatch(void)
{
    UWORD i;
    int code;
    M = (UBYTE *)amt_any_pool();
    switch (AMT_CPU()) {
    case AMT_CPU_68000: case AMT_CPU_68010: fam = 0; break;
    case AMT_CPU_68EC020: case AMT_CPU_68020: fam = 2; break;
    case AMT_CPU_68EC030: case AMT_CPU_68030: fam = 3; break;
    case AMT_CPU_68EC040: case AMT_CPU_68LC040: case AMT_CPU_68040: fam = 4; break;
    default: fam = 6; break;
    }
    for (i = 0; i < ds_ncases; i++) {
        const struct ds_case *c = &ds_cases[i];
        ccase = c;
        if (!((c->cpumask >> AMT_CPU()) & 1)) {
            amt_case(c->id, AMT_ST_SKIP, c->cls == 0 ? "class not supported by this CPU"
                     : c->cls == 1 ? "class c020 needs a 68020/030/040 (64-bit MUL/DIV are unimplemented on the 68060)"
                     : "class c040 needs a 68040/68060");
            continue;
        }
        install_vectors();
        code = amt_try(case_body);
        remove_vectors();
        if (code) {
            amt_case_open(c->id, AMT_ST_FAIL);
            json_str("why", code == AMT_ABORT_WATCHDOG ? "watchdog: a block did not return (runaway loop?)"
                                                       : "unexpected exception outside the block harness");
            json_obj("expect");
            json_str("result", "every block returns");
            json_obj_end();
            json_obj("got");
            json_int("abort", code);
            json_int("vector", amt_last_exc.vector);
            {
                /* as an offset from the entry of the block that ran (010+:
                 * the PC from the frame; the 68000 catch-all decodes it) */
                ULONG pc = amt_last_exc.pc;
                if (AMT_CPU() != AMT_CPU_68000)
                    pc = ((ULONG)amt_last_exc.frame[1] << 16) | amt_last_exc.frame[2];
                json_int("pc_off_block", (long)(pc - (ULONG)ds_entry));
            }
            json_obj_end();
            json_obj("in");
            json_int("block", (long)(cur_blk - c->first));
            json_obj_end();
            amt_case_close();
        }
    }
}
