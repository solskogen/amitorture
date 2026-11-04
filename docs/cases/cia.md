# cia (module 7, AMT_PH_BARE)

8520 CIA-A ($BFE001) and CIA-B ($BFD000). References: Amiga Hardware Reference Manual (HRM) appendix F
"8520 Complex Interface Adapters", and the CSG/MOS 8520 data sheet. 8520 vs 6526: the TOD is a 24-bit
binary counter (no BCD, no AM/PM, no 50/60 Hz divider; it counts pulses on its TOD pin), CRA bit 7 is
unused. CIA-A TOD counts the 50/60 Hz tick (VSYNC on most Amigas, or the power line), CIA-B TOD counts
HSYNC. E clock = CPU clock / 10 = colour clock / 5 (709379 Hz PAL, 715909 Hz NTSC).

Timer cases run for every timer; the id prefix is `ciaa_ta_`, `ciaa_tb_`, `ciab_ta_` or `ciab_tb_`.
Per-CIA cases are prefixed `ciaa_` / `ciab_`.

The module never sets PBON (CIA-A PB = parallel port, CIA-B PB = floppy control), never uses CIA-B
SPMODE/CNT (parallel BUSY/POUT), and never writes PRA/PRB/DDRx. Saved and restored (`modules/cia_hw.h`):
CRA/CRB, the latches (taken from the counter of a stopped timer, or sampled after a wrap for a
running continuous one), the ICR mask (from ciaX.resource AbleICR(0)), and the TOD (saved value plus
the elapsed frames/lines). The TOD alarm is write-only and its OS value unknown: it is parked at the
restored TOD - 1, so it matches only after a full 24-bit wrap (CIA-B ~18 min, CIA-A ~93 h) instead of
at a test value ($3B0) a few minutes later. OS events drained from
ICR are re-posted for TA/TB/ALRM when the OS had them enabled. A keyboard byte arriving meanwhile is
acknowledged (handshake), so the keyboard does not hang; that key is lost.

If another device holds INT2/INT6 (CD32 Akiko: see interrupts.md), `cia?_icr_irq_to_paula`,
`cia?_icr_read_releases_irq` and `cia?_icr_intreq_clear_while_asserted` report `untestable` for that CIA.
The check runs at module start; when `cia?_icr_read_releases_irq` fails, it is repeated (both ICR masks
clear and drained): a device that started holding the line later (IDE drive, network board) makes the
case `untestable` instead.

The CIA raises /IRQ about one E clock after the ICR flag, and no latency to INTREQ is specified, so
every INTREQR check after an underflow polls for up to 20 E clocks (`intreq_wait`) and every INTREQ
clear after an ICR read first waits 20 E clocks: the verdicts do not depend on CPU speed or JIT.
Values the hardware leaves open are reported under `ctx.undef_*` names, which the runner keeps out of
the fingerprint.

All TOD tolerances come from beam stamps (lines for CIA-B, frames for CIA-A) taken around the TOD
accesses, so a slow CPU widens the window instead of failing. The beam is read as VPOSR, VHPOSR,
VPOSR, retried until both VPOSR reads agree (`cia_beam()`): V8 lives in VPOSR, and a single
VPOSR+VHPOSR pair read across line 255->256 gives line 0, which the stamp took for a frame wrap
(+1 frame: a TOD window 313 lines too long, so `tod_wait()` ended early).

| case | intent | rule (reference) | a failure usually means |
|---|---|---|---|
| `*_stopped_hi_write_loads` | latch lo/hi written to a stopped timer (CR=$00): counter = latch, for $0000/$0001/$00FF/$0100/$8000/$FFFF + random | HRM F: the latch is loaded into the counter when the high byte is written while the timer is stopped | counter not loaded on the high-byte write, or the bytes are swapped |
| `*_lo_write_no_load` | a low-byte write only changes the latch; the following high-byte write loads latch hi:lo (4 E clocks between accesses: the load latency after a high-byte write is not specified, and a fast CPU's next write can land inside it) | 8520 data sheet, timer latch/counter | low-byte write reloads the counter, or the latch low byte is lost |
| `*_oneshot_hi_write_starts` | CR=RUNMODE (one-shot, START=0); writing the high byte loads and starts the timer | 8520 data sheet / HRM F: "in one-shot mode a write to timer-high transfers the latch to the counter and initiates counting regardless of the start bit" | 6526 behaviour emulated (no auto start) |
| `*_oneshot_underflow_icr` | the one-shot (latch $2000) sets its ICR flag within 30 ms | ICR TA/TB flag on underflow | no underflow / no ICR flag |
| `*_oneshot_stops_reloads` | after the one-shot underflow START reads 0 and the counter = latch | the latch is reloaded on every underflow; one-shot clears START | START left set, or counter not reloaded |
| `*_oneshot_single_event` | no second underflow after the one-shot stopped | RUNMODE=1 | timer kept running |
| `*_continuous_repeats` | continuous mode (latch $0800): 3 underflows within 40 ms, START stays set, counter <= latch | RUNMODE=0 | continuous mode stops, or reloads wrong |
| `*_start_stop_holds` | clearing START holds the counter | CR START | counter keeps running when stopped |
| `*_restart_resumes` | setting START again resumes from the held value (no reload) | CR START | START reloads the counter |
| `*_force_load_stopped` | LOAD strobe with the timer stopped loads latch $FFFF | CR bit 4 LOAD, force load | force load ignored |
| `*_load_strobe_reads_0` | LOAD reads back 0, and LOAD alone does not start | CR bit 4 is a strobe | LOAD stored as a bit, or it starts the timer |
| `*_force_load_running` | LOAD while running reloads and keeps counting | force load | force load ignored while running |
| `*_running_hi_write_no_reload` | high-byte write to a running continuous timer does not reload | only underflow, force load or stopped hi-write load | counter reloaded by a latch write |
| `*_new_latch_on_underflow` | the latch written while running is used at the next underflow | latch reload on underflow | old latch reused |
| `*_ta_inmode_cnt_no_e_clock` | TA with INMODE=1 (count CNT) does not count E clocks (no CNT activity) | CRA bit 5 INMODE | INMODE ignored |
| `*_cr_readback` | CRA/CRB patterns (START=0; no PBON, no CRA SPMODE) read back as written, LOAD reads 0; CRA bit 7 masked (unused) | control registers are R/W | wrong CR read-back |
| `cia?_tb_counts_ta_underflows` | CRB INMODE=10: TB counts exactly the K one-shot TA underflows (TB = latch-K), and no E clocks; TB is read 8 E clocks after the last underflow (no cascade latency is specified; a fast emulated CPU can read TB in the same E clock as the CRA read that saw START=0) | HRM F CRB INMODE | cascade mode counts E clocks or misses pulses |
| `cia?_tb_ta_ratio_exact` | TA continuous (1023), TB counting TA (latch 7): exactly 8 TA underflows per TB underflow, 8 times | timer-to-timer ratio is exact (period = latch+1 pulses) | lost/merged underflows, off-by-one period |
| `cia?_icr_read_clears` | mask clear: ICR reads $01 (no IR) once, then $00 | ICR read clears the flags | flags not cleared, IR set for a masked source |
| `cia?_icr_ir_bit_set` | mask TA: ICR reads $81 | IR = bit 7 when an enabled flag is set | IR missing |
| `cia?_icr_irq_to_paula` | enabled CIA-A event sets INTREQ PORTS (bit 3), CIA-B sets EXTER (bit 13), within 20 E clocks of the underflow (polled) | HRM: CIA-A /IRQ = INT2, CIA-B /IRQ = INT6 | CIA interrupt not routed to Paula |
| `cia?_icr_read_releases_irq` | after the ICR read and 20 E clocks, clearing the INTREQ bit sticks | ICR read releases /IRQ | /IRQ stays asserted |
| `cia?_icr_intreq_clear_while_asserted` | once INTREQ shows the line, clear it and read again 4 E clocks later: records whether it was re-set while /IRQ is still asserted (both accepted; `ctx.undef_intreq_relatched`, not fingerprinted) | HRM undefined | informational |
| `cia?_icr_mask_setclr` | writes $7F,$81,$82,$01: mask = TB only; TA event reads $01, TB event $82; after $7F, TB reads $02 | ICR write: bit 7 = set/clear, other mask bits unchanged | wrong set/clear semantics |
| `cia?_icr_two_flags` | TA and TB pending, both enabled: $83 | ICR data | flags lost |
| `cia?_icr_mask_after_event` | mask enabled after the TA flag: TA flag must still be there; IR 0 or 1 accepted (`ctx.undef_icr`, and `ctx.undef_intreq` = INTREQ seen within 20 E clocks; not fingerprinted) | data sheet undefined for IR | flag lost |
| `ciaa_tod_counts_vsync` | CIA-A TOD counts one per frame (+-1) | HRM: CIA-A TOD = 50/60 Hz tick | wrong TOD-A source/rate |
| `ciab_tod_counts_hsync` | CIA-B TOD counts one per raster line. The window comes from (frame, line) stamps that follow the beam and never go back: the beam wraps to line 0 a few colour clocks before INTREQ VERTB shows, and a fast CPU sampling in between made the old stamp drop by a frame | HRM: CIA-B TOD = HSYNC | wrong TOD-B source/rate |
| `cia?_tod_latch_hi_read` | reading TODHI latches; MID/LO read W ticks later still old | 8520: read latch held until TODLO is read | no read latch |
| `cia?_tod_lo_read_unlatched` | after TODLO is read, TODLO alone follows the counter | latch released by the LO read | latch never released |
| `cia?_tod_write_hi_stops` | writing TODHI stops the clock (MID written too) | 8520 TOD write order | clock keeps running |
| `cia?_tod_write_lo_starts` | writing TODLO restarts it from the written value; max = stamp window + 1, + 1 more for a TOD pulse already in flight at the write (the data sheet does not say whether it counts; Amiberry counts it) | 8520 TOD write order | clock stays stopped / wrong start value |
| `cia?_tod_binary_lo_09` | $000009 -> $00000A (for all four steps: the value used is the first one that two consecutive reads agree on after the change, because a read during a carry across a 12-bit boundary can see the 8520 TODMED transient, e.g. $2F -> $20 -> $30) | 8520 TOD is binary (6526 is BCD) | BCD counting |
| `cia?_tod_binary_lo_carry` | $0000FF -> $000100 | binary carry | carry bug |
| `cia?_tod_binary_mid_carry` | $00FFFF -> $010000 | binary carry | carry bug |
| `cia?_tod_rollover_24bit` | $FFFFFF -> $000000 | 24-bit counter | wrong width |
| `cia?_tod_alarm_write_keeps_tod` | with CRB bit 7 set, TOD writes go to the alarm; the TOD is unchanged and running | CRB ALARM | alarm writes land in the TOD |
| `cia?_tod_alarm_fires` | ALRM flag set when TOD reaches the alarm, not before (TOD read after the flag <= alarm + the ticks since the stamp before the previous poll's ICR read: the flag can be set just after that read) | ICR bit 2 | alarm missing/early/late |
| `cia?_tod_alarm_ir` | ALRM enabled in the mask: ICR reads $84 | IR + ALRM | alarm with IR lost |
| `cia?_tod_alarm_past_no_fire` | TOD started past the alarm: no ALRM | exact match only | alarm on >= instead of == |
| `cia?_tod_alarm_repeat` | 4/12 alarms in a row at varying tick phases; each must set ALRM | as above | alarms lost depending on the tick phase (see FINDINGS in the module report) |
| `ciaa_sdr_output_shift` | CIA-A serial port output mode, TA latch 3, SDR=$FF: ICR SP set between half and 4x the 8-bit shift time (16 TA underflows x 4 E clocks = 64 E, computed from the latch: 32..256 E), timed with CIA-A timer B (E clocks, interrupts masked, read right after the SDR write and after SP; the loop reads ICR, and TB every 16th poll for the bound); CRA reads SPMODE back while set and after it is cleared; time in `timing` | MOS 8520 data sheet / HRM F serial port: one bit per two TA underflows, SP after the 8th bit | output shift not emulated, shift clocked at the wrong rate (too slow, or SP at once), CRA bit 6 not readable |
| perf `ciaa_ta_eclock_65536`, `ciab_ta_eclock_65536` | one-shot of 65536 E clocks measured in colour clocks (`ticks`); expected ratio 5.0 | E clock = colour clock / 5 | wrong E-clock rate (host compares with tolerance) |
