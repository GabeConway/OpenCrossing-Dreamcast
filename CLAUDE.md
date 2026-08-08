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
- **`src/` builds at `-Os`, with a reviewed hot list at `-O3`.** ⚠️ **THIS
  REVERSES THE OLD `-O0` RULE (2026-08-06, user directive).** The ban came
  from the armhf port's history and was never reproduced on SH-4; measured on
  this tree, `-Os` costs **2,826,288 B less `.text`** and took the town from
  **11.6 to 18.5 FPS**, and the `-O3` hot list took it to **20.0**. The knob is
  `DC_OPT_PROFILE` (`perf` | `size` | `o0`), the lists are `dc/opt-lists.mk`,
  and `DC_OPT_PROFILE=o0` is a byte-identical revert. Optimization is now a
  first-class lever for BOTH speed and RAM — but every raise needs a screenshot
  pair, not just counters, and a TU proven to miscompile goes on the quarantine
  list with its evidence rather than dragging the whole tree back to `-O0`.
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
| 0 | **`kb/RESUME.md`** | **if you are a fresh context: start here.** Where the port is, the build line to use verbatim, and the ranked open list |
| 1 | **`kb/STATE.md`** | current numbers, the fit inequality, next actions. ~200 lines, short by design. |
| 2 | `kb/closed.md` | **read before proposing any RAM/size/architecture idea** — what is already dead, and why |
| 3 | `kb/traps.md` | read before touching the build, harness, prelude, or instrumentation |
| 4 | `PLAN.md` | the port plan: milestones, the four hard problems, risks |
| — | `kb/state-log.md` | only when you need the evidence behind a number in `STATE.md` |

**The port walks the town, with music.** It boots on retail hardware with
**loading at parity with the emulator**, and in Flycast it reaches the town,
walks around it, meets Tom Nook and is taken to the houses. Every summer acre is
in the image — plus, since 2026-08-06, the interiors, the winter set and the
gyroids. ⭐ **Since 2026-08-08 the BGM plays** (the audio command queue is
drained every tick, not only when synthesis runs) **and G3 culls at
`G_TRIN_INDEPEND` entry**: town frame **49.9 ms with sound on**, against 45.6 ms
for the old silent build. Both are ON by default — `kb/RESUME.md` §0c-§0e.
Read `kb/RESUME.md` first — in particular the **nine** measurement rules.
Four were paid for recently: `MEMLEDGER FIT … OK` does not mean the image
boots; an average cost per command is not the cost of any command; **in a
stubbed image, an asset class's resident cost is what the KEEP LIST kept, not
what the class totals** (so a pool is worth building when it delivers content
the keep list cannot afford, not when it "frees" bytes the stub system already
dropped); and — new on 2026-08-06 — **state the denominator: `[EMU64H]` is per
LOGIC TICK, not per presented frame, so every figure it prints must be doubled.
Two sessions quoted the halved numbers.**

⚠️ **RAM has stopped being the binding constraint** (2026-08-06): real headroom
is ~2.05 MB, `-Os` having paid for it. What binds now is **residency** — the
keep list still decides what exists. `kb/STATE.md` is the current arithmetic.

<details><summary>the older status line, kept for the sequence</summary>

**The port draws the title screen.** A `DC_ASSET_STUB=1` + `DC_DISC_ROOT=…`
image boots in Flycast, runs the game loop at **29.3 FPS / 98 % speed**, and
renders the Animal Crossing title overlay — "PRESS START" and the copyright
line — through a real PowerVR backend (`dc/src/dc_pvr.c` +
`dc_pvr_texture.c`). The town behind the logo is black because only 53,792 B
of real assets are in that image; that is the S4 loader's job, not a renderer
bug. Read `kb/STATE.md` before assuming anything is untested.

**Status (2026-08-02): M0/M1 met, M2 has first pixels, still blocked on RAM
for a real-asset build.** All 3917 TUs compile and link for sh-elf.

⚠️ **The heap is TWO pools that compete** — the arena (`__osMalloc`) and KOS
`sbrk` (libc `malloc`) are carved from the same region. Growing the arena
starves libc and makes the game get *less* far. The "Out of memory. Requested
sbrk_base …" message is sbrk, not the arena. `kb/heap-two-pools.md` is the rule
and the measurements; this cost a full debug cycle.

State the *image* fit as **one inequality**, never two pools; splitting it has
already produced two wrong numbers.

</details>

---

## 3. Document map

### Live state — changes as work lands

| file | contents |
|---|---|
| `kb/RESUME.md` | the handoff: current state, the exact build line, what was just fixed, and what is still open |
| `kb/STATE.md` | headline numbers, the fit inequality, the ranked next actions. **Short by design — start here** |
| `kb/state-log.md` | the evidence behind those numbers: what was observed running, when, and what it cost. Newest first |
| `kb/heap-two-pools.md` | the arena-vs-sbrk rule. **Read before touching `DC_ARENA_BYTES` / `DC_ARAM_WINDOW`** |
| `kb/plan-stages.md` | the agreed S1→S5 RAM plan and the reasoning behind each step |
| `kb/levers.md` | **the ranked RAM ledger** — applied cuts, and every lever still live. ⚠️ Its top section now says RAM is **not** the binding constraint; **L10 (T1, textures)** is the live one |
| `kb/closed.md` | settled questions: MMU paging (dead), `--icf`, emu64-is-not-an-emulator, strip/compress = 0 — **and the `-O0` post-mortem, the one entry this file got wrong** |
| `kb/traps.md` | mechanical gotchas: `fsqrt`, POSIX `link()`, `scif_flush()`, `bash -lc`, mkdcdisc padding |
| `kb/boot-blockers.md` | **what the running game hits next**, ranked by reach rather than by bytes. The counterweight to `kb/levers.md` |
| `kb/issues.md` | known game-side bugs and leads (armhf-era, still accurate) |
| `kb/station-bugs.md` | the two train-station bugs traced 2026-08-02: black floor (solved) and roof clip-through — **now reproducible for the first time**, since `DC_AUTOWALK` can walk a character under it |
| `dc/src/dc_emu64_cull.cpp` | **G3, SHIPPED 2026-08-08 and ON by default** (`DC_EMU64_CULL ?= 1`) — the AABB cull at `G_TRIN_INDEPEND` entry. **−19.9 ms of a 69.8 ms town frame**, `fps_p50` 19.5 → 23.2, late-cull `vcull` 9,915 → 1,002. Gate `-DDC_EMU64_CULL_VERIFY` passed `falsecull=0 gfxp_bad=0 reinst=0`. ⚠️ It installs into the same dispatch table as G1/G2 — read the ordering trap in `kb/traps.md` before touching any of the three |
| `dc/src/dc_emu64_hist.c` | **G1** — the per-opcode emu64 timing histogram (`DC_EMU64_HIST=<N>`). Thunks swapped into emu64's dispatch table at runtime; `src/` untouched. **Run 2026-08-05 and again 2026-08-06**, and it is the only thing allowed to price an opcode. ⚠️ **Its output is per LOGIC TICK — double it** (measurement rule 9) |
| `harness/dc/bench/` | `bench_mem.c` plus the build path it never had. It passes — and Flycast cannot answer it (`kb/closed.md`) |
| `PLAN.md` | milestones, the four hard problems, risk register, open questions |

### Build & test

| file | contents |
|---|---|
| `BUILDING-DC.md` | **the DC build**: entry points, make targets, env knobs, flag assembly, include-path order, prelude, troubleshooting. Its optimization section is now `DC_OPT_PROFILE`, not the old `-O0` rule |
| `dc/opt-lists.mk` | **the `-O3` hot list and the `-O0` quarantine list**, each entry with the measurement or symptom that earned it. Read before adding either; a stale entry is a hard error, not a no-op |
| `tools/dcopt/` | `warnscan_report.py` reduces the `make warnscan` log to the UB classes an optimizer can act on; `bisect_o0.sh` + `predicate_town.sh` binary-search a miscompiling TU through `DC_OPT_O0_EXTRA` |
| `harness/dc/README.md` | Flycast harness: setup, the scripts, guest-side protocol, env overrides, known limits |
| `tools/dcqa/run_report.py` | **the regression gate.** Reduces a `console.log` to the ~20 numbers a "did this get worse" call rests on; `--vs` diffs two runs. A game smoke run always exits 1, so this is the verdict, not the exit code. ⚠️ It is the FLOOR — it cannot see colour, so judge a renderer change on a screenshot pair |
| `kb/design-toolchain.md` | **index** to the M0 toolchain docs below. Everything is tagged [VERIFIED]/[UNVERIFIED] |
| `kb/toolchain-decision.md` | why build from source with `kos-chain`; rejected options; fallbacks F1–F5 |
| `kb/toolchain-host-env.md` | colima/Docker facts, the `$HOME`-only bind-mount trap, the qemu 17–23×/ICE measurement |
| `kb/toolchain-components.md` | kos-chain / KOS / GLdc / mkdcdisc recipes, float ABI, the signed-`char` correction |
| `kb/toolchain-dockerfile.md` | the two-stage Dockerfile with pinned SHAs; the mkdcdisc `-N` padding measurement |
| `kb/toolchain-build-invocation.md` | measured image build times; bind-mount / container invocation design |
| `kb/design-harness.md` | **split index** — harness design rationale, tagged [V]/[U]. Parts: `-flycast-setup`, `-capture` (console/SCIF, framebuffer hash, crash triage), `-runner`, `-alternatives` (lxdream/RetroArch, NO-GO), `-corrections` (⚠️ what the real KOS 2.3 image changed — read alongside any other part) |
| `kb/design-shelf-hazards.md` | **split index** — sh-elf compile hazards for `src/`. ⚠️ measured on GCC 9.3/KOS `525cbda`, **not** our GCC 15.2/KOS 2.3; it missed both collisions that actually bit us, and parts of it tell you to edit `src/`, which §1 forbids. Parts: `-exclusions`, `-flags`, `-alignment`, `-abi-libc` |
| `BUILDING.md`, `kb/build-test.md` | **armhf-era.** Base-repo build, not this target |
| `harness/README.md`, `portmaster/**/README.md` | **armhf-era.** Old 3-tier harness and PortMaster packaging — not this target |

### Memory — the blocking problem

| file | contents |
|---|---|
| `kb/levers.md` | start here for any size work |
| `kb/ram-plan.md` | **the solution stack that closes the gap** — eight ranked moves P1–P8 with closing arithmetic, second-rank levers, experiment queue. Documentation only, nothing implemented |
| `kb/research-budget-premises.md` | **split index** — audit of the budget's premises |
| `kb/research-budget-corrected.md` | §1: the corrected image budget and the single-inequality fit test |
| `kb/research-budget-bucket6.md` | §2: the arena — 1,294,497 B provably dead. **§2.4 is the bucket-6 measurement recipe** (now partly answered by `DC_ARENA_PROBE`) |
| `kb/research-budget-evidence.md` | §3: the sourced numbers — KOS memory model, the bucket-1 double-count, `s_assets[]` |
| `kb/research-budget-actions.md` | §4/§5/§7: revised ledger, ranked cheapest actions, bottom line |
| `kb/research-budget-unfinished.md` | **§6: what is unfinished**, numbered, with next steps |
| `kb/research-size-reduction.md` | **split index** — fitting in 16 MB without changing codegen; source of `kb/levers.md` L3, which re-costed every number here and found them all wrong. Parts (⚠️ the files are `kb/research-size-*.md`, NOT `research-size-reduction-*`): `-baseline`, `-techniques`, `-memory-map`, `-plan` |
| `kb/mem-budget.md` | **index** to the split ledger below. ⚠️ the §4 ledger itself is void |
| `kb/mem-budget-m1-sh4.md` | **the part that is still true** — the real sh-elf link: section sizes, the `.bss` split, levers applied, dead ends, the boot-size gate |
| `kb/mem-budget-armhf-working-set.md` | armhf-era: the 65 MB start, the 15.6 MB REL boot transient, boot residency, disc contents |
| `kb/mem-budget-armhf-binary-size.md` | armhf-era: section totals, per-tree attribution, top `.bss`/`.text` symbols. ⚠️ its "the only real lever is codegen" conclusion violates §1 and is void |
| `kb/mem-ledger-runtime-design.md` | `dc/src/dc_mem_ledger.c` design — the budget as a runtime object that fails loudly |
| `kb/mem-probe-plan.md` | `PC_MEMPROBE=1` probes for the real arena / ARAM / asset high-water marks. Partly overtaken by `DC_ARENA_PROBE` |
| `kb/mem-budget-void-ledger.md` | ⚠️ **VOID** — the 12-bucket ledger and C1–C11. Kept only for its bucket numbering |
| `kb/research-mmu-paging.md` | **the DEAD verdict + index.** Read before ever reconsidering MMU paging |
| `kb/research-mmu-kos-capability.md` | [DEAD] what KOS's MMU driver can do: 4 KB pages, 253,952 B TLB reach, no eviction, forced uncached |
| `kb/research-mmu-fault-cost.md` | [DEAD] interrupts masked in the fault handler ⇒ no CD-backed pager; SH7750 hazards |
| `kb/research-mmu-hardware-tax.md` | [DEAD] static remapping is redundant against P1/P2; the store-queue erratum taxes *any* MMU use |
| `kb/research-mmu-game-impact.md` | [DEAD] the five semantic breakages in `src/` (DMA first); why `.text` need not page |
| `kb/research-mmu-reopening.md` | [DEAD] precedent, the four preconditions to reopen, all sources |
| `kb/research-n64-origin.md` | why the 22.5 MB image is *not* an emulation artefact |
| `kb/research-creative-ram.md` | **unbanked CONCEPTS**, ranked, each with a failure mode and a cheapest experiment. ⚠️ **T1 has GRADUATED — it is designed, much cheaper than this page says, and lives in `kb/levers.md` L10** (−579,248 B, then all 5,685 remaining textures for +68,000 B). The rest of the page needs re-ranking now that the gap is gone |
| `kb/research-second-tier-memory.md` | ⚠️ salvaged fragment, not a finished doc. VRAM/AICA bandwidth, never run |

### Assets & disc

| file | contents |
|---|---|
| `kb/asset-pack.md` | **the `assets.pak` contract** between `tools/dcasset` and the future `dc/` runtime. Built and verified against the real ISO |
| `tools/dcasset/README.md` | the extraction/pack tool: what it does, usage, what remains |
| `tools/dcstub/make_stub_data.py` | the `DC_ASSET_STUB` rewriter (S1). Header comment is the doc |
| `tools/dcstub/measure_dedup.py` | the L6 dedup measurement (S2). Header comment is the doc |
| `tools/dcstub/census_resolve.py` | resolves a `DC_ASSET_CENSUS=1` console log into the scene's real working set (symbol, size, kind). The only way to learn what a scene touches — acres and NPCs are named by index, not by symbol |
| `tools/dcstub/census_keeplist.py` | joins a resolved census to the linked map and emits `DC_STUB_KEEP`. Replaces the hand-written keep list |
| `tools/dcstub/keeplist-opening.txt` | the censused keep list for the opening scene. Still the right list for title-screen and size work |
| `tools/dcstub/keeplist-town.txt` | **the keep list to BUILD WITH.** All 371 summer acre TUs, the map overlay, the date/time HUD, Nook and the raccoon NPCs, two houses. ⚠️ Enumerated from the tree, NOT censused — `sys_math.c:7` seeds the town from `sqrand(osGetCount())`, so every boot lays out a different town and no census can be correct |
| `tools/dcstub/make_keeplist_town.py` | generates the above; its header carries the reasoning and the cost |
| `tools/dcfb/fbimg_to_png.py` | decodes a `DC_FB_IMAGE` console log into PNG screenshots. No third-party modules |
| `tools/dcstub/make_src_shrink.py` | the `DC_SRC_SHRINK` scratch-tree data rewriter (S3, on by default). Header comment is the doc |
| `dc/stage-disc.sh` | flatten a `dcasset extract` tree into a disc root for `DC_DISC_ROOT` |
| `kb/save-budget.md` | **verdict + index** — 295,910 B of save into a 100 KB VMU. Harness: `tools/savebench/`. ⚠️ all compression numbers are SYNTHETIC |
| `kb/save-layout.md` | what the save actually is: verified struct sizes, `.gci` layout, bytes by field group |
| `kb/save-compression.md` | codec comparison and the VMU fit verdict (synthetic), plus the measured PLAN §6 options |
| `kb/save-plan.md` | the recommended VMU plan, compression + flash write cost, open items |
| `tools/savebench/README.md` | save-size measurement harness |
| `tools/savebench/dcvmu/README.md` | on-console VMU flash-cost harness — source of the 84.6 ms/block number |

### Graphics & audio

| file | contents |
|---|---|
| `kb/tev-map.md` | **split index** — all 101 TEV configs → fixed-function PVR strategies. Parts: `-table` (**the reference table — load this to implement**), `-implementation`, `-alpha`, `-hard-cases`, `-decoding` |
| `kb/audio-plan.md` | jaudio_NES on DC. **Verdict: a real risk, not solved** — verdict + index |
| `kb/audio-engine.md` | what the engine is (N64 libaudio, *not* JAudio), shipped parameters, `audiorom.img` |
| `kb/audio-cpu-cost.md` | per-voice op counts, the SH-4 cycle model, A0–A4 configs, the sm64-dc precedent |
| `kb/audio-stage-a-software.md` | stage A: rspsim at 22 kHz, effect-cut order, `snd_stream`, main-RAM footprint |
| `kb/audio-stage-b-aica.md` | stage B: the exact seam, AICA register map, residency policy, the offline tools |
| `kb/audio-plan-of-record.md` | the plan of record, and the ranked list of what is still unmeasured |
| `kb/upstream-pc-port.md` | what the upstream ACGC-PC-Port releases do and do NOT give us. **Their perf work does not transfer** (shader variants; the PVR has none, and we are CPU-bound). Read before mining their changelog |
| `kb/research-fps-ideas.md` | **unbanked FPS concepts**, ranked, each with a failure mode and a cheapest experiment. Carries the 60 %-of-vertices-culled-after-the-fact finding, the emu64 `G_CULLDL` route, and the **decision gate** on interposing on emu64's dispatch table |
| `kb/research-ram-tiers.md` | **second-tier memory concepts** (VRAM/AICA as eviction tiers, cold `.text` relocation, `--wrap=malloc`), plus **two corrections to `kb/ram-plan.md`'s arithmetic** |
| `kb/perf-dc.md` | **where the town frame actually goes**, measured. ⚠️ Its §2b numbers are stale twice over — `-O0`, *and* halved by the per-tick denominator. Re-run 2026-08-06: **`G_TRIN_INDEPEND` is 34.4 ms of a 45.6 ms frame (75 %)**, of which only `cull 2.0 + xform 8.8` is attributed — **~23.6 ms is `dl_G_TRIN`'s index expansion plus our own `GX*` setters, never separated**, and that is the largest unattributed block in the project. `gap` is CLOSED (it is the dispatch loop). Also the instruments, the applied per-vertex wins, and what was ruled out |
| `kb/texture-path.md` | **what happens to a GC texture on its way to VRAM** — filtering, format, per-format colour loss, mipmaps, the VRAM budget, the NPOT pad. Read before proposing any texture-quality work: paletted and VQ are both closed here with reasons |
| `kb/renderer.md` | the GX→GLES layer. **armhf-era** — accurate on the GX layer's behavior, wrong on hardware |
| `kb/design-platform-api.md` | **index** to the platform-API split below — every symbol `dc/` must provide, derived from `pc/src` |
| `kb/platform-api-overview.md` | symbol counts and dispositions, ranked landmines, unverified gaps. **Read first — carries the table legend** |
| `kb/platform-api-boot-order.md` | the verified init order and the six hard ordering rules |
| `kb/platform-api-os-core.md` | arena / the `+0x28` word, cache maintenance, time & ticks, threads; the `OS*` tables |
| `kb/platform-api-os-stubs.md` | REL, libc64 malloc, N64 trig, PPC, EXI/SI/DB, libc, libultra, GBA, Famicom, JSystem vtables |
| `kb/platform-api-math.md` | `PSMTX*`/`C_MTX*`/`gu*`, with the SH-4 FTRV/FIPR candidates |
| `kb/platform-api-gx.md` | the GX state machine and every `GX*` symbol, incl. textures and TLUTs |
| `kb/platform-api-vi-pad.md` | `VI*` frame pacing and `PAD*` input |
| `kb/platform-api-dvd-aram.md` | `DVDReadAsyncPrio`, `ARStartDMA`, the runtime asset table, the disc reader |
| `kb/platform-api-audio.md` | `AI*`/`DSP*` — the `AIInitDMA` handoff point |
| `kb/platform-api-save-card.md` | save layout, `CARD*`, `pc_m_card.c`, GCI byte-swap |
| `kb/platform-api-pc-only.md` | settings, profiler, overlay, texture packs — the `drop` pile |
| `kb/platform-api-globals.md` | global *variables* the platform layer must define |

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

`PLAN.md`, `kb/STATE.md`, `kb/state-log.md`, `kb/levers.md`, `kb/closed.md` and
`kb/traps.md` are the project memory. Update them as facts change — the plan is
expected to be revised repeatedly as measurements come in.

**When you settle something, move it:** a dead idea goes to `kb/closed.md`, a
debugging gotcha to `kb/traps.md`, a live saving to `kb/levers.md`, and the
narrative of what was observed goes to `kb/state-log.md`. **`STATE.md` stays
short** — if a section there is growing a history, that history belongs in the
log. If you add a document, add a row to §3.

⚠️ kb docs from the first fan-out were written by agents whose adversarial
verifiers all died: **treat their numbers as claims until verified.** Three have
already been falsified — see the caveat at the end of `kb/closed.md`.
