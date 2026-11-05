"""Native production-helper/model checks; these do not run a guest or emulator."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest

ROOT = Path(__file__).resolve().parents[2]
MOD = ROOT / "disk" / "modules"


def function(source, name):
    match = re.search(r"^(?:static )?(?:inline )?[^\n;]+\b" + name +
                      r"\([^;]*?\)\s*\{", source, re.MULTILINE)
    if not match:
        raise AssertionError(f"missing production function {name}")
    end, depth = match.end(), 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[match.start():end]


class GuestRTG(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = shutil.which("cc")
        if not cls.cc:
            raise unittest.SkipTest("native C compiler unavailable")
        cls.build = ROOT / "work" / f"rtg-native-{os.getpid()}"
        cls.build.mkdir(parents=True, exist_ok=True)
        cls.addClassCleanup(shutil.rmtree, cls.build)
        cls.files = {name: (MOD / name).read_text() for name in
                     ("rtg.h", "rtg.c", "rtg_s3.c", "rtg_cirrus.c", "rtg_misc.c")}
        h, common, s3, cl, misc = (cls.files[n] for n in cls.files)
        types = "\n".join(re.findall(r"enum\s*\{[^}]+\};", h))
        types += "\n".join(re.findall(
            r"struct (?:rtg_win|rtg_card|vga_state)\s*\{[^}]+\};", h))
        prelude = r"""
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#define assert(x) do { if (!(x)) { fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x); exit(91); } } while (0)
typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef uint32_t ULONG;
typedef int32_t LONG;
enum { AMT_ST_PASS, AMT_ST_FAIL, AMT_ST_SKIP, AMT_ST_UNTESTABLE };
#define AMT_ABORT_UNEXPECTED 1
#define RTG_WORK 65536UL
#define WALK_MAX 256
"""
        source = prelude + types + r"""
static struct rtg_card card, *rtg_cur = &card;
static UBYTE vram[0x400000], buffers[16384], *rtg_buf = buffers;
static ULONG walk_save[WALK_MAX], walk_pending;
static volatile ULONG *alias_pending;
static ULONG alias_saved[2];
static int alias_longs;
static int status, aborts;
static int vblank_status = -1, skips, zz_reads;
static UWORD fw;
static ULONG frames;
static int monitor_pending, monitor_off;
static void rtg_monswitch(int on) { assert(!on); monitor_off++; }
static ULONG expected, actual;
static const char *why_seen;
static void amt_case_open(const char *id, int st) { status = st; if (!strcmp(id, "zz_vblank")) vblank_status = st; }
static void amt_case_close(void) {}
static const char *rtg_cid(const char *s) { return s; }
static void rtg_pass(const char *s) { status = AMT_ST_PASS; if (!strcmp(s, "zz_vblank")) vblank_status = status; }
static void rtg_skip(const char *s, const char *why) { (void)why; skips++; if (!strcmp(s, "zz_vblank")) vblank_status = AMT_ST_SKIP; }
static void json_obj(const char *s) { (void)s; }
static void json_obj_end(void) {}
static void json_str(const char *s, const char *v) { (void)s; (void)v; }
static void json_int(const char *s, long v) { (void)s; (void)v; }
static void json_hex(const char *s, ULONG v) { (void)s; (void)v; }
static void json_hexn(const char *s, ULONG v, int n) { (void)s; (void)v; (void)n; }
static void rtg_ctx_card(void) {}
static void vram_fail(const char *id, const char *why, ULONG o, int w, ULONG e, ULONG g, const char *win)
{ (void)id; (void)o; (void)w; (void)win; status = AMT_ST_FAIL; expected = e; actual = g; why_seen = why; }
static void amt_abort(int code) { (void)code; aborts++; exit(92); }
static ULONG rng = 1;
static ULONG amt_rand(void) { rng = rng * 1664525u + 1013904223u; return rng; }
static ULONG amt_frames_now(void) { return frames++; }
static UWORD zz_read(ULONG offset) {
    if (offset == 0xc0) return fw;
    if (offset == 0x4c) return (++zz_reads & 1) ? 0x20 : 0;
    return 1;
}
#define ZZR(o) zz_read(o)
static UWORD host_ctl;
#define HCTL host_ctl
static struct vga_state sst;
static UBYTE seq[32], misc_reg, clock_lo, clock_hi;
static int loads, misc_writes;
static void mock_sr(UBYTE i, UBYTE v) {
    if (i == 0x15 && (v & 0x20)) {
        clock_lo = seq[0x12]; clock_hi = seq[0x13]; loads++;
    }
    seq[i] = v;
}
#define SR_W(i,v) mock_sr(i,v)
static void vo(UWORD port, UBYTE v) {
    assert(port == 0x3c2); misc_reg = v; misc_writes++;
    if ((v & 0x0c) == 0) { seq[0x12] = 0x61; seq[0x13] = 0x6d; }
}
static ULONG mmio[0xb000 / 4], writes[32];
static int nwrites, executions;
static int s3_engine_saved, s3_engine, untestable;
static ULONG virge_cmdset, virge_regs[8];
static UWORD trio_regs[10], trio_mf[7], trio_subsys;
static volatile ULONG *virge_mmio(void) { return mmio; }
static void ew(UWORD port, UWORD v) { (void)port; (void)v; }
static void trio_require_idle(void) {}
static void rtg_untestable(const char *id, const char *why) { (void)id; (void)why; untestable++; }
static UBYTE pixels[65536], original[65536];
static void mm_write(ULONG *m, int reg, ULONG value) {
    int x, y, w, h, sx, sy, dx, dy;
    writes[nwrites++] = reg; m[reg / 4] = value;
    if ((reg == 0xa500 && !(value & 1)) ||
        (reg == 0xa50c && (m[0xa500 / 4] & 1))) {
        ULONG cmd = m[0xa500 / 4];
        if (((cmd >> 27) & 15) == 15) return;
        executions++;
        w = (m[0xa504 / 4] >> 16) + 1; h = m[0xa504 / 4] & 0xffff;
        sx = m[0xa508 / 4] >> 16; sy = m[0xa508 / 4] & 0xffff;
        dx = m[0xa50c / 4] >> 16; dy = m[0xa50c / 4] & 0xffff;
        assert(w == 40 && h == 12 && sx == 8 && sy == 4 && dx == 320 && dy == 40);
        for (y = 0; y < h; y++) for (x = 0; x < w; x++)
            pixels[(dy+y)*640+dx+x] = pixels[(sy+y)*640+sx+x];
    }
}
"""
        for name in ("vga_port",):
            source += function(h, name) + "\n"
        for name in ("setup_card", "in_board", "layout_fits", "vram_walk",
                     "rtg_vram_restore", "rtg_vram_tests", "rtg_seed_rect", "rtg_monitor_restore"):
            if name == "rtg_vram_tests":
                source += r"""
static void vram_bwl(void) {}
static void vram_addr_lines(void) {}
static void vram_swap(int i) { (void)i; }
"""
            source += function(common, name) + "\n"
        for src, names in ((cl, ("rop8",)), (s3, ("mix8", "rop3", "s3_cr_used", "s3_clock_restore")),
                           (misc, ("hctl", "rtg_zz9000"))):
            source += "\n".join(function(src, name) for name in names) + "\n"
        issue = function(s3, "virge_issue")
        # Instrument the actual volatile stores; no replacement command routine.
        issue = re.sub(r"m\[(0x[0-9a-f]+) / 4\] = ([^;]+);",
                       r"mm_write((ULONG *)m, \1, \2);", issue)
        source += issue
        for array in ("trio_ports", "virge_offsets"):
            source += re.search(r"static const UWORD " + array + r"\[[^\]]+\] = \{[^}]+\};", s3).group() + "\n"
        restore = function(s3, "s3_restore_engine")
        restore = re.sub(r"m\[(0x[0-9a-f]+) / 4\] = ([^;]+);",
                         r"mm_write((ULONG *)m, \1, \2);", restore)
        restore = restore.replace("m[virge_offsets[i] / 4] = virge_regs[i];",
                                  "mm_write((ULONG *)m, virge_offsets[i], virge_regs[i]);")
        source += restore
        board_section = common[common.index("struct cdef {"):common.index("static int board_bus")]
        source += r"""
#define AMT_PROBE_BOARDS_MAX 16
struct amt_probe_board { ULONG addr, size, man, prod, flags; };
static struct { int n_boards; struct amt_probe_board boards[16]; } amt_probe_result;
static int amt_board_ok(const struct amt_probe_board *b) { return b->addr && !b->flags; }
static int claim_calls, claim_allowed[16];
static int amt_board_claim(const struct amt_probe_board *b) {
    int i; claim_calls++;
    for (i = 0; i < amt_probe_result.n_boards; i++)
        if (b == &amt_probe_result.boards[i]) return claim_allowed[i];
    return 0;
}
static struct rtg_card cards[8];
static int ncards, nrej;
"""
        source += board_section
        source += r"""
static const struct cdef *rejected[8];
static ULONG rej_addr[8], rej_flags[8];
"""
        for name in ("board_bus", "add_card", "known_board", "find_cards", "claim_card"):
            source += function(common, name) + "\n"
        source += r"""
static void test_rop(void) {
    unsigned s, d, i;
    static const UBYTE cl_codes[16] = {0,5,6,9,11,13,14,80,89,109,144,149,173,208,214,218};
    /* Truth-table nibbles indexed by (S<<1)|D; independently encoded,
       rather than copying the production switch operators. */
    static const UBYTE cl_truth[16] = {0,8,10,4,5,12,15,2,6,14,1,9,13,3,11,7};
    static const UBYTE s3_truth[16] = {5,0,15,10,3,6,9,12,7,11,13,14,8,2,4,1};
    for (s = 0; s < 256; s++) for (d = 0; d < 256; d++) {
        for (i = 0; i < 16; i++) {
            UBYTE a = 0, b = 0; unsigned bit;
            for (bit = 0; bit < 8; bit++) {
                unsigned t = (((s >> bit)&1)<<1) | ((d >> bit)&1);
                a |= ((cl_truth[i] >> t)&1) << bit;
                b |= ((s3_truth[i] >> t)&1) << bit;
            }
            assert(rop8(cl_codes[i], s, d) == a);
            assert(mix8(i, s, d) == b);
            assert(rop3(cl_truth[i] * 17, s, d) == a);
        }
    }
}
static void test_ports(void) {
    card.kind = K_PICASSO2;
    assert(vga_port(0x3c9, 0) == 0x13c8);
    assert(vga_port(0x3c9, 1) == 0x13c8);
    assert(vga_port(0x3c7, 1) == 0x13c6);
    assert(vga_port(0x3c7, 0) == 0x3c7);
    assert(vga_port(0x3c8, 1) == 0x3c8);
    card.kind = K_CV643D_Z3; card.ioxor = 3;
    assert(vga_port(0x3c9, 1) == (0x3c9 ^ 3));
}
static void test_layout(void) {
    const char *what = 0;
    card.kind = K_PICASSO4_Z3; card.mem = (UBYTE *)(uintptr_t)0x10000000;
    card.memsize = 0x4000000;
    assert(!setup_card(&card));       /* unverified 64 MB variant */
    card.memsize = 0x2000000; card.chip = CH_CIRRUS;
    assert(setup_card(&card) && layout_fits(&card, &what));
    assert((uintptr_t)card.win[0].p == 0x11000000);
    memset(&card, 0, sizeof(card));
    card.kind = K_ZZ9000; card.chip = CH_ZZ9000; card.bus = 3;
    card.mem = (UBYTE *)(uintptr_t)0x10000000; card.memsize = 0x20000;
    assert(setup_card(&card));
    assert(card.win[0].size == 0x10000 && layout_fits(&card, &what));
    card.memsize = 0x800000;
    assert(setup_card(&card)); assert(card.win[0].size == 0x400000);
    card.bus = 2; assert(setup_card(&card)); assert(card.win[0].size == 0x200000);
    card.memsize = 0xffff; assert(!setup_card(&card));
}
static void test_minimum(void) {
    card.kind = K_PICASSO2; card.win[0].p = vram;
    card.win[0].size = card.vram_max = 65536; card.vram_min = 0x100000;
    status = -1; vram_walk(); assert(status == AMT_ST_FAIL);
    card.kind = K_CV643D_Z2; card.win[0].size = card.vram_max = 0x3a0000;
    card.vram_min = 0x400000; vram_walk(); assert(status == AMT_ST_PASS);
    card.win[0].size = card.vram_max = 65536;
    vram_walk(); assert(status == AMT_ST_FAIL);
}
static void test_alias(void) {
    card.nwin = 2; card.win[0].p = vram; card.win[1].p = vram + 65536;
    card.win[1].name = "alias";
    *(ULONG *)(vram + 0x3450) = 0x12345678;
    *(ULONG *)(vram + 65536 + 0x3450) = 0x600dcafe; /* first direction appears good */
    rtg_vram_tests();
    assert(status == AMT_ST_FAIL && expected == 0x0badf00d && actual == 0x600dcafe);
    assert(strstr(why_seen, "alias-to-natural"));
    assert(*(ULONG *)(vram + 0x3450) == 0x12345678);
    assert(*(ULONG *)(vram + 65536 + 0x3450) == 0x600dcafe);
}
static void test_host(void) {
    unsigned x, t;
    for (x = 0; x < 65536; x++) for (t = 0; t < 8; t++) {
        hctl(x, t << 11);
        assert((host_ctl & 0x8207) == (x & 0x8207));
        assert((host_ctl & 0x3800) == t << 11);
        assert((host_ctl & 0x4188) == 0x0080); /* no CF, NMI, INTIN; INTOUT no-op */
    }
}
static void test_clocks(void) {
    unsigned m;
    for (m = 0; m < 4; m++) {
        sst.misc = 0xe1 | (m << 2);
        sst.sr[0x10] = 0x34; sst.sr[0x11] = 0x56;
        sst.sr[0x12] = 0x47; sst.sr[0x13] = 0x65; sst.sr[0x15] = 0x02;
        s3_clock_restore();
        assert(misc_reg == sst.misc && loads == (int)m + 1);
        assert(clock_lo == 0x47 && clock_hi == 0x65);
        assert(seq[0x12] == 0x47 && seq[0x13] == 0x65 && !(seq[0x15]&0x20));
    }
}
static void test_command(void) {
    unsigned i, ae;
    for (ae = 0; ae < 2; ae++) {
        for (i = 0; i < sizeof(pixels); i++) pixels[i] = original[i] = (i * 13 + (i >> 7));
        memset(mmio, 0, sizeof(mmio)); nwrites = executions = 0;
        mmio[0xa500 / 4] = ae;  /* poisoned inherited autoexecute / geometry */
        mmio[0xa504 / 4] = 0xffffffff;
        virge_issue(mmio, 0x06000020 | (0xccu << 17), 40, 12, 8, 4, 320, 40);
        assert(executions == 1 && nwrites == 5);
        assert(writes[0] == 0xa500 && writes[1] == 0xa504 &&
               writes[2] == 0xa508 && writes[3] == 0xa50c && writes[4] == 0xa500);
        for (i = 0; i < sizeof(pixels); i++) {
            unsigned x = i % 640, y = i / 640;
            UBYTE e = x >= 320 && x < 360 && y >= 40 && y < 52
                    ? original[(y - 40 + 4)*640 + x - 320 + 8] : original[i];
            assert(pixels[i] == e);
        }
    }
}
static void test_registers(void) {
    assert(!s3_cr_used(0x50, 1) && s3_cr_used(0x50, 0));
    assert(!s3_cr_used(0x4a, 0) && !s3_cr_used(0x4b, 1));
    assert(s3_cr_used(0x66, 1) && !s3_cr_used(0x66, 0));
}
static void test_discovery(void) {
    const struct cdef *d = &cdefs[0];
    struct amt_probe_board b = {0x200000, 0x200000, d->man, d->pmem, 0};
    amt_probe_result.n_boards = 1; amt_probe_result.boards[0] = b;
    find_cards(); assert(ncards == 1 && cards[0].kind == K_PICASSO2 && !cards[0].regs);
    ncards = 0; amt_probe_result.n_boards = 2;
    b.prod = d->preg; b.addr = 0x600000; b.size = 0x10000; b.flags = 1;
    amt_probe_result.boards[1] = b;
    find_cards(); assert(ncards == 1 && nrej == 1 && !cards[0].regs);
    /* Equal product IDs: register descriptor may precede the memory board. */
    d = &cdefs[13]; assert(d->kind == K_OMNIBUS);
    ncards = 0;
    b.man = d->man; b.prod = d->pmem; b.flags = 0;
    amt_probe_result.boards[0] = b;
    b.size = 0x100000; b.addr = 0x200000; amt_probe_result.boards[1] = b;
    find_cards(); assert(ncards == 1 && cards[0].memsize == 0x100000 && cards[0].regs);
}
static void test_seed(void) {
    UBYTE copy[480]; unsigned x, y, r = rng;
    card.win[0].p = vram;
    rtg_seed_rect(4*640+8, 40, 12, 640, copy);
    for (y = 0; y < 12; y++) for (x = 0; x < 40; x++) {
        r = r * 1664525u + 1013904223u;
        assert(copy[y*40+x] == (UBYTE)(r >> 13));
        assert(vram[(y+4)*640+x+8] == copy[y*40+x]);
    }
    assert(!aborts);
}
static void test_pending(void) {
    unsigned i;
    card.win[0].p = vram;
    for (i = 0; i < 3; i++) {
        walk_save[i] = 0x12340000 + i;
        *(ULONG *)(vram + (i << 16) + 0x100) = 0xdeadbeef;
    }
    walk_pending = 3;
    alias_saved[0] = 0x11223344; alias_saved[1] = 0x55667788;
    alias_longs = 2; alias_pending = (ULONG *)(vram + 0x500);
    rtg_vram_restore();
    assert(!walk_pending && !alias_pending);
    assert(*(ULONG *)(vram + 0x500) == 0x11223344);
    assert(*(ULONG *)(vram + 0x504) == 0x55667788);
    for (i = 0; i < 3; i++)
        assert(*(ULONG *)(vram + (i << 16) + 0x100) == 0x12340000 + i);
}
static void test_zz(void) {
    unsigned i;
    card.bus = 3; card.win[0].p = vram; card.win[0].size = 65536;
    memset(vram, 0x55, 65536); fw = 0x010e;
    rtg_zz9000();
    assert(vblank_status == AMT_ST_PASS && skips == 4 && zz_reads >= 2);
    assert(card.win[0].size == 65536);
    for (i = 0; i < 65536; i++) assert(vram[i] == 0x55);
    zz_reads = skips = 0; fw = 0x010d; vblank_status = -1;
    rtg_zz9000();
    assert(vblank_status == AMT_ST_SKIP && skips == 5 && !zz_reads);
}
static void test_engine_restore(void) {
    unsigned i, ae;
    for (ae = 0; ae < 2; ae++) {
        s3_engine_saved = s3_engine = 1; untestable = nwrites = executions = 0;
        virge_cmdset = 0x01980020 | ae;
        for (i = 0; i < 8; i++) virge_regs[i] = 0x123 + i * 0x100;
        mmio[0xa500/4] = 1; /* inherited autoexecute must be disarmed before RDEST */
        s3_restore_engine(1);
        assert(!executions && untestable == !ae);
        assert(mmio[0xa500/4] == (ae ? virge_cmdset : 0x78000000));
        for (i = 0; i < 8; i++) assert(mmio[virge_offsets[i]/4] == virge_regs[i]);
        s3_engine = 0; nwrites = 0;
        s3_restore_engine(1); assert(!nwrites);
    }
}
static void test_monitor(void) {
    monitor_pending = 1; rtg_monitor_restore();
    assert(!monitor_pending && monitor_off == 1);
    rtg_monitor_restore(); assert(monitor_off == 1);
}
static void test_claim(void) {
    struct amt_probe_board copy;
    amt_probe_result.n_boards = 2;
    card.mem_board = &amt_probe_result.boards[0];
    card.reg_board = &amt_probe_result.boards[1];
    claim_allowed[0] = 1;
    assert(!claim_card(&card) && claim_calls == 2);
    claim_allowed[1] = 1;
    assert(claim_card(&card) && claim_calls == 4);
    card.reg_board = 0;
    assert(claim_card(&card) && claim_calls == 5);
    copy = amt_probe_result.boards[0]; card.mem_board = &copy;
    assert(!claim_card(&card) && claim_calls == 6);
    card.mem_board = 0;
    assert(!claim_card(&card) && claim_calls == 6);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (argv[1][0]) {
    case 'r': test_rop(); break; case 'p': test_ports(); break;
    case 'l': test_layout(); break; case 'm': test_minimum(); break;
    case 'a': test_alias(); break; case 'h': test_host(); break;
    case 'c': test_clocks(); break; case 'v': test_command(); break;
    case 'g': test_registers(); break; case 'd': test_discovery(); break;
    case 's': test_seed(); break; case 'w': test_pending(); break;
    case 'z': test_zz(); break; case 't': test_engine_restore(); break;
    case 'u': test_monitor(); break; case 'j': test_claim(); break; default: assert(0);
    }
    return 0;
}
"""
        cls.source = source
        cls.binary = cls.compile(source, "rules")
        cleanup_source = prelude + "#include <setjmp.h>\n" + types + r"""
static struct rtg_card card, *rtg_cur = &card;
static struct vga_state cst, sst;
static UBYTE vram[RTG_WORK], saved[RTG_WORK], *rtg_save = saved;
static UBYTE sr[32], gr[64], cr[128], selectors[4], misc, xlo, ylo;
static UBYTE p4_bank, gr31_pre, gr33_pre, cl_sr6, cl_misc, s3_sr8, s3_saved[0x50];
static int cl_stage, cl_extensions, cl_engine, s3_stage, s3_engine, s3_engine_saved;
static UWORD s3_adv;
static ULONG virge_adv, virge_cmd, pci[2], mm[0x9000/4];
static volatile ULONG *virge_pci;
static int busy, reset_works, resets, param_writes, restored, vram_restores, failures;
static int cleanup_clock, cleanup_std, starts, board_off, cia_restored;
static int fatal_calls;
static int cv_order;
static int cr50_check, cr50_writes, cr50_widths, cr50_lengths;
static UWORD last_port, last_word;
static ULONG frames;
static jmp_buf jump;
static void amt_abort(int code) { longjmp(jump, code); }
static void amt_abort_fatal(int code) { fatal_calls++; longjmp(jump, code); }
static int rtg_chk(const char *id, const char *what, ULONG e, ULONG g, const char *why)
{ (void)id; (void)what; (void)why; if (e != g) failures++; return e == g; }
static UBYTE vi(UWORD p) {
    if (p == 0x3cc) return misc;
    if (p == 0x3c4) {
        int i = selectors[0] & 31;
        return (selectors[0] & 31) | (i == 0x10 ? xlo : i == 0x11 ? ylo : 0);
    }
    if (p == 0x3ce) return selectors[1];
    if (p == 0x3d4) return selectors[2];
    if (p == 0x3b4) return selectors[3];
    return 0;
}
static void vo(UWORD p, UBYTE v) {
    if (p == 0x3c2) misc = v;
    if (p == 0x3c4) selectors[0] = v;
    if (p == 0x3ce) selectors[1] = v;
    if (p == 0x3d4) selectors[2] = v;
    if (p == 0x3b4) selectors[3] = v;
}
static UBYTE read_sr(UBYTE i) { selectors[0] = i; return sr[i & 31]; }
static void write_sr(UBYTE i, UBYTE v) {
    selectors[0] = i; sr[i & 31] = v;
    if ((i & 31) == 0x10) xlo = i & 0xe0;
    if ((i & 31) == 0x11) ylo = i & 0xe0;
}
static UBYTE read_gr(UBYTE i) { selectors[1] = i; return gr[i]; }
static void write_gr(UBYTE i, UBYTE v) {
    selectors[1] = i; gr[i] = v;
    if (i == 0x31 && (v & 4)) { resets++; if (reset_works) busy = 0; }
    else if (i == 0x31 && (v & 2)) { starts++; busy = 1; }
    else if (i != 0x31) { assert(!busy); param_writes++; }
}
static UBYTE read_cr(UBYTE i) { selectors[2] = i; return cr[i]; }
static void write_cr(UBYTE i, UBYTE v) {
    if (card.chip == CH_S3VIRGE) assert(i != 0x50);
    if (card.chip == CH_S3VIRGE || card.chip == CH_S3TRIO) assert(i != 0x4a && i != 0x4b);
    if (i == 0x50 && cr50_check) {
        unsigned width = ((v & 1) << 2) | (v >> 6), length = (v >> 4) & 3;
        assert(width != 5 && width != 7 && length != 2);
        assert((v & 0x0e) == 0x0e);
        cr50_widths |= 1 << width; cr50_lengths |= 1 << length; cr50_writes++;
    }
    selectors[2] = i; cr[i] = v;
    if (i == 0x66 && (v & 2)) { resets++; if (reset_works) busy = 0; }
}
#define SR_R(i) read_sr(i)
#define SR_W(i,v) write_sr(i,v)
#define GR_R(i) read_gr(i)
#define GR_W(i,v) write_gr(i,v)
#define CR_R(i) read_cr(i)
#define CR_W(i,v) write_cr(i,v)
#define DAC_M 0x3c6
static UBYTE ar_r(UBYTE i) { (void)i; return 0; }
static void ar_w(UBYTE i, UBYTE v) { (void)i; (void)v; }
static void dac_read_all(UBYTE *d) { memset(d, 0, 768); }
static void dac_write_all(const UBYTE *d) { (void)d; }
static int cl_wait(void) { return !busy; }
static int s3_idle(void) { return !busy; }
static int virge_idle(volatile ULONG *m) { (void)m; return !busy; }
static volatile ULONG *virge_mmio(void) { return mm; }
static void rtg_vram_restore(void) { assert(!busy); if (card.kind == K_CV64) assert(!cv_order); vram_restores++; }
static void rtg_monitor_restore(void) { assert(!busy); }
static void s3_restore_engine(int virge) { (void)virge; assert(!busy); restored++; }
static void s3_clock_restore(void) { cleanup_clock++; }
static void cv64_ctl(UBYTE v) { cv_order = v; }
static void ew(UWORD port, UWORD v) {
    last_port = port; last_word = v;
    if (port == 0x42e8 && v == 0x8000) { resets++; if (reset_works) busy = 0; }
}
static ULONG amt_frames_now(void) { return frames++; }
static UWORD host_ctl, host_l, host_h, host_data;
#define HCTL host_ctl
#define HADRL host_l
#define HADRH host_h
#define HDATA host_data
#define HST_HLT 0x8000
#define HST_INCW 0x0800
static UWORD tms_ctl, tms_l, tms_h, tms_save[32];
static ULONG tms_addr;
static int tms_stage, tms_pending;
static UBYTE vint_level, vint_c11, vint_crx;
static int vint_stage, vint_cleaning;
static UWORD vint_intena;
static void (*vint_enable)(int);
static void (*vint_old_hook)(ULONG);
static void (*amt_irq_hook[8])(ULONG);
static UWORD custom[2];
#define R_INTENA 0
#define R_INTREQ 1
#define CREG(r) custom[r]
static int vint_cia;
static void cia_restore(int *s) { (void)s; cia_restored++; }
static void irq_old(ULONG n) { (void)n; }
static void irq_test(ULONG n) { (void)n; }
static void board_enable(int on) { assert(!on); board_off++; }
"""
        for src, names in (
                (common, ("vga_save_selectors", "vga_save", "vga_restore_selectors", "vga_restore_std", "vint_restore")),
                (cl, ("cl_finish", "cl_blt", "cl_normalize_blt", "cl_restore")),
                (s3, ("virge_require_idle", "s3_enable_engine", "s3_irq_mode", "trio_cr50_test", "s3_restore")),
                (misc, ("hctl", "tms_seek", "tms_restore"))):
            cleanup_source += "\n".join(function(src, name) for name in names) + "\n"
        cleanup_source += r"""
static void initialize(void) {
    card.kind = K_PICASSO2; card.chip = CH_CIRRUS; card.win[0].p = vram;
    cl_stage = 8 | 16 | 4 | 2; cl_extensions = cl_engine = 1;
    busy = 0; reset_works = 1; resets = param_writes = restored = 0;
    fatal_calls = 0;
    memset(vram, 0xee, sizeof(vram)); memset(saved, 0x44, sizeof(saved));
}
static void cursor(void) {
    int x, y;
    for (x = 0; x < 8; x++) for (y = 0; y < 8; y++) {
        initialize(); sr[0] = 1; sr[0x10] = 1; sr[0x11] = 2;
        xlo = x << 5; ylo = y << 5; misc = cl_misc = 1;
        selectors[0] = 0x13; selectors[1] = 7; selectors[2] = 0x21; selectors[3] = 0x15;
        vga_save_selectors(&cst); vga_save(&cst, 32, 64, 64);
        cst.gr[0x2f] = 0x63; cst.gr[0x33] = 7;
        sr[0] = 3; sr[0x10] = sr[0x11] = 0; xlo = ylo = 0;
        cl_restore();
        assert(sr[0] == 1 && sr[0x10] == 1 && sr[0x11] == 2);
        assert(xlo == x << 5 && ylo == y << 5);
        assert(selectors[0] == 0x13 && selectors[1] == 7 &&
               selectors[2] == 0x21 && selectors[3] == 0x15);
        assert(gr[0x2f] == 0x63 && gr[0x33] == 7 && vram[0] == 0x44);
    }
}
static void cirrus_timeout(int recover) {
    int code;
    initialize(); busy = 1; reset_works = recover;
    code = setjmp(jump);
    if (!code) cl_restore();
    if (recover) {
        assert(!code && resets == 1 && vram[0] == 0x44 && param_writes > 0);
    } else {
        assert(code && fatal_calls == 1 && resets == 2 && vram[0] == 0xee && !param_writes && gr[0x31] == 4);
    }
}
static void cirrus_submission(void) {
    int code;
    initialize(); busy = 1;
    code = setjmp(jump);
    if (!code) cl_blt(0x1000, 0x100, 40, 12, 640, 640, 0, 0x0d);
    assert(code && starts == 0 && param_writes == 0);
    initialize(); busy = 0;
    code = setjmp(jump);
    if (!code) cl_blt(0x1000, 0x100, 40, 12, 640, 640, 0, 0x0d);
    assert(code && starts == 1 && cl_engine && vram[0] == 0xee);
    cl_restore(); assert(vram[0] == 0x44 && !busy);
}
static void s3_timeout(int virge, int recover) {
    int code;
    initialize();
    card.kind = virge ? K_CV643D_Z3 : K_CV64;
    card.chip = virge ? CH_S3VIRGE : CH_S3TRIO;
    s3_stage = 256 | 64 | 32; s3_engine = s3_engine_saved = 1;
    cv_order = virge ? 0 : 0x60;
    virge_pci = pci; virge_cmd = 7; busy = 1; reset_works = recover;
    code = setjmp(jump);
    if (!code) s3_restore();
    if (recover) {
        assert(!code && !busy && restored == 1 && vram[0] == 0x44);
    } else {
        assert(code && fatal_calls == 1 && !restored && vram[0] == 0xee);
        if (virge) assert((cr[0x66] & 2) && pci[1] == 3);
    }
}
static void virge_wait(void) {
    int code;
    initialize(); busy = 1;
    code = setjmp(jump);
    if (!code) virge_require_idle(mm);
    assert(code && failures && !restored && vram[0] == 0xee);
}
static void irq_cleanup(void) {
    vint_level = 2; vint_c11 = 0x83; vint_crx = 0x31;
    vint_stage = 7; vint_intena = 8; vint_enable = board_enable;
    vint_old_hook = irq_old; amt_irq_hook[2] = irq_test;
    vint_restore();
    assert(!vint_stage && board_off == 1 && cia_restored == 1);
    assert(amt_irq_hook[2] == irq_old && custom[R_INTENA] == 0x8008);
    assert(cr[0x11] == 0x83 && selectors[2] == 0x31);
    vint_restore(); assert(board_off == 1 && cia_restored == 1);
}
static void host_cleanup(void) {
    tms_stage = 1; tms_pending = 1; host_ctl = HST_HLT;
    tms_ctl = 0x8000 | 0x200 | 0x3800 | 5;
    tms_l = 0x1230; tms_h = 0x00ab; tms_addr = 0x10000; tms_save[0] = 0x4242;
    tms_restore();
    assert(host_data == 0x4242 && !tms_pending);
    assert(host_l == 0x1230 && host_h == 0xab);
    assert((host_ctl & 0xba07) == tms_ctl);
}
static void extension_state(void) {
    initialize(); cst.gr[0x2f] = gr[0x2f] = 0x63; cst.gr[0x33] = gr[0x33] = 7;
    cl_normalize_blt(0xa8);
    assert(!cl_extensions && gr[0x2f] == 0x63 && gr[0x33] == 7);
    cl_normalize_blt(0xb8);
    assert(cl_extensions && gr[0x2f] == 0x60 && gr[0x33] == 0);
    cl_restore(); assert(gr[0x2f] == 0x63 && gr[0x33] == 7);
}
static void engine_modes(void) {
    s3_saved[0x36] = 0x80; virge_adv = 0x11; s3_adv = 0x14;
    s3_enable_engine(1); assert(cr[0x66] == 0x81);
    s3_enable_engine(0); assert(last_port == 0x4ae8 && last_word == 0x11);
    s3_irq_mode(1, 1);
    assert(!(cr[0x66] & 1) && !(mm[0x850c/4] & 1));
    s3_irq_mode(1, 0);
    assert((cr[0x66] & 1) && mm[0x850c/4] == 0x11);
    s3_irq_mode(0, 1); assert(last_port == 0x4ae8 && !(last_word & 1));
    s3_irq_mode(0, 0); assert(last_port == 0x4ae8 && (last_word & 1));
}
static void standard_state(void) {
    initialize(); cst.sr[0] = 1; sr[0] = 3;
    cst.srx = 0x12; cst.grx = 0x23; cst.crx = 0x34; cst.crmx = 0x45;
    vga_restore_std(&cst);
    assert(sr[0] == 1 && selectors[0] == 0x12 && selectors[1] == 0x23 &&
           selectors[2] == 0x34 && selectors[3] == 0x45);
}
static void trio_fields(void) {
    card.chip = CH_S3TRIO; cr[0x50] = 0x8e; cr50_check = 1;
    trio_cr50_test();
    assert(!failures && cr[0x50] == 0x8e && cr50_writes == 9);
    assert(cr50_widths == 0x5f && cr50_lengths == 0x0b);
}
int main(int argc, char **argv) {
    assert(argc == 2);
    switch (argv[1][0]) {
    case 'c': cursor(); break;
    case 'r': cirrus_timeout(1); break; case 'f': cirrus_timeout(0); break;
    case 'b': cirrus_submission(); break;
    case 's': s3_timeout(0, 1); s3_timeout(1, 1); break;
    case 'q': s3_timeout(0, 0); s3_timeout(1, 0); break;
    case 'v': virge_wait(); break; case 'i': irq_cleanup(); break;
    case 'h': host_cleanup(); break; case 'x': extension_state(); break;
    case 'e': engine_modes(); break; case 'o': standard_state(); break;
    case 'l': trio_fields(); break; default: assert(0);
    }
    return 0;
}
"""
        cleanup_source = cleanup_source.replace("cleanup_std, ", "").replace(
            "static ULONG amt_frames_now(void) { return frames++; }", "")
        cleanup_source = cleanup_source.replace("static ULONG frames;\n", "")
        cls.cleanup_source = cleanup_source
        cls.cleanup_binary = cls.compile(cleanup_source, "cleanup")
        fifo_source = prelude + r"""
static ULONG frames;
static int full, writes;
static UBYTE vi(UWORD p) { (void)p; return full ? 0x80 : 0; }
static void vo(UWORD p, UBYTE v) { (void)p; (void)v; writes++; }
static ULONG amt_frames_now(void) { return frames++; }
static void amt_abort(int code) { (void)code; exit(92); }
static int rtg_chk(const char *id, const char *what, ULONG e, ULONG g, const char *why)
{ (void)id; (void)what; (void)e; (void)g; (void)why; return 0; }
"""
        fifo_source += function(s3, "ew")
        fifo_source += r"""
int main(int argc, char **argv) {
    assert(argc == 2);
    full = 1;
    if (argv[1][0] == 'f') { ew(0x82e8, 1); return 93; }
    ew(0x42e8, 0x8000); assert(writes == 2);
    ew(0xbee8, 0xf000); assert(writes == 4);
    full = 0; ew(0x82e8, 1); assert(writes == 6);
    return 0;
}
"""
        cls.fifo_binary = cls.compile(fifo_source, "fifo")

    @classmethod
    def compile(cls, source, name):
        binary = cls.build / name
        result = subprocess.run(
            [cls.cc, "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror",
             "-Wno-pointer-to-int-cast", "-Wno-int-to-pointer-cast",
             "-x", "c", "-", "-o", str(binary)], input=source, text=True,
            capture_output=True, env={**os.environ, "TMPDIR": str(ROOT / "work" / "tmp")})
        if result.returncode:
            raise AssertionError(result.stderr)
        return binary

    def run_case(self, mode, binary=None):
        result = subprocess.run([binary or self.binary, mode], capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stderr)

    def test_exhaustive_independent_rop_truth_tables(self):
        self.run_case("r")

    def test_picasso_ii_dac_direction_specific_ports(self):
        self.run_case("p")

    def test_verified_layouts_and_bounded_zz9000_apertures(self):
        self.run_case("l")

    def test_vram_minimum_only_exempts_known_cv643d_z2_aperture(self):
        self.run_case("m")

    def test_reverse_alias_failure_reports_actual_and_restores_both_views(self):
        self.run_case("a")

    def test_host_control_preserves_messages_without_command_side_effects(self):
        self.run_case("h")

    def test_programmable_pll_restored_after_final_misc_write(self):
        self.run_case("c")

    def test_virge_command_uses_initialized_geometry_with_inherited_ae_on_or_off(self):
        self.run_case("v")

    def test_chip_specific_registers_exclude_reserved_and_cursor_stacks(self):
        self.run_case("g")

    def test_missing_rejected_registers_and_reversed_equal_id_pair(self):
        self.run_case("d")

    def test_engine_inputs_have_independent_generated_bytes(self):
        self.run_case("s")

    def test_cursor_low_bits_reset_and_selectors_survive_real_restore(self):
        self.run_case("c", self.cleanup_binary)

    def test_cirrus_timeout_quiesces_before_restoring_and_holds_failed_reset(self):
        for mode in ("r", "f", "b"):
            self.run_case(mode, self.cleanup_binary)

    def test_trio_and_virge_timeout_cleanup_does_not_restore_live_engine(self):
        for mode in ("s", "q", "v"):
            self.run_case(mode, self.cleanup_binary)

    def test_abort_irq_cleanup_restores_previous_hook_mask_and_cia(self):
        self.run_case("i", self.cleanup_binary)

    def test_tms_abort_cleanup_restores_pending_memory_then_final_control(self):
        self.run_case("h", self.cleanup_binary)

    def test_only_applicable_cirrus_extensions_normalized_and_restored(self):
        self.run_case("x", self.cleanup_binary)

    def test_enhanced_enables_and_deliberate_vga_irq_modes(self):
        self.run_case("e", self.cleanup_binary)

    def test_s3_readable_engine_state_restores_without_replaying_commands(self):
        self.run_case("t")

    def test_generic_vga_restores_reset_and_all_readable_selectors(self):
        self.run_case("o", self.cleanup_binary)

    def test_abort_restores_pending_walk_samples_and_non_alias_window(self):
        self.run_case("w")

    def test_abort_monitor_cleanup_is_idempotent(self):
        self.run_case("u")

    def test_only_canonical_memory_and_register_claims_allow_access(self):
        self.run_case("j")

    def test_trio_cr50_uses_only_documented_width_and_pixel_encodings(self):
        self.run_case("l", self.cleanup_binary)

    def test_official_zz9000_vblank_mask_capability_skip_and_no_firmware_ram_writes(self):
        self.run_case("z")

    def test_trio_waits_for_fifo_space_but_reset_and_read_selector_bypass_it(self):
        self.run_case("r", self.fifo_binary)
        result = subprocess.run([self.fifo_binary, "f"], capture_output=True, text=True)
        self.assertEqual(result.returncode, 92)

    def test_mutations_are_detected(self):
        mutations = (
            ("dac", "p", "(write && port == 0x3c7)", "((write || 1) && port == 0x3c7)"),
            ("minimum", "m", "c->kind == K_CV643D_Z2 && lim == 0x3a0000UL && good == n",
             "good == n && n > 0"),
            ("alias", "a", 'vram_fail(suf, "alias-to-natural read differs", 0x3450, 4, 0x0badf00dUL, g2,',
             'vram_fail(suf, "alias-to-natural read differs", 0x3450, 4, 0x600dcafeUL, g,'),
            ("host", "h", "keep & 0x8207", "keep & 0xc000"),
            ("pll", "c", "SR_W(0x15, sst.sr[0x15] | 0x20);", "SR_W(0x15, sst.sr[0x15] & ~0x20);"),
            ("trigger", "v", "mm_write((ULONG *)m, 0xa500, cmd);",
             "mm_write((ULONG *)m, 0xa500, cmd); mm_write((ULONG *)m, 0xa500, cmd);"),
            ("inherit-ae", "v", "mm_write((ULONG *)m, 0xa500, 0x78000000UL);", ""),
            ("reserved-cr", "g", "(i == 0x53 || i == 0x66)", "(i == 0x50 || i == 0x53 || i == 0x66)"),
            ("vblank", "z", "ZZR(0x4c) & 0x20", "ZZR(0x4c) & 1"),
            ("command-replay", "t",
             "(virge_cmdset & 1) || (virge_cmdset & 0x78000000UL) == 0x78000000UL", "1"),
            ("register-claim", "j", "(!c->reg_board || amt_board_claim(c->reg_board))", "1"),
        )
        for name, mode, old, new in mutations:
            with self.subTest(name=name):
                self.assertIn(old, self.source)
                binary = self.compile(self.source.replace(old, new), name)
                result = subprocess.run([binary, mode], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0, "regression mutant survived")
        cleanup_mutations = (
            ("cursor-low", "c", "0x10 | cst.cursor_xlo", "0x10"),
            ("busy-restore", "f", "cl_engine && !cl_wait()", "cl_engine && cl_wait()"),
            ("reset-state", "o", "SR_W(0, s->sr[0]);", "SR_W(0, 3);"),
            ("irq-hook", "i", "amt_irq_hook[vint_level] = vint_old_hook;",
             "amt_irq_hook[vint_level] = 0;"),
            ("extensions", "x", "cl_extensions = (id & 0xfc) >= 0xac;",
             "cl_extensions = (id & 0xfc) >= 0xa8;"),
            ("enhanced-irq", "e", "virge_adv & ~1", "virge_adv | 1"),
            ("reserved-trio-width", "l", "0xc0, 0x01, 0x81", "0xc0, 0x41, 0x81"),
            ("reserved-trio-length", "l", "0x81, 0x10, 0x30", "0x81, 0x20, 0x30"),
        )
        for name, mode, old, new in cleanup_mutations:
            with self.subTest(name=name):
                self.assertIn(old, self.cleanup_source)
                binary = self.compile(self.cleanup_source.replace(old, new), name)
                result = subprocess.run([binary, mode], capture_output=True, text=True)
                self.assertNotEqual(result.returncode, 0, "cleanup regression mutant survived")
