#!/usr/bin/env bash
#
# run-flycast.sh -- boot a Dreamcast image in Flycast with the harness's
# verified invocation, bounded by wall clock and by guest output markers.
#
#   ./run-flycast.sh                       # boots dc/build/OpenCrossing.cdi
#   ./run-flycast.sh path/to/thing.cdi
#   ./run-flycast.sh path/to/prog.elf      # reios direct-ELF boot, no disc
#   ./run-flycast.sh IMG --timeout 30 --quiet
#   ./run-flycast.sh IMG -c config:Dynarec.Enabled=no
#
# Guest serial is streamed to STDERR live; JSON result goes to STDOUT.
# Exit 0 only when the run reached OC-DC-HARNESS-END (or --end-marker).
#
# Options:
#   --timeout N        wall-clock bound, seconds (default 60)
#   --end-marker RE    regex that ends the run successfully
#   --fail-marker RE   regex that ends the run as a failure
#   --run-dir DIR      where console.log / runner.json land
#   --no-fast-gdrom    model real GD-ROM read latency (default: skip it)
#   --quiet            don't stream guest serial to stderr
#   -c, --config K=V   extra `-config` pairs, repeatable
#
# What is baked in by _runner.py and is NOT optional:
#   config:Debug.SerialConsoleEnabled=yes   guest SCIF TX -> host stdout
#   config:UseReios=yes                     HLE BIOS; also what boots raw ELFs
#   config:Dreamcast.RamMod32MB=no          the 16 MB hardware contract
#   log:LogToConsole=no                     Flycast's own log off stdout
#
# Known, unavoidable (design-harness.md §2): there is no headless mode. A
# window opens and briefly takes focus. SDL_VIDEODRIVER=dummy/offscreen abort
# with exit code 6. Flycast also never exits on its own -- not on return from
# main, not on a kernel panic -- which is why this is marker-driven.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
RUNNER="$HERE/_runner.py"

IMAGE="${OC_DC_IMAGE:-$REPO/dc/build/OpenCrossing.cdi}"
RUNROOT="${OC_DC_RUNROOT:-$HOME/.cache/oc-dc-harness/runs}"
TIMEOUT=60
RUN_DIR=""
QUIET=0
FAST_GDROM=1
EXTRA=()
PASSTHRU=()

while [ $# -gt 0 ]; do
    case "$1" in
        --timeout)      TIMEOUT="$2"; shift 2 ;;
        --end-marker)   PASSTHRU+=(--end-marker "$2"); shift 2 ;;
        --fail-marker)  PASSTHRU+=(--fail-marker "$2"); shift 2 ;;
        --run-dir)      RUN_DIR="$2"; shift 2 ;;
        --no-fast-gdrom) FAST_GDROM=0; shift ;;
        --quiet|-q)     QUIET=1; shift ;;
        -c|--config)    EXTRA+=(-c "$2"); shift 2 ;;
        -h|--help)      sed -n '2,40p' "$0"; exit 0 ;;
        -*)             echo "unknown arg: $1" >&2; exit 2 ;;
        *)              IMAGE="$1"; shift ;;
    esac
done

case "$IMAGE" in
    /*) ;;
    *)  IMAGE="$(cd "$(dirname "$IMAGE")" 2>/dev/null && pwd)/$(basename "$IMAGE")" || true ;;
esac

if [ ! -f "$IMAGE" ]; then
    echo "run-flycast: image not found: $IMAGE" >&2
    echo "  build one with harness/dc/selftest/build.sh, or pass a path." >&2
    exit 2
fi

if [ -z "$RUN_DIR" ]; then
    RUN_DIR="$RUNROOT/run-$(basename "${IMAGE%.*}")-$(date +%Y%m%d-%H%M%S)-$$"
fi
mkdir -p "$RUN_DIR"

# FastGDRomLoad skips modelled GD-ROM read latency. Right for CI; WRONG for the
# read-ahead / streaming tests, where the latency is the thing under test.
if [ "$FAST_GDROM" -eq 1 ]; then
    case "$IMAGE" in
        *.cdi|*.gdi|*.chd|*.cue) EXTRA+=(-c config:FastGDRomLoad=yes) ;;
    esac
fi

[ "$QUIET" -eq 1 ] || PASSTHRU+=(--tee)

set +e
"$RUNNER" "$IMAGE" --run-dir "$RUN_DIR" --timeout "$TIMEOUT" \
    "${PASSTHRU[@]}" ${EXTRA[@]+"${EXTRA[@]}"}
rc=$?
set -e
exit "$rc"
