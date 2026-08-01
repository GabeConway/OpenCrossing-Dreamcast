# The N64 origin of Animal Crossing, and what it means for 16 MB

Research pass, 2026-08-01. Question posed: *Animal Crossing GC is widely said
to be the N64 game "running under emulation". If so, the 22.5 MB image is
probably an emulation artefact (an emulated RDRAM image, GameCube-sized
arenas) and the real game fit in 4 MB on N64.*

**Short answer: the premise is half right and the conclusion is wrong.**
There is no emulated RDRAM. The N64→GC translation machinery costs ~650 KB
total, and 549 KB of *that* is a texture cache the **PC port** inflated, not
the emulator. The bloat is somewhere else entirely, and the N64 heritage
tells us exactly where — see §5.

Measurement provenance for every number marked **[M]**: `nm --size-sort -S`
over all 3,621 `.o` files in `dc/build/obj` (the DC-target objects, `sh-elf`,
`-DTARGET_PC`), aggregated by object and directory. The linked
`AnimalCrossing.elf` / `.map` were deleted by a concurrent rebuild partway
through this pass, so per-object aggregation is the source of truth here; it
excludes KOS/newlib/GLdc and linker padding. It cross-checks against
`kb/mem-budget.md`'s independent PC-build figures to the byte on
`src/data/**` BSS (8,519,191 in both), so the attribution is sound.

---

## 1. Emulation verdict

### 1.1 What emu64 actually is

**emu64 is a GBI display-list interpreter that emits GX. It is not a machine
emulator.** No CPU emulation, no RSP emulation, no RDRAM image.

The decisive evidence is in the tree, not on the web (the web has essentially
nothing technical on emu64 — see §1.4):

| Evidence | Location | What it proves |
|---|---|---|
| `u32 segments[EMU64_NUM_SEGMENTS];` with `EMU64_NUM_SEGMENTS 16` | `include/libforest/emu64/emu64.hpp:750`, `:24` | The N64 segment table is **16 words = 64 bytes** of *real GameCube pointers*. Not a base-plus-offset into an emulated memory image. |
| `seg2k0()` GC branch: `k0 = segments[(segadr>>24)&0xF] + (segadr&0xFFFFFF)` then panics unless `k0 >= 0x80000000 && k0 < 0x83000000` | `src/static/libforest/emu64/emu64_utility.c` (the `#else`, i.e. non-`TARGET_PC`, branch) | The resolved address must land in **GameCube MEM1 cached space** (0x80000000–0x83000000 = the 48 MB-capable window). If an emulated 4/8 MB RDRAM buffer existed, the bound would be its extent. It isn't. |
| emu64 state constants: `NUM_TILES 8`, `NUM_TLUTS 16`, `MTX_STACK_SIZE 10`, `VTX_COUNT 128`, `DL_MAX_STACK_LEVEL 18`, `TMEM_ENTRIES 128` | `include/libforest/emu64/emu64.hpp`, `.../texture_cache.h:18` | These mirror the **F3DEX2 RSP register file and RDP tile state**, i.e. the graphics microcode's *state*, at the exact sizes real N64 microcode used. A hardware emulator would need RDRAM; a DL interpreter needs only this. |
| `emu64_class` object is 8,824 B **[M]** | `src/static/libforest/emu64/emu64.c` | The entire emulator instance is under 9 KB. |

So: N64 display lists, vertex arrays, textures and matrices are stored **in
GameCube RAM in N64 (GBI) binary format**, addressed with N64 segmented
addresses, and `emu64` walks them command-by-command issuing GX calls. That
is a *format* translation, not a *machine* emulation.

### 1.2 What is genuinely emulated

One thing is: **the N64 audio RSP microcode**, re-implemented in software.

- `src/static/jaudio_NES/internal/rspsim.c` — `static u8 DMEM[0x1000]`, the
  ADPCM code-book buffer, and the 64×4 `RES_FILTER` resampler table. This is
  a faithful software re-implementation of the N64 audio microcode's DMEM
  scratchpad and its ADPCM/resample kernels.
- Cost: **4 KB of DMEM.** Irrelevant to the budget.

### 1.3 What is *ported*, not emulated

The game logic is native PowerPC C. Three independent proofs:

1. **File-set identity with the N64 decomp.** `zeldaret/af` (the N64
   Doubutsu no Mori decomp) has `src/code/{main.c, game.c, graph.c,
   gamealloc.c, gfxalloc.c, listalloc.c, m_malloc.c, TwoHeadArena.c,
   THA_GA.c, PreRender.c, m_game_dlftbls.c}` and `src/boot/{boot_main.c,
   idle.c, fault.c, irqmgr-adjacent files, ovlmgr.c, yaz0.c, m_std_dma.c}`.
   This repo has `src/{main.c, game.c, graph.c, gamealloc.c, gfxalloc.c,
   TwoHeadArena.c, THA_GA.c, PreRender.c, executor.c, irqmgr.c, padmgr.c,
   c_keyframe.c, lb_rtc.c, zurumode.c}` — the same translation units, same
   names, same allocator architecture. The GameCube version is the N64
   version's **source tree**, recompiled.
2. **libultra is reimplemented, not emulated.** `src/static/libultra/`
   contains real N64 OS API entry points — `osCreateThread`, `osSendMesg`,
   `osRecvMesg`, `osSetTimer`, `osContReadData`, `gu*` math, `xprintf` —
   implemented on top of Dolphin OS. `include/libultra/*.h` is the N64
   headers. The game still *calls* the N64 OS; the calls are serviced
   natively.
3. **N64 library namespaces survive verbatim** in `src/static/`: `libu64`,
   `libc64`, `libultra`, `libforest`. `af` has `src/boot/libu64`,
   `src/boot/libc64` in the same shape.

### 1.4 Precise formulation, and what the web supports

> Animal Crossing (GC) is the **source code** of Doubutsu no Mori (N64)
> recompiled for PowerPC, retaining the N64 graphics API (GBI/F3DEX2) and the
> N64 OS API. A runtime layer (`emu64`) interprets the retained N64 display
> lists into GX calls, and the N64 audio microcode is re-implemented in
> software (`rspsim.c`). Nothing is emulated at the machine level.

Web sourcing on this specific point is **weak** — searches for
emu64/GBI-to-GX turn up nothing technical. What is well-sourced is the
*lineage*: `zeldaret/af`'s README states "There is a decompilation project
for the GameCube versions (*Animal Crossing*, etc.)
[here](https://github.com/Prakxo/ac-decomp/)", and `ACreTeam/ac-decomp`'s
README states "A decompilation of the original N64 version of the game is
being worked on [here](https://github.com/zeldaret/af)". Neither project
describes the GC build as emulated; ac-decomp treats it as a native GameCube
decompilation. The popular "it's an N64 game in an emulator" claim conflates
the retained N64 API surface with machine emulation. **Do not plan around it.**

(The separate, genuinely-emulated thing people remember is the hidden **NES**
emulator — `src/famicom_emu.c`, `src/static/Famicom/ks_nes_core.cpp`. Total
footprint **83,038 B** **[M]** — 20,890 text / 61,102 bss / 950 data / 96
rodata, of which `FONT_nes_tex_font1` is 24,576 and `emusound.c`'s
`SoundQbuf` + `sound_loop_buffer` are 32,768. Deleting the NES emulator
saves under 83 KB. **Not a lever.** Do not spend a milestone on it.)

---

## 2. The N64 decompilation project

| | |
|---|---|
| **Active project** | **https://github.com/zeldaret/af** — "Animal Forest", WIP decomp of どうぶつの森 |
| Site | https://zeldaret.github.io/af/ (progress dashboard: Boot / Code / Overlays + Objects asset analysis; the static page did not expose numeric percentages to fetch — **[UNVERIFIED]** exact %) |
| Target | Japan release, `baseroms/jp/baserom.z64` |
| Build | `make venv` → `make setup` → `make extract` → `make` → `make compress`; mips toolchain, Python 3 |
| Superseded predecessor | https://github.com/BluRosie/doubutsu-no-mori ("superseded by zeldaret/af") |
| Other WIP fork | https://github.com/Lrs121/animalforest |
| Translation/tooling side-repo | https://github.com/BluRosie/doubutsu-no-mori-binary |
| GC counterparts | https://github.com/ACreTeam/ac-decomp (reported 100% DOL), https://github.com/Prakxo/ac-decomp, https://github.com/fraser125/Animal-Crossing-Decomp |
| **Port stance** | Both `af` and `ac-decomp` explicitly state that being a basis for a port is a **non-goal**. There is **no** Ship-of-Harkinian-style N64 PC port of Doubutsu no Mori to copy memory strategy from. |

### 2.1 What `af` tells us about the original memory architecture

This is the useful part, and it is the key to §5.

- **Directory `src/dmadata`** plus `src/boot/m_std_dma.c`, `src/boot/ovlmgr.c`
  and `src/boot/yaz0.c`. That is the canonical N64 architecture: a DMA table
  in ROM, an overlay manager, and Yaz0 decompression. **Assets and code
  overlays were DMA'd out of the 16 MB cartridge on demand and decompressed
  into RAM.** Nothing was resident that did not need to be.
- **Allocators, one-to-one with this repo**: `include/system_heap.h`
  (`SystemHeap_Init/Malloc/Free`), `include/m_malloc.h`,
  `include/gamealloc.h`, `include/listalloc.h`, `include/TwoHeadArena.h`,
  `include/THA_GA.h`, `include/gfxalloc.h`, `include/audio_heap.h`,
  `include/segment_symbols.h`.
- **The system heap is "whatever is left"**, not a constant. From
  `src/code/main.c`:

  ```c
  fb = func_800D94F0_jp();
  sysHeap = (uintptr_t)SEGMENT_VRAM_END(buffers);
  gSystemHeapSize = fb - sysHeap;
  SystemHeap_Init((void*)sysHeap, gSystemHeapSize);
  ```

  i.e. heap = (framebuffer base) − (end of the linked BSS segment). There is
  no hardcoded heap budget to copy; the design is "reserve as little
  statically as possible and give the remainder to the heap".
- Main thread stack is `STACK(sMainStack, 0x900)` = **2,304 bytes**
  (`src/boot/idle.c`). For scale.

The architectural lesson is blunt: **on N64 the static footprint was
deliberately minimal and everything else was paged in from the cart.** The
PC port inverted that (§5).

---

## 3. The N64 original's memory budget

| Fact | Value | Source |
|---|---|---|
| RDRAM | **4 MB, stock N64. Expansion Pak NOT required.** | Cartridge/hardware teardowns; no Expansion Pak requirement is documented for Doubutsu no Mori, and it is absent from every Expansion Pak game list. |
| Mask ROM | **16 MB (128 Mbit)**, `MX23L12802-35C` | Cartridge teardown (`NUS-NAFJ-JPN`) |
| Save | 1 Mbit FlashRAM (`29L1100KC-15B0`) + Controller Pak support | ditto |
| RTC | `RTCK-NUS` + 32.768 kHz crystal + CR2032 (clock only, not save) | ditto |
| Framebuffer | 320×240, 16 bpp, double-buffered ⇒ 2 × 153,600 = **307,200 B**, plus a same-size Z-buffer where used | N64 standard; corroborated by `SCREEN_WIDTH`/`SCREEN_HEIGHT` = 320/240 in this tree (`m_play.c:54` doubles them for GC) |
| System heap | `framebuffer_base − end_of_BSS` — **no constant**; on a 4 MB machine with ~0.9 MB of code (`func_800D94F0_jp` sits at 0x800D94F0, ~890 KB into RDRAM) this is roughly 2.5–3 MB | `af` `src/code/main.c` |

**Breakdown beyond the above is [UNVERIFIED]** — `af` does not publish a
memory map and I did not build it.

**Anchor for the port:** the entire N64 game — code, data, heap, audio heap,
framebuffers, Z-buffer — lived in **4,194,304 bytes**. Our Dreamcast build
currently reserves **4,000,000 bytes for the JKRHeap alone**
(`DC_MAIN_MEMORY_SIZE`, `dc/include/dc_platform.h:102`), *on top of* a 22.5 MB
image. That is the whole indictment in one line.

---

## 4. Large static buffers in this repo, with provenance

### 4.1 Section totals **[M]** (per-object, excludes KOS/newlib/GLdc)

| Section | Bytes |
|---|---:|
| `.text` | 4,884,384 |
| `.data` | 2,632,757 |
| `.rodata` | 418,848 |
| `.bss` (+common) | **13,229,260** |

### 4.2 `.bss` by directory **[M]** — the shape of the problem

| Directory | `.bss` | `.data` |
|---|---:|---:|
| `src/data/model` | **5,682,621** | 1,124,899 |
| `src/data/npc` | **1,593,792** | 567,762 |
| `src/game` | 1,548,236 | 100,162 |
| `src/static/jaudio_NES` | 1,265,101 | 29,664 |
| `src/data/field` | **1,144,896** | 524,056 |
| `src/static/libforest` (emu64) | 562,374 | 805 |
| `src/actor` | 525,523 | 74,196 |
| `pc/src` (platform staging still linked) | 320,158 | — |
| `src/actor/npc` | 185,535 | 60,661 |
| `src/system` | 148,806 | — |
| `src/data/{item,submenu,font}` + misc | 97,882 | — |
| **`src/data/**` total** | **8,519,191** | **2,216,717** |

`src/data/**` is **64.4 % of all BSS**. It is not the emulator. It is not
GameCube arenas. It is **assets**.

### 4.3 Individual buffers, with what they are and where their size came from

Legend for **Origin**: *N64* = size inherited from the N64 design;
*GC* = sized for GameCube hardware (640×480, 24 MB MEM1 + 16 MB ARAM);
*PC* = **inflated by the Anbernic/PC port**, larger than what shipped on
GameCube.

| Bytes **[M]** | Symbol | Declared at | What it is | Origin | Safe DC size | Saving |
|---:|---|---|---|---|---:|---:|
| 1,228,800 | `prbuf` | `src/game/m_play.c:54` — `static u8 prbuf[(2*SCREEN_WIDTH)*(2*SCREEN_HEIGHT)*sizeof(u32)]` | EFB→texture capture of the frame, used as the frozen backdrop behind the pause/inventory/submenu and the FB-demo wipe | **GC** (the `2*` is the 320×240→640×480 upgrade; N64's equivalent was 320×240×2 = 153,600) | `sizeof(u16)` = 614,400. **Proven exactly**: the only writer is `copy_efb_to_texture()` (`m_play.c:657`) which does `GXSetTexCopyDst(640, 480, GX_TF_RGB565, 0)` — 2 B/px — and the only reader binds it as `G_IM_SIZ_16b` (`m_play.c:748`). The `sizeof(u32)` is a 2× over-allocation **in the retail game**. | **−614,400** |
| 589,824 | `audiomemory` | `src/static/jaudio_NES/game/game64.c_inc:587` — `u8 audiomemory[0x90000]`; handed whole to `Jac_Start(audiomemory, sizeof(audiomemory), v)` at `:1861` | The jaudio audio heap: sequence banks, wave-table headers, session heap (`AG.audio_heap_p` / `audio_heap_size`, `internal/system.c:1545`, `internal/memory.c:358`) | **GC** (N64 jaudio heaps were a fraction of this; 576 KB is 14 % of the whole N64 machine) | Wave data belongs in the 2 MB AICA SRAM, not SH-4 RAM. Floor is **[UNVERIFIED]** without an `audiomemory` high-water probe — instrument `Nas_HeapInit` and measure. Conservative first cut 0x40000. | **−262,144** (conservative) |
| 524,288 | `texture_buffer_data` | `src/static/libforest/emu64/emu64.c:41`, sized by `TEX_BUFFER_DATA_SIZE` at `include/libforest/emu64/texture_cache.h:71` | emu64's N64→GX texture conversion arena for textures living in `.data` | **PC** — the header literally says `0x80000 /* 512 KB (was 48 KB) */`; the retail GameCube value is `0xC000` at `texture_cache.h:74` | `0xC000` = 49,152. **Provably sufficient: the retail game shipped with it.** | **−475,136** |
| 320,150 | `l_keepSave` 155,648 + `l_keepOriginal` 52,384 + `l_keepMail` 47,780 + `l_keepDiary` 47,618 + `l_mcd_foreigner_file` 16,384 | `pc/src/pc_m_card.c` | Staging copies of the ~456 KB GameCube memory-card save | **GC** | Restructure for the ~100 KB VMU path (`kb/save-budget.md`). Exact residual **[UNVERIFIED]**. | **−250,000** (est.) |
| 294,912 | `aSTR_overlay` | `src/actor/ac_structure.c:25` — `static u8 aSTR_overlay[aSTR_ACTOR_TBL_COUNT][aSTR_OVERLAY_SIZE]`, 32 × 0x2400 | Landing buffers for 32 concurrently-resident *structure* actor overlays | **N64** — this is the cart-paging design (`ovlmgr.c` in `af`), working as intended | Keep. Reducing the count is a gameplay change (fewer simultaneous buildings). | 0 |
| 275,456 | `seq` | `src/static/jaudio_NES/internal/seqsetup.c` | Sequence data staging | **N64/GC** | Keep (audio plan owns this) | 0 |
| 187,328 | `common_data` | `src/game/m_common_data.c` | The live save state — the town, villagers, inventories | **N64** | Keep. Untouchable. | 0 |
| 137,864 | `aBTD_island_prg` (86,596, `ac_boat_demo.c:35`, `[0x15244]`) + `aBTD_island_ldr` (51,260) | `src/actor/ac_boat_demo.c` | Island (GBA-link) overlay code+data buffers | **N64** overlay design | Keep, or make demand-allocated — the island is entered rarely, so these two are excellent candidates for a scene-scoped allocation instead of BSS. | **−137,864** (if made dynamic) |
| 132,104 | `sys_dynamic` | `src/system/sys_dynamic.c:3` — `dynamic_t sys_dynamic;` | The GX display-list build buffers that emu64 writes into | **GC** (pure GX-side machinery, no N64 counterpart) | Right-size against PVR vertex-buffer needs once the renderer lands. **[UNVERIFIED]** | ? |
| 81,920 / 65,536 / 65,536 / 37,792 / 32,768 / 25,600 / 24,576 | `CHANNEL`, `dmabuffer` (`heapctrl.c`), `dvd_buf.3` (`dvdthread.c`), `AG` (`audiowork.c`), `CALLSTACK` (`dvdthread.c`), `pc_task_buf` (`neosthread.c`), `CH_BUF` (`dspinterface.c`) | `src/static/jaudio_NES/internal/*` | jaudio driver state, DMA staging, DVD read thread | **N64/GC** mix | `dvd_buf` and `dmabuffer` are 64 KB disc-staging buffers sized for the GC DVD; CD-R at 500 KB/s wants read-ahead, so **do not shrink these blindly**. `CALLSTACK` 32 KB is generous. | small |
| 39,168 | `nintendo_hi_0` | `src/static/nintendo_hi_0.c` | Boot logo | GC | Evict to disc | −39,168 |
| 24,576 | `FONT_nes_tex_font1` | `src/data/font/` | NES-emulator font | GC | Dies with the NES emulator (non-goal) | −24,576 |
| 16,384 | `texture_buffer_bss` | `emu64.c:42`, `TEX_BUFFER_BSS_SIZE` at `texture_cache.h:72` (`0x4000 /* 16 KB (was 1 KB) */`) | emu64 conversion arena for textures living in `.bss` | **PC** — retail GC value is `0x400` (`texture_cache.h:75`) | `0x400` = 1,024 | **−15,360** |
| 8,192 | `texture_cache_list` | `emu64.c:47`, `TEXTURE_CACHE_LIST_SIZE` at `texture_cache.h:14` (`1024 /* PC: larger list */`) | original→converted texture address map | **PC** — retail GC value is `256` (`texture_cache.h:16`) | 256 entries = 2,048 | **−6,144** |
| 64 | `emu64::segments[16]` | `include/libforest/emu64/emu64.hpp:750` | **The entire N64 segment table.** Listed here because it is the buffer the lead expected to be 4–8 MB. | N64 | Keep | 0 |
| 4,096 | `DMEM` | `src/static/jaudio_NES/internal/rspsim.c:8` | The emulated N64 audio-RSP DMEM. **The only genuinely emulated memory in the build.** | N64 | Keep | 0 |

### 4.4 Buffers that do **not** exist — negative results worth recording

- **No emulated RDRAM image.** No 4 MB or 8 MB static array exists anywhere in
  `src/`, `include/`, `pc/src/` or `dc/`. Largest static array in the tree is
  `prbuf` at 1.17 MB.
- **No RSP task / OSTask buffer, no RDP command FIFO, no GBI DL scratch pool.**
  emu64 walks display lists in place out of whatever memory the segment table
  points at; there is no interpreter-owned DL buffer.
- **`DC_MAIN_MEMORY_SIZE` is not a static array.** `dc/include/dc_platform.h:102`
  = 4,000,000, but `dc/src/dc_os.c:400` obtains it via
  `dc_mem_alloc(DCMEM_JKRHEAP, …)` at runtime. It therefore does **not**
  appear in the 13.5 MB BSS — it is **4 MB on top of it**. The PC value it
  descends from is `PC_MAIN_MEMORY_SIZE = 24 MB` (`pc/include/pc_platform.h:36`),
  so this one has already been right-sized by a factor of six.
- **emu64's real cost is trivial.** Whole-layer total **[M]**: 82,924 text +
  562,358 bss + 805 data + 1,490 rodata = **648,229 B**. Subtract the three
  PC-inflated buffers (548,864) and emu64's genuine runtime state is
  **~13.5 KB**. The N64→GX translation layer is *not* why we are over budget.

---

## 5. Synthesis — where the 6.5 MB actually is, and why the N64 heritage is the fix

### 5.1 The one-sentence diagnosis

The N64 kept ~8.5 MB of models, textures and display lists **on the 16 MB
cartridge** and DMA'd them in on demand (`src/dmadata`, `ovlmgr.c`, `yaz0.c`
in `af`). The GameCube kept them compiled into the DOL/REL, which was fine in
24 MB. The Anbernic/PC port then converted them into **empty BSS placeholders
that `pc_assets.c` fills eagerly at boot from the DOL/REL** — a design that is
correct with 24 MB of host RAM and catastrophic with 16 MB, because it keeps
the GameCube's "everything resident" model *and* pays full BSS for it.

Confirmed mechanism: `pc/src/pc_assets.c` — `s_assets[]` (14,495 entries,
6,531,534 B) is walked unconditionally in `pc_assets_init()`, plus 1,848
per-file `_pc_load_src_*()` loaders across 768 files (2,239,824 B).
**8,771,358 B loaded into BSS at boot**, cross-checked against
`kb/mem-budget.md`. Declaration pattern (`src/data/model/lat_letter64_xk_tex.c:8`):

```c
#ifdef TARGET_PC
u16 lat_letter01_pal[0x20 / sizeof(u16)] ATTRIBUTE_ALIGN(32);   /* BSS */
#else
u16 lat_letter01_pal[] ATTRIBUTE_ALIGN(32) = { #include "assets/lat_letter01_pal.inc" };
#endif
```

and `dc/Makefile:165` defines `-DTARGET_PC`, so the Dreamcast build takes the
BSS branch. **The Dreamcast is the machine that most resembles the N64 here**
— a slow bulk medium (CD-R, ~500 KB/s) standing in for the cart, and not
enough RAM to hold the medium's contents. The fix is to restore the N64's own
architecture: demand-page assets, keyed by scene/acre, into a bounded pool.

### 5.2 Ranked recommendations

None of these are compiler optimizations. Right-sizing a static array,
changing an `#ifdef`, and moving eager loads to demand loads are all source
changes with no effect on codegen, so the `-O0` constraint is untouched.

| # | Action | Saving | Confidence | Effort |
|--:|---|---:|---|---|
| 1 | **Demand-page `src/data/**` instead of reserving it in BSS.** Replace the `#ifdef TARGET_PC` empty-array placeholders + eager `pc_assets_init()` with a bounded, disc-backed pool keyed on scene/acre residency, using `tools/dcasset` output as the "cartridge". Natural granularity is already there: 2,080 object files, ~2.5 KB median group. | **≈ −7,000,000** (8,519,191 BSS reduced to a ~1.5 MB resident pool) | High on the size, medium on the residency set — needs a per-scene working-set trace | Large. This is a milestone, and it is the only thing that closes the gap. |
| 2 | **`prbuf`: `sizeof(u32)` → `sizeof(u16)`** at `src/game/m_play.c:54`. | **−614,400** | **Very high** — `GXSetTexCopyDst(…, GX_TF_RGB565, …)` writes 2 B/px and the consumer binds `G_IM_SIZ_16b`. The extra 2× is dead in the retail game too. | One line. Do it first. |
| 2b | Then move the remaining 614,400 out of main RAM into an 8 MB-VRAM PVR render-to-texture. | −614,400 more | Medium (renderer-dependent) | Medium |
| 3 | **Revert the PC texture-cache inflation** in `include/libforest/emu64/texture_cache.h`: gate lines 71/72/14 so the DC build takes the retail GameCube values (`0xC000`, `0x400`, `256`) rather than the `TARGET_PC` values (`0x80000`, `0x4000`, `1024`). | **−496,640** (475,136 + 15,360 + 6,144) | **Very high** — these are the values the retail disc shipped with, so sufficiency is proven by the original product. Strictly better justified than `kb/mem-budget.md`'s proposed `0x80000 → 0x20000`. | Three lines. Note `TARGET_PC` is non-negotiable (`dc/Makefile:159`), so add a `TARGET_DC` guard inside the `TARGET_PC` branch rather than undefining anything. |
| 4 | **`audiomemory` 0x90000 → measured floor**, with wave data resident in AICA SRAM rather than SH-4 RAM. Instrument `Nas_HeapInit` (`internal/memory.c:358`) for a high-water mark first. | −262,144 conservative, plausibly −400,000+ | Medium. Floor is **[UNVERIFIED]**. | Medium; owned by `kb/audio-plan.md` |
| 5 | **`pc/src/pc_m_card.c` GC-save staging → VMU path** (`kb/save-budget.md`). | ≈ −250,000 **[UNVERIFIED]** | Medium | Medium |
| 6 | **Make the island/boat overlay buffers scene-scoped** (`aBTD_island_prg` + `aBTD_island_ldr`, `src/actor/ac_boat_demo.c`). Entered rarely; no reason to be resident. | −137,864 | High | Small |
| 7 | Evict `nintendo_hi_0` (boot logo) to disc; drop the NES emulator and `FONT_nes_tex_font1`. | −63,744 | High | Small |
| — | *Do not bother:* shrinking emu64's actual state (13.5 KB), deleting the NES emulator for size (83 KB total), or hunting for an emulated RDRAM buffer (does not exist). | — | — | — |

**Total from items 2–7 alone: ≈ 1.82 MB**, all low-risk, no codegen impact.
**Item 1 is the milestone** and is worth ~7 MB on its own — and it is not a
hack, it is a restoration of how the game was architected on the machine it
was written for.

### 5.3 The framing that should go in PLAN.md

> Doubutsu no Mori ran in **4 MB of RDRAM** off a **16 MB cartridge**, paging
> assets and code overlays in on demand. Animal Crossing kept that code and
> that data and simply stopped paging, because the GameCube had 24 MB. The
> Dreamcast has 16 MB and a 500 KB/s optical drive — it is architecturally
> much closer to the N64 than to the GameCube. Porting therefore means
> putting the cartridge paging back, with the CD-R as the cart. The
> "emulation layer" is a red herring: it costs 650 KB, and 549 KB of that is
> a PC-port texture cache we can delete today.

---

## Sources

- [zeldaret/af — Animal Forest N64 decompilation](https://github.com/zeldaret/af)
- [Animal Forest Decompilation progress site](https://zeldaret.github.io/af/)
- [BluRosie/doubutsu-no-mori (superseded)](https://github.com/BluRosie/doubutsu-no-mori)
- [BluRosie/doubutsu-no-mori-binary](https://github.com/BluRosie/doubutsu-no-mori-binary)
- [Lrs121/animalforest](https://github.com/Lrs121/animalforest)
- [Prakxo/ac-decomp — GameCube Animal Crossing decompilation](https://github.com/Prakxo/ac-decomp)
- [ACreTeam/ac-decomp](https://github.com/ACreTeam/ac-decomp)
- [Animal Crossing for the GameCube has been decompiled — GBAtemp](https://gbatemp.net/threads/animal-crossing-for-the-gamecube-has-been-decompiled.672373/)
- [Doubutsu no Mori (Controller Pak) (Japan) [NUS-NAFJ-JPN] — cartridge/PCB scans, Internet Archive](https://archive.org/details/doubutsu-no-mori_202405)
- [Nintendo 64 Expansion Pak — 4 MB RDRAM add-on](https://zelda-archive.fandom.com/wiki/Expansion_Pak)
- [Nintendo hid a fully-working NES emulator inside Animal Crossing — Nintendo Life](https://www.nintendolife.com/news/2018/07/nintendo_hid_a_fully-working_nes_emulator_inside_animal_crossing_on_gamecube)
- In-repo primary sources: `include/libforest/emu64/emu64.hpp`,
  `include/libforest/emu64/texture_cache.h`,
  `src/static/libforest/emu64/emu64_utility.c`,
  `src/static/libforest/emu64/emu64.c`,
  `src/static/jaudio_NES/internal/rspsim.c`,
  `src/static/jaudio_NES/game/game64.c_inc`,
  `src/game/m_play.c`, `pc/src/pc_assets.c`, `dc/include/dc_platform.h`,
  `dc/Makefile`, `kb/mem-budget.md`
