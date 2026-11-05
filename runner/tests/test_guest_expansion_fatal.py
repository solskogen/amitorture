"""Trap during expansion fatal reporting must not bypass quarantine."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest

from tests.test_guest_board_claim import public_function
from tests.test_guest_display_timing import function

ROOT = Path(__file__).resolve().parents[2]
MOD = (ROOT / "disk/rt/mod.c").read_text()


class GuestExpansionFatal(unittest.TestCase):
    def test_sticky_flag_and_fallback_order(self):
        self.assertRegex(MOD, r"static volatile int fatal_quarantine\s*;")
        fatal = public_function(MOD, "amt_abort_fatal")
        self.assertRegex(fatal.split("{", 1)[1], r"^\s*fatal_quarantine = 1;")
        self.assertEqual(re.findall(r"\bfatal_quarantine\s*=\s*([^;]+);", MOD), ["1"])
        run = function(MOD, "run_module")
        self.assertRegex(run, r"code = amt_try\(bare_body\);\s*amt_wd_armed = 0;\s*"
                         r"if \(fatal_quarantine\) \{\s*amt_set_sr\(0x2700\);\s*"
                         r"for \(;;\)\s*;\s*\}\s*bare_reset_env\(\);")
        check = run.index("if (fatal_quarantine)")
        for cleanup in ("bare_reset_env();", "cpu_module_policy();", "chip_module_policy();"):
            self.assertLess(check, run.index(cleanup, run.index("code = amt_try(bare_body);")))

    def test_fault_while_reporting_reaches_no_restore_fallback(self):
        cc = shutil.which("cc")
        if not cc:
            self.skipTest("native compiler required")
        build = ROOT / "work" / f"expansion-fatal-native-{os.getpid()}"
        build.mkdir(parents=True, exist_ok=True)
        self.addCleanup(shutil.rmtree, build)
        run = function(MOD, "run_module")
        start = run.index("code = amt_try(bare_body);")
        end = run.index("bare_reset_env();", start) + len("bare_reset_env();")
        recovery = run[start:end]
        source = r"""
#include <assert.h>
#include <setjmp.h>
#include <stdlib.h>
#define AMT_ST_FAIL 1
#define MBOX_STATE_ABORTED 3
static volatile int fatal_quarantine;
static int amt_wd_armed = 1, amt_n_aborted, fault, resets, disabled, forbidden;
static jmp_buf outer;
static void Disable(void) { assert(fatal_quarantine); disabled++; }
static void Forbid(void) { assert(fatal_quarantine); forbidden++; }
static void amt_case_open(const char *id, int status)
{
    (void)id; (void)status;
    assert(fatal_quarantine && !amt_wd_armed && disabled == 1 && forbidden == 1);
    longjmp(outer, 77); /* Model an exception from fatal reporting. */
}
static void json_str(const char *key, const char *value) { (void)key; (void)value; abort(); }
static void json_int(const char *key, int value) { (void)key; (void)value; abort(); }
static void json_obj(const char *key) { (void)key; abort(); }
static void json_obj_end(void) { abort(); }
static void amt_case_close(void) { abort(); }
static void mbox_set_state(int state) { (void)state; abort(); }
static void amt_set_sr(unsigned short value)
{
    assert(value == 0x2700 && fatal_quarantine && !amt_wd_armed);
    assert(fault && !resets && amt_n_aborted == 1 && disabled == 1 && forbidden == 1);
    exit(0); /* Observe production fallback before its permanent spin. */
}
static void bare_reset_env(void) { assert(!fatal_quarantine); resets++; }
"""
        source += public_function(MOD, "amt_abort_fatal")
        source += r"""
static void bare_body(void) { if (fault) amt_abort_fatal(123); }
static int amt_try(void (*fn)(void))
{
    int code = setjmp(outer);
    if (!code) fn();
    return code;
}
static void run_recovery(void)
{
    int code;
"""
        source += recovery + "\n(void)code;\n}\n"
        source += r"""
int main(int argc, char **argv)
{
    assert(argc == 2);
    fault = argv[1][0] == 'f';
    run_recovery();
    assert(!fault && resets == 1 && !amt_wd_armed && !fatal_quarantine && !amt_n_aborted);
    return 0;
}
"""
        mutations = [
            ("original", source),
            ("no-sticky-flag", source.replace("fatal_quarantine = 1;", "fatal_quarantine = 0;", 1)),
            ("no-fallback", source.replace("if (fatal_quarantine)", "if (0)", 1)),
            ("wrong-ipl", source.replace("amt_set_sr(0x2700);", "amt_set_sr(0x2000);", 1)),
        ]
        for name, text in mutations:
            c, binary = build / f"{name}.c", build / name
            c.write_text(text)
            subprocess.run([cc, "-std=c99", "-Wall", "-Wextra", "-Werror",
                            str(c), "-o", str(binary)], check=True, capture_output=True, text=True)
            for mode in ("n", "f") if name == "original" else ("f",):
                with self.subTest(name=name, mode=mode):
                    result = subprocess.run([str(binary), mode], capture_output=True, timeout=2)
                    if name == "original":
                        self.assertEqual(result.returncode, 0, result.stderr.decode())
                    else:
                        self.assertNotEqual(result.returncode, 0, f"undetected mutant {name}")
