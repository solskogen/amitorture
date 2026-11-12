"""Benchmark-only build selection, production CPU gates, and threaded comparisons."""
from pathlib import Path
from types import SimpleNamespace
import re
import shutil
import subprocess
import tempfile
import unittest

from amitorture import analysis, cases, matrix, problems, report, runner
from tests.test_guest_display_timing import function
from tests.test_hostspeed import block_recs, samples, sub_with

ROOT = Path(__file__).resolve().parents[2]


class BenchmarkMatrix(unittest.TestCase):
    def test_only_benchmarks_and_every_cpu_family(self):
        entries = matrix.expand("benchmarks")
        self.assertEqual({e["options"]["cpu_model"] for e in entries},
                         {"68000", "68010", "68020", "68030", "68040", "68060"})
        for e in entries:
            self.assertEqual(e["params"]["modules"], matrix.module_mask(["hostspeed"]))
            self.assertEqual(e["disk_groups"], ["exp"])
            self.assertEqual(e["options"]["fpu_softfloat"], "false")
            self.assertFalse(e["id"].endswith("-strict"))
        mmu = next(e for e in entries if e["id"] == "bench-68030")
        self.assertEqual(mmu["options"]["mmu_model"], "68030")

    def test_threaded_pairs_differ_in_exactly_one_option_including_jit(self):
        entries = {e["id"]: e for e in matrix.expand("benchmarks")}
        for base in entries.values():
            if base["twin_of"] or not base["threaded_capable"]:
                continue
            for suffix in ("", "-jit") if base["jit_capable"] else ("",):
                off = entries[base["id"] + suffix]
                on = entries[base["id"] + suffix + "-thr"]
                self.assertEqual(matrix.option_diff(off["options"], on["options"]),
                                 [("cpu_threaded", "false", "true")])
                self.assertEqual(off["jit"], on["jit"])
                self.assertEqual(off["params"], on["params"])
        for e in entries.values():
            if e["options"]["cpu_model"] in ("68000", "68010") or e["options"]["mmu_model"] != "0":
                self.assertFalse(e["jit"])
            if e["options"]["cpu_cycle_exact"] == "true":
                self.assertEqual(e["options"]["cpu_threaded"], "false")
        # threaded twins where Amiberry may now run the CPU thread: 68000/010
        # and the MMU (compatible core) too
        for eid in ("bench-68000-thr", "bench-68010-thr", "bench-68030-thr"):
            self.assertIn(eid, entries)

    def test_make_target_is_isolated_and_contains_no_correctness_module(self):
        make = shutil.which("make")
        if not make:
            self.skipTest("GNU make unavailable")
        with tempfile.TemporaryDirectory(dir=ROOT / "work" / "tmp") as tmp:
            result = subprocess.run([make, "-C", str(ROOT / "disk"), "--dry-run",
                                     f"B={tmp}", "benchmarks"], check=True, capture_output=True, text=True)
            self.assertIn(f"B={tmp}/benchmarks MODS=hostspeed BENCHMARK_ONLY=1", result.stdout)
            self.assertIn("-DAMT_BENCHMARK_ONLY", result.stdout)
            modules = set(re.findall(r" -c -o \S+ (modules/\S+\.c)", result.stdout))
            self.assertEqual(modules, {"modules/hostspeed.c"})
            self.assertIn(f"{tmp}/benchmarks/amitorture.adf", result.stdout)

    def test_benchmark_matrix_does_not_fall_back_to_the_correctness_disk_set(self):
        opts = SimpleNamespace(matrix="benchmarks", adf=None, disk_set=None)
        self.assertIsNone(runner.resolve_disk_set(opts))
        opts.disk_set = str(ROOT / "disk")
        self.assertEqual(runner.resolve_disk_set(opts), opts.disk_set)


class BenchmarkRecords(unittest.TestCase):
    def benchmark(self, rtf=1.5, samples_present=True):
        bl = [block_recs("stop", 1000, 50, 3537240, units=50),
              block_recs("empty", 1001, 160000, 3537240, units=10000),
              block_recs("custom_read", 1002, 102400, 3537240)]
        for block in bl:
            for rec in block:
                rec["benchmark"] = 1
        bl.append([{"t": "speed", "mod": "hostspeed", "ev": "skip", "insn": "pmove_tc", "cls": "int",
                    "pos": 1003, "benchmark": 1, "why": "requires a 68030 with its integrated MMU"}])
        smp = [s for i in range(3) for s in samples(1000 + i, 10.0 + 2 * i, rtf,
                                                   cpu_per_s=0.1 * (i + 1), f0=100 + 60 * i)]
        return sub_with(bl, smp if samples_present else [])

    def test_measurements_cannot_fail_correctness_and_skips_remain_visible(self):
        sub = self.benchmark()
        # A lagging host is not a correctness failure in benchmark-only mode.
        for s in sub["hostspeed"]["samples"]:
            s[3] = int(s[0] * 1.5e9)
        blocks = cases.speed_blocks(sub, sub["_merged"])
        self.assertEqual([b["st"] for b in blocks], ["measured", "measured", "measured", "skip"])
        self.assertAlmostEqual(blocks[2]["rtf"], 1.5, places=4)
        self.assertGreater(blocks[2]["ops_per_host_s"], 0)
        self.assertTrue(all(cases.speed_case(b, sub) is None for b in blocks))
        self.assertEqual(runner.host_failed_cases(sub, sub["_merged"]), [])
        result = dict(sub, id="bench", jit=False, options={"cpu_threaded": "false"})
        summary = report.speed_summary([result])
        self.assertTrue(summary[0]["blocks"][0]["benchmark"])
        text = "\n".join(report._speed_lines(summary))
        self.assertIn("pmove_tc | skip | requires a 68030", text)
        self.assertNotIn("**FAIL**", text)

    def test_guest_score_records_are_not_timed_blocks(self):
        sub = self.benchmark()
        before = cases.speed_blocks(sub, sub["_merged"])
        recs = sub["_merged"].records
        pos = max(r["rec"].get("pos", 0) for r in recs) + 1
        for insn, extra in (("int", {"x100": 242, "ref": "A500"}), ("fpu", {"ops": 13825000})):
            rec = dict({"t": "speed", "mod": "hostspeed", "ev": "score", "insn": insn,
                        "benchmark": 1, "blocks": 6, "pos": pos}, **extra)
            recs.append(dict(recs[-1], rec=rec))
        after = cases.speed_blocks(sub, sub["_merged"])
        self.assertEqual([(b["insn"], b["st"]) for b in after], [(b["insn"], b["st"]) for b in before])
        self.assertEqual(runner.host_failed_cases(sub, sub["_merged"]), [])

    def test_missing_or_incomplete_measurement_is_never_claimed_measured(self):
        sub = self.benchmark(samples_present=False)
        self.assertEqual(cases.speed_blocks(sub, sub["_merged"])[0]["st"], "untestable")
        sub = self.benchmark()
        sub["_merged"].records = [r for r in sub["_merged"].records if r["rec"].get("ev") != "end"]
        self.assertEqual(cases.speed_blocks(sub, sub["_merged"])[0]["st"], "untestable")

    def test_benchmark_measures_synchronization_waits_but_rejects_excessive_noise(self):
        sub = self.benchmark(rtf=1.2)
        # A waiting worker is itself part of the benchmark, not a failed real-time test.
        self.assertEqual(cases.speed_blocks(sub, sub["_merged"])[2]["st"], "measured")
        for i, sample in enumerate(sub["hostspeed"]["samples"]):
            sample[0] += (0.0, 0.1, -0.1, 0.1)[i % 4]
        blocks = cases.speed_blocks(sub, sub["_merged"])
        self.assertEqual(blocks[2]["st"], "untestable")
        self.assertIn("benchmark timing noise", blocks[2]["why"])

    def test_report_pairs_jit_and_interpreter_separately_and_requires_identical_options(self):
        results = []
        for cache, suffix in (("0", ""), ("8192", "-jit")):
            for threaded, thr_suffix in (("false", ""), ("true", "-thr")):
                sub = self.benchmark(rtf=0.9 if threaded == "true" else 1.0)
                sub["options"] = {"cpu_threaded": threaded, "cachesize": cache, "cpu_speed": "max"}
                results.append(dict(sub, id="bench" + suffix + thr_suffix, jit=cache != "0"))
        summary = report.speed_summary(results)
        self.assertEqual(summary[1]["thread_reference"], "bench")
        self.assertEqual(summary[3]["thread_reference"], "bench-jit")
        text = "\n".join(report._threaded_speed_lines(summary))
        self.assertIn("`bench` / `bench-thr`", text)
        self.assertIn("`bench-jit` / `bench-jit-thr`", text)
        self.assertIn("1.111x", text)
        self.assertIn("skip: requires a 68030", text)
        results[1]["options"]["cpu_speed"] = "real"
        summary = report.speed_summary(results)
        self.assertIsNone(summary[1]["thread_reference"])
        self.assertNotIn("`bench` / `bench-thr`", "\n".join(report._threaded_speed_lines(summary)))
        results[1]["options"]["cpu_speed"] = "max"
        results[1]["binary"] = "/another/amiberry"
        self.assertIsNone(report.speed_summary(results)[1]["thread_reference"])
        results[1].pop("binary")
        results[1]["params"] = {"flags": 16}  # noscreen changes the baseline cost
        self.assertIsNone(report.speed_summary(results)[1]["thread_reference"])


class ThreadSpeedVerdict(unittest.TestCase):
    """analysis.thread_speed_items: the threaded twin against the unthreaded one."""

    def entry(self, eid, threaded, rtfs, cpu=None):
        cpu = cpu or {}
        names = ["stop", "cia_write", "stop_timer"]
        bl = [block_recs("stop", 1000, 50, 3537240, units=50),
              block_recs("cia_write", 1001, 102400, 3537240),
              block_recs("stop_timer", 1002, 50, 3537240, units=50)]
        for block in bl:
            for rec in block:
                rec["benchmark"] = 1
        smp = [s for i, name in enumerate(names)
               for s in samples(1000 + i, 10.0 + 3 * i, rtfs.get(name, 1.0), k=6,
                                cpu_per_s=cpu.get(name, 0.2), f0=100 + 90 * i)]
        sub = sub_with(bl, smp, options={"cpu_threaded": threaded, "cpu_speed": "max", "cachesize": "8192"})
        return dict(sub, id=eid, jit=True)

    def items(self, on_rtfs=None, on_cpu=None, off_rtfs=None, off_cpu=None):
        results = [self.entry("bench-jit", "false", off_rtfs or {}, off_cpu),
                   self.entry("bench-jit-thr", "true", on_rtfs or {}, on_cpu)]
        return results, analysis.thread_speed_items(results)

    def test_threaded_twin_behind_real_time_where_the_reference_keeps_up(self):
        _, items = self.items(on_rtfs={"cia_write": 1.35})
        self.assertEqual([(i["entry"], i["ref_entry"], i["case"]) for i in items],
                         [("bench-jit-thr", "bench-jit", "speed_cia_write")])
        self.assertIn("kept up", items[0]["why"])
        self.assertAlmostEqual(items[0]["twin"]["rtf"], 1.35, places=3)
        # within the margin: no item
        self.assertEqual(self.items(on_rtfs={"cia_write": 1.02})[1], [])
        # the unthreaded JIT ahead of real time (rtf < 1), the threaded one on time: no item
        self.assertEqual(self.items(on_rtfs={"cia_write": 0.998}, off_rtfs={"cia_write": 0.919})[1], [])
        # the unthreaded twin falls behind as well: the host, not the thread
        self.assertEqual(self.items(on_rtfs={"cia_write": 1.35}, off_rtfs={"cia_write": 1.3})[1], [])

    def test_noisy_blocks_are_compared_with_their_sigma(self):
        results = [self.entry("bench-jit", "false", {}), self.entry("bench-jit-thr", "true", {"cia_write": 1.35})]
        summary = report.speed_summary(results)
        for e in summary:
            for b in e["blocks"]:
                if b["insn"] == "cia_write":
                    b.update(st="untestable", sigma=0.02)
        from unittest import mock
        with mock.patch.object(report, "speed_summary", return_value=summary):
            self.assertEqual([i["case"] for i in analysis.thread_speed_items(results)], ["speed_cia_write"])
            for e in summary:
                for b in e["blocks"]:
                    if b["insn"] == "cia_write":
                        b["sigma"] = 0.06        # 3 sigma each: 1.0 + 0.03 + 0.36 > 1.35
            self.assertEqual(analysis.thread_speed_items(results), [])

    def test_idle_cpu_thread_that_spins_costs_too_much(self):
        # an idle Workbench on a Pi 500: 0.73 host cores threaded, 0.48 unthreaded
        # (cpu_total_per_emu_s is twice cpu_per_s in these samples)
        _, items = self.items(on_cpu={"stop_timer": 0.365}, off_cpu={"stop_timer": 0.24})
        self.assertEqual([i["case"] for i in items], ["speed_stop_timer"])
        self.assertIn("spins", items[0]["why"])
        # after the fix: 0.45 cores
        self.assertEqual(self.items(on_cpu={"stop_timer": 0.225}, off_cpu={"stop_timer": 0.24})[1], [])
        # cia_write is not an idle block: its cost is not compared
        self.assertEqual(self.items(on_cpu={"cia_write": 0.9}, off_cpu={"cia_write": 0.3})[1], [])

    def test_items_become_a_normal_problem_with_a_readable_title(self):
        results, items = self.items(on_rtfs={"cia_write": 1.35}, on_cpu={"stop_timer": 0.4},
                                    off_cpu={"stop_timer": 0.2})
        for n, it in enumerate(items):
            it["id"] = f"F{n + 1:03d}"
        for r in results:
            r["status"] = "pass"
        probs = problems.group(items, results, None)
        self.assertEqual(len(probs), 1)
        self.assertEqual(probs[0]["severity"], "normal")
        self.assertIn("threaded CPU behind its unthreaded twin (cia_write, stop_timer)", probs[0]["title"])
        text = "\n".join(report._render_item(items[0]))
        self.assertIn("bench-jit (reference, unthreaded)", text)
        self.assertIn("bench-jit-thr (threaded twin)", text)
        self.assertIn("`cpu_threaded` false vs true", text)
        self.assertIn("thread_speed", report.verdict(results, True, probs) + probs[0]["kind"])

    def test_an_inactive_thread_is_not_compared(self):
        results = [self.entry("bench-jit", "false", {}),
                   self.entry("bench-jit-thr", "true", {"cia_write": 1.35})]
        results[1]["adjusted"] = [{"option": "cpu_threaded", "resolved": "false (main thread)"}]
        self.assertEqual(analysis.thread_speed_items(results), [])


class FloodRecovery(unittest.TestCase):
    """flood_recovery: the threaded CPU must come back to its thread after a flood."""

    def entry(self, eid, threaded, quiet_share, flood_share):
        bl = [block_recs("stop", 1000, 50, 3537240, units=50),
              block_recs("flood_recovery", 1001, 1, 3537240 * 21, units=1)]
        for block in bl:
            for rec in block:
                rec["benchmark"] = 1
        smp = samples(1000, 10.0, 1.0, k=6, cpu_per_s=0.2, f0=100)
        t, f = 20.0, 400
        for cycle in range(3):   # flood phase samples at the block's case, quiet ones at 1999
            smp += samples(1001, t, 1.0, k=3, cpu_per_s=flood_share, f0=f)
            t, f = t + 1.0, f + 50
            smp += samples(cases.SPEED_QUIET_POS, t, 1.0, k=8, cpu_per_s=quiet_share, f0=f)
            t, f = t + 3.0, f + 150
        sub = sub_with(bl, smp, options={"cpu_threaded": threaded, "cpu_speed": "max", "cachesize": "0"})
        return dict(sub, id=eid, jit=False)

    def items(self, quiet_share, flood_share):
        results = [self.entry("bench", "false", 0.1, 0.1), self.entry("bench-thr", "true", quiet_share, flood_share)]
        return analysis.thread_speed_items(results)

    def test_shares_are_measured_per_phase(self):
        sub = self.entry("bench-thr", "true", 0.9, 0.1)
        b = {x["insn"]: x for x in cases.speed_blocks(sub, sub["_merged"])}["flood_recovery"]
        self.assertAlmostEqual(b["quiet_thread_share"], 0.9, places=2)
        self.assertAlmostEqual(b["flood_thread_share"], 0.1, places=2)
        self.assertEqual(b["quiet_samples"], 24)

    def test_cpu_held_on_the_main_thread_after_the_flood_fails(self):
        items = self.items(quiet_share=0.1, flood_share=0.1)
        self.assertEqual([(i["entry"], i["case"]) for i in items], [("bench-thr", "speed_flood_recovery")])
        self.assertIn("stays off its thread", items[0]["why"])

    def test_cpu_back_on_its_thread_passes(self):
        self.assertEqual(self.items(quiet_share=0.85, flood_share=0.1), [])

    def test_floods_the_thread_kept_up_with_are_not_judged(self):
        self.assertEqual(self.items(quiet_share=0.3, flood_share=0.9), [])

    def test_a_thread_ending_between_samples_is_left_out(self):
        pts = [{"t": 0.0, "heartbeat": 0, "cpu_main_ns": 0, "cpu_total_ns": 5 * 10 ** 9},
               {"t": 1.0, "heartbeat": 10, "cpu_main_ns": 10 ** 9, "cpu_total_ns": 10 ** 9},   # CPU thread gone
               {"t": 2.0, "heartbeat": 20, "cpu_main_ns": 2 * 10 ** 9, "cpu_total_ns": 22 * 10 ** 8}]
        self.assertAlmostEqual(cases.thread_share(pts), 0.2, places=3)


class GuestBenchmarkGates(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        compiler = shutil.which("cc")
        if not compiler:
            raise unittest.SkipTest("native C compiler unavailable")
        tmp = tempfile.TemporaryDirectory(dir=ROOT / "work" / "tmp")
        cls.addClassCleanup(tmp.cleanup)
        binary = Path(tmp.name) / "benchmark-gates"
        source = (ROOT / "disk/modules/hostspeed.c").read_text()
        probe = (ROOT / "disk/probe/probe.h").read_text()
        table = re.search(r"static const struct hs_blk blocks\[\] = \{.*?\n\};", source, re.S)[0]
        cls.block_flags = {name: flags.strip() for name, flags in re.findall(
            r'\{\s*"(\w+)",\s*hs_\w+,\s*([^}]+)\}', table)}
        c = r"""
#include <stdint.h>
#include <stdio.h>
#include <assert.h>
typedef uint32_t ULONG;
typedef uint16_t UWORD;
typedef ULONG (*hs_fn)(ULONG, void *);
static int probe_cpu, probe_fpu, mmu, chip;
#define AMT_CPU() probe_cpu
#define AMT_FPU() probe_fpu
#define AMT_MMU() mmu
static int amt_any_pool_on_chip_bus(void) { return chip; }
struct hs_blk { const char *insn; hs_fn fn; UWORD flags; };
static ULONG fake_ticks;
static void *hs_mem;
static ULONG amt_ticks(void) { return fake_ticks; }
"""
        c += "\n".join(re.findall(r"^#define (?:AMT_CPU_|AMT_FPU_).*$", probe, re.M)) + "\n"
        c += "\n".join(re.findall(r"^#define B_\w+.*$", source, re.M)) + "\n"
        for fn in sorted(set(re.findall(r"\b(hs_\w+)\s*,", table))):
            c += f"static ULONG {fn}(ULONG n, void *p) {{ (void)p; fake_ticks += (n + 127) / 128; return 0; }}\n"
        c += table + "\n" + "\n".join(function(source, name) for name in
                                    ("skip_why", "per_unit", "time_units", "size_units")) + "\n"
        c += r"""
int main(void) {
    int cpu, fpu;
    const int fpus[] = { AMT_FPU_NONE, AMT_FPU_68881, AMT_FPU_68882, AMT_FPU_68040, AMT_FPU_68060 };
    const struct hs_blk empty = { "empty", hs_empty, 0 };
    const struct hs_blk regular = { "add_l", hs_add_l, 0 };
    const struct hs_blk half = { "fmod", hs_fmod, B_HALF };
    ULONG units = size_units(&empty, 1000000);
    assert(units > 64000000 && units * per_unit(&empty) <= 0x7fffffffUL);
    units = size_units(&regular, 1000000);
    assert(units == 0x7fffffffUL / 1024 && units * per_unit(&regular) <= 0x7fffffffUL);
    units = size_units(&half, 1000000);
    assert(units == 0x7fffffffUL / 512 && units * per_unit(&half) <= 0x7fffffffUL);
    for (cpu = AMT_CPU_68000; cpu <= AMT_CPU_68060; cpu++)
        for (unsigned f = 0; f < sizeof(fpus) / sizeof(*fpus); f++)
            for (mmu = 0; mmu < 2; mmu++)
                for (chip = 0; chip < 2; chip++) {
                    fpu = fpus[f];
                    probe_cpu = cpu;
                    probe_fpu = fpu;
                    for (unsigned b = 0; b < sizeof(blocks) / sizeof(*blocks); b++) {
                        const char *why = skip_why(&blocks[b]);
                        printf("%d\t%d\t%d\t%d\t%s\t%s\n", cpu, fpu, mmu, chip, blocks[b].insn,
                               why ? why : "");
                    }
                }
    return 0;
}
"""
        cfile = binary.with_suffix(".c")
        cfile.write_text(c)
        subprocess.run([compiler, "-std=c99", "-Wall", "-Wextra", "-Werror", "-DAMT_BENCHMARK_ONLY", str(cfile), "-o", str(binary)],
                       check=True, capture_output=True, text=True)
        result = subprocess.run([str(binary)], check=True, capture_output=True, text=True, timeout=10)
        cls.rows = [(int(cpu), int(fpu), int(mmu), int(chip), name, why)
                    for cpu, fpu, mmu, chip, name, why in
                    (line.split("\t") for line in result.stdout.splitlines())]

    def test_generation_specific_and_universal_workloads_across_every_variant(self):
        universal = {"stop", "empty", "add_l", "ram_read", "custom_read", "cia_read", "empty_final",
                     "vbl_poll", "chipset_load", "cia_write", "stop_timer", "flood_recovery"}
        checks = 0
        for cpu, fpu, mmu, chip, name, why in self.rows:
            allowed = (True if name in universal else cpu >= 1 if name == "movec_vbr" else
                       cpu == 5 and bool(mmu) if name == "pmove_tc" else
                       cpu >= 6 if name == "move16" else cpu >= 9 if name == "movec_pcr" else None)
            if allowed is None:
                continue
            checks += 1
            self.assertEqual(not why, allowed, (cpu, fpu, mmu, chip, name, why))
            if not allowed:
                self.assertIn("requires", why)
        self.assertEqual(checks, 12 * 5 * 2 * 2 * 16)

    def test_existing_fpu_integer_and_memory_restrictions_remain_intact(self):
        for cpu, fpu, mmu, chip, name, why in self.rows:
            flags = set(re.findall(r"B_\w+", self.block_flags[name]))
            forbidden = (("B_020" in flags and cpu < 2) or ("B_NO060" in flags and cpu >= 9) or
                         ("B_FPU" in flags and not fpu) or ("B_RMW" in flags and chip) or
                         ("B_FPSP" in flags and fpu in (68040, 68060)) or
                         ("B_FPSP040" in flags and fpu == 68040))
            if forbidden:
                self.assertTrue(why, (cpu, fpu, mmu, chip, name))


if __name__ == "__main__":
    unittest.main()
