#!/usr/bin/env bash
# Build the OpenCrossing-Dreamcast SDK image (sh-elf + KallistiOS + GLdc +
# mkdcdisc).  Host-side wrapper; run from anywhere.
#
#   dc/build-dc-image.sh            # build if missing
#   dc/build-dc-image.sh --force    # rebuild even if the image exists
#
# Cold build is ~27 min (stage 1 = 24 min toolchain, stage 2 = 2.5 min SDK).
# Stage 1 is keyed on KOS_SHA + TOOLCHAIN_PROFILE and is cached forever.
# See kb/design-toolchain.md §5-§6.
set -euo pipefail

REPO_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${DC_SDK_IMAGE:-opencrossing-dc:sdk}"
JOBS="${DC_BUILD_JOBS:-4}"
FORCE=0
[[ "${1:-}" == "--force" ]] && FORCE=1

if [[ $FORCE -eq 0 ]] && docker image inspect "$IMAGE" >/dev/null 2>&1; then
    echo "[build-dc-image] $IMAGE already present; nothing to do (--force to rebuild)."
    exit 0
fi

echo "[build-dc-image] building $IMAGE (JOBS=$JOBS) — this takes ~27 min cold."

# This host has no BuildKit: use the legacy builder and never pass --progress.
DOCKER_BUILDKIT=0 docker build --platform linux/arm64 \
    --build-arg "JOBS=$JOBS" \
    -t "$IMAGE" \
    -f "$REPO_ROOT/dc/Dockerfile" \
    "$REPO_ROOT/dc"

echo "[build-dc-image] done: $IMAGE"
