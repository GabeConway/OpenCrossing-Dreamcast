# Unbanked FPS concepts — ranked, each with a failure mode

Written 2026-08-04, from a deliberately out-of-the-box pass over the frame-time
problem, then vetted against the code. The companion to `kb/perf-dc.md`, which
records what has been *measured*; this file records what has not been *tried*.
`kb/research-creative-ram.md` is the same shape for memory.

⚠️ **Read `kb/perf-dc.md` §4 first.** Everything already ruled out with evidence
— texture upload cost, strip/fan conversion, the batch-merge path, the frameskip
tick — is not repeated here.

---

## The arithmetic that has to stay in front of any plan

The town frame is ~79-90 ms. The renderer (`gx=`, all of it in `dc/`) is
**19-27 %** of it, at the median and at the tail alike. emu64 display-list
traversal plus game logic is **77-80 %**, all in `src/`, which is closed to
editing and where compiler flags are banned.

**Deleting the renderer entirely would take the worst 5 % of frames from 84.7 ms
to 66.0 ms — 11.8 to 15.2 FPS.** Any plan that quotes a renderer optimisation as
a route to 30 FPS in the town is wrong on arithmetic.

⚠️ **G1 re-cut that split along a different axis, and the new one is the useful
one (2026-08-05).** The `gx=` counter still says 19-27 %, but the per-opcode
histogram shows that **65 % of the heaviest "emu64" opcode is our own `-O2`
code**, billed through `GXEnd` (`emu64.c:4935`): of `G_TRIN_INDEPEND`'s
152 µs/call, ~15 µs is the AABB cull and ~81 µs is `dc_gx_backend_submit`, and
only ~53 µs is `src/` at `-O0`. **So the editable share of the frame is larger
than "19-27 % renderer" suggests, and the un-editable share is smaller** — which
is why the queue below starts with a `dc/` function and not with a sign-off
request.

---

## The ranked queue as of 2026-08-05 — READ THIS BEFORE PICKING AN IDEA BELOW

The ideas on this page are ranked by their own merits; this is the order to
actually work in, after G1's histogram. `kb/STATE.md` carries the same list.

| # | what | est | gate |
|---|---|---|---|
| 1 | **`dc_gx_backend_submit`** — 12.2 ms, 15.6 % of `draw`, `dc/src/dc_pvr.c:2448`. First step is free: print the vertex-memo hit ratio and a per-batch `count` histogram from `dc_gx_flush_vertices`, which also settles F3's ceiling dispute | unmeasured; the block is 12.2 ms | **none — it is ours** |
| 2 | **Per-slot shadow of `G_MTX`** (2.18 ms / 113 calls) as the proof the per-slot machinery works — self-contained, no `gfx_p` rewriting | 0.9-1.4 ms [ESTIMATED] | G2/G3 sign-off already given |
| 3 | **Slot-60 `TRIN_INDEPEND` cull-only shadow** — AABB over the index window at entry, tested against `projection_mtx ×` (identity for SHARED / `position_mtx_stack` for NONSHARED), `dl_G_TRIN` kept as the fallback | 4.5-7.0 ms [ESTIMATED] | as above |
| — | the `gap=7.92 ms` | unknown | it is not even attributed yet |

⚠️ **HAZARD FOR ANY THIRD INSTALLER INTO THE DISPATCH TABLE.**
`dc_emu64_hist.c:262` and `dc_emu64_shadow.cpp:492` each `memcpy` **all 64
slots** on frame open and restore all 64 on close. A per-slot shadow that arms
after either of them will be silently reverted on the next frame boundary, and a
per-slot shadow that arms before will be captured *into* their saved copy.
**Arm slot-wise, or initialise strictly first.** G1 and G2 are already mutually
exclusive by `#error` for the same reason (`dc/include/dc_platform.h:417`).

---

## The five facts these ideas rest on

All verified in the tree, 2026-08-04.

1. **66.8 % of the vertices emu64 produces are thrown away by our own AABB cull,
   AFTER emu64 has paid full `-O0` price for them.** ⚠️ **Re-measured
   2026-08-05 and re-costed downward.** This fact used to read "60 % … at
   ~6.9 µs of emu64 work per vertex that is **~29 ms/frame**", derived across
   two differently-instrumented builds from `pc_gx_culled_draws`, which counts
   BATCHES. The `vcull=` counter (`dc/src/dc_gx.c`) now counts vertices
   directly: **`vcull=6042` against `v=3002`.**
   **But it is not worth 29 ms, and it is not worth 14.9 ms either.** `dc_gx.c`
   returns *before* `dc_gx_backend_submit`, so culled vertices already cost
   `xform` nothing. An ideal cull at TRIN entry removes the staging of 6,042
   vertices (5.0-5.6 ms [ESTIMATED]) plus the per-vertex AABB scan
   (2.1 ms [MEASURED]), minus a new O(window) test (0.25-0.40 ms [ESTIMATED]):
   **4.5-7.0 ms, central ~6.0 ⇒ 11.5 → 12.2-12.6 FPS.** `kb/perf-dc.md` §2b.
2. **emu64 already contains a working display-list cull.** `dl_G_CULLDL`
   (`emu64.c:5189-5318`) classifies vertex-cache entries against six clip planes
   and pops the DL stack on a hit — exactly the "make emu64 skip a whole object"
   mechanism wanted, already implemented, already counted.
3. **The game's data almost never uses it.** `gsSPCullDisplayList` appears
   **twice** in all of `src/`: `ac_field_draw.c:325` (an acre-sized box around
   the whole acre BG list) and `obj_item_fish.c:62`. So acre-level cull already
   runs; the 60 % waste is the *contents* of partially-visible acres and
   per-object models — the granularity the game never culled.
4. **The acre and object display lists are C text, not binary.**
   `src/data/field/bg/acre/` holds 268 acre models as `Gfx foo_model[] = {...}`.
   `grep -rE "gsSPDisplayList\(&" src/data` returns **0** and `gsSPBranchList`
   returns **0** — no interior pointers into any `Gfx` array, so an offline
   rewriter can restructure them safely, through the proven scratch-tree
   mechanism that leaves `src/` untouched.
5. **`emu64`'s dispatch table is writable.** `static dl_func
   dl_func_tbl[NUM_COMMANDS]` (`emu64.c:5702`) is a non-const file-static array
   of member-function pointers in `.data`, called at `emu64.c:5850`. There is no
   MMU write protection anywhere.

---

## The ideas, ranked by (estimated win) / (risk x effort)

### ✅ F0 — DONE 2026-08-04. ✅ G1 — RUN 2026-08-05, and it OVERTURNS this entry.

`dc/src/dc_vi.c` prints `[EMU64]` next to `[PHASE]`. The town, per frame:

```
cmds=2867 noop=1 vtx=265 tri=258 dl=250 | cullvis=6 cullrej=3
```

**773 of 2,867 commands do geometry work. The other 2,094 — 73 % — are RDP
and RSP STATE.**

⚠️ **Do NOT price that at 12.31 µs/cmd**: 12.31 µs/cmd is a fit of `emu64_ms`
against TOTAL `cmds`, and total `cmds` correlates with `vtx`, so the coefficient
is dominated by the opcode that does the most work per command.

⚠️ **AND THE REPLACEMENT ARITHMETIC IN THIS ENTRY WAS ALSO WRONG. Corrected in
place 2026-08-05.** This paragraph used to read: "265 `G_VTX` commands carry
~6,951 vertices into `dc_gx` — about 26 vertices each — and at the ~6.9
µs/vertex of emu64 work already measured, **`G_VTX` alone accounts for ~48 ms,
i.e. essentially the whole emu64 budget**." It is wrong on both inputs, and it
is what G2/G3 were costed against:

- **`~6.9 µs/vertex` is itself a whole-command average applied to a subset** —
  measurement rule 7, committed inside the paragraph that states rule 7.
- **The ~6,951 were `GXPosition3f32` *references*, not loaded vertices.**
  `G_VTX` loads ~3,601 per town frame; the rest are re-emissions of the same
  sources, which is exactly what the vertex memo exists to catch.

**G1 measured it:** `G_VTX` is **5.40 ms over 149 calls**. The budget is in
`G_TRIN_INDEPEND` — **22.25 ms over 146 calls, 152 µs/call, 63 % of emu64
dispatch and 28 % of the whole 78.3 ms frame.** Full histogram and the
consequences: `kb/perf-dc.md` §2b.

Two consequences that hold regardless:

- **F8 is answered, not just reopened. It is worth ~nothing.** The state
  opcodes G1 priced are all ≤ 0.55 ms each: `MOVEMEM 0.55/207`,
  `SETTILE_DOLPHIN 0.32/109`, `SETCOMBINE 0.28/58`, `SETTIMG 0.25/112`,
  `LOADTLUT 0.13/42`. A large *count* of near-free commands is worth nothing to
  strip — which is what F8's original static-count dismissal said, for the
  wrong reason. **Do not build a strip rule.**
- **`cullvis=6`** (7 in the G1 run). emu64's own display-list cull runs nine
  times a frame and rejects two or three. The acre-level cull the game ships is
  barely doing anything.

<details><summary>the original F0, for the reasoning</summary>

### F0. Print the counters that already exist. Do this first, unconditionally.

`pc_emu64_frame_noop/vtx/tri/dl_cmds` and `pc_emu64_frame_cull_visible/rejected`
are maintained by emu64 (`emu64.c:5824-5834`, `:5309-5315`) and defined in
`dc/src/dc_gx.c:90-97` — but `[PERF]` prints only `cmds` and `crashes`. Every
idea below needs the town's opcode mix and the current CULLDL hit rate, and both
are already being counted for free.

**Win: 0 ms. Risk: 0. Unlocks everything else.**

</details>

### F1. Offline bbox-CULLDL injection into acre and model display lists

**10-20 ms/frame. Low-medium risk. Fully inside the rules.**

A new scratch-tree rewriter splits each large static DL into sub-lists, each
prefixed with 8 synthetic AABB corner vertices and a `gsSPCullDisplayList`:

```c
Gfx grd_s_c1_1_model[] = {          /* same symbol, same extern type */
    gsSPDisplayList(chunk0), ... gsSPEndDisplayList()
};
static Gfx chunk0[] = {
    gsSPVertex(chunk0_bbox, 8, 0),  /* 8 AABB corners, computed offline */
    gsSPCullDisplayList(0, 7),
    <the original commands for this chunk>,
    gsSPEndDisplayList(),
};
```

This is byte-for-byte the idiom the game itself uses at
`ac_field_draw.c:322-334`: we extend the developers' own acre cull down to
sub-acre and object granularity. A culled chunk pops back to the parent and the
next chunk still runs — which is *why* the split into sub-lists is mandatory. A
`gsSPCullDisplayList` inserted inline cannot work: a cull hit ends the whole
current DL.

**Why 8 synthetic verts and not the batch's own 30:** `dl_G_CULLDL` transforms
every tested vertex through `-O0` `guMtxXFM1F_dol2`, so testing 30 real vertices
on *visible* chunks would cost more than it saves.

Failure modes, all checkable mechanically:
- A model DL executed as a root task or via `G_DL_NOPUSH`: a cull would set
  `end_dl` and kill the rest of the frame (`emu64.c:5301-5305`). Inject only into
  DLs statically reachable *only* via push-mode `gsSPDisplayList` — fact 4 says
  that is exhaustively checkable.
- The 8 bbox verts clobber cache slots 0-7. Safe for chunks that begin with
  their own `gsSPVertex` (every acre chunk does), fatal for a DL that consumes
  vertices loaded by its caller. Detectable.
- Zero normals hit `PSVECNormalize` under `G_TEXTURE_GEN` → NaN. Give them a
  unit normal; they are never drawn.
- ⚠️ **RAM: ~60-120 KB of `.data`** for bbox vertices and the extra `Gfx`, on a
  budget already 4.7 MB over. Bounding injection to DLs of 50+ vertices caps it.
  **This must be declared in the ledger — it is the one real cost.**

Cheapest experiment: hand-transform ONE town acre (`grd_s_c1_1`) plus one large
object model in a scratch copy, run 600 s, read `cull_rejected` (F0) and the
matched-frame `cmds`/`v` drop.

### ⚠️ F2 — THE PREMISE WAS FALSE. Corrected and reimplemented 2026-08-04.

F2 said `dl_G_VTX` calls `PSMTXMultVec` twice per shared vertex **with the same
matrix**, so a residency token turns the second call into one FTRV. Read the
call site:

```c
emu64.c:4708   PSMTXMultVec(position_mtx,         &pos,    &pos);
emu64.c:4710   PSMTXMultVec(this->model_view_mtx, &normal, &normal);
```

**Two different matrices, alternating once per vertex.** XMTRX is physically a
single slot, so a residency cache misses on every call in the one loop that
matters. F2 as specified is worth **zero**.

What the call site *does* establish is that the single-vector entry points
should never have been on the XMTRX path at all. For ONE vector the cost is
entirely in getting the matrix in:

| | |
|---|---|
| XMTRX | `transpose44` = 12 loads + 16 stores, `mat_load` = 8 double-`fmov` + the `frchg` pair, then 1 FTRV — **~50+ cycles** |
| scalar | 12 loads, 9 `fmul`, 9 `fadd`, 3 stores, no memory round-trip, no bank switch — **~30-40 cycles** |

`dc_mtx.c`'s own header already said this ("PSMTXMultVecArray: **this** is where
XMTRX actually pays") and then loaded XMTRX per call anyway.

**Implemented instead:** `PSMTXMultVec`/`PSMTXMultVecSR` take FTRV only when the
matrix is provably still resident, and plain scalar otherwise. The miss path
deliberately does **not** touch XMTRX, so a scalar miss cannot evict the matrix
a bulk transform loaded. Residency is pointer + a full 12-word content compare
(emu64 rewrites matrix-stack entries in place, so pointer equality alone is the
documented stale-matrix failure), plus an explicit `dc_mtx_xmtrx_invalidate()`
that `dc_pvr.c` calls after its per-batch `mat_load` — those two are the only
XMTRX writers in the image. Kill switch `-DDC_MTX_NO_XMTRX_CACHE`.

**Unmeasured as of this edit.** Rule 5: the win is whatever a matched-frame A/B
says it is, not what the cycle table above says.

### F3. Memo generation counter instead of a per-batch flush

**1-2 ms/frame. Low risk.**

The vertex memo (128 slots, 48.2-48.9 % hit rate) is invalidated at the top of
every `dc_gx_backend_submit()`. But emu64 splits one object into many
consecutive batches while the folded matrix, lights and TEV constants are often
unchanged. Hash the per-batch constants and invalidate only when the hash moves.

⚠️ **OPEN, and it decides whether F3 is worth anything at all: is the memo
already at its ceiling?** Two derivations disagree on one input, the total
staged vertex references per frame.

| | total refs | loaded vertices | refs/vertex | ceiling | reading |
|---|---:|---:|---:|---:|---|
| A | 6,951 (`dc_gx.c:870`'s comment, older docs) | 3,601 | 1.93 | **48.2 %** | measured 48.2 % IS the ceiling — F3 is worth zero |
| B | `v + vcull` = 3,002 + 6,042 = **9,044** | 3,601 | 2.51 | **~60 %** | ~11 points of headroom |

B's 9,044 comes from the `vcull` counter added 2026-08-05 and is MEASURED;
A's 6,951 is an older figure and the `dc_gx.c:870` comment carrying it is now
**stale**. That alone does not settle it — the two counts may not cover the same
population. **The experiment is a direct count of distinct vertex references per
batch.** Until it runs, neither "the memo is done" nor "there are 11 points
left" may be stated as fact.

⚠️ Failure mode: any per-batch constant *not* in the hash makes a stale entry an
exact-looking wrong vertex. The four "recorded and never consumed" bugs are the
cautionary tale — enumerate the constants from `shade_vertex`'s reads and assert
in debug.

### F4. Temporal predicted cull

**1-2 ms/frame. Near-zero risk.**

Thread batch identity (the source `Vtx` pointer, via the seam
`dc/include/dc_census_vtx.h` already proves) into `dc_gx`. If (identity, matrix
hash) was AABB-culled last frame, skip staging and the cull entirely. Degrade
safely by only skipping when the matrix hash is bit-identical — which is exactly
when a pop-in could not happen.

### F5. I-cache packing by linker placement

**Unknown, 0-10 %. Hardware-only measurement. Explicitly legal.**

SH7750's **8 KB** I-cache is direct-mapped (confirmed 2026-08-05; the D-cache is
16 KB — see F6) and `-ffunction-sections` is already on,
so a linker-script ordering block can place the dispatch loop, `dl_G_VTX`, the
TRI handlers, `set_position`, `GXPosition*` and `PSMTX*` contiguously and remove
*conflict* eviction between the interpreter and the GX layer it calls per
vertex. Pure layout — "codegen is banned; layout is fair game".

Failure mode: the `-O0` hot set is likely 40-80 KB, far over 8 KB, so this only
removes conflict misses, not capacity misses. **Flycast models no cache, so this
cannot be measured in the emulator at all** — it costs a burn cycle to evaluate.

### F6. OCRAM for the renderer's hot scratch

**Unknown. Hardware-only. Speculative.**

`.ocram` (8 KB at `0x7c001000`) is in the linker script and completely empty.
The SH-4's operand-cache-RAM mode gives 8 KB of 1-cycle scratch at the price of
halving D-cache. ⚠️ **Pin the numbers before sizing anything against this
(2026-08-05): the SH7750 has an 8 KB direct-mapped I-cache and a 16 KB
D-cache** — so OCRAM's price is halving a **16 KB** D-cache to 8 KB, not a
32 KB one, and the headroom both F5 and F6 trade against is half what an
SH-4-generic 16/32 assumption would give. The vertex memo (now 128 slots), 
`g_gx.current_vertex`, the folded matrix and the per-batch lighting block are
~2-4 KB together and would fit.

**The interesting near-miss:** the static `emu64_class` instance is ~8,312 B
(`emu64.c:5437`) — it misses fitting by ~120 bytes. If a future shrink of its
debug arrays (`dl_history`, `command_info`) got it under 8 KB, the *entire
interpreter state* could live in 1-cycle scratch, and for `-O0` code that
reloads `this->field` incessantly that could be the largest cache lever
available anywhere in this project.

Verify KOS actually enables OC-RAM index mode before spending anything.

### F7. `FrameCansel` as a frame-budget relief valve

**0 on average FPS; raises `fps_min`. Product-feel call.**

`FrameCansel` (`emu64.c:5700`) is a non-static global the dispatch loop tests
every command, and the game already handles the abort path. A dc/-owned watchdog
could set it when a traversal exceeds a budget, turning a 185 ms stall into a
bounded stutter with a partially-drawn frame.

⚠️ Failure mode: submission order means the late lists (XLU, UI) vanish first,
which is visually bad. Only viable with a budget well above p90.

### ⚠️ F8 — REOPENED 2026-08-04. The runtime mix does not match the static count.

Static counts say `src/data` carries ~1,400 sync commands total (881 PipeSync,
372 LoadSync, 139 TileSync) against 42k triangle commands, and
`dl_G_RDPPIPESYNC` is already a near-empty body — which is why this was ranked
last.

**F0 now says 73 % of the town's 2,867 commands per frame are state**, and
`vtx + tri + dl = 773` accounts for all the geometry. A static ratio over
`src/data` cannot predict the runtime mix, because the runtime mix is dominated
by which display lists actually execute, not by how many exist. Whatever those
2,094 commands are, they are ~26 ms of frame.

This is now **the thing G1 exists to name.** Do not build a strip rule against
the static count; build it against the histogram, and only for opcodes whose
handler bodies are genuinely no-ops on a PVR backend.

---

## The decision gate: interposing on emu64 through its dispatch table

**This is the user's call, not engineering's, and it must be surfaced rather
than buried.**

Fact 5 means `dc/` can replace individual emu64 handlers without editing `src/`:
`objcopy --globalize-symbol` on `emu64.c.o` (a Makefile post-step — layout, not
codegen) exposes the table, and `objcopy --redefine-sym` can interpose any
handler. `src/` stays unedited and stays at `-O0`; `dc/src/` already builds at
`-O2`. Three escalating uses:

⚠️ **RE-COSTED 2026-08-05 against G1's histogram. Both wins shrank, and the
biggest block in the frame turned out to need no gate at all.**

| | what | est. win | status |
|---|---|---|---|
| **G1** | **Per-opcode time histogram.** Wrap every table entry in a timing thunk that calls the original `-O0` handler. | 0 ms, unlocks G2/G3 | ✅ **RUN 2026-08-05.** `TRIN_INDEPEND 22.25/146`, `VTX 5.40/149`, `gap 7.92`. `kb/perf-dc.md` §2b |
| G2 | **Reimplement the dispatch LOOP only.** `emu64_taskstart_r` (`:5769-5901`) is one self-contained function, paid once per command × ~3,500 commands at `-O0`. A `dc/` `-O2` replacement calls the untouched `-O0` handlers through the same table. | ~~7-14 ms~~ → **2-4 ms** [ESTIMATED], hard cap ~8.3 ms | ✅ signed off 2026-08-04 and BUILT (`dc_emu64_shadow.cpp`); +0.8 FPS whole-run, town not separated out |
| G3 | **`-O2` shadow handlers for the VTX/TRI path, with a runtime batch AABB cull built in.** | ~~25-35 ms~~ → the cull half is **4.5-7.0 ms** [ESTIMATED]; the `-O2` half sits inside G2's ~8.3 ms cap | ✅ signed off 2026-08-04, unbuilt. **Build the cull half alone** (queue item 3) — that is where the whole win is |
| — | **`dc_gx_backend_submit`** (`dc/src/dc_pvr.c:2448`) | the block is **12.2 ms = 15.6 % of the draw phase**; the win in it is unmeasured | **no gate — it is `dc/` code already at `-O2`** |

**Why both shrank, in one measurement.** `GXEnd` is live at `emu64.c:4935`, so
the 152 µs/call charged to `G_TRIN_INDEPEND` already includes
`dc_gx_flush_vertices` → the AABB cull (~15 µs) and `dc_gx_backend_submit`
(~81 µs). **Only ~53 µs of it is `src/` at `-O0`** — i.e. 65 % of the "emu64"
bucket the G-series proposed to attack is code this project owns and compiles at
`-O2` today. Interposing on emu64's dispatch table buys much less than the
decision assumed, and item 4 of the table costs nothing politically.

**The honesty flag.** G2 and G3 do not violate the letter of the rules — `src/`
unedited, `src/` still `-O0`, `dc/` already `-O2`, kill switch trivial. But they
reimplement `src/` logic in optimised `dc/` code, which walks near the *spirit*
of the `-O0` directive. The distinction from what failed before is real: the
armhf failures were global `-O1`/`-O2` miscompiles of 3,917 TUs, where this is a
hand-vetted rewrite of 2-6 functions behind the screenshot and regression gates.
Real, but not engineering's to decide.

G3's specific technical risk beyond the political one: the decal-Z path in
`set_position` (`emu64.c:2724-2783`) is genuinely hairy, and divergence there
would present as z-fighting weeks later. Mitigation is to keep the decal path
calling the original `-O0` code and shadow only the common path.

⚠️ ~~**G3 is the only idea on this page large enough to reach ~20 FPS in the
town on its own.**~~ **FALSIFIED 2026-08-05.** That claim rested on the ~48 ms
`G_VTX` figure, which G1 measured at 5.40 ms. Re-costed, **nothing on this page
reaches 20 FPS in the town on its own**: G3's cull half is 4.5-7.0 ms and its
`-O2` half is inside an ~8.3 ms cap, against a 78.3 ms frame. Reaching 20 FPS
needs several of these together, or content work (`kb/levers.md` L5).

---

## What the 3DS port's perf commit does and does not give us

`AnimalCrossing-3ds-Port/ACGC-3ds` commit `cef117e` — "Skip VBlank waits on
frameskip ticks, add frame pacing, state caching". Read 2026-08-04. Four
changes, and **the two biggest are already banked here**:

| their change | our status |
|---|---|
| `VIWaitForRetrace` discards batched geometry and returns immediately without `gspWaitForVBlank()` on frameskip ticks — "saving ~33 ms per visual frame" | **already done.** `dc_vi.c`'s `g_pc_frameskip_active` branch returns before the swap and before `dc_pace_frame()`, and `vi=0.5 ms` at the median confirms nothing is waiting. |
| `GXSetZMode`/`GXSetCullMode`/`GXSetAlphaCompare`/`GXSetBlendMode` compare against current state and bail early | **already done** — `dc_gx_state_dedup`, on ~35 setters, and the early-out is correctly placed *before* `dc_gx_flush_if_begin_complete()` so a repeated setter does not break the batch. |
| `GXBegin` merges consecutive same-state primitives into one batch; `GXEnd` becomes a no-op when clean | ours is **dead** (`merged=0` in every window). Reviving it is worth ~0.4 ms by our own arithmetic — 228 batches x ~330 instructions of per-batch setup — so it is not where the time is. Kept as a known-small item. |
| **`acre_render` setting: 0 = 9 acres, 1 = 5 acres, 2 = 1 acre** | **the genuinely new idea, and it is out of reach as they implemented it.** They edited `src/actor/ac_field_draw.c`; we may not. But it attacks `cmds`, which IS the dominant predictor (12.31 µs/cmd in the town), so a draw-scope cut is worth more per byte of change than anything in the renderer. F1 is the reachable version of the same idea. |

### The seam that makes an `acre_render`-shaped lever reachable

`emu64_set_aflags(int idx, int value)` / `emu64_get_aflags(int idx)` are
**ordinary extern C functions** (`emu64.c:6048`, declared in
`include/libforest/emu64.h:13`) — i.e. `dc/` can write emu64's debug flag array
at runtime with no `src/` edit at all and no interposition trickery.

The flag names are in `include/libforest/emu64/emu64.hpp:75+`. Several are
work-skipping knobs the original developers used: `AFLAGS_SKIP_TEXTURE_CONV`,
`AFLAGS_SKIP_TILE_SETUP`, `AFLAGS_SET_DIRTY_FLAGS`, `AFLAGS_SETUP_ALL_TEVSTAGES`,
`AFLAGS_FORCE_PIPE_SYNC`, `AFLAGS_SKIP_ALPHA_COMPARE`,
`AFLAGS_SKIP_PROJECTION_TRANSFORM`, `AFLAGS_SKIP_W_CALCULATION`.

⚠️ These are DEBUG knobs, and most of them will break rendering — that is the
expected outcome, not a surprise. The value here is that **each is a one-line
build with a real A/B**, so the whole set can be swept cheaply for the one or
two that skip work the DC backend does not need. `AFLAGS_MAX_POLYGONS` is
explicitly NOT one of them: it forces emu64's slow one-triangle-at-a-time path
(`emu64.c:5073`) and would be slower.

**Unmeasured. This is a surface to sweep, not a result.**

## Measurement hygiene, learned the hard way this session

⚠️ **`DC_FB_PROBE` inflates the 1 % lows and I was quoting them.** The
framebuffer dump costs **1,506 ms**, smeared over the following 30-frame window,
and lands in the `vi` bucket. In one run it hit 26 of 358 windows and dragged p1
from **11.56 FPS to 8.50**. A counter-identical pair proves it: same `cmds=2629
v=2712 draws=90 culled=137 gx=13.0 draw=60.7`, one window at 8.5 FPS with
`vi=50.6` and two windows later 14.9 FPS with `vi=0.3`.

**Build perf runs WITHOUT `DC_FB_PROBE`.** Screenshot runs and perf runs are
different experiments again — `DC_SCIF_FAST` fixed the *progression* problem, not
this one.

Probe-free numbers for the current build, for future comparison:
`min 10.70, p1 11.56, p5 12.15, p50 24.30, max 29.90` (capped at `fps_target`).

## Where the worst frames are

**14 of the 17 worst probe-free windows are scene 9 — the outdoor town.** Scene
9 probe-free: min 10.7, p1 11.3, p50 14.9, max 14.9. The town never exceeds
14.9 FPS, and its worst windows are the run's worst. The 1 % lows are not a
different problem from the median; they are the same wall, deeper.

Within-scene fits (r > 0.95):
- `emu64_ms = 12.31 µs/cmd x cmds + 9.20 ms` in the town. **The 9.20 ms offset is
  independent of command count and is suspiciously equal to the `posms=9.15`
  the GX API census measured** — i.e. it may be the per-vertex GX entry points,
  which are ours and editable. Confirming that is the cheapest high-value
  experiment on this page after F0.
- `gx_ms = 4.56 µs/vertex x v + 0.69 ms`. Essentially linear through the origin.
- `draws` predicts nothing (r ~ 0.0). Batch count is not the cost; commands and
  lit vertices are.
