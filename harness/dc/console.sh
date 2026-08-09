#!/usr/bin/env bash
#
# console.sh -- run a Dreamcast image and give back everything it said.
#
#   ./console.sh prog.elf
#   ./console.sh disc.cdi --timeout 30
#   ./console.sh prog.elf --print            # also pretty-print the log to stderr
#   ./console.sh prog.elf --capture-only     # no markers; run to the timeout, exit 0
#   ./console.sh prog.elf --grep 'PERF '     # only matching lines in the JSON
#
# JSON on stdout: the full console as an array plus the parsed typed records.
# Exit 0 when the guest reached OC-DC-HARNESS-END with rc=0 and no failed
# ASSERTs (or always, under --capture-only).
#
# This is the workhorse of the DC harness. It is how any unit test of ported
# game code reports itself: build a KOS ELF whose main() checksums a decoded
# texture / walks a heap / byte-swaps a save struct, have it print
#   ASSERT ok|fail <name>
# lines, and let this script turn that into an exit code.
#
# Guest-side protocol (see selftest/selftest.c for the reference implementation):
#   OC-DC-HARNESS-BEGIN
#   MARK:<name> [arg]  |  ASSERT ok|fail <name>  |  PERF k=v ...
#   FBHASH <hex>       |  FBTHUMB WxH <base64>   |  MEM k=v ...
#   OC-DC-HARNESS-END rc=<n>
#
# Two guest-side rules that are load-bearing (harness/dc/README.md, and
# measured here 2026-08-01):
#   1. Raise the SCIF baud before logging. Flycast models SCIF baud faithfully
#      and KOS busy-waits on the TX FIFO, so at the KOS default of 57600 you get
#      ~5.8 KB/s and ~190 bytes of logging eats a whole 30fps frame budget.
#      scif_set_parameters(1562500, 1); scif_init();  -> ~150 KB/s.
#      Emulator-only: a real coder's cable will not sync at 1.5 Mbps, so keep it
#      behind a build flag.
#   2. NEVER call scif_flush() from guest code. Flycast never re-raises TEND on
#      an idle TX FIFO, so KOS's flush spin times out and latches
#      serial_enabled = 0 -- all further output, including any crash dump,
#      vanishes and the run can only end in a timeout. printf() already flushes.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
RUNNER="$HERE/_runner.py"

IMAGE="${OC_DC_IMAGE:-$REPO/dc/build/OpenCrossing.cdi}"
RUNROOT="${OC_DC_RUNROOT:-$HOME/.cache/oc-dc-harness/runs}"
TIMEOUT=60
RUN_DIR=""
PRINT=0
CAPTURE_ONLY=0
GREP_RE=""
EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        --timeout)      TIMEOUT="$2"; shift 2 ;;
        --run-dir)      RUN_DIR="$2"; shift 2 ;;
        --print|-p)     PRINT=1; shift ;;
        --capture-only) CAPTURE_ONLY=1; shift ;;
        --grep)         GREP_RE="$2"; shift 2 ;;
        -c|--config)    EXTRA+=(-c "$2"); shift 2 ;;
        -h|--help)      sed -n '2,45p' "$0"; exit 0 ;;
        -*)             echo "unknown arg: $1" >&2; exit 2 ;;
        *)              IMAGE="$1"; shift ;;
    esac
done

case "$IMAGE" in /*) ;; *) IMAGE="$PWD/$IMAGE" ;; esac
if [ ! -f "$IMAGE" ]; then
    echo "console: image not found: $IMAGE" >&2
    exit 2
fi

[ -n "$RUN_DIR" ] || RUN_DIR="$RUNROOT/console-$(basename "${IMAGE%.*}")-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN_DIR"

case "$IMAGE" in
    *.cdi|*.gdi|*.chd|*.cue) EXTRA+=(-c config:FastGDRomLoad=yes) ;;
esac

ARGS=(--emit-lines)
if [ "$CAPTURE_ONLY" -eq 1 ]; then
    # No marker can end the run: read until the wall clock expires.
    ARGS+=(--end-marker '\0NEVER\0' --fail-marker '\0NEVER\0')
fi

set +e
"$RUNNER" "$IMAGE" --run-dir "$RUN_DIR" --timeout "$TIMEOUT" \
    "${ARGS[@]}" ${EXTRA[@]+"${EXTRA[@]}"} >/dev/null 2>"$RUN_DIR/runner-stderr.log"
rc=$?
set -e

# _runner.py always drops runner.json into the run dir, so reformat from there
# rather than juggling its stdout.
python3 - "$RUN_DIR/runner.json" "$GREP_RE" "$PRINT" <<'PY'
import json, re, sys
path, grep_re, do_print = sys.argv[1], sys.argv[2], sys.argv[3]
d = json.load(open(path))
lines = d.pop("lines", [])
if grep_re:
    rx = re.compile(grep_re)
    lines = [l for l in lines if rx.search(l)]
d["harness"] = "console"
d["lines"] = lines
if do_print == "1" and lines:
    sys.stderr.write("\n".join(lines) + "\n")
json.dump(d, sys.stdout, indent=1)
sys.stdout.write("\n")
PY

[ "$CAPTURE_ONLY" -eq 1 ] && exit 0
exit "$rc"
