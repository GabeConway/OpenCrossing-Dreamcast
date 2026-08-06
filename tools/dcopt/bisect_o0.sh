#!/usr/bin/env bash
# =============================================================================
# bisect_o0.sh — find the TU that miscompiles when optimized
# =============================================================================
# Binary search over a candidate list of decomp sources, quarantining half of
# them at -O0 per iteration (dc/Makefile's DC_OPT_O0_EXTRA) and asking a
# PREDICATE whether the resulting image is good.
#
#   bash tools/dcopt/bisect_o0.sh CANDIDATES.txt [PREDICATE]
#
#   CANDIDATES.txt  one repo-relative source path per line (# comments and
#                   blank lines ignored), exactly as dc/opt-lists.mk spells
#                   them. Generate one from a warnscan:
#                     python3 tools/dcopt/warnscan_report.py \
#                         dc/build/warnscan.log --paths > /tmp/cand.txt
#   PREDICATE       a command run after each build. Exit 0 = the image is GOOD
#                   (the bug is NOT in the quarantined half). Default:
#                   tools/dcopt/predicate_town.sh, which boots the image and
#                   requires the run to reach the town.
#
# THE INVARIANT, and it is the whole reason this is a script and not a habit:
# quarantining a file means compiling it at -O0, i.e. the KNOWN-GOOD state. So
# a build whose quarantined half is GOOD means the culprit is in the OTHER
# half — the half still being optimized. Getting that backwards costs a full
# bisect, so it is asserted below rather than left to whoever is reading.
#
# COST. One iteration is a ~96 s whole-tree rebuild (the flag stamp changes, so
# every TU rebuilds) plus one predicate run, which for a game image is 10-20
# min of wall clock. log2(3900) is 12 iterations — do not start here. Start
# from a warnscan-ranked candidate list of tens of files, which is 5-6.
#
# ⚠️ Never run two builds at once and never edit the tree while one runs
# (kb/traps.md). This script serialises both by construction; do not
# background it and keep working in the same tree.
# =============================================================================
set -euo pipefail

REPO="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
CAND_FILE="${1:?usage: bisect_o0.sh CANDIDATES.txt [PREDICATE]}"
PREDICATE="${2:-bash $REPO/tools/dcopt/predicate_town.sh}"

[ -f "$CAND_FILE" ] || { echo "ERROR: no such candidate file: $CAND_FILE" >&2; exit 2; }

mapfile -t ALL < <(grep -v '^[[:space:]]*#' "$CAND_FILE" | grep -v '^[[:space:]]*$')
[ "${#ALL[@]}" -gt 0 ] || { echo "ERROR: candidate list is empty" >&2; exit 2; }

echo "== bisect_o0: ${#ALL[@]} candidates, predicate: $PREDICATE"

# ---------------------------------------------------------------------------
# build_and_test <files...>  — quarantine these at -O0, build, run the
# predicate. Returns the predicate's verdict: 0 = image GOOD.
# ---------------------------------------------------------------------------
build_and_test() {
    local list="$*"
    echo "-- building with $(echo "$list" | wc -w | tr -d ' ') file(s) at -O0"
    if ! DC_OPT_O0_EXTRA="$list" bash "$REPO/dc/build-dc.sh" > /tmp/bisect-build.log 2>&1; then
        echo "!! BUILD FAILED — see /tmp/bisect-build.log" >&2
        tail -20 /tmp/bisect-build.log >&2
        exit 3
    fi
    $PREDICATE
}

# ---------------------------------------------------------------------------
# Sanity gates. Both are cheap relative to a bisect and both have been wrong
# assumptions in this project before: that the "all optimized" build actually
# reproduces the failure, and that -O0 actually fixes it.
# ---------------------------------------------------------------------------
echo "== gate 1: nothing quarantined — the failure MUST reproduce"
if build_and_test ""; then
    echo "!! The fully-optimized image PASSES the predicate. There is nothing" >&2
    echo "!! to bisect: either the predicate is too weak or the bug is not a" >&2
    echo "!! codegen bug. Fix the predicate first." >&2
    exit 4
fi
echo "-- gate 1 OK: reproduces"

echo "== gate 2: every candidate quarantined — the failure MUST go away"
if ! build_and_test "${ALL[*]}"; then
    echo "!! Quarantining ALL candidates still fails. The culprit is outside" >&2
    echo "!! this candidate list — widen it (or the bug is not per-TU: link" >&2
    echo "!! order, a shared inline in a header, or dc/src itself)." >&2
    exit 5
fi
echo "-- gate 2 OK: quarantine fixes it, so the culprit is in the list"

# ---------------------------------------------------------------------------
# The search. Invariant: quarantining SUSPECT fixes the image.
# ---------------------------------------------------------------------------
SUSPECT=("${ALL[@]}")
while [ "${#SUSPECT[@]}" -gt 1 ]; do
    half=$(( ${#SUSPECT[@]} / 2 ))
    A=("${SUSPECT[@]:0:$half}")
    B=("${SUSPECT[@]:$half}")
    echo "== ${#SUSPECT[@]} left; testing first ${#A[@]} at -O0"
    if build_and_test "${A[*]}"; then
        echo "-- GOOD with A quarantined => culprit is in A"
        SUSPECT=("${A[@]}")
    else
        echo "-- still bad with A quarantined => culprit is in B"
        SUSPECT=("${B[@]}")
    fi
done

echo
echo "=================================================================="
echo "CULPRIT: ${SUSPECT[0]}"
echo
echo "Add it to OPT_QUARANTINE_SRC in dc/opt-lists.mk WITH ITS EVIDENCE:"
echo "the symptom, the run directory that showed it, and today's date."
echo "=================================================================="
