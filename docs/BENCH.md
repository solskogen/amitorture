# The bench disk

`make -C disk bench` builds `disk/build/amitorture-bench.adf`: one disk, for every CPU, that
measures how fast a machine runs a fixed set of work. It is for regression checking while
working on an emulator or an FPGA core: run it before and after a change, and compare.

The accuracy disks (`make -C disk`, the disk set) say whether the machine behaves correctly.
The bench disk says only how fast it is. It contains no accuracy tests.

## What the disk needs

| CPU | Memory |
|---|---|
| 68000, 68010 | 1 MB: 512 KB chip + 512 KB slow or fast RAM (A500 with trapdoor RAM) |
| 68020 and up | 2 MB chip RAM; fast RAM (Zorro II/III or CPU board) recommended |

Without fast RAM, the kernels and the micro blocks run from chip or slow RAM, the
`mem_read_fast` block is skipped, and the records say where the memory was (`mem`).
Compare only runs that used the same memory.

## Guest side: the same disk everywhere

The disk does not know what it runs on. It uses only:
- the beam counter as its clock (`amt_ticks()`: colour clocks, 3546895 per second PAL),
  with CIA-B's TOD line counter as a check on it (below, "Lost frames");
- the serial port and the memory mailbox for its output (CONTRACT §5-7).

With run-parameter flag `host` (512), which the runner sets, the disk also asks the host
whether it needs longer or more repetitions (`bench_longer`, `bench_more`, CONTRACT §13).
Without that flag (real hardware, FPGA) it waits on nothing. Each block then takes as long
as its work takes and the guest's own clock decides when the timing is good enough.

## How a block is run

A **block** is a fixed amount of work, `run(count)`. It returns a checksum of its results.
For every block the harness (`disk/bench/harness.c`):

1. **Warm-up.** Runs `run(1)` once (JIT translation, caches). Its time is reported as `cold`.
2. **Sizing.** Doubles `count` from 1 until one run takes at least 100 ms of the guest's
   clock. Without a host, a run must also be at least 400 times the clock's resolution
   (`res_ticks`), which is measured at startup. With a host, the run is long enough when
   the host says so (`bench_longer` refused or unanswered). The sizing run that qualifies
   counts as the first repetition. With a work plan (below), `count` comes from the plan
   and there is no sizing.
3. **Repetitions.** Runs `run(count)` again until the fastest two repetitions agree within
   0.25%, at most 12 times. With a host, the host decides (`bench_more`). A block that
   reaches 12 repetitions without agreement is reported `unconfirmed`.
4. **Check.** After the first and the last repetition the results are checked:
   - kernels: the checksum must equal the one computed on the host at build time
     (`disk/bench/kernels/kexpect.h`). `k020s` (soft float) must give IEEE single precision
     results. `k020f` runs with the FPCR at single precision and is built with
     `-fexcess-precision=standard`: the FPU rounds every result to a single's mantissa but keeps
     the extended exponent range in registers. Its reference comes from the same code built for
     the x87 at single precision, which does the same, and can differ from IEEE single (it does
     for `nfs3-clip`). The checksum doesn't depend on whether the FPU computes in single, double
     or extended precision internally, as long as it rounds as told: Amiberry's exact FPU and
     its JIT both give the reference. An FPU that ignores the FPCR's rounding precision
     (Musashi, the MiSTer Minimig core's) gives the second, "wide" checksum in kexpect.h,
     the same build on the x87 at extended precision. That counts as correct, with the note
     that the FPU ignored the rounding precision: the work and its time are the same;
   - micro blocks: all repetitions must return the same checksum, except blocks that read
     changing hardware registers.

   Wrong results give `st: fail`. The timing is still reported.

Every timed run starts just after a frame edge. Right before it the guest sends a `go`
record and sets the mailbox position to `$10000 | id << 8 | rs` (`rs`: the run's sequence
number, 1-255); the position is 0 between runs. Nothing is sent during a run, so a host that
stamps the arrival of serial bytes times the run from the `go` record's newline to the
first byte of the `run` record. The runner does that (below).

Time is what the work takes. Nothing runs for a fixed number of seconds. A faster machine
gets a larger `count`, so the time per repetition stays about the same.

### Linearity

At every doubling during sizing, the harness checks that doubling `count` doubles the time
the run adds: (time(n) − time(n/2)) should be twice (time(n/2) − time(n/4)), within 2 % of
the latter plus 1/16 frame. The 1/16 frame covers per-frame costs, such as interrupts,
that a run picks up or misses depending on where in the frame it starts. A lost frame is
far outside it.
Comparing the added time means that a fixed cost per run, such as a frame block starting
part-way into a frame, does not count against it. Steps too short to judge are skipped:
the added time must be at least 12.5 ms and 100 × `res_ticks`. Very short runs are noise,
and one can take less time than the run before it; once a run is 12.5 ms or longer, the
next one taking less time fails the check. If any step fails, the end
record has `nonlinear: 1`. That means the clock does not follow the work, for example an
emulator whose emulated time does not advance with CPU work (JIT). Without a host the
guest's clock is the only one, so the block then ends `unconfirmed` even if its
repetitions agree, and the runner does not confirm its guest times either. With a host,
the host's times are used and the flag is only a remark.

### Lost frames

`amt_ticks()` counts frames in the VERTB interrupt. A machine that serves that interrupt
more than a frame late loses a whole frame from the clock: two vertical blanks are
counted as one. One FPGA core did this during back-to-back CIA writes (`cia_write`), and
its times were short by whole frames. CIA-B's TOD counter counts lines by itself, without
an interrupt. At startup the harness checks it against the beam alone, over 150 lines
of one frame, up to four times (env `tod: 1` if it counts them; otherwise `tod_lines` and
`tod_beam` give the counts of the last try). A counter that doesn't move at all is
restarted once by writing its low byte, as a write to its high byte stops it. If it does, every timed run also reads it, and a run whose
line count says it took more frames than `amt_ticks()` counted has the missing frames
added back. Its `run` record then has `lost_frames`.

## Tiers

### Micro tier (`bench_micro`, module 40)

One instruction class or memory path per block (`disk/bench/micro_asm.s`). A block is
unrolled bodies of 64 instructions; `per` is the instruction count of one `count`. A block
runs only on a CPU or FPU that implements its instructions in hardware.

- **Integer:** empty, add_l, div_w, abcd_sbcd, roxl_roxr, movem, movep, trap_rte, mul_l,
  mul_l64, div_l, div_l64, bfextu, bfffo, cas, cas2, chk2, pack_unpk.
- **System:** custom_read, cia_read, movec_vbr, pmove_tc, move16, movec_pcr.
- **Memory:** mem_read_chip (bitplane, copper and sprite DMA off), mem_read_fast,
  mem_read_rom. Each reads 64 KB per `count`, in MOVEM.L bursts.
- **FPU:** fadd, fmul, fdiv, fsqrt, frem, fscale, fintrz, fgetexp, fsin, fatan, fetox,
  flogn, fmovecr, fmove_p_in, fmove_p_out, fmovem, fsave_frestore.

What is skipped, and why:
- On a 68060: what it traps as unimplemented (MOVEP, CAS2, CHK2, 64-bit MUL/DIV).
- On a 68040 or 68060: what the FPSP would emulate (transcendentals, FREM, FSCALE,
  FGETEXP, FMOVECR, packed decimal; also FINTRZ on the 68040).
- CAS and CAS2 when their memory is on the chip bus.

### Kernel tier (`bench_kernels`, module 41)

Real workloads from benchwork, in kernel sets compiled per CPU class (`disk/bench/kernels.mk`):

| Set | Runs on | Kernels |
|---|---|---|
| `k000` | every CPU | integer kernels (dhrystone, scene-2d, zlib deflate/inflate, ftgrays, memcpy-large, aes-gcm, chacha20-poly1305) and the float kernels with soft float |
| `k020` | 68020 and up | the integer kernels, compiled for 68020-68060 |
| `k020f` | 68020 and up with an FPU | the float kernels using the FPU |
| `k020s` | 68020 and up without an FPU | the float kernels with soft float |

A 68020 or later runs both `k000` and its own sets. The 68000 code under a JIT is a useful
measurement too.

Each kernel gets its memory from a 160 KB arena in the module's pool, fast RAM when there is
some. Plan ids: `k000` 64+, `k020` 96+, `k020f` 128+, `k020s` 160+, in set order.

### Frame and thread tiers (`bench_system`, module 42)

These blocks measure what an emulator spends besides executing instructions. One `count`
is one frame, so the guest's clock can only say that a frame took a frame. They therefore
need a host that times them with its own clock, and they are skipped without one:

| Block | Tier | What |
|---|---|---|
| `stop` | frame | the CPU idle in STOP, woken by the VBL: the emulator's own cost |
| `vbl_poll` | frame | busy-waiting on the beam, as games do |
| `chipset_load` | frame | STOP under a busy display (6 planes, 8 sprites, copper colour changes on every line) |
| `cia_write` | thread | back-to-back CIA writes (runs without a host too) |
| `stop_timer` | thread | STOP woken 1500 times a second by a CIA timer, as an idle AmigaOS |
| `flood_recovery` | thread | a CIA write flood, then quiet integer work; mailbox position `$20000` during the quiet phases |

## The work plan (CONTRACT §15)

A fresh disk sizes every block itself. A baseline run records each block's `count`. The
runner can write those counts into the disk (`mkadf.py --plan`), so that later runs do
exactly the same work as the baseline and skip sizing. The plan can also skip blocks, or
run only the listed blocks (`exclusive`).

## Records

`bench` records (docs/schema.md §1.11):
- `env` once;
- for every block, `begin`, a `go` and a `run` per timed run, and `end`;
- or a single `skip`.

All times are in guest colour clocks.

## Real hardware and FPGA cores

Write `amitorture-bench.adf` to a floppy (or a Gotek, or the core's disk image slot) and boot it.
The status screen shows each module's progress. The results go to the serial port: 115200 baud,
8N1, no handshake (docs/USAGE.md §5, "Real hardware and FPGA", has the cable,
capture and baud-rate details).

Without the runner, every block is timed by the Amiga's own clock (`basis: guest`). The frame
and thread tiers need a host clock, so they are skipped. To check a core change:

```
python3 -m amitorture import before.txt --label core-before
python3 -m amitorture bench plan ../work/results/core-before -o plan.json        # optional: freeze the counts
python3 ../tools/mkadf.py --in ../disk/build/amitorture-bench.adf -o fixed.adf --plan plan.json
# boot fixed.adf on the changed core, capture after.txt
python3 -m amitorture import after.txt --label core-after
python3 -m amitorture bench diff ../work/results/core-before ../work/results/core-after
```

With the plan, both runs do exactly the same work. Without it, a run sizes each block itself, and
two runs may pick different counts. The values are per unit, so they still compare, and the diff
notes the different count.

## The runner (Amiberry)

The guest side above works anywhere. Everything below is the runner's, and is specific to
Amiberry.

### Running it

```
amitorture run --amiberry BIN --matrix bench [--config 'bench-68020*'] [--bench-baseline RESULTS_DIR] [--pin auto|CPUS|none] [--launches N]
amitorture bench diff A_RESULTS B_RESULTS [-o BENCH-DIFF.md]
amitorture bench plan RESULTS [--entry ID] [-o plan.json]
amitorture compare --amiberry OLD --amiberry NEW [--repeat 3] [--threshold 1] [--pin 2-5]
```

`--matrix bench` (15 entries, runner/amitorture/matrix.py `bench_entries`) runs the bench disk on
68000, 68000 cycle-exact, 68010, 68020 (JIT, threaded CPU, JIT + threaded, threaded with one host
CPU), 68030, 68EC030 (+JIT), 68040 without MMU (+JIT) and 68060 without MMU (+JIT). A run takes
either bench entries or accuracy entries, never both. It always runs one launch at a time.

**Host CPUs.** A bench run pins every emulator to the same host CPUs (`run --pin`, default
`auto`): the 4 fastest-boosting physical cores, one thread each, CPU 0 last (ranked by
`amd_pstate_prefcore_ranking`, ACPI CPPC `highest_perf` or the maximum frequency). The cores of
one CPU may boost to rather different clocks, and an unpinned launch runs on whichever core the
scheduler picks. `--pin 2-5` picks the CPUs, `--pin none` turns it off. Each entry records
`pinned_to`; the diff notes when the two runs used different CPUs. For steadier numbers, also
use the `performance` governor (the host's governor and boost are recorded in each run).

**Launches.** Pinning does not make one launch repeatable. In A/A runs on a desktop host
(Ryzen 9 9950X, pinned), a block's value moved by a median of 0.9% between two launches of the
same binary, and 5% of blocks by more than 4%. Some blocks moved by up to 68%, even though each
launch had confirmed its time to 0.25%. Disabling address-space randomisation did not help.
So a bench run launches each entry twice by default (`run --launches N`), in rounds: every
entry once, then every entry again with the first round's counts. Each block keeps its fastest
launch (`value`), its per-launch values (`launch_values`) and their spread (`launch_spread_pct`,
slowest over fastest). A failed launch of a block wins over a faster one, and the entry's status
is the worst launch's. Raw files of launch 2 and later are in `raw/<entry>/launch<k>/`.
Sometimes both launches of a run land on the same slow mode (a 68060 JIT block that came out 1.18
in five runs and 1.30 in both launches of the sixth). So with `--bench-baseline`, an entry with a
block the diff calls slower is launched again, up to twice more (`bench.RECHECKS`), and the diff
is made again over all its launches. A real regression is slower on every launch, so it stays
slower, at the cost of those extra launches. A run without a baseline calibrates its counts in
launch 1, which came out about 1–5% slower than launch 2 per entry. Its fastest values are still
right, but the spread is wider, so a diff against it is a little less sensitive than one against
a run that had a baseline.

**Pacing.** The 68000/68010 entries (cpu_speed=real, no JIT, no threaded CPU) run warped: the
emulator runs as fast as it can instead of in step with real time, so their timings say what
the emulator costs and not how fast the real machine was. The 020+ entries run at cpu_speed=max,
which never waits for real time anyway. Each block records its `pacing` (`warp` or `paced`);
results with different pacing are not compared.

**Timing.** The runner stamps each serial byte with the host's monotonic clock and its process
CPU time as it arrives (TCP_NODELAY, one write per byte). A timed run is the time from the newline of its
`go` record to the first byte of its `run` record. While a run is open the runner does not poll
the emulator over IPC, because each IPC request costs the emulation thread up to a few
milliseconds. The exception is `flood_recovery`: it needs samples of the threads' CPU time while it runs.

**Answering the guest** (CONTRACT §13):
- `bench_longer`: ack (make the run longer) while the last run took less than 0.2 s of host time
  (0.5 s for the cost blocks), or when the host has no time for it.
- `bench_more`: ack while the two fastest repetitions differ by more than 0.25% (1% for the
  cost blocks). It always refuses at 12 repetitions.

**Values** (`summary.json` `bench`, docs/schema.md §5): each block's `value` is from its fastest
repetition:
- micro and kernel tiers: host ns per unit (`ns_per_unit`), or guest ns when the host could not
  time it (`basis: guest`);
- frame and thread tiers: host CPU seconds per emulated second (`cpu_per_emu_s`), and `rtf`
  (host seconds per emulated second; above 1 means slower than real time).

A block is `confirmed` when its two fastest repetitions agree.

### Comparing two runs

`run --bench-baseline DIR` first writes each entry's counts from the baseline run into its
disk (the work plan), so both runs do exactly the same work. After the run it writes
`BENCH-DIFF.md` and `summary.json` `bench_diff`, and returns exit status 1 when the diff is not
OK. `bench diff A B` does the same for two finished runs.

Verdict per block, B against A (lower is better):

| verdict | when |
|---|---|
| same | within 1% (`ns_per_unit`) or 2% (`cpu_per_emu_s`) |
| borderline | within 3% or 5% |
| slower / faster | beyond that, and beyond 1.5 × the sum of both runs' `launch_spread_pct` (a side with one launch: both sides confirmed) |
| noisy | beyond borderline, but within that launch allowance (one launch: a side is unconfirmed) |
| fail | a side's work came out wrong |
| incomparable | different kernel set, memory, timing basis, pacing or units per count, or a different checksum at the same count (the disk build changed the block's results) |
| skipped, new, info | nothing to compare (`flood_recovery` is info only) |
| incomplete | the block or entry is missing from one run |

The diff fails (exit 1) on any slower, fail or incomplete block, or when nothing was
compared. A noisy block is listed but does not fail it: the runs cannot tell. On the A/A runs
above, these rules flag 0–1 of 284 blocks in simulation; a real A/A run with two launches had 1
slower block before the recheck, which it covers, and 5 faster. A 5% slowdown is flagged in about 80% of the blocks,
a 10% slowdown in about 94%; one launch with the old 0.5%/1% limits flagged 18–37 blocks slower and
over 100 noisy on A/A. It also gives the geometric mean change per entry and tier. That is a summary only: a
regression in one block can hide in it, so only the per-block verdicts decide.

`compare` runs both binaries `--repeat` times, alternating which goes first, each launch with
`--bench-baseline` of the first launch. It compares the medians of the repeats: **slower** or
**faster** when they differ by more than `--threshold` percent (default 1) and the repeats do not
overlap, **borderline** when they overlap. Writes `COMPARE.md` and `compare.json`; exit status 1
when any block is slower.

### Threaded CPU checks

A threaded entry is compared with its unthreaded twin in the same run (same binary, parameters
and options except `cpu_threaded`) on the frame and thread blocks: falling behind real time, a
spinning idle CPU thread, poor flood recovery, repeated refloods, and a CPU thread with one host
CPU each give a `thread_speed` item in the report verdict. The rules and limits: docs/schema.md §5.6a.

A bench block whose work comes out wrong is a `case_fail` item (`mod` `bench`).

The bench disk replaces the accuracy disks' former `hostspeed` module (id 26, retired). Baselines
that still hold its `host.speed_*` cases report them as `retired`, never as regressions.
