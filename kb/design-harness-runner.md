# Flycast harness — the marker-driven runner and the five harnesses

Timeout/exit handling and the reference Python runner (§5), then the shared
contract and the five harnesses `smoke` / `console` / `screenshot` / `perf` /
`crash` (§8). Read before writing or changing anything in `harness/dc/`.
Part of `kb/design-harness.md`, whose stub maps every § to its file.

Recon pass 2026-08-01. Everything below is tagged **[V]** = verified by running
it on this machine today, or **[U]** = unverified / inferred from source only.
Where something did not work, it says so.

⚠️ §12 and §13 (`kb/design-harness-corrections.md`) were written later
the same day against the real `opencrossing-dc:sdk` image and correct
parts of this file — read them too.
Specifically: the §5 fail-marker regex misses KOS assertion failures (§12.3),
CDI boot is now verified (§12.1), and §8d's "exactly reproducible" timing
needs a tolerance band (§13.4).

## 5. Timeout / exit handling

- **[V]** Flycast **does not exit** when the guest program returns from `main`
  or when KOS panics. It sits in its window forever. There is no
  exit-on-condition flag in this build (`flycast.exit()` exists only in the
  Lua API, which is not compiled in — §7).
- **[V]** macOS has **no `timeout`/`gtimeout`** by default here (`coreutils`
  not installed). Don't write harness scripts that assume it.
- **[V] Solution: a marker-driven supervisor.** Read the child's stdout line
  by line; kill the process group the moment an end-marker or a failure marker
  appears, or when a wall-clock deadline expires. Verified all three exits:

| scenario | status | wall clock |
|---|---|---|
| program prints `OC-DC-HARNESS-END` | `ok`, rc 0 | **~1 s** |
| program hits `Unhandled exception` / `kernel panic` | `fail`, rc 1 | ~1 s |
| marker never appears | `timeout`, rc 1 | exactly the deadline |

Reference implementation used for all measurements in this document (lift it
into `harness/dc/` — it is not committed anywhere yet):

```python
#!/usr/bin/env python3
"""Marker-driven, time-bounded Flycast runner. Emits JSON on stdout."""
import json, os, re, signal, subprocess, sys, time
FLYCAST = "/Applications/Flycast.app/Contents/MacOS/Flycast"

def main():
    import argparse
    ap = argparse.ArgumentParser()
    ap.add_argument("image")                       # .elf or .cdi
    ap.add_argument("--timeout", type=float, default=60.0)
    ap.add_argument("--end-marker",  default=r"OC-DC-HARNESS-END")
    ap.add_argument("--fail-marker", default=r"(kernel panic|Unhandled exception|assertion)")
    ap.add_argument("--home", required=True)       # per-run isolated HOME
    ap.add_argument("--log",  required=True)
    ap.add_argument("-c", "--config", action="append", default=[])
    a = ap.parse_args()

    os.makedirs(os.path.join(a.home, ".flycast"), exist_ok=True)
    env = dict(os.environ, HOME=a.home)
    cfg = ["config:Debug.SerialConsoleEnabled=yes",
           "config:UseReios=yes",
           "config:Dreamcast.RamMod32MB=no",
           "log:LogToConsole=no"] + a.config
    argv = [FLYCAST]
    for c in cfg:
        argv += ["-config", c]
    argv += [a.image]

    t0 = time.time()
    p = subprocess.Popen(argv, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         env=env, start_new_session=True, bufsize=0)
    end_re, fail_re = re.compile(a.end_marker), re.compile(a.fail_marker)
    os.set_blocking(p.stdout.fileno(), False)
    lines, status, buf, drain_until = [], "timeout", b"", None
    while True:
        now = time.time()
        if drain_until is None and now - t0 >= a.timeout: break
        if drain_until is not None and now >= drain_until: break
        chunk = p.stdout.read(4096)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                ln, buf = buf.split(b"\n", 1)
                s = ln.decode("utf-8", "replace").rstrip("\r")
                lines.append(s)
                if drain_until is None and fail_re.search(s):
                    status = "fail"
                    drain_until = time.time() + 1.0   # keep the register dump
                elif drain_until is None and end_re.search(s):
                    status = "ok"; drain_until = time.time() + 0.1
        else:
            if p.poll() is not None: 
                if status == "timeout": status = "exited"
                break
            time.sleep(0.02)
    try: os.killpg(os.getpgid(p.pid), signal.SIGKILL)
    except Exception: pass
    p.wait()
    open(a.log, "w").write("\n".join(lines) + "\n")
    print(json.dumps({"status": status, "lines": len(lines), "log": a.log,
                      "markers": [l for l in lines
                                  if l.startswith(("MARK:", "PERF ", "FBHASH ", "MEM "))]},
                     indent=1))
    sys.exit(0 if status == "ok" else 1)
main()
```

The one refinement over what was measured: on a failure marker, keep draining
for ~1 s so the whole KOS register dump makes it into the log (the version
tested cut off at the first line).

---

## 8. The harnesses

Shared contract for every harness, so agents can treat them uniformly:

- Non-interactive, hard wall-clock bound, **JSON on stdout**, exit 0 only on
  pass.
- Per-run scratch dir `$RUNROOT/<name>-<timestamp>/` containing `.flycast/`
  (the isolated `HOME`), `console.log`, `result.json`.
- Guest programs bracket their output with `OC-DC-HARNESS-BEGIN` /
  `OC-DC-HARNESS-END rc=<n>`, and emit typed one-line records:
  `MARK:<name>`, `PERF <k>=<v> …`, `FBHASH <hex>`, `FBTHUMB16x12 <b64>`,
  `MEM <k>=<v> …`, `ASSERT <ok|fail> <name>`.
- Every harness asserts `Dreamcast.RamMod32MB=no` in its own config list —
  the 16 MB contract must be mechanically enforced, not remembered.
- Nothing is written into the repo; disc images and ELFs live outside it.

### (a) `smoke` — boot and reach a marker

`harness/dc/smoke.sh [--elf PROG | --cdi DISC]`

- Runner from §5, end-marker `MARK:BOOT_OK`, fail-marker
  `(kernel panic|Unhandled exception|Failed to (open ELF|locate bootfile))`,
  timeout 60 s.
- Assertions from the free KOS banner already on stdout: KOS revision line
  present, `maple: attached devices` lists `A0: Dreamcast Controller`.
- **[V]** ELF mode works today, ~1 s per run.
- **[U]** CDI mode is untested — no CDI exists yet and the toolchain image
  used here has no `mkdcdisc` (see §10). Flycast accepts `.cdi/.chd/.gdi/.cue`
  and boots via `IP.BIN`'s `boot_filename` (`reios_locate_bootfile`), so this
  is expected to work; add `-config config:FastGDRomLoad=yes` to skip modelled
  GD-ROM latency for CI, and *omit* it for the read-ahead/streaming tests where
  the latency is the thing under test.

### (b) `console` — run a program, return its stdout

`harness/dc/console.sh PROG.elf [--timeout N]` → JSON with the full captured
log plus the parsed `MARK:`/`PERF`/`ASSERT` records.

- **[V]** This is the workhorse and it is fully proven. It is how any unit
  test of ported game code (checksum a decoded texture, walk a heap, byte-swap
  a struct, exercise `pc_save_bswap`) reports its result.
- The guest test main() should raise the SCIF baud first (§3) and print a
  compact summary rather than a stream.

### (c) `screenshot` — capture frame N

`harness/dc/screenshot.sh PROG.elf --frame N`

- **[V]** Guest-side only: the program renders, waits N vblanks
  (`vid_waitvbl()` / PVR frame callback), then emits `FBHASH` and
  `FBTHUMB16x12` and `MARK:SHOT_N`. Harness compares `FBHASH` against a
  golden value stored in the repo (a hex string, not an image), and on
  mismatch dumps both thumbnails for a human/agent to eyeball.
- Full-frame base64 dump is the escape hatch when the hash goes red and
  someone needs to actually look at it.
- **Do not** attempt `screencapture` or F12 — both blocked (§4).

### (d) `perf` — frame timings

`harness/dc/perf.sh PROG.elf --frames N`

- **[V]** Guest measures itself with `timer_us_gettime64()`; accumulate
  per-frame deltas into a fixed array, print **one** `PERF` line per phase at
  the end (never per frame — serial cost, §3). Report min/mean/p95/max plus a
  breakdown (`PERF frame=… logic=… tnl=… submit=… wait=…`).
- **[V] Guest-measured emulated time is exactly reproducible** — a 1 000 000
  iteration loop measured `35087 us` in every single run, including three
  simultaneous instances. That makes this a real regression gate (fail the
  build if `logic_us` regresses > X%), not a noisy benchmark.
- **[U] It is emulated time, not silicon time.** Flycast's SH-4 timing is
  scheduler-driven and models no cache, no bus contention, no store-queue
  stalls. Treat the numbers as **relative** — good for "did this change make it
  worse", worthless as an absolute answer to the M3 "≤ 25 ms" gate. That gate
  needs hardware, and `PLAN.md` §3.2 should be read with that caveat.
- Host wall-clock timing of the Flycast process is meaningless (it runs faster
  than real time and blocks on serial); never use it.

### (e) `crash` — catch and report an exception

`harness/dc/crash.sh PROG.elf`

- **[V] Primary path (works now, no GDB):** run with the fail-marker set,
  capture the KOS `Unhandled exception: PC … / R0-R7 … / SR … PR …` block, then
  post-process on the host: `sh-elf-addr2line -f -C -e PROG.elf <PC>` and
  `<PR>`, plus `sh-elf-objdump -d` around the PC. Emit JSON
  `{pc, pr, sr, evt, symbol, file, line, disasm}`.
- **[U] GDB path is unavailable** until a from-source Flycast with
  `ENABLE_GDB_SERVER=ON` exists (§7). When it does:
  `-config config:Debug.GDBEnabled=yes -config config:Debug.GDBPort=3263
  -config config:Debug.GDBWaitForConnection=yes`, then drive
  `sh-elf-gdb -batch -ex 'target remote :3263' -ex 'bt' -ex 'info registers'`.
  `sh-elf-gdb` is already present inside the KOS toolchain container.
- **[U] Alternative worth trying first, because it needs no Flycast rebuild:**
  KOS's own GDB stub (`gdb_init()`) speaks the RSP over SCIF. Combine it with
  `Debug.SerialPTY=yes` (§3) and point `sh-elf-gdb` at `/dev/ttysNNN`. That
  gives symbol-level, KOS-aware debugging of the guest with the stock binary.
  Not attempted.
- **[V] Do not expect alignment faults to show up here at all** (§6).

---
