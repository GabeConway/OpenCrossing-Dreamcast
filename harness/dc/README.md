# Dreamcast emulator test harness (macOS host, Flycast)

Boots a Dreamcast image in Flycast, reads what the guest printed over the
emulated SCIF serial port, and turns that into an exit code. Non-interactive,
hard wall-clock bound, JSON on stdout, human line on stderr.

The full recon and the reasoning behind every choice here is
[`kb/design-harness.md`](../../kb/design-harness.md). This README is the
operating manual; that document is the evidence.

## One-time setup

```bash
harness/dc/install-flycast.sh        # idempotent; exits 0 if already there
```

Installs the Homebrew cask and strips `com.apple.quarantine` (Flycast is ad-hoc
signed with no Team ID, so without that every launch is Gatekeeper-blocked). No
BIOS ROM is needed — Flycast falls back to its HLE BIOS (reios), which is also
what lets it boot a raw `.elf` with no disc image at all.

> The cask is **deprecated in Homebrew and scheduled for disable 2026-09-01**.
> Keep a copy of the `.app` or the release DMG before then, or point
> `OC_DC_FLYCAST` at a hand-installed binary.

## Building something to run

```bash
harness/dc/selftest/build.sh                 # -> ~/.cache/oc-dc-harness/selftest/
```

Builds `selftest.{elf,cdi}` and `crashtest.{elf,cdi}` in the
`opencrossing-dc:sdk` container. These are the harness's own test subjects, not
part of the game build. CDIs are made with `mkdcdisc -N` (no padding): 1.8 MB
and 21 ms, versus 740 MB and 15.6 s without it.

Nothing is written into the repo, and Docker mounts stay under `$HOME` — colima
bind-mounts of `/private/tmp/...` are silently empty on this host.

### ELF provenance sidecars — required of every CDI producer

A CDI contains a scrambled, stripped `1ST_READ.BIN`, so a crash inside one is
just hex. `crash.sh` can only symbolise it if it can find the exact ELF the
image was built from *and* prove that ELF has not been rebuilt since. So every
script that runs `mkdcdisc` **must** write a sidecar named `<image>.src.json`
beside the image in the same step:

```json
{
  "image": "/abs/path/to/foo.cdi",
  "elf": "/abs/path/to/foo.elf",
  "elf_sha256": "<64 hex>",
  "elf_size": 2068792,
  "built_utc": "2026-08-01T20:16:05Z",
  "toolchain_image": "opencrossing-dc:sdk",
  "producer": "harness/dc/selftest/build.sh"
}
```

**This binds `dc/build-dc-docker.sh` too.** Without the sidecar, crash triage on
the game build is dead on arrival — `crash.sh` refuses to guess which ELF goes
with an image, and refuses to symbolise against one whose sha256 no longer
matches, because a confidently wrong line number is worse than no answer.

## The scripts

Every one takes the image as a positional argument and defaults to
`dc/build/OpenCrossing.cdi`. `.elf` boots directly via reios (~1 s round trip);
`.cdi/.gdi/.chd/.cue` boot through `IP.BIN` (~3 s) and get
`FastGDRomLoad=yes` automatically unless you pass `--no-fast-gdrom`.

| Script | What it does | Exit 0 when |
|---|---|---|
| `install-flycast.sh` | install / verify / de-quarantine Flycast | a usable Flycast exists |
| `run-flycast.sh` | boot an image, stream guest serial to stderr, JSON to stdout | run reached its end marker |
| `console.sh` | run and return the whole console plus parsed records | end marker, `rc=0`, no failed `ASSERT` |
| `screenshot.sh` | guest-side framebuffer hash/thumbnail, golden compare | hash matched the golden |
| `crash.sh` | symbolise a KOS register dump → file:line + disassembly | a dump was found and symbolised |
| `perf.sh` | collect guest `PERF` records, gate against a baseline | nothing regressed outside its band |
| `smoke.sh` | **the M0 gate** — boot, assert, triage | all nine checks below |

### `smoke.sh` — what it asserts

```bash
harness/dc/smoke.sh                                  # dc/build/OpenCrossing.cdi
harness/dc/smoke.sh path/to/disc.cdi --timeout 90
harness/dc/smoke.sh --elf path/to/prog.elf
harness/dc/smoke.sh IMG --expect 'MARK:TITLE_UP'     # repeatable
```

1. Flycast is installed (else exit **2**).
2. The image file exists (else exit **2**).
3. The run ended on the end marker — not a fail marker, not the timeout.
4. The KOS banner reached the host. This is the check that proves the guest is
   *executing*, not merely that a window opened.
5. Maple enumeration lists `A0: Dreamcast Controller`.
6. `MARK:BOOT_OK` was printed.
7. `OC-DC-HARNESS-END rc=0`.
8. Zero `ASSERT fail` records.
9. Every `--expect` regex matched some console line.

On failure it prints the last 25 console lines and, if those contain a register
dump or an assertion, **automatically runs `crash.sh`** and folds the symbolised
result into the JSON under `"crash"`. So a failing gate reports:

```
DC SMOKE FAIL [fail_marker] -- failed: run_reached_end_marker, end_rc_zero
  CRASH Illegal instruction
    fault : PC 8c010112  main at /src/crashtest.c:39
    stack : 8c0109ca  arch_main at .../kernel/arch/dreamcast/kernel/init.c:319
```

Pass `--no-crash-triage` to skip it.

The **16 MB hardware contract is enforced mechanically**: every run passes
`config:Dreamcast.RamMod32MB=no`. Do not remove it from `_runner.py`.

### Timeout and exit semantics

Flycast never exits on its own — not when the guest returns from `main`, not on
a KOS kernel panic. It sits in its window forever, and the cask build has no
exit-on-condition flag. This host also has no `timeout(1)`/`gtimeout(1)`. So
supervision is marker-driven inside `_runner.py`, which kills the process group
on the first end marker, the first fail marker, or the deadline.

| runner status | meaning | script exit |
|---|---|---|
| `ok` | end marker seen | 0 |
| `fail_marker` | panic / unhandled exception / failed assert | 1 |
| `timeout` | deadline hit with no marker — a hang | 1 |
| `exited_early` | Flycast died before any marker | 1 |
| `launch_error` | no Flycast, or no such image | 2 |

**A window will open and briefly take focus.** There is no headless mode;
`SDL_VIDEODRIVER=dummy` and `=offscreen` both abort with exit code 6.

### `console.sh`

```bash
harness/dc/console.sh prog.elf --grep '^(ASSERT|PERF)'
harness/dc/console.sh prog.elf --print            # log to stderr as well
harness/dc/console.sh prog.elf --capture-only     # no markers, run to timeout, exit 0
```

The workhorse. Any unit test of ported game code — checksum a decoded texture,
walk a heap, byte-swap a save struct — is a KOS ELF that prints
`ASSERT ok|fail <name>` lines and lets this turn them into an exit code.

### `screenshot.sh`

```bash
harness/dc/screenshot.sh prog.elf --frame 0 --golden 1489d5c5
harness/dc/screenshot.sh prog.elf --frame 0 --write-golden dc/golden/title.hash
```

**Host-side capture is blocked on this machine and these scripts never attempt
it.** `screencapture` fails without the Screen Recording TCC grant, and
`osascript`/System Events hangs on the Accessibility prompt, so Flycast's own
F12 screenshot is unreachable unattended. Instead the *guest* hashes its own
framebuffer and ships an `FBHASH` plus a 16×12 `FBTHUMB` over the console.
Those are byte-identical across runs and across parallel instances, so the
golden image is a hex string in the repo, not a PNG.

### `crash.sh`

```bash
harness/dc/crash.sh prog.elf                    # run it, then triage
harness/dc/crash.sh disc.cdi                    # ELF found via the sidecar
harness/dc/crash.sh --run-dir DIR --image IMG   # triage an already-captured run
harness/dc/crash.sh --console LOG --elf p.elf   # triage a bare console log
```

There is no debugger — the cask Flycast has `ENABLE_GDB_SERVER=OFF` and nothing
listens on port 3263. There does not need to be one: KOS prints the faulting PC,
all 16 GPRs, SR, PR and an unwound stack over the serial console, and this
script symbolises that offline with `sh-elf-addr2line` and `sh-elf-objdump`
inside `opencrossing-dc:sdk`. Output is JSON with `fault`, `stack[]` (one
`symbol at file:line` per frame) and a disassembly window with the faulting
instruction marked `>>`:

```
   8c010110:       09 00           nop
>> 8c010112:       fd ff           .word 0xfffd
   8c010114:       0d d4           mov.l   8c01014c <main+0x6c>,r4
```

Exit 1 means no dump was in the log; exit 2 means it could not resolve or trust
the ELF (see the sidecar rule above). Build guest code with `-g` and
`-fno-omit-frame-pointer` or KOS prints "frame pointers not enabled!" and the
stack trace is empty.

### `perf.sh`

```bash
harness/dc/perf.sh prog.elf --repeat 3 --write-baseline dc/perf/boot.json
harness/dc/perf.sh prog.elf --baseline dc/perf/boot.json      # gate
harness/dc/perf.sh prog.elf --baseline B --tolerance 0.05
harness/dc/perf.sh prog.elf --baseline B --exact acc          # checksum key
harness/dc/perf.sh prog.elf --baseline B --lower-is-worse fps
```

Collects every numeric `PERF k=v` field the guest emitted, takes the median over
`--repeat` runs, and compares against a stored baseline JSON.

**The gate is a band, not equality.** Guest-measured emulated time is very
stable but not bit-identical — the selftest's 1 M-iteration loop measured 35085
and 35090 µs across runs, a spread of 0.014%. The default band is
**max(2% of baseline, 100 µs)**, roughly 140× the observed jitter, so it cannot
flap while still catching anything that matters against a 33 333 µs frame
budget. The absolute floor exists because 2% of a 40 µs metric is pure noise.

A metric is assumed to be a duration: moving above the band is a **regression**
and fails; moving below it is an **improvement**, reported but not fatal unless
`--strict`. Use `--lower-is-worse KEY` for fps-like metrics and `--exact KEY`
for checksums.

## Guest-side protocol

Bracket output with `OC-DC-HARNESS-BEGIN` … `OC-DC-HARNESS-END rc=<n>` and emit
typed one-line records in between:

```
MARK:<name> [arg]        ASSERT ok|fail <name>      PERF k=v k=v
FBHASH <hex>             FBTHUMB 16x12 <base64>     MEM k=v k=v
```

`selftest/selftest.c` is the reference implementation. Two rules in it are
load-bearing:

1. **Raise the SCIF baud before logging.** Flycast models SCIF baud faithfully
   and KOS busy-waits on the TX FIFO, so at the KOS default of 57600 you get
   ~5.8 KB/s and ~190 bytes of logging eats an entire 30 fps frame budget.
   `scif_set_parameters(1562500, 1); scif_init();` gives ~150 KB/s. This is
   emulator-only — a real coder's cable will not sync at 1.5 Mbps — so keep it
   behind a build flag, and never log inside a timed measurement window.
2. **Never call `scif_flush()`.** KOS's `scif_flush()` clears TEND and spins
   waiting for it to come back; Flycast never re-raises TEND on an already-idle
   TX FIFO, so the spin times out and KOS latches `serial_enabled = 0`. All
   further serial output dies silently — *including the crash dump*, so a crash
   after an explicit flush is invisible and shows up only as a wall-clock
   timeout. `printf()` already flushes; you never need the call.

Also print `MARK:BOOT_OK` early, and don't print the END line until you really
are done — the host kills Flycast the moment it sees it.

## Environment overrides

| Variable | Default |
|---|---|
| `OC_DC_FLYCAST` | `/Applications/Flycast.app/Contents/MacOS/Flycast` |
| `OC_DC_IMAGE` | `dc/build/OpenCrossing.cdi` |
| `OC_DC_RUNROOT` | `~/.cache/oc-dc-harness/runs` |
| `OC_DC_TOOLCHAIN_IMAGE` | `opencrossing-dc:sdk` (build.sh) |

Each run gets `$OC_DC_RUNROOT/<name>-<timestamp>-<pid>/` holding an isolated
`home/.flycast/` (own `emu.cfg`, `dc_nvmem.bin`, `vmu_save_A*.bin`),
`console.log`, `flycast-stderr.log` and `runner.json`. That isolation is what
makes runs parallel-safe and is where the VMU-save harness will read and write.

## Known limits

- **No alignment faults.** Flycast does not trap unaligned accesses under
  either the dynarec or the interpreter, and a write to `0x00000000` does not
  fault either. The `-O2`-on-decomp alignment class this project expects to hit
  is invisible here; it needs real hardware or host-side UBSan.
- **Emulated time, not silicon time.** Guest `timer_us_gettime64()` numbers are
  reproducible to within a few µs and make a fine relative regression gate (see
  `perf.sh`), but Flycast models no cache, no bus contention and no store-queue
  stalls. They cannot answer the M3 "≤ 25 ms/frame" question. Host wall-clock
  timing of the Flycast process is meaningless — never use it.
- **No GDB, no Lua, no test-automation, no SH4 profiler** in the cask build
  (`ENABLE_GDB_SERVER`, `USE_LUA`, `TEST_AUTOMATION`, `ENABLE_DC_PROFILER` all
  OFF). Port 3263 has nothing listening; `flycast.lua` is never executed. The
  fix is a from-source build, which is blocked on this Mac's 2021-vintage
  Command Line Tools.
