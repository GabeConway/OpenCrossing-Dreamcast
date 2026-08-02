#!/usr/bin/env bash
#
# screenshot.sh -- guest-side framebuffer capture and golden-hash regression.
#
#   ./screenshot.sh prog.elf
#   ./screenshot.sh prog.elf --frame 2
#   ./screenshot.sh prog.elf --frame 0 --golden 1489d5c5
#   ./screenshot.sh prog.elf --golden-file dc/golden/title.hash
#   ./screenshot.sh prog.elf --frame 0 --write-golden dc/golden/title.hash
#
# JSON on stdout: every FBHASH/FBTHUMB the guest emitted, the selected frame,
# and (with --golden*) the comparison verdict. Exit 0 only if the run reached
# its end marker, produced at least one FBHASH, and matched the golden if one
# was given.
#
# ---------------------------------------------------------------------------
# READ THIS BEFORE ASKING WHY THERE IS NO PNG
#
# Host-side screen capture is BLOCKED on this machine and this script will
# never try it (kb/design-harness.md §4, verified):
#   * `screencapture -x -t png out.png` -> "could not create image from
#     display" (Screen Recording TCC permission not granted to the terminal).
#   * `osascript` + System Events -> HANGS on the Accessibility TCC prompt.
# Therefore Flycast's own F12 screenshot (EMU_BTN_SCREENSHOT ->
# ~/Pictures/Flycast-<ISO8601>.png) is unreachable unattended: there is no way
# to synthesise the keypress. Unblocking it needs a human to grant Screen
# Recording + Accessibility in System Settings. It is a manual-only path.
#
# What works instead: the guest owns the framebuffer, so it hashes and samples
# it itself and ships the result over the serial console. Verified byte-identical
# across separate runs and across 3 parallel instances, so a hex hash checked
# into the repo is a valid golden image -- no image files needed.
#
# Guest side (see selftest/selftest.c fb_report()):
#   FNV-1a over vram_s -> `FBHASH %08lx`
#   16x12 RGB565 sample, base64 -> `FBTHUMB 16x12 <b64>`
#   `MARK:FRAME <n>` just before each pair
# Emit them after vid_waitvbl(), once per checkpoint -- ~256 chars is ~45 ms of
# emulated time at the harness baud, cheap; a full 640x480x2 base64 dump is
# ~614 KB / ~4 s emulated and is the escape hatch for when a hash goes red and
# a human actually has to look at the picture.
# ---------------------------------------------------------------------------

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
RUNNER="$HERE/_runner.py"

IMAGE="${OC_DC_IMAGE:-$REPO/dc/build/OpenCrossing.cdi}"
RUNROOT="${OC_DC_RUNROOT:-$HOME/.cache/oc-dc-harness/runs}"
TIMEOUT=60
FRAME=""
GOLDEN=""
GOLDEN_FILE=""
WRITE_GOLDEN=""
RUN_DIR=""
FB_WRITEBACK=1
EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        --frame)        FRAME="$2"; shift 2 ;;
        --golden)       GOLDEN="$2"; shift 2 ;;
        --golden-file)  GOLDEN_FILE="$2"; shift 2 ;;
        --write-golden) WRITE_GOLDEN="$2"; shift 2 ;;
        --timeout)      TIMEOUT="$2"; shift 2 ;;
        --run-dir)      RUN_DIR="$2"; shift 2 ;;
        -c|--config)    EXTRA+=(-c "$2"); shift 2 ;;
        # MEASURED 2026-08-02: without this the guest hashes an all-zero frame
        # no matter what is on screen — Flycast's hardware renderer never
        # writes back to emulated VRAM (kb/traps.md). A game image is useless
        # here without it, so unlike smoke.sh this defaults ON and the flag
        # turns it off for selftest images, which own their own framebuffer.
        --no-fb-writeback) FB_WRITEBACK=0; shift ;;
        -h|--help)      sed -n '2,45p' "$0"; exit 0 ;;
        -*)             echo "unknown arg: $1" >&2; exit 2 ;;
        *)              IMAGE="$1"; shift ;;
    esac
done

case "$IMAGE" in /*) ;; *) IMAGE="$PWD/$IMAGE" ;; esac
[ -f "$IMAGE" ] || { echo "screenshot: image not found: $IMAGE" >&2; exit 2; }

if [ "$FB_WRITEBACK" -eq 1 ]; then
    EXTRA+=(-c config:rend.EmulateFramebuffer=yes)
fi

if [ -n "$GOLDEN_FILE" ]; then
    case "$GOLDEN_FILE" in /*) ;; *) GOLDEN_FILE="$REPO/$GOLDEN_FILE" ;; esac
    [ -f "$GOLDEN_FILE" ] || { echo "screenshot: no golden file: $GOLDEN_FILE" >&2; exit 2; }
    GOLDEN="$(tr -d '[:space:]' < "$GOLDEN_FILE")"
fi
if [ -n "$WRITE_GOLDEN" ]; then
    case "$WRITE_GOLDEN" in /*) ;; *) WRITE_GOLDEN="$REPO/$WRITE_GOLDEN" ;; esac
fi

[ -n "$RUN_DIR" ] || RUN_DIR="$RUNROOT/shot-$(basename "${IMAGE%.*}")-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN_DIR"

case "$IMAGE" in
    *.cdi|*.gdi|*.chd|*.cue) EXTRA+=(-c config:FastGDRomLoad=yes) ;;
esac

set +e
"$RUNNER" "$IMAGE" --run-dir "$RUN_DIR" --timeout "$TIMEOUT" \
    ${EXTRA[@]+"${EXTRA[@]}"} >/dev/null 2>"$RUN_DIR/runner-stderr.log"
run_rc=$?
set -e

python3 - "$RUN_DIR/runner.json" "$FRAME" "$GOLDEN" "$WRITE_GOLDEN" "$run_rc" <<'PY'
import json, os, sys
path, frame, golden, write_golden, run_rc = sys.argv[1:6]
d = json.load(open(path))
rec = d.get("records", {})
hashes = [r["hash"] for r in rec.get("fbhash", [])]
thumbs = [r["b64"] for r in rec.get("fbthumb", [])]

idx = int(frame) if frame not in ("", None) else (len(hashes) - 1 if hashes else -1)
sel_hash = hashes[idx] if 0 <= idx < len(hashes) else None
sel_thumb = thumbs[idx] if 0 <= idx < len(thumbs) else None

verdict, ok = "no-golden", (d.get("status") == "ok" and sel_hash is not None)
if golden:
    g = golden.strip().lower()
    if sel_hash is None:
        verdict, ok = "no-frame", False
    elif sel_hash == g:
        verdict = "match"
    else:
        verdict, ok = "mismatch", False

if write_golden and sel_hash:
    os.makedirs(os.path.dirname(write_golden) or ".", exist_ok=True)
    with open(write_golden, "w") as f:
        f.write(sel_hash + "\n")

out = {
    "harness": "screenshot",
    "status": d.get("status"),
    "run_status_rc": int(run_rc),
    "image": d.get("image"),
    "run_dir": d.get("run_dir"),
    "console": d.get("console"),
    "frame": idx,
    "frames_captured": len(hashes),
    "fbhash": sel_hash,
    "fbthumb": sel_thumb,
    "all_fbhash": hashes,
    "golden": golden or None,
    "golden_written": write_golden or None,
    "verdict": verdict,
    "host_capture": "blocked (Screen Recording + Accessibility TCC); "
                    "guest-side FBHASH/FBTHUMB only -- see script header",
    "ok": ok,
}
if verdict == "mismatch":
    out["diff_hint"] = ("golden %s != observed %s; compare fbthumb against the "
                        "golden's thumbnail by eye, or have the guest dump the "
                        "full framebuffer as base64" % (golden, sel_hash))
json.dump(out, sys.stdout, indent=1)
sys.stdout.write("\n")
sys.exit(0 if ok else 1)
PY
