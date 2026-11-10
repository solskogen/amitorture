# amitorture interface contract (v1)

This file fixes the interfaces between the parts of amitorture. Every component must follow it exactly. If you change it, change it here first and say so.

Big-endian everywhere on the Amiga side. "u32" means a big-endian 32-bit unsigned integer.

---------------------------------------------------------------------------
## 1. ADF layout (built by `tools/mkadf.py`)

Standard 880 KB DD ADF (901120 bytes, 1760 sectors × 512).

| Offset | Size | Content |
|---|---|---|
| 0 | 1024 | Bootblock: `"DOS\0"`, the standard bootblock checksum at +4, rootblock 880 at +8, code from +12 (`disk/boot/bootblock.s`). |
| 1024 | 512 | **Payload header** (sector 2), described below. |
| 1536 | n×512 | **Payload blob**: the code segment zero-padded to `alloc_size`, then the data segment and the relocation table, zero-padded to a multiple of 512. |
| rest | | Zero, or reserved for future module payloads. |

### Payload header (sector 2, 512 bytes)
| Off | Type | Field |
|---|---|---|
| 0 | char[4] | magic `"AMTP"` |
| 4 | u32 | header version = 2 |
| 8 | u32 | `image_size`: bytes of code in segment 0 (relocated; multiple of 4) |
| 12 | u32 | `bss_size`: zeroed bytes that follow the data in segment 1 (not stored) |
| 16 | u32 | `reloc_count`: number of u32 entries in the reloc table that follows the data in the blob |
| 20 | u32 | `entry_offset`: offset of the entry point in segment 0 (mkadf takes it from the `_amt_entry` symbol, else the start of hunk 0) |
| 24 | u32 | `blob_sectors`: number of 512-byte sectors in the blob (code sectors + `data_sectors`) |
| 28 | u32 | `blob_crc32`: CRC-32 (IEEE, zlib) of the blob bytes `alloc_size + data_size + reloc_count*4` (the padded code, then data and relocs) |
| 32 | u32 | `alloc_size`: segment 0 size = code sectors × 512 |
| 36 | u32 | `alloc_flags`: exec MEMF_ flags used by the bootblock for both segments (default `MEMF_ANY|MEMF_CLEAR` = 0x10000) |
| 40 | u32 | `data_size`: bytes of initialised data at the start of segment 1 (multiple of 4) |
| 44 | u32 | `data_sectors`: sectors holding data + relocs = ceil((data_size + reloc_count*4)/512) |
| 48 | u32 | `data_alloc`: segment 1 size = max(data_sectors*512, data_size + bss_size, 4) |
| 64 | 64 bytes | **Run parameters** (§4), baked in per run by the runner via `mkadf.py --param` |
| 128 | 64 bytes | build id: an ASCII, NUL-padded string (git rev + build date) |
| rest | | zero |

### Relocation semantics
- `mkadf.py` flattens the linked AmigaOS hunk executable into two segments. CODE hunks go to segment 0, DATA hunks then BSS hunks to segment 1, each aligned to 8 bytes. The hunk order must be CODE*, DATA*, BSS*; the BSS hunks are covered by `bss_size`.
- Two segments let a 512 KB chip + 512 KB slow RAM machine hold a payload that is bigger than either free region.
- Each HUNK_RELOC32 entry is resolved at build time to the target hunk's offset in its segment. The image is patched with that offset, and the entry records where the patch is: bit 31 = the longword is in segment 1 (else 0), bit 30 = the target is in segment 1 (else 0), bits 29-0 = the longword's offset in its segment.
- At load time, for every entry, the bootblock adds the target segment's base to the longword.

---------------------------------------------------------------------------
## 2. Bootblock behaviour (`disk/boot/bootblock.s`)

Entry is the standard Kickstart bootblock entry: A1 = the trackdisk IOStdReq, A6 = SysBase. It must work on Kickstart 1.2-3.2 and AROS ROM.

1. AllocMem a 512-byte header buffer and an 11-sector (one track) bounce buffer, both MEMF_CHIP, because trackdisk.device before V36 needs chip RAM buffers. Read sector 2 (header) with DoIO(CMD_READ) into the header buffer. If the magic or version is bad, exit with a failure (step 6).
2. AllocMem(`alloc_size`, `alloc_flags`), giving `base0` (code), and AllocMem(`data_alloc`, `alloc_flags`), giving `base1` (data + BSS). AllocMem already prefers fast and slow RAM to chip RAM, so on an A500 with slow RAM the two segments can land in different regions.
3. Read the blob through the bounce buffer: `alloc_size` bytes from offset 1536 into `base0`, then `data_sectors*512` bytes into `base1`. Free the bounce buffer.
4. Verify `blob_crc32` over `base0` (`alloc_size` bytes), then `base1` (`data_size + reloc_count*4` bytes). On a mismatch, flash the screen red (COLOR00 = $F00 for ~1 s) and exit with a failure.
5. Apply the relocs (the table starts at `base1+data_size`), then zero `[base1+data_size, base1+data_size+bss_size)`. This is done after the relocs because the reloc table sits in that range. Turn off the drive motor (TD_MOTOR 0). If exec ≥ 37, call CacheClearU() (LVO -636).
6. `jsr base0+entry_offset` with: A6 = SysBase, **A0 = pointer to the 512-byte header copy** (it stays allocated), A1 = the boot IOStdReq, A2 = base0, A3 = base1.
   - On return with D0 = 0: do a normal bootblock exit, i.e. FindResident("dos.library"), A0 = rt_Init, D0 = 0, rts.
   - On D0 ≠ 0 (or failure in steps 1-4, which frees what was allocated): moveq #-1,d0 / rts.

The bootblock must not touch anything else. It must fit in 1012 bytes of code.

---------------------------------------------------------------------------
## 3. Payload entry ABI (`disk/rt/`)

```c
/* called by the bootblock; exec is alive, multitasking on */
long amt_entry(struct ExecBase *SysBase /* A6 */, const struct amt_header *hdr /* A0 */,
               struct IOStdReq *bootio /* A1 */, void *base /* A2 */, void *base2 /* A3 */);
```
- It lives in `disk/rt/start.s`, which saves the registers, stores SysBase/hdr/bootio/base/base2 in globals (`amt_base` = segment 0, code; `amt_base2` = segment 1, data + BSS) and calls `int amt_main(void)` in C.
- The same file has the CLI entry at image offset 0 (`amt_cli_entry`, §14), used when the payload runs as an AmigaDOS program (CD boot); then bootio is NULL.
- The result goes back to the bootblock in D0. 0 means "continue booting DOS", which isn't used in v1. The v1 end actions are in §4.
- C runtime: freestanding, no libnix/ixemul. `-m68000 -Os -ffreestanding -nostdlib -fno-builtin -noixemul -fomit-frame-pointer`, linked with `-nostartfiles`. Exec calls use bebbo's inlines (`<proto/exec.h>` with a global `SysBase`).
- Code for a specific CPU or FPU goes in separate objects (`*_020.c/.s`, `*_fpu.s` …) and is called only after the probe says it's safe.

---------------------------------------------------------------------------
## 4. Run parameters (header offset 64, 64 bytes)
| Off | Type | Field |
|---|---|---|
| 0 | char[4] | `"PARM"` (absent means defaults; mkadf always writes the block, with defaults for keys not given) |
| 4 | u32 | mode: 0 = full (default), 1 = quick (fewer inputs per case, same case ids), 2 = soak (reserved: not implemented, runs like full) |
| 8 | u32 | seed (default 1) |
| 12 | u32 | end_action: 0 = **halt** (default: resume OS execution with best-effort CIA restoration, then Wait(0) forever, so the system stays alive and the host kills the emulator), 1 = ColdReboot, 2 = run OS modules only, skip bare takeover, then return D0=-1 to the bootblock |
| 16 | u32 | flags: bit0 = unsafe tests allowed, bit1 = serial output off, bit2 = mailbox off, bit3 = net: the network cards are on an emulator-only backend (user-mode NAT), so the network module may transmit (the runner sets it for entries with a `slirp` network option), bit4 = noscreen: no status screen (`rt/screen.c`, §10), so every bare module starts with all DMA off, bit5 = intcompat (32): integer compatibility instruction policy, bit6 = fpucompat (64): FPU compatibility instruction policy. mkadf names: `unsafe`, `noserial`, `nombox`, `net`, `noscreen`, `intcompat`, `fpucompat`. |
| 20 | u32 | serial baud (default 115200) |
| 24 | u32[2] | module enable mask (bit n means module id n; all ones means all) |
| 32-63 | | reserved, zero |

`AMT_F_INTCOMPAT` (32) and `AMT_F_FPUCOMPAT` (64) are independent policy
declarations, not requests to change the CPU/FPU. An unset bit means
hardware-strict assertions; raw media and bare-metal defaults remain `flags=0`.
The runner derives each bit from the configured `cpu_no_unimplemented` /
`fpu_no_unimplemented`: false declares compatibility, true declares strict.
An enabled JIT (`cachesize>0`) declares both compatibility policies, matching
Amiberry's JIT fixup. Other user flags are preserved, but stale explicit
compatibility bits are cleared when the corresponding effective policy is strict.
Late runner configuration overrides follow the same rule.

Compatibility policy does not excuse numerical, register or enabled-exception
errors. It only distinguishes hardware-only unimplemented-instruction trap
assertions from an explicit emulator policy that implements those instructions.
Separate strict interpreter lanes retain architectural trap coverage; declaring
compatibility must not be reported as having tested those strict assertions.

---------------------------------------------------------------------------
## 5. Output records (wire format)
- Each record is **one line**: `<json> *<crc>\n`, where `<crc>` is 8 lowercase hex digits of the CRC-32 (zlib/IEEE) of the exact `<json>` bytes. The JSON is ASCII only and holds no newlines.
- Every record has: `"v":1` (schema version), `"seq":<n>` (0,1,2,… per boot), `"t":"<type>"`.
- Numbers: integers in decimal; register-like values as hex **strings** without `0x` (e.g. `"pc":"0001a2c4"`).
- The same lines go to both transports (§6, §7), in the same order.

### v1 record types
| t | Required fields | Meaning |
|---|---|---|
| `hello` | `build`, `mode`, `seed`, `exec` (version.revision), `cpu`, `fpu`, `mmu`, `probe` (object, §8) | first record after the probe |
| `log` | `msg` | free text |
| `mod` | `mod`, `ev` ("begin"/"end"), on end also `pass`, `fail`, `skip` | module boundary |
| `case` | `mod`, `case`, `st` ("pass"/"fail"/"skip"/"untestable"); on fail: `expect`, `got`, `why`; optional `ctx`, `in` | one test case result |
| `capture` | `what` ("screen"/"audio_start"/"audio_stop"), `tag` | asks the host to capture now (best effort) |
| `perf` | `mod`, `ops`, `ticks`; optional `tick_hz`, `unit` | throughput; `amt_perf()` uses colour clocks (`unit:"cck"`), while `amt_perf_hz()` specifies its clock |
| `speed` | `mod`, `ev` ("begin"/"end"/"skip"), `insn`; begin/end also `cls`, `pos`; end also `n` and `timing` (`cck`, `empty_cck`, `frames`, `units`); skip also `why`; optional `benchmark:1` | a hostspeed timed block (docs/schema.md 1.10); not a case; benchmark-only blocks are informational rather than real-time verdicts |
| `done` | `pass`, `fail`, `skip`, `untestable`, `aborted`, `records` (count of all records sent this boot, including `done`) | last record |

New disks also emit `hello.modules`, an object mapping every selected compiled module name to its unique id
(0-63). This is additive to v1: readers accept older captures without it. It records the intersection of the
compiled module table and the run mask, not all modules available in the source tree. Every declared module
must have matching begin/end records before `done`; an empty selection is skipped rather than passed.
They also emit `hello.mbox`, the mailbox address as 8 hex digits, when the mailbox exists (§7): a host can
follow the ring from there without scanning chip RAM for the header. A host must still check the header at that
address (magic, version) before using it.

They also emit `hello.policy`, `{"integer":"strict"|"compat","fpu":"strict"|"compat"}`,
derived from the two run-parameter compatibility bits. This is additive to v1;
older captures without it do not explicitly declare an instruction policy.

---------------------------------------------------------------------------
## 6. Serial transport
- Paula UART, 8N1, polled (wait for the transmit buffer to be empty before each SERDAT write; SERDAT = 0x0100|byte for 8N1). The runtime polls SERDATR's TBE bit 0x2000, which is the buffer state itself (INTREQR's TBE bit 0x0001 is a latch that is not necessarily set at boot), with a timeout so a dead UART cannot hang the run.
- SERPER = round(clock/baud) - 1, with clock = 3546895 (PAL) or 3579545 (NTSC), picked from the probe's PAL/NTSC result. At 115200 baud that is SERPER 30 on both: 114416 baud on PAL (0.7% slow), 115469 on NTSC (0.2% fast).
- It is used both before and after the OS takeover; it never uses serial.device.
- Host side (Amiberry): the config sets `serial_port=TCP://127.0.0.1:<port>/wait`. Amiberry listens there and the runner connects as a client.
- The ROM may print to the serial port before the first record (the AROS ROM prints `ROMInfo: …` lines, CR LF terminated). The host ignores everything before the first line that starts with `{"v":`.

---------------------------------------------------------------------------
## 7. Memory mailbox
`AllocMem(sizeof(struct amt_mbox)+16, MEMF_CHIP|MEMF_CLEAR)`, with the start aligned to 16.
The ring uses `MBOX_RING_SIZE` from `disk/rt/rt.h` (currently 32768 bytes), plus the 48-byte header.
The layout (all u32 big-endian):

| Off | Field |
|---|---|
| 0 | magic, 16 ASCII bytes: `AMITORTURE-MBOX!` |
| 16 | version = 1 |
| 20 | state: 0 = init, 1 = running, 2 = done, 3 = aborted, 4 = halted-after-done |
| 24 | heartbeat (incremented at least every record and from the VBL/timer while running) |
| 28 | total bytes ever written to the ring (monotonic) |
| 32 | ring_size (bytes) |
| 36 | current module id |
| 40 | current case number |
| 44 | **host_ack**: written by the HOST, the id of the last `request` it completed (§13) |
| 48 | ring[ring_size]: the record lines, wrapping |

The host finds it by scanning chip RAM (from 0, in 16-byte steps) for the magic. It reads the ring and uses offset 28 to get the new bytes and detect overruns.

---------------------------------------------------------------------------
## 8. Probe result (`disk/probe/`)
`void amt_probe(struct amt_probe *p);` runs **with exec alive, before takeover** (it may use exec structures; for CPU tests it installs and restores its own trap vectors under Disable()). The header is `disk/probe/probe.h`. The JSON object in `hello.probe` mirrors the struct:
- `cpu`: "68000|68010|68EC020|68020|68EC030|68030|68EC040|68LC040|68040|68EC060|68LC060|68060"
- `cpu_rev`: number or -1
- `fpu`: "none|68881|68882|68040|68060", `fpu_frame` (FSAVE idle frame size/version), `mmu` (0/1), `addr24` (0/1)
- `vbr`, `cacr` (hex)
- `how`: an array of short strings saying which tests decided each value
- `agnus_id`, `denise_id` (hex), `chipset`: "OCS|ECS|AGA" plus `ecs_agnus`/`ecs_denise` (0/1), `video`: "PAL|NTSC"
- `exec_attn` (hex AttnFlags), `kick` (version.revision), `rom_base` (hex)
- `mem`: an array of `{"name":…,"lower":hex,"upper":hex,"attr":hex,"free":n}` from ExecBase->MemList
- `boards`: an array of `{"man":n,"prod":n,"addr":hex,"size":n}` from expansion.library's ConfigDev list (opened read-only; if it isn't there, an empty array)

---------------------------------------------------------------------------
## 9. Repository layout
```
disk/boot/      bootblock.s
disk/rt/        start.s, main.c, mod.c/mod.h (module runner), out.c (serial+mailbox+json), crc32.c, takeover.c/.s,
                screen.c/screen.h (status screen), rt.h
disk/probe/     probe.h, probe.c, probe_asm.s
disk/modules/   (milestone 2+)
disk/Makefile   builds build/payload.exe, build/bootblock.bin, build/amitorture.adf
tools/mkadf.py  ADF builder (+ --param key=value, --dump to inspect an ADF)
runner/         Python package `amitorture` (CLI entry: runner/amitorture/__main__.py), run as `python3 -m amitorture` from runner/
docs/           CONTRACT.md (this), schema.md, catalogue.yaml
```
Toolchain: `AMIGA_TOOLCHAIN` (default `/opt/amiga/bin`): `vasmm68k_mot`, `m68k-amigaos-gcc`, `m68k-amigaos-ld`, `vlink`, `m68k-amigaos-objdump`.

---------------------------------------------------------------------------
## 10. Test modules (milestone 2+)

The API is in `disk/rt/mod.h`, which is authoritative; read it. In short:
- A module is `disk/modules/<name>.c`, plus optional `<name>_*.c` / `<name>_*.s` helpers. The suffixes `_010/_020/_030/_040/_060/_fpu` select CPU flags.
- It declares itself with one `AMT_MODULE(name, id, phase, flags, chip, any);` line at column 0, and its body is `void amt_run_<name>(void)`. The Makefile generates the table (`tools/genmodlist.py`).
- **Phases:**
  - `AMT_PH_OS`: exec alive, user mode.
  - `AMT_PH_BARE`: after takeover, supervisor mode, own 32 KB stack, INTENA = INTEN|VERTB, SR = $2000. DMA: DMACON = DMAEN|BPLEN|COPEN with COP1LC on the status screen's copper list; all DMA off for a module with `AMT_MF_OWNS_DISPLAY`, and for every module when the screen is off (`noscreen` flag, or no chip RAM for it). The default vectors 2..63 go to a catch-all that aborts the module with a full report. Levels 1..6 dispatch to `amt_irq_hook[level]` (C); level-3 VERTB belongs to the runtime (frame counter, heartbeat, watchdog of about 120 s; ×10 with AMT_MF_LONG; kicked by every reported case); it clears VERTB and counts the frame at IPL 7, so a level 4-6 handler reading `amt_ticks()` never sees the request cleared but the frame not yet counted).
  - `end_action=return` runs OS modules but skips the bare phase before takeover: CIA counters, even
    stopped ones, do not reveal the original timer latches. HALT/REBOOT retain destructive-test coverage
    for disposable environments; CIA restoration there is best-effort, not an exact recovery of OS timer
    periods. Default halt resumes OS execution for the screen/mailbox, but does not promise unchanged
    hardware state. A running desktop is not a safe target for HALT/REBOOT tests.
- **Flags** (`AMT_MODULE` argument 4): `AMT_MF_LONG` (watchdog ×10, the 30-minute hard limit ×4), `AMT_MF_NEEDS_FPU` (one `module` skip without an FPU), `AMT_MF_OWNS_DISPLAY` (the module tests display, DMA or beam timing and gets the hardware to itself: the status screen is off while it runs). It is set on blitter, copper, display, dmacontention, interrupts, paula and timing.
- **Status screen** (`disk/rt/screen.c`): 40×25 characters on one lores bitplane (8 KB of chip RAM, allocated after the pools and the bare stack; without it the disk runs without the screen), drawn with the ROM topaz 8 font, shown by the runtime's own copper list.
  - OS phase: a VERTB interrupt server at priority -120 sets COP1LC to that list and DMAEN|BPLEN|COPEN every frame.
  - Bare phase: before each module without `AMT_MF_OWNS_DISPLAY` the runtime sets COP1LC and DMAEN|BPLEN|COPEN; before an owner it turns BPLEN|COPEN off and leaves COLOR00 at the background colour. An owner therefore starts with the display registers the screen's copper list last wrote (DIWSTRT/DIWSTOP for 200 lines, DDFSTRT $0038, DDFSTOP $00D0, BPLCON0 $1200, BPLCON1 0, BPLCON2 $0024, on AGA BPLCON3 $0C00, BPLCON4 $0011 and FMODE 0, BPL1MOD/BPL2MOD 0, BPL1PT, SPRxCTL 0, COLOR00/COLOR01), not with values the OS left (which differed between Kickstarts anyway): it sets every register its cases depend on. With `noscreen` the OS's values stay.
  - `screen_finish` (main.c, after `done`) shows the verdict and the next disk; `screen_close` (end_action = return only) removes the server and gives the OS display back. With halt or reboot the screen stays.
  - The disk number and group come from the build id, which the set build ends with `<n>/<N> <group>`.
- **Results:** `amt_case()`, `amt_check32()`, or `amt_case_open()` + json_* + `amt_case_close()`. A failing case must carry `expect`, `got` and `why`, plus `in` (inputs) and `ctx` (opcode words, SR, …) so an AI can reproduce it without the disk.
- **Services:** `amt_try()`/`amt_abort()` (nestable), `amt_vec_set()`, `amt_last_exc`, `amt_ticks()` (colour clocks from beam position + VBL), `amt_rand()` (seeded per module), `amt_hot()` (hot-loop repetition), `amt_chip_pool()`/`amt_any_pool()`, and `amt_probe_result` (`AMT_CPU()`, `AMT_FPU()`, `AMT_MMU()`).
  The catch-all decodes bus/address-error SR/PC at offsets 8/10 only on a 68000; all 68010+ frames use
  their standard SR/PC prefix at offsets 0/2. `amt_last_exc.frame` retains the first 16 raw words.
  Modules with OS allocations spanning the bare phase register one non-null `amt_on_return(fn)` cleanup
  callback; registration is idempotent and returns -1 if all 64 slots are occupied. On `end_action=return`,
  callbacks run in reverse registration order after OS/display restoration, followed by pool and mailbox
  release using their original allocation bases/sizes. An allocation failure also releases partial test
  pools. Default halt keeps allocations and the mailbox alive for host observation.
  If module cleanup cannot establish a safe idle hardware state, `amt_abort_fatal(code)` reports
  `fatal_cleanup`, marks the mailbox aborted, disables interrupts/task scheduling and quarantines
  in place. It never unwinds through `amt_try`, starts another module, or resumes the OS; the test
  environment must be reset. It deliberately does not emit a normal `done` record or free buffers
  that an unquiesced DMA engine might still access.

### Rules for every module
1. **Expectations come from the architecture,** i.e. the M68000 PRM, the 68020/030/040/060 user manuals, the 6888x manual and the Amiga Hardware Reference Manual, **for the detected variant**. Never encode "what Amiberry does".
   - When Amiberry disagrees with a correct test, that is a finding: keep the test, and report the discrepancy with evidence (the manual reference and the Amiberry source location if you can find it).
   - When the architecture leaves a result undefined, accept every allowed result and say so in the case.
2. **Deterministic:** the same seed and the same CPU/FPU variant must give byte-identical records. That is how JIT-on vs JIT-off and emulator-vs-emulator get compared. So:
   - no timing-dependent values in pass/fail fields;
   - put timing measurements in `perf` records, or in `ctx`/`timing` fields that the host compares with tolerances.
3. **The JIT runs the same tests:** never branch on "is a JIT active". Use `amt_hot_checked()` where
   the callback can validate each result against architectural expectations (return zero only when valid).
   It checks iteration zero and `amt_hot_reps()` subsequent iterations. `amt_hot()` checks repeatability
   only: an independently checked reference is still required. Establish warm-up coverage for each snippet,
   not merely the enclosing dispatcher.
4. **Safety (real hardware):**
   - Touch only memory from the pools, your own stack/BSS, the vector table through `amt_vec_set`, and the hardware the module tests.
   - Leave owned hardware in its initial test state. The runtime resets INTENA/INTREQ/DMACON/vectors
     after each module, stops/masks the CIAs and turns the status screen's DMA back on. It does not
     recover arbitrary CIA timer latches, blitter/audio registers or module copper lists.
   - A module without `AMT_MF_OWNS_DISPLAY` runs with the status screen's DMA on (DMAEN|BPLEN|COPEN, unless `noscreen`): it must not depend on DMA being off, and should leave DMACON's display bits, COP1LC and the display registers alone. A module that needs them, or whose results depend on the DMA load, sets the flag.
   - No writes to unmapped addresses unless `amt_par.flags & AMT_F_UNSAFE`.
   - Autoconfig register/VRAM writes require `amt_board_claim()` on the canonical probe descriptor,
     before any selector/unlock access. It checks live BoardList membership, identity/span and an
     unbound ConfigDev, then consumes current `CDF_CONFIGME` under bare-phase task exclusion. Claims
     remain reserved through HALT/REBOOT and may be reused by our later modules. `unsafe`/`net` cannot
     override a driver claim; `amt_board_ok()` alone is only a structural/read-only guard.
5. **Case ids** are stable snake_case strings, unique within the module, and never renumbered: they are baseline keys. When one case runs per variant, put the variant in the id only if the expectation differs.
6. **Quick mode** (`AMT_QUICK()`) may run fewer iterations or operand sets, but should keep every case id present (fewer inputs per case).
7. Each module ships `docs/cases/<module>.md`: one line per case giving its intent, the rule it checks (with a manual reference), and what a failure usually means.

### Module ids
| id | name | phase | owner |
|---|---|---|---|
| 0 | takeover | OS | runtime |
| 1 | rt | BARE | runtime self-test |
| 2 | cpu_basic | BARE | integer ISA, every opcode family and addressing mode |
| 3 | flags | BARE | CCR/X-flag chains across blocks |
| 4 | exceptions | BARE | all exception types and frame formats per variant |
| 5 | smc | BARE | self-modifying code, prefetch, caches |
| 6 | interrupts | BARE | Paula interrupt levels, nesting, STOP, latency |
| 7 | cia | BARE | CIA-A/B timers, TOD, ICR, SDR |
| 8 | blitter | BARE | minterms, shifts, masks, fill, line, sizes |
| 9 | fpu | BARE | directed FPU edge cases per FPU type |
| 10 | timing | BARE | throughput fingerprint (perf records) |
| 11 | devio | OS | trackdisk/scsi.device I/O on signed test-owned media (disk: exp) |
| 12 | display | BARE | Denise/Lisa: modes, palettes, playfields, sprites, collisions, AGA (disk: core) |
| 13 | copper | BARE | copper instruction set, timing, danger bit, register classes (disk: core) |
| 14 | paula | BARE | audio DMA/interrupts/modulation, disk controller DMA/MFM/sync (disk: core) |
| 15 | input | BARE | keyboard, mouse/joystick counters, pot ports, CD32 pad, via host requests (disk: core) |
| 16 | rtg | BARE | graphics cards, register level (disk: exp) |
| 17 | expsound | BARE | sound cards, register level (disk: exp) |
| 18 | network | BARE | network cards, register level (disk: exp) |
| 19 | misc_hw | BARE | RTC, Gayle/PCMCIA, Akiko, CDTV hardware (disk: exp); Zorro autoconfig is module 24 |
| 20 | mmu | BARE | 030/040/060 MMU (disk: cpu) |
| 21 | memory | BARE | memory paths per bank type, widths, bank boundaries, RMW (disk: exp) |
| 22 | dispatch | BARE | seeded random code blocks, gencode.py (disk: cpu) |
| 23 | dmacontention | BARE | CPU vs blitter/copper/bitplane/audio DMA (disk: core) |
| 24 | misc_hw_os | OS | Zorro autoconfig consistency: ConfigDev ExpansionRom, sizes, address ranges, overlaps with boards and memory (OS-phase half of misc_hw; disk: exp) |
| 25 | memory_os | OS | allocates the per-region test blocks of memory (21) while exec is alive (OS-phase half of memory; disk: exp) |
| 26 | hostspeed | BARE | timed CPU/FPU/memory/I/O blocks for the runner's real-time check or standalone benchmark disk; `speed` records, no cases (disk: exp) |
| 27-63 | reserved | | later |

Disk groups are in disk/Makefile (`DISK_SET_*`). `make check-groups` fails if a module is in no group or in two.

### Building and testing one module in parallel with others
```
cd disk && make B=build-<name> MODS="rt <name>" && make B=build-<name> MODS="rt <name>" check
python3 tests/smoke_disk.py --adf disk/build-<name>/amitorture.adf --model A1200 --cpu 68020 --chipset aga --rom <path> --port <your range>
```
Temporary files go in the repo's `work/` (never /tmp, which is a small tmpfs here).

---------------------------------------------------------------------------
## 11. Test-owned media (devio, module 11)

The devio module writes to a disk unit ONLY when the unit carries this
ownership signature in **both** its first block (LBA 0) and its last block
(LBA total-1). Anything else is read at most (block 0, to look for the
signature) and never written. `runner/amitorture/media.py` creates such media
(ADF for DF1, plain hardfile, RDB hardfile) and gives the Amiberry options that
attach them.

### Signature block (512 bytes, big-endian)
| Off | Type | Field |
|---|---|---|
| 0 | char[8] | magic `"AMTOWN01"` |
| 8 | u8[16] | run UUID: random per created medium set; the same in both signed blocks |
| 24 | u32 | block number: the LBA of this block (0, or total-1) |
| 28 | u32 | `total`: number of 512-byte blocks of the medium |
| 32 | u32 | block size = 512 |
| 36 | u32 | `first_free`: first block the test may write; the writable region is `[max(first_free,2), total-1)` |
| 40 | u32 | kind: 1 = ADF, 2 = plain hardfile, 3 = RDB hardfile, 4 = PCMCIA SRAM card |
| 44 | char[64] | ASCII note, NUL-padded (`amitorture test-owned medium: contents may be overwritten`) |
| 108 | | zero |
| 508 | u32 | CRC-32 (zlib/IEEE) of bytes 0..507 |

A unit is **owned** iff block 0 and block `total-1` (with `total` taken from
block 0) both read without error, both have the magic and a correct CRC, carry
the same UUID and the same `total`, block size 512, and block numbers 0 and
`total-1`. The capacity the device reports (TD_GETGEOMETRY, SCSI READ CAPACITY
or TD_GETNUMTRACKS) is compared with `total` as a separate case; it does not
decide ownership. These metadata queries (and TD_CHANGESTATE/TD_PROTSTATUS) are
sent to every unit, owned or not: they read no medium data, and the I/O buffer's
memory type comes from TD_GETGEOMETRY's `dg_BufMemType`.

Before anything is written, devio also requires (otherwise "owner" FAIL, or SKIP
for the size limit, and the unit is not written):
- both blocks have the same `first_free` and `kind`, block size 512, `kind` 1-4,
  kind 1 (ADF) exactly on trackdisk.device;
- the writable region `[max(first_free,2), total-1)` is not empty;
- `total` <= $800000 (4 GB): devio uses the 32-bit `io_Offset` (no TD64/NSD),
  and media.py refuses hardfiles of 4096 MB or more;
- for a hardfile (kind 2/3), blocks 1-15 (where the OS looks for an RDB): no
  `RDSK` on a plain hardfile; on an RDB hardfile exactly the test-owned RDSK at
  block 1 (valid checksum, no bad-block or filesystem-header list) whose
  partition list is empty or is the one `AMTTEST` PART at block 2
  (PBFF_NOMOUNT, DosType 0, no next). A test-owned RDB disk that the user has
  since partitioned keeps both signatures, but is no longer test-owned.

### Layouts made by media.py
- **ADF** (DF1): 1760 blocks; signature in blocks 0 and 1759, `first_free` 2;
  no DOS bootblock (it must not be the boot disk).
- **Plain hardfile**: N MB rounded down to whole 16-head × 63-sector cylinders
  (so a CHS translation gives back the exact size); signature in block 0 and
  the last block; `first_free` 2; attached with geometry 63/16. Only useful on
  the UAE controller (uaehf.device): on an IDE/SCSI controller Amiberry puts a
  virtual RDB in front of a partition hardfile (hardfile.cpp
  create_virtual_rdb), so the device's LBA 0 is not the file's block 0 and the
  unit is (correctly) not test-owned.
- **RDB hardfile**: same size rule; block 0 signature, block 1 a valid `RDSK`
  (checksum, HostID 7, no bad blocks, no filesystem headers, 2 cylinders
  reserved for the RDB area) with no partitions, or with one `PART` block at
  block 2 that is non-bootable, `PBFF_NOMOUNT` and DosType 0, so the OS mounts
  nothing; `first_free` = 2 × 1008; last block signature. Attached as a whole
  disk (geometry 0,0,0), so the Kickstart scsi.device / board ROMs see it.

- **PCMCIA SRAM card** (every A600/A1200 entry): 4 MB of common memory (8192 blocks; signature in block 0 and
  block 8191, `first_free` 2), then 4 KB of attribute memory. Amiberry maps the bytes after 4 MB of the image as
  the card's attribute memory (gayle.cpp).
  - The attribute memory holds a CIS on its even addresses: CISTPL_DEVICE (SRAM, 4 MB), CISTPL_FORMAT (disk,
    512-byte blocks, the whole card), CISTPL_FUNCID (memory), CISTPL_END. Without CISTPL_FORMAT, Kickstart 3.1 adds
    the card to the system memory list.
  - It is inserted with `pcmciasram_rom_file=:ENABLED`, `pcmciasram_rom_options=inserted=true` and
    `hardfile2=rw,CC0:<img>,0,0,0,512,0,,custom0_pcmciasram`.

### Test blocks written by devio
Each 512-byte block written in the region is: `"AMTD"`, u32 LBA, u32 test id,
u32 seed, then 248 words of xorshift32 output seeded from (LBA, test id, seed).
They are rewritten on every run; the signatures and the RDB area are never
written.

TODO (milestone 3): the DF1 results-disk transport from the plan will reuse
this signature; it is not implemented yet.

---------------------------------------------------------------------------
## 12. Disk set

The test ships as three ADFs, built by `make -C disk` (or `make -C disk set`) into `disk/build/set/`:

| ADF | Modules | Code + data in memory |
|---|---|---|
| `amitorture-1-core.adf` | takeover rt flags exceptions smc interrupts cia blitter timing display copper paula input dmacontention | ~345 KB + ~66 KB |
| `amitorture-2-cpu.adf` | cpu_basic mmu dispatch | ~229 KB + ~241 KB |
| `amitorture-3-exp.adf` | fpu rtg expsound network misc_hw misc_hw_os devio memory memory_os hostspeed | ~234 KB + ~290 KB |

- Each ADF is a complete payload (runtime + probe + its modules) and boots on its own. The minimum machine is 1 MB: 512 KB chip + 512 KB slow RAM on Kickstart 1.x (A500). The two segments (§1) are allocated separately, so neither has to fit in one region together with the other.
- Every module is on exactly one disk, so (mod, case) is unique across the set.
- The runner boots each config once per disk and merges the records into one result per config.
- On real hardware / FPGA, boot each disk in turn and `import` the captures together (docs/USAGE.md section 5). The build id of each disk ends with `<n>/<N> <group>`: the status screen shows it and says which disk to insert next, and `import` matches the boots to their disks by it.
- `make` (no target) builds the disk set. All modules together no longer fit on one 880 KB ADF; `make MODS="rt <modules>"` builds a single ADF with a subset (module development, repro packs).

Module grouping lives in disk/Makefile (`DISK_SET_*`). Keep each segment under ~400 KB so that it fits the free chip or slow RAM of a 512 KB + 512 KB A500 with the module pools (blitter, display, dmacontention, rtg, mmu) still allocatable, and keep every disk within 1760 sectors.

---------------------------------------------------------------------------
## 13. Host request channel

Used for things the guest can't do or observe itself: screenshots, audio capture marks, input injection, CD/disk swaps.

1. The guest calls `amt_request(what, tag, args_json, timeout_frames)`, which emits
   `{"t":"request","id":N,"what":"<what>","tag":"<tag>","args":{...}}`.
   It then polls mailbox offset 44 (`host_ack`) until it equals N, or the timeout runs out.
2. The host (runner) acts on the request, then writes N to `host_ack` with IPC WRITE_MEM (4 bytes at mailbox+44), and writes a `host` record to its own results: `{"t":"host","id":N,"what":…,"result":{…}}` (file names, hashes, injected events).
3. On timeout the guest reports the dependent case as `untestable` ("host did not act on request <what>"). On real hardware / serial-only targets every request times out, so a timeout is never a fail.
4. A host that cannot do a request (an unknown `what`, or no joystick/CD32-pad/PCMCIA injection in Amiberry's IPC) writes N | $80000000 to `host_ack`: the guest stops waiting at once and reports the case as on a timeout. Request ids are small counters, so the high bit is free.

| what | args | host action / result |
|---|---|---|
| `screenshot` | `{"frames":n}` (optional: wait n frames first) | IPC SCREENSHOT to `raw/<entry>/<disk>/shot-<id>-<tag>.png`; result: path, sha256, size. Baselines fingerprint the hash for cycle-exact entries only. |
| `audio_mark` | `{"edge":"start"|"stop"}` | marks a segment in the captured audio stream (SDL disk audio driver); result: byte offsets. Host-side analysis gives frequency/amplitude/timing per segment, written into the host record. |
| `key` | `{"code":n,"up":0|1}` (Amiga raw key code 0..$7F; bit 7 is the `up` field, not part of `code`) | IPC `SEND_KEY <code> <1 down / 0 up>` (Amiberry records the code into its keyboard buffer; the emulated keyboard then sends it over CIA-A SDR and waits for the handshake). Ack after the IPC reply. |
| `mouse` | `{"dx":n,"dy":n,"buttons":m,"port":p}`: dx/dy in mouse counts (+dx right, +dy down/toward the user; one count = one JOYxDAT step); buttons absolute, bit0 left, bit1 right, bit2 middle | IPC `SEND_MOUSE <dx> <dy> <buttons>`. It drives host mouse device 0, which reaches the guest only on the port with `joyportN=mouse`: ack only for that port. Exact counts need mouse speed 100 (`SET_MOUSE_SPEED`/`input_mouse_speed`, Amiberry scales dx/dy by speed/100) and no tablet/mousehack mode (relative moves are dropped then). |
| `joy` | `{"port":p,"dirs":"<subset of udlr>","fire":b}`: absolute state, "" = centred; b bit0 = fire 1 (pin 6), bit1 = fire 2 (pin 9) | IPC joystick injection, if the emulator has one. Amiberry's IPC has none (only SEND_KEY, SEND_MOUSE, SET_JOYPORT_MODE; SEND_KEY codes > $7F are AKS_* input events, not joystick events): not acked. |
| `cd32pad` | `{"port":p,"buttons":mask}`: absolute; mask bit0 blue, 1 red, 2 yellow, 3 green, 4 forward, 5 reverse, 6 play (the pad's shift order) | as above (not acked on Amiberry). A host that has a CD32 pad attached on that port (Amiberry: `joyportN=<device>`, `joyportNmode=cd32joy`, e.g. joyport1=kbd1) but cannot press its buttons may ack `buttons:0` only: the guest then checks the pad id/released state. |
| `wait` | `{}` | host acks immediately (round-trip/latency test) |
| `pacing` | `{}` (tag `boot`) | sent once by the runtime right after `hello`, before the first module. A host that warped the boot (Kickstart, loader, probe) switches the emulator to the disk's own pacing (Amiberry: IPC `SET_WARP`, checked with `GET_WARP`) and acks only after that; result: `warp`, `was`, `switched`. A host that loaded a disk testing the floppy hardware at turbo drive speed sets real drive speed here too (Amiberry: IPC `SET_FLOPPY_SPEED 100`, checked with `GET_FLOPPY_SPEED`), before the ack; result `floppy_speed`. A host that cannot switch refuses it (N \| $80000000). Either way the guest just continues: this request has no case. |

Unknown `what` values are never acknowledged.

Runner notes (runner/amitorture/hostreq.py, docs/RUNNER.md "Host request channel"):
- `mouse` reaches the guest only on a port whose config has `joyportN=mouse`: SEND_MOUSE moves the host mouse device 0.
- `joy` is never acked by Amiberry runs, because its IPC has no joystick injection.
- `pcmcia` (`{"op":"eject"|"insert"}`, used by misc_hw) is not served. Amiberry's IPC has no PCMCIA eject/insert command: gayle.cpp's pcmcia_eject/pcmcia_reinsert are reached only from the GUI's hardfile change. It stays an unknown `what`, never acked.
- `cd32pad` with `{"buttons":0}` is acked when the port is a `cd32joy` port (CD32 entries: port 1), because nothing can press a button there. Any other button mask is never acked.
- `audio_mark` is acked only when the launch captures audio (the default `--audio auto` captures every disk that contains an `audio_mark` request).
- **Expectations in the marks.** The start mark may describe the tone with `expect_hz`, or `freq_mhz`, or `clock` + `period` + `samples`, and with `side`: `left`/`right` (the tone on that output alone), or `both` (a mono source: on both outputs, the quieter at least half the louder). The stop mark may repeat or override any of these. The host then reports `freq_ok` / `side_ok` in its analysis.

The mailbox ring size is defined by `MBOX_RING_SIZE` (`disk/rt/rt.h`, section 7 above).
The host must read `ring_size` from the header, never assume it.

---------------------------------------------------------------------------
## 14. CLI start and CD boot (CD32, CDTV)

The CD32 and the CDTV boot from CD, so on those entries the payload runs as an AmigaDOS program instead of from the bootblock.

### CLI entry (`disk/rt/start.s`)
- Offset 0 of the image (the start of the first hunk) is `amt_cli_entry`. AmigaDOS starts a program there, with A0/D0 = the argument line and no SysBase in A6.
- The shim loads SysBase from address 4, sets A0 = the embedded header copy, A1 = **bootio = NULL**, A2 = the image base, A3 = A2 + `alloc_size` (where the CD image puts segment 1), and calls `amt_entry`. If `amt_main` ever returns, the shim returns RETURN_OK (0) to the CLI.
- The bootblock still jumps to `amt_entry`. mkadf takes `entry_offset` from the `_amt_entry` symbol, so it is no longer 0.
- **Header slot:** the data section holds the 16-byte marker `AMT-CLI-HEADER!!`, followed by 512 bytes. The runner copies the ADF's payload header (sector 2: run parameters, build id) into those 512 bytes. All zeros means default parameters and an empty build id.
- **bootio = NULL:** the runtime and the modules use `amt_bootio` nowhere: devio opens its own units. A module that ever needs the boot IOStdReq must check it for NULL and skip that path.
- **CLI start means live filesystems.** `amt_bootio == NULL` tells the code that DOS, the filesystems and their device drivers are running (also when the suite is started from a hard disk). Before the takeover the runtime (rt/mod.c "takeover guard") waits 2 s, then up to 10 s for disk DMA (DSKBYTR DMAON), the blitter (BBUSY) and Gayle/A4000 IDE (alternate status BSY/DRQ) to be idle; if they stay busy it refuses the takeover and every bare module reports one `module` skip with the reason. The disk DMA and blitter wait also applies to the bootblock start. misc_hw does not touch the Gayle IDE task file in CLI mode unless the unsafe flag is set.

### The CD image (`runner/amitorture/cdimage.py`)
The image is a plain ISO 9660, built by the runner from the already parameter-baked ADF (stdlib only; deterministic, with fixed dates):

| Path | Content |
|---|---|
| `AMITORTURE` | A one-hunk executable: HUNK_CODE = the ADF's segment 0 (padded to `alloc_size`), then segment 1 (data, then the BSS as zeros), with the header copied into the slot. HUNK_RELOC32 = the ADF's reloc table, converted to offsets in that one hunk. |
| `S/STARTUP-SEQUENCE` | `AMITORTURE` |
| `CD32.TM` (2048 bytes) or `CDTV.TM` (22152 bytes) | The trademark file of the machine. The PVD's application-use area (byte 884) has a `TM` tag pointing at it. |

- **PVD system identifier** is `CDTV`, padded to 32 characters. The cdfs of both ROMs accepts `CDTV` or `COMMODORE_AMIGA` there.
- **Size.** The image is padded to 4 MB: the CD32 ROM reads ahead 128 sectors from LBA 16, and a disc shorter than that read does not boot.
- **Where the ROM finds the trademark.** The cdfs of both ROMs parses tags in the PVD's application-use area, starting at byte 884: u16 tag, u16 length, data, ended by tag 0. The tag `TM` (`0x544D`, 20 bytes) gives the file length and up to four LBAs of copies.
- **Trademark check.** Both boot ROMs compare the trademark file on the disc byte for byte with a copy kept in the ROM, compressed with debox.library (method 1 LZSS, method 2 column RLE, nested chunks):
  - **CD32** (cdstrap r40.60) reads one sector at the TM's first LBA and compares it with its CD32.TM. If that does not match, it compares it with the first 2048 bytes of the CDTV one.
  - **CDTV** (bootstrap CheckTM → playerprefs.library LVO -138, which then shows it as the boot screen's trademark picture) needs the length and the contents to equal its CDTV.TM. That file is two debox streams in a row: 6144 bytes, then 16008 bytes (two 40×200 bitplanes and a 4-colour palette).
- **Where the trademark data comes from.** These files are Commodore's, so they are not in the repository. The runner decompresses them from the user's own extended ROM at run time and identifies them by CRC-32: CD32.TM is 2048 bytes with CRC `e56c340f` (Amiberry's `src/blkdev.cpp` knows that one as "[CD32.TM]"), CDTV.TM is 22152 bytes with CRC `37c532d7` (it is in the CDTV ROMs 1.0, 2.30 and 2.7).
- **No AmigaDOS files from disk are needed.** The Kickstart's CLI runs the startup-sequence, and the extended ROM's cdfs mounts the disc as CD0:/SYS:.

### Configuration
- `cdimage0=<iso>,image`, `nr_floppies=0`, `floppy0type=-1`. Test floppies (DF1) are dropped, so there is no floppy drive, as on a real CD32. The CDTV is booted the same way.
- `chipset_compatible=CD32|CDTV` turns on Akiko or the DMAC and the CD drive.
- Results come back over serial and the mailbox as usual.
