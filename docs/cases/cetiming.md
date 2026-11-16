# cetiming (module 27, bare phase)

Cycle-level CPU and chip-bus timing, judged against real hardware. Small fixed loops
(cetiming_asm.s) are copied to chip RAM and timed with CIA-B timer A as a one-shot E-clock counter
(saved and restored with cia_hw.h `cia_save`/`cia_restore`). Each loop runs with two iteration
counts (CPU rows 512 and 4608, DMA rows 256 and 1280); the difference of the two times is the cost
of the extra iterations alone, without the start/stop overhead. Each count is run three times and
the fewest ticks are kept, after one warm-up run. Every run is at SR=$2700.

The guest never judges speed. It reports `perf` records and checks only what is deterministic (the
cases below). The runner turns the perf records into the pseudo cases `host.cet_<name>`
(runner/amitorture/cases.py `CET_REFS`, docs/cases/host.md) on the cycle-exact entries that have a
real-hardware reference.

Hardware touched: CIA-B timer A, BPLCON0, DDFSTRT/STOP, DIWSTRT/STOP, BPL1MOD/BPL2MOD, BPLxPT,
SPRxPT, SPRxCTL, DMACON (BPLEN, SPREN), and after copying the code CACR (68020/030: CI) or CPUSHA
(68040/060). On exit bitplane and sprite DMA are off, BPLCON0 is $0200 and the sprites are disabled (SPRxCTL 0).

## perf records (`"t":"perf","mod":"cetiming"`)
`ops` = extra iterations (N2 - N1), `ticks` = extra E-clock ticks (`tick_hz` 709379 PAL / 715909
NTSC). CPU clocks per iteration = `ticks` x 5 x cpu_multiplier / `ops` (one E tick = 10 colour
clocks; one colour clock = cpu_multiplier / 2 CPU clocks, so 20 CPU clocks on an A1200).

| name | loop body (each ends with `dbra d6`) | body offset mod 4 | beam / DMA |
|---|---|---|---|
| chip_read | `move.w (a0),d0`, chip RAM word | 0 | none |
| chip_write | `move.w d1,(a0)`, chip RAM word | 2 | none |
| move_reg | `move.w d2,d0` | 2 | none |
| lsl_l_8 | `lsl.l #8,d3` | 2 | none |
| mulu_imm | `mulu.w #$5555,d5` | 2 | none |
| dbra | (nothing: the bare `dbra`) | 0 | none |
| move_pair | `move.w d2,d0` / `move.w d3,d1` | 0 | none |
| move_pair_dep | `move.w d2,d0` / `move.w d0,d1` (the second reads the first's result) | 2 | none |
| chip_write_nodma | as chip_write | 0 | synchronised to beam line 96, DMA off |
| chip_write_6pl | as chip_write | 2 | 6 low-res bitplanes (BPLCON0 $6200, DDF $38-$D0, DIW $2C81-$2CC1), from line 96 |
| chip_write_spr | as chip_write | 0 | all 8 sprites (one shared data block, lines $50-$B4), from line 96 |
| chip_write_6pl_spr | as chip_write | 2 | 6 bitplanes and the 8 sprites, from line 96 |
| chip_write_3pl | as chip_write | 2 | 3 low-res bitplanes (BPLCON0 $3200), from line 96 |

**Alignment.** The 68020/030 instruction cache holds longwords: a cached loop's cost depends on
where its words sit (the bare `dbra` takes 7 clocks when its opcode word is longword-aligned, 6
otherwise; MC68020 UM 11.2, instruction-cache fetches). Each loop body starts at the offset mod 4
of the reference capture's loop, so the comparison is like for like (case `layout`).

**Beam.** The DMA rows wait for the top of a frame (line < 20 after a line >= 20), reload the
bitplane and sprite pointers, and start at line 96 (inside the display window and the sprite), so
every run sees the same DMA slots. The longest timed run (1281 iterations at ~13 clocks) ends well
before the window closes.

## Reference (host.cet_<name>)
The references are CPU clocks per iteration measured on a real A1200 (68EC020 at 14.19 MHz, AGA,
PAL, no fast RAM; the loops in chip RAM), from the published real-hardware capture in
CopperlineHQ/Copperline `timing-test` (`REAL` in compare-a1200.py: the rows whose digits were
unambiguous on the CRT photos; only the measurements are used, not code). That capture times each
loop once with CIA E ticks and N iterations (8192, or 1024 for the DMA rows); the per-iteration
cost is (ticks - 5) x 20 / N, the 5 ticks of fixed overhead taken from its bare-dbra row. Rows
read twice differ by at most one tick (about 0.25 %).

| name | real A1200, clocks / iteration |
|---|---|
| chip_read | 16.07 |
| move_reg | 9.00 |
| lsl_l_8 | 11.00 |
| dbra | 7.00 |
| move_pair | 11.00 |
| move_pair_dep | 10.00 |
| chip_write_nodma | 8.01 |
| chip_write_6pl | 12.48 |
| chip_write_spr | 8.03 |
| chip_write_6pl_spr | 12.48 |
| chip_write_3pl | 8.07 |

`chip_write` and `mulu_imm` have no reference (the capture has no unambiguous reading of those
rows): they stay fingerprints. `chip_write_nodma` is the judged plain chip write.

Judged entries: cycle-exact, cpu_model 68020 (68020 and 68EC020 configs), cpu_multiplier 4, AGA
(`cet_refset`). Tolerance +-2 % (`CET_TOL`). The verdict does not depend on the host: in
cycle-exact mode the emulated CIA and CPU share one clock.

**No 68000 reference.** No real-A500 capture of these loops is available; the A500 numbers in
the same repository are emulator output and are not used. On a 68000 the rows are fingerprints
(Amiberry's per-iteration costs are the MC68000 UM section 8 timings plus the taken dbra's 10:
dbra 10, move_reg 14, chip_read/chip_write 18, lsl_l_8 34, mulu_imm 68). The 68030, 040 and 060 have no reference either, and non-cycle-exact cores are not clocked.

## cases
| case | intent | rule (reference) | a failure usually means |
|---|---|---|---|
| layout | every loop body sits at its reference offset mod 4 from the start of the copied code | cetiming_asm.s CET_ALIGN | an assembler/link change moved a loop (test bug, not emulator) |
| timer_no_overflow | no timed run underflowed the 16-bit CIA timer (65535 E ticks) | 8520 timer one-shot mode, ICR TA | the CPU or the CIA runs far slower than expected, or the timer counts too fast |
| beam_sync | each DMA row reached beam line 96 within 4 frames of the frame top | HRM VPOSR/VHPOSR | beam counter or VERTB stuck |
| loop_results | each timed loop computed the right value (read word $A5C3, written word $5A3C, moves, LSL to 0, the MULU sequence, the dependent pair) | MC68000 PRM: MOVE, LSL, MULU, DBcc | an instruction or a chip RAM access gives a wrong result |
| host.cet_* | each row's clocks per iteration on a real A1200 (table above), +-2 % | the real-hardware capture above | see below |

What a `host.cet_*` failure usually means: the 68020 cycle-exact core charges the wrong cost for
an instruction or a cache-hit fetch (`move_reg`, `lsl_l_8`, `dbra`, `move_pair*`: too fast means
the core's per-instruction cost is below the real CPU's), the CPU's chip-bus access is not costed
as on Alice (`chip_read`: a 68020 chip read waits for its slot and the bus cycle; `chip_write*`:
the write takes too many or too few colour clocks), or bitplane/sprite DMA steals the wrong slots
(`chip_write_6pl`, `_spr`, `_6pl_spr`, `_3pl` against `chip_write_nodma`).
