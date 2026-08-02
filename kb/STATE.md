# Session state — resume here

Updated 2026-08-02. This file is **short on purpose**: only what is true *right
now*, plus what to do next. Everything else is one hop away.

| file | read it when |
|---|---|
| `kb/state-log.md` | you need the evidence — what was observed running, when, and what it cost |
| `kb/heap-two-pools.md` | **before touching `DC_ARENA_BYTES` / `DC_ARAM_WINDOW`** or anything that allocates at boot |
| `kb/plan-stages.md` | the agreed S1→S5 RAM plan and the reasoning behind each step |
| `kb/levers.md` | planning any size/RAM work — the ranked ledger of what's left |
| `kb/closed.md` | **before proposing** any RAM/size/architecture idea — what is already dead and why |
| `kb/traps.md` | before touching the build, harness, prelude, or instrumentation |

`CLAUDE.md` is the index to everything else.

## Where the port is

⚠️ **Updated 2026-08-02 (session 2). The port now REACHES THE TOWN.**
`[SCENE_MODE] 0 → 3 → 4 → 18 → 9`; mode 9 is `mFI_FIELD_FG` +
`mEv_CheckFirstIntro()` TRUE (`m_field_make.c:1292`) = SCENE_FG, the outdoor
field. Title → player-select → train intro with Rover → name-entry keyboard →
town, unattended, in one 600 s run. What unblocked it was **input**, not memory:
`DC_AUTOSTART` alternated START/A 1:1 and past the title almost nothing takes
START. Latest full run: **16,889 frames / 600 s**, town ~12 FPS,
`image_span` 10,699,616 B, margin 3,588,448 B, fit OK.

Human verdict on the current build: K.K. Slider correct, Rover correct in the
train, scrolling trees and glass present. **Still wrong: the train door, and the
scrolling window texture sits entirely above the window.** Both are tracked in
`kb/RESUME.md` §5, items 1-2. The door needs the PVR punch-through list and
cannot be fixed by toggling depth write — both settings were tried and both are
visibly broken, for opposite reasons.

**M0 and M1 are met. M2 has real pixels and is not complete.** The renderer is a
real PowerVR backend (`dc/src/dc_pvr.c` + `dc_pvr_texture.c`). Four renderer
bugs of one family — **GX state recorded by `dc_gx.c` and never consumed by
`dc_pvr.c`** — have now been found and fixed (wrap mode, TEV constants,
`GX_TEXMAP_NULL`, alpha compare, colour update). Sweeping every `g_gx` field for
a *consumer* is the standing technique; fog is the largest one still unread.

- **3917 / 3917 translation units compile and link for sh-elf**, zero
  exclusions. `src/` carries only **four** `#if defined(TARGET_DC)` branches;
  every compat fix lives in `dc/include/dc_prelude.h` as a force-include.
- **The harness works and is verified against real CDIs**, not asserted.
- **The full image still does not fit. That is the only thing between here and
  a playable build** — the renderer, the platform layer and the boot path are
  all observed working.

The build that renders (2026-08-02, session 2 — supersedes the older line):

```bash
DC_STUB_KEEP="$(grep -v '^#' tools/dcstub/keeplist-opening.txt | paste -sd: -)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=131072 DC_ARENA_BYTES=1900000 DC_AUTOSTART=300 \
  bash dc/build-dc.sh
bash harness/dc/smoke.sh dc/build/OpenCrossing.cdi --timeout 600 -c config:LimitFPS=no
```

**600 s and `LimitFPS=no`, not 180 s.** The town is ~4,000 frames in; a short
run stops in the train intro and reads as a progression regression. Build to a
*copy* of the CDI before a long run — Flycast holds the file open for the whole
run.

⚠️ A game smoke run **always** exits 1 with `status=exited_early` — the game
never returns, so the end-marker checks cannot pass. The console log is the
artefact. See `kb/traps.md`.

## The one inequality

State the fit as **one inequality, never two pools**. Splitting it into an
"image budget" and a "heap budget" has already produced two wrong numbers
(14,451,476 and 11,068,532).

```
(image span) + (genuinely additive heap) ≤ 16,646,144

  image span        18,993,020   post-P7 + ARAM-pager (+4,604), 2026-08-02
                                 pre-pager 18,988,416: text 5,804,776 /
                                 data 2,337,976 / bss 10,837,376, _end 0x8d22bd80
  additive heap      2,358,752   KOS 262,144 + arena 1,900,000
                                 + ARAM LRU 131,072 + threads 65,536
  ⇒ over by          4,705,628
```

At the *policy* arena (2,705,504; ARAM is now fixed at 131,072) it is over by
5,511,132. These supersede the 6,999,924 / 6,424,032 / 5,421,920 that older
docs and earlier versions of this block quote.

⚠️ **Correction, 2026-08-02:** an earlier version of this block said the
pre-P7 span was 18,997,600 and the gap 5,431,104. That was an arithmetic slip
— `0x8d265f60 - 0x8c010000` is **19,226,464**, which is what the older docs
said all along. The `size` "dec" column is not the span: it omits inter-section
alignment and it counts `.ocram`, which lives at `0x7c001000` and is not in the
image at all. **Take the span from `_end` minus `0x8c010000`, never from
`dec`.**

`.text` + `.data` = 8,142,752 B and neither can shrink — `-O0` is mandatory, so
`.text` can only be *relocated*. The lever big enough is demand-loading the
8,771,358 B of asset destination arrays (`kb/levers.md` L1), **but the pool it
loads into is additive heap**, which is what makes S4's pool size the binding
constraint. `dc_mem_ledger.c` prints this line at boot as `MEMLEDGER FIT …`.

⚠️ **Measure only against a clean rebuild.** `dc/build/flags.stamp` now forces
one when flags change; before that fix a stale `dc_main.c.o` made a non-stub
ELF read 356,776 B too small.

⚠️ **The ARAM debt is paid. `DC_ARAM_WINDOW=131072`.** PLAN §3.1's disc-backed
pager landed 2026-08-02 (`dc/src/dc_aram.c`, `dc/include/dc_aram_lru.h`, the
provenance ring in `dc_dvd.c`, kill switch `DC_ARAM_LRU=0`). The window is no
longer a window: an extent map learned from the write stream maps ARAM ranges
onto byte ranges of `/cd` files, and a read miss is one `fs_read` into the
caller's buffer. The pool is a 4 × 32 KB LRU cache. **−720,896 B of additive
heap, +4,604 B of image span, net −716,292 B off the gap**; `MEMLEDGER FIT`
additive_heap 3,079,648 → **2,358,752**, verified in this tree. The old 851,968
"floor" rested on a wrong measurement — `forest_1st.arc` arrives as 26 transfers
of ≤32,768 B over one *contiguous* 851,744 B extent, not as one transfer.

## Latest measurements (2026-08-02) — full narrative in `kb/state-log.md`

- **Arena, first real measurement.** `DC_ARENA_PROBE=<frames>` reports the
  game's own allocator. At the title screen: **used 256,192 B of a 1,412,704 B
  zelda arena**, inside a 1,900,000 B knob, while libc had taken 2,666,496 B
  from sbrk. The arena is not where the pressure is. **Title scene only** — a
  loaded town is unmeasured, so this licenses a smaller *bring-up* arena, not a
  smaller shipping one.
- **Asset working set, textures.** `DC_ASSET_CENSUS=1` +
  `tools/dcstub/census_resolve.py`: the title screen touches **50 symbols /
  111,136 B** of real texture bytes — the logo glyphs, all seven `obj_train1_t*`
  textures, `grl_1_*`, and the animals' eye/mouth TA textures — against the
  4.6 MB of texture destinations the image keeps in `.bss`. Strongest evidence
  yet for `kb/research-creative-ram.md` T1.
- **The vertex/model half is measured: the whole title-screen working set is
  93,312 B** — 30,688 B of models (58 `gsSPVertex` batches, 18,720 B actually
  read) plus 62,624 B of textures and palettes, against **8,771,358 B** of asset
  destination arrays in `.bss`. `GXSetArray` is **dead in this game**, not just
  quiet: its only call site anywhere in `src/` is `GXInit.c:252`'s own reset
  loop. The seam that works is `OSs16tof32()`, whose only three calls in the
  emu64 TU are the three components of a source vertex; `dc/include/dc_census_vtx.h`
  wraps it and `dc/Makefile` force-includes it into that one TU.
- **The framebuffer probe is attributed, not fixed.** `PVR_FB_R_SOF1` reads
  `0x000E7480`: the display scans out 947,840 B into VRAM, and `vram_s` has
  never been the displayed surface. `FBNONZERO` is the assertion to trust; a
  hash cannot tell black from wrong-address.

## ⭐ 2026-08-02 (latest) — the button got pressed. The port is in the train intro.

`kb/boot-blockers.md`'s three cheap wins all landed, and the first was worth
much more than "an unattended START": **the game leaves the title screen and
reaches the player-select scene — the train intro, with Rover, dialogue frames
and real textures.** Confirmed both in the console census and by a human
watching Flycast. Narrative and numbers: `kb/state-log.md`, top entry.

```bash
python3 tools/dcstub/census_resolve.py <run>/console.log \
    --sizes-from dc/build/nonstub/AnimalCrossing.elf --top 0 > /tmp/census.txt
DC_STUB_KEEP="$(python3 tools/dcstub/census_keeplist.py /tmp/census.txt \
                 --with-default --colon)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=131072 DC_ARENA_BYTES=1900000 DC_AUTOSTART=300 \
  bash dc/build-dc.sh
```

- **`DC_AUTOSTART=<N>`** (`dc/src/dc_pad.c`) synthesises START/A pulses from
  `PADRead` call N. ⚠️ **`DC_AUTOSTART_PERIOD=24` is WORSE than the default 90**
  — measured: pressing every 0.8 s stalled the run at 38 draws instead of
  reaching the train. Dialogue needs press/release edges, not a held button.
- **Console flood limiter** (`dc/src/dc_misc.c`, a `printf` override + the same
  table from `OSReport`): `SendStart::Mesg Full Queue` 741 → 15 lines.
  `DC_CONSOLE_LIMIT=0` reverts. Two ways to get this wrong are in `kb/traps.md`.
- **`OSGetSoundMode()` → stereo** (`dc_stubs.c`). It returned mono and
  `src/audio.c:147` hard-locked the game to mono off that.
- **The cull mapping was inverted** (`dc_pvr.c`) — `GX_CULL_BACK` mapped to
  `PVR_CULLING_CCW`, which KOS defines as "cull if the screen-space determinant
  is negative", and `emit_projected` already negates Y. So the port culled
  exactly the faces it should keep and every character rendered inside-out. The
  title screen was unaffected the whole time because the logo draws with
  `GX_CULL_NONE`. See `kb/traps.md` → Renderer.
- **The keep list is now generated, not written.** `tools/dcstub/census_keeplist.py`
  joins a census to the linked map and emits `DC_STUB_KEEP` (66 files,
  `dc_stub_keep.inc` 546 rows / 390,848 B; `.bss` +322,112, `.text` +37,280).
- **`DC_FB_IMAGE=<1|2|4>`** dumps the whole framebuffer as base64 rows;
  `tools/dcfb/fbimg_to_png.py` decodes a run into PNGs. Built because every
  rendering question past "is it black" was being answered by a human watching
  the emulator. **Now run end to end** — the port has screenshots.
- **The GX wrap mode is honoured** (`dc_pvr.c`, `dc_gx.c`). It was stored and
  never read, so every texture repeated. The opening's spotlight was drawn 2.7
  times across the frame; it is now one cone over a legible floor with a
  silhouette in it. Same keep list, same draw counts (96/49, q/t unchanged) —
  a clean A/B. Kill switch `DC_XDEFS='-DDC_PVR_NO_UVCLAMP'`.
- **TEV constant colours reach the vertex** (`dc_pvr.c`, N3's first slice).
  `g_gx.tev_colors[]`/`tev_k_colors[]` were stored and never read, so a
  combiner whose colour is `PRIMITIVE` or `ENVIRONMENT` got the texture's own
  RGB instead. The opening's shade quad is
  `gsDPSetCombineLERP(0,0,0,PRIMITIVE, 0,0,0,TEXEL0)` with `PRIM = BLACK`
  (`grd_player_select.c:69`), so a black vignette rendered as a **white** one:
  27.9 % of the frame at pure `0xFFFF`, now **0.0 %**. Only the
  `a=b=c=ZERO, d=<const>` shape is recognised; everything else keeps the raster
  path. Kill switch `DC_XDEFS='-DDC_PVR_NO_TEVCONST'`.
- **`DC_XDEFS`** passes raw `-D` flags through `dc/build-dc.sh` into the
  Makefile, so the renderer kill switches are reachable from a command line
  instead of by hand-editing. Verified reaching 3920 compile lines.
- **The dialogue balloon renders.** Its arrays live in `m_msg_data.c_inc`, and
  `make_stub_data.py` globbed `*.c` only, so that TU never entered `stub.list`
  and `census_keeplist.py` dropped it. Both tools now handle `.c_inc`:
  `cinc_includes()` rewrites and shadows them (`-I$(STUBDIR)/include`, the
  mechanism `DC_SRC_SHRINK` already used) and the keep list no longer drops
  non-stubbable sources. The balloon textures went from **0/8192 non-zero
  texels to 6475/8192**.
- **The keep list was re-censused and the animals have textures.** The old
  31-file list covered 90 asset loads; the regenerated 76-file list covers
  **779**, and blank texture uploads fell from **77/117 to 11/119**. The
  "animal textures used to work and now don't" report was not a renderer
  regression at all — those textures were never in the image, and a stubbed
  asset decodes to a transparent rectangle that draws as a black silhouette.
  The scene now renders K.K. Slider, his guitar, the stage floor and readable
  dialogue. `image_span` 10,239,776 → 10,622,368 B, margin 3,665,024 B, fit
  still OK, 29.3 FPS unchanged. List checked in at
  `tools/dcstub/keeplist-opening.txt` with the command to reproduce it.
  The residual 15 are outdoor acre/scenery textures this indoor scene loads but
  never draws.
- **`DC_TEX_LOG=1`** logs what each texture upload actually decoded to
  (non-zero texel count, value range, distinct values, palette head). This is
  what separated "missing asset" from "renderer bug" — see `kb/traps.md`.
- **`DC_PVR_BATCH_LOG=<N>`** dumps every batch's state — tex/wrap/blend/cull/z
  plus the screen bbox and z range of what was actually emitted — on every Nth
  frame. Pair it with `DC_FB_PROBE` at the same N and a region of a decoded PNG
  can be attributed to the state that drew it. This is what found both bugs
  above; the `[PERF]` draw counters cannot distinguish "submitted" from "on
  screen".

### A correction to `kb/boot-blockers.md` item 5

`SCENE_PLAYERSELECT` does **not** gate on the memory card.
`aNPS_setup_game_start` waits on `mCD_InitGameStart_bg()`, and while
`src/game/m_card.c:5096` is a 10-step card state machine,
**`pc/src/pc_m_card.c:1188` overrides that symbol** (the link carries
`--allow-multiple-definition`) and returns `mCD_TRANS_ERR_NONE`
unconditionally. The gate is the dialogue FSM and a 440-frame timer — i.e.
input. `[PC] toNextLand: keepSave not set, aborting` is likewise not a blocker;
it is the town-to-town transfer path with no save.

### The number the plan was waiting on — answered

**93,312 B.** A title-screen-complete build does not need S4. Extending
`DC_STUB_KEEP` with the censused models and textures is the next concrete step
(N1 item 2 below).

## ⭐ Ranked next actions — SUPERSEDED 2026-08-02 (session 2)

**Read `kb/RESUME.md` §5 instead.** The list below predates the town being
reachable. What changed: N1 ("get the town to draw") is essentially done — the
keep list is 107 files from a town census, uploads went 119→269 with blanks
9.2 %→5.6 %, and the town renders. The live queue is now
**(1) the PVR punch-through list** — blocking the train door, and the one thing
that cannot be worked around, since both depth-write settings are visibly
broken; **(2) the window scroll's UV offset**; **(3) fog**, entirely
unimplemented though the game asks for `GX_FOG_PERSP_LIN`; **(4) the missing
speaker-name / reply text**, with `DC_ARAM_TBL_PROBE` written and ready to
adjudicate it.

Audio is parked by the user's instruction. Its root cause was found (the jaudio
pipeline had never ticked once — `pc_audio_process_frame` had no caller and was
being dropped by `--gc-sections`), a real `snd_stream` device and pump now
exist, and it produces silence because `dc_aram.c` throws `audiorom.img` away.

## Ranked next actions (2026-08-02, session 1) — the list before parking

The S1→S5 plan in `kb/plan-stages.md` is still the RAM strategy and is not
superseded. These are the concrete next moves now that pixels exist.

### N1. Get the town to draw. [reframed 2026-08-02 — the texture half is measured, the vertex half is not]

The keep-list-by-hand plan is dead: the title demo's acres and animals are
named by index and profile ID, so there is no static list to extend from.
`DC_ASSET_CENSUS=1` replaces it and already answered the texture side (50
symbols, 111,136 B — see "Latest measurements" above). Two follow-ups, in order:

1. ✅ **The vertex side is censused** — 93,312 B for the whole title screen, no
   `src/` edit needed. See "Latest measurements".
2. **Extend `DC_STUB_KEEP` from the measured list.** 93 KB fits trivially, so a
   **title-screen-complete build should land without S4**. The census names the
   models: `boy_1_v`/`grl_1_v`, `mnk_1_v`, `wol_1_v`, `dog_1_v`, the three
   `obj_train*_v`, five `logo_us_*_v`, `ef_hanabira01_00_v`, `ef_shadow_out_v`.
   ⚠️ Animal species attribution is **not decidable on a stub image** — every
   `xxx_1_v` is 16 B apart and all species share a rig, so 4 of 58 batches are
   ambiguous and some "certain" animal hits may be aliases. Keeping the named
   models gives them real sizes and spacing, which makes the join exact: do that
   first, then re-run the census to confirm before trusting the list.
3. Then re-run on a **town** scene — that is what actually sizes S4's pool, and
   the batch table caps at 1,024 (`full=0` on the title screen; a town run must
   check that counter).

### N2. ✅ DONE 2026-08-02 — the unattended visual gate works.

```bash
bash harness/dc/smoke.sh dc/build/OpenCrossing.cdi --fb-golden 25789d43
```

`fb_saw_pixels` and `fb_golden` come back in the JSON. The golden is the stub
title screen at `DC_ARAM_WINDOW=851968 DC_ARENA_BYTES=1900000`;
`FBNONZERO 13711 of 307200`, reproduced across four runs. The decoded 16×12
thumbnail shows two centred text bands in the lower third over black — "PRESS
START" above the copyright line, which is what a human reported seeing.

**`--fb-writeback` is required, not optional**, and my earlier note in this file
saying otherwise was wrong: without it every candidate surface reads zero, with
it the scanout surface reads real pixels. `0xA5000000 + FB_R_SOF1` was the right
address all along; the 64-bit-aperture hypothesis was wrong and Flycast's
32-bit aperture merely mirrors every block at +4 MB. The frame-rate cost of the
flag is **unmeasured** — the old 24.8 → 16.8 FPS did not reproduce and no
controlled pair exists.

### N2b. Wire the game's save path to the VMU. [the backend is real; nothing calls it]

`dc/src/dc_card.c` is a working KOS `vmufs`/`vmu_pkg` backend, proven in Flycast
and re-verified host-side out of the flash image. **But the game never calls
it:** `pc/src/pc_m_card.c` does its I/O with `<stdio.h>` against the relative
path `save/card_a/DobutsunomoriP_MURA.gci`, so of the 29 `CARD*` entry points
only `CARDInit()` is on its path and `[PC] No save file found` is a failed
`stat()`. `pc_card_scan_for_gci()` deliberately still returns 0 — returning a
path would make the game print "found" and then fail to `fopen` it.

The designed fix is `kb/save-plan.md` §7.8: a KOS `vfs_handler_t` mounted at
`/dcsave` plus an `fs_chdir()` from `dc_main.c`, committing to the VMU on the
`->rename` callback — which is exactly `pc_save_write_gci_ex`'s last step, so
it is a free atomic commit point with no edit to `pc/`.

**Measured and load-bearing:** a VMU block costs **84.6 ms** to write
(`write ≈ 0.678 s + 84.6 ms/block`, within 1.3% of the KOS-source ceiling), so a
150-block save is **13.4 s**. Incremental writes are mandatory and the shipped
chunking does *not* deliver them — `vmufs_write()` rewrites the whole file. The
format is byte-stable; the writer needs a block-diffing pass. Deflate-6 on SH-4
is 295,910 B → 99,657 B in 0.129 s, i.e. compression is free next to the flash
cost. ⚠️ Every compression ratio so far is against **synthetic** data; a real
`.gci` is the top open item.

### N3. Correct the TEV mapping. [the logo renders; is it renders *right*?]

`dc_pvr.c` implements exactly one TEV configuration — modulate texture by
rasterised colour — against the 101 in `kb/tev-map.md`. Konst-colour and
multi-stage configs currently come out untinted. Now that something is on
screen, this is measurable for the first time: instrument which of the 101
configs the title screen and the field actually request, and implement by
frequency rather than by enum order.

### N4. Measure bucket 6 properly. [PARTLY DONE 2026-08-02 — title scene measured, gameplay is not]

`DC_ARENA_PROBE=60` reports the game's own allocator every 60 presented frames.
At the title screen: **used 256,192 B of a 1,412,704 B zelda arena**, inside a
1,900,000 B arena knob, while libc had taken 2,666,496 B. No bisect needed and
no arena-side OOM is reachable from here.

What is left is the part that decides the shipping number: **the same probe on
a scene that has a town loaded**, which does not exist yet. Until it does, do
not cut `DC_MAIN_MEMORY_SIZE` on the strength of the title figure — cut it for
bring-up images if libc needs the room, and re-run the probe the moment S4
loads real field data.

### N5. Then S4 — the asset loader. [unchanged, still the critical path]

`kb/plan-stages.md` S4 still applies in full — read it before starting. Two
things changed since it was written: the pool must be sized against the
**libc** side of the split, not treated as a free-floating extent
(`kb/heap-two-pools.md`); and the S3 remainder is smaller than billed — P6
measured −598,424 B, not −821,569.

### Also worth knowing

- `SendStart::Mesg Full Queue` spams the console ~1,000 times per run. It is
  jaudio, it is not fatal, and it makes logs hard to read. Worth silencing.
- **The ARAM window no longer thrashes** — `rebases=14` → the concept is gone.
  Verified in this tree: all 4,982,400 B of graph-half writes mapped
  (`forest_1st` + `forest_2nd` exactly), **`LOST=0`, 0 reads zero-filled** (was
  ≥5,121), 2 extents, `pin_peak=0`. Grep `[DC/ARAM] LRU` — `LOST=` must be 0.
  ⚠️ **Real archive content now reaches the renderer, and it costs: 29.3 →
  12.6 FPS**, `cmds` 851 → ~3,600, textures 78/301,312 B → 173/676,608 B.
  Measured *not* to be disc — at the same frame index under `--no-fast-gdrom`
  the frame is 100.6 vs 66.5 ms with `gx` only 29.0 vs 26.0, so ~31 ms/frame is
  SH-4/emu64 work on content the port used to throw away. **That is the next
  performance question and it is a new one.**
  ⚠️ `pin_peak=0` only because the save path never runs. `m_card.c:1607`'s three
  ARAM save blocks are 147,782 B of writes with no disc provenance; raise
  `DC_ARAM_WINDOW` to 262,144 when the VMU write path is wired up (N2b).


## Toolchain

`opencrossing-dc:sdk` in the local Docker daemon — **do not rebuild, ~27 min
cold**. sh-elf GCC 15.2.0, newlib 4.6.0.20260123, binutils 2.45.1, KOS 2.3.0
(`1c6398f9`), kos-ports (`f4faacc4`), GLdc (`a1cd80a8`), mkdcdisc (`3c2ef63a`),
`-m4-single`, thread model kos. Clean build ≈ 97 s for 3917 TUs + link + CDI at
`-j4`. Entry points, every env knob and the flag assembly: `BUILDING-DC.md`.
Gotchas: `kb/traps.md`.

## Standing constraints

`CLAUDE.md` §1 is authoritative and must not be restated differently here.
The short version: stock 16 MB (the 32 MB mod must never become a requirement),
`src/` at `-O0` with codegen flags banned, never edit `src/`, never commit ROM
material or disc images, every optimization gets a kill switch, agents do not
run git — the main thread commits.

**Be honest in reporting.** "Still N MB short with `-O0` mandatory" is a valid
and important result. If the levers do not close the gap, cutting content
(`kb/levers.md` L5 — the user's call, not engineering's) or declaring a
stock-16 MB build infeasible are the honest options; quietly reopening the
optimization question is not.
