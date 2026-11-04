# Using amitorture

This guide covers running amitorture on an emulator and reading what comes out. Building the test disks and changing the tool are covered in [BUILDING.md](BUILDING.md). The command reference is [RUNNER.md](RUNNER.md), and every record and result field is described in [schema.md](schema.md).

All runner commands are run from `runner/` as `python3 -m amitorture <command> …`. `--help` works on every command.

---------------------------------------------------------------------------
## 1. What you need

1. **The test disks:** `disk/build/set/*.adf`. Build them once with `make -C disk` (see [BUILDING.md](BUILDING.md)). The runner finds them by itself.
2. **Python 3.10+.** The runner uses the standard library only.
3. **An emulator binary.** amitorture never builds Amiberry. Pass `--amiberry /path/to/amiberry`, or have `amiberry` on `PATH`.
4. **Kickstarts.** Put your ROM files in one directory; file names don't matter. The runner uses `~/Amiberry/ROMs` when it exists; otherwise pass `--kickstarts DIR` or set `AMT_KICKSTARTS`. Check what was found:
   ```
   python3 -m amitorture roms --kickstarts ~/Amiberry/ROMs          # --recursive for subdirectories
   ```
   This lists every file and what it was identified as (by CRC32/SHA1), then every matrix entry with the ROM it will use, or why it will be skipped.
   - Encrypted Cloanto ROMs need `rom.key` in the same directory.
   - AROS entries need no ROM file: they use the AROS ROM built into Amiberry.
5. **Sanity check** (no test disk, about 30 s):
   ```
   python3 -m amitorture run --amiberry /path/to/amiberry --kickstarts ~/Amiberry/ROMs --config a1200-68ec020-fast --selftest
   ```
   It checks that Amiberry starts isolated, the IPC socket, serial-over-TCP and reading the guest's mailbox.

---------------------------------------------------------------------------
## 2. Everyday workflows

Times below were measured on a Raspberry Pi 500 with 4 emulators in parallel (`--jobs 4`).

### "Is this Amiberry build OK?" (quick)
```
python3 -m amitorture run --amiberry ./amiberry --kickstarts ~/Amiberry/ROMs --matrix smoke --jobs 4
```
`smoke` covers 10 machines (68000 to 68060, OCS/ECS/AGA, PAL/NTSC, AROS) with JIT twins, each running every disk of the set. It takes about 25 minutes, plus up to the narrowing budget if something fails (see 3.3).

Exit code: 0 means no problems, 1 means problems found (read the report), 2 is a host-side error, 3 means the isolation check failed, 130 means the run was stopped with Ctrl+C (see "Stopping a run" below).

### Thorough runs
| Matrix | Entries | Time | What it adds |
|---|---|---|---|
| `smoke` | 10 | ~25 min + narrowing | one machine per CPU class and chipset |
| `standard` | 135 | ~6 h, including a 2 h narrowing budget | every CPU/FPU variant, every RTG, sound and network board, Gayle/PCMCIA, RTC boards, SCSI controllers, MMU configs, CD32, CDTV |
| `full` | 533 | not measured yet (about 4× `standard`) | every JIT sub-option, threaded and softfloat twins |

`python3 -m amitorture matrix --matrix standard` lists the entries.

### "Did my change break or fix anything?" (regression check)
1. **Once, with a build you trust,** record a baseline: 3 runs, so flaky cases are recognised.
   ```
   python3 -m amitorture baseline --amiberry ./amiberry-good --kickstarts ~/Amiberry/ROMs \
       --matrix smoke --name good --repeat 3 --jobs 4
   ```
2. **Then, for every new build:**
   ```
   python3 -m amitorture check --amiberry ./amiberry-new --kickstarts ~/Amiberry/ROMs --baseline good --jobs 4
   ```
   The result is `work/results/check-<time>/REPORT.md`:

   | Kind | Meaning |
   |---|---|
   | `regression` | a case that was stable in the baseline now gives a different result. Counted. |
   | `fixed` | it failed in the baseline and passes now |
   | `became_unstable` | it was stable, now it differs between runs. Counted. |
   | `fail_detail_changed` | it still fails the same way and only the context changed (addresses after a rebuild). Informational. |
   | `perf_out_of_band` / `timing_out_of_band` | speed moved; informational when the entry runs at `cpu_speed=max` |
   | `unstable_excluded` | it was already flaky in the baseline, so it isn't compared |
   | `new` / `missing` | the case ran only in the check / only in the baseline. `missing` is counted. |
   | `entry_status` | the machine as a whole ended differently (crash, hang, skip …) |

   Every finding lists the config options that differ from the baseline, so a deliberate `--set` shows up pinned to the cases it changed.
3. **Bisecting without building:** pass several binaries, oldest first:
   ```
   python3 -m amitorture check --amiberry ./a ./b ./c --baseline good
   ```
   The report says, for each changed case, the first binary where it changed.

Baselines live in `work/baselines/<name>/` and are not part of the repository: record your own from a build you trust (it takes about 70 minutes for `smoke` × 3).

### Only some machines or some settings
```
--config 'a1200*'                      # glob on entry ids; repeatable
--config a4000-68040nommu-fast-jit     # one entry
--set gfx_resolution=superhires        # override an Amiberry option on every selected entry
--adf ../disk/build/set/amitorture-6-cpux.adf   # one disk of the set instead of all of them
--param modules=0x400000               # module mask patched into the disks (bit n = module id n,
                                       #   ids in CONTRACT.md §10); 0x400000 = dispatch, which is on
                                       #   the cpux disk, so combine it with the --adf above
--disk-set DIR                         # another disk set (e.g. one built from a branch)
--timeout S --hang S                   # see section 6
```

### Boot one disk quickly, without the runner
```
python3 tests/smoke_disk.py --model A1200 --cpu 68020 --chipset aga --rom /path/to/kick31.rom \
    [-s option=value …] [--adf disk/build/set/amitorture-4-chip.adf]     # --rom aros for the AROS ROM
```
Run it from the repository root. It boots the core disk by default, prints every record and checks the CRCs. It doesn't answer host requests (screenshots, key/mouse injection, audio), so those cases come out `untestable` here.

- `--screenshots DIR` saves the emulator's screen as `DIR/shot-NNN.png` every `--shot-every S` seconds (default 5), and once more after `done`. The emulator runs headless, so this is how you see the disk's status screen (below).
- Ctrl+C stops the emulator and exits with code 130.

### Stopping a run (Ctrl+C)
Press Ctrl+C once (SIGTERM does the same). The runner starts no new emulator and stops the running ones: SIGTERM first, SIGKILL for any still alive after 5 s. Then it exits with code 130.
- **`run`** writes a partial report of the entries that finished. The verdict starts with `INTERRUPTED (n of m entries finished)`, and REPORT.md lists the entries that did not run. An entry whose disks only partly ran is left out. Parity and narrowing are not done.
  - Stopped after every entry had run, i.e. during the analysis (narrowing): the verdict starts with `INTERRUPTED (all entries finished; parity, narrowing and repro packs not done)`. `python3 -m amitorture report DIR` rebuilds parity and the repro packs from it. Narrowing needs a new run.
- **`baseline`** writes nothing. A new baseline is recorded into `runs.new/` first and replaces an existing baseline of the same name only once it is complete, so the old one stays exactly as it was.
- **`check`** and the other commands stop without a report.

A second Ctrl+C kills the emulators and quits at once. Temp files under `WORKDIR/tmp` are left behind then.

### The status screen
Every disk shows what it is doing on the Amiga's own display: the emulator window, or a real machine's monitor. The runner's emulators run headless, so there you see it only with `tests/smoke_disk.py --screenshots` or a Amiberry window of your own (open the ADF as DF0).

| Rows | Content |
|---|---|
| title | `AMITORTURE  disk n of N: <group>`. A single ADF built with `MODS=…` says `test disk`. |
| build | the disk's build id (git revision and date) |
| machine | CPU, FPU and MMU; chipset, PAL/NTSC and Kickstart version; chip and fast RAM in KB and the number of expansion boards, as the probe found them |
| modules | one row per enabled module (OS-phase modules first): PASS, FAIL, SKIP, N/T (untestable), and a state |
| TOTAL | the totals, and the elapsed time of the bare phase (m:ss, counted in frames once the first bare module has run) |
| messages | what is happening; at the end the verdict, the serial settings and what to do next |

| State | Meaning |
|---|---|
| `-` | not run yet |
| `run` | running |
| `run*` | running, and the module owns the display hardware: the status screen is off until it ends (the message lines say so first). Blitter, copper, display, dmacontention, interrupts, paula and timing do this. |
| `ok` | finished: no failure, at least one pass or untestable |
| `FAIL` | at least one case failed |
| `skip` | finished with skips only |
| `ABRT` | the module aborted (unexpected exception or watchdog) |

The background is blue while the disk runs, green when it finished without failures, and red when a case failed or a module aborted. At the end the screen says `FINISHED: no failures.` or `FINISHED: n failed case(s).`, then `Next: insert disk n, then reset.` (or `That was the last disk of the set.`), and `Halted. Reset to continue.` with the default end action.

**Cost, and turning it off.** The screen's bitplane and copper DMA stay on while the other bare modules run. The core disk took 86 s with the screen and 82 s without it on a chip-only A1200. The run flag `noscreen` turns the screen off; every bare module then starts with all DMA off, as before the screen existed:
```
--param flags=noscreen          # runner (run, baseline, check); for a disk file see section 5
```
`check` takes only the mode, seed and end action from the baseline, not its `--param`: give a check the same `--param flags=noscreen` as its baseline. A disk also runs without the screen if the screen's 8 KB of chip RAM isn't free once the test pools are allocated.

---------------------------------------------------------------------------
## 3. Reading the report

Start with `work/results/<run-id>/REPORT.md`. It is written to be read by an AI as well as a person; every statement points to files in the same directory.

### 3.1 The top
- **Coverage:** modules × entries with pass/fail/skip/untestable/xfail/engine_mismatch counts, then the capability catalogue ([catalogue.yaml](catalogue.yaml)) with covered/partial/not_covered per run. Anything `not_covered` or only `untestable` was not tested in this run.
- **Entries:** one row per machine, with a status (worst over its disks). The statuses:

  | Status | Meaning |
  |---|---|
  | `pass` | done, no failures |
  | `fail` | at least one case failed (very common: most machines hit a known Amiberry bug) |
  | `crash`, `hang`, `timeout` | the emulator crashed, stopped making progress, or ran too long |
  | `incomplete` | records were lost, or a module aborted |
  | `skip` | a ROM is missing |
  | `error` | host-side trouble |

- **"As designed":** `no` means Amiberry changed or ignored an option of that config (see "config adjusted by the emulator"). Such an entry is not counted as run as intended.
- **Engine parity:** each JIT, threaded or softfloat twin against its reference entry. The same tests must give the same records; differences are `engine_mismatch`.

### 3.2 Case statuses
| Status | Meaning |
|---|---|
| `pass` / `fail` | judged against the manual rule in the case's `why` |
| `skip` | not applicable to this machine (the reason is given) |
| `untestable` | could not be tested here: the host did not act on a request (screenshot, key/mouse, audio), hardware held a line (CD32 INT2), the module was refused the takeover, etc. Never a pass. |
| `xfail` | a failure matching a known emulator bug in `runner/amitorture/xfail.json` |
| `fixed` | an xfail case that passes now |
| `unstable` | the same config gave different results on reruns (intermittent) |

### 3.3 One problem section
Failures that share a cause are grouped into one problem: same module, same narrowing result, same failing entries, same differing fields. Each problem has:
- **Title:** the cases and the differing fields, plus the narrowing result, e.g. "fails only with JIT on", "fails with every neighbouring setting tried", or "not reproduced in N rerun(s) of the module alone", which means intermittent. A title starting with "[config contradicts the cases' stated condition]" means the case itself says it only passes with another option value. That is a configuration note, not a bug.
- **Kind, module, severity, scope:** how many cases on how many entries.
- **fails on / passes on:** the entries where it fails and the ones where the same cases pass. Comparing their configs is often enough to find the setting involved.
- **Representative failure:**

  | Field | What it holds |
  |---|---|
  | `in` | inputs |
  | `expect` / `got` | what should have happened, and what did |
  | `why` | the rule, with a manual reference |
  | `ctx` | context: opcode words, disassembly, frames, registers |

  Then the case's line from [docs/cases/](cases/) (intent, rule, what a failure usually means), and the evidence for the expectation if the module recorded one.
- **Where to look in the emulator:** the Amiberry files and functions to look at, worked out from the config and the opcode ([emulator-map/amiberry.md](emulator-map/amiberry.md)). For CPU cases this is the generated handler, e.g. `op_4c90_0_ff` in `cpuemu_0.cpp`. For JIT-only problems it is the backend files of the host architecture. They are hints, not verdicts. They need the Amiberry source next to the binary, or `--amiberry-src DIR`.
- **Emulator log while the module ran:** suspicious lines from Amiberry's log in that module's time window (dummy_bank accesses, JIT faults, MMU messages …), with counts.
- **Settings that change what the module's cases may expect:** the resolved emulator options that matter for that module, for each failing entry.
- **Repro pack:** `repro/<problem>/`. See 4.

**Narrowing.** When a case fails, the runner reruns only that case on neighbouring configs: JIT off/on and each `comp_*` option, core mode, `mmu_model=0`, 24-bit addressing, `gfx_resolution`, FPU native/softfloat, threaded off, and so on. It records which setting flips the result. The default budget is 2 hours (`--narrow-budget S`, or `--no-narrow`). "Budget exhausted" means some variants were not tried; the report lists them.

### 3.4 Machine-readable copies
| File | Contents |
|---|---|
| `summary.json` | the whole report |
| `failures.jsonl` | one line per failure |
| `narrowing.json` | every narrowing launch |
| `environment.json` | binary sha256 and version, ROM ids, host |
| `configs/` | the exact Amiberry configs and the emulator's resolved view of them |
| `raw/<entry>/<disk>/` | every record (`records.jsonl`), the serial capture, mailbox reads, `amiberry.log` |

`python3 -m amitorture report DIR` rebuilds REPORT.md from that data without the emulator. That's useful after editing `xfail.json`; add `--xfail FILE` to use another registry.

---------------------------------------------------------------------------
## 4. Working on a bug

1. **Find it** in REPORT.md (3.3). The record's `why` and `expect` give the manual rule; the code pointers and log lines say where to look.
2. **Reproduce it from the repro pack** (one disk with only that module, the minimal config):
   ```
   python3 -m amitorture repro work/results/<run>/repro/<problem> --amiberry ./amiberry   # prints the case result
   python3 -m amitorture repro … --gdb       # starts Amiberry's GDB server; see gdb.cmd (m68k-amigaos-gdb)
   ```
   Each pack also has `run.sh` and `host-gdb.cmd`, with breakpoints on the suggested emulator functions.
3. **After a fix,** rerun the pack, then `check` against your baseline. The case should come out `fixed` and nothing else should change.
4. **Known bugs** live in `runner/amitorture/xfail.json` (key `<module>.<case>`, globs allowed, optional `configs` glob and `when` option conditions, `ref`/`why` text; a key may hold a list of entries when one case has several independent known causes). Add an entry when a finding is confirmed, so it shows as `xfail` instead of a new problem; the runner reports it as `fixed` once it passes. The findings themselves are documented in `docs/findings/`.

---------------------------------------------------------------------------
## 5. Real hardware and FPGA (not tested yet)

The disks are meant to run unchanged on a real Amiga or an FPGA core. **Nobody has run them there yet:** everything in this section comes from the code, not from a run on real hardware.

### What you need
- **The seven ADFs** from `disk/build/set/` (section 1), in one of these forms:
  - written to real DD floppies, with an ADF transfer tool on an Amiga or a USB flux-level floppy interface on a PC (a PC floppy controller can't write Amiga tracks);
  - as image files for a floppy emulator in the drive (Gotek, HxC), on its USB stick or SD card;
  - as image files on an FPGA core's SD card, inserted as DF0.

  Each disk boots from DF0 on its own. No Workbench, no hard disk and no OS files are needed, only the Kickstart in the machine.
- **Memory:** 512 KB is enough. Every disk of the set is built to fit a 512 KB Kickstart 1.x machine (each payload is under about 300 KB, CONTRACT §12). If a disk can't get its test pools, each of its modules reports one `module` skip "not enough memory on this machine for this disk".
- **To get the results out:** a null-modem cable from the Amiga's serial port to a PC, directly or through a USB serial adapter (below). An FPGA core must route Paula's serial port to a UART you can reach. Without serial you only have the status screen.

### Running the set
1. Start the serial capture (below), then boot disk 1, `amitorture-1-core.adf`.
2. Watch the status screen (section 2, "The status screen"). A module shown as `run*` turns the screen off while it runs: that is expected, not a hang. A stuck module is aborted by the watchdog after about 120 s without a reported case (×10 for the long modules) and shows `ABRT`.
3. When the disk has finished, the background turns green or red and the screen says `Next: insert disk 2, then reset.` Insert the next disk (or select the next image) and reset the machine.
4. Repeat until the screen says `That was the last disk of the set.`

Expect the disks to take longer than in the emulator. Nothing answers the host requests (screenshots, audio marks, key and mouse injection, CONTRACT §13): each one waits for its timeout (10 s in most modules) and its case comes out `untestable`.

### Capturing the serial output
The runtime drives Paula's UART itself (rt/out.c), polled, never through serial.device: **115200 baud, 8N1, no handshake** (no RTS/CTS, no XON/XOFF). The screen shows the rate the disk uses. On Linux, for example:
```
stty -F /dev/ttyUSB0 115200 cs8 -parenb -cstopb -crtscts raw -echo
cat /dev/ttyUSB0 > capture.txt
```
- One long capture across all seven disks works: each boot is told apart by its `hello` record, and the lines between them (noise around a reset) are ignored.
- Separate files work too, one per disk, with any names.
- **Baud rate accuracy:** Paula's rate is the colour clock divided by an integer, so 115200 comes out as 114416 baud on PAL (0.7% slow) and 115469 on NTSC (0.2% fast), well within what a PC UART accepts. If the capture is still garbled, or `import` finds bad CRCs or no records (a long or poor cable), use a lower rate such as 38400.

### Changing the baud rate or other parameters
The run parameters are in each disk's header (CONTRACT §4). `tools/mkadf.py` writes them into a copy (from the repository root):
```
python3 tools/mkadf.py --in disk/build/set/amitorture-1-core.adf -o hw/amitorture-1-core.adf --param baud=38400
```
For the whole set:
```
mkdir -p hw
for f in disk/build/set/*.adf; do python3 tools/mkadf.py --in "$f" -o "hw/$(basename "$f")" --param baud=38400; done
```
- Always give `-o`: without it mkadf rewrites the input file. It doesn't create the output directory.
- Keys: `mode=full|quick`, `seed=N`, `end_action=halt|reboot|return`, `baud=N`, `modules=all|MASK`, and `flags=` with `unsafe`, `noserial`, `nombox`, `net`, `noscreen` (comma-separated, or a number). `flags=` replaces all flags at once.
- Parameters not given keep the value already in the disk. `python3 tools/mkadf.py --dump FILE` shows them.

### Making the report
From `runner/`:
```
python3 -m amitorture import ~/capture.txt --out ../work/results/hw-a1200
python3 -m amitorture import ~/core.txt ~/cpu.txt ~/fpu.txt … --label hw-a1200     # -> work/results/hw-a1200
```
- **Inputs:** raw serial captures, or `mailbox.jsonl` files written by `run`. All files must come from one machine.
- **Disks:** each boot is matched to its disk by the build id in its `hello` record (it ends with `n/N group`), so the file names and their order don't matter. A serial capture and a mailbox dump of the same boot are merged as two transports of one disk run. Boot each disk once: a disk booted twice shows up as two runs (`core`, `core#2`).
- **Status:** the report has one entry, `imported`. Each disk gets its own status and the entry gets the worst. A disk without a `done` record is reported as "no done record: the capture stopped, or the machine hung, before this disk finished". The probe results of the disks are compared (free memory ignored): a difference, i.e. the disks did not all see the same machine, is reported as `probe_inconsistent`.
- **What you get:** the same REPORT.md, `summary.json` and `failures.jsonl`, with problems grouped, `xfail.json` applied and the docs/cases lines. There is no narrowing, no repro packs, no engine parity, and no code pointers or emulator log, because no emulator ran. The exit code is 0 for PASS, 1 otherwise.

### What the disks do to the machine
- **Memory and hardware:** a module touches only its pools, its own stack and BSS, the vector table (through `amt_vec_set`) and the hardware it tests. No writes to unmapped addresses unless the run has the `unsafe` flag (CONTRACT §10, rule 4). After every module the runtime resets the interrupts, DMA and vectors. At the end it restores the OS and, with the default end action, halts with the OS still running.
- **Floppies and hard disks:** nothing is written to a disk unit unless it carries the test-owned signature in its first and its last block (CONTRACT §11). devio (exp disk) reads only block 0 of any other unit, to look for the signature. The paula module (chip disk) only reads DF0, its own boot disk, and writes DF1 only when it is signed and not write-protected. Only the runner makes such media (`runner/amitorture/media.py`), for emulator runs, so your own disks are never written.
- **Takeover guard** (rt/mod.c): before it takes the machine over, the runtime waits until floppy DMA and the blitter are idle, and, when started from a CLI, the Gayle or A4000 IDE port too. If they are still busy after 10 s it refuses: nothing is taken over, and every bare module reports one `module` skip with the reason (CONTRACT §14).
- **Other hardware:** on a Gayle machine (A600/A1200) misc_hw sends IDENTIFY DEVICE to the IDE drive, a command that only reads, and restores the task file. The battery-backed clock is saved and set back at the end, adjusted for the time the test took. A network card sends no frame onto the network unless the `net` or `unsafe` flag is set (the A2065 tests use the LANCE's internal loopback; the one external frame is an ARP broadcast).
- **The `unsafe` flag** is off by default; leave it off on real hardware. It allows: a read of an unmapped address to provoke a bus error (exceptions), reading block 0 of units that are not test-owned (devio, read only), writes to the ZZ9000's RAM (rtg), probing for a Prelude 1200 on the clock port (expsound), transmitting on a network card (network), and the Gayle IDE tests when started from a CLI (misc_hw).

### CD32 and CDTV
These boot from CD (CONTRACT §14): for an emulator run, the runner turns each baked disk into a bootable ISO. It does that inside the launch's temp directory and deletes the ISO afterwards (`--keep` keeps it). The trademark file the boot ROM checks is unpacked from your own extended ROM. No command writes such a disc for a real machine, and no CD has been tried on one, so CD32 and CDTV runs are emulator-only for now.

### Not verified yet
- No run on a real Amiga or an FPGA core so far: the takeover and restore, the timing expectations and the serial rate tolerance are untried there.
- No DF1 results disk: the planned transport that writes the records to a signed DF1 is not implemented (TODO in disk/modules/devio.c, CONTRACT §11). Serial is the only way to get results off a real machine.
- The mailbox is only read by the runner, through the emulator's IPC. On real hardware nothing reads it.

---------------------------------------------------------------------------
## 6. Environment and housekeeping

| Setting | Effect |
|---|---|
| `--workdir DIR` / `AMT_WORKDIR` | everything amitorture writes goes here (default `work/` in the checkout): `results/`, `baselines/`, `tmp/` |
| `--kickstarts DIR` / `AMT_KICKSTARTS` | the ROM directory, read only (default `~/Amiberry/ROMs` when it exists) |
| `AMT_MAX_EMU=N` | at most N emulators machine-wide (default and maximum: `nproc`). The limit is shared by every amitorture process and checkout (lock files in `$AMT_LOCK_DIR`, default `$XDG_CACHE_HOME/amitorture/locks`, else `~/.cache/amitorture/locks`). |
| `--jobs N` | entries in parallel for one run (≤ the emulator slots) |
| `--ports LO-HI` / `AMT_PORTS` | TCP port range for the emulators' serial ports (default 23540-23599) |
| `--timeout S` / `--hang S` | per-disk launch limit (default 600 s) / no-progress limit (default 120 s) |
| `--audio auto|on|off` | capture emulator audio for audio checks (default: only disks that ask for it) |
| `--keep` | keep per-run temp directories (otherwise removed) |
| `--show` | show the emulator windows on your desktop (`run`, `baseline`, `check`, `repro`, and `tests/smoke_disk.py`); one window per running emulator, so `--jobs 4` gives four. Your own display settings are used as they are (your `SDL_*` video variables, `DISPLAY`, `WAYLAND_DISPLAY`); audio stays the runner's. HOME and `XDG_RUNTIME_DIR` stay isolated (Amiberry names its IPC socket `$XDG_RUNTIME_DIR/amiberry.sock`, which a test emulator must never take from your own Amiberry), so a relative `WAYLAND_DISPLAY` is passed as the full path of the same socket and an unset `XAUTHORITY` as your `~/.Xauthority`. Run it from a terminal on the desktop. Timing values may differ from a headless run. |
| `--end-action halt|reboot|return` | what the disk does when finished. The runner stops the emulator itself; leave it at `halt`. |
| `TMPDIR` | point it into the work dir if `/tmp` is a small tmpfs. amitorture's own temp files always go to `<workdir>/tmp`. |

Isolation: every emulator gets its own HOME and XDG directories under the work dir and a generated config, so your own Amiberry settings, ROM paths and recent-file lists are never read or written. A run fails loudly (exit code 3) if your real Amiberry config directory changed during it. The runner also looks for Amiberry processes running on your real HOME during the run (every 5 s) and names them in the report as the likely cause, e.g. an Amiberry you started yourself meanwhile. If it saw none, the emulator under test is the likely cause.

---------------------------------------------------------------------------
## 7. Troubleshooting

| Symptom | Likely cause and fix |
|---|---|
| `amislot: all N emulator slots busy … waiting` | other amitorture runs (or orphaned Amiberry processes) are using the slots: wait, or check `pgrep -a amiberry` |
| entry `skip`: "no Kickstart …" | the ROM that machine needs isn't in `--kickstarts`; `roms` lists what each entry needs |
| entry `timeout` with no records | the disk didn't boot: check `raw/<entry>/<disk>/amiberry.log`. Also "SDL could not initialize": the runner sets the SDL drivers itself; make sure nothing in your environment overrides them. |
| entry `crash` | Amiberry crashed (the log shows its crash handler). That is a finding in itself. |
| a disk reports `module` skip "not enough memory on this machine for this disk" | the disk's test pools or its 32 KB bare-phase stack could not be allocated. Each disk of the set is built to fit a 512 KB machine (CONTRACT §12), so look for a machine with less free memory than that, or a single ADF built with many modules (`MODS=…`). |
| `mkadf: payload does not fit on the disk` | the disks were built as one ADF with every module: build the set, see [BUILDING.md](BUILDING.md) |
| `transport_corrupt_recovered` | the serial line delivered a corrupt line (an Amiberry serial bug), but the mailbox copy was intact: no data lost |
| verdict `INTERRUPTED (n of m entries finished)`, exit code 130 | the run was stopped with Ctrl+C or SIGTERM. The report covers only the finished entries; rerun the rest (see "Stopping a run" in section 2). |
| exit code 3 and "likely cause: an Amiberry not started by amitorture ran on your own config" | you ran Amiberry yourself during the run, on your real config. Close it and run again. |
| the Amiga's screen goes blank during a module | a module marked `run*` owns the display hardware; the status screen comes back when it ends |
| real hardware: no records, garbage or bad CRCs in the capture | check the null-modem cable and the capture settings (115200 8N1, no handshake). With a long or poor cable, bake a lower rate into the disks (section 5). |
| no code pointers in the report | the runner didn't find the Amiberry source next to the binary: pass `--amiberry-src /path/to/amiberry/src` |

---------------------------------------------------------------------------
## 8. Adding tests

Writing test modules and changing the disks is covered in [BUILDING.md](BUILDING.md).
