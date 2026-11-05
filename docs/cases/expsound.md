# expsound (module 17, AMT_PH_BARE, disk group "exp")

Sound cards, register level. Only boards found in `amt_probe_result.boards` (expansion.library's
ConfigDev list) with a known manufacturer/product are touched:

| board | man/prod | codec | registers (board offsets) | interrupt |
|---|---|---|---|---|
| MacroSystem Toccata | 18260 ($4754) / 12 | AD1848 | R0 index $6001, R1 data $6801, play FIFO $2000, board status $0000 | INT6 |
| ACT Prelude (Zorro II) | 16945 ($4231) / 1 | CS4231A | R0 $80, R1 $82, FIFO $88, FIFO status $8C (read), reset strobe $8A (write) | INT6 |
| ACT Prelude 1200 (A1200 clock port, $D80000) | not autoconfig | CS4231A | R0 $01, R1 $05, FIFO $11, FIFO status / reset $15 | INT6 |
| UAE uaesnd Z2/Z3 | 6502 / 2 | (UAE ABI) | see the specification comment in Amiberry sndboard.cpp | INT6 |

The Prelude 1200 is not an autoconfig board. `prelude1200_ownership` is untestable: there is no
ConfigDev provenance or exclusive clock-port reservation. It is never probed or written,
including with `unsafe`; its layout above is reference information, not exercised coverage.

**Expectation sources.** The codec cases follow the Analog Devices AD1848K and Crystal CS4231A data
sheets (index/data protocol, R0 INIT/MCE/TRD, the MCE protection of I8/I9/I28 with PEN/CEN as the
only on-the-fly bits, I11 ACI, the read-only ID/version bits of I12/I25, MODE2) and the OSS/Linux
`ad1848` probe sequence that relies on them. The board glue (addresses, the Toccata status register,
the Prelude FIFO flag bits) has no public documentation; its *layout* comes from Amiberry's
sndboard.cpp, the flag polarity is the IDT720x FIFO convention (active-low /EF, /HF, /FF). uaesnd is
a UAE-only device: its specification is the comment block in sndboard.cpp ("uaesnd spec").

**Ownership and selection.** Every destructive autoconfig test requires `amt_board_claim()`.
Unclaimed boot-owned boards retain ordinary coverage without `unsafe` or `net`. The helper checks
current BoardList membership before dereferencing retained ConfigDev provenance, revalidates
manufacturer/product/type/serial/address/size, rejects a bound driver or unsafe live flags, and
atomically clears current CDF_CONFIGME under bare-phase Forbid/Disable exclusion. Probe flags are
not ownership. Neither `unsafe` nor `net` bypasses a refused claim. Claims remain reserved for
later functions of the same board through the intentional HALT/REBOOT lifecycle; RETURN skips
bare testing. The OS probe pins the list's library storage only for HALT/REBOOT, deriving the
actual list-head sentinel from the first FindConfigDev result under configuration binding.
No private ExpansionBase offset or semaphore-taking enumeration is used in bare mode.
RAM, FIFOs, stream positions and pending events are not recoverable.
Only the first ConfigDev entry of each family is selected; additional
Toccata, Prelude and uaesnd cards (including a Z2/Z3 pair) are logged with address/probe index, not
tested again under duplicate IDs. A selected undersized aperture is skipped as `board_span`.

Codec cases are prefixed with the board: `toccata_`, `prelude_`, `prelude1200_`. Configuration
cleanup saves I0-I15 and, on CS4231, writable MODE2 I16/I17/I18/I19/I20/I21/I26/I28/I30/I31 before
resetting the board. It restores the clock under MCE, waits for INIT before further accesses,
restores extended state while MODE2 is accessible, then restores I12's original mode. PEN/CEN,
I10's interrupt enable and I16's timer enable remain off: this is **not full device/driver
restoration**. An initially asserted MCE prevents a stable snapshot and makes the board untestable.
All I8 writes wait for INIT; MCE exit also checks INIT/ACI. A failed readiness wait aborts rather
than using ignored reads/writes as evidence.

A nested `amt_try` scope runs phase-tracked device cleanup on normal completion, exceptions and
watchdog aborts, before restoring CIA-B masks or propagating the abort to the runtime. Incomplete
cleanup is a failure, never a successful restore. Cleanup readiness/quiescence failures or
exceptions call `amt_abort_fatal`: permanent in-place halt, with no OS resumption, subsequent
module, buffer release or normal completion. Nested abort catchers cannot swallow that halt.
If fatal reporting itself traps, sticky quarantine stops outer bare recovery at SR=$2700 before
restoration or further module execution.
CIA-B (also on INT6) uses `cia_hw.h`; unrelated
shared-line activity makes the IRQ-dependent verdict untestable rather than a card failure.
Every interrupt hook has a 20000-entry storm guard. Board reset/quiescence still relies on the
documented provenance limits of the glue model; it cannot reconstruct lost hardware state.

## Codec boards (`<board>_*`)

| case | intent | rule (reference) | a failure usually means |
|---|---|---|---|
| `init_bit_clear` | R0 bit 7 (INIT) reads 0 once the codec is idle (waits up to 1 s) | AD1848/CS4231 R0 INIT | codec never leaves initialisation; nothing else is meaningful |
| `index_readback` | R0 written with IXA 0-15, alone, with MCE (bit 6) and with TRD (bit 5), reads back (bits $6F) | R0 is read/write except INIT | index register not latched, or MCE/TRD dropped |
| `ireg_readback` | I0/I1 with $AA/$45 both ways (OSS probe values), I2-I7, I13, I14, I15 with reserved bits 0 read back | data sheet register map | indirect register file not read/write, index/data mixed up |
| `codec_id` | I12 bits 3-0: CS4231 $A; AD1848 $A (rev C) or $1 (rev B) | I12 ID; OSS ad1848.c revision note | wrong chip emulated or wrong register |
| `codec_id_readonly` | writing ~ID to I12 bits 3-0 leaves them unchanged | I12 ID bits are read-only | ID bits writable |
| `no_mode2` (AD1848) | I12 bit 6 reads 0 after writing $40 | AD1848 has no MODE2 | AD1848 behaves like a CS4231 |
| `i8_rates_formats` | with MCE: all 16 CSL/CFS rate selections and the format bits read back | I8 read/write in mode change | rate/format bits not stored |
| `i8_needs_mce` | with MCE clear, a write to I8 is ignored | data sheet R0 MCE: "I8 and I9 CANNOT be changed unless this bit is set" | MCE protection not emulated |
| `i9_needs_mce` | explicitly establish I9=0 under MCE, then with MCE clear write SDC ($04), compare bits $CC (which includes $04) | same; PEN/CEN are the only on-the-fly bits | MCE protection not emulated |
| `autocal_aci` | MCE=1, I9=ACAL, MCE=0: I11 ACI reads 1 within 0.2 s, then 0 within 3 s | I9 ACAL, I11 ACI (Linux snd_wss_mce_down waits the same way) | no autocalibration emulated, or ACI stuck |
| `mode2_enable` (CS4231) | I12 <- $40 reads $CA (bit 7 = 1, MODE2, ID $A) | CS4231 I12; OSS detection `& 0xc0 == 0xc0` | MODE2 not supported |
| `i25_version` (CS4231) | I25 & $E7 is $A0 (CS4231A) or $80 (CS4231) | CS4231 I25 | wrong chip/version |
| `i25_readonly` (CS4231) | writing ~I25 leaves bits $E7 unchanged | I25 is read-only (OSS probe step) | version register writable |
| `i16_separate` (CS4231) | in MODE2, writing I0 does not change I16 | MODE2 has 32 separate registers | index 16-31 aliased to 0-15 in MODE2 |
| `mode2_regs` (CS4231) | I18/I19 (line in), I26 (mono I/O) read back | CS4231 MODE2 registers | MODE2 register file missing |
| `i28_needs_mce` (CS4231) | MODE2: with MCE clear a write to I28 is ignored | R0 MCE protects I28 too | MCE protection not emulated |
| `mode1_back` (CS4231) | I12 <- $00 clears MODE2 | I12 MODE2 is read/write | stuck in MODE2 |
| `record_loopback` | skip: the codecs have no digital DAC-to-ADC loopback (only the analog post-mix input) | | |
| `fifo_flags` (Prelude, Prelude 1200) | FIFO status nibble (/HF play, /EF play, /HF rec, /FF rec) after reset, 1, 600, 1024 bytes written to the idle play FIFO, and after another reset: $B,$F,$7,$7,$B | IDT720x flags, active low; layout from sndboard.cpp | FIFO levels or flag bits wrong, reset does not empty the FIFO |
| `play_u8m_5512` | configure 8-bit mono at nominal 5512.5 Hz, deliver 4096 bytes (2048 quick): fill 1024, refill 512 at each half-empty interrupt; exactly (N-1024)/512+1 attributable interrupts. Prelude empty is measured; Toccata empty and actual rate are not part of this verdict | half-FIFO delivery | missing/doubled interrupts, lost bytes or Prelude FIFO not empty |
| `play_s16s_11025` | same delivery check, configured as 16-bit stereo little endian at nominal 11025 Hz, 8192 bytes (4096 quick); does not prove rate or stereo output | as above | as above, for 4-byte frames |
| `drain_u8m_5512`, `drain_s16s_11025` (Toccata) | untestable: no documented play-empty indication; a timed delay is not empty evidence | hardware observability limitation | |
| `rate_s16s_11025` | untestable: IRQ period is diagnostic only and there is no independent calibrated stereo-rate measurement | oracle limitation | |
| `play_intena_off` (Toccata) | PLAY_INTENA clear: no attributable interrupt during the fixed playback observation interval; drain is not measured | board status enable | interrupt not gated by the enable |
| `audio_capture` | `audio_mark` start/stop host requests around `play_u8m_5512`; untestable when the host does not acknowledge. Informational: the start mark describes the tone (square wave, 32 frames per cycle at 5512.5 Hz: `freq_mhz` 172265, `tol_pct` 2, `ms` = frames / rate, 743 or 372 quick), and the runner judges it as the pseudo case `host.tone_<board>_u8m_5512` | CONTRACT section 13; docs/schema.md 5.1 | (pseudo case) codec rate wrong, or the board's output not mixed |

Perf records `<board>_<fmt>_irq_period`: ops = interrupt intervals, ticks = colour clocks between the
first and the last interrupt. One interval is 512 bytes / bytes-per-frame / sample rate (329434 cck
PAL for u8m_5512, 41179 cck for s16s_11025); the same value is in `ctx.timing.expect_period_cck` of a
failing play case. These performance records do not impose an unstated rate tolerance.
The E02 audit allegation that `$CC` omitted SDC is withdrawn: `$CC & $04 == $04`.
The explicit local I9 baseline is test self-containment strengthening, not a repaired mask bug.

## uaesnd (`uaesnd_*`)

Stream 1 ($0100) is used. All registers are accessed with the width the spec names (.B/.W/.L). On a
68000, and on any CPU through the 16-bit Zorro II bus, a .L access reaches the board as two word
cycles; the spec states that 68000/010 word accesses are supported.

| case | intent | rule (reference) | a failure usually means |
|---|---|---|---|
| `info_streams` | $008E.B = 8 | spec: max simultaneous streams | |
| `info_ram` | Z2: $0090.L = $8000, $0094.L = $8000; Z3: $800000 / $800000 | spec: Z2 32 KB at $8000-$FFFF, Z3 8 MB upper half | RAM window wrong |
| `ram_rw` | longs written at RAM +0, +$1000, +$4000, +$7FF0 read back | board RAM | RAM not mapped |
| `alloc_mask` | $00E0.L <- 3 reads 3, <- 0 reads 0, $00E3.B <- 1 reads 1 | spec: $E0.L R/W, byte access to $E3 allowed | long write lost (e.g. split into words that the board ignores) |
| `master_inten` | $00F4.L <- 1 reads 1 | spec: $F4.L R/W | as above |
| `stream_enable` | $00F0.L <- 1 reads 1 | spec | |
| `latch_rw` | $0140-$017F (latched set structure) read/write | spec | |
| `timer_irq` | $0194.L = 200<<8 (200 Hz) written right after a VBL, 0 written right after the 100th VBL (120th NTSC): exactly floor(window / period) INT6 interrupts = 400 PAL (100 × 71051 cck / (3546895/200) = 400.6), 401 NTSC (120 × 59832.5 / (3579545/200) = 401.2, HRM: 227.5 cck per NTSC line), no margin; $0187.B = $90 at each, none after writing 0. The period is in emulated time: (colour clocks per s) / rate (sndboard.cpp uaesnd_timer_period: base_event_clock × CYCLE_UNIT × 256 / v), the first interrupt one period after the write. The nearest interrupt is >= 0.37 period (~29 lines) PAL, >= 0.16 period (~12 lines) NTSC, from either window edge while each edge lands within a line of its VBL, so the count is exact; a +-1 margin would hide a rate error up to 0.25 %. A failure has the beam-measured mean period in `ctx.timing.period_cck` (expected cck/200 = 17734 PAL) | spec: $0194.L frequency in Hz × 256 (as 12.L), sets status bit 4, interrupt level 6, reading clears, 0 disables | timer rate wrong (e.g. counted in CPU cycles or frames, or a sound-board clock scaled by the display refresh: Amiberry 399, finding), not running, wrong status, or not stoppable |
| `play_irqs` | one-shot set (4410 frames, 22050 Hz, 8-bit signed mono, 34.B = start|end): exactly 2 interrupts, status & $87 = $81 then $82 | spec: 34.B bits 0/1, $0187 bit 7 | start/end interrupt missing or doubled |
| `play_stopped` | after the end $0193.B bit 0 (play active) = 0 | spec | stream not stopped |
| `setptr_cleared` | after the end $0180.L reads 0 | spec: the set pointer "also clears automatically when stream ends" | pointer not cleared |
| `diag_start_stop` | one playback adds 1 to $00A8 and 1 to $00AC (diagnostics capability only) | spec diagnostics block | counters wrong (or the playback never started) |
| `audio_capture` | `audio_mark` around `play_irqs` (informational); the start mark describes the tone (square wave, 50 frames per cycle at 22050 Hz: `freq_mhz` 441000, `tol_pct` 2, `ms` 200), judged by the runner as the pseudo case `host.tone_uaesnd_play` | CONTRACT section 13; docs/schema.md 5.1 | (pseudo case) wrong playback rate, or the stream not mixed |
| `mono_active` | silent repeating mono must start and remain active ($0193 bit 0) at every poll through a 25-frame observation window | uaesnd play-active status | mono never started or stopped early |
| `stream_reuse` | 12 one-shot sets (1102 frames, 22050 Hz, silent) played one after another, each to its end: every one starts ($0193 bit 0 one frame after $0180), and with diagnostics the alloc-failure count $00A4 does not change | spec: up to 8 streams at a time; a stream that ends is released | ended streams keep their mixer stream (Amiberry up to 07708117f: the stream callback revived each ended stream as an ownerless one-channel stream), so new streams fail to allocate |
| `repeat_irqs` | 2205 frames + repeat count 2 (1102 frames), 34.B = repeat|end: status & $86 = $84, $84, $82 | spec: repeat count, 34.B bit 2; bits 0/3 are set whenever their condition occurs, so they are masked | repeat count off by one |
| `invalid_set` | flags = 1 (must be 0): no start, $0193 bit 0 = 0, and with diagnostics $00A0 += 1, $00B8 = 1 | spec: validation, last error 1 = invalid set | invalid sets played |

Perf: `uaesnd_timer_200hz_period` (interrupt intervals, cck), `uaesnd_play_4410_22050_start_to_end`.
The timer handler counts status mismatches on **every** interrupt, not just its first eight
diagnostic samples. uaesnd cleanup disables timer/interrupts/streams, verifies stream 1 stopped
and enable masks clear, then releases the test allocation. It does not restore overwritten RAM
or pre-existing driver allocations.

`no_board`: skip when no known board is present. `<board>_board_rejected` (toccata, prelude, uaesnd):
skip, the board fails amt_board_ok() (shut up / no valid Zorro address) and is not touched (review H1).
Failure ctx carries `glue_source`: the board glue is modelled on Amiberry sndboard.cpp (review M6).

Not covered: the ZZ9000AX audio interface (a firmware ring-buffer ABI with no chip data sheet behind
it), the uaesnd capture registers ($00C0-$00DC, $0900-$090C; they need a host capture device), and
recording on the codec boards (Amiberry takes it from a host capture device).

Native/source-model regressions (no emulator):
`cd runner && TMPDIR=/home/solskogen/source/amitorture/work/tmp PYTHONDONTWRITEBYTECODE=1 python3 -B -m unittest -v tests.test_guest_board_claim tests.test_guest_expansion tests.test_guest_expansion_fatal`.
