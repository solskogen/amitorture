# amitorture disk build (GNU make)
#
#   make            the disk set: build/set/amitorture-<n>-<group>.adf (all modules)
#   make MODS="rt x" one ADF with only those modules: build/amitorture.adf
#   make check      mkadf.py --dump on the ADF (header, params, CRC, relocs)
#   make benchmarks       benchmark-only ADF: build/benchmarks/amitorture.adf
#   make check-benchmarks verify the benchmark-only ADF
#   make clean
#
# AMIGA_TOOLCHAIN   toolchain bin dir (default /opt/amiga/bin)
# BUILD_ID          string baked into the header (default: git rev + date)
# PARAMS            extra mkadf --param arguments, e.g. PARAMS="mode=quick seed=5"

AMIGA_TOOLCHAIN ?= /opt/amiga/bin
NDK_INCLUDE     ?= $(AMIGA_TOOLCHAIN)/../m68k-amigaos/ndk-include

VASM    := $(AMIGA_TOOLCHAIN)/vasmm68k_mot
CC      := $(AMIGA_TOOLCHAIN)/m68k-amigaos-gcc
OBJDUMP := $(AMIGA_TOOLCHAIN)/m68k-amigaos-objdump
PYTHON  ?= python3
MKADF   := $(PYTHON) ../tools/mkadf.py

B ?= build

CFLAGS  := -m68000 -Os -ffreestanding -nostdlib -fno-builtin -noixemul -fomit-frame-pointer -MMD -MP \
           -fno-tree-loop-distribute-patterns -Wall -Wextra -Wno-unused-parameter \
           -I$(NDK_INCLUDE) -Irt -Iprobe
ifeq ($(BENCHMARK_ONLY),1)
CFLAGS += -DAMT_BENCHMARK_ONLY
endif
ASFLAGS := -Fhunk -quiet -m68000 -I$(NDK_INCLUDE)
LDFLAGS := -m68000 -nostdlib -nostartfiles -noixemul
LIBGCC  := $(shell $(CC) -m68000 -print-libgcc-file-name)

BUILD_ID ?= $(shell git rev-parse --short HEAD 2>/dev/null || echo nogit) $(shell date -u +%Y-%m-%d)

# start.s must be linked first; then the rest of the runtime.
RT_C   := rt/main.c rt/out.c rt/crc32.c rt/takeover.c rt/mem.c rt/mod.c rt/screen.c
RT_S   := rt/takeover_asm.s rt/takeover_010.s rt/divmod.s rt/bare.s rt/cpustate.s
# test modules (CONTRACT.md section 10), picked up automatically
# MODS="rt cia" limits the build to those modules (modules/<name>.c and
# modules/<name>_*.{c,s}); default: all. Use with B=build-<x> so parallel
# builds of different module sets don't share a build directory.
ifeq ($(MODS),)
MOD_C  := $(wildcard modules/*.c)
MOD_S  := $(wildcard modules/*.s)
else
MOD_C  := $(sort $(foreach m,$(MODS),$(wildcard modules/$(m).c modules/$(m)_*.c)))
MOD_S  := $(sort $(foreach m,$(MODS),$(wildcard modules/$(m).s modules/$(m)_*.s)))
endif
# probe sources, picked up automatically; the stub only while probe.c is missing
PROBE_C := $(wildcard probe/*.c)
PROBE_S := $(wildcard probe/*.s)
ifeq ($(wildcard probe/probe.c),)
RT_C += rt/probe_stub.c
endif

C_SRCS := $(RT_C) $(PROBE_C) $(MOD_C)
S_SRCS := $(RT_S) $(PROBE_S) $(MOD_S)
OBJS := $(B)/rt/start.o $(C_SRCS:%.c=$(B)/%.o) $(S_SRCS:%.s=$(B)/%.o) $(B)/modlist.o

# CPU-specific objects (section 3): *_010/_020/_030/_040/_060/_fpu sources get
# the matching -m flag. (vasm sources may also set "machine" themselves.)
cpuflag = $(if $(findstring _010.,$1),-m68010,$(if $(findstring _020.,$1),-m68020,$(if $(findstring _030.,$1),-m68030,$(if $(findstring _040.,$1),-m68040,$(if $(findstring _060.,$1),-m68060,$(if $(findstring _fpu.,$1),-m68020 -m68881,))))))

# `make`: the disk set (all modules; one ADF can't hold them any more).
# `make MODS="rt cia"`: a single ADF with just those modules ($(B)/amitorture.adf),
# for developing a module or for a repro pack.
.PHONY: all one check clean FORCE
ifeq ($(MODS),)
all: set
else
all: $(B)/amitorture.adf
endif
one: $(B)/amitorture.adf

.PHONY: benchmarks check-benchmarks
benchmarks:
	$(MAKE) --no-print-directory B=$(B)/benchmarks MODS=hostspeed BENCHMARK_ONLY=1 \
		BUILD_ID="$(BUILD_ID) benchmarks" one

check-benchmarks: benchmarks
	$(MKADF) --dump $(B)/benchmarks/amitorture.adf

$(B)/bootblock.bin: boot/bootblock.s
	@mkdir -p $(dir $@)
	$(VASM) -Fbin -m68000 -no-opt -quiet -o $@ $<

# rewritten only when the module selection changes, so that removing a
# module from MODS (or a disk group) regenerates modlist.c
$(B)/mods.list: FORCE
	@mkdir -p $(dir $@)
	@echo '$(MOD_C)' | cmp -s - $@ || echo '$(MOD_C)' > $@

$(B)/modlist.c: $(MOD_C) $(B)/mods.list ../tools/genmodlist.py
	@mkdir -p $(dir $@)
	$(PYTHON) ../tools/genmodlist.py $@ $(MOD_C)

$(B)/modlist.o: $(B)/modlist.c rt/mod.h Makefile
	$(CC) $(CFLAGS) -c -o $@ $<

$(B)/%.o: %.c rt/rt.h rt/mod.h rt/screen.h probe/probe.h Makefile
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) $(call cpuflag,$<) -c -o $@ $<

$(B)/%.o: %.s
	@mkdir -p $(dir $@)
	$(VASM) $(ASFLAGS) $(call cpuflag,$<) -o $@ $<

$(B)/payload.exe: $(OBJS)
	$(CC) $(LDFLAGS) -o $@ $(OBJS) $(LIBGCC)

$(B)/amitorture.adf: $(B)/bootblock.bin $(B)/payload.exe ../tools/mkadf.py
	$(MKADF) --boot $(B)/bootblock.bin --payload $(B)/payload.exe -o $@ \
		--build-id "$(BUILD_ID)" $(if $(PARAMS),--param $(PARAMS))

# `make check`: verify every disk of the set (bootblock checksum, CRC, relocs);
# with MODS, the single ADF.
ifeq ($(MODS),)
check: set
	@for f in $(SET)/*.adf; do echo "== $$f"; $(MKADF) --dump $$f || exit 1; done
else
check: $(B)/amitorture.adf
	$(MKADF) --dump $<
endif

clean:
	rm -rf $(B)

# ---------------------------------------------------------------- disk set
# The full payload needs more memory than a 512K chip + 512K slow A500 has
# in one piece, so the test ships as a set of disks. Each disk is a complete
# payload (runtime + probe) with one group of modules; the runner boots every
# config once per disk and merges the records. Every module must be in exactly
# one group (`make check-groups`). The bootblock loads a disk's code and its
# data + BSS as two allocations, so each must fit the largest free block of
# that A500 (about 470 KB of chip, 490 KB of slow RAM, less the test pools);
# `make set` prints the sizes.
#   core  timing-sensitive hardware, floppy DMA (real floppy speed, no warp)
#   cpu   integer CPU, MMU, random code blocks (functional: warp in
#         --warp auto)
#   exp   FPU, expansion hardware, devices, memory, hostspeed
# cpu_basic and fpu each carry over 200 KB of test vectors: keep them apart.
# devio needs a 128 KB I/O buffer after the pools on a 1 MB chip-only A600:
# keep the exp disk small enough for that.
DISK_SETS      := core cpu exp
DISK_SET_core  := takeover rt flags exceptions smc interrupts cia blitter timing display copper paula input dmacontention
DISK_SET_cpu   := cpu_basic mmu dispatch
DISK_SET_exp   := fpu rtg expsound network misc_hw misc_hw_os devio memory memory_os hostspeed
SET := $(B)/set
ALL_MODULE_NAMES := $(shell $(PYTHON) ../tools/genmodlist.py --names $(wildcard modules/*.c))

.PHONY: set check-groups
set: check-groups
	@mkdir -p $(SET)
	@rm -f $(SET)/*.adf
	@n=0; for g in $(DISK_SETS); do n=$$((n+1)); \
	  case $$g in core) m="$(DISK_SET_core)";; cpu) m="$(DISK_SET_cpu)";; exp) m="$(DISK_SET_exp)";; esac; \
	  have=""; for x in $$m; do grep -q "^AMT_MODULE( *$$x *," modules/*.c && have="$$have $$x"; done; \
	  [ -n "$$have" ] || { echo "set: group $$g has no modules yet, skipped"; continue; }; \
	  $(MAKE) --no-print-directory B=$(B)/set-$$g MODS="$$have" BUILD_ID="$(BUILD_ID) $$n/$(words $(DISK_SETS)) $$g" || exit 1; \
	  cp $(B)/set-$$g/amitorture.adf $(SET)/amitorture-$$n-$$g.adf; \
	done
	@for f in $(SET)/*.adf; do printf '%s: ' $$f; $(MKADF) --dump $$f | awk '/^alloc_size/{c=$$2}/^data_alloc/{d=$$2}END{print c " code + " d " data bytes in memory"}'; done

check-groups:
	@bad=0; for m in $(ALL_MODULE_NAMES); do c=0; \
	  for g in $(foreach d,$(DISK_SETS),$(DISK_SET_$(d))); do \
	    [ "$$g" = "$$m" ] && c=$$((c+1)); done; \
	  [ $$c -eq 1 ] || { echo "module $$m is in $$c disk groups (must be exactly 1): edit DISK_SET_* in disk/Makefile"; bad=1; }; \
	done; exit $$bad

.PHONY: check-gen
check-gen:
	../tools/check_generated.sh

-include $(C_SRCS:%.c=$(B)/%.d) $(B)/modlist.d

FORCE:
