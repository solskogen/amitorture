# interrupts (module 6, AMT_PH_BARE)

Audit observation gates: CIA latency requires every requested sample to be
coherent and usable. CIA-B's independent 32-bit E-clock bounds each acquisition
to less than two CIA-A periods; a possible extra reload or torn sample is
untestable, not a short-latency PASS. Both latency producers guard their 64-slot
buffers before writing; saturation disables the source and records overflow.
VBL storm calibration measures sixty-four complete beam periods with CIA-B
brackets around the first and last wraps, at SR=$2700 so the normal runtime
VBL handler cannot hide the boundary; it does not require polling the
terminal line. The period interval includes one E tick of counter quantization
at either end and must have relative width at most 1/512.
Its midpoint keeps expected-count uncertainty below half an IRQ over the
maximum five-second storm, before rounding to nearest and applying the
one-interrupt boundary tolerance. Failed calibration fails `storm_vertb`;
CIA and software-interrupt coverage remains active. Calibration bounds and
the number of measured periods are recorded under `timing`.

Paula interrupt logic and the 680x0 interrupt model. References: Amiga Hardware Reference Manual (HRM)
chapter 7 "System Control Hardware" (INTENA/INTREQ, table of levels), M68000 Family Programmer's
Reference Manual (PRM) section 6 (exception processing, interrupts) and the STOP/MOVE to SR entries,
MC68010-060 user manuals (format $0 frame, vector offset word).

INTREQ bit -> level: TBE/DSKBLK/SOFT 1, PORTS 2, COPER/VERTB/BLIT 3, AUD0-3 4, RBF/DSKSYN 5, EXTER 6;
bit 14 INTEN master, bit 15 SET/CLR. Autovector = 24 + level.

Handlers are in `modules/interrupts_asm.s` (vectors 25-30 replaced for the module). VERTB is always
chained to the runtime handler (frame counter, watchdog); a no-op `amt_irq_hook[3]` is installed (it kept
the pre-fix runtime from clearing other levels' requests on its "spurious" path; harmless now). Exact-sequence tests run
in a beam window right after a VBL. CIA-A TA is used as a timed interrupt source, CIA-B TA+TB as a
32-bit E-clock reference; both CIAs are saved and restored (`modules/cia_hw.h`).

The storm reads each CIA timer HI/LO/HI until coherent, brackets TA with two
equal TB reads, and avoids the TA underflow boundary. Its reference must also
be nondecreasing (modulo a genuine 32-bit wrap). A backward sample fails
`storm_reference_clock`, preserves the start/previous/offending samples in
`timing`, aborts the storm, and makes its three count cases `untestable`; it
is not clamped or retried away. In particular, a one-tick backward sample
must not turn unsigned elapsed time into `$FFFFFFFF` and manufacture a
`storm_vertb` lost-interrupt failure. The Amiberry fast-CPU CIA read adjustment
could fabricate just such a decrement followed by a rebound without changing
the counter; coherent byte reads alone cannot fix that emulator defect.
Healthy runs still require the original exact SOFT count and ±1 CIA/VERTB
bounds. Focused clock arithmetic tests: `cd runner && python3 -m unittest
tests.test_interrupts_clock` (requires a host C compiler).

Held external lines: at module start (both CIA masks clear, ICR drained) the module checks
whether INTREQ PORTS or EXTER comes straight back after a clear, i.e. another device holds
INT2/INT6 (the CD32's Akiko holds INT2 while its own pending+enabled interrupt bits are set,
as the OS leaves them). Then every case that needs that line (map_ports/map_exter,
sr_mask_l2/l6, priority_*, cia_*_timer_level*, the STOP cases with the CIA waker, the
two boundary cases, latency_cia, storm_*) reports `untestable`, a log record says which line,
and INTREQR read-back ignores the held bit. As a last resort the handlers disable a source that
will not clear (log full, or 4096 level-2 storm entries without a CIA-A TA flag), so every case
ends in bounded time. The check is repeated after any failing case that counts interrupts or reads
INTREQR (both CIA masks clear and drained, INTENA bit off): a device that started holding the line
during the module (an IDE drive finishing a command, a network board) makes that case, and every
later case needing the line, `untestable` instead of a fail.

Keyboard: every CIA-A ICR read in a handler checks for a keyboard byte (SP with CRA SPMODE = 0). The
read consumed it, so the handler sets a flag and the C side gives the keyboard its handshake
(`cia_kbd_handshake`) between cases and in the storm loop; the key is lost but the keyboard does not
time out and resync.

Nesting window: after a handler writes its trigger request to INTREQ it waits for two beam line
changes (64-128 us, at most 65536 polls), not a fixed number of reads, so "taken inside the window"
does not depend on how much emulated time a chip read costs.

Timing values (`timing` objects) differ between runs and emulators; pass/fail is on exact counts and
orders only.

| case | intent | rule (reference) | a failure usually means |
|---|---|---|---|
| `map_tbe` ... `map_exter` (14) | each INTREQ bit set by an INTREQ SET write is taken once at its HRM level via autovector 24+L; handler SR S=1 T=0 mask=L; stacked SR = $2000 | HRM INTENA/INTREQ table; PRM 6.3.2 | wrong level mapping, lost or doubled software requests |
| `autovector_format_word` | 68010+: the frame's format/vector word = (24+L)*4 for all 14 sources (skip on 68000) | MC68010+ format $0 frame | wrong vector offset in the frame |
| `inten_master` | INTEN clear blocks a pending SOFT; setting INTEN lets it through | HRM INTENA bit 14 | master enable ignored |
| `intena_source_off` | SOFT pending with its INTENA bit clear: not taken, visible in INTREQR; taken when enabled | HRM | per-source enable ignored |
| `sr_mask_l1` ... `sr_mask_l6` | for masks 0-7: taken iff level > mask (mask 7 blocks 1-6); taken exactly once after lowering to 0 | PRM 6.3.2 | wrong mask comparison, lost/doubled pending request |
| `priority_order` | all six levels pending, mask lowered: entries 6,5,4,3,2,1, each handler completes first | PRM 6.3.2 | wrong priority |
| `priority_back_to_back_pc` | all six frames stack the same PC (taken back to back after each RTE) | instruction-boundary sampling | interrupt delayed into the interrupted code |
| `nest_higher_preempts` | a level-4 request raised inside the level-1 handler preempts it; the inner frame stacks mask 1 | PRM 6.3.2 | no nesting |
| `nest_lower_waits` | a level-1 request raised in the level-4 handler waits for its RTE | PRM 6.3.2 | lower level taken inside a higher handler |
| `nest_same_level_waits` | AUD1 raised in the AUD0 handler waits (level must be > mask) | PRM 6.3.2 | same-level nesting |
| `nest_three_deep` | 1 -> 3 -> 5 nested, stacking masks 1 and 3 | PRM 6.3.2 | nesting depth/mask bugs |
| `movesr_lower_taken_next` | SOFT pending under mask 7; MOVE #$2000,SR: taken before the next instruction (stacked PC = after the MOVE) | interrupts are recognised at the next instruction boundary | late (JIT block end, or delayed IPL) |
| `andisr_lower_taken_next` | same with ANDI #$F8FF,SR | as above | as above |
| `stacked_sr_is_pre_interrupt` | level 4 taken when MOVE #$2315,SR lowers the mask to 3: stacked SR = $2315 (old SR incl. CCR), PC after the MOVE | PRM 6.2: the SR copy saved is the pre-exception SR | new SR stacked, CCR lost, late interrupt |
| `handler_sr_new_mask` | SR in that handler = $2415 (S set, T clear, mask = level, CCR unchanged) | PRM 6.3.2 | wrong mask/CCR in the handler (this is the "stacked SR has the new mask" item of the brief: per the PRM the new mask is in the live SR, the stacked one is the old SR) |
| `intenar_readback` | INTENAR reads back single bits 0-14 and patterns | HRM | read-back bug |
| `intena_setclr` | $80F0, $8F00, $0030, $8000, $0000 -> $00F0,$0FF0,$0FC0,$0FC0,$0FC0 | HRM bit 15 SET/CLR | wrong set/clear semantics |
| `intreqr_readback` | INTREQR reads back bits 0-13 (bit 14 excluded, not a request bit) | HRM | read-back bug |
| `intreq_setclr` | as `intena_setclr` for INTREQ | HRM | as above |
| `cia_a_timer_level2` | a real CIA-A TA underflow (mask $81) is taken once at level 2, ICR = $81 in the handler | HRM: CIA-A -> INT2 | CIA interrupt routing |
| `cia_b_timer_level6` | same for CIA-B at level 6 | HRM: CIA-B -> INT6 | CIA interrupt routing |
| `stop_wakes_pc_after` | STOP #$2000 woken by a CIA-A timer (level 2; VERTB as fallback): stacked PC = after STOP, stacked SR = $2000; time stopped in `timing` | PRM STOP | STOP not stopping, wrong PC/SR |
| `stop_pending_taken_at_once` | SOFT already pending: STOP #$2000 takes it at once, PC after STOP | PRM STOP | missed pending interrupt at STOP |
| `stop_masked_level_waits` | STOP #$2100 with SOFT (level 1) pending stays stopped until the level-2 timer; SOFT taken only at mask 0 | PRM STOP: only a level above the new mask resumes | STOP woken by a masked level |
| `irq_between_instructions` | CIA timer into a 2000 x ADDQ chain: stacked PC on a boundary and D0 = number of ADDQs before it (3/8 trials) | interrupts between instructions | PC/register state inconsistent |
| `irq_after_movem` | CIA timer into a MOVEM.L chain alternating two buffers: all 13 registers from the buffer of the last MOVEM before the stacked PC | interrupts taken after the instruction completes | partially executed MOVEM |
| `latency_cia_ta_to_handler` | 8/32 samples of CIA-A TA underflow -> handler entry, in E clocks (`timing`: min/max/sum/histogram); fails if a sample is lost or the largest latency exceeds 2 raster lines (454 cck = 90.8 E clocks). The samples start at a fixed beam position (after a VBL, line 20; seeded spacing), so the verdict does not depend on what ran before the case | worst case on a 7 MHz 68000 by MC68000UM timings: a runtime VERTB handler in the way (~590 clocks) + the level 2 exception (44) + the handler's prologue and E-clock-synced CIA read (~50) = ~345 cck = 1.5 lines; faster CPUs only shorten it (HRM 7 interrupts) | lost CIA interrupts, or interrupts held back (IPL/INTREQ propagation, interrupt sampling); the distribution is compared by the host |
| `latency_coper_to_handler` | 4/16 frames of copper WAIT $80 + MOVE INTREQ COPER -> handler entry, in colour clocks (`timing`); fails if a frame's COPER is lost or a latency exceeds 2 raster lines (454 cck) or is negative | the copper MOVE follows the WAIT within a few cck (HRM 2); the CPU side is the current instruction + the level 3 exception + prologue, well under a line on a 7 MHz 68000 (MC68000UM) | lost copper interrupts, or COPER/IPL delivered late |
| `storm_cia_timer` | 1 s (quick) / 5 s: CIA-A TA every 142 E + back-to-back SOFTINT + VERTB; CIA-A count = elapsed/142 +-1 (elapsed from CIA-B E-clock counter). A fail shows only `got.count` "too few"/"too many"; the numbers are in `got.timing` and `timing` (they depend on the elapsed time) | no lost or doubled interrupts | interrupts dropped/doubled under load (e.g. requests queued from other threads) |
| `storm_reference_clock` | every coherent CIA-B timestamp during the storm is nondecreasing, including SOFT waits and the final elapsed sample | HRM appendix F: TA counts E clocks, TB counts TA underflows, no reload while running | invalid CIA reference (e.g. synthetic timer-read decrements), not evidence of lost interrupts |
| `storm_softint` | in the same storm every SOFTINT issued is taken exactly once (each waited for, 3000 E timeout) | as above | lost software interrupts |
| `storm_vertb` | in the same storm VERTB count = rounded elapsed/calibrated frame period +-1; CIA-bracketed sixty-four-period beam calibration, independent of interrupt counts | as above | lost/extra VBL interrupts or failed/insufficiently precise beam calibration |
