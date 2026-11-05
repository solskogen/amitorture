# timing (module 10, bare phase)

A throughput fingerprint plus deterministic beam/clock sanity cases. Speed is never pass/fail: the
loops become `perf` records that the host compares against bands and ordering rules. Hardware used:
CIA-B timers A+B chained into a 32-bit E-clock down counter, saved and restored with cia_hw.h
`cia_save`/`cia_restore` (CRA/CRB, latch estimate, OS ICR mask: events drained at the start and the
OS's re-posted at the end, so neither the OS timer periods nor its ICR see the module), BPLCON0 (LACE;
left at $0200), INTREQ VERTB while polling at SR=$2700.

Every poll loop at SR=$2700 (where the watchdog cannot fire) is bounded: beam loops by the CIA E
clock (2n+4 frames' worth for n frames), the CIA-second loop by the VBL count (3x the expected), and
all of them by a poll count when neither clock moves (4M polls). A loop that hits its bound fails its
case with "beam/VERTB/CIA stuck" (`expect.progress` / `got.progress`) instead of hanging the machine.

## perf records (`"t":"perf","mod":"timing"`)
Each loop: one warm-up run (outer=1), then three measured runs; `ticks` = fewest CIA E-clock ticks
(709379 Hz PAL / 715909 Hz NTSC, `tick_hz`), `cck` = colour clocks (amt_ticks(), `cck_hz`) of that
run, `runs_ticks`/`runs_cck` all three. `ops` = instructions of the measured kind.

| name | loop (timing_asm.s / timing_fpu.s) | ops |
|---|---|---|
| dbra | 100 x 1000 `dbra d2,*` | 100000 DBRA |
| move_reg | 250 x 100 x 8 `move.l dN,dM` | 200000 MOVE.L |
| move_mem_chip | 100 x 2048-byte copy, `move.l (a0)+,(a1)+`, chip pool | 51200 MOVE.L |
| move_mem_any | the same in the any pool (fast RAM if present) | 51200 MOVE.L |
| mulu | 25 x 100 x 8 (`move.w`,`mulu.w`) $3333*$5555 | 20000 MULU |
| divu | 10 x 100 x 8 (`move.l`,`divu.w`) $123456/$1234 | 8000 DIVU |
| branch | 64 x 256 LFSR-driven Bcc/BTST/EOR iterations | 16384 iterations |
| fpu | 25 x 100 x 4 (`fmul.x`,`fadd.x`), only with an enabled FPU | 20000 FP pairs |

Host rules worth applying: `ticks*5 ~= cck` for every loop (E = CCK/5); within one CPU family the
cycle-exact numbers are stable to a few ticks run to run; a 68040/060 in cycle-exact mode must not be
slower than a 68030 in cycle-exact mode at the same clock (see the Amiberry finding in the report:
newcpu.cpp update_68k_cycles misses an `else`).

## cases
| case | intent | rule (reference) | a failure usually means |
|---|---|---|---|
| lines_per_frame_lace | with BPLCON0 LACE set, 8 frames alternate long/short; each frame's length follows the LOF bit read in it (PAL 313/312, NTSC 263/262). The length comes from a copper list (WAIT line 311/261 sets INTREQ DSKBLK, WAIT line 312/262 sets SOFT; read and cleared at each V wrap), not from the highest line a CPU poll saw: a 68000 poll takes ~0.75 line and a non-cycle-exact core may skip lines, so polling missed the last line | HRM ch.3 (interlace), VPOSR LOF; ch.6 beam counter | LOF not toggling, wrong vertical total, V8-V10 read glitch |
| lines_per_frame_nolace | LACE clear: LOF stays constant and every frame has the length LOF says (the test enters with LOF=1, so 313/263); same copper marks | HRM VPOSR/VPOSW LOF | LOF toggling in non-interlace, wrong line count |
| cck_per_line | VHPOSR H counter, sampled in blocks of 1000 with a pseudo-random period over at least 8 frames and 100 blocks. Pass requires minimum $00 and maximum $E2/$E1 (PAL) or $E3/$E2 (NTSC). Values above the architectural line end fail. Missing endpoints is `untestable`: instruction/block timing can quantize reads to a subset of H positions despite whole-frame coverage, so polling cannot prove the endpoints were absent. This does not relax the independent E-clock/beam-rate checks | HRM ch.6 beam position, VHPOSR; NTSC LOL | out-of-range H counter; missing endpoints means insufficient polling coverage, not a proven short line; "beam stuck" still fails when V never advances (`timing.bound`) |
| eclock_cck_ratio | CIA E-clock ticks over 10 whole frames (V wrap to V wrap) x 5 equals the frames' colour clocks (lines from each frame's LOF bit, x 227 PAL, x 227.5 NTSC) within 0.1%; the CPU's delay in noticing each wrap is read from the beam position next to the E-counter read and taken off (`timing.lead0/1_cck_x2`), so the result does not depend on the polling rate. A "beam stuck" verdict names the bound that expired (`timing.bound`); the both-clocks-frozen poll bound is 32M polls (68000/010) or 512M (020+), because a fast emulated CPU keeps running while chipset time is frozen at host sync points. The E counter (TB counting TA underflows) is re-read while TA is within 16 E clocks of its reload, since TB counts the underflow a little later | HRM appendix F (8520 timers count E = CCK/5) | CIA clocked at the wrong rate relative to the beam |
| vbl_per_cia_second | VERTB edges in one CIA second (709379 / 715909 E ticks): 50 +-1 PAL, 60 +-1 NTSC (49.92 / 59.83 Hz) | HRM (frame rate from 227(.5) x 313/263 cck) | VBL rate or CIA rate wrong |
| loop_result_mulu | the perf MULU loop result = $1110EEEF, same on 100 hot repetitions | PRM MULU | MULU.W result bug (interpreter/JIT divergence) |
| loop_result_divu | the DIVU loop result = $00560100 (rem:quot) | PRM DIVU | DIVU.W result bug |
| loop_result_branch | the branch loop result = the C model's | PRM LSR/EOR/BTST/Bcc | flag/branch bug |
| loop_result_fpu | x <- x*0.5 + 1.0 (400 times, extended, round-to-nearest) gives exactly 2.0; skip without FPU or with 68060 PCR.DFP set | 6888x/040/060 FMUL/FADD | FPU arithmetic or FPCR rounding bug |
| cia_restore | after `cia_restore`, CIA-B CRA/CRB read back as `cia_save` found them (START ignored for a one-shot timer) | 8520 data sheet: CRx are read/write except LOAD | CRA/CRB not readable as written |

Focused native regressions (no emulator):
`cd runner && python3 -m unittest tests.test_guest_display_timing -v`.
These execute the guest's verdict helper, including incomplete PAL observations
`$02..$DF`, `$00..$E0` and `$01..$E2`, as well as valid endpoints and illegal
PAL/NTSC maxima. They do not establish which counter positions an emulator
actually visits. The polling loop's LFSR restarts at each 1000-sample block;
its variable instruction delays are not a guarantee of complete bus-phase
coverage. A missing endpoint remains inconclusive, not a pass.
