"""Native production-helper tests; no emulator or guest execution."""
import os
from pathlib import Path
import shutil
import subprocess
import unittest

from tests.test_guest_display_timing import function

ROOT = Path(__file__).resolve().parents[2]
SOUND = (ROOT / "disk/modules/expsound.c").read_text()
NET = (ROOT / "disk/modules/network.c").read_text()


class ExpansionNative(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = shutil.which("cc")
        if not cls.cc:
            raise unittest.SkipTest("native C compiler unavailable")
        cls.build = ROOT / "work" / f"expansion-native-{os.getpid()}"
        cls.build.mkdir(parents=True, exist_ok=True)
        cls.addClassCleanup(shutil.rmtree, cls.build)
        cls.network = cls.network_source()
        cls.codec = cls.codec_source()
        cls.uae = cls.uae_source()
        cls.discovery = cls.discovery_source()
        cls.lifecycle = cls.lifecycle_source()
        cls.programs = {}
        for name in ("network", "codec", "uae", "discovery", "lifecycle"):
            cls.programs[name] = cls.compile(getattr(cls, name), name)

    @classmethod
    def compile(cls, source, name):
        path = cls.build / (name + ".c")
        binary = cls.build / name
        path.write_text(source)
        result = subprocess.run(
            [cls.cc, "-std=c99", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-function", "-Wno-unused-parameter",
             "-Wno-unused-variable", "-o", str(binary), str(path)],
            text=True, capture_output=True)
        if result.returncode:
            raise AssertionError(result.stderr)
        return binary

    def run_program(self, name, mode, binary=None):
        return subprocess.run([str(binary or self.programs[name]), mode],
                              text=True, capture_output=True)

    def assert_program(self, name, mode):
        result = self.run_program(name, mode)
        self.assertEqual(result.returncode, 0, result.stderr)

    @staticmethod
    def network_source():
        source = r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef uint32_t ULONG;
#define D_STP 0x0200
#define D_ENP 0x0100
#define BUFSZ 0x600
#define ZW(o) ((volatile UWORD *)(uintptr_t)(o))
static const UBYTE padr[6] = { 2, 0, 0x5a, 0xa2, 6, 5 };
static int data_reads, pending = 1, ack_broken, disabled;
static UBYTE es_rb(volatile UBYTE *p) { data_reads++; return *p; }
static UWORD es_rw(volatile UWORD *p) { assert((uintptr_t)p == 4); return pending; }
static void es_ww(volatile UWORD *p, UWORD v)
{
    assert((uintptr_t)p == 4);
    if (v == 0x18) { if (!ack_broken) pending = 0; }
    else { assert(v == 0); disabled++; pending = 0; }
}
"""
        source += "\n".join(function(NET, name) for name in
                            ("arp_reply", "a2065_arp_reply", "z_ack", "z_rx_length"))
        source += r"""
static void reply(UBYTE *r)
{
    static const UBYTE header[] = { 8, 6, 0, 1, 8, 0, 6, 4, 0, 2 };
    static const UBYTE spa[] = { 10, 0, 2, 2 }, tpa[] = { 10, 0, 2, 15 };
    memset(r, 0xa5, BUFSZ);
    memcpy(r, padr, 6); memcpy(r + 32, padr, 6);
    memcpy(r + 12, header, 10);
    memset(r + 6, 0x22, 6); memcpy(r + 22, r + 6, 6);
    memcpy(r + 28, spa, 4); memcpy(r + 38, tpa, 4);
}
int main(int argc, char **argv)
{
    UBYTE r[BUFSZ];
    unsigned n, i;
    assert(argc == 2);
    reply(r);
    switch (argv[1][0]) {
    case 'a':
        assert(arp_reply(r, 42, padr));
        assert(arp_reply(r, 1518, padr));
        for (i = 12; i < 22; i++) {
            r[i] ^= 1; assert(!arp_reply(r, 42, padr)); r[i] ^= 1;
        }
        for (i = 0; i < 42; i++) {
            r[i] ^= 1; assert(!arp_reply(r, 42, padr)); r[i] ^= 1;
        }
        for (n = 0; n < 42; n++) assert(!arp_reply(r, n, padr));
        assert(!arp_reply(r, 1519, padr));
        break;
    case 'l':
        for (n = 0; n < 46; n++) {
            data_reads = 0;
            assert(!a2065_arp_reply(r, D_STP | D_ENP, n));
            assert(data_reads == 0);
        }
        assert(a2065_arp_reply(r, D_STP | D_ENP, 46));
        assert(a2065_arp_reply(r, D_STP | D_ENP, 1518));
        data_reads = 0;
        assert(!a2065_arp_reply(r, D_STP | D_ENP, 1519));
        assert(data_reads == 0);
        for (i = 0; i < 0x10000; i += 0x100) {
            if (i == (D_STP | D_ENP)) continue;
            data_reads = 0;
            assert(!a2065_arp_reply(r, i, 46));
            assert(data_reads == 0);
        }
        break;
    case 'z':
        for (n = 0; n < 65536; n++)
            assert(z_rx_length(n) == (n >= 42 && n <= 1518));
        assert(z_ack() == 1 && !disabled && !pending);
        assert(z_ack() == -1 && !disabled);
        pending = ack_broken = 1;
        assert(z_ack() == 0 && !disabled && pending);
        break;
    default: assert(0);
    }
    return 0;
}
"""
        return source

    @staticmethod
    def codec_source():
        source = r"""
#include <assert.h>
#include <stdint.h>
#include <setjmp.h>
#include <string.h>
typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef uint32_t ULONG;
#define AMT_ST_FAIL 1
#define AMT_ABORT_USER 3
#define INT_EXTER 0x2000
#define R_INTENA 0
#define R_INTREQ 1
#define CREG(r) custom[r]
enum { CB_TOCCATA, CB_PRELUDE, CB_P1200 };
static UWORD custom[2];
static void (*amt_irq_hook[7])(ULONG);
static UBYTE port[5], regs[32], sav[32], mce;
static const UBYTE ext_rw[] = { 16, 17, 18, 19, 20, 21, 26, 28, 30, 31 };
static struct cb {
    const char *n;
    int kind, cs;
    ULONG base;
    volatile UBYTE *r0, *r1, *fifo, *stat, *rst;
} board = { "codec", CB_TOCCATA, 1, 0, port, port+1, port+2, port+3, port+4 };
static struct cb *B = &board;
static int codec_saved, codec_ext_saved, codec_dirty, codec_restoring;
static unsigned clock_now, busy, data_writes, selected, status_fail;
static int stuck, stuck_on_clock, aci_stuck, allow_sdc, fatal_calls, i9_ok = -1;
static jmp_buf abort_env;
static UBYTE es_rb(volatile UBYTE *p)
{
    if (p == B->r0) {
        if (stuck || busy) { if (busy) busy--; return 0x80; }
        return mce | selected;
    }
    assert(p == B->r1 && !busy && !stuck);
    if (selected == 11) return aci_stuck ? 0x20 : 0;
    return regs[selected];
}
static void es_wb(volatile UBYTE *p, UBYTE v)
{
    if (p == B->stat || p == B->rst) {
        memset(regs, 0, sizeof(regs)); busy = 0; return;
    }
    if (p == B->r0) {
        assert(!busy && !stuck);
        selected = v & 31;
        return;
    }
    assert(p == B->r1 && !busy && !stuck);
    data_writes++;
    if (selected == 8) {
        if (mce) { regs[8] = v; busy = 3; stuck = stuck_on_clock; }
    } else if (selected == 9) {
        if (mce || allow_sdc) regs[9] = v;
        else regs[9] = (regs[9] & ~3) | (v & 3);
    } else regs[selected] = v;
}
static ULONG frames(void) { return clock_now++; }
static const char *pc(const char *id) { return id; }
static void amt_case(const char *id, int status, const char *why)
{ (void)id; (void)why; assert(status == AMT_ST_FAIL); status_fail++; }
static void amt_abort(int code) { assert(code == AMT_ABORT_USER); longjmp(abort_env, 1); }
static void amt_abort_fatal(int code)
{ assert(code == AMT_ABORT_USER); fatal_calls++; longjmp(abort_env, 1); }
static char es_sb[200];
static void sb_reset(void) {}
static void sb_s(const char *s) {}
static void sb_h(ULONG v, int n) {}
static void rep(const char *id, int ok, const char *why, const char *what,
                ULONG e, ULONG g, int dig, const char *in)
{
    if (!strcmp(id, "i9_needs_mce")) i9_ok = ok;
    else assert(ok);
}
static int cinit_wait(ULONG n);
static void codec_ready(int ok);
"""
        source += "\n".join(function(SOUND, name) for name in
                            ("cx", "cr", "cw", "cmce", "cinit_wait", "caci_wait",
                             "cmce_off", "codec_ready", "t_i8", "board_quiet",
                             "board_reset", "codec_cleanup"))
        source += r"""
int main(int argc, char **argv)
{
    unsigned i, writes;
    assert(argc == 2);
    switch (argv[1][0]) {
    case 'w':
        cmce(1); cw(8, 0x53); cw(9, 0); cmce_off();
        assert(!busy && regs[8] == 0x53 && !mce);
        break;
    case 's':
        cmce(1); stuck_on_clock = 1;
        if (!setjmp(abort_env)) { cw(8, 0x53); assert(0); }
        assert(status_fail == 1 && !fatal_calls);
        break;
    case 'f':
        cmce(1); codec_restoring = stuck_on_clock = 1;
        if (!setjmp(abort_env)) { cw(8, 0x53); assert(0); }
        assert(status_fail == 1 && fatal_calls == 1);
        break;
    case 'c':
        if (!setjmp(abort_env)) {
            aci_stuck = 1; cmce_off(); assert(0);
        }
        assert(status_fail == 1);
        break;
    case 'i':
        regs[9] = 0xcc; t_i8(); assert(i9_ok == 1);
        allow_sdc = 1; t_i8(); assert(i9_ok == 0);
        break;
    case 'r':
        for (i = 0; i < 32; i++) sav[i] = i ^ 0x55;
        codec_saved = codec_ext_saved = codec_dirty = 1;
        codec_cleanup();
        for (i = 0; i < 8; i++) assert(regs[i] == sav[i]);
        assert(regs[8] == sav[8]);
        assert(regs[9] == (sav[9] & ~3));
        assert(regs[10] == (sav[10] & ~2));
        assert(regs[12] == sav[12]);
        for (i = 13; i < 16; i++) assert(regs[i] == sav[i]);
        for (i = 0; i < sizeof(ext_rw); i++) {
            UBYTE reg = ext_rw[i];
            assert(regs[reg] == (reg == 16 ? sav[reg] & ~0x40 :
                               reg == 28 ? sav[reg] & 0xf0 : sav[reg]));
        }
        assert(!codec_dirty && !codec_restoring);
        writes = data_writes; codec_cleanup(); assert(writes == data_writes);
        break;
    default: assert(0);
    }
    return 0;
}
"""
        return source

    @staticmethod
    def discovery_source():
        source = r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
typedef uint8_t UBYTE;
typedef uint32_t ULONG;
#define AMT_F_UNSAFE 1
#define AMT_ST_SKIP 2
#define AMT_ST_UNTESTABLE 3
#define AMT_PROBE_BOARDS_MAX 8
#define MAN_MACROSYSTEM 18260
#define PROD_TOCCATA 12
#define MAN_ACT 16945
#define PROD_PRELUDE 1
#define MAN_UAE 6502
#define PROD_UAESND 2
#define MAN_COMMODORE_WC 514
#define PROD_A2065 112
#define MAN_MNT 28014
#define PROD_ZZ9000_Z2 3
#define PROD_ZZ9000_Z3 4
enum { CB_TOCCATA, CB_PRELUDE, CB_P1200 };
struct amt_probe_board { ULONG man, prod, addr, size, flags; };
static struct { int n_boards; struct amt_probe_board boards[8]; } amt_probe_result;
static struct { ULONG flags; } amt_par;
static struct cb {
    const char *n;
    int kind, cs;
    ULONG base;
    volatile UBYTE *r0, *r1, *fifo, *stat, *rst;
    UBYTE hf, ef, fmask, fshift;
} cbs;
static char ids[32][80], combined[80];
static int ncase, nboard, nfinish, nlog;
static int claim_allowed = 1;
static ULONG addresses[8];
static const char *es_cid(const char *p, const char *s)
{
    unsigned n = strlen(p);
    strcpy(combined, p); combined[n] = '_'; strcpy(combined + n + 1, s);
    return combined;
}
static void amt_case(const char *id, int status, const char *why)
{
    int i;
    for (i = 0; i < ncase; i++) assert(strcmp(ids[i], id));
    strcpy(ids[ncase++], id);
}
static void amt_case_open(const char *id, int status) { amt_case(id, status, 0); }
static void amt_case_close(void) {}
static void json_begin(const char *s) { nlog++; }
static void json_str(const char *k, const char *v) {}
static void json_hex(const char *k, ULONG v) {}
static void json_hexn(const char *k, ULONG v, int n) {}
static void json_int(const char *k, int v) {}
static void json_end(void) {}
static void json_obj(const char *s) {}
static void json_obj_end(void) {}
static int amt_board_ok(const struct amt_probe_board *b) { return !b->flags; }
static int amt_board_claim(const struct amt_probe_board *b) { (void)b; return claim_allowed; }
static int board_usable(const struct amt_probe_board *b, const char *name)
{
    if (amt_board_ok(b)) return 1;
    amt_case(es_cid(name, "board_rejected"), 2, "rejected"); return 0;
}
static void codec_board(struct cb *b)
{ addresses[nboard++] = b->base; amt_case(es_cid(b->n, "test"), 0, 0); }
static void uaesnd_board(const struct amt_probe_board *b)
{ addresses[nboard++] = b->addr; amt_case("uaesnd_test", 0, 0); }
static void a2065_board(const struct amt_probe_board *b)
{ addresses[nboard++] = b->addr; amt_case("a2065_test", 0, 0); }
static void zz9000_board(const struct amt_probe_board *b)
{ addresses[nboard++] = b->addr; amt_case("zz9000_test", 0, 0); }
static void finish_board(void) { nfinish++; }
"""
        source += function(SOUND, "expsound_body")
        source += function(NET, "network_body")
        source += r"""
int main(int argc, char **argv)
{
    int i, net;
    assert(argc == 2);
    net = argv[1][0] == 'n' || argv[1][0] == 'p';
    amt_probe_result.n_boards = net ? 4 : 6;
    amt_par.flags = argv[1][0] == 'o' || argv[1][0] == 'p' ? 9 : net ? 8 : 0;
    claim_allowed = argv[1][0] != 'o' && argv[1][0] != 'p';
    for (i = 0; i < amt_probe_result.n_boards; i++) {
        struct amt_probe_board *b = &amt_probe_result.boards[i];
        b->addr = 0x200000 + i * 0x10000; b->size = 0x10000;
        if (net) {
            b->man = i < 2 ? MAN_COMMODORE_WC : MAN_MNT;
            b->prod = i < 2 ? PROD_A2065 : i == 2 ? PROD_ZZ9000_Z2 : PROD_ZZ9000_Z3;
        } else {
            b->man = i < 2 ? MAN_MACROSYSTEM : i < 4 ? MAN_ACT : MAN_UAE;
            b->prod = i < 2 ? PROD_TOCCATA : i < 4 ? PROD_PRELUDE : PROD_UAESND;
        }
    }
    if (argv[1][0] == 'r') amt_probe_result.boards[0].flags = 1;
    if (net) network_body(); else expsound_body();
    assert(ncase == (net ? 2 : 4) && nlog == (net ? 2 : 3));
    if (argv[1][0] == 'o' || argv[1][0] == 'p')
        assert(nboard == 0 && nfinish == 0);
    else if (argv[1][0] == 'r')
        assert(nboard == 2 && addresses[0] == 0x220000);
    else
        assert(nboard == (net ? 2 : 3) && nfinish == nboard && addresses[0] == 0x200000);
    return 0;
}
"""
        # ULONG is a 32-bit target address; host uintptr_t is needed only for
        # the discovery pointer casts, which this harness never dereferences.
        return source.replace("typedef uint32_t ULONG;", "typedef uintptr_t ULONG;")

    @staticmethod
    def lifecycle_source():
        source = r"""
#include <assert.h>
#include <stdint.h>
#include <setjmp.h>
typedef uint16_t UWORD;
typedef uint32_t ULONG;
#define INT_PORTS 8
#define INT_EXTER 0x2000
#define R_INTENA 0
#define R_INTREQ 1
#define CREG(r) custom[r]
#define C0_STOP 4
#define AMT_ST_FAIL 1
#define AMT_ABORT_USER 3
#define ZW(o) ((volatile UWORD *)(uintptr_t)(o))
static UWORD custom[2];
static void (*amt_irq_hook[7])(ULONG);
static int net_dirty, z_mac_saved, cia_a_saved, cia_b_saved, cleanup_failed;
static UWORD z_saved_mac[3] = { 1, 2, 3 }, *rap;
static int phase, broken_stop, bad_read, fatal_calls, quiescent, restored_a, restored_b, nmac, reports;
static struct cia_saved { int id; } cia_a = { 1 }, cia_b = { 2 };
static jmp_buf env[4];
static int depth;
static int amt_try(void (*fn)(void))
{
    int code, slot = depth++;
    code = setjmp(env[slot]);
    if (!code) fn();
    depth--;
    return code;
}
static void amt_abort(int code) { longjmp(env[depth - 1], code); }
/* Test escape only: production fatal never unwinds any amt_try. */
static void amt_abort_fatal(int code)
{ fatal_calls++; depth = 1; longjmp(env[0], code); }
static void amt_case(const char *id, int status, const char *why)
{ assert(status == AMT_ST_FAIL); reports++; }
static void stop(void) { if (!broken_stop) quiescent = 1; }
static UWORD csr_r(UWORD r)
{ if (bad_read) amt_abort(9); return quiescent ? C0_STOP : 0; }
static void es_ww(volatile UWORD *p, UWORD v)
{
    if ((uintptr_t)p == 4) { assert(v == 0); quiescent = 1; }
    if ((uintptr_t)p >= 0x84) { assert(quiescent); nmac++; }
}
static void cia_restore(struct cia_saved *s)
{
    assert(quiescent || phase == 0);
    if (s->id == 1) restored_a++; else restored_b++;
}
static void network_body(void)
{
    net_dirty = phase;
    if (phase) { cia_a_saved = cia_b_saved = 1; z_mac_saved = phase == 2; }
    amt_abort(7);                    /* any suite fault/watchdog exception */
}
"""
        source += function(NET, "network_cleanup")
        source += function(NET, "finish_board")
        source += function(NET.replace("void amt_run_network", "static void amt_run_network"),
                           "amt_run_network")
        source += r"""
int main(int argc, char **argv)
{
    int code;
    assert(argc == 2);
    phase = argv[1][0] == 'z' ? 2 : argv[1][0] == 'r' ? 0 : 1;
    broken_stop = argv[1][0] == 'f';
    bad_read = argv[1][0] == 'x';
    code = amt_try(amt_run_network);
    if (broken_stop || bad_read) {
        assert(code == (bad_read ? 9 : AMT_ABORT_USER) && reports == 1 && fatal_calls == 1);
        assert(!restored_a && !restored_b); /* never restore masks first */
    } else {
        assert(code == 7 && !reports && !net_dirty && !fatal_calls);
        assert(restored_a == (phase != 0) && restored_b == (phase != 0));
        assert(nmac == (phase == 2 ? 3 : 0));
    }
    return 0;
}
"""
        return source

    @staticmethod
    def uae_source():
        source = r"""
#include <assert.h>
#include <stdint.h>
#include <setjmp.h>
typedef uint8_t UBYTE;
typedef uint16_t UWORD;
typedef uint32_t ULONG;
#define INT_EXTER 0x2000
#define S0 0x100
#define UB(o) ((volatile UBYTE *)(uintptr_t)(o))
#define UL(o) ((volatile ULONG *)(uintptr_t)(o))
#define R_INTREQ 0
#define R_INTENA 1
#define AMT_ST_FAIL 1
#define AMT_ABORT_USER 3
#define CREG(r) custom[r]
static UWORD custom[2];
static void (*amt_irq_hook[7])(ULONG);
static ULONG clock_now, u_irq, u_spur, u_t0, u_t1, u_bad_status;
static UBYTE u_seq[8], u_expected_status, irq_status = 0x90;
static int drop_at = -1, u_dirty, cleanup_mode, broken_stop, active, fatal_calls, allocations = 1;
static ULONG enables = 1, inten = 1, writes, failures;
static jmp_buf abort_env;
static ULONG frames(void) { return clock_now++; }
static ULONG cia_now(void) { return clock_now; }
static int storm_check(void) { return 0; }
static UBYTE es_rb(volatile UBYTE *p)
{
    if ((uintptr_t)p == S0 + 0x87) return irq_status;
    assert((uintptr_t)p == S0 + 0x93);
    if (cleanup_mode) return active;
    return drop_at < 0 || clock_now != (ULONG)drop_at;
}
static ULONG es_rl(volatile ULONG *p)
{
    assert((uintptr_t)p == 0xf0 || (uintptr_t)p == 0xf4);
    return (uintptr_t)p == 0xf0 ? enables : inten;
}
static void es_wb(volatile UBYTE *p, UBYTE v)
{ assert((uintptr_t)p == S0 + 0x8f && !v); writes++; }
static void es_wl(volatile ULONG *p, ULONG v)
{
    uintptr_t o = (uintptr_t)p;
    assert(!v); writes++;
    if (o == 0xf0) enables = v;
    if (o == 0xf4) inten = v;
    if (o == S0 + 0x80 && !broken_stop) active = 0;
    if (o == 0xe0) { assert(!active && !enables && !inten); allocations = 0; }
}
static void amt_case(const char *id, int status, const char *why)
{ assert(status == AMT_ST_FAIL); failures++; }
static void amt_abort(int code) { assert(code == AMT_ABORT_USER); longjmp(abort_env, 1); }
static void amt_abort_fatal(int code)
{ assert(code == AMT_ABORT_USER); fatal_calls++; longjmp(abort_env, 1); }
"""
        source += "\n".join(function(SOUND, name) for name in ("hook_uae", "u_active_for", "u_cleanup"))
        source += r"""
int main(int argc, char **argv)
{
    unsigned i;
    assert(argc == 2);
    switch (argv[1][0]) {
    case 't':
        u_expected_status = 0x90;
        for (i = 0; i < 400; i++) hook_uae(INT_EXTER);
        assert(u_irq == 400 && !u_bad_status && !u_spur);
        irq_status = 0x81; hook_uae(INT_EXTER);
        assert(u_irq == 401 && u_bad_status == 1);
        irq_status = 0; hook_uae(INT_EXTER);
        assert(u_spur == 1);
        break;
    case 'm':
        assert(u_active_for(25));
        clock_now = 0; drop_at = 1; assert(!u_active_for(25));
        clock_now = 0; drop_at = 17; assert(!u_active_for(25));
        break;
    case 'u':
        u_dirty = cleanup_mode = active = 1;
        u_cleanup();
        assert(!u_dirty && !active && !enables && !inten && !allocations);
        i = writes; u_cleanup(); assert(i == writes);
        break;
    case 'f':
        u_dirty = cleanup_mode = active = broken_stop = 1;
        if (!setjmp(abort_env)) { u_cleanup(); assert(0); }
        assert(failures == 1 && fatal_calls == 1 && allocations == 1 && !u_dirty);
        break;
    default: assert(0);
    }
    return 0;
}
"""
        return source

    def test_complete_arp_identity(self):
        self.assert_program("network", "a")

    def test_lance_length_before_any_payload_read(self):
        self.assert_program("network", "l")

    def test_legacy_rx_bounds_and_enabled_ack(self):
        self.assert_program("network", "z")

    def test_codec_i8_waits_before_next_access(self):
        self.assert_program("codec", "w")

    def test_codec_failed_wait_aborts(self):
        self.assert_program("codec", "s")
        self.assert_program("codec", "c")
        self.assert_program("codec", "f")

    def test_i9_baseline_and_forbidden_sdc_write(self):
        self.assert_program("codec", "i")

    def test_codec_configuration_cleanup(self):
        self.assert_program("codec", "r")

    def test_timer_status_after_first_eight(self):
        self.assert_program("uae", "t")

    def test_mono_active_throughout_not_just_start(self):
        self.assert_program("uae", "m")

    def test_select_one_family_and_require_claim_not_unsafe(self):
        for mode in ("d", "o", "r", "n", "p"):
            with self.subTest(mode=mode):
                self.assert_program("discovery", mode)

    def test_abort_cleanup_phases_and_order(self):
        for mode in ("l", "z", "r", "f", "x"):
            with self.subTest(mode=mode):
                self.assert_program("lifecycle", mode)
        self.assert_program("uae", "u")
        self.assert_program("uae", "f")

    def test_mutations_are_detected(self):
        mutations = (
            ("network", "l", "len < 46 ||", ""),
            ("network", "a", "i < 10; i++", "i < 0; i++"),
            ("network", "z", "after = es_rw(ZW(0x04));",
             "es_ww(ZW(0x04), 0); after = es_rw(ZW(0x04));"),
            ("codec", "w", "if (i == 8)", "if (i == 31)"),
            ("codec", "i", "(g & 0xcc) == 0x00", "(g & 0xc8) == 0x00"),
            ("codec", "r", "cw(12, sav[12]);", "cw(12, 0);"),
            ("codec", "f", "amt_abort_fatal(AMT_ABORT_USER);", "amt_abort(AMT_ABORT_USER);"),
            ("uae", "t", "u_expected_status && v != u_expected_status",
             "u_irq < 8 && u_expected_status && v != u_expected_status"),
            ("uae", "m", "if (!(es_rb(UB(S0 + 0x93)) & 1))\n            active = 0;",
             "if (!(es_rb(UB(S0 + 0x93)) & 1))\n            active = active;"),
            ("uae", "u", "es_wl(UL(S0 + 0x80), 0);", "/* missing stream stop */"),
            ("uae", "f", "amt_abort_fatal(AMT_ABORT_USER);", "amt_abort(AMT_ABORT_USER);"),
            ("discovery", "d", "if (seen[family]++)", "if (0)"),
            ("discovery", "o", "if (!amt_board_claim(bd))", "if (0)"),
            ("lifecycle", "l", "finish_board();", "/* missing abort cleanup */"),
            ("lifecycle", "x", "amt_abort_fatal(code);", "amt_abort(code);"),
        )
        for index, (name, mode, old, new) in enumerate(mutations):
            with self.subTest(name=name, mutation=old):
                source = getattr(self, name)
                self.assertIn(old, source)
                binary = self.compile(source.replace(old, new, 1), f"mutant-{index}")
                self.assertNotEqual(self.run_program(name, mode, binary).returncode, 0)

    def test_source_lifecycle_and_ownership(self):
        for source, body, wrapper in (
                (SOUND, "expsound_body", "amt_run_expsound"),
                (NET, "network_body", "amt_run_network")):
            work = function(source, body)
            self.assertLess(work.index("amt_board_claim(bd)"), work.index("board(bd)" if body == "network_body"
                                                                         else "codec_board(b)"))
            self.assertNotIn("AMT_F_UNSAFE", work)
            self.assertIn("seen[family]++", work)
            self.assertIn("additional same-family board not selected", work)
            # The public entry catches the whole suite, runs cleanup before
            # rethrow, and never relies on the runtime's Amiga-only reset.
            entry = source[source.index("void " + wrapper):]
            self.assertLess(entry.index("amt_try(" + body), entry.index("finish_board();"))
            self.assertLess(entry.index("finish_board();"), entry.index("amt_abort(code)"))
        cleanup = function(SOUND, "expsound_cleanup")
        self.assertLess(cleanup.index("codec_cleanup()"), cleanup.index("cia_restore"))
        self.assertLess(cleanup.index("u_cleanup()"), cleanup.index("cia_restore"))
        cleanup = function(NET, "network_cleanup")
        self.assertLess(cleanup.index("stop()"), cleanup.index("cia_restore"))
        self.assertNotIn("ZW(0x8c)", NET.lower().replace("zw", "ZW"))
        self.assertNotIn("CUSTOM_BASE + 0x0b0", SOUND)
        self.assertNotIn("CREG(0x0b", SOUND)
        # The Paula side of the uaesnd mono test lives in the module that owns
        # Paula: it claims the board before any write and stops the stream
        # after its amt_try, also after an abort.
        self.assertNotIn("uaesnd_mono_paula_capture", SOUND)
        paula = (ROOT / "disk/modules/paula.c").read_text()
        mix = function(paula, "t_uaesnd_mono_mix")
        self.assertLess(mix.index("amt_board_claim(bd)"), mix.index("es_wb("))
        self.assertLess(mix.index("umono_base = bd->addr"), mix.index("UAESND_S_SETPTR), set)"))
        run = paula[paula.index("void amt_run_paula(void)"):]
        run = run[:run.index("\n}\n")]
        self.assertLess(run.index("amt_try(t_uaesnd_mono_mix)"), run.index("umono_stop()"))
        self.assertIn("drained = -1", SOUND)


if __name__ == "__main__":
    unittest.main()
