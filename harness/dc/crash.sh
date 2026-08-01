#!/usr/bin/env bash
#
# crash.sh -- turn a KOS crash dump into file:line + disassembly.
#
#   ./crash.sh prog.elf                      # run it, then triage the crash
#   ./crash.sh disc.cdi                      # ditto; ELF found via the sidecar
#   ./crash.sh --run-dir DIR --image IMG     # triage an ALREADY captured run
#   ./crash.sh --console LOG --elf prog.elf  # triage a bare console log
#   ./crash.sh disc.cdi --elf /path/other.elf   # override ELF resolution
#
# JSON on stdout: {pc, pr, sr, evt, code, exception, registers, stack[],
# frames[] (symbol/file/line per address), disasm, elf, elf_verified}.
# Exit 0 when a dump was found AND symbolised; 1 when no dump was in the log;
# 2 on a resolution error (no ELF, stale ELF, no Docker).
#
# Why this exists in this shape (kb/design-harness.md §6/§8e):
#   * The cask Flycast has ENABLE_GDB_SERVER=OFF -- port 3263 has nothing
#     listening and the binary contains no RSP strings. There is no debugger.
#   * There does not need to be one. KOS prints the faulting PC, all 16 GPRs,
#     SR, PR and an unwound stack over the serial console, and the host can
#     symbolise that offline. That is a complete triage loop.
#   * KOS 2.3 even prints a ready-made `$KOS_ADDR2LINE ...` command line; this
#     script is that command line, automated, with the ELF resolved for you.
#
# ELF PROVENANCE -- the part that makes this work on a CDI
#
# A CDI holds a scrambled, stripped 1ST_READ.BIN. To symbolise a crash in one
# you need the exact ELF it was built from. Resolution order:
#   1. --elf PATH                       (explicit; trusted, no hash check)
#   2. the image itself, if it is *.elf
#   3. <image>.src.json sidecar         (written by every CDI producer -- see
#      the header of selftest/build.sh for the required schema; the same rule
#      binds dc/build-dc-docker.sh)
# The sidecar records the ELF's sha256 at build time. If the file on disk no
# longer matches, this script FAILS LOUDLY rather than printing line numbers
# from a different binary -- a wrong answer here is worse than no answer.
#
# Docker note: the ELF's directory is bind-mounted into the toolchain image, so
# it must live under $HOME. colima bind-mounts of /private/tmp/... are silently
# empty on this host.
#
# LIMITATION (verified): Flycast does NOT trap unaligned accesses under either
# the dynarec or the interpreter, and a write to 0x00000000 does not fault.
# The alignment class of bug that -O2 on the decomp is expected to produce
# never reaches this script. That needs real hardware or host-side UBSan.

set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO="$(cd "$HERE/../.." && pwd)"
RUNNER="$HERE/_runner.py"
TOOLCHAIN="${OC_DC_TOOLCHAIN_IMAGE:-opencrossing-dc:sdk}"

IMAGE="${OC_DC_IMAGE:-$REPO/dc/build/OpenCrossing.cdi}"
RUNROOT="${OC_DC_RUNROOT:-$HOME/.cache/oc-dc-harness/runs}"
TIMEOUT=60
RUN_DIR=""
CONSOLE=""
ELF=""
CONTEXT=24          # bytes of disassembly either side of the faulting PC
EXTRA=()

while [ $# -gt 0 ]; do
    case "$1" in
        --run-dir)   RUN_DIR="$2"; shift 2 ;;
        --console)   CONSOLE="$2"; shift 2 ;;
        --image)     IMAGE="$2"; shift 2 ;;
        --elf)       ELF="$2"; shift 2 ;;
        --timeout)   TIMEOUT="$2"; shift 2 ;;
        --context)   CONTEXT="$2"; shift 2 ;;
        -c|--config) EXTRA+=(-c "$2"); shift 2 ;;
        -h|--help)   sed -n '2,45p' "$0"; exit 0 ;;
        -*)          echo "unknown arg: $1" >&2; exit 2 ;;
        *)           IMAGE="$1"; shift ;;
    esac
done

case "$IMAGE" in /*) ;; *) IMAGE="$PWD/$IMAGE" ;; esac
[ -z "$ELF" ]     || case "$ELF"     in /*) ;; *) ELF="$PWD/$ELF" ;; esac
[ -z "$CONSOLE" ] || case "$CONSOLE" in /*) ;; *) CONSOLE="$PWD/$CONSOLE" ;; esac
[ -z "$RUN_DIR" ] || case "$RUN_DIR" in /*) ;; *) RUN_DIR="$PWD/$RUN_DIR" ;; esac

die() {  # rc reason
    local rc="$1"; shift
    printf '{\n  "harness": "crash",\n  "ok": false,\n  "error": "%s",\n' "$*"
    printf '  "image": "%s",\n  "elf": "%s"\n}\n' "$IMAGE" "$ELF"
    echo "crash: $*" >&2
    exit "$rc"
}

# ---------------------------------------------------------------- get a console
if [ -z "$CONSOLE" ]; then
    if [ -n "$RUN_DIR" ]; then
        CONSOLE="$RUN_DIR/console.log"
        [ -f "$CONSOLE" ] || die 2 "no console.log in --run-dir $RUN_DIR"
    else
        # No captured run given: produce one.
        [ -f "$IMAGE" ] || die 2 "image not found: $IMAGE"
        RUN_DIR="$RUNROOT/crash-$(basename "${IMAGE%.*}")-$(date +%Y%m%d-%H%M%S)-$$"
        mkdir -p "$RUN_DIR"
        case "$IMAGE" in
            *.cdi|*.gdi|*.chd|*.cue) EXTRA+=(-c config:FastGDRomLoad=yes) ;;
        esac
        set +e
        "$RUNNER" "$IMAGE" --run-dir "$RUN_DIR" --timeout "$TIMEOUT" \
            ${EXTRA[@]+"${EXTRA[@]}"} >/dev/null 2>"$RUN_DIR/runner-stderr.log"
        set -e
        CONSOLE="$RUN_DIR/console.log"
    fi
fi
[ -f "$CONSOLE" ] || die 2 "console log not found: $CONSOLE"

# -------------------------------------------------------------------- find ELF
SIDECAR=""
ELF_VERIFIED="not-checked"
if [ -z "$ELF" ]; then
    case "$IMAGE" in
        *.elf) ELF="$IMAGE"; ELF_VERIFIED="image-is-elf" ;;
        *)
            SIDECAR="$IMAGE.src.json"
            [ -f "$SIDECAR" ] || die 2 \
"no ELF provenance for $IMAGE: expected sidecar $SIDECAR. Every CDI producer must write <image>.src.json (see harness/dc/selftest/build.sh header). Rebuild the image, or pass --elf explicitly."
            ELF="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["elf"])' "$SIDECAR")"
            WANT_SHA="$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["elf_sha256"])' "$SIDECAR")"
            [ -f "$ELF" ] || die 2 \
"sidecar $SIDECAR points at $ELF, which does not exist. Rebuild the image, or pass --elf."
            GOT_SHA="$(shasum -a 256 "$ELF" | awk '{print $1}')"
            if [ "$GOT_SHA" != "$WANT_SHA" ]; then
                die 2 \
"$ELF has changed since $IMAGE was built (sha256 $GOT_SHA != recorded $WANT_SHA). Line numbers from it would be WRONG. Rebuild the CDI from the current ELF, or pass --elf explicitly if you know better."
            fi
            ELF_VERIFIED="sha256-ok"
            ;;
    esac
else
    ELF_VERIFIED="explicit-override"
fi
[ -f "$ELF" ] || die 2 "ELF not found: $ELF"

case "$ELF" in
    /private/tmp/*|/tmp/*)
        die 2 "ELF is under /tmp ($ELF); colima cannot bind-mount that on this host. Move it under \$HOME." ;;
esac

# --------------------------------------------------------------- parse the dump
[ -n "$RUN_DIR" ] || RUN_DIR="$(dirname "$CONSOLE")"
DUMPJSON="$RUN_DIR/crash-dump.json"
SYMS="$RUN_DIR/crash-symbols.txt"

python3 - "$CONSOLE" > "$DUMPJSON" <<'PY'
import json, re, sys
lines = open(sys.argv[1], errors="replace").read().splitlines()

exc = re.compile(r"Unhandled exception:\s*PC\s+([0-9a-fA-F]+),\s*code\s+(-?\d+),\s*evt\s+([0-9a-fA-F]+)")
r07 = re.compile(r"^\s*R0-R7:\s*(.*)$")
r815 = re.compile(r"^\s*R8-R15:\s*(.*)$")
srpr = re.compile(r"^\s*SR\s+([0-9a-fA-F]+)\s+PR\s+([0-9a-fA-F]+)")
stk = re.compile(r"^\s+([0-9a-fA-F]{8})\s*$")
kind = re.compile(r"Encountered\s+(.*?)\.\s")
# NB: KOS wraps the function name in a backtick-quote pair. A literal backtick
# cannot appear here -- this heredoc used to sit inside $( ), where bash reads
# one as legacy command substitution. Built from chr(96) so it can never bite
# again if this block is moved.
asrt = re.compile('Assertion\\s+"(.*)"\\s+failed at\\s+(\\S+):(\\d+)\\s+in\\s+'
                  + chr(96) + "(.*)'")
panic = re.compile(r"kernel panic:\s*(.*)$")

d = {"pc": None, "pr": None, "sr": None, "evt": None, "code": None,
     "exception": None, "registers": {}, "stack": [], "assertion": None,
     "panic": None, "frame_pointers": True, "kind": None}

in_stack = False
for ln in lines:
    m = exc.search(ln)
    if m:
        d["pc"], d["code"], d["evt"] = m.group(1).lower(), int(m.group(2)), m.group(3).lower()
        d["kind"] = "exception"
        continue
    m = r07.match(ln)
    if m:
        for i, v in enumerate(m.group(1).split()):
            d["registers"]["r%d" % i] = v.lower()
        continue
    m = r815.match(ln)
    if m:
        for i, v in enumerate(m.group(1).split()):
            d["registers"]["r%d" % (i + 8)] = v.lower()
        continue
    m = srpr.match(ln)
    if m:
        d["sr"], d["pr"] = m.group(1).lower(), m.group(2).lower()
        continue
    m = kind.search(ln)
    if m:
        d["exception"] = m.group(1)
        continue
    m = asrt.search(ln)
    if m:
        d["assertion"] = {"expr": m.group(1), "file": m.group(2),
                          "line": int(m.group(3)), "func": m.group(4)}
        d["kind"] = d["kind"] or "assertion"
        continue
    m = panic.search(ln)
    if m:
        d["panic"] = m.group(1).strip()
        continue
    if "frame pointers not enabled" in ln:
        d["frame_pointers"] = False
        continue
    if "Stack Trace (innermost first)" in ln:
        in_stack = True
        continue
    if "End Stack Trace" in ln:
        in_stack = False
        continue
    if in_stack:
        m = stk.match(ln)
        if m:
            d["stack"].append(m.group(1).lower())

# Addresses to symbolise, in report order, de-duplicated but order-preserving.
addrs, seen = [], set()
for a in [d["pc"], d["pr"]] + d["stack"]:
    if a and a not in seen:
        seen.add(a)
        addrs.append(a)
d["addrs"] = addrs
d["found"] = bool(d["pc"] or d["assertion"] or d["panic"])
json.dump(d, sys.stdout, indent=1)
PY

jqp() { python3 -c 'import json,sys;d=json.load(open(sys.argv[1]));print(sys.argv[2].join(d[sys.argv[3]]) if isinstance(d[sys.argv[3]],list) else (d[sys.argv[3]] if d[sys.argv[3]] is not None else ""))' "$DUMPJSON" "$1" "$2"; }

FOUND="$(jqp ' ' found)"
if [ "$FOUND" != "True" ]; then
    printf '{\n  "harness": "crash",\n  "ok": false,\n  "found": false,\n'
    printf '  "console": "%s",\n  "image": "%s",\n' "$CONSOLE" "$IMAGE"
    printf '  "note": "no KOS register dump, assertion or panic in this console log"\n}\n'
    echo "crash: no crash dump in $CONSOLE" >&2
    exit 1
fi

# ------------------------------------------------------- symbolise in container
ADDRS="$(jqp ' ' addrs)"
PC="$(jqp ' ' pc)"

ELF_DIR="$(dirname "$ELF")"
ELF_BASE="$(basename "$ELF")"

command -v docker >/dev/null 2>&1 || die 2 "docker not found; cannot run $TOOLCHAIN for symbolisation"

# One container invocation for everything: per-address addr2line (-i so inlined
# frames appear), delimited by ===ADDR lines so it parses back, plus an objdump
# window around the faulting PC. Values go in as env vars rather than being
# spliced into the script text -- no quoting minefield.
#
# Two container gotchas, both found the hard way:
#   * `bash -c`, NOT `bash -lc`. A login shell re-runs /etc/profile, which
#     rebuilds PATH and drops the toolchain bin dir; sh-elf-* then comes back
#     "command not found" and every address silently symbolises to "??".
#     opencrossing-dc:sdk already bakes the toolchain into the image ENV.
#   * Do NOT source environ.sh here. Under `set -u` it dies on an unbound
#     variable and takes the whole shell with it -- exit 127, no stderr.
CSCRIPT='set -u
command -v sh-elf-addr2line >/dev/null || { echo "no sh-elf-addr2line in image" >&2; exit 3; }
elf="/elf/$OC_ELF"
for a in $OC_ADDRS; do
    echo "===ADDR 0x$a"
    sh-elf-addr2line -f -C -i -e "$elf" "0x$a"
done
echo "===DISASM"
if [ -n "$OC_PC" ]; then
    pc=$((0x$OC_PC))
    # objdump lines start at column 0 with the address ("8c010112:\t..." or
    # "8c0100fa <main+0x1a>:"), so filter on that, not on leading whitespace.
    sh-elf-objdump -d -C --start-address=$((pc-OC_CTX)) --stop-address=$((pc+OC_CTX)) \
        "$elf" | grep -E "^[0-9a-f]+[ :]" || true
fi'
CSCRIPT="${CSCRIPT//OC_CTX/$CONTEXT}"

set +e
docker run --rm --platform linux/arm64 -v "$ELF_DIR":/elf:ro \
    -e OC_ELF="$ELF_BASE" -e OC_ADDRS="$ADDRS" -e OC_PC="$PC" \
    "$TOOLCHAIN" bash -c "$CSCRIPT" \
    > "$SYMS" 2>"$RUN_DIR/crash-docker-stderr.log"
drc=$?
set -e
[ "$drc" -eq 0 ] || die 2 "toolchain container failed (rc=$drc); see $RUN_DIR/crash-docker-stderr.log"

python3 - "$DUMPJSON" "$SYMS" "$ELF" "$ELF_VERIFIED" "$SIDECAR" "$CONSOLE" "$IMAGE" <<'PY'
import json, sys

dump_path, syms_path, elf, verified, sidecar, console, image = sys.argv[1:8]
d = json.load(open(dump_path))
text = open(syms_path, errors="replace").read()

# addr2line -f -C -i emits pairs of lines (symbol, file:line) per inline frame.
sym_blocks, disasm = {}, []
cur = None
for ln in text.splitlines():
    if ln.startswith("===ADDR "):
        cur = ln.split()[1].lower()
        sym_blocks[cur] = []
    elif ln == "===DISASM":
        cur = "__disasm__"
    elif cur == "__disasm__":
        # Point at the faulting instruction so nobody has to eyeball addresses.
        mark = ">>" if d["pc"] and ln.lower().startswith(d["pc"] + ":") else "  "
        disasm.append(mark + " " + ln.expandtabs(8).rstrip())
    elif cur:
        sym_blocks[cur].append(ln)

def frames_for(addr):
    out, raw = [], sym_blocks.get("0x" + addr, [])
    for i in range(0, len(raw) - 1, 2):
        func, loc = raw[i].strip(), raw[i + 1].strip()
        f, _, l = loc.rpartition(":")
        out.append({
            "symbol": func if func != "??" else None,
            "file": f if f not in ("", "??") else None,
            "line": int(l) if l.isdigit() else None,
            "inlined": i > 0,
        })
    return out

def label(fr):
    if not fr:
        return "?? (no debug info)"
    f = fr[0]
    where = "%s:%s" % (f["file"], f["line"]) if f["file"] else "??"
    return "%s at %s" % (f["symbol"] or "??", where)

pc_frames = frames_for(d["pc"]) if d["pc"] else []
pr_frames = frames_for(d["pr"]) if d["pr"] else []
stack = [{"addr": a, "frames": frames_for(a), "label": label(frames_for(a))}
         for a in d["stack"]]

out = {
    "harness": "crash",
    "ok": True,
    "found": True,
    "kind": d["kind"],
    "image": image,
    "console": console,
    "elf": elf,
    "elf_verified": verified,
    "sidecar": sidecar or None,
    "exception": d["exception"],
    "panic": d["panic"],
    "assertion": d["assertion"],
    "pc": d["pc"], "pr": d["pr"], "sr": d["sr"],
    "evt": d["evt"], "code": d["code"],
    "fault": label(pc_frames),
    "caller": label(pr_frames) if d["pr"] else None,
    "pc_frames": pc_frames,
    "pr_frames": pr_frames,
    "registers": d["registers"],
    "stack": stack,
    "frame_pointers": d["frame_pointers"],
    "disasm": disasm,
    "symbols_raw": syms_path,
}
json.dump(out, sys.stdout, indent=1)
sys.stdout.write("\n")

# Human summary on stderr -- the one-line answer to "where did it die".
e = d["exception"] or d["panic"] or "crash"
sys.stderr.write("CRASH %s\n  fault : PC %s  %s\n" % (e, d["pc"], label(pc_frames)))
if d["assertion"]:
    a = d["assertion"]
    sys.stderr.write("  assert: %s failed at %s:%d in %s\n"
                     % (a["expr"], a["file"], a["line"], a["func"]))
for fr in stack:
    sys.stderr.write("  stack : %s  %s\n" % (fr["addr"], fr["label"]))
if not d["frame_pointers"]:
    sys.stderr.write("  note  : build with -fno-omit-frame-pointer for a real stack trace\n")
PY
