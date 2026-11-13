# hostspeed (module 26, bare phase, exp disk)

How fast the emulator runs each costly CPU and FPU instruction, measured against real time. The guest
cannot see host time, so the guest only runs the instructions and the **runner** judges them: the cases
are the runner's pseudo cases `host.speed_<insn>` (docs/cases/host.md, runner/amitorture/cases.py). The
module itself reports no `case` records, only `speed` records (docs/schema.md 1.10).
Failed runner pseudo cases make the disk and entry fail, even though the guest
has no failed `case` records and `done.fail` can be zero. Reanalysis applies the
same rule to saved launches from runners that only used the guest verdict.

**The verdict depends on the host.** A slow host may fail where a fast one passes, and that is accepted:
the question is "does this emulator keep real time on this machine". Every failure names the host it was
measured on (CPU model, core count, board, `--jobs`, load average). Compare baselines and checks of
`host.speed_*` only between runs on the same host with the same `--jobs`.

## Benchmark-only build and threaded CPU comparisons

From the repository root, `make -C disk benchmarks` builds just this module and the shared runtime,
not the correctness suite. `make -C disk check-benchmarks` verifies the resulting ADF.
From `runner/`:

```
python3 -m amitorture run --amiberry /path/to/amiberry --kickstarts ~/Amiberry/ROMs \
    --matrix benchmarks --jobs 1 --warp off
```

The matrix automatically selects `disk/build/benchmarks/amitorture.adf`; for a custom build directory
pass `--adf /path/to/benchmarks/amitorture.adf`. Its 23 entries cover every CPU family, including a real
68030 MMU core with a 68882 and an EC030 without MMU, plus `bench-68000-ce`: an A500 on the cycle-exact
core at real speed, where the chipset, not the CPU, sets the host cost (no threaded twin: the threaded CPU
does not run cycle-exact). The fast 68020/EC030 lanes omit the external FPU
to keep their four-way CPU comparisons focused; the 040/060 lanes also cover their internal FPUs.
The supported fast-core 68020, EC030, 68040 and 68060 entries
each run **interpreter off/on** and **JIT off/on** pairs of `cpu_threaded`. Every other emulator option
in a pair is identical. The 68000, 68010 and 68030/MMU entries get an interpreter `-thr` twin as well;
an emulator that declines the thread for them runs unthreaded and the entry is reported as adjusted.
Cycle-exact entries never get a threaded twin. The report has a separate paired comparison
table, including host operations/s, net ns/op and total process CPU seconds per emulated second.
Pairs are matched by emulator options, binary and run parameters, not by an entry-name suffix.
The table and the summary use the CPU thread that actually ran (the emulator log's "CPU emulation runs
on the CPU/main thread" line), not the requested option; an entry that asked for `cpu_threaded=true` but
ran on the main thread is listed under "requested but not active". The host line records the CPU
frequency governor and driver (e.g. `powersave (amd-pstate-epp)`): a power-saving governor adds wake-up
latency that mostly hurts the threaded CPU's hand-offs.

**Frame blocks** (`stop`, `vbl_poll`, `chipset_load`, `stop_timer`) count frames, not operations: their figure
is the total process CPU seconds per emulated second, lower is better, and `over_idle_per_emu_s` is that minus
the `stop` block's (the extra host cost over an idle CPU). No ns/op is given for them.

### Threaded CPU verdict

The measurements above are informational, with one exception. A threaded entry is compared with its unthreaded
twin in the same run (`thread_speed`, docs/schema.md 5.6a). That makes it a pass/fail check that doesn't
depend on the host, and it's what the two last blocks are for:
- **Real time.** At `cpu_speed=max` Amiberry gives the CPU the host time a frame leaves, so frames stay on time
  and a single block's rtf says little. A threaded CPU can break that: each hardware access it makes that the
  register snapshot doesn't cover is a hand-off to the main thread, and a flood of them held the chipset below
  real time. Kickstart 1.3's Alert routine blinks the LED with back-to-back CIA writes; Amiberry's threaded CPU
  ran it at 30 FPS with the JIT on a Pi 500, where the unthreaded CPU kept 50. On any block where the twin keeps
  real time, the threaded entry must not be more than 0.03 rtf behind real time or the twin, whichever is later
  (beyond both 3 sigma): an unthreaded JIT that runs ahead of real time (rtf < 1) is no bar. `cia_write`
  reproduces the Alert pattern.
- **Recovery.** Where the thread loses a flood to the host, the emulator moves the CPU to the main thread. When the flood
  ends, it must come back. `flood_recovery` alternates floods with quiet integer work, and the runner measures the CPU
  thread in the quiet phases from the per-thread CPU time it samples: the host CPU of every thread but the main one per
  host second (`cases.thread_share`; a thread that ends between two samples leaves that pair out). It must be at least
  0.6. If the floods never moved the CPU (their own share is at least 0.6, the host keeps up), there is nothing to judge.
  Amiberry's timed retries (a hold doubling up to 32 s) gave 0.29 (interpreter) and 0.48 (JIT) on a Pi 500; retrying
  once the would-be requests fall below the flood rate gives about 1.
- **Going back and forth.** Amiberry logs a re-flood when a count on the main thread ended a hold and the thread flooded
  again at once: the count missed requests the thread makes (the Jim Power title's beam polling did that, through
  torn snapshot reads). Two in a row on a threaded entry is a `thread_speed` problem, `host.thread_refloods`. It
  shows only where the host is slow enough to lose the workload on the thread; a single host core does not help
  force it (both threads on one core run a polling workload at about 1 FPS, a separate problem).
- **Idle cost.** An idle AmigaOS waits in STOP and is woken more than a thousand times a second. A CPU thread
  that spins before it sleeps keeps a host core busy: an idle Workbench cost 0.73 host cores threaded against
  0.48 unthreaded. On `stop` and `stop_timer` the threaded entry's total CPU/s must stay within 1.25 x the twin's
  + 0.05. `vbl_poll` is not compared: a polling CPU keeps two threads busy by design.

Why these were added: amitorture didn't notice either regression. Its hardware blocks only read registers that
the snapshot serves without a round trip, `stop` is woken only 50 times a second, and benchmark verdicts were all
informational.

Benchmark-only `speed` records carry `benchmark:1`. Supported, adequately sampled blocks are
**measured**, not pass/fail. Missing or incomplete samples, or 3-sigma timing uncertainty exceeding
5% of the measured rtf, remain **untestable**. Waiting for synchronization is part of the benchmark,
not grounds for applying the normal real-time test's "lag while idle" rejection.
Unsupported instructions emit **skip** with a reason before any calibration or execution.
These are informational measurements, not correctness pseudo cases; a slow host does not fail the
benchmark. Crashes, hangs, malformed records and other infrastructure failures still fail normally.
The normal correctness disk retains its existing real-time speed verdicts.

Use `--jobs 1`, normal pacing and the same host, load and Amiberry configuration when comparing runs.
For just one family, retain the matrix selector and add e.g. `--config 'bench-68020*'`.
Run both the before-change and after-change binaries and repeat runs: off/on is a comparison of the
option, not proof that a particular code change improved it. `amitorture compare` does exactly that
(see RUNNER.md): it alternates the two binaries over several launches and calls each block slower,
faster, borderline or the same. No single timing sample establishes a
speedup. Fast 020+ benchmark entries use `cpu_speed=max`: host throughput and CPU cost matter, not
the paced frame rate. Host ops/s is the block's operation count per emulated second divided by rtf,
so it includes frame pacing; it is not an unrestricted CPU MHz estimate. Net instruction costs can
round to zero when below loop/idle subtraction resolution. The total CPU/s comparison includes all
process threads, so less spinning is visible even if throughput stays the same.

The new RAM, custom-chip and CIA read loops exercise both ordinary memory access and the
CPU-to-chipset handoffs that Amiberry's multi-threaded CPU changes affect. The read loops only read
VHPOSR and CIA-A PRA, neither of which acknowledges interrupts or changes hardware state. `cia_write`
writes only the power LED bit of CIA-A PRA (harmless on a real Amiga: the LED and the audio filter) and
puts it back as it was. `stop_timer` runs CIA-B timer A and its INT6 interrupt only while the block runs.
The generation-specific blocks read control state or copy owned, aligned memory: they never enable
MMU translation, change vector bases, alter cache controls or change PCR settings.

### On-screen progress and scores (real hardware, FPGA)

The benchmark-only disk needs no runner. Without a host it waits 5 s for one ("Benchmark: waiting
for a host"), then shows `Benchmark n/N: <block>` and `About m:ss left.` on the status screen,
updated between blocks (never while one is timed). At the end it shows:

- **Integer:** the geometric mean, over the blocks every CPU runs (add_l, div_w, abcd_sbcd,
  roxl_roxr, movem, trap_rte), of the net operations per second relative to a PAL A500 = 1.00.
  The A500 reference values were measured with this disk on Amiberry's cycle-exact core (68000 at
  7.09 MHz, 512 KB chip + 512 KB slow RAM) and match the 68000's documented cycle counts (ADD.L
  Dn,Dn: 8 cycles, 887k/s; measured 880k/s with DMA).
- **FPU:** the geometric mean of the net operations per second of fadd, fmul, fdiv, fsqrt and
  fmovem, which every FPU (6888x, 68040, 68060) implements in hardware. There is no A500 reference.
- "Per-block details: serial port."

Net operations per second = `n` x colour clock rate / (`cck` - `empty_cck`): the guest's own clock,
with the loop's overhead subtracted. On real hardware and FPGA that is real time. In an emulator it
is emulated time: at `cpu_speed=max` the scores show how much emulated work fits in an emulated
second, not the host's speed. When the runner answers the start request, the screen says so
and the guest asks for a screenshot of the result (`raw/<entry>/shot-*-benchmark.png`). The scores
are also sent as `speed` `score` records (docs/schema.md 1.10); the runner does not judge them.

## How a block is measured
- **Guest** (disk/modules/hostspeed.c, hostspeed_asm.s): one block per instruction class: the instruction
  unrolled 64 times inside a DBF loop (16 iterations per unit, an outer counter of units), with operands
  that keep it on its normal path for the whole block (no exception, overflow, denormal, NaN or infinity;
  an instruction that changes its own operand alternates with its inverse, or uses a fixed point). The
  number of units is sized from a short trial run (amt_ticks()) so that the block takes 1 s of
  **emulated** time (HS_BLOCK_MS). The signed-32-bit record-count limit is applied using the block's
  actual operations per unit, so fast empty reference loops are not needlessly capped at the same
  unit count as 1024-instruction bodies. A capped block may still be too short to sample and then is
  explicitly untestable. The module first asks the host to follow the mailbox (a `wait`
  request, 5 s timeout). While a block runs, the mailbox position says which one it is (module 26, case
  1000 + block index); before it the guest sends `speed` `begin`, after it `speed` `end` with `n`
  (instructions executed) and `timing` (`cck` emulated colour clocks, `empty_cck` the loop alone for the
  same units, `frames`, `units`).
- **Runner** (runner.EntryRun.speed_sample): every mailbox poll (every ~0.26 s) reads the header in one
  IPC batch; while a block runs it keeps the mailbox heartbeat (the guest's VBL count: no records are sent
  inside a block), the host clock (`time.monotonic()` right after the reply) and the emulator's thread CPU
  times (`/proc/<pid>/task/*/schedstat`). Amiberry serves IPC on the emulation thread between frames, so
  every sample is frame-aligned. They are saved in `raw/<entry>/result.json`, `disks[].hostspeed`.
- **Verdict** (cases.speed_blocks): host time = a + b x frame, least squares over the block's samples
  (one sample far off the line through the others, more than 1 ms and 8x their spread, is dropped when 4
  or more are left). **rtf** (real-time factor) = b x the emulated frame rate (PAL 3546895 / (313 x 227)
  = 49.920 Hz, NTSC 3579545 / (263 x 227.5)): host seconds per emulated second. **sigma** = per-sample
  noise / sqrt(Sxx) x frame rate, the noise being the larger of the block's own residual RMS and the run's
  reference noise (the median residual RMS of `stop`, the `empty*` blocks, `add_l`, `fadd`).
  - **fail**: rtf > 1.01 + 3 sigma (`SPEED_RTF_LIMIT`): the emulator ran more than 1 % slower than real
    time while executing the instruction.
  - **pass**: not a fail, and either rtf + 3 sigma <= 1.01 or 3 sigma <= 0.5 % (`SPEED_MAX_NOISE`).
  - **untestable**: no samples (no mailbox, or not run by this runner), fewer than 3 samples inside the
    block, or a noise too large to tell (3 sigma > 0.5 % around the limit). Never a fail on noise.
- **Informational** (the pseudo case's `timing`, banded by baselines; REPORT.md "Host speed per
  instruction"): `cpu_per_emu_s` = host CPU seconds of the emulation thread (the main thread; plus the
  busiest other thread with cpu_threaded=true) per emulated second (1.0 = no headroom; Amiberry spins
  the last 2 ms of every frame, custom.cpp framewait FRAMEWAIT_MIN_MS, so even an idle block shows
  ~0.1); `ns_per_op` = host CPU per instruction net of the emulator's own cost per emulated second (the
  `stop` block) and of the loop (the `empty` block, per DBF iteration); `ratio_ref` = that relative to
  ADD.L (integer) or FADD (FPU). CPU time is not disturbed by other emulators running in parallel and
  also works at cpu_speed=max, where the frame rate stays on time by design.

Measured noise (Raspberry Pi 500, Cortex-A76 x4): per-sample residual 0.02-0.1 ms on blocks that keep up,
3 sigma of rtf 0.03-0.1 %; 1-4 ms on blocks that fall behind (the frames themselves are irregular), still
3 sigma < 2 %. About one sample in 200 is 5-15 ms late (a late IPC reply or a descheduled runner); the
outlier rule drops it.

Known bias: after answering a request, Amiberry's IPC handler waits up to 2 ms for a follow-up on the
emulation thread (osdep/amiberry_ipc_socket.cpp IPC_FOLLOWUP_WAIT), after the frame's deadline was set.
At one poll per ~0.26 s that adds at most ~0.8 % to the rtf of a block that is already at the limit of
its frame budget (`got.ipc_bound` in a failure), and nothing to a block with headroom. The runner's
mailbox polling is the same in every run, so this is the environment every amitorture verdict is made
in; a borderline fail (rtf within ~0.01 of the limit) should be confirmed with a second run.

Where the rtf is meaningful: at cpu_speed=real, the cpu_speed=real fast-core entries (`*-fast-real`,
matrix.SPEED_SPECS, with JIT twins) and the compatible and cycle-exact entries. At cpu_speed=max (the
other fast and JIT entries) Amiberry gives the CPU whatever host time a frame leaves: the frames stay on
time and a slow instruction only lowers the emulated CPU's throughput, so the rtf stays near 1 and only
`ns_per_op` tells the cost. Normal correctness entries therefore leave the module out (matrix.py
`_hostspeed_only_at_real_speed`: their module mask clears hostspeed), which keeps its 10-55 emulated
seconds per entry off the ~115 max-speed entries of `standard`.

A real slowdown keeps the emulation thread busy. A block that lags while the thread was less than 90 %
busy (`SPEED_CPU_BOUND`) is `untestable`, not `fail`: the lag came from the host (scheduling, another
process), not from the instruction. Seen once on a cycle-exact-speed A500 with two emulators running:
ABCD/SBCD at rtf 1.026 with the thread 38 % busy.

Cost: 1 s of emulated time per supported block, plus sizing; a block that falls behind takes rtf x 1 s
of host time. There are 69 block slots, including reference loops; CPU/FPU/MMU and memory capabilities
determine how many are executed. Skipped blocks are not timed.

Not done yet: per-block host profiling. A non-stripped Amiberry build would allow `perf record -p <pid>`
per block (perf_event_paranoid 2 allows user-space sampling of one's own processes), which would name
the emulator function behind a slow instruction. The binary under test here is stripped.

## Blocks (`host.speed_<insn>`)
What a **failure** means for each: the emulator's code path for that instruction costs more host time
than the emulated time the configured core charges for it. On the interpreter fast core (cpu_compatible
=false) every instruction is charged the same few cycles (in the runs here 4 CPU cycles at 14 MHz per
instruction on the 68020, whatever the instruction; the JIT charges per compiled block, ~16), so an
instruction whose emulation takes more than about 1/4 of a microsecond on the host makes it fall behind;
on the cycle-exact and compatible cores the charge is the instruction's real cycle count. Look at the
named code path first, then at the cycle charge (newcpu.cpp, the `cpu_cycles` of the core in use).

| case | instruction(s) | runs on | operands (normal path) | a failure usually means |
|---|---|---|---|---|
| `speed_stop` | STOP #$2000, woken by the VBL | all | one unit = one frame | the emulator cannot keep real time with the CPU idle: chipset/display emulation or frame pacing (custom.cpp framewait), not an instruction |
| `speed_empty` / `speed_empty_end` / `speed_empty_final` | the DBF loop alone | all | drift references; empty_end keeps its original mailbox position, empty_final follows the new blocks | as `stop`, plus the interpreter's/JIT's basic dispatch (newcpu.cpp m68k_run_*, the JIT's compiled loop) |
| `speed_vbl_poll` | benchmark disk only, after `empty_final`: per frame, poll VPOSR.L until line $2C, then until it moves on | all | one unit = one frame; the game/demo beam-wait idiom | each custom-register read is expensive: with the threaded CPU every read is a hand-off to the main thread, so a polling loop can keep two host cores busy |
| `speed_chipset_load` | benchmark disk only: STOP per frame with 6 lowres bitplanes, 8 sprites, a copper list writing 8 colours on every line $2C-$FF and sprite collision detection on | all | one unit = one frame; DMA is switched off and the status screen restored afterwards | chipset emulation cost (custom.cpp bitplane/sprite/copper, drawing.cpp), the cost a busy game adds over `stop` |
| `speed_cia_write` | benchmark disk only, after `chipset_load`: BSET/BCLR #1,$BFE001, 32 pairs per inner loop | all | the power LED bit; put back afterwards. Kickstart 1.3's Alert blink | each CIA write is a hand-off to the main thread with the threaded CPU (newcpu.cpp process_cpu_indirect_memory_write, service_cpu_indirect_request); a threaded entry behind real time here is a `thread_speed` problem |
| `speed_stop_timer` | benchmark disk only: STOP, woken by the VBL and by CIA-B timer A 1500 times a second | all | one unit = one frame; timer and INT6 off again after the block | the cost of waking an idle CPU; with the threaded CPU, how long its idle wait spins (newcpu.cpp cpu_thread_idle_wait). Much more than the unthreaded twin is a `thread_speed` problem |
| `speed_flood_recovery` | benchmark disk only, last: 6 times 1 s of cia_write's BSET/BCLR flood, then 3 s of ADD.L; a fixed schedule (B_FIXED: not sized), about 24 s | all | the quiet phases set mailbox case 1999 (HS_POS_QUIET), the floods the block's own | a threaded CPU that a flood moved to the main thread must come back for the quiet work: its CPU thread's share of a core in the quiet phases must be at least 0.6 (held: about 0.2, the other threads; back: about 1). Below that is a `thread_speed` problem (newcpu.cpp cpu_thread_check_flood) |
| `speed_add_l` | ADD.L Dn,Dn | all | integer reference | basic instruction dispatch too slow for the cycle charge (interpreter cpuemu_*, JIT compemu) |
| `speed_div_w` | DIVU.W / DIVS.W | all | $03E80BB8 / 21846 = 3000 r 1000: a fixed point | cpuemu DIVU/DIVS (getDivu68kCycles/getDivs68kCycles on CE) |
| `speed_abcd_sbcd` | ABCD / SBCD Dn | all | $45 + $27 and back, X clear | BCD helpers in cpuemu |
| `speed_roxl_roxr` | ROXL.L / ROXR.L #7 by register | all | rotate and back | multi-bit rotate through X |
| `speed_movem` | MOVEM.L 11 registers to and from memory | all | fast RAM if present | movem loops and memory banks (memory.cpp) |
| `speed_movep` | MOVEP.L both directions | 68000-68040 | 0(a5) | MOVEP byte-lane emulation; on the JIT a fallback to the interpreter |
| `speed_trap_rte` | TRAP #0 + RTE (vector 32 = an RTE) | all | n = TRAPs | exception entry and return (newcpu.cpp Exception, m68k_rte); on the JIT every exception leaves compiled code |
| `speed_mul_l` | MULU.L / MULS.L 32-bit | 68020+ | x * a * a^-1 mod 2^32 | 32-bit multiply (cpuemu, m68k_mull) |
| `speed_mul_l64` | MULU.L / MULS.L 64-bit (Dh:Dl) | 68020-68040 | as mul_l | 64-bit multiply (m68k_mull) |
| `speed_div_l` | DIVUL.L / DIVSL.L 32-bit | 68020+ | dividend < divisor: it moves between Dq and Dr and back | m68k_divl |
| `speed_div_l64` | DIVU.L / DIVS.L 64/32 | 68020-68040 | $12345678:$369D0368 / $55555556: a fixed point | m68k_divl 64-bit path |
| `speed_bfextu` | BFEXTU (a5){d2:d3} | 68020+ | offset 5, width 19 | memory bit-field helpers |
| `speed_bfffo` | BFFFO (a5){d2:d3} | 68020+ | as bfextu | bit-field find-first-one |
| `speed_cas` | CAS.L, compare equal | 68020+, non-chip-bus RAM | writes the same value back | CAS (an indivisible RMW cycle); on the JIT a fallback |
| `speed_cas2` | CAS2.L | 68020-68040, non-chip-bus RAM | both compare equal | CAS2 |
| `speed_chk2` | CHK2.L (a5),d0 | 68020-68040 | 0 <= 500 <= 1000: no trap | CHK2/CMP2 bounds check |
| `speed_pack_unpk` | PACK / UNPK register forms | 68020+ | sources unchanged | PACK/UNPK |
| `speed_fadd` | FADD.X FPn,FPn | FPU | FPU reference: 1.5 + n x 0.5, exact | FPU dispatch (fpp.cpp fpuop_arithmetic) too slow for the charge |
| `speed_fmul` / `speed_fdiv` | FMUL.X / FDIV.X by 3 and 1/3 alternately | FPU | stays near 1.5 | fpp_mul/fpp_div of the FPU mode (fpp_native.cpp / fpp_softfloat.cpp) |
| `speed_fsqrt` | FSQRT.X of 0.5 | FPU | | fp_sqrt |
| `speed_fmod` / `speed_frem` | FMOVE.X 7.25 then FMOD/FREM.X 2 (n counts FMOD/FREM) | 6888x | | fp_mod/fp_rem (fmodl/remainderl) |
| `speed_fscale` | FSCALE.X by 3 and -3 | 6888x | | fp_scale |
| `speed_fint` / `speed_fintrz` | FINT/FINTRZ.X of 1234.5678 | 6888x, 68060 | | fp_int/fp_intrz and the host rounding mode |
| `speed_fgetexp` / `speed_fgetman` | of 1234.5678 | 6888x | | fp_getexp/fp_getman (frexpl) |
| `speed_fsin` `speed_fcos` `speed_ftan` `speed_fsincos` `speed_fasin` `speed_facos` `speed_fatan` `speed_fatanh` `speed_fsinh` `speed_fcosh` `speed_ftanh` `speed_fetox` `speed_fetoxm1` `speed_ftwotox` `speed_ftentox` `speed_flogn` `speed_flognp1` `speed_flog2` `speed_flog10` | the transcendental of 0.5 | 6888x | in range for every one | the transcendental in the FPU mode in use. Native mode (fpp_native.cpp fp_sin etc.) calls the C library's long-double functions (sinl, asinl, sinhl, expl, logl ...) even though fptype is double on non-x86 hosts: on AArch64 long double is IEEE binary128, done in software, which costs ~0.25-1.3 us per call on a Cortex-A76. Softfloat mode: fpp_softfloat.cpp / softfloat_fpsp.cpp |
| `speed_fmovecr` | FMOVECR #0 (pi) | 6888x | | the constant ROM table (fpp.cpp) |
| `speed_fmove_p_in` / `speed_fmove_p_out` | FMOVE.P from/to memory (k-factor 6) | 6888x | +1.2345678E+03 | packed-decimal conversion (fpp.cpp fpp_to_pack/fpp_from_pack: string and long-double conversions) |
| `speed_fmovem` | FMOVEM.X FP0-FP7 to and from memory | FPU | | fpp_to_exten_fmovem/fpp_from_exten_fmovem: 16 extended-precision conversions per pair |
| `speed_fsave_frestore` | FSAVE/FRESTORE of the idle frame | FPU | | fpuop_save/fpuop_restore frame building |
| `speed_render` | a renderer's inner loop, 16 instructions repeated: FMOVE.S loads, FMUL/FADD (a vector length), FSQRT, FDIV, FCMP + FBGT, FMUL.S <mem>, FMOVE.L to Dn, FMOVE.S store, ADD.L; ns/op is per instruction of the mix | FPU | | the FPU in mixed code: under a JIT also the cost of FPU instructions it does not compile (leaving compiled code, reloading registers) |
| `speed_ram_read` | MOVE.L (a5),d0 | all | owned RAM pool, fast RAM if available | ordinary memory reads and interpreter/JIT memory-bank dispatch |
| `speed_custom_read` | MOVE.W (a0),d0 from VHPOSR | all | read-only custom-chip register | custom-chip access and threaded CPU handoff |
| `speed_cia_read` | MOVE.B (a0),d0 from CIA-A PRA | all | read-only port data, not interrupt status | CIA access and threaded CPU handoff |
| `speed_movec_vbr` | MOVEC VBR,d0 | 68010+ | read vector base, never write it | supervisor control-register access |
| `speed_pmove_tc` | PMOVE TC,(a5) | 68030 with integrated MMU only | read translation control into owned RAM | 68030 MMU instruction handling; skipped on 68020, EC030 and 68040/060, regardless of FPU |
| `speed_move16` | MOVE16 (a0)+,(a1)+ | 68040/68060, including EC/LC | 64 aligned 16-byte copies per inner loop within the pool; n counts MOVE16s, not bytes | 040/060 memory-copy path |
| `speed_movec_pcr` | MOVEC PCR,d0 | 68060, including EC/LC | read processor control, never write it | 060 control-register path |

Skipped, with a `speed` `skip` record (deterministic per CPU/FPU variant): 68020+ instructions on the
68000/010; on the 68060 what it traps as unimplemented integer instructions (MOVEP, CAS2, CHK2, 64-bit
MUL/DIV: the 060 ISP emulates them on real hardware); on the 68040/68060 FPU what the FPSP would emulate
(FMOD FREM FSCALE FGETEXP FGETMAN, the transcendentals, FMOVECR, packed decimal; FINT/FINTRZ on the 68040
only; docs/cases/fpu.md "040/060 restrictions"); CAS/CAS2 without non-chip-bus RAM (chip RAM and
slow RAM at $C00000-$DBFFFF, even when marked MEMF_FAST, have no indivisible read-modify-write
cycle); every FPU block without an FPU. The new blocks additionally check VBR availability (010+),
the 030 integrated MMU, MOVE16 (040+) and PCR (060 only). For example a 68020 reports `pmove_tc`
as skipped, not failed, even with a 68882.
