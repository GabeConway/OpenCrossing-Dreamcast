# Session state — resume here

Last updated 2026-08-01, end of the first execution session. Written so a
fresh context can pick up without replaying anything.

## Where the project is

Planning is done and the first execution fan-out has partially landed. Nothing
builds for Dreamcast yet. Read in this order: `CLAUDE.md`, `PLAN.md`, then the
kb docs listed below.

Repo history: `main` only. `e0eabd5` initial plan → `4e73867` merged the whole
`GabeConway/OpenCrossing-Anbernic` tree in (remote `anbernic`, so upstream
`flyngmt` fixes stay cherry-pickable) → `ae36e58` Dreamcast-specific CLAUDE.md.
Everything from the fan-out below is **uncommitted working tree** unless a
later commit says otherwise.

## What the fan-out produced

A 17-agent workflow (Opus 5 / high) ran. **9 agents completed, 9 died on a
session token limit** — not on errors in the work. Split:

**Completed** — 4 recon designs, 4 measurements, 1 build:
- `kb/design-toolchain.md` (777 lines) — KOS/dc-chain toolchain approach
- `kb/design-harness.md` (605) — Flycast automation design
- `kb/design-platform-api.md` (1517) — the symbol manifest `dc/` must satisfy
- `kb/design-shelf-hazards.md` (747) — sh-elf exclusions, flags, alignment risk
- `kb/mem-budget.md` (601) — the 16 MB ledger
- `kb/tev-map.md` (732) — TEV configs → PowerVR strategies
- `kb/save-budget.md` (375) — save size vs VMU
- `kb/audio-plan.md` (599) — rspsim cost, AICA plan
- `tools/dcasset/` — the ISO extractor, **fully working and verified**
- `tools/savebench/` — save compression benchmark

**Died before finishing** (session limit, mid-write in some cases):
- `build:toolchain` — `dc/docker/`, `dc/build-dc-docker.sh` were never created
- `build:harness` — only `harness/dc/_runner.py` and `harness/dc/selftest/`
  landed; `install-flycast.sh`, `smoke.sh`, `run-flycast.sh`, `screenshot.sh`,
  `console.sh`, `README.md` are all missing
- `build:dc-platform` — `dc/src/` and `dc/include/` are **partial**: present
  are dc_os/dc_gx/dc_vi/dc_pad/dc_audio/dc_aram/dc_dvd/dc_card/dc_misc/
  dc_stubs/dc_mem_ledger .c and dc_platform/dc_gx_internal/dc_mem_ledger/
  dc_mem_budget .h; **missing dc_main.c and dc_mtx.c**. Note the odd files
  `dc/include/SDL.h` and `dc/include/pc_platform.h` — probably include shims,
  unreviewed, verify before trusting.
- `build:buildsystem` — no `dc/CMakeLists.txt`, no `dc/Makefile`, no
  `dc/cmake/`, no `BUILDING-DC.md`. Nothing can compile until this exists.
- **All 5 adversarial verifiers died.** Every deliverable above is therefore
  **unreviewed**. Treat the kb numbers as claims, not facts, until verified.

## The one verified headline

`tools/dcasset` measured the real ISO. The lead's padding intuition was right:

- Image 1,459,978,240 B; **real content 27,573,513 B (1.89%)**; the other
  98.11% is trailing zeros, confirmed by scanning every byte, not sampled.
- Payload deflate-9 = 16,743,158 B, which matches the remembered "~16 MB".
- Recommended DC disc layout (foresta.rel Yaz0-expanded) = **35.24 MB, 5.3%
  of a CD-R.** Disc capacity is a non-issue — spend it freely on offline
  conversions that trade disc space for SH-4 cycles.
- Biggest files: `audiorom.img` 8.30 MB (31% of content), `foresta.rel.szs`
  6.14 MB → 15.64 MB expanded, `foresta.map` 4.85 MB, `forest_2nd.arc`
  4.13 MB, `famicom.arc` 1.70 MB (droppable — NES is a non-goal).

**New finding that changes PLAN §3.1:** `pc_assets.c` keeps the entire
decompressed REL (15.64 MB) plus main.dol (0.92 MB) resident — **16.56 MB,
larger than the whole Dreamcast**. `dcasset relmap` merged every
`pc_load_asset` reference and found the actually-referenced bytes total only
**8.66 MB across 2,496 spans** (plus 122 KB across 13 spans in main.dol), with
all 2,641 call sites reconciled and zero unaccounted. Emitting those spans as
an offset-indexed pack read from `/cd` removes the resident blob entirely.
This is the single biggest RAM lever found so far. Caveat: only
`pc_load_asset` sites were parsed, so 8.66 MB is exact-for-the-loader and a
lower bound overall. Unaudited: whether the runtime reads `foresta.map` /
`static.map` (5.40 MB, 20% of content) — likely only `dvderr.c`
symbolication. Check before M2.

## Resume plan

1. **Read the kb docs before spawning anything.** Four measurement docs and
   four design docs already exist; do not re-derive them.
2. **Re-run the dead agents.** The workflow script is at
   `~/.claude/projects/-Users-gabe-Documents-GitHub-OpenCrossing-Anbernic/4315b266-f213-4fa0-b4ce-046b58a18ee9/workflows/scripts/dc-m0-m1-fanout-wf_98a081cc-e2d.js`
   and resuming with `Workflow({scriptPath, resumeFromRunId: 'wf_98a081cc-e2d'})`
   replays the 9 completed agents from cache and re-runs only the failed ones.
   That is much cheaper than starting over. Do this in a fresh session with
   budget, since 9 agents at Opus/high is what exhausted the last one.
3. Order that matters: **buildsystem and toolchain first** — nothing is
   testable until sh-elf compiles something. Then finish `dc_main.c`/`dc_mtx.c`,
   then the harness scripts, then run the verifiers over everything.
4. Commit the working tree before doing anything else.

## Standing constraints

Stock 16 MB DC. No shaders, no T&L, one texture unit. VMU ≈ 100 KB. CD-R
~500 KB/s. Never commit ROM material or built disc images. Agents must not run
git — the main thread commits. Dev console is a known-good MIL-CD unit that
boots burned CD-Rs. Emulator-first iteration (Flycast), hardware for truth.
