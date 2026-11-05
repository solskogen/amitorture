# paula (module 14, AMT_PH_BARE, disk group "chip")

Audit validity and restoration: modulator alternatives are the disjoint
intervals 2800-3600 and 6000-6800 cck, not their convex hull (4800 is rejected).
ISR/stamp beam sampling has a 256-attempt budget; exhausted samples invalidate
dependent timing verdicts as failures and the ISR disables the audio source before returning.
Invalidity resets per capture; a failed trial remains failed in an aggregate
even after later captures succeed.
DF1 marker pairs must agree on first-free, block size, kind, total and UUID
before writes. A completed STEP edge and its displacement accounting are
interrupt-atomic, before the settling delay; abort cleanup completes and
accounts a pulse interrupted while STEP was low before restoring displacement.

Paula audio (DMA, interrupts, direct mode, ADKCON attach) and the disk controller (DMA, sync, MFM, drive lines).
References: Amiga Hardware Reference Manual (HRM) ch.5 "Audio hardware" (incl. the audio state diagram: states
000 idle, 001/101 DMA start, 010/011 high/low byte; `napnav`, AUDxIP = the channel's INTREQ bit, AUDxIR =
request), ch.6 DMA time-slot allocation (one audio slot per channel per line), ch.7 INTREQ levels (AUDx 4,
DSKBLK 1, DSKSYN 5), ch.8 "Disk controller" (DSKLEN, DSKPT, DSKSYNC, ADKCON disk bits, DSKBYTR, CIA-B PRB drive
lines, CIA-A PRA status lines, drive ID), appendix F 8520 (CIA-A PRA bit 1 = LED/filter), and the
AmigaDOS/trackdisk MFM track format.

Timing: audio interrupt times are logged by `pa_l4` (vector 28) from VPOSR/VHPOSR plus the VBL count, in colour
clocks (cck; PAL line 227, NTSC 227.5 average). High/low/high reads guard V8-V10.
Only H=0/1 is rejected: V increments at H=2, which is already coherent,
including when a fast core waits there for host pacing. Late-line positions
are valid too. Measured values go in `timing` objects only; the bounds in `in.bounds` are fixed, so
records match across JIT on/off, CE/fast and emulators. Audio runs with volume 0 (silent) except the host tones.

Disk safety: DF0 (the boot disk) is only read. DF1 is written only when raw-read blocks 0 and 1759 carry the
CONTRACT section 11 signature (same UUID, 1760 blocks, ADF), it is not write-protected and the target track 81
(blocks 891-901) is verified by a read first. Heads go back to the cylinder found (steps counted from /TRACK0),
motors off, drives deselected, CIA-B PRB as found. Raw reads are retried (up to 4 reads, good sectors merged) as
trackdisk does on soft errors; the number of reads is a timing field. The watchdog is kicked before every read
try (each is bounded to 1.2 s).

Abort safety: the audio part, the disk part and the host tones each run under `amt_try`. The disk restore
(`pa_disk_restore`: DSKLEN $4000, disk DMA and interrupts off, the moved head stepped back by its net step
count, all four motors off, drives deselected, PRB as found) runs on every path, also after an unexpected
exception, a watchdog expiry or `amt_abort`; the parts after an abort are not run and the abort is passed on.
The step count is kept from the first step, so even an abort inside the /TRACK0 seek puts the head back; only
when /TRACK0 was never found is the head left where it is (its position is unknown).

Stepping: after a DIR change the module waits a full step period (4 ms) before the STEP pulse, and the
`df0_track0_sensor` reads come after the 20 ms head settle.

Failing records keep measured values (interrupt times, DSKSYN counts, DSKBYTR samples, read retries) under a
`timing` object inside `got`; only the bound and whether the value was below/above/missing are fingerprinted.

| case | intent | rule (reference) | a failure usually means |
|---|---|---|---|
| `dmaconr_aud_bits` | DMACON SET/CLR AUD0EN-3EN (DMAEN off) read back in DMACONR | HRM DMACON | DMACON read-back |
| `adkcon_readback` | every ADKCON bit except UARTBRK (0-10, 12-14) set/cleared alone, and 3 patterns, read back in ADKCONR (mask $77FF) | HRM ADKCON | missing/stuck attach, FAST, MSBSYNC, WORDSYNC, MFMPREC or PRECOMP bits |
| `aud0_dma_interval` ... `aud3_dma_interval` | steady-state AUDx interrupt interval = LEN*2*PER cck +-PER, for (16,124), (32,200), (64,428) (quick: (32,200)) | HRM ch.5: interrupt at each LC/LEN reload, 2 samples per word, PER cck per sample | wrong period unit, length counting, interrupt per word/sample, per-channel bug |
| `aud0_dma_start` ... `aud3_dma_start` | first interrupt <= 3 lines after DMACON SET DMAEN\|AUDxEN; first block between 2(LEN-1)PER-PER and 2LEN*PER+PER | HRM state diagram 000->001->101 (interrupt when the first word arrives, 2 words prefetched) | DMA start latency, first interrupt missing or at block end |
| `aud_double_buffer_len` | AUD0LEN 32->16 written in the handler of interrupt 3: interval 3->4 still 32 words, 4->5 and 5->6 16 words | HRM ch.5 double buffering (the interrupt means LC/LEN were copied) | LEN applied immediately (not double-buffered) or one block late |
| `aud_per_change` | AUD1PER 200->300 in the handler of interrupt 3: that block already 1 old + 63 new samples; later blocks 2*32*300 | HRM ch.5: PER reloads at every sample end (not buffered) | period latched per block/word |
| `aud_min_period` | PER 64 and 100 (< 124): block of 32 words lasts 32 lines (+-1 line), not 2*32*PER | HRM ch.5 minimum period 124; one audio DMA slot per channel per line | audio DMA not limited by slots |
| `aud_manual_words` | direct mode (no DMA): 8 AUDxDAT words fed by the handler: 8 interrupts, the first at once (<= PER), then every 2*PER | HRM ch.5 direct output; state diagram 000->010 (AUDxIR), 011->010 | no interrupt per word, wrong rate |
| `aud_manual_idle` | interrupt 8 left pending (INTENA cleared instead): channel idles; clearing it later raises nothing | state diagram 011->000 (!AUDxON & AUDxIP) | channel keeps running/looping in direct mode |
| `aud_dma_off_direct` | AUD1EN cleared in the handler of DMA interrupt 3: next interrupt 2*PER later (end of the current word), then every 2*PER (direct mode on the last data), idle once interrupt 8 is left pending | state diagram 011->010 when !AUDxON & !AUDxIP | DMA off stops at once / goes idle / keeps DMA running |
| `adk_pmod_01`, `adk_pmod_12`, `adk_pmod_23` | USExPy: modulated channel (16 words, own PER 150) runs at the data period 300: 2*16*300 per block; for 0->1 also the modulator (8 words, PER 400) block = 8*400 or 2*8*400 (both accepted) | HRM ch.5 attached channels | period modulation not applied, or applied to the wrong channel |
| `adk_vmod_01` | USE0V1 only: channel 1 keeps its own period (2*16*150); modulator block 8*400 or 2*8*400 | HRM ch.5 | volume attach changes the period |
| `adk_pvmod_01` | USE0V1\|USE0P1: words alternate volume, period: channel 1 at 300; modulator 8 words in 8*400 (one word per period) | HRM ch.5 (both attached: words alternate) | wrong word order, wrong modulator rate |
| `adk_use3pn` | USE3PN: channel 3 attached to nothing still runs; block 8*400 or 2*8*400 | HRM ch.5 | channel 3 stops / wrong rate |
| `aud_intreq_poll` | INTENA AUD3 off: INTREQR bit 10 still set at each block start, polled: 6 requests, 2*LEN*PER apart | HRM ch.7 (requests independent of INTENA) | request gated by INTENA |
| `filter_led` | CIA-A PRA bit 1 (/LED, audio filter) written 0/1 reads back; restored (bit 0 OVL kept) | HRM appendix F, 8520 output port | PRA output latch read-back |
| `drive_ids` | informational: drive ID of DF0-DF3 (32 SEL pulses, /RDY low = 1) | HRM ch.8 drive identification | (none: config-dependent; DF0 need not answer) |
| `df0_motor_ready` | DF0 selected with motor on: /DSKRDY low within 1 s | HRM ch.8 | RDY not emulated, motor latch on /SEL edge |
| `df0_seek_track0` | stepping out (DIR=1, 4 ms) reaches /TRACK0 within 90 steps; entry cylinder in timing | HRM ch.8 | STEP/DIR lines, TRACK0 |
| `df0_track0_sensor` | /TRACK0 low at cyl 0, high after one inward step, low again after one outward | HRM ch.8 | TRACK0 not tied to the head position |
| `df0_disk_change` | after a step with a disk in, /DSKCHANGE reads high | HRM ch.8 | change latch not reset by STEP |
| `dsklen_single_write` | one DSKLEN write with DMAEN: no DMA (canary intact, no DSKBLK) in 40 ms | HRM ch.8 (write DSKLEN twice) | single-write start |
| `dsklen_double_write` | the second write starts it: DSKBLK and buffer written within 100 ms (no word sync) | HRM ch.8 | double-write start broken |
| `dsklen_exact_count` | a word-synced 1024-word read writes words 0..1023 and nothing in the 64 guard words after | HRM ch.8 (DSKLEN = word count) | DMA overruns the buffer (seen with fast floppy modes) |
| `wordsync_waits` | DSKSYNC $FFFF (impossible in MFM) + WORDSYNC: nothing transferred, no DSKSYN, in 450 ms. Every transfer waits ~110 us after the ADKCON write before DSKSYNC/DSKLEN: with FAST off between transfers the DD data is sampled in 4 us cells and the shift register can fill with $FFFF, which then matched at once | HRM ch.8 WORDSYNC | DMA not waiting for the sync word |
| `wordsync_off` | no WORDSYNC: 256 words arrive and DSKBLK within 100 ms | HRM ch.8 | read DMA only works synced |
| `dskblk_irq` | track 0 read: exactly one DSKBLK, taken at level 1 | HRM ch.7/8 | wrong level, missing/doubled DSKBLK |
| `dsksyn_irq` | same read (7000 words, > 1 rev): >= 11 DSKSYN at level 5, first before DSKBLK | HRM ch.7/8 | DSKSYN not raised per sync match, wrong level |
| `dskbytr_read` | during the read DSKBYTR shows DSKBYT and DMAON, never DISKWRITE | HRM ch.8 DSKBYTR | DSKBYTR status bits |
| `df0_raw_sectors` | every track holding bootblock+header+blob: 11 sectors with good checksums, $FF, track = cyl*2+side, gap 1..11 | trackdisk MFM format; HRM ch.8 SIDE/STEP | MFM/sync/side/step bug, data corruption in disk DMA |
| `df0_bootblock` | decoded sectors 0-1: 'DOS', checksum with carry = $FFFFFFFF. This case, `df0_payload_header` and `df0_blob_crc` are `untestable` when sector 2 decodes cleanly but is not our header (no "AMTP" magic, or another build id at offset 128-191): Kickstart 2+ booted from DF1-DF3 through trackdisk and DF0 holds another disk | AmigaDOS bootblock | as above |
| `df0_payload_header` | decoded sector 2 = the header the bootblock read with trackdisk (amt_hdr), byte for byte | trackdisk = raw | raw DMA data differs from what trackdisk saw |
| `df0_blob_crc` | CRC-32 of the decoded blob = blob_crc32 (checked by the bootblock on trackdisk data) | CONTRACT section 1 | as above, over every blob track |
| `df0_head_restored` | after stepping back by the counted steps, a read of the entry cylinder gives that track number | HRM ch.8 | lost steps (head not where trackdisk thinks) |
| `df1_signature` | DF1 blocks 0 and 1759 raw-read and decoded carry matching AMTOWN01 signatures (skip otherwise: DF1 is then never written). DF1 is only touched when its drive ID is $FFFFFFFF (3.5" DD) or $AAAAAAAA (3.5" HD): a 5.25" drive ($55555555, 40 tracks) has no track 159 | CONTRACT section 11; HRM ch.8 drive ID | (skip: no test-owned disk in DF1) |
| `df1_write_dskblk` | 6032-word write DMA of track 81 (PRECOMP 140 ns, MFMPREC, FAST): one DSKBLK, words*32 us +-10% after the start | HRM ch.8 (2 us cells in FAST mode) | write DMA rate wrong (e.g. a turbo floppy setting) |
| `dskbytr_write` | DSKBYTR DISKWRITE (bit 13) seen during the write | HRM ch.8 | DISKWRITE not reported |
| `df1_write_readback` | the 11 sectors read back with good checksums and the section 11 test pattern (test id 14) | HRM ch.8 + MFM format | write DMA/MFM encoding, ADF write-back |
| `df1_head_restored` | as `df0_head_restored`, DF1 | HRM ch.8 | lost steps |
| `host_tone_aud0` ... `host_tone_aud3` | informational: 0.5 s tone between `audio_mark` start/stop (ch0 square PER 254 vol 64, ch1 sine 226/48, ch2 square 202/32, ch3 sine 170/64; 32 samples per cycle); the stop mark carries `expect_hz` (tol 2 %); expected side in the start args (0+3 left, 1+2 right); the host record has the analysis. The verdict is the runner's pseudo case `host.tone_aud0` ... `host.tone_aud3`: frequency on the expected side within 2 % of clock / (PER × 32), on every entry that captured audio | CONTRACT section 13; HRM Paula audio period; docs/schema.md 5.1 | untestable: no host audio capture. `host.tone_audN` fails: wrong period/clock, wrong channel side, or no output |
| `uaesnd_mono_keeps_right` | with a uaesnd board (claimed, stream 1 allocated), a silent one-shot set played to its end, then a silent, endlessly repeating 8-bit mono stream (35.B = 0: left and right) runs while channel 1 (right) plays a square tone (PER 254, vol 64, 32 samples: ~436 Hz PAL) for 0.5 s between `audio_mark` start/stop (tag `uaesnd_mono_paula`, side right); informational, the verdict is the pseudo case `host.tone_uaesnd_mono_paula`; skip without a uaesnd board | uaesnd specification 35.B; HRM: channels 1+2 are the right output; CONTRACT section 13 | untestable: board not claimable, RAM layout not as specified, or no host audio capture. `host.tone_uaesnd_mono_paula` fails: the mixer drops Paula's right side while the mono stream plays. A one-shot set plays to its end first: Amiberry up to 07708117f revived an ended uaesnd stream as an ownerless one-channel mixer stream, and with one of those in the mixer the right output lost Paula (mixer part fixed in #2612). The stream is allocated ($E0) and must be playing, else untestable |
