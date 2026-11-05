# Building amitorture

This covers building the test disks and changing the tool: the toolchain, the disk build, the generated tables, the unit tests and writing test modules. To run the tool on an emulator and read its results, see [USAGE.md](USAGE.md).

amitorture never builds the emulator. Only the test disks are built.

---------------------------------------------------------------------------
## 1. Requirements

| What | Used for |
|---|---|
| `vasmm68k_mot` (vasm 2.x) | bootblock and assembler parts |
| `m68k-amigaos-gcc` (bebbo, with binutils) | the runtime and modules in C; also links the payload |
| `m68k-amigaos-objdump` | disassembly in the generated case docs |
| NDK 3.x headers (`m68k-amigaos/ndk-include`, shipped with bebbo's toolchain) | exec structures for the OS-phase code |
| GNU make, Python 3.10+ (stdlib only) | the build, `tools/*.py`, the runner |

Everything is expected in `/opt/amiga/bin`. Otherwise:
```
make -C disk AMIGA_TOOLCHAIN=/path/to/bin      # NDK_INCLUDE=… if the headers are elsewhere
```

---------------------------------------------------------------------------
## 2. Building the test disks

```
make -C disk             # the disk set
make -C disk check       # verify every disk: bootblock checksum, payload CRC, relocations
make -C disk clean
```

`make` builds the **disk set**: seven bootable ADFs in `disk/build/set/`, one per module group. Each disk is a complete payload: bootblock, runtime, probe and its own modules.

| Disk | Modules |
|---|---|
| `amitorture-1-core.adf` | takeover rt flags exceptions smc interrupts cia blitter timing |
| `amitorture-2-cpu.adf` | cpu_basic |
| `amitorture-3-fpu.adf` | fpu |
| `amitorture-4-chip.adf` | display copper paula input |
| `amitorture-5-exp.adf` | rtg expsound network misc_hw misc_hw_os devio |
| `amitorture-6-cpux.adf` | memory memory_os dispatch dmacontention hostspeed |
| `amitorture-7-mmu.adf` | mmu |

The build prints each disk's size in memory. Every disk must stay under about 300 KB so that it runs on a 512 KB Kickstart 1.x machine. All the modules together are too big for one 880 KB ADF, which is why the set exists.

The runner picks up `disk/build/set` by itself. Rebuild after any change under `disk/` or to a generator in `tools/`.
C objects have compiler-generated `.d` dependencies, so changes to module-local headers also rebuild their
consumers. A Makefile change invalidates existing C objects, including objects built before dependency tracking.

Variables:

| Variable | Effect |
|---|---|
| `MODS="rt cia"` | build one ADF with only these modules, `$(B)/amitorture.adf` (development) |
| `B=dir` | build directory, default `build`. Use one per `MODS` selection, so different builds don't share objects. |
| `BUILD_ID=…` | string baked into the disk header (default: git revision and date). The set build appends ` <n>/<N> <group>` (e.g. `1/7 core`); the status screen shows "disk n of N" from it when the id before it has at least two words, as the default has. |
| `PARAMS="mode=quick seed=5"` | default run parameters baked into the disk (the keys are listed at the top of `tools/mkadf.py` and in [USAGE.md](USAGE.md) section 5). The runner patches its own parameters into a copy anyway. |

`tools/mkadf.py --dump some.adf` shows any disk's header, baked-in parameters, sizes and CRCs.
It rejects payload/relocation ranges and sector spans that extend beyond the physical image, even if the
available bytes have a matching CRC.

---------------------------------------------------------------------------
## 3. Generated tables

Four modules use tables made by host-side reference models. The generated files are checked in, so a normal build doesn't run the generators.

| Generator | Output |
|---|---|
| `tools/gen_cpu_basic.py` | `disk/modules/cpu_basic_tab.s`, `docs/cases/cpu_basic.md` |
| `tools/gen_fpu.py` | `disk/modules/fpu_gen.s` |
| `tools/gen_blitter.py` | `disk/modules/blitter_tab.c` |
| `tools/gencode.py` | `disk/modules/dispatch_tab.s`, `docs/cases/dispatch.md` |

```
make -C disk check-gen   # regenerate all of them into work/tmp and compare: "ok" or "DRIFT"
```

After changing a generator, rerun it with the output paths shown in `tools/check_generated.sh`, and commit the generator and its output together.

---------------------------------------------------------------------------
## 4. Tests of the tool itself

```
cd runner && python3 -m unittest discover -s tests       # runner unit tests, no emulator needed
make -C disk check check-gen check-groups                # disks, generated tables, disk groups
```

Run both before a commit or push; there is no hosted CI (it could neither build the disks, without the Amiga toolchain, nor run them, without an emulator).

To try a disk on an emulator without the runner, see [USAGE.md](USAGE.md#boot-one-disk-quickly-without-the-runner) (`tests/smoke_disk.py`).

---------------------------------------------------------------------------
## 5. Writing a test module

1. Read [MODULE_AGENT_BRIEF.md](MODULE_AGENT_BRIEF.md) (the rules every module follows) and [CONTRACT.md](CONTRACT.md) §10: the module API in `disk/rt/mod.h`, phases, records and the module id table. `disk/modules/rt.c` is a small example.
2. Create `disk/modules/<name>.c` with `AMT_MODULE(name, id, phase, flags, chip_bytes, any_bytes)`, using a free id from the table in CONTRACT.md §10. The flags (`disk/rt/mod.h`):
   - `AMT_MF_LONG`: watchdog limit ×10 (hard limit ×4);
   - `AMT_MF_NEEDS_FPU`: the whole module is one `module` skip on a machine without an FPU;
   - `AMT_MF_OWNS_DISPLAY`: see below.

   **The status screen and `AMT_MF_OWNS_DISPLAY`.** A bare module without the flag starts with DMAEN, BPLEN and COPEN on: the copper runs the runtime's list that shows the status screen (`disk/rt/screen.c`: one lores bitplane, about 30 copper moves at the top of the frame). Set the flag if the module tests the display, the copper, the blitter, Paula's DMA or interrupts, or measures DMA or beam timing; in short, if its results depend on which DMA runs, or it needs DMACON, the copper or the bitplane and colour registers to itself. It then starts with all DMA off and the screen blank (`run*` on the screen). The modules that set it today: blitter, copper, display, dmacontention, interrupts, paula, timing. A module without the flag should leave DMAEN, BPLEN, COPEN, COP1LC and the display registers alone: the runtime puts them back after the module, but the screen is broken meanwhile. With the `noscreen` run flag every bare module starts with all DMA off.
3. Build and boot just that module:
   ```
   make -C disk B=build-x MODS="rt <name>"
   python3 tests/smoke_disk.py --adf disk/build-x/amitorture.adf --model A1200 --cpu 68020 --chipset aga --rom /path/to/kick31.rom
   ```
4. Add it to exactly one `DISK_SET_<group>` in `disk/Makefile`. `make -C disk check-groups` fails if a module is in no group or in two. Keep the disk under the memory limit from 2, and at 10 modules or fewer: the status screen has 10 module rows, and modules after the tenth are not shown.
5. Write `docs/cases/<name>.md`: one line per case with its intent, the rule and what a failure usually means. The report quotes it.
6. If the module adds a capability, list it in `docs/catalogue.yaml` so the coverage table shows it.
7. Run a smoke matrix (USAGE.md) with JIT twins. The JIT run must produce exactly the same records as the interpreter.

---------------------------------------------------------------------------
## 6. Where things are

| Path | Contents |
|---|---|
| `disk/boot/` | bootblock: loads and relocates the payload |
| `disk/rt/` | bare-metal runtime: takeover and restore, records, serial, mailbox, exceptions, IRQs, watchdog, CPU state guard, status screen (`screen.c`) |
| `disk/probe/` | CPU/FPU/MMU/chipset/memory/board identification |
| `disk/modules/` | test modules |
| `tools/` | `mkadf.py` (ADF builder), the generators, `amislot.py` (emulator limiter), `check_generated.sh` |
| `runner/amitorture/` | the host runner |
| `docs/CONTRACT.md` | interfaces between disk and runner: disk layout, parameters, records, mailbox, host requests |
| `docs/schema.md` | every record and result field |
| `docs/emulator-map/amiberry.md` | how failures map to Amiberry source (used for code pointers) |
