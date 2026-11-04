# copper (module 13, AMT_PH_BARE, disk "chip")

The Copper instruction set and its CPU-visible effects. References: Amiga Hardware Reference
Manual (HRM) chapter 2 "Coprocessor Hardware" (MOVE/WAIT/SKIP, comparison enable bits, BFD,
COP1LC/COP2LC/COPJMP, vertical blank restart, COPCON danger bit, waiting past line 255), chapter 7
(INTREQ COPER), appendix C (ECS: with CDANG the copper may write every register); AGA docs
(BPLCON2 RDRAM colour read-back).

Every pass/fail observation is persistent state read at a safe point, so records do not depend
on how closely the CPU core follows the beam:
- markers: INTREQ bits nothing else sets while their interrupts/DMA are off (SOFT=A, DSKBLK=B,
  COPER=C, BLIT=D where no blit runs), read lines (or frames) after they must have been set;
- release positions (all `wait_*` cases and `skip_copjmp2_loop`): the case's list is run as part of
  a list the copper restarts every frame; it starts with no bitplanes and the release point is a
  MOVE #$1200,BPLCON0 that turns on an all-ones plane. One-line DMA sprites sit on line V-1 (H $1A0
  and at the end of the window, H $1B0), on line V well after the release H, and (if there is room) on line V just before it. CLXDAT, read at
  the vertical blank after three discarded and two complete frames, says whether the plane was on at each: exactly "after"
  must hit;
- a copper-programmed blit, JOY0DAT via JOYTEST, DMACONR/ADKCONR, AGA colour read-back, COPER
  interrupts counted from VBL to VBL.
Beam-polled latencies go to `timing` objects only. Beam reads re-read VPOSR around VHPOSR and treat
"same V, smaller H" as the next line (H wraps before V counts on real chips and in Amiberry).

| case | intent | rule (reference) | a failure usually means |
|---|---|---|---|
| move_intreq | MOVE #$8004,INTREQ after a CPU COPJMP1 strobe (latency in timing) | MOVE writes the register (HRM 2) | copper not started by COPJMP1, MOVE lost |
| move_sequence | set A+B, clear A, set C, set D | MOVEs in order (HRM 2) | MOVE order/SET-CLR wrong |
| move_dmacon_adkcon | copper writes DMACON BLTPRI and ADKCON low byte | registers >= $80 always writable (HRM 2) | copper writes to these dropped |
| wait_v / wait_vh | WAIT $8001 / $8081 | release at the beam position (HRM 2) | WAIT compare wrong |
| wait_v_masked | WAIT VP=$05 VE=$0F from line $40 | only enabled bits compared, releases on $45 (HRM 2) | VE mask ignored |
| wait_v7_always_compared | WAIT VP=$80 VE=0 | V7 has no enable bit (HRM 2) | V7 masked |
| wait_h_masked_same_line | VE=0 WAIT H=$A0, MOVE, VE=0 WAIT H=$40 (already true) | second WAIT releases at once (HRM 2) | copper re-waits for the next line |
| wait_h_past_line_end_e4 / _fe | WAIT H=$E4/$FE (never reached) | releases when V becomes V+1 (HRM 2) | waits forever / releases early |
| wait_line_256_wrap | WAIT $FFDF then WAIT $xx01 | lines 256+ via the line-255 wait (HRM 2) | 8-bit V compare / wrap wrong |
| wait_after_255_8bit_compare | WAIT $FF07 then WAIT $0801 | compared on line 255, $FF >= $08: immediate (HRM 2) | V8 compared |
| skip_compare | 5 SKIP variants (true/false, masked V, H only, equal position) before MOVE A; MOVE B | SKIP skips next instruction when >= (HRM 2) | SKIP compare wrong |
| skip_over_wait | true SKIP before WAIT $FFFF,$FFFE, then MOVE C | the skipped instruction may be a WAIT (HRM 2: "skip the next instruction") | SKIP only suppresses MOVE writes |
| copjmp2_by_copper | MOVE to COPJMP2 inside a list | jump at once, rest of list1 not run (HRM 2) | strobe ignored/late |
| copjmp2_by_cpu | CPU writes COPJMP2 while the copper waits at END | restart at COP2LC (HRM 2) | strobe ignored |
| cop1lc_write_no_jump | copper writes COP1LC | no jump until vertical blank (HRM 2) | COP1LC write jumps |
| vbl_restart_cop1lc | the stored COP1LC list runs at line 0 (SKIP $0031 guards "early") | restart at vertical blank (HRM 2) | no/late restart |
| skip_copjmp2_loop | SKIP/COPJMP2 loop until line $90 | HRM 2 SKIP loop example | loop exit wrong |
| copen_pause_resume | COPEN off during a WAIT, on later | copper does not run while off, resumes its WAIT (HRM 2) | copper runs without DMA / restarts |
| wait_bfd0_waits_blitter / wait_bfd1_ignores_blitter | WAIT already true with BFD=0/1 during a ~2-frame blit (64 x 1024, modulo -128) | BFD=0 also waits for the blitter (HRM 2) | blitter wait ignored / always applied |
| danger_off_blitter_stops | CDANG=0, MOVE to BLTCON0 | copper stops; restarts at VBL (HRM 2) | protected write executed |
| danger_on_blitter_runs | CDANG=1, copper programs a 1-word blit | $040-$07E allowed with CDANG (HRM 2) | danger bit ignored |
| danger_off_below_40 | CDANG=0, MOVE to JOYTEST | stops (HRM 2) | protected write executed |
| danger_on_below_40 | CDANG=1, MOVE to JOYTEST: OCS Agnus stops, ECS/AGA writes (`ctx.undef_joy1dat_after` informational) | HRM 2 / appendix C | Agnus variant rule wrong |
| coper_interrupt | copper INTREQ COPER with INTENA, counted over 8 whole frames (3 quick); line in timing | one level-3 interrupt per frame, SR mask 3 (HRM 2, 7) | interrupt lost/doubled/wrong level |
| colour_writes_midline | 96 mid-line COLOR00 changes then MOVE A; AGA reads COLOR00 back (RDRAM); screenshot `copper_colour_bars` | colour registers are copper targets (HRM 2) | list stalls; AGA read-back wrong |
| timing_move_rate | 65 MOVEs after a WAIT, cck in timing | HRM 2: a MOVE is 2 copper memory cycles | (informational, pass if the list finishes) |
| timing_cpu_vs_copper | CPU chip reads in 32 lines, copper idle vs MOVE loop | timing only | (informational) |
