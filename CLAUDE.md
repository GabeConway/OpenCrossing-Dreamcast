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
# (M0 deliverables — being built now)
dc/build-dc-docker.sh          # sh-elf + KOS + GLdc + mkdcdisc in Docker
harness/dc/smoke.sh            # boot a CDI in Flycast, capture console, assert
```

Docker runs under colima on an Apple M4 (arm64 host, 10 cores, 24 GB).
Flycast is the iteration emulator; the real Dreamcast + burned CD-R is the
truth. Always give absolute paths in scripts — agents run from varying cwds.

## Status (2026-08-01)

Planning complete, M0/M1 execution starting. Nothing builds for Dreamcast
yet. Keep `PLAN.md` and `kb/` updated as facts change — they are the project
memory, and the plan is expected to be revised repeatedly as measurements
come in.
