# Session log — what was observed running, in order

## 2026-08-05 — G1 ran, and ONE opcode is 28 % of the town frame

Run `smoke-G1-20260805-160640-92325`, town (scene 9), probe-free,
`DC_EMU64_HIST=1`, 11.5-12.1 FPS. This is the first per-opcode number the
project has ever had, and it moves the FPS argument off `G_VTX`.

```
[PERF]   11.5 FPS | draws=167 culled=191 cmds=3458 gx=15.2ms
[PHASE]  draw=78.3 skip=8.2 (n=30) vi=0.4 | cull=2.2 xform=12.2 | v=3002 vsrc=3002 vlit=2517 vcull=6042 us/v=4.07
[EMU64]  cmds=3458 noop=1 vtx=298 tri=295 dl=276 | cullvis=7 cullrej=2
[EMU64H] tot=42.86ms gap=7.92ms probe=79.9ns | TRIN_INDEPEND 22.25/146 VTX 5.40/149 MTX 2.18/113
         MOVEMEM 0.55/207 TEXRECT 2.17/25 SETTILE_DOLPHIN 0.32/109 DL 0.29/138 SETCOMBINE 0.28/58
         SETTIMG 0.25/112 TRI2 0.19/2 ENDDL 0.18/131 LOADTLUT 0.13/42
```

| | ms / calls | share |
|---|---|---|
| **`G_TRIN_INDEPEND`** | **22.25 / 146 = 152 µs per call** | **63 % of emu64 dispatch, 28 % of the whole 78.3 ms frame** |
| `G_VTX` | 5.40 / 149 | 13 % of dispatch |
| `G_MTX` | 2.18 / 113 | 5 % |
| `G_TEXRECT` | 2.17 / 25 | 5 % |
| every other opcode | ≤ 0.6 ms | — |
| **`gap`** | **7.92 ms** | **18 % of `tot`, and unexplained — OPEN** |

`G_TRIN_INDEPEND = 0x0A` (`include/libforest/gbi_extensions.h:52`), table index
60, handler at `emu64.c:4798` forwarding to `dl_G_TRIN` (`emu64.c:4802-4940`).
`gap` is time inside the draw phase but outside any emu64 command; nothing in
this run attributes it. **Write it down as an open question, not as overhead.**

### The `G_VTX ≈ 48 ms` figure this project has been costing G3 against was
### WRONG, twice over

`kb/research-fps-ideas.md` F0, `kb/STATE.md`, `kb/RESUME.md` rule 7 and
`kb/traps.md` all carried "265 `G_VTX` carrying ~6,951 vertices at ~6.9 µs each
is ~48 ms, i.e. most of the emu64 budget". Measured, `G_VTX` is **5.40 ms**.
Two independent errors produced that number:

1. It applied a whole-command average (12.31 µs/cmd, then ~6.9 µs/vertex) to a
   subset — **exactly the failure measurement rule 7 was written to prevent, in
   the same sentence that states the rule.**
2. The ~6,951 "vertices" were `GXPosition3f32` **references**, not loaded
   vertices. `G_VTX` loads ~3,601; the rest are re-emissions of the same
   sources, which is precisely what the vertex memo exists to catch.

All four sites are corrected in place.

### 65 % of the heavy opcode is ALREADY `dc/` code at `-O2`

`GXEnd` is live at `emu64.c:4935`, so the 152 µs charged to `G_TRIN_INDEPEND`
includes everything `dc_gx_flush_vertices` triggers: the per-batch AABB cull
(~15 µs) and `dc_gx_backend_submit` (~81 µs). **Only ~53 µs of the 152 is `src/`
at `-O0`.**

Consequence for the open G2/G3 decision: "reimplement the TRIN loop at `-O2`" is
bounded above by **~8.3 ms** (146 × 53 µs, if the `src/` half went to zero) and
realistically recovers **2-4 ms** [ESTIMATED]. It is not the 25-35 ms G3 was
sold on. **The biggest single addressable block in the frame is
`dc_gx_backend_submit` at 12.2 ms — 15.6 % of the draw phase — and it is ours
(`dc/src/dc_pvr.c:2448`), needing no trampoline, no `objcopy` and no CLAUDE.md
argument.**

### The cull rate is a QUANTITY now, not an inference

New counter `dc_gx_phase_culled_verts` (`dc/src/dc_gx.c`, incremented on the
reject path), printed as `vcull=` in `[PHASE]`. **`vcull=6042` against
`v=3002`: 66.8 % of vertices are thrown away by `dc_gx_batch_is_offscreen()`
after emu64 has paid the full `-O0` price for them.** The old "60 %" was derived
across two differently-instrumented builds, and `pc_gx_culled_draws` counts
BATCHES, not vertices.

⚠️ **A cull is NOT worth 22.25 × 0.668 = 14.9 ms, and the arithmetic that says
so is wrong in an instructive way.** `dc_gx.c` returns *before*
`dc_gx_backend_submit`, so culled vertices already contribute **zero** to
`xform`. What an ideal cull at TRIN entry actually removes:

| term | ms | tag |
|---|---:|---|
| staging 6,042 vertices that are then rejected | 5.0-5.6 | ESTIMATED |
| the per-vertex AABB scan they cause (`cull=2.2`) | 2.1 | MEASURED |
| *minus* a new O(window) visibility test | −0.25…−0.40 | ESTIMATED |
| *minus* the irreducible per-call cost on culled calls | not separated | — |
| **defensible range** | **4.5-7.0, central ~6.0** | ESTIMATED |

At 78.3 ms/frame that is **11.5 → 12.2-12.6 FPS**. Quote the range, not the
14.9.

### OPEN — two analyses disagree about the vertex memo's ceiling

Unresolved, and it must not be written down as settled either way. The
disagreement is entirely in one input:

| | total staged refs | loaded vertices | refs/vertex | ceiling | vs measured 48.2-48.9 % |
|---|---:|---:|---:|---:|---|
| **A** | 6,951 (`dc_gx.c:870`'s comment + older docs) | 3,601 | 1.93 | **48.2 %** | the memo is AT its ceiling; further work is worth zero |
| **B** | `v + vcull` = 3,002 + 6,042 = **9,044** | 3,601 | 2.51 | **~60 %** | **~11 points of headroom** |

B's 9,044 rests on the new `vcull` counter and is MEASURED this run; A's 6,951
is an older figure and `dc_gx.c:870`'s comment carrying it is now **stale**.
That does not by itself decide it — the two counts may not be measuring the same
population. **The experiment that settles it is a direct count of distinct
vertex references per batch**, which is cheap and unbuilt. Until it runs, do not
plan memo work and do not close F3.

### Four instrument holes, and G1 could not have printed through any of them

The histogram had been "in the tree, never run" for a session. Getting one
number out of it took four fixes, all in `82cb299`, and three of them are the
same shape — **an instrument that arms and then cannot report**:

1. **G1's `[EMU64H]` report was nested inside `#ifdef DC_PERF_PHASE` while its
   arm sites were not.** A `DC_EMU64_HIST=1` build without `-DDC_PERF_PHASE`
   therefore armed the thunks, paid a clock read on ~2,867 commands a frame,
   and printed nothing — which reads as "the instrument is broken", not "you
   built it wrong".
2. **G1 and G2 both install into emu64's dispatch table, and were silent about
   it.** G2's trampolines overwrite G1's thunks and its loop calls `s_orig[]`
   directly, so a both-on build measures nothing while costing both overheads.
   Now an `#error` (`dc/include/dc_platform.h:417`).
3. **`EMU64_TBL_OBJ` spelled a literal object path**, so the
   `objcopy --globalize-symbol` that both G1 and G2 depend on would have stopped
   matching the moment `emu64.c` entered the stub or shrink tree — latent, and
   it would have presented as an undefined symbol at link with no hint why.
4. **G2's shadow had lost the original's 5-message rate limiter** on the
   unexpected-command diagnostic (the `vprintf` flood trap, at emulator speed),
   and its punt path dropped one command; it now rewinds `gfx_p`,
   `cmds_processed` and `dl_history_start` before handing back.

### R1 landed — acre ground textures are demand-loaded, and it is the first
### asset pool this port has ever had

96 `mFM_grd_*` symbols (**150,880 B**: 46 summer / 73,312 B, 41 winter /
70,144 B, 9 shared / 7,424 B) were pure `bcopy` **sources** into 32
always-resident staging buffers in `src/game/m_bg_tex.c` (33,792 B), filled by
`mFM_LoadBGCommonTex` (`src/game/m_field_make.c:1101-1133`). Zero other
consumers anywhere in `src/`, `include/` or `pc/` — so the sources never have to
be resident, only the 32 destinations.

**Measured, one clean rebuild against the previous shipping build:**

| | before | after | Δ |
|---|---:|---:|---:|
| `.bss` | 4,027,212 | 3,945,356 | **−81,856** |
| `MEMLEDGER margin` | 3,103,956 | 3,191,348 | **+87,392** |
| `ASSET MISSING` / `aram LOST` | 0 / 0 | 0 / 0 | — |
| `deepest_scene` | 18 | 18 | — |
| `fps_p50` | 24.1 | 24.2 | within noise |

**Screenshot-verified on a `DC_BGTEX_DEMAND=0` vs `=1` pair**, so it clears
measurement rule 2 and not merely the counters. (An earlier version of this
entry said verification was still in flight; it has since landed.)

**The seam is NOT `--wrap`.** `mFM_LoadBGCommonTex` and all six segment tables
are `static`, and `bcopy` has no symbol in the linked image at all (GCC folds it
to `memmove`). It is `tools/dcstub/make_src_shrink.py` rewriting the one `bcopy`
call in the shrink-tree twin of `m_field_make.c`; `src/` is untouched.

**The trick that makes it cheap, and it generalises.** The stub rewriter leaves
each unkept source as a **1-byte `.bss` symbol with a unique address**, and the
segment tables still reference those symbols by name — so `bg_tex_tbl[i]` is a
unique *key* naming which asset the slot wants. `dc/src/dc_bgtex.c` looks the
pointer up in a generated 96-row map and calls the existing
`dc_stub_keep_load_one()`; a miss falls back to `memmove` so a kept asset still
works. **No season logic and no `tex_idx` logic is duplicated in `dc/`.** Kill
switch `DC_BGTEX_DEMAND=0`.

It also defuses half a dated time bomb: `mFM_grd_w_*` was never in the keep
list, so a winter town would have drawn black ground. It is loadable on demand
now. ⚠️ The 84 `obj_w_*` structures are **still absent**, so the winter bomb is
reduced, not cleared.

Two hazards banked with it:

- **Vanilla over-reads its own source array by 1,024 B on every call.**
  `l_bg_tex_common_dummy[15]` wants 2,048 B but its source
  `mFM_grd_s_beach_tex` is 1,024 B (`pc_assets.c:22791`). Reading the **DEST**
  size off the disc reproduces the GameCube's own behaviour exactly; it logs at
  runtime rather than being silently papered over.
- **27 scattered `fs_seek`+`fs_read` now happen inside `mFM_FieldInit`**, and
  the same loop runs mid-scene on the island boat trip
  (`m_field_make.c:1745,1754`, from `ac_boat_demo_move.c_inc:92-102`). The
  payload is only 33,632 B (~67 ms at 500 KB/s), but `dc_main.c`'s own sweep
  model prices a scattered seek at 20-100 ms, so 27 seeks could be **0.5-2.7 s**
  [UNMEASURED]. The follow-up is a sorted batch helper mirroring
  `dc_keep_sweep()`.

### R2 and R3 landed — and building them exposed an accounting error that runs
### through the whole RAM plan, and INVERTS what a pool is for

`dc/src/dc_npctex.c` (R2, villager textures, 16 slots) and `dc/src/dc_npcmdl.c`
(R3, villager models, 16 slots) are in the tree, **and both default to OFF**.
The interesting results are not the pools; they are what building them revealed.

#### First: THE TOWN HAS NO VILLAGERS, and that is why the knobs are 0

Both pools load from `aNPC_dma_draw_data_proc` (`ac_npc_ctrl.c_inc:687`), which
runs only when a **villager** NPC actor is constructed. **Measured: this port
never constructs one.** `mNpc_SetNpcList` fills the town from the save's
`Animal_c animals[]` (`m_start_data_init.c:559`); the VMU path is unwired, so
`[PC] No save file found` and the list stays empty. Two 900 s runs that reach
scene 9 and walk around printed **not one** `[DC/NPCTEX]` or `[DC/NPCMDL]` line.

Two consequences, and the second is a schedule change:

- Every villager the port has ever drawn came from somewhere else — special
  NPCs, Tom Nook and the raccoons — which is why "the villagers look wrong" has
  never been reported: there are none.
- **Wiring the VMU save path (PLAN N2b) is now a PREREQUISITE for testing R2/R3,
  not a separate feature.** Turn the knobs to 1 and read the log *after* a save
  exists; until then neither pool can be exercised at all, and neither byte
  column below has been observed under load.

**Every pool claim in the kb was costed against the NON-STUB total, as if those
bytes were resident today.** `kb/STATE.md`'s "the RAM plan from here is a POOL"
section, `kb/levers.md` L1, `kb/ram-plan.md` P1/P2, `kb/plan-stages.md` S4 and
`kb/research-creative-ram.md` T1 all read "villager textures 1,154,944 B",
"~992 KB of NPC textures", "60 of 72 villager models are still stubbed" — and
then present a pool as **freeing** that. It does not. `DC_ASSET_STUB` already
dropped those bytes: an unkept asset is a **1-byte `.bss` symbol** and its load
is suppressed, so what a stubbed image pays for an asset class is what the
**keep list kept**, not what the class totals.

Measured in the shipping tree (`dc/build/AnimalCrossing.map`, keep list
`tools/dcstub/keeplist-town.txt`):

| class | non-stub total | RESIDENT before R2/R3 | why |
|---|---:|---:|---|
| villager textures | 1,154,944 B / 276 sets | **90,464 B** | 21 of the 236 villager sets were kept (37 of 276 files, the other 16 being special NPCs); 215 villager species had **no texture at all** |
| villager models | 438,640 B / 72 files | **5,536 B** | `cbr_1` alone. The other 11 kept `mdl/*.c` are special-NPC skeletons, not villagers, so 31 of the 32 villager species had **no vertices** |

**So the pools do not free RAM. They convert MISSING into PRESENT at a bounded
resident cost.** The honest ledgers:

```
R2 — villager textures, 16 x 4,832 B
  keep-list entries removed            -90,464 B .bss
  pool + zero block + bookkeeping      +78,872 B .bss   (measured off the map)
  generated map                        +~6,900 B .rodata
  net                                   ~-4,700 B
  content delivered:  21 species with textures  ->  236

R3 — villager models, 16 x 7,552 B
  resident villager-model .bss before     5,536 B   (cbr_1, and only cbr_1)
  pool + bookkeeping                   +120,956 B   (measured; the file's
                                                     header rounds it to
                                                     120,960)
  generated .rodata                      +~4,600 B
  keep list given back                    -5,536 B
  net                                  +115,424 B .bss, +~4.6 KB .rodata
  content delivered:  1 villager species with geometry  ->  32
```

**R3 SPENDS bytes.** The comparison that justifies the pool's *shape* is not
"today", it is "the alternative": keeping all 32 villager `mdl/*.c` costs
194,400 B, so the pool is **73,568 B cheaper than the content it delivers**.
That is the only arithmetic a pool proposal is entitled to make.

Also worth stating once, since it is the obvious follow-up lever: **16
max-sized slots waste 15,248 B** against the 16 largest species packed end to
end (105,584 B). A bump arena recovers that, at the price of a second failure
axis ("slots free but bytes exhausted"). `DC_NPCMDL_SLOTS` / `DC_NPCTEX_SLOTS`
are the knobs to cut first — 32 models serve 236 texture sets, so 16 villagers
collide often and the pools rarely fill.

**The rule, which applies to every remaining pool idea (acres, structures,
interiors):** *in a stubbed image, an asset class's resident cost is what the
KEEP LIST kept, not what the class totals. A pool is worth building when it
delivers content the keep list cannot afford — not when it "frees" bytes the
stub system already dropped.* Corrected in place in `kb/STATE.md`,
`kb/levers.md`, `kb/ram-plan.md`, `kb/plan-stages.md`,
`kb/research-creative-ram.md` and `kb/RESUME.md`.

#### The one place the OLD framing is still right: the acre pool

The 371 summer acre TUs really **are** kept — that is what `keeplist-town.txt`
bought on 2026-08-04 — so acre bytes really are resident and an acre pool really
would free RAM. Measured from the linked map rather than assumed:

| | `.bss` | TUs |
|---|---:|---:|
| **summer acres (`grd_s_*`), 100 % of it `*_v` vertex arrays** | **815,024** | 242 |
| the rest of `src/data/field/bg/acre/` (interiors, `grd_player_select`, `rom_train_in` 23,504, `police_indoor` 19,792) | 54,837 | 25 |
| whole acre tree resident today | 869,861 | 267 |

242, not 371: the keep list has 391 acre entries, **144 of them `*_evw_anime.c`
which carry no `.bss` at all**, and the 242 that do are exactly the 242
`grd_s_*` directories in the tree — i.e. every summer acre is kept, one `.bss`
section each. **Quote 815,024 B, not "~1.1 MB"** — the acre lever is real
but it is a fifth smaller than the round number suggests. (Cross-check: the
map's `.bss` sums to 4,059,052 B, which is exactly the ELF's `.bss` section
size, so this parse is not dropping sections.)

#### Three smaller corrections found in the same pass

- **`--wrap` does not work as spelled anywhere in this kb.** sh-elf uses a
  leading-underscore user label prefix — `dc/build/dedup/syms.txt` has
  `8c35f384 00000318 T _mNpc_SetNpcList` — so the linker symbol is
  `_mNpc_SetNpcList` and `--wrap=mNpc_SetNpcList` matches nothing. **Silently**:
  `--wrap` on an unknown symbol is not diagnosed. Now in `kb/traps.md`; the two
  kb sites that propose `--wrap` seams (`kb/STATE.md`'s villager section,
  `kb/research-ram-tiers.md` R2's `--wrap=malloc`) carry the caveat.
- **There are only 32 distinct villager MODELS — not 72, not 236.** The 382
  `npc_draw_data_tbl[]` rows resolve through `model_skeleton` to 72 distinct
  skeletons: **32 villager-only, 40 special-only, ZERO shared**. The 236
  villager texture sets share those 32 models. "60 of 72 villager models are
  stubbed" was counting the whole `mdl/` directory as if every file were a
  villager with its own pooled model.
- **`gsDPLoadTextureBlock_4b_Dolphin` expands to TWO `Gfx`**, not one — it is a
  comma pair at `include/libforest/gbi_extensions.h:1133` — and there are
  **1,619** of them in `src/data/npc/model/mdl/`. Any tool that counts `Gfx` by
  counting macros is wrong by that much. Related, and the reason R3 uses a
  generated table instead of a runtime display-list walk: a `gsSPNTriangles_5b`
  packet's top byte is vertex-index data and **reads as `G_VTX` (0x01) whenever
  `v11 == 0` and `v10` is 4..7**, which is ordinary geometry. Both in
  `kb/traps.md`.

### Corrections banked this session, each verified against the tree

- **The vertex memo is 128 slots (`dc_pvr.c:1955`), not 32**, and its hit rate
  is **48.2-48.9 %**, not 42.5 %. `kb/perf-dc.md` §3.5 had the pre-resize
  numbers and has been corrected in place.
- **`-mfsrra` and `-mfsca` are inert in this build.** They are in
  `$KOS_CFLAGS`, but GCC acts on them only with `-funsafe-math-optimizations`
  (and `-ffinite-math-only` for `fsrra`), and neither appears anywhere in this
  tree. `kb/perf-dc.md` §3.6 already had the corroborating evidence — `fsqrt`
  and three `fdiv` compiled literally into the shipped object. Now in
  `kb/closed.md` so nobody rediscovers the flags.
- **PVR Direct Rendering is UNBLOCKED.** `kb/perf-dc.md` §5 item 3 declined it
  because "KOS 2.3 exposes `pvr_dr_addr` with no `pvr_dr_init()`, so who sets
  QACR is unresolved". Read out of the pinned KOS tree: `pvr_list_begin()` calls
  `sq_lock((void*)PVR_TA_INPUT)`, which sets QACR0/QACR1, and
  `pvr_list_finish()` calls `sq_unlock()`; `pvr_dr_init`/`pvr_dr_finish` survive
  only as deprecated no-ops in `pvr_legacy.h`. DR is safe inside the bracket the
  code already has. Item reopened.
- **"KOS 2.3" is not a release.** `include/kos/version.h` says 2.3.0, but
  `git describe` on the pinned `KOS_SHA=1c6398f9` gives `v2.2.0-946-g1c6398f9`
  and tags stop at v2.2.2. Both `pvr_dr_addr` and `dcache_toggle_ocram()` are
  master-only and absent from every release tag. In `kb/traps.md`.
- **sh4zam is a PASS**, and it is in `kb/closed.md` with the reasons so it stops
  being re-proposed at every toolchain review. Full workings in the SH-4 math
  section below.
- **The N-triangle count in `G_TRIN_INDEPEND` is 7 bits, not 5.**
  `emu64.c:4814` is `n_faces = ((w0 >> 17) & 0x7F) + 1`, i.e. 1..128 faces per
  command. The "5-bit" figure in `kb/closed.md`'s F1 entry is the per-vertex
  **index** width (`POLY_5b`, `gbi_extensions.h:64,69-86`), not the face count.
  F1's verdict is unchanged — a `gsSPVertex` still never exceeds 32 vertices —
  but the stated reason was imprecise and has been fixed.
- **The SH-4's caches are 8 KB I / 16 KB D**, not 16/32. That halves the
  headroom `kb/research-fps-ideas.md` F5 (I-cache packing) and F6 (OCRAM) were
  sized against: OCRAM's price is halving a **16 KB** D-cache, not a 32 KB one.
  Corrected in place in both entries.

### SH-4 math — the sh4zam question, answered with map evidence

The user asked about **sh4zam** (`sh4zam.com`, MIT) after community advice that
GCC does not emit the SH-4's T&L instructions. The advice is true in general and
mostly already spent here.

**Already live in this port**, all through KOS `dc/fmath.h`: FTRV
(`dc_pvr.c:2666`, `:2721`; `dc_mtx.c:246-427`), FIPR (`dc_pvr.c:188-191`), FSRRA
(`dc_pvr.c:187`). **Two genuinely unexploited gaps were found, and both are now
addressed:**

- `dc_mtx.c:71-72` carried `DC_MTX_USE_FIPR 0` with a `TODO(M3)`. **It defaults
  to 1 now**; kill switch `-DDC_MTX_NO_FIPR`, which is also the A/B — the flag
  was resolved rather than measured, and that is worth saying out loud.
- **272 `sqrtf` sites in `src/` were linking newlib's SOFTWARE
  `__ieee754_sqrtf`** — a bit-twiddling loop over the mantissa, where the SH-4
  has a single instruction. `dc/src/dc_fmath.c` defines `sqrtf` via KOS
  `fsqrt()`; kill switch `-DDC_NO_FSQRT`. The mechanism is **archive
  extraction, not symbol overriding**: our objects precede `-lm` on the link
  line, so `libm_a-wf_sqrt.o` (84 B) and `libm_a-ef_sqrt.o` (240 B) are never
  pulled. The hottest caller is `guMtxNormalize` (`emu64_utility.c:265`), three
  `sqrtf` per call, 113 `dl_G_MTX` calls a frame = **339 software square roots
  per town frame**, all at `-O0`.

**Map evidence that shrinks the scope, and it contradicts `dc_mtx.c`'s own old
comment**, so it is recorded rather than left as a reading: `PSVECDotProduct`,
`PSVECMag`, `PSVECCrossProduct`, `C_MTXLookAt` and `PSMTXMultVecArray` are all
in the map's *Discarded input sections* — **not in the image**. Only
`PSVECNormalize` is live (`0x8c4cdde4`), with a single caller (`emu64.c:4696`)
gated on texture-gen batches. The "camera basis precision" worry has **no
mechanism in this build**.

**Verdicts banked in `kb/closed.md`** so the question stops being reopened:
`shz_sqrtf` is *not* FSQRT (`shz_scalar.inl.h:315-325` is
`shz_inv_sqrtf_fsrra(x) * x`, an FSRRA approximation); FSQRT needs no precision
screenshot because KOS sets `FPSCR = 0x00040000` at `startup.S:74-85`, making it
correctly-rounded IEEE and bit-identical to newlib for every normal input;
replacing KOS `mat_*`/`fipr`/`frsqrt` with sh4zam's is a **no-op** (same
instructions, ~15 µs of extra `jsr`/`rts` against a 78.3 ms frame); sh4zam is
header-only, depends on nothing from KOS and keeps no shadow copy of XMTRX, so
if it is ever wanted it should be **vendored into `dc/third_party/sh4zam/`**
rather than pulled from kos-ports, which would force a ~27 min SDK image
rebuild; and FSCA is not a lever, because there are only **four** live
`sinf`/`cosf` sites (`m_camera2.c:87-90`) and everything else uses
`sins()`/`coss()`, a table lookup at the same 16-bit angular resolution FSCA
takes as input.

**A RAM find fell out of the same sweep** and is now `kb/levers.md` L9:
`dc/src/dc_misc.c:421` builds its sine table with the **double** `sin()`,
dragging in **5,940 B** of libm (`k_rem_pio2.o` 2,720 + `e_rem_pio2.o` 1,408 +
`k_cos.o` 408 + `s_scalbn.o` 392 + `s_floor.o` 376 + `k_sin.o` 264 +
`s_sin.o` 192 + `s_frexp.o` 148 + `s_fabs.o` 32) — **44 % of the image's
13,400 B of libm, in our own code**, removable without touching `src/`.

### The black wedges are TEV config #007 losing BOTH alpha factors — diagnosed, NOT fixed

`dc/src/dc_pvr.c` carries a new `tev_const_alpha_last()` (kill switch
`-DDC_PVR_NO_TEVALPHA_LAST`, counter `tevalpha_last batches=`) that recognises a
final TEV stage of the shape `APREV * konst`. **It does not fix the black
wedges**, and the investigation found the brief's premise wrong:
`ef_shadow_out.c:34-35` records as **two** stages, not three —

```
stage0 alpha = (ZERO, TEXA, A1,    ZERO)
stage1 alpha = (ZERO, TEXA, APREV, ZERO)      emu64.c:1888-1897
```

config #007 in `kb/tev-map-table.md:120`. PRIM.a is on **stage 0**, in a
*mirrored* spelling that `tev_const_alpha()` does reach and then discards at its
`konst != GX_CA_A0` narrowing; and the last stage is `APREV * TEXEL1.a`, the
mirror of what `tex1_alpha_active()` tests. **So config #007 currently loses
both alpha factors and draws at `vtx.a * T0.a` — where `vtx.a` is the
`G_RM_FOG_SHADE_A` fog coefficient. A flat dark quad: exactly the reported
symptom.**

The two real levers are widening `tev_const_alpha()`'s A1 arm and adding the
mirrored shape to `tex1_alpha_active()`. Both are *widenings*, and widenings in
this family have regressed before (`dc_pvr.c:1080-1098`), so **both need a
screenshot pair**. ⚠️ **This diagnosis has not yet been transcribed into
`kb/tev-map-alpha.md` / `kb/tev-map-hard-cases.md`** — it lives only here and in
`kb/RESUME.md` §4 item 6. Move it when those files are next touched.

### The audio plan has been steering on a 2× arithmetic error

`dc/src/dc_audio.c:53-55, :129-131, :139-141, :916` all say one jaudio DAC frame
is "~35 ms of audio". **It is 17.49 ms.** `aictrl.c:292` passes
`AIInitDMA(..., DAC_SIZE * 2)` where `DAC_SIZE` is in **s16 units**, so
`DAC_SIZE * 2` is 2,240 **bytes** = 1,120 s16 = 560 stereo pairs; at
`JAC_DAC_RATE = 32028.5` (`internal/rate.c:4-7`) that is 17.49 ms.
`kb/audio-engine.md` has had the right number all along (57.19 Hz).

**Consequence: synthesis runs at 0.88× real time and needs ~113 % of the SH-4,
not 1.8× at 57 %.** `dc_audio.c:120-122`'s own note that the ring "starves
essentially always" corroborates it — that cannot happen above real time.

Two more audio numbers move with it:

- `kb/audio-cpu-cost.md`'s 80-180 cyc/voice-sample band is **optimistic**:
  `rspsim.c` compiles at `-O0` like all of `src/`. Back-solving from the
  measured 0.88× gives **~200+ cyc**, so every A0-A4 row needs scaling by
  ~1.7× (A1 34 % → ~57 %, A4 13 % → ~22 %).
- `kb/audio-engine.md`'s "no individual soundfont exceeds 2 MB" is true of
  2 MB and false of the **usable** sound RAM. KOS reserves 196,608 B
  (`AICA_RAM_START 0x030000`, `aica_cmd_iface.h:37-38`), leaving 1,900,544 B;
  bank 153 transcodes to 1,971,016 B, **over by 70,472 B**. It fits only if
  `snd_stream` is retired and channels are driven directly.

---

## 2026-08-04 — frame rate, the second texture unit, and why the ARM7 is asleep

Seven 600 s Flycast runs at `--timeout 600 -c config:LimitFPS=no`, each judged on
`tools/dcqa/run_report.py --vs` **and** a screenshot pair.

### The renderer got 17-26 % faster, and the win came from reading emu64's output

`kb/perf-dc.md` §3.5-3.6 carries the detail. The short version is that the two
biggest levers were both things nobody had looked at:

1. **emu64 hands this backend an expanded triangle SOUP, not a mesh.**
   `emu64.c:5100-5150` opens ONE `GXBegin(GX_TRIANGLES, 3*ntris)` for a whole run
   of triangle commands and then re-emits every attribute per triangle INDEX
   into its own 32-entry vertex cache. A vertex shared by six triangles was
   transformed, lit and texgen'd six times for the same 28 bytes. A per-batch
   memo keyed on the source vertex is **exact** — every other input to the
   per-vertex block is a batch constant — and it hits **48.2 %** of the time.
2. **`sqrtf` + `fdiv` had been compiled literally.** `$KOS_CFLAGS` carries
   `-mfsrra` but not `-funsafe-math-optimizations`, so GCC never folded the
   light normalise; the object has `fsqrt` followed by three `fdiv` at +0x178.
   `frsqrt()` is one FSRRA. `fipr` replaced 21 multiplies and 15 adds in the
   eye/normal transforms.

Measured, matched build line, `DC_PERF_PHASE`:

| | before | after 3.5+3.6 | after 128-slot memo |
|---|---:|---:|---:|
| `us/v` p50 | 4.71 | 3.92 | **3.81** |
| `us/v` p90 | 6.03 | 4.45 | **4.28** |
| `xform` p50 | 12.6 ms | 10.3 ms | **10.0 ms** |
| FPS p50 | 22.6 | 23.6 | 23.4 |

⚠️ **The memo table was 32 slots first, and 32 was wrong for a reason worth
keeping.** emu64's vertex cache is 32 entries, so 32 bounds the WORKING SET
correctly — and says nothing about COLLISIONS. Direct-mapped 32-into-32 measured
42.5 %; 128 slots measured 48.2 % for 3,456 more bytes. Sizing a hash table from
the working set instead of from the load factor cost about 6 points.

### The train window's light was a two-texture effect on a one-texture GPU

Human report: "the light on Rover when the window opens ... needs to be subtle,
not this rectangle". It is `rom_train_out_shineglass_modelT`, and the batch log
named it exactly: `64x8 ... st=2 tm=0,1 t1=1 argb=88FFFFFF bbox=-144,-165..337,494`
— flat white, ONE alpha for every vertex, over a screen-sized wedge.

The combiner is `alpha = TEXEL0.a * TEXEL1.a * PRIM_LOD_FRAC`, where TEXEL0 is a
64x8 horizontal ramp and TEXEL1 a 16x16 **vertical** one. Neither texture is a
light shaft; the soft 2-D falloff IS the product, one ramp per texture unit. The
PVR has one, so the port dropped TEXEL1 — and since all four shine vertices
carry the same `s` (168.0 texels, GX_CLAMP), what remained was a single
constant.

Fixed by sampling texmap1's alpha on the CPU, per vertex, through texgen 1, out
of an 8x8 map built at upload, and folding it into the vertex alpha;
MODULATEALPHA supplies the TEXEL0 factor in hardware. Fires on **5,210 of
1,170,059 batches (0.45 %)**. Human-confirmed fixed.

### The reply bubble is NOT too big — measured twice

Reported as oversized. It is not: the balloon's batch bbox is
`79.8,266.7..587.6,473.9` = 508x207 of 640x480, and counting near-white pixels
in the decoded PNG gives **245 px of 320 = exactly `width = 245.0`**
(`m_msg_main.c_inc:394`). The geometry is pixel-exact at its authored size.
Whatever the human is seeing is either the CHOICE panel (`con_sentaku2_v`, which
IS text-fitted by `scale_x/scale_y` at `m_choice_main.c_inc:137-171` and was not
captured in any probe frame) or the art itself. **Do not go looking for a scale
bug in the transform chain; there isn't one.**

### The AICA ARM7 ran, and then wedged. Three probes, three answers.

`[DC/AUDIO]` now reports `aicadrv`, `aicaclk` and `aicapos`, and together they
turn "audio is silent" into one decidable question:

- `aicadrv=EA00002D` — 0xEA is the ARM `B` opcode, so KOS's driver IS resident.
- No `ASSERTION` line anywhere — `snd_sh4_to_aica()` opens with
  `assert_msg(q_cmd->valid, ...)` (`snd_iface.c:84`), that assert is live in
  `libkallisti`, and the ONLY writer of that word is the ARM's own `arm_main()`.
  **So the ARM7 executed.**
- `aicaclk=0` — but `AICA_MEM_CLOCK` is incremented in exactly one place,
  `crt0.s`'s Timer-A FIQ handler, never by the main loop.

⇒ the ARM reached `timer_wait(10)` at the bottom of its first pass
(`arm/main.c:224`), which is `fin = timer + 10; while (timer <= fin);`, and with
the FIQ never delivered it spins there forever. `aicapos=0,0,0,0` agrees: it ran
`aica_get_pos()` once and never again.

Two other things were fixed on the way and are necessary but not sufficient:

- **`DC_ARAM_AUDIO_DROP=0`.** `audiorom.img` (8,300,384 B) was being thrown away
  by the ARAM pager, so the synth read zeros. Letting it through: `mapped`
  4,982,400 → **13,282,784**, `ext=3/32`, `LOST=0`, and `zero` **1 → 0** — the
  extent-table overflow the guard's comment feared did not happen.
- **The stream buffer was 16x too big for its own scratch.**
  `snd_stream_alloc(cb, SND_STREAM_BUFFER_MAX)` is 65,536 PER CHANNEL and
  `snd_stream_fill` asks for `size * chans` (`snd_stream.c:693-706`), i.e.
  131,072 B against a 4,096 B scratch. The callback clamped and returned 3 % of
  every request. `cb=2 pulled=8192` was literally the two prefills inside
  `snd_stream_start()`, both of which ran at boot before jaudio produced a
  sample. Now `snd_stream_init_ex(2, 8192)`, which also hands 56 KB back to
  sbrk — the starved half of the two-pool heap.

### Two renderer defects found by auditing g_gx for consumers

- **`dc_gx_backend_frame_begin()` ran TWICE per scene.** 52 `BATCHLOG BEGIN`
  against 27 `BATCHLOG END` in one log. `pc_gx_begin_frame()` has two callers —
  `dc_vi.c` and `JW_BeginFrame` (`jsyswrap.cpp:326`) — and there was no re-entry
  guard, so every frame ran `pvr_wait_ready()` + `pvr_scene_begin()` twice.
  Nothing visibly broke because the two calls were adjacent in every logged
  frame, but a second `scene_begin` after geometry has been submitted resets
  `s_pt_n` and discards every buffered punch-through record.
- **`t1=` in the batch log is saturated and cannot support the claim it was
  built for.** It reads 1 in 3,048 of 3,048 batches, including the 576 whose
  stage-1 texmap is `GX_TEXMAP_NULL`, because `tex_handle[1]` is never cleared —
  the same stale-handle family as the already-fixed "a draw that binds no
  texture still got one". The "52 % of batches request two texmaps" figure in
  that comment did not come from this field.


> ⚠️ **2026-08-02, read first:** the top entry below (framebuffer probe, "N2
> NOT solved", FBNONZERO 0) is **SUPERSEDED** — N2 is done: `--fb-writeback` is
> REQUIRED, golden `25789d43` works. See `kb/STATE.md` N2 and `kb/traps.md`.
> Also stale below: vertex census "unmeasured" (done — 93,312 B, `accc232`),
> "nothing is ever drawn / backend stub" (false since `fd4ee2c`), ARAM window
> thrash (pager landed, `29ffca5`), VMU write unimplemented (backend landed,
> `b4a177d`). Narrative for those four commits currently lives in `kb/STATE.md`.

---

## 2026-08-03 (later) — boot time, input edges, and a splash

All of this is aimed at the machine that now boots the game, where the disc is
~500 KB/s with real seeks and Flycast's `FastGDRomLoad` hides every bit of it.

### 15 seconds of boot were being spent printing

`DC_LOG` fired once per kept asset — 1,392 of them, **86,357 B of console**.
KOS busy-waits on the SCIF TX FIFO whether or not a cable is attached (the same
mechanism that cost 8x the frame rate in `kb/traps.md`), so at 57,600 baud that
is **15.0 s of dead boot with nothing on screen** — precisely the window in
which a human cannot tell "loading" from "hung". The whole log was 51.4 s.
Gated behind `-DDC_KEEP_TRACE`. Measured: console **296,122 → 214,511 B**,
`[DC/KEEP]` lines **1,393 → 1**.

### The keep list was read in source order, which is scattered

From the real trace: 874,736 useful bytes over offsets 3,012,288..14,399,968 of
a 15.6 MB file; **1,235 of 1,392 reads under 2048 B, 932 under 512 B**; 64
backward jumps; **255 MB of seek distance for 874 KB of payload**. Only 559
distinct sectors are wanted, in 112 runs. Modelling KOS's 16 x 2048 B sector
cache against that order gives **~578 single-sector GD-ROM commands**.

`DC_KEEP_SWEEP` records every request on one pass, sorts by
`(rom_src, rom_off)`, and replays through a 16 KB window that always refills on
a 2048-byte boundary. **Measured: 578 → 130 disc reads, 1,392 assets, 0
failed.** Predicted 129.

⚠️ **One pass, not two — a two-pass traversal silently drops assets.** Some
rewritten loaders keep the generator's load-once guard
(`src/furniture/ac_radio_test.c` has `static int radio_pal_loaded`), so a
second traversal skips them.

RAM is transient and freed before `OSInit` carves the arena: 40,960 B table +
16,384 B window, reported by the sweep rather than guessed.

### What was NOT built, and why

- **DMA is already in use** at every disc call site (`fs_iso9660.c:279,829`).
  The obvious "switch to DMA" win does not exist.
- **KOS already does read-ahead twice** — a 16 x 2048 B sector cache and a
  drive-level stream to end-of-file on any sector-aligned read. The
  `3 x 128 KB` ring in `dc_dvd.c`'s TODO would duplicate it for 393,216 B
  against a budget already 4.7 MB over. **Delete that TODO, do not implement
  it.** What KOS cannot fix is request ORDER.
- **Disc file order is already monotonic** for the boot access pattern, so
  reordering buys one seek against ~578.
- ⚠️ **`DC_CDI_PAD=1` does not push content to the outer edge** — measured
  identical LBAs padded vs unpadded, all ~684 MB appended after the filesystem.
  The comment claiming otherwise has been corrected.

### The button latch — the strongest remaining explanation for the K.K. stop

`PADRead` samples maple **once per logic tick**, and the game's gates are
EDGES derived from a single-sample XOR. `src/padmgr.c:309-314` says the GC
padmgr accumulated triggers between reads and that the PC port dropped it. At
Flycast's 22-30 FPS a tap always straddles a sample; **at hardware's ~11 FPS the
period is 91 ms and a tap can fall entirely between two**, producing no edge.
A 60 Hz vblank handler now ORs every button into a sticky mask that `PADRead`
consumes. Buttons only — the D-pad drives the C-stick, which is a level.

**And the free hardware test, needing no rebuild:** `chkButton(BUTTON_L)`
auto-advances dialogue under `TARGET_PC` (`m_msg_normal.c_inc:4`), and it is a
HELD test. **Holding the left trigger at the K.K. scene decides whether input
reaches the game at all.**

Also relevant to reading that scene: the disc is *not* a sufficient explanation
for a permanent stop — between `SCENE_MODE 0→3` and `3→4` the pager did only
**15 reads / 1.1 MB**, ~5 s even pessimistically. And `strum_timer = 440` is
**440 TICKS, not frames**: 7 s at 60 Hz but ~40 s at 11 FPS.

### The splash, and why the screen was black afterwards

Added "TechProGabe Presents..." between the Sega licence screen and the game —
white bfont text, zero RAM (bfont is in the BIOS), any button skips, and it
reports its own pixel count (`1206 px lit of 307200`) rather than asserting
success, per the framebuffer rule already in `kb/traps.md`.

The black gap after it was **`pvr_init()`**, which ran inside `dc_gx_init()`
*before* the disc check and the asset load, and which reprograms the display
controller at its own buffers. Split out as `dc_gx_backend_start()` and called
after the assets are in; the splash is no longer cleared. `DC_SPLASH_MS` is now
a MINIMUM on-screen time and the load happens under the text.

---

## ⭐ 2026-08-03 — IT BOOTS ON REAL HARDWARE

A padded CD-R burn of the `dev` build (`DC_ASSET_STUB=1`, checked-in opening
keep list, `DC_AUTOSTART` unset, no `DC_SCIF_FAST`) **boots on the user's retail
Dreamcast**. Until now every observation in this log came from Flycast.

**It stops at the K.K. Slider / player-select scene.** Three candidate causes,
and they are very different from each other:

1. **Input never arrives.** That burn had `DC_AUTOSTART` unset, so the game is
   waiting for a real controller through `PADRead`. If the real maple path
   differs from Flycast's, the scene simply never advances. `dc_pad.c`.
2. **The disc, not a hang.** The ARAM pager services a miss with a real read
   off a CD-R at ~500 KB/s *with seeks*, and every Flycast run has
   `FastGDRomLoad=yes`. The player-select scene is exactly where
   `forest_1st`/`forest_2nd` are paged in — `[DC/ARAM] LRU` on the last
   emulator run shows 57 disc reads / 2,742,848 B / 2 opens. On hardware that
   is minutes, not milliseconds. **`kb/traps.md` already warns that a short
   run is usually not a hang; this is the hardware version of the same trap.**
3. **A genuine hardware-only hang.** The one worth chasing. Distinguished from
   (2) by whether the scene is still animating.

**The disc built to separate them:** `DC_AUTOSTART=300` + `DC_CDI_PAD=1`,
handed over with the decision table above. It needs no coder's cable — if it
walks past K.K. unattended the cause is (1), if it stalls while still animating
it is (2), if the picture is frozen it is (3).

⚠️ **Do not put `DC_SCIF_FAST` in a hardware build.** A real coder's cable will
not sync at 1,562,500 baud, and the console — including any crash dump — is
lost.

⚠️ **`DC_CDI_PAD=1` for burns.** The 740 MB file is raw 2352-byte sectors:
314,663 sectors = 69.9 minutes, which fits a 74- or 80-min CD-R. The byte count
looks like it will not fit and that is an artefact of the format.

---

## 2026-08-03 — the reply box had no assets, and the counters nearly shipped a regression

Two visible bugs fixed, one measurement discipline changed, and one lever built
that makes every future rendering question cheaper.

### The regression gate came first, and it earned its keep immediately

A game smoke run always exits 1 (the game never returns), so every "did this
build get worse" call had been a hand-grep over a 2.6 MB log.
`tools/dcqa/run_report.py` reduces a console.log to the ~20 numbers that
decide it and `--vs` diffs two runs.

**It caught its own flaw within the session, twice, and both are written into
the tool:**

1. **Base64 matches everything.** The first version reported `oom x748` off a
   screenshot log, because `OOM` occurs in real `FBROW` pixel data. `FBROW` is
   now skipped before any matcher sees it.
2. **Total frames is confounded by scene mix.** The town runs at ~11 FPS and
   the train intro at ~30, so a run that reaches the town *sooner* accumulates
   *fewer* frames in the same 600 s. Two runs of ONE build measured **10,499 vs
   7,979**. `frames` now has a 30 % band and is documented as a hang detector;
   `deepest_scene` is the progression metric.

### `DC_SCIF_FAST` — screenshots stop being a different experiment

KOS's default SCIF is 57,600 baud and KOS busy-waits on the TX FIFO, so a
`DC_FB_IMAGE` capture costs ~35 s of wall clock. That is why `kb/RESUME.md` had
to warn that a screenshot run is not a progression run — 5069 frames without it
versus 1379 with it. The harness selftest has used 1,562,500 baud since M0;
`DC_SCIF_FAST=1` gives the game build the same ~27x.

Measured: a 90 s run reached 1889 frames with five **320x240** captures at
29.9 FPS p50. The old 160x120 build managed 12,449 frames over 900 s at 20.1.
4x the linear resolution AND no progression cost. Emulator only — a coder's
cable will not sync at 1.5 Mbps.

### The reply box: two assets that were never in the image

Human report: "the reply text boxes are messed up". Not a renderer bug. Every
log on disk back to 2026-08-02 carries exactly two asset failures and no
others:

```
[PC] ASSET MISSING: assets/con_waku_swaku3_tex.bin
[PC] ASSET MISSING: assets/con_sentaku2_v.bin
```

`con_waku_swaku3_tex` is the choice window's **only** texture — one filled I4
128x64 ellipse, there is no 9-slice and no border — and `con_sentaku2_v` is its
four vertices. Zeroed, that is a transparent texture on a degenerate quad which
the display matrix stretches into the pale haze visible across the lower half
of the train interior.

**Cause: the `.c_inc` trap has a second half.** `cinc_includes()` already knew a
TU's asset arrays and loader can live in an `#include`d `.c_inc` — that was the
dialogue-balloon fix. But all of that handling sits *below* an early
`continue` that asks the wrong file: `if "#ifdef TARGET_PC" not in text` tests
the **`.c`**, and skips the whole TU when it has none, which is exactly the
shape of a TU that keeps all its asset code in the `.c_inc`.
`src/game/m_choice.c` has zero `TARGET_PC` guards and is the **only** one of the
193 keep-list entries with that shape; `src/game/m_msg.c` survived the first fix
purely because it happens to carry one.

It hid because `dc_stub_keep.inc` declared *and called*
`_pc_load_src_game_m_choice_draw_c_inc()` either way, so the generated header
looked complete — and because the reply *text* was missing too until the
`tev_const_alpha` fix, so there was no text to notice a missing box around. That
mismatch is now a **hard error**.

Measured: `ASSET MISSING` 2 → 0, blank texture uploads 2/306 → 0/306,
`dc_stub_keep.inc` 1 → 2 `.c_inc` rows, frames 10,439 → 10,499, `ptdrop` 0,
`LOST` 0. The reply panel now renders — a yellow rounded box with "Please! /
No way!" and the cursor triangle.

### The alpha texture-env fix, and the A/B that was measuring a broken image

`PVR_TXRENV_MODULATEALPHA` had been programmed on every textured batch since
M1, making final alpha `vertex.a x texel.a`. Pushed through emu64's real
tables, **4,376 of 5,611 display-list sites (78 %)** have stage-0 alpha
`(ZERO, ZERO, ZERO, TEXA)` — texel alpha alone — and the vertex alpha byte there
is the `G_RM_FOG_SHADE_A` fog coefficient, which this port does not use because
it fogs in PVR hardware. `PVR_TXRENV_MODULATE` is the exact match. It fires on
**574,504 of 941,818 batches** at runtime.

**A/B #1 said REGRESS and the fix was shipped OFF.** The dialogue balloon got
correct (a grey block behind the body text disappears, the nameplate goes from
desaturated olive to the right yellow-green) and the train station canopy
became a flat teal-green slab where it had been textured beams. Counters passed
on both sides — **the counters would have shipped that regression.**

**A/B #2, after the reply-box assets were fixed, said the canopy is CORRECT.**
The teal slab was never this switch alone; it was this switch *plus* the two
zeroed assets. `tex_content_hash` (`dc_pvr_texture.c:317-322`) hashes only the
first and last 256 bytes above 512 B, so an all-zero texture aliases any other
texture with zero ends — the reply panel shared a VRAM image with something it
should not have, and honouring texel alpha then painted it.

**The lesson, which outlives the switch: a renderer A/B run against a build
that is missing assets does not measure the renderer.** `grep 'ASSET MISSING'`
must be empty before any visual comparison is believed. That is now a bad
marker in the gate, an entry in `kb/traps.md`, and a paragraph in the code.

### NPOT + `GX_REPEAT`: real bug, zero instances

Censused all 12,108 Dolphin-path texture binds in `src/data` (3,212 files, every
display list enumerable as source): **0** are NPOT + REPEAT/MIRROR. The artists
clamped every NPOT axis, and the N64 tile path forces `GX_CLAMP` for any
dimension outside `{4,8,…,512}`. All three candidate fixes are recorded as
rejected with reasons in `kb/closed.md`. One unverified sub-8 residual has a
detector rather than a patch.

### Built but OPT-IN, because its benefit is unproven

`-DDC_PVR_TEVFOLD` generalises `tev_const_color()` from one shape to the whole
affine stage — `d + (1-c)a + c*b` over `{ZERO, ONE, HALF, C0..C2, A0..A2,
KONST, RASC}` folded into the vertex colour as `K0 + K1*RASC`. It restores the
train window band's `ENV + PRIM_LOD_FRAC * PRIM` constant, which is the
time-of-day sun+ambient colour, and which the narrow shape rejects at its first
test. Measured: no regression, and the window scenery visibly darkens — which
is the ENV term arriving. It stays off because for the P3 shapes whose second
stage is `CPREV + TEXC*CPREV` (the band among them) GX computes `K*(1+T0)` and
this computes `K*T0`, i.e. about K too dark, and no screenshot yet says that
trade is a win. **The exact completion is the PVR's offset colour**: `oargb` is
added after the texture env, so `col = K` with `oargb = K` gives `K(1+T0)`
exactly. `dc_pvr.c` currently writes `oargb = 0` and leaves specular disabled.

---

## 2026-08-02 (session 2) — the town is reachable; three renderer bugs of one family

**Headline: the port reaches the TOWN.** `[SCENE_MODE] 0 → 3 → 4 → 18 → 9`;
mode 9 is `mFI_FIELD_FG` with `mEv_CheckFirstIntro()` TRUE
(`m_field_make.c:1292`), i.e. SCENE_FG, the outdoor field. Previously the run
stopped in the train intro.

### What unblocked it — input, not memory and not the renderer

`kb/boot-blockers.md` had this filed as a menu problem. It was arithmetic.
`dc_pad.c:64` alternated START and A **1:1**, and past the title screen START
advances almost nothing: dialogue pages take A or B only
(`m_msg_normal.c_inc:2`) and every choice menu defaults to index 0, which A
accepts. Half of every run's presses were wasted. The gate that actually held
the port was Rover's forced name-entry keyboard
(`ac_npc_guide_move.c_inc:662,665`), which needs *some* A presses to type a
character and then a START to accept — and rejects an all-blank name
(`m_editor_ovl.c:1165`).

Made A-dominant (`DC_AUTOSTART_START_EVERY`, default 4 → 3 A per START;
`=2` restores the old 1:1). Measured 188 A / 62 START, and the run walked
through the keyboard into the town.

⚠️ A save file is NOT needed for this. `pc_m_card.c:1282-1290` overrides the
card state machine and returns `mCD_TRANS_ERR_NONE` unconditionally; the
new-game path builds the town in RAM (`m_start_data_init.c:175`).

### The town ran at 1.1 FPS, and it was the CONSOLE

`gx=35.1ms` on a ~900 ms frame — the renderer was 4 % of it. The rest was one
line: emu64's
`非シェアードの三角形群にシェアードの頂点が混ざっているので破綻しました!`
(`emu64.c:2690`) printed **10,877 times** in one 600 s run over a 57600-baud
SCIF. It escaped the flood limiter because `emu64_print.cpp:18`'s `Printf0`
calls **`vprintf`**, and only `printf` was overridden. `g_pc_verbose` is forced
on by `DC_ASSET_STUB` (`dc_main.c:81`), so every stub build had it.

Overrode `vprintf` through the same table (and moved `dc_log_impl` /
`dc_loge_impl` / the `printf` override onto `vfprintf`, so our own diagnostics
are never rate-limited and no call site is charged twice).

**10,877 → 18 lines. Town 1.1 → 9.3 FPS.** Later, with the bigger keep list,
**12.1 FPS**, and frames-per-600 s went 8,159 → 10,199 → **16,889**.

### Keep list: 77 → 107 files, by census + union

Censused a town-reaching run (517 batches, `full=0`), resolved 325 addresses →
272 symbols. ⚠️ **The raw census would have DROPPED 11 files** the
player-select scene needs (the `flg_/kal_/mob_/mol_/mos_/xsq_` animal textures,
`kan_tizu`) because the town run never showed those animals. **Always union;
never replace.** That rule is now in the file header.

Texture uploads **119 (11 blank) → 269 (15 blank)**, i.e. blank fraction
9.2 % → 5.6 %. `image_span` 10,621,344 → 10,699,616 B, margin 3,588,448 B, fit
still OK.

Also hand-added `src/data/model/boy_model.c`: a census only names what DREW,
and the train-exit player never drew *because it was stubbed*.
`mPlib_get_player_mdl_p` (`m_player_lib.c:1319`) picks `cKF_bs_r_boy_1` for
gender MALE, the new-game default (`m_start_data_init.c:193`), while
`girl_model.c` was the one being kept.

### Three renderer bugs, all "GX state recorded and never consumed"

That makes **four** counting last session's wrap mode and TEV constants. A
sweep of every `g_gx` field for a consumer in `dc_pvr.c` is now the standing
technique.

1. **Untextured draws inherited a stale texture.** `dc_pvr.c` bound
   `tex_handle[0]` unconditionally, never reading `tev_stages[0].tex_map`.
   The whole JSystem 2D path sets `GX_TEXMAP_NULL` + `GXSetNumTexGens(0)`
   (`J2DGrafContext.cpp:29-31`) and `GXPosition3f32` zeroes the texcoord per
   vertex, so those panes sampled texel (0,0) of whatever emu64 last bound and
   `MODULATEALPHA` folded it into colour AND alpha — order-dependent per frame.
   Fixed; `-DDC_PVR_NO_TEXNULL` reverts. ⚠️ Suppress only on an explicit
   `GX_TEXMAP_NULL`; `tex_map == 0` is `GX_TEXMAP0`, the zero-init default.
2. **Alpha test never implemented.** `GXSetAlphaCompare` stores five fields;
   `dc_pvr.c` read none. 23 of the 101 TEV configs ask for a test. Cutouts were
   drawn with `GX_BM_NONE` → `src=ONE dst=ZERO`, so **fully transparent texels
   painted at full opacity and wrote depth**. Measured **316 of 2331 batches
   (13.6 %)** are cutouts. Human confirmation after the fix: "rover looks much
   better in the train".
3. **`GXSetColorUpdate` never consumed.** A depth-only pass painted solid
   geometry. Fixed as `src=ZERO dst=ONE` (destination untouched, depth still
   written) — the exact GX semantics. Measured impact: **1 batch**. Correct,
   but it was not a layering cause; recorded so nobody re-derives it.

### The alpha-test fix over-corrected, and the door is the evidence

First cut dropped `depth.write` for every alpha-tested batch. Wrong:
`AA_ZB_TEX_EDGE2` is the game's ordinary **opaque-with-holes** mode — the train
door frame and leaf (`obj_romtrain_door.c:44,71`) and the tunnel
(`rom_train_out.c:135`) all use it. With one submission-ordered list and
autosort off, a batch that writes no depth is painted over by everything
submitted later, and all XLU window scenery is submitted after the OPA
geometry — the passing trees drew straight through the closed door.

Narrowed to `if (g_gx.blend_mode == GX_BM_BLEND) cxt.depth.write = false;`
(`GX_BM_NONE` + test = opaque with holes → keeps depth).

**That is still not right, and the reason is fundamental:** `alpha_ref` (144 by
default, `emu64.c:718`) is read only to detect that a test exists, never as a
threshold. So with depth write ON, a door's transparent window holes still
write depth and occlude the scenery behind them; with it OFF the door does not
occlude at all. **Neither extreme is correct — this needs the real
punch-through list.** See `kb/RESUME.md` item 1.

### Measured, and worth not re-deriving

- **Multi-texture is a 9 % problem, not a 52 % one.** 1209 of 1231 two-stage
  batches request texmap1, but only **220** bind a genuinely different image
  (`t1=1`); the rest point both texmaps at the same tile — N64 LOD
  interpolation, which the PVR does in hardware and which is free to drop.
- **`cu=1,0` on 2330 of 2331 batches**: alpha update is off almost everywhere.
  Harmless (no destination alpha in the framebuffer), unlike colour update.
- **Fog is entirely unimplemented** and the game does use it
  (`emu64.c:3219`, `GX_FOG_PERSP_LIN`). Every train model carries `G_FOG`.
  Cosmetic — it cannot make geometry vanish.

### Audio: root cause found, pipe built, still silent

The jaudio pipeline had **never ticked once**. `pc_audio_process_frame`
(`audiothread.c:92`) is the only caller of `Jac_UpdateDAC`, its only caller was
the SDL thread `pc_audio_start_producer_thread` — which `dc_audio.c:201`
overrides with a no-op. `--gc-sections` had been dropping
`.text.pc_audio_process_frame` entirely. The single `AIInitDMA` the port ever
executed was `aictrl.c:70`'s init call with a zeroed buffer.

⚠️ **`[TRG_SE] NO FREE` is a SYMPTOM of that, not a DC bug** — and
`kb/RESUME.md`'s old claim that it was "a real bug in the DC audio layer" was
wrong. `Nap_ReadSubPort` returns −1 while the group is disabled
(`sub_sys.c:426`), and the free test is `!p5` (`game64.c_inc:1026`), which −1
never satisfies. It frees itself once the sequencer runs.

Wired a real `snd_stream` device plus a budgeted per-frame pump
(`dc_audio.c`, called from `dc_vi.c` before the frameskip early-out).
**The KOS API question that had blocked this since M0 is answered** — read out
of the SDK image, not guessed: the callback type is
`void *(*)(snd_stream_hnd_t, int, int *)`, and in `snd_stream.c:697-720` KOS
calls `get_data(hnd, needed_bytes, &got_bytes)` — **`smp_req` is BYTES despite
the name**, in and out. The existing callback already matched.

Result: device up, AICA pulling (`cb=2 pulled=8192`), but `[NEOS_OUT] peak=0`
— synthesis running on silence, because `dc_aram.c` discards every ARAM write
below `aram_audio_end` and throws `audiorom.img` away. `DC_ARAM_AUDIO_DROP=0`
now exists to let it through, **unproven and risky** (extent-table ordering).
Also `fill=` sat at 4480 with `cb=2` for a whole run: the consumer stalls after
two callbacks, cause unknown. First pump gated on `fill < RING/2` and
deadlocked (a stalled consumer left the ring half full, so synthesis never
ran); now gated on a headroom margin.

### An ARAM bug found while chasing missing text — real, but not the cause

The small-read fast path (`len <= ARAM_BLK`) `memset` a 32 KB block to zero,
called `dc_dvd_pager_read`, **ignored the return value**, bumped the SUCCESS
counter, and cached the block as authoritative — so a failed or short read
published zeros while `zero=0` looked clean. That size class is exactly the
64 B message/string TABLE reads (`m_msg_main.c_inc:289`). Fixed to demand the
full count (`dc_dvd_pager_read` returns BYTES, `dc_dvd.c:228`), free the block,
and log `SHORT READ`.

**Measured `SHORT READ = 0`**, so this was NOT the cause of the missing speaker
name and reply text. That remains open; `DC_ARAM_TBL_PROBE` was added to
adjudicate it (see `kb/RESUME.md`).

Also proven, so nobody re-checks: **forest_1st IS fully mapped.**
851,744 + 4,130,656 = 4,982,400 = the reported `mapped=`, to the byte. Two
extents cover both archives.

### Closed with evidence

- **Near-plane clipper works** — `clipped=1798` over 6.69M triangles. The old
  `clipped=0` came from short 2D-heavy runs: emu64 forces `GX_ORTHOGRAPHIC`
  for rect paths, ortho gives `w ≡ 1`, and `w ≡ 1` cannot trip `w <= EPS`.
- **Ortho `z ≡ 1.0` collapse is harmless** — across 314 logged batches, every
  depth-TESTED batch carries real perspective z and every collapsed-z batch has
  depth OFF. Zero overlap.
- **The "invisible" quads draw** — sane bboxes, `verts=6` per quad; the 50→3
  alternation is dialogue glyphs appearing and disappearing.

### Process notes

- **A short run is usually the human closing the emulator window.** One
  479-frame run was diagnosed as an audio deadlock; it was not.
- **`-c config:LimitFPS=no`** (harness passthrough) unlocks the frame limiter
  for play-testing — the user's tip, worth using on every long run.
- Running a kill-switch A/B changes OTHER things too: the
  `-DDC_PVR_NO_UVCLAMP` run built to test the trees regressed K.K. Slider,
  because that switch is what fixed his spotlight last session. Say so before
  handing over an A/B build.

---

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

