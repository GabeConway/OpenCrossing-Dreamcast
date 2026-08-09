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

# `objs` deliberately does not link, so there is nothing to package. Same for
# the diagnostic targets: `warnscan` compiles into a throwaway objdir it then
# deletes, and `optreport` only prints. Any of them followed by the link step
# would either relink stale objects or fail on a missing ELF.
case "$DC_TARGET" in
    objs|warnscan|optreport|count|sources)
        echo "-- DC_TARGET=$DC_TARGET: no link step, skipping CDI."
        exit 0
        ;;
esac

if [ ! -f "$ELF" ]; then
    echo "ERROR: $ELF was not produced." >&2
    exit 1
fi
sh-elf-size "$ELF" || true

# ---------------------------------------------------------------------------
# 2. CDI
# ---------------------------------------------------------------------------
# kb/toolchain.md, VERIFIED measurement:
#   mkdcdisc     -e elf -o out.cdi  ->  740,083,145 B, 15.6 s
#   mkdcdisc -N  -e elf -o out.cdi  ->    1,783,337 B,  0.021 s
# a 415x size / 740x time difference.
#
# ⚠️ CORRECTED 2026-08-03. This comment used to say "the padding is deliberate:
# it pushes content toward the outer tracks, which is what a real CD-R wants".
# **That is false, and it was measured false.** In the 740,090,153 B padded CDI
# the two RARC magics sit at file offsets 48,918,408 and 49,892,520; the same
# files in the unpadded image compute to 48,927,752 and 49,901,864 — identical
# LBAs, a constant 4-sector prefix delta. All ~684 MB of padding is appended
# AFTER the filesystem, so every game byte lives on the INNERMOST ~10 % of the
# burned disc either way. Padding buys disc geometry realism for timing runs
# (the drive spins where a real pressed disc would), not outer-edge placement.
# Getting content onto the outer tracks would need a large dummy file placed
# FIRST via mkdcdisc's -S/--sort-file.
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
# -D, not -d: `-d` includes the root DIRECTORY ITSELF, which puts everything at
# /cd/discroot/<name> and makes every DVDFastOpen miss with no clue why (MEASURED
# — the boot listing showed a lone /cd/discroot entry). `-D` includes its
# CONTENTS, which is what dc_dvd.c's flat "/cd/<name>" expects.
DISC_ARGS=()
if [ -d /discroot ]; then
    DISC_ARGS=(-D /discroot)
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

# --- 3. The ELF provenance sidecar -------------------------------------------
# REQUIRED OF EVERY CDI PRODUCER, and this one did not write it until 2026-08-09
# (S14). harness/dc/README.md §"ELF provenance sidecars" states the rule and
# says in terms "This binds dc/build-dc-docker.sh too. Without the sidecar,
# crash triage on the game build is dead on arrival" — and it was: a CDI holds a
# scrambled, stripped 1ST_READ.BIN, so a register dump out of one is just hex,
# and harness/dc/crash.sh REFUSES to guess which ELF goes with an image or to
# symbolise against one whose sha256 no longer matches. Every game crash on a
# burn has therefore been un-triageable, silently, for the whole project.
#
# The sha256 is the load-bearing field, not the path: the ELF at that path is
# overwritten by the next build, and a confidently wrong line number is worse
# than no answer.
ELF_SHA=$(sha256sum "$ELF" | cut -d' ' -f1)
ELF_SIZE=$(wc -c < "$ELF")
cat > "${CDI}.src.json" <<EOF
{
  "image": "$CDI",
  "elf": "$ELF",
  "elf_sha256": "$ELF_SHA",
  "elf_size": $ELF_SIZE,
  "built_utc": "$(date -u +%Y-%m-%dT%H:%M:%SZ)",
  "toolchain_image": "opencrossing-dc:sdk",
  "producer": "dc/build-dc-docker.sh"
}
EOF
echo "-- sidecar: ${CDI}.src.json  (elf sha256 ${ELF_SHA:0:12}...)"
echo "-- done."
