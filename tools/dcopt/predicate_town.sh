#!/usr/bin/env bash
# =============================================================================
# predicate_town.sh — the default GOOD/BAD verdict for tools/dcopt/bisect_o0.sh
# =============================================================================
# Boots whatever dc/build/OpenCrossing.cdi currently is and answers one
# question: did this image get to the town and stay sane?
#
#   exit 0  GOOD — reached scene 9, no crash, no missing asset
#   exit 1  BAD
#
# Knobs (environment):
#   PRED_TIMEOUT   seconds of emulator wall clock, default 420. Long enough to
#                  reach scene 9 through DC_AUTOSTART, short enough that a
#                  12-step bisect is not an overnight job.
#   PRED_SCENE     the scene mode that counts as arrival, default 9 (the town).
#
# ⚠️ IT COPIES THE CDI FIRST. kb/traps.md: never run a long emulator session
# against the file the next build is going to overwrite.
#
# ⚠️ IT IS A PROGRESSION GATE, NOT A RENDERING GATE. It cannot see colour. A
# miscompile that leaves the town reachable but draws it wrong passes here —
# judge that on a screenshot pair (kb/RESUME.md §0b rule 2). If you are
# bisecting a VISUAL regression, write a different predicate and pass it as
# bisect_o0.sh's second argument.
# =============================================================================
set -uo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
TIMEOUT="${PRED_TIMEOUT:-420}"
SCENE="${PRED_SCENE:-9}"

SRC="$REPO/dc/build/OpenCrossing.cdi"
[ -f "$SRC" ] || { echo "predicate: no $SRC — did the build run?" >&2; exit 1; }

CDI="$HOME/.cache/oc-dc-bisect.cdi"
cp "$SRC" "$CDI"

bash "$REPO/harness/dc/smoke.sh" "$CDI" --timeout "$TIMEOUT" \
    -c config:LimitFPS=no >/dev/null 2>&1 || true

RUN="$(ls -td "$HOME"/.cache/oc-dc-harness/runs/smoke-oc-dc-bisect-* 2>/dev/null | head -1)"
LOG="$RUN/console.log"
if [ ! -f "$LOG" ]; then
    echo "predicate: BAD (no console.log — the guest never produced output)"
    exit 1
fi

# Reached the target scene?
if ! grep -qE "\[SCENE_MODE\][[:space:]]+[0-9]+[[:space:]]*->[[:space:]]*$SCENE\b" "$LOG"; then
    echo "predicate: BAD (never reached scene $SCENE)"
    exit 1
fi
# Crashed, asserted, or faulted on the way?
if grep -qE "crashes=[1-9]|ASSERT fail|unhandled exception|Unhandled exception|PC=0x" "$LOG"; then
    echo "predicate: BAD (fault in the log)"
    exit 1
fi
# An image whose assets stopped resolving is not a codegen verdict, it is a
# broken build line — fail loudly rather than blaming the bisect's midpoint.
if grep -q "ASSET MISSING" "$LOG"; then
    echo "predicate: BAD (ASSET MISSING — check the build line, not the TU)"
    exit 1
fi

echo "predicate: GOOD (reached scene $SCENE, clean) — $RUN"
exit 0
