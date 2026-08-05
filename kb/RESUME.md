# RESUME — pick the session back up here

Rewritten 2026-08-04 (session 2), amended session 3. **Read `kb/STATE.md`
first** — session 3's changes are all there and this file is not yet rewritten
around them. Full narrative: `kb/state-log.md`. Everything below §6 is older
material kept because it is still the reasoning behind items that are now
closed.

## SESSION 3 IN TEN LINES

1. **All 84 summer structures render.** They were affordable because every
   `obj_s_*.c` carries winter too and the keep list was buying it: the new
   `path.c#!obj_w_` filter in `make_stub_data.py` drops it. +108,320 B `.bss`,
   no OOM, no regression, confirmed on a screenshot pair.
2. **Regenerating `keeplist-town.txt` used to delete Tom Nook** and 24 other
   hand-typed entries. They are in `EXTRA_SOURCES` now.
3. **Interiors (`rom_*`, `tmp*`, …) are excluded by default** — +269,312 B,
   does not fit yet. `--interiors` turns them on.
4. **`DC_EMU64_HIST` was never forwarded into the container.** G1 was
   unreachable from the documented build line; that is why it was never run.
5. **G1 also armed on the wrong tick** (the frameskipped one, which issues no
   commands). Fixed. **G1 still has to be re-run — there is no per-opcode
   number yet, so nothing in F1/F8/G2/G3 is costed.**
6. **G2 is written** (`dc/src/dc_emu64_shadow.cpp`, off by default) and not yet
   measured. Its 7-14 ms estimate is **above its own ceiling**; expect 2-5 ms.
7. **`emu64.hpp`'s offset comments are PowerPC and wrong here.**
   `sizeof(emu64)` = 0x2278. Never hand-write an offset map.
8. **`emu64_set_aflags()` is not a seam** — `AFLAGS_MAX` is 0.
9. **`mFM_DecideAcre` does not exist**; it is `mRF_MakeRandomField`, and the
   layout lives in the SAVE, not in per-boot randomness.
10. **`tools/dcfb/shot_diff.py`** makes the screenshot gate mechanical.

⚠️ **Killing a build mid-link corrupts `dc/build/objs.rsp` with NUL bytes** and
the next link fails with `undefined reference to 'main'`. `rm` it and relink;
do not go hunting in your own diff. (`kb/traps.md`.)

## 0. Everything is committed on `dev`

**The main thread commits; agents do not.** Session 2 is these commits:

```
0f269d1 fix(dc): Nook and the houses; and K.K.'s strum is slaved to the music
f6e8a8d feat(dc): keep every summer acre -- the town stops disappearing
7970358 docs(dc): hardware loading is at parity with the emulator
ad2f6da fix(dc): the wide keep list does NOT fit, and MEMLEDGER said OK anyway
a2c0738 docs(dc): correct my own arithmetic on the state-command finding
91da28f test(dc): bench_mem runs clean, and Flycast cannot answer it
2fcaa80 fix(dc): the clock screen had no assets, and autowalk drove the keyboard
3aefe97 fix(dc): most of the town is missing GEOMETRY, not textures
34d3772 feat(dc): print the opcode mix, and 73% of the town frame is state
```

## 0b. THE MEASUREMENT RULES. Now SEVEN, and 6 and 7 were paid for this session.

1. **`grep 'ASSET MISSING' <run>/console.log` must be empty** before you believe
   any visual comparison.
2. **Judge a renderer change on a screenshot pair, not on counters.**
3. **Total frames is NOT a progression metric.** Use `deepest_scene`.
4. ⚠️ **NEVER build a perf run with `DC_FB_PROBE`.** The dump costs **1,506 ms**
   into the `vi` bucket and dragged p1 from 11.56 FPS to 8.50. Screenshot runs
   and perf runs are different experiments.
5. ⚠️ **Estimate from a matched-frame A/B, not from instruction counts.**
6. ⚠️ **`MEMLEDGER FIT … OK` IS NOT A STATEMENT THAT THE IMAGE BOOTS.** `margin=`
   *is* libc's pool and the ledger has no model of libc's demand. A build
   reported `margin=1606292 OK` and died on the splash with
   `Out of memory … diff 1449984`. Plan `.bss` work against the *measured*
   headroom (§2b), never against `margin=`. `kb/heap-two-pools.md`.
7. ⚠️ **An AVERAGE cost per command is not the cost of ANY command.**
   `emu64_ms = 12.31 µs/cmd × cmds + 9.20 ms` is a fit against TOTAL `cmds`,
   which correlates with `vtx`, so the coefficient belongs to the heaviest
   opcode. I applied it to the state-command subset and was wrong by a wide
   margin — 265 `G_VTX` carrying ~6,951 vertices at ~6.9 µs each is ~48 ms,
   i.e. most of the emu64 budget from one opcode. `kb/traps.md`.

## 1. Where the port is

**It boots on real hardware and LOADING IS AT PARITY WITH THE EMULATOR** —
human verdict on the `AC-DC-20260804` burn. That closes the "the CD-R is simply
slow" candidate that has been open since 2026-08-03. ⚠️ Parity on I/O is **not**
parity on compute: the only hardware FPS figure is still "~11 FPS in the town".

In Flycast it reaches the town, walks around it, meets Tom Nook and is taken to
the houses. `[SCENE_MODE] 0 → 3 → 4 → 18 → 9`.

**Current numbers (2026-08-04, probe-free, audio off, opening keep list):**

| | |
|---|---|
| FPS | min 8.0, p50 **24.0**, max 29.9 |
| the town (scene 9) | p50 **~15** |
| `ASSET MISSING` / `ptdrop` / `LOST` | 0 / 0 / 0 |

### THE ARITHMETIC THAT GOVERNS EVERY FPS PLAN

The town frame is **19-27 % renderer** (`gx=`, in `dc/`, editable) and
**77-80 % emu64 traversal + game logic** (in `src/`, closed to editing, compiler
flags banned). **Deleting the renderer entirely takes the worst frames from 11.8
to 15.2 FPS.**

**NEW, and it reframes the FPS problem — the opcode mix, per town frame:**

```
[EMU64] cmds=2867 noop=1 vtx=265 tri=258 dl=250 | cullvis=6 cullrej=3
```

**773 of 2,867 commands do geometry work; 2,094 (73 %) are RDP/RSP state.** See
rule 7 before pricing that. Also `cullvis=6`: emu64's own display-list cull runs
nine times a frame and rejects three.

## 2. The build lines

Common prefix (**note the keep list and the arena — both changed this session**):

```bash
DC_STUB_KEEP="$(grep -v '^#' tools/dcstub/keeplist-town.txt | paste -sd: -)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=131072 DC_ARENA_BYTES=1200000 DC_AUTOSTART=300 DC_SCIF_FAST=1 \
```

**PERF run — no framebuffer probe:**
```bash
  DC_XDEFS='-DDC_PERF_PHASE' bash dc/build-dc.sh
bash harness/dc/smoke.sh <copy>.cdi --timeout 600 -c config:LimitFPS=no
```

**SCREENSHOT run — probe on, `--fb-writeback` REQUIRED, and now it can WALK:**
```bash
  DC_FB_PROBE=150 DC_FB_IMAGE=2 DC_XDEFS='-DDC_PERF_PHASE -DDC_AUTOWALK=1' \
    bash dc/build-dc.sh
bash harness/dc/smoke.sh <copy>.cdi --timeout 900 --fb-writeback -c config:LimitFPS=no
python3 tools/dcfb/fbimg_to_png.py <run>/console.log --out /tmp/shots
```

**RELEASE / burn image:** drop `DC_SCIF_FAST`, `DC_AUTOSTART`, `DC_AUTOWALK` and
every probe; add `DC_CDI_PAD=1`. ⚠️ `DC_SCIF_FAST` on hardware loses the console.

**Verdict on any run:**
`python3 tools/dcqa/run_report.py <run>/console.log --vs <baseline>/console.log`

⚠️ **Build to a COPY of the CDI before a long run.** ⚠️ **Never edit the tree
while a build runs**; never run two builds at once.

## 2b. THE MEMORY ARITHMETIC THAT ACTUALLY BINDS

`margin=` is not headroom. The real number, from the pair of runs that produced
rule 6:

```
libc peak demand  ≈ margin + shortfall = 1,606,292 + 1,449,984 = 3,056,276
margin on the build that booted                                  3,202,932
⇒ real headroom for .bss growth, BEFORE this session            ≈ 146,656 B
```

Two levers were then applied, and they are what paid for the acre fix:

| lever | bytes | evidence |
|---|---|---|
| **arena cut**, `DC_ARENA_BYTES` 1,900,000 → 1,200,000 | +700,000 | `[DC/ARENA] zelda used=289536 free=1124944` — **measured in a loaded TOWN**, the first time that probe has been read anywhere but the title screen |
| **jaudio `.bss` shrink**, keyed to `DC_AUDIO=0` (S8/S9) | +450,368 | the pump is `#if DC_AUDIO` and every downstream function has exactly one caller, so the sequencer never ticks at the default |

Current shipping image: `image_span=11749436 additive_heap=1658752
margin=3237956 OK`, no OOM.

## 3. WHAT WAS WRONG WITH THE TEXTURES — it was mostly GEOMETRY

Human: *"once the character leaves acre one, basically all the textures are
missing."* An acre `.c` stubs its **`Vtx` array** under `TARGET_PC` while its
`Gfx` display list stays real, so an unkept acre executes normally against
all-zero vertices, every triangle collapses to the origin, and the acre draws
nothing. The keep list had **18 of 268**. Same shape for `obj_s_*` and NPC
models — Tom Nook rendered as a **black spiky mess**.

⚠️ **AND A CENSUS CAN NEVER FIX IT.** `src/system/sys_math.c:7` seeds the town
from `sqrand(osGetCount())`, and on DC `osGetCount()` is boot-elapsed time, so
**every boot lays out a different town**. `tools/dcstub/keeplist-town.txt`
(generated by `tools/dcstub/make_keeplist_town.py`) enumerates from the tree
instead: all 371 summer acre TUs, the map overlay, the date/time HUD, Nook and
the raccoon NPCs, `obj_s_house1`, `obj_s_myhome1`.

⚠️ **THE CENSUS HAS A SECOND, SYSTEMIC BLIND SPOT: it only ever sees the
depth-0 branch of every decision.** `DC_AUTOSTART` presses A, every choice menu
defaults to index 0, and the clock screen lives behind index **1**
(`ac_npc_guide_move.c_inc:302-314`) — which is why `tim_win.c` was missing for
two sessions and a human had to report it. Anything gated on
`mEv_CheckFirstIntro() == FALSE` is invisible for the same reason. **The census
mechanism is sound; its driver is the blind spot.** Two cheap fixes are
specced and unbuilt: a zero-asset detector at texture upload (`g_gx.tex_obj_src[]`
already mirrors the source pointer, so `census_resolve.py` symbolises it for
free), and a `DC_AUTOCHOICE` knob that presses Down-then-A on the Nth choice.

## 4. STILL BROKEN, ranked

1. **60 of 72 villager NPC models are still stubbed** — 392 KB more, plus
   ~992 KB of NPC textures. Villagers are randomised per town, so this is the
   same class as the acres and it does **not** fit. S4 territory.
2. **The other 74 `obj_s_*` structures** — 333 KB (shop1, museum, tailor,
   police box, shrine, post office…). Each is one building silhouette.
3. **Winter ground** (`mFM_grd_w_*`, ~70 KB) is deliberately absent. **The town
   floor will go black in December** with the same signature as the bug
   `kb/station-bugs.md` §1 fixed. Dated time bomb; write it down.
4. **The station roof clip-through.** Now REPRODUCIBLE for the first time —
   `DC_AUTOWALK` can walk a character under it, `DC_PVR_BATCH_LOG` prints
   `src=`/`vram=` so a batch joins to a symbol, and `-DDC_PVR_PT_NEAREST` is a
   one-line test of H2. Never run.
5. **`aram zero=7 (2016 B)`** appeared in the walking run and must be 0. Small,
   new, unexplained.

## 5. THE OPEN DECISION — the user's call, not engineering's

`kb/research-fps-ideas.md` carries this in full.

| | what | win | gate |
|---|---|---|---|
| **G1** | per-opcode timing histogram | 0 ms, unlocks the rest | **DONE — `DC_EMU64_HIST=<N>`, in the tree, NEVER RUN** |
| G2 | reimplement the dispatch LOOP in `dc/` at `-O2` | 7-14 ms | **needs sign-off** |
| G3 | `-O2` shadow `dl_G_VTX`/TRI with a runtime AABB cull | **25-35 ms** | **needs sign-off** |

**⚠️ F1 (offline bbox-CULLDL injection) is NO LONGER RECOMMENDED.** A full design
pass found its cost is **594 KB of `.data`** for town scope — 5-16× its own
estimate — because its "50+ vertex" cap selects **zero** chunks (the game uses
the 5-bit N-triangle format exclusively, so no `gsSPVertex` anywhere exceeds
32). It also cannot compute its bboxes without the ROM. It buys a *smaller*
version of what G3 gets for zero bytes. Details and the surviving 212 KB
model-granularity variant are in `kb/research-fps-ideas.md`.

**RUN G1 BEFORE COSTING ANY OF THEM** (rule 7).

## 5b. K.K.'S STRUM IS SLAVED TO THE MUSIC — one cause, two symptoms

Human: *"the silent loading right before kk starts talking"* and *"kk is not
strumming in the intro like he should be"*. Same bug.

`p_sel->strum_timer = 440` (`ac_npc_p_sel_schedule.c_inc:71`) = **7.3 s** before
K.K. offers dialogue. That is his guitar intro, and the game requests the track
every tick (`[TRG_VOL] id=0x044D`). And `ac_npc_p_sel.c:141-148`:

```c
if (animation_id == aNPC_ANIM_4HAKU_E1) {
    sAdos_GetStaffRollInfo(&info);
    if (info.staffroll_part != STAFFROLL_PART_FINISH) {
        speed = 0.0f;          /* animation frozen */
        arm_flag = FALSE;      /* arms disabled    */
        current_frame = 1.0f + 64.0f * info.percent;   /* driven by the SONG */
```

`Na_StaffRollStart` (`game64.c_inc:1512`) fires on **every** BGM start, not just
the credits, latching `start_flag = TRUE`; `Na_GetStaffRollInfo` (`staff.c:37`)
then tests `AG.groups[…].flags.enabled`, false because the sequencer never ticks
at `DC_AUDIO=0`, and returns `PART_START` forever. `info.percent` is written
only at `staff.c:223`, past that early return — so `current_frame` reads an
**uninitialised stack value**.

**`DC_AUDIO_SCENES=3` is the fix** (new this session): the audio pump runs only
in the named scene modes, so the K.K. scene (888 vertices, at the 30 FPS cap)
gets sound while the town (6,951 vertices, 14.9 FPS) does not. Verified arming:
`[DC/AUDIO] gate scene=3 armed=1 gain=256`, 642 frames synthesised,
`arms=1 disarms=1` on the scene change, no OOM.
⚠️ **The strum was NOT visually confirmed** — probes 60 frames apart alias
against a ~112-frame cycle. Needs adjacent-frame capture or a human.
⚠️ `DC_AUDIO=1` disables the S8 shrink by design, costing **455,848 B** of span.

## 5c. RAM — bench_mem is answered, and the answer is "not in Flycast"

`harness/dc/bench/{Makefile,build.sh}` now exist. It **passes** — `end_rc=0`,
every case checksum-verified, nothing hangs. But the numbers are artefacts:
read == write == **114.3 MB/s** at every size in *both* VRAM windows, and the
DMA rows come off 0-2,240 ns samples. Flycast models neither VRAM latency nor
Holly contention. **R1/R3/R4/R5/R6 stay gated, now on a CD-R burn rather than on
an unrun benchmark.** A hardware build must drop `BENCH_BAUD` to 57,600 first.

## 6. What is new in the tree this session

| thing | what it is |
|---|---|
| `[EMU64]` line | the opcode mix, free, under `-DDC_PERF_PHASE` |
| `DC_EMU64_HIST=<N>` | **G1.** Timing thunks swapped into emu64's dispatch table at runtime; `src/` untouched, one `objcopy --globalize-symbol` is the only build change. **Never run.** |
| `DC_AUTOWALK=<N>` | deterministic 8-direction stick walk, gated to `DC_AUTOWALK_SCENE` (default 9, the town). ⚠️ Ungated it drives the NAME-ENTRY KEYBOARD and the run never leaves the intro |
| `DC_AUDIO_SCENES=<list>` | per-scene audio gate; `bash dc/build-dc.sh` derives `DC_AUDIO=1` from it |
| `DC_AUDIO_HEAPLOG` | jaudio heap high-water; the gate on shrinking `audiomemory` further |
| `DC_PVR_TEXNULL_STAGE0_ONLY` | reverts the `GX_TEXMAP_NULL` fix (it read stage 0 only; the one combiner that parks NULL on stage 0 has all six of its `src/` sites in `obj_s_shop1.c`, so the bug was "Nook's shop draws untextured") |
| `DC_PVR_PT_NEAREST` | point-sample the punch-through list — the one-line test of station-roof H2 |
| `DC_MTX_NO_XMTRX_CACHE` | reverts the F2 rework. ⚠️ F2's premise was FALSE: `dl_G_VTX` alternates TWO matrices per vertex, so a residency cache misses every call. Single-vector `PSMTXMultVec`/`SR` now take FTRV only when the matrix is provably resident and plain scalar otherwise, never clobbering XMTRX on the miss path |
| `DC_PVR_BATCH_LOG` `src=`/`vram=` | joins a batch to a source symbol through `census_resolve.py` |
| unhandled-texture-format log | an unhandled format decoded to a transparent rectangle **in silence**, which is byte-identical to a missing asset |
| `tools/dcstub/make_keeplist_town.py` | generates `keeplist-town.txt` |
| `harness/dc/bench/` | bench_mem's build path |

---

# Older material, kept for the reasoning behind items now closed

## What was fixed in session 1 (do not re-investigate)

All four were the same shape: **state that is recorded and never consumed.**

1. **GX wrap mode** — stored since M1, never read; every texture repeated.
2. **TEV constant colours** — `g_gx.tev_colors[]` stored, never read; a black
   vignette rendered **white** over 27.9 % of the frame.
3. **The keep list** — 77 of 117 texture uploads decoded to all-zero.
4. **`.c_inc` files** — invisible to `make_stub_data.py`, so the dialogue
   balloon's arrays got a `.bss` buffer and no loader.

Narrative and numbers: `kb/state-log.md`. Gotchas: `kb/traps.md`.

## The punch-through list, and why `MODULATEALPHA` is wrong

`opb_sizes[4] = PVR_BINSIZE_32`, cutouts route to `PVR_LIST_PT_POLY`, threshold
pinned to 144, geometry buffered until the base list closes and replayed.

**`cxt.txr.env` is `PVR_TXRENV_MODULATEALPHA`, so the alpha the PT comparator
tests is `vertex_alpha × texel_alpha`. On the N64 those two were never
multiplied.** Every alpha-tested display list runs `G_RM_FOG_SHADE_A`, where the
vertex alpha byte is the per-vertex **fog coefficient**. Fixed by forcing vertex
alpha opaque on textured PT batches; kill switch
`-DDC_PVR_PT_KEEP_VTXALPHA`. **A/B settled: PT stays ON.**

## Fog — hardware, and the mapping is exact

`PVR_FOG_TABLE` with a custom 129-entry table, derived in closed form rather
than fitted. Vertex fog was not an option: KOS's `pvr_fog_vertex_color()` is an
`assert_msg(0, "not implemented")` stub. Kill switch `-DDC_PVR_NO_FOG`.

## The speaker name / reply text — fixed, and it was never the loader

`mFont_SetVertex_dol` writes `cn[0..3] = 0` into every glyph vertex and the font
display list clears `G_LIGHTING`, so `shade_vertex()` returned `0x00000000` and
`MODULATEALPHA` multiplied the glyph away. On GX that zero is harmless because
`mFont_CC_FONT` never reads `RASC`/`RASA`. The **fifth** member of the
recorded-but-never-consumed family.

## Environment

⚠️ **A short run is usually the human closing the emulator window, not a hang.**
Ask before bisecting a short run.
