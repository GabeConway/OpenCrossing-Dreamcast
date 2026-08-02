#!/usr/bin/env bash
# =============================================================================
# stage-disc.sh — flatten a `dcasset extract` tree into a Dreamcast disc root.
# =============================================================================
#   bash dc/stage-disc.sh <dcasset-extract-dir> <staging-dir>
#
# WHY THIS EXISTS
#   `dcasset extract` writes the GameCube shape: files/ for the FST payload and
#   sys/ for main.dol. dc_dvd.c does not reproduce that shape — every path it
#   opens is "/cd" + "/" + name (dc/src/dc_dvd.c:113 dc_dvd_make_path), so the
#   game asks for /cd/forest_1st.arc, /cd/static.str, /cd/main.dol. Handing
#   mkdcdisc the extract tree directly puts them at /cd/files/... and every
#   DVDFastOpen misses.
#
#   So: copy every file to the staging root, flat. Names are unique across
#   files/ and sys/ in the GAFE01 layout (verified — the script fails loudly if
#   that ever stops being true rather than silently overwriting).
#
# The staging directory is disc content built from the user's own ISO. It is
# scratch: keep it OUT of the repo (CLAUDE.md §1 — no ROM material, no disc
# images committed).
#
# Then:
#   DC_DISC_ROOT=<staging-dir> DC_ASSET_STUB=1 bash dc/build-dc.sh
# =============================================================================
set -euo pipefail

if [ $# -ne 2 ]; then
    sed -n '2,26p' "${BASH_SOURCE[0]}"
    exit 2
fi

SRC="$(cd "$1" && pwd)"
DST="$2"

if [ ! -d "$SRC" ]; then
    echo "ERROR: '$SRC' is not a directory." >&2
    exit 2
fi

mkdir -p "$DST"
DST="$(cd "$DST" && pwd)"

if [ "$SRC" = "$DST" ]; then
    echo "ERROR: source and staging directory are the same." >&2
    exit 2
fi

n=0
total=0
while IFS= read -r f; do
    base="$(basename "$f")"
    # manifest.json is dcasset bookkeeping, not disc content.
    [ "$base" = "manifest.json" ] && continue
    if [ -e "$DST/$base" ]; then
        echo "ERROR: name collision on '$base' — the GameCube layout is no" >&2
        echo "       longer flat-safe. Fix dc_dvd.c's path mapping instead of" >&2
        echo "       silently overwriting." >&2
        exit 1
    fi
    cp "$f" "$DST/$base"
    n=$((n + 1))
    total=$((total + $(wc -c < "$f")))
done < <(find "$SRC" -type f)

echo "staged $n files, $total bytes -> $DST"
echo
echo "next:"
echo "  DC_DISC_ROOT=$DST DC_ASSET_STUB=1 bash dc/build-dc.sh"
