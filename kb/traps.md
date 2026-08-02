# Traps already paid for — do not re-discover these

Each one cost real debugging time. **Read this before touching the build, the
harness, or the prelude.** These are mechanical gotchas, not design decisions —
for those see `kb/closed.md`.

## Compile / link

- **`fsqrt` collision.** KOS `dc/fmath.h:109` defines
  `static inline float fsqrt(float)`; the decomp's `math64.h:34`
  `#define fsqrt(x) sqrtf(x)` rewrites KOS's *definition* into a static `sqrtf`
  that collides with newlib.
- **POSIX `link()` vs the decomp's `typedef struct link_ link`**, arriving via
  `<stdio.h>` → `<sys/stdio.h>` → `<unistd.h>`. A blanket `-Dlink=` does **not**
  work — it renames both sides. `dc/include/dc_prelude.h` renames only the POSIX
  declaration, then gives the identifier back.
- **`-fno-builtin` breaks the link** — see `kb/closed.md`.
- **3900 object paths exceed `execve`'s `ARG_MAX`.** The Makefile uses make's
  `$(file …)` to build a linker response file.
- **`char` is SIGNED by default** on this toolchain build, so `-fsigned-char`
  is belt-and-braces, not load-bearing.

## `DC_ASSET_STUB` — full-size passes over `[1]`-sized destinations

- **The stub build corrupts `.bss` unless every full-size pass is neutralised,
  not just `pc_assets_init()`.** Shrinking a destination array does not shrink
  the loops that write it: their bounds are compiled-in constants. `boot.c`
  runs four endian-fixup passes immediately before the `HotStartEntry` loop and
  all four overrun —
  `pc_bswap_house_pos_list()` writes 0x978 B into a `u8[1]` (2,423 B over),
  `pc_bswap_u8_tlut_palettes()` 14 × 32 B into `u8[1]` (434 B),
  `pc_bswap_raw_display_lists()` 112 B into three `u8[1]` (109 B), and
  `mFM_InitActableEndian()` walks six actables looking for a sentinel that no
  longer exists, so it is unbounded.
  **Symptom, measured:** `boot.c`'s own `HotStartEntry` came back as
  `0x64b3418c`, the game jumped to it and died on an illegal instruction at
  `PC=65000004`. The victim symbol is ~3,000 B from the arrays being swapped
  and **moves whenever `.bss` moves**, so the same bug presents as a silent
  hang in one build and a wild jump in the next. Before this was found it read
  as "the renderer broke the boot".
  Fixed by the `NEUTRALISE` table in `tools/dcstub/make_stub_data.py`, which
  rewrites the four call sites under `#ifndef DC_ASSET_STUB` with an anchored,
  hard-erroring match count. **Any new full-size pass over asset arrays needs
  an entry there.**
- Corollary for debugging: in a `DC_ASSET_STUB` image, a crash whose address
  changes when unrelated `.bss` changes is an overrun, not a logic bug. Look
  for a loop bound that survived the stubbing.

## Per-TU make rules vs the scratch trees

- **A `$(OBJDIR)/src/…` per-TU rule stops firing the moment a rewriter emits
  that source.** `stubify`/`shrinkify` change the object path, and make just
  skips the rule — no warning, no error. The two `-Dmain=` renames are the
  dangerous instance, and they are now written as
  `$(OBJDIR)/$(call shrinkify,$(call stubify,src/main.c)).o`.
  **What it looked like when it bit:** `boot.c` moved into the stub tree, lost
  `-Dmain=boot_main`, and the image ended up with two `main()`s.
  `-Wl,--allow-multiple-definition` (required for the 1,367 multiply-defined
  data symbols) swallowed the clash, the linker kept the wrong `main`, and
  `--gc-sections` then deleted everything the real entry chain reached: `.text`
  5,289,364 → 851,684, and `boot_main`/`ac_entry`/`graph_proc`/`mainproc` were
  simply absent from the ELF. **It linked, produced a CDI, and exited 0.**
  Cheap detector, worth running after any Makefile or rewriter change:
  `sh-elf-nm build/AnimalCrossing.elf | grep -c ' _graph_proc$'` must be 1.
  Note the **leading underscore** — this toolchain prefixes every C symbol, so
  `grep ' graph_proc$'` matches nothing even in a healthy ELF and reads as the
  same failure.

## Instrumentation

- **Never gate a periodic probe on `pc_frame_counter`.** `dc_vi.c`'s retrace
  handler returns early on every frameskipped tick — *after* incrementing
  `pc_frame_counter` — so a `pc_frame_counter % N == 0` test is evaluated only
  on presented frames, at counter values that jump by the skip factor.
  **What it looked like when it bit:** a run that presented 1,769 frames fired
  the arena probe three times, all inside the first two seconds, then never
  again; the counter simply stopped landing on a multiple of 60. Both probes
  now share one local `probe_tick` incremented where they are called.
- **A `[1]`-sized stub build still censuses correctly.** The addresses the GX
  layer is handed are link-time constants, so `DC_ASSET_CENSUS` names the same
  symbols in a stub image as it would in a full one; only the *sizes* are
  wrong. `tools/dcstub/census_resolve.py --sizes-from <full ELF>` is what
  turns that into real bytes — quoting the stub column as a working-set total
  understates it by about 20%.

## Harness / emulator

- **Guest `scif_flush()` permanently kills the Flycast console. Never call it.**
  KOS's flush clears TEND and spins; Flycast never re-raises TEND on an idle TX
  FIFO; KOS latches `serial_enabled = 0`; a later crash then prints **nothing**.
  Bisected across 7 guest variants — raising baud is fine, the flush is the
  killer.
- **KOS 2.3 assertion text** is `*** ASSERTION FAILURE ***` / capital-A
  `Assertion "x" failed`. The documented lowercase regex never matched, so a
  failed `assert()` only ever surfaced as a timeout.
- **mkdcdisc padding**: default 740,083,145 B / 15.6 s vs `-N` 1,783,337 B /
  0.021 s. Use `-N` for every emulator run; `DC_CDI_PAD=1` only for burns and
  read-speed-realistic timing.
- **Flycast's hardware renderer never writes the frame back into emulated
  VRAM — SOLVED 2026-08-02, use `smoke.sh --fb-writeback`.** The guest-side
  framebuffer probe read all-zero from both `vram_s` and
  `pvr_get_front_buffer()` while a human watching the window could see the
  title logo, because the rendered surface lives on the host GPU and the
  guest's VRAM is never updated from it. Adding
  `config:rend.EmulateFramebuffer=yes` (Flycast's "Full Framebuffer
  Emulation") makes `FBHASH` real and frame-varying. It costs about a third of
  the frame rate — 24.8 → 16.8 FPS on the title screen — so it is a flag, not
  a default. **A zero `FBHASH` from a run without that flag says nothing about
  what was drawn.**
- **A smoke run of the game "fails" by construction.** The game never returns,
  so `run_reached_end_marker` / `mark_boot_ok` / `end_rc_zero` can never hold
  and `smoke.sh` exits 1 with `status=exited_early` even on a perfect run. For
  game images the console log is the artefact; read `[PERF]`, `[DC/PVR]` and
  the probe lines, not the exit code. The PASS/FAIL gate is meaningful for
  `selftest.cdi` and for anything that terminates.

## Docker / SDK image

- **`bash -lc` inside the SDK image** re-runs `/etc/profile`, which drops
  `/opt/toolchains/dc/sh-elf/bin` from PATH. `sh-elf-addr2line` then vanishes
  and every address silently symbolises to `??`. **Use `bash -c`.**
- **Sourcing `environ.sh` under `set -u`** exits 127 with nothing on stderr.
- **Host has no BuildKit** — `DOCKER_BUILDKIT=0`, never pass `--progress`.
  `--platform linux/arm64` is not optional; without it an amd64 pull drops the
  build into qemu.
- **Do not rebuild `opencrossing-dc:sdk`** — ~27 min cold. It is already in the
  local Docker daemon.

## The build tracks timestamps, not flags — FIXED, do not remove the fix

- **A flag change alone used to leave a stale image.** After a
  `DC_ASSET_STUB=1` build, a plain `bash dc/build-dc.sh` printed
  `make: Nothing to be done for 'all'` and left the **stub** ELF in place, so
  `sh-elf-size` reported the stub's sections for what looked like a real build.
  Two causes: toggling the flag swaps 2,521 sources for their stub twins and
  *both* sets of `.o` already exist and are older than the ELF, so nothing
  relinks; and any object whose source did not change keeps the `-D` set it was
  built with, which would have shipped a non-stub image whose `dc/src` objects
  still skipped `pc_assets_init()`.
- **The fix is `dc/build/flags.stamp`**: it holds `DC_ASSET_STUB`,
  `DECOMP_OPT`, `DC_OPT` and `DEFINES`, is rewritten by `$(file …)` when any of
  them changes, and every object and the link depend on it. Changing a flag now
  costs a full rebuild, which is the correct price.
- **`$(file …)` needs GNU make 4.0.** The container has 4.3; the macOS host has
  **3.81**, where `$(file …)` silently expands to nothing. That is why the
  stamp also has an ordinary recipe. The host only ever runs `make count` /
  `make sources`, never a compile.
- **When in doubt, `rm dc/build/AnimalCrossing.elf`** and re-link. A missing
  ELF cannot be stale.

## Disc content and the scratch-tree mechanism

- **`mkdcdisc -d DIR` puts DIR ITSELF on the disc**, so files land at
  `/cd/DIR/name` and every `DVDFastOpen` misses with no diagnostic. The flag
  you want is **`-D`** (contents, excluding the root). `dc_dvd.c:113` builds
  every path as `"/cd" + "/" + name`, flat, with no subdirectory.
- **Colima does not share `/private/tmp` with the VM.** A `-v` bind mount of a
  path under it is silently EMPTY inside the container — the build printed
  "0 files" and carried on. Stage disc content somewhere under `$HOME`.
- **`"${ARR[@]}"` on an empty array is an unbound-variable error under `set -u`
  in bash 3.2**, the macOS system bash. Every `dc/build-dc.sh` run without
  `DC_DISC_ROOT` died on it. Use `${ARR[@]+"${ARR[@]}"}`.
- **A quoted `#include` resolves against the INCLUDING FILE'S directory first**,
  so an `-I` shadow of a header in `include/` can never reach a consumer that
  pulls it in via a *sibling header* in `include/`. MEASURED: a shadow of
  `include/ac_structure.h` reaches `src/actor/ac_structure.c` but **not**
  `src/actor/npc/ac_npc.c` — a half-applied shadow, i.e. a silent ODR split that
  `--allow-multiple-definition` will not complain about. Header shadows are only
  safe for headers included directly by the TUs you care about; otherwise use a
  per-TU source swap confined to one TU, plus a compile-time assert pinning the
  unshrunk `sizeof`. `tools/dcstub/make_src_shrink.py` is built around this.
- **Every rewrite rule must hard-error on no-match.** A regex that silently
  matches nothing produces a build that looks fine and saves nothing, or worse,
  shrinks one of two consumers.

## Agent hygiene

- **Agents must not run git.** The main thread commits.
- **One build at a time.** `dc/build/` is a single shared object tree; two
  concurrent `make` runs corrupt it. Investigation agents get read-only
  `sh-elf-nm`/`objdump`/`readelf` over `docker run`, never `make`.
- **Always give absolute paths in scripts** — agents run from varying cwds.
