# timing (module 10, bare phase)

Deterministic beam/clock sanity cases plus result checks of a few fixed CPU loops. Throughput is
not measured here: the bench disk owns it (docs/BENCH.md; the old `perf` records `timing.dbra`,
`move_reg`, `move_mem_chip`, `move_mem_any`, `mulu`, `divu`, `branch` and `fpu` were dropped as
duplicates of its blocks). Hardware used: CIA-B timers A+B chained into a 32-bit E-clock down
counter, saved and restored with cia_hw.h `cia_save`/`cia_restore` (CRA/CRB, latch estimate, OS ICR
mask: events drained at the start and the OS's re-posted at the end, so neither the OS timer periods
nor its ICR see the module), BPLCON0 (LACE; left at $0200), INTREQ VERTB while polling at SR=$2700.

Every poll loop at SR=$2700 (where the watchdog cannot fire) is bounded: beam loops by the CIA E
clock (2n+4 frames' worth for n frames), the CIA-second loop by the VBL count (3x the expected), and
all of them by a poll count when neither clock moves (4M polls). A loop that hits its bound fails its
case with "beam/VERTB/CIA stuck" (`expect.progress` / `got.progress`) instead of hanging the machine.

## cases
| case | intent | rule (reference) | a failure usually means |
|---|---|---|---|
| lines_per_frame_lace | with BPLCON0 LACE set, 8 frames alternate long/short; each frame's length follows the LOF bit read in it (PAL 313/312, NTSC 263/262). The length comes from a copper list (WAIT line 311/261 sets INTREQ DSKBLK, WAIT line 312/262 sets SOFT; read and cleared at each V wrap), not from the highest line a CPU poll saw: a 68000 poll takes ~0.75 line and a non-cycle-exact core may skip lines, so polling missed the last line | HRM ch.3 (interlace), VPOSR LOF; ch.6 beam counter | LOF not toggling, wrong vertical total, V8-V10 read glitch |
| lines_per_frame_nolace | LACE clear: LOF stays constant and every frame has the length LOF says (the test enters with LOF=1, so 313/263); same copper marks | HRM VPOSR/VPOSW LOF | LOF toggling in non-interlace, wrong line count |
| cck_per_line | CIA E-clock brackets around beam-line endpoints over at least 8 frames and 100032 H samples. Require the entire inferred period interval, including up to one line of unknown endpoint H phase and one E tick of counter quantization, within 227 (PAL) or 227.5 (NTSC) +/- 0.25 cck. Sample H in short 64-read blocks to avoid losing complete frames on a 68000; out-of-range H still fails | HRM ch.6 beam position, VHPOSR; E clock = cck/5; NTSC LOL | wrong line duration, out-of-range H, insufficient timing precision, or a stalled beam/CIA; no endpoint-coverage exemptions |
| eclock_cck_ratio | CIA E-clock ticks over 10 whole frames (V wrap to V wrap) x 5 equals the frames' colour clocks (lines from each frame's LOF bit, x 227 PAL, x 227.5 NTSC) within 0.1%; the CPU's delay in noticing each wrap is read from the beam position next to the E-counter read and taken off (`timing.lead0/1_cck_x2`), so the result does not depend on the polling rate. A "beam stuck" verdict names the bound that expired (`timing.bound`); the both-clocks-frozen poll bound is 32M polls (68000/010) or 512M (020+), because a fast emulated CPU keeps running while chipset time is frozen at host sync points. The E counter (TB counting TA underflows) is re-read while TA is within 16 E clocks of its reload, since TB counts the underflow a little later | HRM appendix F (8520 timers count E = CCK/5) | CIA clocked at the wrong rate relative to the beam |
| vbl_per_cia_second | VERTB edges in one CIA second (709379 / 715909 E ticks): 50 +-1 PAL, 60 +-1 NTSC (49.92 / 59.83 Hz) | HRM (frame rate from 227(.5) x 313/263 cck) | VBL rate or CIA rate wrong |
| loop_result_mulu | the MULU loop (100 x 8 `move.w`,`mulu.w` $3333*$5555, timing_asm.s) result = $1110EEEF, same on 100 hot repetitions | PRM MULU | MULU.W result bug (interpreter/JIT divergence) |
| loop_result_divu | the DIVU loop (100 x 8 `move.l`,`divu.w` $123456/$1234) result = $00560100 (rem:quot) | PRM DIVU | DIVU.W result bug |
| loop_result_branch | the branch loop (256 LFSR-driven Bcc/BTST/EOR iterations) result = the C model's | PRM LSR/EOR/BTST/Bcc | flag/branch bug |
| loop_result_fpu | x <- x*0.5 + 1.0 (400 times, extended, round-to-nearest) gives exactly 2.0; skip without FPU or with 68060 PCR.DFP set | 6888x/040/060 FMUL/FADD | FPU arithmetic or FPCR rounding bug |
| cia_restore | after `cia_restore`, CIA-B CRA/CRB read back as `cia_save` found them (START ignored for a one-shot timer) | 8520 data sheet: CRx are read/write except LOAD | CRA/CRB not readable as written |

Focused native regressions (no emulator):
`cd runner && python3 -m unittest tests.test_guest_display_timing -v`.
These execute the guest's verdict helper with quantified E-clock intervals,
including polling phases that miss H endpoints, wrong PAL/NTSC line periods,
overly wide reference intervals and illegal H maxima. The production loop
and short/long-frame rollover accounting are also exercised by
`tests.test_hardware_audit_regressions`; oversized blocks that alias complete
frames cannot pass. Sampled H extrema remain diagnostics, not substitutes for
the independently measured line period.
