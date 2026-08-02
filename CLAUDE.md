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

**The port boots.** `DC_ASSET_STUB=1 bash dc/build-dc.sh` builds a throwaway
image whose asset arrays are one element each; it fits (`MEMLEDGER FIT …
margin=725740 OK`) and runs all the way to `JW_Init2` in Flycast. Read
`kb/STATE.md` §"S1 IS DONE" before assuming anything is untested.

**Status (2026-08-01): M0 and M1 met, M2 blocked on RAM.** All 3917 TUs compile
and link for sh-elf. The image spans 21,374,068 B against 16,646,144 B of
usable RAM and **is over by 8,273,108 B**, so it links but will not boot — the
failure is size alone, proven by controlled experiment. State the fit as **one
inequality**, never two pools; splitting it has already produced two wrong
numbers.

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
| `kb/research-budget-premises.md` | audit of the budget's premises; §2.4 is the bucket-6 measurement recipe, §6 lists what is unfinished |
| `kb/research-size-reduction.md` | fitting in 16 MB without changing codegen — source of `kb/levers.md` L3 |
| `kb/mem-budget.md` | the original 16 MB ledger. ⚠️ superseded in parts — its own header lists two corrections, and its `-O2`/`-Os` remedies are void |
| `kb/research-mmu-paging.md` | why MMU paging is DEAD. Read before ever reconsidering |
| `kb/research-n64-origin.md` | why the 22.5 MB image is *not* an emulation artefact |
| `kb/research-second-tier-memory.md` | ⚠️ salvaged fragment, not a finished doc. VRAM/AICA bandwidth, never run |

### Assets & disc

| file | contents |
|---|---|
| `kb/asset-pack.md` | **the `assets.pak` contract** between `tools/dcasset` and the future `dc/` runtime. Built and verified against the real ISO |
| `tools/dcasset/README.md` | the extraction/pack tool: what it does, usage, what remains |
| `tools/dcstub/make_stub_data.py` | the `DC_ASSET_STUB` rewriter (S1). Header comment is the doc |
| `tools/dcstub/measure_dedup.py` | the L6 dedup measurement (S2). Header comment is the doc |
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
