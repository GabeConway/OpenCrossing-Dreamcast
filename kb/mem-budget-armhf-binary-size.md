# RAM budget — armhf-era binary size attribution (§2)

Section totals, per-tree bucket attribution, the top-40 `.bss` and `.text`
symbols with their DC fate, and how much `.data` is evictable.
**Measured on the armhf build, 2026-08-01** — measurement subjects and tools are
in `kb/mem-budget-armhf-working-set.md`. Read for per-symbol attribution; the
8,771,358 B asset-destination figure here was later corroborated to the byte
against the real DC link (`kb/mem-budget-m1-sh4.md` §8.2).

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
