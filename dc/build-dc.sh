#!/usr/bin/env bash
# =============================================================================
# build-dc.sh — HOST-side wrapper. Runs dc/build-dc-docker.sh in the SDK image.
# =============================================================================
# Usage:
#     bash dc/build-dc.sh                 # full build -> ELF + unpadded CDI
#     DC_TARGET=objs bash dc/build-dc.sh  # compile only, no link (M1 signal)
#     DC_SRC_SHRINK=0 bash dc/build-dc.sh # kill switch for the .bss shrink pass
#                                         # (default 1; 0 is a byte-identical revert)
#     DC_AUDIO=1    bash dc/build-dc.sh   # sound ON (default 0; costs ~45 % FPS,
#                                         # and gives back 401,216 B of .bss —
#                                         # see the DC_AUDIO block in dc/Makefile)
#     DC_BGTEX_DEMAND=0 bash dc/build-dc.sh
#                                         # kill switch for R1 — puts the acre
#                                         # ground textures back in .bss
#                                         # (+80,736 B) instead of reading them
#                                         # off the disc. Default 1.
#     DC_NPCTEX_POOL=0 bash dc/build-dc.sh
#                                         # kill switch for R2 — puts the 21
#                                         # censused villager texture sets back
#                                         # in .bss (+90,464 B) and leaves the
#                                         # other 215 species untextured, as
#                                         # before R2. Default 1.
#     DC_NPCMDL_POOL=0 bash dc/build-dc.sh
#                                         # kill switch for R3 — gives back the
#                                         # 16-slot villager MODEL pool
#                                         # (-115,296 B of .bss) and leaves 31
#                                         # species drawing as a black spiky
#                                         # mess, as before R3. Default 1.
#                                         # DC_NPCMDL_SLOTS=<N> cuts the pool
#                                         # without turning it off (7,552 B each).
#     DC_TEXPOOL_PROBE=1 DC_ASSET_STUB=1 bash dc/build-dc.sh
#                                         # T1's falsification probe. Counts
#                                         # only — no bytes move, no pixel
#                                         # changes. Needs DC_ASSET_STUB=1.
#                                         # Read the result with:
#                                         #   grep '\[DC/TEXPOOL\]' console.log
#                                         # Default 0.
#     DC_CDI_PAD=1  bash dc/build-dc.sh   # padded 740 MB CDI for CD-R burns
#     JOBS=8        bash dc/build-dc.sh
#     bash dc/build-dc.sh clean           # rm -rf dc/build
#
# The image opencrossing-dc:sdk is built once by dc/build-dc-image.sh; this
# script never rebuilds it (24 min for stage 1 — see kb/design-toolchain.md §6).
#
# --platform linux/arm64 is explicit on purpose: without it an accidental amd64
# pull would silently drop the whole build into qemu, which is slow AND flaky
# (kb/design-toolchain.md §2).
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${DC_SDK_IMAGE:-opencrossing-dc:sdk}"

if ! docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "ERROR: docker image '$IMAGE' not found." >&2
    echo "       Build it once with: bash $REPO/dc/build-dc-image.sh" >&2
    exit 2
fi

# `clean` and any other make target can be passed straight through.
if [ "${1:-}" = "clean" ]; then
    exec docker run --rm --platform linux/arm64 \
        -v "$REPO":/work "$IMAGE" \
        bash -c 'make -C /work/dc clean'
fi

# DC_ASSET_STUB=1 -> the S1 bring-up image (kb/STATE.md). The rewritten TUs are
# generated HOST-side because python3 is not part of the SDK image's contract;
# the container only ever sees the resulting dc/build/stubsrc tree through the
# bind mount. The generator is idempotent and only rewrites files whose content
# actually changes, so re-running it does not invalidate objects.
#
# --bgtex-demand is R1's kill switch and it keys BOTH scratch trees: this tool
# emits dc_bgtex_map.inc (or, at 0, puts the 27 mFM_grd_*.c files back on the
# keep list), and make_src_shrink.py below rewrites the copy loop that reads it.
# It is expanded HERE rather than left to the container so that one value drives
# both generators and dc/Makefile's -DDC_BGTEX_DEMAND; the generated
# m_field_make.c #errors if they disagree, but only after a full compile.
#
# --npctex-pool is R2's kill switch and works exactly the same way: this tool
# emits dc_npctex_map.inc (or, at 0, puts the 21 censused villager tex files
# back on the keep list), and make_src_shrink.py below inserts the load and
# reset calls that use it.
#
# --npcmdl-pool is R3's, same shape again: dc_npcmdl_map.inc (or, at 0, cbr_1
# back on the keep list) here, the load and reset calls there. ⚠️ Unlike R1 and
# R2 this one COSTS ~115,296 B of .bss; it buys 31 villager species the geometry
# they never had. See the DC_NPCMDL_POOL block in dc/Makefile.
if [ "${DC_ASSET_STUB:-0}" = "1" ]; then
    echo "-- DC_ASSET_STUB=1: regenerating $REPO/dc/build/stubsrc" \
         "(DC_BGTEX_DEMAND=${DC_BGTEX_DEMAND:-1}" \
         "DC_NPCTEX_POOL=${DC_NPCTEX_POOL:-0}" \
         "DC_NPCMDL_POOL=${DC_NPCMDL_POOL:-0}" \
         "DC_TEXPOOL_PROBE=${DC_TEXPOOL_PROBE:-0})"
    python3 "$REPO/tools/dcstub/make_stub_data.py" \
        --bgtex-demand="${DC_BGTEX_DEMAND:-1}" \
        --npctex-pool="${DC_NPCTEX_POOL:-0}" \
        --npcmdl-pool="${DC_NPCMDL_POOL:-0}" \
        --texpool="${DC_TEXPOOL_PROBE:-0}"
fi

# DC_SRC_SHRINK=1 (the DEFAULT) -> the .bss literal-shrink tree, 1,159,392 B of
# .bss (kb/levers.md L3, dc/Makefile's DC_SRC_SHRINK block). Same host-side
# story as the stub tree above: python3 is not part of the SDK image's
# contract, so the tree is generated here and the container only sees it
# through the bind mount. The generator is idempotent and content-compared.
#
# It hard-errors if any of its anchored rules stops matching the vendored
# source. That is deliberate — a silently-not-applied rewrite would produce a
# build that looks correct and saves nothing. Do not paper over it here.
#
# --audio is NOT optional and NOT a default: rules S8a-d shrink jaudio pools
# that are only dead while DC_AUDIO=0, so the tree and the build must agree.
# Passing it here is what keeps them in step, because the tree is generated
# before the container starts and outlives any single `make`. The generated TUs
# also carry an #error for the mismatch — this line is the belt, that is the
# braces, and both are wanted: the #error only fires on a tree someone built by
# hand or left behind after editing dc/Makefile's default.
# DC_AUDIO_SCENES implies sound, and the SHELL has to derive that, not just
# dc/Makefile. Both the shrink tool below and the -e forward further down
# expand ${DC_AUDIO:-0}, so leaving it unset here would push DC_AUDIO=0 into the
# container -- and make treats an environment variable as already-defined, so
# the Makefile's `DC_AUDIO ?= 1` inside its DC_AUDIO_SCENES block would be a
# no-op. `DC_AUDIO_SCENES=3 bash dc/build-dc.sh` would then build a silent
# image AND an S8-shrunk jaudio tree that disagrees with it.
if [ -n "${DC_AUDIO_SCENES:-}" ] && [ -z "${DC_AUDIO+x}" ]; then
    DC_AUDIO=1
    export DC_AUDIO
    echo "-- DC_AUDIO_SCENES='${DC_AUDIO_SCENES}' implies DC_AUDIO=1"
fi

if [ "${DC_SRC_SHRINK:-1}" = "1" ]; then
    echo "-- DC_SRC_SHRINK=1: regenerating $REPO/dc/build/shrinksrc" \
         "(DC_AUDIO=${DC_AUDIO:-0} DC_BGTEX_DEMAND=${DC_BGTEX_DEMAND:-1}" \
         "DC_NPCTEX_POOL=${DC_NPCTEX_POOL:-0}" \
         "DC_NPCMDL_POOL=${DC_NPCMDL_POOL:-0})"
    python3 "$REPO/tools/dcstub/make_src_shrink.py" --audio="${DC_AUDIO:-0}" \
        --bgtex-demand="${DC_BGTEX_DEMAND:-1}" \
        --npctex-pool="${DC_NPCTEX_POOL:-0}" \
        --npcmdl-pool="${DC_NPCMDL_POOL:-0}"
fi

ENVARGS=(
    -e JOBS="${JOBS:-4}"
    -e DC_TARGET="${DC_TARGET:-all}"
    -e DC_CDI_PAD="${DC_CDI_PAD:-0}"
    -e DC_ASSET_STUB="${DC_ASSET_STUB:-0}"
    -e DC_SRC_SHRINK="${DC_SRC_SHRINK:-1}"
    # Explicit :-0 rather than the forward-only form below: DC_AUDIO has a real
    # default on both sides and they must be the same one, so the make ?= must
    # never be the thing that decides it.
    -e DC_AUDIO="${DC_AUDIO:-0}"
    # Forwarded ONLY when set, unlike DC_AUDIO above: dc/Makefile derives
    # DC_AUDIO from DC_AUDIO_SCENES with ?=, and an empty -e would make the
    # variable "defined" inside the container and defeat that.
    -e DC_DIAG="${DC_DIAG:-0}"
    -e DC_ARENA_BYTES="${DC_ARENA_BYTES:-}"
    -e DC_ARAM_WINDOW="${DC_ARAM_WINDOW:-}"
    -e DC_FB_PROBE="${DC_FB_PROBE:-}"
    -e DC_FB_IMAGE="${DC_FB_IMAGE:-}"
    -e DC_ARENA_PROBE="${DC_ARENA_PROBE:-}"
    -e DC_ASSET_CENSUS="${DC_ASSET_CENSUS:-}"
    -e DC_ARAM_LRU="${DC_ARAM_LRU:-}"
    -e DC_ARAM_TRACE="${DC_ARAM_TRACE:-}"
    -e DC_AUTOSTART="${DC_AUTOSTART:-}"
    -e DC_AUTOSTART_PERIOD="${DC_AUTOSTART_PERIOD:-}"
    -e DC_CONSOLE_LIMIT="${DC_CONSOLE_LIMIT:-}"
    -e DC_TEX_LOG="${DC_TEX_LOG:-}"
    -e DC_AUDIO_HEAPLOG="${DC_AUDIO_HEAPLOG:-}"
    -e DC_AUDIO_HEAPLOG_TICK="${DC_AUDIO_HEAPLOG_TICK:-}"
    -e DC_PVR_BATCH_LOG="${DC_PVR_BATCH_LOG:-}"
    -e DC_SCIF_FAST="${DC_SCIF_FAST:-}"
    -e DC_XDEFS="${DC_XDEFS:-}"
)
[ -n "${DC_AUDIO_SCENES+x}" ] && ENVARGS+=(-e DC_AUDIO_SCENES="$DC_AUDIO_SCENES")
# R1's kill switch, forward-only: dc/Makefile has `DC_BGTEX_DEMAND ?= 1`, and
# make treats an environment variable as already-defined — so a plain
# `-e DC_BGTEX_DEMAND=` would blank the default and expand
# -DDC_BGTEX_DEMAND= into every TU, where the rewritten m_field_make.c's
# `#if defined(DC_BGTEX_DEMAND) && !DC_BGTEX_DEMAND` is a preprocessor error
# rather than a wrong build. The two generators above already saw the value.
[ -n "${DC_BGTEX_DEMAND+x}" ] && ENVARGS+=(-e DC_BGTEX_DEMAND="$DC_BGTEX_DEMAND")
# R2's kill switch, forward-only for exactly the same reason: dc/Makefile has
# `DC_NPCTEX_POOL ?= 0`, and a plain `-e DC_NPCTEX_POOL=` would blank it and
# expand -DDC_NPCTEX_POOL= into every TU, where the rewritten TUs'
# `#if defined(DC_NPCTEX_POOL) && !DC_NPCTEX_POOL` is a preprocessor error.
# The two generators above already saw the value.
[ -n "${DC_NPCTEX_POOL+x}" ] && ENVARGS+=(-e DC_NPCTEX_POOL="$DC_NPCTEX_POOL")
# The pool's slot count. dc/Makefile guards it with `ifneq (…,)` so an empty
# value would be harmless here, but it is forwarded the same way as every other
# optional knob so the ENVARGS list has one rule rather than two.
[ -n "${DC_NPCTEX_SLOTS+x}" ] && ENVARGS+=(-e DC_NPCTEX_SLOTS="$DC_NPCTEX_SLOTS")
# R3's kill switch, forward-only for exactly the same reason: dc/Makefile has
# `DC_NPCMDL_POOL ?= 0`, and a plain `-e DC_NPCMDL_POOL=` would blank it and
# expand -DDC_NPCMDL_POOL= into every TU, where the rewritten TUs'
# `#if defined(DC_NPCMDL_POOL) && !DC_NPCMDL_POOL` is a preprocessor error.
# The two generators above already saw the value.
[ -n "${DC_NPCMDL_POOL+x}" ] && ENVARGS+=(-e DC_NPCMDL_POOL="$DC_NPCMDL_POOL")
# The model pool's slot count, same forwarding rule as DC_NPCTEX_SLOTS above.
# This is the knob to reach for first if R3's 120,832 B does not fit.
[ -n "${DC_NPCMDL_SLOTS+x}" ] && ENVARGS+=(-e DC_NPCMDL_SLOTS="$DC_NPCMDL_SLOTS")
# G1, and it must be the FORWARD-ONLY form. Omitting it entirely is how the
# histogram came to be "in the tree, never run": dc/Makefile has
# DC_EMU64_HIST ?= 0, so from the HOST entry point a DC_EMU64_HIST=300 build
# compiled the instrument out AND skipped the objcopy that globalises the
# dispatch table, silently -- the run reached the town and simply printed no
# [EMU64H] line, which reads as "the instrument failed", not "it was never
# built". But a plain -e DC_EMU64_HIST= would be worse than the bug: the guard
# is `ifneq ($(DC_EMU64_HIST),0)`, and empty is not 0, so an unset variable
# would turn the instrument ON for every build.
[ -n "${DC_EMU64_HIST+x}" ] && ENVARGS+=(-e DC_EMU64_HIST="$DC_EMU64_HIST")
# T1's probe, and it MUST be the forward-only form for BOTH of the reasons
# DC_EMU64_HIST documents above. Its dc/Makefile guard is
# `ifneq ($(DC_TEXPOOL_PROBE),0)` -- empty is not 0 -- so a plain
# `-e DC_TEXPOOL_PROBE=` would arm the probe on EVERY build and then $(error)
# out of any build that is not DC_ASSET_STUB=1. Omitting the line entirely would
# be the G1 bug verbatim: dc/Makefile has DC_TEXPOOL_PROBE ?= 0, so from this
# HOST entry point a DC_TEXPOOL_PROBE=1 build would compile the instrument out
# while make_stub_data.py above still emitted the map, and the run would reach
# the town and simply print no [DC/TEXPOOL] line -- which reads as "the probe
# found nothing", not "it was never built". That is what left G1 unrun for two
# sessions (kb/traps.md).
[ -n "${DC_TEXPOOL_PROBE+x}" ] && \
    ENVARGS+=(-e DC_TEXPOOL_PROBE="$DC_TEXPOOL_PROBE")
# G2, forward-only for the same reason as DC_EMU64_HIST above -- its Makefile
# guard is `ifneq (...,0)` too, so an empty -e would arm it on every build.
[ -n "${DC_EMU64_SHADOW_LOOP+x}" ] && \
    ENVARGS+=(-e DC_EMU64_SHADOW_LOOP="$DC_EMU64_SHADOW_LOOP")
# Forward these ONLY if actually set. An empty -e VAR= still counts as "set"
# for make's ?= operator, which would silently blank the Makefile default
# (e.g. DECOMP_OPT would become empty and KOS_CFLAGS' own -O2 would win).
[ -n "${DECOMP_OPT+x}" ] && ENVARGS+=(-e DECOMP_OPT="$DECOMP_OPT")
[ -n "${DC_OPT+x}"     ] && ENVARGS+=(-e DC_OPT="$DC_OPT")
[ -n "${V+x}"          ] && ENVARGS+=(-e V="$V")
# The optimization profile (dc/Makefile's DC_OPT_PROFILE block, dc/opt-lists.mk).
# Forward-only for the same reason as everything else here: dc/Makefile picks
# DECOMP_OPT/DECOMP_HOT_OPT from the profile with ?=, and an empty -e would make
# the variable "defined" in the container, blanking the default so $KOS_CFLAGS'
# own -O2 wins silently.
#
#   DC_OPT_PROFILE=o0 bash dc/build-dc.sh      # the kill switch, byte-identical
#                                              # to the pre-2026-08-06 build
#   DC_OPT_PROFILE=size bash dc/build-dc.sh    # -Os even on the hot list
#   DC_OPT_O0_EXTRA='src/a.c src/b.c' ...      # quarantine while bisecting
[ -n "${DC_OPT_PROFILE+x}" ] && ENVARGS+=(-e DC_OPT_PROFILE="$DC_OPT_PROFILE")
[ -n "${DECOMP_HOT_OPT+x}" ] && ENVARGS+=(-e DECOMP_HOT_OPT="$DECOMP_HOT_OPT")
[ -n "${DC_OPT_O0_EXTRA+x}" ] && ENVARGS+=(-e DC_OPT_O0_EXTRA="$DC_OPT_O0_EXTRA")
# The uninitialised-local diagnostic. DC_AUTOVAR_INIT=zero when an optimized
# image misbehaves; if the symptom goes away the bug is an uninitialised read.
[ -n "${DC_AUTOVAR_INIT+x}" ] && ENVARGS+=(-e DC_AUTOVAR_INIT="$DC_AUTOVAR_INIT")

# DC_DISC_ROOT=<dir> puts real game data on the disc. The directory is mounted
# read-only at /discroot and handed to mkdcdisc with -d, so its contents land at
# the DISC ROOT — which is what dc_dvd.c expects, since it opens "/cd/<name>"
# with no subdirectory. Build one with:
#
#   python3 tools/dcasset/dcasset.py extract "<the ISO>" --out /tmp/discroot
#   bash dc/stage-disc.sh /tmp/discroot /tmp/discflat
#   DC_DISC_ROOT=/tmp/discflat DC_ASSET_STUB=1 bash dc/build-dc.sh
#
# Never commit the result: built disc images and ROM material stay out of the
# repo (CLAUDE.md §1).
MOUNTARGS=()
if [ -n "${DC_DISC_ROOT:-}" ]; then
    if [ ! -d "$DC_DISC_ROOT" ]; then
        echo "ERROR: DC_DISC_ROOT='$DC_DISC_ROOT' is not a directory." >&2
        exit 2
    fi
    MOUNTARGS+=(-v "$(cd "$DC_DISC_ROOT" && pwd)":/discroot:ro)
fi

# "${MOUNTARGS[@]}" on its own is an UNBOUND VARIABLE under `set -u` in bash 3.2
# (the macOS system bash) when the array is empty — which is every build that
# does not set DC_DISC_ROOT. The +"${...}" form expands to nothing instead.
exec docker run --rm --platform linux/arm64 \
    -v "$REPO":/work \
    ${MOUNTARGS[@]+"${MOUNTARGS[@]}"} \
    "${ENVARGS[@]}" \
    "$IMAGE" \
    bash /work/dc/build-dc-docker.sh
