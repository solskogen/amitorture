/* smc: self-modifying code, prefetch and caches (CONTRACT.md section 10, id 5).
 *
 * Code is copied from templates (smc_asm.s) into the pools, executed,
 * patched with data writes and executed again. What the new execution must
 * see depends on the variant:
 *   68000/68010  no cache; a two-word prefetch (68000 UM 8.x, 68010 UM):
 *                the word right after the writing instruction is already
 *                fetched, so patching it still runs the OLD word. The 68010
 *                loop mode (DBcc + one loopable instruction) runs from the
 *                prefetch queue without opcode fetches.
 *   68020/68030  on-chip instruction cache (CACR E/F/CE/C, CAAR; 68020 UM
 *                ch. 6, 68030 UM ch. 6): not coherent with data writes, so
 *                without a clear the old code may run; after CACR C (CI),
 *                with the cache disabled or the entry cleared, the new code
 *                must run (code far from the PC; near the PC the prefetch
 *                pipeline makes it undefined).
 *   68040/68060  copyback data cache + instruction cache (040 UM 4.x, 060 UM
 *                5.x): new code needs CPUSH DC (push the write) and CINV/
 *                CPUSH IC; CPUSHA BC does both.
 * Undefined outcomes accept every allowed value; they are reported in the
 * clearly separate ctx field "undef_observed" (it may differ between
 * emulators and JIT on/off), never in pass/fail.
 *
 * CACR is restored to the value found after every case (also after an
 * aborted case); data caches are never invalidated without a push. */
#include "mod.h"

AMT_MODULE(smc, 5, AMT_PH_BARE, 0, 4096, 8192);

void *memcpy(void *, const void *, unsigned long);

/* smc_asm.s */
extern const UBYTE smc_tpl_ret[], smc_tpl_ret_end[];
extern const UBYTE smc_tpl_self[], smc_tpl_self_end[];
extern const UBYTE smc_tpl_loop[], smc_tpl_loop_end[];
extern const UBYTE smc_tpl_op[], smc_tpl_op_end[];
extern const UBYTE smc_tpl_bra[], smc_tpl_bra_end[];
ULONG smc_call0(void *code);
ULONG smc_call_self(void *code, void *patch, ULONG word);
ULONG smc_call_loop(void *code, ULONG word, ULONG count);
ULONG smc_call2(void *code, ULONG a, ULONG b);
/* smc_020.s (68020+) */
ULONG smc_cacr_get(void);
void smc_cacr_set(ULONG v);
void smc_caar_set(ULONG v);
/* smc_040.s (68040/68060) */
void smc_cpusha_bc(void);
void smc_cpusha_dc(void);
void smc_cinva_ic(void);
void smc_cpushl_bc(void *a);
void smc_cpushl_dc(void *a);
void smc_cinvl_ic(void *a);
void smc_cinvl_dc(void *a);

/* observations */
#define O_OLD     0
#define O_NEW     1
#define O_OTHER   2
#define O_WARMUP  3   /* the unpatched code did not give the old value */
#define O_NOBLIT  4   /* the blitter did not copy the code */
#define O_MIXED   5   /* immediate made of old and new 16-bit halves: the two
                         longwords it straddles came one stale, one fresh */
#define A(o)      (1UL << (o))
static const char *const o_names[6] = { "old", "new", "other", "warmup_wrong", "blit_not_copied", "mixed" };
#define STALE (A(O_OLD) | A(O_NEW) | A(O_MIXED))

static ULONG classify(ULONG r, ULONG newv, ULONG oldv)
{
    ULONG hi = r & 0xffff0000UL, lo = r & 0xffff;
    if (r == newv) return O_NEW;
    if (r == oldv) return O_OLD;
    if ((hi == (newv & 0xffff0000UL) || hi == (oldv & 0xffff0000UL)) &&
        (lo == (newv & 0xffff) || lo == (oldv & 0xffff)))
        return O_MIXED;
    return O_OTHER;
}

/* 020/030 CACR */
#define C_EI   0x0001UL
#define C_FI   0x0002UL
#define C_CEI  0x0004UL
#define C_CI   0x0008UL
#define C_CD   0x0800UL   /* 68030 only: clear data cache */
/* 040/060 CACR */
#define C4_IE  0x8000UL
#define C6_CABC 0x00400000UL

static int fam;           /* 0: 68000/010, 2: 68020/030, 4: 68040/060 */
static int is060, is030;
static ULONG cacr0;
static UBYTE *anyp, *chipp;

/* ------------------------------------------------------------ cache helpers */
static void ic_flush(void)
{
    if (fam == 2)
        smc_cacr_set(smc_cacr_get() | C_CI);
    else if (fam == 4) {
        smc_cpusha_bc();
        if (is060)
            smc_cacr_set(smc_cacr_get() | C6_CABC);
    }
}
static ULONG cacr_ic_on(void)  { return fam == 2 ? ((cacr0 | C_EI) & ~C_FI) : (cacr0 | C4_IE); }
static ULONG cacr_ic_off(void) { return fam == 2 ? (cacr0 & ~(C_EI | C_FI)) : (cacr0 & ~C4_IE); }
static void set_cacr(ULONG v)
{
    if (!fam)
        return;
    if (fam == 4)
        smc_cpusha_bc();
    smc_cacr_set(v);
    if (fam == 4)
        smc_cinva_ic();
}
static void restore_caches(void)
{
    if (fam) {
        set_cacr(cacr0);
        ic_flush();
    }
}
static void ic_on(void)  { if (fam) set_cacr(cacr_ic_on()); }

static void install(UBYTE *dst, const UBYTE *s, const UBYTE *e)
{
    memcpy(dst, s, (unsigned long)(e - s));
    ic_flush();
}

/* ------------------------------------------------------------ case runner */
static ULONG (*cur_fn)(ULONG);
static ULONG cur_allowed, obs_seen;
static long bad_iter;
static ULONG bad_obs, bad_got, bad_exp;
static ULONG last_got, last_exp;
static long hot_res;
static ULONG hot_first, hot_bad, cur_reps;

static ULONG obs_tramp(ULONG it)
{
    ULONG v;
    last_got = last_exp = 0;
    v = cur_fn(it);
    obs_seen |= A(v);
    if (!(cur_allowed & A(v)) && bad_iter < 0) {
        bad_iter = (long)it;
        bad_obs = v;
        bad_got = last_got;
        bad_exp = last_exp;
    }
    return v;
}
static void obs_body(void)
{
    hot_res = amt_hot(obs_tramp, cur_reps, &hot_first, &hot_bad);
}

static void obs_names(const char *key, ULONG mask)
{
    int i;
    json_arr(key);
    for (i = 0; i < 6; i++)
        if (mask & A(i))
            json_str(0, o_names[i]);
    json_arr_end();
}

/* Run fn(0..reps) under amt_hot. allowed: mask of acceptable observations.
 * insn/how describe the case for the record. */
static void obs_case(const char *id, ULONG (*fn)(ULONG), ULONG reps, ULONG allowed,
                     const char *how, const char *why)
{
    int code;
    cur_fn = fn;
    cur_allowed = allowed;
    cur_reps = reps;
    obs_seen = 0;
    bad_iter = -1;
    hot_res = -1;
    code = amt_try(obs_body);
    restore_caches();
    if (code) {
        amt_case_open(id, AMT_ST_FAIL);
        json_obj("expect");
        obs_names("one_of", allowed);
        json_obj_end();
        json_obj("got");
        json_int("abort", code);
        json_int("vector", amt_last_exc.vector);
        {
            /* the stacked PC as an offset into the pool it lies in, so the
             * record does not depend on where the pools were allocated */
            ULONG pc = amt_last_exc.pc;
            if (AMT_CPU() != AMT_CPU_68000)
                pc = ((ULONG)amt_last_exc.frame[1] << 16) | amt_last_exc.frame[2];
            if (pc - (ULONG)anyp < 8192)
                json_hex("pc_any_pool_off", pc - (ULONG)anyp);
            else if (pc - (ULONG)chipp < 4096)
                json_hex("pc_chip_pool_off", pc - (ULONG)chipp);
            else
                json_str("pc", "outside the pools (module code)");
        }
        json_hexn("sr", amt_last_exc.sr, 4);
        json_obj_end();
        json_str("why", "unexpected exception while running patched code (the patched or stale code did not execute as either version)");
        json_obj("in");
        json_str("how", how);
        json_obj_end();
        amt_case_close();
        return;
    }
    if (bad_iter < 0 && obs_seen) {
        if (allowed & (allowed - 1)) {
            /* undefined outcome: report what happened, separately */
            amt_case_open(id, AMT_ST_PASS);
            json_obj("ctx");
            obs_names("undef_observed", obs_seen);
            json_int("undef_changed_at", hot_res);
            json_obj_end();
            amt_case_close();
        } else
            amt_case(id, AMT_ST_PASS, 0);
        return;
    }
    amt_case_open(id, AMT_ST_FAIL);
    json_obj("expect");
    obs_names("one_of", allowed);
    if (bad_exp)
        json_hex("value", bad_exp);
    json_obj_end();
    json_obj("got");
    json_str("observed", o_names[bad_obs]);
    if (bad_exp)
        json_hex("value", bad_got);
    json_int("iteration", bad_iter);
    json_obj_end();
    json_str("why", why);
    json_obj("in");
    json_str("how", how);
    json_obj_end();
    json_obj("ctx");
    json_hex("cacr_found", cacr0);
    json_int("cpu_id", AMT_CPU());
    obs_names("observed_all", obs_seen);
    json_obj_end();
    amt_case_close();
}

static void skip(const char *id, const char *why)
{
    amt_case(id, AMT_ST_SKIP, why);
}

/* ------------------------------------------------------------ near patches */
static ULONG sm_dist, sm_warm;
static ULONG fn_selfmod(ULONG it)
{
    UBYTE *p = anyp + 256, *t;
    ULONG r;
    (void)it;
    install(p, smc_tpl_self, smc_tpl_self_end);
    t = p + 4 + sm_dist;
    if (sm_warm && smc_call_self(p, t, 0x4e71) != 0)   /* NOP over NOP */
        return O_WARMUP;
    r = smc_call_self(p, t, 0x7001);                    /* MOVEQ #1,d0 */
    return r == 1 ? O_NEW : r == 0 ? O_OLD : O_OTHER;
}

static void run_selfmod(void)
{
    static const UBYTE dist[4] = { 0, 2, 4, 8 };
    static const char *const ids[8] = {
        "selfmod_d0_cold", "selfmod_d0_warm", "selfmod_d2_cold", "selfmod_d2_warm",
        "selfmod_d4_cold", "selfmod_d4_warm", "selfmod_d8_cold", "selfmod_d8_warm"
    };
    int i, w;
    for (i = 0; i < 4; i++)
        for (w = 0; w < 2; w++) {
            ULONG allowed;
            sm_dist = dist[i];
            sm_warm = (ULONG)w;
            if (fam == 0)
                allowed = dist[i] == 0 ? A(O_OLD) : dist[i] == 2 ? A(O_OLD) | A(O_NEW) : A(O_NEW);
            else
                allowed = A(O_OLD) | A(O_NEW);
            obs_case(ids[i * 2 + w], fn_selfmod, amt_hot_reps(), allowed,
                     "MOVE.W d1,(a0) (one word) patches the NOP at distance D after itself with MOVEQ #1,d0; warm = routine ran once before (cached)",
                     fam == 0 ? (dist[i] == 0
                                 ? "68000/68010: the word right after the writing instruction is already in the two-word prefetch queue, so the OLD instruction must run (68000 UM prefetch; 68010 UM)"
                                 : "68000/68010: a word 4+ bytes after the writer is fetched after the write, so the NEW instruction must run (no cache)")
                              : "patch near the PC: any of old/new is allowed (prefetch pipeline)");
        }
}

/* ------------------------------------------------------------ DBcc loop / 68010 loop mode */
#define LOOP_N 8
static int loop_icoff;
static ULONG fn_loop(ULONG it)
{
    UBYTE *p = anyp + 512;
    ULONG r;
    (void)it;
    install(p, smc_tpl_loop, smc_tpl_loop_end);
    if (loop_icoff && fam)
        set_cacr(cacr_ic_off());
    r = smc_call_loop(p, 0x5242, LOOP_N);
    if (loop_icoff && fam)
        set_cacr(cacr0);
    return r == 0 ? O_OLD : r == LOOP_N - 1 ? O_NEW : O_OTHER;
}
static void run_loop(void)
{
    int k;
    for (k = 0; k < 2; k++) {
        ULONG allowed;
        loop_icoff = k;
        if (AMT_CPU() == AMT_CPU_68000)
            allowed = A(O_NEW);
        else if (AMT_CPU() == AMT_CPU_68010)
            allowed = A(O_OLD) | A(O_NEW);
        else if (fam == 2 && k)
            allowed = A(O_NEW);
        else
            allowed = A(O_OLD) | A(O_NEW);
        obs_case(k ? "loop_selfmod_icoff" : "loop_selfmod", fn_loop, amt_hot_reps(), allowed,
                 "loop: MOVE.W d1,(a0); DBF d0,loop with a0 = loop, d1 = ADDQ.W #1,d2, 8 passes: d2=0 old, 7 new; icoff = I-cache disabled for the loop",
                 AMT_CPU() == AMT_CPU_68000
                     ? "68000: the taken DBF refills the prefetch from memory, so the NEW instruction runs from the second pass (d2 = 7)"
                     : "68020/68030 with the instruction cache disabled: the taken DBF refetches from memory, so the NEW instruction must run");
    }
    /* 68010 loop mode is documented (68010 UM loop mode) to suppress opcode
     * fetches while looping; whether the first DBcc re-reads the loop word
     * before entering loop mode is not specified, so both are accepted. */
}

/* ------------------------------------------------------------ far patches */
enum { M_NOFLUSH, M_FLUSH, M_OFF, M_CE, M_FREEZE_COLD, M_FREEZE_WARM,
       M_CPUSHL_BC, M_CPUSHL_DC_CINVL_IC, M_CINVA_IC_ONLY, M_CPUSHA_DC_CINVA_IC };
static int far_mode;
static ULONG fn_far(ULONG it)
{
    UBYTE *p = anyp;      /* 16-byte aligned: the routine is in one 040/060 line */
    ULONG oldv = 0x10000000UL + it * 2, newv = oldv ^ 0x5a5a0001UL, r;
    install(p, smc_tpl_ret, smc_tpl_ret_end);
    *(ULONG *)(p + 2) = oldv;
    ic_flush();
    switch (far_mode) {
    case M_OFF: set_cacr(cacr_ic_off()); break;
    case M_FREEZE_COLD: smc_cacr_set(cacr_ic_on() | C_CI | C_FI); break;
    default: ic_on(); break;
    }
    if (smc_call0(p) != oldv)
        return O_WARMUP;
    if (far_mode == M_FREEZE_WARM)
        smc_cacr_set(cacr_ic_on() | C_FI);
    *(ULONG *)(p + 2) = newv;                       /* data write */
    switch (far_mode) {
    case M_FLUSH: ic_flush(); break;
    case M_OFF: if (fam == 4) smc_cpusha_dc(); break;
    case M_CE:
        smc_caar_set((ULONG)p);
        smc_cacr_set(cacr_ic_on() | C_CEI);
        smc_caar_set((ULONG)p + 4);
        smc_cacr_set(cacr_ic_on() | C_CEI);
        break;
    case M_CPUSHL_BC: smc_cpushl_bc(p); break;
    case M_CPUSHL_DC_CINVL_IC: smc_cpushl_dc(p); smc_cinvl_ic(p); break;
    case M_CINVA_IC_ONLY: smc_cinva_ic(); break;
    case M_CPUSHA_DC_CINVA_IC: smc_cpusha_dc(); smc_cinva_ic(); break;
    default: break;
    }
    r = smc_call0(p);
    last_got = r;
    last_exp = newv;
    if (fam)
        set_cacr(cacr0);
    return classify(r, newv, oldv);
}

struct fardef {
    const char *id;
    int mode;
    UBYTE fams;            /* bit 0: fam 0, bit 1: fam 2, bit 2: fam 4 */
    UBYTE strict;          /* 1: NEW only; 0: OLD or NEW (fam 0 always NEW) */
    const char *how, *why;
};
static const struct fardef fars[] = {
    { "ic_noflush_far", M_NOFLUSH, 7, 0,
      "I-cache on, run routine, patch its immediate, run again without any cache maintenance",
      "no maintenance: the I-cache is not coherent with data writes, so old, new or (per cached longword) mixed is allowed" },
    { "ic_flush_far", M_FLUSH, 7, 1,
      "I-cache on, run, patch, then 020/030: CACR |= C (clear I-cache); 040/060: CPUSHA BC (+060 CACR CABC); run again",
      "after the documented cache flush the NEW code must run (68020/68030 UM CACR C bit; 68040/68060 UM CPUSH)" },
    { "ic_off_far", M_OFF, 7, 1,
      "I-cache disabled (020/030 CACR E=0; 040/060 CACR IE=0), run, patch, (040/060: CPUSHA DC), run again",
      "with the instruction cache disabled every fetch goes to memory, so the NEW code must run" },
    { "ic_clear_entry_far", M_CE, 2, 1,
      "68020/68030: I-cache on, run, patch, CAAR = each patched longword + CACR CE (clear entry), run again",
      "after clearing the cache entries of the patched longwords the NEW code must run (68020/68030 UM CAAR/CE)" },
    { "ic_freeze_cold_far", M_FREEZE_COLD, 2, 1,
      "68020/68030: CACR = E|F|C (cleared and frozen), run (a miss, not filled), patch, run again",
      "a frozen cache does not fill on a miss, so the routine was never cached and the NEW code must run (68020/68030 UM CACR F)" },
    { "ic_freeze_warm_far", M_FREEZE_WARM, 2, 0,
      "68020/68030: I-cache on, run (cached), freeze, patch, run again",
      "frozen hit: old, new or per-longword mixed allowed" },
    { "cpushl_bc_far", M_CPUSHL_BC, 4, 1,
      "68040/68060: run, patch, CPUSHL BC,(line), run again",
      "CPUSHL BC pushes the dirty data line and invalidates both cache lines, so the NEW code must run (68040/68060 UM CPUSH)" },
    { "cpushl_dc_cinvl_ic_far", M_CPUSHL_DC_CINVL_IC, 4, 1,
      "68040/68060: run, patch, CPUSHL DC,(line); CINVL IC,(line), run again",
      "the write is pushed to memory and the instruction line invalidated, so the NEW code must run (68040/68060 UM CPUSH/CINV)" },
    { "cinva_ic_only_far", M_CINVA_IC_ONLY, 4, 0,
      "68040/68060: run, patch, CINVA IC only (no data push), run again",
      "without a data cache push the copyback line may not be in memory: old, new or mixed allowed" },
    { "cpusha_dc_cinva_ic_far", M_CPUSHA_DC_CINVA_IC, 4, 1,
      "68040/68060: run, patch, CPUSHA DC; CINVA IC, run again",
      "the write is pushed and the instruction cache invalidated, so the NEW code must run (68040/68060 UM)" },
};

/* 040/060 with the MMU enabled by the OS (probe "tc:e=1"): CPUSHL/CINVL take
 * a physical address (68040UM/68060UM CPUSH/CINV) and this module only has
 * logical ones; logical = physical is not guaranteed then */
static int mmu_on;
static int pfx(const char *s, const char *p)
{
    while (*p)
        if (*s++ != *p++)
            return 0;
    return 1;
}
static int probe_tc_enabled(void)
{
    int i;
    for (i = 0; i < amt_probe_result.n_how && i < AMT_PROBE_HOW_MAX; i++)
        if (amt_probe_result.how[i] && pfx(amt_probe_result.how[i], "tc:e=1"))
            return 1;
    return 0;
}

static void run_far(void)
{
    int i, fbit = fam == 0 ? 1 : fam == 2 ? 2 : 4;
    for (i = 0; i < (int)(sizeof(fars) / sizeof(fars[0])); i++) {
        const struct fardef *d = &fars[i];
        if (!(d->fams & fbit)) {
            skip(d->id, d->fams == 2 ? "68020/68030 cache control (CAAR/CE/freeze) only"
                                      : "68040/68060 cache instructions only");
            continue;
        }
        if (mmu_on && (d->mode == M_CPUSHL_BC || d->mode == M_CPUSHL_DC_CINVL_IC)) {
            skip(d->id, "the MMU was enabled (TC.E=1) when the module started: CPUSHL/CINVL take a "
                        "physical address and this module cannot assume logical = physical");
            continue;
        }
        far_mode = d->mode;
        obs_case(d->id, fn_far, amt_hot_reps(),
                 (fam == 0 || d->strict) ? A(O_NEW) : STALE, d->how,
                 (fam == 0 && !d->strict) ? "68000/68010 have no cache: the NEW code must run" : d->why);
    }
}

/* ------------------------------------------------------------ generated code */
static UBYTE *gen_base;
static ULONG gen_prev;
static ULONG gen_val(ULONG it) { return (it + 1) * 0x9e3779b9UL; }

static ULONG fn_gen_imm(ULONG it)
{
    ULONG v = gen_val(it), r;
    if (it == 0) {
        install(gen_base, smc_tpl_ret, smc_tpl_ret_end);
        gen_prev = 0x11111111UL;
    }
    *(ULONG *)(gen_base + 2) = v;
    ic_flush();
    r = smc_call0(gen_base);
    last_got = r;
    last_exp = v;
    r = classify(r, v, gen_prev);
    gen_prev = v;
    return r;
}

static const UWORD gen_ops[5] = { 0xd081, 0x9081, 0xc081, 0x8081, 0xb380 }; /* add sub and or eor .l d1,d0 */
static ULONG gen_op_model(int k, ULONG a, ULONG b)
{
    switch (k) {
    case 0: return a + b;
    case 1: return a - b;
    case 2: return a & b;
    case 3: return a | b;
    default: return a ^ b;
    }
}
static ULONG fn_gen_op(ULONG it)
{
    ULONG a = 0x12345678UL ^ (it * 0x01010101UL), b = 0x0f0f0f0fUL, r, e, o;
    int k = (int)(it % 5), pk = (int)((it + 4) % 5);
    if (it == 0)
        install(gen_base, smc_tpl_op, smc_tpl_op_end);
    *(UWORD *)gen_base = gen_ops[k];
    ic_flush();
    r = smc_call2(gen_base, a, b);
    e = gen_op_model(k, a, b);
    o = it ? gen_op_model(pk, a, b) : a + b;
    last_got = r;
    last_exp = e;
    return r == e ? O_NEW : r == o ? O_OLD : O_OTHER;
}

#define GEN_MANY 64
static ULONG fn_gen_many(ULONG it)
{
    int k;
    ULONG obs = O_NEW;
    for (k = 0; k < GEN_MANY; k++) {
        UBYTE *p = gen_base + k * 16;
        if (it == 0)
            memcpy(p, smc_tpl_ret, (unsigned long)(smc_tpl_ret_end - smc_tpl_ret));
        *(ULONG *)(p + 2) = gen_val(it * GEN_MANY + (ULONG)k);
    }
    ic_flush();
    for (k = 0; k < GEN_MANY; k++) {
        ULONG r = smc_call0(gen_base + k * 16), e = gen_val(it * GEN_MANY + (ULONG)k);
        if (r != e) {
            ULONG o = it ? gen_val((it - 1) * GEN_MANY + (ULONG)k) : 0x11111111UL;
            if (obs == O_NEW) {
                last_got = r;
                last_exp = e;
            }
            if (r == o) {
                if (obs == O_NEW)
                    obs = O_OLD;
            } else
                obs = O_OTHER;
        }
    }
    return obs;
}

static const UBYTE bra_disp[3] = { 2, 6, 10 };
static ULONG fn_gen_bra(ULONG it)
{
    ULONG k = it % 3, r;
    if (it == 0)
        install(gen_base, smc_tpl_bra, smc_tpl_bra_end);
    gen_base[1] = bra_disp[k];
    ic_flush();
    r = smc_call0(gen_base);
    last_got = r;
    last_exp = k + 1;
    return r == k + 1 ? O_NEW : (it && r == ((it + 2) % 3) + 1) ? O_OLD : O_OTHER;
}

static void run_gen(void)
{
    const char *flush = fam == 0 ? "no cache: nothing" : fam == 2 ? "CACR |= C" : is060 ? "CPUSHA BC + CACR CABC" : "CPUSHA BC";
    const char *why = "generated code: after each patch and the cache flush the patched code must run (what a JIT must handle: flush = invalidate translations)";
    (void)flush;
    gen_base = anyp + 1024;
    obs_case("gen_imm_any", fn_gen_imm, amt_hot_reps(), A(O_NEW),
             "MOVE.L #imm,d0; RTS in the any pool; per iteration patch imm, flush, call", why);
    gen_base = chipp + 64;
    obs_case("gen_imm_chip", fn_gen_imm, amt_hot_reps(), A(O_NEW),
             "MOVE.L #imm,d0; RTS in chip RAM; per iteration patch imm, flush, call", why);
    gen_base = anyp + 1024;
    obs_case("gen_opcode_any", fn_gen_op, amt_hot_reps(), A(O_NEW),
             "one opcode cycled ADD/SUB/AND/OR/EOR.L d1,d0; per iteration patch opcode, flush, call", why);
    gen_base = anyp + 2048;
    obs_case("gen_many_any", fn_gen_many, AMT_QUICK() ? 20 : 100, A(O_NEW),
             "64 routines (16-byte stride) MOVE.L #imm,d0; RTS; per iteration patch all, one flush, call all", why);
    gen_base = anyp + 1024;
    obs_case("gen_bra_any", fn_gen_bra, amt_hot_reps(), A(O_NEW),
             "BRA.S with its displacement byte cycled to 3 targets (returns 1/2/3); per iteration patch, flush, call", why);
}

/* ------------------------------------------------------------ DMA (blitter) */
#define BLT(off) (*(volatile UWORD *)(CUSTOM_BASE + (off)))
#define BLTL(off) (*(volatile ULONG *)(CUSTOM_BASE + (off)))
static int blit_wait(void)
{
    ULONG n = 0;
    (void)CREG(R_DMACONR);                  /* first read after BLTSIZE may be early */
    while (CREG(R_DMACONR) & 0x4000)        /* BBUSY */
        if (++n > 200000)
            return -1;
    return 0;
}
/* A -> D copy of `words` words, one line (HRM blitter: BLTCON0 USEA|USED,
 * minterm $F0 = A; masks all ones; no shift, no modulo) */
static int blit_copy(void *dst, const void *src, UWORD words)
{
    UWORD saved = CREG(R_DMACONR) & 0x0240;
    int r;
    CREG(R_DMACON) = 0x8240;                /* SET | DMAEN | BLTEN */
    if (blit_wait() < 0) {
        CREG(R_DMACON) = (UWORD)(0x0240 & ~saved);
        return -1;
    }
    BLT(0x040) = 0x09f0;                    /* BLTCON0 */
    BLT(0x042) = 0x0000;                    /* BLTCON1 */
    BLT(0x044) = 0xffff;                    /* BLTAFWM */
    BLT(0x046) = 0xffff;                    /* BLTALWM */
    BLT(0x064) = 0;                         /* BLTAMOD */
    BLT(0x066) = 0;                         /* BLTDMOD */
    BLTL(0x050) = (ULONG)src;               /* BLTAPT */
    BLTL(0x054) = (ULONG)dst;               /* BLTDPT */
    BLT(0x058) = (UWORD)((1 << 6) | words); /* BLTSIZE: 1 line */
    r = blit_wait();
    CREG(R_DMACON) = (UWORD)(0x0240 & ~saved);
    return r;
}
static int dma_flush;
static ULONG fn_dma(ULONG it)
{
    UBYTE *dst = chipp, *src = chipp + 16;
    ULONG oldv = 0x20000000UL + it, newv = oldv ^ 0x0f0f0f0fUL, r, mem;
    memcpy(dst, smc_tpl_ret, 8);
    memcpy(src, smc_tpl_ret, 8);
    *(ULONG *)(dst + 2) = oldv;
    *(ULONG *)(src + 2) = newv;
    ic_flush();                             /* 040/060: also pushes both to memory */
    ic_on();
    if (smc_call0(dst) != oldv)
        return O_WARMUP;
    if (blit_copy(dst, src, 4) < 0)
        return O_NOBLIT;
    if (dma_flush)
        ic_flush();
    r = smc_call0(dst);
    /* check the copy through the data side */
    if (is030)
        smc_cacr_set(smc_cacr_get() | C_CD);   /* write-through: nothing lost */
    else if (fam == 4 && !mmu_on)
        smc_cinvl_dc(dst);                      /* line owned by this module */
    else if (fam == 4)
        smc_cpusha_dc();                        /* MMU on: no physical address for CINVL;
                                                 * the dst line is clean (pushed before
                                                 * the blit), so a push-all is safe */
    mem = *(volatile ULONG *)(dst + 2);
    if (fam)
        set_cacr(cacr0);
    last_got = r;
    last_exp = newv;
    if (mem != newv) {
        last_got = mem;
        return O_NOBLIT;
    }
    return classify(r, newv, oldv);
}
static void run_dma(void)
{
    dma_flush = 1;
    obs_case("dma_blit_code_flush", fn_dma, AMT_QUICK() ? 50 : 300, A(O_NEW),
             "MOVE.L #old,d0; RTS in chip RAM, run, blitter A->D copy of MOVE.L #new,d0; RTS over it, flush (020/030 CACR C; 040/060 CPUSHA BC), run",
             "code overwritten by blitter DMA must run as NEW after the cache flush (no cache on 68000/010); blit_not_copied = the blit itself failed (HRM blitter)");
    dma_flush = 0;
    obs_case("dma_blit_code_noflush", fn_dma, AMT_QUICK() ? 50 : 300,
             fam == 0 ? A(O_NEW) : STALE,
             "as dma_blit_code_flush but no flush after the blit",
             "68000/68010: no cache, the NEW code must run; 020+: DMA is not snooped by the I-cache, old, new or per-longword mixed allowed");
}

/* ------------------------------------------------------------ body */
void amt_run_smc(void)
{
    int cpu = AMT_CPU();
    fam = cpu >= AMT_CPU_68EC040 ? 4 : cpu >= AMT_CPU_68EC020 ? 2 : 0;
    is060 = cpu >= AMT_CPU_68EC060;
    mmu_on = fam == 4 && probe_tc_enabled();
    is030 = cpu == AMT_CPU_68EC030 || cpu == AMT_CPU_68030;
    anyp = amt_any_pool();
    chipp = amt_chip_pool();
    cacr0 = fam ? smc_cacr_get() : 0;

    run_selfmod();
    run_loop();
    run_far();
    run_gen();
    run_dma();

    restore_caches();
    if (fam)
        amt_check32("cacr_restored", "cacr", cacr0, smc_cacr_get(),
                    "CACR read back differs from the value found at module start after restoring it (MOVEC CACR)");
    else
        amt_case("cacr_restored", AMT_ST_SKIP, "no CACR on 68000/68010");
}
