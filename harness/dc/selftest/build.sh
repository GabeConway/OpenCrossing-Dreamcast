#!/usr/bin/env bash
# Build the harness self-test guest ELFs inside a Dreamcast toolchain container.
#
# These are the programs used to prove the harness itself works. They are NOT
# part of the game build. Output goes OUTSIDE the repo (no ELFs are committed).
#
#   ./build.sh                 -> builds selftest.{elf,cdi} and crashtest.{elf,cdi}
#   ./build.sh --image IMG     -> use a different toolchain container image
#   ./build.sh --out DIR       -> output directory
#   ./build.sh --no-cdi        -> ELFs only (Flycast boots raw ELFs directly)
#
# Emits JSON on stdout. Exit 0 only if every ELF built.
#
# CDIs are always built with mkdcdisc -N (--no-padding). Without -N every image
# is 740 MB and takes 15.6 s; with it, 1.8 MB and 21 ms (design-toolchain.md
# §5.2). Padding only matters for real-media seek layout, never for emulators.
#
# ---------------------------------------------------------------------------
# ELF PROVENANCE SIDECAR -- read this before writing any other CDI producer.
#
# A CDI carries a scrambled 1ST_READ.BIN with no symbols, so a crash inside it
# is just hex. crash.sh can only symbolise it if it can find the ELF the image
# was built from AND prove that ELF has not been rebuilt since. So every CDI
# this project produces MUST get a sidecar next to it named
#
#     <image>.src.json
#
# with exactly these keys:
#
#     {
#       "image":           "/abs/path/to/foo.cdi",
#       "elf":             "/abs/path/to/foo.elf",
#       "elf_sha256":      "<64 hex>",
#       "elf_size":        <bytes>,
#       "built_utc":       "2026-08-01T16:20:00Z",
#       "toolchain_image": "opencrossing-dc:sdk",
#       "producer":        "<script that wrote this>"
#     }
#
# THIS RULE APPLIES TO dc/build-dc-docker.sh TOO. Any script that runs
# `mkdcdisc -e X.elf -o Y.cdi` writes Y.cdi.src.json in the same step, or crash
# triage on the game build is dead on arrival. crash.sh refuses to guess.
# ---------------------------------------------------------------------------
#
# IMPORTANT (verified 2026-08-01): colima bind-mounts of /private/tmp/... are
# silently EMPTY inside containers on this host. Every Docker mount used here
# must live under $HOME. Do not "simplify" this back to a /tmp workdir.

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IMAGE="${OC_DC_TOOLCHAIN_IMAGE:-opencrossing-dc:sdk}"
OUT="${OC_DC_HARNESS_HOME:-$HOME/.cache/oc-dc-harness}/selftest"
WANT_CDI=1

while [ $# -gt 0 ]; do
    case "$1" in
        --image)  IMAGE="$2"; shift 2 ;;
        --out)    OUT="$2";   shift 2 ;;
        --no-cdi) WANT_CDI=0; shift ;;
        -h|--help) sed -n '2,52p' "$0"; exit 0 ;;
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
rm -f "$OUT"/selftest.elf "$OUT"/crashtest.elf \
      "$OUT"/selftest.cdi "$OUT"/crashtest.cdi \
      "$OUT"/selftest.cdi.src.json "$OUT"/crashtest.cdi.src.json

STEP='source /opt/toolchains/dc/kos/environ.sh 2>/dev/null; make clean >/dev/null 2>&1; make'
if [ "$WANT_CDI" -eq 1 ]; then
    STEP="$STEP"'
if command -v mkdcdisc >/dev/null 2>&1; then
    for e in selftest crashtest; do
        [ -f "$e.elf" ] && mkdcdisc -q -N -e "$e.elf" -o "$e.cdi"
    done
else
    echo "WARNING: no mkdcdisc in this image; ELF-only build" >&2
fi'
fi

docker run --rm --platform linux/arm64 -v "$OUT":/src -w /src "$IMAGE" bash -lc "$STEP" \
    >"$LOG" 2>&1
rc=$?

> "$OUT/.sidecars"
write_sidecar() {  # cdi elf
    local cdi="$1" elf="$2" sha size
    sha="$(shasum -a 256 "$elf" | awk '{print $1}')"
    size="$(wc -c < "$elf" | tr -d ' ')"
    cat > "$cdi.src.json" <<EOF
{
  "image": "$cdi",
  "elf": "$elf",
  "elf_sha256": "$sha",
  "elf_size": $size,
  "built_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "toolchain_image": "$IMAGE",
  "producer": "harness/dc/selftest/build.sh"
}
EOF
    echo "$cdi.src.json" >> "$OUT/.sidecars"
}

elfs=(); cdis=()
for f in selftest crashtest; do
    [ -f "$OUT/$f.elf" ] && elfs+=("\"$OUT/$f.elf\"")
    if [ -f "$OUT/$f.cdi" ] && [ -f "$OUT/$f.elf" ]; then
        cdis+=("\"$OUT/$f.cdi\"")
        write_sidecar "$OUT/$f.cdi" "$OUT/$f.elf"
    fi
done
sidecars=()
while IFS= read -r s; do [ -n "$s" ] && sidecars+=("\"$s\""); done < "$OUT/.sidecars"
rm -f "$OUT/.sidecars"

n=${#elfs[@]}
m=${#cdis[@]}
ok=false
if [ "$n" -eq 2 ] && { [ "$WANT_CDI" -eq 0 ] || [ "$m" -eq 2 ]; }; then ok=true; fi

printf '{\n'
printf '  "harness": "selftest-build",\n'
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
