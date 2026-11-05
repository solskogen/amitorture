# misc_hw (module 19, AMT_PH_BARE) and misc_hw_os (module 24, AMT_PH_OS)

Gayle audit safety: SRAM ownership markers must agree on UUID, total,
first-free, 512-byte blocks and SRAM kind. Totals and first-free are validated
in the block domain before multiplication; writes stay in the validated
interior and both endpoint markers are checked afterwards. Card-change
success requires acknowledged reinsertion and actual restored presence;
a reinsertion failure is reported explicitly, never hidden by a good eject.

Motherboard and custom-machine hardware outside the custom chips. Disk group `exp`.
Every part runs only when its hardware is found, and the detection never writes to an address
that could be unmapped:

- RTC: a read-only decode of the 16 registers at $DC0000.
- Gayle: card.resource is present (only A600/A1200 Kickstarts have it). With a RAM-type card inserted
  at cold start, Kickstart 3.1 adds the card as fast RAM: card.resource then shows up only as the name
  of that MemHeader.
- Akiko: the machine is AGA and a read of $B80002 inside amt_try gives $CAFE.
- CDTV: the DMAC autoconfig board 514/3 is in the probe's board list.

Each part runs in its own amt_try (a crash is reported as `<part>_aborted`) and restores what it
changed.

References:
- OKI MSM6242B data sheet; Ricoh RP5C01/RF5C01A data sheet (also the Linux rtc-msm6242.c and
  rtc-rp5c01.c drivers, which use the same register maps at $DC0000).
- Gayle register notes from the Linux/NetBSD drivers.
- ATA-4 / ATA/ATAPI-6.
- PC Card Standard (CIS metaformat).
- 24C08 data sheet, I2C specification.
- libraries/configregs.h and configvars.h, the Zorro III technical specification.

## RTC (MSM6242B on A500+/A2000/CDTV, RP5C01 on A3000/A4000)

Register n is at $DC0000 + 4n + 3 (low nibble only). The time is saved at a second edge at the
start and restored at the end: saved time + elapsed time, measured with the beam counter, written
with the clock stopped and started on the exact second. The RP5C01 RAM and the mode-01 registers
are restored too. When the counters ignore writes, every write-dependent case is
`untestable`, not `fail`.
The MSM's original HOLD state is saved, then HOLD is released before
observing the initial seconds edge; a previously latched readout does not
mean its counters stopped. Restoration verification likewise releases HOLD
while observing the counters; the saved control state is reapplied afterwards.
RP5C01 dates are decoded using the sampled leap-year counter, not prior stack contents or the year digits.
Restoration arithmetic preserves that counter through February and year rollover; only the final time
comparison ignores the counter itself.
Missing-second-edge failures include `expect.seconds_edges_min=1`,
`expect.within_ms=2500` and `got.seconds_edges=0`; they remain failures,
including on the counting, rate, hold, rollover and restoration paths.
Automatic warp keeps the entire expansion disk normally paced because
Amiberry's RTC follows host time, not warped emulated time.

| case | intent | rule (reference) | a failure usually means |
|---|---|---|---|
| `rtc_present` | chip found (ctx: chip), or skip | layout + CD/MODE bit 3 (ADJ 0 on an idle MSM, TIMER EN 1 on a running RP5C01) | - |
| `rtc_digits_valid` | the running time decodes: BCD digits in range, date exists | data sheet register tables (10-year digit 0-9) | emulator returns non-BCD digits (Amiberry: Y10 = $C for 2026, it encodes year-1900) |
| `rtc_counting` | the seconds digit changes within 2.5 s | 1 Hz counting | clock stopped or not emulated |
| `rtc_rate` | 3 RTC seconds take 3 s +-5% of colour clock (timing field: ms) | 32.768 kHz time base | RTC runs on a different time base than the emulated machine (host clock while emulation runs slow/fast) |
| `rtc_ce_rw` (MSM) | CE is read/write (patterns with MASK=1) | MSM CE register | control register not emulated |
| `rtc_cf_test_clear` (MSM) | CF TEST bit is 0 | MSM CF: TEST must be 0 in normal use | chip left in test mode |
| `rtc_hold` (MSM) | HOLD=1 at 450 ms after an edge; at 1300 ms the registers are unchanged; after HOLD=0 the held carry is applied once; the next carry keeps its phase (next edge 1850-2150 ms after the first) | MSM CD HOLD | HOLD not emulated (registers keep counting) |
| `rtc_busy` (MSM) | CD BUSY reads 1 at least once in 1.2 s of continuous polling; absence fails | MSM CD BUSY (carry in progress) | missing carry/BUSY pulse; host-paced emulators must expose the sub-frame oscillator coherently to a running guest rather than swallow its entire pulse during a host frame wait |
| `rtc_ram_rw` (RP5C01) | mode 10/11 RAM (2 x 13 nibbles) keeps two patterns; restored | RP5C01 RAM blocks | RAM not emulated / mode select wrong |
| `rtc_leap_counter_rw` (RP5C01) | mode 01 reg B bits 1-0 read/write | RP5C01 leap-year counter | - |
| `rtc_mode_select` (RP5C01) | MODE bits 1-0 read back | RP5C01 MODE register | - |
| `rtc_stop` | stopped clock (MSM STOP+REST, RP5C01 TIMER EN=0 + divider reset) does not count for 1.5 s | data sheets | stop not emulated |
| `rtc_write_readback` | 95-07-14 13:37:42 w5 written while stopped reads back (RP5C01: leap counter too) | counters are read/write | the emulator ignores time writes (Amiberry: RTC = host time) |
| `rtc_start_counts` | after start the first carry gives exactly written+1 s | divider reset on start | - |
| `rtc_roll_minute` / `_hour` / `_day_month` | carries 59 s, 59:59, Apr 30 23:59:59 -> May 1 (W 6 -> 0) | counter chains | carry chain wrong |
| `rtc_roll_feb28_leap` / `rtc_roll_feb29_leap` / `rtc_roll_feb28_common` | Feb 28 -> 29 (year 24 / leap counter 0), Feb 29 -> Mar 1, Feb 28 -> Mar 1 (year 23 / counter 3) | MSM: year % 4; RP5C01: leap counter | leap-year rule wrong |
| `rtc_roll_year` | Dec 31 23:59:59 year 99 -> 00 Jan 1; RP5C01 leap counter 3 -> 0 | data sheets | - |
| `rtc_12h_readback` | 15:37:42 written in 12-hour mode reads back as 3 PM (PM bit: MSM H10 b2, RP5C01 H10 b1) | 24/12 bit (MSM CF b2 set with REST=1; RP5C01 mode 01 reg A b0) | 12-hour mode not emulated |
| `rtc_roll_12h_noon` / `rtc_roll_12h_midnight` | 11:59:59 AM -> 12:00:00 PM; 11:59:59 PM -> 12:00:00 AM next day | MSM6242B: 12-hour sequence 12,1..11 (the OKI sheet only gives H10 0-1 plus PM/AM; the register-compatible Epson RTC-72421 manual ETM17E-02, H10 register: "12-hour clock 12:00 to 11:59, a.m. and p.m."), hour digits 00 invalid. RP5C01: the Ricoh sheet (EK-086-9804, 12/24 select register) gives no hour range, so twelve o'clock as 12 or as 00 (AM 00-11 / PM 20-31, the form chip emulators use) is accepted | 12-hour mode not emulated; on the MSM, an H1 taken from the 24-hour value (Amiberry rtc.cpp) |
| `rtc_30s_adj_down` / `rtc_30s_adj_up` (MSM) | CD 30-s ADJ at :20 -> :00, at :40 -> next minute :00; ADJ self-clears | MSM CD ADJ | - |
| `rtc_restored` | after the restore the clock shows saved time + elapsed (+-1 s); untestable if writes are ignored | (safety self-check) | the machine was left with a wrong time |

## Gayle (A600/A1200)

| case | intent | rule (reference) | a failure usually means |
|---|---|---|---|
| `gayle_present` | card.resource present; ctx CCDET/WR | - | - |
| `gayle_id` | write $DE1000, then reads of bit 7, MSB first: 8 reads give exactly $D0 on the A600 Gayle (OCS/ECS machine) and $D1 on the A1200 Gayle (AGA); 4 reads after a new write give 1101; 8 more after another write give the byte again (a write restarts the sequence, also mid-way) | Gayle ID register: the high nibble $D is what Kickstart 3.1 checks (its Gayle detection reads 4 bits, `cmpi.b #$0D`); exec ReadGayle() reads 8 bits and returns the byte ($FF = no Gayle); low nibble = revision (check on real hardware: the $D0/$D1 revision values come from the usual Gayle notes and WinUAE's own comment, not from a Commodore document, and no ROM code compares them), 0 A600, 1 A1200 ("AA Gayle") | ID counter not reset by the write / wrong ID or revision for the machine |
| `gayle_int_enable_rw` | $DAA000 keeps 00 FF 55 AA 80 01 7E; restored | Gayle interrupt enable (r/w) | - |
| `gayle_irq_write1_keeps` | writing 1 to $DA9000 bits 7-2 (bits 1-0 as read) changes nothing | Gayle IRQ: write 0 clears, 1 keeps | write semantics wrong |
| `gayle_irq_write0_clears` | writing 0 clears bit 7 (IDE; allowed back while INTRQ is live) and leaves the other latched bits, written 1, so a pending card change stays for card.resource | as above | - |
| `ide_present` | skip when the task file does not hold both written values (no drive), when BSY/DRQ show a command in flight (ATA-4 7.15.6: no command block writes then), or on a CLI start (bootio NULL: scsi.device and the filesystems own the port) without the unsafe flag | - | - |
| `ide_taskfile_rw` | sector count, LBA low/mid/high read back 6-12 sets of values, no command issued; restored | ATA-4 task file | register file not latched / aliasing |
| `ide_select_head_rw` | device/head bits 3-0 read back, device bit kept; restored | ATA-4 | - |
| `ide_status_idle` | alternate status: BSY=0 DRDY=1 DF=0 DRQ=0 (a PACKET device with the 14/EB signature may show DRDY=0 until IDENTIFY PACKET DEVICE, ATA/ATAPI-4 9.1) | ATA-4 status | - |
| `ide_altstatus_eq_status` | alternate status equals status | ATA-4 | - |
| `ide_identify` | IDENTIFY DEVICE (read-only command): DRQ, 256 words (Gayle crosses the data bytes, so words arrive byte-swapped and are swapped back), word 0 bit 15 = 0, word 255 checksum when A5, status clean after; ctx: geometry words | ATA-4 8.12 | identify / PIO data path broken |
| `ide_intrq_gayle` | after the command: CS b7 = 1, IRQ b7 latched, INTREQ PORTS set (with $DAA000 b7 = 1); status read drops CS b7; writing 0 clears IRQ b7 | Gayle IDE interrupt path, INT2 | interrupt not routed / not latched |
| `ide_lba48_hob` | 48-bit devices only: HOB=1 reads the previously written byte of the 2-deep registers | ATA/ATAPI-6 | - |
| `pcmcia_*` | skip "no PCMCIA card configured" when CCDET = 0 | - | - |
| `pcmcia_cis` | the attribute memory (even bytes at $A00000) holds a tuple chain starting with CISTPL_DEVICE and ending with FF (ctx: tuple codes) | PC Card metaformat | attribute memory not mapped / wrong byte lanes |
| `pcmcia_cis_device` | CISTPL_DEVICE decodes (type, WPS, speed, size = (units+1) * 512 << 2*code) | PC Card device info | - |
| `pcmcia_owner` | common memory block 0 and block total-1 carry the CONTRACT section 11 signature (proposed kind 4 = PCMCIA SRAM); skip if unsigned or if the card is in the exec memory list | CONTRACT section 11 | size / address decoding of common memory wrong |
| `pcmcia_cis_size` | CIS size = signed medium size | - | CIS generated for a different size |
| `pcmcia_common_rw` | owned and WR=1 only: up to 64 KB from first_free written and verified with long, word and byte accesses; end signature intact | SRAM card | common memory path broken / aliasing |
| `pcmcia_status_change` | needs host request `pcmcia` {"op":"eject"/"insert"} (not in CONTRACT section 13 yet: untestable until the runner supports it): eject drops CCDET and latches IRQ b6 | Gayle card detect | - |

## Akiko (CD32)

| case | intent | rule (reference) | a failure usually means |
|---|---|---|---|
| `akiko_present` | AGA + $B80002 = $CAFE | Kickstart Akiko check | - |
| `akiko_id_long` | $B80000.L = $C0CACAFE | Akiko ID | - |
| `akiko_c2p` | 7-12 input sets (0, FF, identity, single bits, checkers, seeded random): 8 longs written, 8 read; plane b bit 31-p = bit b of pixel p, against a CPU reference | Akiko C2P | C2P order/bit mapping wrong, write/read pointer not reset |
| `akiko_c2p_hot` | the same conversion repeated amt_hot_reps() times is stable | (JIT self-check) | JIT caching a hardware read |
| `akiko_i2c_idle` | with SCL/SDA released the bus reads high | I2C | direction/data register wrong |
| `akiko_nvram_ack` | random read (A0, address 0, repeated START, A1) is acknowledged at all three bytes, twice | 24C08 | EEPROM not attached / bit-banging timing |
| `akiko_nvram_read_stable` | the two reads of bytes 0-15 are equal (the data is never printed) | EEPROM | - |
| `akiko_i2c_nack_absent` | address 91h is not acknowledged | I2C | bus answers for every address |

NVRAM is never written: no data byte is ever sent after an address, so no write cycle can start.
SCL is driven; SDA is open drain (direction input = 1). Each step writes data, then direction, then
data again. The second data write is a no-op on the chip, and it makes the step work on emulators that
update the lines only on data writes (Amiberry akiko.cpp).

## CDTV

| case | intent | rule (reference) | a failure usually means |
|---|---|---|---|
| `cdtv_present` | DMAC board 514/3 present (ctx: address) | - | - |
| `cdtv_dmac_size` | the DMAC board is 64 KB | autoconfig | - |
| `cdtv_bookmark_stable` | 2 KB at $DC8000 read twice, equal (read only) | battery-backed RAM | - |
| `cdtv_front_panel` | skip: the front panel and IR are reached through CD controller commands, not registers | - | - |

## Other

| case | intent |
|---|---|
| `ciaa_ovl_rom_overlay` | skip: OVL is never set (it would map the ROM over chip RAM under the test) |

## misc_hw_os (module 24): Zorro autoconfig consistency

Read only, through FindConfigDev and exec's MemList. One case per rule over all boards. A failing
case lists the offending boards (man, prod, type, flags, cd_flags, addr, size, phys).

| case | intent | rule (reference) | a failure usually means |
|---|---|---|---|
| `zorro_boards` | diagnostic list of boards and memory headers (ctx); skip with no boards | - | - |
| `zorro_type` | er_Type bits 7-6 = 11 (Z2) or 10 (Z3) | configregs.h ERT_TYPEMASK | bad autoconfig ROM data; on UAE-family emulators the boot ROM's synthetic ConfigDev (2011/4, er_Type $01, filesys.asm AddConfigDev) |
| `zorro_flags` | Z2: er_Flags bits 5-0 = 0; Z3: bit 4 = 1 | configregs.h ERFF_EXTENDED/ERFF_ZORRO_III | emulated board ROM inconsistent with its type |
| `zorro_reserved03` | er_Reserved03 = 0 | configregs.h | - |
| `zorro_manufacturer` | manufacturer not 0/$FFFF | autoconfig | ROM reads all-0/all-1 |
| `zorro_size` | cd_BoardSize = size code (Z3 EXTENDED table), or the Z3 sub-size; a Z3 board below 16 MB (Zorro II size table) may also be recorded as 16 MB, expansion.library's minimum Z3 slot (KS 40.68: Piccolo/Spectrum/Retina Z3) | configregs.h, Zorro III spec | size code wrong |
| `zorro_address_range` | Z2 in $200000-$9FFFFF / $E90000-$EFFFFF; Z3 in $10000000-$7FFFFFFF (or a Z2 space) | E_MEMORYBASE, E_EXPANSIONBASE, EZ3_CONFIGAREA | - |
| `zorro_alignment` | address multiple of the physical size, counted from $200000 in the Z2 8 MB space (expansion.library places a 4 MB board at $200000) | autoconfig decoding | board placed where it cannot decode |
| `zorro_no_overlap` | configured boards do not overlap | - | - |
| `zorro_mem_overlap` | every MemHeader region [mh_Lower, mh_Upper) lies inside one MEMLIST board or outside all boards (AROS keeps MEMF_KICK headers for the ROM) | - | memory added over an I/O board |
| `zorro_memlist` | every configured MEMLIST board (not BADMEMORY) has a MemHeader inside it (skip on AROS, detected by its aros.library resident: its Z3 RAM is not in the MemList when the bootblock runs) | ERTF_MEMLIST | the board's RAM failed the memory test |
| `zorro_diag_vector` | DIAGVALID: er_InitDiagVec < board size | configregs.h DiagArea | - |

### Test-owned PCMCIA SRAM card (for the runner)

A card the OS treats as RAM is never written: Kickstart 3.1 adds an SRAM card without a format
tuple as fast RAM, so it can't be test-owned. What works in Amiberry (a reference generator is in
`work/misc_hw/scripts/mksram4.py`, not part of the repo):

- The image is 4 MB of common memory plus 4 KB of attribute memory. Amiberry reads the bytes after
  4 MB as byte-addressed attribute memory.
- The CIS is on even bytes: CISTPL_DEVICE (SRAM, 100 ns, 4 MB), CISTPL_FORMAT (disk), CISTPL_FUNCID
  (memory), END.
- The section 11 signature is in common-memory blocks 0 and 8191, kind 4, first_free 2.
- Attach it with:
  `-s pcmciasram_rom_file=:ENABLED -s pcmciasram_rom_options=inserted=true -s hardfile2=rw,CC0:<img>,0,0,0,512,0,,custom0_pcmciasram`.
