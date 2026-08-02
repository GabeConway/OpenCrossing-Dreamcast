#!/usr/bin/env bash
# =============================================================================
# build-dc.sh — HOST-side wrapper. Runs dc/build-dc-docker.sh in the SDK image.
# =============================================================================
# Usage:
#     bash dc/build-dc.sh                 # full build -> ELF + unpadded CDI
#     DC_TARGET=objs bash dc/build-dc.sh  # compile only, no link (M1 signal)
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

ENVARGS=(
    -e JOBS="${JOBS:-4}"
    -e DC_TARGET="${DC_TARGET:-all}"
    -e DC_CDI_PAD="${DC_CDI_PAD:-0}"
    -e DC_ASSET_STUB="${DC_ASSET_STUB:-0}"
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

exec docker run --rm --platform linux/arm64 \
    -v "$REPO":/work \
    "${MOUNTARGS[@]}" \
    "${ENVARGS[@]}" \
    "$IMAGE" \
    bash /work/dc/build-dc-docker.sh
