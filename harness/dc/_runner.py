#!/usr/bin/env python3
"""Marker-driven, time-bounded Flycast supervisor.

Internal helper shared by every harness/dc/*.sh script. Not meant to be called
directly by agents -- use run-flycast.sh / smoke.sh / console.sh / screenshot.sh,
which have stable CLIs and stable JSON shapes.

Why this exists instead of `timeout flycast ...`:
  * macOS has no timeout(1)/gtimeout(1) here (coreutils not installed).
  * Flycast never exits on its own -- not when the guest returns from main, not
    on a KOS kernel panic. It sits in its window forever. The only way to end a
    run deterministically is to watch the guest's serial output for a marker and
    kill the process group.
  * There is no headless mode (SDL_VIDEODRIVER=dummy/offscreen both abort with
    exit code 6), so a window WILL open for ~1-2 s per run. Unavoidable.

Channels (verified against Flycast v2.6 / bundle 392a429e8):
  guest SCIF serial -> host STDOUT   (Debug.SerialConsoleEnabled=yes)
  Flycast's own log -> host STDERR   (silenced with log:LogToConsole=no)
They never mix, so they are captured to separate files.
"""

import argparse
import errno
import json
import os
import re
import signal
import subprocess
import sys
import time

FLYCAST = os.environ.get(
    "OC_DC_FLYCAST", "/Applications/Flycast.app/Contents/MacOS/Flycast"
)

# Config passed on EVERY run. Dreamcast.RamMod32MB=no is not optional: the
# 16 MB hardware contract must be enforced mechanically, not remembered.
BASE_CONFIG = [
    "config:Debug.SerialConsoleEnabled=yes",
    "config:UseReios=yes",
    "config:Dreamcast.RamMod32MB=no",
    "log:LogToConsole=no",
]

DEFAULT_END_MARKER = r"OC-DC-HARNESS-END"
# Every one of these was observed coming out of KOS 2.3.0 on Flycast v2.6.
# `ASSERTION FAILURE` and `arch: aborting the system` are NOT optional: KOS 2.3
# prints `*** ASSERTION FAILURE ***` / `Assertion "x" failed at f.c:9` (capital
# A) and then aborts, and without those two alternatives a failing assert is
# only ever caught by the wall-clock timeout.
DEFAULT_FAIL_MARKER = (
    r"(kernel panic|Unhandled exception|ASSERTION FAILURE|[Aa]ssertion \"|"
    r"arch: aborting the system|"
    r"Failed to open ELF|Failed to locate bootfile|ASSERT fail)"
)
BOOT_MARKER = r"KallistiOS |OC-DC-HARNESS-BEGIN"

REC_RE = {
    "mark": re.compile(r"^MARK:(\S+)(?:\s+(.*))?$"),
    "perf": re.compile(r"^PERF\s+(.*)$"),
    "assert": re.compile(r"^ASSERT\s+(ok|fail)\s+(\S+)"),
    "fbhash": re.compile(r"^FBHASH\s+([0-9a-fA-F]+)$"),
    "fbthumb": re.compile(r"^FBTHUMB\s+(\d+)x(\d+)\s+(\S+)$"),
    "mem": re.compile(r"^MEM\s+(.*)$"),
}
END_RC_RE = re.compile(r"OC-DC-HARNESS-END\s+rc=(-?\d+)")
# Two banner formats in the wild:
#   KOS 2.3.0 (GCC 15 SDK image):  "KallistiOS v2.3.0 [dreamcast/pristine]"
#                                  "  Git revision: 1c6398f"
#   KOS 2021 (einsteinx2 image):   "KallistiOS Git revision 525cbda:"
KOS_REV_RE = re.compile(r"[Gg]it revision:?\s+(\S+?):?$")
KOS_VER_RE = re.compile(r"^KallistiOS\s+(\S+)")
MAPLE_RE = re.compile(r"^\s+([A-D]\d):\s+(.*?)\s{2,}\((\S+):\s*(.*)\)")


def build_argv(image, extra_config):
    argv = [FLYCAST]
    for c in BASE_CONFIG + list(extra_config):
        argv += ["-config", c]
    argv.append(image)
    return argv


def run(args):
    run_dir = args.run_dir
    home = os.path.join(run_dir, "home")
    # Flycast mkdir()s its data dir NON-recursively, so $HOME/.flycast must
    # already exist or the run silently falls back to the real ~/Library.
    os.makedirs(os.path.join(home, ".flycast"), exist_ok=True)
    console_path = os.path.join(run_dir, "console.log")
    stderr_path = os.path.join(run_dir, "flycast-stderr.log")

    if not os.path.exists(FLYCAST):
        return {
            "status": "launch_error",
            "error": "Flycast not installed at %s -- run harness/dc/install-flycast.sh"
            % FLYCAST,
        }, None
    if not os.path.exists(args.image):
        return {
            "status": "launch_error",
            "error": "image not found: %s" % args.image,
        }, None

    argv = build_argv(os.path.abspath(args.image), args.config)
    env = dict(os.environ, HOME=home)

    end_re = re.compile(args.end_marker)
    fail_re = re.compile(args.fail_marker) if args.fail_marker else None
    boot_re = re.compile(BOOT_MARKER)

    t0 = time.time()
    errf = open(stderr_path, "wb")
    try:
        proc = subprocess.Popen(
            argv,
            stdout=subprocess.PIPE,
            stderr=errf,
            env=env,
            start_new_session=True,  # own process group -> killpg is clean
            bufsize=0,
        )
    except OSError as e:
        errf.close()
        return {"status": "launch_error", "error": str(e)}, None

    os.set_blocking(proc.stdout.fileno(), False)

    lines = []
    stamps = []           # arrival time (s since launch) per line
    status = "timeout"
    buf = b""
    drain_until = None
    booted = False
    marker_seen = False

    while True:
        now = time.time()
        if drain_until is None:
            if now - t0 >= args.timeout:
                break
        elif now >= drain_until:
            break

        try:
            chunk = proc.stdout.read(65536)
        except OSError as e:
            if e.errno != errno.EAGAIN:
                raise
            chunk = None

        if chunk:
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                s = raw.decode("utf-8", "replace").rstrip("\r")
                lines.append(s)
                stamps.append(round(time.time() - t0, 3))
                if args.tee:
                    # stderr, never stdout -- stdout must stay pure JSON
                    sys.stderr.write(s + "\n")
                    sys.stderr.flush()
                if boot_re.search(s):
                    booted = True
                if drain_until is not None:
                    continue
                if fail_re is not None and fail_re.search(s):
                    status = "fail_marker"
                    # keep draining so the whole KOS register dump lands in the
                    # log -- it is what sh-elf-addr2line needs
                    drain_until = time.time() + args.fail_drain
                elif end_re.search(s):
                    status = "ok"
                    marker_seen = True
                    drain_until = time.time() + args.end_drain
        else:
            if proc.poll() is not None:
                if status == "timeout":
                    status = "exited_early"
                break
            time.sleep(0.01)

    elapsed = round(time.time() - t0, 3)

    try:
        os.killpg(os.getpgid(proc.pid), signal.SIGKILL)
    except Exception:
        pass
    try:
        proc.wait(timeout=5)
    except Exception:
        pass
    try:
        proc.stdout.close()
    except Exception:
        pass
    errf.close()

    if buf:  # trailing partial line
        lines.append(buf.decode("utf-8", "replace").rstrip("\r"))
        stamps.append(elapsed)

    with open(console_path, "w") as f:
        f.write("\n".join(lines) + ("\n" if lines else ""))

    # --- parse typed records ------------------------------------------------
    records = {k: [] for k in REC_RE}
    for i, s in enumerate(lines):
        for kind, rx in REC_RE.items():
            m = rx.match(s)
            if not m:
                continue
            rec = {"t": stamps[i], "line": i}
            if kind == "mark":
                rec["name"] = m.group(1)
                rec["arg"] = m.group(2)
            elif kind == "assert":
                rec["result"] = m.group(1)
                rec["name"] = m.group(2)
            elif kind == "fbhash":
                rec["hash"] = m.group(1).lower()
            elif kind == "fbthumb":
                rec["w"] = int(m.group(1))
                rec["h"] = int(m.group(2))
                rec["b64"] = m.group(3)
            else:
                rec["text"] = m.group(1)
                rec["kv"] = dict(
                    p.split("=", 1) for p in m.group(1).split() if "=" in p
                )
            records[kind].append(rec)
            break

    end_rc = None
    for s in lines:
        m = END_RC_RE.search(s)
        if m:
            end_rc = int(m.group(1))

    kos_rev = None
    kos_ver = None
    maple = []
    for s in lines:
        m = KOS_REV_RE.search(s)
        if m and kos_rev is None:
            kos_rev = m.group(1).rstrip(":")
        m = KOS_VER_RE.match(s)
        if m and kos_ver is None:
            kos_ver = m.group(1)
        m = MAPLE_RE.match(s)
        if m:
            maple.append({"port": m.group(1), "name": m.group(2).strip(),
                          "id": m.group(3), "caps": m.group(4)})

    failed_asserts = [r["name"] for r in records["assert"] if r["result"] == "fail"]
    if status == "ok" and (end_rc not in (None, 0) or failed_asserts):
        status = "fail_marker"

    result = {
        "status": status,
        "booted": booted,
        "marker_seen": marker_seen,
        "end_rc": end_rc,
        "elapsed_s": elapsed,
        "timeout_s": args.timeout,
        "console": console_path,
        "flycast_stderr": stderr_path,
        "run_dir": run_dir,
        "image": os.path.abspath(args.image),
        "line_count": len(lines),
        "kos_version": kos_ver,
        "kos_revision": kos_rev,
        "maple": maple,
        "failed_asserts": failed_asserts,
        "records": records,
        "argv": argv,
    }
    return result, lines


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("image", help=".elf (reios direct load) or .cdi/.gdi/.chd/.cue")
    ap.add_argument("--run-dir", required=True)
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--end-marker", default=DEFAULT_END_MARKER)
    ap.add_argument("--fail-marker", default=DEFAULT_FAIL_MARKER)
    ap.add_argument("--fail-drain", type=float, default=1.0)
    ap.add_argument("--end-drain", type=float, default=0.15)
    ap.add_argument("-c", "--config", action="append", default=[])
    ap.add_argument("--tee", action="store_true",
                    help="echo guest serial lines to stderr as they arrive")
    ap.add_argument("--emit-lines", action="store_true",
                    help="include the full captured console in the JSON")
    a = ap.parse_args()

    os.makedirs(a.run_dir, exist_ok=True)
    result, lines = run(a)
    if a.emit_lines:
        result["lines"] = lines or []
    out = os.path.join(a.run_dir, "runner.json")
    with open(out, "w") as f:
        json.dump(result, f, indent=1)
    print(json.dumps(result))
    sys.exit(0 if result.get("status") == "ok" else 1)


if __name__ == "__main__":
    main()
