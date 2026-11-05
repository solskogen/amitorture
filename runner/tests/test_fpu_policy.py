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
static void fpu_exc_run(void) {
    const struct xent *e = &fpu_exc_tab[fpu_exc_idx];
    amt_last_exc.vector = e->vec;
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


if __name__ == "__main__":
    unittest.main()
