# Kernel sets for the bench disk (see docs/BENCH.md, "Kernel sets").
#
# Every set is compiled to assembly, the assembly is checked for opcodes the
# set's target CPUs cannot run (tools/kset_check.py), then assembled and
# partially linked into one relocatable object. All symbols but the set's
# descriptor are made local, and the descriptor is renamed to <set>_kset, so
# the four sets can be linked side by side into the bench payload.
#
#   k000   68000, integer and soft-float kernels         (all CPUs)
#   k020   68020+ integer kernels                        (020+)
#   k020f  68020+ with 6888x/040/060 FPU, float kernels  (020+ with FPU)
#   k020s  68020+ soft-float kernels                     (020+ without FPU)
#
# Nothing from libnix is linked: only libgcc (for the 64-bit helpers poly1305
# needs) and, for k000, rt/divmod.s. Any undefined symbol left after the
# partial link fails the build.
#
# Included from disk/Makefile; needs CC, VASM, PYTHON, NDK_INCLUDE and B.

K   := bench/kernels
KB  := $(B)/kset
LD  := $(AMIGA_TOOLCHAIN)/m68k-amigaos-ld
CXX := $(AMIGA_TOOLCHAIN)/m68k-amigaos-g++
OBJCOPY := $(AMIGA_TOOLCHAIN)/m68k-amigaos-objcopy
NM  := $(AMIGA_TOOLCHAIN)/m68k-amigaos-nm
KSET_CHECK := $(PYTHON) ../tools/kset_check.py

KSETS := k000 k020 k020f k020s

K_INT_C := kshim.c kset.c corpus.c glyphs.c dhry.c scene.c zlib.c ftgrays.c \
	memcpy.c aesgcm.c chacha20poly1305.c \
	third_party/dhry/dhry_1.c third_party/dhry/dhry_2.c \
	third_party/zlib/adler32.c third_party/zlib/crc32.c third_party/zlib/deflate.c \
	third_party/zlib/inffast.c third_party/zlib/inflate.c third_party/zlib/inftrees.c \
	third_party/zlib/trees.c third_party/zlib/zutil.c \
	third_party/freetype/ftgrays.c \
	third_party/amissl/aes_core.c third_party/amissl/chacha_enc.c \
	third_party/amissl/gcm128.c third_party/amissl/poly1305.c
K_FLOAT_C   := nfs3.c
K_FLOAT_CXX := third_party/nfs3/kernel.cpp
K_FLOAT_BASE_C := kshim.c kset.c

KSET_k000_CPU   := -m68000
KSET_k000_DEFS  := -DKSET_000 -DKSET_INT -DKSET_FLOAT
KSET_k000_C     := $(K_INT_C) $(K_FLOAT_C) softfloat.c
KSET_k000_CXX   := $(K_FLOAT_CXX)
KSET_k000_S     := rt/divmod.s
KSET_k020_CPU   := -m68020-60
KSET_k020_DEFS  := -DKSET_020 -DKSET_INT
KSET_k020_C     := $(K_INT_C)
KSET_k020_CXX   :=
KSET_k020f_CPU  := -m68020-60 -m68881
KSET_k020f_DEFS := -DKSET_020 -DKSET_FLOAT
# The FPU keeps extended exponent range in registers (FPCR rounds only the
# mantissa): pin ISO excess-precision rules so the code, and with it the
# checksums tools/kset_expect.py mirrors on x87, don't depend on GCC's
# register allocation.
KSET_k020f_FP   := -fexcess-precision=standard
KSET_k020f_C    := $(K_FLOAT_BASE_C) $(K_FLOAT_C)
KSET_k020f_CXX  := $(K_FLOAT_CXX)
KSET_k020s_CPU  := -m68020-60
KSET_k020s_DEFS := -DKSET_020 -DKSET_FLOAT
KSET_k020s_C    := $(K_FLOAT_BASE_C) $(K_FLOAT_C) softfloat.c
KSET_k020s_CXX  := $(K_FLOAT_CXX)

K_INC := -I$(K)/inc -I$(K) -I$(NDK_INCLUDE) -I$(K)/third_party/zlib \
	-I$(K)/third_party/amissl/shim -DB_ENDIAN -DZ_SOLO
K_CFLAGS := -O2 -ffreestanding -noixemul -fomit-frame-pointer -fno-math-errno \
	-DNDEBUG -MMD -MP $(K_INC)
K_CXXFLAGS := -O2 -noixemul -fomit-frame-pointer -fno-math-errno -DNDEBUG -MMD -MP \
	-fno-exceptions -fno-rtti -fno-threadsafe-statics -std=c++17 -I$(K)
K_DEPS := $(wildcard $(K)/*.h $(K)/inc/*.h) bench/kernels.mk Makefile

# $(call kset_rules,set)
define kset_rules
$(1)_SRC_S := $$(KSET_$(1)_C:%.c=$(KB)/$(1)/%.s) $$(KSET_$(1)_CXX:%.cpp=$(KB)/$(1)/%.s)
$(1)_OBJS  := $$($(1)_SRC_S:%.s=%.o) $$(KSET_$(1)_S:%.s=$(KB)/$(1)/asm/%.o)

$(KB)/$(1)/%.s: $(K)/%.c $(K_DEPS)
	@mkdir -p $$(@D)
	$(CC) $$(KSET_$(1)_CPU) $(K_CFLAGS) $$(KSET_$(1)_DEFS) $$(KSET_$(1)_FP) \
		$$(if $$(findstring freetype/ftgrays,$$<),-DSTANDALONE_) -S -o $$@ $$<

$(KB)/$(1)/%.s: $(K)/%.cpp $(K_DEPS)
	@mkdir -p $$(@D)
	$(CXX) $$(KSET_$(1)_CPU) $(K_CXXFLAGS) $$(KSET_$(1)_DEFS) $$(KSET_$(1)_FP) -S -o $$@ $$<

$(KB)/$(1)/%.o: $(KB)/$(1)/%.s ../tools/kset_check.py
	$(KSET_CHECK) --set $(1) $$<
	$(CC) $$(KSET_$(1)_CPU) -c -o $$@ $$<

$(KB)/$(1)/asm/%.o: %.s
	@mkdir -p $$(@D)
	$(VASM) $(ASFLAGS) -o $$@ $$<

$(KB)/$(1).o: $$($(1)_OBJS)
	$(LD) -r -o $(KB)/$(1).r.o $$^ $$(shell $(CC) $$(KSET_$(1)_CPU) -print-libgcc-file-name)
	@u=$$$$($(NM) -u $(KB)/$(1).r.o); if [ -n "$$$$u" ]; then \
		echo "kernel set $(1) has undefined symbols:"; echo "$$$$u"; exit 1; fi
	$(OBJCOPY) --prefix-symbols=_$(1) -G _$(1)_kset $(KB)/$(1).r.o $$@

-include $$($(1)_SRC_S:%.s=%.d)
.SECONDARY: $$($(1)_SRC_S)
endef

$(foreach s,$(KSETS),$(eval $(call kset_rules,$(s))))

KSET_OBJS := $(KSETS:%=$(KB)/%.o)

.PHONY: ksets
ksets: $(KSET_OBJS)

# for tools/kset_expect.py: the sources and defines of a set
.PHONY: kset-info-%
kset-info-%:
	@echo "defs: $(KSET_$*_DEFS)"
	@echo "c: $(KSET_$*_C)"
	@echo "cxx: $(KSET_$*_CXX)"
	@echo "fp: $(KSET_$*_FP)"
