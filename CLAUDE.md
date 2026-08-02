# CLAUDE.md

**OpenCrossing-Dreamcast** — native Sega Dreamcast port of Animal Crossing
(GameCube decomp). Bootstrapped 2026-08-01 by merging
`GabeConway/OpenCrossing-Anbernic` (armhf handheld port) into a fresh repo.
Target: **retail Sega Dreamcast, stock 16 MB RAM**, SH-4 @ 200 MHz, PowerVR
CLX2 (fixed-function, no shaders), KallistiOS. Dev console is a known-good
MIL-CD unit that boots burned CD-Rs.

> This repo still contains the full Anbernic/PC platform layer in `pc/` and
> its `kb/` docs. **`pc/` is reference material, not the build target.** The
> Dreamcast platform layer lives in `dc/`. Do not "fix" `pc/` for Dreamcast.

## Read first

1. `PLAN.md` — the port plan. Milestones, the four hard problems, risks.
2. `kb/base-repo-map.md` — what transfers from the Anbernic base, what dies.
3. `kb/research-dreamcast.md` — KOS/PVR/AICA facts, precedent ports.
4. `kb/research-ecosystem.md` — decomp ecosystem, upstream state, legal model.

Anbernic-era kb files (`kb/device.md`, `renderer.md`, `perf.md`,
`build-test.md`, `game.md`, `history.md`, `issues.md`) describe the **ARM
handheld port**. They are accurate about the game code, emu64, saves, and the
GX layer's behavior — and wrong about anything device- or GLES-specific.
Read them for game/decomp facts; ignore their hardware advice.

## Ground rules

- Branches: `main` = releases, `dev` = daily work. Never tag dev.
- **Never commit ROM material or built disc images.** No `.iso`/`.gcm`/
  `.cdi`/`.gdi`/`.gci`. The user's ISO lives at
  `/Users/gabe/Documents/GitHub/OpenCrossing-Anbernic/harness/rom/Animal Crossing.iso`
  (GAFE01 USA Rev 0, 1,459,978,240 bytes) — reference it, never copy it in.
- Decomp game code (`src/`) is vendored: gnu89, UB-dependent, 32-bit pointer
  assumptions everywhere. SH-4 is ILP32 little-endian, so the base port's
  32-bit and endianness work carries over unchanged. Keep the UB-guard flags.
- **`src/` builds at `-O0`. `-O1`/`-O2`/`-Os`/LTO are banned** — user
  directive, not a preference: "the optimizations cause problems and we cant
  use them without the port being broken." armhf record: `-O2` = wild-pointer
  crash loop from boot, `-O1` = hard SIGBUS on the intro train scene. Do not
  propose optimization as a size or speed lever, and do not benchmark it as
  one. What *is* allowed is anything that doesn't change instruction
  selection: `-ffunction-sections -fdata-sections` + `-Wl,--gc-sections`,
  `.bss` right-sizing, linker script placement, moving data to `/cd`, dropping
  non-goal subsystems. Codegen is banned; layout is fair game.
- **Never edit `src/`** to make it compile. Compat fixes go in
  `dc/include/dc_prelude.h`, which is force-included. All 3917 TUs build this
  way today with zero exclusions.
- Platform code: `dc/` (Dreamcast) — new. `pc/` (Linux/SDL/GLES) — reference.
- Host-side tools (asset extraction, conversion, disc build) go in `tools/`.
- Every optimization gets a kill switch, as in the base repo (`PC_NO_*`
  env vars there; compile-time or settings flags here).

## Hardware contract (do not violate)

- **16 MB main RAM, stock.** A 32 MB RAM mod exists; it must never become a
  requirement. Budget ledger in `PLAN.md` §3.1.
- **8 MB VRAM.** Textures are twiddled 16-bit / VQ / paletted — never linear
  RGBA8.
- **No shaders, no hardware T&L, one texture unit.** Vertex transform and
  lighting run on SH-4. TEV configs map to fixed-function PVR modes,
  per-vertex color, or extra passes — see `PLAN.md` §3.3.
- **2 MB AICA sound RAM,** 64 ADPCM channels, weak ARM7. Do not plan CPU work
  on the ARM7.
- **VMU ≈ 100 KB user data** vs a ~456 KB GC save. See `PLAN.md` §6.
- **CD-R streams at ~500 KB/s.** All disc I/O needs read-ahead.

## Toolchain & testing

```bash
dc/build-dc-image.sh           # build opencrossing-dc:sdk (idempotent, ~27 min cold)
dc/build-dc.sh                 # HOST entry point: build -> ELF + unpadded CDI
DC_TARGET=objs dc/build-dc.sh  # compile only, no link
harness/dc/smoke.sh            # boot a CDI in Flycast, capture console, assert
harness/dc/crash.sh            # symbolise a fault via sh-elf-addr2line
```

`dc/build-dc-docker.sh` runs *inside* the container — not a host entry point.
Docker runs under colima on an Apple M4 (arm64 host, 10 cores, 24 GB) and this
host has **no BuildKit**: use `DOCKER_BUILDKIT=0` and never pass `--progress`.
Inside the SDK image use `bash -c`, never `bash -lc` — `-l` re-runs
`/etc/profile`, drops `sh-elf/bin` from PATH, and every address then
symbolises to `??`. Pass `-N` to mkdcdisc for emulator runs (unpadded: 1.8 MB
/ 0.02 s vs 740 MB / 15.6 s). Flycast is the iteration emulator; the real
Dreamcast + burned CD-R is the truth. Always give absolute paths in scripts —
agents run from varying cwds.

## Status (2026-08-01)

**M0 and M1 met. M2 blocked on RAM.** All 3917 TUs compile and link for
sh-elf; the harness is verified against real CDIs. The linked image spans
**21,374,068 B against a 16 MB machine** (text 6,318,552 / data 2,638,852 /
bss 12,415,508), so it links but will not boot — the failure is size alone,
proven by controlled experiment.

State the fit as **one inequality**, never as two pools; splitting it has
already produced two wrong numbers:

```
(image span) + (genuinely additive heap) ≤ 16,646,144
  21,374,068  +  3,545,184   ⇒ over by 8,273,108 B
```

**8,273,108 B must come out using layout levers only.** KOS's `mm_sbrk()`
starts at the ELF `end` symbol with no MMU and no lazy commit, so every `.bss`
byte destroys a heap byte. The one lever big enough is demand-loading the
8,771,358 B of asset destination arrays out of `.bss`.

**Read `kb/STATE.md` first** — it carries current numbers, the ranked RAM
levers, and a list of traps already paid for (the `fsqrt` collision, POSIX
`link()`, guest `scif_flush()` killing the Flycast console, `-fno-builtin`
breaking the link). Do not re-discover them.

Keep `PLAN.md`, `kb/STATE.md`, and `kb/` updated as facts change — they are
the project memory, and the plan is expected to be revised repeatedly as
measurements come in. kb docs from the first fan-out were written by agents
whose adversarial verifiers all died: treat their numbers as claims until
verified. Two have already been falsified.
