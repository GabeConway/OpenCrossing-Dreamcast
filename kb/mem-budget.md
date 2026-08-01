# RAM budget — 16 MB ledger

> ## ⚠️ Two corrections since this was written (2026-08-01, later same day)
>
> 1. **Every `-O2`/`-Os` remedy below is void.** Optimization is banned by
>    user directive (CLAUDE.md, PLAN §3.2) — `src/` builds at `-O0`, full
>    stop. Wherever this document proposes `-O2` for hot TUs or `-Os` for cold
>    ones (bucket 3, and the open-question list), read it as **"unresolved,
>    must be closed by a layout-class lever instead"**: `--gc-sections`,
>    `.bss` right-sizing, linker placement, moving data to `/cd`, or dropping
>    non-goal subsystems.
> 2. **The real sh-elf link now exists**, so bucket 3 no longer needs an ARM
>    proxy. Measured at `-O0`, all 3917 TUs, zero exclusions:
>    **text 6,318,568 / data 2,638,852 / bss 13,526,548 = 22,483,968 B.**
>    That is ~6.5 MB over budget before heap headroom.
>
> Also landed since: the resident REL blob (16,558,776 B) is solved by
> `dcasset pack` (−15.68 MB, see `kb/asset-pack.md`), which in turn exposed
> **8.22 MB of static asset destination arrays** — 15,726 of them, resident
> regardless of source, and now the single largest line in this ledger. It
> needs its own bucket. `kb/STATE.md` carries the live numbers.

Measured 2026-08-01. Supersedes the sketch in `PLAN.md` §3.1; PLAN should be
updated to point here. Every number below is tagged **[M]** measured today,
**[D]** derived from measured numbers, or **[?]** unverified estimate that
needs a real measurement (each has a named owner milestone).

Measurement subjects:

- ELF: `/Users/gabe/Documents/GitHub/OpenCrossing-Anbernic/pc/build-armhf/bin/AnimalCrossing`
  (32-bit ARM EABI5 PIE, not stripped, 10,958,900 B, built 2026-07-29) —
  the DC repo has no build tree of its own, and its `pc/` + `src/` are
  byte-identical in the areas measured.
- 3,929 `.o` files under `pc/build-armhf/CMakeFiles/` — same build, so
  per-source attribution is exact, not inferred. Sums agree with the linked
  image to within 0.3 % (`.text` 3,327,876 vs 3,335,604; `.bss` 18,244,355 vs
  18,255,512 — the difference is crt/libgcc objects outside the CMake tree).
- The user's ISO (read-only, never copied): GAFE01 USA Rev 0.
- Tools: `llvm-readelf` / `llvm-nm` from `/opt/homebrew/opt/llvm/bin`.

---

## 1. The real starting number is 65 MB, not 45 MB

`PLAN.md` §3.1 says "~45 MB working set". The measured figure is worse.

| Region | Bytes | | Source |
|---|---:|---|---|
| ELF `PT_LOAD` #1 (text+rodata+relocs) | 4,895,852 | [M] | `readelf -l` |
| ELF `PT_LOAD` #2 memsz (data+bss) | 21,269,212 | [M] | `readelf -l` |
| Main arena (`PC_MAIN_MEMORY_SIZE`) | 25,165,824 | [M] | `pc/include/pc_platform.h:36` |
| Emulated ARAM (`PC_ARAM_SIZE`) | 16,777,216 | [M] | `pc/include/pc_platform.h:37` |
| **Total address space** | **68,108,104** (64.95 MiB) | [D] | |

And essentially all of it is *touched*, so RSS ≈ VM: `pc/src/pc_os.c:277`
`memset(arena_memory, 0, PC_MAIN_MEMORY_SIZE)` and `pc/src/pc_aram.c:12`
`memset(aram_base, 0, PC_ARAM_SIZE)` both fault in the whole reservation at
boot. **4.06× a stock Dreamcast's entire RAM.**

Two corrections to existing kb text while we are here:

- `kb/base-repo-map.md` lists "SystemHeapSize 22.8 MB (`0x16C7000`)". That
  constant (`src/static/jsyswrap.cpp:36`) is a **dead default** — it is
  overwritten at `jsyswrap.cpp:493` with
  `arena_hi - arena_lo - 0xD0` = 24 MiB − 0x3100 − 0xD0 = **25,153,072 B
  (23.99 MiB)** [M]. The system heap is the whole arena, not 22.8 MB.
- `gameheap_len` (`jsyswrap.cpp:37`, `0x380000` = 3.5 MB) is likewise dead.
  `jsyswrap.cpp:547` sets it to `JKRHeap_getFreeSize(systemHeap) - 0x10000`
  — i.e. **the game heap swallows the entire remaining system heap** and
  `MallocInit()` hands it to `__osMalloc`. So the port has *never* measured
  what the game actually needs; it just gives it everything. That is why
  bucket 6 below is the #1 open measurement.

### Boot-time transient (worse than steady state)

`pc_assets_init()` (`pc/src/pc_assets.c:29863`) decompresses the whole REL
into RAM before copying assets out of it:

| Transient | Bytes | | Source |
|---|---:|---|---|
| `foresta.rel.szs` Yaz0-decompressed | **15,640,056** | [M] | Yaz0 header in ISO |
| `main.dol` | 918,720 | [M] | DOL header in ISO |

15.6 MB held live, on a machine with 16 MB total. The current asset path is
not merely too big on DC — it is arithmetically impossible. It must become
seek-and-read-per-asset from the disc (§5, change **C6**).

---

## 2. Binary size: top contributors (task 2 answer)

### Section totals [M]

| Section | Bytes | DC fate |
|---|---:|---|
| `.text` | 3,335,604 | stays (SH-4 recompile — size ratio unknown, see [?]) |
| `.rodata` | 713,756 | stays (note: 699,686 B of this is string/merge pools with no sized symbols — only 14,070 B is symbol-attributed) |
| `.data` | 2,643,072 | stays, minus evictions |
| `.data.rel.ro` | 364,308 | becomes plain rodata on a non-PIE static ELF |
| `.got` | 5,956 | ~0 |
| `.rel.dyn` | **818,680** | **gone** — PIE relocations; DC ELF is static non-PIE |
| `.rel.plt` | 1,256 | gone |
| `.ARM.exidx` + `.ARM.extab` | 18,036 | replaced by (smaller or absent) SH-4 unwind data |
| `.symtab` + `.strtab` + `.shstrtab` | 3,047,933 | gone (stripped for 1ST_READ.BIN) |
| `.bss` | **18,255,512** | the actual problem — see §3 |

The headline "10.9 MB ELF" is misleading: 3.05 MB of it is the unstripped
symbol table and 0.82 MB is PIE relocations. The DC-relevant loadable content
is **6.75 MB initialized + 17.4 MB BSS**.

### Verdict on "`src/data/` generated tables dominate" — **partly refuted** [M]

| Bucket | .text | .rodata | .data | .bss | TOTAL |
|---|---:|---:|---:|---:|---:|
| `src/data/` (generated tables) | 80,044 | 0 | 2,250,589 | 8,519,191 | **10,849,824** |
| `pc/` platform layer | 121,512 | 1,615 | 350,408 | 5,327,075 | 5,800,610 |
| `src/` game logic (non-data) | 2,700,602 | 8,155 | 356,769 | 2,433,867 | 5,499,393 |
| `src/static/jaudio_NES` | 213,246 | 3,291 | 29,664 | 1,265,101 | 1,511,302 |
| `src/static/libforest` (emu64) | 31,168 | 360 | 1,080 | 562,453 | 595,061 |
| `src/static/` other | 32,332 | 0 | 7,048 | 113,486 | 152,866 |
| `src/static/JSystem` | 103,632 | 649 | 2,602 | 18,254 | 125,137 |
| `lib/glad` | 45,340 | 0 | 0 | 4,928 | 50,268 |
| **TOTAL** | **3,327,876** | **14,070** | **2,998,160** | **18,244,355** | **24,584,461** |

`src/data/` *is* the single largest bucket (10.85 MB, 44 %) — but **only 2.25 MB
of it is compiled-in `.data`. The other 8.52 MB is `.bss`**, and that BSS is
not "generated tables compiled in" at all: `gen_runtime_assets.py` has
*already* converted those arrays into empty `#ifdef TARGET_PC` placeholders
that `pc_assets.c` fills from the DOL/REL at boot. Example
(`src/data/model/lat_letter64_xk_tex.c:8`):

```c
#ifdef TARGET_PC
u16 lat_letter01_pal[0x20 / sizeof(u16)] ATTRIBUTE_ALIGN(32);
#else
u16 lat_letter01_pal[] ATTRIBUTE_ALIGN(32) = { #include "assets/lat_letter01_pal.inc" };
#endif
```

So the PLAN's proposed fix ("extend `gen_runtime_assets.py` to evict the
biggest tables") has largely already happened; what it produced is *static
BSS reservations loaded eagerly*, which on a 16 MB machine is no better than
compiling them in. The remaining lever is different and bigger: make those
reservations **dynamic and demand-paged**.

**Eagerly-loaded asset payload, exact** [M]:

| Path | Assets | Bytes |
|---|---:|---:|
| `s_assets[]` central table (`pc_assets.c:14595`) | 14,495 | 6,531,534 |
| per-file `_pc_load_src_*()` loaders in 768 `src/**.c` files | 1,848 | 2,239,824 |
| **Total loaded at boot into BSS** | **16,343** | **8,771,358** |

Where it lives (central table, attributed via symbol→object map) [M]:

| Owning tree | Bytes | % |
|---|---:|---:|
| `src/data/model/*` (objects, furniture, buildings, player, UI windows) | 5,264,400 | 80.6 |
| `src/data/npc/model/*` (villager models) | 1,154,944 | 17.7 |
| `src/data/item`, `submenu/map`, `font`, misc | 112,190 | 1.7 |

Spread over **2,080 object files** — a natural residency granularity of
~2.5 KB median per group. `rom_src` is 0 (REL) for 6,531,440 of the
6,531,534 B; only 94 B comes from the DOL.

### Top 40 symbols, `.bss` [M] (asset-staging symbols excluded)

`.bss` splits cleanly: **8,771,358 B staged assets** / **9,472,997 B true
runtime state**. The true-runtime side:

| Bytes | Symbol | Object | DC fate |
|---:|---|---|---|
| 3,152,684 | `g_gx` | `pc/src/pc_gx.c` | shrink (65536 × 48 B vertex buffer) |
| 1,228,800 | `prbuf` | `src/game/m_play.c:54` | → PVR RTT in VRAM (EFB capture, `(2*320)*(2*240)*4`) |
| 786,432 | `g_loaded_cache` | `pc/src/pc_texture_pack.c` | **dies** (texture packs are a non-goal) |
| 589,824 | `audiomemory` | `jaudio_NES/game/game64.c_inc:587` (`0x90000`) | shrink |
| 524,288 | `texture_buffer_data` | `libforest/emu64/emu64.c:41` | shrink (decode straight to VRAM) |
| 294,912 | `aSTR_overlay` | `src/actor/ac_structure.c:25` (32 × 0x2400) | keep / reduce count |
| 275,456 | `seq` | `jaudio_NES/internal/seqsetup.c` | keep |
| 270,336 | `g_fst_files` | `pc/src/pc_disc.c` | **dies** (ISO parsing moves host-side) |
| 196,608 | `quad_index_buf` | `pc/src/pc_gx.c:4` | **dies** (GL element buffer) |
| 187,392 | `common_data` | `src/game/m_common_data.c:8` | keep (live save state) |
| 165,888 | `s_cache` | `pc/src/pc_gx_tev.c:186` | **dies** (GLSL shader cache) |
| 155,648 | `l_keepSave` | `pc/src/pc_m_card.c` | shrink |
| 133,120 | `dvd_entry_table` | `pc/src/pc_dvd.c` | shrink |
| 132,112 | `sys_dynamic` | `src/system/sys_dynamic.c:3` | keep (GX display-list build buffers) |
| 98,304 | `tex_cache` | `pc/src/pc_gx_texture.c:96` (2048 entries) | keep |
| 98,304 | `ov_verts` | `pc/src/pc_overlay.c` | **dies** / 1/8 size |
| 86,596 | `aBTD_island_prg` | `src/actor/...` | keep (island overlay buffer) |
| 81,920 | `CHANNEL` | `jaudio_NES/internal/driverinterface.c` | keep |
| 65,536 | `ring_buffer` | `pc/src/pc_audio.c` | shrink (SDL ring → `snd_stream`) |
| 65,536 | `dvd_buf.3` | `jaudio_NES/internal/dvdthread.c` | keep |
| 65,536 | `dmabuffer` | `jaudio_NES/internal/heapctrl.c` | keep |
| 52,384 | `l_keepOriginal` | `pc/src/pc_m_card.c` | shrink |
| 51,260 | `aBTD_island_ldr` | | keep |
| 47,780 | `l_keepMail` | `pc/src/pc_m_card.c` | shrink |
| 47,618 | `l_keepDiary` | `pc/src/pc_m_card.c` | shrink |
| 44,224 | `aNNW_client_prg` | `src/actor/npc/ac_npc_needlework.c` | keep |
| 39,168 | `nintendo_hi_0` | `src/static/nintendo_hi_0.c` | evict to disc (boot logo) |
| 37,792 | `AG` | `jaudio_NES/internal/audiowork.c` | keep |
| 36,888 | `aNPC_k_overlay` | `src/actor/npc/ac_npc.c` | keep |
| 36,128 | `aNNW_client_ldr` | | keep |
| 32,768 | `CALLSTACK` | `jaudio_NES/internal/dvdthread.c` | shrink |
| 30,720 | `aGYO_overlay` | `src/actor/ac_gyoei.c` | keep |
| 25,600 | `pc_task_buf` | `jaudio_NES/internal/neosthread.c` | keep |
| 24,576 | `CH_BUF` | `jaudio_NES/internal/dspinterface.c` | keep |
| 24,576 | `FONT_nes_tex_font1` | `src/data/font` | evict (NES emu is a non-goal) |
| 23,424 | `aSTR_actor_cl` | `src/actor/ac_structure.c` | keep |
| 22,680 | `aNPC_n_actor_cl_tbl` | `src/actor/npc/ac_npc.c` | keep |
| 21,528 | `aINS_overlay` | `src/actor/ac_insect.c` | keep |
| 20,496 | `aNPC_e_overlay` | `src/actor/npc/ac_npc.c` | keep |
| 16,736 | `JUTResFONT_Ascfont_fix12` | `JSystem/JFramework/JFWSystem.cpp:24` | keep |

Tail below these 40: ≈ 1.35 MB across ~4,000 symbols.

### Top contributors, `.text` [M] — no big single cut exists

Long tail: the top 40 objects are only 45.6 % of `.text`.

| Bytes | cum% | Object |
|---:|---:|---|
| 178,480 | 5.4 | `src/game/m_player.c` |
| 131,796 | 9.3 | `src/actor/ac_museum_fish.c` |
| 80,600 | 11.7 | `src/f_furniture.c` |
| 76,904 | 14.1 | `src/actor/ac_my_room.c` |
| 69,180 | 16.1 | `src/actor/ac_museum_insect.c` |
| 64,352 | 18.1 | `src/game/m_collision_bg.c` |
| 62,398 | 19.9 | `src/game/m_tag_ovl.c` |
| 54,712 | 21.6 | `src/actor/npc/ac_npc.c` |
| 47,600 | 23.0 | `src/game/m_npc.c` |
| 45,340 | 24.4 | `lib/glad/src/gles2.c` (**dies**) |
| 41,312 | 25.6 | `src/actor/npc/ac_npc2.c` |
| 38,896 | 26.8 | `jaudio_NES/game/game64.c` |
| 32,064 | 27.8 | `src/actor/ac_event_manager.c` |
| 30,284 | 28.7 | `libforest/emu64/emu64.c` |
| 29,908 / 29,272 / 29,116 / 29,080 | 32.2 | `src/bg_item/bg_{xmas,winter,cherry,}_item.c` |
| 27,926 | 33.0 | `src/game/m_design_ovl.c` |
| 26,384 | 33.8 | `src/game/m_field_info.c` |
| 26,272 | 34.6 | `pc/src/pc_gx.c` |
| 26,012 | 35.4 | `src/game/m_msg.c` |
| 22,840 ×2 | 36.8 | `m_camera2.c`, `m_all_grow_ovl.c` |
| 21,356 / 21,156 | 38.0 | `m_player_lib.c`, `m_event.c` |
| 19,716 / 19,572 / 19,424 / 19,288 | 40.4 | `ac_npc_{super,depart,conv,shop}_master.c` |
| 18,832 / 18,532 / 18,352 / 18,024 | 42.6 | `m_kankyo.c`, `jammain_2.c`, `jaudio system.c`, `emusound.c` |
| 17,616 / 17,272 / 16,980 / 16,770 / 16,536 / 15,772 | 45.6 | `m_shop.c`, `pc_gx_texture.c`, `ac_npc_mamedanuki.c`, `ac_npc_needlework.c`, `m_bgm.c`, `jaudio track.c` |

Content-cut candidates, measured [M]:

| Feature | `.text` | % |
|---|---:|---:|
| npc actors (`src/actor/npc/*`) | 444,544 | 13.4 |
| jaudio_NES engine | 213,246 | 6.4 |
| museum exhibits (fish/insect/fossil) | 212,772 | 6.4 |
| effects (`src/effect/*`) | 158,166 | 4.8 |
| JSystem | 103,632 | 3.1 |
| GL/SDL-only (`glad`, `pc_gx_tev`, `pc_texture_pack`, `pc_model_viewer`) | 64,964 | 2.0 |
| emu64 | 31,168 | 0.9 |
| island / boat | 29,764 | 0.9 |
| famicom/NES glue | 24,838 | 0.7 |

Free deletions total only ~90 KB. **There is no `.text` diet that matters —
the only real lever is codegen** (game code is currently `-O0`; DC will be
`-O2`/`-Os` on 16-bit SH-4 instructions).

### `.data`: how much can be evicted [M]

Using the 100,993 dynamic relocations that land inside `.data`/`.data.rel.ro`
as an exact pointer map:

| Class | Symbols | Bytes |
|---|---:|---:|
| Contains ≥1 pointer (display lists, tables of `Gfx*`/`Vtx*`) | 14,551 | 2,049,704 |
| Pointer-free (keyframe tables, LUTs, price tables, ADPCM headers) | 8,152 | **948,688** |

Pointer-free bytes can be moved to disc with the *existing*
`gen_runtime_assets.py` mechanism (no new machinery). The 2.05 MB
pointer-bearing set needs a load-time fix-up pass to evict — only 403,972 B
of it is actual pointer content, so a REL-style relocation table would let
~1.6 MB more go to disc. That is phase 2.

`s_assets[]` itself is 347,880 B of `.data.rel.ro` [M] — 14,495 × 24 B
records with a string pointer each. On DC it becomes a disc-side index.

---

## 3. What is resident at boot vs on demand (task 3 answer)

Boot order [M] (`pc/src/pc_main.c:519` → `:611` → `:614`):

1. `pc_platform_init()` → `pc_gx_init()` (`pc_main.c:308`): touches `g_gx`
   (3.15 MB) and builds `quad_index_buf` (196 KB).
2. `pc_check_disc_or_die()` → `pc_disc_init()` (`pc_main.c:475`): fills
   `g_fst_files` (270 KB).
3. `pc_assets_init()` (`pc_main.c:611`): extracts + Yaz0-decompresses the REL
   (**15.64 MB live**) and the DOL (0.92 MB), then **memcpy+byteswap all
   16,343 assets (8.77 MB) into BSS in one loop**, then frees the two blobs.
4. `ac_entry()`; `boot_main()` → `OSInit()` (24 MB arena, memset) →
   `JFWSystem::firstInit()` (`JFWSystem.cpp:115`) → `JKRExpHeap::createRoot` +
   `JKRExpHeap::create(sysHeapSize)` → `JFWSystem::init()` →
   `JKRAram::create(0x810000, 0x6A3780)` (`jsyswrap.cpp:499-500`; `pc_aram.c`
   mallocs and memsets 16 MB) → `JUTCreateFifo(0x10001)` (65,568 B from the
   system heap, `JUTGraphFifo.cpp:9`) → `JW_Init2` mounts `forest_1st.arc` and
   hands **all remaining system-heap free space** to `MallocInit`
   (`jsyswrap.cpp:547-549`) → `JW_Init3` mounts `forest_2nd.arc`.

**Nothing in the asset path is demand-loaded.** The only on-demand mechanism
that exists is `JKRAramArchive` → `ARStartDMA` (a `memcpy` in
`pc/src/pc_aram.c`), which is the one seam PLAN §3.1 correctly identifies.

Disc content, measured from the user's ISO [M]:

| File | Bytes | Note |
|---|---:|---|
| `audiorom.img` | 8,300,384 | → AICA sound RAM + disc stream, **0 main RAM** |
| `foresta.rel.szs` | 6,137,393 | Yaz0 → **15,640,056** decompressed |
| `foresta.map` | 4,849,144 | debug map, not shipped |
| `forest_2nd.arc` | 4,132,608 | RARC, uncompressed |
| `famicom.arc` | 1,699,904 | RARC — NES emu is a non-goal, drop |
| `main.dol` | 918,720 | text 681,440 + data 237,024, bss 1,272,157 |
| `forest_1st.arc` | 852,896 | RARC, uncompressed |
| others (`static.map`, `opening.bnr`, …) | 559,450 | |
| **total real content** | **26,531,779** (25.30 MiB) | the 1.46 GB image is padding |

Note `forest_1st.arc + forest_2nd.arc = 4,985,504 B` of *actual* graph-ARAM
content against a 6,961,536 B reservation — the reservation is 40 % slack.

---

## 4. The 16 MB ledger (task 4 answer)

Budget = 16,777,216 B. Every bucket below names the code change that hits its
target. **Sum 15,026,608 B (89.6 %), margin 1,750,608 B (10.4 %).**

| # | Bucket | Today [M] | DC target | Δ | Code change required |
|---:|---|---:|---:|---:|---|
| 1 | KOS kernel + newlib + drivers (text/data/bss/kernel structs) | n/a | 1,000,000 [?] | — | none — but **measure at M0**: a KOS hello-world with GLdc linked, `(uintptr_t)_arch_mem_top - (uintptr_t)&end` after `pvr_init`. If it lands >1.5 MB, take it from bucket 6. |
| 2 | Reserved low RAM `0x8C000000–0x8C010000` | n/a | 65,536 | — | fixed by hardware/KOS. Not spendable. |
| 3 | Game+platform `.text` | 3,327,876 | 2,600,000 [?] | −727,876 | **C1**: `-O2` for hot TUs, `-Os` for cold ones (game code is `-O0` today). **C2**: drop `glad`/`pc_gx_tev`/`pc_texture_pack`/`pc_model_viewer`/famicom glue (−90 KB measured). SH-4 vs ARM code-size ratio is **unknown** — resolve at M1, first link. |
| 4 | Game+platform `.rodata` + `.data` | 3,012,230 | 1,800,000 | −1,212,230 | **C3**: run `gen_runtime_assets.py` over the 948,688 B of measured pointer-free `.data` symbols (mechanism already exists, just not applied to non-`.inc` tables). **C4**: replace `s_assets[]` (347,880 B) with a disc-side index + ≤64 KB in-RAM hash. |
| 5 | Game+platform `.bss` (true runtime state) | 9,472,997 | 1,950,000 | −7,522,997 | itemised in §4.1 |
| 6 | JKRHeap system heap + `__osMalloc` game arena | 25,153,072 reserved, **usage unmeasured** | 4,000,000 [?] | — | **C5**: set `JFWSystem::CSetUpParam::sysHeapSize` from measured peak (§6) instead of `arena_hi - arena_lo`, and stop `jsyswrap.cpp:547` handing the whole remainder to `MallocInit` — give it a fixed, ledgered extent. **This is the single biggest unknown in the budget.** |
| 7 | Asset residency pool (was 8.77 MB of static BSS) | 8,771,358 | 1,500,000 | −7,271,358 | **C6**: convert the 2,080 asset-owning objects into ~2,080 disc-resident groups with a generated ID table; `gen_runtime_assets.py` emits pointer-indirection stubs instead of arrays; runtime LRU pool keyed by group ID, warmed per acre/room load. Kills the 15.64 MB REL transient at the same time (assets are read individually from a packed disc file). |
| 8 | Graph-ARAM (RARC) LRU window | 6,961,536 reserved / 4,985,504 real | 512,000 [?] | −6,449,536 | **C7**: `dc_aram.c` replaces `ARStartDMA`'s memcpy with a paged disc read against `forest_{1st,2nd}.arc` laid out on outer tracks. Window size is a guess — **measure at M1** (§6, probe 3). |
| 9 | Audio work RAM, SH-4 side (`audiomemory`, `seq`, `CHANNEL`, `dmabuffer`, `dvd_buf`, `CALLSTACK`, `AG`, `CH_BUF`, `pc_task_buf`, `ring_buffer`, `SoundQbuf`, `sound_loop_buffer`) | 1,297,312 | 700,000 | −597,312 | **C8**: `audiomemory` `0x90000`→`0x40000`; `snd_stream` double-buffer replaces the 65,536 B SDL ring; sequence data only in main RAM — samples live in AICA's separate 2 MB. Sound ARAM (8,458,240 B) → **0 main RAM**. |
| 10 | Disc read-ahead ring + KOS VFS buffers | ~470,000 (`dvd_entry_table` + `dvd_buf`) | 384,000 | — | **C9**: read-ahead thread with a 3 × 128 KB ring (CD-R ≈ 500 KB/s ⇒ ~0.75 s of lookahead). Sized to hide the base port's known 8.7 s stall class. |
| 11 | PVR vertex staging / TA buffers in main RAM | n/a | 384,000 | — | **C10**: stage-A GLdc holds a vertex array in main RAM; stage B submits via store queues with only a small block buffer. Framebuffers and textures cost **0 main RAM** (8 MB VRAM is a separate address space). |
| 12 | Thread stacks (main 64 K, audio 32 K, read-ahead 16 K, KOS idle/misc) | n/a | 131,072 | — | **C11**: explicit `kthread` stack sizes, no defaults. |
| | **TOTAL** | | **15,026,608** | | **margin 1,750,608 B / 10.4 %** |

Separate address spaces, budgeted elsewhere (not part of the 16 MB):
**8 MB VRAM** (framebuffers + all textures, twiddled/VQ/paletted) and
**2 MB AICA sound RAM** (8,300,384 B of `audiorom.img` does *not* fit — needs
offline bank splitting + streaming; that is PLAN §3.4's problem, and it needs
its own ledger).

### 4.1 Bucket 5 derivation — measured, not guessed

Starting from the measured 9,472,997 B of true-runtime BSS:

| Item | Bytes | Action |
|---|---:|---|
| `g_loaded_cache` + `g_neg_cache` (texture packs) | −802,816 | delete (non-goal) |
| `g_fst_files` (ISO FST) | −270,336 | delete (host-side tool) |
| `quad_index_buf` (GL) | −196,608 | delete |
| `s_cache` (GLSL shader cache) | −165,888 | delete |
| `ov_verts` (SDL overlay), net of a 12,288 B DC overlay | −86,016 | shrink |
| `glad` BSS | −4,928 | delete |
| `g_gx` 65536 × 48 B → 8192 × 32 B + state ≈ 300,000 | −2,852,684 | shrink (`PC_GX_MAX_VERTS`, `PCGXVertex`) |
| `prbuf` EFB capture → PVR render-to-texture in VRAM | −1,228,800 | move to VRAM (fallback: 320×240×2 = 153,600 in RAM) |
| `texture_buffer_data` (emu64 `0x80000` → `0x20000`, decode to VRAM) | −393,216 | shrink |
| `dvd_entry_table` 133,120 → 16,384 | −116,736 | shrink |
| `pc_m_card` keep-buffers 303,430 → 160,000 (compressed VMU staging) | −143,430 | shrink |
| misc `pc_disc`/`pc_dvd` residue | −30,000 | delete |
| audio BSS reclassified into bucket 9 | −1,297,312 | move |
| **remaining** | **1,871,939** | |

Target 1,950,000 B leaves 78 KB of slack inside the bucket. Everything left
is genuinely live state: `common_data` (187,392), `sys_dynamic` (132,112),
`aSTR_overlay` (294,912), the `a*_overlay`/`a*_prg`/`a*_ldr` actor overlay
buffers, `tex_cache` (98,304), `JUTResFONT_Ascfont_fix12` (16,736), and a
~1.35 MB tail of ~4,000 small symbols.

### 4.2 Verdict

**16 MB is reachable, with ~10 % margin, but only if bucket 6 lands at or
under 4 MB — and that number is currently a guess.** Everything else in the
ledger is either measured or bounded by a mechanical code change.

**The single biggest lever is C6, the asset residency pool: 7.27 MB.** It is
also the change that removes the 15.64 MB boot transient, so it is both the
largest and the most urgent. Second is the arena (C5, unknown but nominally
21 MB of reservation), third is graph-ARAM windowing (C7, 6.45 MB).

Risks to the margin, honestly stated:

- Bucket 3 (`.text` 2.6 MB) is a **[?]**. If SH-4 `-O2` output exceeds ARM
  `-O0` output, the whole 1.75 MB margin can evaporate on this one line.
  It is the first thing M1 must report.
- Bucket 1 (KOS 1.0 MB) is quoted from `kb/research-dreamcast.md`'s "~2–4 MB
  practical", which we have **not verified** and which we believe is
  pessimistic because it likely folds in GLdc's own buffers (our bucket 11).
  If KOS really costs 2.5 MB, the budget is at 16.5 MB and something must go.
- Fragmentation is not modelled. Bucket 6 is a first-fit expheap plus a
  second-level `__osMalloc`; long-session fragmentation could force a larger
  reservation than the measured peak. Add ≥15 % on top of the measured peak
  when setting `sysHeapSize`.
- The "DC edition" content cuts (famicom.arc 1.7 MB, museum exhibits, island)
  buy `.text` and disc space but almost no RAM — do not count on them.

---

## 5. `dc/src/dc_mem_ledger.c` — design (task 5; **do not implement here**,
`dc/src/` is another agent's)

Purpose: make the table in §4 a runtime object, so the budget is *enforced*
rather than hoped, and an overrun is a loud, greppable failure rather than a
mysterious crash three scenes later.

### Files

- `dc/include/dc_mem_budget.h` — the §4 table as `#define DC_BUDGET_<BUCKET>`
  constants, plus `DC_RAM_BUDGET_BYTES (16*1024*1024)`. Generated from this kb
  file by a small host script so doc and code cannot drift.
- `dc/include/dc_mem_ledger.h` — API.
- `dc/src/dc_mem_ledger.c` — implementation.

### API sketch

```c
typedef enum {
    DCMEM_KOS, DCMEM_IMAGE_TEXT, DCMEM_IMAGE_DATA, DCMEM_IMAGE_BSS,
    DCMEM_JKRHEAP, DCMEM_ASSET_POOL, DCMEM_ARAM_GRAPH, DCMEM_AUDIO,
    DCMEM_DISC_IO, DCMEM_PVR_STAGING, DCMEM_STACKS, DCMEM_NBUCKET
} dc_mem_bucket_t;

void  dc_mem_ledger_init(void);                     /* before any big alloc */
void* dc_mem_alloc(dc_mem_bucket_t, size_t, size_t align, const char* owner);
void  dc_mem_free (dc_mem_bucket_t, void*);
void  dc_mem_note (dc_mem_bucket_t, ptrdiff_t);     /* allocators we don't own */
void  dc_mem_frame(void);                           /* per-frame poll, O(1) */
void  dc_mem_report(int verbose);
```

### Boot-time behaviour

1. **Measure the image, don't trust a header.** Use the linker symbols
   (`_executable_start`, `_etext`, `__data_start`, `_edata`, `__bss_start`,
   `end`) to fill `DCMEM_IMAGE_*` exactly. This catches "someone added a
   200 KB array to BSS" on the boot after it happens, not at M5.
2. **Refuse the RAM mod.** Read `_arch_mem_top`. If it is `0x8E000000`
   (32 MB), print `MEMLEDGER WARN 32MB-detected budget-still-16MB` and clamp
   the arena to 16 MB unless built with `DC_ALLOW_32MB`. This is the
   mechanical guard that stops the stock-16 MB line eroding by accident
   (PLAN §3.1 / CLAUDE.md hardware contract).
3. **Carve one contiguous arena** from `&end` to the clamped top, then split
   it into fixed, non-overlapping extents per bucket from
   `dc_mem_budget.h`. A bucket physically cannot overrun into another — an
   overrun is a `NULL` from that bucket's sub-allocator, attributed by name.
4. `_Static_assert(sum(DC_BUDGET_*) <= DC_RAM_BUDGET_BYTES)` — over-commit is
   a **compile error**, which is the cheapest possible place to catch it.
5. Print the ledger, one machine-parseable line per bucket:
   `MEMLEDGER <bucket> budget=<B> reserved=<B> used=<B> peak=<B>` and a final
   `MEMLEDGER TOTAL used=<B> budget=16777216 margin=<B>`. `harness/dc/smoke.sh`
   greps these; the M2 gate ("RAM ledger ≤ 16 MB true") becomes a grep instead
   of a judgement call.

### Runtime behaviour

- Per-bucket `cur` / `peak` / `peak_tag` / `largest_free`, updated on every
  alloc/free (counters only — no free-list walking on the frame path).
- `dc_mem_tag(const char*)` from scene transitions (town / house / shop /
  museum / island / event) so peaks are attributable to a place.
- **Third-party allocations get captured**: link with
  `-Wl,--wrap=malloc,--wrap=free,--wrap=memalign` so KOS's and GLdc's
  allocations land in `DCMEM_KOS` / `DCMEM_PVR_STAGING` rather than
  disappearing. Without this the ledger lies.
- Overrun policy:
  - `DC_MEM_STRICT` (default for dev/Flycast builds): print
    `MEMLEDGER FAIL bucket=<n> owner=<s> want=<B> free=<B> ra=<addr>`,
    dump the whole table, then `arch_exit()`. Loud, immediate, greppable.
  - Release builds: log, return `NULL`, let the caller degrade — the asset
    pool evicts LRU and retries once; anything else treats `NULL` as fatal.
- Fragmentation watch: warn when a bucket's `largest_free < 25 %` of its free
  bytes. This is how we'll catch the `__osMalloc` fragmentation risk noted in
  §4.2 before it becomes a hang.
- `dc_mem_frame()` also feeds the VMU LCD / debug overlay (PLAN §7 already
  wants a VMU display) — total used as a bar is free instrumentation.

### Hooks other agents must add (not in this file)

`dc/src/dc_os.c` (arena carve + `OSGetArenaLo/Hi`), `dc/src/dc_main.c`
(`dc_mem_ledger_init` first, `dc_mem_report` after boot and on exit),
`dc/src/dc_aram.c` (graph window `dc_mem_note`), the asset pool, the
read-ahead thread, and the `JKRHeap` shim.

---

## 6. Patch plan: instrument the **existing PC build** for real high-water
marks (task 6)

Goal: replace bucket 6's `[?]` with a measured number, and size the bucket 8
window from data. All of this runs on the armhf/desktop build where it is
cheap. Nothing here is Dreamcast-specific.

### New module

`pc/src/pc_memprobe.c` + `pc/include/pc_memprobe.h`, compiled only when
`-DPC_MEMPROBE=1` (add the option to `pc/CMakeLists.txt`; keep it off by
default so the shipping build is unchanged — same kill-switch discipline as
the `PC_NO_*` env vars).

State: a fixed array of probes, each `{name, cur, peak, peak_tag, peak_frame}`.
Two entry points: `pc_memprobe_sample(void)` (cheap, per frame) and
`pc_memprobe_tag(const char*)` (scene label).

### Probe 1 — game malloc arena (the number that sets bucket 6)

`src/static/libc64/malloc.c` already exports exactly what we need:

```c
extern void GetFreeArena(size_t* max, size_t* free, size_t* alloc);  /* TARGET_PC variant, line 19 */
```

`alloc` is used bytes; `max` is the largest free block (fragmentation).
No game-code change needed to *read* it.

For an exact high-water (not a per-frame sample, which can miss transient
spikes during a scene load), add to `src/static/libc64/__osMalloc.c`, guarded
by `#ifdef PC_MEMPROBE`:

- in `__osMalloc` (line 265) and `__osMallocR` (line 269): after a successful
  block split, `g_probe_arena_cur += size; if (cur > peak) peak = cur, record tag`.
- in `__osFree` (line 381): `g_probe_arena_cur -= size`.

This is ~8 lines in two functions and is the highest-value patch in this
document.

### Probe 2 — JKRHeap system heap

`JKRHeap::getTotalFreeSize()` / `getFreeSize()` on
`JFWSystem::getSystemHeap()` and on the root heap; used = heap size − total
free. Sample per frame via the probe module. For exact peaks, add the same
`#ifdef PC_MEMPROBE` counter pair to `JKRExpHeap::do_alloc` / `do_free`
(`src/static/JSystem/JKernel/JKRExpHeap.cpp`).

Also record `getFreeSize()` (largest contiguous) separately — the gap between
"total free" and "largest free" *is* the fragmentation number that §4.2 warns
about, and it is what decides the safety margin on top of the measured peak.

### Probe 3 — graph-ARAM working set (sets bucket 8)

The highest-value measurement after probe 1. In `pc/src/pc_aram.c`, inside
`ARStartDMA` (the memcpy seam), record every (aram_addr, length) into a
bitmap at 2 KB page granularity. Maintain rolling unique-page counts over
1 s / 5 s / 30 s windows plus an all-time max. Dump at exit.

Output: "the largest 5-second graph-ARAM working set over a full playthrough
was N KB" → bucket 8 = N + 50 %. Today's 512,000 B is a placeholder.

Do the same for the sound half to confirm the AICA-side budget.

### Probe 4 — renderer and asset numbers (cheap, confirms §4.1)

- `g_gx.vertex_count` peak per frame → validates shrinking
  `PC_GX_MAX_VERTS` from 65536 to 8192. If the real peak is >8192, bucket 5's
  arithmetic changes and we need to know now.
- `tex_cache_count` (`pc/src/pc_gx_texture.c:96`) peak and total decoded
  bytes → sizes the VRAM texture budget.
- Number of *distinct* asset groups touched per acre/room → sizes bucket 7
  (the 1.5 MB asset pool). Instrument by giving each of the 2,080 asset
  objects an ID in `gen_runtime_assets.py` and logging first-touch. This is
  the measurement that decides whether C6 is 1.5 MB or 3 MB.

### Hook points

| File | Where | Call |
|---|---|---|
| `src/static/jsyswrap.cpp` | end of `JW_Init2` (after `MallocInit`, line 549) | `pc_memprobe_init()` — captures heap base/size |
| `pc/src/pc_vi.c` | `VIWaitForRetrace` | `pc_memprobe_sample()` |
| `src/game/m_play.c` / `src/game/m_field_info.c` | scene/acre transition | `pc_memprobe_tag("town:acre_N")` etc. |
| `pc/src/pc_main.c` | after `boot_main` returns, and in the crash handler | `pc_memprobe_report()` |

Put the report in the crash handler too — the peak that matters is often the
one immediately before an OOM.

### Output and reduction

- `mem_profile.csv`, one row per sampled frame:
  `frame,tag,sysheap_used,sysheap_peak,sysheap_largest_free,arena_used,arena_peak,arena_largest_free,aram_sound_used,aram_graph_ws_1s,aram_graph_ws_5s,gx_verts,texcache_bytes,texcache_count`
- `pc/tools/mem_report.py` (new, host-side) reduces it to a per-tag peak table
  and prints the proposed `sysHeapSize`/`gameheap` values directly, so the
  output of the measurement is the patch.
- Optional `PC_MEMPROBE_SITES=1`: record `__builtin_return_address(0)` with
  each `__osMalloc`, dump a top-50 allocation-site histogram, resolve with
  `addr2line`. This is what tells us *which* subsystem to attack if the peak
  comes in over 4 MB.

### Driving it

`harness/smoke.sh armhf` already boots with the real ROM. Add
`PC_MEMPROBE=1` to a new `harness/mem_profile.sh` that runs the existing
playthrough script (kb/build-test.md) end to end and then runs
`mem_report.py`. Worst case must come from a **late-game save with a full
house and a full town**, not a fresh boot — that is the number bucket 6 has
to survive.

---

## 7. Open measurements (ordered by how much of the budget they decide)

| # | Question | Decides | Milestone |
|---|---|---|---|
| 1 | Real `__osMalloc` + `JKRExpHeap` high-water over a full playthrough | bucket 6 (4.0 MB, 24 % of RAM) | M1, §6 probe 1+2 |
| 2 | SH-4 `-O2`/`-Os` code size vs ARM `-O0` | bucket 3 (2.6 MB, 15 %) | M1, first link |
| 3 | Graph-ARAM 5 s working set | bucket 8 (512 KB) | M1, §6 probe 3 |
| 4 | Distinct asset groups live per acre/room | bucket 7 (1.5 MB) | M1, §6 probe 4 |
| 5 | KOS + GLdc real overhead on a hello-world | bucket 1 (1.0 MB) | M0 |
| 6 | Peak `g_gx.vertex_count` in a town frame | bucket 5 (1.95 MB) | M1, §6 probe 4 |
| 7 | `__osMalloc` fragmentation over a long session | the margin on bucket 6 | M3 |

Until #1 and #2 report, treat the 10.4 % margin as provisional.

---

## 8. M1 post-link measurement — the real numbers from a real Dreamcast ELF

**Measured 2026-08-01 on `dc/build/AnimalCrossing.elf`**, the first ELF this
project has ever linked for SH-4. Everything in §§1–7 above was measured on the
*armhf* build and extrapolated; this section supersedes those extrapolations
wherever they disagree. All figures **[M]** unless marked.

Subject: `sh-elf-gcc 15.2.0`, KOS master, `DECOMP_OPT=-O0`, 3,917 TUs.
Tools: `sh-elf-size -A`, `sh-elf-nm --size-sort -S`, `sh-elf-readelf -lW`,
`sh-elf-objdump -h`, all inside `opencrossing-dc:sdk`.

### 8.1 Section sizes, before and after this pass

| | `.text` | `.data` | `.bss` | allocated total | image end |
|---|---:|---:|---:|---:|---|
| M1 first link | 6,318,568 | 2,638,852 | 13,526,548 | **22,486,548** | `0x8d581c14` |
| after §8.3 fixes | 6,318,568 | 2,638,852 | **12,415,508** | **21,374,996** | `0x8d472814` |

`_arch_mem_top` (stock 16 MB console) is `0x8d000000`. The image alone ends
**4,663,316 B past the top of RAM**, before a single byte of heap.

> **Correction to §4, bucket 6.** `DC_MAIN_MEMORY_SIZE` (4,000,000 B) is **not**
> in `.bss`. `dc/src/dc_os.c:400` obtains it with `dc_mem_alloc(DCMEM_JKRHEAP,…)`
> at runtime, so it sits on the KOS heap **on top of** the 21.37 MB image. Any
> ledger that counted the arena inside `.bss` understates the overage by 4 MB.

### 8.2 The `.bss` split that decides the next lever

`.bss` symbol-attributed total 13,603,920 B across 17,776 symbols (slightly
above the section figure — per-object sums include alignment padding).

Destination symbols were recovered from source, read-only: the 14,495-entry
`s_assets[]` table in `pc/src/pc_assets.c` plus 1,848 `pc_load_asset(...)` call
sites in the generated `_pc_load_src_*()` functions under `src/` — **15,704
unique destination symbols**.

| | bytes | symbols | share |
|---|---:|---:|---:|
| **asset-destination arrays** | **8,771,358** | 16,340 | **64.5 %** |
| everything else | 4,832,562 | 1,436 | 35.5 % |

**8,771,358 B matches §2's independently-derived "Total loaded at boot into
BSS" to the byte**, and matches the asset-pack agent's ~8,617,214 B from a
third direction. Triple-corroborated.

By owning tree:

| bucket | total | asset-dest | non-asset |
|---|---:|---:|---:|
| `src/data` | 8,519,191 | 8,518,368 | 823 |
| `src/game` | 1,548,236 | 2,448 | 1,545,788 |
| `src/static/jaudio_NES` | 1,265,101 | 0 | 1,265,101 |
| `src/actor` | 711,058 | 138,048 | 573,010 |
| `src/static/libforest` (emu64) | 562,374 | 0 | 562,374 |
| `dc/src` | 372,585 | 0 | 372,585 |
| `pc/src` (reused) | 320,158 | 0 | 320,158 |
| `src/other` | 173,485 | 1,280 | 172,205 |
| `src/static/other` | 113,466 | 111,214 | 2,252 |
| `src/static/JSystem` | 18,266 | 0 | 18,266 |

**Conclusion: the next lever is demand-loading into pooled storage, not arena
right-sizing.** Two thirds of `.bss` is asset destinations; no amount of
right-sizing the remaining third can close a 13 MB gap.

Top 10 **non**-asset-destination `.bss` symbols (the genuinely-classifiable
part), each tagged GC-sized / N64-era / required:

| bytes | symbol | object | class |
|---:|---|---|---|
| 1,228,800 → 614,400 | `prbuf` | `src/game/m_play.c` | GC-sized, halved in §8.3 |
| 589,824 | `audiomemory` | `jaudio_NES/game/game64.c` | GC-sized (→ AICA) |
| 524,288 → 49,152 | `texture_buffer_data` | `emu64/emu64.c` | PC-inflated, reverted in §8.3 |
| 334,088 | `g_gx` | `dc/src/dc_gx.c` | ours, already shrunk from PC's 3.15 MB |
| 294,912 | `aSTR_overlay` | `src/actor/ac_structure.c` | GC-sized (32 × 0x2400) |
| 275,456 | `seq` | `jaudio_NES/internal/seqsetup.c` | required (sequence data) |
| 187,328 | `common_data` | `src/game/m_common_data.c` | **required** (live save state) |
| 155,648 | `l_keepSave` | `pc/src/pc_m_card.c` | GC-sized (456 KB GC save) |
| 132,104 | `sys_dynamic` | `src/system/sys_dynamic.c` | required (GX DL build buffers) |
| 81,920 | `CHANNEL` | `jaudio_NES/internal/driverinterface.c` | required |

**There is no emulated-N64-RDRAM buffer.** The N64-origin lead was checked
directly against the linked image: the entire `src/static/libforest` (emu64)
tree is 562,374 B of `.bss`, the largest single item being
`texture_buffer_data`. No 4 MB or 8 MB machine-memory image exists anywhere in
the binary. This agrees with `kb/research-n64-origin.md`: emu64 is a GBI
display-list interpreter, not a machine emulator.

### 8.3 Levers applied this pass (measured individually)

Sum of the four = 1,111,040 B, and the measured `.bss` delta is exactly
1,111,040 B — nothing else moved.

| lever | Δ `.bss` | where | why it is safe |
|---|---:|---|---|
| `prbuf` `sizeof(u32)`→`sizeof(u16)` | −614,400 | `src/game/m_play.c:54`, `#ifdef TARGET_DC` | the only writer is `copy_efb_to_texture()` → `GXSetTexCopyDst(…, GX_TF_RGB565, 0)` = 2 B/px; the only reader binds `G_IM_SIZ_16b`. VERIFIED at both ends in this tree. The 2× is dead on GameCube too. |
| `TEX_BUFFER_DATA_SIZE` `0x80000`→`0xC000` | −475,136 | `include/libforest/emu64/texture_cache.h:71` | reverts a PC-port inflation to the **retail GameCube** value; sufficiency proven by the shipped product |
| `TEX_BUFFER_BSS_SIZE` `0x4000`→`0x400` | −15,360 | same file | same |
| `TEXTURE_CACHE_LIST_SIZE` `1024`→`256` | −6,144 | same file:16 | same |

All four are `#if defined(TARGET_DC)` branches added alongside the existing
`TARGET_PC` guards — the PC and GameCube paths are byte-for-byte untouched.

### 8.4 Levers measured and found to be dead ends

| lever | measured result |
|---|---|
| `-g0` / strip debug info | **0 B of RAM.** `sh-elf-readelf -SW` shows every `.debug_*`, `.symtab`, `.strtab` section has no `A` flag — they are not in any `PT_LOAD`. Saves ELF/disc bytes only. |
| drop NES/famicom emulation | **≈39,444 B `.bss` + 1,724 B `.text`.** `src/static/Famicom/` is already excluded by the build filters; what remains in-image is `famicom_emu.c` (20 B), `int_tak_nes01` (14,208 B) and six `FONT_nes_*` textures (25,216 B). The 1.70 MB `famicom.arc` is **disc** space, not RAM. Not worth the risk. |
| drop debug / symbolication machinery | **≈88 KB total** (37,938 `.bss` + 42,392 `.text` + 8,337 `.data`/`.rodata`), of which `dvderr.c` is 33,204 B of the `.bss`. Marginal. |
| `src/static/jaudio_NES` as a "non-goal" | **Not a non-goal.** Despite the name it is the *game audio engine* (1,265,101 `.bss` + 319,194 `.text`), not the NES emulator. Dropping it removes all sound. |

### 8.5 `--gc-sections` is mandatory, not an optimization

`DC_GC_SECTIONS=0` **does not link at all**. Without section GC the decomp has
genuinely undefined symbols whose referencing sections are unreachable —
`JKRTask::searchBlank()`, `vtable for JSUOutputStream`, `vtable for
JSURandomOutputStream`, `JSURandomOutputStream::getAvailable() const`,
`::skip(long)` — plus KOS's own `__kos_romdisk`. So "with vs without" cannot be
compared on two linked images; the saving has to be read off the discard list.

`-Wl,--print-gc-sections` reports **16,164 dropped sections**; summing their
sizes from each object's section headers gives **395,207 B**
(`.text` 87,744 · `.rodata` 49,043 · `.data` 6,062 · `.bss` 110,674 · other
141,684). `kb/research-size-reduction.md` reports 522,150 B over 29,471
sections from the link map, which also counts library objects this per-object
sweep did not reach — take the map figure as authoritative. Either way it is
already spent, and it is ~0.4–0.5 MB, not megabytes.

Codegen-identity note: `-ffunction-sections -fdata-sections` change only which
named section a function is emitted into, never instruction selection. This is
therefore **not** an optimization and is compatible with the `-O0` policy.

### 8.6 Optimization levels — measured, then REJECTED BY POLICY

Recorded once so the number is not mistaken for unknown. **User decision
2026-08-01: decomp game code stays at `-O0`.** Do not propose these.

| `DECOMP_OPT` | `.text` | `.data` | `.bss` | total |
|---|---:|---:|---:|---:|
| `-O0` (policy) | 6,318,568 | 2,638,852 | 13,526,548 | 22,483,968 |
| `-O1` | 3,697,460 | 2,525,704 | 13,526,164 | 19,749,328 |
| `-Os` | 3,477,188 | 2,525,704 | 13,526,132 | 19,529,024 |
| `-O2` | 3,825,312 | 2,525,672 | 13,526,132 | 19,877,116 |

The load-bearing observation is that **even `-Os` does not reach 16,777,216 B**,
let alone the 8,035,072 B image budget. Optimization was never sufficient on
its own, so the policy costs less than it appears to: the budget always had to
come out of `.bss`.

The one real consequence: §4's bucket 3 budgets `.text` at 2,600,000 B, which
is only reachable with optimization. At `-O0` it is 6,318,568 B — **3.7 MB over
bucket 3, against a ledger whose entire margin was 1.75 MB.** §4 no longer
closes as written, independently of the asset work.

### 8.7 Boot attempt — mechanism confirmed, no crash to symbolise

`harness/dc/smoke.sh dc/build/OpenCrossing.cdi --timeout 90` → **exit 1,
`timeout`, zero bytes of console output.** No KOS banner, no maple
enumeration, nothing.

Controls run to attribute it:

| image | `.bss` | image end | result |
|---|---:|---|---|
| `selftest.cdi` (harness reference) | 22,728 | `0x8c048948` | **PASS, 3.10 s** |
| hello-world + 4.7 MB `.bss` | 4,722,728 | `0x8c4c40a8` (under top) | **PASS, 3.08 s** |
| hello-world + 21 MB `.bss` | 21,022,728 | `0x8d44f888` (over top) | **FAIL, 0 bytes console** |
| `OpenCrossing.cdi` | 12,415,508 | `0x8d472814` (over top) | **FAIL, 0 bytes console** |

A stock KOS hello-world containing *nothing but* a large `.bss` array fails
identically once its image end crosses `_arch_mem_top`, at essentially the same
address as the game. So the game's silence is **fully explained by image size
alone** — it is not a game fault, not the `dc_main.c` exception trampoline, and
not something `-DDC_NO_CRASH_PROTECTION` can distinguish. The guest
never executes an instruction, so **there is no PC to symbolise**.

Mechanism: KOS has no MMU paging and `mm_sbrk()` starts at the ELF `end`
symbol, so `.bss` is committed RAM and the startup zeroing walks past the top
of physical memory before `scif_init()` ever runs.

**Nothing about the port's correctness can be tested until the image fits.**
Boot is gated on size, and only on size.

### 8.8 Where the gap stands

Against the 8,035,072 B image budget from `kb/research-size-reduction.md`
(which reserves the ledger's own 7.61 MB of heap plus ~1 MB for KOS):

| | bytes |
|---|---:|
| allocated image today | 21,374,996 |
| image budget | 8,035,072 |
| **still to shed** | **13,339,924** |

This pass removed 1,111,040 B of it. The remaining 13.34 MB cannot come from
codegen (policy), from stripping (0 B), from `--gc-sections` (already spent),
or from dropping the NES path (~40 KB). It has to come from the ranked plan in
`kb/research-size-reduction.md` — of which **8.45 MB is demand-loading the
`src/data` staged assets**, i.e. the asset pack's runtime loader. That is the
critical path, and it is the next milestone's work.
