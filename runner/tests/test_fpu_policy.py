"""Execute the guest's FPU policy decisions without a ROM or emulator."""

import os
from pathlib import Path
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[2]
SOURCE = ROOT / "disk/modules/fpu.c"


def function(text, signature):
    start = text.index(signature)
    return text[start:text.index("\n}", start) + 2]


class FpuPolicy(unittest.TestCase):
    def test_only_hardware_policy_traps_are_untestable_in_compatibility(self):
        source = SOURCE.read_text()
        formats = source[source.index("static const UBYTE f_0[]"):
                         source.index("static void run_traps(")]
        code = r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef uint32_t ULONG;
typedef uint16_t UWORD;
typedef uint8_t UBYTE;
#define AMT_F_FPUCOMPAT 64
#define AMT_ST_PASS 1
#define AMT_ST_FAIL 2
#define AMT_ST_UNTESTABLE 3
#define AMT_ABORT_UNEXPECTED 1
#define FM_6888X 3
#define FM_040 4
#define FM_060 8
#define EXC_MID_6888X 0x8000
struct { ULONG flags; } amt_par;
struct { UWORD frame[16]; ULONG pc, vector; } amt_last_exc;
struct tent { ULONG start, next; const char *name; ULONG data; UWORD vec, mask; };
struct xent { ULONG start, fnop; const char *name; ULONG data; UWORD vec, enable, excbit, mask; };
static const struct tent fpu_trap_tab[] = {
    {100, 104, "fline", 0, 11, 3}, {100, 104, "unimp", 0, 11, 12},
    {100, 104, "effadd", 0, 60, 8}, {100, 104, "datatype", 0, 55, 12}
};
static const struct xent fpu_exc_tab[] = {
    {100, 104, "enabled", 0, 53, 0x1000, 0x1000, 15},
    {100, 104, "disabled", 0, 53, 0, 0x1000, 12},
    {100, 104, "moveout", 0, 52, 0x2000, 0x2000, EXC_MID_6888X | 3}
};
static const ULONG fpu_n_trap = 4, fpu_n_exc = 3;
static ULONG fpu_trap_idx, fpu_exc_idx, at[8];
static UWORD fm;
static unsigned calls, passed, untestable, failed;
static int forced_format = -1, wrong_instruction_pc;
static void amt_case(const char *id, int status, const char *why) {
    (void)id;
    if (status == AMT_ST_PASS) ++passed;
    else if (status == AMT_ST_UNTESTABLE) {
        assert(why && strstr(why, "configured FPU compatibility policy"));
        ++untestable;
    } else ++failed;
}
static void amt_case_open(const char *id, int status) { amt_case(id, status, 0); }
static void amt_case_close(void) {}
static void json_obj(const char *key) { (void)key; }
static void json_obj_end(void) {}
static void json_int(const char *key, long value) { (void)key; (void)value; }
static void json_str(const char *key, const char *value) { (void)key; (void)value; }
static void json_hexn(const char *key, ULONG value, int width) { (void)key; (void)value; (void)width; }
static void trap_ctx(ULONG start, ULONG next, int code) { (void)start; (void)next; (void)code; }
static void fpu_trap_run(void) {
    const struct tent *e = &fpu_trap_tab[fpu_trap_idx];
    amt_last_exc.vector = e->vec;
    amt_last_exc.pc = e->start;
    amt_last_exc.frame[3] = e->vec * 4 | ((fm == FM_040 && e->vec == 11) ? 0x2000 : 0);
    at[1] = fm == FM_040 ? 0x41300000 : 0x0000e000;
}
static ULONG hot_reps;                   /* 0: no repetitions (the counts below) */
static ULONG amt_hot_reps(void) { return hot_reps; }
static unsigned exc_runs, wrong_vector_from;   /* from this run on, no trap (0: never) */
static void fpu_exc_run(void) {
    const struct xent *e = &fpu_exc_tab[fpu_exc_idx];
    ++exc_runs;
    amt_last_exc.vector = wrong_vector_from && exc_runs >= wrong_vector_from ? 0 : e->vec;
    amt_last_exc.pc = e->start;
    amt_last_exc.frame[3] = e->vec * 4;
    if (e->mask & EXC_MID_6888X) {
        amt_last_exc.frame[3] |= (forced_format < 0 ? 9 : forced_format) << 12;
        amt_last_exc.pc = e->fnop;
        amt_last_exc.frame[4] = e->start >> 16;
        amt_last_exc.frame[5] = e->start + wrong_instruction_pc;
    }
    at[4] = e->excbit;
    at[5] = e->start;
}
static int amt_try(void (*operation)(void)) { ++calls; operation(); return AMT_ABORT_UNEXPECTED; }
static void fpu_after_trap(ULONG *out) { (void)out; }
"""
        code += function(source, "static int fmt_allowed(") + "\n" + formats
        code += function(source, "static void run_traps(") + "\n"
        code += function(source, "static void run_excs(") + r"""
int main(void) {
    for (unsigned model = 2; model <= 8; model *= 2) {
        fm = model;
        amt_par.flags = 0;
        calls = passed = untestable = failed = 0;
        run_traps();
        assert(calls && passed == calls && !failed && !untestable);
        const unsigned strict_cases = calls;
        amt_par.flags = AMT_F_FPUCOMPAT;
        calls = passed = untestable = failed = 0;
        run_traps();
        assert(!calls && !passed && !failed && untestable == strict_cases);
        calls = passed = untestable = failed = 0;
        run_excs();
        assert(calls == (model == 2 ? 2 : 1) && passed == calls && !failed);
        assert(untestable == (model == 2 ? 0 : 1));
        amt_par.flags = 0;
        calls = passed = untestable = failed = 0;
        run_excs();
        assert(calls == 2 && passed == calls && !failed && !untestable);
        if (model == 2) {
            for (int format = 0; format <= 3; format += 3) {
                forced_format = format;
                calls = passed = untestable = failed = 0;
                run_excs();
                assert(calls == 2 && passed == 1 && failed == 1 && !untestable);
            }
            forced_format = -1;
            wrong_instruction_pc = 2;
            calls = passed = untestable = failed = 0;
            run_excs();
            assert(calls == 2 && passed == 1 && failed == 1 && !untestable);
            wrong_instruction_pc = 0;
        }
    }
    /* each passing case runs again 2 x amt_hot_reps() times (compiled by a
     * JIT by then); one repetition that does not trap fails the case */
    fm = 3;
    amt_par.flags = 0;
    hot_reps = 3;
    calls = passed = untestable = failed = 0;
    exc_runs = wrong_vector_from = 0;
    run_excs();
    assert(passed == 2 && !failed && calls == 2 * (1 + 6));
    calls = passed = untestable = failed = 0;
    exc_runs = 0;
    wrong_vector_from = 3;
    run_excs();
    assert(failed == 2 && !passed);
    hot_reps = 0;
    wrong_vector_from = 0;
}
"""
        with tempfile.TemporaryDirectory(prefix=".fpu-policy-", dir=ROOT / "runner/tests") as directory:
            directory = Path(directory)
            driver = directory / "test.c"
            binary = directory / "test"
            driver.write_text(code)
            subprocess.run(["cc", "-std=c99", "-O2", str(driver), "-o", str(binary)],
                           env=dict(os.environ, TMPDIR=str(directory)), check=True)
            subprocess.run([str(binary)], check=True)

    def test_060_compatibility_checks_computed_dynamic_and_predicate_results(self):
        source = SOURCE.read_text()
        dynamic = function(source, "static void case_fmovem(")
        self.assertIn("if ((fm & FM_060) && !(amt_par.flags & AMT_F_FPUCOMPAT))", dynamic)
        self.assertIn("!eq(st, gs, 12)", dynamic)
        self.assertIn("!eq(e, ga, 24)", dynamic)
        main = function(source, "void amt_run_fpu(")
        self.assertIn("i == 0 || !is060 || (amt_par.flags & AMT_F_FPUCOMPAT)", main)

    def test_tolerance_counts_in_ulps_of_the_fpcr_precision(self):
        source = SOURCE.read_text()
        code = r"""
#include <assert.h>
#include <stdint.h>
typedef uint32_t ULONG;
""" + source[source.index("typedef unsigned long long u64;"):source.index("static const ULONG all_ones")] + r"""
static const ULONG mask[3] = { 0xffffffffUL, 0xffffffffUL, 0xffffffffUL };
#define FX 0x00
#define FS 0x40
#define FD 0x80
static int ok(const ULONG *e, const ULONG *g, ULONG tol, ULONG fpcr) { return x_ok(e, g, mask, tol, prec_shift(fpcr)); }
int main(void)
{
    const ULONG one[3] = { 0x3fff0000UL, 0x80000000UL, 0 };
    /* extended: raw mantissa ulps, as before */
    const ULONG x2[3] = { 0x3fff0000UL, 0x80000000UL, 2 }, x3[3] = { 0x3fff0000UL, 0x80000000UL, 3 };
    assert(ok(one, x2, 2, FX) && !ok(one, x3, 2, FX) && ulp_dist(one, x3, prec_shift(FX)) == 3);
    /* single: ulps of a 24-bit mantissa (bit 40), and the result must be rounded to it */
    const ULONG s2[3] = { 0x3fff0000UL, 0x80000200UL, 0 }, s3[3] = { 0x3fff0000UL, 0x80000300UL, 0 };
    const ULONG s_unrounded[3] = { 0x3fff0000UL, 0x80000000UL, 1 };
    assert(ok(one, s2, 2, FS) && !ok(one, s3, 2, FS) && ulp_dist(one, s3, prec_shift(FS)) == 3);
    assert(!ok(one, s_unrounded, 2, FS) && ulp_dist(one, s_unrounded, prec_shift(FS)) == -2);
    assert(ok(one, s_unrounded, 2, FX));    /* 1 extended ulp: fine at extended precision */
    /* double: bit 11 */
    const ULONG d1[3] = { 0x3fff0000UL, 0x80000000UL, 0x800 }, d_unrounded[3] = { 0x3fff0000UL, 0x80000000UL, 0x400 };
    assert(ok(one, d1, 1, FD) && !ok(one, d_unrounded, 2, FD) && ulp_dist(one, d1, prec_shift(FD)) == 1);
    /* across a binade: the largest single below 2 and 2.0 are 1 single ulp apart */
    const ULONG below2[3] = { 0x3fff0000UL, 0xffffff00UL, 0 }, two[3] = { 0x40000000UL, 0x80000000UL, 0 };
    assert(ok(below2, two, 1, FS) && ok(two, below2, 1, FS) && ulp_dist(below2, two, prec_shift(FS)) == 1);
    const ULONG below2x[3] = { 0x3fff0000UL, 0xffffffffUL, 0xffffffffUL };
    assert(ok(below2x, two, 1, FX) && ulp_dist(two, below2x, prec_shift(FX)) == 1);
    return 0;
}
"""
        with tempfile.TemporaryDirectory(prefix=".fpu-ulp-", dir=ROOT / "runner/tests") as directory:
            directory = Path(directory)
            driver = directory / "test.c"
            binary = directory / "test"
            driver.write_text(code)
            subprocess.run(["cc", "-std=c99", "-O2", "-Wall", str(driver), "-o", str(binary)],
                           env=dict(os.environ, TMPDIR=str(directory)), check=True)
            subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    unittest.main()
