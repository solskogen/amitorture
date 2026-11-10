# amitorture

A torture test and regression finder for Amiga emulators, Amiberry first.

A set of bootable test disks (ADFs) exercises the 68000-68060 CPUs and FPUs, the MMUs, the OCS/ECS/AGA chipset, Paula, the CIAs, input, RTG, sound and network cards, the RTC, Gayle/PCMCIA, Zorro, CD32 and CDTV. Every result is checked against the Motorola and Amiga hardware manuals. A host runner boots the disks in the emulator across a matrix of machines and settings, and writes a report detailed enough for an AI (or a person) to find the faulty emulator code.

- **Same tests, any engine:** with the JIT on, the same tests run and must give identical records.
- **No OS installation needed:** only the Kickstart ROMs you provide (or the AROS ROM built into Amiberry).
- **Isolated:** never touches your own Amiberry configuration; everything lives in its own `work/` directory.
- **Shows its progress on the Amiga's own screen:** each module's counts, the verdict, and which disk to insert next.
- **Designed to be safe on real hardware:** it writes nothing it doesn't own.

Status: developed and tested against Amiberry. Running on real hardware and FPGA cores is designed in but not tested yet (see [docs/USAGE.md](docs/USAGE.md) section 5).

## Documentation

| Document | For |
|---|---|
| [docs/BUILDING.md](docs/BUILDING.md) | **building the tool:** the toolchain, building and verifying the test disks, generated tables, unit tests, writing test modules |
| [docs/USAGE.md](docs/USAGE.md) | **using the tool:** running it on an emulator, regression checks against a baseline, reading the report, reproducing a bug, real hardware |
| [docs/RUNNER.md](docs/RUNNER.md) | reference for every runner command and option |
| [docs/schema.md](docs/schema.md) | every record and result field |
| [docs/CONTRACT.md](docs/CONTRACT.md) | the interfaces between the disks and the runner |

## Build it

You need an Amiga cross toolchain (vasm and bebbo's m68k-amigaos-gcc) in `/opt/amiga/bin`, or set `AMIGA_TOOLCHAIN`, plus GNU make and Python 3.10+.

```
make -C disk             # the disk set: disk/build/set/amitorture-<n>-<group>.adf
make -C disk check       # verify every disk
make -C disk benchmarks  # standalone benchmark-only disk/build/benchmarks/amitorture.adf
```

All the modules together don't fit on one 880 KB disk, so the test is a set of three disks. Each one is a complete, bootable payload that runs on a 1 MB (512 KB chip + 512 KB slow RAM) Kickstart 1.x A500 or anything bigger.

## Use it

You need the built disks, Python 3.10+ (standard library only), an Amiberry binary (`--amiberry PATH`, or `amiberry` on `PATH`; amitorture never builds it) and a directory of Kickstart ROMs: `~/Amiberry/ROMs` by default, or `--kickstarts DIR` / `$AMT_KICKSTARTS`. ROMs are identified by checksum, not by file name.

From `runner/`:

```
python3 -m amitorture roms     --kickstarts ~/Amiberry/ROMs          # what was found, what can run
python3 -m amitorture run      --amiberry /path/to/amiberry --kickstarts ~/Amiberry/ROMs \
                               --matrix smoke --jobs 4               # 12 machines; also: standard, full
python3 -m amitorture baseline --amiberry /path/to/amiberry --kickstarts ~/Amiberry/ROMs \
                               --matrix smoke --name mybase --repeat 3
python3 -m amitorture check    --amiberry /path/to/new/amiberry --kickstarts ~/Amiberry/ROMs \
                               --baseline mybase                     # regressions and fixes vs the baseline
```

Emulators run headless; add `--show` to watch their windows. At most `nproc - 1` emulators run at once, machine-wide (at least 1), so a core stays free for the runner and the host.
For lightweight measurements rather than correctness tests, build `benchmarks` and run:

```
python3 -m amitorture run --amiberry /path/to/amiberry --kickstarts ~/Amiberry/ROMs \
    --matrix benchmarks --jobs 1 --warp off
```

This covers 68000 through 68060, with interpreter/JIT and **threaded CPU off/on pairs** where supported.
Unsupported CPU/FPU/MMU workloads are explicitly skipped. The report includes paired throughput and
host CPU-cost tables; benchmark measurements do not impose a speed pass/fail threshold.
Booted on a real Amiga or an FPGA, the benchmark disk shows its progress and an integer score
(A500 = 1.00) plus an FPU score on screen.
See [hostspeed.md](docs/cases/hostspeed.md#benchmark-only-build-and-threaded-cpu-comparisons).
To check a change for performance regressions, compare two binaries:

```
python3 -m amitorture compare --amiberry ./amiberry-old --amiberry ./amiberry-new \
    --kickstarts ~/Amiberry/ROMs --config 'bench-68020*' --repeat 3
```

It alternates the binaries, repeats the launches and writes COMPARE.md with a slower/faster/borderline/same
verdict per block (exit status 1 if anything got slower).

Add `--warp auto` to accelerate CPU/FPU/MMU set disks (not on JIT entries, where warp is slower) while keeping all other disks, including audio,
input, RTC and host-speed tests, normally paced. No tests are removed. `--warp off` uses normal pacing
everywhere; `--warp on` is an experimental forced mode, not a correctness-equivalent run.
Some timing-observation cases already vary between `pass` and `untestable` at normal
speed, so full-suite verdict equivalence is not yet established; see [Automatic warp](docs/RUNNER.md#automatic-warp).

Results go to `work/results/<run-id>/`:

| File | Contents |
|---|---|
| `REPORT.md` | start here: coverage, then one section per problem, with inputs, expected vs got, the manual rule, which setting flips it, emulator log lines and code pointers |
| `summary.json`, `failures.jsonl` | the same, machine-readable |
| `repro/` | a minimal reproduction per problem |
| `raw/` | every record and emulator log |

Known emulator bugs are registered in `runner/amitorture/xfail.json`. Local evidence reports can be kept in
`docs/findings/`, which is ignored by Git and not shipped with the repository. A failure that matches a
registered bug shows as `xfail`, and as `fixed` once a build no longer has the bug.

## Layout

| Path | Contents |
|---|---|
| `disk/` | bootblock, bare-metal runtime (`rt/`), CPU/chipset probe (`probe/`), test modules (`modules/`) |
| `tools/` | ADF builder, table generators (reference models), emulator slot limiter |
| `runner/` | the host runner (`python3 -m amitorture`) and its unit tests |
| `tests/smoke_disk.py` | boot one disk on one config, quickly |
| `docs/cases/` | one line per test case: intent, rule, what a failure means |
| `docs/catalogue.yaml` | what an Amiga can do, and what amitorture covers |
| `docs/findings/` | local, Git-ignored evidence reports and suspected emulator code |
| `docs/review/` | code reviews of amitorture itself and their resolutions |

## Licence

MIT, see [LICENSE](LICENSE); [COPYING.md](COPYING.md) notes the one derived data file. No ROM images are included.
