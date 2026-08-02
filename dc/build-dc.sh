#!/usr/bin/env bash
# =============================================================================
# build-dc.sh — HOST-side wrapper. Runs dc/build-dc-docker.sh in the SDK image.
# =============================================================================
# Usage:
#     bash dc/build-dc.sh                 # full build -> ELF + unpadded CDI
#     DC_TARGET=objs bash dc/build-dc.sh  # compile only, no link (M1 signal)
#     DC_SRC_SHRINK=0 bash dc/build-dc.sh # kill switch for the .bss shrink pass
#                                         # (default 1; 0 is a byte-identical revert)
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
if [ "${DC_ASSET_STUB:-0}" = "1" ]; then
    echo "-- DC_ASSET_STUB=1: regenerating $REPO/dc/build/stubsrc"
    python3 "$REPO/tools/dcstub/make_stub_data.py"
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
if [ "${DC_SRC_SHRINK:-1}" = "1" ]; then
    echo "-- DC_SRC_SHRINK=1: regenerating $REPO/dc/build/shrinksrc"
    python3 "$REPO/tools/dcstub/make_src_shrink.py"
fi

ENVARGS=(
    -e JOBS="${JOBS:-4}"
    -e DC_TARGET="${DC_TARGET:-all}"
    -e DC_CDI_PAD="${DC_CDI_PAD:-0}"
    -e DC_ASSET_STUB="${DC_ASSET_STUB:-0}"
    -e DC_SRC_SHRINK="${DC_SRC_SHRINK:-1}"
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
    -e DC_PVR_BATCH_LOG="${DC_PVR_BATCH_LOG:-}"
    -e DC_XDEFS="${DC_XDEFS:-}"
)
# Forward these ONLY if actually set. An empty -e VAR= still counts as "set"
# for make's ?= operator, which would silently blank the Makefile default
# (e.g. DECOMP_OPT would become empty and KOS_CFLAGS' own -O2 would win).
[ -n "${DECOMP_OPT+x}" ] && ENVARGS+=(-e DECOMP_OPT="$DECOMP_OPT")
[ -n "${DC_OPT+x}"     ] && ENVARGS+=(-e DC_OPT="$DC_OPT")
[ -n "${V+x}"          ] && ENVARGS+=(-e V="$V")

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
