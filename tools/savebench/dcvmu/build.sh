#!/usr/bin/env bash
# Build the dcvmu guest program: dc/src/dc_card.c's VMU backend, on its own,
# bootable in Flycast.
#
#   bash tools/savebench/dcvmu/build.sh
#   bash harness/dc/smoke.sh ~/.cache/oc-dc-harness/dcvmu/dcvmu.cdi --timeout 90
#
# WHY: the game image is 21.4 MB against a 16 MB machine (kb/STATE.md) and
# prints nothing at all, so the save path cannot be observed from a game build.
# This one is ~40 KB and boots.
#
# Two host constraints, both already paid for by harness/dc/selftest/build.sh —
# do not "simplify" either away:
#   * colima bind-mounts of /tmp are silently EMPTY inside containers on this
#     host, so the work dir must live under $HOME.
#   * inside the SDK image use `bash -c`, never `bash -lc`: -l re-runs
#     /etc/profile and drops sh-elf/bin from PATH (kb/traps.md).
# And every CDI this project produces gets a <image>.src.json sidecar or
# harness/dc/crash.sh cannot symbolise a fault in it.
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/../../.." && pwd)"
IMAGE="${OC_DC_TOOLCHAIN_IMAGE:-opencrossing-dc:sdk}"
OUT="${OC_DC_HARNESS_HOME:-$HOME/.cache/oc-dc-harness}/dcvmu"
EXTRA_DEFS="${DCVMU_DEFS:-}"

case "$OUT" in
    /private/tmp/*|/tmp/*)
        echo "refusing to build into $OUT: colima cannot bind-mount /tmp here" >&2
        exit 2 ;;
esac

mkdir -p "$OUT" || exit 2
cp "$HERE/main.c" "$HERE/Makefile" "$OUT/" || exit 2
cp "$ROOT/dc/src/dc_card.c" "$ROOT/dc/include/dc_card.h" "$OUT/" || exit 2

LOG="$OUT/build.log"
rm -f "$OUT/dcvmu.elf" "$OUT/dcvmu.cdi" "$OUT/dcvmu.cdi.src.json"

STEP='source /opt/toolchains/dc/kos/environ.sh 2>/dev/null
make clean >/dev/null 2>&1
make EXTRA_DEFS="'"$EXTRA_DEFS"'" || exit 1
mkdcdisc -q -N -e dcvmu.elf -o dcvmu.cdi'

docker run --rm --platform linux/arm64 -v "$OUT":/src -w /src "$IMAGE" \
    bash -c "$STEP" >"$LOG" 2>&1
rc=$?

if [ ! -f "$OUT/dcvmu.cdi" ]; then
    echo "build FAILED (rc=$rc); see $LOG" >&2
    tail -40 "$LOG" >&2
    exit 1
fi

sha="$(shasum -a 256 "$OUT/dcvmu.elf" | awk '{print $1}')"
size="$(wc -c < "$OUT/dcvmu.elf" | tr -d ' ')"
cat > "$OUT/dcvmu.cdi.src.json" <<EOF
{
  "image": "$OUT/dcvmu.cdi",
  "elf": "$OUT/dcvmu.elf",
  "elf_sha256": "$sha",
  "elf_size": $size,
  "built_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "toolchain_image": "$IMAGE",
  "producer": "tools/savebench/dcvmu/build.sh"
}
EOF

echo "$OUT/dcvmu.cdi"
exit 0
