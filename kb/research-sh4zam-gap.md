# sh4zam — what we are actually missing

Opened 2026-08-08 on a user directive ("I still feel like we are not using
sh4zam to the fullest"). Sources: an audit of our own pipeline, plus four
research passes over the projects on
<https://sh4zam.com/index.html#autotoc_md18>. Pruned 2026-08-09.

**Every number that describes our port is quoted from a measurement; every
number that describes another project is a claim until reproduced.** Live
figures: `kb/RESUME.md`. Evidence: `kb/state-log.md`.

Read alongside `kb/closed.md` §sh4zam (the 2026-08-06 REOPEN — read it first)
and `dc/third_party/sh4zam/VENDORING.md` (why it is vendored, the `shz_sqrtf`
precision rule).

---

## 0. The headline, stated correctly

**We are not "failing to use SH-4 vector instructions". We already emit FTRV,
FIPR, FSRRA and FSQRT — through KOS, not through sh4zam.**

| instruction | where, in the default build | file:line |
|---|---|---|
| FTRV | one per vertex, `mat_trans_nodiv` | `dc/src/dc_pvr.c:3584` |
| FTRV | one `mat_load` per batch | `dc/src/dc_pvr.c:3479` |
| FTRV | `PSMTXConcat`, 3 rows | `dc/src/dc_mtx.c:349`, `:354`, `:359` |
| FIPR | 7 per **lit** vertex | `dc/src/dc_pvr.c:3597-3608` |
| FIPR | 4 per light in `chan_eval` | `dc/src/dc_pvr.c:892`, `:915`, `:918`, `:927` |
| FSRRA | normal renormalise, light normalise | `dc/src/dc_pvr.c:3610`, `:902` |
| FSQRT | `sqrtf` bound to the real instruction | `dc/src/dc_fmath.c:105-107` |

**And sh4zam itself contributes ZERO instructions to a shipping image.** Its only
include is `dc/src/dc_mtx.c:141`, behind `DC_MTX_FIPR_MULTVEC`, which **defaults
to 0** (`dc/src/dc_mtx.c:270-271`). That default is correct and measured
(`:227-228`, run `smoke-oc-dc-shz-20260806-133743`): `us/v 3.11 → 3.12`, town FPS
`20.6 → 20.4`. Inside noise.

So the gap is **not** "swap `mat_*` for `shz_*`" — `kb/closed.md` forbids
re-running exactly that. The gap is §1.

⚠️ **The line numbers in this table drift.** They were re-verified against the
tree on 2026-08-09 and were wrong by ~500 lines before that. Grep before quoting.

### ⚠️ 0a. The FIPR/FTRV reopening — STANDS, BUT DEMOTED TO THE BOTTOM

> 🔴 **DEMOTION FIRST, SO IT IS NOT READ WITHOUT IT: this section is aimed at
> ~0.8 ms of a ~30 ms frame — about 2.7 %.** The `[VTXSPLIT]` measurement (§3)
> put every floating-point stage in the vertex path at well under 1 ms combined,
> and a *perfect* FTRV rewrite takes only part of that. The argument below is
> technically correct and it is still the last thing to do, not the first.

`kb/RESUME.md` §6 item 11 records, as settled, that the per-lit-vertex block is
"**ALREADY OPTIMAL** and must stop being listed as an sh4zam candidate", because
the seven ops are already `fipr()`.

**That conclusion does not survive contact with the FIPR pipelining rule.** From
Falco Girgis's writeup, <https://dreamcast.wiki/SH4_FIPR_Optimizations>: the
compiler *"cannot pipeline FIPR for shit"* and reloads all 8 registers between
calls even when one vector is constant; **FIPR has 4-5 cycles of latency and the
next instruction stalls on its result, every call.** The rule for what to do
instead (<https://dreamcast.wiki/SH4_FTRV_Optimizations>): *"1. There are **3 or
more dot products (ax + by + cz + dw) being calculated back-to-back.** 2. **One of
the vectors is held constant** while the other vector argument is variable across
each."* → use FTRV. Issue/latency: `fipr` 1 / 4-5, `ftrv` 1 / 5-8.

**`dc/src/dc_pvr.c:3597-3607` matches both preconditions exactly, twice**: three
dots of the vertex against constant `mv` rows, then three of the normal against
constant `nm` rows. That is two FTRVs, not six stalling FIPRs. ✅ **Verified**:
`mv` and `nm` are bound once per batch (`:3450`, `:3452`), each group of three
shares one variable vector, and the fourth output slot is free in both.

⚠️ ~~`chan_eval`'s four per-light FIPRs are the same shape.~~ **FALSE — checked
2026-08-08.** None of the four converts to a batch-resident FTRV: `:892` and
`:3608` are **self-dots** (both operands vary); `:915`/`:918`'s "constant"
operand is `nrm`, which is **per-vertex**; `:927`'s is `lights[li].dir`, constant
across vertices but indexed **per light**, against a per-vertex-per-light
operand. Making any of them FTRV needs an **XMTRX reload per vertex** — eight
`fmov` pairs — which is the opposite of the win. The FIPR-*stall* argument
applies to them; the FTRV *conversion* does not. Active lights are 1-3 in
practice (the town runs exactly **2**), hard-capped at 8.

⚠️ The blocker: XMTRX already holds `comb` across our whole vertex loop (loaded
at `dc/src/dc_pvr.c:3479`), so an FTRV for lighting cannot coexist with the
position FTRV in the same pass. **The two findings compose into G-D — split the
loop into passes.** `kb/RESUME.md`'s "loses, because `comb` needs XMTRX"
conclusion is correct *for a single-pass loop* and stopped there.

**Being FIPR is not the same as being fast.** Ranking anything on "it is already
`fipr()`" is invalid until an A/B says otherwise — and any such A/B must clear
the ±2 % noise floor (measurement rule 11), which a 2.7 % target barely does.

---

## 1. The gaps, ranked

Ranked by (expected gain × confidence) ÷ cost, **re-cost against §3's
memory-bound reading**.

### G-A ✅ DONE 2026-08-08 — the build could not link most of sh4zam

**Result:** `dc/Makefile` now names `source/sh4/shz_complex_sh4.c` alongside
`source/*.c`, `SHZ_S` carries the two `.s` files with their own assemble rule,
and `$(SHZ_OBJS): TU_DEFS = -DNDEBUG` scopes the assert kill to sh4zam only. The
five previously-undefined out-of-line symbols (`_shz_fft_dc`, the three
`shz_mem_sh4.s` routines, the three `shz_xmtrx_load_apply_store_*_sh4`) are
present as `T` and a throwaway TU calling them links clean.
**G-A buys capability, not speed** — `sh-elf-nm` finds zero of them in the
shipping ELF, because nothing in `dc/` calls them and `--gc-sections` drops them.

⚠️ **Traps for anyone extending it:** `source/sw/*.c` must **NOT** be compiled on
SH-4 (`shz_xmtrx_sw.c` defines a conflicting `xmtrx_state_`) — a naive *recursive*
glob is a build break, not a slowdown; our glob excludes it by depth, now on
purpose. And `-DNDEBUG` is required, not optional: sh4zam's memory and inline-asm
routines carry alignment and FP-mode `assert()`s that would sit inside the vertex
loop.

### G-B 🔴 THE ONE ENTRY THIS FILE MUST NOT BLUR — IT IS **TWO** CHANGES

> 🔴 **"G-B" names two different things, and conflating them is the single most
> common error in this kb. Only the small one shipped.**

#### G-B(1) ✅ The vertex-index SIDE CHANNEL — SHIPPED 2026-08-09, ON by default

`dc/src/dc_emu64_cull.cpp`'s AABB index walk already visits indices **in exactly
the order `set_position3()` will replay them**, so it records that sequence and
hands it to `dc/src/dc_gx.c` (`dc_gx_vtxid_arm`, `:1115`; armed at
`dc_emu64_cull.cpp:574` and `:670`). `GXPosition3f32` (`dc_gx.c:1131`) consumes it
with a cursor and stamps `(epoch << 8) | index` into **`DCGXVertex` bytes 30-31,
which were dead padding** — `sizeof` stays 32 and session 12's `aligned(32)` is
untouched. The memo (`s_vmemo_vid`, `dc/src/dc_pvr.c:2650`) then keys on the
stamp: **no hash, no 30-byte content compare, and above all no random read into
`verts[]`** — the operand-cache miss that made the memo the most expensive stage
charged on every vertex.

**Result: `us/v` 2.68 → 2.51 (−6.3 %), memo hit rate 50.9 → 53.7 %.** Kill:
**`-DDC_GX_NO_VTXID`**. Gate **`-DDC_GX_VTXID_VERIFY`** content-checks every id
hit: **`vidchk=15,538,941 vidbad=0 over=0`**.

- **The epoch is load-bearing.** `GXBegin` **merges** batches: one submit can hold
  two TRIN commands and emu64 **reloads `vertices[]` between them**, so a bare
  index would hand the second TRIN the first one's transforms. The epoch makes a
  stale id miss.
- **Reach is 100 % of what is legal today** — armed only where the batch survives
  the frustum test AND none of `dc_emu64_cull.cpp`'s three punts fired.
  ⭐ **The decal-Z punt is 58 % of all punts, and lifting it FOR ARMING ONLY is
  +51 % reach** (`-DDC_GX_VTXID_DECAL`, default OFF, gate passed, perf not
  measured — `kb/RESUME.md` §13). Arming needs only *"the same index means a
  byte-identical staged vertex within this submit"*, which decal-Z meets and
  `G_TEXTURE_GEN` / mixed `MTX_NONSHARED` do not.
- ⚠️ **The win did not land where it was aimed.** `memo` itself barely moved; the
  gain is in `shade`/`lit`/`tex`/`post`, because those are charged on memo
  **MISSES** (measurement rule 10) and the miss count fell.
- ⚠️ **Flycast understates this by construction** — what it deletes is a read that
  misses the operand cache, and Flycast models no cache. **2.51 is a FLOOR.**

#### G-B(2) 🔴 The INDEXED-SUBMIT REWRITE — **13.31 ms, UNSTARTED, multi-session**

**Transform each unique vertex ONCE, before index expansion, and index into the
result — thereby deleting emu64's index expansion loop AND our own `GX*`
attribute setters.** That block is **13.31 ms, 43.8 % of the draw**, and it is
**still the largest single block in the project.**

🔴 **A cheap memo is not this.** G-B(1) removes no setter and expands no fewer
indices. Do not mark this section done off the side channel; several kb files
have conflated the two.

Xash3D DC hit exactly this and quantified the fix
(<https://github.com/maximqaxd/xash3d-fwgs_dc/blob/04306ef/ref/pvr/pvr_studio.c#L2653>):
*"barney has ~430 unique verts but ~2156 strip corners — each vertex was
previously FTRV'd **~5× (once per corner reference)**. Now it's done exactly once,
sequentially, cache-friendly, and the strip loop just indexes `s_clip[vi]`."* And
on the structure: *"Zero intermediate arrays. One loop: `s_clip[vi]` → persp
divide → `pvr_dr_commit`. Eliminates ~60KB of `s_tv`/`s_strip_uv`/`s_strip_ac`
traffic per frame."*

**We have direct evidence the redundancy is real here**: the memo exists solely to
short-circuit repeated source vertices and its hit rate is >50 %. Doing it
structurally beats caching it — and deletes the memo, the epoch and the stamp
along with it.

⚠️ **Three mutation hazards, all in `set_position`, which is NOT a pure read of
`vertices[]`:** `emu64.c:2694-2708` flips `MTX_NONSHARED`; `:2712-2717` multiplies
a normal with no idempotence latch; `:2724-2781` submits a round-tripped
position. ✅ **The distinct-vertex walk it needs ALREADY EXISTS** —
`dc/src/dc_emu64_cull.cpp`'s index walk and its `mark[4]` 128-bit bitset
(`:363`), which is also the free instrument for "how many distinct references per
batch" (one `__builtin_popcount` against `n_faces * 3`; nothing counts it today).

**Design before coding.** This is the multi-session item.

### G-C ✅ DONE 2026-08-08 — `emit` −34 %, ON by default

**Result:** `emit_projected()` (`dc/src/dc_pvr.c:2495`) writes the eight TA words
straight into `pvr_dr_target()` (`:2515`) and `pref`s with `pvr_dr_commit()`,
instead of building a stack `pvr_vertex_t` for `pvr_prim`/`sq_fast_cpy` to read
back. **`[VTXSPLIT] emit` 2.20 → 1.45 ms; `us/v` 3.24 → 2.89** on that change
alone. Counter `[DC/PVR] dr verts=` reads **81.7 %**, exactly the
non-punch-through share — the PT producer keeps the stack path because a PT record
must be **held** in `s_pt_buf` (`:497`, written `:551`, replayed `:3180`) until
list 4 can legally be opened. Kill: **`-DDC_PVR_NO_DR`**.

⚠️ It landed as **pattern A (fused DR)**, not the pattern C (stage-then-blast)
this section originally recommended — same effect, without changing what
`g_gx.vertex_buffer[]` is. ⚠️ **`dc/src/` now depends on QACR, hence on the MMU
staying off** (`kb/research-mmu-paging.md`). The four KOS 2.3 facts it rests on
were read out of the SDK image, not assumed: `pvr_dr_addr` is a global
statically initialised to `MEM_AREA_SQ_BASE`, `pvr_dr_init()` is a deprecated
no-op, `pvr_list_begin()` already `sq_lock`s the TA, and **`sq_flush()` clobbers
`"memory"`**, which is what orders the eight plain stores before the `pref`.

The three submission patterns in the surveyed set, for reference: **A. fused DR**
(one touch per vertex — `bruces_balls.c`, DMS-Engine, Xash studio path, and now
us); **B. transform-in-place in the DMA list** (one touch — sh4zam
`example/pvr_dma`, Doom 64 DC `r_phase3.c`; submission is a pointer bump, and
`sizeof(pvr_vertex_t) == sizeof(pvr_poly_hdr_t) == 32` so KOS's multiple-of-32
requirement falls out free); **C. stage then blast** (two touches — GLdc,
Simulant, Xash world path). Doom 64 DC proves **mixing DMA and DR in one frame is
legal**.

### G-D 🟡 Split T&L into passes over 32-vertex blocks

The enabler for §0a's FTRV lighting *and* a standalone win. From
<https://dreamcast.wiki/Fast_SH4_Vertex_Processing>: the naive per-vertex loop
(fetch → N·L → specular → clamp/pack → transform → 1/w → store) *"will get quite a
big loop and you will **run out of FPU registers for sure**… subject to **bad
pipelining** since most of the operations depend on the results of previous
operations."* Split it into independent passes — diffuse, specular, clamp+pack,
transform+divide — and run each over a **block**, not the whole buffer: *"run the
small loops not over the whole buffer at once but over smaller blocks… the
currently processed vertices will be kept in the cache for each of the many loop
iterations. As to my experience **a block size of 32 vertices gives the best
performance.**"*

Composability is a stated side benefit ("if you don't need specular lighting then
just don't call the specular lighting loop"), which maps onto our `need_light`
guard (`dc/src/dc_pvr.c:3487`, tested at `:3591`). Each small loop also has spare
FP registers, so it can be software-pipelined (G-E).

Same page, cache rules that apply to any such pass:
- **16 KB direct-mapped** operand cache; address *n* and *n*+16384 collide.
- `PREF` the next element at the **top** of the loop.
- **`MOVCA.L` is not optional.** Without it a store to a write-only output buffer
  causes a read-for-ownership fetch of a line you are about to fully overwrite.
  "While the OCBP instruction can be left out without any performance drops this
  does not apply for MOVCA.L." → `shz_dcache_alloc_line()`.
- **Prefer FSRRA to FSQRT** — "rewrite your math formulas (e.g. range falloff
  lighting)" to suit it.
- `FMAC` is "as fast as normal float addition… so basically you pay one and get
  another operation for free."

⚠️ **Ranked low on absolute milliseconds** (§3), and ⚠️ **it costs cache to buy
cache**: a 32-vertex block must materialise `eye[32][3]` and `nrm[32][3]` — 768 B
currently never stored at all — plus `ClipVtx blk[32]`, `slot[32]` and a
memo-hit bitmask, ~1796 B against today's ~112 B, into a 16 KB direct-mapped
operand cache. **In a memory-bound frame that is the wrong direction and it must
be measured, not argued.** Also: the loop indexes **primitives, not vertices**, so
32 vertices is 8 quads but 10⅔ triangles.

### G-E 🟡 Software-pipeline the submit loop

DMS-Engine's `render_fast` issues vertex *i+1*'s FTRV **before** storing vertex
*i*, hiding the 5-8 cycle latency behind the SQ stores
(<https://github.com/ianmicheal/DMS-Engine/blob/main/Sonic_example/dms/dc_model.c>):

```c
for (int i = 1; i < count; i++) {
    SHZ_PREFETCH(&src[i + 4]);
    shz_vec4_t next_t = shz_xmtrx_transform_vec4(shz_vec4_init(nx, ny, nz, 1.0f));
    pvr_vertex_t* pv = pvr_dr_target(*dr);   /* submit PREVIOUS while next_t settles */
    pv->flags = cur_flags; pv->x = cur_sx; pv->y = cur_sy; pv->z = cur_invw;
    pv->u = cur_u; pv->v = cur_v; pv->argb = cur_argb;
    pvr_dr_commit(pv);
    next_t = shz_vec4_swizzle(next_t, 1, 2, 3, 0);
    cur_invw = shz_invf_fsrra(next_t.w);
}
```

4-vertex prefetch distance; source vertex `__attribute__((aligned(32)))` and
exactly 32 bytes — ✅ **both of which our `DCGXVertex` already satisfies since
session 12.** Falco's own example uses `SHZ_MEMORY_BARRIER_SOFT()` between
`pvr_dr_target()` and the first read of the FTRV result: *"Prevent GCC from
reordering our memory accesses in a way which causes us to access the transformed
position before it's ready, which would cause a pipeline stall."*

⭐ **This is the FP-side idea that survives the memory-bound reading best**,
because its mechanism is overlapping latency with *stores*, not doing less
arithmetic — and G-C already put us on the DR path it assumes.

### G-F 🟢 DEMOTED — four frustum planes in one FTRV

⚠️ **Re-costed and demoted before anyone wrote code.** The late-path cull is
**0.70 ms of a 30.39 ms draw — 2.3 %** (§3), not the 2.0 ms this was ranked on,
and G3's own calls are **not timed at all**. Cheap experiment, low value.

Three facts the DMS shape does not survive contact with:

- **There are no stored frustum planes.** The six "planes" are literally the
  comparison lines in `dc_gx_aabb_is_offscreen()` (`dc/src/dc_gx.c:571-601`, e.g.
  `:591`); each corner is pushed through MV *then* P. Nothing folds P·MV at cull
  time (`dc_pvr.c:3455-3479` does, but only for *surviving* batches, downstream).
  An FTRV form must pay the fold itself **per call** unless it caches planes per
  matrix change.
- **A cheaper shape exists that needs no FTRV at all.** Gribb-Hartmann planes
  extracted from a folded MVP (cached per matrix change) plus the positive-vertex
  test is **6 dots total**, against today's fixed 8 corners × 7 dots with no
  early-out. That is the experiment to run first; the FTRV is second-order.
- ⚠️ `dc_gx.c` records that the function is scalar **on purpose** so it needs no
  `dc_mtx_xmtrx_invalidate()`. An XMTRX form makes that invalidate mandatory, and
  it must ship behind **`-DDC_GX_NO_FTRV_CULL`** (the reserved kill-switch name
  for this change; nothing defines it yet).
- ⚠️ **Ordering:** `dirty_check()` and `setup_1tri_2tri_1quad()` must run
  **before** the cull call (`dc/src/dc_emu64_cull.cpp:594-595`, test at `:653`),
  because `dc_gx_aabb_is_offscreen` reads `g_gx.projection_mtx` and
  `g_gx.current_mtx` live.

**The shape, if it is ever run:** DMS-Engine packs the four side planes as a
matrix (`shz_mat4x4_t side_planes` + separate `near_plane`/`far_plane` vec4s, the
struct `aligned(32)`), FIPRs the near and far planes with early-outs, then
`shz_xmtrx_load_4x4(&side_planes)` + `shz_xmtrx_transform_vec4(pos)` gets all four
side distances in **one FTRV**. DCA3 (GTA3 DC) does the same — 6 plane dots become
2 FIPRs + one FTRV.

### G-G 🟢 WXYZ permutation — W at cycle 4 instead of 7

FTRV writes its result one component per cycle: **X at 4, Y at 5, Z at 6, W at
7** (<https://sh4zam.com/tips.html> §"Faster Perspective Division", the only cycle
table sh4zam publishes). The perspective divide needs W *first*.

Fix: fold a WXYZ permutation into the projection so FTRV yields W in the X slot,
then swizzle back — `shz_xmtrx_init_permutation_wxyz()` before the screen and
perspective applies, then `shz_vec4_swizzle(trans_pos, 1, 2, 3, 0)` to deswizzle;
or `shz_xmtrx_load_wxyz_4x4()` at load time. ⚠️ `tips.html` names it
`shz_xmtrx_load_4x4_wxyz()` — **that function does not exist**; the header
declares `shz_xmtrx_load_wxyz_4x4()` (`shz_xmtrx.h:127`).

**Nearly free for us**: our `comb` fold (`dc/src/dc_pvr.c:3455-3470`) is already a
hand-written index swap (`comb[j][i] = s`). Making it WXYZ is a second index
change in a loop that runs once per batch. ⚠️ Its target is inside `emit`, which
G-C already took 34 % out of — measure, do not assume.

### G-H 🟢 SQ warm-up for batch-constant fields

KOS alternates between exactly two store queues. Falco's example writes
per-vertex-*constant* fields into **both** SQs once, outside the loop, and never
per vertex:

```c
((pvr_vertex_t*)pvr_dr_target(*dr_state))->argb = base_color;   /* ×2, hoisted */
((pvr_vertex_t*)pvr_dr_target(*dr_state))->flags = PVR_CMD_VERTEX;
```

Applies to our `oargb` (written 0 every vertex), `flags`, and often `argb`.
⭐ **Newly relevant since G-C put us on DR**, and its mechanism is *fewer stores*,
which is the right shape for a memory-bound frame. ⚠️ It is also the exact shape
`kb/closed.md` warns about — a hoist that turns a compile-time constant into a
runtime load has measured **negative** twice on this tree. Hoist stores, not
predicates.

### G-I 🟢 `shz_fft` for audio — mechanism known, budget not

`shz_fft` is radix-2 **decimation-in-frequency** Cooley-Tukey, in place, and the
acceleration is **FTRV, not FIPR** — a 2-point butterfly *is* a real 4×4 map on
`(re₀,im₀,re₁,im₁)`, held in XMTRX. Twiddles come from FSCA computed **in the back
bank in place** (`shz_xmtrx_update_fft_butterfly`), the one division is
`shz_divf_fsrra`, and all data movement is paired `fmov.d` (a `shz_complex_t` is
exactly 8 bytes). Bit-reversal uses the back bank as swap scratch.

Constraints: size must be a power of two, buffer 8-byte aligned, **it clobbers
XMTRX**, and **there is no inverse FFT** (`shz_complex.h` `\todo`).

FFFFTT (<https://github.com/meisei4/fffftt>) uses it directly — 1024-point,
512 bins, 22 kHz mono, Blackman window via `shz_cosf`, work buffer `alignas(32)`.

⚠️ **No absolute cost per transform is published anywhere**, and **nobody has
established that jaudio's hot path contains an FFT-shaped transform at all.** Read
`kb/audio-cpu-cost.md` before writing code. This stays 🟢 until then.

### G-J 🟢 Scalar remainders

| site | file:line | shape |
|---|---|---|
| `chan_eval` spot polynomials + accumulate | `dc/src/dc_pvr.c:927-945` | leftover after the FIPRs |
| texgen | `dc/src/dc_pvr.c:2876-2900` | 2 × dot-3 |
| `pack_argb` | `dc/src/dc_pvr.c:569` | 4 × (mul, add, 2 clamps, `ftrc`) per vertex |
| `lerp_vtx` | `dc/src/dc_pvr.c:2725` | clip path only |
| `GXNormal3f32` | `dc/src/dc_gx.c:1234` | 3 mul + 6 compares + 3 float→short |

⚠️ Note on the last row: emu64 calls `GXNormal3f32` thousands of times a frame to
fill a field the backend **discards when `need_light` is false**. Skipping it for
unlit batches is legal (GX state cannot change mid-batch) — that is a
*work-removal* idea and outranks every arithmetic idea in this table.

---

## 2. What is already dead — do not re-derive

- **Swapping `mat_load` → `shz_xmtrx_load`.** Same instructions, different
  header. `kb/closed.md` §sh4zam.
- **`shz_sqrtf`.** It is `x · FSRRA(x)`, not FSQRT. FSRRA's bound is **±2⁻²¹**
  (Renesas, via <https://shared-ptr.com/sh_insns.html>), and multiplying by `x`
  compounds it. Worse: sh4zam guards only `x == 0.0f`, so **`shz_sqrtf(negative)`
  is garbage, not NaN**. `dc/src/dc_fmath.c:105-107` binds `sqrtf` to real FSQRT —
  keep it.
- **FSCA.** Only four live `sinf`/`cosf` sites (`src/game/m_camera2.c:87-90`);
  everything else is `sins()`/`coss()`, a table lookup at the same 16-bit
  resolution FSCA takes as input. `kb/closed.md`.
- **FIPR for `PSMTXMultVec`.** Measured, inside noise
  (`dc/src/dc_mtx.c:227-228`, default 0 at `:270-271`).
- **Composing `P·MV` in XMTRX** — costed and dropped, ~67 µs of a 45 ms frame.
  ⚠️ But see G-G: making the *existing* fold WXYZ is a different, much cheaper
  change.
- ~~**The per-lit-vertex block is already optimal.**~~ **REOPENED — see §0a — but
  DEMOTED to the bottom on absolute milliseconds.**
- **`-ffast-math` on our tree.** Forbidden, `kb/closed.md`. ✅ **And it costs us
  nothing**: <https://sh4zam.com/tips.html> §"Selective Fast Math" — *"SH4ZAM is
  smart enough to fall-back to inline ASM for FSCA and FSRRA rather than relying
  on the compiler to emit them."* Confirmed in source;
  `shz_inv_sqrtf_fsrra_sh4` is unconditionally `asm("fsrra %0")`.
- ⚠️ **`-mfsrra`/`-mfsca` are INERT in this build** — `$KOS_CFLAGS` carries them
  but not `-funsafe-math-optimizations`. `kb/closed.md`.

---

## 3. ✅ ANSWERED — the frame is MEMORY-BOUND, not FPU-BOUND

**This section used to be the blocking measurement. It ran. The conclusion is the
content; the intermediate tables have been deleted.**

⭐⭐⭐ **All the floating-point stages of the vertex path together are ~0.8 ms of
a ~30 ms frame.** What costs is memory traffic: the memo's read, the per-corner
store-queue copy, and the shade pass. Corroborated from the other end by
`tools/dcopt/icache_map.py` (the 12-symbol inner loop is 1.4x an 8 KB
direct-mapped icache; the whole frame hot set is 11.9x). ⚠️ **Flycast models
neither cache, so every figure here is an understatement.**

**Current `[VTXSPLIT]` split** (`-DDC_PVR_VTXSPLIT=16`, town, per PRESENTED
frame, session 13 — the post-G-C, post-side-channel state):

| stage | ms | what it is |
|---|---:|---|
| **shade** | **1.82** | `shade_vertex()`, the per-light loop — **the largest stage, and it was never on any list** |
| **emit** | **1.40** | near clip + perspective divide + the store-queue write (**G-C took −34 % here**) |
| **memo** | **1.20** | the vertex memo, **paid on every vertex** (the side channel made it a stamp compare, not a hash + random read) |
| tex | 0.57 | `apply_texgen` + uv scale |
| post | 0.55 | PT alpha, TEV const/fold, memo store |
| **lit** | **0.54** | ⭐ **the six FIPRs §0a is about** |
| **xf** | **0.22** | ⭐ **the position FTRV** |
| sum / `xform` | **6.31 / 6.9** | the residual is per-primitive loop overhead |

`us/v` **2.51**, `draw` ~29 ms, memo hit rate **53.7 %**.

⚠️ **RULE 10 — THE BUCKETS DO NOT SHARE A DENOMINATOR.** `memo` is charged on
every vertex; `emit` is per primitive; **`xf`, `lit`, `tex`, `shade` and `post`
sit downstream of the memo-hit `continue` and are charged on memo MISSES only.**
Dividing them by `v` understates each by the hit rate. This is what retired the
old "44 cycles ≈ six FIPRs, so the block was never slow" argument — **the §0a/G-D
demotion still stands, but on absolute milliseconds, not on that argument.**

⚠️ **RULE 11 — the `us/v` noise floor is ~±2 %.** `us/v` normalises for vertex
COUNT but not for WHICH vertices, and the town reseeds every boot. A change under
~4 % cannot be resolved by one A/B pair.

**The ranking that follows from this table is §5.** In one line: G-B(2) is the
only multi-millisecond item left, `shade_vertex` is the largest stage inside
`xform` and was never on any list, ideas whose mechanism is *locality* or *fewer
stores* outrank ideas whose mechanism is *less arithmetic*, and everything in §0a
/ G-D / G-F / G-G is sub-1 ms.

⚠️ **G3 ADDED cull calls, it did not replace them.** Its trampolines skip emu64's
handler only on the `return` path, so punt *and visible* batches both fall
through to `GXEnd` → the late cull. The scalar cull therefore runs from G3 **and**
on the late path, **and only the second set is inside the `cull=` bracket** —
`dc_emu64_cull.cpp` has zero `dc_time_us` reads. The `vcull` collapse was a drop
in the late cull's *yield*, not its call count, and "visible" is its most
expensive answer because it has no early-out. **Any G-F costing needs a new
bracket first.**

⚠️ **Caveat on the runs behind this table.** Without `-DDC_AUTOWALK` the camera
never moves and every window is byte-identical — great signal-to-noise, **one
static town view, not a walk.**

### Corollaries that survive regardless

1. `[EMU64H]` is per **logic tick** — double it. `[PHASE]`, `[GXSPLIT]` and
   `[VTXSPLIT]` are per presented frame — do not.
2. **`us/v` is the instrument, not FPS and not `draw`** (`kb/perf-dc.md` §1), and
   **`dc_pvr.c` line references drift by hundreds of lines between sessions** —
   grep, do not trust a quoted line.
3. Every FPS number this project has produced came from **Flycast, which models
   no instruction cache, no operand cache and no disc seek timing**, against a
   ~2.88 MB `.text` on an 8 KB direct-mapped icache. **Every Flycast figure is a
   floor.** The only instrument that can answer why the console is slower is
   **PMCR on a burn** — Xash3D DC's `perf_cntr_start(PRFC1, …)` split (PRFC0 is
   KOS's) is what `dc/src/dc_pmcr.c` implements (`DC_PMCR=1`, `DC_PMCR_HUD=1`),
   and **Flycast returns 0 for every event.**

---

## 4. Showcase teardown

### 4a. The two architectures

**GLdc-based** (mk64-dc, sf64-dc, sm64-dc, DemoTek, early FDV): keep GLdc as the
TA layer, accept a **double vertex transform** — an XMTRX pass game-side purely
for clip outcodes and backface orientation, then GLdc does the real T&L — and put
the effort into *not submitting* triangles. ⚠️ **Do not copy this shape.**

**Native PVR** (QuakeSpasm DC, Xash3D DC, DMS-Engine, DKR, Doom 64 DC, sh4zam's
own examples): one fused transform → near-clip → 1/w → SQ or DMA-list pass.
⭐ QuakeSpasm's stated reason for leaving GLdc was **RAM, not speed**
(`Quake/pvr_local.h`): GLdc accumulates the whole frame's OP/PT/TR vertex lists in
main-RAM vectors that grow to a high-water mark and never shrink, overrunning a
16 MB budget; submitting straight to the TA through `pvr_dr_*` one small batch at
a time keeps render RAM at a fixed few hundred KB.

### 4b. Per project

| project | source | what it is worth to us |
|---|---|---|
| **sh4zam `example/bruces_balls`** | in-repo | The fused-DR reference + the **4.5 M poly/s** benchmark. WXYZ permutation, `SHZ_MEMORY_BARRIER_SOFT`, SQ warm-up, `shz_dcache_alloc_line`. No clipping — treat 4.5 M as a zero-clip upper bound |
| **sh4zam `example/pvr_dma`** (jnmartin64) | in-repo | Pattern B. Extracted from **Doom 64 DC** `r_phase3.c` — production code. The **vismask jump-table near clip** |
| **Xash3D DC** `pvrrender` | ✅ GPL | Best-documented. Studio path = **G-B(2)**. World path is the anti-pattern, in the same repo — a natural A/B. PMCR profiler |
| **QuakeSpasm DC** | ✅ GPL-2.0 | Full native-PVR module map; every `pvr_*.c` header comment is a design doc |
| **DMS-Engine** | ⚠️ canonical URL 404s; history at `ianmicheal/DMS-Engine` | Software-pipelined submit (G-E), 4-planes-in-1-FTRV cull (G-F), bone-palette skinning |
| **Simulant** | ✅ `next` branch | `Mat4Scratch` RAII XMTRX ownership. **2-lights-per-renderable hard cap** |
| **mk64-dc / sf64-dc / sm64-dc** | ✅ | F3DEX interpreters — structurally our emu64 problem. sf64's lighting-as-a-matrix-product; `clip_rej[]` sized to one cache line |
| **Diddy Kong Racing** (Bruceleeto) | ✅ CC0 | `shz_sq_memcpy32_1` for headers, **`shz_sq_memcpy32_xmtrx`** for batched vertices |
| **Sonic Mania DC** | ❌ | **Deleted vertex lighting entirely** to hit 60 FPS. Root cause was fixed-point math locking out the FPU |
| **The Cave / FDV** | ❌ | The only published lighting budget: 4 point + 1 directional at ~60 FPS *in a tech test*; shipping scenes run **2 real-time lights** over baked AO/GI. Specular is a **sphere map**, not computed |
| **FFFFTT** | ✅ | Uses `shz_fft` directly. 1024-pt, 22 kHz |
| **Monkey Ball DC** | ❌ stripped ELF only | Not a decomp-source port. Transferable pieces: the **TPL → PowerVR VQ** texture pipeline (`tools/vqenc.py`) and **MusyX → AICA ADPCM** conversion. The only project in the set using VQ |
| **Quake 2 DC**, **Meese**, **Gael Force**, **DemoTek**, **Elias Daler** | ❌ gone/stub | No published technical detail. `dcemulation.org` and `dreamcast-talk.com` were Cloudflare-blocked throughout |

### 4c. Lighting, since we have no hardware T&L

Everyone's answer is the same and brutally simple: **a small fixed light cap,
chosen by distance, everything else baked.** Simulant hard-caps at 2 lights per
renderable, GLdc at `MAX_GLDC_LIGHTS 4`; nobody uses probes or per-frame
amortisation. ✅ **Our town already runs exactly 2 lights** — corroboration, not
a lever. GLdc's `GL/lighting.c` is the readable reference: per-light material
products precomputed on state change; three early-outs in cost order before any
normalise or dot (distance → spot-cone → attenuation); specular via a bit-trick
`faster_pow`, never `powf`; infinite viewer by default; normals packed to 8
bits/component; `PREFETCH(vertex + 1)`; and **lighting as a separate pass** over
an already-built vertex array — which is what makes G-D/§0a's FTRV lighting
possible at all.

⭐ **Unexploited by everyone: PVR modifier volumes.** KOS's `dc/pvr.h` shows the
modifier vertex types carry *two complete parameter sets*, and the poly context
has `alpha`/`alpha2`, `fog_type`/`fog_type2`, `src`/`src2`, `dst`/`dst2`,
`txr`/`txr2` — outside vs inside the volume. A modifier volume is therefore a
per-pixel switch between two full shading configurations at **zero per-vertex CPU
cost**; `oargb` is the hardware's specular slot. [INFERRED] a point light could be
a modifier volume whose "inside" set is the lit material. **No project in this
survey published doing it.** Speculative, but the one genuinely unexplored
direction found.

### 4e. Traps other people already paid for

- 🔴 **Never emit a poly header you might not follow with a vertex.** Xash3D:
  *"the TA can mis-parse the next header as a vertex, which manifests as gray
  screen + tiny quad and then no frames."* It runs an entire extra transform pass
  over a texture chain just to answer "will anything be emitted?"
- 🔴 **Unclipped `w<=0` hangs the TA — hard reboot.** QuakeSpasm `pvr_clip.c`.
  Our `DC_PVR_W_EPS` near clip is load-bearing, not a quality choice.
- 🔴 **`shz_sq_memcpy32_1_xmtrx` and `shz_fft` clobber XMTRX.** DMS pushes poly
  headers with it *before* loading the MVP. Get the order wrong and every vertex
  is transformed by a poly header. **74 in-memory `shz_mat4x4_*` routines also
  clobber XMTRX**, and `shz_xmtrx_apply_store_*` / `load_apply_store_*` leave
  XMTRX **garbage** despite the name — *"The result of the multiplication is not
  stored within XMTRX, despite it getting clobbered."* This matters to us because
  we hold `comb` in XMTRX across the whole vertex loop.
  ✅ **Confirmed safe** with a live matrix: all scalar, trig, vector and
  quaternion routines, and the non-`_xmtrx` memory routines (front bank only).
- ⚠️ **Two codebases independently reject FSRRA for the clip parameter.** Xash3D:
  *"near-plane clipping frequently involves negative denominators; using
  FSRRA-based reciprocal can introduce large errors and warp geometry."* Same
  conclusion we reached for the perspective divide (`dc/src/dc_pvr.c:188-194`).
- ⚠️ **The near test depends on your projection.** With
  `shz_xmtrx_apply_perspective`, `clip.z = near_z` and `clip.w = -z_eye`, so the
  correct test is `(w >= z)`, **not** GL's `(z >= -w)`.
- ⚠️ **`shz_invf_fsrra` returns the ABSOLUTE value** — `FSRRA(x*x)`. Falco's own
  example takes it anyway and pays with a scene invariant ("our Z coords are in
  the positive direction"). [INFERRED] squaring also halves the usable exponent
  range.
- ⚠️ **`-m4-single` + `double` near sh4zam inline asm breaks the FP mode.**
  <https://sh4zam.com/tips.html>: *"NEVER let `double` precision variables get
  anywhere near your inline SH4 assembly routines."* Relevant to a decomp tree.
  Also: `float`×`uint32_t` promotes to double; `float`×`uint16_t` does not.
- ⚠️ **`__builtin_constant_p` short-circuits every fast scalar routine to the
  exact path.** A unit test written with literals exercises libm, not FSRRA.
- ⚠️ Only **one prefetch in flight at a time**, ~10-12 cycles to complete;
  overlapping prefetches stall the pipeline. (The only quantitative memory
  statement in the sh4zam repo, and it is not on the doc site.)
- ⚠️ **QuakeSpasm hit a GCC ICE** from sh4zam's sincos inline asm under
  `-finstrument-functions`, and LTO breaks profiler symbol resolution.

### 4f. Independent corroboration of our own decisions

- **`-Os` + a named `-O3` hot list**: every project surveyed does exactly this.
  Falco reports GTA3/Vice City landed *"within 1FPS of the performance from
  compiling everything with -O3 while simultaneously saving nearly a megabyte of
  RAM on code size"*. Nobody ships `-O0`. Vindicates the 2026-08-06 reversal.
- **Precise divide, not FSRRA, where precision decides geometry** — two codebases,
  same conclusion. **Per-batch XMTRX hold** is the right granularity — mk64's
  `gfx_sp_vertex` loads XMTRX once per `G_VTX` batch.
- **KOS preserves both FP banks across IRQs** — `irq_context` declares `fr[16]`
  *and* `frbank[16]`, and `entry.s` `frchg`s between them. Confirms the safety
  argument at `dc/src/dc_pvr.c:3470-3478`. [INFERRED] every IRQ pays that cost
  whether or not we use XMTRX, so holding a matrix resident adds nothing.
- **ARM7 is not a DSP target.** ~2.82 MHz, **no cache** — consistent with
  `CLAUDE.md`'s existing rule.
- ⭐ **KOS's own build flags are a lever nobody here has considered**: jnmartin84's
  ports build KOS with `-O3 -flto`; Xash3D patches it to `-Os`. ⚠️ Costs one
  ~27 min SDK image rebuild (`kb/traps.md`).

---

## 5. Next actions, in order

✅ **G-A — DONE 2026-08-08.** ✅ **G-C — DONE 2026-08-08.**
✅ **§3's blocking measurement — DONE; the frame is memory-bound.**
✅ **G-B(1), the vertex-index side channel — SHIPPED 2026-08-09.**

1. 🔴 **G-B(2) — the indexed-submit rewrite, 13.31 ms.** Design before coding;
   read §1 G-B(2)'s three `set_position` mutation hazards. Multi-session.
2. **`shade_vertex` — 1.82 ms**, the largest stage inside `xform` and never on
   any list. ⚠️ Its two written-and-defaulted-OFF shortcuts
   (`-DDC_PVR_SHADE_LAZYRGBA`, `-DDC_PVR_SHADE_ALPHA8`) are **settled-negative
   twice**, hoisted and unhoisted (`kb/closed.md`). A third variant needs a new
   mechanism, not a new placement.
3. **The decal-Z arming lift** (`-DDC_GX_VTXID_DECAL`) — written, gate passed,
   perf **not** measured, and its expected effect sits AT the ±2 % floor, so it
   needs repeated runs (`kb/RESUME.md` §13).
4. **G-E** (overlaps latency with stores, the right shape here), then **G-J's
   `GXNormal3f32` skip for unlit batches** (work removal, not arithmetic), then
   **F5/F6** in `kb/research-fps-ideas.md` (icache ordering, OCRAM — correct shape
   for a memory-bound frame, **hardware-only to measure**).
5. **G-F** — a cheap 0.70 ms experiment, not a valuable one. Run the
   Gribb-Hartmann + positive-vertex form first, and **add a bracket to
   `dc_emu64_cull.cpp` before costing anything.**
6. **§0a + G-D + G-G** — sub-1 ms each. Last, if ever. ⚠️ §0a/G-D needs the pass
   split because XMTRX holds `comb` across the whole loop, and a 32-vertex block
   spends more cache than it saves unless measured.
7. **G-I** — read `kb/audio-cpu-cost.md` before writing anything.
8. 🔴 **The PMCR burn** (`dc/src/dc_pmcr.c`) — everything above is a Flycast
   floor, and only `istall`/`dstall` on real hardware prices the gap.

⚠️ **None of this is a hardware verdict.** **Screenshot rule applies to all of
it**: `tools/dcqa/run_report.py` is the floor and cannot see colour. Judge any
renderer change on a matched screenshot pair (`CLAUDE.md` §3) — and remember
`shot_diff.py` cannot gate a change that alters the frame rate.
