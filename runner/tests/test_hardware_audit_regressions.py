"""Native production-helper regressions; no emulator or guest is launched."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
MODULES = ROOT / "disk/modules"
PREAMBLE = r"""
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#define assert(x) do { if (!(x)) exit(1); } while (0)
typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef uint32_t ULONG;
typedef int32_t LONG;
"""

def function(source, name):
    match = re.search(r"^static [^\n]+\b" + name + r"\([^;]*?\)\s*\{",
                      source, re.MULTILINE)
    if not match:
        raise AssertionError(f"missing production helper {name}")
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


class HardwareAuditRegressions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = shutil.which("cc")
        if not cls.cc:
            raise unittest.SkipTest("native compiler unavailable")
        cls.build = ROOT / "work" / f"hardware-audit-native-{os.getpid()}"
        cls.build.mkdir(parents=True, exist_ok=True)
        cls.addClassCleanup(shutil.rmtree, cls.build)
        cls.serial = 0

    def compile(self, source):
        type(self).serial += 1
        binary = self.build / str(self.serial)
        result = subprocess.run(
            [self.cc, "-std=c99", "-Wall", "-Wextra", "-x", "c", "-", "-o", str(binary)],
            input=source, text=True, capture_output=True,
            env={**os.environ, "TMPDIR": str(self.build)},
        )
        self.assertEqual(result.returncode, 0, result.stderr)
        return binary

    def check(self, source, mutations=()):
        self.assertEqual(subprocess.run([self.compile(source)]).returncode, 0)
        for before, after in mutations:
            with self.subTest(mutation=before):
                self.assertIn(before, source)
                self.assertNotEqual(
                    subprocess.run([self.compile(source.replace(before, after))]).returncode, 0)

    def test_domains_disjoint_intervals_and_independent_observation_gates(self):
        source = PREAMBLE + (MODULES / "hardware_audit.h").read_text() + r"""
int main(void) {
    assert(hw_owned_domain(1760, 0, 1760 * 512));
    assert(hw_owned_domain(1760, 2, 1760 * 512));
    assert(!hw_owned_domain(1760, 0x00800000, 1760 * 512));
    assert(!hw_owned_domain(0x00800004, 2, 2048));
    assert(!hw_owned_domain(1760, 1759, 1760 * 512));
    assert(!hw_owned_domain(3, 2, 1536));
    assert(hw_owner_match(1760, 2, 512, 1, 1760, 2, 512, 1));
    assert(!hw_owner_match(1760, 2, 512, 1, 1760, 1000, 512, 1));
    assert(!hw_owner_match(1760, 2, 512, 1, 1760, 2, 1024, 1));
    assert(!hw_owner_match(1760, 2, 512, 1, 1760, 2, 512, 4));
    assert(hw_modulator_period(2800) && hw_modulator_period(3600));
    assert(hw_modulator_period(6000) && hw_modulator_period(6800));
    assert(!hw_modulator_period(2799) && !hw_modulator_period(6801));
    assert(!hw_modulator_period(4800) && !hw_modulator_period(-1));
    assert(hw_latency_observation_valid(32, 32, 0, 0, 0));
    assert(!hw_latency_observation_valid(32, 32, 32, 0, 0));
    assert(!hw_latency_observation_valid(32, 32, 0, 1, 0));
    assert(!hw_latency_observation_valid(64, 32, 0, 0, 1));
    assert(!hw_latency_observation_valid(0, 0, 0, 0, 0));
    assert(hw_bfd_order(1, 1, 0) == -1);
    assert(hw_bfd_order(1, 0, 1) == 1);
    assert(hw_bfd_order(1, 0, 0) == 0);
    assert(hw_bfd_order(0, 0, 1) == 0);
    assert(hw_keyboard_window(0, 99 * 3547));
    assert(!hw_keyboard_window(0, 100 * 3547));
    assert(!hw_keyboard_window(0, 200 * 3547));
    assert(hw_keyboard_window(UINT32_MAX - 100, 100));
    assert(hw_frame_endpoint(312, 312, 0, 39));
    assert(!hw_frame_endpoint(304, 304, 0, 400));
    assert(!hw_frame_endpoint(312, 310, 0, 20));
    return 0;
}
"""
        self.check(source, [
            ("total <= bytes / 512", "(ULONG)(total * 512) <= bytes"),
            ("first < total - 1", "1"),
            ("first == first2", "1"),
            ("bsize2 == 512", "1"),
            ("kind == kind2", "1"),
            ("(t >= 2800 && t <= 3600) || (t >= 6000 && t <= 6800)",
             "(t >= 2800 && t <= 6800)"),
            ("&& !torn", ""),
            ("&& !ambiguous", ""),
            ("busy_after ? -1 : saw_done_clear ? 1 : 0",
             "busy_after ? -1 : 1"),
            ("&& elapsed < 40", ""),
        ])

    def test_button_first_match_at_deadline_is_not_stability(self):
        source = PREAMBLE + (MODULES / "hardware_audit.h").read_text() + r"""
static ULONG frame, first;
static int flicker;
static ULONG amt_frames_now(void) { return frame++; }
static int sample(int which) {
    (void)which;
    return frame >= first && (!flicker || frame % 2);
}
"""
        source += function((MODULES / "input.c").read_text(), "wait_level")
        source += r"""
int main(void) {
    frame = 0; first = 48; assert(wait_level(sample, 0, 1, 0) == 1);
    frame = 0; first = 50; assert(wait_level(sample, 0, 1, 0) == 0);
    frame = 0; first = 1; flicker = 1; assert(wait_level(sample, 0, 1, 0) == 0);
    return 0;
}
"""
        self.check(source, [("if (now - f0 >= 50) {\n            ok = 0;",
                             "if (now - f0 >= 50) {")])

    def test_crc_valid_pcmcia_marker_pairs_must_agree(self):
        crc = (ROOT / "disk/rt/crc32.c").read_text().replace('#include "rt.h"', "")
        source = PREAMBLE + (MODULES / "hardware_audit.h").read_text() + crc + r"""
static UBYTE blocks[1024];
static UBYTE hw_rd8(ULONG addr) { assert(addr < sizeof blocks); return blocks[addr]; }
static void put(UBYTE *b, int off, ULONG v) {
    b[off] = v >> 24; b[off+1] = v >> 16; b[off+2] = v >> 8; b[off+3] = v;
}
static void marker(UBYTE *b, ULONG lba, ULONG first, ULONG size, ULONG kind) {
    memset(b, 0, 512); memcpy(b, "AMTOWN01", 8);
    put(b, 24, lba); put(b, 28, 4); put(b, 32, size);
    put(b, 36, first); put(b, 40, kind); put(b, 508, crc32_update(0, b, 508));
}
"""
        source += function((MODULES / "misc_hw_gayle.c").read_text(), "sig_ok")
        source += r"""
int main(void) {
    UBYTE uuid[16]; ULONG total = 0, first = 0;
    marker(blocks, 0, 2, 512, 4);
    marker(blocks+512, 3, 2, 512, 4);
    assert(sig_ok(0, 0, uuid, &total, &first));
    assert(sig_ok(512, 3, uuid, &total, &first));
    marker(blocks+512, 3, 1000, 512, 4);
    assert(!sig_ok(512, 3, uuid, &total, &first));
    marker(blocks+512, 3, 2, 1024, 4);
    assert(!sig_ok(512, 3, uuid, &total, &first));
    marker(blocks+512, 3, 2, 512, 1);
    assert(!sig_ok(512, 3, uuid, &total, &first));
    return 0;
}
"""
        self.check(source)

    def test_timeout_initialization_is_transactional_even_for_signal_zero(self):
        source = PREAMBLE + r"""
struct MsgPort { int mp_SigBit; };
struct Message { struct MsgPort *mn_ReplyPort; unsigned mn_Length; };
struct Library { int lib_Version; };
struct Device { struct Library dd_Library; };
struct Request { struct Message io_Message; struct Device *io_Device; };
struct timerequest { struct Request tr_node; };
struct EClockVal { ULONG ev_lo; };
typedef const char *CONST_STRPTR;
struct IORequest;
#define TIMERNAME "timer.device"
#define UNIT_VBLANK 1
static struct MsgPort tport, cport;
static struct timerequest treq, creq;
static struct Device device, *TimerBase;
static int timer_ok, have_eclock, fail_port, fail_open, calls, freed, closed;
static ULONG clk_hz, frequency;
static int port_init(struct MsgPort *p) {
    int n = ++calls; if (n == fail_port) return 0; p->mp_SigBit = n-1; return 1;
}
static void FreeSignal(int bit) { freed |= 1 << bit; }
static int OpenDevice(CONST_STRPTR n, int u, struct IORequest *r, int f) {
    (void)n; (void)u; (void)r; (void)f; treq.tr_node.io_Device = &device; return fail_open;
}
static void CloseDevice(struct IORequest *r) { (void)r; closed++; }
static ULONG ReadEClock(struct EClockVal *ev) { ev->ev_lo = 0; return frequency; }
static void reset(void) {
    fail_port = fail_open = calls = freed = closed = 0;
    timer_ok = have_eclock = 1; clk_hz = 123;
    device.dd_Library.lib_Version = 36; frequency = 709379;
}
"""
        devio = (MODULES / "devio.c").read_text()
        source += function(devio, "timer_open") + function(devio, "timer_close")
        source += r"""
int main(void) {
    reset(); fail_port = 1;
    assert(!timer_open() && calls == 1 && !freed && !timer_ok && !clk_hz);
    reset(); fail_port = 2;
    assert(!timer_open() && calls == 2 && freed == 1 && !timer_ok && !clk_hz);
    reset(); fail_open = 1;
    assert(!timer_open() && freed == 3 && !timer_ok && !clk_hz);
    reset(); assert(timer_open() && timer_ok && have_eclock && clk_hz == frequency);
    assert(!freed); timer_close(); assert(closed == 1 && !timer_ok);
    reset(); frequency = 0;
    assert(timer_open() && !have_eclock && clk_hz == 1000000);
    reset(); device.dd_Library.lib_Version = 34;
    assert(timer_open() && !have_eclock && clk_hz == 1000000);
    return 0;
}
"""
        self.check(source)
        run = devio[devio.index("void amt_run_devio"):]
        self.assertLess(run.index("if (!timer_open())"), run.index("find_devices();"))
        self.assertIn('return;\n    }\n    find_devices();', run)

    def test_denise_probe_ignores_upper_bits_and_requires_independent_identity(self):
        source = PREAMBLE + r"""
struct GfxBase { UBYTE ChipRevBits0; };
struct Library;
typedef const char *CONST_STRPTR;
#define GFXF_HR_DENISE 2
static struct GfxBase gfx;
static int os_version = 36, serial, unstable;
static UWORD agnus, low;
static UWORD read_denise(void) {
    serial++; return low | ((serial & 1) << 8) | (unstable && (serial & 1) ? 1 : 0);
}
#define VPOSR agnus
#define DENISEID read_denise()
static struct { ULONG agnus_id, denise_id; UBYTE ecs_agnus, ecs_denise, pal; const char *chipset; } result, *P = &result;
static struct { int VBlankFrequency; } exec, *SysBase = &exec;
static struct Library *OpenLibrary(CONST_STRPTR name, int version) {
    (void)name; return os_version >= version ? (struct Library *)&gfx : 0;
}
static void CloseLibrary(struct Library *lib) { (void)lib; }
static void how_begin(void) {}
static void how_end(void) {}
static void how_s(const char *s) { (void)s; }
static void how_x(ULONG x, int w) { (void)x; (void)w; }
static void how_d(LONG x) { (void)x; }
"""
        source += function((ROOT / "disk/probe/probe.c").read_text(), "probe_chips")
        source += r"""
int main(void) {
    agnus = 0x2200; low = 0xf8; probe_chips();
    assert(P->ecs_denise && P->denise_id == 0xf8 && !strcmp(P->chipset, "AGA"));
    agnus = 0x2000; low = 0xfc; gfx.ChipRevBits0 = GFXF_HR_DENISE; probe_chips();
    assert(P->ecs_denise && P->denise_id == 0xfc);
    gfx.ChipRevBits0 = 0; probe_chips(); /* ECS Agnus can still have OCS Denise. */
    assert(!P->ecs_denise && P->denise_id == 0xffff);
    agnus = 0; gfx.ChipRevBits0 = GFXF_HR_DENISE; probe_chips(); /* real upgrade */
    assert(P->ecs_denise && P->denise_id == 0xfc);
    unstable = 1; probe_chips(); assert(!P->ecs_denise);
    unstable = 0; os_version = 34; probe_chips(); assert(!P->ecs_denise);
    return 0;
}
"""
        self.check(source, [
            ("(v & 0xff) != (d & 0xff)", "v != d"),
            ("os_ecsd && stable", "stable"),
        ])

    def test_pcmcia_reinsertion_acknowledgement_and_presence_are_both_required(self):
        source = PREAMBLE + r"""
enum { AMT_ST_PASS, AMT_ST_FAIL, AMT_ST_SKIP, AMT_ST_UNTESTABLE };
#define G_IRQ 0
#define G_CS 1
static int card_is_ram, stage, insert_ack, present, status;
static ULONG ms;
static UBYTE hw_rd8(ULONG addr) {
    return addr == G_IRQ ? (stage ? 0x40 : 0) : (stage == 2 && present ? 0x40 : 0);
}
static void hw_wr8(ULONG addr, UBYTE v) { (void)addr; (void)v; }
static ULONG amt_secs(ULONG n) { return n; }
static ULONG amt_ticks(void) { ms = 0; return 0; }
static ULONG mh_ms_since(ULONG t) { (void)t; return ms += 1000; }
static void amt_watchdog_kick(void) {}
static int amt_request(const char *kind, const char *tag, const char *args, ULONG secs) {
    (void)kind; (void)args; (void)secs;
    if (!strcmp(tag, "eject")) { stage = 1; return 1; }
    stage = 2; return insert_ack;
}
static void amt_case(const char *id, int st, const char *why) { (void)id; (void)why; status = st; }
static void amt_case_open(const char *id, int st) { amt_case(id, st, 0); }
static void amt_case_close(void) {}
static void json_obj(const char *k) { (void)k; }
static void json_obj_end(void) {}
static void json_str(const char *k, const char *v) { (void)k; (void)v; }
static void json_hexn(const char *k, ULONG v, int n) { (void)k; (void)v; (void)n; }
"""
        source += function((MODULES / "misc_hw_gayle.c").read_text(), "t_status_change")
        source += r"""
int main(void) {
    stage = 0; insert_ack = present = 1; t_status_change(); assert(status == AMT_ST_PASS);
    stage = 0; insert_ack = 0; present = 1; t_status_change(); assert(status == AMT_ST_FAIL);
    stage = 0; insert_ack = 1; present = 0; t_status_change(); assert(status == AMT_ST_FAIL);
    return 0;
}
"""
        self.check(source, [
            ("!inserted || !(hw_rd8(G_CS) & 0x40)", "!(hw_rd8(G_CS) & 0x40)"),
            ("!inserted || !(hw_rd8(G_CS) & 0x40)", "!inserted"),
        ])

    def test_copper_waits_bound_stalls_not_total_fast_cpu_polls(self):
        source = PREAMBLE + r"""
#define AMT_ABORT_WATCHDOG 2
#define INTREQR 0
static ULONG reads, polls_per_line;
static UWORD start_line, frame_lines, marker_line, regs[1], sr;
static int frozen, masked;
static jmp_buf abort_point;
#define CU(r) regs[r]
static UWORD hw_vline(void) {
    UWORD v = frozen ? start_line :
              (start_line + reads / polls_per_line) % frame_lines;
    reads++;
    if (v >= marker_line) regs[0] = 4;
    return v;
}
static ULONG hw_beam(void) { return (ULONG)hw_vline() << 8; }
static ULONG hw_beam_fix(ULONG p, ULONG prev) { (void)prev; return p; }
static ULONG amt_frames_now(void) {
    return masked || frozen ? 0 : (start_line + reads / polls_per_line) / frame_lines;
}
static ULONG amt_ticks(void) {
    return frozen ? 0 : reads / polls_per_line * 227;
}
static void amt_set_sr(UWORD value) { sr = value; }
static void amt_abort(int code) {
    assert(code == AMT_ABORT_WATCHDOG);
    longjmp(abort_point, 1);
}
static void reset(UWORD lines, UWORD start, ULONG rate, int stopped) {
    frame_lines = lines; start_line = start; polls_per_line = rate; frozen = stopped;
    reads = 0; regs[0] = 0; marker_line = 0xffff; sr = 0x2700; masked = 0;
}
"""
        copper = (MODULES / "copper.c").read_text()
        source += "\n".join(function(copper, name) for name in (
            "wait_progress", "wait_vbl", "wait_line", "wait_frame_wrap", "poll"))
        source += r"""
int main(void) {
    ULONG lo, hi;
    reset(313, 64, 6000, 0);
    wait_line(304);
    assert(reads > 1000000 && reads < 1500000 && sr == 0x2700);
    reset(263, 64, 6000, 0);
    wait_line(256);
    assert(reads > 1000000 && reads < 1200000);
    reset(313, 300, 6000, 0);
    wait_frame_wrap(); wait_line(16);
    assert(reads > 78000 && reads < 180000);
    reset(263, 250, 6000, 0);
    wait_frame_wrap(); wait_line(16);
    assert(reads > 78000 && reads < 180000);
    reset(313, 64, 6000, 0);
    assert(wait_vbl() && reads > 1000000 && reads < 1600000);
    reset(313, 64, 1, 0); masked = 1;
    assert(!wait_vbl() && reads < 1500);
    reset(313, 64, 6000, 0); marker_line = 290;
    assert(poll(4, 304UL << 8, &lo, &hi));
    assert(reads > 1000000 && lo <= hi && hi >= 290UL << 8);
    reset(313, 64, 1, 0);
    assert(!poll(4, 100UL << 8, &lo, &hi) && reads < 100);
    reset(313, 64, 1, 0);
    if (!setjmp(abort_point)) { wait_line(500); assert(0); }
    assert(sr == 0x2000 && reads < 1500);
    for (int kind = 0; kind < 4; kind++) {
        reset(313, 64, 1, 1);
        if (!setjmp(abort_point)) {
            if (kind == 0) wait_line(304);
            else if (kind == 1) wait_frame_wrap();
            else if (kind == 2) wait_vbl();
            else poll(4, 304UL << 8, &lo, &hi);
            assert(0);
        }
        assert(sr == 0x2000 && reads <= 1000002);
    }
    return 0;
}
"""
        self.check(source, [
            ("*budget = 1000000;", "*budget = 2;"),
            ("if (now != last)", "if (0)"),
            ("++wraps > 3", "++wraps > 30"),
        ])

    def test_copen_requires_a_valid_trial_even_if_last_markers_look_correct(self):
        source = PREAMBLE + r"""
enum { AMT_ST_PASS, AMT_ST_FAIL, AMT_ST_UNTESTABLE };
#define OFF_L1 0
#define MK_A 4
#define DMACON 0
#define DMAF_COPEN 0x80
#define DMAF_SET 0x8000
static UWORD regs[1], line;
#define CU(r) regs[r]
static int status, reads;
static void lst(int n) { (void)n; }
static void cwait(int v, int h) { (void)v; (void)h; }
static void cmark(int m) { (void)m; }
static void cend(void) {}
static void mk_clear(void) { reads = 0; }
static void sync_line(int n) { (void)n; }
static void wait_line(int n) { (void)n; }
static void cop_start(int n) { (void)n; }
static UWORD hw_vline(void) { return line; }
static UWORD mk_get(void) { return reads++ ? MK_A : 0; }
static void cop_idle(void) {}
static void unsync(void) {}
static void amt_case(const char *id, int st, const char *why) { (void)id; (void)why; status = st; }
static void amt_case_open(const char *id, int st) { amt_case(id, st, 0); }
static void amt_case_close(void) {}
static void json_obj(const char *k) { (void)k; }
static void json_obj_end(void) {}
static void json_str(const char *k, const char *v) { (void)k; (void)v; }
static void json_int(const char *k, LONG v) { (void)k; (void)v; }
static void json_hexn(const char *k, ULONG v, int n) { (void)k; (void)v; (void)n; }
"""
        source += function((MODULES / "copper.c").read_text(), "copen_test")
        source += r"""
int main(void) {
    line = 0x60; copen_test(); assert(status == AMT_ST_PASS);
    line = 0x80; copen_test(); assert(status == AMT_ST_UNTESTABLE);
    return 0;
}
"""
        self.check(source, [("if (!valid) {", "if (0) {")])

    def test_step_motion_accounting_survives_both_abort_windows(self):
        source = PREAMBLE + r"""
#define PRB_DIR 2
#define PRB_STEP 1
#define PA_MS(n) ((ULONG)(n) * 3547)
#define R_DSKLEN 0
#define R_DMACON 1
#define R_INTENA 2
#define R_INTREQ 3
#define R_DSKSYNC 4
#define INT_DSKBLK 2
#define INT_DSKSYN 0x1000
#define SYNC 0x4489
static UWORD regs[5];
#define CREG(r) regs[r]
static void *amt_irq_hook[8];
static UBYTE prb, prb0;
static int cur_cyl, rs_disp, rs_active, rs_drv, rs_known;
static UWORD sr;
static ULONG interrupt_at;
static jmp_buf abort_point;
static UWORD amt_get_sr(void) { return sr; }
static void amt_set_sr(UWORD value) { sr = value; }
static void prb_w(UBYTE b) { prb = b; }
static void pa_wait_cc(ULONG cc) {
    if (cc == interrupt_at) { interrupt_at = 0; longjmp(abort_point, 1); }
}
static void drv_select(int d, int motor) { (void)d; (void)motor; }
static void drv_motor_off(int d) { (void)d; }
static void drv_deselect(void) {}
static void settle(void) {}
"""
        disk = (MODULES / "paula_disk.c").read_text()
        source += function(disk, "step1")
        restore = disk[disk.index("void pa_disk_restore(void)"):]
        source += restore[:restore.index("\n}") + 2]
        source += r"""
static void scenario(ULONG stop) {
    prb = prb0 = 0xff; cur_cyl = 10; rs_disp = 0; rs_active = 1;
    rs_known = 1; rs_drv = -1; sr = 0x2000; interrupt_at = stop;
    if (!setjmp(abort_point)) step1(1);
    if (stop == 16) assert(rs_disp == 0 && !(prb & PRB_STEP));
    else assert(rs_disp == -1 && (prb & PRB_STEP));
    pa_disk_restore();
    assert(rs_disp == -1 && cur_cyl == 9 && prb == prb0 && !rs_active);
    assert(sr == 0x2000);
}
int main(void) { scenario(16); scenario(PA_MS(4)); return 0; }
"""
        self.check(source, [
            ("rs_disp += out ? -1 : 1;\n    amt_set_sr(sr);\n    pa_wait_cc(PA_MS(4));",
             "amt_set_sr(sr);\n    pa_wait_cc(PA_MS(4));\n    rs_disp += out ? -1 : 1;"),
            ("rs_disp += (prb & PRB_DIR) ? -1 : 1;", ""),
        ])

    def test_assembly_producers_bound_before_any_buffer_store(self):
        irq = (MODULES / "interrupts_asm.s").read_text()
        for label, store in (("_ir_lat2:", "move.b\tCIAA_TAHI,(a0)+"),
                             ("_ir_lat3:", "move.w\tVHPOSR,(a0)")):
            body = irq[irq.index(label):]
            self.assertLess(body.index("cmp.w\t#64,_ir_lat_n"), body.index(store))
            self.assertLess(body.index("bhs.s\t.full"), body.index(store))
            self.assertIn("move.w\t#1,_ir_lat_overflow", body)
        paula = (MODULES / "paula_asm.s").read_text()
        self.assertEqual(paula.count("move.w\t#255,d3"), 2)
        self.assertEqual(paula.count(".retry:\tdbf\td3,.t"), 2)
        self.assertEqual(paula.count("move.w\t#1,_pa_sample_invalid"), 2)


if __name__ == "__main__":
    unittest.main()
