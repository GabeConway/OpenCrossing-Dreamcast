# Base repo map — OpenCrossing-Anbernic → Dreamcast survey

Surveyed 2026-08-01 at OpenCrossing-Anbernic `dev` @ `74a9998`. What the base
contains, what transfers to DC, what dies. Full paths refer to the base repo.

## Platform layer (`pc/src/`, 27 files ≈ 46k LOC)

Portable as-is (plain C, no GL/SDL): `pc_disc.c` (GC disc/CISO/GCM/FST/Yaz0
reader — becomes host tool), `pc_dvd.c` (sync fread + faked async — becomes
KOS VFS + read-ahead), `pc_assets.c` (30k-line generated table: ~2500 assets
→ DOL/REL offsets + per-asset bswap class; generator
`pc/tools/gen_runtime_assets.py`), `pc_card.c` + `pc_m_card.c` (GCI saves,
village gen, validation), `pc_save_bswap.c` (LE↔BE GCI interchange — SH-4 is
LE, keep unchanged), `pc_settings.c`, `pc_mtx.c` (PPC paired-singles
replacement → rewrite hot paths in SH-4 FTRV/FIPR), `pc_misc.c`, `pc_prof.c`,
stubs.

Rewrite for KOS: `pc_main.c` (SDL/GL init, signal-based crash recovery),
`pc_pad.c` (→ maple), `pc_audio.c` (→ AICA/snd_stream), `pc_vi.c` (frame
pacing logic portable, swap → pvr_scene_finish), `pc_os.c` (mmap arena →
static allocation; DC/IC cache ops are no-ops today but SH-4 + PVR DMA needs
real ones), `pc_typing.c`/`pc_keybindings.c` (→ DC keyboard), overlay.

Dies: `pc_gx_tev.c` (897 LOC TEV→GLSL generator + shader caches — no shaders
on PVR), GL half of `pc_gx.c`, `pc_texture_pack.c`, `pc_model_viewer.c`,
`glibc_compat.c`.

## Renderer facts that drive the DC design

- Layering: game → emu64 (game's own N64→GC DL translator,
  `src/static/libforest/emu64/`) → GX → `pc_gx*` → GL. emu64 omits `GXEnd`
  (flush-on-count logic exists), reuses buffers (content-hash cache keys
  exist for this).
- **101 unique TEV configs** harvested from a full playthrough
  (`shader_seed.bin`) — the complete finite spec for DC TEV approximation.
  Max 3 TEV stages (`PC_GX_MAX_TEV_STAGES`).
- Per-vertex GC lighting (8 lights, angular+distance atten) lives in
  `pc/shaders/default.vert` — on DC this becomes SH-4 CPU work.
- Vertex path: 65536 × 48 B static batch buffer (3.1 MB) → streaming VBO
  (6 MB GPU). Batch merging, state-set dedup, strip/fan→independent-tri
  conversion, whole-batch CPU frustum cull (object-space AABB × exact P*MV).
  **Cull is the big win: 60–80% of submitted batches fully offscreen** (game
  submits whole acres; avg ~286 culled/frame). All pure C — transfers.
- Draws/frame after all passes: **114.6 avg** (was 491–600). GL time
  5.4 ms/frame. `merged=0` in practice — state changes break 90% of batches
  (mv matrix loads 41%, tex 30%). CPU pre-transform idea (open) would help DC
  too.
- Texture decode: 10 GC formats, pure-C decoders (+NEON variants guarded by
  `__ARM_NEON`, scalar fallbacks byte-identical). Output today: linear RGBA8
  → DC needs twiddled 16-bit / paletted output stage. 2048-entry cache,
  content-hash keys, per-frame decode budget (8 non-tiny decodes, placeholder
  + retry when over).
- EFB capture (`GXCopyTex`) via FBO blit — DC needs per-callsite strategy.
- TLUT wrinkle: ROM-sourced TLUTs are BE, emu64/EFB ones native LE — per-slot
  `is_be` flag (`pc_gx_internal.h`).

## Memory (the numbers that must shrink)

| Allocation | Size | Source |
|---|---|---|
| Main arena (MEM1) | 24 MB | `PC_MAIN_MEMORY_SIZE`, pc_platform.h |
| SystemHeapSize | 22.8 MB | jsyswrap.cpp `0x16C7000` |
| gameheap | 3.5 MB | jsyswrap.cpp `0x380000` |
| ARAM emu | 16 MB malloc, DMA=memcpy | pc_aram.c |
| — soundAram | 8.44 MB (`0x810000`); audiorom.img 8.3 MB preloaded at boot | jsyswrap.cpp |
| — graphAram | 6.96 MB (`0x6A3780`); RARC archives | jsyswrap.cpp |
| Vertex batch buffer | 3.1 MB BSS | pc_gx_internal.h |
| Streaming VBO | 6 MB GPU | pc_gx.c |
| armhf ELF | 10.9 MB (bulk = `src/data/` 464k LOC compiled-in tables) | build |

Steady state ≈ 43–45 MB + 6 MB GPU. DC = 16 + 8 + 2. See PLAN §3.1.

Pointer heuristic: arena is mmap'd ≥ `0x10000000` because seg2k0
distinguishes real pointers from N64 segment addresses by range; KOS RAM
lives at `0x8C000000` — re-derive (PLAN §11.6).

## Build system / flags

- Hard 32-bit guard (`FATAL_ERROR` on 64-bit) — SH-4 is ILP32 ✅.
- Game code today: **no -O** (`CMAKE_C_FLAGS_RELEASE` carries only NDEBUG +
  UB guards). History in CMakeLists comments: **-O2 → wild-pointer crash loop
  from boot; -O1 → hard SIGBUS intro train (unaligned LDRD/VFP)** on ARM.
  Only emu64.c/emu64_utility.c proven -O2-safe. Platform code -O2,
  `pc_gx_texture.c` -O3.
- UB guards: global `-fno-strict-aliasing -fwrapv`; release adds
  `-fno-delete-null-pointer-checks -fno-lifetime-dse
  -fno-aggressive-loop-optimizations -fno-strict-overflow`.
- **Upstream flyngmt moved to -O2 everywhere** (commit `4f428276`,
  2026-07-10) with `-fno-strict-aliasing -fwrapv` — x86 only; the ARM/SH-4
  alignment class is untested there. See PLAN §3.2.
- `-fsigned-char` required (PPC chars signed; ARM GCC defaults unsigned).
  ⚠️ **CORRECTED 2026-08-02:** this used to say SH-4 GCC also defaults to
  unsigned `char`. It does not — confirmed on the GCC 15.2.0 in our SDK image
  (`kb/toolchain-components.md` §3.1 and §5.1). Passing `-fsigned-char` on
  sh-elf is harmless and stays; the claim about the default was wrong.
- Endianness: GC data BE → LE handled at asset load (per-asset class), GCI
  bswap, and ~scattered per-site swaps documented in `pc/DOCUMENTATION.md`
  ("cannot centralize" — ARAM holds mixed-width layouts). All transfers to
  LE SH-4 unchanged.
- Docker cross-build pattern (`build-armhf-docker.sh`, QEMU debian container)
  → replicate as KOS/dc-chain container.

## Perf state (predicts SH-4 budget)

v0.3.0 on RG-34XX SP (A53 1.5 GHz, one core effective): 56.4 fps avg, 98.4%
game speed. **Bottleneck is CPU game logic (at -O0), not GPU.** All 75
remaining stutters work-dominated (median 24 ms, max 114 ms). Known open
levers: per-TU -O2 triage, ISO read-ahead thread, CPU pre-transform. Kill-
switch env vars exist for every optimization (`PC_NO_*`) — keep the pattern.

## Game code stats

`src/` ≈ 990k LOC total; non-data ≈ 515k LOC gnu89. `src/data/` 464k LOC
generated tables (binary-size lever). emu64 ≈ 6.4k LOC. jaudio_NES 61 files
≈ 33k LOC incl. rspsim software DSP (ADPCM/RESAMP/ENVMIX, reverb, comb,
Haas, Dolby). Famicom NES core is PPC asm — excluded (PC port swapped in
fixNES; DC: non-goal). Excluded dirs already known-good: furniture/, AUS/,
Dolphin SDK, libultra, etc.

## Audio path detail

32 kHz s16 stereo; game thread → message queues → producer thread
(`pc_audio_process_frame` → `CreateAudioTask` → `RspStart2`) → 32768-sample
SPSC ring → SDL callback. `AIInitDMA` = handoff point. DSP calls are stubs
(rspsim does everything). Audio banks live in emulated ARAM
(soundAram 8.44 MB). DC strategy: PLAN §3.4.
