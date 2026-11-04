# Amiberry emulator map (for diagnosing amitorture failures)

Where amitorture's failures point in Amiberry's source (`src/` in the Amiberry repo). Everything here is a **hint**:
it says which code ran for a given config, not which line is wrong. The runner computes the same mapping
(`runner/amitorture/codemap.py`) and puts it in each REPORT.md problem under "Where to look". When the Amiberry
source tree is found (next to the binary, `--amiberry-src DIR` or `$AMIBERRY_SRC`), handler names and line numbers
come from the source itself. Otherwise they are derived names and are marked "guess".

Checked against Amiberry master at the time of writing (src/newcpu.cpp `build_cpufunctbl`, src/cpustbl.cpp,
src/main.cpp `fixup_cpu`).

## 1. Which CPU core ran

`build_cpufunctbl()` (src/newcpu.cpp) picks one of 56 generated handler tables, `cputbls[lvl][mode]`:

- `lvl = (cpu_model - 68000) / 10`, with 68060 giving 5.
- `mode` comes from the *resolved* config (see the `.resolved.uae` next to every config in a results dir):

| mode | condition (checked in this order) | meaning |
|---|---|---|
| 1 | `cachesize > 0` and `comp_trustbyte=direct` | JIT; table = interpreter fallback for uncompiled opcodes |
| 2 | `cachesize > 0`, `comp_trustbyte` indirect | JIT, indirect memory access |
| 7 | `mmu_model` set, cycle-exact | MMU + CE |
| 6 | `mmu_model` set, `cpu_compatible` | MMU + prefetch |
| 5 | `mmu_model` set | MMU |
| 4 | `cpu_cycle_exact` (cycle_exact=true) | cycle-exact |
| 3 | `cpu_compatible` (also cycle_exact=memory) | prefetch ("more compatible") |
| 0 | otherwise | fast |

`cputbls` (op_smalltbl_N):

| CPU | 0 fast | 1 JIT direct | 2 JIT indirect | 3 compat | 4 CE | 5 MMU | 6 MMU compat | 7 MMU CE |
|---|---|---|---|---|---|---|---|---|
| 68000 | 5 | 45 | 55 | 12 | 14 | - | - | - |
| 68010 | 4 | 44 | 54 | 11 | 13 | - | - | - |
| 68020 | 3 | 43 | 53 | 20 | 21 | - | - | - |
| 68030 | 2 | 42 | 52 | 22 | 23 | 32 | 34 | 35 |
| 68040 | 1 | 41 | 51 | 25 | 25 | 31 | 31 | 31 |
| 68060 | 0 | 40 | 50 | 24 | 24 | 33 | 33 | 33 |

Notes:
- **68040/68060 cycle-exact uses the same table as compatible** (25 / 24). CE on those CPUs only changes the timing
  code (`do_cycles_ce020`, custom.cpp) and `update_68k_cycles` (newcpu.cpp).
- **FPU on a 68000/010:** the F-line `$F200-$F3FF` handlers are copied from op_smalltbl_3 (the 68020 table).
  Amiberry refuses an FPU on 68000/010 in compatible/CE mode (fixup_cpu).
- **Unimplemented opcodes:** opcodes with `unimpclev` at or below the CPU level become `op_illg_1`. On the 68060
  with `cpu_no_unimplemented=false`, the integer instructions the 060 lacks become `op_unimpl_1` (vector 61).
- **`cpu_data_cache`** needs `cpu_compatible`, no JIT and a 68030+. Otherwise fixup_cpu clears it.

## 2. Which handler executes an opcode

gencpu writes one handler per *merged* opcode group: `op_<opcode>_<N>_ff` (flag-producing).
- Opcodes that differ only in register fields share the handler of the lowest opcode of the group (readcpu.cpp
  `handle_merges`). For example, `d291` (ADD.L (A1),D1) runs `op_d090_*`, and every MOVEQ runs `op_7000_*`.
- Identical handlers are shared between tables, so the `<N>` in a function name is **not always the table
  number**. For example, op_smalltbl_5 (68000 fast) uses `op_0030_4_ff`. The exact name is the table entry in
  src/cpustbl.cpp, `{ handler_ff, handler_ff_noret, 0xOPCODE, length, {disp020}, branch }, /* MNEMONIC */`, where
  one of the first two is NULL.

The file is chosen by the handler's `<N>`:

| N | file |
|---|---|
| 0-5 | cpuemu_0.cpp |
| 11, 12 | cpuemu_11.cpp |
| 13, 14 | cpuemu_13.cpp (68000/010 CE) |
| 20, 21, 22, 23 | cpuemu_20.cpp ... cpuemu_23.cpp |
| 24, 25 | cpuemu_24.cpp (68060/68040 compat+CE) |
| 31-35 | cpuemu_31.cpp ... cpuemu_35.cpp (MMU) |
| 40-49 | cpuemu_40.cpp (JIT fallbacks, direct) |
| 50-55 | cpuemu_50.cpp (JIT fallbacks, indirect) |

**JIT (cachesize > 0):**
- Compiled code comes from `src/jit/<arch>/compemu_<arch>.cpp` (`arm` on ARM/AArch64, `x86` on x86-64), with
  handlers `op_<opcode>_0_comp_ff`, or `_comp_nf` when the flags are dead and `comp_nf=true`. The table is
  `src/jit/<arch>/compstbl_<arch>.cpp` (opcode given in decimal).
- An opcode without a compiled handler runs the fallback interpreter table (mode 1/2 above).
- The block compiler, cache flushes and SMC detection are in `src/jit/<arch>/compemu_support_<arch>.cpp`:
  `compile_block`, `flush_icache_hard`, `invalidate_block`, `check_checksum`.
- Flag handling is in `src/jit/<arch>/flags_<arch>.h`, and the JIT FPU in `src/jit/<arch>/compemu_fpp_<arch>.cpp`
  (only with `compfpu=true`).
- `comp_trust*`: `direct` accesses host memory directly and relies on the SIGSEGV fault handler for I/O space
  (`comp_catchfault`). `indirect` goes through the memory banks.

The runner names the handler whenever a CPU case puts the instruction word in `ctx` (or `in`) under one of the
keys `op`, `opcode`, `opc`, `insn`, `iw`, `words`, `opcodes`, `code`, `instr`, `ins`. The value is a hex string
(`"d090"`, `"d0900000"`, `"d090 0001"`), a list of hex words, or an integer.

## 3. Subsystems by module

| amitorture module | Amiberry code (src/) | entry points |
|---|---|---|
| cpu_basic, flags | the handler (section 2); JIT: compemu + flags_<arch>.h | `op_<opcode>_<N>_ff` |
| exceptions | newcpu.cpp | `Exception`, `Exception_normal` (frame formats), `Exception_ce000` (68000 CE), `Exception_mmu030`, `exception3_read` (address error), `op_illg` |
| smc | newcpu.cpp; JIT: compemu_support_<arch>.cpp | `fill_prefetch`, `flush_cpu_caches`, `fill_icache030`; `flush_icache_hard`, `invalidate_block`, `check_checksum` |
| interrupts | custom.cpp, newcpu.cpp, threaddep/thread.h | `INTREQ`, `INTENA`, `INTREQ_f`, `rethink_intreq`; `do_interrupt`, `check_uae_int_request`; `atomic_and` |
| cia | cia.cpp | `CIA_update`, `CIA_calctimers`, `ReadCIAA`/`WriteCIAA`, `ReadCIAB`/`WriteCIAB` |
| blitter | blitter.cpp | `do_blitter`, `blitter_handler`, `blitter_dofast` (immediate), `blitter_end`, `blitter_interrupt` |
| fpu | fpp.cpp + fpp_native.cpp (native, host double) or fpp_softfloat.cpp (`fpu_softfloat=true`); JIT: jit/<arch>/compemu_fpp_<arch>.cpp | `fpuop_arithmetic`, `fpuop_save`, `fpuop_restore`, `fpsr_set_exception`; native `fp_to_exten`/`fp_from_exten`; softfloat `to_exten` |
| timing | newcpu.cpp, custom.cpp | `update_68k_cycles` (cycle unit), `do_cycles_ce`, `do_cycles_ce020` |
| devio (floppy) | disk.cpp | `DISK_update`, `DSKLEN` |
| devio (SCSI) | scsi.cpp, a2091.cpp | `scsi_emulate_cmd`, `dmac_a2091_bput` |
| devio (IDE) | ide.cpp, gayle.cpp | `ide_write_reg`, `gayle_bput` |
| takeover, rt | custom.cpp, newcpu.cpp | `custom_wput_1`, `Exception` |
| threaded CPU (`cpu_threaded=true`) | newcpu.cpp | `m68k_run_2_020` (CPU thread main loop) |

## 4. Known issues worth checking first (also in runner/amitorture/xfail.json)

- **Interrupts from other threads are dropped.** `atomic_and()` in src/threaddep/thread.h uses
  `__atomic_and_fetch`, which returns the **new** value. `check_uae_int_request()` (src/newcpu.cpp) relies on the
  old value: `if (atomic_read(&uae_interrupt) && atomic_and(&uae_interrupt, 0))` and
  `v = atomic_and(&uae_interrupts2[i], 0); if (v) ...`. Both are always 0, so INT2/INT6 raised through
  `uae_interrupts2/6` never get delivered. WinUAE's `atomic_and` returns the old value.
- **68040/68060 cycle unit in CE.** In `update_68k_cycles()` (src/newcpu.cpp), the
  `cpu_memory_cycle_exact && cpu_clock_multiplier == 0` branch reads
  `if (cpu_model >= 68040) {...} if (cpu_model == 68030) {...} else if (68020) ... else cpucycleunit = CYCLE_UNIT/2`.
  The missing `else` makes 68040/060 end up with CYCLE_UNIT/2 instead of /16. This only happens with
  `cpu_multiplier=0`, so the full matrix has `*-ce-mult0` entries to reach it.
- **Native FPU NaN/Inf (fixed by PR #2594).** FRESTORE of a NULL frame, and Inf round trips through extended
  precision, in fpp_native.cpp.

## 5. Config fixups (what "config adjusted by emulator" means)

`fixup_cpu()` / `fixup_prefs()` in src/main.cpp rewrite incompatible settings and print a message. The runner
compares `--dump-config` output with what it wrote: options Amiberry writes only when non-default (`cfgfile_dwrite`,
e.g. `cpu_threaded`, `fpu_softfloat`) count as reset to the default when they are missing. Seen so far:

| written | resolved | rule (main.cpp) |
|---|---|---|
| `cpu_threaded=true` with MMU, compatible, CE or a 68000/010 | false | "Threaded CPU mode is not compatible with ... MMU modes" |
| `cachesize>0` on a 68000/010 | 0 | "JIT requires 68020 or better CPU." |
| `mmu_model` with JIT or a CPU below 68030 | 0 | "MMU emulation requires 68030/040/060 and it is not JIT compatible." |
| `cachesize>0` with cycle-exact | 0 | "JIT and cycle-exact can't be enabled simultaneously." |
| `fpu_softfloat=true` with JIT and `compfpu=true` | false | "JIT FPU emulation is not compatible with softfloat FPU emulation." |
| `cpu_24bit_addressing=true` on 68040/060 | false | "24-bit address space is not supported with 68040/060 configurations." |
| `fpu_model` on 68000/010 compat/CE | 0 | "FPU is not supported with 68000/010 configurations." |
| `cpu_data_cache=true` without compat, with JIT or below 68030 | false | "Data cache emulation requires More compatible ..." |

Amiberry also applies `cpu_cycle_exact`/`cycle_exact` against an *earlier* `cachesize` while parsing
(cfgfile.cpp), so option order matters. The runner writes `cachesize` after the core options.

## 6. GDB

- **Guest (m68k-amigaos-gdb).** Use `debugging_features=gdbserver` plus `gdb_port=N` (cfgfile.cpp; default port
  2345). src/gdb_server.cpp then listens on 127.0.0.1:N, but refuses with `cpu_threaded=true` ("WinUAE GDB: not
  available with the threaded CPU"). Repro packs enable it (port 23597, in the runner's range) and ship `gdb.cmd`.
- **Host.** `host-gdb.cmd` in each repro pack sets the isolated environment, sets breakpoints on the functions
  above and runs Amiberry on the pack's config.
