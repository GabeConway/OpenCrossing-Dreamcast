# CLAUDE.md

**OpenCrossing-Dreamcast** — native Sega Dreamcast port of Animal Crossing
(GameCube decomp). Target: **retail Sega Dreamcast, stock 16 MB RAM**, SH-4 @
200 MHz, PowerVR CLX2 (fixed-function, no shaders), KallistiOS. Dev console is
a known-good MIL-CD unit that boots burned CD-Rs.

**This file is an index.** It carries the rules that must never be violated,
and a map of every other document. Everything else is loaded on demand — do not
read the whole `kb/` tree, read the one file the table points at.

---

## 1. Hard rules — violating these breaks the port

- **Stock 16 MB RAM.** A 32 MB mod exists; it must never become a requirement.
- **`src/` builds at `-O0`. `-O1`/`-O2`/`-Os`/LTO are banned** — user
  directive, not a preference: "the optimizations cause problems and we cant
  use them without the port being broken." Do not propose optimization as a
  size or speed lever, and do not benchmark it as one. **Codegen is banned;
  layout is fair game** — `--gc-sections`, `.bss` right-sizing, linker
  placement, moving data to `/cd`, dropping subsystems.
- **Never edit `src/` to make it compile.** Compat fixes go in
  `dc/include/dc_prelude.h`, which is force-included. All 3917 TUs build this
  way with zero exclusions. `src/` carries exactly **four** small
  `#if defined(TARGET_DC)` branches; that is the whole licence.
- **`-DTARGET_PC` must stay.** It means "not GameCube", not "PC" — it guards
  the base port's little-endian correctness fixes. `-DTARGET_DC` goes
  *alongside* it.
- **Never commit ROM material or built disc images.** No `.iso`/`.gcm`/`.cdi`/
  `.gdi`/`.gci`. The user's ISO lives at
  `/Users/gabe/Documents/GitHub/OpenCrossing-Anbernic/harness/rom/Animal Crossing.iso`
  (GAFE01 USA Rev 0, 1,459,978,240 bytes) — reference it, never copy it in.
- **`pc/` is reference material, not the build target.** The Dreamcast platform
  layer is `dc/`. Do not "fix" `pc/` for Dreamcast. Host-side tools go in
  `tools/`.
- **Branches:** `main` = releases, `dev` = daily work. Never tag dev. Agents
  must not run git — the main thread commits.
- **Every optimization gets a kill switch** (compile-time or settings flag).

### Hardware contract

8 MB VRAM, textures twiddled 16-bit / VQ / paletted — never linear RGBA8. No
shaders, no hardware T&L, one texture unit; vertex transform and lighting run
on SH-4. 2 MB AICA sound RAM, 64 ADPCM channels, weak ARM7 — do not plan CPU
work on the ARM7. VMU ≈ 100 KB user data vs a ~456 KB GC save. CD-R streams at
~500 KB/s, so all disc I/O needs read-ahead.

---

## 2. Start here

| order | file | why |
|---|---|---|
| 1 | **`kb/STATE.md`** | current numbers, boot status, next actions. Short by design. |
| 2 | `kb/closed.md` | **read before proposing any RAM/size/architecture idea** — what is already dead, and why |
| 3 | `kb/traps.md` | read before touching the build, harness, or prelude |
| 4 | `PLAN.md` | the port plan: milestones, the four hard problems, risks |

**The port draws the title screen.** A `DC_ASSET_STUB=1` + `DC_DISC_ROOT=…`
image boots in Flycast, runs the game loop at **29.3 FPS / 98 % speed**, and
renders the Animal Crossing title overlay — "PRESS START" and the copyright
line — through a real PowerVR backend (`dc/src/dc_pvr.c` +
`dc_pvr_texture.c`). The town behind the logo is black because only 53,792 B
of real assets are in that image; that is the S4 loader's job, not a renderer
bug. Read `kb/STATE.md` **top section** before assuming anything is untested.

**Status (2026-08-02): M0/M1 met, M2 has first pixels, still blocked on RAM
for a real-asset build.** All 3917 TUs compile and link for sh-elf.

⚠️ **The heap is TWO pools that compete** — the arena (`__osMalloc`) and KOS
`sbrk` (libc `malloc`) are carved from the same region. Growing the arena
starves libc and makes the game get *less* far. The "Out of memory. Requested
sbrk_base …" message is sbrk, not the arena. `kb/STATE.md` has the measurements;
this cost a full debug cycle.

State the *image* fit as **one inequality**, never two pools; splitting it has
already produced two wrong numbers.

---

## 3. Document map

### Live state — changes as work lands

| file | contents |
|---|---|
| `kb/STATE.md` | headline numbers, the fit inequality, boot status, next actions |
| `kb/levers.md` | **the ranked RAM ledger** — applied cuts, and every lever still live |
| `kb/closed.md` | settled questions: `-O0`, MMU paging (dead), `--icf`, emu64-is-not-an-emulator, strip/compress = 0 |
| `kb/traps.md` | mechanical gotchas: `fsqrt`, POSIX `link()`, `scif_flush()`, `bash -lc`, mkdcdisc padding |
| `kb/issues.md` | known game-side bugs and leads (armhf-era, still accurate) |
| `PLAN.md` | milestones, the four hard problems, risk register, open questions |

### Build & test

| file | contents |
|---|---|
| `BUILDING-DC.md` | **the DC build**: entry points, make targets, env knobs, flag assembly, include-path order, prelude, troubleshooting |
| `harness/dc/README.md` | Flycast harness: setup, the scripts, guest-side protocol, env overrides, known limits |
| `kb/design-toolchain.md` | how the SDK image was built and measured (M0). Tagged [VERIFIED]/[UNVERIFIED] |
| `kb/design-harness.md` | harness design rationale, tagged [V]/[U] |
| `kb/design-shelf-hazards.md` | sh-elf compile hazards for `src/`. ⚠️ measured on GCC 9.3/KOS `525cbda`, **not** our GCC 15.2/KOS 2.3 — it missed both collisions that actually bit us |
| `BUILDING.md`, `kb/build-test.md` | **armhf-era.** Base-repo build, not this target |

### Memory — the blocking problem

| file | contents |
|---|---|
| `kb/levers.md` | start here for any size work |
| `kb/ram-plan.md` | **the solution stack that closes the gap** — eight ranked moves P1–P8 with closing arithmetic, second-rank levers, experiment queue. Documentation only, nothing implemented |
| `kb/research-budget-premises.md` | audit of the budget's premises; §2.4 is the bucket-6 measurement recipe, §6 lists what is unfinished |
| `kb/research-size-reduction.md` | fitting in 16 MB without changing codegen — source of `kb/levers.md` L3 |
| `kb/mem-budget.md` | the original 16 MB ledger. ⚠️ superseded in parts — its own header lists two corrections, and its `-O2`/`-Os` remedies are void |
| `kb/research-mmu-paging.md` | why MMU paging is DEAD. Read before ever reconsidering |
| `kb/research-n64-origin.md` | why the 22.5 MB image is *not* an emulation artefact |
| `kb/research-creative-ram.md` | **unbanked CONCEPTS**, ranked, each with a failure mode and a cheapest experiment. T1 (textures never pooled) is the highest-value open idea in the project |
| `kb/research-second-tier-memory.md` | ⚠️ salvaged fragment, not a finished doc. VRAM/AICA bandwidth, never run |

### Assets & disc

| file | contents |
|---|---|
| `kb/asset-pack.md` | **the `assets.pak` contract** between `tools/dcasset` and the future `dc/` runtime. Built and verified against the real ISO |
| `tools/dcasset/README.md` | the extraction/pack tool: what it does, usage, what remains |
| `tools/dcstub/make_stub_data.py` | the `DC_ASSET_STUB` rewriter (S1). Header comment is the doc |
| `tools/dcstub/measure_dedup.py` | the L6 dedup measurement (S2). Header comment is the doc |
| `tools/dcstub/census_resolve.py` | resolves a `DC_ASSET_CENSUS=1` console log into the scene's real working set (symbol, size, kind). The only way to learn what a scene touches — acres and NPCs are named by index, not by symbol |
| `dc/stage-disc.sh` | flatten a `dcasset extract` tree into a disc root for `DC_DISC_ROOT` |
| `kb/save-budget.md` | 295,910 B of save into a 100 KB VMU. Harness: `tools/savebench/` |
| `tools/savebench/README.md` | save-size measurement harness |

### Graphics & audio

| file | contents |
|---|---|
| `kb/tev-map.md` | all 101 TEV configs the game uses → fixed-function PVR strategies |
| `kb/audio-plan.md` | jaudio_NES on DC. **Verdict: a real risk, not solved** |
| `kb/renderer.md` | the GX→GLES layer. **armhf-era** — accurate on the GX layer's behavior, wrong on hardware |
| `kb/design-platform-api.md` | every symbol `dc/` must provide, derived from `pc/src`. 124 KB — grep it, don't read it |

### Background & reference

| file | contents |
|---|---|
| `kb/base-repo-map.md` | what transfers from the Anbernic base, what dies |
| `kb/research-dreamcast.md` | KOS/PVR/AICA facts, precedent ports |
| `kb/research-ecosystem.md` | decomp ecosystem, upstream state, legal model |
| `kb/game.md` | game-side knowledge: decomp, emu64, data |
| `kb/history.md` | project history & decisions |
| `kb/device.md`, `kb/perf.md` | **armhf-era.** Accurate on game/decomp facts, wrong on hardware |
| `pc/DOCUMENTATION.md` | the PC port's architecture. Reference only |
| `README.md`, `docs/*.md` | project intro; decomp/Ghidra onboarding |

---

## 4. Toolchain quick reference

```bash
dc/build-dc-image.sh           # build opencrossing-dc:sdk (idempotent, ~27 min cold — do not rebuild)
dc/build-dc.sh                 # HOST entry point: build -> ELF + unpadded CDI
DC_TARGET=objs dc/build-dc.sh  # compile only, no link
harness/dc/smoke.sh            # boot a CDI in Flycast, capture console, assert
harness/dc/crash.sh            # symbolise a fault via sh-elf-addr2line
```

`dc/build-dc-docker.sh` runs *inside* the container — not a host entry point.
Docker runs under colima on an Apple M4 (arm64 host, 10 cores, 24 GB) and this
host has **no BuildKit**: use `DOCKER_BUILDKIT=0` and never pass `--progress`.
Inside the SDK image use `bash -c`, never `bash -lc`. Pass `-N` to mkdcdisc for
emulator runs. Flycast is the iteration emulator; the real Dreamcast + burned
CD-R is the truth. Always give absolute paths in scripts — agents run from
varying cwds. Full rationale for each: `kb/traps.md`.

---

## 5. Keeping this current

`PLAN.md`, `kb/STATE.md`, `kb/levers.md`, `kb/closed.md` and `kb/traps.md` are
the project memory. Update them as facts change — the plan is expected to be
revised repeatedly as measurements come in.

**When you settle something, move it:** a dead idea goes to `kb/closed.md`, a
debugging gotcha to `kb/traps.md`, a live saving to `kb/levers.md`. `STATE.md`
stays short. If you add a document, add a row to §3.

⚠️ kb docs from the first fan-out were written by agents whose adversarial
verifiers all died: **treat their numbers as claims until verified.** Three have
already been falsified — see the caveat at the end of `kb/closed.md`.
