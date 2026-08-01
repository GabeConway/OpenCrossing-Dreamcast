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

ENVARGS=(
    -e JOBS="${JOBS:-4}"
    -e DC_TARGET="${DC_TARGET:-all}"
    -e DC_CDI_PAD="${DC_CDI_PAD:-0}"
)
# Forward these ONLY if actually set. An empty -e VAR= still counts as "set"
# for make's ?= operator, which would silently blank the Makefile default
# (e.g. DECOMP_OPT would become empty and KOS_CFLAGS' own -O2 would win).
[ -n "${DECOMP_OPT+x}" ] && ENVARGS+=(-e DECOMP_OPT="$DECOMP_OPT")
[ -n "${DC_OPT+x}"     ] && ENVARGS+=(-e DC_OPT="$DC_OPT")
[ -n "${V+x}"          ] && ENVARGS+=(-e V="$V")

exec docker run --rm --platform linux/arm64 \
    -v "$REPO":/work \
    "${ENVARGS[@]}" \
    "$IMAGE" \
    bash /work/dc/build-dc-docker.sh
