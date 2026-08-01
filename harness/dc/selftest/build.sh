#!/usr/bin/env bash
# Build the harness self-test guest ELFs inside a Dreamcast toolchain container.
#
# These are the programs used to prove the harness itself works. They are NOT
# part of the game build. Output goes OUTSIDE the repo (no ELFs are committed).
#
#   ./build.sh                 -> builds selftest.elf and crashtest.elf
#   ./build.sh --image IMG     -> use a different toolchain container image
#   ./build.sh --out DIR       -> output directory
#
# Emits JSON on stdout. Exit 0 only if every ELF built.
#
# IMPORTANT (verified 2026-08-01): colima bind-mounts of /private/tmp/... are
# silently EMPTY inside containers on this host. Every Docker mount used here
# must live under $HOME. Do not "simplify" this back to a /tmp workdir.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="${OC_DC_TOOLCHAIN_IMAGE:-einsteinx2/dcdev-kos-toolchain:latest}"
OUT="${OC_DC_HARNESS_HOME:-$HOME/.cache/oc-dc-harness}/selftest"

while [ $# -gt 0 ]; do
    case "$1" in
        --image) IMAGE="$2"; shift 2 ;;
        --out)   OUT="$2";   shift 2 ;;
        -h|--help) sed -n '2,20p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

case "$OUT" in
    /private/tmp/*|/tmp/*)
        echo "refusing to build into $OUT: colima cannot bind-mount /tmp on this host" >&2
        exit 2 ;;
esac

mkdir -p "$OUT" || exit 2
cp "$HERE"/selftest.c "$HERE"/crashtest.c "$HERE"/Makefile "$OUT"/ || exit 2

LOG="$OUT/build.log"
docker run --rm -v "$OUT":/src -w /src "$IMAGE" bash -lc \
    'source /opt/toolchains/dc/kos/environ.sh && make clean >/dev/null 2>&1; make' \
    >"$LOG" 2>&1
rc=$?

built=()
for f in selftest.elf crashtest.elf; do
    [ -f "$OUT/$f" ] && built+=("\"$OUT/$f\"")
done

n=${#built[@]}
printf '{\n'
printf '  "harness": "selftest-build",\n'
printf '  "image": "%s",\n' "$IMAGE"
printf '  "out_dir": "%s",\n' "$OUT"
printf '  "build_log": "%s",\n' "$LOG"
printf '  "make_rc": %d,\n' "$rc"
printf '  "elfs": [%s],\n' "$(IFS=,; echo "${built[*]:-}")"
printf '  "ok": %s\n' "$([ "$n" -eq 2 ] && echo true || echo false)"
printf '}\n'

[ "$n" -eq 2 ] || exit 1
exit 0
