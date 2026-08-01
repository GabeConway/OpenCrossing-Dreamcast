# OpenCrossing-Dreamcast — Port Plan

Written 2026-08-01, from three research passes recorded in `kb/`:
`kb/base-repo-map.md` (what the Anbernic base actually contains),
`kb/research-dreamcast.md` (DC homebrew platform state), and
`kb/research-ecosystem.md` (the AC decomp/port ecosystem). Decisions already
made: base = OpenCrossing-Anbernic; dev on Flycast first, real Dreamcast +
burned CD-R later; vertical slice before full game; output = self-booting CDI
built from the user's own ISO.

## 1. Goal

Boot to a playable Animal Crossing town on a stock (16 MB) Dreamcast at a
stable 30 fps, saving to VMU, with music — built entirely from the user's own
GAFE01 disc image. Full-game parity is the long-term goal; a trimmed "DC
edition" is the acceptable fallback (documented cuts, not silent breakage).

## 2. Hardware reality check

| | GameCube (native) | RG-34XX SP (base port) | Dreamcast (target) |
|---|---|---|---|
| CPU | Gekko PPC 485 MHz | 4× Cortex-A53 1.5 GHz (game uses 1 core) | SH-4 200 MHz, 1 core |
| Main RAM | 24 MB + 16 MB ARAM | 1 GB | **16 MB** |
| GPU | Flipper (TEV, T&L) | Mali-G31 (GLES 3.1, shaders) | PVR CLX2: fixed-function, tile-based, **no shaders, no T&L, 1 texture unit** |
| VRAM | 3 MB embedded | shared | 8 MB |
| Audio | DSP + 16 MB ARAM | CPU (rspsim software DSP) | AICA: 64 HW channels, ADPCM, 2 MB sound RAM, weak ARM7 |
| Storage | 1.46 GB disc | SD card | CD-R 700 MB / GD 1 GB, ~0.5–1.8 MB/s |
| Save | Memory card (~57 blocks ≈ 456 KB) | filesystem | **VMU: ~100 KB user data** |
| Endian/width | BE / 32-bit | LE / 32-bit | LE / 32-bit ✅ |

The measured baseline that anchors all estimates: the base port holds ~56 fps
average on one A53 core at 1.5 GHz **with game code compiled at -O0**, and its
bottleneck is CPU (game logic), not GPU (GL time is only 5.4 ms/frame after
the batching/culling passes).

## 3. The four hard problems

Everything else is porting labor; these four decide viability. Ordered by risk.

### 3.1 RAM: ~45 MB working set into 16 MB

Current base-port steady state: 24 MB main arena (SystemHeapSize 22.8 MB) +
16 MB emulated ARAM (8.44 MB sound + 6.96 MB graph) + 3.1 MB vertex buffer +
caches + 10.9 MB binary. Plan of attack:

- **ARAM dies as a resident buffer.** Sound half (audiorom.img, 8.3 MB):
  moves to AICA-side ADPCM + disc streaming (see 3.4); never resident in main
  RAM. Graph half (RARC archives, ~7 MB): becomes a disc-backed segment with
  an LRU window — JKRAramArchive already funnels reads through one seam
  (`ARStartDMA` is a memcpy today; it becomes a cached disc read).
- **Shrink the main arena.** 24 MB mirrors the GC, not actual need — the N64
  original ran this game concept in 4 MB. Audit real JKRHeap high-water marks
  with heap instrumentation on the PC build (cheap to do there), then set the
  DC arena to measured-peak + margin. Target: ≤ 9 MB.
- **Shrink the binary.** The armhf ELF is 10.9 MB largely because `src/data/`
  (464k LOC of generated tables) compiles in. Extend `gen_runtime_assets.py`
  to evict the biggest tables to disc-loaded assets. **Not `-Os`** — see the
  directive at the head of §3.2. The size levers are all layout-class:
  `-ffunction-sections -fdata-sections` + `-Wl,--gc-sections`, `.bss`
  right-sizing, dropping non-goal subsystems (NES), and moving rodata to
  `/cd`. Target: ≤ 4 MB text+data resident.
- **Vertex buffer:** 3.1 MB static (65536 × 48 B) shrinks — DC submits in
  32-byte `pvr_vertex16_t`-style records per-list; batch buffer can be ¼ the
  size with 16-bit quantized formats (dca3's meshlet trick, decoded by FTRV).
- **Texture memory moves to VRAM.** Base port keeps linear RGBA8 copies in
  main RAM + GL. On DC, decoded textures live only in VRAM as twiddled
  16-bit/VQ; the 8 MB VRAM is the texture cache.

Budget sketch (must be enforced with a boot-time ledger):
binary 4 + arena 9 + streaming/IO buffers 1.5 + KOS ~1.5 = 16 MB. Tight but
in dca3 territory. **Fallback:** the 32 MB RAM mod exists and KOS supports
it, but "stock 16 MB" is the design constraint; the mod must never become a
requirement (dca3 holds this line, we hold it too).

**Measured 2026-08-01 — the sketch above is aspiration, these are facts:**

| | bytes | note |
|---|---:|---|
| linked ELF `.text` | 6,318,568 | at `-O0`, all 3917 TUs, zero exclusions |
| linked ELF `.data` | 2,638,852 | |
| linked ELF `.bss` | 13,526,548 | classification in flight |
| **total** | **22,483,968** | ends `0x8d581c14`; `_arch_mem_top` = `0x8d000000` |

It links; it will not boot.

**The cut required is ~14.45 MB.** An earlier pass said 6.5 MB; that figure
only gets the image under `_arch_mem_top` with ~1.2 MB of heap left, which is
less than the game's first archive mount. Against the ledger's own 7.61 MB
heap (`dc/include/dc_mem_budget.h` buckets 6–12) plus KOS's ~1 MB, **the image
budget is 8,035,072 B.** Measure proposals against that, not against 16 MB.

The reason `.bss` is not free: **KOS's `mm_sbrk()` starts at the ELF `end`
symbol** — no MMU, no lazy commit, so every `.bss` byte destroys a heap byte.
This single fact is what makes the problem structural rather than cosmetic,
and it is why compression and debug-stripping are worth exactly zero here
(`.bss` is `NOBITS`).

`-O0` is mandatory throughout. `kb/research-size-reduction.md` ranks the
codegen-neutral techniques and totals **−14.77 MB against −14.45 MB required:
it closes on paper with a 2% margin**, which is "closes on paper", not "safe".
The dominant term is demand-loading `src/data` (−8.45 MB, 57% of the cut);
nothing else is within a factor of five. Levers measured rather than guessed:

- **Resident REL blob, −15.68 MB (solved, tool built).** `pc_assets.c` kept
  the decompressed `foresta.rel` + `main.dol` resident = 16,558,776 B.
  `dcasset pack` replaces it with an 8,917,568 B `assets.pak` on `/cd` plus a
  51,104 B resident index, pre-byte-swapped and laid out in load order so an
  8 KB window yields zero seeks. See `kb/asset-pack.md`.
- **Asset destination arrays, +8.22 MB (the new floor).** Those assets land in
  15,726 static arrays totalling ~8,617,214 B that stay resident regardless of
  source. This is now the largest single line in the budget. The pack format
  makes every asset individually addressable, so demand-loading into pooled
  storage is a loader-only change — no codegen, permitted under §3.2.
- **`foresta.map`/`static.map`, −5,402,023 B of disc (settled, droppable).**
  Only reader is `JUTException::queryMapAddress_single` on the
  `OSSetErrorHandler` path, which returns false outside
  `0x80000000..0x82FFFFFF` — no SH-4 address qualifies.
- **`DC_MAIN_MEMORY_SIZE` (4,000,000) is heap-allocated** at `dc_os.c:400` —
  4 MB *on top of* the image, not inside it. Counted in the heap side of the
  budget above, not the 22.5 MB.

**Dead ends, verified — do not re-propose** (details in
`kb/research-size-reduction.md`): `--gc-sections` is already applied and
already spent (29,471 discarded sections recover 522,150 B of allocatable RAM
and that is all there is — GCC does not emit unreferenced `static`s even at
`-O0`); `--icf` has no SH implementation in gold, `ld.bfd` or `lld`; SH GCC has
no small-data model, so the KOS script's `.sdata`/`.sbss` are inert; `-mrelax`
is a codegen change and is disqualified; stripping debug info and compressing
`1ST_READ.BIN` each save exactly 0 RAM; AICA's 2 MB is DMA-only over a 16-bit
25 MHz G2 bus and cannot hold a C array — budget 0 MB of it.

**Shelved, not rejected:** code overlays are real and shipping on DC (ScummVM's
`backends/platform/dc/dcloader.cpp` + `plugin.x`, an SH-4 `R_SH_DIR32` ELF
loader in production since 0.7.0) — shelved because they buy `.text`, and
`.text` is only 5.26 MB of the problem. VRAM as a store is legitimate and KOS's
linker script already shows the mechanism (`.ocram 0x7c001000 (NOLOAD)`), but
~half of VRAM is unavailable during PVR rendering and there is **no measured
SH-4↔VRAM read figure** — measure before relying on it.

**Measure these three before trusting the 2% margin:** KOS+GLdc baseline RAM,
`pvr_mem_available()` after a real texture load, and `__osMalloc` peak. Ledger
bucket 6 (4.0 MB `JKRHeap`/`__osMalloc`) is entirely unmeasured; if it is
really 6 MB, the plan is 2 MB short.

### 3.2 CPU: 200 MHz vs -O0 decomp code

> **Revised 2026-08-01 by user directive.** This section previously planned to
> adopt upstream's `-O2`. **Optimization is off the table**: "the optimizations
> cause problems and we cant use them without the port being broken." Game
> code (`src/`) builds at **`-O0`**, and that is a fixed input to every other
> plan in this document, RAM included.

- **`-O0` is the contract.** The armhf record is why: `-O2` produced a
  wild-pointer crash loop from boot; `-O1` produced a hard SIGBUS on the intro
  train scene. x86 tolerates this code because it is byte-addressable and
  forgiving; ARM did not, and SH-4 is a strict-alignment ISA like ARM. Upstream
  flyngmt commit `4f428276` (2026-07-10, "Compile everything at -O2") with
  `-fno-strict-aliasing -fwrapv` is an **x86-only** data point and does not
  transfer. Do not re-propose `-O1`/`-O2`/`-Os`/LTO as a size or speed lever.
- **Codegen vs layout.** The ban is on anything that changes instruction
  selection. It is *not* a ban on `-ffunction-sections -fdata-sections` +
  `-Wl,--gc-sections`, `.bss` right-sizing, linker script placement, or moving
  data to disc — those are layout, and they are the RAM plan (§3.1).
- **Speed therefore has to come from elsewhere:** SH-4 FTRV/XMTRX in the
  matrix layer (`dc/src/dc_mtx.c`, done), store queues for PVR submission,
  fewer draw calls, a 30 fps target, and hand-optimizing individual hot
  functions at the source level — which is portable, upstreamable, and
  reviewable in a way `-O2` is not. If a specific TU is measured hot and
  provably alignment-clean, a per-TU exception can be argued on evidence; the
  `dc/Makefile` supports per-TU flags. It is not the plan.
- **Alignment is still the residual class** even at `-O0`. The base repo's
  ARM record: -O1 caused
  a hard SIGBUS (unaligned LDRD/VFP) on the intro train scene; x86 never sees
  this. SH-4 is also a strict-alignment ISA, so expect the same class.
  Mitigation: SH-4 raises a CPU exception on unaligned access — install an
  exception handler that logs PC + faulting address (the base port's SIGBUS
  recovery pattern), triage per-TU exactly as the Anbernic repo did, fix the
  misaligned casts at the source where found (upstreamable fixes).
- **Budget arithmetic — now worse, state it plainly.** Single-thread
  A53@1.5 GHz ≈ 10–20× SH-4@200 MHz scalar. The old arithmetic leaned on
  `-O0`→`-O2` for ≥2–3× of that; **that term is gone.** What remains is the
  30 fps target (2× the frame budget of 60), FTRV/XMTRX in the matrix layer,
  and source-level work on whatever the profiler names. This does not
  provably close the gap, and pretending otherwise would make the M3 gate
  meaningless. **Go/no-go gate at M3:** measured game
  logic time for a town frame must be ≤ 25 ms on hardware/Flycast-with-
  realistic-timing before renderer perf work continues.
- **30 fps is the design target,** not degraded-60. Upstream's 2026-08-01
  wave of delta-time commits ("Fix fishing dt", NPC/event/animation timing —
  the unlocked-FPS work with Cuyler36) is exactly the frame-rate-independence
  layer a 30 fps port needs; track and cherry-pick it.
- New CPU work DC adds: all vertex transform + lighting (no T&L hardware),
  texture twiddling on decode, audio (see 3.4). Counted in the budget gate.

### 3.3 Renderer: GX → PowerVR, no shaders

Layering stays `game → emu64 → GX → platform`, same as every successful port
of this codebase. emu64 is kept (it's decompiled, -O2-safe, and its quirks
are already handled by the base port's GX layer). The GX implementation is
rewritten in two stages:

- **Stage A (bring-up): GLdc.** Kazade/Simulant's GL 1.2-on-PVR. The pc_gx
  state machine, batch merging, strip→triangle conversion, and whole-batch
  CPU frustum cull all carry over (pure C, and precisely what a tile-based
  GPU wants). Draw dispatch is rewritten from VBO/`glDrawElementsBaseVertex`
  to vertex arrays; TEV collapses to modulate/decal + vertex color. Wrong
  colors in places — acceptable; sm64-dc shipped full-speed on GLdc master.
- **Stage B (perf/correctness): direct KOS PVR API,** dca3-style. Explicit
  OP/PT/TR list submission via store queues, 32-byte vertex records, FTRV
  T&L loops (~32-vertex blocks), OCR scratchpad. This is where 30 fps
  headroom comes from — dca3 proves the ceiling.
- **TEV approximation strategy.** The base port harvested exactly **101 TEV
  configurations** for its shader seed — a complete, finite spec of what AC
  actually asks for (max 3 stages). Classify all 101: (1) single-stage
  modulate/decal/blend → native PVR poly modes; (2) two-stage with constant
  second stage → per-vertex offset color (PVR's built-in specular add) or
  baked into vertex colors; (3) genuinely multi-textured/multi-stage →
  second pass into the TR list (GLdc's multitexture does this already);
  (4) indirect-texture effects (if present in the 101) → precomputed
  animated textures or dropped, per-case. Deliverable: a `tev_map.md` table
  of all 101 configs → PVR strategy, written during M2.
- **Lighting:** GC per-vertex lighting (8 lights, angular+distance
  attenuation) currently runs in the vertex shader. On DC it runs on SH-4 —
  but AC scenes are mostly ambient + few dynamic lights; bake what's static,
  FIPR the rest, clamp the light count per measured usage.
- **Translucency ordering:** PVR sorts per-tile automatically (hardware OIT)
  — the game's draw-order-dependent blending mostly *stops mattering*, which
  is a simplification, but punch-through vs translucent classification per
  TEV alpha config needs the same care dca3 took.
- **Fog:** PVR table fog replaces the fragment-uniform fog — near-native fit.
- **EFB capture** (`GXCopyTex`, used by e.g. the NES/TV and transition
  effects): PVR render-to-texture exists but is costly; enumerate actual
  call sites in AC first (few) and handle per-case.
- **Textures:** keep the base port's decoders (pure C + scalar paths) but
  output twiddled ARGB1555/4444/RGB565 instead of linear RGBA8; palettized
  formats (CI4/CI8) map to PVR's native 4/8-bit paletted textures — a *win*
  (VRAM ¼ cost). VQ encoding is offline-only (too slow at runtime): add a
  content-hash → pre-VQ'd-texture disc cache built by a harvesting
  playthrough, same trick as the base port's shader seed. Max texture
  1024×1024 fits AC easily. Tools: `pvrtex` (KOS utils).

### 3.4 Audio: rspsim vs AICA, and the 8.3 MB bank

- **Precedent says software synthesis is feasible:** sm64-dc runs the N64
  audio engine (same lineage as jaudio_NES) on SH-4 at full speed. AC's
  mixer is heavier (reverb, comb, Haas, Dolby) but effects can be tiered off.
- **Stage A:** run rspsim on SH-4 at 22.05 kHz mono/stereo, effects minimal,
  feeding KOS `snd_stream`. Sequenced music keeps working with zero format
  work. Measure; this is the simplest thing that can possibly work.
- **Stage B (if A blows the CPU budget):** offload to AICA hardware — the
  64 ADPCM channels *are* a sampler; jaudio's instrument banks (ADPCM
  codebooks, key regions, envelopes) convert offline to AICA-native ADPCM
  samples in the 2 MB sound RAM, sequencing stays on SH-4 (cheap), synthesis
  moves to hardware (free). This is real work (a new backend under the
  `Na_*`/jaudio seam) but is the "correct" DC architecture.
- **The 8.3 MB audiorom never sits in main RAM.** Offline tool splits it:
  instrument samples → AICA sound RAM (converted) or disc-streamed; sequence
  data (small) → main RAM.
- Ambient SFX / K.K. songs that are streamed on GC can become disc-streamed
  ADPCM (KOS `snd_stream`, `wav2adpcm`/`dcaconv` offline).

## 4. Everything that transfers from the base repo

Directly reusable (little or no change): batch merging + state dedup + whole-
batch frustum cull; texture cache keying/content-hash scheme + decode budget;
`pc_disc.c`/`pc_dvd.c` (as **host-side tool code** now — see §5); generated
asset table + `gen_runtime_assets.py`; `pc_card.c`/`pc_m_card.c` save logic;
`pc_save_bswap.c` (SH-4 is LE — GCI stays Dolphin-compatible, keep as-is);
`pc_settings.c`, `pc_prof.c`, stubs; the seg2k0 pointer-heuristic knowledge
(re-derive for KOS's 0x8C000000 map); per-site byte-swap fixes in game code
(all LE→LE, untouched); the kill-switch env-var triage pattern (becomes
compile-time or settings flags on DC); harness/smoke-test discipline; kb/
discipline.

Dies or is rewritten: all GL/GLES code (`pc_gx` GL half, `pc_gx_tev.c`
entirely, shader caches/seeds, overlay renderer), SDL2 (window/input/audio →
KOS/maple/AICA), `pc_os.c` mmap arena (→ static KOS allocation), NEON decode
paths (scalar fallbacks retained; SH-4 rewrites where hot), texture packs,
model viewer, glibc shims.

Non-goals inherited from base: NES emulator (PPC-asm core, already excluded;
fixNES on SH-4 is a post-1.0 curiosity), multiplayer, HD texture packs.

## 5. Asset & disc pipeline (host-side)

The base port parses the ISO at runtime; the DC build moves all of that
offline into `tools/` (plain C/Python on the host, reusing `pc_disc.c`):

```
user's GAFE01 ISO (rom/)                          [never committed]
  → extract: main.dol, foresta.rel (Yaz0), FST files   (~30 MB real content;
     the 1.46 GB image is padding — strips out)
  → convert: audio banks → AICA ADPCM / stream files
             harvested textures → VQ/twiddled PVR cache
             biggest data tables → disc-resident assets
  → layout:  DC disc filesystem + 1ST_READ.BIN (game ELF)
  → mkdcdisc → OpenCrossing.cdi   (boots Flycast, burns to CD-R, GDEMU)
```

- Runtime reads go through KOS VFS (`/cd`) with a **read-ahead thread** from
  day one (the base port's known 8.7 s stall lesson; CD-R streams at only
  ~500 KB/s — hot files placed on outer tracks).
- The CDI contains Nintendo assets → **never distributed**; the repo ships
  tools only (dca3/sm64-dc legal model, which has survived — see
  `kb/research-ecosystem.md` §9). Optionally add a Colab-style one-click
  builder later, as dca3 and sm64-dc both did.

## 6. Saves: 456 KB into a 100 KB VMU

The honest second-hardest problem after RAM. AC's save is ~57 GC blocks
(≈456 KB). VMU user space ≈ 100 KB (200 × 512 B blocks). Options, in order:

1. **Measure real entropy.** Much of `Save_t` is item grids / letter slots /
   pattern data that compress well. Prototype early (M1, host-side): LZ
   (miniLZO/zlib) over real late-game saves. If worst-case compressed ≤ ~95
   blocks, ship compressed single-VMU saves (dedicated VMU, dca3 model).
2. **Segmented saves across 2 VMUs** (DC controller has two slots) if
   compression alone falls short.
3. **Trim the save** (fewer letter/pattern slots in DC edition) — last resort,
   breaks GCI interchange, documented loudly.
4. Modern hardware (VM2, VMU Pro) makes this trivial but must stay optional.

Keep GCI import/export via a host-side converter tool so towns move between
DC ↔ Dolphin ↔ other ports regardless of on-VMU format.

## 7. Input

DC pad: 1 analog stick, dpad, A/B/X/Y, analog L/R triggers, Start — no
C-stick, no Z, no dual shoulder clicks. Mapping draft: stick=walk,
A=action, B=cancel/tools, X=inventory, Y=map/pockets toggle, L trigger=Z
menu substitute, R trigger=run/hold, dpad=camera/C-stick substitute, Start=
start. In-game typing: DC keyboard peripheral supported via maple (nice
native fit for AC letters); on-screen keyboard already exists in-game.
VMU LCD: town name / bells on the 48×32 screen — free charm, do it at M4.

## 8. Toolchain & workflow

- **Toolchain (built, pinned, working):** `opencrossing-dc:sdk` — sh-elf GCC
  15.2.0, newlib 4.6.0.20260123, binutils 2.45.1, KOS 2.3.0 (`1c6398f9`),
  kos-ports (`f4faacc4`), GLdc (`a1cd80a8`), mkdcdisc (`3c2ef63a`),
  `-m4-single`, thread model kos. Cold build ≈ 27 min. Host entry points:
  `dc/build-dc-image.sh` (image, idempotent) then `dc/build-dc.sh` (build).
  `dc/build-dc-docker.sh` runs *inside* the container and is not a host entry
  point. This host has **no BuildKit**: `DOCKER_BUILDKIT=0`, never pass
  `--progress`. Note **char is SIGNED by default** on this build, so
  `-fsigned-char` is belt-and-braces rather than load-bearing — the ARM trap
  does not reproduce here. kos-ports: GLdc, zlib.
- **Iteration:** build → `mkdcdisc` → **Flycast** headless/windowed (author
  lineage = dca3's skmp; accuracy proven by dca3 itself). GDB: Flycast's GDB
  server (port 3263) for crash triage; lxdream-nitro when MMIO-level truth is
  needed. Harness: `harness/smoke.sh dc` boots the CDI in Flycast and greps
  serial/console output — same discipline as the base repo, no ROM material
  committed.
- **Hardware phase:** burned CD-R. **Check console revision first:** MIL-CD
  boot works on VA0/VA1; late VA2 (post-Nov-2000) units cannot boot CD-Rs —
  verify the target console before buying blanks. dcload-serial/ip only if a
  coder's cable / BBA turns up; otherwise CD-R iteration is slow → keep it
  for milestone gates, not daily work.
- **Branches:** `main` = releases, `dev` = daily, tags build CIs — inherited
  rules. CI cross-builds the ELF only (no assets), Actions-minutes-cheap.

## 9. Milestones

- **M0 — scaffold. ✅ MET 2026-08-01.** Toolchain container builds a KOS
  hello-world; mkdcdisc + Flycast smoke harness runs it. Repo: `dc/`,
  `tools/`, kb seeded. *Gate: `harness/dc/smoke.sh` green — verified for real:
  exits 0 on selftest, 1 on crashtest, `crash.sh` symbolises the fault to
  `crashtest.c:39`, `perf.sh` passes in-band and fails on a shifted baseline.*
- **M1 — it links. ✅ MET 2026-08-01.** *Gate: ELF links; extractor output
  complete; save-compression numbers in kb.* **3917/3917 TUs compile for
  sh-elf at `-O0`+guards** (the gate originally read "-O2+guards"; see §3.2 —
  `-O0` is now the contract), zero exclusions, and `src/` was not modified:
  every compat fix lives in `dc/include/dc_prelude.h` as a force-include. It
  links and produces a 27 MB unpadded CDI.
- **M2 — pixels. ← current milestone; RAM is the blocker.** Boots to title
  screen in Flycast on GLdc stage-A renderer; arena shrunk with boot-time
  memory ledger; assets load from `/cd`. `tev_map.md` written (all 101 configs
  classified). *Gate: title screen at any fps; RAM ledger ≤ 16 MB true.*
  **The linked image is 22.5 MB against a 16 MB machine (§3.1) — it will not
  boot until ~6.5 MB comes out, and `-O0` is mandatory. This is the gate.**
- **M3 — vertical slice.** Load/create town, walk, enter buildings, talk,
  save/load on emulated VMU; audio stage A (rspsim @ 22 kHz). **CPU go/no-go
  gate:** town-frame game logic ≤ 25 ms. *This is the milestone that decides
  the project.*
- **M4 — make it fast.** Stage-B PVR renderer, FTRV T&L, VQ texture cache,
  AICA offload if needed, read-ahead tuning, VMU LCD. *Gate: 30 fps stable in
  town on Flycast; first real-hardware CD-R boot.*
- **M5 — full game + release.** Playthrough coverage (events, island,
  museum…), DC-edition cuts documented, builder polished (one command →
  CDI), release process. *Gate: harness playthrough script + manual test
  plan pass on hardware.*

## 10. Top risks

| Risk | Sev | Mitigation |
|---|---|---|
| RAM doesn't fit even after §3.1 | **fatal, and now ACTIVE** | measured at 22.5 MB vs 16 MB; levers are layout-class only (§3.2). Asset pack lands −15.68 MB of peak but exposes an 8.22 MB destination-array floor. If layout levers don't close it, the honest outcomes are: cut content, or the port doesn't fit — not "turn on -O2" |
| Game logic too slow on SH-4 at `-O0` | fatal | M3 gate. The `-O2` term is **gone** (§3.2), so the budget is genuinely worse: 30 fps target, FTRV/XMTRX, store queues, source-level work on profiled hot functions |
| Source-level hot-path rewrites diverge from upstream | med | keep them minimal and upstreamable; they're reviewable in a way `-O2` is not |
| Alignment faults at `-O0` (residual class) | high | exception-handler triage (installed in `dc_main.c`), per-TU fallback, fix at source |
| Save won't fit VMU | high | compression prototype at M1, 2-VMU spanning, converter tool preserves GCI interchange |
| TEV configs that don't approximate | med | finite set (101); per-config table; worst cases get baked assets |
| EFB-capture effects | med | enumerate call sites; per-case (pre-render, stub, or PVR RTT) |
| CD-R streaming too slow for acre loads | med | read-ahead thread, outer-track layout, decode budget carries over |
| Console revision can't boot CD-R | low | check VA revision before hardware phase; GDEMU fallback |
| Upstream divergence (dt fixes land daily) | low | track flyngmt; cherry-pick dt/timing work — it's exactly what 30 fps needs |

## 11. Open questions (to resolve during M0–M1)

**Resolved 2026-08-01:** `foresta.map`/`static.map` are droppable — only
reader is `JUTException::queryMapAddress_single` on the `OSSetErrorHandler`
path, which returns false outside `0x80000000..0x82FFFFFF`. And the resident
REL blob is solved by `dcasset pack` (§3.1). New question raised by that work:
**does `emu64` emulate an N64 CPU/RSP, or only translate microcode to GX?**
That decides whether an RDRAM-sized buffer is sitting in our `.bss` — the game
shipped on N64 in 4 MB, so buffers sized for a 24 MB GameCube are not sized
for need. (Feeds §3.1 and question 6.)

1. Real JKRHeap high-water marks — instrument the PC build. (Feeds §3.1.)
2. Real save entropy — compress a late-game GCI corpus. (Feeds §6.)
3. Do any of the 101 TEV configs use indirect textures / EFB feedback in
   ways that matter visually? (Feeds §3.3.)
4. rspsim CPU cost at 22 kHz on SH-4 — measure in Flycast at M3. (Feeds §3.4.)
5. Which Dreamcast revision is the target console? (Feeds §8.)
6. emu64's GC-address-range assumptions (0x80000000–0x83000000) vs KOS's
   0x8C000000 RAM base — re-derive the seg2k0 pointer heuristic. (M1.)
