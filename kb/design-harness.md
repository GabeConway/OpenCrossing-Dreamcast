# Emulator test-harness design (macOS host, Flycast)

Recon pass 2026-08-01. Everything below is tagged **[V]** = verified by running
it on this machine today, or **[U]** = unverified / inferred from source only.
Where something did not work, it says so.

**Bottom line: console capture is GO.** A KOS `printf` inside the emulated
Dreamcast lands on the host process's **stdout**, in real time, with no
permissions, no GUI interaction and no extra tooling. Round-trip
"build ELF → boot in Flycast → read program output → exit" measured at
**~1 second wall clock**, bit-for-bit deterministic across runs and across
parallel instances. Agents can self-test.

---

## 1. Install (verified)

```bash
brew install --cask flycast          # installs /Applications/Flycast.app (v2.6)
xattr -dr com.apple.quarantine /Applications/Flycast.app   # REQUIRED
```

- **[V]** Installed version **v2.6**, `CFBundleVersion 392a429e8` — which is
  exactly the upstream git tag `v2.6` (`392a429e8`), so upstream source at
  `https://github.com/flyinghead/flycast/tree/v2.6` matches the binary. Read
  that tag, not `master`; `master` has already renamed config keys (e.g.
  `bios.UseReios` → `UseReios`).
- **[V]** Universal binary (x86_64 + arm64), ad-hoc signed, **no Team ID**.
  The cask is *deprecated in Homebrew* ("does not pass the macOS Gatekeeper
  check", scheduled for disable **2026-09-01**). It installs and runs fine
  today, but pin a copy of the `.app` / the release DMG somewhere before that
  date, or switch to the GitHub release artifact directly.
- **[V]** Because it is ad-hoc signed, the quarantine xattr must be stripped
  or every launch is blocked. Do this once, not per-run.
- **[V]** No BIOS ROM (`dc_boot.bin`) is needed — Flycast falls back to its
  HLE BIOS (reios).

Config/data lives at (`shell/apple/emulator-osx/emulator-osx/osx-main.mm`):

- **[V]** `$HOME/.flycast/` **if that directory already exists**, otherwise
  `$HOME/Library/Application Support/Flycast/`.
- **[V]** It `mkdir()`s (non-recursively) the chosen dir, then `data/` inside
  it. So overriding `HOME` to an empty dir *fails silently* (can't create
  `…/Library/Application Support/`), but overriding `HOME` to a dir that
  already contains `.flycast/` gives a **fully hermetic run**: own `emu.cfg`,
  own `data/vmu_save_A1.bin`, own `dc_nvmem.bin`, own savestates.
- **[V]** This is how harness runs are isolated and made parallel-safe.
  3 concurrent runs completed in 2 s total with identical output.

---

## 2. Exact working invocation (verified)

```bash
RUN=/some/scratch/run-XYZ
mkdir -p "$RUN/.flycast"

env HOME="$RUN" /Applications/Flycast.app/Contents/MacOS/Flycast \
    -config config:Debug.SerialConsoleEnabled=yes \
    -config config:UseReios=yes \
    -config log:LogToConsole=no \
    /abs/path/to/program.elf
```

- **[V]** CLI grammar is `-config SECTION:KEY=VALUE` (comma-separated pairs
  allowed in one argument). Values are *transient* — never written back to
  `emu.cfg`. Most options live in section **`config`**; logging in **`log`**;
  window geometry in **`window`**; networking in `network`.
- **[V]** Content path is a bare positional argument. Extensions recognised as
  a disc: `.cdi .chd .gdi .cue`. Extension `.elf` takes the **reios direct-ELF
  path** (`core/reios/reios.cpp:658` → `reios_loadElf()` → `reios_setup_state(0x8C010000)`).
- **[V] Booting a raw KOS `.elf` works, with no CDI and no disc image at all.**
  This is the single biggest iteration win: the console/unit-test harnesses do
  not need `mkdcdisc` in the loop.
- **[V]** In v2.6 `cl.cpp` sets the *stale* key `config:bios.UseReios` for
  `.elf` content, which no longer matches the option (`UseReios`). It still
  booted here only because no BIOS ROM is installed. **Always pass
  `-config config:UseReios=yes` explicitly** so ELF boot keeps working if a
  real `dc_boot.bin` is ever dropped into the data dir.
- **[V]** `-config log:LogToConsole=no` silences Flycast's own logging.
  Flycast's logs go to **stderr**; the guest's serial output goes to
  **stdout** — they never mix. Keep them on separate fds.
- **[V]** `-config window:width=320,window:height=240` is accepted (section
  `window`, keys `width`/`height`/`fullscreen`/`maximized`/`left`/`top`).
  Actual on-screen size not visually confirmed.
- **[V] There is no headless mode.** `SDL_VIDEODRIVER=dummy` and `=offscreen`
  both abort with exit code 6. A window *will* open. With the marker-driven
  runner below it exists for ~1–2 s.
- **[V]** No `-help` option beyond `-help/--help`; there is no boot-to-game
  flag needed (content on the command line boots straight in, no GUI prompt).

Useful config keys confirmed present in the v2.6 binary
(`core/cfg/option.cpp`), all in section `config` unless noted:

| Key | Use |
|---|---|
| `Debug.SerialConsoleEnabled` | **the console channel** — SCIF TX → host stdout |
| `Debug.SerialPTY` | SCIF → a pty instead of stdout (bidirectional) |
| `Debug.GDBEnabled` / `Debug.GDBPort` / `Debug.GDBWaitForConnection` | present as options but **dead in this build** (§7) |
| `UseReios` | force HLE BIOS |
| `FastGDRomLoad` | skip GD-ROM read latency (use for disc harnesses) |
| `Dreamcast.RamMod32MB` | **must stay `no`** — assert it in every harness |
| `Dynarec.Enabled` | `no` → interpreter (slower, sometimes more accurate) |
| `rend.ThreadedRendering`, `rend.Resolution` | render tuning |
| `aica.DSPEnabled`, `aica.Volume` | audio |
| `Profiler.Enabled` / `Profiler.OutputTTY` | **no-ops** — the SH4 profiler is behind CMake `ENABLE_DC_PROFILER`, OFF in this build |
| `log:LogToConsole`, `log:LogToFile`, `log:Verbosity`, `log:LogServer` | Flycast-side logging; `LogToFile=yes` writes `flycast.log` into the data dir |

---

## 3. Console capture — the mechanism (VERIFIED, this is the GO)

### How it works

1. KOS registers dbgio handlers in `arch/dreamcast/kernel/init.c` (dcload,
   dcls, **scif**, null, fb) and calls `dbgio_dev_select_auto()`.
   `scif_detected()` unconditionally returns 1, and dcload/dcls are absent
   under Flycast, so **`scif` is auto-selected**. `printf` → newlib
   `_write_r` → `fs_write(1,…)` → KOS console pty (unattached) →
   `dbgio_write_buffer_xlat()` → SCIF.
2. Flycast v2.6 `core/hw/sh4/modules/serial.cpp` installs a `PTYPipe` whose
   `write()` does `::write(tty, &data, 1)` with `tty == 1` (**stdout**) when
   `Debug.SerialConsoleEnabled` is on and `Debug.SerialPTY` is off.

### Verified output

Building a stock KOS hello-world and running the invocation in §2 produced on
**stdout**:

```
--
KallistiOS Git revision 525cbda:
…
maple: attached devices:
  A0: Dreamcast Controller          (01000000: Controller)
OC-DC-HARNESS-BEGIN
dbgio_dev=scif
RAW-DBGIO-LINE
TICK 0 … TICK 4
MARK:BOOT_OK
OC-DC-HARNESS-END rc=0
arch: shutting down kernel
```

**[V]** Both `printf()` and `dbgio_write_str()` arrive. **[V]** The KOS banner
and maple enumeration arrive too — free boot diagnostics.
**[V]** Guest programs should still call `dbgio_dev_select("scif")` explicitly
at startup so the channel can't be stolen by a future `fb`/dcload selection.

### Serial throughput is *emulated-baud limited* — and this matters [V]

Measured inside the guest with `timer_us_gettime64()`:

| SCIF baud | 7200 bytes of `printf` cost, in **emulated** µs |
|---|---|
| KOS default (57 600) | **1 246 075 µs** (1.25 s — ≈ 5.8 KB/s) |
| `scif_set_parameters(1562500, 1); scif_init();` | **47 900 µs** (≈ 150 KB/s) |

1.25 s matches 7200 bytes × 10 bits ÷ 57600 exactly, i.e. Flycast models SCIF
baud faithfully and KOS busy-waits on the TX FIFO. At the default baud, ~190
bytes of logging consumes an entire 30 fps frame budget.

**Design rule:** the DC build must raise SCIF to 1 562 500 baud
(`SCBRR2 = 0`, the SH-4 maximum: 50 MHz / 32) behind a harness/debug flag
before any logging, and must never emit high-volume serial inside a timed
measurement window. **[U]** 1.5 Mbps is emulator-only; a real coder's cable
will not sync there, so keep the baud a build/settings switch, not a constant.

### PTY variant [V]

`-config config:Debug.SerialConsoleEnabled=yes -config config:Debug.SerialPTY=yes`
→ stdout stays empty and Flycast logs to stderr:
`hw/sh4/modules/serial.cpp:491 N[BOOT]: Pseudoterminal is at /dev/ttys000`.
The harness can parse that line and open the pty for **bidirectional** serial
— i.e. host → guest bytes. That is the transport for a future in-guest command
REPL (drive a test scenario without touching the GUI). Each instance gets its
own `/dev/ptmx` slave, so it is parallel-safe. Only the "a pty appears and is
named on stderr" part is verified; no round-trip byte was pushed guest-ward.

---

## 4. Screenshots

### Host-side capture is BLOCKED on this machine [V]

- `screencapture -x -t png out.png` → `could not create image from display`
  (Screen Recording TCC permission not granted to the agent's terminal).
- `osascript -e 'tell application "System Events" …'` → **hangs** (blocked on
  the Accessibility TCC prompt), killed at 10 s.

So the Flycast built-in screenshot (default hotkey **F12**, `EMU_BTN_SCREENSHOT`,
saved as `~/Pictures/Flycast-<ISO8601>.png` via `hostfs::saveScreenshot`) is
**not reachable unattended** — there is no way to synthesise the keypress.
Unlocking it requires a human to grant Screen Recording + Accessibility to the
terminal app in System Settings. Treat host-side capture as a manual-only path.

### Guest-side framebuffer dump — WORKS, use this [V]

The guest owns the framebuffer, so it can hash and describe it and ship the
result over the console channel. Verified end-to-end:

```
FBHASH 5ea3f8c5
FBTHUMB16x12 AChQeKDI8BhAaJC44AgwWAEpUXmhyfEZQWmRueEJMVkCKlJ6osryGkJq…
```

- **[V]** `vid_set_mode(DM_640x480, PM_RGB565)`, write pattern to `vram_s`,
  read it back, FNV-1a over 640×480×2 → stable hash.
- **[V]** A 16×12 sample of the framebuffer, base64'd, is ~256 chars ≈ 45 ms
  emulated at high baud — cheap enough to emit per checkpoint.
- **[V] Both the hash and the thumbnail were byte-identical across separate
  runs and across 3 parallel instances.** Golden-image regression testing on a
  hash is therefore viable, no image files needed.
- **[U]** Not yet done with real PVR output (TA-rendered scene rather than a
  CPU-written framebuffer). PVR renders into VRAM and the front buffer is
  readable the same way, so this should carry over — verify at M2.
- If a real PNG is ever needed, emit the full framebuffer as base64 over the
  high-baud serial link (640×480×2 = 614 KB ≈ 4 s emulated) and have the host
  decode it. Slow but permission-free.

---

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

## 6. Crash triage — works without GDB [V]

An illegal instruction (`.word 0xfffd`) in the guest produced, on stdout:

```
Unhandled exception: PC 8c010226, code 1, evt 0180
 R0-R7:  00000000 00000000 ffffffff 00002889 8c07be48 8c07bdf0 00000000 00002889
 R8-R15: 8c038910 8c037410 8c04de00 00000000 00000000 00000000 ffffffff 8cffffec
 SR 40000101 PR 8c010226
Stack Trace: frame pointers not enabled!
kernel panic: unhandled IRQ/Exception
arch: aborting the system
```

and `sh-elf-addr2line -f -C -e prog.elf 0x8c010226` resolved it to
`main` / `prog.c:8`. **[V]** That is a complete crash-triage loop with no
debugger at all: fault PC + all registers + `evt` code, symbolised on the host.

Two notes:

- **[V]** Build guest code with `-g` and, for usable stack traces, with frame
  pointers (`-fno-omit-frame-pointer`) — KOS printed "frame pointers not
  enabled!".
- **[V] CAUTION — Flycast does NOT trap unaligned accesses.** A deliberately
  misaligned 32-bit read (`*(volatile u32*)(buf+1)`) executed silently and
  returned data, under **both** the dynarec and the interpreter
  (`Dynarec.Enabled=no`). Real SH-4 raises an address error here. This is
  exactly the failure class `PLAN.md` §3.2 predicts from `-O2` on the decomp
  (the base port's ARM SIGBUS). **Flycast cannot find it.** Alignment triage
  needs real hardware, or a more accurate emulator, or host-side detection
  (build the same TUs for the PC port under `-fsanitize=alignment` / UBSan and
  triage there). Record this against the M3 gate.
- **[V]** A write to address `0x00000000` did *not* fault either (it lands in
  the boot ROM area with the MMU off). Don't use null-deref as a crash canary.

---

## 7. What is *not* in the official macOS build [V]

Checked by scanning the binary's string table and confirmed against the v2.6
`CMakeLists.txt` defaults:

| Feature | CMake option | State |
|---|---|---|
| **GDB server (port 3263)** | `ENABLE_GDB_SERVER` | **OFF.** No GDB RSP strings (`qSupported`, `vCont`, `PacketSize`) in the binary. Launching with `Debug.GDBEnabled=yes Debug.GDBPort=3263 Debug.GDBWaitForConnection=yes` left **nothing listening** on 3263 (`lsof`, `nc -z` both negative). The `Debug.GDB*` config keys exist but are inert. |
| **Lua scripting API** | `USE_LUA` | **OFF in practice.** `core/lua/lua.cpp` exposes `flycast.exit()`, `flycast.saveState/loadState`, `memory.read8/16/32/64` + `readTable*` + `write*`, `input.pressButtons/releaseButtons/setAxis`, and a `flycast_callbacks` table with a per-frame **`vblank`** hook — a near-perfect automation surface. But the shipped binary contains no Lua strings, and a `$HOME/.flycast/flycast.lua` placed for a run was never executed. |
| **Test automation** | `TEST_AUTOMATION` | **OFF.** Would give raw video dump to a file (`record:rawvid=PATH`), input record/replay (`record:record_input` / `record:replay_input`), unbuffered stdio, muted audio, and `die()` on load failure. |
| **SH4 profiler** | `ENABLE_DC_PROFILER` | **OFF** — so `Profiler.Enabled` / `Profiler.OutputTTY` do nothing. |

**Recommendation (follow-up task, not done here):** build Flycast from source
once with

```
cmake -B build -DENABLE_GDB_SERVER=ON -DUSE_LUA=ON -DTEST_AUTOMATION=ON \
      -DENABLE_DC_PROFILER=ON -DUSE_BREAKPAD=OFF -DCMAKE_BUILD_TYPE=Release
```

and keep the result next to the cask build as `flycast-dev`. That single build
unlocks the GDB crash path, frame-exact scripted control, and raw video dump.

**[V] Prerequisite that will block it today:** this Mac has macOS 26.5.2 but
2021-vintage Command Line Tools — `clang 12.0.5`, newest SDK `MacOSX13.sdk`,
linking against a `11.3` deployment target, `cmake 3.21.4`, no Xcode.app.
Flycast v2.6 needs a modern C++17/20 toolchain and Metal/MoltenVK headers.
`xcode-select --install` (after removing the stale `/Library/Developer/CommandLineTools`)
is step zero. Mark the whole from-source path **[U]**.

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

## 9. Secondary emulator: lxdream-nitro / "Nitrocast" — NOT usable today

`https://gitlab.com/simulant/community/lxdream-nitro` (last activity
2026-07-29; the `simulant/lxdream-nitro` URL redirects to it). Positions
itself as "a Dreamcast emulator designed primarily to aid homebrew
development": very accurate SH-4, weak PVR. Its CLI is *exactly* what an
AI harness wants — **[V]** from `--help` of a binary built here:

```
-H, --headless          Run in headless (no video) mode
-t, --run-time=SECONDS  Run for the specified number of seconds   (auto-quits)
-e, --execute=PROGRAM   Load and execute the given SH4 program
-g, --gdb-sh4=PORT      Start GDB remote server on PORT for SH4
-G, --gdb-arm=PORT      Start GDB remote server on PORT for ARM
-b, --biosless          Run without the BIOS boot rom
-r, --ram-size=SIZE     [16, 32]
-x / -X                 interpreter only / interpreter+translator cross-check
-T, --trace=REGIONS     trace output
-m, --multiplier=SCALE  SH4 speed multiplier
```

plus (per its README) mounting a host folder at `/pc` and **exiting with the
guest ELF's return code**.

**[V] Build on this Mac: it compiles, it does not run.**

- `brew install meson ninja gtk+3` (glib/libpng already present);
  `meson setup builddir && meson compile -C builddir`.
- Needed a 3-line patch: `src/drivers/osx_iokit.m` uses `kIOMainPortDefault` /
  `IOMainPort`, which the stale 11.3 SDK doesn't have → replaced with
  `kIOMasterPortDefault` / `IOMasterPort`. (Goes away once the CLT are updated.)
- **Two genuine Apple-Silicon bugs found in `src/mem.c`:**
  1. `map_perms()` tests `perms & PERMS_EXEC`, but the enum is
     `PERMS_READ=0x81, PERMS_WRITE=0x82, PERMS_EXEC=0x84` — they share bit
     `0x80`, so **every** allocation requested `PROT_EXEC`. On arm64 macOS
     W^X is enforced, so a plain RW page map `mmap` returned `EPERM`
     (`FATAL Unable to allocate page map!`). Fixed locally by testing
     `0x01/0x02/0x04`.
  2. Genuinely executable allocations then still failed: on Apple Silicon
     `PROT_EXEC` anon mappings need `MAP_JIT` plus the
     `com.apple.security.cs.allow-jit` entitlement. Added both
     (ad-hoc `codesign --entitlements`).
- After both fixes it gets past memory init and then **SIGBUS**es with an
  all-zero SH-4 register dump before executing any guest code. lxdream's core
  is x86-oriented (SSE2 paths, x86-only translator, RWX assumptions); porting
  it to arm64 macOS is a real project, not a patch.

**Verdict: NO-GO as a harness right now.** Keep it on the list because its
GDB-for-SH4-*and*-ARM7, MMIO tracing and interpreter/translator cross-check
are the right tools for the alignment class that Flycast cannot see (§6).
Two **[U]** ways back in, in order of promise: (1) build and run it inside an
`--platform linux/amd64` container under colima (qemu-emulated, slow, but the
x86 assumptions hold); (2) upstream the two `mem.c` fixes and finish the arm64
port. Also **[U]**: `deecy` (Zig, referenced by Nitrocast's own README as the
better modern emulator) was not evaluated.

**[U] RetroArch + flycast libretro core** as a "headless" option was not
tested. RetroArch has no true headless mode for running content on macOS, and
the libretro core is a different build with different config plumbing; it also
loses the `-config Debug.SerialConsoleEnabled` path (libretro logging goes
through the frontend). Lower priority than a from-source Flycast.

---

## 10. Host-environment gotchas found while doing this

- **[V] No `timeout(1)`.** Either `brew install coreutils` (→ `gtimeout`) or,
  better, keep timeouts inside the Python runner as above.
- **[V] colima bind-mounts of `/private/tmp/...` are unreliable.** Files
  written on the host under the agent scratchpad were *not* visible inside
  containers (the directory appeared, empty). Mounts under `$HOME` work fine.
  **All Docker-based build steps must use paths under `$HOME`.** This will bite
  the `dc/build-dc-docker.sh` work too.
- **[V] Docker/colima has 4 CPUs and 8 GiB** allocated (not the host's 10/24).
  Raise it (`colima stop && colima start --cpu 8 --memory 16`) before building
  a dc-chain toolchain.
- **[V] KOS toolchain container used for this recon:**
  `einsteinx2/dcdev-kos-toolchain:latest` — has a native **arm64** manifest,
  pulls and runs on this M4 with no emulation. Contains `sh-elf-gcc 9.3.0`,
  `sh-elf-gdb`, prebuilt `libkallisti.a`, kos-ports (GLdc/libGL, zlib, SDL…),
  `mkisofs`/`genisoimage`. Build with
  `source /opt/toolchains/dc/kos/environ.sh && make` and a `Makefile` that
  includes `$(KOS_BASE)/Makefile.rules`.
  **It is too old for the project** — KOS git rev `525cbda` (2021-03-15),
  GCC 9.3.0, and **no `mkdcdisc`**; `PLAN.md` §8 wants GCC 14+ and KOS master.
  Use it only as a stopgap for harness work; the real M0 container is a
  separate task. Nothing in this harness design depends on it.
- **[V]** Flycast opens a real window that briefly takes focus. Unavoidable
  (§2). With the marker runner it is on screen for about a second.
- **[V]** Flycast writes a `dc_nvmem.bin` and two 128 KB `vmu_save_A*.bin` per
  isolated `HOME` — that is where the VMU-save harness will read/write, and
  why per-run `HOME` isolation matters for save tests.

---

## 11. Verified-claims index (so nothing here is taken on trust)

Verified by execution today: cask install + quarantine strip; v2.6 == upstream
tag `392a429e8`; `-config section:key=value` grammar; direct `.elf` boot via
reios; hermetic `HOME` via pre-created `$HOME/.flycast/`; 3-way parallel runs;
`Debug.SerialConsoleEnabled` → stdout with KOS banner + `printf` +
`dbgio_write_str`; Flycast logs on stderr and `log:LogToConsole=no`;
`Debug.SerialPTY` → `/dev/ttys000` named on stderr; SCIF baud cost 57 600 vs
1 562 500 (1 246 075 µs → 47 900 µs for 7200 B); determinism of guest timer,
`FBHASH`, and thumbnail across runs and instances; illegal-instruction →
KOS register dump on stdout → `sh-elf-addr2line` symbolisation; unaligned
access **not** trapped (dynarec *and* interpreter); write to `0x0` not trapped;
GDB port 3263 not listening + no RSP strings in binary; Lua not present and
`flycast.lua` never executed; `SDL_VIDEODRIVER=dummy/offscreen` fail (exit 6);
`screencapture` and `osascript`/System Events both blocked by TCC; marker
runner ok/fail/timeout paths; Nitrocast builds on macOS arm64 with 3 patches
and then SIGBUSes; stale Command Line Tools (clang 12.0.5 / SDK 11.3) on
macOS 26.5.2; colima `/private/tmp` mount failure; `einsteinx2` image contents.

Not verified: CDI boot (no CDI exists yet); PVR-rendered framebuffer readback;
guest→host bytes over the PTY; KOS `gdb_init()` stub over SCIF; any
from-source Flycast build; RetroArch/libretro headless; deecy; the absolute
accuracy of any timing number relative to real SH-4 silicon.

---

## 12. Corrections from building the harness (2026-08-01, later the same day)

The scripts in §8 were written and run end to end against real CDIs built from
`harness/dc/selftest/` with the real `opencrossing-dc:sdk` image
(KOS **2.3.0**, git `1c6398f`, GCC **15.2.0**) — not the 2021 `einsteinx2`
image everything above was measured on. Four things changed.

### 12.1 CDI boot works — §8(a)'s **[U]** is now **[V]**

`mkdcdisc -q -N -e prog.elf -o prog.cdi` → Flycast boots it through `IP.BIN`
exactly as predicted. Guest serial, maple enumeration, `FBHASH` and the crash
dump all behave identically to the ELF path. Cost: **~3.1 s** per run with
`config:FastGDRomLoad=yes`, versus **~1.0 s** for the same program booted as a
raw `.elf`. Keep using ELF boot for iteration; use CDI for the gate.

### 12.2 **`scif_flush()` from guest code silently kills the console** [V]

This is the big one, and it is a trap the §3 design walks straight into.
KOS's `scif_flush()` clears TEND (`SCFSR2 &= 0xbf`) and then spins up to
800 000 iterations waiting for TEND to come back. **Flycast never re-raises
TEND on an already-idle TX FIFO**, so the spin always times out, and on timeout
KOS latches `serial_enabled = 0` (`hardware/scif.c`) — permanently disabling
*all* further serial output, including the panic path.

Consequence: a guest that calls `scif_flush()` and then crashes produces **no
`Unhandled exception` block at all**. The run can only end as a wall-clock
timeout, with no triage information. Isolated by bisection over seven guest
variants:

| guest does | crash dump reaches host? | run ends as |
|---|---|---|
| nothing special | yes | `fail_marker`, 1.6 s |
| `scif_set_parameters(1562500,1); scif_init()` | **yes** | `fail_marker`, 1.6 s |
| `scif_flush()` (any baud, with or without `scif_init`) | **no** | `timeout` |

So raising the baud (§3) is safe and stays; the explicit flush must go.
`printf` → `scif_write_buffer` already flushes internally on a FIFO that has
just been written, and that path succeeds. Both `selftest.c` and `crashtest.c`
were fixed. **Rule: guest code never calls `scif_flush()`.**

### 12.3 The §5 fail-marker regex misses KOS assertion failures [V]

KOS 2.3 prints `*** ASSERTION FAILURE ***` / `Assertion "x" failed at f.c:9`
(capital A) and then `arch: aborting the system`. The regex in §5 tests for
lowercase `assertion "`, so a failed guest `assert()` was caught only by the
timeout. `_runner.py`'s `DEFAULT_FAIL_MARKER` now also matches
`ASSERTION FAILURE` and `arch: aborting the system`.

### 12.4 The KOS banner changed shape [V]

2021 image: `KallistiOS Git revision 525cbda:` on one line.
KOS 2.3.0: `KallistiOS v2.3.0 [dreamcast/pristine]` then `  Git revision: 1c6398f`
on the next. Any assertion that greps for the old single-line form silently
fails. `_runner.py` now parses both and reports `kos_version` separately.

### 12.5 Minor

- KOS 2.3 also enumerates two VMUs (`A1`, `A2`) alongside the controller; the
  §8(a) assertion should key on `A0: Dreamcast Controller` only.
- Guest `PERF` timings are reproducible but **not bit-identical**: the 1 M
  iteration loop measured 35085 / 35090 µs across runs, not one fixed value.
  A regression gate needs a tolerance band, not equality.
- Crash triage confirmed on the new toolchain: PC `8c010112` →
  `sh-elf-addr2line -f -C -i -e crashtest.elf` → `main` / `crashtest.c:39`,
  the illegal instruction. KOS 2.3 additionally prints a ready-made
  `$KOS_ADDR2LINE …` command line with the PC, PR and unwound stack.

---

## 13. Building `crash.sh` and `perf.sh` (§8d/§8e), same day

### 13.1 §8e's crash path is fully automated and needs no Flycast rebuild [V]

`harness/dc/crash.sh` parses the KOS dump out of a console log and symbolises it
with `sh-elf-addr2line` + `sh-elf-objdump` inside `opencrossing-dc:sdk`.
Verified: `crashtest.cdi` → `Illegal instruction`, PC `8c010112` →
`main at /src/crashtest.c:39`, stack `arch_main at …/init.c:319`, plus a
disassembly window with `>> 8c010112: fd ff .word 0xfffd` marked. `smoke.sh`
runs it automatically on any failure whose console holds a dump. **The GDB path
in §8e is not needed for this class of bug at all** — deprioritise the
from-source Flycast build accordingly (it is still the only route to frame-exact
scripted control and raw video dump).

### 13.2 ELF provenance is a hard requirement, not a nicety [V]

A CDI holds a scrambled, stripped `1ST_READ.BIN`, so nothing in the image says
which ELF produced it. Every CDI producer now writes `<image>.src.json` beside
the image recording the ELF's absolute path, sha256 and size;
`selftest/build.sh` does it, and **`dc/build-dc-docker.sh` must too**.
`crash.sh` refuses to symbolise when the sidecar is missing, or when the ELF on
disk no longer matches the recorded hash — verified both refusals, plus the
happy path after restoring the original ELF. A confidently wrong line number is
worse than no answer.

### 13.3 Two container gotchas that cost real time [V]

- **`bash -lc` breaks the toolchain PATH.** `opencrossing-dc:sdk` bakes
  `/opt/toolchains/dc/sh-elf/bin` into the image `ENV`, but a *login* shell
  re-runs `/etc/profile`, which rebuilds `PATH` and drops it. `sh-elf-addr2line`
  then comes back "command not found" and every address symbolises to `??`.
  Use `bash -c`. (`selftest/build.sh` gets away with `-lc` only because it
  explicitly sources `environ.sh`.)
- **Sourcing `environ.sh` under `set -u` kills the shell** — unbound variable,
  exit 127, nothing on stderr. Don't add it "for safety".
- Bash parses backticks inside `$( … )` **even in a quoted heredoc**, so a
  Python regex containing a literal backtick (KOS writes ``failed at f.c:9 in
  `main'``) makes the whole script an unterminated-quote syntax error. Build it
  with `chr(96)`.

### 13.4 §8d's regression gate: use a band, never equality [V]

The §8d claim that guest timing is "exactly reproducible" is very nearly true
but not literally so: the 1 M-iteration loop measured **35085 µs** on most runs
and **35090 µs** on one — 0.014% spread. `perf.sh` therefore gates on
**max(2% of baseline, 100 µs)**, ~140× the observed jitter and still far tighter
than anything that matters against a 33 333 µs frame budget. The absolute floor
is there because 2% of a 40 µs metric is noise. Verified: passes unchanged
(±0.000%), fails a baseline shifted to 30000 (+16.95%), passes one shifted to
34600 (+1.4%, inside the band), fails one at 34000 (+3.2%, outside it), and
treats a faster-than-baseline result as an `improvement` (non-fatal without
`--strict`, a `regression` under `--lower-is-worse`).

Also: `PERF` lines carrying checksums rather than measurements (selftest's
`acc=`) need `--exact`, so prefer emitting checksums as `MEM` or `ASSERT`
records instead.
