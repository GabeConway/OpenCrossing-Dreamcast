# RESUME — pick the session back up here

Rewritten 2026-08-05 (session 4). **This file is the handoff — start here, then
`kb/STATE.md` for the current numbers.** The narrative and the evidence behind
every figure below are in `kb/state-log.md`, top entry. Everything under "Older
material" is kept because it is still the reasoning behind items now closed.

## ⭐ SESSION 5 (2026-08-06) — READ THIS BEFORE ANYTHING BELOW IT

**The `-O0` directive is REVERSED by user decision.** `src/` builds at `-Os`
with 14 hot TUs at `-O3`; `DC_OPT_PROFILE=o0` is the byte-identical revert.

1. **Town 11.6 → 20.0 FPS, `.text` 5,506,964 → 2,729,152 B (−2.78 MB).**
   Matched windows, `ASSET MISSING 0`, `crashes=0`, screenshot-gated against a
   same-source `DC_OPT_PROFILE=o0` control. `kb/state-log.md` has everything.
2. **The `dc/` phase is the control and it did not move** (`xform` 13.1 →
   12.4 ms). That is what makes this a real result and not drift.
3. **EVERY FPS NUMBER WRITTEN BEFORE TODAY IS `-O0` AND IS NOW WRONG.** §1's
   78.3 ms frame is 46.8 ms; G1's per-opcode histogram was measured at `-O0`
   and **must be re-run before any opcode is costed again**. The `src/`-heavy
   framing that justified G2/G3 has shifted toward `dc/`, which is where
   `dc_gx_backend_submit` already was.
4. **A full rebuild is 96 seconds.** It was never measured before; every plan
   in this kb that treats a rebuild as expensive was costing a guess. Bisecting
   a bad TU is now cheap — `tools/dcopt/bisect_o0.sh`.
5. **The build knows what UB it is standing on.** `DC_TARGET=warnscan` +
   `tools/dcopt/warnscan_report.py`: 35 missing returns in 30 files, 99
   uninitialised reads. `emu64.c` is CLEAN on missing returns; `jammain_2.c`
   is the one C++ TU that is not, and it is audio-only.
6. ⚠️ **`shot_diff.py` cannot gate an optimization change** — probes fire per
   presented frame, so a faster build samples a different point in the same
   camera pan. Judge the SCENE, not the pixels.
7. ⚠️ **`-O3` on `emu64.c` is unproven; `-O2` on it is device-verified (armhf).**
   First two experiments if the display list misbehaves:
   `DC_OPT_O0_EXTRA=src/static/libforest/emu64/emu64.c`, then
   `DECOMP_HOT_OPT=-O2`.
8. ⚠️ **`JUTRomFont::spFontHeader_` is still undefined and is being LEFT that
   way** — if an optimized build emits a reference, the link fails loudly
   instead of the game dereferencing NULL. Do not "fix" it by defining it.
9. **Nothing here has run on hardware.** Flycast models no instruction cache,
   so it understates a change that deleted 2.8 MB of `.text`.
10. **sh4zam is vendored (`dc/third_party/sh4zam`) and ships NOTHING.** The
    FIPR `PSMTXMultVec` experiment measured **flat** (`us/v` 3.11 → 3.12) and
    is off by default on precision grounds. What it did find: that function's
    residency probe was **dead code** and is gone. The two experiments still
    worth running with it are the AABB cull (`dc_gx.c:495`, ~2.0 ms/frame,
    scalar, never touches the matrix unit) and the per-lit-vertex block
    (`dc_pvr.c:2868`). Composing `P·MV` in XMTRX is **costed and dropped** —
    67 µs of a 45 ms frame. `kb/state-log.md` top entry.
11. **The RAM picture changed as much as the FPS picture.** −2.78 MB of `.text`
    is bigger than every `.bss` lever landed so far, combined; `kb/levers.md`
    and `kb/ram-plan.md` are costed against an image that no longer exists.

## SESSION 4 IN TEN LINES (2026-08-05)

1. **G1 RAN, and it moved the FPS argument off `G_VTX`.**
   `TRIN_INDEPEND 22.25 ms / 146 calls` = **152 µs a call: 63 % of emu64
   dispatch and 28 % of the whole 78.3 ms town frame, from one opcode.**
   `G_VTX` is **5.40 ms**, not the ~48 ms that four documents costed G3
   against.
2. **65 % of that heavy opcode is ALREADY `dc/` code at `-O2`.** `GXEnd` is
   live at `emu64.c:4935`, so the AABB cull (~15 µs) and
   `dc_gx_backend_submit` (~81 µs) bill into it; only ~53 µs is `src/` at
   `-O0`. G2 and G3 shrink accordingly — and **the biggest single addressable
   block in the frame is `dc_gx_backend_submit` at 12.2 ms, which needs no
   sign-off, no trampoline and no `objcopy`.**
3. **The cull rate is a measurement now:** `vcull=6042` against `v=3002` —
   **66.8 %** of vertices discarded after emu64 paid the full `-O0` price.
   ⚠️ It is **not** worth 22.25 × 0.668; culled vertices already cost `xform`
   nothing. An ideal cull at TRIN entry is **4.5-7.0 ms**.
4. **`gap=7.92 ms`** — 18 % of emu64's own total, inside the draw phase and
   outside every command. Unexplained. **OPEN.**
5. **R1 landed** (`60222ba`): the 96 acre ground textures load off `/cd`
   instead of living in `.bss`. **`.bss` −81,856 B, `margin` +87,392 B**, FPS
   unchanged, screenshot-verified. Kill switch `DC_BGTEX_DEMAND=0`.
6. **R2/R3 are written and DEFAULTED OFF (`35cb00c`), and the reason is the
   finding:** the port constructs **ZERO villager NPCs**. `mNpc_SetNpcList`
   reads the save's `Animal_c animals[]`, the VMU path is unwired,
   `[PC] No save file found`. Two 900 s town runs printed not one
   `[DC/NPCTEX]`/`[DC/NPCMDL]` line. **Wiring the save (N2b) is now a
   PREREQUISITE for testing the pools, not just a feature.**
7. ⭐ **THE ACCOUNTING CORRECTION — the most reusable finding of the session.**
   In a stubbed image an asset class's resident cost is **what the KEEP LIST
   kept**, not what the class totals: villager textures are 1,154,944 B on
   paper and **90,464 B resident**. A pool converts MISSING into PRESENT; it
   usually frees nothing. Cost one against *keeping the class*, never against
   the class total. **The acres are the one exception** — all 242 summer acre
   TUs really are kept, so **815,024 B** of `grd_s_*` vertex arrays really is
   resident and an acre pool really would free RAM.
8. **Four instrument holes fixed** (`82cb299`). G1's report sat inside
   `#ifdef DC_PERF_PHASE` while its arm sites did not, so without that flag it
   armed, paid a clock read on ~2,867 commands a frame and printed nothing;
   **G1 and G2 overwrite each other's table entries and were silent about it**
   (now an `#error`, `dc_platform.h:417`); `EMU64_TBL_OBJ` spelled a literal
   object path that breaks the moment `emu64.c` enters a scratch tree; and G2's
   shadow had lost the original's 5-message rate limiter and dropped one
   command on its punt path.
9. **SH-4 math: FTRV/FIPR/FSRRA were already live; two real gaps were not.**
   `DC_MTX_USE_FIPR` now defaults to **1** (`-DDC_MTX_NO_FIPR` reverts), and
   **272 `sqrtf` sites in `src/` were linking newlib's SOFTWARE
   `__ieee754_sqrtf`** — `dc/src/dc_fmath.c` binds them to FSQRT
   (`-DDC_NO_FSQRT` reverts). **sh4zam stays a PASS**, for corrected reasons:
   `kb/closed.md`.
10. **`--wrap` DOES NOT WORK AS SPELLED anywhere in this kb.** sh-elf prefixes
    user labels with `_` (`_mNpc_SetNpcList`), and ld does not diagnose
    `--wrap` on an unknown symbol — so every proposed `--wrap` seam matches
    nothing, silently.

<details><summary>SESSION 3 IN TEN LINES — kept for the sequence; items 4-6 are superseded above</summary>

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
   commands). Fixed.
6. **G2 is written** (`dc/src/dc_emu64_shadow.cpp`, off by default) and
   measured once at +0.8 FPS whole-run.
7. **`emu64.hpp`'s offset comments are PowerPC and wrong here.**
   `sizeof(emu64)` = 0x2278. Never hand-write an offset map.
8. **`emu64_set_aflags()` is not a seam** — `AFLAGS_MAX` is 0.
9. **`mFM_DecideAcre` does not exist**; it is `mRF_MakeRandomField`, and the
   layout lives in the SAVE, not in per-boot randomness.
10. **`tools/dcfb/shot_diff.py`** makes the screenshot gate mechanical.

</details>

⚠️ **Killing a build mid-link corrupts `dc/build/objs.rsp` with NUL bytes** and
the next link fails with `undefined reference to 'main'`. `rm` it and relink;
do not go hunting in your own diff. (`kb/traps.md`.)

## 0. What is committed on `dev`

**The main thread commits; agents do not.** Session 4 so far:

```
35cb00c feat(dc): villager texture and model pools -- written, and OFF,
                  because the town has no villagers to test them on
60222ba feat(dc): the acre ground textures load off the disc, not out of .bss
82cb299 fix(dc): G1 could not print, and 63% of emu64 is ONE opcode
```

⚠️ **The SH-4 math work (item 9) and the TEV `tev_const_alpha_last()` work were
still in the tree, uncommitted, behind a running build when this was written.**
Check the tree before assuming either is committed or measured.

<details><summary>sessions 2-3, newest first</summary>

```
bb12d18 feat(dc): every summer structure renders, and G2 takes over the dispatch loop
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

</details>

## 0b. THE MEASUREMENT RULES. Now EIGHT — 6 and 7 were paid for on 2026-08-04, 8 on 2026-08-05.

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
   margin. ⚠️ **And then broke the same rule inside the rule.** This item used
   to end "265 `G_VTX` carrying ~6,951 vertices at ~6.9 µs each is ~48 ms, i.e.
   most of the emu64 budget from one opcode" — which re-applied a whole-command
   average to a subset *and* counted `GXPosition3f32` references as loaded
   vertices. **G1 measured `G_VTX` at 5.40 ms and `G_TRIN_INDEPEND` at
   22.25 ms** (2026-08-05). Only the histogram is allowed to price an opcode.
   `kb/traps.md`.
8. ⚠️ **IN A STUBBED IMAGE AN ASSET CLASS COSTS WHAT THE KEEP LIST KEPT — not
   what the class totals.** `DC_ASSET_STUB` already dropped the rest; an unkept
   asset is a 1-byte `.bss` symbol with its load suppressed. Every "pool X and
   free N bytes" claim written before 2026-08-05 costed N against the *non-stub*
   total. Measured: villager textures are 1,154,944 B on paper and **90,464 B
   resident**; villager models 438,640 B on paper and **5,536 B resident**.
   **Cost a pool against the alternative — keeping the class — never against
   the class total.** The acres are the only class where the old framing still
   holds. `kb/levers.md`.

## 1. Where the port is

**It boots on real hardware and LOADING IS AT PARITY WITH THE EMULATOR** —
human verdict on the `AC-DC-20260804` burn. That closes the "the CD-R is simply
slow" candidate that has been open since 2026-08-03. ⚠️ Parity on I/O is **not**
parity on compute: the only hardware FPS figure is still "~11 FPS in the town".

In Flycast it reaches the town, walks around it, meets Tom Nook and is taken to
the houses. `[SCENE_MODE] 0 → 3 → 4 → 18 → 9`.

**Current numbers (2026-08-05, probe-free, audio off, town keep list, post-R1):**

| | |
|---|---|
| FPS whole run | p50 **24.2**, max 29.9 |
| the town (scene 9) | **11.5-12.1** |
| `MEMLEDGER margin` | 3,191,348 (⚠️ read rule 6 before spending it) |
| `ASSET MISSING` / `ptdrop` / `LOST` | 0 / 0 / 0 |

### THE ARITHMETIC THAT GOVERNS EVERY FPS PLAN — REWRITTEN BY G1

The old framing was "19-27 % renderer, 77-80 % `src/`, so deleting the renderer
takes the worst frames from 11.8 to 15.2 FPS". **That split is still true of the
`gx=` counter and it is no longer the useful decomposition**, because the
per-opcode histogram shows most of the "emu64" bucket is our own code:

```
[PHASE]  draw=78.3 skip=8.2 (n=30) vi=0.4 | cull=2.2 xform=12.2 |
         v=3002 vsrc=3002 vlit=2517 vcull=6042 us/v=4.07
[EMU64]  cmds=3458 noop=1 vtx=298 tri=295 dl=276 | cullvis=7 cullrej=2
[EMU64H] tot=42.86ms gap=7.92ms probe=79.9ns | TRIN_INDEPEND 22.25/146
         VTX 5.40/149 MTX 2.18/113 TEXRECT 2.17/25 MOVEMEM 0.55/207
         SETTILE_DOLPHIN 0.32/109 DL 0.29/138 SETCOMBINE 0.28/58
         SETTIMG 0.25/112 TRI2 0.19/2 ENDDL 0.18/131 LOADTLUT 0.13/42
```

| | |
|---|---|
| `G_TRIN_INDEPEND` | **22.25 ms / 146 calls = 152 µs each** — 63 % of dispatch, **28 % of the frame** |
| of which already `dc/` at `-O2` | ~96 µs (AABB cull ~15 + `dc_gx_backend_submit` ~81) |
| of which `src/` at `-O0` | **~53 µs** ⇒ a G2-shaped rewrite is capped at ~8.3 ms |
| `G_VTX` | 5.40 ms / 149 — **not the ~48 ms four documents assumed** |
| every state opcode | ≤ 0.55 ms each. **Stripping state commands is worth nothing** (F8 is answered) |
| `gap` | 7.92 ms, 18 % of `tot`, unattributed. **OPEN** |
| `vcull` / `v` | 6,042 / 3,002 = **66.8 % of vertices culled after emu64 paid for them** |

**The single largest addressable block in the frame is `dc_gx_backend_submit` at
12.2 ms (15.6 % of `draw`) — `dc/src/dc_pvr.c:2448`, ours, already `-O2`, no
sign-off required.** See §5 before costing anything else.

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

**G1 — the per-opcode histogram (this is the line that produced §1's numbers):**
```bash
  DC_EMU64_HIST=1 DC_XDEFS='-DDC_PERF_PHASE' bash dc/build-dc.sh
```
⚠️ **G1 and G2 are mutually exclusive** — both install into emu64's dispatch
table, G2 wins, and G1 would measure nothing. That is an `#error` now
(`dc/include/dc_platform.h:417`), not a silent wrong number.

**RELEASE / burn image:** drop `DC_SCIF_FAST`, `DC_AUTOSTART`, `DC_AUTOWALK` and
every probe; add `DC_CDI_PAD=1`. ⚠️ `DC_SCIF_FAST` on hardware loses the console.

**Verdict on any run:**
`python3 tools/dcqa/run_report.py <run>/console.log --vs <baseline>/console.log`

⚠️ **Build to a COPY of the CDI before a long run.** ⚠️ **Never edit the tree
while a build runs**; never run two builds at once.
⚠️ **The harness writes `console.log` only when Flycast EXITS.** Polling it
mid-run finds nothing and reads exactly like a hang. A 900 s run is ~17-20 min
of wall clock; wait for it. (`kb/traps.md`.)

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

Current shipping image after R1 (2026-08-05): **`.bss` 3,945,356**,
`margin=3191348`, no OOM, `ASSET MISSING 0`.

⚠️ **Do NOT plan the next `.bss` spend against a pool's "non-stub total"** —
rule 8. R2 was ~break-even and R3 *spends* 115,424 B; neither freed room for
the interiors, and the section in `kb/STATE.md` that used to promise otherwise
is corrected.

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

1. **THE TOWN HAS NO VILLAGERS AT ALL, and that is a save bug, not an asset
   bug (found 2026-08-05).** `mNpc_SetNpcList` populates the town from the
   save's `Animal_c animals[]` (`m_start_data_init.c:559`); the VMU path is
   unwired, so `[PC] No save file found` and **not one villager actor is ever
   constructed**. Measured, not inferred: two 900 s runs that reach scene 9 and
   walk printed zero `[DC/NPCTEX]`/`[DC/NPCMDL]` lines, because
   `aNPC_dma_draw_data_proc` (`ac_npc_ctrl.c_inc:687`) never runs.
   **R2 and R3 exist and are DEFAULTED OFF for exactly this reason** —
   `DC_NPCTEX_POOL=1` / `DC_NPCMDL_POOL=1` turn them on, and the first honest
   test of either is after N2b wires the save. ⚠️ The old text here said "60 of
   72 villager models are stubbed, 392 KB + ~992 KB"; **both figures were
   non-stub totals** (rule 8) and the model count is 32, not 72.
2. **The other 74 `obj_s_*` structures** — 333 KB (shop1, museum, tailor,
   police box, shrine, post office…). Each is one building silhouette.
3. ✅ **Winter ground — FIXED 2026-08-05 by R1.** All 41 `mFM_grd_w_*` textures
   (70,144 B) are demand-loaded off `/cd` by `dc/src/dc_bgtex.c` instead of
   being resident-or-absent, so the December black-floor bomb is defused. ⚠️
   **The winter time bomb is REDUCED, NOT CLEARED**: the 84 `obj_w_*`
   structures are still stubbed, so a winter town still draws every building as
   a black spiky mess (item 2's summer twin). Still needs a `DC_SEASON=winter`
   build to verify.
4. **The station roof clip-through.** Now REPRODUCIBLE for the first time —
   `DC_AUTOWALK` can walk a character under it, `DC_PVR_BATCH_LOG` prints
   `src=`/`vram=` so a batch joins to a symbol, and `-DDC_PVR_PT_NEAREST` is a
   one-line test of H2. Never run.
5. **`aram zero=7 (2016 B)`** appeared in the walking run and must be 0. Small,
   new, unexplained.
6. **The large black wedges in the town are TEV config #007 losing BOTH of its
   alpha factors — diagnosed 2026-08-05, NOT fixed.** `ef_shadow_out.c:34-35`
   records as **two** stages, not the three an earlier reading assumed:
   `stage0 alpha = (ZERO, TEXA, A1, ZERO)` and
   `stage1 alpha = (ZERO, TEXA, APREV, ZERO)` (`emu64.c:1888-1897`, config #007
   in `kb/tev-map-table.md:120`). PRIM.a sits on stage 0 in a **mirrored**
   spelling that `tev_const_alpha()` reaches and then throws away at its
   `konst != GX_CA_A0` narrowing, and the last stage is `APREV * TEXEL1.a`, the
   mirror of what `tex1_alpha_active()` tests. **So the batch draws at
   `vtx.a * T0.a`, where `vtx.a` is the `G_RM_FOG_SHADE_A` fog coefficient — a
   flat dark quad, exactly the reported symptom.** The two real levers are
   widening `tev_const_alpha()`'s A1 arm and adding the mirrored shape to
   `tex1_alpha_active()`; both are widenings, and widenings in this family have
   regressed before (`dc_pvr.c:1080-1098`), so **both need a screenshot pair.**
   ⚠️ The new `tev_const_alpha_last()` in the tree (kill switch
   `-DDC_PVR_NO_TEVALPHA_LAST`, counter `tevalpha_last batches=`) recognises a
   final stage of shape `APREV * konst` and **does not fix this**. ⚠️ This
   diagnosis still has to be transcribed into `kb/tev-map-alpha.md` /
   `kb/tev-map-hard-cases.md`; only `kb/state-log.md` and this file carry it.

## 5. THE RANKED NEXT ACTIONS (2026-08-05)

Same list as `kb/STATE.md`; this is the ordering a fresh context should work
down. Items 1-3 are frame time, 4 unblocks the villagers, 5 decides the audio
architecture, 6 is a visible bug.

1. **`dc_gx_backend_submit` — 12.2 ms, ours, `-O2`, no gate.** The first step
   is free and settles two questions at once: print the vertex-memo hit ratio
   and a per-batch `count` histogram from `dc_gx_flush_vertices`. That decides
   whether this is a 3 ms or an 8 ms lever **and** settles the memo-ceiling
   dispute below. ⚠️ **OPEN, and neither reading may be stated as fact:** the
   vertex memo is **128 slots** (`dc_pvr.c:1955`) hitting **48.2-48.9 %**, and
   its ceiling is either 48.2 % (already there, further work worth zero) from
   6,951 staged references, or ~60 % (~11 points of headroom) from the newly
   measured `v + vcull = 9,044`. `dc_gx.c:870`'s 6,951 comment is **stale**.
   The experiment that settles it is a direct count of distinct vertex
   references per batch.
2. **Per-slot shadow of `G_MTX`** (2.18 ms over 113 calls) as the proof that
   the per-slot machinery works — self-contained, no `gfx_p` rewriting, est
   **0.9-1.4 ms**. ⚠️ **Hazard:** `dc_emu64_hist.c:262` and
   `dc_emu64_shadow.cpp:492` each `memcpy` **all 64 slots** on frame open and
   restore all 64 on close, so a third installer must arm slot-wise or
   initialise strictly before them.
3. **Slot-60 `TRIN_INDEPEND` cull-only shadow** — an AABB over the index window
   at entry, tested against `projection_mtx ×` (identity for SHARED /
   `position_mtx_stack` for NONSHARED), with `dl_G_TRIN` kept as the fallback.
   Est **4.5-7.0 ms** ⇒ 11.5 → 12.2-12.6 FPS.
4. **Wire the VMU save path (N2b).** Now blocking rather than optional: it is
   the only way to get a villager into the town, and therefore the only way to
   test R2/R3 at all (§4 item 1).
5. **The audio sequencer floor.** `-Wl,--wrap=_RspStart2` — **note the leading
   underscore**, §0b/`kb/traps.md` — plus an empty `__wrap_` deletes exactly
   the work AICA would absorb. Read `synth_us=` against 19,840: **≤2,000 means
   AICA Stage B is worth 6-10 weeks; ≥8,000 means an `-O2` rspsim shadow
   outranks it.**
6. **The TEV shadow-alpha fix** — §4 item 6. Written code exists and does not
   fix it; the two real levers are named there.

## 5a. The emu64 decision gate — RE-COSTED 2026-08-05, and it shrank

`kb/research-fps-ideas.md` carries this in full.

⚠️ **G1 RAN, and it shrinks G2 and G3.**

| | what | win | gate |
|---|---|---|---|
| **G1** | per-opcode timing histogram | 0 ms, unlocks the rest | ✅ **RUN 2026-08-05.** `TRIN_INDEPEND 22.25 ms/146`, `VTX 5.40/149`, `gap 7.92` |
| G2 | reimplement the dispatch LOOP in `dc/` at `-O2` | ~~7-14 ms~~ → **2-4 ms** [ESTIMATED], capped at ~8.3 | ✅ signed off 2026-08-04; built, +0.8 FPS whole-run, town not separated |
| G3 | `-O2` shadow `dl_G_VTX`/TRI with a runtime AABB cull | ~~25-35 ms~~ → **4.5-7.0 ms** [ESTIMATED] from the cull; the `-O2` half is inside G2's ~8.3 ms cap | ✅ signed off 2026-08-04. **Build the cull half first** (§5 item 3) — that is where the whole win is |
| — | `dc_gx_backend_submit` (`dc/src/dc_pvr.c:2448`) | **12.2 ms is the block itself**; win unmeasured | **no gate at all — it is ours** |

**The re-cost, in one line:** 65 % of `G_TRIN_INDEPEND`'s 152 µs/call is already
`dc/` code at `-O2` (`GXEnd` at `emu64.c:4935` → the AABB cull ~15 µs +
`dc_gx_backend_submit` ~81 µs); only ~53 µs is `src/` at `-O0`. Interposing on
emu64 buys much less than the decision assumed, and the largest single
addressable block in the frame needs no sign-off.

**⚠️ F1 (offline bbox-CULLDL injection) is NO LONGER RECOMMENDED.** A full design
pass found its cost is **594 KB of `.data`** for town scope — 5-16× its own
estimate — because its "50+ vertex" cap selects **zero** chunks (no
`gsSPVertex` anywhere in `src/data` exceeds 32 vertices; ⚠️ **corrected
2026-08-05** — the bound comes from the 5-bit per-vertex *index* width
(`POLY_5b`), not from the N-triangle count, which `emu64.c:4814` reads as
**7 bits**, 1..128 faces). It also cannot compute its bboxes without the ROM. It buys a *smaller*
version of what G3 gets for zero bytes. Details and the surviving 212 KB
model-granularity variant are in `kb/research-fps-ideas.md`.

~~**RUN G1 BEFORE COSTING ANY OF THEM** (rule 7).~~ ✅ Done 2026-08-05; the
table above is the re-cost. Rule 7 still binds every *future* opcode claim.

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

## 6. What is new in the tree — SESSION 4 (2026-08-05)

| thing | what it is |
|---|---|
| `dc/src/dc_bgtex.c`, `DC_BGTEX_DEMAND=0` | **R1.** 96 acre ground textures demand-loaded off `/cd`; −81,856 B `.bss`. The seam is `make_src_shrink.py` rewriting one `bcopy`, not `--wrap` |
| `dc/src/dc_npctex.c`, `DC_NPCTEX_POOL=1` | **R2**, 16 × 4,832 B villager texture slots. **OFF by default** — there are no villagers to load into it |
| `dc/src/dc_npcmdl.c`, `DC_NPCMDL_POOL=1` | **R3**, 16 × 7,552 B villager model slots. **OFF by default**, same reason; costs +115,424 B when on |
| `dc/src/dc_fmath.c`, `-DDC_NO_FSQRT` | binds `sqrtf` to SH-4 `FSQRT` so newlib's software `__ieee754_sqrtf` is never linked. **Bit-identical to newlib for every normal input** — KOS sets `FPSCR = 0x00040000` (DN=1, round-to-nearest-even) at `startup.S:74-85` — so there is nothing to A/B on a screenshot |
| `DC_MTX_USE_FIPR` now **1**, `-DDC_MTX_NO_FIPR` | the `TODO(M3)` at `dc_mtx.c:71-72` resolved rather than measured; the kill switch is the A/B |
| `tev_const_alpha_last()`, `-DDC_PVR_NO_TEVALPHA_LAST` | recognises a final TEV stage of shape `APREV * konst`; counter `tevalpha_last batches=`. ⚠️ **Does not fix the black wedges** — §4 item 6 |
| `vcull=` in `[PHASE]` | `dc_gx_phase_culled_verts` — vertices rejected by the AABB cull, the counter that turned "60 % culled" into a measurement |
| `#error` in `dc_platform.h:417` | G1 and G2 both install into emu64's dispatch table; building both was silently useless |

<details><summary>session 3's additions</summary>

| thing | what it is |
|---|---|
| `[EMU64]` line | the opcode mix, free, under `-DDC_PERF_PHASE` |
| `DC_EMU64_HIST=<N>` | **G1.** Timing thunks swapped into emu64's dispatch table at runtime; `src/` untouched, one `objcopy --globalize-symbol` is the only build change. ✅ **RUN 2026-08-05** |
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

</details>

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
