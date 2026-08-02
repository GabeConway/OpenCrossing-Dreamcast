#!/usr/bin/env bash
# =============================================================================
# build-dc-docker.sh — runs INSIDE the opencrossing-dc:sdk container.
# =============================================================================
# Invoked as:
#     docker run --rm --platform linux/arm64 \
#       -v /Users/gabe/Documents/GitHub/OpenCrossing-Dreamcast:/work \
#       opencrossing-dc:sdk bash /work/dc/build-dc-docker.sh
#
# The image's dc-env entrypoint has already exported KOS_BASE, KOS_PORTS,
# KOS_CFLAGS, KOS_LDFLAGS and a PATH carrying sh-elf-*, kos-cc/kos-c++ and
# mkdcdisc, so this script does no toolchain setup of its own.
#
# ENV KNOBS
#   JOBS=4          parallel make jobs (default 4 — the colima VM has 4 cores)
#   DC_CDI_PAD=1    build a PADDED CDI (740 MB) instead of the default
#                   unpadded one (1.8 MB). See §5.2 below.
#   DC_TARGET=objs  build target: objs (compile only, no link) | all (default)
#   DECOMP_OPT=-O0  optimization level for decomp game code
#   V=1             echo full compiler command lines
#
# OUTPUTS (inside the bind mount, so they survive the container)
#   /work/dc/build/AnimalCrossing.elf     unstripped ELF, used by addr2line
#   /work/dc/build/AnimalCrossing.map     link map
#   /work/dc/build/OpenCrossing.cdi       burnable / Flycast-loadable image
# =============================================================================
set -uo pipefail

# Absolute paths everywhere: agents and CI run from varying cwds (CLAUDE.md).
DC_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$DC_DIR/.." && pwd)"
BUILD="$DC_DIR/build"

JOBS="${JOBS:-4}"
DC_TARGET="${DC_TARGET:-all}"
ELF="$BUILD/AnimalCrossing.elf"
CDI="$BUILD/OpenCrossing.cdi"

echo "=============================================================="
echo " OpenCrossing-Dreamcast build"
echo "   repo    : $ROOT"
echo "   target  : $DC_TARGET"
echo "   jobs    : $JOBS"
echo "   KOS_BASE: ${KOS_BASE:-<unset!>}"
echo "   gcc     : $(sh-elf-gcc -dumpversion 2>/dev/null || echo '<missing>')"
echo "=============================================================="

if [ -z "${KOS_BASE:-}" ]; then
    echo "ERROR: KOS_BASE is unset. This script must run inside" >&2
    echo "       opencrossing-dc:sdk (its entrypoint exports the KOS env)." >&2
    exit 2
fi

# ---------------------------------------------------------------------------
# 1. Compile / link
# ---------------------------------------------------------------------------
START=$(date +%s)
make -C "$DC_DIR" -j"$JOBS" "$DC_TARGET"
RC=$?
END=$(date +%s)
echo "-- make $DC_TARGET finished in $((END - START))s (rc=$RC)"

if [ $RC -ne 0 ]; then
    echo "ERROR: build failed." >&2
    exit $RC
fi

# `objs` deliberately does not link, so there is nothing to package.
if [ "$DC_TARGET" = "objs" ]; then
    echo "-- DC_TARGET=objs: compile-only run, skipping link + CDI."
    exit 0
fi

if [ ! -f "$ELF" ]; then
    echo "ERROR: $ELF was not produced." >&2
    exit 1
fi
sh-elf-size "$ELF" || true

# ---------------------------------------------------------------------------
# 2. CDI
# ---------------------------------------------------------------------------
# kb/design-toolchain.md §5.2, VERIFIED measurement:
#   mkdcdisc     -e elf -o out.cdi  ->  740,083,145 B, 15.6 s
#   mkdcdisc -N  -e elf -o out.cdi  ->    1,783,337 B,  0.021 s
# a 415x size / 740x time difference. The padding is deliberate: it pushes
# content toward the outer tracks, which is what a real CD-R wants (PLAN §5).
#
# So: -N by default (the Flycast iteration loop — 740 MB per iteration is
# untenable), and the padded form only when DC_CDI_PAD=1, i.e. for CD-R burns
# and for any timing run that has to be read-speed-realistic. Measuring
# streaming/acre-load performance against an unpadded image gives optimistic
# numbers.
PAD_ARGS=(-N)
PAD_DESC="unpadded (-N; fast Flycast loop)"
if [ "${DC_CDI_PAD:-0}" = "1" ]; then
    PAD_ARGS=()
    PAD_DESC="PADDED (CD-R burn / read-speed-realistic timing)"
fi

# Disc content. dc_dvd.c builds every path as "/cd" + "/" + name, so the game's
# files must sit at the DISC ROOT, flat — not under a files/ subdirectory. The
# host wrapper bind-mounts a staging directory at /discroot when DC_DISC_ROOT is
# set; without it the image is ELF-only and every DVDFastOpen misses, which is
# what the S1 bring-up image did.
DISC_ARGS=()
if [ -d /discroot ]; then
    DISC_ARGS=(-d /discroot)
    echo "-- disc content: /discroot ($(find /discroot -type f | wc -l) files, \
$(du -sh /discroot 2>/dev/null | cut -f1))"
else
    echo "-- disc content: NONE (ELF only). Every DVD open will miss."
fi

echo "-- mkdcdisc: $PAD_DESC"
START=$(date +%s)
mkdcdisc "${PAD_ARGS[@]}" \
    -e "$ELF" \
    "${DISC_ARGS[@]}" \
    -n "OpenCrossing" \
    -o "$CDI"
RC=$?
END=$(date +%s)

if [ $RC -ne 0 ]; then
    echo "ERROR: mkdcdisc failed (rc=$RC)." >&2
    exit $RC
fi

SIZE=$(wc -c < "$CDI")
echo "-- CDI: $CDI  ${SIZE} bytes  ($((END - START))s)"
echo "-- done."
