#!/usr/bin/env bash
# Build the second-tier memory benchmark guest ELF inside a Dreamcast
# toolchain container.
#
#   ./build.sh                 -> builds bench_mem.{elf,cdi}
#   ./build.sh --image IMG     -> use a different toolchain container image
#   ./build.sh --out DIR       -> output directory
#   ./build.sh --no-cdi        -> ELF only (Flycast boots raw ELFs directly)
#
# Emits JSON on stdout. Exit 0 only if the ELF (and, unless --no-cdi, the CDI)
# built. Then:
#
#   bash harness/dc/smoke.sh ~/.cache/oc-dc-harness/bench/bench_mem.cdi --timeout 180
#
# This one DOES pass/fail meaningfully, unlike a game smoke run: bench_mem
# terminates and prints OC-DC-HARNESS-END, so the exit code is real.
#
# ⚠️ WHAT A FLYCAST RUN OF THIS DOES AND DOES NOT PROVE. Flycast emulates VRAM
# as a byte array; it models neither VRAM access latency nor Holly bus
# contention. So an emulator run proves the benchmark executes, that every
# checksum verifies, and that no transfer path hangs — it does NOT produce the
# SH-4-from-VRAM read bandwidth number that kb/research-ram-tiers.md R1/R3/R4/
# R5/R6 are all gated on. That number needs a CD-R burn, and a hardware build
# must drop BENCH_BAUD to 57600 first (kb/traps.md: a coder's cable will not
# sync at 1.5 Mbps and the console is then lost, crash dumps included).
#
# Everything about the container invocation, the /tmp bind-mount trap and the
# ELF provenance sidecar is documented at length in harness/dc/selftest/build.sh
# — read that file before changing this one. The three rules it encodes:
#   * the work dir must live under $HOME (colima cannot bind-mount /private/tmp)
#   * `bash -c`, never `bash -lc` (kb/traps.md: -l drops sh-elf from PATH)
#   * every CDI gets a <image>.src.json sidecar or crash.sh cannot symbolise it

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="${OC_DC_TOOLCHAIN_IMAGE:-opencrossing-dc:sdk}"
OUT="${OC_DC_HARNESS_HOME:-$HOME/.cache/oc-dc-harness}/bench"
WANT_CDI=1

while [ $# -gt 0 ]; do
    case "$1" in
        --image)  IMAGE="$2"; shift 2 ;;
        --out)    OUT="$2";   shift 2 ;;
        --no-cdi) WANT_CDI=0; shift ;;
        -h|--help) sed -n '2,33p' "$0"; exit 0 ;;
        *) echo "unknown arg: $1" >&2; exit 2 ;;
    esac
done

case "$OUT" in
    /private/tmp/*|/tmp/*)
        echo "refusing to build into $OUT: colima cannot bind-mount /tmp on this host" >&2
        exit 2 ;;
esac

mkdir -p "$OUT" || exit 2
cp "$HERE"/bench_mem.c "$HERE"/Makefile "$OUT"/ || exit 2

LOG="$OUT/build.log"
rm -f "$OUT"/bench_mem.elf "$OUT"/bench_mem.cdi "$OUT"/bench_mem.cdi.src.json

STEP='source /opt/toolchains/dc/kos/environ.sh 2>/dev/null; make clean >/dev/null 2>&1; make'
if [ "$WANT_CDI" -eq 1 ]; then
    STEP="$STEP"'
if command -v mkdcdisc >/dev/null 2>&1; then
    [ -f bench_mem.elf ] && mkdcdisc -q -N -e bench_mem.elf -o bench_mem.cdi
else
    echo "WARNING: no mkdcdisc in this image; ELF-only build" >&2
fi'
fi

docker run --rm --platform linux/arm64 -v "$OUT":/src -w /src "$IMAGE" bash -c "$STEP" \
    >"$LOG" 2>&1
rc=$?

elfs=(); cdis=(); sidecars=()
if [ -f "$OUT/bench_mem.elf" ]; then
    elfs+=("\"$OUT/bench_mem.elf\"")
    if [ -f "$OUT/bench_mem.cdi" ]; then
        cdis+=("\"$OUT/bench_mem.cdi\"")
        sha="$(shasum -a 256 "$OUT/bench_mem.elf" | awk '{print $1}')"
        size="$(wc -c < "$OUT/bench_mem.elf" | tr -d ' ')"
        cat > "$OUT/bench_mem.cdi.src.json" <<EOF
{
  "image": "$OUT/bench_mem.cdi",
  "elf": "$OUT/bench_mem.elf",
  "elf_sha256": "$sha",
  "elf_size": $size,
  "built_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "toolchain_image": "$IMAGE",
  "producer": "harness/dc/bench/build.sh"
}
EOF
        sidecars+=("\"$OUT/bench_mem.cdi.src.json\"")
    fi
fi

ok=false
if [ "${#elfs[@]}" -eq 1 ] && { [ "$WANT_CDI" -eq 0 ] || [ "${#cdis[@]}" -eq 1 ]; }; then
    ok=true
fi

printf '{\n'
printf '  "harness": "bench-build",\n'
printf '  "image": "%s",\n' "$IMAGE"
printf '  "out_dir": "%s",\n' "$OUT"
printf '  "build_log": "%s",\n' "$LOG"
printf '  "make_rc": %d,\n' "$rc"
printf '  "elfs": [%s],\n' "$(IFS=,; echo "${elfs[*]:-}")"
printf '  "cdis": [%s],\n' "$(IFS=,; echo "${cdis[*]:-}")"
printf '  "sidecars": [%s],\n' "$(IFS=,; echo "${sidecars[*]:-}")"
printf '  "ok": %s\n' "$ok"
printf '}\n'

[ "$ok" = true ] || exit 1
exit 0
