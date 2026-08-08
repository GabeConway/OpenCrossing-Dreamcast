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
#   DC_BGTEX_DEMAND=1  acre ground textures read off the disc, DEFAULT ON
#                    (R1, -150,880 B of keep list). 0 restores the bcopy and
#                    the 27 keep-list entries. MUST match the --bgtex-demand=
#                    that BOTH tools/dcstub scripts were run with.
#   DC_NPCTEX_POOL=1 villager textures served out of 16 disc-backed slots,
#                    DEFAULT ON (R2, -993,984 B of keep list). 0 restores the
#                    21 censused keep-list entries and pools nothing. MUST
#                    match the --npctex-pool= that BOTH scripts were run with.
#   DC_NPCMDL_POOL=1 villager MODELS served out of 16 disc-backed slots, with
#                    their 933 baked gsSPVertex words relocated. DEFAULT ON
#                    (R3). ⚠️ COSTS ~115,296 B of .bss — it restores geometry
#                    31 species never had, it does not save bytes. 0 restores
#                    cbr_1 to the keep list and pools nothing. MUST match the
#                    --npcmdl-pool= that BOTH scripts were run with.
#   DC_TEXPOOL_PROBE=0  T1's falsification probe, DEFAULT OFF. Counts only —
#                    moves no bytes, changes no pixel. Needs DC_ASSET_STUB=1 and
#                    a stub tree built with --texpool=1. MUST match the
#                    --texpool= that make_stub_data.py was run with.
#   DC_AUDIO=0       sound. DEFAULT OFF — it costs ~45 % of the frame rate
#                    (dc/src/dc_audio.c). Also selects the S8 half of the
#                    shrink tree, so it MUST match the --audio= that
#                    tools/dcstub/make_src_shrink.py was run with.
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
# Vendored sh4zam (dc/third_party/sh4zam/VENDORING.md). Defined HERE, with the
# other paths, because $(INCLUDES) below is := and expands it long before the
# source-discovery section that builds $(SHZ_C) from it.
SHZ_DIR   := $(DC_DIR)/third_party/sh4zam

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
  # R1's map. Checked here rather than left to dc/src/dc_bgtex.c's #include so
  # a stub tree generated before R1 landed says so at once, instead of failing
  # 3,900 TUs later with a "no such file" on one platform source.
  ifeq ($(wildcard $(STUBDIR)/dc_bgtex_map.inc),)
    $(error DC_ASSET_STUB=1 but $(STUBDIR)/dc_bgtex_map.inc is missing — the \
            stub tree predates R1. Run: python3 \
            $(ROOT)/tools/dcstub/make_stub_data.py)
  endif
  # R2's map, checked here for the same reason as R1's above.
  ifeq ($(wildcard $(STUBDIR)/dc_npctex_map.inc),)
    $(error DC_ASSET_STUB=1 but $(STUBDIR)/dc_npctex_map.inc is missing — the \
            stub tree predates R2. Run: python3 \
            $(ROOT)/tools/dcstub/make_stub_data.py)
  endif
  # R3's map, checked here for the same reason as R1's and R2's above.
  ifeq ($(wildcard $(STUBDIR)/dc_npcmdl_map.inc),)
    $(error DC_ASSET_STUB=1 but $(STUBDIR)/dc_npcmdl_map.inc is missing — the \
            stub tree predates R3. Run: python3 \
            $(ROOT)/tools/dcstub/make_stub_data.py)
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
# Optimization — DC_OPT_PROFILE (KILL SWITCH: DC_OPT_PROFILE=o0)
# =============================================================================
# *** POLICY REVERSED 2026-08-06 BY USER DECISION. The old rule was "decomp
# game code stays at -O0, settled, not a tuning parameter". It is now a tuning
# parameter, and the default is NOT -O0. ***
#
# WHY THE OLD RULE EXISTED, and why it did not survive contact:
#
#   pc/CMakeLists.txt:21-29 records the armhf device build's history — -O2 gave
#   a wild-pointer crash loop from boot, -O1 a hard SIGBUS on the intro train
#   scene, no -O was stable. That history is REAL but it is ARM, a different
#   compiler and a different ABI, and it was inherited into this port as policy
#   without ever being reproduced on SH-4.
#
#   The counter-argument, from the KOS/sh4zam maintainer (2026-08-06): GCC's
#   SH-4 output at -O0 is not "unoptimized", it is pathological — dead branches
#   still emit instructions, every local round-trips through memory, and whole
#   codebases run at a third of their -O2 frame rate. -O0 costs more here than
#   on any other target this decomp has been built for, and this port is
#   CPU-bound with 77 % of its frame in decomp code.
#
# MEASURED ON THIS TREE, sh-elf GCC 15.2, 2026-08-06 (town keep list, stub
# image, DC_SRC_SHRINK=1 — i.e. the shipping configuration, not M1's):
#     -O0   text 5,506,964   data 2,337,980   bss 3,945,356
#     -Os   text 2,680,676   data 2,224,832   bss 3,945,484
# -Os is 2,826,288 B of .text cheaper, which is RAM as much as it is speed
# (kb/closed.md: every image byte destroys a heap byte).
#
# THE SHAPE OF THE POLICY, and it is the one every major DC port uses: you want
# -O3's speed on the frame path and -Os's size everywhere else, and 16 MB says
# you cannot have -O3 everywhere. So:
#
#   DECOMP_OPT      -Os   every decomp TU (~3,900 of them: size is the budget)
#   DECOMP_HOT_OPT  -O3   the reviewed hot list in dc/opt-lists.mk only
#   -O0                   the QUARANTINE list in dc/opt-lists.mk — TUs that are
#                         MEASURED to miscompile. Each entry needs a reason,
#                         a date, and the run that caught it.
#
# PROFILES
#   DC_OPT_PROFILE=perf  (default)  -Os everywhere, -O3 on the hot list
#   DC_OPT_PROFILE=size             -Os everywhere, hot list gets -Os too.
#                                   The lever to pull when the image will not
#                                   fit; costs frame time, saves .text.
#   DC_OPT_PROFILE=o0               -O0 everywhere, hot list and quarantine
#                                   both inert. THE KILL SWITCH: this is a
#                                   byte-identical revert to the pre-2026-08-06
#                                   build, and it is what you bisect against
#                                   when an optimized image misbehaves.
#
# DECOMP_OPT / DECOMP_HOT_OPT set in the environment still win over the
# profile, so a one-off `DECOMP_OPT=-O1 bash dc/build-dc.sh` needs no edit here.
#
# NOTE: an explicit -O<n> (not "empty") is always required, because $KOS_CFLAGS
# already contains -O2 and the LAST -O on the command line wins.
DC_OPT_PROFILE ?= perf

ifeq ($(DC_OPT_PROFILE),perf)
  DECOMP_OPT     ?= -Os
  DECOMP_HOT_OPT ?= -O3
else ifeq ($(DC_OPT_PROFILE),size)
  DECOMP_OPT     ?= -Os
  DECOMP_HOT_OPT ?= -Os
else ifeq ($(DC_OPT_PROFILE),o0)
  DECOMP_OPT     ?= -O0
  DECOMP_HOT_OPT ?= -O0
else
  $(error DC_OPT_PROFILE='$(DC_OPT_PROFILE)' is not one of: perf size o0)
endif

# dc/src is code we own, and since the decomp stopped being the whole frame it
# is a much larger share of it: xform was 15.6 % of the draw phase at -O0 and is
# ~27 % now. MEASURED 2026-08-06, matched town windows, -O2 -> -O3:
#     us/v      4.05 -> 3.11   (-23 %, the per-vertex transform cost itself)
#     xform     12.4 -> 9.9 ms
#     gx        14.6 -> 12.3 ms
#     town FPS  20.0 -> 20.6
#     .text     +24,548 B
# Screenshot-gated against the -O2 build of the same source.
DC_OPT ?= -O3

# Extra guards that only make sense once the optimizer is actually running.
# They are appended to UB_GUARDS for decomp TUs and are EMPTY at -O0, so the
# kill-switch profile stays byte-identical.
#
#   -fno-isolate-erroneous-paths-dereference
#       On at -O2. It rewrites a path that provably dereferences NULL into a
#       trap, on the theory that the path is unreachable. Decomp code reaches
#       such paths for real (the game tolerated the read on GameCube), and a
#       trap there is exactly the "wild-pointer crash from boot" shape the
#       armhf record describes. Same intent as -fno-delete-null-pointer-checks,
#       which is already in UB_GUARDS but does not cover this transform.
#   -fno-ipa-icf
#       Identical-code folding merges functions that compare equal. Harmless
#       for correctness, but it makes a crash address ambiguous in
#       harness/dc/crash.sh, and the whole point of this change is that we can
#       triage what it breaks.
#   -fno-ipa-sra
#       IPA-SRA rewrites the calling convention of static functions whose
#       parameters it can prove unused. The decomp defines 968 functions with
#       K&R empty parameter lists and CALLS SOME OF THEM WITH ARGUMENTS —
#       m_camera2.c:226 `static f32 Camera2_BorderSize()` is called
#       `Camera2_BorderSize(play)` at :258,:259,:261 and nine more sites, on
#       the per-frame camera path. That is real UB and IPA-SRA is the pass
#       that acts on exactly this shape.
#   -fno-store-merging
#       On at -O2. It fuses adjacent narrow stores into one wide store, and
#       SH-4 traps ANY misaligned mov.w/mov.l — there is no -mno-unaligned-
#       access on this target (kb/design-shelf-flags.md §7). It was measured
#       not to fire on GCC 9.3 and has NEVER been re-measured on our GCC 15.2,
#       so this is insurance against an untested class, not a known bug.
ifeq ($(DECOMP_OPT),-O0)
  OPT_GUARDS :=
else
  OPT_GUARDS := -fno-isolate-erroneous-paths-dereference -fno-ipa-icf \
                -fno-ipa-sra -fno-store-merging
endif

# DC_AUTOVAR_INIT=zero — force every uninitialised local to zero (GCC 12+).
#
# NOT a default, and the reason is that it is a DIAGNOSTIC first. Reading an
# uninitialised local is the one UB class with no guard flag and a confirmed
# instance in this tree (ac_npc_p_sel.c:141-148 reads `current_frame` from a
# struct field that sAdos_GetStaffRollInfo's early return never writes —
# kb/RESUME.md §5b). At -O0 such a read gets whatever was on the stack; at -Os
# it gets whatever VRP decided, and the two differ.
#
# So when an optimized image misbehaves, build it again with
# DC_AUTOVAR_INIT=zero: if the symptom disappears, the bug is an uninitialised
# read and the warnscan log names the candidates. It costs .text and cycles on
# every function with locals, which is why it is off in a shipping image.
ifneq ($(DC_AUTOVAR_INIT),)
  OPT_GUARDS += -ftrivial-auto-var-init=$(DC_AUTOVAR_INIT)
endif

# The hot list and the quarantine list live in their own file because they are
# data with justifications attached, not build logic. They are resolved into
# object paths (and validated) further down, after $(stubify)/$(shrinkify).
include $(DC_DIR)/opt-lists.mk

# =============================================================================
# WARNSCAN — enumerate the UB that optimization is allowed to punish
# =============================================================================
# `make -C dc warnscan` (or DC_TARGET=warnscan bash dc/build-dc.sh) recompiles
# every TU at -O2 with the warning suppression REMOVED, into a throwaway
# objdir, and leaves the log at dc/build/warnscan.log.
#
# WHY IT EXISTS. The decomp is compiled with -w -fpermissive -Wno-return-type,
# so the two diagnostics that predict an optimization miscompile are silenced
# by construction:
#   * control reaches end of non-void function — UB, and in the four TUs
#     compiled as C++ (CXXFROMC_SRC, emu64.c among them) G++ turns it into
#     __builtin_unreachable and deletes the path at -O2.
#   * may be used uninitialised — the port has already been bitten by one of
#     these for real (ac_npc_p_sel.c:141-148, kb/RESUME.md §5b).
# A quarantine list built from this scan is evidence; one built from bisecting
# crashes costs a run per candidate.
#
# It is a separate objdir on purpose: warnscan objects are -O2 regardless of
# profile and must never be linked or mistaken for build output.
WARNSCAN_FLAGS := -Wreturn-type -Wuninitialized -Wmaybe-uninitialized \
	-Warray-bounds -Wstrict-aliasing=2 -Wsequence-point -Wnonnull \
	-Wno-error=return-type
ifeq ($(WARNSCAN),1)
  DECOMP_QUIET :=
else
  DECOMP_QUIET := -w
endif

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
	-I$(SHZ_DIR)/include \
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

# =============================================================================
# DC_BGTEX_DEMAND — the acre ground textures, off the disc (R1). DEFAULT ON.
# =============================================================================
# The 96 mFM_grd_* source arrays (150,880 B: 46 summer, 41 winter, 9 shared)
# exist only to be bcopy'd, once per acre load, into the 27 always-resident
# staging buffers in src/game/m_bg_tex.c. Keeping them stores the same bytes
# twice, and the keep list could only ever afford ONE season — which is why the
# town ground goes black in December. With this on they are read straight off
# the disc into the staging buffer and neither season needs keeping.
#
# Three parts and they must agree, which is why this is ONE number:
#   tools/dcstub/make_stub_data.py --bgtex-demand   emits dc_bgtex_map.inc, and
#                                                   at 0 puts the 27 summer
#                                                   files back on the keep list
#   tools/dcstub/make_src_shrink.py --bgtex-demand  rewrites the copy loop
#   this -D                                         is what the generated TU
#                                                   checks itself against
#
# Both scratch trees survive a flag flip and flags.stamp cannot regenerate
# them, so the rewritten m_field_make.c carries an #error for a mismatch in
# EITHER direction. Do not delete that guard — re-run the generators
# (dc/build-dc.sh does both for you).
#
# The define is unconditional so both sides always see the same literal. It is
# read by the shrink tree's m_field_make.c only; dc/src/dc_bgtex.c keys off the
# generated map's own row count instead, so a non-stub build (where every
# source is full size and resident) degrades to a plain memmove with no flag.
DC_BGTEX_DEMAND ?= 1
DEFINES += -DDC_BGTEX_DEMAND=$(DC_BGTEX_DEMAND)

# =============================================================================
# DC_NPCTEX_POOL — the villager textures, out of a 16-slot pool (R2). DEFAULT ON.
# =============================================================================
# src/data/npc/model/tex/*.c is 1,154,944 B of .bss in 276 sets, 993,984 B of
# it the 236 VILLAGER sets. A town holds at most ANIMAL_NUM_MAX=15 villagers
# plus one islander, so at most 16 of those 236 can ever be wanted at once —
# the rest is .bss a given save cannot draw. The keep list was paying 90,464 B
# for the 21 sets a census run happened to see and leaving the other 215
# species with no texture at all. With this on, a set is read off the disc into
# one of 16 static slots at the moment an actor snapshots its
# npc_draw_data_tbl[] row, and neither the 21 nor the 215 cost anything.
#
# ⚠️ The SPECIAL NPCs are NOT pooled. Rows at or above ALL_NPC_NUM — Tom Nook,
# Rover, K.K., Porter, the raccoons — stay resident through the keep list, and
# dc/src/dc_npctex.c refuses to touch them. Dropping them silently has already
# cost this project a session (kb/RESUME.md, session 3 item 2).
#
# Three parts and they must agree, which is why this is ONE number:
#   tools/dcstub/make_stub_data.py --npctex-pool   emits dc_npctex_map.inc, and
#                                                  at 0 puts the 21 villager
#                                                  tex files back on the keep
#                                                  list
#   tools/dcstub/make_src_shrink.py --npctex-pool  inserts the load + the two
#                                                  reset calls (S11)
#   this -D                                        is what the generated TUs
#                                                  check themselves against
#
# Both scratch trees survive a flag flip and flags.stamp cannot regenerate
# them, so all three rewritten TUs carry an #error for a mismatch in EITHER
# direction. Do not delete those guards — re-run the generators (dc/build-dc.sh
# does both for you).
#
# The define is unconditional so both sides always see the same literal. It is
# read by the shrink tree's three TUs only; dc/src/dc_npctex.c keys off the
# generated map's own set count instead, so a non-stub build (where every
# villager texture is full size and resident) degrades to a pair of empty
# functions with no flag.
# ⚠️ DEFAULTS TO 0, AND THE REASON IS NOT THAT IT IS BROKEN — IT IS UNEXERCISED.
# The pool loads on aNPC_dma_draw_data_proc, which is reached only when an NPC
# actor is constructed for a VILLAGER, and MEASURED 2026-08-05: the port never
# constructs one. mNpc_SetNpcList reads the save's Animal_c animals[]
# (m_start_data_init.c:559), the VMU path is unwired, `[PC] No save file found`,
# so the town has ZERO villagers. Two 900 s runs that reach scene 9 and walk
# around print not one [DC/NPCTEX] line.
#
# Turning it on would therefore ship a keep-list removal (21 files, 90,464 B)
# whose replacement has never once executed, on the strength of a screenshot
# pair that cannot show a villager because there are none. Off until the save
# path (PLAN N2b) makes villagers exist; then set =1 and read the log.
DC_NPCTEX_POOL ?= 0
DEFINES += -DDC_NPCTEX_POOL=$(DC_NPCTEX_POOL)

# DC_NPCTEX_SLOTS=<N> — the pool's slot count, default 16 (ANIMAL_NUM_MAX + the
# islander), 4,832 B of .bss each. Raise it only against a
# "[DC/NPCTEX] POOL FULL" line in the log: that is an event or mask NPC whose
# texture_id resolved to a villager row (ac_npc_ctrl.c_inc:158-176), which the
# 15+1 ceiling does not count. A villager the pool cannot serve renders
# untextured and says so; it is never another animal's skin.
ifneq ($(DC_NPCTEX_SLOTS),)
DEFINES += -DDC_NPCTEX_SLOTS=$(DC_NPCTEX_SLOTS)
endif

# =============================================================================
# DC_NPCMDL_POOL — the villager MODELS, out of a 16-slot pool (R3). DEFAULT ON.
# =============================================================================
# ⚠️ THIS ONE COSTS .bss. Read the ledger before turning it on in a build that
# is already tight.
#
# src/data/npc/model/mdl/*.c is 438,640 B in 72 species, 194,400 B of it the 32
# reachable from a VILLAGER row of npc_draw_data_tbl[] and 244,240 B the 40
# reachable only from a SPECIAL row. Exactly ONE villager model was resident
# before R3 — cbr_1, 5,536 B, because a census run saw it — so the other 31
# species drew as a black spiky mess no matter how good the texture R2 painted
# on them (an unkept asset loses its VERTICES, kb/traps.md).
#
#     resident before      5,536 B      pool           120,832 B  (16 x 7,552)
#     keep list returned  -5,536 B      bookkeeping        128 B
#     .rodata              4,372 B      ------------------------------
#                                       net           +115,296 B of .bss
#
# R3 is a CONTENT RESTORATION, not a saving. What makes the POOL the right
# shape is the alternative: keeping all 32 costs 194,400 B, so this is 73,568 B
# cheaper than the geometry it delivers. Cut DC_NPCMDL_SLOTS, or set this to 0,
# if the image cannot afford it.
#
# ⚠️ The SPECIAL NPCs are NOT pooled, and here that is structural rather than a
# rule: the 40 skeletons only special rows use have no entry in the map, so the
# lookup cannot find them. Verified 2026-08-05 that the villager and special
# skeleton sets do not intersect at all; the generator hard-errors if that
# changes.
#
# Unlike R1 and R2 this one RELOCATES. 933 gsSPVertex words are R_SH_DIR32
# relocations the linker baked into .data and emu64::seg2k0 passes an
# 0x8Cxxxxxx address through untouched, so moving a vertex array means
# rewriting them. dc/src/dc_npcmdl.c carries the argument for why the patch
# table is generated offline and re-checked at runtime rather than walked.
#
# Three parts and they must agree, which is why this is ONE number:
#   tools/dcstub/make_stub_data.py --npcmdl-pool   emits dc_npcmdl_map.inc, and
#                                                  at 0 puts cbr_1 back on the
#                                                  keep list
#   tools/dcstub/make_src_shrink.py --npcmdl-pool  inserts the load + the two
#                                                  reset calls (S12)
#   this -D                                        is what the generated TUs
#                                                  check themselves against
#
# Both scratch trees survive a flag flip and flags.stamp cannot regenerate
# them, so all three rewritten TUs carry an #error for a mismatch in EITHER
# direction. Do not delete those guards — re-run the generators (dc/build-dc.sh
# does both for you).
#
# The define is unconditional so both sides always see the same literal. It is
# read by the shrink tree's three TUs only; dc/src/dc_npcmdl.c keys off the
# generated map's own species count instead, so a non-stub build (where every
# vertex array is full size and every baked word already correct) degrades to a
# pair of empty functions with no flag.
# ⚠️ DEFAULTS TO 0 for exactly the reason DC_NPCTEX_POOL does — see that comment.
# It shares the same seam, so it has never executed either. It is also the one
# of the two that SPENDS bytes (+115,424 B .bss for a 16-slot pool), so there is
# no argument for carrying it on by default while it does nothing.
DC_NPCMDL_POOL ?= 0
DEFINES += -DDC_NPCMDL_POOL=$(DC_NPCMDL_POOL)

# DC_NPCMDL_SLOTS=<N> — the pool's slot count, default 16 (ANIMAL_NUM_MAX + the
# islander, matching DC_NPCTEX_SLOTS so a species that gets a texture slot can
# get a model slot), 7,552 B of .bss each. This is the FIRST knob to cut if the
# image is short: only 32 distinct villager models exist for 236 texture sets,
# so 16 villagers collide often and a smaller pool usually still serves them
# all. A villager the pool cannot serve keeps its stub geometry and says so; it
# is never another animal's limbs.
ifneq ($(DC_NPCMDL_SLOTS),)
DEFINES += -DDC_NPCMDL_SLOTS=$(DC_NPCMDL_SLOTS)
endif

# =============================================================================
# DC_TEXPOOL_PROBE — T1's falsification probe (KILL SWITCH: =0, the default)
# =============================================================================
# NOT a pool and NOT a lever: it moves no bytes and changes no pixel. It counts,
# per texture bind, whether the source pointer is the EXACT BASE of a mapped
# asset row — which is the one question that decides whether T1 can synthesise a
# texture's cache key from its asset identity instead of reading the array back
# out of main RAM on every bind (dc_pvr_texture.c:1083, ~109 binds/frame).
#
# Three counters kill the design if non-zero — `interior`, `mutated`,
# `oversize` — plus `aliased`, which dc/src/dc_texpool.c adds for the same
# reason `mutated` exists. RUN IT BEFORE WRITING THE LOADER (kb/levers.md L10).
#
# Two parts and they must agree, which is why this is ONE number:
#   tools/dcstub/make_stub_data.py --texpool   emits dc_texpool_map.inc
#   this -D                                    compiles the probe in
# Unlike R1/R2/R3 there is no keep-list half: the probe restores nothing and
# frees nothing, so at 0 the generator emits an empty map and the TU is empty.
#
# ⚠️ REQUIRES DC_ASSET_STUB=1, because the map is generated into $(STUBDIR) and
# only make_stub_data.py writes it. Checked below rather than left to
# dc/src/dc_texpool.c's #include, so a mismatch says so at once instead of
# failing 3,900 TUs later on one platform source.
#
# COSTS, and why it is off by default: ~97,472 B of .rodata for the 6,092 rows
# plus their names, and ~42.6 KB of .bss for the sorted order, the per-row
# baseline hashes and the seen flags. That is a diagnostic build, not a shipping
# one.
DC_TEXPOOL_PROBE ?= 0
ifneq ($(DC_TEXPOOL_PROBE),0)
  ifneq ($(DC_ASSET_STUB),1)
    $(error DC_TEXPOOL_PROBE=$(DC_TEXPOOL_PROBE) needs DC_ASSET_STUB=1 — \
            dc_texpool_map.inc is generated into $(STUBDIR) and only \
            make_stub_data.py writes it)
  endif
  ifeq ($(wildcard $(STUBDIR)/dc_texpool_map.inc),)
    $(error DC_TEXPOOL_PROBE=$(DC_TEXPOOL_PROBE) but \
            $(STUBDIR)/dc_texpool_map.inc is missing — the stub tree was \
            generated without --texpool. Run: python3 \
            $(ROOT)/tools/dcstub/make_stub_data.py --texpool=1)
  endif
DEFINES += -DDC_TEXPOOL_PROBE=$(DC_TEXPOOL_PROBE)
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

# =============================================================================
# DC_AUDIO — sound, and the second thing it now switches
# =============================================================================
# DEFAULT 0. It was already a knob, but only as an #ifndef inside
# dc/src/dc_audio.c, reachable solely through DC_XDEFS. It is promoted to a
# first-class variable here because it no longer affects one TU: it also
# selects which jaudio pools tools/dcstub/make_src_shrink.py is allowed to
# shrink (rules S8a-d, 401,216 B of .bss).
#
# WHY OFF. Measured over matched 600 s runs differing only by this flag:
# FPS p50 23.5 -> 13.0. One jaudio DAC frame costs ~19.8 ms of SH-4 and yields
# ~35 ms of audio, so synthesis needs ~57 % of the machine to stay level with
# playback. The full argument, and why no DC_AUDIO_BUDGET_US setting rescues
# it, is the comment block at dc/src/dc_audio.c:44.
#
# ⚠️ THE TWO HALVES MUST AGREE. dc/build/shrinksrc is generated HOST-side and
# survives both `make` and a flag flip; flags.stamp forces a rebuild when this
# changes but cannot regenerate that tree. So a stale --audio=0 tree plus
# DC_AUDIO=1 would compile an 8-track sequencer and 8 mixer channels into a
# build that thinks it has 256 of each. The generated TUs carry an #error for
# exactly this case — do not "fix" that by deleting the guard; re-run the
# generator (dc/build-dc.sh does it automatically).
# DC_AUDIO_SCENES — WHICH scenes pay for sound.
#
# 45 % is a TOWN number. The town draws ~6,951 vertices/frame and never exceeds
# 14.9 FPS; the K.K. / player-select scene draws 888 and sits AT the 30 FPS cap
# with cycles to spare. One global switch made the cheap scene pay the
# expensive scene's price, and a user noticed the result: K.K.'s scene opens
# with `strum_timer = 440` (ac_npc_p_sel_schedule.c_inc:71), 7.3 s in which he
# plays a song the game requests every tick and we render as silence.
#
#   DC_AUDIO_SCENES=3            bash dc/build-dc.sh   # K.K. sings, town silent
#   DC_AUDIO=1                   bash dc/build-dc.sh   # every scene (old meaning)
#
# Scene modes: 0 title (also the .bss zero value, i.e. "not chosen yet"),
# 3 K.K./player-select, 4 train intro, 9 outdoor town, 18 name entry.
# `all`/`*` everywhere, `none`/empty nowhere. Rationale and the transition
# design are in dc/src/dc_audio.c.
#
# THIS BLOCK MUST STAY ABOVE `DC_AUDIO ?= 0`. It uses ?= so that naming scenes
# implies sound, while an explicit DC_AUDIO=0 in the environment still wins
# (make treats an environment variable as already-defined, so ?= is a no-op
# against it) — and so the shrink-tree keying below sees the final value.
#
# ⚠️ A build with sound also needs DC_ARAM_AUDIO_DROP=0, or jaudio synthesises
# from zeroed instrument samples ([NEOS_OUT] peak=0). That used to be a manual
# DC_XDEFS recipe documented in two places and wired in none, and it cost a
# whole session: every DC_AUDIO=1 build before 2026-08-06 streamed all
# 8,300,384 B of audiorom.img into ARAM and threw it away. It is DERIVED now —
# see the block under DC_AUDIO below. Set DC_ARAM_AUDIO_DROP=1 explicitly to
# get the old drop back with sound on (only useful for bisecting the ordering
# hazard described at dc/src/dc_aram.c:295-312).
DC_AUDIO_SCENES ?=
ifneq ($(DC_AUDIO_SCENES),)
DC_AUDIO ?= 1
DEFINES += -DDC_AUDIO_SCENES='"$(DC_AUDIO_SCENES)"'
endif

DC_AUDIO ?= 0
ifeq ($(DC_AUDIO),1)
DEFINES += -DDC_AUDIO=1
# The derivation. `?=` so an explicit DC_ARAM_AUDIO_DROP in the environment
# still wins, and so a silent build is completely unaffected: with DC_AUDIO=0
# nothing here fires and dc_aram.c keeps its own `#define … 1` default.
DC_ARAM_AUDIO_DROP ?= 0
# ⭐ AND THE TWO PEAK-COST LEVERS, DEFAULTED ON WITH SOUND, 2026-08-08. Same
# argument as DC_EMU64_CULL: measured, and therefore the default. Together they
# took [STUTTER] 192 -> 70 and synth_us 2,732 -> 1,202 on matched 900 s town
# runs, and the human verdict on the build was "sounds pretty ok … not
# stuttering as bad" against "stutters like hell" before. Both are `?=`, so
# naming either explicitly still wins, and DC_AUDIO=0 never reaches this block.
# The full justification for each value is at the write site in dc/src/dc_audio.c.
DC_AUDIO_MIXRATE  ?= 24000
DC_AUDIO_SUBDELAY ?= 0
endif

# -----------------------------------------------------------------------------
# THE THREE PEAK-COST LEVERS. Promoted from #ifndef-in-a-TU to first-class knobs
# on 2026-08-08, because a measurement finally priced them.
# -----------------------------------------------------------------------------
# MEASURED, the first run in this project's history with music actually
# playing (the port-queue drain in dc/src/dc_audio.c is what made that
# possible). Cost of one jaudio DAC frame, bucketed by voice-updates summed
# over the frame's four updates — so bucket N means N/4 concurrent voices:
#
#   v>= 0  n=8741  mean=2332us      v>=32  n=1794  mean= 9355us
#   v>= 8  n=16085 mean=3907us      v>=40  n=480   mean=11415us
#   v>=16  n=7985  mean=5701us      v>=48  n=460   mean=13533us
#   v>=24  n=6061  mean=7583us      v>=56  n=303   mean=17285us
#
# ⭐ MONOTONIC. cost ≈ 2,332 us fixed + ~265 us per voice-update. THE BIMODAL
# "~2.5 ms / ~10 ms" the project has been chasing since 2026-08-06 is simply
# SFX-only versus music-playing, and `filt@=0 comb@=0` on every stutter row
# says the conditional FIR/comb stages are not involved. The earlier census
# that "refuted" voice count was taken while the music was silent — an
# instrument pointed at a subsystem that is not running.
#
# DC_AUDIO_VOICES  (L1) NA_SPEC_CONFIG[0]._05, shipped 24. Per-voice work is
#                  exactly linear, so this caps the WORST frame and barely
#                  moves the mean. Degrades to priority-ordered note stealing
#                  via __Nas_GetLowerPrio (channel.c:788-811) — no new drop
#                  logic. It also bounds the Acmd command list: the DC path
#                  writes into pc_task_buf[2][NEOSTHREAD_ACMD_BUF_NUM] with no
#                  check, and jaudio's own bound is 24*20*4+30+400 = 2,350
#                  (memory.c:1029). See S8e in tools/dcstub/make_src_shrink.py,
#                  which grows that buffer to 2,432 so this knob is a
#                  PERFORMANCE choice and not a safety requirement.
# DC_AUDIO_MIXRATE (L2) NA_SPEC_CONFIG[0]._00, shipped 48000. The INTERNAL mix
#                  rate, not the output rate. 24000 halves num_samples_per_update
#                  (200 -> 96, memory.c:974) and therefore the resampler, the
#                  envelope mixer and the reverb bus. PITCH IS PRESERVED — the
#                  error term in the resample ratio is independent of this
#                  field. ⚠️ It does NOT cut ADPCM decode, and it pushes more
#                  notes over the pitch>=2.0 two-part threshold
#                  (channel.c:129), which claws some of it back. Pick a value
#                  whose rate/60/4 is a multiple of 8: 24000 -> 96 ✓.
# DC_AUDIO_SUBDELAY (L5) NA_HALL_DELAY.sub_delay, shipped 0x14. 0 deletes the
#                  hall reverb's SECOND tap: ~16 KB of memcpy and ~1,664 MACs
#                  per DAC frame, for the echo tail only. Every consumer
#                  already has an else branch. ⚠️ NOT the same lever as
#                  NA_SPEC_CONFIG[0]._09 — that one NULL-derefs, and the
#                  argument is at the write site in dc/src/dc_audio.c.
#
# All three default to "leave the shipped value alone", so an unset build is
# byte-identical in behaviour. Forward-only guards for the same reason as every
# other optional knob in this file.
ifneq ($(DC_AUDIO_VOICES),)
DEFINES += -DDC_AUDIO_VOICES=$(DC_AUDIO_VOICES)
endif
ifneq ($(DC_AUDIO_MIXRATE),)
DEFINES += -DDC_AUDIO_MIXRATE=$(DC_AUDIO_MIXRATE)
endif
ifneq ($(DC_AUDIO_SUBDELAY),)
DEFINES += -DDC_AUDIO_SUBDELAY=$(DC_AUDIO_SUBDELAY)
endif

# DC_AUDIO_DISC_FRAMES — how many DAC frames one dc_audio_disc_yield() may
# synthesise while a disc read is blocking (default 4, dc/src/dc_audio.c).
# ⚠️ It is NOT the pump's DC_AUDIO_MAX_FRAMES and must not be confused with it:
# the pump is a per-tick producer keeping level, this is CATCHING UP inside a
# stall and has to be allowed to outrun consumption. Bounded by the ring
# filling up, so raising it cannot overshoot -- it only decides how much of a
# long read gets converted into buffered audio. Costs nothing when no read is
# in flight.
ifneq ($(DC_AUDIO_DISC_FRAMES),)
DEFINES += -DDC_AUDIO_DISC_FRAMES=$(DC_AUDIO_DISC_FRAMES)
endif

# DC_AUDIO_MAX_FRAMES — DAC frames one PUMP may synthesise (default 6). The
# ceiling on audio production is this x 17.49 ms x 2 ticks per presented frame,
# so it is the constant that decides the FRAME RATE below which sound cannot
# keep up at all: 2 gave 70 ms/frame (~14 FPS), 6 gives 210 ms (~4.8 FPS).
# Bounded above by the ring filling, so it cannot overshoot.
ifneq ($(DC_AUDIO_MAX_FRAMES),)
DEFINES += -DDC_AUDIO_MAX_FRAMES=$(DC_AUDIO_MAX_FRAMES)
endif

ifneq ($(DC_ARAM_AUDIO_DROP),)
DEFINES += -DDC_ARAM_AUDIO_DROP=$(DC_ARAM_AUDIO_DROP)
endif

# DC_AUDIO_HEAPLOG=1 — one-shot dump of the jaudio heaps' own bookkeeping
# (dc/src/dc_vi.c). Absent by default. This is the gate on shrinking
# `audiomemory` past make_src_shrink.py's S4: Nas_WaveDmaNew drops voices
# silently on exhaustion, so the heap has to be measured, not argued about.
# DC_AUDIO_HEAPLOG_TICK=<N> moves the sample point (default 600 logic ticks).
# ⚠️ This is the only dc/ TU that pulls in an include/jaudio_NES header; it does
# so only when this flag is set.
ifneq ($(DC_AUDIO_HEAPLOG),)
DEFINES += -DDC_AUDIO_HEAPLOG=$(DC_AUDIO_HEAPLOG)
endif
ifneq ($(DC_AUDIO_HEAPLOG_TICK),)
DEFINES += -DDC_AUDIO_HEAPLOG_TICK=$(DC_AUDIO_HEAPLOG_TICK)
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

# =============================================================================
# P1 — the SH7750 performance counters (dc/src/dc_pmcr.c)
# =============================================================================
# DC_PMCR=1 — bracket the frame with PRFC1 and rotate it through 8 hardware
# events (elapsed cycles, icache/dcache pipeline-freeze cycles, icache/operand
# miss counts, fill cycles, instructions issued). Absent by default; the file
# compiles to nothing without it and every call site is a no-op macro from
# dc/include/dc_pmcr.h, so a shipping image is byte-identical.
#
# ⚠️ THIS IS THE ONLY INSTRUMENT THAT CAN ANSWER "WHY IS HARDWARE SLOWER THAN
# FLYCAST". Flycast models no instruction cache, so it cannot: expect a Flycast
# run to print zeros or nonsense here, and that IS the expected result, not a
# bug. The measurement is a burn.
#
# It uses PRFC1 only. PRFC0 is KOS's own perf_cntr_timer_ns() clock and
# perf_cntr_start(PRFC0, ...) would silently take it over.
ifneq ($(DC_PMCR),)
DEFINES += -DDC_PMCR=$(DC_PMCR)
endif

# DC_PMCR_DWELL=<N> — report windows (30 presented frames each) per event
# before rotating. Default 1: a full 8-event rotation is ~12 s at 20 FPS.
# Raise it when the scene is noisy and each event needs a longer average.
ifneq ($(DC_PMCR_DWELL),)
DEFINES += -DDC_PMCR_DWELL=$(DC_PMCR_DWELL)
endif

# DC_PMCR_HUD=1 — draw the rotating table onto the TV, straight into the
# scanned-out framebuffer, instead of relying on the console.
#
# ⚠️ THIS IS WHAT MAKES A BURN READABLE. DC_SCIF_FAST loses the console on real
# hardware, and at KOS's default 57,600 baud the console is boot time even with
# no cable attached (kb/traps.md) — a burn that reports over SCIF perturbs the
# frame it is reporting on. Needs DC_PMCR; costs a screenful of uncached VRAM
# writes per presented frame, which lands in the instrument's own `vi=` bucket.
ifneq ($(DC_PMCR_HUD),)
DEFINES += -DDC_PMCR_HUD=$(DC_PMCR_HUD)
endif

# DC_CONSOLE_MUTE=1 — dbgio_disable() at boot: no console output at all.
# FOR BURNS. On hardware the SCIF console is charged to the guest as frame
# time whether or not a cable is attached, and a perf build emits ~10 lines
# per 30-frame window — tens of milliseconds of stall inside the very window
# being measured. Pair it with DC_PMCR_HUD=1, which puts the numbers on the
# TV instead. ⚠️ It silences crash dumps too; leave it off for a triage burn.
ifneq ($(DC_CONSOLE_MUTE),)
DEFINES += -DDC_CONSOLE_MUTE=$(DC_CONSOLE_MUTE)
endif

# DC_PMCR_LOG=1 — force the [PMCR] console line back on in a HUD build (an
# emulator run wants both channels). Default: the HUD suppresses it, because
# a burn that logs measures the log. DC_PMCR_LOG=0 suppresses it always.
ifneq ($(DC_PMCR_LOG),)
DEFINES += -DDC_PMCR_LOG=$(DC_PMCR_LOG)
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
#
# $(DECOMP_QUIET) is `-w` in a normal build and EMPTY under WARNSCAN=1. It has
# to be a variable rather than a literal because GCC's -w sets a global
# inhibit that no later -W option can undo, so the scan cannot simply append
# warnings to these lines. -Wno-return-type stays put: $(WARNSCAN_FLAGS) is
# appended to TU_OPT, which the COMPILE rule places AFTER TU_LANG, so the
# scan's -Wreturn-type is the last word and wins.
DECOMP_C_LANG    := $(DECOMP_QUIET) -std=gnu89 -fpermissive -Wno-return-type -Wno-error=return-type
# jaudio_NES / emu64 carry PC-port additions using C99/C11 features.
DECOMP_C11_LANG  := $(DECOMP_QUIET) -std=gnu11 -fpermissive -Wno-return-type -Wno-error=return-type
# §3.3: VERIFIED that no kept C++ TU uses throw/try/catch/typeid/dynamic_cast,
# so -fno-exceptions -fno-rtti (KOS_CPPFLAGS convention) are safe and save
# a meaningful slice of the 16 MB budget.
DECOMP_CXX_LANG  := $(DECOMP_QUIET) -fpermissive -fno-exceptions -fno-rtti

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

# =============================================================================
# sh4zam — vendored SH-4 math library (dc/third_party/sh4zam/VENDORING.md)
# =============================================================================
# MIT, pinned at d4c648f. NOT a kos-ports dependency: taking it from there
# would force a ~27 min SDK image rebuild, and the library needs nothing from
# KOS. Almost all of it is SHZ_INLINE in headers; these four .c files are the
# out-of-line remainder.
#
# It is built with the dc/src flag set (our code, $(DC_OPT), gnu11) rather than
# the decomp set, because it is a library we chose rather than vendored decomp
# — with warnings OFF, since it is upstream code at a pinned commit and a
# warning in it is not actionable here.
# NB $(SHZ_DIR) itself is defined up with the other paths, NOT here: $(INCLUDES)
# is a := assignment far above this line and would expand an empty SHZ_DIR into
# a bare `-I/include`, which is a real directory on some systems and silently
# not this one.
# G-A (kb/research-sh4zam-gap.md §1): the glob below USED TO BE source/*.c only,
# i.e. the four top-level files. Upstream's CMakeLists names SEVEN sources for a
# Dreamcast build, and the three missing ones are all under source/sh4/. Without
# them the library half-links: shz_fft, shz_sq_memcpy32, shz_memcpy128,
# shz_memset8 and the shz_xmtrx_load_apply_store_* forms have no definition, and
# the only reason nothing broke is that nothing in dc/ called them yet.
#
# ⚠️ source/sw/ IS DELIBERATELY NOT GLOBBED, and a recursive wildcard here is a
# BUILD BREAK, not a slowdown: shz_xmtrx_sw.c defines its own xmtrx_state_ that
# conflicts with the SH-4 backend. The old glob excluded it by accident of
# depth; this list excludes it on purpose. Name the three files, do not recurse.
SHZ_C := $(patsubst $(ROOT)/%,%,$(sort $(wildcard $(SHZ_DIR)/source/*.c) \
                                       $(SHZ_DIR)/source/sh4/shz_complex_sh4.c))
DC_C  += $(SHZ_C)

# The two hand-written .s files. Plain gas syntax (`!` comments, no cpp
# directives), so they are assembled, not preprocessed — hence their own rule
# near the bottom of this file rather than a TU_LANG override here.
SHZ_S := $(patsubst $(ROOT)/%,%,$(SHZ_DIR)/source/sh4/shz_xmtrx_sh4.s \
                                $(SHZ_DIR)/source/sh4/shz_mem_sh4.s)

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

SHZ_S_OBJS    := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(SHZ_S)))

OBJS := $(GAME_C_OBJS) $(GAME_CXX_OBJS) $(DC_C_OBJS) $(DC_CXX_OBJS) $(SHZ_S_OBJS)
# The .s objects carry no -MMD (gcc does not run cpp on a lowercase .s), so
# filter their would-be .d out rather than -include a file that never appears.
DEPS := $(filter %.c.d %.cpp.d,$(OBJS:.o=.d))

# =============================================================================
# Per-object flag variables (target-specific; defaults below, overrides after)
# =============================================================================
COMMON  = $(INCLUDES) $(PRELUDE) $(DEFINES) $(GC_CFLAGS)

# Defaults: decomp C at gnu89, unoptimized, full UB guard set.
$(OBJS): TU_CC    = $(KCC)
$(OBJS): TU_XLANG =
$(OBJS): TU_LANG  = $(DECOMP_C_LANG) $(C_ONLY_DEFINES)
$(OBJS): TU_OPT   = $(DECOMP_OPT) $(UB_GUARDS) $(OPT_GUARDS)
$(OBJS): TU_DEFS  =

# =============================================================================
# The two optimization lists (dc/opt-lists.mk) — resolved and CHECKED here
# =============================================================================
# Spelled through both rewriters for the reason the -Dmain= rules below spell
# it out: a per-TU rule is keyed on the OBJECT path, and $(stubify)/$(shrinkify)
# change it. A list entry naming a file that a rewriter emits would otherwise
# land on an object nobody builds, and the TU would silently keep the default
# flags — the exact failure mode that left G1 unrun (kb/RESUME.md session 3).
#
# THE CHECK IS THE POINT. An entry that matches no object in this build is an
# error, not a no-op: these lists are how a hot file gets its -O3 and how a
# miscompiling file gets its -O0, and both fail SILENTLY and PLAUSIBLY when the
# path is stale. (A file the PC filters exclude, or a path typo, both land
# here.)
resolve_tu = $(addprefix $(OBJDIR)/,$(addsuffix .o,$(call shrinkify,$(call stubify,$(1)))))

OPT_HOT_OBJS        := $(call resolve_tu,$(OPT_HOT_SRC))
OPT_QUARANTINE_OBJS := $(call resolve_tu,$(strip $(OPT_QUARANTINE_SRC)))

OPT_HOT_BAD  := $(filter-out $(OBJS),$(OPT_HOT_OBJS))
OPT_QUAR_BAD := $(filter-out $(OBJS),$(OPT_QUARANTINE_OBJS))
ifneq ($(OPT_HOT_BAD),)
  $(error dc/opt-lists.mk: OPT_HOT_SRC names $(words $(OPT_HOT_BAD)) file(s) \
          this build does not compile: $(patsubst $(OBJDIR)/%.o,%,$(OPT_HOT_BAD)))
endif
ifneq ($(OPT_QUAR_BAD),)
  $(error dc/opt-lists.mk: OPT_QUARANTINE_SRC/DC_OPT_O0_EXTRA names \
          $(words $(OPT_QUAR_BAD)) file(s) this build does not compile: \
          $(patsubst $(OBJDIR)/%.o,%,$(OPT_QUAR_BAD)))
endif

# ORDER IS LOAD-BEARING, and it is the same rule the TU_LANG overrides above
# rely on: for one target and one variable, the LAST target-specific assignment
# wins. So hot beats the default, and quarantine beats hot. A file on both
# lists is therefore quarantined, which is the safe direction.
$(OPT_HOT_OBJS):        TU_OPT = $(DECOMP_HOT_OPT) $(UB_GUARDS) $(OPT_GUARDS)
$(OPT_QUARANTINE_OBJS): TU_OPT = -O0 $(UB_GUARDS)

# WARNSCAN=1 forces every decomp TU to -O2 (most of these warnings need the
# optimizer's dataflow to fire at all) and appends the warning set last.
ifeq ($(WARNSCAN),1)
$(OBJS): TU_OPT = -O2 $(UB_GUARDS) $(OPT_GUARDS) $(WARNSCAN_FLAGS)
endif

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

# sh4zam: same flag set as dc/src above, minus the warnings. Upstream code at a
# pinned commit — a warning in it is not actionable here, and letting it into
# the log would bury the ones in dc/src, which we do act on. MUST come after
# the $(DC_C_OBJS) rules above, which also match these objects.
SHZ_OBJS := $(addprefix $(OBJDIR)/,$(addsuffix .o,$(SHZ_C)))
$(SHZ_OBJS): TU_LANG = -std=gnu11 -w

# -DNDEBUG, scoped to sh4zam ONLY — not to dc/src, not to the decomp tree.
# sh4zam's memory and inline-asm routines carry assert()s on alignment and on FP
# mode that are ON by default, and <https://sh4zam.com/tips.html> §"Non-Debug
# Builds" says to build with NDEBUG. Ours would sit inside the vertex loop.
# Scoped rather than global because the decomp tree's own asserts are not ours
# to switch off in the same edit.
$(SHZ_OBJS): TU_DEFS = -DNDEBUG

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
EMU64_TBL_GLOBAL := 1
endif

# =============================================================================
# G2: emu64's dispatch LOOP, at -O2, in dc/ (KILL SWITCH: =0, the default)
# =============================================================================
# The value is a SAMPLING PERIOD in presented frames, like DC_EMU64_HIST:
# 0 = off, 1 = armed on every frame, N > 1 = armed one frame in N. The last
# form is the matched-frame A/B -- armed and unarmed windows from ONE run,
# against the same camera and the same geometry, which is what kb/RESUME.md
# §0b rule 5 asks for.
#
# WHY THIS EXISTS AND WHAT AUTHORISED IT. The town frame is 77-80 % emu64
# traversal plus game logic, all in src/, where editing and compiler flags are
# both banned. dc/src/dc_emu64_shadow.cpp takes over the PER-COMMAND LOOP only
# -- every handler it calls is still the untouched -O0 original. The user
# signed this off explicitly on 2026-08-04 ("Approve G2+G3"), having been told
# it walks near the spirit of the -O0 directive; kb/research-fps-ideas.md's
# decision gate is the write-up.
#
# ⚠️ THE ESTIMATE IN THAT DOC IS TOO HIGH. It says 7-14 ms/frame. The whole of
# emu64_taskstart_r is 0x480 bytes of .text and SH-4 instructions are 2 bytes,
# so the function is 576 instructions END TO END -- prologue, the setjmp block,
# the loop, the epilogue. Executing ALL of it once per command at 2,867
# commands/frame and IPC 1.0 is 8.3 ms, and the loop body is a fraction of
# that. Expect 2-5 ms. Measure, do not quote.
#
# SAME LEGALITY ARGUMENT AS G1 ABOVE, and it costs nothing extra: every
# function the shadow calls -- all 64 handlers -- is already a global text
# symbol in emu64.c.o. The only build change is the SAME objcopy G1 needs, on
# the same object, which is why the two knobs share EMU64_TBL_GLOBAL below.
DC_EMU64_SHADOW_LOOP ?= 0
ifneq ($(DC_EMU64_SHADOW_LOOP),0)
DEFINES += -DDC_EMU64_SHADOW_LOOP=$(DC_EMU64_SHADOW_LOOP)
EMU64_TBL_GLOBAL := 1
endif

# =============================================================================
# G3: the AABB cull at G_TRIN_INDEPEND ENTRY (KILL SWITCH: =0, the default)
# =============================================================================
# The biggest measured lever in the project. G1 (2026-08-06, x2-corrected) put
# G_TRIN_INDEPEND at 34.4 ms of a 45.6 ms town frame, and the [PHASE] line says
# vcull=5250 against v=2899 — 75 % of the vertex references that opcode expands
# are rejected by dc_gx_batch_is_offscreen() one step LATER, after emu64 has
# walked the index stream, after set_position() has transformed every one of
# them, and after our own GX* attribute setters have staged them. G4 priced our
# half of a reference at 0.536 us and emu64's at roughly four times that;
# rejecting the same batch one command earlier is worth 5.4-19.2 ms.
#
# The value is a SAMPLING PERIOD in presented frames, same convention as G1/G2:
# 0 = off, 1 = armed always, N > 1 = armed one frame in N for a matched-frame
# A/B out of a single run (kb/RESUME.md §0b rule 5).
#
# ⚠️ UNLIKE G2, THIS COEXISTS WITH G1 — by discipline, not by luck. G1 and G2
# rewrite ALL 64 dispatch slots per sampled frame from a snapshot taken at their
# own init; G3 writes TWO slots ONCE at init and dc_main.c calls it BEFORE both,
# so their snapshot contains G3's trampolines and their restore puts them back.
# dc_emu64_cull.cpp carries a per-frame tripwire (`reinst=`) for the day someone
# reorders that. Do not add an #error here — add it if a fourth installer
# appears.
#
# The correctness gate is -DDC_EMU64_CULL_VERIFY, which never culls: it runs our
# entry test AND the original handler and counts any batch we would have dropped
# that dc_gx.c's own flush-time cull drew (`falsecull=`, must be 0) plus any
# disagreement about where gfx_p lands (`gfxp_bad=`, must be 0). That is a
# correctness run, never a perf run — it is slower than an uninstrumented build
# by construction.
#
# Same objcopy as G1/G2, same EMU64_TBL_GLOBAL, applied at most once below.
#
# ⭐ DEFAULT 1 SINCE 2026-08-08, AND THAT IS THE POINT. Measured, matched build
# line, audio on, town: draw 69.8 -> 49.9 ms (-19.9, the top of the predicted
# range), fps_p50 19.5 -> 23.2, and the late cull's vcull collapsed 9,915 -> 1,002
# because there is nothing left for it to reject. Gate run first:
# falsecull=0 gfxp_bad=0 reinst=0 over 473 windows through to the town.
# A breakthrough that only exists in someone's command line is one unset
# environment variable away from being lost, so the good build is the DEFAULT
# build and DC_EMU64_CULL=0 is the bisection revert.
DC_EMU64_CULL ?= 1
ifneq ($(DC_EMU64_CULL),0)
DEFINES += -DDC_EMU64_CULL=$(DC_EMU64_CULL)
EMU64_TBL_GLOBAL := 1
endif

# The correctness gate, as a knob rather than a DC_XDEFS incantation — it is
# the thing that has to be RUN before the cull is believed, so it should be as
# easy to spell as the cull itself. Meaningless without DC_EMU64_CULL, and
# dc_emu64_cull.cpp compiles to nothing in that case, so no guard is needed
# here beyond the #error it carries against -DDC_NO_BATCH_CULL.
DC_EMU64_CULL_VERIFY ?= 0
ifneq ($(DC_EMU64_CULL_VERIFY),0)
DEFINES += -DDC_EMU64_CULL_VERIFY=$(DC_EMU64_CULL_VERIFY)
endif

# Promote dl_func_tbl from local to global so dc/ can name it. Shared by G1 and
# G2; it must be applied AT MOST ONCE, because a second TU-local TU_POST
# assignment would silently replace the first rather than append to it.
#
# Spelled through both rewriters, same caveat as the DC_ASSET_CENSUS rule above:
# this was a literal $(OBJDIR)/src/... path, which is the object name emu64.c has
# only while it is in NEITHER scratch tree. LATENT TODAY — emu64.c is in neither
# stub.list nor shrink.list — but the day either rewriter emits it, the TU_POST
# assignment lands on an object nobody builds, objcopy never runs, dl_func_tbl
# stays local, and the failure is a link-time `undefined reference` at best and a
# silently dead interposition at worst.
ifdef EMU64_TBL_GLOBAL
EMU64_TBL_OBJ := $(OBJDIR)/$(call shrinkify,$(call stubify,src/static/libforest/emu64/emu64.c)).o
$(EMU64_TBL_OBJ): TU_POST = sh-elf-objcopy --globalize-symbol=__ZL11dl_func_tbl $@
endif

# DC_AUDIO is listed EXPLICITLY even though DEFINES already carries
# -DDC_AUDIO=1 in the on case. DEFINES alone would still distinguish the two
# builds, but the stamp is also the human-readable record of what a build dir
# holds, and DC_AUDIO now decides which of two DIFFERENT scratch trees the
# objects were compiled against. Spelling it out is what makes a mismatched
# dc/build/shrinksrc diagnosable from the stamp instead of from a link error.
# DC_BGTEX_DEMAND is here for exactly the same reason, and it keys BOTH scratch
# trees at once (shrinksrc's copy loop and stubsrc's map + keep list).
# DC_NPCTEX_POOL keys both trees too (shrinksrc's three TUs and stubsrc's map +
# keep list), so it is spelled out alongside them. DC_NPCMDL_POOL keys the same
# three shrinksrc TUs and its own stubsrc map, for the same reason.
FLAGSTAMP := $(BUILD)/flags.stamp
FLAGSTATE := DC_ASSET_STUB=$(DC_ASSET_STUB) DC_SRC_SHRINK=$(DC_SRC_SHRINK) \
             DC_AUDIO=$(DC_AUDIO) DC_BGTEX_DEMAND=$(DC_BGTEX_DEMAND) \
             DC_NPCTEX_POOL=$(DC_NPCTEX_POOL) \
             DC_NPCMDL_POOL=$(DC_NPCMDL_POOL) \
             DC_TEXPOOL_PROBE=$(DC_TEXPOOL_PROBE) \
             DC_OPT_PROFILE=$(DC_OPT_PROFILE) \
             DECOMP_OPT=$(DECOMP_OPT) DECOMP_HOT_OPT=$(DECOMP_HOT_OPT) \
             OPT_HOT=$(OPT_HOT_SRC) OPT_QUAR=$(strip $(OPT_QUARANTINE_SRC)) \
             DC_OPT=$(DC_OPT) DEFINES=$(DEFINES)

ifneq ($(FLAGSTATE),$(strip $(shell cat $(FLAGSTAMP) 2>/dev/null)))
$(shell mkdir -p $(BUILD))
$(file >$(FLAGSTAMP),$(FLAGSTATE))
endif

# =============================================================================
# Rules
# =============================================================================
.PHONY: all objs clean sources count help warnscan optreport
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

# What flags a build is actually using. One line, cheap, and the thing to paste
# into a state-log entry — the stamp is authoritative but not readable.
optreport:
	@echo "DC_OPT_PROFILE = $(DC_OPT_PROFILE)"
	@echo "DECOMP_OPT     = $(DECOMP_OPT)     ($(words $(OBJS)) TUs, minus the two lists)"
	@echo "DECOMP_HOT_OPT = $(DECOMP_HOT_OPT) ($(words $(OPT_HOT_OBJS)) TUs)"
	@echo "quarantined -O0: $(words $(OPT_QUARANTINE_OBJS)) TUs $(strip $(OPT_QUARANTINE_SRC))"
	@echo "DC_OPT         = $(DC_OPT)     (dc/src, $(words $(DC_C_OBJS) $(DC_CXX_OBJS)) TUs)"
	@echo "UB_GUARDS      = $(UB_GUARDS)"
	@echo "OPT_GUARDS     = $(OPT_GUARDS)"

# See the WARNSCAN block near the top. Recursive because it needs DECOMP_QUIET
# empty at PARSE time, and because its objects must not pollute $(OBJDIR).
# -k so one hard error does not truncate the scan; the log is the artefact and
# a nonzero rc here is expected and ignored.
warnscan:
	@echo "== warnscan: recompiling every TU at -O2 with warnings ON =="
	@mkdir -p $(BUILD)
	-$(MAKE) -C $(DC_DIR) -k -j$(or $(JOBS),4) WARNSCAN=1 \
	    OBJDIR=$(BUILD)/warnobj objs > $(BUILD)/warnscan.log 2>&1
	@rm -rf $(BUILD)/warnobj
	@echo "== warnscan: $(BUILD)/warnscan.log =="
	@grep -c 'warning:' $(BUILD)/warnscan.log || true

TU_POST = true
COMPILE = $(Q)mkdir -p $(@D) && $(TU_CC) $(TU_XLANG) $(COMMON) $(TU_DEFS) $(TU_LANG) $(TU_OPT) \
	  -MMD -MP -MF $(@:.o=.d) -MT $@ -c $< -o $@ && $(TU_POST)

$(OBJDIR)/%.c.o: $(ROOT)/%.c
	@echo "  CC  $*.c"
	$(COMPILE)

$(OBJDIR)/%.cpp.o: $(ROOT)/%.cpp
	@echo "  CXX $*.cpp"
	$(COMPILE)

# sh4zam's two .s files. Lowercase .s means gcc assembles WITHOUT running cpp,
# so $(PRELUDE)/$(DEFINES)/-MMD would all be dead weight or errors here; only
# $(GC_CFLAGS) and the KOS arch flags kos-cc injects matter.
$(OBJDIR)/%.s.o: $(ROOT)/%.s
	@echo "  AS  $*.s"
	$(Q)mkdir -p $(@D) && $(KCC) $(GC_CFLAGS) -c $< -o $@

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
