"""Compile the production RTC calendar, read and restoration paths without hardware."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

from tests.test_guest_display_timing import function
from amitorture import records

ROOT = Path(__file__).resolve().parents[2]


class GuestRTC(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("native C compiler unavailable")
        (ROOT / "work" / "tmp").mkdir(parents=True, exist_ok=True)
        tmp = tempfile.TemporaryDirectory(dir=ROOT / "work" / "tmp")
        cls.addClassCleanup(tmp.cleanup)
        cls.build = Path(tmp.name)
        cls.compiler = compiler
        guest = (ROOT / "disk" / "modules" / "misc_hw_rtc.c").read_text()
        source = r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t UBYTE;
typedef uint32_t ULONG;
enum { CHIP_MSM = 1, CHIP_RIC = 2 };
enum { S1, S10, MI1, MI10, H1, H10, D1, D10, MO1, MO10, Y1, Y10, WD, NDIG };
enum { AMT_ST_PASS, AMT_ST_FAIL, AMT_ST_SKIP, AMT_ST_UNTESTABLE };
#define CD_HOLD 1
#define CF_24H 4
struct rt { UBYTE s, mi, h, d, mo, y, w, leap; };
static int chip, cur24 = 1, last_status;
static UBYTE pm_bit, sampled_leap, digits[NDIG];
static ULONG elapsed_ms;
static UBYTE controls[16], ric_md;
static int orig_md = -1;
static struct rt written;
static struct {
    UBYTE cd, ce, cf, md, sel, leap, ram[26];
    int is24, have_t0, running, ram_saved;
    struct rt t0;
    ULONG tk0;
} sv;
static void read_digits(UBYTE *g) { memcpy(g, digits, NDIG); }
static UBYTE ric_leap_read(void) { return sampled_leap; }
static int wait_edge(int ms) { (void)ms; assert(!(controls[13] & CD_HOLD)); return 1; }
static ULONG mh_ms_since(ULONG start) { (void)start; return elapsed_ms; }
static ULONG amt_cck_hz(void) { return 3546895; }
static ULONG amt_ticks(void) { return UINT32_MAX; }
static void clk_stop(void) {}
static void clk_start(void) {}
static void set24(int mode) { cur24 = mode; }
static UBYTE rr(int reg) { return controls[reg]; }
static void rw(int reg, UBYTE value) { controls[reg] = value; }
static void ric_bank(int bank) { (void)bank; }
static void ric_mode(UBYTE mode) { (void)mode; }
static void write_time(const struct rt *t) { written = *t; }
static void amt_case(const char *id, int status, const char *why)
{ (void)id; (void)why; last_status = status; }
static void amt_case_open(const char *id, int status) { amt_case(id, status, 0); }
static void amt_case_close(void) {}
static void json_str(const char *key, const char *value) { (void)key; (void)value; }
static void json_obj(const char *key) { (void)key; }
static void json_obj_end(void) {}
static void json_int(const char *key, long value) { (void)key; (void)value; }
static const char *fmt_time(const struct rt *t) { (void)t; return "time"; }
static int wr_ok = 1;
"""
        source += "\n".join(function(guest, name) for name in
                            ("is_leap", "mdays", "add_secs", "encode", "decode", "same",
                             "read_time", "msm_unlock", "save_state", "restore_state",
                             "case_edge_timeout", "t_restore_check"))
        source += r"""
static void sample(const struct rt *t)
{
    encode(t, 1, digits);
    sampled_leap = t->leap;
}
int main(int argc, char **argv)
{
    struct rt t = { 59, 59, 23, 28, 2, 23, 2, 3 }, got;
    UBYTE g[NDIG];
    unsigned poison, leap;
    assert(argc == 2);
    chip = CHIP_RIC;
    sv.have_t0 = sv.running = sv.is24 = 1;
    switch (argv[1][0]) {
    case 'h':
        chip = CHIP_MSM;
        sample(&t);
        for (int held = 0; held <= 1; held++) {
            controls[13] = held;
            controls[15] = CF_24H;
            save_state();
            assert(sv.cd == held && sv.running);
            assert(!(controls[13] & CD_HOLD));
            controls[13] = held;
            t_restore_check();
            assert(last_status == AMT_ST_PASS);
            assert(!(controls[13] & CD_HOLD));
        }
        break;
    case 'd':
        t.d = 29;
        for (poison = 0; poison < 256; poison += 17)
            for (leap = 0; leap < 4; leap++) {
                t.leap = leap;
                sample(&t);
                memset(&got, poison, sizeof(got));
                assert(read_time(&got, g) == (leap == 0));
                assert(got.leap == leap);
                t.y = 24;
                memset(&got, poison, sizeof(got));
                assert(read_time(&got, g) == (leap == 0));
                t.y = 23;
            }
        break;
    case 'm':
        chip = CHIP_MSM;
        t.d = 29; t.y = 24; t.leap = 3;
        sample(&t);
        assert(read_time(&got, g));
        t.y = 23; t.leap = 0;
        sample(&t);
        assert(!read_time(&got, g));
        break;
    case 'c':
        sv.t0 = t;
        elapsed_ms = 1000;
        t.s = t.mi = t.h = 0; t.d = t.mo = 1; t.mo = 3; t.w = 3;
        sample(&t);
        t_restore_check();
        assert(last_status == AMT_ST_PASS);
        t.d = 29; t.mo = 2;
        sample(&t);
        t_restore_check();
        assert(last_status == AMT_ST_FAIL);
        break;
    case 'l':
        sv.t0 = t; sv.t0.y = 24; sv.t0.leap = 0;
        elapsed_ms = 1000;
        t = sv.t0; t.s = t.mi = t.h = 0; t.d = 29; t.w = 3;
        sample(&t);
        t_restore_check();
        assert(last_status == AMT_ST_PASS);
        break;
    case 'y':
        t.mo = 12; t.d = 31; t.y = 99;
        sv.t0 = t; sv.leap = t.leap;
        elapsed_ms = 0;
        restore_state();
        assert(written.mo == 1 && written.d == 1 && written.y == 0 && written.leap == 0);
        sv.t0 = written;
        sv.leap = 3; /* snapshot before the edge; t0 was sampled after it */
        restore_state();
        assert(written.leap == 0);
        break;
    default: assert(0);
    }
    return 0;
}
"""
        cls.source = source
        cls.binary = cls.compile_program(source, "rtc")

    @classmethod
    def compile_program(cls, source, name):
        binary = cls.build / name
        compiled = subprocess.run(
            [cls.compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", "-x", "c", "-", "-o", str(binary)],
            input=source, text=True, capture_output=True, env={**os.environ, "TMPDIR": str(cls.build)})
        if compiled.returncode:
            raise AssertionError(compiled.stderr)
        return binary

    def test_ricoh_february_uses_sampled_counter_not_stack_or_year(self):
        subprocess.run([self.binary, "d"], check=True)

    def test_msm_february_still_uses_year_digits(self):
        subprocess.run([self.binary, "m"], check=True)

    def test_initial_msm_hold_is_saved_and_released_before_observing_counting(self):
        subprocess.run([self.binary, "h"], check=True)

    def test_common_february_restore_accepts_march_and_rejects_february_29(self):
        subprocess.run([self.binary, "c"], check=True)

    def test_leap_february_restore_accepts_february_29(self):
        subprocess.run([self.binary, "l"], check=True)

    def test_restore_carries_year_and_preserves_coherent_t0_counter(self):
        subprocess.run([self.binary, "y"], check=True)

    def test_regressions_reject_the_original_read_and_restore_mutants(self):
        mutations = [
            ("hold", "h", "rw(13, 0);", "rw(13, CD_HOLD);"),
            ("restore_hold", "h", "if (chip == CHIP_MSM)\n        msm_unlock();",
             "if (chip == CHIP_MSM)\n        rw(13, CD_HOLD);"),
            ("read", "d", "return decode(g, cur24, leap, t);",
             "int ok = decode(g, cur24, t->leap, t); t->leap = leap; return ok;"),
            ("restore", "c", "e0 = sv.t0;", "e0 = sv.t0; e0.leap = 0;"),
        ]
        for name, scenario, old, new in mutations:
            with self.subTest(name=name):
                self.assertIn(old, self.source)
                source = self.source.replace(old, new).replace(
                    "#include <assert.h>", "#define assert(x) do { if (!(x)) return 1; } while (0)")
                binary = self.compile_program(source, name)
                self.assertNotEqual(subprocess.run([binary, scenario]).returncode, 0)


class RTCEdgeRecords(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("native C compiler unavailable")
        tmp = tempfile.TemporaryDirectory(dir=ROOT / "work" / "tmp")
        cls.addClassCleanup(tmp.cleanup)
        cls.binary = Path(tmp.name) / "rtc-records"
        guest = (ROOT / "disk/modules/misc_hw_rtc.c").read_text()
        out = (ROOT / "disk/rt/out.c").read_text()
        source = r"""
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef uint32_t ULONG;
enum { AMT_ST_PASS, AMT_ST_FAIL, AMT_ST_SKIP, AMT_ST_UNTESTABLE };
struct { ULONG written; } *amt_mbox;
static void ser_write(const char *s, ULONG n) { assert(fwrite(s, 1, n, stdout) == n); }
static void mbox_write(const char *s, ULONG n) { (void)s; (void)n; }
static void mbox_beat(void) {}
"""
        source += function(out.replace("char *amt_utoa(", "static char *amt_utoa("), "amt_utoa")
        source += (ROOT / "disk/rt/crc32.c").read_text().replace('#include "rt.h"', "")
        source += out[out.index("#define LINE_MAX"):]
        source += r"""
static void amt_case_open(const char *id, int status) {
    json_begin("case");
    json_str("mod", "misc_hw"); json_str("case", id);
    json_str("st", status == AMT_ST_FAIL ? "fail" : status == AMT_ST_PASS ? "pass" : "untestable");
}
static void amt_case_close(void) { json_end(); }
static void amt_case(const char *id, int status, const char *why) {
    amt_case_open(id, status);
    if (why) json_str("why", why);
    amt_case_close();
}
#define NDIG 13
struct rt { int value; };
static struct { int running; } sv;
static int edge = 1, elapsed_ms = 3000;
static int read_time(struct rt *t, UBYTE *g) { (void)t; (void)g; return 1; }
static int wait_edge(int ms) { assert(ms == 2500); return edge; }
static ULONG amt_ticks(void) { return 100; }
static ULONG mh_ms_since(ULONG t) { assert(t == 100); return elapsed_ms; }
static void json_digits(const char *k, const UBYTE *g) { (void)k; (void)g; }
"""
        source += "\n".join(function(guest, name) for name in
                            ("case_edge_timeout", "t_digits_counting", "t_rate"))
        source += r"""
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (argv[1][0]) {
    case 'c': sv.running = 0; t_digits_counting(); break;
    case 'r': sv.running = 1; edge = 0; t_rate(); break;
    case 'p': sv.running = 1; t_digits_counting(); t_rate(); break;
    case 's': sv.running = 0; t_rate(); break;
    case 'h': case_edge_timeout("rtc_hold", "no second edge"); break;
    case 'o': case_edge_timeout("rtc_roll_minute", "no carry"); break;
    case 't': case_edge_timeout("rtc_restored", "no restored edge"); break;
    default: assert(0);
    }
    return 0;
}
"""
        p = subprocess.run([compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                            "-x", "c", "-", "-o", str(cls.binary)],
                           input=source, text=True, capture_output=True)
        if p.returncode:
            raise AssertionError(p.stderr)

    def test_timeout_records_are_schema_valid_and_keep_failure_verdicts(self):
        for scenario, case in (("c", "rtc_counting"), ("r", "rtc_rate"), ("h", "rtc_hold"),
                               ("o", "rtc_roll_minute"), ("t", "rtc_restored")):
            with self.subTest(case=case):
                p = subprocess.run([self.binary, scenario], check=True, capture_output=True)
                parsed = [records.parse_line(line) for line in p.stdout.splitlines()]
                self.assertTrue(all(r.crc_ok and not r.problems for r in parsed))
                r = next(r.obj for r in parsed if r.obj["case"] == case)
                self.assertEqual(r["st"], "fail")
                self.assertEqual(r["expect"], {"seconds_edges_min": 1, "within_ms": 2500})
                self.assertEqual(r["got"], {"seconds_edges": 0})

    def test_passes_and_dependency_untestable_are_unchanged(self):
        for scenario, expected in (("p", ["pass", "pass", "pass"]), ("s", ["untestable"])):
            p = subprocess.run([self.binary, scenario], check=True, capture_output=True)
            parsed = [records.parse_line(line) for line in p.stdout.splitlines()]
            self.assertTrue(all(r.crc_ok and not r.problems for r in parsed))
            self.assertEqual([r.obj["st"] for r in parsed], expected)

    def test_no_rtc_timeout_uses_schema_invalid_short_failure_path(self):
        guest = (ROOT / "disk/modules/misc_hw_rtc.c").read_text()
        self.assertNotRegex(guest, r"amt_case\([^;]*AMT_ST_FAIL")
        for name in ("t_digits_counting", "t_rate", "t_hold", "roll", "t_restore_check"):
            self.assertIn("case_edge_timeout(", function(guest, name))


if __name__ == "__main__":
    unittest.main()
