# Session log — what was observed running, in order

> ⚠️ **2026-08-02, read first:** the top entry below (framebuffer probe, "N2
> NOT solved", FBNONZERO 0) is **SUPERSEDED** — N2 is done: `--fb-writeback` is
> REQUIRED, golden `25789d43` works. See `kb/STATE.md` N2 and `kb/traps.md`.
> Also stale below: vertex census "unmeasured" (done — 93,312 B, `accc232`),
> "nothing is ever drawn / backend stub" (false since `fd4ee2c`), ARAM window
> thrash (pager landed, `29ffca5`), VMU write unimplemented (backend landed,
> `b4a177d`). Narrative for those four commits currently lives in `kb/STATE.md`.

The dated narrative of this port's bring-up: what executed, when, and what it
cost to get there. `kb/STATE.md` carries only what is true *now* and stays
short; everything that is history but still evidence lives here. Newest first.

## ⭐ 2026-08-02 (latest) — the first screenshots, and what they showed

`DC_FB_IMAGE` had been written but never run end to end. Decoding the run it
left behind took two fixes to `tools/dcfb/fbimg_to_png.py` — a run killed
mid-row leaves a partial base64 payload that threw `binascii.Error: Incorrect
padding` and lost **every** frame, and a trailing `FBIMG BEGIN` with no `END`
dropped the last one. Both are now tolerated and reported.

Eight frames, 320×240, spanning ~540 rendered frames of the opening /
player-select scene (post-`aAL_setupAction: 3 -> 4`). What they measured:

- **26.9 % of the frame was exactly `0xFFFF`, 43.2 % exactly `0x0000`,** and
  the remaining 30 % was 879 distinct near-black colours — brightest common one
  `0x28C2` = RGB(41,24,16). Blown-out core, dead-black surround, nothing
  between.
- The bright region was **one shape repeated at a fixed 117 px pitch**, hard
  vertical seam on one edge and a real 12 px falloff (`0xF79E → 0x0020` across
  x=113–124) on the other. That is texture repeat, not lighting.
- **The GX wrap mode was stored and never consumed.** `dc_gx.c` has held
  `TEXOBJ_WRAP_S/T` since M1; `dc_pvr.c` hardcoded `PVR_UVCLAMP_NONE`. Fixed by
  mirroring wrap into `g_gx`, folding it into `header_key()` and the
  `GXLoadTexObj` dedup key, and mapping GX → `uv_clamp`/`uv_flip`. Result: one
  cone, a legible floor, a silhouette standing in it. Draw counts identical
  across the A/B (96/49, same q/t), so nothing else moved. Distinct colours
  879 → 536.
- **The remaining ~28 % of pure white was the TEV, and the per-batch dump found
  it.** `DC_PVR_BATCH_LOG` was written for this and it worked first try: the
  offending batch was a 32×64 I-format texture, `wrap=2,0`, `bm=1,4,5`,
  `argb=FFFFFFFF`, bbox covering the whole frame. Those numbers name the draw
  exactly, and the source data says what it should be
  (`grd_player_select.c:69`):

  ```c
  gsDPSetCombineLERP(0, 0, 0, PRIMITIVE,  0, 0, 0, TEXEL0, ...)
  gsDPSetPrimColor(0, 255, 0, 0, 0, 255)      // PRIM = BLACK
  gsDPLoadTextureBlock_4b_Dolphin(rom_open_shade_tex, G_IM_FMT_I, 32, 64, 15,
                                  GX_MIRROR, GX_CLAMP, 0, 0)
  ```

  Colour is `(0-0)*0 + PRIMITIVE` = black; alpha is `TEXEL0`. GX expands an
  I-format texel to `(I,I,I,I)`, so modulating by a **white** vertex turned a
  black vignette white. `g_gx.tev_colors[]` had been stored by
  `GXSetTevColor` and never read — the same "recorded but not consumed" shape
  as the wrap mode, one layer up. Folding the constant into the vertex RGB
  takes pure white to **0.0 %**. The frame is now a dark room with a lit
  spotlight pool and a silhouette in it.

  The same dump independently confirmed the wrap fix: it reported `wrap=2,0`
  (MIRROR, CLAMP) for the spot quad and `wrap=0,0` for the floor, matching
  `gsDPSetTile_Dolphin(..., GX_MIRROR, GX_CLAMP, ...)` and
  `gsDPLoadTextureBlock_4b_Dolphin(rom_open_floor_tex, ..., GX_CLAMP, GX_CLAMP, ...)`
  in the same display list.
- **A latent trap in that fix, caught in review before it could bite.**
  libforest's `TEV_*` constants alias `GXTevColorArg` deliberately, but
  `TEV_COMBINED` is 0 and so is `GX_CC_CPREV` — and those mean different
  things. Treating `CPREV` as a constant register would have blacked out the
  ~245 `(0, 0, 0, COMBINED)` cycle-0 draws in `src/data/model/`. Removed;
  see `kb/traps.md`. It changed nothing in this scene (identical histograms
  either way), which is exactly why it needed catching by reading rather than
  by measuring.
- **The black silhouette was K.K. Slider, and he was never in the image.** The
  user reported that animal textures used to work and had stopped. Every
  renderer suspect was wrong: the wrap change provably cannot reach them (NPC
  textures are power-of-two, so `u_scale == 1` and clamp pins at the true
  edge), the TEV change provably cannot reach them (914 of ~940 NPC combiners
  start with `TEXEL0`, so the constant-colour rule bails), and no path
  populates a `GXTexObj` outside `GXInitTexObj*`.

  `DC_TEX_LOG=1` answered it in one run: **77 of 117 texture uploads decoded to
  a single value — zero.** The palette dump showed `raw=0000,0000,0000,0000`:
  the TLUT bytes themselves were zero, so it was never a decode bug. The blank
  uploads were 32×16, 32×32 and 16×8 CI4 — exactly `anime_1/3/4_txt`, the NPC
  set. On a `DC_ASSET_STUB` image a stubbed array is `[1]` bytes, so texels and
  palette both read as zeros, the decoder produces a transparent rectangle, and
  the model draws as a black silhouette with every counter looking healthy.

  Re-censusing on a keep-list build (the action `kb/STATE.md` N1 and
  `kb/RESUME.md` §5.2 had been carrying) grew the list from 31 files / 90 asset
  loads to 71 files / **779**, and blank uploads fell to 15/119. The scene now
  draws K.K. Slider, his guitar, the stage floor and readable dialogue;
  distinct colours in the frame went 387 → 1346. Cost: `image_span`
  10,239,776 → 10,622,368 B, margin still 3,665,024 B, frame rate unchanged at
  29.3 FPS. The list is checked in at `tools/dcstub/keeplist-opening.txt`
  because regenerating it costs two full builds and a 240 s run.
- **Then the balloon behind the dialogue, which was the same bug one level
  down.** `make_stub_data.py:532` globs `*.c`, so `src/game/m_msg.c` — whose
  asset arrays and `_pc_load_src_game_m_msg_data_c_inc()` both live in the
  `#include`d `m_msg_data.c_inc` — never entered `stub.list`, and
  `census_keeplist.py:183` dropped its symbols on the grounds that files
  outside `stub.list` are "already full size, so naming them would be a no-op
  at best". That rationale is false under `DC_ASSET_STUB`, where the keep list
  is the *only* asset-loading path that runs: the arrays got a correctly-sized
  `.bss` buffer and no loader.

  First attempt emitted `dc_stub_keep_load_one()` rows for them directly and
  **failed to link** — the arrays are `static`. They can only be filled from
  inside the TU, so the fix is to `keep_file()` the `.c_inc` as well and shadow
  it on the include path with `-I$(STUBDIR)/include`, which is exactly the
  mechanism `DC_SRC_SHRINK` already used for its own two `.c_inc` files
  (`dc/Makefile`, "Include-path shadow", verified there with `gcc -E -H`).

  Result: balloon textures **0/8192 → 6475/8192** non-zero texels, blanks
  15/119 → 11/119, `image_span` 10,622,432 B, margin 3,664,960 B, 29.3 FPS.
  ⚠️ The `INCLUDES` change is invisible to `flags.stamp`, so the first build
  after it silently kept the old `.c_inc`; the objects had to be deleted by
  hand. In `kb/traps.md`.
- **The "hang" was the instrumentation, not the game.** Reported as "hangs
  forever when K.K. starts talking". It does not: `mMsg_sound_PAGE_OKURI()` is
  `sAdo_SysTrgStart(0xB)` and is reachable only from two button-driven sites in
  `mMsg_request_main_index_fromNormal`, so every `SE 0x000B` in the log is a
  page advance — and there are 6 in that run, with screenshots showing two
  different dialogue pages. `DC_FB_PROBE=200 DC_FB_IMAGE=2` streams 8 × ~205 KB
  of base64 over a 57600-baud SCIF: `console.log` is 1,631,116 B versus 118,743
  B without it, i.e. roughly 150 s of the 200 s timeout went to screenshot
  traffic. The same build without `DC_FB_IMAGE` reached **5069 frames vs 1379**
  in 1.2× the wall time, logged 12 page advances, and got past the dialogue
  into a field with player footsteps. **Screenshot runs are not speed runs —
  do not read progression off one.**
- **Two anomalies worth carrying forward.** `tris in == out`, `clipped=0`,
  `dropped=0` cumulatively over 623,614 triangles — the near-plane clipper has
  never fired once, which for a camera inside geometry is implausible. And
  quads alternate 50 → 3 → 49 → 3 between frames whose exact pixel diff is
  922/76800 (1.2 %) of scattered edge noise with no coherent silhouette: **47
  quad draws per frame are producing nothing visible.**

Also landed: `DC_XDEFS`, a raw `-D` passthrough, because the renderer kill
switches previously required hand-editing the Makefile — which is why the three
A/B CDIs in `~/.cache` are not reproducible from a command line.

## 2026-08-02 — the button got pressed, and the port left the title screen

`kb/boot-blockers.md`'s three cheap wins (its items 4, 2 and 9) landed together,
and the first of them turned out to be worth far more than "an unattended
START": **the game reaches the train intro** — the player-select scene, with
Rover, real dialogue windows and real textures. A human watching Flycast
confirmed it independently.

### What was built

- **`DC_AUTOSTART=<N>` (`dc/src/dc_pad.c`).** From `PADRead` call N onward,
  synthesise a pulse of 6 calls every `DC_AUTOSTART_PERIOD` (default 90),
  alternating START and A. The title takes either; the menus after it take A.
  Absent by default, so a normal image is unchanged. Works on hardware too,
  which a Flycast input script would not.
- **Console flood limiter (`dc/src/dc_misc.c`).** A `printf` OVERRIDE in
  DC-owned code plus the same table consulted from `OSReport`. Keyed on the
  format-string POINTER — one call site is one pointer, so nothing has to be
  formatted to decide. Emission backs off to powers of two per site and
  surviving repeats are prefixed `[xN]`, so a flood becomes a heartbeat
  carrying its own count rather than silence. `DC_CONSOLE_LIMIT=0` reverts.
  - **MEASURED: `SendStart::Mesg Full Queue` 741 lines → 15** in the very next
    run, up to `[x8192]`.
  - ⚠️ **The first version did nothing at all** and looked correct doing it. It
    was an open-addressed table that gave up ("print it") when full — and boot
    alone produces more than 32 distinct call sites, so it was full before the
    flood even started. The property that matters is REPLACEMENT, not
    associativity: direct-mapped, evict on miss. A flooding site re-claims its
    slot forever; an evicted one-shot line simply prints again.
  - A second flood only becomes visible once the title is passed:
    `game64.c_inc`'s `[TRG_VOL]`/`[WALK]` call `printf` DIRECTLY, so an
    `OSReport`-only limiter cannot catch them. That is why the sink is `printf`.
    Our own diagnostics are unaffected: `DC_LOG`/`DC_LOGE` go through
    `dc_log_impl`, which calls `vprintf`.
- **`OSGetSoundMode()` → stereo (`dc/src/dc_stubs.c`).** It returned 0 =
  `OS_SOUND_MODE_MONO`, so `sAdo_SetOutMode` (`src/audio.c:147`) forced
  `Na_SetOutMode(1)` and the port hard-locked itself to mono against
  `kb/audio-plan-of-record.md` §9.1. Now a stored value defaulting to stereo,
  and `OSSetSoundMode` keeps the player's choice for the session.

### The reach, traced

`[LOGO] aAL_setupAction: 0 → 2 → 3 → 4 → 5`, then `[SCENE_MODE] 0 → 3`, then a
scene whose census is unambiguous: `rom_train_in`/`rom_train_out` geometry,
`rom_train_{seat,wall,roof,floor,bgcloud,bgtree}_tex`, `con_kaiwa2_w*` dialogue
frames, `FONT_nes_tex_font1`, and eye/mouth TA textures for a dozen species.
`FBNONZERO` went from 13,711 (title logo) to **22,305–52,675 of 307,200**.

`[PC] toNextLand: keepSave not set, aborting` fires on the way through and is
**not** a blocker — it is the town-to-town transfer path with no save present.

### What the next scene actually waits on — a correction to `kb/boot-blockers.md`

An agent trace concluded `SCENE_PLAYERSELECT` can never advance because
`aNPS_setup_game_start` (`ac_npc_p_sel_schedule.c_inc:1-16`) gates on
`mCD_InitGameStart_bg() == mCD_TRANS_ERR_NONE`, i.e. on the memory card. **That
is true of `src/game/m_card.c:5096` and false of the build:**
`pc/src/pc_m_card.c:1188` overrides that symbol (the link carries
`--allow-multiple-definition`) and **returns `mCD_TRANS_ERR_NONE`
unconditionally**. The card is not the gate. The gate is the dialogue FSM and
the 440-frame `strum_timer` in the same file — i.e. input, which now exists.

### The keep list stopped being hand-written

`tools/dcstub/census_keeplist.py` joins a `census_resolve.py` table to the
linked map (`.bss.<sym>` → object → source file), intersects with
`stub.list`, and prints a `DC_STUB_KEEP` list. Measurement → keep list, with no
step where a human guesses which acre the title demo uses.

- **66 files, `dc_stub_keep.inc` 546 rows / 390,848 B**, from a census taken
  with `DC_AUTOSTART` on so it covers the train scene as well as the title.
- Image cost: `.bss` 2,417,568 → 2,739,680 (+322,112), `.text` +37,280. The
  stub image is ~10.5 MB; there is room.
- ⚠️ **The map has TWO section-line shapes** — name alone on its line, or name
  and address/size/object on one line when the name is short. Parsing only the
  first shape silently lost exactly the 12 NPC vertex arrays (`grl_1_v` and
  friends), which are the symbols the keep list most needs.

### Frame rate, measured on the way

| scene | FPS | gx ms | cmds |
|---|---:|---:|---:|
| title, stub assets only | 29.3 | 0.0 | 12 |
| title logo drawing | 8.8–11.5 | 29–30 | ~3,300 |
| train intro, keep list on | 17.7–22.4 | 17–19 | 1,650–2,000 |

`gx` is the DC GX layer alone; `kb/STATE.md` already measured that roughly
another 31 ms/frame is emu64 in `src/` at `-O0`, which is not a legal target.

## ⭐ 2026-08-02 (later) — the framebuffer probe is attributed, the arena is 5.5× oversized at title

Three of the five next actions moved. All numbers below are from two runs of
one instrumented image (`DC_ARENA_PROBE=60 DC_FB_PROBE=120 DC_ASSET_CENSUS=1`),
plus one clean full-size rebuild.

**N2 is NOT solved. Framebuffer emulation was not the answer, and reading two
different hashes was not evidence that it was.**

Full sequence, because the wrong conclusion was reached first and is worth not
repeating. `config:rend.EmulateFramebuffer=yes` (Flycast's "Full Framebuffer
Emulation", now `smoke.sh --fb-writeback`) made `FBHASH` show two distinct
values instead of one, which *looked* like the fix. It was not: the probe now
also prints `FBNONZERO`, and the answer is

```
FBNONZERO 0 of 307200      (every probe, every frame, with writeback ON)
FBHASH bae41dc5
```

`bae41dc5` is simply the FNV-1a of 614,400 zero bytes. **Counting nonzero
pixels is the assertion; a hash cannot tell "black" from "reading the wrong
surface".** The probe was also point-sampling its 16×12 thumbnail, which could
step over a logo covering a few per cent of the frame — it box-filters whole
cells now, and still reports black, which is consistent.

`FBSWEEP` — the display controller's own scanout registers plus a sweep of all
8 MB of VRAM in 64 KB blocks — was added to attribute it, and it acquits the
emulator:

```
FBSWEEP sof1=000e7480 sof2=000e7980 hot_blocks=3/128  first=12,14,78,0
FBSWEEP sof1=000e7480 sof2=000e7980 hot_blocks=12/128 first=0,12,14,23
FBSWEEP sof1=000e7480 sof2=000e7980 hot_blocks=20/128 first=0,12,14,20
```

**VRAM is not empty and it fills up as the run proceeds** (3 → 12 → 20 hot
blocks), so "Flycast writes nothing back" is dead. And **`PVR_FB_R_SOF1` is
0x000E7480 — the display scans out from VRAM offset 947,840, not from offset
0.** `pvr_init()` allocates its own buffers and programs the display controller
at them; `vram_s`, which the probe was reading, is the base of VRAM and is not
the displayed surface. That was our bug, not the emulator's.

Pointing the probe at `0xA5000000 + SOF1` is the obvious fix and **it is not
sufficient — that read is still 0 of 307,200.** Meanwhile one run reading the
*old* address with writeback on did once report `FBNONZERO 13711`, so content
does reach low VRAM eventually.

**Next step, and the strong hypothesis:** the Dreamcast exposes VRAM through
two windows — the 32-bit linear area at `0xA5000000` and the 64-bit
bank-interleaved area at `0xA4000000` — and the SOF registers are in the
hardware's own offset terms. Reading the right bytes through the wrong window
returns the wrong bytes. Try `0xA4000000 + SOF1`, and hash the hot blocks the
sweep names (12, 14, 20, 23) directly to find where the 640×480×2 image
actually is; the sweep already prints everything needed to locate it. If both
windows fail, fall back to `pvr_scene_begin_txr()` and render one frame into a
texture the guest allocated itself.

`--fb-writeback` is kept and stays opt-in (24.8 → 16.8 FPS). It is not known to
be necessary.

**N4 has its first real measurement.** The arena is not where the pressure is:

```
[DC/ARENA] touched=54,272  peak=54,272  of 1,900,000 B | brk_used=2,666,496
[DC/ARENA] zelda used=256,192  free=1,156,512  largest_free=1,156,512
```

At the title screen the game's own allocator reports **256,192 B in use out of
a 1,412,704 B zelda arena** — the arena is 5.5× what bucket 6 is actually
holding, and libc has taken 2,666,496 B from sbrk over the same period.
`zelda_InitArena` is handed `game_getFreeBytes()` (`m_play.c:494`), so the
arena knob scales the game's heap directly and every byte cut goes to libc.
⚠️ **This is the title scene only.** A loaded town is unmeasured and will be
much larger, so this licenses a smaller *bring-up* arena, not a smaller
shipping one. The touched-byte scan (54,272 B) is a floor, not the answer —
zero-filled allocations are invisible to it; the zelda line is the real number.

**N1 could not be answered statically, so it is answered at runtime now.**
A subagent traced `m_titledemo.c` / `title_demo.c` / `ac_animal_logo.c` and
stopped at the ten logo TUs (8,824 B): the title demo names its acres through
`BLOCK_COMBI_GRD_*` indices into `l_combiID[]` and its 15 NPCs through profile
IDs, and neither is statically resolvable. `DC_ASSET_CENSUS=1`
(`dc/src/dc_asset_census.c`) records every asset address the GX layer is handed
and `tools/dcstub/census_resolve.py` resolves them against the ELF:

```
working set: 63 distinct addresses -> 50 symbols (0 unresolved)
total (real sizes): 111,136 B, all textures
```

The list is exactly what static tracing missed: the logo glyphs, all seven
`obj_train1_t*` textures, `grl_1_*` (skin/hair/shoe/bottom), and the
`mnk_/mob_/mol_/mos_1_*` eye-and-mouth TA textures of the animals on screen —
plus one 49,152 B `texture_buffer_data`, which is emu64 scratch and not an
asset. **So the title screen's entire real texture working set is ~62 KB
against the 4.6 MB of texture destinations the image keeps in `.bss`.** That is
the strongest evidence yet for `kb/research-creative-ram.md` T1.

⚠️ **The census sees textures only.** `GXSetArray` recorded **zero** hits — the
title path does not use indexed vertex fetch, and emu64 dereferences `Vtx` and
`Gfx` pointers inside `src/`, where there is no seam to hook without editing
it. The model/vertex half of the working set is still unmeasured, and it is the
half that decides whether the town draws.

**The span was re-measured on a clean full-size rebuild** (no probes, no stub,
`DC_SRC_SHRINK=1`): text 5,749,944 / data 2,638,872 / bss 10,837,376, `_end` at
`0x8d265f60` ⇒ **span 19,226,464** (⚠️ this was first written up as 18,997,600,
which was an arithmetic slip — see `kb/traps.md`, "Measuring the image").
Against the knobs the running image actually used at that point:

```
span 19,226,464 + additive 3,079,648 (KOS 262,144 + arena 1,900,000
                 + ARAM 851,968 + threads 65,536) = 22,306,112
                                        usable    = 16,646,144
                                        ⇒ over by    5,659,968
```

P7 has since taken 238,048 B off that span. `kb/STATE.md` carries the current
figure; the old 6,999,924 is no longer the number under any knob setting.

## ⭐ 2026-08-02 — THE TITLE SCREEN RENDERS

**The port draws pixels.** A `DC_ASSET_STUB` image boots in Flycast, runs the
game loop at **29.3 FPS / 98% speed**, reaches the title-demo scene, and
renders the Animal Crossing title overlay: **"PRESS START" and the copyright
line are on screen**, confirmed by eye. The PVR backend is submitting ~558,000
triangles per run with zero drops and zero unsupported primitives.

What is black behind the logo is **expected, not a bug**: only the ten
`src/data/model/logo_*` / `log_win_*` TUs carry real asset bytes in this build
(the `DC_STUB_KEEP` allowlist, 53,792 B). Every acre model behind them is still
a `[1]`-sized stub, so the town has no geometry to draw. Un-stubbing it is the
S4 loader, not a renderer fix.

The build that does it:

```bash
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
  DC_ARAM_WINDOW=851968 DC_ARENA_BYTES=1900000 \
  bash dc/build-dc.sh
bash harness/dc/smoke.sh dc/build/OpenCrossing.cdi --timeout 180
```

Four things had to be true at once, and each was a real defect:

1. **The renderer existed at all.** `dc/src/dc_pvr.c` (init, frame, SH-4 T&L,
   near-plane clip, submit) + `dc/src/dc_pvr_texture.c` (GC formats → twiddled
   16-bit VRAM). One PVR list, `PVR_LIST_TR_POLY` with **autosort disabled**,
   which turns it into the submission-ordered Z-buffered rasteriser the game
   was written against — and costs **zero bytes of main RAM**, unlike buffering
   three lists. `-DDC_PVR_BACKEND=0` restores the old NONE backend.
2. **The stub build was spraying `.bss`** — four full-size endian passes in
   `boot.c` writing into `[1]` arrays, which overwrote `HotStartEntry` and
   jumped to `0x65000004`. See `kb/traps.md`.
3. **A per-TU `-Dmain=` rule stopped firing** when a rewriter moved its source,
   producing two `main()`s, a silently-wrong link, and `--gc-sections` deleting
   5/6ths of the game. `.text` 5,289,364 → 851,684 and it still built a CDI.
   See `kb/traps.md`.
4. **The heap split was wrong.** See the next section — this is the RAM result.

## ⚠️ SUPERSEDED (kept for the record) — "the framebuffer probe does not work in this harness"

*Written 2026-08-02 morning. The conclusion below — that the harness and the
probe see different surfaces — was half right for the wrong reason. The actual
cause is that `pvr_init()` scans out from VRAM offset 947,840 while the probe
read offset 0; see the top of this file. The advice not to trust a black
`FBHASH` still stands, and is now enforced by `FBNONZERO`.*

## ⚠️ The framebuffer probe does not work in this harness — do not trust a black FBHASH

`dc_pvr_fb_probe()` emits `MARK:FRAME` / `FBHASH` / `FBTHUMB` (the protocol
`harness/dc/screenshot.sh` already parses), enabled with `DC_FB_PROBE=<frames>`.
**It reported a constant all-zero frame at the same moment a human watching
Flycast could see the copyright line render.** Tried against both
`pvr_get_front_buffer()` and `vram_s`; both read zero. Flycast has no headless
mode (`harness/dc/_runner.py`), so what the harness runs and what the probe can
see are not the same surface. Until that is understood, **the renderer census
(`[DC/PVR] frames/batches/tris`) is the trustworthy in-harness signal** and the
framebuffer hash is not. Do not re-run the "nothing is drawn" investigation on
the strength of a zero hash — that already cost a cycle.

## How far it gets today — read this before assuming anything is untested

`DC_ASSET_STUB=1` + `DC_DISC_ROOT=<flat disc root>` builds an image that fits
(`margin=1,934,444 OK`) and runs in Flycast. Rebuild and run it with:

```bash
python3 tools/dcasset/dcasset.py extract "<the ISO>" --out /tmp/discroot
bash dc/stage-disc.sh /tmp/discroot ~/.cache/oc-dc-discroot
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 bash dc/build-dc.sh
bash harness/dc/smoke.sh dc/build/OpenCrossing.cdi --timeout 180
```

Confirmed running, in order, from one boot: `dc_main.c`'s trampoline · KOS 2.3
init and the serial console · maple (controller + 2 VMUs) · `MEMLEDGER FIT` ·
`vid_set_mode` 640x480IL NTSC · the GX accumulator · iso9660 `/cd` mount and a
14-entry root listing · `ac_entry()` · `boot_main()` → `OSInit()` arena ·
`DVDInit` · ARAM window · `PADInit` · `GXInit` · `AIInit` · `Na_InitAudio` ·
`sound_initial()`'s 2.5 s wait · `initial_menu_init` · `dvderr_init` ·
`sound_initial2()` · `LoadStringTable` (`/cd/static.str` loads) · `JW_Init2`
mounting **`forest_1st.arc`** (852,896 B, 29 files, RARC sig verified) ·
`HotStartEntry` · `entry()` · `mainproc` · `CreateIRQManager` · `padmgr_Create`
· `JW_Init3` mounting **`forest_2nd.arc`** (4,132,608 B, 57 files) ·
`mMsg_aram_init2` · `famicom_mount_archive` · **`graph_proc`** · the save scan.

It stops at "No save file found". **Nothing is ever drawn** — `dc_gx`'s backend
is still `NONE (stub)`, so a Flycast window sitting on the Sega logo is the
expected result, not a fault. Rendering is M2/GLdc.

Known-wrong behaviour in this configuration, all understood:

- Assets are `[1]`-sized, so any asset the game touches is garbage.
- The ARAM window thrashes: mounting `forest_2nd.arc` rebases it 4 times
  (`rebases=11` by the end). That counter is the signal PLAN §3.1's LRU can no
  longer be deferred.
- Nothing saves — `dc_vmu_write_file()` is `DC_UNIMPLEMENTED`.

## S1 IS DONE — the port has executed (2026-08-01)

**The Dreamcast port runs.** `DC_ASSET_STUB=1` shrinks every asset destination
array to one element; the image fits and boots in Flycast, and for the first
time in the project's history the platform layer has been observed working
rather than assumed to work.

```
MEMLEDGER FIT image_span=12375220 additive_heap=3545184 usable=16646144
              margin=725740 OK
```

Confirmed running, in this order, from one boot: `dc_main.c`'s trampoline · KOS
2.3 init and the serial console · maple enumeration (controller + 2 VMUs) ·
`dc_mem_ledger_init()` and `MEMLEDGER FIT` · `vid_set_mode` 640x480IL NTSC ·
the GX accumulator (`verts=8192 x 40B`) · iso9660 `/cd` mount · `ac_entry()` ·
`boot_main()` → `OSInit()` arena (0x8cbf8bc0–0x8ce8d420, 2642 KB) · `DVDInit` ·
the ARAM window · `PADInit` · `GXInit` · `AIInit` and the audio ring ·
`Na_InitAudio` (the jaudio heap sets up: `AUDIOHEAP SET ADDR 8c9d6e20h`) ·
`sound_initial()`'s 2.5 s wait · `initial_menu_init` · `dvderr_init` ·
`sound_initial2()` · `LoadStringTable` · `JW_Init2`.

**Where it stops, and why it is not a port bug:** the CDI is built from the ELF
alone, so `/cd` carries no game data. `JKRAramArchive::open()` mounts a
zero-byte `forest_1st.arc`, byte-swaps a garbage `num_file_entries`
(4,235,863,808) and walks off memory. Every stop before it is the same story —
`miss: /cd/audiorom.img`, `/cd/COPYDATE`, `/cd/static.str`. Getting further
needs disc content, which is the `tools/dcasset` track, not a platform fix.

Three things this cost, all now fixed and kept: `MEMLEDGER FIT` is printed from
`dc_mem_ledger_init()` (it used to print only from `dc_mem_report()`, which runs
when `main()` returns — the game never returns); `g_pc_verbose` defaults on
under `DC_ASSET_STUB` or `-DDC_VERBOSE`, because every `OSReport` in the game is
gated on it and a burned CD-R passes no argv, so without it a bring-up run is
blind; and `dc_main.c` skips `pc_assets_init()` under `DC_ASSET_STUB` so the
central table cannot memcpy full-size assets over one-element destinations.

How to rebuild it:

```bash
DC_ASSET_STUB=1 bash dc/build-dc.sh    # regenerates dc/build/stubsrc, then builds
bash harness/dc/smoke.sh dc/build/OpenCrossing.cdi --timeout 120
```

`tools/dcstub/make_stub_data.py` rewrites 2,535 TUs (16,317 arrays,
**8,716,158 B**) into `dc/build/stubsrc`, mirroring repo-relative paths;
`dc/Makefile` swaps those in per-TU. `src/` is not touched and nothing is
committed — this is a throwaway image, thrown away when S4 lands. Sections with
the stub: text 5,794,828 / data 2,638,852 / bss 3,939,828.

**The corollary in the next section is now discharged: the trampoline is
tested.** The section after this one describes the unstubbed image, which is
unchanged.

## Boot status — failure fully explained

`harness/dc/smoke.sh` on the real CDI: **timeout, zero bytes of console
output.** Attributed by controlled experiment, not inference:

| image | `.bss` | end | result |
|---|---:|---|---|
| `selftest.cdi` (control) | 22,728 | `0x8c048948` | PASS 3.10 s |
| hello-world + 4.7 MB bss | 4,722,728 | `0x8c4c40a8` | PASS 3.08 s |
| hello-world + 21 MB bss | 21,022,728 | `0x8d44f888` | **FAIL, 0 bytes** |
| `OpenCrossing.cdi` | 12,415,508 | `0x8d472874` | **FAIL, 0 bytes** |

A stock KOS hello-world containing *nothing but* a big array fails identically
at the same image end. **The silence is size alone** — not a game fault, and
not the `dc_main.c` trampoline. Startup zeroing runs off physical memory before
`scif_init()`, so the guest never executes an instruction. There is no crash to
symbolise until the image fits.

Corollary: the trampoline is still **untested**, merely not implicated.

