# hostspeed (module 26, bare phase, cpux disk)

How fast the emulator runs each costly CPU and FPU instruction, measured against real time. The guest
cannot see host time, so the guest only runs the instructions and the **runner** judges them: the cases
are the runner's pseudo cases `host.speed_<insn>` (docs/cases/host.md, runner/amitorture/cases.py). The
module itself reports no `case` records, only `speed` records (docs/schema.md 1.10).

**The verdict depends on the host.** A slow host may fail where a fast one passes, and that is accepted:
the question is "does this emulator keep real time on this machine". Every failure names the host it was
measured on (CPU model, core count, board, `--jobs`, load average). Compare baselines and checks of
`host.speed_*` only between runs on the same host with the same `--jobs`.

## How a block is measured
- **Guest** (disk/modules/hostspeed.c, hostspeed_asm.s): one block per instruction class: the instruction
  unrolled 64 times inside a DBF loop (16 iterations per unit, an outer counter of units), with operands
  that keep it on its normal path for the whole block (no exception, overflow, denormal, NaN or infinity;
  an instruction that changes its own operand alternates with its inverse, or uses a fixed point). The
  number of units is sized from a short trial run (amt_ticks()) so that the block takes 1 s of
  **emulated** time (HS_BLOCK_MS). The module first asks the host to follow the mailbox (a `wait`
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
  reference noise (the median residual RMS of `stop`, `empty`, `empty_end`, `add_l`, `fadd`).
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
`ns_per_op` tells the cost. Those entries therefore leave the module out (matrix.py
`_hostspeed_only_at_real_speed`: their module mask clears hostspeed), which keeps its 10-55 emulated
seconds per entry off the ~115 max-speed entries of `standard`.

A real slowdown keeps the emulation thread busy. A block that lags while the thread was less than 90 %
busy (`SPEED_CPU_BOUND`) is `untestable`, not `fail`: the lag came from the host (scheduling, another
process), not from the instruction. Seen once on a cycle-exact-speed A500 with two emulators running:
ABCD/SBCD at rtf 1.026 with the thread 38 % busy.

Cost: 1 s of emulated time per block, plus ~0.05 s of sizing; a block that falls behind takes rtf x 1 s
of host time. Blocks per CPU/FPU: 68000/010 10; 68020/030 20, plus 35 with a 6888x; 68040 20 + 6 FPU;
68060 15 + 8 FPU: 10 to 55 emulated seconds per entry.

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
| `speed_empty` / `speed_empty_end` | the DBF loop alone | all | first and last block: drift reference | as `stop`, plus the interpreter's/JIT's basic dispatch (newcpu.cpp m68k_run_*, the JIT's compiled loop) |
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
| `speed_cas` | CAS.L, compare equal | 68020+, non-chip RAM | writes the same value back | CAS (an indivisible RMW cycle); on the JIT a fallback |
| `speed_cas2` | CAS2.L | 68020-68040, non-chip RAM | both compare equal | CAS2 |
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

Skipped, with a `speed` `skip` record (deterministic per CPU/FPU variant): 68020+ instructions on the
68000/010; on the 68060 what it traps as unimplemented integer instructions (MOVEP, CAS2, CHK2, 64-bit
MUL/DIV: the 060 ISP emulates them on real hardware); on the 68040/68060 FPU what the FPSP would emulate
(FMOD FREM FSCALE FGETEXP FGETMAN, the transcendentals, FMOVECR, packed decimal; FINT/FINTRZ on the 68040
only; docs/cases/fpu.md "040/060 restrictions"); CAS/CAS2 without non-chip RAM (chip RAM has no
indivisible read-modify-write cycle); every FPU block without an FPU.
