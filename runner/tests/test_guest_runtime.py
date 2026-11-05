"""Exercise production allocation cleanup and catch-all frame selection without a guest."""
from pathlib import Path
import re
import shutil
import subprocess
import tempfile
import unittest

from tests.test_guest_display_timing import function as static_function
from amitorture import records

ROOT = Path(__file__).resolve().parents[2]


def function(source, name):
    source = re.sub(r"(?m)^(?=(?:int|void)\s+" + name + r"\b)", "static ", source)
    return static_function(source, name)


class AbortRecords(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("native C compiler unavailable")
        tmp = tempfile.TemporaryDirectory(dir=ROOT / "work" / "tmp")
        cls.addClassCleanup(tmp.cleanup)
        cls.binary = Path(tmp.name) / "abort-records"
        mod = (ROOT / "disk/rt/mod.c").read_text()
        header = (ROOT / "disk/rt/mod.h").read_text()
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
struct module { const char *name; int id; };
static const struct module module = { "copper", 13 }, *cur = &module;
static const char *const st_names[] = { "pass", "fail", "skip", "untestable" };
static ULONG m_pass, m_fail, m_skip, m_untestable, case_no;
static ULONG amt_n_pass, amt_n_fail, amt_n_skip, amt_n_untestable, amt_n_aborted;
static ULONG amt_wd_hard = 100, wd_limit = 60000;
static UWORD amt_wd_armed = 1;
static int bare_active = 1, fatal_quarantine, disabled, forbidden;
#define MBOX_STATE_ABORTED 3
static void Disable(void) { disabled++; }
static void Forbid(void) { forbidden++; }
static void amt_watchdog_kick(void) {}
static void mbox_set_pos(int id, ULONG n) { assert(id == 13 && n == case_no); }
static void screen_count(const struct module *m, ULONG p, ULONG f, ULONG s, ULONG u,
                         ULONG tp, ULONG tf, ULONG ts, ULONG tu)
{ (void)m; (void)p; (void)f; (void)s; (void)u; (void)tp; (void)tf; (void)ts; (void)tu; }
static void ser_write(const char *s, ULONG n) { assert(fwrite(s, 1, n, stdout) == n); }
static void mbox_write(const char *s, ULONG n) { (void)s; (void)n; }
static void mbox_beat(void) {}
static void mbox_set_state(ULONG state) {
    assert(state == MBOX_STATE_ABORTED && fatal_quarantine);
    assert(disabled == 1 && forbidden == 1 && !amt_wd_armed);
    assert(amt_n_aborted == 1 && m_fail == 1 && case_no == 1);
    exit(0);
}
"""
        source += "\n".join(re.findall(r"^#define AMT_ABORT_\w+.*$", header, re.M)) + "\n"
        source += re.search(r"struct amt_exc_info \{.*?\n\};", header, re.S)[0]
        source += "\nstatic struct amt_exc_info amt_last_exc;\n"
        source += static_function(out.replace("char *amt_utoa(", "static char *amt_utoa("),
                                  "amt_utoa")
        source += (ROOT / "disk/rt/crc32.c").read_text().replace('#include "rt.h"', "")
        source += out[out.index("#define LINE_MAX"):]
        source += "\n".join(function(mod, name) for name in (
            "count", "amt_case_open", "amt_case_close", "amt_abort_fatal", "report_abort"))
        source += r"""
int main(int argc, char **argv) {
    int code;
    assert(argc == 2);
    code = atoi(argv[1]);
    if (code == 4) {
        amt_abort_fatal(123);
        assert(!"fatal cleanup returned");
    }
    amt_wd_hard = code == 0 ? 0 : 100;
    amt_last_exc.vector = 3;
    amt_last_exc.sr = 0x2708;
    amt_last_exc.pc = 0x12345678;
    for (int i = 0; i < 16; i++)
        amt_last_exc.frame[i] = 0x1000 + i;
    report_abort(code < 2 ? AMT_ABORT_WATCHDOG :
                 code == 2 ? AMT_ABORT_UNEXPECTED : AMT_ABORT_USER);
    assert(amt_n_aborted == 1 && m_fail == 1 && case_no == 1);
    return 0;
}
"""
        cfile = cls.binary.with_suffix(".c")
        cfile.write_text(source)
        subprocess.run([compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                        str(cfile), "-o", str(cls.binary)], check=True,
                       capture_output=True, text=True)

    def test_all_abort_paths_emit_schema_valid_failure_records(self):
        for scenario in range(5):
            with self.subTest(scenario=scenario):
                result = subprocess.run([str(self.binary), str(scenario)], check=True,
                                        capture_output=True, timeout=5)
                lines = result.stdout.splitlines()
                self.assertEqual(len(lines), 1)
                record = records.parse_line(lines[0])
                self.assertIsNotNone(record)
                self.assertEqual(record.problems, [])
                obj = record.obj
                self.assertEqual((obj["mod"], obj["st"]), ("copper", "fail"))
                for key in ("expect", "got"):
                    malformed = dict(obj)
                    del malformed[key]
                    self.assertTrue(records.validate(malformed))
                if scenario == 4:
                    self.assertEqual(obj["case"], "fatal_cleanup")
                    self.assertEqual(obj["got"]["code"], 123)
                    self.assertIn("quarantined", obj["why"])
                else:
                    self.assertEqual((obj["case"], obj["after_case"]), ("aborted", 1))
                    self.assertEqual(obj["expect"]["abort_code"], 0)
                    if scenario < 2:
                        self.assertEqual(obj["got"]["abort_code"], 2)
                        self.assertEqual(obj["limit_frames"], 60000)
                        self.assertIn("hard time limit" if scenario == 0 else
                                      "did not finish", obj["why"])
                    elif scenario == 2:
                        self.assertEqual(obj["got"], {
                            "abort_code": 1, "vector": 3, "sr": "2708", "pc": "12345678"})
                        self.assertEqual(obj["frame"], [f"{0x1000 + i:04x}" for i in range(16)])
                    else:
                        self.assertEqual((obj["got"]["abort_code"], obj["code"]), (3, 3))


class GuestRuntime(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("native C compiler unavailable")
        tmp = tempfile.TemporaryDirectory(dir=ROOT / "work" / "tmp")
        cls.addClassCleanup(tmp.cleanup)
        cls.binary = Path(tmp.name) / "runtime"
        cls.compiler = compiler
        mod = (ROOT / "disk/rt/mod.c").read_text()
        out = (ROOT / "disk/rt/out.c").read_text()
        memory = (ROOT / "disk/modules/memory_os.c").read_text()
        header = (ROOT / "disk/modules/memory.h").read_text()
        rt_header = (ROOT / "disk/rt/rt.h").read_text()
        source = r"""
#include <assert.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <setjmp.h>
typedef uint8_t UBYTE;
typedef unsigned short UWORD;
typedef uintptr_t ULONG;
typedef long LONG;
typedef void *APTR;
#define MEMF_CHIP 1
#define MEMF_FAST 2
#define MEMF_ANY 0
#define MEMF_CLEAR 4
#define AMT_PH_OS 0
#define AMT_PH_BARE 1
#define AMT_CPU_68000 0
#define AMT_CPU() cpu
#define BARE_STACK 32768
typedef const char *CONST_STRPTR;
struct Library { int unused; };
static void *amt_bootio;
enum { AMT_ST_PASS, AMT_ST_FAIL, AMT_ST_SKIP, AMT_ST_UNTESTABLE };
struct Node { struct Node *ln_Succ; };
struct MemHeader {
    struct Node mh_Node;
    APTR mh_Lower, mh_Upper;
    ULONG mh_Attributes;
    int index;
};
static struct { struct { struct Node *lh_Head; } MemList; } sys, *SysBase = &sys;
"""
        source += "\n".join(re.findall(
            r"^#define (?:MBOX_RING_SIZE|MBOX_STATE_\w+|AMT_F_NOMBOX|AMT_END_RETURN)\b[^\n]*",
            rt_header, re.M)) + "\n"
        source += "\n".join(re.findall(r"^#define (?:MEM_|MB_).*$", header, re.M))
        source += "\n" + re.search(r"struct mem_reg \{.*?\n\};", header, re.S)[0]
        source += r"""
static struct mem_reg mem_regs[MEM_MAXREG];
static LONG mem_nreg, mem_os_ran;
static UBYTE *chip_pool, *any_pool, *stack_mem, *chip_alloc, *any_alloc;
static ULONG chip_size, any_size;
static int any_is_chip, cpu;
static UWORD amt_bus_frame_68000;
static UWORD amt_wd_armed;
static ULONG amt_n_aborted;
static volatile int fatal_quarantine;
static void (*return_cleanup[64])(void);
static int n_return_cleanup;
static const char *oom_why, *refuse_why = "busy";
static int saved;
static struct { ULONG flags; int end_action; } amt_par;
struct amt_mbox {
    UBYTE magic[16];
    ULONG version, state, ring_size;
    UBYTE ring[MBOX_RING_SIZE];
};
static volatile struct amt_mbox *amt_mbox;
static UBYTE *mbox_alloc;
struct amt_module { ULONG chip, any; int phase; };
static const struct amt_module os = { 0, 0, AMT_PH_OS };
static const struct amt_module bare = { 64, 80, AMT_PH_BARE };
static const struct amt_module *amt_modules[] = { &os, &bare };
static int amt_n_modules = 2;
static struct {
    UBYTE *raw, *given;
    ULONG size;
    struct MemHeader *owner;
    int freed;
} allocations[32];
static unsigned allocated, freed, alloc_calls, fail_mask;
static int forbidden, screens, modules, quiet = 1, status, callbacks, ide_probes, takeovers;
static int disabled, fatal_state, fatal_code;
static const char *fatal_why;
static jmp_buf fatal_exit;

static void *track_alloc(ULONG size, struct MemHeader *owner)
{
    unsigned n = allocated++;
    UBYTE *raw = malloc(size + 64);
    assert(raw && n < 32);
    allocations[n].raw = raw;
    allocations[n].given = (UBYTE *)(((ULONG)raw + 15) & ~15UL) + 8;
    allocations[n].size = size;
    allocations[n].owner = owner;
    return allocations[n].given;
}
static void *AllocMem(ULONG size, ULONG flags)
{
    (void)flags;
    if (fail_mask & (1U << ++alloc_calls))
        return 0;
    return track_alloc(size, 0);
}
static void free_tracked(void *ptr, ULONG size, struct MemHeader *owner)
{
    unsigned i;
    for (i = 0; i < allocated; i++)
        if (allocations[i].given == ptr && !allocations[i].freed) {
            assert(allocations[i].size == size && allocations[i].owner == owner);
            if (ptr == mbox_alloc) {
                struct amt_mbox *old = (struct amt_mbox *)(((ULONG)ptr + 15) & ~15UL);
                assert(!amt_mbox && !old->magic[0]);
            }
            allocations[i].freed = 1;
            freed++;
            free(allocations[i].raw);
            return;
        }
    assert(!"wrong allocation base or double free");
}
static void FreeMem(void *ptr, ULONG size) { free_tracked(ptr, size, 0); }
static void Deallocate(struct MemHeader *mh, void *ptr, ULONG size)
{ assert(forbidden); free_tracked(ptr, size, mh); }
static void *Allocate(struct MemHeader *mh, ULONG size) { return track_alloc(size, mh); }
static void *AllocAbs(ULONG size, void *ptr)
{
    unsigned n = allocated++;
    allocations[n].given = ptr;
    allocations[n].size = size;
    return ptr;
}
static ULONG TypeOfMem(void *ptr) { (void)ptr; return MEMF_FAST; }
static void Forbid(void) { forbidden++; }
static void Permit(void) { assert(forbidden); forbidden--; }
static void Enable(void) {}
static int enabled(const struct amt_module *m) { (void)m; return 1; }
static void run_module(const struct amt_module *m) { (void)m; modules++; }
static void amt_log(const char *s) { (void)s; }
static void screen_init(void) { screens++; }
static void find_ide(void) { ide_probes++; }
static const char *hw_busy(int cli) { (void)cli; return quiet ? 0 : "busy"; }
static void dos_delay(struct Library *dos, ULONG ticks) { (void)dos; (void)ticks; }
static struct Library *OpenLibrary(CONST_STRPTR name, ULONG version)
{ (void)name; (void)version; return 0; }
static void CloseLibrary(struct Library *dos) { (void)dos; }
static void Disable(void) { disabled++; }
static void beam_frames(ULONG frames) { (void)frames; }
static void takeover_save(int *state) { (void)state; }
static void bare_super(void) {}
static void amt_supervisor_run(void (*fn)(void), void *top)
{ (void)fn; assert(top == stack_mem + BARE_STACK); takeovers++; }
static const char *kind_of(ULONG lo, ULONG attr, UBYTE *bus)
{ (void)lo; (void)attr; *bus = 0; return "fast"; }
static ULONG find_free(struct MemHeader *mh, ULONG step, ULONG rem)
{ return mh->index == 0 && step == 0x10000 && rem == 0 ? 0x10000400 : 0; }
static void amt_case(const char *id, int st, const char *why)
{ (void)id; (void)why; status = st; }
static void amt_case_open(const char *id, int st) { amt_case(id, st, 0); }
static void amt_case_close(void) {}
static void json_obj(const char *s) { (void)s; }
static void json_obj_end(void) {}
static void json_hex(const char *s, ULONG v) { (void)s; (void)v; }
static void json_hexn(const char *s, ULONG v, int width) { (void)s; (void)v; (void)width; }
static void json_int(const char *s, LONG v) { if (!strcmp(s, "code")) fatal_code = v; }
static void json_str(const char *s, const char *v) { (void)s; fatal_why = v; }
static void mbox_set_state(ULONG state) { fatal_state = state; longjmp(fatal_exit, 1); }
"""
        source += "\n".join(function(mod, name) for name in (
            "amt_on_return", "free_test_memory", "amt_return_cleanup",
            "quiesce", "skip_all_oom", "amt_run_modules", "amt_abort_fatal"))
        source += "\n".join(function(out, name) for name in ("mbox_init", "mbox_close"))
        source += "\n".join(function(memory, name) for name in (
            "scpy", "release_blocks", "amt_run_memory_os"))
        source += r"""
static void callback_a(void) { callbacks = callbacks * 10 + 1; }
static void callback_b(void) { callbacks = callbacks * 10 + 2; }
static void assertion_exit(int signal_number) { (void)signal_number; _Exit(3); }
int main(int argc, char **argv)
{
    struct MemHeader regions[3] = { 0 };
    unsigned before;
    signal(SIGABRT, assertion_exit);
    assert(argc == 2);
    switch (argv[1][0]) {
    case 'p':
    case 'q':
    case 'f':
        quiet = argv[1][0] != 'q';
        cpu = quiet ? 0 : 10;
        fail_mask = argv[1][0] == 'f' ? 1U << 2 : 0;
        assert(amt_run_modules() == 0);
        assert(screens == 1 && allocated == 3 && freed == 1);
        assert(chip_pool != chip_alloc && any_pool != any_alloc);
        assert(((ULONG)chip_pool & 15) == 0 && ((ULONG)any_pool & 15) == 0);
        assert(!stack_mem && !forbidden && amt_bus_frame_68000 == (cpu == 0));
        amt_return_cleanup();
        assert(freed == 3 && !chip_pool && !any_pool && !any_alloc && !chip_alloc);
        amt_return_cleanup();
        assert(freed == 3);
        break;
    case '1':
    case '2':
    case '3':
        fail_mask = argv[1][0] == '1' ? 1U << 1 :
                    argv[1][0] == '2' ? (1U << 2) | (1U << 3) : 1U << 3;
        assert(amt_run_modules() == 0);
        assert(oom_why && modules == 2 && !screens && allocated == freed);
        assert(!chip_pool && !any_pool && !stack_mem);
        amt_return_cleanup();
        assert(allocated == freed);
        break;
    case 'c':
        assert(amt_on_return(callback_a) == 0);
        assert(amt_on_return(callback_a) == 0 && n_return_cleanup == 1);
        assert(amt_on_return(callback_b) == 0);
        n_return_cleanup = 64;
        assert(amt_on_return(callback_a) == 0);
        assert(amt_on_return(bare_super) == -1);
        n_return_cleanup = 2;
        amt_return_cleanup();
        assert(callbacks == 21 && n_return_cleanup == 0);
        amt_return_cleanup();
        assert(callbacks == 21);
        break;
    case 'm':
        assert(mbox_init() == 0 && allocated == 1);
        assert(((ULONG)amt_mbox & 15) == 0 && amt_mbox->ring_size == MBOX_RING_SIZE);
        assert(amt_mbox->magic[0] == 'A' && amt_mbox->state == MBOX_STATE_INIT);
        assert((UBYTE *)amt_mbox != mbox_alloc);
        assert(mbox_init() == 0 && allocated == 1);
        mbox_close();
        assert(!amt_mbox && !mbox_alloc && freed == 1);
        mbox_close();
        assert(freed == 1);
        break;
    case 'n':
        amt_par.flags = AMT_F_NOMBOX;
        assert(mbox_init() == 0 && !allocated);
        mbox_close();
        amt_par.flags = 0;
        fail_mask = 1U << 1;
        assert(mbox_init() == -1 && !amt_mbox && !mbox_alloc);
        mbox_close();
        assert(!allocated && !freed);
        break;
    case 'r':
        regions[0].mh_Node.ln_Succ = &regions[1].mh_Node;
        regions[1].mh_Node.ln_Succ = &regions[2].mh_Node;
        regions[1].index = 1;
        SysBase->MemList.lh_Head = &regions[0].mh_Node;
        amt_run_memory_os();
        assert(mem_os_ran && mem_nreg == 2 && allocated == 2 && !freed);
        assert(mem_regs[0].blk[0] == 0x10000000);
        assert(mem_regs[1].plain_alloc == (ULONG)allocations[1].given);
        assert(mem_regs[1].plain_alloc != mem_regs[1].blk[MB_PLAIN]);
        assert(mem_regs[1].plain_owner == &regions[1]);
        before = allocated;
        amt_run_memory_os();
        assert(freed == before && allocated == before + 2 && n_return_cleanup == 1);
        amt_return_cleanup();
        assert(freed == allocated && !mem_nreg && !mem_os_ran && !forbidden);
        amt_return_cleanup();
        assert(freed == allocated);
        break;
    case 's':
        n_return_cleanup = 64;
        amt_run_memory_os();
        assert(!allocated && !mem_os_ran && !mem_nreg && status == AMT_ST_UNTESTABLE);
        break;
    case 't':
        amt_par.end_action = AMT_END_RETURN;
        assert(amt_run_modules() == 0);
        assert(strstr(refuse_why, "CIA timer latches"));
        assert(!ide_probes && !takeovers && !forbidden);
        assert(modules == 2 && screens == 1 && freed == 1);
        amt_return_cleanup();
        assert(freed == allocated);
        break;
    case 'z':
        any_alloc = AllocMem(96, MEMF_FAST);
        any_size = 80;
        amt_wd_armed = 1;
        if (!setjmp(fatal_exit)) {
            amt_abort_fatal(123);
            assert(!"fatal quarantine returned");
        }
        assert(disabled == 1 && forbidden == 1 && !amt_wd_armed);
        assert(fatal_quarantine == 1);
        assert(status == AMT_ST_FAIL && amt_n_aborted == 1 && fatal_code == 123);
        assert(fatal_state == MBOX_STATE_ABORTED && strstr(fatal_why, "quarantined"));
        assert(allocated == 1 && !freed && any_alloc);
        free_test_memory();
        break;
    default: assert(!"unknown scenario");
    }
    return 0;
}
"""
        cfile = cls.binary.with_suffix(".c")
        cfile.write_text(source)
        cls.source = source
        subprocess.run([compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                        str(cfile), "-o", str(cls.binary)], check=True,
                       capture_output=True, text=True)

    def test_cleanup_allocation_paths(self):
        for scenario in ("p", "q", "f", "1", "2", "3", "c", "m", "n", "r", "s", "t", "z"):
            with self.subTest(scenario=scenario):
                subprocess.run([str(self.binary), scenario], check=True,
                               capture_output=True, text=True)

    def test_cleanup_is_only_on_os_return(self):
        main = function((ROOT / "disk/rt/main.c").read_text(), "amt_main")
        branch = main.split("case AMT_END_RETURN:", 1)[1].split("default:", 1)[0]
        self.assertRegex(branch, r"screen_close\(\);\s*amt_probe_release_boards\(&probe\);\s*"
                                 r"amt_return_cleanup\(\);\s*mbox_close\(\);")
        self.assertEqual(main.count("amt_return_cleanup()"), 1)
        module = function((ROOT / "disk/rt/mod.c").read_text(), "run_module")
        self.assertLess(module.index("else if (refuse_why)"), module.index("bare_reset_env();"))
        self.assertIn("amt_case(\"module\", AMT_ST_SKIP, refuse_why);", module)
        fatal = function((ROOT / "disk/rt/mod.c").read_text(), "amt_abort_fatal")
        self.assertNotIn("amt_abort(", fatal)
        self.assertRegex(fatal, r"mbox_set_state\(MBOX_STATE_ABORTED\);\s*for \(;;\)")
        self.assertLess(fatal.index("fatal_quarantine = 1;"), fatal.index("Disable();"))
        recovery = module.split("code = amt_try(bare_body);", 1)[1]
        self.assertLess(recovery.index("if (fatal_quarantine)"), recovery.index("bare_reset_env();"))
        self.assertRegex(module, r"if \(fatal_quarantine\)\s*\{[^}]*amt_set_sr\(0x2700\);[^}]*for \(;;\)")

    def test_old_ownership_mutations_are_rejected(self):
        mutations = (
            ("FreeMem(any_alloc, any_size + 16);", "FreeMem(any_pool, any_size + 16);", "p"),
            ("FreeMem(mbox_alloc, sizeof(struct amt_mbox) + 16);", "(void)mbox_alloc;", "m"),
            ("r->plain_alloc = (ULONG)p;", "r->plain_alloc = ((ULONG)p + 15) & ~15UL;", "r"),
            ("stack_mem = 0;", "(void)stack_mem;", "p"),
            ("if (amt_par.end_action == AMT_END_RETURN)", "if (0)", "t"),
        )
        for before, after, scenario in mutations:
            with self.subTest(mutation=before):
                source = self.source.replace(before, after)
                self.assertNotEqual(source, self.source)
                cfile = self.binary.with_name("mutant.c")
                binary = self.binary.with_name("mutant")
                cfile.write_text(source)
                subprocess.run([self.compiler, "-std=c99", "-Wall", "-Wextra", "-Werror",
                                str(cfile), "-o", str(binary)], check=True,
                               capture_output=True, text=True)
                result = subprocess.run([str(binary), scenario], capture_output=True, text=True)
                self.assertEqual(result.returncode, 3, result.stderr)


class CatchallFrames(unittest.TestCase):
    @staticmethod
    def decode(assembly, cpu, vector, words):
        """Evaluate the capture block's branches and frame loads, not a copied decoder."""
        block = assembly.split("cmp.w\t#3,-2(a0)", 1)[1].split(".raw:", 1)[0]
        lines = block.replace(".grp12:", ".grp12:\n").splitlines()
        labels = {line.strip(): i for i, line in enumerate(lines) if line.strip().startswith(".")}
        pc, zero, greater, result = 0, False, vector > 3, []
        while pc < len(lines):
            line = lines[pc].split(";", 1)[0].strip()
            pc += 1
            if not line or line.startswith("."):
                continue
            if line.startswith("bhi.s"):
                if greater:
                    pc = labels[line.split()[-1] + ":"] + 1
            elif line.startswith("tst.w"):
                self_symbol = line.split()[-1]
                if self_symbol != "_amt_bus_frame_68000":
                    raise AssertionError(self_symbol)
                zero = cpu != 0
            elif line.startswith("beq.s"):
                if zero:
                    pc = labels[line.split()[-1] + ":"] + 1
            elif line.startswith("bra.s"):
                break
            else:
                match = re.fullmatch(r"move\.([wl])\s+(\d*)\(a1\),\(a0\)\+", line)
                if not match:
                    raise AssertionError(line)
                kind, offset = match.groups()
                i = int(offset or 0) // 2
                result.append(words[i] if kind == "w" else words[i] << 16 | words[i + 1])
        return result

    def test_all_cpu_families_and_vectors(self):
        assembly = (ROOT / "disk/rt/bare.s").read_text()
        mod = function((ROOT / "disk/rt/mod.c").read_text(), "amt_run_modules")
        self.assertIn("amt_bus_frame_68000 = AMT_CPU() == AMT_CPU_68000;", mod)
        for cpu in (0, 10, 20, 30, 40, 60):
            for vector in (2, 3, 4, 9, 58):
                words = [0x1122] * 16
                offset = 4 if cpu == 0 and vector in (2, 3) else 0
                words[offset:offset + 3] = [0x2708, 0x1234, 0x5678]
                original = words[:]
                with self.subTest(cpu=cpu, vector=vector):
                    self.assertEqual(self.decode(assembly, cpu, vector, words),
                                     [0x2708, 0x12345678])
                    self.assertEqual(words, original)
        old = re.sub(r"\s*tst\.w\s+_amt_bus_frame_68000\s+beq\.s\s+\.grp12", "", assembly)
        self.assertNotEqual(self.decode(old, 10, 2, [0x2708, 0x1234, 0x5678] + [0] * 13),
                            [0x2708, 0x12345678])
