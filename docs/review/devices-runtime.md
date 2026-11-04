# Review: runtime, boot, device modules, host-side safety

Independent read-only review. Scope:
- Runtime and boot: disk/boot/bootblock.s, disk/rt/*, disk/probe/*, tools/mkadf.py.
- Device modules: devio, rtg*, expsound*, network*, misc_hw* (with misc_hw_os), takeover.c, rt.c.
- Host side: media.py, isolation.py, emulator.py (environment), the ROM-dir and home handling in runner.py and amiberry_cfg.py, and tools/amislot.py.

Nothing was run: no emulator and no build. Line numbers refer to the tree as reviewed.

Severity:
- **high**: could damage a real machine or user data, or gives a wrong verdict.
- **medium**: a real risk under plausible conditions, or a likely false fail/pass.
- **low**: hardening, a cosmetic issue, or an unlikely case.

Counts: **2 high, 14 medium, 35 low.**

---------------------------------------------------------------------------
## HIGH

### H1. Board bases come from unverified ConfigDev entries; shut-up and unconfigured boards get written at address 0
- **Where:**
  - disk/probe/probe.c:732-749 (`probe_boards`) and probe.h:52-57;
  - rtg.c:958-1030 (`board_bus`, `add_card`, `find_cards`) and `setup_card` at :1033-1102;
  - expsound.c:1315-1350;
  - network.c:979-989.
- **What:**
  - `amt_probe_board` keeps only man/prod/addr/size. `cd_Flags` is dropped, so CDF_SHUTUP and CDF_CONFIGME boards look like configured ones.
  - expansion.library keeps a shut-up board on the list with `cd_BoardAddr` = 0. misc_hw_os.c:22 says so itself ("Shut-up boards have no address").
  - None of the three modules checks addr != 0, the Z2/Z3 window, the size, or the size against the largest offset the layout uses.
  - `rtg.c board_bus()` classifies addr 0 as Z2, so a shut-up Picasso II, Piccolo or similar still matches its card definition.
- **What then gets written into low chip RAM, unrestored** (the vector table when VBR = 0; payload, pools, mailbox):
  - VGA ports at $3B4-$3DF;
  - Picasso II `regs[$9000/$9001/$A000/$B000]`;
  - A2065 init block and rings at $4000-$EFFF;
  - Toccata FIFO bytes at $2000 and the status register write at $0000;
  - the uaesnd RAM walk from $8000.
- **Also unchecked:**
  - Too-small boards. Examples: CV64/3D Z3 uses offsets up to $C0E0008; Picasso IV Z2 regs need 128 KB.
  - A board still in config space at $E80000: writes there can configure or shut up the pending board.
- **When it happens:** Z2 space runs out, or there is a Z3 board under KS1.x.
- **Rule:** CONTRACT §10 rule 4; the Autoconfig spec (cd_BoardAddr is valid only for configured boards).
- **Fix:**
  1. Record `cd_Flags` in `amt_probe_board`, and in the hello JSON.
  2. Add one shared `amt_board_ok(b, span)` that requires:
     - no SHUTUP/CONFIGME flag;
     - addr inside $200000-$9FFFFF, $E90000-$EFFFFF, or >= $10000000 for Z3;
     - `(addr & (size-1)) == 0` (Z2 8 MB space counted from $200000);
     - size >= the per-layout span;
     - no wrap;
     - no overlap with a MemHeader.
  3. Report a skip with the reason when a board fails the check.

### H2. CLI/CD start: the takeover can cut into DOS I/O in flight; IDE tests can hijack a live command
- **Where:**
  - mod.c:458-461 and rt/takeover.c:193-197 (DMACON = $7FFF);
  - misc_hw_gayle.c:342-369 (`t_ide`) and :160-301 (task-file writes, IDENTIFY);
  - misc_hw_gayle.c:121-133, 230, 267 (Gayle IRQ latch cleared).
- **What:**
  - From the bootblock nothing is mounted yet. Through `amt_cli_entry` (CONTRACT §14) DOS and the filesystems are live. The executable is also the natural way to run the suite from a real machine's hard disk.
  - Forbid()/Disable() does not stop hardware already in progress:
    - **Floppy:** a trackdisk write DMA in progress is frozen half way by DMACON DSKEN = 0. DSKLEN is still armed, so it resumes minutes later at a random rotational position once `takeover_restore` re-enables DSKEN. The track on the user's floppy is corrupted.
    - **Gayle IDE:** `t_ide` checks only BSY (`ide_wait_notbusy`), not DRQ. With a PIO command waiting for data (DRQ = 1), it overwrites the command block registers and issues IDENTIFY. ATA-4 §6/§9 forbids command block writes while BSY or DRQ is set. It then clears the Gayle IDE IRQ latch, so the interrupted scsi.device request either hangs or completes against the wrong command. A write in flight can leave a partly written sector.
- **Rule:** never harm user data (MODULE_AGENT_BRIEF "Safe on real hardware").
- **Fix:**
  1. Before `takeover_take`, quiesce:
     - wait (bounded) for DSKBYTR bit 14 DMAON = 0 and DMACONR BBUSY = 0;
     - on Gayle/A4000 IDE, wait for alt status BSY = 0 and DRQ = 0.
     Refuse the takeover (skip all BARE modules with a reason) if they stay busy.
  2. In CLI mode (`amt_bootio == NULL`), also Inhibit() the DOS devices or Delay() a few seconds after the OS phase.
  3. Gate IDE IDENTIFY and the task-file writes on BSY = 0 and DRQ = 0, and in CLI mode on `AMT_F_UNSAFE`.
  4. As a safety net, write DSKLEN = $4000 in `takeover_take`.

---------------------------------------------------------------------------
## MEDIUM

### M1. No runtime safety net after a module abort, nor at the final restore
- **Where:** mod.c:324-340 (`bare_reset_env`), mod.c:396-408, rt/takeover.c:199-214.
- **What:**
  - When a module aborts (exception or watchdog), its own restore code never runs. The runtime then resets only SR, INTENA, INTREQ, DMACON and the vectors.
  - **CPU state, never saved or restored:**
    - CACR (written by smc_020, memory_020, dispatch_040, dmacontention_040, mmu_asm);
    - MMU TC/CRP/TT0/TT1 (030) and TC/URP/SRP/ITT/DTT (040/060);
    - the 060 PCR;
    - FPCR and FPSR.
  - **Hardware state, never saved or restored:**
    - COP1LC/COPCON. Copper and display restore COP1LC only on their normal path, and copper.c:951 sets CDANG.
    - DSKLEN;
    - BEAMCON0/FMODE/BPLCON3;
    - CIA ICR masks and timer control.
  - **Result:** the next module, and then the OS after `takeover_restore`, can run:
    - with an MMU table from the pool (wrong aliasing);
    - with a stale copper list that may still have CDANG set;
    - with an armed disk DMA;
    - with a VARBEAM sync on a fixed-frequency monitor.
  - CACR left changed also breaks determinism between modules.
- **Rule:** CONTRACT §10 ("the runtime resets … after each module"); "leave the hardware as you found it".
- **Fix:** save these registers once at takeover (CPU family from the probe) and restore them in `bare_reset_env` after any abort, and in `bare_super` before `takeover_restore`:
  - CACR, the MMU registers and PCR;
  - FPCR = FPSR = 0;
  - COPCON = 0 and COP1LC = GfxBase->copinit (the display_hw.h:90 helper, moved into rt);
  - DSKLEN = $4000;
  - BEAMCON0/FMODE as found (from GfxBase on ECS/AGA);
  - the CIA ICR mask and CRA/CRB.

### M2. A test-owned RDB disk the user partitions is still "owned"; devio overwrites the new partition
- **Where:** devio.c:983-1110; media.py:199-252.
- **What:**
  - The RDB medium looks like a valid, empty, unpartitioned disk: RDSK at block 1, no PART.
  - HDToolBox or another partitioner keeps the RDB in block 1 and adds partitions from cylinder 2 (block 2016) up. That is exactly the writable region `[first_free, total-1)`.
  - Blocks 0 and total-1 are untouched, so the next devio run overwrites the user's new partition.
  - The plain HDF and the ADF are safe: formatting them rewrites block 0.
- **Fix:**
  - For kind 3, re-read the RDSK at block 1 before writing and require: valid checksum, `rdb_PartitionList` = $FFFFFFFF, or only the media.py `AMTTEST` NOMOUNT/DosType 0 PART; FileSysHeaderList = $FFFFFFFF.
  - Otherwise report "owner" FAIL with "medium was repartitioned" and do not write.
  - Optionally put an `AMTOWN01` marker in the RDSK `rdb_DiskVendor`/`DiskProduct` fields (already "AMITORTU") and check it.

### M3. devio does not validate the signature fields that bound its writes
- **Where:** devio.c:424-434 (`region_alloc`), 985-1014, 1100-1102, 682/700/805 (`lba * BS`).
- **What:**
  - The last block's `bsize`, `first_free` and `kind` are never compared with block 0.
  - `first_free < total-1` is never checked. With `first >= end`, `u->end - u->first` wraps to a huge ULONG. `region_alloc` then hands out LBAs from `first`, i.e. *beyond* the signed last block. When the device is larger than `total` (legal, since geometry is a separate case), those blocks are unowned.
  - `total > $7FFFFF` makes `lba*512` overflow the 32-bit io_Offset. Today that only causes a misleading owner FAIL, because the last-block read hits the wrong block.
- **Rule:** CONTRACT §11 (writable region `[max(first_free,2), total-1)`).
- **Fix:**
  - Require all of these: `s0.first < total-1`; `sl.bsize == 512`; `sl.first == s0.first`; `sl.kind == s0.kind`; `total <= geom_total` when known; `total <= $7FFFFF`. The last limit lasts until TD64/NSD is used.
  - Make `region_alloc` use signed or explicit bound checks.
  - media.py: refuse `size_mb` >= 4096.

### M4. network: a real ARP broadcast goes out unconditionally (from the network/expsound reviewer, checked)
- **Where:** network.c:671-720 (A2065), 861-901 (ZZ9000); header at :32.
- **What:**
  - The packet is a broadcast ARP request with sender 10.0.2.15 and MAC 02:00:5a:a2:06:05, sent with no `AMT_F_UNSAFE` gate.
  - On a real LAN, the RFC 826 merge rule makes every host with 10.0.2.15 cached rewrite that entry to our MAC.
- **Fix:** gate it behind `AMT_F_UNSAFE` (or a "net" flag), otherwise report untestable. Alternatively send an RFC 5227 probe (sender IP 0.0.0.0).

### M5. network: any ARP reply counts as ours (false fail on a live LAN)
- **Where:** network.c:714 and :895.
- **Fix:** match sender 10.0.2.2 and target 10.0.2.15 before deciding; skip other frames.

### M6. expsound: the Toccata/Prelude glue is modelled on Amiberry's sndboard.cpp (rule 1)
- **Where:** expsound.c:15-22, 527-569, 772-816, 1317-1345.
- **What:**
  - Several things come from the emulator, not a hardware document: register offsets, status bits $80/$08, reset strobes, FIFO-flag polarity, and the interrupt-count model (N-1024)/512+1.
  - So the cases confirm Amiberry against itself, and give false fails on real boards.
- **Fix:**
  - Cross-check against NetBSD `toccata.c` and the AHI drivers.
  - Until then, mark these fields in ctx as emulator-derived. The codec register cases themselves follow the data sheet and are fine.

### M7. expsound: AD1848/CS4231 index test reads R0 right after MCE 1→0 without waiting for INIT/ACI (false fail on real codecs)
- **Where:** expsound.c:139-155 (`t_index`).
- **Fix:** call `cmce_off()`, or the INIT wait, before the TRD group.

### M8. rtg: the ZZ9000 VRAM tests write into a running coprocessor's RAM (from the RTG reviewer)
- **Where:** rtg_misc.c:253-296, rtg.c:678-696 and :1093-1096.
- **What:**
  - The window at `regs+$10000` is ARM-side RAM in use by the firmware.
  - The writes and the restore are temporary, but they race with firmware updates.
- **Fix:** limit the writes to the documented framebuffer, or put these cases behind `AMT_F_UNSAFE`.

### M9. rtg: ET4000 CR37 is forced to "32-bit, 256Kx4, 1 MB"
- **Where:** rtg_misc.c:56-59, with the save at :75.
- **What:**
  - The value is an assumption about board wiring, not a chip rule. The Merlin then sees only 1 MB: a false fail on hardware, while the emulator passes.
  - VRAM is saved after the mapping change, so the restore can land in different cells.
- **Fix:** do not write CR37, or save VRAM before writing it.

### M10. rtg: every GD5428 Picasso II is assumed to be a II+ with INT2
- **Where:** rtg_cirrus.c:344-345.
- **Fix:** report `vint_amiga` untestable when no interrupt arrives on a card whose interrupt support is only inferred. Do not write regs $9000/$9001 there.

### M11. Bare-phase cache state is not defined (68040/060 I/O through the data cache)
- **Where:** runtime as a whole (CACR never set); this hits rtg (dac_rw, vga_attr, vga_vretrace, cl_wait, s3_idle) and all register-level device tests.
- **What:**
  - With the 040/060 data cache on and no TT/MMU cache-inhibit for Zorro space, register reads become line fills.
  - Those fills have side effects: they advance the DAC index and reset the attribute flip-flop.
  - This also makes results depend on the boot CACR, which is a determinism problem.
- **Fix:** the runtime sets a defined CACR for the bare phase (data cache off, or DTT0 cache-inhibit for $00000000-$7FFFFFFF I/O) and restores it (see M1). Modules that test caches set them explicitly.

### M12. rtg: board state left changed (from the RTG reviewer)
- **Where:**
  - rtg_s3.c:415-417: the ViRGE PCI command register is not written back.
  - rtg_s3.c:538-539: CV64/3D Z2 `+$3A0008` is set to 0.
  - rtg.c:870-911: monitor switch and IRQ enable are forced off.
  - rtg_cirrus.c:293/427: the Picasso IV Z2 bank is read from `regs[0]`, which may be flash on real hardware.
  - The 8514/S3d engine state is not restored.
- **Fix:** save and restore what can be read; document the rest.

### M13. RTC 12-hour mode: noon and midnight are encoded as "12"; the chip data sheets need checking (likely false fail on RP5C01)
- **Where:** misc_hw_rtc.c:119-137 (`encode`), :156-161 (`decode` requires 1..12), :929-933 (`rtc_roll_12h_noon` and `_midnight`); docs/cases/misc_hw.md:54.
- **What:**
  - To my knowledge the Ricoh RP5C01/RP5C15 count 12-hour time as AM 00-11 and PM 20-31: H10 bit 1 is the PM flag, and 12 o'clock is hour 0.
  - The MSM6242B table must be checked too.
  - The test encodes 12 o'clock as H10 = 1, H1 = 2, and `decode()` rejects hour 0. If the data sheet says 0-11, `rtc_12h_readback`, the noon/midnight rolls and the restored-time decode in 12-hour mode all fail on a real A3000/A4000.
  - Separately, Amiberry's MSM 12-hour H1 (`rtc.cpp:34`: `tm_hour % 10` from the 24-hour value) looks like a real emulator bug that the test would rightly catch.
- **Rule:** CONTRACT §10 rule 1 (expectations from the data sheet; accept every allowed result if unclear).
- **Fix:** quote the data sheets in docs/cases/misc_hw.md. If either chip uses 0-11, encode and expect 0 for 12 o'clock on that chip. Until it is checked, accept both forms.

### M14. amislot does not make "at most nproc emulators" airtight
- **Where:** tools/amislot.py:146-147, 169-174, 183-200; emulator.py:44-80; emulator.py:266-289.
- **What:**
  - The lock dir is per checkout (`ROOT/work/locks`). Every git worktree, which agent tooling uses, gets its own nproc slots. Only the /proc count is shared, and it is checked before the flock, so two launchers can both pass it with orphans present (TOCTOU).
  - `AMT_MAX_EMU` raises the limit with no cap.
  - `os.cpu_count()` is not `nproc` under affinity or cgroups.
  - The vendored fallback in emulator.py has no /proc check at all.
  - `Process.stop()` returns "could not stop" and the slot is released while the process may still be alive.
  - Processes not named exactly `amiberry` (for example a renamed binary) are not counted.
- **Fix:**
  - Use a machine-wide lock dir (`$XDG_RUNTIME_DIR/amitorture-locks`, or `/var/tmp/amitorture-locks`).
  - Take the flock first, then re-check the /proc count while holding it.
  - Cap at `len(os.sched_getaffinity(0))`.
  - Keep the slot while the child is alive.
  - Match on the exe path as well as on comm.

---------------------------------------------------------------------------
## LOW

**Boot and runtime**
- **L1.** bootblock.s:95-112, 156-179: header fields are not bounds-checked, and the header is not CRC-protected. A corrupt `blob_sectors > alloc_size` makes trackdisk DMA past the allocation; reloc offsets are not range-checked. There is room in the 1012 bytes: check `blob_sectors*512 <= alloc_size`, `image+bss <= alloc_size`, `image + 4*nrel <= blob_sectors*512`, and `r < image_size-3`.
- **L2.** bootblock.s:187-191: there is no cache flush on exec < 37 with a 68020+/040 accelerator (stale I-cache, 040 copyback). Use `Supervisor(CPUSHA/CINVA)` when AttnFlags shows AFF_68040, or `move cacr` with CI on 020/030.
- **L3.** rt/takeover.c:187: VBR is read only when AttnFlags has AFF_68010, while the probe uses MOVEC independently. When the two disagree (a VBR-moving board ROM with an exec that does not flag the CPU), the runtime installs vectors at 0 while the CPU uses another table. Use `amt_probe_result.vbr`.
- **L4.** mod.c:106-129: `amt_ticks()` is only meaningful in the bare phase, because no frame counter runs in the OS phase. The static `last` from OS-phase calls clamps the first bare-phase values. Reset `last` at takeover and document it as bare-only.
- **L5.** bare.s stub 24 and mod.c:332-337: a spurious interrupt (vector 24) aborts the module as "unexpected exception". Real boards can raise spurious IACKs. Count spurious interrupts and RTE instead.
- **L6.** No guard on the 32 KB bare stack (mod.c:452). Locals look small today, but an overflow silently corrupts OS memory before the restore. Put a canary pattern at the bottom, check it after each module, and halt rather than restore the OS if it is broken.
- **L7.** out.c:325-337: when a record overflows LINE_MAX, the case is replaced by a log, but it has already been counted. The run-level verdict catches it through `done.fail`, but the per-case baseline loses the key. Emit a minimal `case` record (mod, case, st, why = "record too long") instead.
- **L8.** modules/takeover.c:17-35: the test is largely tautological. It compares registers right after writing them back and cannot detect missing state (see M1). Have it also check COP1LC/COPCON/CACR once the runtime saves them.
- **L9.** modules/rt.c:86-91: `vbl_clock` cannot fail. If VERTB is broken, the loop never ends, and the watchdog is also driven by VERTB, so the run hangs with no diagnostic. Bound the loop by counting beam wraps (`beam_frames_wait_step`).
- **L10.** mod.c:426-444: pools are never freed. That is fine with the halt end action, but it leaks with end_action = return.

**Probe**
- **L11.** probe.c:168-231: the 68030 MMU walk test enables translation on any CPU whose PMOVE to TT works, including a 68EC030, where TC writes are undocumented. The table and cleanup are correct (identity map, TT and CRP restored, ATC flushed by PMOVE with FD = 0). For an EC030, prefer the read-back result and run the walk only when TC read-back shows the register is real.
- **L12.** probe.c:732-749: the board list is capped at 16 entries with no truncation flag. Add `boards_truncated`.

**devio**
- **L13.** devio.c:971: TD_GETGEOMETRY, READ CAPACITY, TD_GETNUMTRACKS and TD_GETDRIVETYPE are sent to *unowned* units before the ownership check. They are read-only, but CONTRACT §11 says "read at most block 0". Either move them after ownership or amend the contract.
- **L14.** devio.c:313-316: AROS `ata.device` and `carddisk.device` are not in the list. On AROS IDE entries the media are attached but no unit is ever tested, and nothing reports that. Add the names, and emit an `owned_units` count the host can compare with what it attached.
- **L15.** devio.c:660-663: the odd-alignment cases are permanent SKIP records (6 per unit). They add no information; drop them or emit one note.

**misc_hw**
- **L16.** misc_hw_gayle.c:356: `if (a != 0x55 && b != 0xaa)` should be `||`. A floating Gayle bus that latches the last written byte ($AA) passes as "IDE present" on a drive-less A600/A1200, and the IDE cases then fail.
- **L17.** misc_hw_gayle.c:209-216: `ide_status_idle` requires DRDY = 1. ATAPI devices keep DRDY = 0 until IDENTIFY PACKET (ATA/ATAPI-4 §8), which gives a false fail on an A1200 with an IDE CD-ROM. Test for the 14/EB signature first.
- **L18.** misc_hw_gayle.c:121-133: writing 0 clears every Gayle IRQ latch, including a CCDET change still pending for card.resource. The OS can then miss a card event. Clear only the bits the test itself caused.
- **L19.** misc_hw_gayle.c:645-646: Gayle detection by card.resource. Confirm the CD32 ROM (40.060) has no card.resource; if it does, the module pokes unmapped $DA8000-$DE1000 on a CD32.
- **L20.** misc_hw_rtc.c:657-686: in the MSM BUSY poll, each iteration calls `amt_ticks()` (software multiplies), roughly tens of µs on a 68000. The BUSY window around each carry can be missed. Poll `rr(13)` in a tight counted loop and read the clock every N iterations.
- **L21.** misc_hw_rtc.c:1045-1058: the restore runs in its own `amt_try`, which is good. But if the watchdog fires inside `rtc_body`, `amt_wd_armed` stays 0 for the rest of misc_hw, so the later parts run without a watchdog. Re-arm it in `part()`.
- **L22.** misc_hw_os.c:170-174: Zorro II `er_Flags` bits 5-0 must be 0. That is correct per the spec, but some real boards have junk in reserved bits. Consider reporting this as a warning in ctx rather than a FAIL.

**Device modules, from the two sub-reviewers (expsound/network L1-L7, RTG L1-L10)**
- **L23.** network.c:943-948: the ZZ9000 rx-count check needs a drop of exactly one; any frame that arrives in between breaks it. Accept a drop of at least one.
- **L24.** network.c:885/903: the ZZ9000 CONFIG register is left at 0 and not restored.
- **L25.** network.c:723-725: the untestable "why" text depends on stray LAN frames, so it is non-deterministic. Use a fixed string and put the detail in ctx.
- **L26.** expsound.c:549-569, 678-681: any level-6 interrupt is counted as spurious or treated as a FIFO event. Count only what the board's own status shows.
- **L27.** expsound.c:600, 654: `dur_frames` hard-codes 50 Hz. Use `amt_secs()`.
- **L28.** expsound.c:617-618, 1207: square-wave tones go out unmuted at -12 dB on real line outs. Use -30 dB, or document it.
- **L29.** network.c:580-581: one missing frame fails three cases. Report the dependent ones as untestable.
- **L30.** rtg: two separate shadows of the CV64 control byte (rtg.c:864, rtg_s3.c:9).
- **L31.** rtg: W32 detection by port read-back at 217A can be fooled by bus latching (rtg_misc.c:66-68).
- **L32.** rtg: clock-select changes are made without SR0 synchronous reset (rtg.c:186, 198, 374, 393).
- **L33.** rtg: some cases are silently omitted instead of skipped, so the case set depends on the card (`vint_amiga`, `cl_blt_autostart`). `mode_screenshot` passes on a host ack alone.
- **L34.** rtg: `vram_walk` passes a card whose upper half is mirrored (rtg.c:700). Register pairing is ambiguous with two identical cards (rtg.c:1016). The ET4000 KEY is left unlocked.

**Host side**
- **L35.** Isolation gaps and repro instructions:
  - **isolation.py:20-29:**
    - `~/Amiberry` is watched non-recursively, so writes inside `nvram/`, `Harddrives/`, `Floppies/` and `Kickstarts/` go undetected.
    - The kickstarts dir is watched non-recursively and only at "warn".
    - This does not affect safety, because the runner never hands the emulator a user path (see below). Make the watch recursive for `~/Amiberry/*` at severity fail anyway.
  - **problems.py:246-275:**
    - The gdb.cmd comment tells the user to run `amiberry --config config.uae` directly, which uses the real home.
    - host-gdb.cmd does not set XDG_CACHE_HOME or XDG_STATE_HOME.
    - Fix: point to `./run.sh --gdb` only, and set every XDG variable.
  - **media.py:256-301:** `verify()` ignores block_size, first_free and kind, and detects SRAM images by file size alone (4 MB + 4 KB).

---------------------------------------------------------------------------
## What I checked and found correct

**Bootblock**
- Uses only KS1.2 LVOs; CacheClearU is guarded by exec >= 37; MEMF_CHIP is added on exec < 36.
- The CRC-32 matches zlib (reflected, $EDB88320, init/final ~0).
- Relocation and BSS clearing use 32-bit counts.
- Failure paths free exactly what was allocated, turn the motor off and return -1. The CRC flash runs about 50 frames.
- The size check is at assembly time. The success path returns the dos.library rt_Init.

**mkadf**
- Rejects BSS that is not at the end.
- Rejects relocs outside the stored data, at odd addresses, or duplicated.
- RELOC32SHORT padding is handled.
- The bootblock checksum is add-with-carry then NOT.
- `alloc_size` = max(...) as the contract says.
- `--dump` cross-checks every header field.

**start.s**
- `amt_cli_entry` is at image offset 0: start.o is linked first (Makefile:31/54).
- The header slot marker is correct and bootio = NULL. No module uses `amt_bootio` except paula_disk.c:970, which checks it for NULL.

**bare.s and mod.c**
- try/abort: the frame layout is consistent and nesting works.
- Watchdog: hard and soft limits, disarmed on fire.
- The ISR masks requests per level (HRM table 7-1), and spurious requests are cleared per level only.
- `amt_supervisor_run` returns with RTE, so it works with any frame format.
- Pools are 16-byte aligned with a fallback to any memory.
- `amt_vec_set` range-checks the vector number.

**rt/takeover.c**
- INTENA/DMACON/ADKCON are restored with the right SET/CLR masks; INTEN is left to Enable().
- Vectors 2-63 are restored at the saved VBR.
- Leaving INTREQ alone at the end is a reasonable choice: CIA lines are level-triggered.

**out.c**
- JSON escaping, CRC suffix, ring wrap, and the TBE poll with timeout.
- The mailbox magic is written last.
- The host validates the mailbox header (version, state, ring_size), so the `magic` literal in the image cannot be mistaken for the mailbox.

**Probe**
- Every control-register write is restored: CACR, PCR, 040 TC with E = 0, URP, 030 TT and CRP.
- The 030 ATC is flushed (PMOVE with FD = 0) and the D-cache is cleared after the walk.
- The FSAVE/FRESTORE pairing is right, and the io[] buffers are big enough for a 68882 busy frame.
- The addr24 alias test writes only to two AllocAbs'd blocks, and the board variant only reads.
- The JSON is bounded.

**devio**
- Writes happen only after both signatures check out: magic, CRC, lba 0 and total-1, same total, same UUID.
- Blocks 0 and total-1 and the RDB area are never written.
- Every request has a timeout. A hung request is never freed, and all I/O stops after it.
- Track buffer flushes with CMD_UPDATE/CMD_CLEAR.
- Guard bytes around the buffers; chip buffers on V34; device nodes with the same name are opened by node.
- Timing values only appear in `timing` and perf records.

**media.py**
- RDSK layout and offsets match hardblocks.h: Reserved1[6] = $FFFFFFFF, RDBBlocks, Lo/HiCylinder, HighRDSKBlock.
- The checksum is right; the PART block has PBFF_NOMOUNT, DosEnvec TableSize 16, DosType 0 and BootPri -128.
- The ADF has no DOS bootblock, the plain HDF has bootpri -128, and the CIS includes CISTPL_FORMAT so KS 3.1 does not add the card as RAM.
- A user disk cannot pass as test-owned by accident: that needs an AMTOWN01 magic, CRC-32 and matching UUID in both end blocks. The remaining risk is M2, where test media is repurposed.

**misc_hw RTC**
- Register maps and bit meanings for the MSM6242B (CD/CE/CF, PM in H10 b2) and the RP5C01 (mode banks, TIMER EN b3, ALARM EN b2, 12/24 in bank 1 reg A, leap counter in reg B, PM in H10 b1) are right.
- Detection only reads.
- Time is restored with the elapsed-time correction; RP5C01 RAM, 12/24 select, MODE and the MSM CE/CF registers are restored.
- The restore runs in its own `amt_try`.

**misc_hw Gayle**
- The card status register and the config register (voltage) are never written. IRQ bits 1-0 are written back as read.
- Attribute memory (and so the PCMCIA config registers) is only read.
- Common memory is written only on a card signed at both ends, not in the MemList, and only in `[max(ff,2), total-1)` capped at 64 KB.
- The task file is saved and restored. IDENTIFY reads only (the caveat is H2).
- The Gayle register map matches the Linux/NetBSD notes.

**misc_hw Akiko and CDTV**
- The Akiko probe is a read inside `amt_try`.
- The 24C08 is accessed with random reads only; no write cycle can start (no data byte before STOP).
- The C2P model is right: pixel 0 in bit 31 of plane 0.
- CDTV bookmark RAM is only read, and the DMAC base is not used for writes.

**misc_hw_os**
- Read only.
- The Z2 and Z3 size tables are right: Z3 EXTENDED is 16 MB << n, and the sub-size table is right.
- Address windows, alignment counted from $200000, the overlap check, MemList ownership, and DiagVec inside the board are all right.
- Shut-up boards are excluded.

**Host side**
- `isolated_env` redirects HOME, every XDG variable and AMIBERRY_HOME_DIR, and builds the environment from a whitelist.
- The runner copies every ROM (Kickstart, extended, board) into the run's `work/.../media/roms` (runner.py:375-386, media.py:411-417), so the emulator never gets a path in the user's ROM directory.
- `amiberry.rom_path`, `floppy_path`, `hardfile_path` and `cd_path` all point into the work dir.
- `ks_write_enabled=false`, `floppy0wp=true`, and flash/rtc files are empty.
- `_safe_rmtree` refuses paths outside its root.
- Every Amiberry launch holds a slot (`--version`, `--dump-config`, runs). The limits of that are in M14.

---------------------------------------------------------------------------
## Resolution

Status per item: **fixed**, **partly fixed**, **rejected** (with the reason) or **deferred**.
Case ids are unchanged; new ids are only added (`<card>_board_rejected`). The verification runs are
listed at the end.

### High
- **H1 fixed.**
  - Already done before this pass: the probe records cd_Flags/er_Type, and there is a shared guard, `amt_board_ok()`.
  - **Correction to the guard (and to this review):** only CDF_SHUTUP disqualifies a board.
    - libraries/configvars.h defines CDB_CONFIGME as "this board needs a driver to claim it". expansion.library sets it on every configured board that no driver has bound yet.
    - At bootblock time that is every RTG, sound and network card. Rejecting CONFIGME made the guard refuse all of them: the first verification run showed `*_board_rejected` with cd_flags 02 on every card.
    - A board that was never configured has no valid cd_BoardAddr, so the address-window checks still catch it.
  - **The guard is now used by every module that touches a board:**
    - **rtg:** rejected boards are never paired or written. A recognised card gets `<card>_board_rejected` (skip, with addr and cd_flags in ctx).
      - New `layout_fits()`: every VRAM window, the I/O port span and the board-specific registers must lie inside the autoconfig size of the board they are on. For example, CV64/3D Z3 needs up to $C0E0008, Picasso II needs regs $9000-$B000, and Picasso IV Z2 has its I/O at +$10000. If not, `<card>_suite` skip with ctx `needs`.
      - A missing register board is also a `suite` skip; before, `io` was computed from a NULL base.
    - **expsound:** `toccata/prelude/uaesnd_board_rejected`. The uaesnd RAM block must lie within the board size.
    - **network:** `a2065/zz9000_board_rejected`.
    - **memory_os:** rejected boards are not used to classify regions.
  - **Left as they are:**
    - exceptions.c reads `$7F000000` only under unsafe. It deliberately treats any listed board as decoding it, which is the conservative choice.
    - misc_hw_cd only reports the DMAC base.
    - misc_hw_os is read-only.
  - **Not added:** the Z2 alignment and the MemHeader-overlap checks. misc_hw_os already tests both as consistency cases.
- **H2 fixed.**
  - **Takeover guard** (rt/mod.c `quiesce()`, documented in CONTRACT section 14). Before the takeover, with Forbid+Disable held, the runtime requires:
    - DSKBYTR DMAON = 0 and DMACONR BBUSY = 0, at every start;
    - in CLI mode (bootio NULL), also Gayle IDE ($DA101A) or A4000 IDE ($DD303A) alternate status with BSY = DRQ = 0. Gayle is found by card.resource; A4000 IDE by AGA, no card.resource, and a ROM scsi.device or an ata.device. $FF/$7F counts as no device.
  - **Waiting:**
    - CLI mode first waits 2 s (dos Delay) so filesystems flush.
    - While busy, it releases Forbid/Disable and waits 0.1 s, up to 10 s.
    - After that it refuses: every bare module reports `module` skip "takeover refused: …" and nothing is taken over.
  - DSKLEN = $4000 is written before every module by the M1 policy.
  - **misc_hw_gayle:**
    - No task-file access in CLI mode without AMT_F_UNSAFE (`ide_present` skip).
    - BSY = 0 and DRQ = 0 are required before the first task-file write and again before IDENTIFY (ATA-4 7.15.6).
  - **Not done: Inhibit() of the DOS volumes.** Inhibiting every volume, including the CD or HD the program runs from, forces a re-validation of each one. The bounded idle wait reaches the same safety without that.

### Medium
- **M1 fixed** (done before this pass).
- **M2 fixed** (devio `owner_layout()`, CONTRACT section 11). After both signatures match, blocks 1-15 are read:
  - A plain hardfile (kind 2) must have no RDSK.
  - An RDB hardfile (kind 3) must have exactly the test-owned RDSK at block 1: valid checksum, no bad-block list, no filesystem-header list. Its partition list must be empty, or be the one AMTTEST PART at block 2 (PBFF_NOMOUNT, DosType 0, no next).
  - Anything else is `owner` FAIL "the medium was repartitioned", and the unit is not written.
- **M3 fixed.**
  - **Signature fields:** both blocks must agree on first_free and kind; the last block's block size must be 512; kind must be 1-4, and kind 1 only on trackdisk.
  - **Region:** `[max(first_free,2), total-1)` must be non-empty.
  - **4 GB limit:** total <= $800000, otherwise `owner` skip "larger than 4 GB" (refused, since TD64/NSD are not used). media.py refuses hardfiles of 4096 MB or more.
  - `region_alloc()` uses explicit bounds and cannot wrap.
  - **Rejected part: "total <= geom_total".** CONTRACT section 11 makes capacity a separate case, and the last block's own LBA field already proves that block total-1 is the signed block, so a device that wraps addresses cannot pass.
- **M4 fixed.**
  - The broadcast ARP is sent only with the new run flag `net` (AMT_F_NET, bit 3: "the network card is on an emulator-only backend") or with `unsafe`. Otherwise the `*_ext_arp` cases (and ZZ9000 `rx_accept`/`rx_irq`) are untestable.
  - The runner sets `net` for entries whose a2065/zz9000_net/ne2000_* option is `slirp` (runner.py `_with_net_flag`).
  - Documented in CONTRACT section 4, network.c and docs/cases/network.md.
  - I chose this over the RFC 5227 probe because the probe is still a broadcast on a real LAN, and the flag says exactly what is known: only the host knows the backend.
- **M5 fixed.**
  - Only an ARP reply with sender 10.0.2.2 and target 10.0.2.15 counts.
  - Other A2065 frames are skipped and their RX descriptor is handed back.
- **M6 partly fixed.** Failure ctx of the codec boards carries `glue_source` (board glue modelled on Amiberry sndboard.cpp; codec registers from the data sheets). **Deferred:** the cross-check against NetBSD toccata.c and the AHI drivers.
- **M7 fixed.** `t_index` leaves MCE through `cmce_off()` (INIT and ACI wait) before the TRD group.
- **M8 fixed.** The ZZ9000 `vram_bwl/vram_addr_lines/vram_walk/vram_draw` run only with unsafe; otherwise they skip. There is no documented free area in the firmware's RAM. The register reads (fw version, vblank) still run.
- **M9 fixed in part; the VRAM half is rejected.**
  - **CR37 is no longer forced.** It is kept as found, and always on the W32 (Merlin).
    - The one exception is a 1 MB ET4000AX board (oMniBus, Domino) where it still reads 0, i.e. nothing programmed it. There the CPU reaches only 64 KB, and the first run without any write turned `vram_walk` into a fail (good_kb 64) on both boards.
    - There it is set to 32-bit/256Kx4 (board model) and restored. The value is logged.
  - The W32 index register 217A is restored to its found value.
  - **Rejected: the "VRAM saved after the mapping change" part.** The save and the restore both happen in the test mapping, before CR37 is restored, so they address the same cells.
  - KEY stays unlocked (L34): its state cannot be read.
- **M10 fixed, partly disputed.**
  - When no interrupt arrives on a Picasso II whose INT2 is only inferred from its GD5428 (`irq_inferred`), `vint_amiga` is untestable, not fail.
  - **Disputed: dropping the $9000/$9001 writes.** They are kept: they are the board's interrupt enable/disable as the Picasso96 driver uses it (the access-path model), and a II+ without them could not be tested at all. The register board decodes the rest of that range as VGA I/O mirrors. There is no data sheet either way, and the docs say so.
- **M11: no module change needed.**
  - The runtime's bare-module policy sets CACR to instruction cache on, data cache off on the 020/030 and the 040/060. Register reads are then never line fills, and the policy is the same for every module (deterministic).
  - None of the device modules (rtg, expsound, network, misc_hw, devio) touches CACR. The modules that test caches (smc, memory, dispatch, dmacontention, mmu) set them explicitly.
- **M12 partly fixed.**
  - Fixed: the ViRGE PCI command register is written back.
  - Documented as write-only or reset-value registers that cannot be saved: CV64/3D Z2 +$3A0008, the monitor switch, the board IRQ enable.
  - **Deferred:** the Picasso IV Z2 bank read at regs[0]. The only model of that register (WinUAE/Amiberry) returns the bank there. And the 8514/S3d engine state.
- **M13 fixed.** Data sheets checked:
  - **MSM6242B: twelve o'clock is "12".**
    - The OKI sheet (FEDL6242B-02) gives only H10 0-1 plus the PM/AM bit.
    - The register-compatible Epson RTC-72421 manual (ETM17E-02, H10 register) gives "12-hour clock 12:00 to 11:59, a.m. and p.m.".
    - So hour digits 00 stay invalid, and the test's encoding was right.
  - **RP5C01: undefined.** The Ricoh sheet (EK-086-9804) only says D1 of the 10-hour counter is PM; it gives no hour range. Chip emulators (openMSX) use AM 00-11 / PM 20-31.
    - So on the RP5C01 `decode()` now accepts both 12 and 00 for twelve o'clock (CONTRACT section 10 rule 1); the case text and docs/cases/misc_hw.md say so.
    - Writes still use "12". That matters only for a restore of a 12-hour-mode clock at 12 o'clock, and the OS runs the clock in 24-hour mode.
  - The Amiberry finding stands: rtc.cpp builds H1 from the 24-hour value in 12-hour mode, for both chips.
- **M14 fixed** (done before this pass).

### Low
- **Fixed:**
  - **L3:** takeover reads the VBR with MOVEC when exec flags a 68010+ or the probe found one.
  - **L13:** the contract was amended instead. TD_GETGEOMETRY, READ CAPACITY, TD_GETNUMTRACKS/DRIVETYPE, TD_CHANGESTATE and TD_PROTSTATUS read no medium data, and the buffer memory type comes from dg_BufMemType. CONTRACT section 11 now allows them on every unit.
  - **L16:** `||`.
  - **L17:** DRDY = 0 is accepted on a device with the 14/EB signature.
  - **L18:** `gayle_irq_write0_clears` writes 1 to pending latches other than IDE, so a card change stays pending.
  - **L23:** a drop of at least one.
  - **L25:** a fixed "why"; the frame count is in ctx.
- **Rejected:**
  - **L22:** Zorro II er_Flags bits 5-0 must be 0 by the spec (libraries/configregs.h, Zorro II spec). A board with junk there is non-conforming, and the case says so.
- **Deferred** (not in the device scope of this pass, or no clear fix yet):
  - boot, runtime and probe: L1, L2, L4-L12;
  - L14: adding ata.device/carddisk.device would add new units and cases;
  - L15, L19, L20, L21;
  - L24: the ZZ9000 CONFIG enable bits are not readable;
  - L26-L35.

### Verification (Amiberry, `make -C disk B=build-fixdev set`; runner `--matrix standard`)
Result directories are under work/results.

**Exp disk, 29 entries (20261103-204322Z, with ET4000 re-run 20261103-215031Z):**
- Boards covered:
  - RTG: Picasso II/II+/IV Z2/Z3, CV64, CV64/3D Z2/Z3, oMniBus ET4000 and W32, Merlin, Domino, ZZ9000 Z2/Z3, Piccolo, A2410;
  - sound: Toccata (+JIT), Prelude, uaesnd Z2 (+JIT), uaesnd Z3;
  - A2065;
  - A500+ RTC, A600, A3000/A4000 Z2+Z3, A3000;
  - CD32 and CDTV (CLI start).
- **What changed:**
  - No board is rejected any more. The first run, with the CONFIGME bug in the guard, rejected all of them.
  - The `*_ext_arp` cases pass, because the runner sets `net` on the slirp entries.
  - The ZZ9000 VRAM cases (4 per board) are now skip.
  - With CR37 left as found, oMniBus/Domino `vram_walk` failed (64 KB). With the conditional set it passes again.
- **Remaining fails are not caused by these changes:** they are in code paths this pass did not touch:
  - RTC writes ignored or the clock not stoppable;
  - `vga_vint_status` on ET4000/ViRGE;
  - Merlin `dac_rw`, W32 `dac_pelmask`, ViRGE ROP CC;
  - the A2065 loopback cases;
  - codec MCE protection;
  - the uaesnd interrupt counts;
  - Akiko I2C.

**Core disk, 10 entries (20261103-205520Z):** A600 Gayle IDE, A4000 IDE (+JIT), A3000 SCSI, A2091, A4091, CD32 (+JIT), CDTV, A500+.
- Compared case by case with the core disk built before this pass, the devio verdicts are identical.
- The A2091 `rw*_w2` fails and the A4091 `geometry_td` fail (finding D4) are as before.
- On CD32/CDTV the takeover guard lets every bare module run: no `module` skip appears.

**Ownership (`tests/smoke_disk.py`, A4000 IDE, 3 hardfiles made with media.py):**

| Hardfile | Result | Blocks written |
|---|---|---|
| RDB with the AMTTEST PART | owned | 754 |
| The same with that PART changed to a mountable DOS\3 "DH0" | `owner` FAIL "repartitioned" | 0 |
| Plain hardfile with an RDSK added at block 1 | `owner` FAIL "partitioned" | 0 |

**Cpux disk, A4000/040 (20261103-215147Z):** memory_os, 18 pass, no fail.
