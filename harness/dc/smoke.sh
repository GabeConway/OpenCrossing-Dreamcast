#!/usr/bin/env bash
#
# smoke.sh -- the M0 gate. Boot a Dreamcast image in Flycast, capture the
# emulated serial console, assert the guest actually came up, exit nonzero on
# crash or timeout. CI-shaped: non-interactive, hard wall-clock bound, JSON on
# stdout, one PASS/FAIL line on stderr.
#
#   ./smoke.sh                              # dc/build/OpenCrossing.cdi
#   ./smoke.sh path/to/disc.cdi
#   ./smoke.sh --elf path/to/prog.elf       # reios direct-ELF boot, no disc
#   ./smoke.sh --cdi path/to/disc.cdi --timeout 90
#   ./smoke.sh IMG --no-fast-gdrom          # keep modelled GD-ROM latency
#   ./smoke.sh IMG --expect 'MARK:TITLE_UP' # extra required console line (repeatable)
#   ./smoke.sh IMG --no-crash-triage        # don't auto-run crash.sh on a dump
#
# On a failure whose console contains a KOS register dump or assertion, this
# automatically runs crash.sh against the same run directory and merges the
# symbolised result into the JSON under "crash" -- so a failing gate tells you
# the file and line, not just that it failed. Needs the image's ELF: either an
# .elf image, or a CDI with its <image>.src.json provenance sidecar.
#
# ASSERTIONS (all must hold for exit 0):
#   1. Flycast is installed          -- else run install-flycast.sh
#   2. the image file exists
#   3. the run ended on the end marker, not on a fail marker and not on the
#      wall-clock timeout
#   4. the KOS banner reached the host  (proves the guest is executing at all,
#      not just that Flycast opened a window)
#   5. maple enumeration lists A0: Dreamcast Controller
#   6. MARK:BOOT_OK was printed
#   7. OC-DC-HARNESS-END rc=0
#   8. zero `ASSERT fail` records
#   9. every --expect regex matched some console line
#
# The 16 MB hardware contract is enforced mechanically on every run:
# _runner.py always passes config:Dreamcast.RamMod32MB=no. Never remove it.
#
# EXIT / TIMEOUT SEMANTICS (kb/design-harness.md §5, verified). Flycast does
# not exit when the guest returns from main and does not exit on a KOS kernel
# panic -- it sits in its window forever, and there is no exit-on-condition
# flag in the cask build. macOS here also has no timeout(1)/gtimeout(1). So the
# supervision is marker-driven inside _runner.py, which kills the whole process
# group on any of:
#
#   runner status   | meaning                                   | smoke exit
#   ----------------+-------------------------------------------+-----------
#   ok              | end marker seen                           | 0 (if 4-9 hold)
#   fail_marker     | panic / unhandled exception / ASSERT fail | 1
#   timeout         | deadline hit, no marker -- hang           | 1
#   exited_early    | Flycast died before any marker            | 1
#   launch_error    | no Flycast, or no such image              | 2
#
# A window WILL briefly open and take focus: there is no headless mode
# (SDL_VIDEODRIVER=dummy and =offscreen both abort with exit code 6).

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
RUNNER="$HERE/_runner.py"
FLYCAST="${OC_DC_FLYCAST:-/Applications/Flycast.app/Contents/MacOS/Flycast}"

IMAGE="${OC_DC_IMAGE:-$REPO/dc/build/OpenCrossing.cdi}"
RUNROOT="${OC_DC_RUNROOT:-$HOME/.cache/oc-dc-harness/runs}"
TIMEOUT=60
RUN_DIR=""
FAST_GDROM=1
AUTO_CRASH=1
EXPECT=()
EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        --elf|--cdi)     IMAGE="$2"; shift 2 ;;
        --timeout)       TIMEOUT="$2"; shift 2 ;;
        --run-dir)       RUN_DIR="$2"; shift 2 ;;
        --no-fast-gdrom) FAST_GDROM=0; shift ;;
        --expect)        EXPECT+=("$2"); shift 2 ;;
        --no-crash-triage) AUTO_CRASH=0; shift ;;
        -c|--config)     EXTRA+=(-c "$2"); shift 2 ;;
        -h|--help)       sed -n '2,58p' "$0"; exit 0 ;;
        -*)              echo "unknown arg: $1" >&2; exit 2 ;;
        *)               IMAGE="$1"; shift ;;
    esac
done

case "$IMAGE" in /*) ;; *) IMAGE="$PWD/$IMAGE" ;; esac

fail_hard() {  # reason
    printf '{\n  "harness": "smoke",\n  "ok": false,\n  "status": "launch_error",\n'
    printf '  "image": "%s",\n  "reason": "%s"\n}\n' "$IMAGE" "$1"
    echo "DC SMOKE FAIL -- $1" >&2
    exit 2
}

[ -x "$FLYCAST" ] || fail_hard "Flycast not installed at $FLYCAST; run $HERE/install-flycast.sh"
[ -f "$IMAGE" ]   || fail_hard "image not found: $IMAGE (build one with $HERE/selftest/build.sh, or pass a path)"

[ -n "$RUN_DIR" ] || RUN_DIR="$RUNROOT/smoke-$(basename "${IMAGE%.*}")-$(date +%Y%m%d-%H%M%S)-$$"
mkdir -p "$RUN_DIR"

# FastGDRomLoad skips modelled GD-ROM read latency: correct for a boot gate,
# wrong for the read-ahead/streaming tests where the latency is under test.
if [ "$FAST_GDROM" -eq 1 ]; then
    case "$IMAGE" in
        *.cdi|*.gdi|*.chd|*.cue) EXTRA+=(-c config:FastGDRomLoad=yes) ;;
    esac
fi

set +e
"$RUNNER" "$IMAGE" --run-dir "$RUN_DIR" --timeout "$TIMEOUT" \
    ${EXTRA[@]+"${EXTRA[@]}"} >/dev/null 2>"$RUN_DIR/runner-stderr.log"
set -e

EXPECT_FILE="$RUN_DIR/expect.txt"
: > "$EXPECT_FILE"
for e in ${EXPECT[@]+"${EXPECT[@]}"}; do printf '%s\n' "$e" >> "$EXPECT_FILE"; done

set +e
python3 - "$RUN_DIR/runner.json" "$EXPECT_FILE" > "$RUN_DIR/smoke.json" <<'PY'
import json, re, sys

d = json.load(open(sys.argv[1]))
expects = [l.rstrip("\n") for l in open(sys.argv[2]) if l.strip()]
lines = open(d["console"]).read().splitlines() if d.get("console") else []
rec = d.get("records", {})
marks = {r["name"] for r in rec.get("mark", [])}

checks = []
def check(name, ok, detail=""):
    checks.append({"name": name, "ok": bool(ok), "detail": detail})

status = d.get("status")
check("run_reached_end_marker", status == "ok", "runner status=%s" % status)
check("kos_banner", bool(d.get("kos_version") or d.get("kos_revision")),
      "version=%s revision=%s" % (d.get("kos_version"), d.get("kos_revision")))
ctrl = [m for m in d.get("maple", [])
        if m["port"] == "A0" and "Controller" in m["name"]]
check("maple_a0_controller", bool(ctrl),
      "maple=%s" % ", ".join("%s %s" % (m["port"], m["name"]) for m in d.get("maple", [])))
check("mark_boot_ok", "BOOT_OK" in marks, "marks=%s" % sorted(marks))
check("end_rc_zero", d.get("end_rc") == 0, "end_rc=%s" % d.get("end_rc"))
check("no_failed_asserts", not d.get("failed_asserts"),
      "failed=%s" % d.get("failed_asserts"))
for e in expects:
    rx = re.compile(e)
    check("expect:%s" % e, any(rx.search(l) for l in lines))

ok = all(c["ok"] for c in checks)

# On failure surface the tail of the console -- for a crash that is the whole
# KOS register dump + stack trace + the $KOS_ADDR2LINE template line, which is
# the entire triage story without a debugger.
tail = lines[-25:] if not ok else []

out = {
    "harness": "smoke",
    "ok": ok,
    "status": status,
    "image": d.get("image"),
    "elapsed_s": d.get("elapsed_s"),
    "timeout_s": d.get("timeout_s"),
    "kos_version": d.get("kos_version"),
    "kos_revision": d.get("kos_revision"),
    "maple": d.get("maple"),
    "end_rc": d.get("end_rc"),
    "marks": sorted(marks),
    "asserts": [(r["result"], r["name"]) for r in rec.get("assert", [])],
    "failed_asserts": d.get("failed_asserts"),
    "perf": [r["kv"] for r in rec.get("perf", [])],
    "mem": [r["kv"] for r in rec.get("mem", [])],
    "fbhash": [r["hash"] for r in rec.get("fbhash", [])],
    "checks": checks,
    "console": d.get("console"),
    "run_dir": d.get("run_dir"),
    "console_tail": tail,
}
json.dump(out, sys.stdout, indent=1)
sys.stdout.write("\n")

bad = [c["name"] for c in checks if not c["ok"]]
if ok:
    sys.stderr.write("DC SMOKE PASS -- %s in %.2fs (%s)\n"
                     % (d.get("image"), d.get("elapsed_s") or 0.0, status))
else:
    sys.stderr.write("DC SMOKE FAIL [%s] -- failed: %s\n  console: %s\n"
                     % (status, ", ".join(bad), d.get("console")))
sys.exit(0 if ok else 1)
PY
smoke_rc=$?
set -e

# ------------------------------------------------------------- auto crash triage
# A gate that only says "it failed" wastes the most expensive part of the loop.
# If the guest left a register dump behind, symbolise it now and fold the result
# into the JSON so the failure report carries file:line.
if [ "$smoke_rc" -ne 0 ] && [ "$AUTO_CRASH" -eq 1 ] \
   && grep -qE 'Unhandled exception|ASSERTION FAILURE|kernel panic' "$RUN_DIR/console.log" 2>/dev/null; then
    set +e
    "$HERE/crash.sh" --run-dir "$RUN_DIR" --image "$IMAGE" \
        > "$RUN_DIR/crash.json" 2>"$RUN_DIR/crash-stderr.log"
    crash_rc=$?
    set -e
    sed 's/^/  /' "$RUN_DIR/crash-stderr.log" >&2 || true
    python3 - "$RUN_DIR/smoke.json" "$RUN_DIR/crash.json" "$crash_rc" <<'PY'
import json, sys
smoke_path, crash_path, crash_rc = sys.argv[1], sys.argv[2], int(sys.argv[3])
s = json.load(open(smoke_path))
try:
    c = json.load(open(crash_path))
except Exception as e:
    c = {"ok": False, "error": "crash.sh produced no JSON: %s" % e}
s["crash"] = {k: c.get(k) for k in
              ("ok", "error", "kind", "exception", "panic", "assertion",
               "pc", "pr", "sr", "evt", "code", "fault", "caller",
               "elf", "elf_verified", "stack", "disasm")}
s["crash"]["triage_rc"] = crash_rc
json.dump(s, open(smoke_path, "w"), indent=1)
PY
fi

cat "$RUN_DIR/smoke.json"
exit "$smoke_rc"
