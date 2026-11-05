# blitter (module id 8, AMT_PH_BARE)

Area and line blits, checked against a host model of the blitter written from
the Amiga Hardware Reference Manual (HRM), chapter 6 "Blitter Hardware", and
appendix C (ECS registers). The model and the case table live in
`tools/gen_blitter.py`, which writes `disk/modules/blitter_tab.c`.

How a case runs. All memory is in the 128 KB chip pool; pointers in records
are offsets into it.
1. The guest fills the source areas and the destination window with words
   from the case's seeds. An odd seed gives xorshift32 words; an even seed
   gives the constant `seed>>16`.
2. It runs a 1x1 "priming" blit, with A and B read from a zero word.
3. It enables only `DMAEN|BLTEN`, programs the case, and starts it through
   BLTSIZE (or BLTSIZV/BLTSIZH).
4. It waits on BBUSY (reading DMACONR twice, as the HRM advises), reads
   BZERO, and turns the DMA off again.
5. It CRC-32s the destination window. The window includes 8 guard bytes on
   each side, which must stay untouched.

`tools/gen_blitter.py --dump <case>` prints the registers, the fills and the
expected window as hex, for comparing with a failing record (whose `ctx`
has the first 8 D words it got).
`--mutate aold_unmasked|fill_excl_as_incl|texture_up|sing_ignored` builds a
deliberately wrong table, to check that the guest catches those errors.

Expected results are the same on OCS, ECS and AGA, PAL and NTSC, with or
without cycle-exact and JIT. The one exception: the `big_*`/`ecs_*` cases are
`skip` on an OCS Agnus (probe `ecs_agnus` = 0), because those registers do
not exist there.

| case | intent | rule (HRM ch. 6 unless noted) | a failure usually means |
|---|---|---|---|
| `minterm_00` … `minterm_ff` (256) | each LF byte with A, B, C and D on; 4x4 words, modulos 2/4/6/2; BZERO checked | D bit = LF bit number (A<<2 \| B<<1 \| C) ("Logic functions"; ABC=$80 … NANBNC=$01) | minterm table or channel-to-minterm-input mix-up |
| `shift_asc_00` … `shift_asc_15` | ASH=n, BSH=15-n, ascending, D=AB+!AC ($CA), 5x4 words | A/B are shifted right; the bits of the previous word come in at the left; at a line start they come from the last word of the previous line ("Shifting") | barrel shifter, or the previous-word register not carried across lines |
| `shift_desc_00` … `shift_desc_15` | the same in descending mode | descending shifts left; pointers decrement; modulos are subtracted ("Descending mode") | descending shift direction or previous-word handling |
| `shift_b_only_00` … `_15` | B alone (A off), every BSH | as above, B channel only | B shifter |
| `mask_w1`, `mask_w2`, `mask_w5`, `mask_w5_zero_first`, `mask_w5_zero_last`, `mask_w1_desc`, `mask_w5_desc` | BLTAFWM/BLTALWM on 1-, 2- and 5-word lines, ascending and descending | FWM is ANDed with the first A word of each line, LWM with the last; both with a 1-word line; masking happens before the shift ("Masking") | masks applied to the wrong word, to B/C, or after the shift |
| `mask_shift_asc`, `mask_shift_desc` | masks combined with ASH=5 over 5 lines | the masked word is what shifts into the next word and (from LWM) into the next line | the shifted-in bits are taken from the unmasked word |
| `mask_extra_word_shift` | the HRM idiom: one extra word, ALWM=0, AMOD=-2 | ALWM=0 keeps the previous line's bits out of the next line's first word | as above |
| `mask_adat_rect` | A DMA off, BLTADAT=$FFFF; the masks alone shape the edges (D = A xor C) | the masks apply to the A data register whether it was fetched or preloaded (see "Unsure" below) | the masks are applied only to DMA-fetched A |
| `mod_positive`, `mod_negative_a_repeat_row`, `mod_negative_b`, `mod_negative_d_upward`, `mod_desc_positive`, `mod_desc_negative` | positive and negative modulos per channel, both directions; `mod_negative_b` uses D=A xor B (`$3C`) so B's negative modulo and DMA affect the destination CRC | the modulo is added to the pointer at the end of each line (subtracted when descending); bit 0 is ignored ("Modulos") | sign extension of the modulo, or the modulo applied per word |
| `mod_inplace_c_eq_d` | C and D are the same memory (D = A xor C in place) | each D word is written after its C word was read | read/write ordering |
| `mod_overlap_desc_move` | an overlapping move upwards in memory, descending, with a shift | descending mode exists for overlapping copies where the destination is above the source | a descending pointer or ordering error |
| `use_none` … `use_abcd` (16) | every USEA/USEB/USEC/USED combination, D = A^B^C, with BLTxDAT preloaded for the channels that are off | a disabled source uses its data register for every word; with D off nothing is written, but BZERO is still computed | preloaded data ignored, or D written when disabled |
| `zero_{d,nod}_same_src` | A and B read the same words, LF = A&!B: the result is all zero | BZERO = 1 when every D word the blit produced was zero, D enabled or not (DMACONR bit 13) | BZERO not computed without D, or from the wrong data |
| `zero_{d,nod}_shifted_src` | the same with BSH=1: not zero | as above | stale BZERO |
| `zero_{d,nod}_masked` | the only non-zero data is outside the 1-word mask | as above, with masks | masks ignored for BZERO |
| `zero_{d,nod}_lf00`, `zero_{d,nod}_lfff` | minterm $00 (zero) and $FF (not zero) | as above | |
| `zero_{d,nod}_last_word` | a single set bit in the last word of the last line | BZERO covers every word, up to the last one | the last word is not included |
| `fill_incl_fci0`, `fill_incl_fci1`, `fill_excl_fci0`, `fill_excl_fci1` | area fill of a sparse edge image (D = A&B&C), descending, FCI 0/1 | fill goes right to left, bit by bit; the fill state starts at FCI on every line and toggles at each 1 bit; inclusive keeps both edges, exclusive drops the left edge ("Area fill mode"); BZERO checked (see "Unsure") | fill direction, FCI not reloaded per line, exclusive/inclusive swapped; BZERO stale in fill mode |
| `fill_incl_shift`, `fill_excl_shift` | fill after a shifted, masked minterm | the fill acts on the minterm output; BZERO checked | fill applied before the logic |
| `fill_incl_inplace`, `fill_excl_inplace` | the polygon idiom: one edge plane filled in place (A=D) | as above | |
| `line_o1_0` … `line_o8_2` (24) | 3 slopes per octant (codes as in hardware/blit.h OCTANT1..8). The middle slope has dmin = dmax/2, so the error term reaches exactly 0 | BLTAPTL = 4dmin-2dmax, SIGN set if negative. Each pixel: SIGN clear means a minor step and error += BLTAMOD (4(dmin-dmax)); SIGN set means error += BLTBMOD (4dmin); the major step is always taken. SUD/SUL/AUL choose the axes; BLTSIZE = (dmax+1)<<6 \| 2 ("Line mode"). BZERO checked in every line case (see "Unsure") | octant decoding, sign test (< 0 vs <= 0), error update order, word-crossing of ASH; BZERO stale after a line blit |
| `line_horizontal`, `line_horizontal_left`, `line_vertical`, `line_vertical_up`, `line_diag`, `line_single_pixel`, `line_two_pixels`, `line_long` | degenerate and long lines (dmin=0, 45 degrees, 1 pixel, 320 pixels) | as above | |
| `line_sing_o1` … `line_sing_o8` | SING with the XOR minterm $4A, one line per octant | SING draws one pixel per horizontal line (the fill-able line idiom). Modelled as the first pixel on each row | SING ignored, or the wrong pixel kept |
| `line_texture_0` … `_5` | BLTBDAT patterns with different BSH start bits | the BLTBDAT texture bit decides each pixel; BSH is the start bit, 0 = least significant; the next pixel uses the next lower bit (see "Unsure") | texture ignored, or rotated the wrong way |
| `line_lf_fa`, `_0a`, `_4a`, `_2a`, `_8a`, `_ba` | line minterms of the form X<<4 \| $A, with a texture | A is the pixel bit ($8000>>ASH) and B the texture bit; outside the pixel D = C | wrong A/B roles in line mode |
| `size_1x1` | the smallest blit | | |
| `size_1x1024` | BLTSIZE height field 0 | height 0 = 1024 lines (the OCS maximum) | height 0 treated as 0 |
| `size_64x16` | width field 0, with shift and masks | width 0 = 64 words | width 0 treated as 0 |
| `size_64x1_desc` | a 64-word line, descending | | |
| `big_sizv_1100x2` | ECS: 1100 lines through BLTSIZV/BLTSIZH | ECS: BLTSIZV (15 bits) then BLTSIZH (11 bits) starts the blit (appendix C) | ECS big blit missing |
| `big_sizh_100x20`, `big_sizh_200x3_desc` | ECS: lines wider than 64 words | as above | |
| `big_small_via_sizv` | ECS: a normal-sized blit through BLTSIZV/BLTSIZH | as above | |
| `ecs_bltcon0l` | ECS: BLTCON0L replaces only the minterm byte | BLTCON0L ($05A) writes the low 8 bits of BLTCON0 (appendix C) | register missing or writes all 16 bits |
| `hot_minterm_e4`, `hot_shift_desc_07`, `hot_fill_excl_fci0`, `hot_line_o5_1` | the same fill + blit + CRC repeated under amt_hot() (250 reps full, 25 quick) | the same blit must give the same CRC and BZERO every time, and that result must be the expected one | the JIT miscompiling the CPU-side fill/CRC, or a blitter state leaking between blits |
| `nasty_off`, `nasty_on` | one 32x32 ABCD blit with DMACON BLTPRI clear and set | BLTPRI changes only bus priority, never the result ("Blitter priority") | |
| `irq_intreq_set` | INTENA BLIT off: INTREQR bit 6 is clear before the start and set once the blit is done | the blitter-finished request is set when a blit completes (chapter 7, interrupts; chapter 6) | BLIT request missing |
| `irq_level3` | INTENA INTEN\|BLIT: the level-3 hook (`amt_irq_hook[3]`) runs exactly once, with INTREQR BLIT set, and the blit is correct | BLIT is a level-3 interrupt | interrupt missing, doubled, or at the wrong level |
| `bbusy_idle_clear` | BBUSY is clear before the start (after the priming blit) | DMACONR bit 14 | stale busy |
| `bbusy_set_while_running` | a 64x64-word ABCD blit (~16 K colour clocks) is still busy at the second DMACONR read after BLTSIZE | the HRM's test-twice rule | the blit finishes instantly, or BBUSY is not set |
| `bbusy_clear_when_done` | BBUSY clears and stays clear; the polled blit is correct | | |

The `perf` record `blit_64x64_abcd` gives the colour clocks from BLTSIZE to
BBUSY clear for that blit. It is timing only and not compared.

## Unsure / deliberately not tested
- **Line texture bit order.** The HRM only says "BLTCON1 BSH = start bit for
  line texture (0 = least significant bit)". The model uses texture bit
  `(BSH - k) & 15` for the k-th pixel, i.e. the pattern rotates down one bit
  per pixel step, whether or not SING suppresses that pixel. The line
  minterms are limited to the form X<<4|$A, so the B value outside the
  pixel's bit cannot matter.
- **SING.** The HRM says "one bit per horizontal line". The model keeps the
  first pixel on each row, which matters only for x-major lines.
- **Masks on a disabled A channel.** The model ANDs the masks with BLTADAT
  whether it was fetched or preloaded (`mask_adat_rect`, `use_*` with
  $FFFF masks). The HRM masking text speaks of "the A source data" without
  saying "DMA". Evidence for the expectation: WinUAE/Amiberry `blitter.cpp`
  masks the A data whether fetched or preloaded (`blitter_doblit`: with A off
  `bltadat = blt_info.bltadat`, then `&= blit_masktable[i]`;
  `blitter_loadadat`: `&= bltafwm` / `bltalwm`). Not confirmed on real
  hardware: a real-hardware fail of `mask_adat_rect` means this rule is
  wrong, not that the hardware is.
- **The A/B previous-word registers at the start of a blit** are not
  specified. The priming blit makes them zero under both possible behaviours
  (cleared at the start, or kept from the last blit). The data registers of
  enabled channels are never written by the CPU.
- **BZERO in fill and line mode.** HRM 6: BZERO is 1 when every word the
  blit produced was zero. Fill mode: the model takes the D output after the
  fill circuit; line mode: the D words written. The HRM does not settle
  whether fill-mode BZERO is taken before or after the fill, or what a SING
  step without a dot counts as, so `tools/gen_blitter.py` also computes BZERO
  under the other readings (before the fill; with the dot on suppressed SING
  steps; with C on them) and checks BZERO only where all agree. All 60 fill
  and line cases agree (their outputs are never all zero: BZERO = 0), so all
  are checked; none tells the readings apart.
- Not tested:
  - fill in ascending mode (the HRM says fill needs descending mode);
  - IFE and EFE set together;
  - overlapping source and destination other than C=D and the descending
    move, because the result depends on the pipeline;
  - the exact cycle at which BBUSY clears relative to the interrupt and the
    last D write;
  - line mode with D pointer ≠ C pointer.
- A blit with no channels enabled (`use_none`) is assumed to run normally and
  to compute BZERO from the data registers.
