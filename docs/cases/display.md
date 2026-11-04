# display (module 12, AMT_PH_BARE, disk "chip")

Denise/Lisa: bitplane modes, fetch and window edges, scrolling, modulos, sprites, collisions,
IDs, AGA FMODE/palette/sprite extensions. References: Amiga Hardware Reference
Manual (HRM) chapters 3 (playfields), 4 (sprites), 6 (DMA time slots), 7 (collisions, beam
counters), appendix C (ECS: DIWHIGH, DENISEID, BPLCON3); AGA register documentation (FMODE,
BPLCON1 fine scroll, BPLCON2 RDRAM, BPLCON3 LOCT/BANK/SPRES, BPLCON4, CLXCON2, LISAID).

## How it observes the display
The collision hardware is the oracle. A *probe* is a one-line DMA sprite (sprites 0/2/4/6; the odd
sprites point to an empty list unless a case needs them) over the playfield. CLXCON enables one
bitplane with match value 1, so CLXDAT bit 1+n (odd plane) or 5+n (even plane) says whether any
playfield pixel under sprite 2n has that plane bit set. All reads are timing-independent: scenes
are static from frame to frame; the CPU changes CLXCON (and sprite lists) at a vertical blank,
throws away CLXDAT at the next three (a core may deliver a frame's collisions late) and reads it
two frames later, so the value is the OR of two complete, fully set-up frames. A late CPU only adds collisions of an identical frame. Each frame
measures one group of up to 4 probes for one bitplane; enabling one plane at a time makes the
read-out independent of how the odd/even groups combine (see `clx_spf_odd_independent`). Scenes
are set up by a copper list (registers, BPLxPT, SPRxPT, palette) that the copper reloads every
frame. Frame length, LOF and the H counter range are checked by timing (`lines_per_frame_*`,
`cck_per_line`), not here.

Playfield patterns: *cells* are 16 lores pixels wide (1/2/4 words in lores/hires/shres), one row
repeated with modulo -rowbytes. *Address-encoded* (AE) planes have plane p word w = $FFFF if bit p
of w is set: the index under a probe tells which memory word is displayed there, which turns fetch
widths and modulos into exact numbers. Edges are measured with 1-pixel probes and compared
relative to a reference scan, so the sprite/playfield alignment (reported as `ctx.origin`) is not
assumed.

`timing` objects (frame counts, CPU read counts, sampled H maximum) differ between runs and emulators;
pass/fail uses only CPU-observable state. Screenshots (CONTRACT section 13) are requested for some
mode patterns; `shot` = acked / not_acked / no_host is informational (only the `shot_*` cases depend
on it: pass when acked, `untestable` otherwise). After the first unacknowledged request no more are
sent.

| case | intent | rule (reference) | a failure usually means |
|---|---|---|---|
| align_lores / align_hires / align_shres | 1-pixel probe scan over a 0->1 edge at playfield x=160, standard DDF/DIW; records the sprite H of playfield x=0 (`ctx.origin`) | collisions follow displayed pixels; one monotonic step (HRM 7, 3) | no/broken edge: collisions or fetch broken; origins differ between modes: DDF/scroll pipeline offset per resolution |
| mode_lores_1bpl .. mode_lores_8bpl | 20 cells with random colours read back through collisions; 7/8 planes AGA only (5 and 6=EHB and 8 also screenshotted) | BPLxPT n feeds plane n; standard DDF fetches 20 words (HRM 3) | wrong plane order/count, plane data dropped, BPU3 (AGA) ignored |
| mode_hires_1bpl .. mode_hires_4bpl | same, hires (40 words, 2 words per cell) | HRM 3 hires fetch | hires fetch/serialisation wrong |
| mode_shres_1bpl / mode_shres_2bpl | same, superhires (80 words); needs ECS Agnus and ECS Denise | HRM appendix C SHRES | shres fetch or shres collisions wrong |
| mode_ham6_raw / mode_dpf_raw / mode_ham8_raw | raw plane data seen by collisions in HAM6, dual playfield, HAM8 (screenshots carry the colours) | HAM/DPF change colour lookup only (HRM 3) | mode bit changes fetched data |
| clx_spf_odd_independent | single playfield, CLXCON planes 1+2 match 1,1, sprite over colour 1 (odd matches, even not) and colour 2; same in DPF | DPF strict (HRM 7). Single playfield: the HRM does not say whether the odd bit needs the even match; both accepted, observed variant in `ctx.undef_coupling` (check on real hardware) | DPF: odd/even groups mixed up; SPF: even bit missing |
| clx_pf1_pf2 | bit 0 over a frame for 32 random CLXCON/CLXCON2 values (8 in quick) | bit 0 = some pixel matches on enabled odd AND enabled even planes; group with no enabled plane matches (HRM 7) | PF-PF collision wrong, CLXCON2 (AGA) ignored |
| clx_spr_pf_dpf / clx_spr_pf_spf | per clean frame a random CLXCON/CLXCON2 and 4 sprites on random cells, 48 combinations (12 quick); DPF and single playfield | bits 1-8 per HRM 7 match rule; bits 9-14 where probes share a cell; _spf accepts the independent or the coupled odd-bit model if one fits every combination (`ctx.undef_coupling`) | sprite-playfield match rule wrong |
| clx_spr_spr | 8 sprite layouts (3 quick) x 16 ENSP combinations, whole-frame CLXDAT | bits 9-14 = overlaps of sprite groups, odd sprite joins with ENSP (HRM 7) | ENSP ignored, wrong bit mapping |
| clx_read_clears | read CLXDAT twice back to back in the vertical blank after a collision frame | read clears (HRM 7) | latch not cleared on read |
| clx_aga_clxcon2_reset | CLXCON2 written, then CLXCON | a CLXCON write resets CLXCON2 (AGA docs) | CLXCON2 not cleared |
| clx_attached_ensp | attached odd sprite with/without ENSP1 | ATTACH does not change collision rules (HRM 4/7) | attach mode alters collisions |
| spr_manual_mode | sprite DMA off; SPRxDATA write arms, SPRxCTL write disarms | HRM 4 manual mode | arming logic wrong, manual sprites not displayed (Amiberry loses them unless cycle_exact=true: an emulator bug, finding V5, the only case expected to differ between its cores) |
| spr_vstart_vstop | tall sprites against the vertical window | shown on VSTART..VSTOP-1 (HRM 4) | VSTOP inclusive / off by one |
| diw_hstart / diw_hstop | window start/stop moved by 1/4/16/64 pixels; first/last visible pixel must move one for one | HRM 3 display window (HSTOP bit 8 implied 1) | window edge resolution/offset wrong |
| diwhigh_hstop_h8 | ECS: DIWHIGH with HSTOP H8=0, stop $F1 | HRM appendix C DIWHIGH | DIWHIGH ignored |
| diw_vstart_vstop | first/last lines inside/outside the window; VSTOP $5B means line $15B | V8 of VSTOP = NOT V7 (HRM 3) | vertical window off by one, V8 rule missing |
| ddf_lores_* / ddf_hires_* | AE planes, rows 0,1,3,4..: words per line from (DDFSTOP-DDFSTRT) | lores (STOP-STRT)/8+1, hires 2*((STOP-STRT)/8+1) words (HRM 3) | fetch count/start wrong |
| modulo_lores_odd_even / modulo_lores_negative / modulo_hires_odd_even | AE planes with BPL1MOD != BPL2MOD, negative | odd planes add BPL1MOD, even BPL2MOD (HRM 3) | modulo applied to the wrong planes/sign |
| scroll_pf1_lores / scroll_pf2_lores / scroll_pf1_hires / scroll_pf2_hires | edge position for delays 0-15 (hires 0-7), other playfield given the opposite delay | delay in lores pixels per playfield (HRM 3) | scroll applied to wrong planes, wrong unit |
| scroll_aga_fine | AGA BPLCON1 PF1H1-0 + PF1H5-2 (values in shres steps), probes placed with SH1/SH0 | AGA docs: 35 ns scroll steps | fine scroll bits ignored or truncated |
| spr_aga_shres_position | shres-step and lores-step edge scans agree | AGA SPRxCTL SH1/SH0 | sprite fine position ignored |
| fmode_1_lores / fmode_2_lores / fmode_3_lores | AGA 8-plane AE playfield; the pixel origin is first measured in that FMODE (edge between words 0 and 1 of row 0, `ctx.origin_fmode`); then row 0 words in order, row stride W a whole number of fetch units (W in ctx) | AGA docs FMODE | wide fetch reorders/duplicates words |
| spr_aga_width_16/32/32_spagem/64 | sprite over 5 cells of distinct planes | FMODE SPR32/SPAGEM width 16/32/64 (AGA docs) | wide sprite format/width wrong |
| spr_aga_spres_0..3 | sprite pixel width from BPLCON3 SPRES | AGA docs BPLCON3 | sprite resolution ignored |
| pal_aga_rdram_24bit | 256 colours written high/low nibble per bank, read back with RDRAM | AGA docs BPLCON2 RDRAM, BPLCON3 LOCT/BANK | palette banks/LOCT wrong |
| pal_aga_loct_copy | LOCT=0 write copies nibbles into the low half | AGA docs | 12-bit compatibility write wrong |
| deniseid | 16 reads, checked against the Agnus ID read from VPOSR (not the probe): Alice => $F8 and stable; no Alice => not $F8; $FC must be stable; bits 15-8 ignored | HRM appendix C, AGA docs | wrong/unstable ID, Lisa without Alice or vice versa |
| dma_cpu_* | timing only: CPU chip reads in 64 lines per mode | HRM 6 DMA slots | (informational) |
| shot_dpf_priorities / shot_ecs_border / shot_aga_bplcon4 | screenshot-only patterns: BPLCON2 priorities with sprites; BRDRBLNK/BRDSPRT; BPLCON4 XOR and sprite banks | pass = host acked | untestable without a host |
