"""Native execution of CPU audit fixes; no emulator or guest is launched."""
import os
from pathlib import Path
import re
import resource
import shutil
import subprocess
import unittest

from tests.test_guest_display_timing import function

ROOT = Path(__file__).resolve().parents[2]
MODULES = ROOT / "disk" / "modules"
TYPES = """
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef uint32_t ULONG;
"""


def runtime_function(name):
    source = (ROOT / "disk" / "rt" / "mod.c").read_text()
    source = re.sub(r"^(long|int) " + name + r"\(", r"static \1 " + name + "(",
                    source, flags=re.MULTILINE)
    return function(source, name)


class CPUAuditFixes(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = shutil.which("cc")
        if not cls.cc:
            raise unittest.SkipTest("native C compiler unavailable")
        cls.build = ROOT / "work" / f"cpu-audit-tests-{os.getpid()}"
        cls.build.mkdir(parents=True, exist_ok=True)
        cls.addClassCleanup(shutil.rmtree, cls.build)

    def native(self, source, mutations=()):
        """The same assertions must pass production and reject each mutation."""
        for index, variant in enumerate([source, *mutations]):
            cfile = self.build / f"{self._testMethodName}-{index}.c"
            binary = cfile.with_suffix("")
            cfile.write_text(TYPES + variant)
            compiled = subprocess.run([self.cc, "-std=c99", "-O0", "-fno-pie", "-no-pie", "-Wall", "-Wextra",
                                       "-Wno-unused-parameter", "-Wno-unused-function",
                                       "-Wno-pointer-to-int-cast", "-I", str(MODULES),
                                       str(cfile), "-o", str(binary)],
                                      capture_output=True, text=True)
            self.assertEqual(compiled.returncode, 0, compiled.stderr)
            result = subprocess.run([str(binary)], capture_output=True, text=True,
                                    preexec_fn=lambda: resource.setrlimit(resource.RLIMIT_CORE, (0, 0)))
            if index:
                self.assertNotEqual(result.returncode, 0, "old-code mutation survived")
            else:
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_unimplemented_integer_expectations_follow_declared_policy(self):
        helper = function((MODULES / "exceptions.c").read_text(), "unimpl060_strict_policy")
        code = """
#define AMT_F_INTCOMPAT 32
#define AMT_ST_UNTESTABLE 3
static struct { ULONG flags; } amt_par;
static int emitted;
static void amt_case(const char *id, int status, const char *why) {
    assert(!strcmp(id, "unimpl060_movep_w"));
    assert(status == AMT_ST_UNTESTABLE);
    assert(strstr(why, "strict-policy interpreter lane"));
    emitted++;
}
""" + helper + """
int main(void) {
    for (unsigned flags = 0; flags < 128; flags++) {
        amt_par.flags = flags;
        emitted = 0;
        int applicable = unimpl060_strict_policy("unimpl060_movep_w");
        assert(applicable == !(flags & AMT_F_INTCOMPAT));
        assert(emitted == !!(flags & AMT_F_INTCOMPAT));
    }
}
"""
        self.native(code, [code.replace("amt_par.flags & AMT_F_INTCOMPAT", "0")])

    def test_checked_hot_includes_first_call(self):
        helper = runtime_function("amt_hot_checked")
        source = """
static ULONG kicks, calls, fail_at;
static void amt_watchdog_kick(void) { kicks++; }
""" + helper + """
static ULONG callback(ULONG i) { calls++; return i == fail_at ? 7 : 0; }
int main(void) {
    ULONG first = 999, bad = 999;
    fail_at = 0;
    assert(amt_hot_checked(callback, 20, &first, &bad) == 0);
    assert(first == 0 && bad == 7 && calls == 1);
    calls = 0; fail_at = 20;
    assert(amt_hot_checked(callback, 20, &first, &bad) == 20);
    assert(calls == 21 && bad == 7);
    calls = kicks = 0; fail_at = 999;
    assert(amt_hot_checked(callback, 100, 0, 0) == -1);
    assert(calls == 101 && kicks == 2);
}
"""
        # Equivalent to learning the expected result from callback(0).
        mutant = source.replace("ULONG result = fn(i);", "ULONG result = fn(i); if (!i) result = 0;")
        self.assertNotEqual(source, mutant)
        self.native(source, [mutant])

    def test_fpu_semantics_masks_and_tolerance_every_pass(self):
        guest = (MODULES / "fpu.c").read_text()
        structs = guest[guest.index("struct fvec {"):guest.index("extern const struct fgroup")]
        source = structs + """
typedef unsigned long long u64;
static const ULONG all_ones[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
static UWORD fm = 1;
static const struct fgroup *hot_g;
static struct fout hot_o;
static ULONG hot_bad_i, executions;
static int corrupt;
static void fpu_exec(const struct fvec *v, struct fout *o) {
    memcpy(o->res, v->res, sizeof o->res);
    memcpy(o->res2, v->res2, sizeof o->res2);
    o->fpsr = v->fpsr;
    if (v->op == 0) {
        o->res[0] ^= ((executions / 2) & 1) ? 0x80000000U : 0;
        o->fpsr ^= ((executions / 2) & 1) ? 0x08000000U : 0;
    } else o->res[2] += (executions / 2) & 1;
    if (corrupt) o->res[1] ^= 0x100;
    executions++;
}
"""
        source += "\n".join(function(guest, name) for name in
                            ("ulp_ok", "x_ok", "vec_ok", "hot_fn"))
        source += """
int main(void) {
    struct fvec v[3] = {{0}};
    struct fgroup g = {"mask", v, 3, 1};
    ULONG i;
    v[0].flags = 1;
    v[0].res[0] = 0x7fff0000U;
    v[0].res[1] = 0xc1234567U;
    v[0].rmask[0] = 0x7fffffffU; /* undefined NaN sign */
    v[0].rmask[1] = v[0].rmask[2] = UINT32_MAX;
    v[0].fpsr_mask = ~0x08000000U; /* undefined NaN N */
    v[1] = v[0]; v[1].op = 1; v[1].flags = 1 | (1 << 8);
    v[1].res[0] = 0x3fff0000U; v[1].res[1] = 0x80000000U;
    v[1].rmask[0] = UINT32_MAX;
    v[2].flags = 2; /* another FPU: must not execute */
    hot_g = &g;
    for (i = 0; i < 10; i++) assert(hot_fn(i) == 0);
    assert(executions == 20);
    corrupt = 1; executions = 0;
    /* A consistently wrong first hot execution must never become a baseline. */
    assert(hot_fn(0) == 1 && hot_bad_i == 0);
    assert(hot_fn(1) == 1);
    corrupt = 0; v[0].rmask[0] = UINT32_MAX; executions = 2;
    assert(hot_fn(0) == 1); /* a defined sign bit still matters */
}
"""
        self.native(source, [source.replace("if (!vec_ok(v, &hot_o))", "if (0)"),
                             source.replace("v->fpsr_mask)", "UINT32_MAX)"),
                             source.replace("return tol && ulp_ok(e, g, tol);", "return 0;")])

    def test_flags_rechecks_independent_model(self):
        guest = (MODULES / "flags.c").read_text()
        source = """
struct tcase { ULONG n, w; ULONG (*run)(void); void (*model)(ULONG, ULONG *); };
static const struct tcase *cur_tc;
static ULONG got[4], wrong;
""" + function(guest, "first_bad") + function(guest, "tramp") + """
static void model(ULONG i, ULONG *e) { e[0] = i; }
static ULONG run(void) {
    ULONG i;
    for (i = 0; i < 4; i++) got[i] = i + wrong;
    return 0x12345678U; /* constant checksum is not semantic evidence */
}
int main(void) {
    struct tcase t = {4, 1, run, model};
    cur_tc = &t;
    assert(tramp(0) == 0);
    wrong = 1;
    assert(tramp(0) == 4 && tramp(1) == 4);
}
"""
        self.native(source, [source.replace("return first_bad(cur_tc, &nbad, e) < 0 ? 0 : nbad;",
                                            "return 0;")])

    def test_cpu_coverage_each_snippet_and_vector_after_warmup(self):
        guest = (MODULES / "cpu_basic.c").read_text()
        table = (MODULES / "cpu_basic_tab.s").read_text()
        case = re.search(r'dc\.l\s+cbcn_76,cbcr_76\s+dc\.w\s+(\d+),(\d+)', table)
        first, count = map(int, case.groups())
        sizes = [int(size) for index, size in re.findall(
            r'dc\.w\s+\(cbe_(\d+)-cbs_\d+\)/2,(\d+)', table)
                 if first <= int(index) < first + count]
        self.assertEqual(len(sizes), 53)
        source = """
struct cb_tpl { UWORD nvec; int enabled; };
struct cb_case { UWORD first, n; };
static struct cb_tpl cb_tpls[55];
static struct cb_case c = {1, 53};
static const struct cb_case *ccase = &c;
static ULONG hot_iter, counts[55], after[55][31], bad_template, base = 20;
static int form_ok(const struct cb_tpl *t) { return t->enabled; }
static ULONG amt_hot_reps(void) { return base; }
static void amt_watchdog_kick(void) {}
static int run_vec(const struct cb_tpl *t, UWORD vi) {
    ULONG j = t - cb_tpls;
    assert(vi < t->nvec); counts[j]++;
    if (hot_iter >= 4) after[j][vi]++;
    return j == bad_template;
}
""" + function(guest, "hot_reps") + function(guest, "hot_fn") + """
int main(void) {
    ULONG i, j;
    for (i = 1; i <= 53; i++) { cb_tpls[i].nvec = 1 + i % 31; cb_tpls[i].enabled = 1; }
    cb_tpls[7].enabled = 0;
    for (i = 0; i <= hot_reps(); i++) assert(hot_fn(i) == 0);
    for (i = 1; i <= 53; i++) {
        if (i == 7) { assert(counts[i] == 0); continue; }
        assert(counts[i] >= 5);
        for (j = 0; j < cb_tpls[i].nvec; j++) assert(after[i][j] > 0);
    }
    bad_template = 53; assert(hot_fn(0) == 53);
    {
        static const UWORD actual_sizes[53] = {SIZES};
        for (base = 20; base <= 100; base += 80) {
            memset(counts, 0, sizeof counts); memset(after, 0, sizeof after);
            bad_template = 0;
            for (i = 1; i <= 53; i++) {
                cb_tpls[i].nvec = actual_sizes[i - 1]; cb_tpls[i].enabled = 1;
            }
            for (i = 0; i <= hot_reps(); i++) assert(hot_fn(i) == 0);
            for (i = 1; i <= 53; i++) {
                assert(counts[i] == base + 1);
                for (j = 0; j < cb_tpls[i].nvec; j++) assert(after[i][j] > 0);
            }
        }
    }
}
""".replace("SIZES", ",".join(map(str, sizes)))
        # Prior scheduling visited only one snippet per iteration.
        mutant = source.replace("if (form_ok(t) && t->nvec && run_vec",
                                "if (j == (int)(i % ccase->n) && form_ok(t) && t->nvec && run_vec")
        self.native(source, [mutant])

    def test_dispatch_first_hot_failure_preserves_block(self):
        guest = (MODULES / "dispatch.c").read_text()
        source = """
struct st { ULONG value; UWORD dm, ccr; };
static struct { UWORD first, n, nq; } cs = {48, 48, 16}, *ccase = &cs;
static struct st g_st, hot_got;
static UWORD cur_blk, hot_vec, hot_block, hot_frame[8], ds_frame[8], exp_dm[48];
static ULONG exp_hash[48];
static int hot_have, call_count;
#define AMT_QUICK() 1
static UWORD ds_exec(UWORD b, int stop, struct st *s, int flags) {
    s->value = b; s->ccr = 0; call_count++;
    return call_count == 1 ? 4 : 0;
}
static ULONG ds_hash(const struct st *s) { return s->value; }
""" + function(guest, "hot_fn") + """
int main(void) {
    ULONG i;
    for (i = 0; i < 48; i++) { exp_dm[i] = 31; exp_hash[i] = cs.first + i; }
    assert(hot_fn(0) == 1);
    assert(hot_have && hot_block == 48 && hot_vec == 4);
    assert(hot_fn(1) == 0);
    assert(hot_block == 48 && hot_got.value == 48);
}
"""
        self.native(source, [source.replace("hot_block = b;", "hot_block = (UWORD)(0 - 1);")])
        body = function(guest, "case_body")
        self.assertIn("amt_hot_checked(", body)
        self.assertIn("UWORD b = hot_block;", body)
        self.assertNotIn("(bad - 1)", body)

    def test_exceptions_independent_frames_and_trace(self):
        guest = (MODULES / "exceptions.c").read_text()
        source = """
#define XLOGMAX 40
#define T_INT 0
#define T_H4 1
#define X_NOSIZE 1
#define X_NOPC 2
#define A(p) ((ULONG)(uintptr_t)(p))
static int fam, corrupt, variant;
#define GE020 (fam >= 2)
#define IS000 (fam == 0)
struct tev { UWORD vec, sr; ULONG pc, addr; };
struct xlog { UWORD vec, hsr; ULONG ssp; UWORD f[12]; };
static volatile struct xlog xr_log[XLOGMAX];
static volatile UWORD xr_frame[30], xr_vec, xr_hsr, xr_nlog;
static volatile ULONG xr_spbase, xr_ssp, xr_in[8];
static UWORD input_sr;
static ULONG k_fail, k_tot;
static struct { ULONG at; const char *pcrule; int ev; } R;
static UBYTE xs_trap[64], xs_ill[176], xs_tcc[384];
static UBYTE xs_chkw[2], xs_chkw_at[2], xs_chkw_nx[2];
static UBYTE xs_chkl[2], xs_chkl_at[2], xs_chkl_nx[2];
static UBYTE xs_divuw[2], xs_divuw_at[2], xs_divuw_nx[2];
static UBYTE xs_divul[2], xs_divul_at[2], xs_divul_nx[2];
static UBYTE xs_trapv[2], xs_trapv_at[2], xs_trapv_nx[2];
static UBYTE xs_tr1[2], xs_tr1_a[2], xs_tr1_b[2], xs_tr1_c[2], xs_tr1_e[2],
             xs_tr1_sub[2], xs_tr1_s1[2], xs_tr1_f[2], xs_tr1_g[2], xs_tr1_h[2];
static void put32(volatile UWORD *p, ULONG v) { p[0] = v >> 16; p[1] = v; }
static void rbegin(ULONG at) { R.at = at; k_tot++; }
static void rend(void) {}
static void rframe_from(volatile UWORD *p, int n) {}
static int rcmp(const char *key, ULONG expected, ULONG got, int type) {
    if (expected != got) k_fail++;
    return expected == got;
}
static int rcmpo(const char *key, ULONG expected, ULONG got) {
    return rcmp(key, expected - R.at, got - R.at, 0);
}
static void xset(UWORD sr) { input_sr = sr; memset((void *)xr_in, 0, sizeof xr_in); }
"""
        source += "\n".join(function(guest, name) for name in ("deffmt", "fsize", "frl", "rexc"))
        source += """
static ULONG xrun(ULONG fn) {
    ULONG pc, at;
    UWORD vec, sr = input_sr;
    if (fn >= A(xs_trap) && fn < A(xs_trap) + 64) {
        vec = 32 + (fn - A(xs_trap)) / 4; at = fn; pc = fn + 2;
    } else if (fn == A(xs_chkw)) {
        assert(xr_in[0] == 0xffff && xr_in[1] == 10);
        vec = 6; at = A(xs_chkw_at); pc = A(xs_chkw_nx);
        sr = variant ? 0x270c : 0x2708; /* both are architecturally valid */
        if (corrupt == 1) sr &= ~8; /* defined N must be set */
    } else if (fn == A(xs_chkl)) {
        assert(xr_in[0] == 0x10000 && xr_in[1] == 0xffff);
        vec = 6; at = A(xs_chkl_at); pc = A(xs_chkl_nx); sr = 0x2700;
    } else if (fn == A(xs_divuw)) {
        vec = 5; at = A(xs_divuw_at); pc = A(xs_divuw_nx); sr = variant ? 0x270e : 0x2700;
    } else if (fn == A(xs_divul)) {
        vec = 5; at = A(xs_divul_at); pc = A(xs_divul_nx); sr = variant ? 0x270e : 0x2700;
    } else if (fn == A(xs_trapv)) {
        vec = 7; at = A(xs_trapv_at); pc = A(xs_trapv_nx);
    } else if (fn >= A(xs_tcc) && fn < A(xs_tcc) + sizeof xs_tcc) {
        ULONG form = (fn - A(xs_tcc)) / 128;
        vec = 7; at = fn; pc = fn + 2 + form * 2;
    } else { vec = fn == A(xs_ill) ? 4 : 10; at = pc = fn; }
    xr_vec = vec; xr_frame[0] = sr;
    put32(xr_frame + 1, pc); xr_frame[3] = (deffmt(vec) << 12) | vec * 4;
    put32(xr_frame + 4, at);
    xr_hsr = (sr | 0x2000) & 0x3fff;
    xr_spbase = 0x2000; xr_ssp = xr_spbase - (IS000 ? 6 : fsize(deffmt(vec)));
    return vec;
}
static int tmatch(ULONG r, const struct tev *e, int n) { return 1; }
static void tdump(void) {}
static ULONG trace_run(ULONG fn, ULONG stop, UWORD skip) {
    static UBYTE *const pcs[] = {xs_tr1_a, xs_tr1_b, xs_tr1_c, xs_tr1_e,
                                xs_tr1_sub, xs_tr1_s1, xs_tr1_f, xs_tr1_g, xs_tr1_h};
    int i;
    xr_nlog = 8;
    for (i = 0; i < 8; i++) {
        xr_log[i].vec = 9; xr_log[i].hsr = corrupt == 3 ? 0x2704 : 0x2700;
        xr_log[i].f[0] = i == 7 ? 0x2700 : 0xa700;
        put32(xr_log[i].f + 1, A(pcs[i + 1]) + (corrupt == 2 ? 2 : 0));
        xr_log[i].f[3] = GE020 ? 0x2024 : 0x24;
        put32(xr_log[i].f + 4, A(pcs[i]));
    }
    return 0;
}
"""
        source += "\n".join(function(guest, name) for name in
                            ("hrec", "hot_frames", "tr1_expect", "tcheck", "hot_trace"))
        source += """
int main(void) {
    for (fam = 0; fam <= 5; fam++) {
        corrupt = variant = 0; k_fail = 0;
        assert(hot_frames(0) == 0);
        variant = 1; assert(hot_frames(1) == 0);
        corrupt = 1; k_fail = 0;
        assert(hot_frames(0) > 0 && hot_frames(1) > 0);
        corrupt = 0; k_fail = 0; assert(hot_trace(0) == 0);
        corrupt = 2; assert(hot_trace(0) > 0);
        corrupt = 3; k_fail = 0; assert(hot_trace(0) > 0);
    }
}
"""
        self.native(source, [
            source.replace("6, 0x2708, 0xff18,", "6, 0x2708, 0xffff,"),
            source.replace("rexc(vec, sr, mask, pc, at, -1, 0);", "(void)vec;"),
            source.replace("tcheck(r, e, n);", "(void)r;")])

    def test_wb1_memory_alignment_all_sizes_offsets(self):
        header = (MODULES / "cpu_checks.h").read_text()
        source = header + """
int main(void) {
    static const ULONG longs[4] = {0x12345678U, 0x34567812U, 0x56781234U, 0x78123456U};
    static const ULONG words[4] = {0x1234, 0x3456, 0x5678, 0x7812};
    static const ULONG bytes[4] = {0x12, 0x34, 0x56, 0x78};
    ULONG i;
    assert(cpu_wb1_data(0x5a000000U, 0x1000, 1) == 0x5a);
    for (i = 0; i < 4; i++) {
        assert(cpu_wb1_data(0x12345678U, 0x1000 + i, 0) == longs[i]);
        assert(cpu_wb1_data(0x12345678U, 0x1000 + i, 1) == bytes[i]);
        assert(cpu_wb1_data(0x12345678U, 0x1000 + i, 2) == words[i]);
    }
}
"""
        # Old WB1 path passed raw data through exactly as WB2/WB3.
        mutant = source.replace("ULONG bits = (addr & 3) * 8;", "return data; ULONG bits = (addr & 3) * 8;")
        self.native(source, [mutant])
        mmu = (MODULES / "mmu.c").read_text()
        self.assertIn("if (n == 0)\n                data = cpu_wb1_data(data, addr, sz);",
                      function(mmu, "wb040"))
        asm = (MODULES / "exceptions_asm.s").read_text()
        self.assertIn("XWB\t18,40,44,1", asm)
        self.assertIn("XWB\t16,32,36,0", asm)
        self.assertIn("XWB\t14,24,28,0", asm)
        self.assertIn("rol.l\td0,d1", asm)

    def test_mmu_writeback_slots_validity_and_replay(self):
        guest = (MODULES / "mmu.c").read_text()
        source = """
#include "cpu_checks.h"
static struct { ULONG ssp; } mm_log[1];
static UWORD frame[30], status[3];
static int wb_done, writes;
static ULONG addresses[3], values[3], sizes[3], fcs[3];
static void mm_moves_put(ULONG addr, ULONG data, ULONG fc, ULONG size) {
    addresses[writes] = addr; values[writes] = data; fcs[writes] = fc;
    sizes[writes++] = size;
}
""" + function(guest, "fl32") + function(guest, "wb040") + """
static void put32(int word, ULONG v) { frame[word] = v >> 16; frame[word + 1] = v; }
int main(void) {
    mm_log[0].ssp = (ULONG)(uintptr_t)frame;
    frame[9] = 0xa5; put32(20, 0x1000); put32(22, 0x5a000000U);
    frame[8] = 0xa1; put32(16, 0x2001); put32(18, 0x5b);
    frame[7] = 0xc6; put32(12, 0x3002); put32(14, 0x7654);
    wb040(0);
    assert(writes == 3 && wb_done == 3);
    assert(addresses[0] == 0x1000 && values[0] == 0x5a && sizes[0] == 1 && fcs[0] == 5);
    assert(addresses[1] == 0x2001 && values[1] == 0x5b && sizes[1] == 1 && fcs[1] == 1);
    assert(addresses[2] == 0x3002 && values[2] == 0x7654 && sizes[2] == 2 && fcs[2] == 6);
    assert(frame[9] == 0x25 && frame[8] == 0x21 && frame[7] == 0x46);
    wb040(0); assert(writes == 3 && wb_done == 3); /* do not replay twice */
    frame[9] = 0xe5; wb040(0); /* line writeback is not a scalar transfer */
    assert(writes == 3 && frame[9] == 0xe5);
}
"""
        self.native(source, [source.replace("if (n == 0)", "if (0)"),
                             source.replace("if (n == 0)", "if (1)")])

    def test_chip_bus_gate_boundaries_and_callers(self):
        source = """
static ULONG address;
static int chip;
static void *amt_any_pool(void) { return (void *)(uintptr_t)address; }
static int amt_any_pool_is_chip(void) { return chip; }
#define any_pool ((void *)(uintptr_t)address)
#define any_is_chip chip
""" + runtime_function("amt_any_pool_on_chip_bus") + """
int main(void) {
    address = 0x10000000; chip = 1; assert(amt_any_pool_on_chip_bus());
    chip = 0; assert(!amt_any_pool_on_chip_bus());
    address = 0xbfffff; assert(!amt_any_pool_on_chip_bus());
    address = 0xc00000; assert(amt_any_pool_on_chip_bus());
    address = 0xdbffff; assert(amt_any_pool_on_chip_bus());
    address = 0xdc0000; assert(!amt_any_pool_on_chip_bus());
    address = 0x200000; assert(!amt_any_pool_on_chip_bus());
}
"""
        helper = runtime_function("amt_any_pool_on_chip_bus")
        mutant = source.replace(helper, "static int amt_any_pool_on_chip_bus(void) { return amt_any_pool_is_chip(); }")
        self.native(source, [mutant])
        for module in ("cpu_basic", "hostspeed", "mmu"):
            self.assertIn("amt_any_pool_on_chip_bus()", (MODULES / f"{module}.c").read_text())


if __name__ == "__main__":
    unittest.main()
