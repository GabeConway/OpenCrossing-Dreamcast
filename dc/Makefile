# =============================================================================
# OpenCrossing-Dreamcast — game build (GNU make, KallistiOS idiom)
# =============================================================================
# This is the DC translation of pc/CMakeLists.txt. It is deliberately a plain
# makefile and not CMake: KOS ships wrapper compilers (kos-cc / kos-c++) that
# already carry the whole target ABI in $KOS_CFLAGS / $KOS_LDFLAGS, so a
# toolchain file would just be a second place for those flags to drift.
#
# RUN IT INSIDE THE SDK CONTAINER. From the host:
#     bash dc/build-dc.sh              # wrapper -> docker run -> build-dc-docker.sh
# or directly:
#     docker run --rm --platform linux/arm64 \
#       -v /Users/gabe/Documents/GitHub/OpenCrossing-Dreamcast:/work \
#       opencrossing-dc:sdk bash -c 'make -C /work/dc -j4 objs'
#
# TARGETS
#   objs     compile every TU, do NOT link.  <-- the M1 signal
#   all      objs + link -> build/AnimalCrossing.elf
#   clean    rm -rf build/
#   sources  print the computed source list (debugging the filters)
#   count    print TU counts
#
# KNOBS (env or make VAR=...)
#   DECOMP_OPT=-O0   optimization level for decomp game code (see below)
#   DC_OPT=-O2       optimization level for dc/src platform code
#   V=1              echo full compiler command lines
# =============================================================================

# --- Paths -------------------------------------------------------------------
# Always absolute: agents and CI run from varying cwds (CLAUDE.md).
DC_DIR   := $(patsubst %/,%,$(dir $(abspath $(lastword $(MAKEFILE_LIST)))))
ROOT     := $(abspath $(DC_DIR)/..)
BUILD    := $(DC_DIR)/build
OBJDIR   := $(BUILD)/obj
TARGET   := $(BUILD)/AnimalCrossing.elf
RSP      := $(BUILD)/objs.rsp

# --- Toolchain ---------------------------------------------------------------
# kos-cc / kos-c++ prepend $KOS_CFLAGS (which carries -ml -m4-single, the KOS
# include paths, -ffunction-sections/-fdata-sections and -O2). Anything we pass
# comes AFTER, so our -O0 / -w override theirs. Do not hand-roll sh-elf-gcc
# here — the wrappers are the only thing that keeps us ABI-compatible with the
# prebuilt libkallisti/newlib/libstdc++.
KCC  := kos-cc
KCXX := kos-c++

ifeq ($(V),1)
  Q :=
else
  Q := @
endif

# =============================================================================
# Optimization level — READ kb/design-shelf-hazards.md §3.4 BEFORE CHANGING
# =============================================================================
# History on the armhf device build (pc/CMakeLists.txt:21-29):
#   -O2 -> wild-pointer CRASH loop from boot (black screen with music)
#   -O1 -> hard SIGBUS on the intro train scene
#   no -O -> stable.
# *** POLICY (2026-08-01, user decision): DECOMP GAME CODE STAYS AT -O0. ***
#
# This is SETTLED, not a tuning parameter. The armhf history above is treated
# as authoritative: the port is broken with optimization on. Do not raise
# DECOMP_OPT, and do not propose raising it as a way to meet the RAM budget.
#
# For the record, measured on this tree at M1 (whole-tree rebuilds, sizes in
# bytes) so nobody has to re-run it — these are REJECTED BY POLICY, recorded
# only so the number is not mistaken for unknown:
#     -O0  text 6,318,568   data 2,638,852   bss 13,526,548   total 22,483,968
#     -O1  text 3,697,460   data 2,525,704   bss 13,526,164   total 19,749,328
#     -Os  text 3,477,188   data 2,525,704   bss 13,526,132   total 19,529,024
#     -O2  text 3,825,312   data 2,525,672   bss 13,526,132   total 19,877,116
# Note that even -Os does NOT get under 16,777,216 — so optimization was never
# sufficient on its own anyway. The budget has to come out of .bss.
#
# NOTE: -O0 (not "empty") is required, because $KOS_CFLAGS already contains
# -O2 and the LAST -O on the command line wins.
DECOMP_OPT ?= -O0
DC_OPT     ?= -O2

# =============================================================================
# Link-time dead-section elimination (KILL SWITCH: DC_GC_SECTIONS=0)
# =============================================================================
# This is NOT a codegen change. -ffunction-sections/-fdata-sections only alter
# which named section each function/object is emitted into; instruction
# selection inside a surviving function is bit-identical. --gc-sections then
# drops sections nothing references. Safe under the -O0 policy above.
# VERIFIED on this tree by disassembling a surviving function with and without
# the flags and diffing (see BUILDING-DC.md).
#
# $KOS_CFLAGS already carries -ffunction-sections -fdata-sections and
# $KOS_LDFLAGS already carries -Wl,--gc-sections, so DC_GC_SECTIONS=1 is a
# no-op that documents intent; DC_GC_SECTIONS=0 actively countermands them
# (last flag wins for both the -f pair and --gc-sections/--no-gc-sections).
DC_GC_SECTIONS ?= 1
ifeq ($(DC_GC_SECTIONS),1)
  GC_CFLAGS  := -ffunction-sections -fdata-sections
  GC_LDFLAGS := -Wl,--gc-sections
else
  GC_CFLAGS  := -fno-function-sections -fno-data-sections
  GC_LDFLAGS := -Wl,--no-gc-sections
endif
# DC_GC_PRINT=1 lists every section the linker dropped (very verbose).
ifeq ($(DC_GC_PRINT),1)
  GC_LDFLAGS += -Wl,--print-gc-sections
endif

# UB guards. Same set as pc/CMakeLists.txt lines 9-31 plus the two KOS/SH-4
# additions justified in design-shelf-hazards §3.1.
UB_GUARDS := \
	-fno-strict-aliasing \
	-fwrapv \
	-fno-delete-null-pointer-checks \
	-fno-lifetime-dse \
	-fno-aggressive-loop-optimizations \
	-fno-strict-overflow \
	-fno-stack-protector \
	-fsigned-char

# NOT included: -fno-builtin.
#   design-shelf-hazards §3.1 recommends it as "KOS convention (KOS_CFLAGS,
#   VERIFIED)". MEASURED FALSE against this image: $KOS_CFLAGS is
#   "-O2 -fno-PIC -fno-PIE -fomit-frame-pointer -m4-single -ml -mfsrra -mfsca
#    -ffunction-sections -fdata-sections -matomic-model=soft-gusa
#    -ftls-model=local-exec -D__DREAMCAST__ … -Wall -g" — no -fno-builtin.
#   And it actively breaks the link: -fno-builtin stops GCC expanding
#   __builtin_alloca inline, so src/game/m_select.c:936,993 emit calls to a
#   real `alloca` symbol, which newlib does not provide. There is no
#   -fbuiltin-alloca to re-enable it selectively (cc1 rejects the option).
#   The flag's stated benefit (stopping GCC from recognising hand-rolled copy
#   loops and substituting memcpy under stronger alignment assumptions) does
#   not apply at -O0 anyway. Revisit if DECOMP_OPT is raised to -O2.

# =============================================================================
# Include paths
# =============================================================================
# ORDER IS LOad-BEARING: dc/include MUST precede pc/include so that
# dc/include/pc_platform.h (the SDL-free shim) wins over the real one, and
# dc/include/SDL.h satisfies jaudio_NES/internal/os.c's `#include <SDL.h>`.
#
# include/libc is deliberately NOT on the path (same reason as the PC build):
# having both include/ and include/libc/ breaks the decomp's #include_next
# shadow-header chains.
INCLUDES := \
	-I$(DC_DIR)/include \
	-I$(ROOT)/pc/include \
	-I$(ROOT)/include \
	-I$(ROOT)/src \
	-I$(ROOT)

# Force-included into every TU. Kills the KOS `page_count` macro (which no
# include ordering can avoid) and plugs newlib's missing transitive <string.h>.
# See dc/include/dc_prelude.h for the full rationale.
PRELUDE := -include $(DC_DIR)/include/dc_prelude.h

# =============================================================================
# Preprocessor defines
# =============================================================================
# TARGET_PC is NON-NEGOTIABLE (design-shelf-hazards §3.1): it guards the base
# port's little-endian correctness fixes (byte-wise texconv in emu64.c, the
# swapped u16 pair ordering in sys_matrix.c, the overlap-safe Jac_bcopy in
# sample.c). Here it means "not GameCube", not "PC". TARGET_DC is for branches
# that are genuinely Dreamcast-only.
DEFINES := \
	-DTARGET_PC \
	-DTARGET_DC \
	-DBUILD_USER=\"dreamcast\" \
	-DVERSION=0 \
	-DF3DEX_GBI_2 \
	-DNDEBUG \
	-D_LANGUAGE_C

# jaudio_NES C files use `nullptr`. C only — C++ has it natively.
C_ONLY_DEFINES := -Dnullptr=NULL

# =============================================================================
# Warning / language flags
# =============================================================================
# Decomp uses K&R empty parameter lists: `void f() {}` against a prototype
# `void f(int,int)`. Under C99+ that is a type conflict; under gnu89 it is
# "unspecified args" and compatible. Hence gnu89 for decomp C by default.
DECOMP_C_LANG    := -w -std=gnu89 -fpermissive -Wno-return-type -Wno-error=return-type
# jaudio_NES / emu64 carry PC-port additions using C99/C11 features.
DECOMP_C11_LANG  := -w -std=gnu11 -fpermissive -Wno-return-type -Wno-error=return-type
# §3.3: VERIFIED that no kept C++ TU uses throw/try/catch/typeid/dynamic_cast,
# so -fno-exceptions -fno-rtti (KOS_CPPFLAGS convention) are safe and save
# a meaningful slice of the 16 MB budget.
DECOMP_CXX_LANG  := -w -fpermissive -fno-exceptions -fno-rtti

# dc/src is code we own: warn on it.
DC_WARN := -Wall -Wextra -Wno-unused-parameter -Wno-sign-compare -Wno-comment \
	-Wno-unused-function -Wno-builtin-declaration-mismatch

# =============================================================================
# Source discovery
# =============================================================================
# Reproduces all 35 filters from pc/CMakeLists.txt verbatim (every one of them
# is PC-generic, not GLES/SDL-specific — design-shelf-hazards §2), and then
# adds the DC-only list below. Expected result: 3854 .c + 46 .cpp = 3900 TUs,
# which is the exact number §0 measured as compiling for sh-elf.

# --- Inherited PC filters (C) ---
PC_FILTER_C := \
/AUS/|\
/dolphin/(gx|os|vi|pad|card|dvd|ai|ar|dsp|exi|si|db|mtx|base|gba|amcstubs|OdemuExi2|odenotstub)/|\
/TRK_MINNOW_DOLPHIN/|\
/Runtime\.PPCEABI\.H/|\
/MSL_C\.PPCEABI\.bare\.H/|\
^src/furniture/|\
^src/data/model/obj_e_boat/|\
/ac_npc_shop_common\.c$$|\
/ac_animal_logo_misc\.c$$|\
/m_item_debug\.c$$|\
/actor/npc/.*_anime\.c$$|\
/osreport\.c$$|\
/Famicom/|\
/MSL_C/|\
/jaudio_NES/internal/dsp_cardunlock\.c$$|\
/jaudio_NES/internal/dsp_GBAKey\.c$$|\
/libc64/malloc\.c$$|\
/libc64/sprintf\.c$$|\
/libc64/aprintf\.c$$|\
/libultra/|\
/ReconfigBATs\.c$$|\
/executor\.c$$|\
/ef_effect_lib\.c$$|\
/m_card\.c$$|\
/emu64_utility\.c$$

# --- Inherited PC filters (C++) ---
PC_FILTER_CXX := \
/AUS/|\
/dolphin/|\
/TRK_MINNOW_DOLPHIN/|\
/Runtime\.PPCEABI\.H/|\
/MSL_C\.PPCEABI\.bare\.H/|\
/Famicom/|\
/emu64_print\.cpp$$|\
/jsyswrapper_ext\.cpp$$|\
/jsyswrapper_main\.cpp$$

# -----------------------------------------------------------------------------
# DC_EXCLUDE — Dreamcast-only exclusions. EVERY ENTRY NEEDS A REASON AND A DATE.
# -----------------------------------------------------------------------------
# Policy: src/ is vendored decomp. When a TU cannot compile for SH-4, it goes
# here rather than getting hacked in place. Only add an entry after reading the
# actual sh-elf-gcc error.
#
# (empty — see the run log in the M1 report; the 12 known failures from
#  design-shelf-hazards §2 are all handled by SHIMS, not exclusions:
#    §2.1 five pc_platform.h users -> dc/include/pc_platform.h
#    §2.2 jaudio os.c <SDL.h>      -> dc/include/SDL.h
#    §2.3 four page_count users    -> dc/include/dc_prelude.h
#    §2.4 JFWDisplay.cpp strlen    -> <string.h> in dc_prelude.h )
DC_EXCLUDE_C   :=
DC_EXCLUDE_CXX :=

# make turns each backslash-newline above into a SPACE, which would end up
# inside the ERE and silently match nothing. Strip them. (No filter pattern
# legitimately contains a space.)
empty :=
space := $(empty) $(empty)
FILTER_C   := $(subst $(space),,$(PC_FILTER_C)$(if $(DC_EXCLUDE_C),|$(DC_EXCLUDE_C)))
FILTER_CXX := $(subst $(space),,$(PC_FILTER_CXX)$(if $(DC_EXCLUDE_CXX),|$(DC_EXCLUDE_CXX)))

GAME_C   := $(shell cd $(ROOT) && find src -name '*.c'   | LC_ALL=C sort | grep -Ev '$(FILTER_C)')
GAME_CXX := $(shell cd $(ROOT) && find src -name '*.cpp' | LC_ALL=C sort | grep -Ev '$(FILTER_CXX)')

# Dreamcast platform layer. Globbed on purpose: dc_main.c / dc_mtx.c and any
# future dc_*.c get picked up automatically with no edit here.
DC_C   := $(patsubst $(ROOT)/%,%,$(sort $(wildcard $(DC_DIR)/src/*.c)))
DC_CXX := $(patsubst $(ROOT)/%,%,$(sort $(wildcard $(DC_DIR)/src/*.cpp)))

# Padclamp.c lives under the excluded src/static/dolphin/pad/ but is pure math
# (analog-stick clamping) with no Dolphin hardware dependency. The PC port adds
# it back the same way (pc/CMakeLists.txt:415).
EXTRA_C := src/static/dolphin/pad/Padclamp.c

# -----------------------------------------------------------------------------
# pc/src files REUSED VERBATIM by the Dreamcast build
# -----------------------------------------------------------------------------
# kb/design-platform-api.md §2a/§2b classifies every pc/src file. These are the
# ones whose .c body needs no edits — they are platform-independent logic that
# sits ABOVE the backend, and dc/src supplies the backend they call into.
# They are compiled with the decomp flag set (gnu11, unoptimized-by-default),
# not the dc/src set, because they are inherited code we do not own.
#
#   pc_assets.c     30677 LOC generated asset dispatch table, plain C. Its data
#                   source changes (ISO -> /cd) but the table does not; the
#                   pc_disc_* backend it calls is implemented in dc/src/dc_dvd.c
#                   (pc_disc_is_open/_extract_dol/_extract_rel). dc_main.c calls
#                   pc_assets_init() directly.
#   pc_save_bswap.c 1029 LOC of GCI byte-swap tables. §2a: "truly verbatim" —
#                   includes only decomp headers + <string.h>. SH-4 is LE
#                   exactly like the base targets.
#   pc_m_card.c     §2b "small edits": 1442 LOC of save/village/travel logic
#                   sitting above CARD*. It re-implements the game's own
#                   src/game/m_card.c, which both builds exclude, so without
#                   it the link is short 29 mCD_* / pc_save_* symbols.
#                   MEASURED: it compiles clean for sh-elf as-is — its host
#                   file I/O is <stdio.h>/<sys/stat.h>, which newlib declares
#                   and KOS implements, and its <direct.h> include is
#                   _WIN32-guarded. COMPILING IS NOT WORKING: the .gci-file
#                   backend still has to become VMU/vmufs, and PLAN §6 says
#                   ~100 KB of VMU has to hold a ~456 KB GC save. That is an
#                   M2 problem; linking now is strictly better than stubbing.
#
# DELIBERATELY NOT HERE:
#   pc_settings.c   §2b: uses SDL_DisplayMode for the video-mode list
#                   (pc_settings.c:366-385). MEASURED to fail on sh-elf. The
#                   SDL window calls have to be dropped first.
#   pc_disc.c       §2b: "moves to tools/" — host-side code, not target code.
PC_REUSE_C := \
	pc/src/pc_assets.c \
	pc/src/pc_save_bswap.c \
	pc/src/pc_m_card.c

# =============================================================================
# Per-TU overrides (mirrors pc/CMakeLists.txt lines 483-544)
# =============================================================================
# These four .c files contain C++ (classes, namespaces, ::) and must be fed to
# the C++ front end with -x c++.
CXXFROMC_SRC := \
	src/static/libforest/emu64/emu64.c \
	src/static/jaudio_NES/internal/ja_calc.c \
	src/static/jaudio_NES/internal/jammain_2.c \
	src/static/jaudio_NES/game/game64.c

# Everything under these dirs is gnu11 (minus the four above, CXX wins).
C11_SRC := $(filter-out $(CXXFROMC_SRC), \
	$(filter src/static/jaudio_NES/game/% src/static/jaudio_NES/internal/% \
	         src/static/libforest/emu64/%, $(GAME_C)))

# =============================================================================
# Object lists
# =============================================================================
# obj/ mirrors the source tree rooted at the repo root and keeps the source
# extension in the object name (foo.c.o / foo.cpp.o), so no two TUs can ever
# collide — including the src/data tree, which has same-named files in many
# directories, and foo.c/foo.cpp pairs.
o = $(OBJDIR)/$(1).o

GAME_C_OBJS   := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(GAME_C) $(EXTRA_C) $(PC_REUSE_C)))
PC_REUSE_OBJS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(PC_REUSE_C)))
GAME_CXX_OBJS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(GAME_CXX)))
DC_C_OBJS     := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(DC_C)))
DC_CXX_OBJS   := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(DC_CXX)))

CXXFROMC_OBJS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(CXXFROMC_SRC)))
C11_OBJS      := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(C11_SRC)))

OBJS := $(GAME_C_OBJS) $(GAME_CXX_OBJS) $(DC_C_OBJS) $(DC_CXX_OBJS)
DEPS := $(OBJS:.o=.d)

# =============================================================================
# Per-object flag variables (target-specific; defaults below, overrides after)
# =============================================================================
COMMON  = $(INCLUDES) $(PRELUDE) $(DEFINES) $(GC_CFLAGS)

# Defaults: decomp C at gnu89, unoptimized, full UB guard set.
$(OBJS): TU_CC    = $(KCC)
$(OBJS): TU_XLANG =
$(OBJS): TU_LANG  = $(DECOMP_C_LANG) $(C_ONLY_DEFINES)
$(OBJS): TU_OPT   = $(DECOMP_OPT) $(UB_GUARDS)
$(OBJS): TU_DEFS  =

# gnu11 decomp C (jaudio_NES + emu64 dirs).
$(C11_OBJS): TU_LANG = $(DECOMP_C11_LANG) $(C_ONLY_DEFINES)

# Reused pc/src files: written against C99+ (declarations after statements,
# for-loop declarations), so gnu89 is wrong for them.
$(PC_REUSE_OBJS): TU_LANG = $(DECOMP_C11_LANG) $(C_ONLY_DEFINES)

# .c files that are actually C++.
$(CXXFROMC_OBJS): TU_CC    = $(KCXX)
$(CXXFROMC_OBJS): TU_XLANG = -x c++
$(CXXFROMC_OBJS): TU_LANG  = $(DECOMP_CXX_LANG)

# Real decomp .cpp files.
$(GAME_CXX_OBJS): TU_CC   = $(KCXX)
$(GAME_CXX_OBJS): TU_LANG = $(DECOMP_CXX_LANG)

# Platform layer: code we own -> optimized and warned on.
$(DC_C_OBJS): TU_CC   = $(KCC)
$(DC_C_OBJS): TU_LANG = -std=gnu11 $(DC_WARN)
$(DC_C_OBJS): TU_OPT  = $(DC_OPT) -fno-strict-aliasing -fwrapv -fsigned-char
$(DC_CXX_OBJS): TU_CC   = $(KCXX)
$(DC_CXX_OBJS): TU_LANG = $(DC_WARN) -fpermissive -fno-exceptions -fno-rtti
$(DC_CXX_OBJS): TU_OPT  = $(DC_OPT) -fno-strict-aliasing -fwrapv -fsigned-char

# KOS owns main(). Rename the game's two entry points, same as the PC build.
$(OBJDIR)/src/main.c.o:        TU_DEFS = -Dmain=ac_entry
$(OBJDIR)/src/static/boot.c.o: TU_DEFS = -Dmain=boot_main

# =============================================================================
# Rules
# =============================================================================
.PHONY: all objs clean sources count help
.SUFFIXES:

all: $(TARGET)

objs: $(OBJS)
	@echo "== objs: $(words $(OBJS)) objects up to date (NOT linked) =="

help:
	@sed -n '2,30p' $(firstword $(MAKEFILE_LIST))

COMPILE = $(Q)mkdir -p $(@D) && $(TU_CC) $(TU_XLANG) $(COMMON) $(TU_DEFS) $(TU_LANG) $(TU_OPT) \
	  -MMD -MP -MF $(@:.o=.d) -MT $@ -c $< -o $@

$(OBJDIR)/%.c.o: $(ROOT)/%.c
	@echo "  CC  $*.c"
	$(COMPILE)

$(OBJDIR)/%.cpp.o: $(ROOT)/%.cpp
	@echo "  CXX $*.cpp"
	$(COMPILE)

# --- Link --------------------------------------------------------------------
# Response file: 3900 objects is ~250 KB of command line. Under ARG_MAX on
# Linux, but @file keeps it robust (and kos-cc explicitly expands @args).
#
# -lGL is GLdc, -lz is the kos-ports zlib. kos-c++ appends
# -lstdc++ -Wl,--start-group -lkallisti -lm -lc -lgcc -Wl,--end-group itself,
# plus the ldscript and -nostdlib from $KOS_LDFLAGS, so nothing else is needed.
#
# --allow-multiple-definition: decomp headers declare functions as bare
# `inline` (not `static inline`). In gnu89 every TU may emit an out-of-line
# copy. The PC build needs the same flag (pc/CMakeLists.txt:602).
#
# The response file is written with make's own $(file …) function, NOT with
# printf/echo: a shell command carrying ~3900 paths is ~250 KB of argv and
# execve() rejects it with "/bin/sh: Argument list too long" (MEASURED).
# $(file …) writes from inside make, so no argv is involved.
$(TARGET): $(OBJS)
	@mkdir -p $(@D)
	$(file >$(RSP))
	$(foreach _o,$(OBJS),$(file >>$(RSP),$(_o)))
	@echo "  LD  $@"
	$(Q)$(KCXX) -o $@ @$(RSP) -lGL -lz \
		-Wl,--allow-multiple-definition $(GC_LDFLAGS) \
		-Wl,-Map,$(BUILD)/AnimalCrossing.map
	@sh-elf-size $@ || true

clean:
	rm -rf $(BUILD)

sources:
	@printf '%s\n' $(GAME_C) $(EXTRA_C) $(PC_REUSE_C) $(GAME_CXX) $(DC_C) $(DC_CXX)

count:
	@echo "decomp .c   : $(words $(GAME_C))"
	@echo "decomp .cpp : $(words $(GAME_CXX))"
	@echo "  of which compiled as C++ : $(words $(CXXFROMC_OBJS))"
	@echo "  of which gnu11           : $(words $(C11_OBJS))"
	@echo "pc/src reuse: $(words $(PC_REUSE_C))"
	@echo "dc/src .c   : $(words $(DC_C))"
	@echo "dc/src .cpp : $(words $(DC_CXX))"
	@echo "extra .c    : $(words $(EXTRA_C))"
	@echo "TOTAL TUs   : $(words $(OBJS))"

-include $(DEPS)
