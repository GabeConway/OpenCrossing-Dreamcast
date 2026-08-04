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
#   DC_SRC_SHRINK=1  source-shrink pass, DEFAULT ON
#                    (-1,159,392 B .bss, -598,648 B .rodata).
#                    0 is a byte-identical revert. Needs the tree from
#                    tools/dcstub/make_src_shrink.py; dc/build-dc.sh runs it.
#   DC_ASSET_STUB=0  throwaway boot-bringup build with [1]-sized asset arrays
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
STUBDIR  := $(BUILD)/stubsrc
STUBREL  := $(patsubst $(ROOT)/%,%,$(STUBDIR))
SHRINKDIR := $(BUILD)/shrinksrc
SHRINKREL := $(patsubst $(ROOT)/%,%,$(SHRINKDIR))

# =============================================================================
# DC_ASSET_STUB — the throwaway boot-bringup build (kb/STATE.md, step S1)
# =============================================================================
# The real image is 8,273,108 B over 16 MB and therefore never executes an
# instruction: startup .bss zeroing runs off physical memory before scif_init(),
# so there is not even console output to look at. Nothing in this port has ever
# run, which means every RAM number rests on a platform layer nobody has
# observed working.
#
# DC_ASSET_STUB=1 shrinks the asset destination arrays to one element so the
# image fits and boots. tools/dcstub/make_stub_data.py writes the rewritten TUs
# to $(STUBDIR), mirroring repo-relative paths; each such source is swapped in
# below in place of its src/ original. src/ itself is never touched (CLAUDE.md
# §1) and $(STUBDIR) lives under dc/build, so `clean` removes it.
#
# The image renders garbage the moment the game reads an asset. That is the
# intended outcome — what this build buys is the FIRST execution of dc_main.c's
# trampoline, KOS init, the console path, MEMLEDGER FIT, and crash.sh against a
# real fault. Throw it away once the S4 loader lands.
#
#   python3 tools/dcstub/make_stub_data.py     # host side, before the build
#   DC_ASSET_STUB=1 bash dc/build-dc.sh        # does the above for you
DC_ASSET_STUB ?= 0
ifeq ($(DC_ASSET_STUB),1)
  ifeq ($(wildcard $(STUBDIR)/stub.list),)
    $(error DC_ASSET_STUB=1 but $(STUBDIR)/stub.list is missing. \
            Run: python3 $(ROOT)/tools/dcstub/make_stub_data.py)
  endif
  # A source is stubbed iff the generator produced a file for it. $(wildcard)
  # is one stat per TU; $(filter) against the 2535-entry list would be 10M
  # string compares on every make invocation.
  stubify = $(foreach f,$(1),$(if $(wildcard $(STUBDIR)/$(f)),$(STUBREL)/$(f),$(f)))
  # Include-path shadow for KEPT .c_inc files, same mechanism and same reason
  # as SHRINK_INCLUDES below. A kept TU whose pc_load_asset() calls live in an
  # #included .c_inc (src/game/m_msg.c -> m_msg_data.c_inc, the dialogue
  # balloon) needs the REWRITTEN .c_inc, and stubify only swaps whole TUs.
  # Those .c_inc are included as "../src/...", which cannot resolve against the
  # includer's own directory and therefore falls through to -I.
  STUB_INCLUDES := -I$(STUBDIR)/include -I$(STUBDIR)/src
else
  stubify = $(1)
  STUB_INCLUDES :=
endif

# =============================================================================
# DC_SRC_SHRINK — the source-shrink pass (kb/levers.md L3). DEFAULT ON.
# =============================================================================
# Recovers 1,159,392 B of .bss by rewriting seven numeric literals in the
# vendored decomp, plus 598,648 B of .rodata by deleting a dead string pool and
# 246,064 B of image by re-encoding one table. Not a codegen change of any kind:
# the compiler flags are untouched, only the constants that size seven buffers,
# one struct field nothing on DC reads, and one table's storage layout.
#
#   aSTR_overlay 294,912->512 · aINS_overlay 21,528->72 ·
#   aNPC_{n,s,k,e}_overlay 75,840->192 · aGYO_overlay 30,720->32   (dead arenas)
#   prbuf 614,400->32                                (writer is a stubbed no-op)
#   audiomemory 0x90000->0x76000        (measured allocation ceiling is 481,152)
#   graph/padmgr/irqmgr stacks 16,384->48      (no thread ever starts on this port)
#   s_assets[].path deleted             (.rodata; the .bin fallback is dead on DC)
#   data_bgd[].collision run-length coded    (.data; one sequential reader, S7)
#
# Each rewrite's justification, and the fifth candidate that was checked and
# REFUTED, are in the header of tools/dcstub/make_src_shrink.py. src/ and
# include/ are never touched (CLAUDE.md §1); the rewritten copies live in
# $(SHRINKDIR), which `clean` removes. DC_SRC_SHRINK=0 is a byte-identical
# revert.
#
# TWO SWAP MECHANISMS, because one does not cover both cases:
#
#   1. Per-TU source swap, exactly like DC_ASSET_STUB above. Used for the seven
#      .c files that are compiled directly, where -I can never reach them —
#      six under src/ plus pc/src/pc_assets.c, which is why $(PC_REUSE_C) is
#      run through $(shrinkify) alongside $(GAME_C) below.
#      $(SHRINKDIR)/shrink.list names them. S7's two files
#      (src/data/field/bg/acre/bg_data.c, src/game/m_field_make.c) need nothing
#      new here: both are already in $(GAME_C).
#
#   2. Include-path shadow, for the two .c_inc files and include/sys_stacks.h.
#      $(SHRINKDIR)/include and $(SHRINKDIR)/src go at the FRONT of INCLUDES
#      below, ahead of the vendored $(ROOT)/include and $(ROOT)/src.
#      VERIFIED with `sh-elf-gcc -E -H` on every consumer, not assumed:
#        src/main.c, src/system/sys_stacks.c -> $(SHRINKDIR)/include/sys_stacks.h
#        src/actor/npc/ac_npc.c              -> .../shrinksrc/include/../src/actor/npc/ac_npc_ctrl.c_inc
#        src/static/jaudio_NES/game/game64.c -> .../shrinksrc/include/../src/static/jaudio_NES/game/game64.c_inc
#      (the two .c_inc are included as "../src/…", which cannot resolve against
#      the includer's own directory and therefore falls through to -I.)
#
#      The mechanism has a REAL hole and the generator is built around it: a
#      quoted #include is resolved against the INCLUDING FILE'S directory first,
#      so a header in include/ that is pulled in by a sibling header in
#      include/ can never be shadowed. MEASURED: a shadow of
#      include/ac_structure.h reaches src/actor/ac_structure.c but NOT
#      src/actor/npc/ac_npc.c. That is why aSTR_overlay and aINS_overlay are
#      done as per-TU swaps confined to one TU, with a compile-time assert on
#      the unshrunk type, rather than as header shadows.
#
#   python3 tools/dcstub/make_src_shrink.py    # host side, before the build
#   bash dc/build-dc.sh                        # does the above for you
DC_SRC_SHRINK ?= 1
ifeq ($(DC_SRC_SHRINK),1)
  ifeq ($(wildcard $(SHRINKDIR)/shrink.list),)
    $(error DC_SRC_SHRINK=1 but $(SHRINKDIR)/shrink.list is missing. \
            Run: python3 $(ROOT)/tools/dcstub/make_src_shrink.py)
  endif
  # Only a handful of files, so $(filter) is cheap here (unlike the 2535-entry
  # stub list, which is why that one uses $(wildcard) instead).
  SHRINK_LIST := $(shell cat $(SHRINKDIR)/shrink.list)
  shrinkify = $(foreach f,$(1),$(if $(filter $(f),$(SHRINK_LIST)),$(SHRINKREL)/$(f),$(f)))
  SHRINK_INCLUDES := -I$(SHRINKDIR)/include -I$(SHRINKDIR)/src
else
  shrinkify = $(1)
  SHRINK_INCLUDES :=
endif

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
#
# $(SHRINK_INCLUDES) MUST COME FIRST when DC_SRC_SHRINK=1: it is how the
# rewritten sys_stacks.h / ac_npc_ctrl.c_inc / game64.c_inc win over the
# vendored -I$(ROOT)/include and -I$(ROOT)/src below. It is empty otherwise,
# so DC_SRC_SHRINK=0 leaves this list byte-identical to what it was.
INCLUDES := \
	$(SHRINK_INCLUDES) \
	$(STUB_INCLUDES) \
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

# dc_main.c must not call pc_assets_init() when the destinations are [1]: the
# central table would memcpy full-size assets over them. The per-TU lazy loads
# are neutralised by the rewriter itself.
ifeq ($(DC_ASSET_STUB),1)
DEFINES += -DDC_ASSET_STUB
endif

# DC_DIAG=1 turns on PC_DIAG() — the decomp's own rate-limited bring-up tracer
# (pc/include/pc_diag.h), which is compiled out unless PC_VERBOSE_DIAG is
# defined. It is the ONLY way to see inside graph_proc / game_main without
# editing src/, which CLAUDE.md forbids. Off by default because each trace
# point costs a format string in .rodata and a printf at runtime, and the
# image is already over budget.
DC_DIAG ?= 0
ifeq ($(DC_DIAG),1)
DEFINES += -DPC_VERBOSE_DIAG
endif

# DC_ARENA_BYTES / DC_ARAM_WINDOW — the two additive-heap knobs, exposed so a
# bring-up image can be given what the game actually asks for without the full
# build silently inheriting a size it cannot afford. Empty = the header default
# (2,705,504 / 1,048,576). Both are 32-byte aligned by contract.
#
# MEASURED 2026-08-02: the title-demo scene OOM'd by exactly 2,265,088 B
# against the 2,705,504 B default. That was KOS's *sbrk*, not the arena —
# the two pools are carved from the same region, so raising the arena to
# 4,980,736 to "fix" it made the run get LESS far. Lowering the arena to
# 1,900,000 removed the OOM entirely. Read the direction off that: when the
# sbrk OOM fires, shrink these. DC_ARENA_PROBE below is how the arena's own
# demand gets measured instead of guessed. The MEMLEDGER FIT line is the
# detector — raise these past what the image span leaves and the boot fails
# loudly instead of thrashing.
ifneq ($(DC_ARENA_BYTES),)
DEFINES += -DDC_MAIN_MEMORY_SIZE=$(DC_ARENA_BYTES)u
endif
ifneq ($(DC_ARAM_WINDOW),)
DEFINES += -DDC_ARAM_WINDOW_SIZE=$(DC_ARAM_WINDOW)u
endif

# DC_FB_PROBE=<N> — make the guest emit MARK:FRAME / FBHASH / FBTHUMB every N
# frames, the protocol harness/dc/screenshot.sh parses. This is the ONLY way
# to see what the port renders without a human watching Flycast: host-side
# capture is blocked by macOS TCC on this machine (harness/dc/screenshot.sh
# documents the two dead ends), and "I looked at the window" cannot tell a
# stalled emulator from a correctly-rendered black frame. Off by default —
# each probe reads 614,400 B back out of VRAM, which is not free.
ifneq ($(DC_FB_PROBE),)
DEFINES += -DDC_FB_PROBE=$(DC_FB_PROBE)
endif

# DC_FB_IMAGE=<1|2|4> — with DC_FB_PROBE set, dump the WHOLE frame as base64
# rows (FBIMG/FBROW) instead of only the 16x12 thumbnail, box-filtered by the
# given factor. tools/dcfb/fbimg_to_png.py turns the log into PNGs. This is the
# only way to SEE what the port renders without a human watching Flycast, and
# every rendering question past "is it black" needs that. ~820 KB of console
# per probe at factor 1, so pair it with a large DC_FB_PROBE.
ifneq ($(DC_FB_IMAGE),)
DEFINES += -DDC_FB_IMAGE=$(DC_FB_IMAGE)
endif

# DC_ARENA_PROBE=<N> — print the arena's touched high-water and the current
# libc break every N frames (kb/STATE.md N4). OSInit() zeroes the arena, so a
# nonzero 1 KB granule is a granule something wrote; the running maximum is the
# high-water. This exists because DC_MAIN_MEMORY_SIZE has never been measured,
# only defended by "no arena-side OOM has been seen", and bisecting it costs a
# full build per data point. Off by default — each probe reads the whole arena.
ifneq ($(DC_ARENA_PROBE),)
DEFINES += -DDC_ARENA_PROBE=$(DC_ARENA_PROBE)
endif

# DC_ASSET_CENSUS=1 — record the asset addresses the renderer is handed and
# print them for tools/dcstub/census_resolve.py to resolve against the ELF.
# This is the only way to learn which acres and NPCs a scene touches: the
# title demo names its acres through BLOCK_COMBI_* indices and its animals
# through profile IDs, so a static trace of src/data sees neither.
#
# Three sources: texture images at GXLoadTexObj, palettes at GXLoadTlut, and
# model vertex data through the OSs16tof32 shim force-included into emu64.c by
# the TU_DEFS rule further down. (Indexed arrays at GXSetArray are recorded too
# and are always empty — this game never uses GX indexed fetch. MEASURED.)
#
# Costs ~110 KB of .bss for the address set and the batch table, which is why
# it is off by default.
# DC_ARAM_LRU=0 — kill switch for PLAN §3.1's disc-backed graph-ARAM pager
# (dc/include/dc_aram_lru.h + dc/src/dc_aram.c + the provenance ring in
# dc_dvd.c). Default 1, as an #ifndef in that header, so the code builds
# without this block; 0 reverts to the 2026-08-02 single anchored window
# (verified byte-for-byte: rebases=14, same anchor offsets) and is kept for
# bisecting. NB the two knobs interact: with the pager ON the window is a
# CACHE and 131072 is the measured setting; with it OFF the floor is 851968.
ifneq ($(DC_ARAM_LRU),)
DEFINES += -DDC_ARAM_LRU=$(DC_ARAM_LRU)
endif

# DC_ARAM_TRACE=1 — log every ARAM DMA (type, offset, length, provenance).
# Off by default: one archive mount is ~150 lines and SCIF is the bottleneck
# on hardware (kb/traps.md). The rate-limited [DC/ARAM] LRU summary is always
# on and is what you normally want.
ifneq ($(DC_ARAM_TRACE),)
DEFINES += -DDC_ARAM_TRACE=$(DC_ARAM_TRACE)
endif

ifneq ($(DC_ASSET_CENSUS),)
DEFINES += -DDC_ASSET_CENSUS=$(DC_ASSET_CENSUS)
endif

# DC_AUTOSTART=<N> — synthesise controller input from PADRead call N onward
# (dc/src/dc_pad.c). kb/boot-blockers.md item 4: the title demo loops forever
# in aAL_game_start_wait waiting for START or A, the harness cannot press a
# button, and therefore NOTHING past the title has ever been observed running.
# Pulses alternate START and A every DC_AUTOSTART_PERIOD calls. Absent by
# default, so a normal image is unchanged.
ifneq ($(DC_AUTOSTART),)
DEFINES += -DDC_AUTOSTART=$(DC_AUTOSTART)
endif
ifneq ($(DC_AUTOSTART_PERIOD),)
DEFINES += -DDC_AUTOSTART_PERIOD=$(DC_AUTOSTART_PERIOD)u
endif

# DC_CONSOLE_LIMIT=0 — kill switch for the console flood limiter
# (dc/src/dc_misc.c: a printf override + the table OSReport also consults).
# Default 1. MEASURED: 85.3 % of one console log was a single jaudio OSReport
# line, and after the title screen game64.c_inc's [TRG_VOL]/[WALK] printf lines
# took over. The limiter keys on the format-string pointer and backs off to
# powers of two, prefixing surviving repeats with [xN].
ifneq ($(DC_CONSOLE_LIMIT),)
DEFINES += -DDC_CONSOLE_LIMIT=$(DC_CONSOLE_LIMIT)
endif

# DC_TEX_LOG=1 — one line per texture UPLOAD describing what the decoder
# produced (non-zero texel count, value range, distinct-value count). Separates
# "never uploaded" from "uploaded as a black rectangle", which the
# uploads/hits/evictions counters cannot.
ifneq ($(DC_TEX_LOG),)
DEFINES += -DDC_TEX_LOG=$(DC_TEX_LOG)
endif

# DC_PVR_BATCH_LOG=<N> — dump every renderer batch's state on every Nth frame.
# Set it to the SAME N as DC_FB_PROBE so the lines describe the frame the
# screenshot captured; that pairing is what lets a region of a decoded PNG be
# attributed to the state that drew it. ~77 lines per logged frame.
ifneq ($(DC_PVR_BATCH_LOG),)
DEFINES += -DDC_PVR_BATCH_LOG=$(DC_PVR_BATCH_LOG)
endif

# DC_SCIF_FAST=1 — raise the console from KOS's 57,600 baud to 1,562,500
# (~5.8 KB/s -> ~150 KB/s). EMULATOR RUNS ONLY: a real coder's cable will not
# sync at 1.5 Mbps and a hardware build with this set loses its console, crash
# dumps included. The harness's own selftest has used exactly this rate since
# M0 (harness/dc/selftest/selftest.c:134).
#
# What it buys: a DC_FB_IMAGE screenshot is ~205 KB of base64, i.e. ~35 s of
# wall clock at the default rate. At the fast rate it is ~1.4 s, so a
# screenshot run stops being a different experiment from a progression run and
# full 640x480 captures (DC_FB_IMAGE=1) become affordable.
ifneq ($(DC_SCIF_FAST),)
DEFINES += -DDC_SCIF_FAST=$(DC_SCIF_FAST)
endif

# DC_XDEFS — raw extra -D flags, appended last so they win.
#
# The renderer's kill switches (DC_PVR_NO_CULL, DC_PVR_CULL_INVERT,
# DC_PVR_NO_UVCLAMP, DC_PVR_NO_LIGHTING, DC_PVR_NO_NEARCLIP,
# DC_PVR_NO_TEXTURES) are compile-time A/B knobs, not settings, so each one
# does not deserve its own named variable. Before this existed they could only
# be reached by hand-editing the Makefile, which is how the three A/B CDIs in
# ~/.cache were built and why none of them is reproducible from a command line.
#
#   DC_XDEFS='-DDC_PVR_NO_UVCLAMP' bash dc/build-dc.sh
#
# It is part of flags.stamp, so changing it forces the rebuild it should.
ifneq ($(DC_XDEFS),)
DEFINES += $(DC_XDEFS)
endif

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

# Stub swap happens HERE, after the per-TU classifications above have matched
# on src/ paths — object names derive from the source path, so C11_SRC has to
# be rewritten too or system.c (the one stubbed jaudio TU) silently reverts to
# gnu89 and fails on its C99 declarations.
GAME_C       := $(call stubify,$(GAME_C))
GAME_CXX     := $(call stubify,$(GAME_CXX))
C11_SRC      := $(call stubify,$(C11_SRC))
CXXFROMC_SRC := $(call stubify,$(CXXFROMC_SRC))

# DC_SRC_SHRINK's per-TU swaps, applied after stubify for the same reason.
# The two swap sets MUST stay disjoint — a file claimed by both would silently
# lose one of the two rewrites, since only one source path per TU is compiled.
# make_src_shrink.py hard-errors if any of its targets appears in stub.list,
# so this cannot drift unnoticed.
GAME_C       := $(call shrinkify,$(GAME_C))
GAME_CXX     := $(call shrinkify,$(GAME_CXX))
C11_SRC      := $(call shrinkify,$(C11_SRC))
CXXFROMC_SRC := $(call shrinkify,$(CXXFROMC_SRC))
# PC_REUSE_C too — S6 rewrites pc/src/pc_assets.c (kb/ram-plan.md P6). This MUST
# stay above the $(PC_REUSE_OBJS) definition below, or the gnu11 override would
# still name the vendored path and the swapped copy would be built as gnu89.
PC_REUSE_C   := $(call shrinkify,$(PC_REUSE_C))

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
#
# THESE MUST FOLLOW THE SOURCE INTO THE SCRATCH TREES. A per-TU rule is keyed on
# the OBJECT path, and $(stubify)/$(shrinkify) change it — so the moment either
# rewriter emits main.c or boot.c, a rule spelled only for $(OBJDIR)/src/... is
# silently skipped and the TU compiles with its own bare main().
#
# MEASURED FAILURE, do not re-discover it: make_stub_data.py's NEUTRALISE rule
# started emitting src/static/boot.c, boot.c lost -Dmain=boot_main, and the
# build then had TWO main()s. -Wl,--allow-multiple-definition (needed for the
# 1,367 multiply-defined data symbols, kb/levers.md L6) swallowed it, the linker
# kept the wrong one, and --gc-sections deleted everything the game's real entry
# chain reached: .text fell 5,289,364 -> 851,684 and boot_main / graph_proc /
# ac_entry / mainproc vanished from the ELF. It LINKED and BUILT A CDI.
#
# $(call stubify,...) / $(call shrinkify,...) resolve to the same path when the
# rewriters are off, so this is a no-op in a plain build.
$(OBJDIR)/$(call shrinkify,$(call stubify,src/main.c)).o:        TU_DEFS = -Dmain=ac_entry
$(OBJDIR)/$(call shrinkify,$(call stubify,src/static/boot.c)).o: TU_DEFS = -Dmain=boot_main

# DC_ASSET_CENSUS: the vertex half of the working set is observable in exactly
# ONE TU. emu64 walks the N64 display lists itself and hands GX finished floats,
# so no dc/ function ever sees a Vtx*; GXSetArray, the obvious hook, is dead in
# this game — its only call site in src/ is GXInit.c:252's own reset loop, and
# the census measured zero hits on it. The one seam is OSs16tof32(), which
# dl_G_VTX uses to read each source vertex's x/y/z, and whose only three call
# sites in the whole TU are exactly those three. dc/include/dc_census_vtx.h
# wraps it.
#
# Force-included HERE rather than from dc_prelude.h on purpose: the shim has to
# pull a decomp header (dolphin/os/OSFastCast.h) in so it can rename the
# vendored inline out of the way, and the prelude must stay free of decomp
# declarations for all 3917 TUs. Scoping it to one TU also means the two jaudio
# TUs that use OSs16tof32 contribute no noise.
#
# Same path caveat as the two rules above: the object name follows the source
# into the stub/shrink scratch trees, so it must be spelled through both calls.
ifneq ($(DC_ASSET_CENSUS),)
$(OBJDIR)/$(call shrinkify,$(call stubify,src/static/libforest/emu64/emu64.c)).o: \
	TU_DEFS = -include $(DC_DIR)/include/dc_census_vtx.h
endif

# =============================================================================
# Flag stamp — make tracks timestamps, not command lines
# =============================================================================
# MEASURED, and it produced a wrong image: after a DC_ASSET_STUB=1 build, a
# plain `bash dc/build-dc.sh` printed "Nothing to be done for 'all'" and left
# the STUB ELF in place. Two independent failures:
#
#   1. Toggling DC_ASSET_STUB swaps 2,521 sources for their stub twins, so OBJS
#      names change — but BOTH sets of .o already exist and are older than
#      $(TARGET), so make sees nothing to do and never relinks.
#   2. Objects whose source did not change keep whatever -D they were built
#      with. dc/src/*.o compiled under -DDC_ASSET_STUB would silently keep
#      skipping pc_assets_init() in a non-stub image.
#
# The stamp holds the flag set that affects codegen. When it changes the file
# is rewritten, and every object plus the link depends on it, so everything
# rebuilds. Written with $(file …) — a shell here-doc would re-run on every
# make invocation and defeat the point.
#
# DC_SRC_SHRINK is in the stamp for BOTH of the reasons above and one more:
# toggling it changes $(INCLUDES), and an include-path change is invisible to
# make's timestamp model in exactly the same way a -D change is. A TU whose
# source did not change would keep an object built against the other
# sys_stacks.h — i.e. a 4,096 B memset into a 16 B array. Full rebuild is the
# correct price.
# =============================================================================
# G1: per-opcode emu64 timing histogram (KILL SWITCH: DC_EMU64_HIST=0, default)
# =============================================================================
# The value is the SAMPLING PERIOD in presented frames: 0 = off, 1 = every
# frame, 4 = one frame in four. Off by default, so a normal build is
# byte-identical.
#
# WHY THIS EXISTS. The [EMU64] counters say 2094 of 2867 town commands -- 73 %
# -- are neither vertex nor triangle nor display-list, i.e. they are pure RDP/
# RSP state. At the measured 12.31 us/cmd that is ~26 ms of an ~85 ms frame,
# and nothing in the tree can say WHICH opcodes. emu64's own command_info[]
# has a free per-opcode CALL count but its time half is dead: both
# EMU64_TIMED_SEGMENT_* macros expand to nothing without EMU64_DEBUG, and the
# EMU64_DEBUG forms do not compile (`this->#stat` is a stray stringify) and
# would divide time/time anyway (emu64.c:5940).
#
# WHY IT IS LEGAL (CLAUDE.md §1). src/ is neither edited nor compiled
# differently. emu64's dispatch table (emu64.c:5702) is a non-const
# file-static array in .data and there is no MMU write protection; the
# instrument swaps in timing thunks at runtime and swaps the originals back.
# The only BUILD change is that objcopy promotes one already-existing local
# symbol to global so dc/ can name it. That is a symbol BINDING change: not
# one byte of loaded image differs and no instruction is reselected. Layout,
# not codegen.
#
# The mangled name is C++ (dc/Makefile puts emu64.c in CXXFROMC_SRC) plus the
# sh-elf leading underscore: `_ZL11dl_func_tbl` in the source, `__ZL11dl_func_tbl`
# in the object. Verified with sh-elf-nm on the shipped emu64.c.o.
DC_EMU64_HIST ?= 0
ifneq ($(DC_EMU64_HIST),0)
DEFINES += -DDC_EMU64_HIST=$(DC_EMU64_HIST)
EMU64_HIST_OBJ := $(OBJDIR)/src/static/libforest/emu64/emu64.c.o
$(EMU64_HIST_OBJ): TU_POST = sh-elf-objcopy --globalize-symbol=__ZL11dl_func_tbl $@
endif

FLAGSTAMP := $(BUILD)/flags.stamp
FLAGSTATE := DC_ASSET_STUB=$(DC_ASSET_STUB) DC_SRC_SHRINK=$(DC_SRC_SHRINK) \
             DECOMP_OPT=$(DECOMP_OPT) \
             DC_OPT=$(DC_OPT) DEFINES=$(DEFINES)

ifneq ($(FLAGSTATE),$(strip $(shell cat $(FLAGSTAMP) 2>/dev/null)))
$(shell mkdir -p $(BUILD))
$(file >$(FLAGSTAMP),$(FLAGSTATE))
endif

# =============================================================================
# Rules
# =============================================================================
.PHONY: all objs clean sources count help
.SUFFIXES:

all: $(TARGET)

$(OBJS) $(TARGET): $(FLAGSTAMP)

# Fallback so the stamp is never a missing prerequisite. The $(file …) above
# is what normally writes it; this rule covers a wiped build dir, and make
# 3.81 on the macOS host, which has no $(file …) and silently expands it to
# nothing. The host never compiles — it only runs `count`/`sources` — so the
# no-op there is harmless.
$(FLAGSTAMP):
	@mkdir -p $(@D) && printf '%s' '$(FLAGSTATE)' > $@

objs: $(OBJS)
	@echo "== objs: $(words $(OBJS)) objects up to date (NOT linked) =="

help:
	@sed -n '2,30p' $(firstword $(MAKEFILE_LIST))

TU_POST = true
COMPILE = $(Q)mkdir -p $(@D) && $(TU_CC) $(TU_XLANG) $(COMMON) $(TU_DEFS) $(TU_LANG) $(TU_OPT) \
	  -MMD -MP -MF $(@:.o=.d) -MT $@ -c $< -o $@ && $(TU_POST)

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
