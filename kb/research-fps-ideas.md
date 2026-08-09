# Unbanked FPS concepts — ranked against a MEMORY-BOUND frame

Written 2026-08-04 from an out-of-the-box pass over the frame-time problem, then
vetted against the code. Pruned 2026-08-09: the settled entries are compressed to
their verdicts, the spent decision gate is one line, and the survivors are
**re-ranked against the measurement that changed everything below.**

> ## ⭐⭐⭐ THE RE-RANKING PREMISE: THE FRAME IS MEMORY-BOUND, NOT FPU-BOUND
>
> `[VTXSPLIT]` split `xform` into seven stages (`-DDC_PVR_VTXSPLIT=<N>`,
> `dc/src/dc_pvr.c`). **All the floating-point stages together are ~0.8 ms of a
> ~30 ms frame.** What costs is memory traffic: the memo's random read, the
> per-corner copy into the store queue, and the shade pass. Independently
> corroborated by `tools/dcopt/icache_map.py` — the 12-symbol innermost draw loop
> is **1.4x** an 8 KB direct-mapped icache and the whole town hot set is
> **11.9x**. ⚠️ **Flycast models neither cache, so both are understatements.**
>
> **What that does to this page:** every idea whose mechanism is *arithmetic* is
> demoted, and every idea whose mechanism is *locality* (F5, F6) is promoted —
> but both of those are **hardware-only to measure**, which is exactly why they
> are still unbanked. The ideas that remove *work* (F3, F4) keep their value
> because no optimizer and no cache can invent a skipped vertex.
>
> Live numbers: `kb/RESUME.md`. Evidence: `kb/state-log.md`. The renderer queue
> proper: `kb/research-sh4zam-gap.md` §3.

⚠️ **Read `kb/perf-dc.md` §4 first.** Everything already ruled out with evidence
— texture upload cost, strip/fan conversion, the batch-merge path, the frameskip
tick, `pvr_dropped` — is not repeated here.

⚠️ **Measurement rule 11 (2026-08-09): the `us/v` noise floor is ~±2 %.** Nothing
on this page estimated at under ~4 % can be resolved by a single A/B pair.

---

## The settled entries — verdicts only

| # | verdict | where the reasoning lives |
|---|---|---|
| **F0** — print the counters emu64 already maintains | ✅ **DONE 2026-08-04.** `[EMU64]` prints `cmds noop vtx tri dl \| cullvis cullrej` next to `[PHASE]`. Its own headline reading ("73 % of commands are state, therefore ~26 ms") was an averaging error and was overturned by G1 | `kb/closed.md` (F8), `kb/traps.md` "An average cost per command…" |
| **F1** — offline bbox-`CULLDL` injection into acre/model display lists | ❌ **NOT RECOMMENDED.** Costs **594 KB of `.data`** for the town scope (not the 60-120 KB it claimed), its own size cap selects nothing (no `gsSPVertex` in `src/data` exceeds 32 vertices), the bboxes cannot be computed without the ROM, and **G3 already delivered a bigger version of the same win for zero bytes** | `kb/closed.md` §F1 |
| **F2** — XMTRX residency token for `dl_G_VTX`'s two `PSMTXMultVec` calls | ⚠️ **PREMISE FALSE, REIMPLEMENTED.** The two calls use **different** matrices (`emu64.c:4708` `position_mtx`, `:4710` `model_view_mtx`), so a residency cache misses every time. What shipped instead: `PSMTXMultVec`/`PSMTXMultVecSR` take FTRV only when the matrix is provably resident and go **plain scalar** otherwise, with the miss path deliberately not touching XMTRX. Kill switch **`-DDC_MTX_NO_XMTRX_CACHE`**; `dc_mtx_xmtrx_invalidate()` is the explicit seam `dc_pvr.c` calls after its per-batch `mat_load`. **Still unmeasured** | `dc/src/dc_mtx.c` |
| **F8** — strip RDP/RSP state commands | ❌ **ANSWERED BY G1, worth ~nothing.** Every state opcode is ≤ 0.55 ms/frame. A large *count* of near-free commands is worth nothing to strip. **Do not build a strip rule** | `kb/closed.md` §F8 |
| **the decision gate — interposing on emu64 through its dispatch table** | ✅ **SPENT.** G1 ran, G2 is dead (its target was `gap`, which the `-O3` hot list collected), and **G3 shipped 2026-08-08 and is ON by default** (`DC_EMU64_CULL ?= 1`). The elaborate `objcopy --globalize-symbol` machinery, the honesty flag and the sign-off request are all moot. **The expensive general lesson: this section spent real design effort engineering around a ban whose evidence was one unreproduced armhf session — check what a ban actually rests on before engineering around it** | `kb/closed.md` §G2, `kb/RESUME.md` §0d |

⚠️ **HAZARD, STILL LIVE, FOR ANY NEW INSTALLER INTO THE DISPATCH TABLE.**
`dc_emu64_hist.c` and `dc_emu64_shadow.cpp` each `memcpy` **all 64 slots** on
frame open and restore all 64 on close; G3 re-installs its two. A per-slot
installer that arms *after* either of them is silently reverted on the next frame
boundary; one that arms *before* is captured into their saved copy. **G3's
tripwire must run BEFORE `dc_emu64_hist_frame_open()` at both `dc/src/dc_vi.c`
sites** (`:490` frameskip, `:839` presented) or G1 bills TRIN's time to whichever
opcode ran before it. `reinst=` is the tripwire. G1 and G2 are already mutually
exclusive by `#error` (`dc/include/dc_platform.h:417`). Full write-up:
`kb/traps.md`.

---

## The five facts these ideas rest on

All verified in the tree 2026-08-04. **These are statements about the CODE, not
about its cost, so they survive every re-measurement.**

1. **A large majority of the vertex references emu64 produces used to be thrown
   away by our own AABB cull, AFTER emu64 had paid for them.** ⭐ **G3 acted on
   this and it was the biggest win the project has booked** — the cull moved to
   `G_TRIN_INDEPEND` entry, so those references never reach `set_position` or our
   `GX*` setters at all. The late cull's yield collapsed accordingly. ⚠️ G3
   **added** cull calls rather than replacing them — punt and visible batches both
   still fall through to `GXEnd` → the late cull.
2. **emu64 already contains a working display-list cull.** `dl_G_CULLDL`
   classifies vertex-cache entries against six clip planes and pops the DL stack
   on a hit — exactly the "make emu64 skip a whole object" mechanism wanted,
   already implemented, already counted.
3. **The game's data almost never uses it.** `gsSPCullDisplayList` appears
   **twice** in all of `src/`: `ac_field_draw.c:325` (an acre-sized box around the
   whole acre BG list) and `obj_item_fish.c:62`. Acre-level cull already runs; the
   waste is the *contents* of partially-visible acres and per-object models — the
   granularity the game never culled.
4. **The acre and object display lists are C text, not binary.**
   `src/data/field/bg/acre/` holds 268 acre models as `Gfx foo_model[] = {...}`.
   `grep -rE "gsSPDisplayList\(&" src/data` returns **0** and `gsSPBranchList`
   returns **0** — no interior pointers into any `Gfx` array, so an offline
   rewriter can restructure them safely, through the proven scratch-tree
   mechanism that leaves `src/` untouched.
5. **emu64's dispatch table is writable.** `static dl_func
   dl_func_tbl[NUM_COMMANDS]` is a non-const file-static array of member-function
   pointers in `.data`. There is no MMU write protection anywhere. (This is what
   G1 and G3 both use.)

---

## The surviving ideas, re-ranked

### F5. I-cache packing by linker placement — ⭐ PROMOTED

**Unknown, 0-10 %. Hardware-only measurement. Explicitly legal.**

SH7750's **8 KB** I-cache is direct-mapped (the D-cache is 16 KB — see F6) and
`-ffunction-sections` is already on, so a section-ordering block can place the
dispatch loop, `dl_G_VTX`, the TRI handlers, `set_position`, `GXPosition*` and
`PSMTX*` contiguously and remove *conflict* eviction between the interpreter and
the GX layer it calls per vertex. Pure layout, no codegen argument needed.

⭐ **Why this is now the top unbanked idea rather than a curiosity.**
`tools/dcopt/icache_map.py` (host-side, seconds, no burn) measured the pressure:
the town frame's hot symbols are **11.9x** the icache and **the 12-symbol
innermost draw loop is 1.4x** it — `dc_gx_backend_submit` shares cache lines with
six of the `GX*` setters it calls per vertex. That is a *conflict* miss pattern,
which is exactly what ordering fixes. It also lines up with the human report that
**hardware is materially slower than Flycast and nobody knows why**
(`kb/RESUME.md` §0h).

- ⚠️ **`--symbol-ordering-file` is an LLD flag and we use GNU ld.** 2.45.1 has
  **`--section-ordering-file FILE`**, which does the same job.
- ⚠️ **Flycast models no cache, so this cannot be measured in the emulator at
  all.** It costs a burn cycle to evaluate, and `dc/src/dc_pmcr.c`'s `istall`
  event is the instrument (`DC_PMCR=1`, `DC_PMCR_HUD=1`, and
  `DC_CONSOLE_MUTE=1` is not optional on a measuring burn — read `kb/traps.md`
  first).
- ✅ The `-O0` reversal helped it: `.text` roughly halved, so the hot set that
  has to fit shrank by about the same factor, and `-Os` optimises *for* code size,
  which is what an I-cache wants.

### F6. OCRAM for the renderer's hot scratch — ⭐ PROMOTED, with its premise corrected

**Unknown. Hardware-only. Speculative.**

`.ocram` (8 KB at `0x7c001000`) is in the linker script and completely empty. The
SH-4's operand-cache-RAM mode gives 8 KB of 1-cycle scratch.

⚠️ **Pin the numbers before sizing anything against this: the SH7750 has an 8 KB
direct-mapped I-cache and a 16 KB D-cache.** So OCRAM's price is halving a
**16 KB** D-cache to 8 KB, not a 32 KB one, and the headroom both F5 and F6 trade
against is half what an SH-4-generic 16/32 assumption would give.

The candidates that fit: the vertex memo (`VMEMO_SLOTS 128`,
`dc/src/dc_pvr.c:2640` — `s_vmemo_src`, `s_vmemo_tag`, `s_vmemo_vid` and
`s_vmemo_val`), `g_gx.current_vertex`, the folded matrix, and the per-batch
lighting block. ⭐ **The memo is precisely the structure the split identified as
a cache miss**, which is what promotes F6 — but note the vertex-index side
channel has already removed the *random read into `verts[]`* that made it
expensive, so F6's remaining target is `s_vmemo_val` itself.

⚠️ **The "entire interpreter state in 1-cycle scratch" idea is DEAD.** It rested
on the static `emu64_class` instance (~8,312 B) missing 8 KB by ~120 bytes, and
on `-O0` code reloading `this->field` incessantly. `emu64.c` is compiled at `-O3`
now, so the reloads are exactly what the optimizer keeps in registers. If F6 is
revived it must be re-argued from the `-O3` disassembly.

Verify KOS actually enables OC-RAM index mode before spending anything.

### F3. Memo generation counter instead of a per-batch flush

**Low risk. ⚠️ Its cost basis moved twice and it needs re-deriving before coding.**

The vertex memo is invalidated at the top of every `dc_gx_backend_submit()`
(`dc/src/dc_pvr.c:3213`). But emu64 splits one object into many consecutive
batches while the folded matrix, lights and TEV constants are often unchanged.
Hash the per-batch constants and invalidate only when the hash moves.

⚠️ **Two things changed under this idea, and both must be handled:**

1. **The memo no longer hashes vertex content — it keys on a vertex-index stamp**
   `(epoch << 8) | index` written into `DCGXVertex`'s two dead padding bytes
   (`-DDC_GX_NO_VTXID`). So F3's saving is no longer "skip a hash"; it is only
   "keep hits across a batch boundary".
2. 🔴 **The epoch is load-bearing and it points the other way.** `GXBegin`
   **merges** batches, and emu64 **reloads `vertices[]` between two TRIN commands
   in one submit** — the epoch exists so a stale index MISSES. F3 proposes
   retaining entries across exactly the boundary the epoch was added to
   invalidate. **Any F3 implementation must show that the constants hash also
   covers emu64's `vertices[]` reload, or it will produce an exact-looking wrong
   vertex.** The four "recorded and never consumed" bugs are the cautionary tale
   — enumerate the constants from `shade_vertex`'s reads and assert in debug.

⚠️ Open, and it decides whether F3 is worth anything: **is the memo at its
ceiling?** Nothing in the tree counts distinct vertex references per batch,
although `dc_emu64_cull.cpp`'s `mark[4]` bitset (`:363`) already holds exactly
the distinct set for every TRIN batch — one `__builtin_popcount` over it against
`n_faces * 3` answers it directly. **Until that runs, neither "the memo is done"
nor "there are N points left" may be stated as fact.**

### F4. Temporal predicted cull

**Near-zero risk. ⚠️ Re-cost against G3 before building it.**

Thread batch identity (the source `Vtx` pointer) into `dc_gx`. If
(identity, matrix hash) was AABB-culled last frame, skip staging and the cull
entirely. Degrade safely by only skipping when the matrix hash is bit-identical —
which is exactly when a pop-in could not happen.

⚠️ **G3 took most of this idea's headroom**: the cull now happens at
`G_TRIN_INDEPEND` entry, before staging, so F4's "skip staging" half is largely
already collected. What is left is skipping **the cull test itself** on batches
that were rejected last frame — and `dc_emu64_cull.cpp` has **zero `dc_time_us`
reads**, so the cost of that test is not measured anywhere. **Add a bracket
before costing F4.**

### F7. `FrameCansel` as a frame-budget relief valve

**0 on average FPS; raises `fps_min`. Product-feel call, not an optimisation.**

`FrameCansel` is a non-static global the dispatch loop tests every command, and
the game already handles the abort path. A `dc/`-owned watchdog could set it when
a traversal exceeds a budget, turning a long stall into a bounded stutter with a
partially-drawn frame.

⚠️ Failure mode: submission order means the late lists (XLU, UI) vanish first,
which is visually bad. Only viable with a budget well above p90.

### The `aflags` seam — a surface to sweep, not a result

`emu64_set_aflags(int idx, int value)` / `emu64_get_aflags(int idx)` are
**ordinary extern C functions** (`emu64.c:6048`, declared in
`include/libforest/emu64.h:13`) — `dc/` can write emu64's debug flag array at
runtime with **no `src/` edit and no interposition trickery at all.**

The flag names are in `include/libforest/emu64/emu64.hpp:75+`. Several are
work-skipping knobs the original developers used: `AFLAGS_SKIP_TEXTURE_CONV`,
`AFLAGS_SKIP_TILE_SETUP`, `AFLAGS_SET_DIRTY_FLAGS`, `AFLAGS_SETUP_ALL_TEVSTAGES`,
`AFLAGS_FORCE_PIPE_SYNC`, `AFLAGS_SKIP_ALPHA_COMPARE`,
`AFLAGS_SKIP_PROJECTION_TRANSFORM`, `AFLAGS_SKIP_W_CALCULATION`.

⚠️ These are DEBUG knobs and most will break rendering — that is the expected
outcome, not a surprise. The value is that **each is a one-line build with a real
A/B**, so the whole set can be swept cheaply for the one or two that skip work the
DC backend does not need. `AFLAGS_MAX_POLYGONS` is explicitly NOT one of them: it
forces emu64's slow one-triangle-at-a-time path and would be slower.

**Unmeasured.**

### The one idea worth taking from the 3DS port

`AnimalCrossing-3ds-Port/ACGC-3ds` commit `cef117e`. Three of its four changes are
already banked here (frameskip VBlank skip → `dc_vi.c`'s `g_pc_frameskip_active`
branch; state-setter dedup → `dc_gx_state_dedup` on ~35 setters, correctly placed
*before* `dc_gx_flush_if_begin_complete()`; `GXBegin` batch merging → ours is dead
and worth ~0.4 ms by our own arithmetic). Full comparison of what the upstream PC
port does and does not give us: `kb/upstream-pc-port.md`.

**The genuinely new one: the `acre_render` setting — 0 = 9 acres, 1 = 5 acres,
2 = 1 acre.** They edited `src/actor/ac_field_draw.c`; we may not. But it attacks
the command count, which is the one lever neither the compiler nor a cache can
take. F1 was the reachable version and is dead on bytes (`kb/closed.md`); the
`aflags` seam above and content scoping (`kb/levers.md` L5, the user's call) are
the remaining routes.

---

## Measurement hygiene, learned the hard way

⚠️ **`DC_FB_PROBE` inflates the 1 % lows and they were being quoted.** The
framebuffer dump costs **1,506 ms**, smeared over the following 30-frame window,
and lands in the `vi` bucket. In one run it hit 26 of 358 windows and dragged p1
from **11.56 FPS to 8.50**. A counter-identical pair proves it: same
`cmds=2629 v=2712 draws=90 culled=137 gx=13.0 draw=60.7`, one window at 8.5 FPS
with `vi=50.6` and two windows later 14.9 FPS with `vi=0.3`.

**Build perf runs WITHOUT `DC_FB_PROBE`.** Screenshot runs and perf runs are
different experiments — `DC_SCIF_FAST` fixed the *progression* problem, not this
one.

⚠️ **And `shot_diff.py` cannot gate an optimization change at all**: probes fire
per presented frame, so a faster build samples a different point in the same
camera pan. Judge the SCENE, not the pixels.

⚠️ **`us/v` is the instrument; `draw` and FPS are not.** The town reseeds every
boot (`sys_math.c:7` seeds from `sqrand(osGetCount())`), so two runs of the same
build draw different towns. `us/v` normalises for vertex COUNT but **not for
WHICH vertices** — the lit / textured / punch-through mix moves with the town,
which is where the ±2 % floor comes from.

## Where the worst frames are

⚠️ **The absolute FPS figures in this section are `-O0`** — the headline
"the town never exceeds 14.9 FPS" is long false, and the `12.31 µs/cmd` slope is a
per-command cost of `-O0` emu64. **Re-fit both regressions before quoting a
slope.** What is quoted below is kept for its SHAPE, which has held through every
re-measurement since.

**14 of the 17 worst probe-free windows are scene 9 — the outdoor town.** Its
worst windows are the run's worst. **The 1 % lows are not a different problem
from the median; they are the same wall, deeper.** This is the reason there is no
separate "stutter" workstream on the renderer side.

Within-scene fits (r > 0.95), coefficients `-O0`, shapes durable:

- `emu64_ms = 12.31 µs/cmd × cmds + 9.20 ms` in the town. **Command count is the
  dominant predictor** — which is what makes a draw-scope cut worth more per byte
  of change than anything in the renderer. The 9.20 ms offset is independent of
  command count and was suspiciously equal to the GX API census's `posms=9.15`;
  ⚠️ that coincidence was never confirmed, and `kb/perf-dc.md` §4 later showed
  `posms=` is mostly the probe measuring itself.
- `gx_ms = 4.56 µs/vertex × v + 0.69 ms`. Essentially linear through the origin.
- **`draws` predicts nothing (r ≈ 0.0). Batch count is not the cost; commands and
  lit vertices are.**
