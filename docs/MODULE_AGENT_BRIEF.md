# Brief for module agents (milestone 2)

You are writing one or more test modules for **amitorture**: a bootable bare-metal Amiga test disk plus a host runner (this repository).

Its purpose is to find bugs and regressions in Amiga emulators (Amiberry first; also WinUAE/FS-UAE, FPGA cores and real hardware). Its output is meant for **an AI to diagnose from**: every failure must say what was tested, the inputs, expected vs actual, and why that is wrong.

## Read first
1. `docs/CONTRACT.md`, all of it, especially §5 (records) and **§10 (modules, rules, ids)**.
2. `disk/rt/mod.h`: the module API. It is authoritative.
3. `disk/modules/rt.c` + `rt_asm.s`: a working example module (vectors, try/abort, hot loop, rich cases).
4. `disk/probe/probe.h` (+ README.md): what you know about the machine (`amt_probe_result`, `AMT_CPU()` ids `AMT_CPU_68000..AMT_CPU_68060`, `AMT_FPU()`, `AMT_MMU()`, `.pal`, chipset fields).

## Hard rules
- **Correct by the manuals,** for the detected CPU/FPU/chipset variant: the M68000 Family Programmer's Reference Manual; the MC68020/030/040/060 user manuals; the MC68881/882 manual; the Amiga Hardware Reference Manual.
  - Never write a test to match what an emulator does.
  - If Amiberry fails a test you believe is correct, keep the test. Re-check your reasoning against the manual and, if you can, find the responsible code in Amiberry's source tree (`src/`, read-only), then report it as a FINDING in your final report: case id, configs, expected vs got, manual reference, suspected Amiberry code.
  - If the manual leaves a result undefined, accept all allowed results and say so.
- **Deterministic:** the same seed and the same variant must give byte-identical records, because JIT on/off and different emulators are compared case by case. Timing values only go in `perf` records or clearly separate `ctx` fields.
- **The JIT runs exactly the same tests.** Never detect or branch on a JIT. Use `amt_hot()` for computational cases where practical, so that interpreted and compiled executions are compared inside one run.
- **Safe on real hardware:** touch only the pools/your BSS/your stack, vectors via `amt_vec_set`, and the hardware you test; restore what the runtime doesn't (CIA registers, blitter, audio, copper). No writes to unmapped space unless `amt_par.flags & AMT_F_UNSAFE`. **Never** touch floppy/HD data except signed test-owned media (devio only).
- **DMA state and the status screen:** a bare module starts with DMAEN|BPLEN|COPEN on, because the runtime's copper list shows the status screen (`disk/rt/screen.c`). If your module tests the display, copper, blitter or Paula DMA/interrupts, measures DMA or beam timing, or needs DMACON or the copper to itself, declare it with `AMT_MF_OWNS_DISPLAY` (`disk/rt/mod.h`): it then starts with all DMA off. Otherwise leave DMACON's display bits, COP1LC and the display registers alone. With the `noscreen` run flag every bare module starts with all DMA off. Details: BUILDING.md section 5.
- **Stable snake_case case ids:** they are baseline keys. Fewer inputs in `AMT_QUICK()` mode, but the same ids.
- **Toolchain:** `/opt/amiga/bin` (vasmm68k_mot Motorola syntax; m68k-amigaos-gcc 16 freestanding, `-m68000` by default; CPU-specific files use the `_020/_030/_040/_060/_fpu` suffix, or vasm `machine`/`mc680x0` directives inside the file). There's no libc; `rt/mem.c` has memcpy/memset; `rt/divmod.s` has the division and multiply helpers. Prefer asm for anything where the exact instruction matters (it nearly always does in CPU tests): gcc picks instructions itself.
- **Write only your own files:**
  - `disk/modules/<yourmodule>*`
  - `docs/cases/<yourmodule>.md` (one line per case: intent, rule with manual reference, what a failure usually means)
  - files your task explicitly names
  
  Do NOT edit `disk/rt/*`, `disk/probe/*`, the Makefile, `tools/*` or other modules. If you need a framework change, describe it in your report (or, if it blocks you, make the smallest change and flag it clearly).
- **Build in your own dir, with only your modules:** `cd disk && make B=build-<mod> MODS="rt <mod>"`.
- **Test in Amiberry:** the binary you were given (`--amiberry`, `$AMIBERRY` or `amiberry` on PATH). Never build or modify it.
  ```
  python3 tests/smoke_disk.py --adf disk/build-<mod>/amitorture.adf --model <A500|A1200|A4000|A3000|A600> \
      --cpu <68000..68060> --chipset <ocs|ecs|aga> --rom <path|aros> --port <your range> [-s option=value ...]
  ```
  - Variant options: `-s fpu_model=68881|68882|68040|68060`, `-s mmu_model=...`, `-s cpu_24bit_addressing=false`, `-s cycle_exact=true`, `-s cpu_compatible=true`, `-s cachesize=16384 -s cycle_exact=false -s cpu_compatible=false` (JIT on. All three are required: with only cachesize, Amiberry silently disables the JIT on A1200/A4000 presets, logging "JIT and cycle-exact can't be enabled simultaneously". Confirm `JIT=` in the emulator log. Compare against the same config with cachesize=0, not against the cycle-exact default. The results must be identical), `-s cpu060_revision=N`.
  - Kickstarts, read-only, in the ROM directory you were given (e.g. `~/Amiberry/ROMs/`):
    - 1.2, 1.3, 2.04, 2.05
    - 3.1: A500/A600/A2000 (40.063), A1200, A3000, A4000 (40.068), A4000T (40.070), CD32
    - 3.2.3 for A1200 and A4000T
    - `--rom aros` for the built-in AROS.
    
    68000 machines need a 68000-capable ROM (1.2/1.3/2.04/2.05/3.1-40.063, or AROS).
  - Use only your assigned serial port range.
  - Temp files go in the repo's `work/`, not /tmp (on the development host /tmp is a small tmpfs that ran out of inodes twice, which blocked every shell command for all agents). **Prefix every build/test command with `export TMPDIR=<repo>/work/tmp;`**: gcc and Python put their temp files in $TMPDIR. Clean up your own temp dirs under work/tmp when you're done with them.
  - NEVER touch `~/.config/amiberry` or `~/Amiberry`, except reading ROMs.
- **At most `nproc` Amiberry instances machine-wide,** shared by ALL agents. `tests/smoke_disk.py` takes a slot automatically (tools/amislot.py, flock on ~/.cache/amitorture/locks/, or `$XDG_CACHE_HOME`/`$AMT_LOCK_DIR`). Any other script that launches Amiberry must use `from amislot import emulator_slot; with emulator_slot(): ...` around the whole emulator lifetime. Run your configs **sequentially**, not in parallel: a wait for a slot is normal.
- **Never wait with `until ! pgrep -f "<pattern>"`.** The waiting shell's own command line contains the pattern, so pgrep always finds it and the loop never ends; 11 of these hung for hours. Run long jobs in the foreground with a timeout, or record the PID (`cmd & pid=$!`) and `wait $pid` / `while kill -0 $pid`.
- **No git commits.**

## Host-dependent checks
Anything the CPU can't observe itself (screen output, audio output, input events) goes through the **host request channel**: `amt_request()` in rt/mod.h, protocol in CONTRACT §13. The runner implements the host side (being added in parallel by the runner agent). If a request isn't acknowledged, report the dependent case `untestable`, never `fail`. smoke_disk.py doesn't answer requests: test those paths with the runner (`cd runner && python3 -m amitorture run --config ... --disk-set ...`) once it supports them. Until then, check that your module degrades to `untestable` cleanly.

Your module belongs to exactly one disk group (see the module id table in CONTRACT §10; the groups are in disk/Makefile). Build only your modules (`make B=build-<mod> MODS="rt <mod>"`), and check the in-memory size with `python3 tools/mkadf.py --dump`: the whole group's disk must stay below ~300 KB.

## Final report (concise)
- the files you wrote
- a case list summary (how many cases per module, and what they cover)
- a test matrix you ran (configs × JIT off/on), with pass/fail counts per config
- **FINDINGS**: suspected emulator bugs, with evidence
- anything you couldn't test or verify, and framework changes you need
