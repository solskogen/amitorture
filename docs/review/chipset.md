# Chipset test review (independent, read-only)

Scope: `disk/modules/interrupts*`, `cia*` + `cia_hw.h`, `blitter*` + `tools/gen_blitter.py`,
`paula*`, `input*`, `timing*`, `dmacontention*`, then `display*`, `copper*`, `display_hw.h`, and the
matching `docs/cases/*.md`. Review date 2026-11-03.

Display/copper: I reviewed copper.c once it had not changed for 24 minutes (mtime 20:24). I reviewed
display.c once it had not changed for 10 minutes (mtime 20:44:51, review started after 20:54:51).
Line numbers below refer to those versions.

Late changes: `cia.c`, `cia_hw.h`, `interrupts.c`, `interrupts_asm.s` and `docs/cases/{cia,interrupts}.md`
changed at 20:45-20:47, after I had read them. The repository has no git history, so I re-checked
them by searching for the changes, not with a diff. The additions are the stuck-INT2/INT6 detection
(see L4) and a runaway guard in the level-2 handlers. M2 and M3 are still present in the current
`cia.c` (lines 607, 633-639, 697-706).

I did not run the emulator. Where I give Amiberry behaviour, I read it in the Amiberry source tree (`src/`).

One framework point affects several findings. `runner/amitorture/cases.py:31` leaves out of the
fingerprint only `timing`, `ticks`, `latency*`, and the exact names `undef_observed` /
`undef_changed_at`. A general `ctx.undef_*` prefix is not excluded. Any other "either result is
accepted" value, in `ctx` or anywhere else, is part of the baseline/parity fingerprint.

Counts: **high 1, medium 9, low 17**.

---------------------------------------------------------------------------
## High

### H1. display: single-playfield sprite/playfield collisions asserted as "odd and even independent"
- **Where:** `disk/modules/display.c:834-886` (`clx_spf_odd_independent`) and `:751-831`
  (`clx_spr_pf_spf`, model lines 784-790 OR odd and even group matches independently).
- **What is wrong:** the case asserts that in single-playfield mode, CLXDAT bits 1-4 (odd planes to
  sprite) depend only on the odd planes.
  - The HRM collision chapter does not say this for single playfield. It describes odd/even matches
    without any DBLPF dependence, and the test reads that silence as a rule.
  - The most hardware-faithful model available, WinUAE (as merged into Amiberry), does the opposite.
    `src/drawing.cpp:4889-4895`: in single-playfield mode the odd-group bit (`<<0`) is set only when
    the even-group match (`clxcon_bpl_enable_aa`) also holds. The dual-playfield branch keeps them
    independent.
  - This distinction is deliberate in WinUAE. Its collision code is tuned against real hardware.
- **Effect:** the case is likely to fail on real hardware and on every WinUAE-derived emulator. The
  doc calls such a failure "collision logic couples the odd-plane bit", which is a false finding.
  `clx_spr_pf_spf` fails too, on every random combination where the odd group matches and the even
  group does not.
- **Rule:** HRM ch.7 "Collision detection" / CLXDAT table. It does not cover single-playfield
  coupling, so the behaviour is not specified by the manual.
- **Fix:** until there is a real-hardware capture, make the single-playfield behaviour an accepted
  alternative:
  - report the observed variant under `ctx.undef_observed` (a name the runner excludes);
  - in `clx_spr_pf_spf`, accept either the independent model or the coupled one (odd bit = odd match
    AND even match);
  - keep `clx_spr_pf_dpf` strict.
- **Rest of the module is fine:** all other display cases enable a single plane per frame, so the
  other group has no enabled plane and always matches. They do not depend on this rule.

---------------------------------------------------------------------------
## Medium

### M1. display `deniseid`: cannot fail (expected value comes from the tested register)
- **Where:** `display.c:2116-2160`; `disk/probe/probe.c:659-668`.
- **What is wrong:** the probe sets `ecs_denise` and `lisa` from DENISEID itself (`(d & 0xff) == 0xfc`
  / `0xf8`, stable). The module then checks DENISEID == $FC only `if (ecsd)` and $F8 only `if (aga)`.
  - On ECS this is a self-comparison, so it always passes.
  - An emulator returning a wrong or unstable ID just becomes "OCS" and the case is skipped.
  - The AGA branch can fail only through Alice detection, never through Lisa itself.
- **Rule:** HRM appendix C (DENISEID $FC); AGA docs (LISAID $F8).
- **Fix:** derive the expectation from something independent, such as the Agnus ID class (Alice ⇒
  Lisa expected; 8372 alone proves nothing). Otherwise drop the case and rely on the probe
  fingerprint (`probe.hello` already contains `denise_id`). At least report a skip when the
  classification came from the same read.

### M2. cia `icr_irq_to_paula`: one INTREQR read immediately after the underflow
- **Where:** `cia.c:604-608`.
- **What is wrong:** `fire()` returns as soon as CR START reads 0, and INTREQR is sampled exactly once
  straight after.
  - Amiberry raises the CIA interrupt one E clock after the underflow (`cia.cpp:816`,
    `event2_newevent_xx(-1, DIV10 + delay, ...)`).
  - Real 8520s also assert /IRQ about a cycle after the ICR flag.
  - Whether the bit is already set therefore depends on how quickly the CPU gets from the CR read to
    the INTREQR read: core speed, JIT, CIA access timing.
  - The result is a false fail on a fast core or JIT, and a JIT-on/off parity difference.
- **Rule:** HRM ch.7 / appendix F (CIA /IRQ to INT2/INT6). No latency is specified.
- **Fix:** poll INTREQR for up to ~20 E clocks (`cia_now()`-bounded) before deciding. Put the
  latency, if wanted, in `timing`.

### M3. cia: timing-dependent "either is fine" values in fingerprinted `ctx`
- **Where:**
  - `cia.c:630-645`, `icr_intreq_clear_while_asserted`: `ctx.intreq_relatched` = `r3`, read right
    after clearing INTREQ.
  - `cia.c:694-707`, `icr_mask_after_event`: `ctx.intreq` = `r1`, read right after the ICR mask write;
    `ctx.icr` too.
- **What is wrong:** both values are accepted either way, but they depend on the same CIA→Paula delay
  as M2. Because they sit in plain `ctx` keys, they are fingerprinted. JIT on/off and core-to-core
  comparisons will flag spurious differences.
- **Rule:** CONTRACT §10 rule 2 (no timing-dependent values outside timing fields).
- **Fix:** rename them to `undef_observed` (excluded by `cases.py:31`), or move them under `timing`.
  Alternatively, if the relatch behaviour is what is being recorded, measure it after a bounded wait.

### M4. timing `cck_per_line`: the pass condition depends on how many lines 100000 samples cover
- **Where:** `timing.c:352` and `timing_asm.s:163-205` (fixed `n*1000` samples).
- **What is wrong:** the case needs both hmax ∈ {$E2,$E1} (PAL) and hmin == 0.
  - The number of beam lines the sampling covers depends on CPU speed versus the beam.
  - A JIT or non-cycle-exact core whose custom-register reads cost little emulated time can finish
    100000 samples in a small part of a line. The minimum or maximum is then never seen: a false fail
    and a JIT-parity difference.
  - Phase-locking can also skip H=0, the same argument the code already uses for $E2.
- **Rule:** HRM ch.6/7 VHPOSR range.
- **Fix:** sample until at least N (e.g. 200) whole lines have passed (count V changes), with a cap.
  `display beam_hpos_max` already bounds by lines.

### M5. paula: no `amt_try`, so an abort leaves the drives running
- **Where:** `paula.c:951-966` (`amt_run_paula`); `paula_disk.c:1268-1321`.
- **What is wrong:** the disk tests run without a try block. The restore code is skipped if any of
  these happens while a drive is selected, its motor on and the head away from its cylinder:
  - an unexpected exception;
  - a watchdog expiry. Watchdog kicks happen only per track, and 4 retries × 1.2 s per track plus
    seeks get close to 120 s on a bad disk.
  - `amt_abort` from a hook.

  The skipped restore is: motor off, deselect, step back to the entry cylinder, PRB, DSKLEN = $4000,
  ADKCON.
  - The runtime turns DMACON off but not DSKLEN. During the DF1 write window DSKLEN stays
    WRITE|DMAEN, which keeps Paula in write state.
  - trackdisk then starts from a wrong head position, a drive left selected and the motor on, on real
    hardware.
- **Rule:** CONTRACT §10 rule 4 ("leave the hardware as you found it").
- **Fix:** wrap `pa_audio_tests` / `pa_disk_tests` / `t_host_tones` in `amt_try`, as cia/input/copper/
  display do. Run the disk restore unconditionally afterwards: DSKLEN = $4000, motor off for DF0/DF1,
  deselect, `seek(entry)` when the head position is known, `prb_w(prb0)`. Also kick the watchdog
  inside `read_track`'s retry loop.

### M6. dmacontention `disk`/`all`: false fail when DF0 holds a non-AmigaDOS disk
- **Where:** `dmacontention.c:299-331` (`dsk_start`), `:369-399` (`dsk_check`), `:722-731`.
- **What is wrong:** DF0 is accepted when /DSKRDY and /DSKCHANGE are high. The first sector after a
  $4489 sync must then be an AmigaDOS sector with good checksums, or the case is a *fail*.
  - On an HD- or CD-booted machine with any other disk in DF0 (PC format, NDOS game disk, unformatted
    disk), this is a false fail.
  - `paula_disk.c` guards the same situation with `amt_bootio` / "trackdisk.device".
- **Rule:** CONTRACT §10 rule 1 (expectations must hold for the configuration). The HRM does not
  require any particular disk format.
- **Fix:**
  - read only when the boot device is trackdisk (as paula does), or
  - turn "no sync / bad format with DSKBLK received" into `untestable` ("DF0 holds no AmigaDOS
    track"), and keep `fail` only for the contention-specific symptoms (missing or doubled DSKBLK,
    data that differs between a no-DMA-load read and a loaded read of the same track).

### M7. timing: CIA-B restore writes a random counter value as the latch, and ICR is ignored
- **Where:** `timing.c:76-112` (`ec_start` / `ec_stop`).
- **What is wrong:**
  - The "latch" put back is the counter value found when the module started. For a continuous timer
    the OS is running, that changes its period from then on.
  - The OS ICR mask is left enabled while the module makes hundreds of TA/TB underflows, so ICR
    TA/TB events are still pending when exec gets control back after the run, and the OS takes
    spurious CIA-B interrupts.
  - `cia_hw.h` already solves both problems (`cia_latch_estimate`, ICR save/drain/re-post) and is not
    used here.
- **Rule:** CONTRACT §10 rule 4; 8520 data sheet (latches are write-only).
- **Fix:** use `cia_save(&s, CIAB_BASE, 0)` / `cia_restore(&s)` around the module. Keep `cia_restore`
  as a separate read-back check.

### M8. timing: unbounded waits at SR=$2700, where the watchdog cannot fire
- **Where:** `timing.c:255` (`sample_frames`: loops until n beam wraps); `:430`
  (`while (!(INTREQR & VERTB))`); `:436-441` (CIA second: until `sec >= tm_hz`). All run with
  interrupts masked.
- **What is wrong:** if the beam counter, VERTB or CIA-B does not advance (exactly the emulator bugs
  this module looks for), the machine hangs with the VERTB watchdog masked. The module then has no
  result, instead of reporting a fail.
- **Rule:** CONTRACT §10 (watchdog/abort reporting); brief "no unbounded waits".
- **Fix:** give each loop an iteration cap, e.g. 4 M polls, and report a fail with "beam/VERTB/CIA
  stuck".

### M9. Fingerprint naming for "accepted alternatives" is inconsistent with the runner
- **Where:** `runner/amitorture/cases.py:31`.
- **What is wrong:** besides M3, these values are fingerprinted:
  - `cia icr_mask_after_event ctx.icr`
  - `paula drive_ids in.id` (configuration-dependent informational value; fine if configs never
    change, otherwise every changed config differs)
  - `copper danger_* ctx.joy1dat_after` (moves when a port-1 device moves on real hardware)

  The brief and this review's instructions assume a `ctx.undef_*` exclusion that does not exist.
- **Fix:** either make `cases.py` exclude every key starting with `undef_`, or rename the module keys
  to `undef_observed`. Medium because it decides whether JIT/core parity runs are clean.

---------------------------------------------------------------------------
## Low

### L1. dmacontention `audio`: elapsed time from `amt_ticks()` read at SR=$2700
`dmacontention.c:635-641`.
- `ta1 = amt_ticks()` is taken right after `amt_set_sr(0x2700)`. If the beam has just wrapped and
  VERTB is pending, `amt_ticks()` is one frame short. That is the documented defect `cia_hw.h`
  `cia_now()` corrects.
- With PER 200 and LEN 16, a frame is about 11 interrupts, well outside ±3: a rare false fail.
- **Fix:** use a wrap-corrected clock (`cia_now()`-style INTREQR VERTB check) for ta0/ta1.

### L2. Failing records carry timing values in non-timing keys
Examples:
- paula `chk_report` `got.got` (`paula.c:243-245`);
- paula disk `j_dres` inside `got` (`paula_disk.c:913`, `:563-573`);
- dmacontention `fail_once` expect/got for `coper_irqs` / `aud_irqs` (`:706-720`);
- interrupts `storm_rec` `got.count/diff`.

When a case fails under both JIT on and JIT off, the fingerprints still differ every run.
**Fix:** put measured values under a `timing` sub-object inside `got`, and keep only the bound and
the verdict in fingerprinted fields.

### L3. interrupts: the asm handlers read CIA-A ICR without the keyboard handshake
`interrupts_asm.s:109, 129, 314, 346` (and the ICR read in `_ir_snap2`).
- If a key event arrives while the module owns CIA-A, the SP flag is consumed with no handshake. On
  real hardware the keyboard then times out (143 ms) and resyncs, losing a key.
- `cia_hw.h` promises an acknowledge; these paths do not give one.
- **Fix:** in the handlers, if bit 3 (SP) is set and CRA SPMODE is 0, set a flag; the C side then calls
  `cia_kbd_handshake()`.

### L4. interrupts/cia: INT2/INT6 held by another device is detected only once
`cia_hw.h:158-170` (`cia_int_line_stuck`), `interrupts.c:1468-1472`, `cia.c:1166-1170`.
- This was added while I was reviewing (20:45-20:47), and it fixes the main false-fail risk:
  Gayle/IDE/Zorro/Akiko sources that hold PORTS/EXTER now make the dependent cases `untestable`.
- What remains: the check runs once at module start, over 3 × 2 lines. A device that asserts INT2
  later (an IDE drive finishing a command, a network board) still gives a sporadic false fail in the
  read-back and mask cases.
- **Fix:** in the cases that compare PORTS/EXTER, re-check after a fail (clear, wait 2 lines, read
  again). Report `untestable` if the bit comes back with both CIA masks clear.

### L5. interrupts: short nesting windows
`interrupts_asm.s:135-141`. A 16-read `.win` after the trigger write in the nesting tests.
- On real hardware Paula needs far less than this window to change IPL, so the hardware cases are
  sound.
- But "taken inside the window" depends on how much emulated time 16 chip reads take. That is fine in
  Amiberry (`custom.cpp:3332`, a 1-CYCLE_UNIT delay), but a core with coarser interrupt delivery
  fails `nest_*` without a real nesting bug.
- **Fix:** bound the window by beam time (e.g. poll until 2 lines have passed), not by read count.

### L6. cia TOD carry cases and the 8520 TODMED glitch
`cia.c:799-801`.
- Real 8520s pass a transient value during carries across a 12-bit boundary (Amiberry/WinUAE emulate
  it: `cia.cpp:840-846`, behind `cs_ciatodbug`).
- `tod_binary_mid_carry` and `tod_rollover_24bit` take "the first value that differs from X". A read
  that lands in the glitch gives a non-monotonic delta and a fail. This is rare, but real.
- **Fix:** take two consecutive equal reads after the change.

### L7. cia: TOD alarm left at test values
`cia.c:1006`; `cia_hw.h` (documented as not restored).
- After the module, the CIA-A/B alarm stays at $3B0. If the OS has ALRM enabled, it gets a spurious
  alarm when the TOD wraps through $3B0. For CIA-B TOD (HSYNC) that is about every 18 minutes.
- Documented, but on real hardware a later OS session may misbehave.
- **Fix:** at least write the alarm to a value just behind the restored TOD (TOD-1, so it fires only
  after a full wrap), or restore from a known OS value if one is available.

### L8. paula: `amt_bootio` is checked for the device name only
`paula_disk.c:972-980`.
- Kickstart 2+ can boot from DF1-DF3 (boot menu or empty DF0). trackdisk.device is still the boot
  device, but DF0 is then not our disk.
- The DF0 bootblock, payload header and blob-CRC cases would then *fail* although nothing is wrong.
- **Fix:** if decoded sector 2 differs from `amt_hdr` *and* sectors 0-1 are not ours, report
  `untestable` ("DF0 is not the boot disk") rather than fail.

### L9. paula: settle time after a direction change before a step
`paula_disk.c:100-112`.
- After changing DIR, only 1 ms passes before the STEP pulse. Many drive data sheets ask for a longer
  settle after reversing direction, and trackdisk uses longer delays around direction changes.
- A missed step on a real drive makes `df0_track0_sensor` / `*_head_restored` fail, and leaves
  trackdisk's cylinder wrong.
- This is not an HRM-mandated number; please check it against the HRM ch.8 drive timing table.
- **Fix:** use ≥ 3 ms after a DIR change, and the 18 ms settle (`settle()` already exists) before the
  sensor reads.

### L10. paula: DF1 5.25" drive
`paula_disk.c:1103+`.
- `id1 = $55555555` (40-track drive) is not excluded, so `read_track(159)` seeks to cylinder 79 and
  bangs the head stop.
- **Fix:** skip DF1 unless the ID is $FFFFFFFF (or $AAAAAAAA for an HD drive).

### L11. dmacontention `blitter`: a timing-dependent fail condition
`dmacontention.c:699`.
- `samples_busy == 0` is a fail. It happens on any core with "immediate blitter" or a very slow CPU
  relative to the blit: a configuration effect, not an HRM violation.
- The module doc says "pass/fail never depends on timing".
- **Fix:** move it to `timing` with an `hrm_` note, or make it a fail only when blits are known not to
  be immediate.

### L12. display FMODE 1/2/3: assumes the FMODE 0 pixel origin
`display.c:1659-1730`.
- Probes are placed at `origin[0] + 16*col + 6`, with `origin[0]` measured in FMODE 0. With 32/64-bit
  fetches, the first displayed word and the pipeline delay can move; the AGA docs say nothing either
  way.
- A shift of 6 pixels or more moves every probe to the neighbouring word and gives a false fail.
- **Fix:** run `edge_scan` per FMODE first, as `align_*` does per resolution, and use that origin.

### L13. display superhires gated on ECS Denise only
`display.c:627-633`, `:2336`.
- SHRES fetch needs ECS Agnus too, and OCS Agnus with ECS Denise exists.
- **Fix:** gate on `ecsa && ecsd`.

### L14. display.c header comment is stale
`display.c:9-17`.
- The header still describes "CPU reads CLXDAT in the gap after each probe line", which is the old,
  timing-dependent design. The code now uses whole-frame `clean_clx`.
- **Fix:** update the comment, since readers and AI diagnosis use it.

### L15. copper oracle does not catch early releases late in line V-1
`copper.c:265-286`.
- The V-1 probe sits at H $1A0 (about cck $D0). A WAIT that wrongly releases at line V-1, H > ~$D8
  (for example an off-by-one at the line wrap) is not detected for the `wait_v` /
  `wait_h_past_line_end_*` cases.
- Coarse but not wrong.
- **Fix:** add a second V-1 probe at the latest sprite H that is still inside DIW ($1BF).

### L16. blitter `mask_adat_rect` relies on an undocumented rule
`gen_blitter.py:416-417`.
- FWM/LWM are applied to a preloaded (non-DMA) BLTADAT. This is documented in docs/cases/blitter.md
  "Unsure", and WinUAE does it the same way.
- I agree with the expectation, but per rule 1 the case should name its evidence: "WinUAE
  blitter.cpp masks bltadat regardless of USEA; hardware-confirmed?".
- Info.

### L17. input: POTGO left at $FF00
`input.c:1474`.
- POTGO is write-only and KS 1.x may use another value, so the OS gets back a different POTGO than it
  set. Documented.
- Low impact (right/middle mouse buttons still work with outputs high).

---------------------------------------------------------------------------
## Checked and found correct (summary)

- **interrupts**
  - INTREQ→level map (TBE/DSKBLK/SOFT 1, PORTS 2, COPER/VERTB/BLIT 3, AUD0-3 4, RBF/DSKSYN 5,
    EXTER 6), autovector 24+L, format $0 vector offset.
  - Handler SR (S=1, T=0, mask=L, CCR kept: $2315→$2415), stacked SR = the pre-exception SR.
  - Mask n>mask rule, priority order and back-to-back PCs.
  - STOP semantics (wake only above the new mask, PC after STOP).
  - MOVEM/ADDQ boundary oracles (k-parity buffer selection is right).
  - Storm arithmetic (frame length ×4, NTSC 227.5).
  - Every timing value is under `timing`; CIA save/restore through `cia_hw.h`.
- **cia**
  - 8520 one-shot hi-write auto-start, stopped hi-write load, LOAD strobe, CRA bit 7 unused, CRB
    INMODE=10 cascade and exact ratio.
  - ICR set/clear semantics, IR only for enabled sources.
  - TOD binary/24-bit, TODHI read latch released by TODLO, TODHI write stops / TODLO write starts,
    ALARM write path via CRB bit 7, exact-match alarm.
  - SDR output 16 TA underflows.
  - Tolerances come from beam stamps (robust on slow CPUs).
  - `cia_hw.h` restore order (stopped-timer hi-write loads the counter, mask before START) is right.
- **blitter/gen_blitter.py**
  - Minterm bit numbering (LF bit = A<<2|B<<1|C).
  - Ascending/descending shifts with the previous word carried across lines; masked A stored as "old".
  - Masks before the shift; signed modulos with bit 0 ignored and subtracted when descending.
  - Fill bit order and FCI per line; inclusive/exclusive: exclusive drops the left edge, as WinUAE's
    fill tables do.
  - BZERO computed with D off.
  - Line mode: BLTAPTL = 4dmin-2dmax, SIGN, AMOD/BMOD, SUD/SUL/AUL decoding matches graphics/blit.h
    OCTANT codes (asserted per case), first pixel at the start; texture bit (BSH-k)&15 matches
    Amiberry `blitter_nxline`; SING = first pixel per row.
  - BLTSIZE 0 = 1024/64; ECS BLTSIZV/H and BLTCON0L.
  - The CRC window has guards. The host model rejects undefined reads, so the oracle sees the effect.
  - Priming blit zeroes the A/B old registers; the BLTBDAT write comes after BLTCON1.
  - The test-twice BBUSY rule is followed.
- **paula audio**
  - Interval 2·LEN·PER, LEN double-buffered, PER not buffered (1 old + 63 new samples).
  - Minimum period 124: one word per line per channel. Amiberry `audio.cpp:2555` also decrements LEN
    per DMA fetch, consistent with the test.
  - Direct-mode state transitions 000→010, 011→010, 011→000; DMA-off continuation.
  - ADKCON attach bit assignments (USExVy bits 0-3, USExPy bits 4-7); modulator rate accepted both
    ways for single attach.
  - INTREQ set independent of INTENA; ADKCON mask $77FF (UARTBRK excluded).
  - PRA bit 1 written with OVL preserved.
  - Host tones only as `audio_mark` (untestable without host).
- **paula disk**
  - Drive select/motor latch order; ID protocol; step pulse and ≥3 ms rate; TK0/CHNG semantics.
  - DSKLEN double-write rule; $FFFF sync cannot occur in MFM.
  - MFM sector layout and checksums (odd/even longs, $55555555).
  - DF1 write gated on both signature blocks (CRC, UUID, total, kind ADF), not write-protected, and
    the target track verified by reading its headers before writing; the write stays inside
    `[first_free, total-1)`.
  - Restore of PRB/side/heads/motor on the normal path.
- **input**
  - Keyboard SDR encoding ~(code rol 1); handshake ≥85 µs via SPMODE; hold-until-handshake (143 ms)
    rule.
  - JOYTEST bits 15-10/7-2 written to all counters, low bits untouched.
  - Joystick decode (right=X1, left=Y1, down=X1^X0, up=Y1^Y0); mouse direction/wrap.
  - POTGO output/data bits; pin-5-low never combined with pin-9 output.
  - CD32 /FIR1 output only after a host ack; DDRA/PRA restore with OVL kept; every host case is
    `untestable` on timeout.
- **timing**
  - PAL 313/312 and NTSC 263/262 by LOF, interlace alternation.
  - E = CCK/5 over whole frames (0.1 %), VBL per CIA second 50±1 / 60±1.
  - The MULU/DIVU expected values and the FPU x·0.5+1 → exactly 2.0 tie-to-even reasoning are right.
  - Perf values only in perf records.
- **dmacontention**
  - Workload result fixed on the host; memory layout stays inside the 36 KB pool (no overlap of
    blitter D, copper list, bitplanes, disk buffer).
  - Copper restart per frame; audio count against elapsed time; disk read-only, no stepping, motor
    off and PRB/ADKCON restored.
  - COP1LC restored to GfxBase->copinit.
- **copper**
  - WAIT/SKIP masked comparisons (V7 always compared, VE/HE masks).
  - H > $E2 releases at the next line; $FFDF idiom versus an 8-bit compare evaluated on line 255
    (deliberately H=$07).
  - SKIP over a WAIT; COPJMP2 from the copper and from the CPU; COP1LC write does not jump; restart at
    VBL.
  - COPEN pause/resume; BFD.
  - Danger bit: CDANG=0 stops below $80; CDANG=1 allows $40-$7E on OCS and everything on ECS/AGA
    (matches WinUAE `ecs_agnus ? 0 : 0x40`).
  - AGA RDRAM read-back with BPLCON3 $0C00.
  - Release oracle: sprite probes plus whole-frame CLXDAT; CLXDAT bits 1-4 = odd planes confirmed in
    Amiberry `drawing.cpp:3966-3978`; with only plane 1 enabled it is immune to H1.
  - All beam latencies are under `timing`; COP1LC restored, COPCON cleared, DMA off at exit.
- **display** (apart from H1, M1, L12-L14)
  - Whole-frame CLXDAT reads, cleared on read.
  - CLXCON/CLXCON2 group model (no enabled plane = match; CLXCON write resets CLXCON2).
  - ENSP rules; attached sprites.
  - Manual-mode arm/disarm; sprite VSTOP exclusive.
  - DIW HSTOP H8 implied 1, VSTOP V8 = NOT V7, ECS DIWHIGH.
  - DDF word-count formulas (lores (STOP-STRT)/8+1; hires 2× that, same as the HRM's
    STRT+4·(n-2) form for these values); early DDF steals the top sprites (handled by dropping
    sprites 4 and 6).
  - Odd/even modulos, BPLCON1 per playfield in lores pixels (hires 0-7 only).
  - AGA fine scroll bit layout (PF1H1-0 bits 9-8).
  - FMODE sprite widths (SPR32/SPAGEM) and SPRES widths.
  - 24-bit palette banks/LOCT and the LOCT=0 copy.
  - Beam frame lengths observed by copper markers (timing-independent).
  - Restore at exit (FMODE, BPLCON0-4, CLXCON, DMA, SPRxCTL, copinit).

---------------------------------------------------------------------------
## Resolution (display/copper)

- **Timing independence (both modules).** Every collision observation is a whole-frame CLXDAT read:
  the scene is static, CLXCON/sprite lists change at a VBL, CLXDAT is thrown away at the next three
  VBLs (a core may deliver a frame's collisions late) and read two frames later. One probe group (4
  sprites) and one plane per measurement. Copper release positions use a copper-internal oracle (the
  release MOVE turns on an all-ones bitplane; one-line sprites before/after it; the same whole-frame
  CLXDAT read). Frame lengths are observed through copper markers on the last lines. COPER is counted
  VBL to VBL. Beam-polled latencies are only in `timing`.
- **H1 (High).** `clx_spf_odd_independent` accepts both results in single playfield (independent or
  coupled odd bit); the observed variant goes to `ctx.undef_coupling` with a `check_on_real_hardware`
  note. `clx_spr_pf_spf` passes if either model fits every combination. `clx_spr_pf_dpf` stays strict.
  This is no longer reported as a finding.
- **M1 (Medium).** `deniseid` now reads DENISEID 16 times and checks against the Agnus ID read from
  VPOSR (not the probe): Alice => $F8 and stable; no Alice => not $F8; $FC must be stable. Bits 15-8 and
  the raw value are informational (`timing.raw`, `ctx.undef_denise_class`).
- **M9.** `copper danger_* ctx.joy1dat_after` renamed `ctx.undef_joy1dat_after`.
- **L12.** The FMODE cases first measure their own pixel origin (edge between words 0 and 1 of row 0,
  all probes on that row) and report it as `ctx.origin_fmode`.
- **L13.** Superhires cases (`align_shres`, `mode_shres_*`, `dma_cpu_shres_2bpl`) are gated on ECS
  Agnus and ECS Denise.
- **L14.** The display.c header describes the clean-frame design.
- **L15.** The copper oracle has a second probe on line V-1 at H $1B0 (the last 16 window pixels).

---------------------------------------------------------------------------
## Resolution (interrupts, cia, blitter, paula, input, timing, dmacontention)

Counts for these modules: fixed 19 (M2-M8, L1-L11, L16) plus M9 for the cia keys; rejected 1
(the M9 `paula drive_ids in.id` part); deferred 1 (L17). The copper key of M9 belongs to the
display/copper work (renamed there). H1, M1, L12-L15 and the copper part of M9 are in "Resolution (display/copper)" above.

Runtime help used: the runner now excludes every `undef_*` key from fingerprints; the runtime
restores DSKLEN/FMODE/copper/caches and quiets both CIAs after every module, even an abort.

- **M2 fixed.** `cia?_icr_irq_to_paula` polls INTREQR for up to 20 E clocks (`intreq_wait`,
  `cia_now()`-bounded) instead of one read. `icr_read_releases_irq` waits 20 E clocks after the ICR
  read before clearing INTREQ, so only a line that stays asserted can set the bit again.
- **M3 fixed.** `ctx.intreq_relatched` -> `ctx.undef_intreq_relatched` (measured after a bounded
  wait: line seen, cleared, read 4 E later); `icr_mask_after_event` `ctx.icr`/`ctx.intreq` ->
  `ctx.undef_icr`/`ctx.undef_intreq` (INTREQ polled 20 E).
- **M4 fixed.** `cck_per_line` samples in blocks of 1000 until at least 8 whole frames of beam
  lines have passed (V counted between blocks) and at least 100 blocks were taken (3 s cap). Sample
  and line counts are `timing` fields.
- **M5 fixed.** `amt_run_paula` runs the audio part, the disk part and the host tones each under
  `amt_try`. `pa_disk_restore()` runs on every path: DSKLEN $4000, disk DMA and interrupts off,
  hooks cleared, the moved head stepped back by its net step count (tracked from the first step, so
  an abort inside the /TRACK0 seek is covered; not moved when /TRACK0 was never found), all motors
  off, drives deselected, PRB as found. After an abort the remaining parts are skipped and the abort
  is passed on. The watchdog is kicked before every read try.
- **M6 fixed.** dmacontention reads DF0 only when booted through trackdisk.device, and only after a
  reference read without other DMA found a $4489 sync and a valid AmigaDOS sector. Otherwise `disk`
  is `untestable` with the reason and `all` runs without its disk part. A loaded read that then
  fails is still a fail (the same track read correctly without load).
- **M7 fixed.** timing uses `cia_save(&s, CIAB_BASE, 0)` / `cia_restore(&s)`; `cia_restore`
  keeps its read-back check of CRA/CRB.
- **M8 fixed.** Every SR=$2700 poll loop in timing is bounded: beam loops by the CIA-B E clock,
  the CIA-second loop by the VBL count (3x expected), all by a poll count (4M) when neither clock
  moves. Hitting a bound fails the case with "beam/VERTB/CIA stuck". In `sample_frames` the guard
  runs only on line changes at the top of the frame or after 256 polls without a line change, so a
  68000 poll loop still sees the frame's last line (one call per poll made A500 CE miss line 312).
- **M9.** cia keys renamed (M3). `paula drive_ids in.id` **rejected**: it is a deterministic
  property of the configuration (HRM ch.8 drive ID), the same for JIT on/off and across cores, and
  a difference between equally configured emulators is a real finding. The copper key is owned by
  the display/copper work (renamed there).
- **L1 fixed.** dmacontention `audio` uses `dc_now()`, a beam clock corrected for an uncounted VBL
  (as `cia_now()`), for both time stamps.
- **L2 fixed.** Failing records keep measured values under `got.timing`: paula `chk_report`
  (`got.result` below/above/timeout, value in `got.timing.got`), paula disk `j_dres` (DSKBLK count
  stays; DSKSYN count, times, DSKBYTR under `timing`), `wordsync_waits`, `df1_write_dskblk`
  (`got.t_dskblk` none/early/late/in_range), `j_tres_timing`; dmacontention COPER/audio counts
  (`coper_irqs_within_1` / `aud_irqs_within_3` = 1/0, counts in `timing.coper/coper_frames`,
  `timing.aud/aud_expected`); interrupts `storm_rec` (`got.count` too few/too many, numbers under
  `got.timing` and `timing`).
- **L3 fixed.** Every CIA-A ICR read in an interrupts handler (`ir_common`, the runaway path,
  `ir_snap2`, `ir_st2`, `ir_lat2`) checks SP with CRA SPMODE = 0 and sets `ir_kbd_pend`; the C side
  calls `cia_kbd_handshake()` in `env()`/`env_end()`, in the storm loop and at module end.
- **L4 fixed.** New `cia_int_line_recheck()` in cia_hw.h (both ICR masks clear and drained, INTENA
  bit off, then `cia_int_line_stuck`). interrupts calls it on every failing case that counts
  interrupts or reads INTREQR; cia on `icr_read_releases_irq`. A newly held line makes the case,
  and later cases needing it, `untestable`.
- **L5 fixed.** The nesting window after a handler's trigger write waits for two beam line changes
  (at most 65536 polls) instead of 16 reads.
- **L6 fixed.** The TOD binary/rollover cases use the first value two consecutive reads agree on
  after the change (8520 TODMED carry transient).
- **L7 fixed.** `cia_restore` parks the TOD alarm at TOD-1 whenever it restores the TOD (only the
  cia module saves TOD), so it matches only after a full 24-bit wrap.
- **L8 fixed.** If sector 2 of DF0 decodes cleanly but has no "AMTP" magic or another build id
  (offset 128-191), `df0_bootblock`, `df0_payload_header` and `df0_blob_crc` are `untestable`
  ("DF0 holds another disk"). A non-decodable DF0 is still a fail (can't tell it apart from a DMA
  bug).
- **L9 fixed.** The HRM ch.8 drive timing gives only the 3 ms step rate and the head settle, no
  direction-reversal figure. The module now waits 4 ms (a full step period) after a DIR change, and
  20 ms (`settle()`) before each `df0_track0_sensor` read.
- **L10 fixed.** DF1 is only touched with drive ID $FFFFFFFF or $AAAAAAAA; other IDs (e.g.
  $55555555, 5.25") skip the DF1 cases.
- **L11 fixed.** `samples_busy == 0` is no longer a fail; it is `timing.samples_busy` with
  `timing.hrm_samples_busy_min` = 1.
- **L16 fixed (info).** gen_blitter.py and docs/cases/blitter.md now give the evidence for
  `mask_adat_rect`: WinUAE/Amiberry blitter.cpp masks the A data whether fetched or preloaded
  (`blitter_doblit`, `blitter_loadadat`); not hardware-confirmed. Table regenerated (unchanged
  output); `tools/check_generated.sh` reports ok.
- **Found while verifying (fixed, not review items):**
  - `ciab_tod_alarm_fires`: the "TOD at the flag" bound counted only the current poll; the flag can
    be set just after the previous poll's ICR read (a VBL interrupt or a JIT stall between two
    polls), which gave 2 false fails (A1200 JIT, A500 NTSC). The window now starts at the stamp
    before the previous ICR read.
  - dmacontention `all` on A500 NTSC: the beam clock counted 227 cck per NTSC line (average 227.5),
    0.2 % short, 9 audio interrupts over the 7 s A500 run. The clock is now in half colour clocks
    with 455 per NTSC line.
- **L17 deferred.** POTGO is write-only (HRM appendix A) and the OS value is only in
  potgo.resource's private fields; $FF00 is what KS 2.04+/AROS write and keeps both buttons
  readable. Restoring the KS 1.x value needs a documented source for it.

Verification (`make -C disk B=build-fixchip MODS="rt interrupts cia blitter paula input timing
dmacontention"`, tests/smoke_disk.py, ports 23880-23887). Fails per config (all known findings):

| config | pass | fail | skip | untestable (host requests) | fails |
|---|---|---|---|---|---|
| A500 KS1.3 OCS cycle-exact | 657 | 0 | 13 | 29 | |
| A500 KS1.3 OCS fast | 656 | 1 | 13 | 29 | storm_cia_timer (H1) |
| A500 KS1.3 OCS cycle-exact NTSC | 657 | 0 | 13 | 29 | |
| A600 KS2.05 ECS | 662 | 0 | 8 | 29 | |
| A1200 KS3.1 AGA cycle-exact | 664 | 0 | 6 | 29 | |
| A1200 KS3.1 AGA fast | 663 | 1 | 6 | 29 | storm_cia_timer (H1) |
| A1200 KS3.1 AGA fast + JIT | 658 | 6 | 6 | 29 | storm_cia_timer (H1); priority_order, priority_back_to_back_pc, movesr/andisr_lower_taken_next, stacked_sr_is_pre_interrupt (J4) |
| A1200 KS3.1 AGA fast NTSC | 663 | 1 | 6 | 29 | storm_cia_timer (H1) |

Fingerprints (minus `timing`/`latency*`/`undef_*`) are identical across cycle-exact/fast/JIT and
PAL/NTSC, apart from variant skips, `drive_ids` (A500 DF0 answers $00000000, A600/A1200
$FFFFFFFF), the fails above, and records lost to the serial byte duplication (D3).

Amiberry verdicts: H1 now also seen on A500 fast (68000, non-cycle-exact), not only on 020+
configs. One unconfirmed observation: `paula wordsync_waits` (DSKSYNC $FFFF with WORDSYNC) failed
once on A1200 fast with a transfer starting at once (DSKBLK after ~8 ms, unsynced data). It passed
in two later runs and in every earlier run, so it is intermittent and not reported as a finding.
