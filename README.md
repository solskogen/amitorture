# amitorture

Accuracy tests and benchmarks for Amiga emulators, Amiberry first.

amitorture builds bootable Amiga disks (ADFs) and a host runner that boots them in the emulator across a matrix
of machines and settings. There are two kinds of disk, with separate jobs:

- **The accuracy disks** say whether the machine behaves correctly. A set of three disks exercises the
  68000-68060 CPUs and FPUs, the MMUs, the OCS/ECS/AGA chipset, Paula, the CIAs, input, RTG, sound and network
  cards, the RTC, Gayle/PCMCIA, Zorro, CD32 and CDTV. Every result is checked against the Motorola and Amiga
  hardware manuals. The runner writes a report detailed enough for an AI (or a person) to find the faulty
  emulator code.
- **The bench disk** says only how fast the machine is, for catching speed regressions while working on an
  emulator or an FPGA core. Each block does a fixed amount of work and reports how long it took; nothing runs
  for a fixed time. It checks that its work came out right, but contains no accuracy tests.

Both kinds share these properties:

- **Same tests, any engine:** with the JIT on, the same tests run and must give identical records.
- **No OS installation needed:** only the Kickstart ROMs you provide (or the AROS ROM that ships with Amiberry).
- **Isolated:** the runner never touches your own Amiberry configuration; everything lives in its own `work/`
  directory.
- **Emulator independent on the guest side:** the disks use only the Amiga's own hardware (beam counter, serial
  port, a memory mailbox), so they also run on real hardware and FPGA cores.
- **Shows progress on the Amiga's own screen:** each module's counts, the verdict, and which disk to insert next.
- **Designed to be safe on real hardware:** the disks write nothing they don't own.

Status: developed and tested against Amiberry. Running on real hardware and FPGA cores is designed in but not
tested yet (see [docs/USAGE.md](docs/USAGE.md) section 5).

## Documentation

| Document | For |
|---|---|
| [docs/BUILDING.md](docs/BUILDING.md) | **building the tool:** the toolchain, building and verifying the disks, generated tables, unit tests, writing test modules |
| [docs/USAGE.md](docs/USAGE.md) | **using the tool:** running it on an emulator, regression checks against a baseline, reading the report, reproducing a bug, real hardware |
| [docs/BENCH.md](docs/BENCH.md) | **the bench disk:** how blocks are timed, the tiers, the work plan, launches, the diff and its verdicts |
| [docs/RUNNER.md](docs/RUNNER.md) | reference for every runner command and option |
| [docs/schema.md](docs/schema.md) | every record and result field |
| [docs/CONTRACT.md](docs/CONTRACT.md) | the interfaces between the disks and the runner |

## Build it

You need an Amiga cross toolchain (vasm and bebbo's m68k-amigaos-gcc) in `/opt/amiga/bin`, or set
`AMIGA_TOOLCHAIN`, plus GNU make and Python 3.10+.

```
make -C disk             # the accuracy disk set: disk/build/set/amitorture-<n>-<group>.adf
make -C disk check       # verify every disk
make -C disk bench       # the bench disk: disk/build/amitorture-bench.adf
```

All the accuracy modules together don't fit on one 880 KB disk, so they are split over three disks:

| Disk | Contents |
|---|---|
| `amitorture-1-core.adf` | takeover, runtime, exceptions, self-modifying code, interrupts, CIAs, blitter, timing, display, copper, Paula, input, DMA contention, cycle-exact timing |
| `amitorture-2-cpu.adf` | CPU instructions, MMU, instruction dispatch |
| `amitorture-3-exp.adf` | FPU, RTG, sound and network cards, other hardware, devices, memory |

Each one is a complete, bootable payload that runs on a 1 MB (512 KB chip + 512 KB slow RAM) Kickstart 1.x
A500 or anything bigger. The bench disk needs the same on a 68000/68010, and 2 MB chip RAM on a 68020 or
higher (fast RAM recommended).

## Use it

You need the built disks, Python 3.10+ (standard library only), an Amiberry binary (`--amiberry PATH`, or
`amiberry` on `PATH`; amitorture never builds it) and a directory of Kickstart ROMs: `~/Amiberry/ROMs` by
default, or `--kickstarts DIR` / `$AMT_KICKSTARTS`. ROMs are identified by checksum, not by file name.

Emulators run headless; add `--show` to watch their windows. At most `nproc - 1` emulators run at once,
machine-wide (at least 1), so a core stays free for the runner and the host.

### Accuracy

From `runner/`:

```
python3 -m amitorture roms     --kickstarts ~/Amiberry/ROMs          # what was found, what can run
python3 -m amitorture run      --amiberry /path/to/amiberry --matrix smoke --jobs 4
python3 -m amitorture baseline --amiberry /path/to/amiberry --matrix smoke --name mybase --repeat 3
python3 -m amitorture check    --amiberry /path/to/new/amiberry --baseline mybase   # regressions and fixes
```

| Matrix | Entries | What it covers |
|---|---|---|
| `smoke` | 16 | one machine per CPU class and chipset (68000 to 68060, OCS/ECS/AGA, PAL/NTSC, AROS), JIT twins |
| `standard` | 145 | every CPU/FPU variant, every RTG, sound and network board, Gayle/PCMCIA, RTC boards, SCSI controllers, MMU configs, CD32, CDTV |
| `full` | 583 | adds every JIT sub-option, threaded and softfloat twins |

`python3 -m amitorture matrix --matrix NAME` lists the entries; `--config PATTERN` runs only some of them, and
`--adf PATH` runs one disk instead of the set.

Add `--warp auto` to accelerate the CPU/FPU/MMU disks (not on JIT entries, where warp is slower) while keeping
all other disks, including audio, input and RTC tests, normally paced. No tests are removed. `--warp off`
(the default) uses normal pacing everywhere; `--warp on` is an experimental forced mode, not a
correctness-equivalent run. Some timing-observation cases already vary between `pass` and `untestable` at
normal speed, so full-suite verdict equivalence is not yet established; see
[Automatic warp](docs/RUNNER.md#automatic-warp).

Exit status: 0 no problems, 1 problems found (read the report), 2 host-side error, 3 isolation check failed
(something else, such as your own Amiberry, wrote to your Amiberry files during the run), 130 stopped with
Ctrl+C.

### Benchmarks

Build the bench disk (`make -C disk bench`), then from `runner/`:

```
python3 -m amitorture run --amiberry ./amiberry-old --matrix bench
python3 -m amitorture run --amiberry ./amiberry-new --matrix bench \
    --bench-baseline ../work/results/<the first run>   # same work as the first run, then a per-block diff
python3 -m amitorture bench diff A_RESULTS B_RESULTS    # diff two finished runs
```

`--matrix bench` has 17 entries from 68000 to 68060, with JIT and threaded-CPU twins. The disk measures
instruction classes and memory paths, real workloads (dhrystone, zlib, AES-GCM ...), the emulator's own cost
per frame and MMU costs, and ends with an xsysinfo-like score on screen (DMIPS, FPU Mops, read MB/s, MMU). To keep the numbers meaningful on a desktop host:

- every emulator is pinned to the same fast host cores (`--pin`, default `auto`);
- each entry is launched twice (`--launches N`), and each block keeps its fastest launch. The spread between
  launches is the noise allowance of the diff;
- with `--bench-baseline`, the second run does exactly the same work as the first (its counts per block), and an
  entry with a block that came out slower is launched up to twice more before the verdict stands.

The diff (`BENCH-DIFF.md`) gives each block a same/borderline/slower/faster/noisy verdict, and the run exits
with 1 if anything got slower, failed or went missing. Use the same host, load and CPU governor for both runs.

To compare two binaries in one go, alternating them and repeating the launches:

```
python3 -m amitorture compare --amiberry ./amiberry-old --amiberry ./amiberry-new \
    --config 'bench-68020*' --repeat 3
```

This writes `COMPARE.md` with a verdict per block (exit status 1 if anything got slower).

On its own (real Amiga, FPGA, or an emulator without the runner) the bench disk is timed by the Amiga's own
clock and writes its results to the serial port; `amitorture import` turns such captures into a results dir
that `bench diff` accepts. See [docs/BENCH.md](docs/BENCH.md).

### Results

Results go to `work/results/<run-id>/` (or `--workdir DIR` / `$AMT_WORKDIR`):

| File | Contents |
|---|---|
| `REPORT.md` | start here: coverage, then one section per problem, with inputs, expected vs got, the manual rule, which setting flips it, emulator log lines and code pointers |
| `BENCH-DIFF.md` | bench runs with a baseline: the per-block verdicts |
| `summary.json`, `failures.jsonl` | the same, machine-readable |
| `repro/` | a minimal reproduction per problem |
| `configs/` | each entry's Amiberry config, as written and as Amiberry resolved it |
| `raw/` | every record, emulator log, screenshot (including `final.png`, the final status screen) and audio capture |

Known emulator bugs are registered in `runner/amitorture/xfail.json`. A failure that matches a registered bug
shows as `xfail`, and as `fixed` once a build no longer has the bug. Local evidence reports can be kept in
`docs/findings/`, which is ignored by Git and not shipped with the repository.

## Layout

| Path | Contents |
|---|---|
| `disk/` | bootblock (`boot/`), bare-metal runtime (`rt/`), CPU/chipset probe (`probe/`), accuracy test modules (`modules/`), the bench disk (`bench/`) |
| `tools/` | ADF builder, table generators (reference models), bench kernel-set tools, emulator slot limiter |
| `runner/` | the host runner (`python3 -m amitorture`) and its unit tests |
| `tests/smoke_disk.py` | boot one disk on one config, quickly |
| `docs/cases/` | one line per test case: intent, rule, what a failure means |
| `docs/catalogue.yaml` | what an Amiga can do, and what amitorture covers |
| `docs/emulator-map/` | where in the emulator's source each area lives, for the report's code pointers |
| `docs/findings/` | local, Git-ignored evidence reports and suspected emulator code |
| `docs/review/` | code reviews of amitorture itself and their resolutions |

## Licence

MIT, see [LICENSE](LICENSE); [COPYING.md](COPYING.md) notes the one derived data file. No ROM images are included.
