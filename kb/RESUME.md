# RESUME — pick the session back up here

Rewritten 2026-08-03. Read `kb/STATE.md` next; this file is the unfinished part
plus the gotchas that would cost a fresh context an hour. Full narrative:
`kb/state-log.md`, top entry.

## 0. Everything is committed

`git status` is clean on `dev`. The last six commits are this session:
the regression gate, `DC_SCIF_FAST`, the alpha texture-env fix, the NPOT
closure, the reply-box asset fix, and the alpha-env default flip.
**The main thread commits; agents do not.**

## 0b. THE THREE RULES THIS SESSION ADDED

1. **`grep 'ASSET MISSING' <run>/console.log` must be empty before you believe
   any visual comparison.** A renderer A/B against a build that is missing
   assets does not measure the renderer — that mistake shipped one fix in the
   OFF position for half a session.
2. **Judge a renderer change on a screenshot pair, not on counters.** The
   alpha-env change passed frames, FPS, `ptdrop`, `LOST` and blank-texture
   count while turning the station canopy into a flat slab.
3. **Total frames is NOT a progression metric.** The town is ~11 FPS and the
   train intro ~30, so reaching the town sooner *lowers* the frame count. Two
   runs of one build measured 10,499 vs 7,979. Use `deepest_scene`.

## 1. Where the port is

### ⭐ IT BOOTS ON REAL HARDWARE (2026-08-03)

A padded CD-R burn of `dev` boots on the user's retail Dreamcast and **stops at
the K.K. Slider / player-select scene**. Three candidates, in order of
likelihood, and they are very different:

1. **Input never arrives** — that burn had `DC_AUTOSTART` unset, so it waits on
   a real controller through `PADRead`. `dc_pad.c`.
2. **The disc, not a hang** — the ARAM pager reads off a CD-R at ~500 KB/s with
   real seeks, and every Flycast run has `FastGDRomLoad=yes`. Player-select is
   exactly where `forest_1st`/`forest_2nd` page in (57 reads / 2,742,848 B on
   the last emulator run). On hardware that is minutes.
3. **A genuine hardware-only hang** — the interesting one. Told apart from (2)
   by whether the scene is still animating.

A `DC_AUTOSTART=300` + `DC_CDI_PAD=1` disc was handed over to separate them;
the decision table is in its README. **Await that result before theorising.**

⚠️ **Never put `DC_SCIF_FAST` in a hardware build** — a coder's cable will not
sync at 1.5 Mbps and the console, crash dumps included, is lost.
⚠️ **`DC_CDI_PAD=1` for burns.** The 740 MB file is raw 2352-byte sectors =
314,663 sectors = 69.9 min, so it fits a 74-min CD-R despite the byte count.

### In the emulator

**The port reaches the TOWN.** `[SCENE_MODE] 0 → 3 → 4 → 18 → 9`; mode 9 is
`mFI_FIELD_FG` + `mEv_CheckFirstIntro()` TRUE (`m_field_make.c:1292`) = SCENE_FG,
the outdoor field. Title → player-select (K.K. on the lit stage) → train intro
with Rover → name entry → town, unattended, in one run.

Numbers on the last full run at HEAD (2026-08-03): **~10,500 frames / 600 s**,
town ~11 FPS, earlier scenes 22-30 FPS, `ASSET MISSING` 0, blank texture uploads
**0 of 306**, `ptdrop` 0, `LOST` 0. Binary end `0x8caa4fd0`, so the image span
is 11,096,016 B and the fit holds.

⚠️ Frame counts across runs are NOT comparable — see rule 3 above.

**Verified by screenshot at HEAD:** K.K. Slider and his dialogue; Rover, the
dialogue balloon (a solid cream oval, correct) and the name plate; the reply
box with its cursor; the name-entry keyboard; the train interior; the station
exterior with a correctly textured canopy, the clock, flowers and brick paving.

**Open, reported by a human against HEAD and NOT yet fixed** — these are the
next jobs:

1. **The text-input (name entry) screen looks very wrong.** Not yet
   investigated. It renders and is legible in a 320x240 capture, so start by
   getting a screenshot of what the human is seeing rather than assuming.
2. **The text while Rover is on the phone looks wrong.** Not yet investigated.
   Likely the same font/TEV family as items in §5.
3. **The scenery band through the train window ("the mountains look messed
   up").** Diagnosed, patch written and OFF — see §5a.

## 2. The build line — use this, do not re-derive it

```bash
DC_STUB_KEEP="$(grep -v '^#' tools/dcstub/keeplist-opening.txt | paste -sd: -)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=131072 DC_ARENA_BYTES=1900000 DC_AUTOSTART=300 \
  bash dc/build-dc.sh
bash harness/dc/smoke.sh dc/build/OpenCrossing.cdi --timeout 600 -c config:LimitFPS=no
python3 tools/dcqa/run_report.py <run>/console.log --vs <baseline>/console.log
```

**Always `--timeout 600` and `-c config:LimitFPS=no`.** The town is ~4,000
frames in; 240 s does not get there, and `LimitFPS=no` is the user's own
play-testing setting.

### Screenshots — now cheap, use them on every renderer change

```bash
DC_SCIF_FAST=1 DC_FB_PROBE=400 DC_FB_IMAGE=2 DC_TEX_LOG=1   # add to the build
bash harness/dc/smoke.sh <img> --timeout 600 --fb-writeback -c config:LimitFPS=no
python3 tools/dcfb/fbimg_to_png.py <run>/console.log --out /tmp/shots
```

`DC_SCIF_FAST=1` raises the console from 57,600 to 1,562,500 baud, so a 320x240
capture costs ~1.4 s instead of ~35 s. **The old warning that "a screenshot run
is not a progression run" no longer applies when it is set** — a 600 s
screenshot run reaches the town like any other. ⚠️ Emulator only; a real
coder's cable will not sync at 1.5 Mbps, so never put it in a hardware build.

⚠️ **Build to a copy before a long run.** Flycast holds
`dc/build/OpenCrossing.cdi` open; `cp` it to the scratchpad and run the copy.

⚠️ **Never edit the tree while a build is running**, and never run two builds
at once — `dc/build/` is one shared object tree.

The trailing `-` in `paste -sd: -` is required; BSD paste will not read stdin
without it.

## 2b. What the second session changed (details in `kb/state-log.md`)

- **`dc_pad.c` — A-dominant autostart.** START/A was 1:1; past the title, START
  advances almost nothing. This is what reached the town.
  `DC_AUTOSTART_START_EVERY=2` restores the old pattern.
- **`dc_misc.c` — `vprintf` now goes through the flood limiter.** emu64's
  `Printf0` bypassed the `printf` override and cost **8× the frame rate** in
  town. See `kb/traps.md`.
- **`dc_pvr.c` — three fixes:** `GX_TEXMAP_NULL` no longer inherits a texture;
  alpha-tested cutouts blend instead of painting transparent texels opaque;
  `GXSetColorUpdate(FALSE)` is honoured as `src=ZERO dst=ONE`.
- **`dc_aram.c` — short/failed reads no longer cache zeros as authoritative.**
- **`dc_audio.c` + `dc_vi.c` — real `snd_stream` device and a budgeted pump.**
  Ships **`DC_AUDIO=0` by default is NOT set** — audio is ON; use
  `DC_XDEFS='-DDC_AUDIO=0'` to remove it. It is harmless but produces silence.
- **keep list 77 → 107 files**, incl. the hand-added `boy_model.c`.

## 3. What was fixed in session 1 (do not re-investigate)

All four were the same shape: **state that is recorded and never consumed.**

1. **GX wrap mode** — stored in `TEXOBJ_WRAP_S/T` since M1, never read;
   `dc_pvr.c` hardcoded `PVR_UVCLAMP_NONE`, so every texture repeated. The
   spotlight was drawn 2.7× across the frame.
2. **TEV constant colours** — `g_gx.tev_colors[]` stored by `GXSetTevColor`,
   never read. The opening's shade quad is `(0,0,0,PRIMITIVE)` with
   `PRIM = BLACK`, so a black vignette rendered **white** over 27.9 % of the
   frame. Now 0.0 %.
3. **The keep list** — 77 of 117 texture uploads decoded to all-zero. The
   animals had never been in the image. Re-censused: 31 files / 90 asset loads
   → 76 files / 779.
4. **`.c_inc` files** — invisible to `make_stub_data.py` (globs `*.c`), so the
   dialogue balloon's arrays got a `.bss` buffer and no loader. Both tools now
   handle them.

Narrative and numbers: `kb/state-log.md` top entry. Gotchas: `kb/traps.md`.

## 4. Instrumentation built this session

| knob | what it answers |
|---|---|
| `DC_FB_IMAGE=<1\|2\|4>` + `tools/dcfb/fbimg_to_png.py` | what the frame actually looks like |
| `DC_PVR_BATCH_LOG=<N>` | per-batch tex/wrap/blend/cull/z + emitted screen bbox. Attributes a region of a PNG to the state that drew it |
| `DC_TEX_LOG=1` | what each texture upload *decoded to*. Separates "missing asset" from "renderer bug" — this is what cracked items 3 and 4 |
| `DC_XDEFS='-D...'` | raw defines, so the renderer kill switches are reachable from a command line |

Kill switches: `DC_PVR_NO_UVCLAMP`, `DC_PVR_NO_TEVCONST`, `DC_PVR_NO_CULL`,
`DC_PVR_CULL_INVERT`, `DC_PVR_NO_LIGHTING`, `DC_PVR_NO_NEARCLIP`,
`DC_PVR_NO_TEXTURES`. See `BUILDING-DC.md`.

## 5. Open, in priority order — REWRITTEN 2026-08-02 (second session)

### 1. ✅ DONE 2026-08-02 (session 3) — the punch-through list is IN, and it exposed a bigger bug.

`opb_sizes[4] = PVR_BINSIZE_32`, cutouts route to `PVR_LIST_PT_POLY`, threshold
pinned to 144, geometry buffered until the base list closes and replayed.
Measured over a 600 s run: `pt batches=159046 verts=6011391 pthi=1078/2048
ptdrop=0` — no overflow, buffer half used, 65,536 B of static `.bss`.

**⚠️ The thing that made it work is NOT in the original write-up below.**
`cxt.txr.env` is `PVR_TXRENV_MODULATEALPHA`, so the alpha the PT comparator
tests is `vertex_alpha × texel_alpha`. **On the N64 those two were never
multiplied.** Every alpha-tested display list in this game runs
`G_RM_FOG_SHADE_A`, where the vertex alpha byte is the per-vertex **fog
coefficient**, and the alpha half of the combiner is `(0,0,0,TEXEL0)` — texel
alpha alone. Before a real alpha test the wrong product only made things faint.
With one, it deletes them:

| model | vertex alpha | result |
|---|---|---|
| `obj_romtrain_door_v[0..7]` (the door leaf) | **0** | whole door discarded |
| `rom_train_out_v[8..15]` (the window's tunnel mask) | **50** | mask discarded, raw sky/cloud/tree scenery exposed as "a big weird light texture" |
| `obj_romtrain_glass_model` | 255, and XLU | never routed to PT — which is why "the glass is still there" |

Decoded from the retail `foresta.rel` and confirmed in a batch log: every frame
carries `pt=1 … argb=32323232` on a 64×32 with a window-sized bbox and
`pt=1 … argb=005A5096` on a 32×64 door-shaped one. Fixed by forcing vertex
alpha opaque on **textured** PT batches (an untextured PT poly has no texel
alpha, so there the vertex alpha genuinely is what GX would have tested).
Kill switch `-DDC_PVR_PT_KEEP_VTXALPHA`.

⚠️ **`MODULATEALPHA` is wrong for this game's alpha combiner everywhere, not
only on PT** — no display list read so far puts `SHADE` in the alpha combiner.
The non-PT cutout path has the same defect. Wider blast radius, wants its own
measured pass.

**A/B settled: PT stays ON.** With `-DDC_PVR_NO_PUNCHTHRU` the door is *still*
missing **and** the trees draw in front of the train window again — the
original bug PT exists to fix. PT was never the cause; it made a pre-existing
alpha error fatal instead of subtle.

<details><summary>the original item 1, for the reasoning that led here</summary>

#### THE PUNCH-THROUGH LIST. This is the next job, and it is blocking two visible bugs.

**The train door is broken** (human-confirmed, current build). The cause is
understood and it is structural, not a guess:

`alpha_ref` — 144 by default (`emu64.c:718`) — is read only to decide THAT an
alpha test exists (`alpha_test_active()`, `dc_pvr.c`), **never applied as a
threshold**, because the PVR has no alpha test outside the punch-through list.
So for a door whose window openings are punched by alpha:

- `depth.write = true`  → the transparent holes write depth and **occlude the
  scenery behind them** (this is the current state).
- `depth.write = false` → the door writes no depth at all, and since everything
  lives in ONE submission-ordered list with autosort off, all the later XLU
  window scenery **paints straight through the closed door** (this was the
  first attempt, and it is what "trees drew over the door" was).

**Neither extreme is correct.** Both were tried and both were observed. The fix
is a real alpha test = `PVR_LIST_PT_POLY`.

What is known about doing it:
- `opb_sizes[4]` is `PVR_BINSIZE_0` (`dc_pvr.c`), so the PT list does not exist
  yet. Enabling it costs VRAM only, not main RAM.
- **`PVR_LIST_PT_POLY = 4`, i.e. LAST in KOS's enum** (verified in the SDK
  image, `dc/pvr/pvr_header.h:65`). Lists must be submitted in increasing order
  and "can never be opened again within a single frame once closed"
  (`pvr.h:945`). So the frame becomes TR (everything else) → PT (cutouts),
  which means **cutout geometry must be buffered until the TR list closes**.
  Cutouts are a measured **13.6 % of batches (316 of 2331)**, so the buffer is
  small — but it is main RAM, which is the project's blocking constraint.
  Size it from a real run before allocating.
- The PT alpha threshold is one global register, set once per render, not per
  poly. Pin it to 144 to match `tex_edge_alpha`.
- Kill switch `-DDC_PVR_NO_PUNCHTHRU` must restore today's behaviour verbatim.
- Design notes: `kb/tev-map-alpha.md`.

</details>

### 2. ❌ CLOSED 2026-08-02 (session 3) — the window scroll is NOT a UV bug. It is item 1.

**The texture-matrix chain is correct end to end, and this was verified
numerically, not argued.** `dc/src/dc_mtx.c:474` is term-for-term identical to
the real GC SDK `C_MTXLightOrtho` (`src/static/dolphin/mtx/mtx.c:544`) and to
`pc/src/pc_mtx.c:259`. No transposition: `dc_gx.c:881` memcpys 12 floats
row-major, `apply_texgen` reads rows 0/1 — the same layout `pc_gx.c:1201` feeds
`u_texmtx_row0/row1` — and `apply_texgen`'s `row·(s,t,0,1)` is byte-identical to
`pc/shaders/default.vert:68-73`.

The derivation predicts `u ∈ [m[0][3], m[0][3]+1.0]`, `v ∈ [0.015625, 1.015625]`
from bgtree vertices read out of the retail `foresta.rel`. **An existing run
already contained the answer** — `smoke-t1-20260802-172246-21069/console.log:1718`:

```
BATCH b=150790 TRI n=12 verts=12 tex=1 128x32 wrap=1,1 bm=1,4,5 zt=1 zf=1 zw=0
  argb=D4D4D4D4  bbox=-1215.9,-182.7..247.8,901.9  uv=-2.80,0.02..-1.80,1.02
```

`u` span exactly 1.00, `v` = 0.02..1.02, and `m[0][3] = -(8·1435-16)/4096 =
-2.7988` matches the logged `-2.80`. **The "derived, unverified U to about −4,
V in [0.016, 1.016]" figures in the old version of this item were never a
symptom — they are the correct GameCube values.**

What the symptom really is: on that same line the trees are XLU with `zw=0`
(no depth write), `zt=1 zf=1` (depth *tested*), and their bbox spans the full
visible height. They are not geometrically above anything. The train wall's
alpha-punched window opening **writes depth at its transparent texels** and
rejects the tree band exactly inside the opening, leaving the band visible only
where no wall covers it. **This is item 1 wearing a third hat.** Do not spend
another session on the texgen path.

Two real but currently **inert** divergences were found while walking it, and
they are worth knowing before someone re-derives them: `dc_pvr.c:1066` drops the
texture matrix's third column (GX expands a `GX_TG_TEX0` source to
`(s,t,1.0,1.0)`, which is why `C_MTXLightPerspective`/`Frustum` park their
translation in `m[*][2]`; `C_MTXLightOrtho` writes 0 there, and
`pc/shaders/default.vert:68` has the same omission, so it is not a regression);
and `dc_pvr.c:1064`'s `cv[k].u *= tex->u_scale` cannot express `GX_REPEAT` on an
NPOT texture at all — scaling a `u` of −2.80 lands on a different texel rather
than repeating. Both are inert for the train window (128×32 is POT, `us=1.000`).
The NPOT one has no patch: the fix is edge/period replication when padding.

<details><summary>the original, now-falsified item 2</summary>

Human-confirmed on the current build; the trees themselves now scroll. This is a
UV / texture-matrix **offset** error, not wrap and not format — both of those
were investigated and cleared:

- Wrap is innocent: `rom_train_out.c:105` sets the trees `GX_REPEAT/GX_REPEAT`,
  which `wrap_gx_to_pvr` maps to `PVR_UVCLAMP_NONE` — byte-identical to the old
  hardcode. **Do not "fix" `wrap_gx_to_pvr` for this.**
- Every train texture format decodes fine (CI4/I4/I8 all have real decoders).
- The texture matrix IS implemented and IS applied (`apply_texgen`,
  `dc_pvr.c`), and emu64 passes exactly `GX_TG_TEX0`.

The mechanism to check: `Train_Window_Actor_move` (`ac_train_window.c:281`) does
`TreeScrollx += 5` forever; `tex_scroll2` (`m_rcp.c:326-336`) turns it into a
`gDPSetTileSize` offset; emu64 folds `sl/tl` into an ortho matrix via
`C_MTXLightOrtho` and `GXLoadTexMtxImm(GX_TEXMTX0)` (`emu64.c:2623-2641`).
**Derived (inference, unverified): U sweeps to about −4 with a ~310-frame
period, V stays in [0.016, 1.016].** A V offset that puts the band above the
window points at the `m[1][3]` term or at `C_MTXLightOrtho`'s translation
convention on DC (`dc_mtx.c:474`). Dump the tree batch's `uv=` from
`DC_PVR_BATCH_LOG=1` and compare against that derivation — that is one run.

</details>

### 5a. The train window's scenery band — diagnosed, patch built, OFF by default

This is the human's "the mountains behind the train look messed up".

`rom_train_out_bgtree_modelT` (`rom_train_out.c:99`) is
`gsDPSetCombineLERP(PRIMITIVE, 0, PRIM_LOD_FRAC, ENVIRONMENT, 0,0,0,TEXEL0,
TEXEL1, 0, COMBINED, 0, 0,0,0, COMBINED)`, and emu64's hand-written case
(`emu64.c:1753-1763`) makes stage 0 `(a,b,c,d) = (ZERO, C1, A0, C2)` =
**ENV + PRIM_LOD_FRAC x PRIM**: a pure constant, no texture and no raster term.
PRIM is literally the time-of-day sun+ambient colour
(`aTrainWindow_SetLightPrimColorDetail`, `ac_train_window.c:435-485`, rewritten
every frame) and ENV is `gsDPSetEnvColor(60, 60, 35, 255)`, the darkening that
makes the band read as distant scenery.

`tev_const_color()` rejects it at its first test — `color_b == GX_CC_C1`, not
ZERO — so the port draws `vtx.cn * T0`: no ENV darkening, no day/night, a
washed-out band.

**`-DDC_PVR_TEVFOLD` fixes stage 0 exactly.** It generalises the narrow shape
to the whole affine stage, `d + (1-c)a + c*b` over
`{ZERO, ONE, HALF, C0..C2, A0..A2, KONST, RASC}`, folded into the vertex colour
as `K0 + K1*RASC`. It runs only where the old shape declined, so every batch the
old code handled keeps its exact result. Measured: no regression, and the window
scenery visibly darkens — the ENV term arriving.

**It is OFF because a screenshot says it REGRESSES.** With the fold on, the
train station canopy renders as a **flat teal slab with no texture at all**,
where the identical build without it shows correctly textured beams. Every
counter passed. A flat *untextured* result does not fit "the vertex colour was
replaced by a constant" — that would still be modulated by the texel — so the
likely shape is `dc_pvr.c`'s `GX_TEXMAP_NULL` guard, which reads **stage 0
only**: a config that binds `GX_TEXMAP_NULL` on stage 0 and carries the texture
on stage 1 (`emu64.c:1764-1773` is exactly that, true output
`T0 * lerp(RASC, C1, A0)`) gets `tex = NULL`, draws untextured, and the folded
constant becomes the whole colour. **Fix that guard — "does ANY stage bind a
texmap" — before touching the fold again.**

It is also not yet exact even where it does fire.** For the P3 shapes whose second stage is `CPREV + TEXC*CPREV` — the band
among them — GX computes `K*(1 + T0)` and this computes `K*T0`, i.e. about K too
dark. Today's error is "wrong hue, no day/night"; the fold's is "right hue,
right day/night, too dark". Neither is correct.

**The exact completion is the PVR's OFFSET COLOUR.** `oargb` is added after the
texture env, so `col = K` with `oargb = K` gives `K(1 + T0)` exactly.
`dc_pvr.c` writes `pv.oargb = 0` unconditionally and never enables
`cxt.gen.specular`. That is the next concrete step for this item, and it needs
`oargb` folded into `header_key()` like every other header bit.

### 2b. Two train-station bugs, traced 2026-08-02 — read `kb/station-bugs.md`.

- **The station floor (and the whole town ground) is black — SOLVED, fix not
  applied.** Keep-list gap: the acre draws from `station_tex_dummy`
  (`m_bg_tex.c`, bare `.bss`), filled by `bcopy` from `mFM_grd_s_station.c` /
  `mFM_grd_s_station1_pal.c`, which are stubbed to zeros. The census can never
  see the sources — it resolves the *dummy* symbol, which is why the list keeps
  the useless `m_bg_tex.c` and not the data. Fix = ~27 `mFM_grd_*.c` keep-list
  additions (≈60-90 KB) + `grd_s_t_st1_2`; structural fix = a dummy→source
  alias table in `census_keeplist.py`. Every common ground texture (grass,
  earth, cliff…) is the same bug.
- **The player clips through the station roof — NOT pinned.** Assets, render
  state, depth mapping and the palette all check clean (walked end to end in
  the doc); with the derived state, clip-through is impossible in either
  submission order, so something in the *station frame* diverges. Three ranked
  hypotheses (state leak / cutout-edge ghosting = item 1 in disguise / roof
  geometry misplaced) and the single batch-log + screenshot run that separates
  them are written down — run that before touching any code.

### 3. ✅ DONE 2026-08-02 (session 3) — hardware fog, and the mapping is exact.

`PVR_FOG_TABLE` with a custom 129-entry table. **Exact, not fitted:** table
entry *j* stands for a known scaled 1/w, so setting `FOG_DENSITY = endz` puts
entry *j* at eye depth `endz/t(j)` in closed form; `emit_projected` already
writes `1/w` as vertex depth, and `w` is the fourth row of the GC projection,
which `GXSetProjection` forces to `(0,0,-1,0)` for `GX_PERSPECTIVE`
(`dc_gx.c:835-839`) — the same quantity and units as emu64's `startz`/`endz`.
GX's `nearz`/`farz` exist only to invert a depth-buffer value back to eye z, and
this backend never leaves eye z, so ignoring them is correct.

Vertex fog was not an option: KOS's `pvr_fog_vertex_color()` is an
`assert_msg(0, "not implemented")` stub. Table fog costs nothing per vertex.
The registers are global and must not be written between `pvr_scene_begin` and
`pvr_scene_finish`, so batches latch and `frame_begin` programs after
`pvr_wait_ready` — a parameter change lands one frame late; on/off is a header
bit and has no latency. `pvr_fog_table_color`'s alpha argument is pinned to 1.0
deliberately: KOS cannot set alpha in `FOG_TABLE_COLOR` and fakes it by scaling
every table entry, which makes that argument a fog *strength*.

Measured live: `ask=1 hw=1 start=585→625 end=1786→1800`, colour dusk-blue then
night-blue, `progs=2`. ⚠️ **`batches=0` for the entire train sequence** (frames
0-9900); only the town fogs, and there it is all 89 draws. So emu64's
`G_BL_CLR_FOG && G_FOG && fog_zmult != 0 && aflags[4] == 0` guard is much more
selective than "every train model carries `G_FOG`" suggested. ~1 KB of image,
0 VRAM, 0 heap. Kill switch `-DDC_PVR_NO_FOG`, diagnostic `-DDC_PVR_FOG_LOG=<N>`.

<details><summary>the original item 3</summary>

#### Fog is entirely unimplemented.
`emu64.c:3219` really does ask for `GX_FOG_PERSP_LIN` with live near/far/colour;
`grep fog dc/src/dc_pvr.c` returns nothing, and `fog_type/start/end/near/far/
color` are all in the never-consumed list. Every train model carries `G_FOG`.
The PVR does fog in hardware. Cosmetic — it cannot make geometry disappear, so
it ranks below the two above.

</details>

### 4. ✅ FIXED 2026-08-02 (session 3) — and it was never the loader.

**Human-confirmed working:** K.K.'s name renders and the "next" arrow appears.

The pairing was the diagnosis. The speaker name (`m_msg_draw_window.c_inc:48`)
and the replies (`m_choice_draw.c_inc:146`) are the **only** text in the balloon
that goes through `mFont_SetLineStrings_AndSpace`, which sets
`mFont_SENTENCE_FLAG_USE_POLY` unconditionally (`m_font_main.c_inc:518`) and so
draws real geometry (`mFont_gppDrawCharPoly`) instead of a texture rectangle.
The body text takes the rect path and always worked.

`mFont_SetVertex_dol` (`m_font_main.c_inc:348-362`) writes `cn[0..3] = 0` into
every glyph vertex, and the font display list clears `G_LIGHTING`, so emu64
programs `GXSetChanCtrl(..., GX_SRC_VTX, …)` (`emu64.c:3327`) — material source
VTX. `shade_vertex()` returned `0x00000000` and `MODULATEALPHA` multiplied the
glyph away. On GX that zero is harmless: `mFont_CC_FONT` (`m_font.c:17`) is
colour `= PRIMITIVE`, alpha `= PRIMITIVE.a × TEXEL0.a`, and never reads `RASC`
or `RASA`. `dc_pvr.c` had restored the RGB half of that constant and not the
alpha half — so this is the **fifth** member of the recorded-but-never-consumed
family. The rect path escaped because `emu64::draw_rectangle` re-programs
`GX_SRC_REG` and never declares `GX_VA_CLR0`, shading to a material register
emu64 sets white once and never touches again.

Confirmed before the patch was written: 47 balloon batches at `verts=6 zt=0
st=1 tm=0,255` reading `argb=00001E00`, alpha byte zero, with body-text batches
on the same frame at `argb=FFxxxxxx` as the control.

⚠️ The matched shape (`a=ZERO, c=TEXA, d=ZERO, b=const`) occurs **446 times** in
`src/`. Everywhere it fires the constant is the correct answer, but it reaches
far more than the font. `-DDC_PVR_TEVCONST_ALPHA_RESCUE_ONLY` narrows it to
vertices that are already fully invisible. Kill switch
`-DDC_PVR_NO_TEVCONST_ALPHA`.

**`DC_ARAM_TBL_PROBE` was not needed and its premise was wrong** — but the probe
was rewritten this session and is now genuinely able to answer a loader
question, so keep it for the next one. See the commit for the decision table.

<details><summary>the original item 4 — the loader theory, now closed</summary>

#### The speaker NAME and the REPLY/choice text never render.
Body dialogue text renders fine. Body = `RESOURCE_MESSAGE` in **forest_2nd**;
name = `RESOURCE_STRING` and choices = `RESOURCE_SELECT`, both in **forest_1st**
(split at `jsyswrap.cpp:450-460`). Ruled out already:
- forest_1st IS fully mapped — `851,744 + 4,130,656 = 4,982,400` = the reported
  `mapped=`, exactly. Two extents cover both archives.
- The silent-zero ARAM bug was real and is fixed, but **`SHORT READ = 0`**, so
  it was not this.
- The table files are RARC members inside the `.arc`; they cannot be missing.

**Next step is `DC_ARAM_TBL_PROBE=1`** (already written into `dc_aram.c`, needs
a `-D` to enable). It logs every 64-byte ARAM read — uniquely
`mMsg_Get_BodyParam`'s table fetch (`m_msg_main.c_inc:284,289`) — with the
address, the extent index and the first three words. Since MESSAGE (works) and
STRING/SELECT (broken) both go through it, one run gives the working control and
the broken case side by side. Decision table is in the comment at the probe.

</details>

### 5. Cutout edges carry a halo.
Consequence of item 1: texels with alpha between 1 and `alpha_ref` were
discarded on GC and are drawn semi-transparently here. Fixed for free by the
punch-through list.

### 6. TEV proper — but it is a 9 % problem, not a 52 % one.
53 of 101 configs are multi-stage and collapse to stage 0. Measured: 1209 of
1231 two-stage batches request texmap1, but only **220 bind a genuinely
different image**; the rest point both texmaps at the same tile (N64 LOD
interpolation, which the PVR does in hardware and which is free to drop).
Produces wrong COLOUR, not wrong occlusion — **do not chase it for a layering
symptom.** Known instance: the tree band's ENV/PRIM tint
(`emu64.c:1753-1763`) is lost, so day/night fade on the window scenery is wrong.

### 7. `DC_SRC_SHRINK=0` is broken — renders nothing (`batches=23 draws=0`).
The lever stays on; worth knowing it is not a valid A/B control.

### 8. Audio: real pipe, no sound. Deliberately parked by the user.
Device up, AICA pulling, but `[NEOS_OUT] peak=0` — synthesis runs on silence
because `dc_aram.c` discards every ARAM write below `aram_audio_end`, throwing
`audiorom.img` away. `DC_ARAM_AUDIO_DROP=0` lets it through: **unproven, and it
risks the extent-table ordering failure documented at that guard — A/B the
`[DC/ARAM] LRU` line and require `LOST=0` and unchanged `mapped=`.**
Second open thread: `fill=` sat at 4480 with `cb=2` for an entire 600 s run, so
the consumer stalls after two callbacks; cause unknown. `DC_AUDIO=0` removes all
of it.

## 6. Environment

The auto-mode classifier that blocked every `docker` command last session is
**gone** — a fresh context cleared it, no `/permissions` change needed. Docker,
the SDK image, the build and the harness all work.

⚠️ **A short run is usually the human closing the emulator window, not a hang.**
One 479-frame run was diagnosed as an audio deadlock and was not; the user had
ended it. Ask before bisecting a short run.

## 7. Closed this session, with the evidence

- **The near-plane clipper works.** `clipped=1798` over 6.69M triangles on a
  600 s run. The old `clipped=0` came from short, 2D-heavy runs: emu64 forces
  `GX_ORTHOGRAPHIC` for every rect path, and ortho gives `w ≡ 1`, which cannot
  trip a `w <= EPS` test. Not a bug.
- **The ortho `z ≡ 1.0` depth collapse is harmless.** Measured over 314 logged
  batches: every depth-TESTED batch carries real perspective z, and every
  collapsed-z batch has depth test OFF (`zt=0 zf=7 zw=0`). Zero overlap.
- **The "invisible" quads draw.** The batch log shows sane bboxes and
  `verts=6` per quad; the 50→3 alternation is dialogue glyphs appearing and
  disappearing.
- **The SE slot leak is not a DC bug.** `Nap_ReadSubPort` returns -1 while the
  group is disabled (`sub_sys.c:426`), the free test is `!p5`
  (`game64.c_inc:1026`), and the sequencer never ran because audio never ticked.
- **The 11 blank uploads were a keep-list gap**, now 15 blank of 269 uploads
  (9.2 % → 5.6 %) after the town census.
