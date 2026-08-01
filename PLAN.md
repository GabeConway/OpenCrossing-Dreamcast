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
  to evict the biggest tables to disc-loaded assets. `-Os` for cold game code.
  Target: ≤ 4 MB text+data resident.
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

### 3.2 CPU: 200 MHz vs -O0 decomp code

- **Adopt upstream's -O2.** flyngmt commit `4f428276` (2026-07-10, "Compile
  everything at -O2") demonstrates game code at -O2 with
  `-fno-strict-aliasing -fwrapv` neutralizing the type-punning and
  signed-overflow UB classes — on x86. Carry the Anbernic repo's extra guards
  (`-fno-delete-null-pointer-checks -fno-lifetime-dse
  -fno-aggressive-loop-optimizations -fno-strict-overflow`).
- **Alignment is the residual class.** The base repo's ARM record: -O1 caused
  a hard SIGBUS (unaligned LDRD/VFP) on the intro train scene; x86 never sees
  this. SH-4 is also a strict-alignment ISA, so expect the same class.
  Mitigation: SH-4 raises a CPU exception on unaligned access — install an
  exception handler that logs PC + faulting address (the base port's SIGBUS
  recovery pattern), triage per-TU exactly as the Anbernic repo did, fix the
  misaligned casts at the source where found (upstreamable fixes).
- **Budget arithmetic.** Single-thread A53@1.5 GHz ≈ 10–20× SH-4@200 MHz
  scalar. Recovering -O0→-O2 (≥2–3×) plus a 30 fps target (2× the frame
  budget of 60) plus SH-4 FTRV/FIPR in `pc_mtx` hot paths closes much of the
  gap, but not provably all of it. **Go/no-go gate at M3:** measured game
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

- **Toolchain:** KOS master + dc-chain GCC (per sm64-dc: GCC 14+, KOS+GLdc
  master), containerized (Docker) exactly like the base repo's armhf flow —
  reproducible `docker run … build-dc.sh`. `-fsigned-char` (SH-4 GCC chars
  are unsigned by default, same trap as ARM). kos-ports: GLdc, zlib.
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

- **M0 — scaffold.** Toolchain container builds a KOS hello-world; mkdcdisc +
  Flycast smoke harness runs it. Repo: `dc/`, `tools/`, kb seeded. *Gate:
  `harness/smoke.sh dc` green.*
- **M1 — it links.** Vendored `src/` compiles for sh-elf at -O2+guards with
  a stubbed `dc/` platform layer; host `tools/` extract a disc layout from a
  real ISO; save-compression feasibility measured on real saves. *Gate: ELF
  links; extractor output complete; save-compression numbers in kb.*
- **M2 — pixels.** Boots to title screen in Flycast on GLdc stage-A renderer;
  arena shrunk with boot-time memory ledger; assets load from `/cd`.
  `tev_map.md` written (all 101 configs classified). *Gate: title screen at
  any fps; RAM ledger ≤ 16 MB true.*
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
| RAM doesn't fit even after §3.1 | fatal | measure early (M2 ledger); graph-ARAM windowing is the big unknown — prototype in M1 on PC build |
| Game logic too slow on SH-4 even at -O2 | fatal | M3 gate; -O2 triage plan §3.2; 30 fps target; profile-guided per-TU work |
| SH-4 -O2 miscompiles (alignment class) | high | exception-handler triage, per-TU fallback, fix at source |
| Save won't fit VMU | high | compression prototype at M1, 2-VMU spanning, converter tool preserves GCI interchange |
| TEV configs that don't approximate | med | finite set (101); per-config table; worst cases get baked assets |
| EFB-capture effects | med | enumerate call sites; per-case (pre-render, stub, or PVR RTT) |
| CD-R streaming too slow for acre loads | med | read-ahead thread, outer-track layout, decode budget carries over |
| Console revision can't boot CD-R | low | check VA revision before hardware phase; GDEMU fallback |
| Upstream divergence (dt fixes land daily) | low | track flyngmt; cherry-pick dt/timing work — it's exactly what 30 fps needs |

## 11. Open questions (to resolve during M0–M1)

1. Real JKRHeap high-water marks — instrument the PC build. (Feeds §3.1.)
2. Real save entropy — compress a late-game GCI corpus. (Feeds §6.)
3. Do any of the 101 TEV configs use indirect textures / EFB feedback in
   ways that matter visually? (Feeds §3.3.)
4. rspsim CPU cost at 22 kHz on SH-4 — measure in Flycast at M3. (Feeds §3.4.)
5. Which Dreamcast revision is the target console? (Feeds §8.)
6. emu64's GC-address-range assumptions (0x80000000–0x83000000) vs KOS's
   0x8C000000 RAM base — re-derive the seg2k0 pointer heuristic. (M1.)
