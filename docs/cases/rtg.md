# rtg (module 16, BARE, disk group "exp")

Graphics cards at register level. For every expansion board whose
manufacturer/product the module recognises (nothing is written to an unknown
board) it runs the suite for the card's chip; case ids are
`<card>_<case>` with `<card>` one of `picasso2`, `picasso4`, `piccolo`,
`sd64`, `spectrum`, `graffity`, `pixel64`, `cv64`, `cv643d`, `omnibus`,
`domino`, `merlin`, `a2410`, `zz9000`, `uaegfx`, and for boards that are only
recognised `retina`, `visiona`, `rainbow2`, `rainbow3`, `harlequin`.

Sources of expectations: IBM VGA Technical Reference (standard registers);
Cirrus Logic GD542X Technical Reference Manual and GD543X/GD5446 TRM; S3
Trio32/Trio64 and ViRGE data books; Tseng ET4000 data book; TI TMS34010
User's Guide. Where a board-level detail has no public document (which
Zorro offset carries the chip's I/O, a monitor switch, a board interrupt
enable), the module uses it only as an access path, taken from Linux
`cirrusfb` (Cirrus boards: register bases, SR7 linear address values),
NetBSD `grf_cv` (CyberVision 64) or, failing that, the Picasso96 behaviour
modelled by WinUAE/Amiberry. Such paths are marked *(board model)* below; a
failure that only involves them is weaker evidence than a chip-level one.

State: every register touched is saved and restored (standard VGA, the chip
extension registers used, the 256-entry DAC, the PEL mask, the CRTC/attribute
index and PAS bit), and the first 64 KB of VRAM (the work area of the mode,
pattern and 2D tests) is saved and written back. Not restorable: the ET4000
KEY state (left unlocked), the CyberVision 64 board control byte (write-only;
written back as 0, its power-up value). The OS does not use the cards at boot,
so the card is left as found.

Notes for all cards
- VRAM tests run after the 640x480 8-bit mode is set: in the VGA planar modes
  CPU writes through the linear aperture go through the VGA write logic (map
  mask, write mode), so plain memory semantics only hold in a packed mode.
- `vga_vint_status` / `vint_amiga` need the CRTC running, so they run after the
  mode set.
- The 68040/060 data cache: the module assumes Zorro space is not cached
  (Kickstart leaves the 040/060 data cache off without 68040.library; the probe
  `cacr` shows it). With it on, VRAM/register reads could be stale.

## Cases

### common
| case | intent / rule | a failure usually means |
|---|---|---|
| `detect` | board recognised; ctx: name, ids, addresses, sizes | (always pass) |
| `uaegfx` | untestable: the UAE RTG board has no hardware register interface (driver traps only) | — |
| `<other>_suite` | skip: recognised board without a register-level suite | — |
| `no_card` | skip: no recognised graphics card | — |
| `vram_bwl` | natural window is plain big-endian memory: long write / byte+word reads, byte writes / long read, word writes / long and odd/even byte reads, a byte write leaves neighbours, at 4 offsets in the first 64 KB | byte lanes swapped or dropped in the board's VRAM path, width-specific handler bug |
| `vram_addr_lines` | unique longs at offsets 2^k, k = 2..15, and offset 0 untouched | an address bit dropped/aliased in the first 64 KB |
| `vram_walk` | one long per 64 KB at the same in-block offset, written highest block first, read back: the first non-matching block gives the VRAM size, which must be a power of two >= the card's documented minimum; ctx `vram_kb` | VRAM smaller than the card's minimum, non-power-of-two decode, bank or segment decode broken. ctx `vram_kb` depends on the configured VRAM and on the chip's DRAM configuration register (Cirrus SR0F bit 7 is left as found, so a 2 MB GD542x can show 1024) |
| `vram_swap_<win>` | a byte-swapped window vs the natural one, lane model: CPU byte o of the window is VRAM byte o ^ mask (mask 1 = bytes swapped in words, 3 = in longs). Word and long reads and writes must follow it exactly. Single byte accesses: lane-swapped and unswapped are both accepted (boards differ, undocumented); ctx `bytes_lane_swapped` says which | the swapped aperture swaps the wrong lanes or not at all |
| `vram_alias_<win>` | a second natural-order view of VRAM shows the same bytes (Picasso IV Z3 VRAM2, CV64 upper copy) | the alias maps elsewhere |
| `vga_sr_index`, `vga_gr_index`, `vga_cr_index` | index registers 3C4 (bits 2:0), 3CE (3:0), 3D4 (4:0) read back (IBM VGA TR) | index register not readable / masked wrongly |
| `vga_seq` | SR2 (3:0), SR3 (5:0) read back | |
| `vga_gr` | GR0-3 (3:0 / 4:0), GR7 (3:0), GR8 (7:0) read back | |
| `vga_crtc` | CR0A/0B/0C/0D/0E/0F/13/14 read back (with CR11 bit 7 clear) | |
| `vga_crtc_protect` | with CR11 bit 7 set a write to CR01 is ignored (IBM VGA TR) | write protect not implemented |
| `vga_attr` | attribute controller: 3DA read resets the flip-flop, index then data at 3C0, data read at 3C1 (AR00/07/0F 6 bits, AR13/14 4 bits), index read back at 3C0 | flip-flop handling wrong |
| `vga_misc` | misc output written at 3C2 reads back at 3CC | |
| `dac_rw` | all 256 DAC entries, 6-bit values, written via 3C8/3C9 and read via 3C7/3C9 with auto-increment | RAMDAC index/increment/width bug |
| `dac_index` | 3C8 reads the write index (+1 per R,G,B triple); 3C7 reads DAC state 0 (write mode) / 3 (read mode) | |
| `dac_pelmask` | 3C6 is 8-bit R/W | |
| `mode_640x480` | after programming IBM mode-12h timings (25.175 MHz, 640x480) with 8-bit packed graphics, misc/SR1-4/CR00-18/GR0-8 read back | a register is not stored or is modified by the chip |
| `mode_draw` (`vram_draw` on ZZ9000) | a CPU-drawn 640x100 pattern reads back (CRC) | VRAM path broken in the 8-bit mode |
| `mode_screenshot` | screenshot request (host side; hash informational); the board monitor switch *(board model)* is set to the card for the shot and back afterwards | untestable when no host answers |
| `vga_vretrace` | input status 1 (3DA) bit 3 toggles within 4 Amiga frames with a 60 Hz CRTC | CRTC timing not running |
| `vga_display_enable` | 3DA bit 0 toggles | |
| `vga_vint_status` | CR11 bit 5 = 0 enables the vertical interrupt: input status 0 (3C2) bit 7 becomes 1 within a frame; CR11 bit 4 = 0 clears it (IBM VGA TR) | vertical interrupt not emulated at the chip |
| `vint_amiga` | the card's vertical interrupt reaches the Amiga at the board's level (Picasso II+ and IV: INT2; Piccolo/SD64/Spectrum: INT6; CyberVision 64: INT6 with control bit 7 = 0) with the board interrupt enable *(board model)*; >= 3 interrupts in 10 frames, each acknowledged at CR11. The CIA on that level (CIA-A for INT2, CIA-B for INT6) has its ICR masked for the duration and restored afterwards (cia_hw.h: OS mask from ciaX.resource, pending timer events re-posted), because the OS leaves CIA sources enabled. Interrupts at that level that the card does not report (3C2 bit 7) are not counted; the level is masked again and the case is untestable; three interrupts inside one Amiga frame (a periodic 60 Hz interrupt needs > 33 ms for three) mean the acknowledge did not remove the request: failure with `storm` = 1 | board interrupt routing/enable broken |

### Cirrus (picasso2, picasso4, piccolo, sd64, spectrum, graffity, pixel64)
Register base, VRAM window and the SR7 linear address follow Linux cirrusfb:
SR7[7:4] = 0x2 (Picasso II/IV, Graffity), 0x8 (Piccolo, Spectrum, Pixel64),
0xF (SD64); SR7[7:4] = 0 turns the linear aperture off (TRM). Picasso IV Z2:
the VGA I/O is at register board + 0x10000 after setting bit 1 (unmap flash)
of the board bank register at + 0 *(board model)*, restored afterwards.

| case | intent / rule | a failure usually means |
|---|---|---|
| `cl_unlock` | SR6 reads 0x0F after writing a value other than 0x12, 0x12 after writing 0x12 (TRM) | extension key not modelled |
| `cl_chip_id` | CR27 bits 7:2: 5426 0x90, 5428 0x98, 5434 0xA8 (0xA4 early), 5446 0xB8; Picasso II accepts 5426 or 5428 (II+), ctx `cr27` | wrong chip emulated for the board |
| `cl_cursor_regs` | SR10, SR11 (8 bits), SR12 bit 0, SR13 (5:0) read back | |
| `cl_gr_ext` | GR9, GRA (offset registers), GR10, GR11 read back | |
| `cl_blt_regs` | GR20-GR2E, GR32 read back within the GD542x minimum widths (width 11 bits, height 10, pitch 12, addresses 21) | |
| `cl_cr_ext` | CR19 read back | |
| `cl_mode_sr7` | SR7 = linear address | 0x01 reads back | |
| `cl_blt_rop_XX` | screen-to-screen BitBLT, 40x12 bytes, pitch 640, all 16 GR32 ROP codes (00 0, 05 S&D, 06 D, 09 S&~D, 0B ~D, 0D S, 0E 1, 50 ~S&D, 59 S^D, 6D S|D, 90 ~(S|D), 95 ~(S^D), AD S|~D, D0 ~S, D6 ~S|D, DA ~(S&D): TRM / XFree86 cirRop), compared with the CPU doing the same op on a copy; the 1-pixel border round the destination must not change | ROP table or BLT address/pitch bug; `done` 0 = GR31 busy stuck |
| `cl_blt_pattern` | 8x8 colour pattern copy (GR30 bit 6), pattern 64-byte aligned, ROP S^D; pattern row = BLT line & 7, column = x & 7 (dest x on a multiple of 8, so the column phase is the same whether it is taken from the BLT or the destination) | |
| `cl_blt_cexp` | colour expansion from display memory (GR30 bit 7), fg GR1, bg GR0 (values <= 0x0F so the 4-bit VGA meaning of GR0/GR1 does not interfere), MSB = leftmost pixel; 64 pixels per line with source pitch 8, so packed and pitched source layouts agree | |
| `cl_blt_cexp_transp` | as above with GR30 bit 3: 0 bits leave the destination | |
| `cl_blt_pat_cexp` | 8x8 monochrome pattern colour expansion (GR30 bits 7 and 6) | |
| `cl_blt_backward` | overlapping move down/right in decrement mode (GR30 bit 0, start addresses = last byte) must be a correct copy | direction handling wrong |
| `cl_blt_autostart` | 5436/5446 only: with GR31 bit 7 (autostart) set, the write of GR2A starts the BLT programmed by the other registers (ROP S|D) | autostart not modelled |

GR31 bit 7 (autostart) is read as found (the Picasso IV boot ROM leaves it set)
and cleared before any BitBLT register is written, because with it set every
GR2A write starts a BLT; it is restored at the end. Failure records of the BLT
cases carry `diff` (number of differing bytes, bitmask of differing rows of
the region) and the GR31/GR33 values as found.

### S3 (cv64 = Trio64 on CyberVision 64 Z3; cv643d = ViRGE on CyberVision 64/3D)
CV64: I/O at board + 0x2000000, VRAM at + 0x1400000 (copy at + 0x3400000),
write-only control byte at + 0x40001 (bit 3 interrupt enable, 4 monitor
switch, 5 word swap, 6 long swap, 7 interrupt level) *(NetBSD grf_cv / board
model)*. CV64/3D Z3: I/O at + 0xC000000 (bytes at port ^ 3), VRAM windows at
+ 0x4800000 (natural), + 0x4400000 (word swapped), + 0x4000000 (long swapped),
MMIO at + 0x5000000, PCI configuration at + 0xC0E0000 *(board model)*; Z2:
PCI configuration then I/O at + 0x3C0000. The 8514/A engine registers are
written as two byte accesses (low byte at the even port), so no 16-bit bus
byte order is assumed.

| case | intent / rule | a failure usually means |
|---|---|---|
| `virge_pci_id` | PCI register 0 = 0x56315333 (device 5631, vendor 5333) | |
| `s3_chip_id` | after CR38 = 0x48: CR30 = 0xE1; CR2D:2E = 0x88:0x11 (Trio64) / 0x56:0x31 (ViRGE); ctx CR2F revision | |
| `s3_cursor_regs` | CR46/47, CR48/49, CR4C/4D, CR4E/4F read back (S3 data book widths) | |
| `s3_ext_regs` | CR50 (bits 7:4,0), CR69 (3:0) read back | |

Mode set: CR31 bit 3 and CR3A bit 4 (enhanced 8-bit modes). Trio64: CR50 = 0x40
(engine width 640), CR58 = 0x13 (4 MB linear window on), CR59/5A = 0x01/0x40,
CR40 bit 0 (enhanced register access). ViRGE: CR58 = 0x13, CR59/5A = 0 (the
board decodes its own window), CR53 bit 3 (new MMIO at window + 16 MB). The
vertical interrupt reaches the bus only with CR32 bit 4 set (S3 data books),
set for `vint_amiga` together with the board enable.
| `s3_cursor_enable` | CR45 bit 0 sets and clears | |
| `vram_wordswap`, `vram_longswap` (cv64) | with control bit 5 / 6 the same window shows 16/32-bit accesses lane swapped (mask 1 / 3) relative to the natural mode | board byte-order logic |
| `s3_blt_mix_X` | Trio64 BitBLT (command 110, +X +Y, CR50 = 640 wide) with foreground mixes 7 (S), 5 (S^D), C (S&D), B (S|D), 4 (~S), 1 (0), 2 (1), 0 (~D), source = display memory (FRGD_MIX bits 6:5 = 11), PIX_CNTL = A000; CPU oracle with border | engine coordinates/mix bug |
| `s3_rectfill` | rectangle fill (command 010) with FRGD_COLOR, mix XOR | |
| `s3_line` | line draw (command 001), horizontal, vertical (Y major) and 45 degree lines with last pixel on: their pixel sets do not depend on Bresenham tie-breaking; ERR_TERM = 2*minor - major, AXSTP = 2*minor, DIASTP = 2*(minor-major) | |
| `virge_blt_rop_XX` | ViRGE S3d BitBLT (command 0, 8 bpp, +X +Y, strides 640) with MS ROP3 CC, 66, 88, EE, 33, 55 (pattern-independent ROPs), started by the RDEST_XY write; done = SUBSYS_STAT bits 13:8 = 0x30 (idle, 16 free FIFO slots; XFree86 s3virge WaitIdleEmpty). On a wrong result the identical command is issued once more and ctx `same_command_again_ok` says whether that one worked (diagnosis only) | S3d engine bug |
| `virge_blt` | skip on the Z2 board (its MMIO path is not used) | |

### ET4000 (omnibus, domino, merlin)
CR37 (memory configuration) describes the board's wiring, not a chip rule
(review M9): it is kept as found (and logged), except on a 1 MB ET4000AX board
(oMniBus, Domino) where it still reads 0 (nothing programmed it, so the CPU
reaches only 64 KB): there it is set to a 32-bit bus of 256Kx4 DRAM (bits
3,1:0 = 1,11) *(board model)* and restored. On an ET4000/W32 (told
apart by its 217A index register reading back) CR36 bit 4 (linear map) and
CR30 = 0 (address map comparator) *(board model)*. The Merlin's palette DAC is
a BT482 at I/O 0-1F (RS0/RS1 from A2/A3, RS2 from A4: write index 00, data 04,
mask 08, read index 0C) *(board model)*; `dac_index` is skipped there (a BT482
has no VGA DAC state register). The KEY state is left unlocked (not readable).

| case | intent / rule | a failure usually means |
|---|---|---|
| `et4_segment` | after the KEY (03h to 3BF, A0h to 3D8) the segment select register 3CD is 8-bit R/W | |
| `et4_cr33` | CR33 bits 1:0 (extended start address) read back | |
| (common VGA, DAC, mode, VRAM, retrace, vint cases) | | |

### A2410 (TMS34010 host interface at board + 0/2/4/6 = HSTADRL/HSTADRH/HSTDATA/HSTCTL)
HSTCTL is always written with HLT and CF as read, NMI = 0, INTIN = 0, MSGIN = 0,
INTOUT = 1 (no effect), so the TMS is neither started, stopped nor interrupted.
The TMS 2D operations need TMS code (PIXBLT/FILL are TMS instructions) and are
not covered; neither is a mode set or the board interrupt.

| case | intent / rule | a failure usually means |
|---|---|---|
| `tms_reset_halted` | nothing has released the TMS34010, so HSTCTL HLT (bit 15) must be set (TMS34010 UG, reset of a host-interface system); if it is clear the TMS runs uninitialised DRAM and the other cases are skipped (`tms_host`), so the running TMS is not disturbed | |
| `tms_host_addr` | HSTADRL/HSTADRH read back (TMS34010 UG) | |
| `tms_host_ctl` | HSTCTL LBL/INCR/INCW (bits 13:11) read back; ctx `halted` | |
| `tms_host_vram` | 32 words written through HSTDATA with INCW from bit address 0x00010000 read back through HSTDATA with INCR (prefetch after the HSTADRH write, LBL = 0) | host port prefetch/increment bug |
| `tms_host_dram` | same in program DRAM (bit address 0x01800000 + 0x7F0000), only when the TMS is halted (else skip) | |
| `tms_ioregs` | CONVSP, CONVDP, PMASK (I/O registers at 0xC0000130/140/160) written and read through the host port | |

### ZZ9000
`vram_bwl`, `vram_addr_lines`, `vram_walk` and `vram_draw` write the memory at
+$10000, which is the ARM firmware's own RAM (no documented free area): they run
only with the unsafe flag, otherwise skip (review M8).
The ZZ9000 blitter registers (fill/copy rect) are not covered: their exact
semantics (units of the pitch, base registers) are only defined by the MNT
driver/firmware sources, which were not available to check against.

| case | intent / rule | a failure usually means |
|---|---|---|
| `zz_fw_version` | firmware version register (0xC0) is not 0000/FFFF; ctx fw/hw | |
| `zz_vblank` | vblank status (0x4C bit 0) seen 0 and 1 within 4 frames | |
| (VRAM cases at board + 0x10000, 2 MB on Z2 / 4 MB on Z3) | | |

### Board guard (review H1)
`<card>_board_rejected` (skip): the card is in the expansion list, but amt_board_ok()
rejects its ConfigDev (shut up, address 0, outside the Zorro II/III windows); nothing
is accessed. `<card>_suite` (skip) also when the autoconfig size is smaller than the
card's register/VRAM layout needs (ctx `needs`), or the register board is missing.
`vint_amiga` is untestable (not fail) when no interrupt arrives on a Picasso II whose
INT2 is only inferred from its GD5428 (review M10).
