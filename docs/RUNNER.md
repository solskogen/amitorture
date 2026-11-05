# amitorture runner (host side)

`runner/amitorture` is a Python package (stdlib only). Run it from `runner/` as `python3 -m amitorture <command>`.
It never builds or modifies Amiberry and never touches `~/.config/amiberry` or `~/Amiberry`. ROMs are only read.
Every emulator gets an isolated HOME/XDG/TMPDIR under the work dir, and the user's dirs are watched (isolation
check in every report).

## Ground rules

- **Work dir** (`--workdir`, default `<repo>/work`). It holds `results/`, `baselines/` and `tmp/`.
  - Every temp file goes under `WORKDIR/tmp`, never /tmp. That includes ADF copies, configs, emulator homes and
    test media. The runner sets `TMPDIR` for itself and inside every emulator's environment.
  - A run's temp root is `WORKDIR/tmp/<cmd><pid>-XXXX/` with one short `eNNN/` per entry (`entry.txt` names it).
    The names are short because the IPC socket path must fit AF_UNIX's 107 bytes.
  - The temp root is deleted after the run unless `--keep` is given.
- **Emulator slots.** At most `nproc` Amiberry processes run on the machine at a time, shared by every agent and
  tool. `tools/amislot.py` (flock on `~/.cache/amitorture/locks/emu-slot-N`, or `$XDG_CACHE_HOME/amitorture/locks` when that is set, or `$AMT_LOCK_DIR`, shared by every checkout; `AMT_MAX_EMU` can lower the count, never raise it; live Amiberry processes no slot holder accounts for count too) is held for the
  whole lifetime of every Amiberry process the runner starts: `--version`, `--dump-config`, emulation runs,
  narrowing reruns, baseline repetitions, repro runs. `--jobs N` (default 1) runs N entries at once and is capped at
  the slot count. Whatever waits for a free slot is recorded as `slot_wait_s` in `raw/<entry>/result.json`.
  A slot that is free when a launcher looks is cleared of what a dead holder left in it (stale holder and
  emulator pids). A wait for a slot gives up at once on Ctrl+C (`emulator_slot(cancel=..., cancel_exc=...)`).
- **Serial/GDB ports.** The runner uses 23540-23599 by default; `--ports LO-HI` (or `$AMT_PORTS`) gives it another
  range, so that several runners on one machine (e.g. one per agent) never compete for a port. Repro packs use 23597
  (GDB) and 23598 (serial).
- **Exit codes.** 0 = pass / no regressions, 1 = problems / regressions, 2 = host error, 3 = isolation violated,
  130 = interrupted (Ctrl+C or SIGTERM).
- **Ctrl+C** (`emulator.py` `interrupt()`, `stop_all()`; `cli.py` `_install_interrupt_handler`). Emulators run in
  their own session, so the terminal's SIGINT never reaches them; the runner stops them itself.
  - The first Ctrl+C (or SIGTERM) sets `emulator.INTERRUPT`: no new emulator starts (launches and slot waits
    raise `Interrupted`), every running one gets SIGTERM, and `stop_all()` SIGKILLs whatever is still alive
    after 5 s. `run_parallel` cancels the queued entries instead of waiting for the running ones.
  - `run` then writes a partial report (`runner.run`): the finished entries, with the verdict prefixed
    `INTERRUPTED (n of m entries finished): `, or `INTERRUPTED (all entries finished; parity, narrowing and repro
    packs not done): ` when it came during the analysis. `environment.json` and `summary.json` record it as
    `interrupted` (`during`, `finished`, `selected`, `not_run`). An entry whose disks only partly ran is left out.
  - `baseline` leaves an existing baseline untouched (see baseline). The other commands just stop.
  - A second Ctrl+C SIGKILLs the emulators and quits at once (`os._exit(130)`), leaving temp files under
    `WORKDIR/tmp`.
- **Isolation guard** (`isolation.py`). The user's Amiberry config, data and home dirs (from the runner's own
  environment) are snapshotted before and compared after the run; a change is `ISOLATION VIOLATED` (exit 3).
  The guard can't tell who wrote, so every 5 s it also records Amiberry processes whose HOME is the user's real
  home (`user_amiberries`), i.e. not one of the runner's isolated emulators. The report names them as the likely
  cause (`isolation.user_amiberry_seen`); with none seen, the emulator under test is the likely cause.

## Disk set

The test ships as a set of disks: `make -C disk` (= `make -C disk set`) writes `disk/build/set/amitorture-1-core.adf`,
`-2-cpu.adf`, `-3-fpu.adf`, `-4-chip.adf`, `-5-exp.adf`, `-6-cpux.adf` and `-7-mmu.adf` (the groups are `DISK_SET_*` in
disk/Makefile; a group without modules yet is skipped, and the numbers stay fixed).
- Each disk is a complete payload of under about 300 KB (the fpu disk, the largest, is about 285 KB), so it fits a
  512 KB chip-only KS1.x machine.
- The set build appends `<n>/<N> <group>` to each disk's build id. The disk's status screen shows "disk n of N"
  from it, and `import` uses the group as the disk's name.
- Every module is on exactly one disk; rt and takeover are on core only.

How the runner uses it:
- **Which disks.** `--disk-set DIR` uses every `*.adf` in DIR, in name order. Without `--disk-set` and `--adf`,
  `disk/build/set` is used if it exists; otherwise a single `--adf PATH` (built with `make -C disk MODS="rt <modules>"`).
- **Launches.** Each entry runs every disk as a separate, sequential emulator launch.
  - Each launch holds its own slot and bakes its own copy of the disk.
  - Its config is `configs/<entry>.<disk>.uae`, and its raw files go to `raw/<entry>/<disk>/`.
  - Apart from media, floppy speed and automatic warp policy, the config is identical for every disk.
- **One entry result.**
  - Case records of all disks are merged into one result. Cases keep their `(mod, case)` key, so parity,
    baselines, check and narrowing work as with one disk.
  - Integrity checks (`done.records`, seq gaps, CRC) are made per disk.
  - The entry status is the worst over its disks: crash > hang > timeout > error > incomplete > fail > pass.
  - A disk that hangs, crashes or times out is reported as such (failure items carry `disk`), and the other disks
    still run.
- **Probe.** Each disk sends its own hello. The probe results of the disks must agree, ignoring free memory, which
  differs with payload size. A difference is a finding, `probe_inconsistent`. The probe is reported once, from the
  first disk.
- **Narrowing and repro packs.** Narrowing reruns only the disk that holds the failing module (`disk_only`). A repro
  pack carries just that disk, with the module mask baked in.
- **Floppy speed.** Set disks that don't test the floppy hardware load at turbo (`floppy_speed=0`); the chip disk
  (paula: raw disk DMA, DSKSYNC, motor timing) and the cpux disk (dmacontention: disk DMA under load) run at
  `floppy_speed=100`, the real drive speed (runner.py `REAL_SPEED_GROUPS`). A set disk passed alone with `--adf` is
  recognised by its header (build id `... <n>/<m> <group>`) and gets the same speed; the all-in-one ADF and unknown
  disks run at real speed. An explicit `floppy_speed` (`--set` or an entry's options) always wins. Turbo breaks raw
  disk-controller DMA: in disk.cpp's doreaddma turbo path DSKBLK comes early and raw MFM reads get bad checksums,
  which the paula module reports as failures. `--timeout` is per disk launch (default 600 s). Measured at floppy_speed=100: the core
  disk takes 243 s on a cycle-exact A500 (cpu 125 s, chip 127 s, cpux 101 s, fpu/exp about 35 s) and 225 s on the
  CDTV, the slowest launches.

### Automatic warp

`--warp auto` accelerates only the `cpu`, `fpu` and `mmu` set disks. The baked disk's
build id, not its filename, identifies its group; renamed set disks passed with `--adf`
get the same policy. `core`, `chip`, `exp`, `cpux`, mixed/unknown disks, selftests and
any launch requiring audio capture stay normally paced. `--audio off` does not make
a timing-sensitive disk eligible for warp. Module masks never change this decision.
All selected disks and cases still run with their existing assertions and verdict rules.

The policy is applied before each fresh boot, so audio/input/RTC/hostspeed sections
never inherit a warped clock, muted audio buffers or synthetic key releases from a
previous section. There are no asynchronous mid-module mode changes. The runner
checks the resolved config and `GET_WARP` at launch; an unsupported command, malformed
reply or unexpected mode is a host error, not a successful fallback. Warped launches
poll the mailbox every 10 ms instead of 250 ms; overruns, serial recovery and record
integrity remain visible through the existing transport checks.

Normal pacing remains the default (`--warp off`). `--warp on` forces continuous warp
for experiments and **invalidates timing-sensitive coverage**: Amiberry suppresses
audio, synthesizes key releases, and advances emulated clocks faster than its host-backed
RTC. Legacy `--set warp=true` remains an explicit experimental override; it cannot be
combined with `--warp`. An explicit `--warp` policy takes precedence over matrix options.

Every launch records its requested policy, effective mode, group, reason and runtime
verification in results and REPORT.md. Baselines store the policy; `check` inherits it
unless overridden and reports changes in per-entry metadata. Engine parity requires
matching effective modes on corresponding disks. Repro packs pin the original module's
effective mode in both their config and stored entry, including packs rebuilt from saved
results. Equivalence means unchanged coverage
and case outcomes, not identical wall-clock timings, live RTC digits or throughput;
validate against matched normal-paced runs and retain any differences as findings.

**Current validation limit:** paired quick smoke runs preserved every case identity
and failure outcome, but Paula timestamp acquisition and the timing module's H-range
coverage check changed some cases between `pass` and `untestable`, even though both
disks used normal pacing. A separate warp-off-only control also reproduced Paula
outcome changes. Automatic warp is therefore not yet certified to give identical
pass/skip/untestable outcomes for the entire suite. These observation failures and
their engine-parity differences remain visible; they are not converted to passes,
excluded from comparisons, or added to the xfail registry.

## Commands

### roms
```
amitorture roms --kickstarts ~/Amiberry/ROMs [--matrix smoke|standard|full] [--recursive] [--json]
```
`--recursive` also scans subdirectories. Identifies every file by CRC32/SHA1 (romdb.json, from Amiberry's rommgr) and shows which ROM each entry of the
matrix would use, or `SKIP: needs ...`.

### matrix
```
amitorture matrix [--matrix NAME] [--config GLOB ...] [-v] [--json]
amitorture matrix --matrix full --validate --amiberry BIN --kickstarts DIR
```
Lists the entries. `--validate` runs `amiberry --dump-config` on every config without emulating, and prints each
option Amiberry changed together with the fixup message that explains it. That is the quick way to see what the
emulator refuses, e.g. a threaded CPU with an MMU.
Failed or unusable dumps are printed as `FAILED`, retained in the JSON diagnostics, and return host-error
exit status 2; they are not counted as validated entries.

The matrices (runner/amitorture/matrix.py):

| name | size | contents |
|---|---|---|
| smoke | 10 | A500 68000 CE, A600 ECS NTSC compat, A1200 68EC020 (+JIT), A3000 68030+68882 MMU compat, A4000 68040 (+JIT), A4000 68060 (+JIT), AROS AGA 68020 |
| standard | 142 | every model preset (A1000, A500, A500+, A600, A2000, A3000, A4000, A4000T, A1200, CD32 and CDTV booting from CD, AROS ECS/AGA), PAL+NTSC samples, every CPU variant, compat/CE on 020-060, JIT twins of every JIT-capable entry, every JIT sub-option on the A4000 68040, threaded CPU, softfloat; MMU configurations (68030/68040/68060 with their MMU in compat/CE, the EC MMU modes `mmu_model=68ec030/68ec040/68ec060`; no JIT twin: the JIT does not support the MMU); one entry per expansion board (below) |
| full | 540 | every CPU variant (68000, 68010, 68EC020, 68020, 68EC030 (mmu 0 and mmu_model=68ec030), 68030, 68EC040, 68LC040, 68040, 68040 without MMU emulation, 68EC060, 68LC060, 68060, 68060 without MMU emulation) x every FPU it can have (none/68881/68882/internal) x fast/compatible/cycle-exact; 68060 PCR revisions; CE with cpu_multiplier=0; every model x PAL/NTSC; OCS/ECS-Agnus/ECS-Denise/ECS; and for every entry that allows it: JIT default + cachesize 128/16384 + comp_trust{byte,word,long,naddr}=indirect + compfpu off + comp_constjump off + comp_flushmode hard + comp_nf off + comp_catchfault off, threaded CPU, JIT+threaded, softfloat, JIT+softfloat; the MMU configurations and every board entry of standard, plus the full-only boards |

- **Entry id.** `<model>-<cpu variant>[-<881|882>]-<fast|compat|ce>[-ntsc]`, plus a twin suffix: `-jit`,
  `-jit-<variant>`, `-thr`, `-jit-thr`, `-sf`, `-jit-sf`. Board entries: `<model>-<kind>-<board>`, e.g.
  `a4000-rtg-picassoivz3`, `a4000-snd-toccata`, `a4000-net-a2065`, `a600-pcmcia-sram`, `a2000-scsi-a2091`.
- **Expansion boards** (`matrix.hardware_entries`), one entry per board, in standard and full, on an A4000 68040
  fast unless the board needs another machine:
  - every hardware RTG board of `gfxcard_type` (gfxboard.cpp), Zorro II, Zorro III and PCI (PCI ones behind a
    Prometheus bridge), with a valid `gfxcard_size`. Not generated, and `not_covered` in the catalogue: uaegfx
    (driver only), x86 VGA (bridgeboard BIOS), Altais (DraCo), EGS 110/24 (GVP local bus), Pixel64 (AteoBus);
  - sound cards (`<name>_rom_file=:ENABLED`): Toccata, Prelude and uaesnd Z2 each on an A500 68000 CE, an A1200
    68020 and an A4000 68040; Prelude 1200 (clock port) on the A1200; uaesnd Z3, ES1370 and FM801 (PCI) on the
    A4000. The A1200/A4000 ones (not PCI) have a JIT twin. Their disks are core and exp;
  - A2065 (`a2065=slirp`: NAT, 10.0.2.2 answers ARP) and the ZZ9000 boards with `zz9000_net=slirp`; full adds
    Ariadne, Ariadne II, Hydra, X-Surf, X-Surf 100, NE2000 PCI/PCMCIA;
  - Gayle: media.py gives every A600/A1200 entry a test-owned IDE hardfile and a PCMCIA SRAM card (4 MB plus a
    CIS, CONTRACT §11), so `a600-68000-compat` (KS 2.05) is the A600 Gayle IDE + SRAM machine; A500+ RTC, A500 + A501
    RTC (full adds the A4000 RF5C01); A3000/A4000 with Zorro II + Zorro III RAM and a Zorro III RTG board
    (`*-z2z3-rtg`); A2091 and A4091 SCSI with a test-owned hardfile (media.py);
  - full adds CyberStorm PPC / Blizzard PPC (68k side; the QEMU PPC plugin and the flash ROMs must be present).
  
  A board entry has `disk_groups` (the disks it runs: core + exp, or core only for SCSI), `board_roms` (romdb
  ids, e.g. the Picasso IV flash ROM 91, copied into the emulator's ROM dir and passed in their option; missing
  means `skip`) and `media_files` (zero-filled images made in the entry's media dir, `{media}` in option values).
  No engine twins, except for the sound cards above.
- **CD32 and CDTV boot from CD** (CONTRACT §14): the runner turns the baked disk into a bootable ISO
  (`cdimage.py`: the payload as an AmigaDOS program, `S/STARTUP-SEQUENCE`, the trademark files decompressed from the
  user's extended ROM), sets `cdimage0=<iso>,image` and no floppy drive. The ISO is described in
  `raw/<entry>/<disk>/result.json` (`cd`).
- **ROMs.** Each entry declares the ROMs it accepts (romdb ids in order of preference, plus an extended ROM for
  CD32/CDTV). If none is found, the entry gets `skip` with the reason.
- **Unsupported combinations** are not generated: JIT with MMU/CE/68000, a threaded CPU with
  MMU/compat/CE/68000, an FPU on a 68000/010.
- **Run-time validation** works two ways.
  - **Config.** Every config is checked with `--dump-config`.
    A failed verification stops that disk before emulation, reports `error`, and preserves the dump error,
    exit code and stderr. An unverified entry is never marked "as designed".
  - **Emulator log.** After the run, the log is checked for what actually ran: build_cpufunctbl's
    `CPU=68040, FPU=68040 (host 64b), MMU=0, JIT=CPU/FPU=8192. fast` line (JIT on or off, FPU, softfloat, MMU,
    CPU), and `uae_start_thread "cpu"` versus "... running the CPU on the main thread" for the threaded CPU.
    This catches the cases Amiberry refuses only at start-up, e.g. "JIT and cycle-exact can't be enabled
    simultaneously".
  - Either way, the entry is marked **"config adjusted by emulator: X"** (`adjusted`, with `source: emulator log`
    for the second kind). It is not counted as run as designed, and twin parity is "not compared" for it, so a
    JIT twin whose JIT was not active is never counted as a JIT run.
  - By construction every JIT/threaded twin is the fast configuration (`cycle_exact=false`,
    `cpu_compatible=false`, no MMU, 68020+) with `cachesize>0` / `cpu_threaded=true`. It is paired with the same
    fast configuration with `cachesize=0` / `cpu_threaded=false`.

### run
```
amitorture run --amiberry BIN --kickstarts DIR [--matrix smoke] [--config GLOB ...] [--set OPT=VAL ...]
               [--adf disk/build/amitorture.adf] [--mode full|quick] [--seed N] [--param KEY=VAL]
               [--disk-set DIR] [--jobs N] [--keep] [--timeout S per disk (600)] [--hang S (120)]
               [--narrow-budget S (7200)] [--narrow-reruns N (3)] [--narrow-binary BIN ...] [--no-narrow]
               [--baseline NAME] [--no-repro] [--amiberry-src DIR] [--xfail FILE]
               [--audio auto|on|off] [--warp off|auto|on] [--ports LO-HI] [--end-action halt|reboot|return] [-q] [--show]
               [--no-disk] [--selftest]
```
- `--end-action`: what the disk does after its last module (default `halt`; the runner stops the emulator itself).
- `--show` (also `baseline`, `check`, `repro`): show the emulator windows on your desktop (`run`, `baseline`, `check`, `repro`, and `tests/smoke_disk.py`); one window per running emulator, so `--jobs 4` gives four. Your own display settings are used as they are (your `SDL_*` video variables, `DISPLAY`, `WAYLAND_DISPLAY`); audio stays the runner's. HOME and `XDG_RUNTIME_DIR` stay isolated (Amiberry names its IPC socket `$XDG_RUNTIME_DIR/amiberry.sock`, which a test emulator must never take from your own Amiberry), so a relative `WAYLAND_DISPLAY` is passed as the full path of the same socket and an unset `XAUTHORITY` as your `~/.Xauthority`. Run it from a terminal on the desktop. Timing values may differ from a headless run. Implementation: `emulator.display_env`; without it every emulator runs with SDL's offscreen driver.
- `--no-disk`: no disk in DF0; checks only the launch, IPC, serial and the mailbox scan.
- `--selftest`: `--no-disk` plus a synthetic mailbox written over IPC, to check the scan and ring reading.

1. Runs every selected entry: bakes the run parameters into a copy of the ADF, writes a complete config, checks it
   with `--dump-config`, launches the emulator isolated, and reads serial and the chip-RAM mailbox until `done`,
   a hang, a crash or the timeout (as in milestone 1). Test media for devio come from `runner/amitorture/media.py`
   (`prepare(entry, options, media_dir, seed)` returns config options) when that file exists; without it the run
   goes on without media.
   A completed guest boot must have matched module begin/end records and test evidence (case, perf or speed
   records). New disks announce their selected compiled modules in `hello.modules`; all must complete with
   matching ids. A disk with an explicitly empty selection is `skip`, not PASS, and does not stop the remaining
   disks of a set. Older captures without the manifest receive lifecycle checks but cannot prove selected-module
   coverage. Synthetic host selftests are exempt from guest-module checks.
2. **Engine parity.** Every twin is compared with its reference entry, case by case (identified by
   `<mod>.<case>`):
   - a case missing on either side is an `engine_mismatch`;
   - any difference in a non-timing field is an `engine_mismatch` with both records, the differing paths and the
     options the twins differ in;
   - timing fields (`perf` records, and keys named `timing`, `ticks` or `latency*` at any depth) are excluded;
   - softfloat twins give `fpu_mode_difference` (informational).
   The effective options must actually select the intended engines and differ only in the designed engine
   settings (including the combined JIT/threaded variant). Overrides that collapse both sides to one engine
   make the pair "not compared". The JIT parity catalogue row requires matching cases in a completed comparison;
   JIT-only runs cannot cover it.
3. **xfail.** Cases in `runner/amitorture/xfail.json` that fail are `xfail` and are not counted as problems. If
   they pass, they are reported as `fixed`. Guest-case xfails never suppress engine parity disagreements,
   even when the differing cases both pass.
4. **Narrowing.** For each (entry, module) with failing cases or mismatches, the runner reruns only that module
   (`modules=<1<<id>`, same seed):
   - N times on the same config, giving `deterministic` / `intermittent` / `not reproduced` and a fail rate. If
     the module alone never fails, the runner tries once with the full disk.
   - On every neighbouring config with one setting changed: JIT off/on, each `comp_*`, cachesize 128/16384,
     fast/compat/CE, the adjacent CPU models, FPU native/softfloat, threaded off/on, chipset, PAL/NTSC, and each
     `--narrow-binary`. For an engine mismatch on a machine-changing axis, the reference entry is rerun with the
     same change and the two are compared.
   - The result is per case: which changes flip it to pass, which still fail, which were inconclusive, and a
     sentence such as "fails only with JIT on and comp_nf=true; passes with jit=off, comp_nf=false; still fails
     with cpu_model=68030".
   - The order is breadth first. Round 1 gives every group one rerun, then a round-robin hands each group its next
     task: the axis most likely to flip it first (JIT twins: `jit=off`, then `comp_nf`, `comp_constjump`, ...; engine
     mismatches: the comp_* options first), with the other reruns interleaved. `--jobs` launches run at once, each
     inside an emulator slot.
   - A variant whose options equal an entry that already ran in this run reuses that result instead of launching
     (e.g. `jit=off` of a JIT twin is its reference entry); it is recorded as `reused` in narrowing.json.
   - A failing case gives the stronger signal, so when a case both fails and differs from the reference, it is
     narrowed as a case failure.
   - If none of the reruns reproduce the failure, no flips are claimed.
   - The total launch time is capped by `--narrow-budget` (default 7200 s: run time matters less than a
     diagnosis). Untried variants are listed; a group never reached says "not narrowed: --narrow-budget exhausted".
   - **Module variants first** (`narrow.module_variants`), because they separate causes:
     - `mmu_model=0` on MMU entries;
     - `cpu_24bit_addressing` toggled on 020/030;
     - `gfx_resolution` lores/superhires for display/copper/dmacontention (Amiberry renders, scrolls and collides at
       the output resolution);
     - `cpu_no_unimplemented` on 68060 integer modules;
     - `fpu_no_unimplemented` / `fpu_strict` for fpu;
     - `floppy_speed=100` for paula;
     - every `option=value` that a failing case's why names as a precondition ("only ... with X=v").
     
     JIT twins then try the JIT sub-options before the other axes.
   - A core-mode variant that runs the same CPU core table (`codemap.core_table`) is not launched, and is listed as
     "not launched (no effect on this machine)".
5. **HRM timing (cycle-exact entries only).** A case may carry the Hardware Reference Manual's exact figure as
   `timing.hrm_<x>` next to its measured `timing.<x>` (dmacontention: DMA slots). On entries with
   `cycle_exact=true`, every pair that differs is an `hrm_timing_mismatch` item. It is low severity, so it appears
   under Notes and not in the verdict. Timing fields are never fingerprinted, so it is not a baseline regression
   either. Other core modes are legitimately not cycle-exact and are not compared.
6. **Problems.** Failures become one problem only when all of these match:
   - the module;
   - the narrowing signature (the flipping settings plus the determinism);
   - the exact set of entries they fail on;
   - their expect/got field names: connected components over shared names, so `addr_off` failures and `vec`
     failures of the same module are two problems.

   Every case/mismatch problem then gets these aids (`diagnose.py`):
   - **Cases**: each failing case with its docs/cases line (intent, rule, failure meaning) and its `evidence:`
     field, or "no real-hardware evidence recorded". Abbreviated ids in docs tables (`_cas2_l`) are resolved. The
     report lists which expect/got/ctx/in field names docs/cases explains and which it does not (for the module
     owners).
   - **Settings that change the module's semantics**, with the resolved value on every failing entry
     (`diagnose.MODULE_OPTIONS`), and the `option=value` pairs a case's why/docs/note names:
     - when the text states a precondition ("Amiberry only traps these with cpu_no_unimplemented=true") and the
       config has another value: "config contradicts the case's precondition". When that covers every failure of
       the problem, the problem gets severity low and a title prefix;
     - when the text only mentions a setting the config does not have: "the case's own explanation does not fit
       this config".
   - **Where the same cases pass**: the entries where every case of the problem passed (not merely "did not
     fail"), the entries that skipped them, every setting whose values separate all failing entries from all
     passing ones (module-relevant settings first: "chipset: ecs vs aga", "jit: on vs off"), the same among the
     passing entries that share the failing entries' JIT/chipset/MMU/CPU ("among jit=on: cpu_24bit_addressing false
     vs true"), and the nearest passing entry's option diff.
   - **Emulator log**: the amiberry.log lines of the failing launches that fall into the module's run time
     (`logmine.py`): dummy-bank stores, JIT faults, Gary timeouts, illegal/unimplemented, MMU, cycle unit,
     warnings. Similar lines are grouped (count, addresses, one example). The time base is `serial-times.json`
     (arrival time of every serial chunk, written since this version: exact module windows), else the mailbox reads
     (approximate, marked). `log_counts` in every result.json comes from the same classifier.
7. **Code pointers.** Computed from the resolved config: which CPU core table ran; the handler function and
   file+line when the case has an opcode in ctx; the subsystem files and functions for the module. See
   docs/emulator-map/amiberry.md.
8. **Repro packs**, `results/<run>/repro/<P..>/`:
   - `case.adf`: only the module enabled, the seed baked in;
   - `config.uae`: standalone, GDB server on;
   - `pack.json`;
   - `run.sh` (calls `amitorture repro`);
   - `gdb.cmd` (m68k-amigaos-gdb against Amiberry's GDB server: `debugging_features=gdbserver`, `gdb_port`);
   - `host-gdb.cmd` (host gdb with breakpoints on the suggested functions).
9. **REPORT.md** (below) and the machine-readable files.

`--set OPT=VAL` overrides an Amiberry option on every selected entry, and is recorded as `overrides`. Use it for
experiments, or to make a deliberate change before `check`. `--baseline NAME` adds each failing case's baseline
state to the problem's history.

### baseline
```
amitorture baseline --amiberry BIN --kickstarts DIR --matrix NAME --name BASELINE [--repeat 3] [--config ...] [--set ...]
```
Runs every entry `--repeat` times and writes `WORKDIR/baselines/<name>/`:
- `baseline.json` stores, per entry:
  - the entry definition (all options);
  - statuses;
  - every case fingerprint (the case record minus seq and timing fields), with `stable` set when all runs agree.
    A case that differs between runs, or is missing from some, is `unstable`;
  - timing bands per case path and perf bands per `<mod>.<name>`, in ticks/op:
    mean ± max(3σ, 5 %·|mean|, 2 ticks/ops) (perf) or 1 (timing fields);
  - ROM id/CRC, disk build id and binary sha256.
- `meta` holds the binary path/sha256/version, disk build id and sha256, ROMs per entry, matrix, params and host.
- `BASELINE.md` is a human/AI summary: unstable cases and perf bands.
- `runs/run<N>/` holds the full results (REPORT.md, raw/) of each repetition.

`--repeat` must be at least 1 for both `baseline` and `check`.
Re-running a baseline with the same name keeps the old files as `baseline.json.previous` and
`BASELINE.md.previous`, and the old runs as `runs.previous/`. Runs, JSON and Markdown are all prepared
before publication. If publication fails or is interrupted, the active baseline and its runs are restored;
a baseline that did not exist before is removed again.

### check
```
amitorture check --amiberry BIN [BIN2 ...] --baseline NAME [--repeat 1] [--config GLOB] [--set OPT=VAL] [--kickstarts DIR]
```
Reruns the baseline's entries as stored, on each binary in turn, with `--set` applied. The kickstarts dir, ADF
or disk set (when it still exists), mode, seed, end action and the other `--param` values (e.g. `flags=noscreen`)
default to the baseline's; giving `--param` replaces the baseline's extra values. Then it diffs fingerprint by fingerprint:

| kind | meaning |
|---|---|
| `regression` | a stable baseline fingerprint changed; `transition` (`pass->fail`, `pass->pass`, `fail->fail`, ...) and the differing paths are given |
| `fixed` | baseline `fail`, now `pass` (with the xfail reference if registered) |
| `new` / `missing` | the case is only in the check / only in the baseline |
| `new_failure` | a new case fails in at least one completed check run without a matching xfail; a regression, even if it is intermittent |
| `became_unstable` | stable in the baseline, differs between the check's own runs (needs `--repeat` > 1) |
| `unstable_excluded` | unstable in the baseline: reported, not compared |
| `perf_out_of_band` / `timing_out_of_band` | outside the band; `direction` lower (faster) or higher (slower). On `cpu_speed=max` entries (fast core, JIT) timing follows the host speed and load: such findings are marked `host_dependent` and are not counted as regressions |
| `perf_missing` / `timing_missing` | a previously present measurement is no longer reported; a regression even on `cpu_speed=max` |
| `entry_status` / `missing_entry` / `new_entry` | the entry as a whole ended differently (crash, skip ...) |
| `fail_detail_changed` | a case that failed in the baseline and fails now, either with the same expect/got/why/in and only paths under `ctx` differing (e.g. absolute addresses after a disk rebuild moved the payload), or as a registered xfail whose measured values changed. Informational, not a regression |

- Each finding lists the entry's config changes vs the baseline. A deliberate `--set` therefore shows up pinned
  to the cases it changed: "config differs from baseline: cpu_model 68020->68030".
- ROM, disk build and binary changes are listed per entry.
- With several binaries, "First binary where each fingerprint changed" gives the first binary in the given order
  where each fingerprint differs. That lets you bisect over prebuilt binaries without building anything.
- Output goes to `WORKDIR/results/check-<id>/` (`REPORT.md`, `check.json`, raw runs under `bin<N>/run<M>/`).
- Regression kinds: `regression`, `missing`, `became_unstable`, `perf_out_of_band`, `timing_out_of_band`,
  `entry_status`, `missing_entry`, `new_failure`, `perf_missing`, `timing_missing`. Any of them gives exit status 1.
- Isolation violations stop `baseline`, `check` and `repro` with exit status 3, independently of case differences.
  A baseline with an isolation violation is not installed. Saved violations also fail `check --rediff`.
- Rediff preserves the check's selected entry scope: missing saved results produce `missing_entry`, not a
  silently reduced comparison.

### Host request channel (CONTRACT §13)
While watching a launch the runner reads `request` records from either transport (the copy that arrives second is
a duplicate) and serves them in order (`hostreq.py`):

| what | host action | result |
|---|---|---|
| `screenshot` | waits `frames`/50 (60 NTSC) s, IPC `SCREENSHOT <path>` | `raw/<entry>/<disk>/shot-<id>-<tag>.png`: path, sha256, size, width, height |
| `audio_mark` | marks the audio stream (below) | start: byte offset; stop: offset, segment, analysis, `audio-<id>-<tag>.wav` |
| `key` | IPC `SEND_KEY <code> <1 down/0 up>` (Amiga raw key 0..$7F) | the injected event |
| `mouse` | IPC `SEND_MOUSE <dx> <dy> <buttons>` (mouse device 0; only for a port whose `joyportN=mouse`) | the injected event |
| `joy` | none: Amiberry's IPC has no joystick injection | not acked |
| `cd32pad` | no injection either; `{"buttons":0}` is acked when `joyport<port>mode=cd32joy` (default port 1; CD32 entries have `joyport1=kbd1`, `joyport1mode=cd32joy`, and no host key is ever pressed, so every button reads released) | released state, or not acked |
| `wait` | none | acked at once |

- **The ack.** After acting, the runner writes the id to `host_ack` (mailbox+44) with `WRITE_MEM <addr> 4 <id>`
  (put_long, big-endian).
- **No ack.** An unknown `what` is never acknowledged. Neither is an action that failed, or a request this launch
  cannot serve (no audio capture, no joystick injection); the guest then reports the dependent case `untestable`.
- **The mailbox comes first.** A request is only served once the mailbox address is known, because its ack goes
  there. While one is waiting, the runner scans all of chip RAM every 0.25 s; otherwise it scans every 0.5 s until
  the mailbox is found.
- **Host records.** Every request gets one: `{"t":"host","id","what","tag","acked","result",...}`. They are kept in
  `raw/<entry>/<disk>/host.jsonl` and merged into `records.jsonl` right after their request, marked `"host":true`.
  They are not guest records: they never enter `done.records`, seq gaps or fingerprints.
- **v1 `capture` records** (`screen`, `audio_start`, `audio_stop`) are served the same way, without an ack.
- **Screenshots in baselines.** On cycle-exact entries (`cycle_exact=true`) each acknowledged screenshot's sha256
  becomes the pseudo case `host.shot_<tag>` (cases.py), so baselines and `check` compare the picture. Elsewhere the
  frame a screenshot catches is not exact, so it is kept but not compared.

### Audio capture
- **When.** `--audio auto` (the default) captures only the launches whose disk contains an `audio_mark` request.
  The string is in the payload, so the runner reads it from the ADF before the launch. `on` captures every launch,
  `off` none. Without capture the emulator gets SDL's dummy audio driver.
- **How.** The emulator runs with `SDL_AUDIO_DRIVER=disk` and `SDL_AUDIO_DISK_OUTPUT_FILE=<entry tmp>/audio.raw`.
  SDL3 reads hints from the environment; the SDL2 spellings are set too. SDL writes the mixed output in real time as
  S16LE, 44100 Hz, stereo: the format Amiberry opens (sounddep/sound.cpp), checked live (440.3 Hz programmed,
  440.25 Hz measured).
- **Fixed input settings in every config.** `input.mouse_speed=100` (SEND_MOUSE counts 1:1), `absolute_mouse=none`,
  `tablet_library=false`, `magic_mouse=false`.
- **Fixed sound settings in every config.** `sound_output=exact`, `sound_frequency=44100`, `sound_channels=stereo`,
  `sound_interpol=none`, `sound_filter=off`, `sound_stereo_separation=10` (Paula 0/3 left, 1/2 right, no
  crosstalk), full volume.
- **Marks.** A mark is the file size at the moment the request is served. The guest waits for the ack, so a start
  mark comes before its sound. For a stop mark the runner first waits 0.35 s, so that SDL's buffered audio reaches
  the file.
- **Analysis.** The segment between a start and a stop mark (matched by tag) is cut out as a WAV and analysed in
  pure Python:
  - per channel (L/R): RMS, peak, DC, onset, active duration, dropout fraction;
  - the dominant frequency, by zero crossings and by FFT (16384 points, Hann window, interpolated);
  - with `"expect_hz"` (and `"tol_pct"`, default 3) in the stop mark's args, `freq_ok`.
- **Verdict.** Each analysed tone with an expected frequency becomes the pseudo case `host.tone_<tag>` (pass/fail,
  on every entry that captured audio, docs/cases/host.md, schema §5.1), so a wrong pitch or a silent channel is a
  failure, not just a line in the report. A tone whose length is not what the guest played (an emulator slower
  than real time stretches the capture) is untestable rather than failed.
  
  The result is in the stop mark's host record. The raw stream stays in the temp dir and is removed with it.

### Coverage catalogue
`docs/catalogue.yaml` lists every capability of the plan's catalogue, mapped to case-key globs (and optionally to
the entries that must report them), or `not_covered` with the reason. `report.py` evaluates it for every run
(`catalogue.py`):
- `covered`: a matching case passed or failed;
- `partial`: only skip/untestable, or the catalogue says only part of it is reachable;
- `not_covered`: no matching case ran, or no test can reach it.

REPORT.md shows the table at the top of the coverage section; `summary.json` has it as `catalogue`.

### repro
```
results/<run>/repro/P01/run.sh [--amiberry BIN] [--gdb] [--keep]
amitorture repro PACKDIR [--amiberry BIN] [--gdb] [--show]
```
Reruns the pack isolated (its own `work/` inside the pack) and prints the case record and its result.
- Exit status: 0 = passes now (or, for an engine mismatch, matches the reference again), 1 = still fails,
  2 = the case was not reported.
- The default binary is `$AMIBERRY`, or the one recorded in the pack.
- `--gdb` turns on the GDB server; then use `m68k-amigaos-gdb -x gdb.cmd`.

### report
```
amitorture report RESULTS_DIR [--no-repro] [--xfail FILE]
```
Rebuilds a run's REPORT.md, summary.json and failures.jsonl from its saved files (`raw/*/result.json`,
`records.jsonl`, `narrowing.json`, `environment.json`) with the current analysis code: parity, xfail registry,
grouping, code pointers, docs/cases lines and repro packs. No emulator is started; narrowing is not redone. Use it
after editing xfail.json, after a module agent writes docs/cases/<module>.md, or after changing the report code.

`check --rediff CHECK_DIR` does the same for a check: it recomputes the diff against the baseline from the saved
raw runs.

### import
```
amitorture import FILE... [--out DIR] [--label L]
```
Builds the same report from serial captures or `mailbox.jsonl` made elsewhere (real hardware, FPGA;
`report.import_files`, `_import_groups`). All files describe one machine.
- **Disk runs.** Each boot in a file (a `hello` with seq 0 starts one) is one disk run, so one long capture across
  every disk works, and so do several files (one per disk, any names, any order). Boots are matched across files
  by the hello's build id, then their observed boot histories. Equal-length histories retain their boot order;
  unequal histories need a unique matching hello or a unique complete boot containing the same valid records.
  Ambiguous epochs stay separate and are reported as integrity problems: without a shared boot nonce, partial
  captures cannot safely be combined merely by right-aligning them. A run is named after the last word of its build id (the group,
  e.g. `core`); the same disk booted twice gives `core` and `core#2`. Records before any hello are a run of their
  own.
- **One entry,** `imported`, combined like a disk set in `run`: a status per disk (without `done`: "no done record:
  the capture stopped, or the machine hung, before this disk finished"), the worst one for the entry, and
  `probe_consistency` across the disks (`probe_inconsistent` when they did not see the same machine).
- **Rebuildable evidence.** The combined `raw/imported/records.jsonl` remains available; multidisk imports also
  save per-disk records under `raw/imported/disks/NNN/`. Corruption, conflicts, schema problems and duplicate
  counts are persisted, so `report` retains the original evidence. Missing saved records are an explicit error.
- **No emulator:** no narrowing, no repro packs, no engine parity, no code pointers (no config), no emulator log.
  xfail and the docs/cases lines are applied. Exit status 0 for PASS, else 1.
- The DF1 results-disk transport is not implemented (CONTRACT §11): serial is the only way off a real machine.

## REPORT.md layout (run)

1. Verdict line and a short paragraph on how to read the report.
2. **Coverage**:
   - the capability catalogue (docs/catalogue.yaml): covered / partial / not_covered per capability, with why;
   - Modules × entries (cases, pass/fail/skip/untestable/xfail/engine_mismatch/unstable, which entries it fails
     on, which entries never reported it);
   - Entries (machine, ROM, engine/twin, status, as designed?, counts, why);
   - skipped entries and why;
   - configs adjusted by the emulator (option wrote -> resolved, the fixup message);
   - engine parity per twin pair;
   - host requests per entry (not acked and why, screenshots with their hashes, audio segments with their
     frequencies);
   - xfail / fixed.
3. **Problems**, most severe first. Each has:
   - title and scope (cases, entries, passes on), representative config;
   - the representative failure in full: intent/rule line from `docs/cases/<module>.md`, why, in, expect, got,
     ctx, the whole record, the records before it, the guest probe;
   - narrowing (statement, determinism, flips/holds/inconclusive, per-case lines);
   - code pointers (core table, handler, subsystem functions with file:line);
   - repro pack;
   - history (baseline, xfail);
   - every other failure of the problem.
4. Notes (low severity: probe mismatches with low confidence, configs not applied, softfloat differences).
5. Environment (binary, source, host, disk, params, matrix, isolation, ROMs), transports per entry, files.

Machine-readable files next to it: `summary.json`, `failures.jsonl`, `narrowing.json`, `environment.json`,
`configs/`, `raw/<entry>/`, `repro/`. Schema: docs/schema.md.

## Code layout

| file | role |
|---|---|
| `cli.py` | commands |
| `matrix.py` | ROMs, model presets, CPU variants, core modes, JIT variants, the three matrices, twins, narrowing neighbours |
| `amiberry_cfg.py` | config writer and `--dump-config` validation (differences, DWRITE defaults, fixup reasons) |
| `runner.py` | Options, Context (temp root, ROMs, isolation guard, slots, jobs), `run_entry`, observation, classification |
| `emulator.py` | isolated environment, emulator slots (tools/amislot.py), process, IPC, serial |
| `cases.py` | fingerprints (timing stripped), perf values, diff paths, module ids |
| `parity.py` | twin vs reference comparison |
| `xfail.py`, `xfail.json` | known-bug registry |
| `narrow.py` | narrowing launches (budgeted) and the pure flip/hold analysis |
| `problems.py` | grouping, docs/cases lookup, repro packs |
| `codemap.py` | Amiberry code pointers (core table, handler from cpustbl.cpp, subsystems) |
| `analysis.py` | post-run pipeline for `run` |
| `baseline.py`, `regress.py` | baseline/check data model and commands, `repro` |
| `report.py` | REPORT.md, summary.json, failures.jsonl, import |
| `isolation.py` | isolation guard: snapshots of the user's Amiberry dirs, Amiberry processes on the user's real HOME |
| `media.py` | test-owned media for devio (written by the devio module) |
| `logmine.py` | amiberry.log classification and module windows (serial-times.json / mailbox.jsonl) |
| `diagnose.py` | per-problem aids: case docs and undefined fields, semantic settings and preconditions, pass/fail separation, emulator log |
| `hostreq.py` | host side of the request channel: intake from both transports, actions, the ack, host records |
| `audio.py` | SDL disk-driver capture, marks, segment analysis (RMS, zero-crossing and FFT frequency) |
| `cdimage.py` | CD boot: ADF -> hunk executable, trademark data from the extended ROM, ISO 9660 writer |
| `catalogue.py` | docs/catalogue.yaml parser and per-run coverage status |

Unit tests: `cd runner && python3 -m unittest discover -s tests` (no emulator needed).
