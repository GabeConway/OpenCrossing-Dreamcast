# RESUME — pick the session back up here

Rewritten 2026-08-02, end of the **"the town is reachable"** session (the
second of that date). Read `kb/STATE.md` next; this file is only the unfinished
part plus the gotchas that would cost a fresh context an hour. Full narrative:
`kb/state-log.md`, top entry.

## 0. ⚠️ NOTHING IS COMMITTED

`git status` shows 10 modified files — `dc/src/{dc_pvr,dc_aram,dc_audio,dc_misc,
dc_pad,dc_vi}.c`, `dc/include/dc_platform.h`, `tools/dcstub/keeplist-opening.txt`,
`kb/RESUME.md`, `kb/traps.md`, plus this session's `kb/state-log.md`. All of it
compiles and runs. **The main thread commits; agents do not.**

## 1. Where the port is

**The port reaches the TOWN.** `[SCENE_MODE] 0 → 3 → 4 → 18 → 9`; mode 9 is
`mFI_FIELD_FG` + `mEv_CheckFirstIntro()` TRUE (`m_field_make.c:1292`) = SCENE_FG,
the outdoor field. Title → player-select (K.K. on the lit stage) → train intro
with Rover → name entry → town, unattended, in one run.

Numbers on the last full run: **16,889 frames / 600 s**, town ~12 FPS, earlier
scenes 20-29 FPS, `image_span` 10,699,616 B, `margin` 3,588,448 B, fit OK.

Human verdict on the current build: K.K. correct, Rover correct, scrolling trees
and glass present. **Still wrong: the train door, and the scrolling window
texture sits entirely ABOVE the window.**

## 2. The build line — use this, do not re-derive it

```bash
DC_STUB_KEEP="$(grep -v '^#' tools/dcstub/keeplist-opening.txt | paste -sd: -)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=131072 DC_ARENA_BYTES=1900000 DC_AUTOSTART=300 \
  bash dc/build-dc.sh
bash harness/dc/smoke.sh dc/build/OpenCrossing.cdi --timeout 600 -c config:LimitFPS=no
```

**Use a 600 s timeout and `-c config:LimitFPS=no`.** The town is ~4,000 frames
in; 240 s does not get there. `LimitFPS=no` unlocks the frame limiter (the
harness passes `-c` straight to Flycast) and is the user's own play-testing
setting.

Add `DC_FB_PROBE=200 DC_FB_IMAGE=2` **and** `--fb-writeback` to get screenshots,
then `python3 tools/dcfb/fbimg_to_png.py <run>/console.log --out /tmp/shots`.

⚠️ **A screenshot run is not a progression run.** `DC_FB_IMAGE` streams ~205 KB
of base64 per frame over a 57600-baud SCIF and eats ~150 s of a 200 s timeout —
the same build reaches **5069 frames without it versus 1379 with it**. Never
judge how far the game gets from a screenshot run.

The trailing `-` in `paste -sd: -` is required; BSD paste on macOS will not read
stdin without it.

⚠️ **Build to a copy before a long run.** Flycast holds `dc/build/OpenCrossing.cdi`
open; `cp` it to the scratchpad and run the copy, so the next build is not
blocked for 10 minutes.

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

### 1. ⭐ THE PUNCH-THROUGH LIST. This is the next job, and it is blocking two visible bugs.

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

### 3. Fog is entirely unimplemented.
`emu64.c:3219` really does ask for `GX_FOG_PERSP_LIN` with live near/far/colour;
`grep fog dc/src/dc_pvr.c` returns nothing, and `fog_type/start/end/near/far/
color` are all in the never-consumed list. Every train model carries `G_FOG`.
The PVR does fog in hardware. Cosmetic — it cannot make geometry disappear, so
it ranks below the two above.

### 4. The speaker NAME and the REPLY/choice text never render.
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
