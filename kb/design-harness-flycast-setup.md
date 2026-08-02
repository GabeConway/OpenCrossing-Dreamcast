# Flycast harness — install, invocation, and what this build lacks

Installing the cask and stripping quarantine (§1), the exact working command
line and the config keys that matter (§2), the features compiled OUT of the
official macOS build (§7), and the host-environment gotchas hit while building
it (§10). Read first when setting the harness up on a new machine.
Part of `kb/design-harness.md`, whose stub maps every § to its file.

Recon pass 2026-08-01. Everything below is tagged **[V]** = verified by running
it on this machine today, or **[U]** = unverified / inferred from source only.
Where something did not work, it says so.

⚠️ §12 and §13 (`kb/design-harness-corrections.md`) were written later
the same day against the real `opencrossing-dc:sdk` image and correct
parts of this file — read them too.

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
