# dmacontention (module 23): the CPU in chip RAM against full DMA load

The CPU workload (`dc_loop` in `dmacontention_asm.s`, copied into the chip pool so that instruction fetches
and data accesses all go over the chip bus) is 16384 iterations of rotate/add/xor with a store to and a
load from a 1 KB chip buffer. Its result (x, y, sum) is computed on the host and fixed in the source: DMA
must be invisible to what the CPU computes. Each case sets up one DMA scenario, runs the workload, stops
the DMA, restores the hardware (DMACON, INTENA, audio volume, DSKLEN, ADKCON disk bits, CIA-B drive
control, BPLCON0, FMODE, COP1LC = GfxBase->copinit) and checks the results below.

Pass/fail never depends on timing. Measurements go in the record's `timing` object and in a perf record
per case (ops = iterations, ticks = colour clocks). `timing.hrm_*` fields hold the value the HRM DMA slot
rules give exactly; the host compares them with the measured field for cycle-exact configs only (a
non-cycle-exact core need not match, and the records must stay identical across cores). Count checks with
a tolerance (COPER per frame, audio interrupts) fail with `expect`/`got` keys `coper_irqs_within_1` /
`aud_irqs_within_3` = 1/0; the counts themselves are `timing.coper`/`coper_frames` and
`timing.aud`/`aud_expected`. Elapsed time for the audio check uses a beam clock corrected for a frame wrap
whose VERTB has not been counted yet (read at SR=$2700), with NTSC lines at their 227.5 cck average.

Disk: DF0 is read only when the machine booted through trackdisk.device (as paula does), and only after a
reference read with no other DMA running found a $4489 sync and a valid AmigaDOS sector. Otherwise (CD/HD/CLI
start, no disk, a PC/NDOS/unformatted disk) `disk` is `untestable` with the reason, and `all` runs without
its disk part (`in.disk` says why). A loaded read that then misses DSKBLK or gives a bad sector is a fail:
the same track read correctly without the load.

| case | intent | rule (manual) | a failure usually means |
|---|---|---|---|
| `baseline` | workload alone (no DMA): result | the workload's host-computed result | CPU bug in the workload itself (see cpu_basic/dispatch) |
| `blitter` | ABCD blit (minterm $96, 32 words x 128 lines, B/C modulo -64), restarted from every BLIT interrupt while the workload runs, BLTPRI=0; DMACONR sampled 4096 times into chip RAM just after the first start | workload result; every blit raises one BLIT interrupt (started = done); D = A^B^C (HRM ch.6 minterms); timing: `blits`, `samples_busy` with `hrm_samples_busy_min` = 1 (HRM: with BLTPRI=0 the blitter gives up a cycle to a CPU that waited 3, so at least one CPU access completes while BBUSY is set; timing only, never a fail: an immediate blitter or a slow CPU sees none) | bus arbitration loses or corrupts CPU or blitter accesses, BLIT interrupt lost/duplicated, blitter starves the CPU without BLTPRI |
| `blitter_nasty` | the same with BLTPRI=1, at most 16 back-to-back blits | workload result, interrupts, D; timing `samples_busy` with `hrm_samples_busy_max` = 2 x `blits` (an ABCD blit uses every cycle and BLTPRI locks the CPU out of the chip bus, but BBUSY is already set in the start-up cycles between the BLTSIZE write and the first blitter DMA cycle, whose length the HRM does not give: a fast CPU returning from the restarting BLIT handler can read DMACONR there, about once per start, so 0 is not an exact figure) | as above; in cycle-exact configs, `samples_busy` above the bound means the CPU got chip-bus cycles during a nasty all-channel blit |
| `copper` | copper list: WAIT line $30, 1200 MOVEs to COLOR00, MOVE INTREQ,#$8010 (COPER), end; restarted every frame | workload result; COPER interrupts = frames +-1 over the run (the copper restarts at COP1LC every vertical blank, HRM ch.2). timing `coper` | copper not restarted per frame, list not completed, COPER lost |
| `bitplanes_hires4` | hires, 4 bitplanes, DDF $3C-$D4, DIW $2C81-$2CC1, modulo -80 (one line repeated), pointers reset by the copper; VHPOSR sampled 4096 times into chip RAM from line $38 | workload result; COPER per frame. timing `in_fetch`/`out_fetch` (samples on lines $40-$F0 with H inside/outside $44-$D0), `lines`, `hrm_in_fetch` = 0 (4 hires planes use every memory cycle of the fetch, HRM ch.6 DMA time slots) | CPU accesses corrupted by bitplane DMA; in cycle-exact configs a non-zero `in_fetch` means the CPU got chip-bus slots that 4-plane hires DMA owns |
| `bitplanes_lores6` | lores, 6 bitplanes, DDF $38-$D0, modulo -40 | workload result; COPER per frame. timing `in_fetch`/`out_fetch` (planes 5/6 take half of the even slots in the fetch window: fewer CPU samples inside than outside) | as above |
| `bitplanes_aga8` | AGA only: lores, 8 bitplanes, FMODE=3 (`skip` on OCS/ECS) | workload result; COPER per frame. timing only (AGA fetch modes are not in the HRM) | as above |
| `audio` | 4 channels, 16-word buffers, periods 200/210/220/230, volume 0 | workload result; per channel one interrupt per 2*AUDxLEN*AUDxPER colour clocks of beam-derived elapsed time, +-3 (HRM ch.5: an interrupt at each buffer reload). timing `aud` | audio DMA/interrupt rate wrong under CPU contention, interrupts lost |
| `disk` | DF0 motor on, MFM read DMA (DSKEN first, then DSKLEN twice, 600 words, WORDSYNC $4489); read only, nothing written | workload result; exactly one DSKBLK; the first sector after the sync has format byte $FF, sector < 22 and valid AmigaDOS header and data checksums (odd/even MFM longs XORed, masked $55555555). `untestable` if not booted through trackdisk, DF0 never reports ready, holds no disk, or the reference read without load finds no valid AmigaDOS sector | disk DMA words lost or corrupted under contention, DSKBLK missing |
| `all` | blitter (BLTPRI=0) + copper (with the 1200 MOVEs) + hires 4 planes + audio + disk at once | all of the checks above together | as above, under the heaviest load |
