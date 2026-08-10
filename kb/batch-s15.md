# Batch S15 — the rollback contract

**Landed 2026-08-09.** Four changes, three of them independent, each with its own
kill switch. This file is what you read **before bisecting anything that broke
after 2026-08-09**, and it is the only place the one-line full revert lives.

Same discipline as `kb/batch-s14.md`: every change names its switch, its gate
counter, and what it would look like if it were wrong.

---

## Measured — one 600 s town run, `smoke-s15-measure-20260809-193333`

Build: shipping config + `DC_NPC_SEED=1 DC_NPCTEX_POOL=1 DC_NPCMDL_POOL=1`,
`-DDC_PERF_PHASE -DDC_PVR_TEVP3`, `DC_AUTOSTART=300`.

```
[PHASE]  draw=26.5 skip=2.7 vi=6.6 | cull=0.5 xform=6.7 | v=2751 us/v=2.43
[EMU64C] trin=7500 cull=4050 vis=1770 punt=1680 lproj=90
         cus=85044 cds=4674 fus=8860  (µs / 30-frame window)
fps_p50 27.3   ASSET MISSING 0   reinst=0   dropped 0   aram zero=0
```

| | pre-S15 | S15 |
|---|---:|---:|
| `us/v` | 2.48 | **2.43** |
| `cds=` ms/frame | 2.23 | **0.156** |
| `lproj=` / `trin=` | — | 90 / 7500 = **1.2 %** |

⚠️ **`us/v` −2 % is INSIDE the ±2 % noise floor (rule 11) and is NOT a claim.**
What is solid is `cds=`: the duplicated `dirty_check` + `setup_1tri_2tri_1quad`
went from 2.23 ms/frame to 0.156, exactly as designed, and `lproj=` confirms the
replicated projection refresh is standing guard (1.2 % of batches) rather than
doing real work. **The saving is smaller than 2.07 ms because the work MOVES to
the handler rather than disappearing** — total `cull_batch()` only fell 3.05 →
2.83 ms/frame. ⚠️ **And this run carried MORE load than the baseline** (a
14-villager roster and both NPC pools resident), which the comparison does not
adjust for. **S15-1's real value is expected to be larger on hardware than
here**: it removes ~2,500 executions/frame of a `dirty_check` that is **2,848 B
of `.text` at `-O3`** — ~89 lines against an 8 KB direct-mapped i-cache — and
Flycast models no i-cache.

⚠️ `.bss` 5,274,332 → **5,388,204** (+113,872) for the two NPC pools; span
10,628,492; `MEMLEDGER FIT … margin=3441396 OK`.

---

## The one-line full revert

```bash
DC_XDEFS='-DDC_CULL_NO_LEANSETUP' DC_NPC_SEED=0 bash dc/build-dc.sh
```

That restores the pre-S15 build **except** for the TEV predicate, which is
opt-in and therefore already off in any build that does not name
`-DDC_PVR_TEVP3`. `DC_OPT_OS_EXTRA` is likewise a lever that defaults to unset.

---

## S15-1 — G3's cull stops doing emu64's state work twice

**`dc/src/dc_emu64_cull.cpp`, in `cull_batch()`. ON by default. Kill switch
`-DDC_CULL_NO_LEANSETUP`.**

`cull_batch()` used to call `self->dirty_check(...)` and
`self->setup_1tri_2tri_1quad(first_vtx)` before the frustum test, because the
test reads `g_gx.projection_mtx` and `g_gx.current_mtx` and both are one command
stale until those two run. The original handler then calls **the same two
functions with identical arguments** (`emu64.c:4812-4813` for TRIN,
`:4952-4953` for QUADN) a moment later.

That double call was `cds=` — measured 2026-08-09 at **2.23 ms/frame of a
3.05 ms cull, 73 %**, with only **175 of 2,572** TRIN batches actually culled,
i.e. ~93 % of it paid to do work the handler immediately redid.

S15-1 replaces the pair with the two pieces of state the frustum test actually
reads, and nothing else:

```c
if (self->dirty_flags[EMU64_DIRTY_FLAG_PROJECTION_MTX]) {
    self->dirty_flags[EMU64_DIRTY_FLAG_PROJECTION_MTX] = false;
    GXSetProjection(self->projection_mtx, self->projection_type);
}
GXSetCurrentMtx((self->vertices[first_vtx].flag & MTX_NONSHARED) == MTX_SHARED
                    ? SHARED_MTX : NONSHARED_MTX);
```

**Why that is the complete set, and it is read off `dc_gx_aabb_offscreen_gh()`
rather than off a comment.** That function touches exactly two things in `g_gx`:
`projection_mtx`, and `pos_mtx[current_mtx]`.

| what | refreshed by | in the two calls? |
|---|---|---|
| `projection_mtx` | `GXSetProjection` | yes — `dirty_check`'s last block, `emu64.c:3432-3435`, guarded on `EMU64_DIRTY_FLAG_PROJECTION_MTX`. **Replicated.** |
| `current_mtx` | `GXSetCurrentMtx` | yes — `setup_1tri_2tri_1quad`'s first branch, `emu64.c:2859-2867`. **Replicated.** |
| `pos_mtx[slot]` | `GXLoadPosMtxImm` | **no.** The whole TU has three call sites (`emu64.c:547`, `:647`, `:4611`) and the live one is `dl_G_MTX` — a different command, already run. Never ours to refresh. |

**Why dropping the rest is not a behaviour change.** Everything else
`dirty_check` does is apply-and-clear against a **sticky** flag, so skipping a
call defers work, it does not lose it.

- **Culled batch** — the handler never runs, so the deferred work lands in the
  next batch's `dirty_check`. The culled batch submits no vertices, so no draw
  observed the difference. Strictly less work: a texture tile dirtied for a
  batch we then cull is no longer uploaded for nobody.
- **Visible batch** — the handler makes both calls itself before submitting a
  single vertex. It now finds the flags still set and does the work **once**
  instead of twice.

**Precedent:** the `CU_DECAL_ARM` path directly above already returns without
making either call, has shipped on by default since S14, and its block comment
states the same two facts.

**Not replicated, deliberately:** `using_nonshared_mtx`. The handler rewrites it
from the same `first_vtx` before any vertex is submitted — the same reasoning
the decal path already relies on.

### The gate

`[EMU64C] … lproj=` — batches on which the lean refresh found
`EMU64_DIRTY_FLAG_PROJECTION_MTX` actually set. Expected **small**: a projection
change is a per-scene-pass event, not a per-batch one. **If `lproj=` ever
approaches `trin=`**, the projection is being re-dirtied per batch, the
replication is doing real work rather than standing guard, and the saving needs
re-deriving.

⚠️ **`cds=` before and after S15-1 are not comparable** — it now brackets the
lean pair, not the full pair. Judge S15-1 on `us/v` and `draw`.

### What it would look like if it were wrong

Geometry vanishing or popping — a stale projection culls things that are on
screen (the ordering rule in the file's own comment). `-DDC_EMU64_CULL_VERIFY`
counts a batch we dropped that the reference drew; `falsecull=` must stay 0.

---

## S15-2 — the TEV P3 predicate, narrowed

**`dc/src/dc_pvr.c`, `tev_p3_affine()`. Still opt-in behind `-DDC_PVR_TEVP3`.
The narrowing itself is on whenever P3 is; `-DDC_PVR_TEVP3_WIDE` restores the
2026-08-09 predicate verbatim.**

### 🔴 The old diagnosis named the wrong mechanism

`kb/tev-map-hard-cases.md` §6.6a said the predicate "is recognising a SHAPE that
#037 shares with something common", which reads as *narrow the colour combine*.
**It cannot be fixed that way.** The colliding batches do not resemble #037 —
their recorded TEV state is byte-for-byte identical to the keyboard's, because
emu64 manufactures it:

> `emu64::combine_auto` computes `two_cycle = (othermode_high & G_CYC_2CYCLE)`
> (`emu64.c:1191`) and **in 1-CYCLE mode silently discards the cycle-1
> `COMBINED * SHADE` term.** A two-stage *lerp then modulate by shade* therefore
> arrives as the one-stage `(C2, C1, TEXC, ZERO)` — indistinguishable, on the
> colour combiner alone, from a genuine #037.

Simulated over all **5,507** `gsDPSetCombineLERP` sites in `src/`: **472**
produce a stage 0 the old predicate accepts, but only **150** are real
single-stage configs. The other **322** are shade-modulated DLs collapsed by the
1-cycle path, and **214 of those are `src/data/field/bg/`** — the acre ground,
the trees, the mailboxes. That is the purple town, exactly, and
`tmpr4.c:30-33` is a worked example: `PRIM(32,48,144) − ENV(144,128,96)` =
`(−112,−80,+48)`, so R and G clamp to 0 and the surface flattens toward ENV with
a blue-only texture response.

Cycle type is **runtime** state, set by `m_rcp.c`'s `z_gsCPModeSet_Data[15][6]`
setup DLs; the acre DLs never set it themselves. **So the same display-list bytes
are a 1-stage or a 2-stage config depending on what ran before them.**

### The fix: gate on an axis the collapse cannot touch

`combine_auto` drops the shade **term**, but the DL's `G_LIGHTING` and `G_FOG`
bits survive into `GXSetChanCtrl`/`GXSetFog`. So:

| | lit | fog |
|---|---|---|
| #037, the keyboard (`kb/tev-map-table.md` row 37) | 0 | 0 |
| a collapsed acre DL | 1 | 1 |

Four gates, in the order they are cheapest:

1. `g_gx.chan_ctrl_enable[0] \|\| [1]` → reject. **This is the fix.**
2. `g_gx.fog_type != GX_FOG_NONE` → reject.
3. stage 0 alpha must be `T0a` alone — excludes #010/#035/#043/#051/#052/#055/#074.
4. no alpha compare — **the only recorded axis separating #037 from #063.**

### The gate

`[DC/PVR] tevp3 batches=… clamped=… of … | rej lit=… fog=… alpha=… atest=…`

🔴 **MEASURED, AND IT CONTRADICTED THE ANALYSIS ABOVE — which is what the
per-gate counters were for.** 600 s town run, keyboard never opened:

```
[DC/PVR] tevp3 batches=9371 clamped=5205 of 1510330
         | rej lit=105 fog=57933 alpha=311815 atest=134
```

- **`alpha=` does 84 % of the work. `lit=` fired 105 times in a whole run.**
  The gate the entire collapsed-acre argument was built on is nearly inert. The
  1-cycle collapse is real — the static count over 5,507 sites stands — but it
  is **not** mostly separated by geometry-mode lighting. **Do not repeat that
  prediction.**
- `batches=` fell 20,305 → **9,371**, which is better and still far too many
  for a 26-display-list widget in a run that never opened it.
- `clamped=5205` — 55 % of survivors — is the tell, and it is about the
  CONSTANTS, not the combiner shape. Hence S15-2b.

### S15-2b — reject where the fix is not exact, instead of clamping

`base = PRIM − ENV` is packed into **unsigned** bytes, so a negative channel
clamps to 0 and the surface flattens toward ENV, losing the texture response in
that channel — `kb/tev-map-hard-cases.md` §6.2 predicted precisely this, and
`tmpr4.c:30-33` is the worked example: `PRIM(32,48,144) − ENV(144,128,96)` =
`(−112,−80,+48)`, R and G clamp, ground goes purple.

So the fifth gate refuses those batches outright and leaves them rendering as
they do today. `base >= 0` in all three channels is **exactly** the condition
under which `(PRIM−ENV)*T0 + ENV` reproduces GX in 8-bit unsigned — this is not
a heuristic. Counter `neg=`; with it on, `clamped=` must read 0. Kill switch
`-DDC_PVR_TEVP3_CLAMP_OK`.

⚠️ **It knowingly costs one keyboard element.** `kai_sousa.c:520-521` is
`PRIM(65,95,165) / ENV(125,45,225)`, base `(−60,+50,−60)`, so that piece stays
as it renders today. One element of the widget is the right trade against
recolouring the town.

🔴 **`-DDC_PVR_TEVP3` IS STILL OPT-IN AND MUST STAY THAT WAY until a run with
S15-2b reads `batches=` in the low hundreds AND a matched-frame screenshot pair
shows the town unchanged.** S15-2b has not been run.

⚠️ **Still owed:** the keyboard itself. `DC_AUTOSTART` presses index 0 and never
reaches name entry (`kb/RESUME.md` §8), so the widget has still never been
photographed. The free falsification is unchanged and is now known to be
well-founded in source: `mED_KeyDraw_keyboard` (`m_editor_ovl.c:2092`) draws the
**40 key caps** with an inline `gDPSetCombineLERP(0,0,0,PRIMITIVE, …)`, which is
the shape `tev_const_color()` already handles. **So the caps should be correct
today and the panel black. If the caps are black too, the whole diagnosis is
wrong and the fault is asset-side, not TEV.**

---

## S15-3 — N2c: villagers without a save file

**`dc/src/dc_npcseed.c`, injected into `mSDI_StartInitAfter` by
`tools/dcstub/make_src_shrink.py`. OFF by default (`DC_NPC_SEED=0`).**

### 🔴 The old diagnosis named the wrong subsystem

`kb/STATE.md` action A and `kb/RESUME.md` §7 item 1 both said the empty town is
a **save** bug and prescribed N2b, wiring the VMU. **`animals[]` is not loaded
from the save on a new game — it is generated**, by
`mNpc_DecideLivingNpcMax` (`m_npc.c:2422`) under
`mNpc_InitNpcAllInfo` (`:4161`), and `mSDI_StartInitNew` carries **no**
`mFRm_CheckSaveData()` gate (only `StartInitFrom`/`NewPlayer`/`Pak` do). Six
starter villagers, one per personality, get valid `id.npc_id` on any new boot.

What keeps them out of the town is one line in `mNpc_SetNpcList`
(`m_npc.c:2807`):

```c
if (ITEM_NAME_GET_TYPE(npc_id) == NAME_TYPE_NPC && animal->home_info.block_x != 0xFF)
```

`home_info` is left at `0xFF` by `mNpc_ClearAnimalInfo` and written **only** by
`mNpc_SetNpcHome` (`:2712`), fed by `mNpc_MakeReservedListBeforeFieldct`
(`:2549`) — a scan of `Save_Get(fg[][])` for `SIGN00..SIGN20` markers. If that
scan finds nothing, `reserved_num == 0`, `mNpc_SetNpcHome` returns without
writing a byte, and every slot is skipped **with no diagnostic**. The markers
come from `RESOURCE_FGDATA` via `mFM_InitFgCombiSaveData`
(`m_field_make.c:1490`), i.e. **ARAM** — nothing to do with the VMU.

⚠️ **The evidence the old claim rested on could not have supported it either.**
"Two 900 s runs printed zero `[DC/NPCTEX]`/`[DC/NPCMDL]` lines" is only evidence
if those builds had the pools on; at the documented defaults (`=0`) both files
compile to empty stubs and the load seam is not inserted at all, so silence was
guaranteed either way.

### What it does

Repairs `animals[]` in place immediately before `mNpc_SetNpcList`, **per slot,
and only where the slot is broken**:

- id not `NAME_TYPE_NPC` → `mNpc_SetDefAnimal()` (extern) against our roster,
  plus the three lines of `mNpc_SetNpcNameID` (which is `static`);
- `home_info.block_x == 0xFF` → a home from the roster.

A slot the game filled correctly is left alone, so the day the FGDATA reserve
scan starts working this file goes quiet on its own.

The roster is **`demo_npc_list` verbatim** (`m_trademark.c:51-67`) — the table
the title demo already feeds to the same `mNpc_SetDefAnimal`. Its coordinates
are the ordinary 1-based FG grid (`block_x ∈ [1,5]`, `block_z ∈ [1,6]`, `ut ∈
[0,15]`), not a demo-specific encoding, and the four acres holding two villagers
each keep them ≥3 units apart.

### The gate

`[DC/NPCSEED] pre ids=… homed=… | seeded id=… home=… | want=… max=…`

**The `pre` pair is the whole diagnosis, and it is printed before a single
write:**

| `pre` | meaning |
|---|---|
| `ids=0 homed=0` | the generator never ran — a start-path bug |
| `ids=6 homed=0` | generator ran, **homes** failed — the FGDATA reserve scan, as argued above |
| `ids=6 homed>0` | neither; something downstream eats the actors |

`DC_NPC_SEED=2` logs and writes nothing, which is the honest way to read that
line without this file's own writes contaminating it.

### Two things it does not do

- **No houses.** `mNpc_BuildHouseBeforeFieldct` is `static` in `m_npc.c`, which
  is in no rewrite set, so it is unreachable from `dc/src`. Villagers exist,
  have world positions, are in `npclist`, and walk; their houses are not stamped
  into `Save_Get(fg[][])`. `mNpc_SetAnimalTitleDemo` has the same gap.
- **No art without R2/R3.** Turn it on with `DC_NPCTEX_POOL=1
  DC_NPCMDL_POOL=1` or the villagers are correctly-positioned and invisible —
  measurement rule 8: those classes are 90,464 B and 5,536 B **resident**
  against 1,154,944 and 438,640 on paper. Not enforced, because a seed-only
  build separates "is an actor constructed" from "does it have art".

⚠️ **`DC_NPC_SEED_MAX` is an FPS dial, not a safety valve.** It sets
`now_npc_max`, and `mNpcW_GET_WALK_NUM` is `now_npc_max / 3`
(`m_npc_walk.h:12`), so 14 gives **4** simultaneously-walking skinned actors and
6 gives 2. If the town frame regresses, turn this down first.

---

## S15-4 — `DC_OPT_OS_EXTRA`, and the question it exists to ask

**`dc/Makefile` + `dc/build-dc.sh`. A lever, unset by default — it changes
nothing until it is named.**

Demotes a hot-list TU from `$(DECOMP_HOT_OPT)` to `$(DECOMP_OPT)` without
editing `dc/opt-lists.mk`. It is **not** a smaller `DC_OPT_O0_EXTRA` and it is
not for miscompiles. It asks one question `-O0` cannot:

> **Is `-O3` on `emu64.c` a net LOSS on real hardware because of the code it
> grows?**

The SH-4's instruction cache is **8 KB direct-mapped**.
`tools/dcopt/icache_map.py` sizes the innermost draw loop at **2.62×** it and
the whole hot set at **16.40×**. `-O3`'s inlining and unrolling makes both
worse. **Flycast models no instruction cache, so it scores `-O3` on its
instruction count alone and cannot see the trade** — this is measurement rule 12
pointed the other way: not "Flycast understates a locality win" but "Flycast
overstates an instruction-count win that costs locality".

### Measured host-side, 2026-08-09 — `emu64.c` `.text`, one TU

| profile | bytes | vs `-O3` |
|---|---:|---:|
| `-O3` (today's default) | 46,002 | — |
| `-O2` | 39,236 | **−6,766 (−14.7 %)** |
| `-Os` | 34,199 | **−11,803 (−25.7 %)** |

Per-symbol, the tier-1 draw-loop subset totals **6,824 → 4,772 B (−30 %)** at
`-Os`. The single biggest mover is `dirty_check`: **2,848 → 1,532 B**, and that
function runs once per TRIN batch — 89 cache lines' worth of instruction
footprint touched ~2,500 times a frame, which is also why S15-1's removal of its
**second** call should pay more on silicon than in the emulator.

⚠️ **This is a burn question and nothing else can answer it.** Build both arms,
burn both, compare. `dc_pmcr.c`'s `istall` on `DC_PMCR=1` is the instrument.

```bash
# arm B — the -Os variant of the hottest TU
DC_OPT_OS_EXTRA=src/static/libforest/emu64/emu64.c … bash dc/build-dc.sh
```

---

## S15-5 — the CD-R short read. A hardware-only bug Flycast cannot execute

**`dc/src/dc_dvd.c`, `dc_dvd_read_yielding()`. ON by default. Kill switch
`-DDC_DVD_SHORT_READ_EOF`.**

The chunked read loop contained:

```c
if ((u32)n < want) break;               /* short read: EOF */
```

**A short read is not EOF.** `fs_read` is KOS's VFS primitive and may return
fewer bytes than requested — the ISO9660 driver services out of its own sector
cache and can hand back a partial buffer at a sector boundary, after a read
retry, or when the request straddles what it currently holds. Only `n == 0` is
EOF. The old code abandoned the rest of the request and returned `done < len`.

### Why this is the shape of the reported fault

The user reported, on hardware, "the textures are fucked up" and "on the title
screen the text is missing to start game" — while the same build looks correct
in Flycast. The chain is exact:

```
dc_texpool_fetch()            T1 stages a display-list texture off /cd/foresta.rel
  -> dc_assetwin_read()       DIRECT, UNALIGNED read at the row's rom_off
     -> dc_dvd_read_yielding()   short read -> break -> returns done < len
  <- returns 0                 (dc_assetwin.c:178, `!= len`)
  -> s_fetch_fail++, return NULL
     "[DC/TEXPOOL] ... disc read FAILED — this texture will draw untextured"
```

⚠️ **And T1 makes every one of the 6,068 display-list texture rows travel that
path**: `dc_texpool_map.inc` reports `DC_TEXPOOL_RESIDENT_N 0`, because
`DEMAND_STUB` beats a keep-list entry. A kept file's textures are still read off
the disc. So this failure mode reaches the title screen even though
`keeplist-full.txt` is a verified strict superset of both `keeplist-town.txt`
and `keeplist-opening.txt` (checked, 0 lines missing from either).

⚠️ **`dc_assetwin.c:99-100` already knew the risk and it was not connected to
this:** *"KOS's iso9660 layer starts a drive-level stream only on a
sector-aligned read"* — and T1's default path (`DC_ASSETWIN_B=0`) reads at
arbitrary unaligned `rom_off`.

### The gate

`[DC/AWIN] … sr=<n> sf=<n>` (`dc_dvd_short_stats`).

- `sr=` short reads that were **continued** instead of abandoned.
- `sf=` loops that still ended with `done < len`.

🔴 **MEASURED, AND IT FALSIFIED THIS SECTION'S FIRST DRAFT.** That draft said
Flycast "cannot execute the branch at all" because `FastGDRomLoad=yes` satisfies
every read in one call. **Wrong.** `smoke-s15b-20260809-200908`, 360 s, Flycast:

```
[DC/AWIN] OFF (DC_ASSETWIN_B=0): req=241 reads=241 bytes=286528 fail=0 narrow=0 sr=3 sf=3
```

`sr=3` — the branch fires in the emulator. `sr == sf` means all three were
**genuine EOF**: a caller asked for more than the file holds, the loop
continued, got nothing more, and ended short. The old `break` produced the
identical result for those three, so **the fix changes nothing about them.**

⚠️ **`sr`/`sf` are GLOBAL across every caller of `dc_dvd_read_yielding`, not
just T1's.** The same run has AWIN `fail=0` over 241 requests, so none of the
three was a texture read — they belong to the ARAM pager, `DVDRead`, or the
keep-list sweep, all of which legitimately read a file's tail.

**So `sf > 0` is NOT automatically a fault, and the "must be 0" gate this file
first published is wrong.** The meaningful texture gate is
`[DC/AWIN] … fail=`, which is per-request and reads 0.

### What this does and does not establish

- **The old code was wrong**, unconditionally: a short read is not EOF, and only
  `n == 0` is. That stands on its own.
- **It is NOT established that this is the cause of the reported garbled
  textures.** The emulator evidence now shows the branch firing only at EOF, and
  the proven cause found in the same session is S15-6 below. Treat S15-5 as a
  correctness fix with an open question on hardware, not as the fix.
- **What a burn would still show:** `sr` much larger than `sf` would mean the
  drive is returning genuine mid-file partial reads that the old `break`
  silently truncated. That asymmetry, not `sr` alone, is the evidence.

---

## S15-6 — 🔴 THE PROVEN GARBLED TEXTURE. A static-homonym stub

**`tools/dcstub/make_stub_data.py`, `scan_asset_declarations()`. Generator-side;
`src/` untouched. No runtime switch — it is a defect fix.**

`scan_asset_declarations()` wrote `out[name] = (size, owner, is_static)`
unconditionally while walking `sorted()` files, so for a symbol declared in more
than one file **the last file won** — and it decided the linkage flag T1 uses to
exclude file-statics.

`lat_letter01_04_tex` and `lat_tegami_fusen_tex` are **`static` in
`src/data/model/dia_win.c`, `dia_win2.c` and `dia_win3.c`** and **global in
`src/data/model/lat_letter64_xk_tex.c`**, which sorts last. Recorded linkage:
"global". T1's `if is_static: continue` never fired, both names entered
`DEMAND_STUB` — and `keep_symbol()` is keyed on the **name only**, so the
rewriter stubbed the three `dia_win` *statics* to `u8 x[1]` and rewrote their
loaders away, **while their display lists went on binding them**:

```c
/* dc/build/stubsrc/src/data/model/dia_win.c — BEFORE the fix */
static u8 lat_letter01_04_tex[1] ATTRIBUTE_ALIGN(32);
...
gsDPSetTextureImage_Dolphin(G_IM_FMT_CI, G_IM_SIZ_4b, 16, 16, lat_letter01_04_tex),
```

`decode_gc_texture()` then read **0x80 bytes out of a one-byte array** —
whatever the linker put next, indexed through a live CI4 palette, i.e.
**right-shaped garbage in plausible colours**. This is precisely the failure
`dc_texpool.c` says must never happen, on the one path T1 does not guard.

### Why it hid

- **`dia_win*` is the DIALOGUE BOX**, and the 900 s autowalk never opens one —
  `kb/STATE.md`'s "a class the autowalk never binds", now identified.
- **T1's probe is structurally blind to it.** A bind landing on a non-map
  address is counted as `unmapped`, a bucket the report calls EXPECTED. And
  under the loader every row has `kept == 0`, so `lookup()`'s early-out makes
  `interior=`/`mutated=` **incapable of incrementing at all**. ⚠️ **Every
  `interior=0 mutated=0` verdict recorded for T1 was taken in a configuration
  where those counters could not move.** The only honest probe config is
  `DC_TEXPOOL_PROBE=1 DC_TEXPOOL_DEMAND=0` with `keeplist-town.txt`.
- **It is NEW with `keeplist-full.txt`.** Under `keeplist-town.txt` the
  `dia_win` files were not kept, so nothing drew and nothing was wrong.

### The fix and its checksum

A name is excluded from the demand path if **any** declaration of it is static,
or if it is declared in **more than one file** — T1 keys on an ADDRESS, and a
name with two definitions does not have one.

```
TEXPOOL_STATIC_EXCLUDED_EXPECTED  6 -> 8
TEXPOOL_ROWS_EXPECTED          6068 -> 6066
TEXPOOL_BYTES_EXPECTED      2992480 -> 2992224      (-256 B = exactly 2 x 0x80)
```

⭐ **The delta being exactly two rows and exactly 256 B is the check** that the
linkage fix dropped those two and nothing else. Verified in the regenerated
tree: both arrays are `[0x80]` again with `dc_stub_keep_load_one()` calls
restored.

⚠️ **The same latent defect is one keep-list regeneration away from hitting the
PALETTES.** 40 `lat_letterNN_pal` symbols have the identical static/global
homonym shape; they are safe today only because the kept file's own static copy
survives. The fix above closes that too.

---

## S15-8 — 🔴 THE GARBLED TEXTURES. T1 cannot serve a TMEM-path texture

**`tools/dcstub/make_stub_data.py`, `scan_texture_operands()` +
`TEXPOOL_DIRECT_MACROS`. Generator-side. This is the fix for the fault the user
actually reported.**

### The mechanism

T1's hook is `dc_gx_backend_texture_upload()` — the PVR upload. That only works
when the **array's own address** is what reaches the upload. Two things happen
in this tree:

| macro | what emu64 does | T1 |
|---|---|---|
| `gsDPSetTextureImage_Dolphin` | records `texture_gfx.image`; the address flows straight to the upload | lookup **HITS**, row is staged off disc ✅ |
| `gDPLoadTextureTile`, `gDPLoadTextureBlock*` (pure N64 GBI) | **emulates TMEM** — `setup_texture_tile`/`texconv_tile` COPY the texels out of the array into `texture_buffer_data` (`emu64.c:41`), and the upload receives a pointer into *that* | lookup correctly returns −1 — **but the copy already read the ONE-BYTE STUB.** The garbage is baked in upstream of every hook T1 owns ❌ |

### The evidence, in order

1. **Screenshot, T1 on:** title screen renders logo, character and scenery
   perfectly, with **two bars of noise** where `Press START!` and
   `© 2001, 2002 Nintendo` belong.
2. **Screenshot, `DC_TEXPOOL_DEMAND=0` + `keeplist-town.txt`:** both lines
   render perfectly. **T1 is the cause.**
3. **`-DDC_TEXPOOL_TRACE=90`** (S15-7, new): the loader names the rows it
   serves. `logo_us_c_1_tex_txt` / `logo_us_c_2_tex_txt` — the "Animal
   Crossing" logo, which renders fine — **are fetched**.
   `log_win_nintendo1..3_tex` and `log_win_logo3/4_tex` **never appear.**
   ⭐ **The interesting result was an ABSENCE.**
4. **Source:** the five missing ones are the five `gDPLoadTextureTile` calls at
   `src/actor/ac_animal_logo.c:535-583`; the working ones are
   `gsDPSetTextureImage_Dolphin` (`logo_us_cros.c:103`).
5. **The bytes were never the problem** — 1,024 B at the map's `rom_off`
   9186144 in `foresta.rel` is a clean IA text texture (0xF0 background, 0xFF
   glyphs). The offset was right; the fetch simply never ran.

### The fix

Only `gsDPSetTextureImage_Dolphin` / `gDPSetTextureImage_Dolphin` are eligible
for the demand path. Any symbol referenced by a LoadTextureTile/LoadTextureBlock
macro **anywhere** stays resident — tie-break is always exclude, because one
TMEM site is enough to render garbage.

```
TEXPOOL_TMEM_EXCLUDED_EXPECTED   0 -> 154      (91,072 B back to residency)
TEXPOOL_ROWS_EXPECTED         6066 -> 5912
TEXPOOL_BYTES_EXPECTED     2992224 -> 2901152
```

⚠️ **The Dolphin `Load*` variants are treated as unsafe too**, though they may
well be direct. Proving it needs a read of emu64's tile path; being wrong
reproduces exactly this fault. A few KB of residency is the cheap side.

### Verified

Rebuilt with T1 on and `keeplist-full.txt`: **`Press START!` and the copyright
line render correctly.** Town run: `MEMLEDGER … margin=3364820 OK`,
`ASSET MISSING 0`, `fail=0`, scene 9, `fps_p50 26.8`. `.bss` 5,388,204 →
5,454,444; span 10,705,068. Human verdict on the town: *"visually looks pretty
good"*.

⚠️ **This was 154 rows, not 5.** The title screen is simply where it was
visible; every other TMEM-path texture in the game was equally garbled.

---

## S15-7 — `-DDC_TEXPOOL_TRACE=<N>`, the instrument that found S15-8

`dc/src/dc_texpool.c`. Names the first N rows the loader serves
(`fetch#<n> <name> row= off= have= need= kept=`). Needs `-DDC_TEXPOOL_PROBE=1`
for the name table, so it costs nothing shipped.

⭐ **Read it for ABSENCES.** A texture that renders as noise and is not listed
was never fetched: its bind missed the map, `src` stayed on the one-byte stub,
and the decoder read whatever the linker put next. That is a **lookup** failure,
not a **loader** failure, and `fetch=133 fail=0` cannot tell the two apart —
which is exactly why T1 looked healthy while the screen was wrong.

---

## What S15 deliberately did NOT touch

- **The 13.31 ms block** — `dl_G_TRIN`'s index expansion plus our own `GX*`
  attribute setters. Still the largest single block in the project, still
  unattributed, still unstarted. S15-1 removes a *duplicate* of emu64's state
  work; it removes no setter and expands no fewer indices. **Do not read one as
  the other** — that conflation is already recorded twice in this kb.
- **The section-order file.** F5 was checked and is correct: `section-order.txt`
  was regenerated after the mangled-symbol regex fix, and 80 of its entries are
  `_ZN5emu64*`. Nothing to redo.
