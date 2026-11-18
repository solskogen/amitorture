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
- the beam counter as its clock (`amt_ticks()`: colour clocks, 3546895 per second PAL);
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
     (`disk/bench/kernels/kexpect.h`);
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

When sizing reaches the floor, the harness compares the time of `count` with the time of
`count/2`. It should be close to double (between 1.96 and 2.04 times). If it is not, the
end record has `nonlinear: 1`. That means the clock does not follow the work, for example
an emulator whose emulated time does not advance with CPU work (JIT).

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
amitorture run --amiberry BIN --matrix bench [--config 'bench-68020*'] [--bench-baseline RESULTS_DIR]
amitorture bench diff A_RESULTS B_RESULTS [-o BENCH-DIFF.md]
amitorture bench plan RESULTS [--entry ID] [-o plan.json]
amitorture compare --amiberry OLD --amiberry NEW [--repeat 3] [--threshold 1] [--pin 2-5]
```

`--matrix bench` (15 entries, runner/amitorture/matrix.py `bench_entries`) runs the bench disk on
68000, 68000 cycle-exact, 68010, 68020 (JIT, threaded CPU, JIT + threaded, threaded with one host
CPU), 68030, 68EC030 (+JIT), 68040 without MMU (+JIT) and 68060 without MMU (+JIT). A run takes
either bench entries or accuracy entries, never both. It always runs one launch at a time.

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
| same | within 0.5% (`ns_per_unit`) or 2% (`cpu_per_emu_s`) |
| borderline | within 1% or 5% |
| slower / faster | beyond that, and both sides confirmed |
| noisy | beyond the first limit, but one side is unconfirmed |
| fail | a side's work came out wrong |
| incomparable | different kernel set, memory, timing basis or pacing |
| skipped, new, info | nothing to compare (`flood_recovery` is info only) |
| incomplete | the block or entry is missing from one run |

The diff fails (exit 1) on any slower, fail, noisy or incomplete block, or when nothing was
compared. It also gives the geometric mean change per entry and tier. That is a summary only: a
regression in one block can hide in it, so only the per-block verdicts decide.

`compare` runs both binaries `--repeat` times, alternating which goes first, each launch with
`--bench-baseline` of the first launch. It compares the medians of the repeats: **slower** or
**faster** when they differ by more than `--threshold` percent (default 1) and the repeats do not
overlap, **borderline** when they overlap. Writes `COMPARE.md` and `compare.json`; exit status 1
when any block is slower.

### Threaded CPU checks

A threaded entry is compared with its unthreaded twin in the same run (same binary, parameters
and options except `cpu_threaded`). These give `thread_speed` items (docs/schema.md §5.6a) in the
report verdict:
- a frame or thread block falls behind real time where the twin keeps up;
- `stop` or `stop_timer` costs much more host CPU than the twin (the idle CPU thread spins);
- `flood_recovery`: the CPU thread runs less than 60% of the quiet work after a flood;
- the CPU goes back to its thread and floods again at once, repeatedly (`refloods`);
- with one usable host CPU, the emulator still runs the CPU on its own thread.

A bench block whose work comes out wrong is a `case_fail` item (`mod` `bench`).

The bench disk replaces the accuracy disks' former `hostspeed` module (id 26, retired). Baselines
that still hold its `host.speed_*` cases report them as `retired`, never as regressions.
