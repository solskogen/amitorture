"""Compile production ownership/probe paths against the actual Amiga NDK structs."""
import os
from pathlib import Path
import re
import shutil
import subprocess
import unittest

from tests.test_guest_display_timing import function

ROOT = Path(__file__).resolve().parents[2]
MOD = (ROOT / "disk/rt/mod.c").read_text()
PROBE = (ROOT / "disk/probe/probe.c").read_text()


def public_function(source, name):
    return function(re.sub(r"(?m)^(?=(?:int|void)\s+" + name + r"\b)", "static ", source), name)


class GuestBoardClaim(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.cc = shutil.which("cc")
        cls.ndk = Path(os.environ.get("NDK_INCLUDE", "/opt/amiga/m68k-amigaos/ndk-include"))
        if not cls.cc or not (cls.ndk / "libraries/configvars.h").is_file():
            raise unittest.SkipTest("native compiler and real Amiga NDK headers required")
        cls.build = ROOT / "work" / f"board-claim-native-{os.getpid()}"
        cls.build.mkdir(parents=True, exist_ok=True)
        cls.addClassCleanup(shutil.rmtree, cls.build)
        source = r"""
#include <assert.h>
#include <stdint.h>
#include <string.h>
#include <exec/execbase.h>
#include <libraries/configvars.h>
#include "probe.h"
#define AMT_END_HALT 0
#define AMT_END_REBOOT 1
#define AMT_END_RETURN 2
static struct amt_probe amt_probe_result, *P = &amt_probe_result;
static struct { ULONG end_action, flags; } amt_par;
static struct ExecBase exec_base, *SysBase = &exec_base;
static int bare_active;
static struct Library library, *amt_expbase;
static struct List board_list;
static struct ConfigDev devices[2];
static volatile struct ConfigDev *board_claims[AMT_PROBE_BOARDS_MAX];
static int binding, closes, opens, sr_reads, open_fails;
static UWORD cpu_sr = 0x2000;
static UWORD amt_get_sr(void) { sr_reads++; return cpu_sr; }
static void amt_set_sr(UWORD value)
{
    /* A restore to the module's enabled IRQ state must follow masking. */
    if (!(value & 0x0700)) assert((cpu_sr & 0x0700) == 0x0700);
    cpu_sr = value;
}
static struct Library *OpenLibrary(CONST_STRPTR name, ULONG version)
{
    (void)name; (void)version; opens++;
    if (open_fails) return 0;
    library.lib_OpenCnt++;
    return &library;
}
static void CloseLibrary(struct Library *lib)
{ assert(lib == &library && lib->lib_OpenCnt); lib->lib_OpenCnt--; closes++; }
static void ObtainConfigBinding(void) { binding++; }
static void ReleaseConfigBinding(void) { assert(binding == 1); binding--; }
static struct ConfigDev *FindConfigDev(struct ConfigDev *old, LONG man, LONG prod)
{
    struct Node *node;
    assert(binding && man == -1 && prod == -1);
    node = old ? old->cd_Node.ln_Succ : board_list.lh_Head;
    return node->ln_Succ ? (struct ConfigDev *)node : 0;
}
static void how(const char *text) { (void)text; }
"""
        source += function(PROBE, "probe_boards")
        source += public_function(PROBE, "amt_probe_release_boards").replace("static void", "void", 1)
        source += public_function(MOD, "amt_board_ok")
        source += public_function(MOD, "amt_board_claim")
        source += r"""
static void append(struct ConfigDev *cd)
{
    struct Node *node = &cd->cd_Node;
    node->ln_Pred = board_list.lh_TailPred;
    node->ln_Succ = (struct Node *)&board_list.lh_Tail;
    board_list.lh_TailPred->ln_Succ = node;
    board_list.lh_TailPred = node;
}
static void remove_device(struct ConfigDev *cd)
{
    cd->cd_Node.ln_Pred->ln_Succ = cd->cd_Node.ln_Succ;
    cd->cd_Node.ln_Succ->ln_Pred = cd->cd_Node.ln_Pred;
}
static void setup(ULONG end_action)
{
    unsigned i;
    memset(&amt_probe_result, 0, sizeof(amt_probe_result));
    memset(&exec_base, 0, sizeof(exec_base));
    memset(&library, 0, sizeof(library));
    memset(&board_list, 0, sizeof(board_list));
    memset(devices, 0, sizeof(devices));
    memset(board_claims, 0, sizeof(board_claims));
    SysBase = &exec_base;
    exec_base.TDNestCnt = exec_base.IDNestCnt = 0;
    bare_active = 1;
    cpu_sr = 0x2000; sr_reads = binding = opens = closes = open_fails = 0;
    amt_par.end_action = end_action; amt_par.flags = 0;
    board_list.lh_Head = (struct Node *)&board_list.lh_Tail;
    board_list.lh_TailPred = (struct Node *)&board_list.lh_Head;
    for (i = 0; i < 2; i++) {
        devices[i].cd_Flags = CDF_CONFIGME | CDF_PROCESSED;
        devices[i].cd_Rom.er_Manufacturer = 6502;
        devices[i].cd_Rom.er_Product = 2;
        devices[i].cd_Rom.er_Type = ERT_ZORROII;
        devices[i].cd_Rom.er_SerialNumber = 100 + i;
        devices[i].cd_BoardAddr = (APTR)(uintptr_t)(0x200000 + 0x10000 * i);
        devices[i].cd_BoardSize = 0x10000;
        append(&devices[i]);
    }
    probe_boards(end_action == AMT_END_HALT || end_action == AMT_END_REBOOT);
    assert(amt_probe_result.n_boards == 2 && !binding && opens == 1 && !amt_expbase);
    assert(amt_probe_result.boards[0].config_dev == &devices[0]);
    assert(amt_probe_result.boards[0].serial == 100);
    if (end_action == AMT_END_RETURN) {
        assert(closes == 1 && !library.lib_OpenCnt);
        assert(!amt_probe_result.board_library && !amt_probe_result.board_list);
    } else {
        assert(closes == 0 && library.lib_OpenCnt == 1);
        assert(amt_probe_result.board_library == &library && amt_probe_result.board_list == &board_list);
    }
}
static void reject(const struct amt_probe_board *b)
{
    struct ConfigDev before[2];
    volatile struct ConfigDev *claims[AMT_PROBE_BOARDS_MAX];
    UWORD sr = cpu_sr;
    memcpy(before, devices, sizeof(devices));
    memcpy(claims, board_claims, sizeof(board_claims));
    assert(!amt_board_claim(b));
    assert(!memcmp(before, devices, sizeof(devices)));
    assert(!memcmp(claims, board_claims, sizeof(board_claims)));
    assert(cpu_sr == sr && !binding);
}
int main(int argc, char **argv)
{
    struct amt_probe_board *b, copy;
    unsigned i;
    assert(argc == 2);
    setup(AMT_END_HALT);
    b = &amt_probe_result.boards[0];
    switch (argv[1][0]) {
    case 'f':                       /* valid unclaimed boot-owned coverage */
        assert(amt_board_claim(b));
        assert(devices[0].cd_Flags == CDF_PROCESSED);
        assert(devices[1].cd_Flags == (CDF_CONFIGME | CDF_PROCESSED));
        assert(b->flags == (CDF_CONFIGME | CDF_PROCESSED));
        assert(cpu_sr == 0x2000 && !amt_par.flags && !closes);
        break;
    case 's':                       /* stale probe flags are not ownership */
        devices[0].cd_Flags = 0;
        assert(amt_board_ok(b)); reject(b);
        amt_par.flags = 9; reject(b); /* unsafe/net cannot claim a driver-owned board */
        devices[0].cd_Flags = CDF_CONFIGME;
        b->flags = 0;
        assert(amt_board_claim(b));
        setup(AMT_END_HALT); b->flags = CDF_SHUTUP;
        assert(amt_board_claim(b));  /* current flags, not the stale snapshot */
        break;
    case 'd':
        devices[0].cd_Driver = &devices[1]; reject(b);
        devices[0].cd_Driver = 0;
        devices[0].cd_Flags |= CDF_SHUTUP; reject(b);
        devices[0].cd_Flags = CDF_CONFIGME | CDF_BADMEMORY; reject(b);
        break;
    case 'r':
        assert(amt_board_claim(b) && amt_board_claim(b));
        assert(amt_board_claim(&amt_probe_result.boards[1]));
        assert(amt_board_claim(b));
        devices[0].cd_Driver = &devices[1]; reject(b);
        devices[0].cd_Driver = 0;
        devices[0].cd_Flags |= CDF_CONFIGME; reject(b);
        break;
    case 'i':                       /* each live identity/span field */
        devices[0].cd_Rom.er_Manufacturer++; reject(b);
        devices[0].cd_Rom.er_Manufacturer--;
        devices[0].cd_Rom.er_Product++; reject(b); devices[0].cd_Rom.er_Product--;
        devices[0].cd_Rom.er_Type ^= 1; reject(b); devices[0].cd_Rom.er_Type ^= 1;
        devices[0].cd_Rom.er_SerialNumber++; reject(b); devices[0].cd_Rom.er_SerialNumber--;
        devices[0].cd_BoardAddr = (APTR)(uintptr_t)0x400000; reject(b);
        devices[0].cd_BoardAddr = (APTR)(uintptr_t)b->addr;
        devices[0].cd_BoardSize++; reject(b); devices[0].cd_BoardSize--;
        b->config_dev = &devices[1]; reject(b);
        b->config_dev = &devices[0]; assert(amt_board_claim(b));
        break;
    case 'w':
        for (i = 0; i < 4; i++) {
            static const ULONG bad_addr[4] = { 0, 0xe80000, 0x9f0001, 0xffff0001 };
            setup(AMT_END_HALT);
            b->addr = bad_addr[i]; b->size = 0x10000;
            devices[0].cd_BoardAddr = (APTR)(uintptr_t)b->addr;
            reject(b);
        }
        setup(AMT_END_HALT); b->size = devices[0].cd_BoardSize = 0; reject(b);
        break;
    case 'b':
        bare_active = 0; reject(b); assert(!sr_reads);
        bare_active = 1;
        exec_base.TDNestCnt = -1; reject(b);
        exec_base.TDNestCnt = 0; exec_base.IDNestCnt = -1; reject(b);
        exec_base.IDNestCnt = 0;
        SysBase = 0; reject(b); SysBase = &exec_base;
        assert(amt_board_claim(b));
        setup(AMT_END_RETURN); reject(b);
        setup(AMT_END_REBOOT); assert(amt_board_claim(b));
        break;
    case 'u':
        copy = *b; reject(&copy); reject(0);
        b->config_dev = 0; reject(b);
        b->config_dev = &devices[0]; amt_probe_result.board_library = 0; reject(b);
        amt_probe_result.board_library = &library; amt_probe_result.board_list = 0; reject(b);
        break;
    case 'm':
        remove_device(&devices[0]);
        devices[0].cd_Node.ln_Succ = devices[0].cd_Node.ln_Pred = (struct Node *)(uintptr_t)1;
        reject(b);                   /* linked snapshot pointer is now removed */
        b->config_dev = (struct ConfigDev *)(uintptr_t)1;
        reject(b);                   /* no dereference of an absent stale pointer */
        break;
    case 'e':
        assert(amt_board_claim(b));
        remove_device(&devices[0]);
        reject(b);                   /* even our cached claim requires current membership */
        break;
    case 'c':
        remove_device(&devices[0]);
        devices[1].cd_Node.ln_Succ = &devices[1].cd_Node;
        reject(b);                   /* corrupt cyclic list is bounded, never ownership */
        break;
    case 'l':
        amt_probe_release_boards(&amt_probe_result);
        assert(closes == 1 && !library.lib_OpenCnt);
        assert(!amt_probe_result.board_library && !amt_probe_result.board_list);
        reject(b);
        amt_probe_release_boards(&amt_probe_result);
        amt_probe_release_boards(0);
        assert(closes == 1);          /* idempotent final RETURN cleanup */
        setup(AMT_END_RETURN);
        /* Closed before any module allocation/error path can run. */
        assert(closes == 1 && !library.lib_OpenCnt);
        amt_probe_release_boards(&amt_probe_result);
        assert(closes == 1 && !library.lib_OpenCnt);
        break;
    case 'a':
        amt_probe_release_boards(&amt_probe_result);
        memset(&amt_probe_result, 0, sizeof(amt_probe_result));
        open_fails = 1;
        probe_boards(1);             /* failed library allocation/open */
        assert(opens == 2 && closes == 1 && !library.lib_OpenCnt && !binding);
        assert(!amt_probe_result.n_boards && !amt_probe_result.board_library &&
               !amt_probe_result.board_list);
        open_fails = 0;
        devices[0].cd_Node.ln_Pred = 0;
        probe_boards(1);             /* list provenance unavailable */
        assert(opens == 3 && closes == 2 && !library.lib_OpenCnt && !binding);
        assert(amt_probe_result.n_boards == 2 && !amt_probe_result.board_library &&
               !amt_probe_result.board_list);
        reject(b);
        break;
    default: assert(0);
    }
    return 0;
}
"""
        cls.source = source
        cls.binary = cls.compile(source, "claim")

    @classmethod
    def compile(cls, source, name):
        src, binary = cls.build / (name + ".c"), cls.build / name
        src.write_text(source)
        result = subprocess.run(
            [cls.cc, "-std=c99", "-Wall", "-Wextra", "-Werror",
             # probe_boards contains an existing 32-bit target-address cast.
             "-Wno-pointer-to-int-cast", "-I", str(cls.ndk),
             "-I", str(ROOT / "disk/probe"), "-o", str(binary), str(src)],
            text=True, capture_output=True)
        if result.returncode:
            raise AssertionError(result.stderr)
        return binary

    def run_program(self, mode, binary=None):
        return subprocess.run([str(binary or self.binary), mode], text=True,
                              capture_output=True, timeout=5)

    def test_real_configdev_claim_paths(self):
        for mode in ("f", "s", "d", "r", "i", "w", "b", "u", "m", "e", "c", "l", "a"):
            with self.subTest(mode=mode):
                result = self.run_program(mode)
                self.assertEqual(result.returncode, 0, result.stderr)

    def test_private_provenance_and_no_blocking_claim_calls(self):
        serializer = public_function(PROBE, "amt_probe_json")
        for field in ("config_dev", "serial", "board_library", "board_list"):
            self.assertNotIn("->" + field, serializer)
        claim = public_function(MOD, "amt_board_claim")
        for call in ("FindConfigDev(", "ObtainConfigBinding(", "OpenLibrary(", "Forbid(", "Disable("):
            self.assertNotIn(call, claim)
        self.assertNotIn("eb_Private05", MOD + PROBE)

    def test_return_and_standalone_lifetime_integration(self):
        self.assertNotIn('#include "rt.h"', PROBE)
        self.assertNotIn("amt_par", PROBE)
        self.assertNotIn("AMT_END_", PROBE)
        plain = public_function(PROBE, "amt_probe")
        self.assertIn("amt_probe_with_claims(p, 0);", plain)
        runtime = public_function((ROOT / "disk/rt/main.c").read_text(), "amt_main")
        self.assertRegex(runtime, r"amt_probe_with_claims\(&probe, amt_par.end_action == AMT_END_HALT \|\|"
                         r"\s*amt_par.end_action == AMT_END_REBOOT\);")
        self.assertLess(runtime.index("amt_probe_with_claims("), runtime.index("amt_run_modules()"))
        self.assertRegex(runtime, r"if \(amt_run_modules\(\) < 0\)\s*amt_n_aborted\+\+;")
        start = runtime.index("case AMT_END_RETURN:")
        returning = runtime[start:runtime.index("default:", start)]
        self.assertIn("amt_probe_release_boards(&probe);", returning)
        self.assertLess(returning.index("amt_probe_release_boards"), returning.index("return -1;"))
        self.assertEqual(runtime.count("amt_probe_release_boards("), 1)

    def test_claim_mutations_are_detected(self):
        mutations = (
            ("b", "!bare_active || ", ""),
            ("b", "SysBase->TDNestCnt < 0 || ", ""),
            ("b", " || SysBase->IDNestCnt < 0", ""),
            ("f", "amt_set_sr(sr | 0x0700);", "/* missing atomic section */"),
            ("s", "if (!(flags & CDF_CONFIGME))", "if (0)"),
            ("s", "flags = cd->cd_Flags;", "flags = (UBYTE)b->flags;"),
            ("i", "cd->cd_Rom.er_SerialNumber != b->serial ||", ""),
            ("i", "(uintptr_t)cd->cd_BoardAddr != (uintptr_t)b->addr ||", ""),
            ("m", "cd = 0;", "cd = b->config_dev;"),
            ("e", "if (!cd)\n        goto out;", "if (!cd) cd = b->config_dev;"),
            ("r", "ok = !(flags & CDF_CONFIGME);", "ok = 0;"),
        )
        for index, (mode, old, new) in enumerate(mutations):
            with self.subTest(mutation=old):
                self.assertIn(old, self.source)
                start = self.source.index("static int amt_board_claim(")
                mutated = self.source[:start] + self.source[start:].replace(old, new, 1)
                binary = self.compile(mutated, f"mutant-{index}")
                self.assertNotEqual(self.run_program(mode, binary).returncode, 0)

    def test_lifetime_mutations_are_detected(self):
        mutations = (
            ("l", "if (boards && retain_boards)", "if (boards && (retain_boards || 1))"),
            ("l", "CloseLibrary(lib);", "(void)lib;"),
            ("l", "p->board_library = 0;", "/* stale retained reference */"),
            ("l", "p->board_list = 0;", "/* stale live list pointer */"),
            ("a", "if (boards && retain_boards)", "if (retain_boards)"),
        )
        for index, (mode, old, new) in enumerate(mutations):
            with self.subTest(mutation=old):
                self.assertIn(old, self.source)
                binary = self.compile(self.source.replace(old, new, 1), f"lifetime-mutant-{index}")
                self.assertNotEqual(self.run_program(mode, binary).returncode, 0)


if __name__ == "__main__":
    unittest.main()
