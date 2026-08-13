# CLAUDE.md

**OpenCrossing-Dreamcast** — native Sega Dreamcast port of Animal Crossing
(GameCube decomp). Target: **retail Sega Dreamcast, stock 16 MB RAM**, SH-4 @
200 MHz, PowerVR CLX2 (fixed-function, no shaders), KallistiOS. Dev console is
a known-good MIL-CD unit that boots burned CD-Rs.

**This file is an index.** It carries the rules that must never be violated and
a map of every other document. Everything else is loaded on demand — do not read
the whole `kb/` tree, read the one file the table points at. **It carries no
status narrative**: for where the port is, read `kb/RESUME.md`; for the numbers,
`kb/STATE.md`.

---

## 1. Hard rules — violating these breaks the port

- **Stock 16 MB RAM.** A 32 MB mod exists; it must never become a requirement.
- **`src/` builds at `-Os`, with a reviewed hot list at `-O3`.** ⚠️ **THIS
  REVERSES THE OLD `-O0` RULE** (2026-08-06, user directive). The ban came from
  the armhf port's history and was never reproduced on SH-4; measured on this
  tree, `-Os` cost **2,826,288 B less `.text`** and took the town from **11.6 to
  18.5 FPS**, and the `-O3` hot list took it to **20.0**.
  ⚠️ **`-O0` IS NOT GONE — IT IS CONDITIONAL, AT THREE LEVELS**, and reading
  "the ban is reversed" as "`-O0` is finished" is the error to avoid:

  | level | knob | when |
  |---|---|---|
  | whole tree | `DC_OPT_PROFILE=o0` | a **byte-identical revert**. Keep it working — that is the contract that replaced the ban |
  | per TU, permanent | `OPT_QUARANTINE_SRC` in `dc/opt-lists.mk` | a TU **measured** to miscompile. `-O0` regardless of profile. **Currently empty** — do not pre-populate it out of caution |
  | per TU, throwaway | `DC_OPT_O0_EXTRA` (⚠️ **space**-separated) | bisecting, driven by `tools/dcopt/bisect_o0.sh`. The env knob is for the search, not for the fix |

  `DC_OPT_PROFILE=size` (flat `-Os`) is the reserve lever if an image stops
  fitting. Every raise needs a screenshot pair, not just counters; a TU proven
  to miscompile goes on the quarantine list **with its evidence** rather than
  dragging the whole tree back.
- **Never edit `src/` to make it compile.** Compat fixes go in
  `dc/include/dc_prelude.h`, which is force-included. All 3,936 objects in the
  link build this way with zero exclusions. The whole decomp tree carries
  **five** `#if defined(TARGET_DC)` branches in four files (verified
  2026-08-09): `src/game/m_play.c`, `src/static/jsyswrap.cpp`,
  `src/static/JSystem/JUtility/JUTXfb.cpp` — one each — and
  `include/libforest/emu64/texture_cache.h`, two. That is the whole licence.
  ⚠️ An older figure of "four" is quoted in the git history; it counted the
  four `.bss` levers, not the branches that carry them.
- **`-DTARGET_PC` must stay.** It means "not GameCube", not "PC" — it guards the
  base port's little-endian correctness fixes. `-DTARGET_DC` goes *alongside* it.
- **Never commit ROM material or built disc images.** No `.iso`/`.gcm`/`.cdi`/
  `.gdi`/`.gci`. The user's ISO lives at
  `/Users/gabe/Documents/GitHub/OpenCrossing-Anbernic/harness/rom/Animal Crossing.iso`
  (GAFE01 USA Rev 0, 1,459,978,240 bytes) — reference it, never copy it in.
- **`pc/` is reference material, not the build target.** The Dreamcast platform
  layer is `dc/`. Do not "fix" `pc/` for Dreamcast. Host-side tools go in
  `tools/`.
- **Branches:** `main` = releases, `dev` = daily work. Never tag dev. Agents must
  not run git — the main thread commits.
- **Every optimization gets a kill switch** (compile-time or settings flag), and
  **the default is the good build**: a result that lives only in a command line
  is one unset environment variable away from being lost.
- ⭐ **A CHANGE WHOSE MECHANISM IS CACHE IS JUDGED ON HARDWARE, AND A FLYCAST
  "NO CHANGE" IS NOT EVIDENCE AGAINST IT** (2026-08-09, batch S14 — the standard
  from here on). Flycast models **no instruction cache and no operand cache**, so
  a locality change is invisible there *by construction*. S14 scored a **wash**
  in the emulator (`us/v` 2.51 → 2.48, inside the ±2 % noise floor) and came back
  from a burned CD-R as *"definitely runs better on real hardware"* with the
  audio stutter gone. **The emulator can falsify an instruction-count claim; it
  can never falsify a locality claim.** Full rule: `kb/RESUME.md` §3 rule 12;
  evidence: `kb/batch-s14.md` §7.
- **The MMU stays off.** `dc/src/` depends on QACR for the store-queue emit path,
  and any MMU use taxes the store queues on SH7750 silicon.

### Hardware contract

8 MB VRAM, textures twiddled 16-bit / VQ / paletted — never linear RGBA8. No
shaders, no hardware T&L, one texture unit; vertex transform and lighting run on
SH-4. 2 MB AICA sound RAM, 64 ADPCM channels, weak ARM7 — do not plan CPU work
on the ARM7. VMU ≈ 100 KB user data vs a ~456 KB GC save. CD-R streams at
~500 KB/s, so all disc I/O needs read-ahead.

---

## 2. Start here

| order | file | why |
|---|---|---|
| 0 | **`kb/RESUME.md`** | **fresh context: start here.** Where the port is, the build lines, the **twelve measurement rules**, the instruments, what is still broken. §6b/§6c are the hardware profiler and what it measured |
| 0.5 | **`kb/next-session.md`** | **the paste-me seed prompt after a context flush**, plus the handful of facts that live only in a human's head (which SD interface, how the card must be partitioned, which disc is safe to burn) and where the profile artefacts are |
| 1 | **`kb/STATE.md`** | the current numbers, the fit inequality, the ranked queue. Short by design |
| 2 | `kb/closed.md` | **read before proposing any RAM / size / architecture idea** — what is already dead, and why |
| 3 | `kb/traps.md` | read before touching the build, harness, prelude, or instrumentation |
| 4 | `PLAN.md` | milestones, the four hard problems, the risk register |
| — | `kb/state-log.md` | only when you need the evidence behind a number. Newest first |

---

## 3. Document map

### Live state — changes as work lands

| file | contents |
|---|---|
| `kb/RESUME.md` | the handoff: current state, build lines, measurement rules, instruments, subsystem status, what is still broken |
| `kb/STATE.md` | headline numbers, the fit inequality, the ranked next actions |
| `kb/state-log.md` | the evidence: what was observed running, when, and what it cost. Newest first |
| `kb/closed.md` | settled questions — do not re-propose. Includes the `-O0` post-mortem |
| `kb/traps.md` | mechanical gotchas already paid for |
| `kb/levers.md` | the RAM ledger: applied cuts and every lever still live. ⚠️ RAM is **not** the binding constraint any more; **L10 (T1, textures)** is the live one |
| `kb/boot-blockers.md` | what the running game hits next, ranked by reach rather than by bytes |
| `kb/heap-two-pools.md` | the arena-vs-sbrk rule. **Read before touching `DC_ARENA_BYTES` / `DC_ARAM_WINDOW`** |
| `kb/plan-stages.md` | the agreed S1→S5 RAM plan and the reasoning behind each step |
| `kb/issues.md` | known game-side bugs and leads (armhf-era, still accurate on game facts) |
| `kb/station-bugs.md` | the two train-station bugs: black floor (solved), roof clip-through (reproducible, never run) |
| `PLAN.md` | milestones, the four hard problems, risks, open questions |

### Performance — the live work

| file | contents |
|---|---|
| `kb/research-sh4zam-gap.md` | the ranked renderer gaps G-A…G-J, re-ranked around the memory-bound reading. ⚠️ **"G-B" is two things** — the shipped vertex-index side channel and the unstarted indexed-submit rewrite (the 13.31 ms block). Do not read one as the other |
| `kb/perf-dc.md` | where the town frame goes: the method, the applied optimizations with their kill switches, and what is ruled out. ⚠️ Its absolute numbers are historical — live ones are in `kb/STATE.md` |
| `kb/batch-s14.md` | **the S14 batch and its ROLLBACK CONTRACT** — seven hardware-shaped changes landed in one pass, each with its kill switch, its gate counter, and the one-line full revert. Read it before bisecting a visual fault in the renderer. ⭐ **§7 is the first hardware win this project banked that Flycast could not see**; §5 carries four kb figures this batch falsified, and §2b an eighth change its own gate proved a no-op |
| `kb/batch-s15.md` | **the S15 batch and its ROLLBACK CONTRACT** — the lean cull refresh (S15-1), the narrowed TEV P3 predicate (S15-2/2b), the villager seeder (S15-3), `DC_OPT_OS_EXTRA` (S15-4) and the CD-R short-read fix (S15-5). ⭐ **§S15-5 is a hardware-only bug Flycast is structurally unable to execute**; §S15-3 and §S15-2 each falsify a diagnosis this kb published |
| `kb/hardware-profiling.md` | **the answer to §6, and it RUNS as of 2026-08-12** — KOS `libgprof` on real silicon: the `-pg`-on-the-link-line-only trick (unbiased flat profile, byte-identical optimized code), the RAM a profiling build must reclaim, and the two sinks. ⭐ **Validated end to end in Flycast: 31,010 samples, symbolised.** ⚠️ It is **two images** — `DC_GPROF_SD=0` for the emulator, `=1` for hardware — because probing for an absent card WEDGES. Idle is 94.76 %, so compare non-idle shares only. `kb/RESUME.md` §6b |
| `tools/dcprof/` | **P2's host side**: `decode_gmon.py` recovers a gmon.out from a base64 console dump; the README carries the build lines, the knob table and the SD-card procedure. ⚠️ It does NOT read a raw `gmon.out` off the card — that goes straight to `sh-elf-gprof` |
| `dc/src/dc_profdump.c` | **P2's target side** — the write-only `/prof` VFS, the z0+base64 console encoder (612,433 B → 1,585 B), the SD sink, and the L+R+START dump chord. 🔴 Its header block carries the `O_APPEND`-vs-`O_MODE_MASK` trap that makes `fopen(…,"a")` unusable on `fs_fat` |
| `kb/research-fps-ideas.md` | unbanked FPS concepts, each with a failure mode and a cheapest experiment |
| `kb/research-ram-tiers.md` | second-tier memory concepts (VRAM/AICA as eviction tiers, cold `.text` relocation, `--wrap=malloc`) |
| `dc/src/dc_emu64_cull.cpp` | **G3** — the AABB cull at `G_TRIN_INDEPEND` entry, ON by default, and the source half of the vertex-index side channel. `kb/RESUME.md` §5 |
| `dc/src/dc_pvr.c`, `dc/src/dc_gx.c` | the PowerVR backend and the GX state machine. The vertex-index side channel, the `pvr_dr_*` emit path and the `[VTXSPLIT]` instrument all live here |
| `dc/include/dc_gx_internal.h` | `DCGXVertex` (`aligned(32)`, `vtxid` in bytes 30-31) and the light-array layout |
| `dc/src/dc_pmcr.c` | **P1** — SH7750 performance counters, `DC_PMCR=1`. ⚠️ **Burn-only: Flycast reports zero for every event** |
| `dc/src/dc_emu64_hist.c` | **G1** — the per-opcode emu64 timing histogram. ⚠️ **Per LOGIC TICK — double it** |
| `tools/dcopt/icache_map.py` | host-side icache pressure: hot-symbol bytes vs the 8 KB direct-mapped cache, and which hot functions share a line. `--emit-order` writes **F5**'s `dc/section-order.txt`; its docstring is the verified reference for the `--section-ordering-file` format |

### Build & test

| file | contents |
|---|---|
| `BUILDING-DC.md` | **the DC build**: entry points, make targets, env knobs, flag assembly, include-path order, prelude, troubleshooting |
| `dc/opt-lists.mk` | the `-O3` hot list and the `-O0` quarantine list, each entry with the measurement or symptom that earned it |
| `dc/section-order.txt` | **F5** — the generated GNU ld `--section-ordering-file` that packs the innermost draw loop into contiguous i-cache lines. Kill switch `DC_SECTION_ORDER=0`; regenerate with `tools/dcopt/icache_map.py --emit-order`. ⚠️ **Unmeasurable in Flycast** (no i-cache model) — `dc_pmcr.c`'s `istall` on a burn is the only instrument |
| `kb/toolchain.md` | why the toolchain is built from source, and the host facts that cost something. `dc/Dockerfile` is the recipe |
| `tools/dcopt/` | `warnscan_report.py` (UB classes an optimizer can act on); `bisect_o0.sh` + `predicate_town.sh` (binary-search a miscompiling TU) |
| `harness/dc/README.md` | the Flycast harness: setup, scripts, guest-side protocol, env overrides, known limits |
| `tools/dcqa/run_report.py` | **the regression gate.** Reduces a `console.log` to the ~20 numbers a "did this get worse" call rests on; `--vs` diffs two runs. ⚠️ It is the FLOOR — it cannot see colour |
| `BUILDING.md` | upstream base-repo build (the PC reference port), not this target |

### Memory & assets

| file | contents |
|---|---|
| `kb/levers.md` | start here for any size work |
| `kb/mem-budget-m1-sh4.md` | the real sh-elf link: section sizes, the `.bss` split, levers applied, dead ends, the boot-size gate |
| `kb/asset-pack.md` | the `assets.pak` contract between `tools/dcasset` and the `dc/` runtime |
| `tools/dcasset/README.md` | the extraction/pack tool |
| `tools/dcstub/make_stub_data.py` | the `DC_ASSET_STUB` rewriter (S1). Header comment is the doc |
| `tools/dcstub/make_src_shrink.py` | the `DC_SRC_SHRINK` scratch-tree rewriter (S3, on by default) |
| `dc/src/dc_npcdiag.c` | **N3 — why no villager ACTOR is ever constructed.** `DC_NPCDIAG=1` wraps all nine serial gates between `npclist` and a live actor in `dc_npcdiag_gate()` (returns its argument untouched, so every `&&` keeps its short-circuit) and prints one cumulative `[DC/NPCDIAG]` line. **Its header comment carries the decision table — one town run is decisive.** Default 0 = byte-identical |
| `tools/dcstub/census_resolve.py` | resolves a `DC_ASSET_CENSUS=1` log into a scene's real working set |
| `tools/dcstub/census_keeplist.py` | joins a resolved census to the linked map and emits `DC_STUB_KEEP` |
| `dc/src/dc_texpool.c` | **T1 — every display-list texture is read off the disc, not kept in `.bss`.** The row index IS the PVR's cache key, so the array is never touched on a bind and never has to be resident. `DC_TEXPOOL_DEMAND=0` reverts; `DC_TEXPOOL_PROBE=1` is the separate falsification instrument |
| `dc/src/dc_assetwin.c` | **the one read-ahead window every demand loader shares** (T1, R1, R2/R3, the mid-scene keep path). ⚠️ Judge it on `reads=` in the `[DC/AWIN]` line, never on wall clock — Flycast models no seek. `DC_ASSETWIN_B=0` reverts |
| `tools/dcstub/keeplist-town.txt` | the pre-T1 keep list. ⚠️ Enumerated from the tree, NOT censused — the town reseeds every boot, so no census can be correct (`kb/RESUME.md` §8) |
| `tools/dcstub/keeplist-full.txt` | **the keep list to BUILD WITH.** keeplist-town plus as much of `src/data/model/` as T1's freed bytes pay for, priority families first. Regenerate with `make_keeplist_town.py --full-model`; ⚠️ **it is budgeted and the budget bites** — what it dropped still renders as nothing, and the generator names the casualties on stderr |
| `tools/dcstub/keeplist-opening.txt` | the censused keep list for the opening scene — still right for title-screen and size work |
| `tools/dcfb/fbimg_to_png.py` | decodes a `DC_FB_IMAGE` log into PNG screenshots |
| `dc/stage-disc.sh` | flatten a `dcasset extract` tree into a disc root for `DC_DISC_ROOT` |
| `kb/research-mmu-paging.md` | MMU demand paging — **DEAD**, with the four preconditions to reopen |

### Graphics, audio & save

| file | contents |
|---|---|
| `kb/tev-map.md` | index to all 101 TEV configs → fixed-function PVR strategies. Parts: `kb/tev-map-table.md` (**the reference table — load this to implement**), `kb/tev-map-implementation.md`, `kb/tev-map-alpha.md`, `kb/tev-map-hard-cases.md`, `kb/tev-map-decoding.md` |
| `kb/texture-path.md` | what happens to a GC texture on its way to VRAM. Read before proposing texture-quality work |
| `kb/audio-plan.md` | jaudio_NES on DC — verdict + index |
| `kb/audio-engine.md` | what the engine is (N64 libaudio, *not* JAudio), shipped parameters, `audiorom.img` |
| `kb/audio-cpu-cost.md` | per-voice op counts, the SH-4 cycle model, A0–A4 configs |
| `kb/audio-stage-a-software.md` | stage A: rspsim at 22 kHz, effect-cut order, `snd_stream` |
| `kb/audio-stage-b-aica.md` | stage B: the exact seam, AICA register map, residency policy, offline tools |
| `kb/audio-plan-of-record.md` | the plan of record and what is still unmeasured |
| `kb/audio-aica-offload.md` | ⭐ **stage B, MEASURED (2026-08-13) — read this before any AICA work.** Confirms the kb's ×8/9 sizing, then changes three things: **two of the four blockers are ONE** (bank 153 overflows *because* 19 of its samples exceed the 65,534-sample channel limit), the **8-bit-PCM mitigation is FALSIFIED** (4.9× usable, not "still fits"), and the loop-click risk is **0 or ~163 samples** depending on one unmeasured hardware fact. 🔴 §8 is the runtime design and it says **do NOT drive voices through KOS's ARM7 queue** — no overflow check, ~430 Hz service |
| `kb/audio-cheap-cpu-wins.md` | ⭐ **two audio CPU wins that do NOT need the AICA offload.** W1: switch the bank to `CODEC_S8` — a complete first-class path in the shipped engine, selected by a bitfield in the ROM **data**, so it costs ZERO runtime code and ~5.5 % of busy; 246/249 sequences still fit the existing `DC_ARAM_WINDOW`. W2: `MAC.W` on the ADPCM filter, whose "unproven bound" is now **proven over all 748,255 frames** (both operands fit s16, accumulator has 29× headroom, one bit of margin). ⚠️ W1 and the offload are the same 18.9 % — not additive |
| `tools/dcaudio/` | the host side: a VADPCM decoder **bit-exact against `rspsim.c` and tested that way**, the `audiorom.img` reader, the AICA-ADPCM codec, and `census.py` (the residency report + the manifest). ⚠️ Needs `audioheaders.c` as well as the image — `audiorom.img` carries **no index at all** |
| `kb/save-budget.md` | 295,910 B of save into a 100 KB VMU — verdict + index. ⚠️ compression numbers are SYNTHETIC |
| `kb/save-layout.md` | what the save actually is: verified struct sizes, `.gci` layout |
| `kb/save-compression.md` | codec comparison and the VMU fit verdict |
| `kb/save-plan.md` | the recommended VMU plan, flash write cost, open items |
| `kb/vmu-lcd.md` | the VMU's own 48×32 LCD |
| `tools/savebench/README.md` | save-size measurement harness |

### Platform API — the symbol map from `pc/src`

`kb/design-platform-api.md` is the index; **`kb/platform-api-overview.md` first**
(symbol counts, dispositions, ranked landmines, the table legend). Parts:
`kb/platform-api-boot-order.md` (the six hard ordering rules),
`kb/platform-api-os-core.md`, `kb/platform-api-os-stubs.md`,
`kb/platform-api-math.md`, `kb/platform-api-gx.md`,
`kb/platform-api-vi-pad.md`, `kb/platform-api-dvd-aram.md`,
`kb/platform-api-audio.md`, `kb/platform-api-save-card.md`,
`kb/platform-api-pc-only.md`, `kb/platform-api-globals.md`.

### Background & reference

| file | contents |
|---|---|
| `kb/base-repo-map.md` | what transfers from the Anbernic base, what dies |
| `kb/research-dreamcast.md` | KOS/PVR/AICA facts, precedent ports |
| `kb/research-ecosystem.md` | decomp ecosystem, upstream state, legal model |
| `kb/upstream-pc-port.md` | what upstream ACGC-PC-Port releases do and do NOT give us. **Their perf work does not transfer** |
| `kb/game.md` | game-side knowledge: decomp, emu64, data |
| `kb/history.md` | project history & decisions |
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
Docker runs under colima on an Apple M4 (arm64, 10 cores, 24 GB) and this host
has **no BuildKit**: use `DOCKER_BUILDKIT=0` and never pass `--progress`. Inside
the SDK image use `bash -c`, never `bash -lc`. Pass `-N` to mkdcdisc for
emulator runs, `DC_CDI_PAD=1` for burns. Flycast is the iteration emulator; the
real Dreamcast on a burned CD-R is the truth. Always give absolute paths in
scripts — agents run from varying cwds. Rationale for each: `kb/traps.md`.

---

## 5. Keeping this current

`PLAN.md`, `kb/STATE.md`, `kb/state-log.md`, `kb/levers.md`, `kb/closed.md` and
`kb/traps.md` are the project memory. Update them as facts change.

**When you settle something, move it:** a dead idea to `kb/closed.md`, a
debugging gotcha to `kb/traps.md`, a live saving to `kb/levers.md`, and the
narrative of what was observed to `kb/state-log.md`. **`STATE.md` and
`RESUME.md` stay short** — if a section there is growing a history, that history
belongs in the log. If you add a document, add a row to §3; if you delete one,
delete its row.

⚠️ **`file.c:NNN` CITATIONS DRIFT, AND THEY DRIFT SILENTLY.** The audit found
references in `dc/src/dc_pvr.c` wrong by 450-500 lines — pointing at unrelated
comments while still reading as precise. **Verify a line number before acting on
it, and prefer citing the SYMBOL** (`tev_const_color()`, `emit_projected()`),
which survives an edit. A wrong line number is worse than none: it looks
checked.

⚠️ **Treat an unsourced kb number as a claim until verified.** These docs were
partly written by agents whose adversarial verifiers died; several figures have
already been falsified. The audit of 2026-08-09 deleted 47 files that were dead,
void, armhf-era or superseded — recover any of them from git history rather than
re-deriving.
