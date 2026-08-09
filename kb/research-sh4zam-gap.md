# sh4zam — what we are actually missing

**Status: research, nothing implemented.** Opened 2026-08-08 on a user
directive ("I still feel like we are not using sh4zam to the fullest").
Sources: an audit of our own pipeline, plus four research passes over the
projects on <https://sh4zam.com/index.html#autotoc_md18>.

Nothing here has been A/B'd on this tree. **Every number that describes our
port is quoted from a measurement; every number that describes another project
is a claim until reproduced.**

Read alongside:

- `kb/closed.md` §sh4zam — the 2026-08-06 REOPEN. Read it first.
- `dc/third_party/sh4zam/VENDORING.md` — why it is vendored, the `shz_sqrtf`
  precision rule.
- `kb/perf-dc.md` — where the frame goes. ⚠️ §2/§3/§5 are `[STALE 2026-08-06]`.

---

## 0. The headline, stated correctly

**We are not "failing to use SH-4 vector instructions". We already emit FTRV,
FIPR, FSRRA and FSQRT — through KOS, not through sh4zam.**

| instruction | where, in the default build | file:line |
|---|---|---|
| FTRV | one per vertex, `mat_trans_nodiv` | `dc/src/dc_pvr.c:3096` |
| FTRV | one `mat_load` per batch | `dc/src/dc_pvr.c:3041` |
| FTRV | `PSMTXConcat`, 3 rows | `dc/src/dc_mtx.c:349`, `:354`, `:359` |
| FIPR | 7 per **lit** vertex | `dc/src/dc_pvr.c:3108-3119` |
| FIPR | 4 per light in `chan_eval` | `dc/src/dc_pvr.c:864`, `:887`, `:890`, `:899` |
| FSRRA | normal renormalise, light normalise | `dc/src/dc_pvr.c:3121`, `:874` |
| FSQRT | `sqrtf` bound to the real instruction | `dc/src/dc_fmath.c:105-107` |

**And sh4zam itself contributes ZERO instructions to a shipping image.** Its
only include is `dc/src/dc_mtx.c:141`, behind `DC_MTX_FIPR_MULTVEC`, which
**defaults to 0** (`dc/src/dc_mtx.c:270-272`). That default is correct and
measured — `dc/src/dc_mtx.c:226-249`, run `smoke-oc-dc-shz-20260806-133743`:
`us/v 3.11 → 3.12`, town FPS `20.6 → 20.4`. Inside noise.

So the gap is **not** "swap `mat_*` for `shz_*`" — `kb/closed.md:246-249`
forbids re-running exactly that. The gap is §1.

### ⚠️ 0a. The correction this research forced

`kb/RESUME.md:370-375` records, as settled, that the per-lit-vertex block is
"**ALREADY OPTIMAL** and must stop being listed as an sh4zam candidate",
because the seven ops are already `fipr()`.

**That conclusion does not survive contact with the FIPR pipelining rule.**
From Falco Girgis's own writeup, <https://dreamcast.wiki/SH4_FIPR_Optimizations>:

> "1) Very often one of the vector arguments stays constant between FIPR calls,
> but unfortunately **the compiler is too dumb to not reload all 8 registers
> between calls** regardless. … 2) **THE COMPILER CANNOT PIPELINE FIPR FOR
> SHIT.**"
>
> "FIPR has **4-5 cycles of latency**, so every fucking call to FIPR, since the
> very next instruction tries to use the result before it's been calculated,
> **the entire pipeline must stall waiting for the result... FOR EVERY FIPR
> CALL.**"

And the rule for what to do instead, <https://dreamcast.wiki/SH4_FTRV_Optimizations>:

> "There are two things to look out for … 1. There are **3 or more dot products
> (ax + by + cz + dw) being calculated back-to-back.** 2. **One of the vectors
> is held constant** while the other vector argument is variable across each.
> When you see such a scenario … you can use **FTRV** to accelerate it."

| instruction | issue | latency |
|---|---|---|
| `fipr FVm,FVn` | 1 | 4/5 |
| `ftrv XMTRX,FVn` | 1 | 5/8 |

**`dc/src/dc_pvr.c:3108-3118` matches both preconditions exactly, twice**: three
dots of the vertex against constant `mv` rows, then three of the normal against
constant `nm` rows. That is two FTRVs, not six stalling FIPRs. ✅ **Verified
2026-08-08**: `mv[0..2]` and `nm[0..2]` are consecutive rows of one matrix each,
both bound once per batch (`:3012`, `:3014`), and each group of three shares one
variable vector. Two FTRVs, fourth output slot free in both.

⚠️ ~~`chan_eval`'s four per-light FIPRs (`:864`, `:887`, `:890`, `:899`) are the
same shape.~~ **FALSE — checked 2026-08-08, and this page got it wrong.** None
of the four converts to a batch-resident FTRV: `:864` and `:3119` are
**self-dots** (both operands vary); `:887`/`:890`'s "constant" operand is `nrm`,
which is **per-vertex**; `:899`'s is `lights[li].dir`, constant across vertices
but indexed **per light**, against a variable operand that is per-vertex-per-
light. Making any of them FTRV needs an **XMTRX reload per vertex** — eight
`fmov` pairs — which is the opposite of the win. The FIPR-*stall* argument still
applies to them; the FTRV *conversion* does not. Active lights are 1-3 in
practice, hard-capped at 8 (`emu64.c:3317` writes `(1 << num_lights) - 1`;
`dc_pvr.c:850` already exits at the last set bit).

**Being FIPR is not the same as being fast.** Ranking anything on "it is already
`fipr()`" is invalid until an A/B says otherwise.

⚠️ The blocker: XMTRX already holds `comb` across our whole vertex loop
(`dc/src/dc_pvr.c:3033-3045`), so an FTRV for lighting cannot coexist with the
position FTRV in the same pass. **The two findings compose into §1 G-F — split
the loop into passes.** `kb/RESUME.md:373` reached the "loses, because `comb`
needs XMTRX" conclusion correctly *for a single-pass loop* and stopped there.

---

## 1. The gaps, ranked

Ranked by (expected gain × confidence) ÷ cost. ⚠️ Read §3 first — the
denominator these are ranked against is stale.

### G-A ✅ DONE 2026-08-08 — the build could not link most of sh4zam

**Landed.** `dc/Makefile`: the glob now names `source/sh4/shz_complex_sh4.c`
alongside `source/*.c`, `SHZ_S` carries the two `.s` files, a new
`$(OBJDIR)/%.s.o: $(ROOT)/%.s` rule assembles them (lowercase `.s` ⇒ gcc runs
**no** cpp, so no `-MMD`, no prelude, no `$(DEFINES)` — `DEPS` filters the
would-be `.d` out), and `$(SHZ_OBJS): TU_DEFS = -DNDEBUG` scopes the assert kill
to sh4zam only. `source/sw/` is still not globbed, now on purpose.

**Verified, not assumed.** The five previously-undefined out-of-line symbols are
present as `T` in the objects:

```
shz_complex_sh4.c.o : _shz_fft_dc
shz_mem_sh4.s.o     : _shz_memset8_sh4_  _shz_memcpy128_sh4_  _shz_sq_memcpy32_sh4_
shz_xmtrx_sh4.s.o   : _shz_xmtrx_load_apply_store_{4x4,3x4,3x3}_sh4
```

and a throwaway TU calling `shz_sq_memcpy32`, `shz_memcpy128`, `shz_memset8`,
`shz_fft` and `shz_xmtrx_load_apply_store_4x4` **links clean** against them
(`kos-cc`, `-DNDEBUG`). Before this change that TU could not link at all.

⚠️ **The shipping image is unchanged so far**: `sh-elf-nm` on
`AnimalCrossing.elf` finds **zero** of those symbols — nothing in `dc/` calls
them yet, so `--gc-sections` drops every one. G-A buys capability, not speed.
`.text` for the town build line is **2,882,948 B**. ⚠️ Do not read the 300 B
against `kb/state-log.md`'s 2,883,248 as a saving — that figure is from a
different session's tree and the two were never built back-to-back.

Trap for anyone extending this: a *recursive* wildcard here is a build break,
not a slowdown — see below.

<details><summary>the original finding</summary>

`dc/Makefile:1173` globs `source/*.c` — top level only. Upstream's
`CMakeLists.txt` gives the real Dreamcast source set, **seven files**:

```cmake
source/{shz_matrix,shz_quat,shz_version,shz_xmtrx}.c
source/sh4/shz_complex_sh4.c
source/sh4/shz_xmtrx_sh4.s
source/sh4/shz_mem_sh4.s
```

⚠️ **`source/sw/*.c` must NOT be compiled on SH-4** — `shz_xmtrx_sw.c` defines
a conflicting `xmtrx_state_`. A naive recursive glob breaks the build. Our
current glob excludes it by accident of depth.

What is missing today, and what breaks:

| missing file | symbols | what fails to link |
|---|---|---|
| `sh4/shz_mem_sh4.s` | `_shz_memset8_sh4_`, `_shz_memcpy128_sh4_`, `_shz_sq_memcpy32_sh4_` | `shz_memset8`, `shz_memcpy128`, **`shz_memcpy32`** (calls `memcpy128_sh4_` at ≥8 blocks), **`shz_sq_memcpy32`** (pure out-of-line call) |
| `sh4/shz_xmtrx_sh4.s` | `_shz_xmtrx_load_apply_store_{4x4,3x4,3x3}_sh4` | the pipelined load·multiply·store forms |
| `sh4/shz_complex_sh4.c` | — | **`shz_fft`** |

`shz_sq_memcpy32_1` and `_1_xmtrx` are fully inline and would link — the
multi-block siblings would not. **Fix this before costing anything else.**

Also required at the same time: **`-DNDEBUG`**. sh4zam's memory and inline-asm
routines carry `assert()`s on alignment and FP mode by default, and
<https://sh4zam.com/tips.html> §"Non-Debug Builds" says to build with `NDEBUG`.
Ours would sit inside the vertex loop.

### G-B 🔴 Transform each unique vertex ONCE, before index expansion

**This is the biggest lever found, and it aims at the largest unattributed
block in the project.** `kb/perf-dc.md:40-45` records ~23.6 ms — 52 % of the
then-frame — as "`dl_G_TRIN`'s index expansion PLUS our own `GX*` setters,
never separated". G4 later split it: `gxgap=18.34 ms` is emu64's vertex loop
(`kb/state-log.md:486-522`).

Xash3D DC hit exactly this and quantified the fix
(<https://github.com/maximqaxd/xash3d-fwgs_dc/blob/04306ef/ref/pvr/pvr_studio.c#L2653>):

> "barney has ~430 unique verts but ~2156 strip corners — each vertex was
> previously FTRV'd **~5× (once per corner reference)**. Now it's done exactly
> once, sequentially, cache-friendly, and the strip loop just indexes
> `s_clip[vi]`."
>
> "Pre-divide here so the submit loop has zero FPU latency per corner. Cost:
> 1 FSRRA + 2 fmul × 430 verts vs. 1 FSRRA + 2 fmul × **2156 corners**."

Its comment on the structure: *"SUPER HOT PATH: whole model in front of near
plane. Zero intermediate arrays. One loop: `s_clip[vi]` → persp divide →
`pvr_dr_commit`. Eliminates ~60KB of `s_tv`/`s_strip_uv`/`s_strip_ac` traffic
per frame."*

**We already have direct evidence the redundancy is real here.** Our per-batch
vertex memo cache (`dc/src/dc_pvr.c:2263-2288`, lookup `:3072-3081`) exists
solely to short-circuit repeated source vertices, and its measured hit rate is
**48.2-48.9 %** (`kb/perf-dc.md:481`). That is a hash-lookup workaround for the
same problem. Doing it structurally — transform the unique set once, index into
it — beats caching it, and deletes the hash and the 12-field compare too.

### G-C ✅ DONE 2026-08-08 — `emit` 2.20 → 1.45 ms, −34 %

**SHIPPED AND ON BY DEFAULT.** Not pattern C (stage-then-blast) as this section
recommends below, but **pattern A (fused DR)**: `emit_projected()` writes the
eight TA words straight into `pvr_dr_target()` and `pref`s them with
`pvr_dr_commit()`. That deletes the stack `pvr_vertex_t` build pass, the
read-back `sq_fast_cpy` did of it, and two calls per corner — touches 2 and 4
of the table below, which is what pattern C was proposed to achieve, without
changing what `g_gx.vertex_buffer[]` is.

| | baseline | G-C |
|---|---:|---:|
| `[VTXSPLIT] emit` | 2.20 ms | **1.45 ms** |
| per emitted vertex | 0.801 µs | **0.513 µs** (−36 %) |
| `us/v` | 3.24 | **2.89** |
| `draw` | 30.7 ms | 29.3 ms |

Counter `[DC/PVR] dr verts=30,386,676 of 37,204,221` = **81.7 %**, exactly the
non-punch-through share: the PT producer keeps the stack path because a PT
record must be **held** until list 4 can legally be opened, and DR writes a
queue currently pointed at the general list. Kill switch `-DDC_PVR_NO_DR`.

The four KOS 2.3 facts it rests on were read out of the SDK image, not assumed
— `pvr_dr_addr` is a global statically initialised to `MEM_AREA_SQ_BASE`
(`pvr_globals.c:21`), `pvr_dr_init()` is a deprecated no-op, `pvr_list_begin()`
already `sq_lock`s the TA (`pvr_scene.c:198`), and **`sq_flush()` clobbers
`"memory"`** (`arch/sq.h:112-118`), which is the one that makes the eight plain
stores ordered before the `pref` at all. `dc/src/` now depends on QACR, hence
on the MMU staying off — noted in `kb/research-mmu-hardware-tax.md:67`.

The section below is kept as the analysis that led here.

### G-C 🔴 Stop calling `pvr_prim`. A vertex is touched 4-5×.

Every TA word funnels through `pvr_prim()` (`dc/src/dc_pvr.c:544-549`). No
`pvr_dr_*`, no `sq_*`, no `QACR` anywhere in `dc/`.

> ⚠️ **STALE AS OF 2026-08-08 — `emit_projected()` NOW USES `pvr_dr_*`.** The
> vertex is built directly in the store queue; the punch-through producer still
> takes the old stack path, and the poly header still goes through `pvr_prim`.
> Kill switch `-DDC_PVR_NO_DR`. The sentence above is kept because the rest of
> this section is written against it. **`dc/src/` now depends on QACR, and so on
> the MMU staying off** — see `kb/research-mmu-hardware-tax.md:67`.

| touch | file:line |
|---|---|
| 1. attributes → `g_gx.current_vertex` | `dc/src/dc_gx.c:1095-1111` |
| 2. 32-byte struct copy → `g_gx.vertex_buffer[]` | `dc/src/dc_gx.c:462` |
| 3. read out, transformed → stack `ClipVtx cv[4]`, → memo cache | `dc/src/dc_pvr.c:3068-3364` |
| 4. field-by-field → stack `pvr_vertex_t pv`, which `pvr_prim` copies **again** into the SQ | `dc/src/dc_pvr.c:2155-2170` |
| 5. punch-through only: `memcpy` into `s_pt_buf`, out again on replay | `dc/src/dc_pvr.c:532-539`, `:2746` |

Alignment is not set up for DR either: `pvr_vertex_t pv` (`:2155`),
`pvr_poly_hdr_t s_hdr` (`:238`) and `ClipVtx cv[4]` (`:3065`) declare none.

**Nobody in the surveyed set does what we do.** Three patterns exist:

| pattern | who | touches |
|---|---|---|
| **A. Fused DR** — FTRV result written field-by-field straight into `pvr_dr_target()` | `bruces_balls.c`, DMS-Engine, Xash studio path | **1** |
| **B. Transform-in-place in the DMA list** — reserve from `pvr_vertbuf_tail()`, write raw attrs there, FTRV / clip / divide **in place**, then bump the tail pointer | sh4zam `example/pvr_dma`, Doom 64 DC `r_phase3.c` | **1** |
| **C. Stage then blast** — own array already in 32-byte TA format, transform in place, bulk-move to SQ | GLdc, Simulant, Xash world path | **2** |

Pattern B's submission is literally a pointer bump:

```c
uint32_t amount = (material_change * sizeof(pvr_poly_hdr_t))
                + (verts_to_process * sizeof(pvr_vertex_t));
pvr_vertbuf_written(list, amount);
```

`sizeof(pvr_vertex_t) == sizeof(pvr_poly_hdr_t) == 32`, so KOS's
multiple-of-32 requirement falls out free.

**The minimum-change route for us is C**: make `g_gx.vertex_buffer[]` already
be `pvr_vertex_t[]` and transform in place. That deletes touches 2 and 4 with
no architecture change.

Note KOS's own docs: *"the DMA is faster for transactions which are
consistently large; however, the store queues tend to have better performance
and have less configuration overhead when bursting smaller chunks of data"*
(<https://kos-docs.dreamcast.wiki/group__store__queues.html>). Neither of those
is `pvr_prim`.

⚠️ **Blocker:** we defer punch-through and replay it last
(`dc/src/dc_pvr.c:2732-2751`); the single-list rationale is `:11-47`. DR
interacts with list ordering. Read that before designing the experiment.
Doom 64 DC proves **mixing DMA and DR in one frame is legal** — its diffuse
pass goes out via DMA vertbuf while the bump pass resubmits the same vertices
via DR.

### G-D 🟡 Split T&L into passes over 32-vertex blocks

This is the enabler for G-A's FTRV lighting *and* a standalone win. From
<https://dreamcast.wiki/Fast_SH4_Vertex_Processing>:

> The naive per-vertex loop (fetch → N·L → specular → clamp/pack → transform →
> 1/w → store) "will get quite a big loop and you will **run out of FPU
> registers for sure**. Additionally this code is subject to **bad pipelining**
> since most of the operations depend on the results of previous operations."

Split into independent passes — diffuse, specular, clamp+pack, transform+divide
— and run each over a **block**, not the whole buffer:

> "The trick now is to run the small loops not over the whole buffer at once but
> over smaller blocks of the buffer. This way the currently processed vertices
> will be kept in the cache for each of the many loop iterations. As to my
> experience **a block size of 32 vertices gives the best performance.**"

Composability is a stated side benefit ("if you don't need specular lighting
then just don't call the specular lighting loop") — which maps onto our
`need_light` guard at `dc/src/dc_pvr.c:3049-3050`. And each small loop has spare
FP registers, so it can be software-pipelined (G-E).

Same page, cache rules that apply to any such pass:
- **16 KB direct-mapped** operand cache; address *n* and *n*+16384 collide.
- `PREF` the next element at the **top** of the loop.
- **`MOVCA.L` is not optional.** Without it a store to a write-only output
  buffer causes a read-for-ownership fetch of a line you are about to fully
  overwrite. "While the OCBP instruction can be left out without any
  performance drops this does not apply for MOVCA.L." → `shz_dcache_alloc_line()`.
- **Prefer FSRRA to FSQRT** — "rewrite your math formulas (e.g. range falloff
  lighting)" to suit it.
- `FMAC` is "as fast as normal float addition… so basically you pay one and get
  another operation for free."

### G-E 🟡 Software-pipeline the submit loop

DMS-Engine's `render_fast` issues vertex *i+1*'s FTRV **before** storing vertex
*i*, hiding the 5-8 cycle latency behind the SQ stores
(<https://github.com/ianmicheal/DMS-Engine/blob/main/Sonic_example/dms/dc_model.c>):

```c
for (int i = 1; i < count; i++) {
    SHZ_PREFETCH(&src[i + 4]);
    ...
    shz_vec4_t next_t = shz_xmtrx_transform_vec4(shz_vec4_init(nx, ny, nz, 1.0f));

    pvr_vertex_t* pv = pvr_dr_target(*dr);   /* submit PREVIOUS while next_t settles */
    pv->flags = cur_flags; pv->x = cur_sx; pv->y = cur_sy; pv->z = cur_invw;
    pv->u = cur_u; pv->v = cur_v; pv->argb = cur_argb;
    pvr_dr_commit(pv);

    next_t = shz_vec4_swizzle(next_t, 1, 2, 3, 0);
    cur_invw = shz_invf_fsrra(next_t.w);
    ...
}
```

4-vertex prefetch distance; source vertex `__attribute__((aligned(32)))` and
exactly 32 bytes. Falco's own example uses `SHZ_MEMORY_BARRIER_SOFT()` between
`pvr_dr_target()` and the first read of the FTRV result, commented: *"Prevent
GCC from reordering our memory accesses in a way which causes us to access the
transformed position before it's ready, which would cause a pipeline stall."*

### G-F 🟢 DEMOTED 2026-08-08 — four frustum planes in one FTRV

⚠️ **Re-costed and demoted before anyone wrote code.** The late-path cull is
**0.70 ms of a 30.39 ms draw — 2.3 %** (§3), not the 2.0 ms this section was
ranked on, and G3's own 254 calls/frame are **not timed at all**. Read §3's
G3-added-calls paragraph before touching it.

Two further facts the DMS shape below does not survive contact with:

- **There are no stored frustum planes.** The six "planes" are literally the six
  comparison lines at `dc_gx.c:591-596`; each corner is pushed through MV *then*
  P. Nothing folds P·MV at cull time (`dc_pvr.c:3011-3033` does, but only for
  *surviving* batches, downstream). An FTRV form must pay the 48-mul fold itself
  **per call**, unless it caches planes per matrix change.
- **A cheaper shape exists that needs no FTRV at all.** Gribb-Hartmann planes
  extracted from a folded MVP (cached per matrix change) plus the positive-vertex
  test is **6 dots total**, against today's fixed 8 corners × 7 dots = 168 muls
  and 168 adds with no early-out. That is the experiment to run first; the FTRV
  is a second-order win on top of it.
- ⚠️ `dc_gx.c:566-569` records that the function is scalar **on purpose** so it
  needs no `dc_mtx_xmtrx_invalidate()`. An XMTRX form makes that invalidate
  mandatory. (Its cited precedent `dc_pvr.c:2812` is stale — now `:3045`.)
- ⚠️ Ordering: `dirty_check()` and `setup_1tri_2tri_1quad()` must run **before**
  the cull call (`dc_emu64_cull.cpp:358-359`), because `dc_gx_aabb_is_offscreen`
  reads `g_gx.projection_mtx` and `g_gx.current_mtx` live.

<details><summary>the original proposal</summary>

#### Four frustum planes in one FTRV — replaces the scalar AABB cull

`dc/src/dc_gx.c:584-590` is ~200 scalar multiplies per batch, ~2.0 ms/frame,
deliberately scalar (`:566-569`), reached from both the late path (`:603-660`)
and G3 (`dc/src/dc_emu64_cull.cpp:399`).

DMS-Engine packs the four side planes as a matrix and gets all four signed
distances in one FTRV:

```c
typedef struct __attribute__((aligned(32))) {
    shz_mat4x4_t side_planes;
    shz_vec4_t   near_plane;
    shz_vec4_t   far_plane;
} WorldFrustum;

int dc_frustum_cull_sphere(const DCCamera* cam, shz_vec3_t center, float radius) {
    shz_vec4_t pos = shz_vec4_init(center.x, center.y, center.z, 1.0f);
    float near_dist = shz_vec4_dot(cam->_frustum.near_plane, pos);   /* FIPR */
    if (near_dist < -radius) return -1;
    float far_dist  = shz_vec4_dot(cam->_frustum.far_plane, pos);    /* FIPR */
    if (far_dist < -radius) return -1;
    shz_xmtrx_load_4x4((shz_mat4x4_t*)&cam->_frustum.side_planes);
    shz_vec4_t dists = shz_xmtrx_transform_vec4(pos);                /* 4 planes, 1 FTRV */
    if (dists.x < -radius || dists.y < -radius ||
        dists.z < -radius || dists.w < -radius) return -1;
    return 0;
}
```

DCA3 (GTA3 DC) does the same — 6 plane dots become 2 FIPRs + one FTRV.
`kb/RESUME.md:411-420` already names our AABB cull the live sh4zam experiment;
this is the shape it should take. ⚠️ Its 2.0 ms cost is **pre-G3**; G3 moved
`vcull` 9,915 → 1,002 (`kb/state-log.md:246-262`). Re-measure before costing.

</details>

### G-G 🟢 WXYZ permutation — W at cycle 4 instead of 7

FTRV writes its result one component per cycle: **X at 4, Y at 5, Z at 6, W at
7** (<https://sh4zam.com/tips.html> §"Faster Perspective Division", the only
cycle table sh4zam publishes). The perspective divide needs W *first*.

Fix: fold a WXYZ permutation into the projection so FTRV yields W in the X
slot, then swizzle back. Falco's example:

```c
shz_xmtrx_init_permutation_wxyz();
shz_xmtrx_apply_screen(screen_width, screen_height);
shz_xmtrx_apply_perspective(fov, aspect, near_z);
...
trans_pos = shz_vec4_swizzle(trans_pos, 1, 2, 3, 0);   /* deswizzle */
```

or `shz_xmtrx_load_wxyz_4x4()` at load time. ⚠️ `tips.html` names it
`shz_xmtrx_load_4x4_wxyz()` — **that function does not exist**; the header
declares `shz_xmtrx_load_wxyz_4x4()` (`shz_xmtrx.h:127`).

**Nearly free for us**: our `comb` fold at `dc/src/dc_pvr.c:3015-3032` is
already a hand-written index swap (`comb[j][i] = s`). Making it WXYZ is a
second index change in a loop that runs once per batch.

### G-H 🟢 SQ warm-up for batch-constant fields

KOS alternates between exactly two store queues. Falco's example writes
per-vertex-*constant* fields into **both** SQs once, outside the loop, and never
per vertex:

```c
((pvr_vertex_t*)pvr_dr_target(*dr_state))->argb = base_color;   /* ×2, hoisted */
((pvr_vertex_t*)pvr_dr_target(*dr_state))->flags = PVR_CMD_VERTEX;
```

Applies to our `oargb` (`dc/src/dc_pvr.c:2168` writes 0 every vertex), `flags`,
and often `argb`.

### G-I 🟢 `shz_fft` for audio — mechanism now known, budget not

`shz_fft` is radix-2 **decimation-in-frequency** Cooley-Tukey, in place, and the
acceleration is **FTRV, not FIPR** — a 2-point butterfly *is* a real 4×4 map on
`(re₀,im₀,re₁,im₁)`, held in XMTRX. Twiddles come from FSCA computed **in the
back bank in place** (`shz_xmtrx_update_fft_butterfly`), the one division is
`shz_divf_fsrra`, and all data movement is paired `fmov.d` (a `shz_complex_t`
is exactly 8 bytes). Inner loop unrolls to 2/4/8/16-point butterflies selected
per stage. Bit-reversal uses the back bank as swap scratch.

Constraints: size must be a power of two, buffer 8-byte aligned, **it clobbers
XMTRX**, and **there is no inverse FFT** (`shz_complex.h` `\todo`).

FFFFTT (<https://github.com/meisei4/fffftt>) uses it directly — 1024-point,
512 bins, 22 kHz mono, Blackman window via `shz_cosf`, work buffer
`alignas(32)`. It ships a profiler using `perf_cntr_timer_ns()`.

⚠️ **No absolute cost per transform is published anywhere.** sh4zam's own test
only asserts "faster than the scalar reference". **And nobody has established
that jaudio's hot path contains an FFT-shaped transform at all.** Read
`kb/audio-cpu-cost.md` before writing code. This stays 🟢 until then.

### G-J 🟢 Scalar remainders

| site | file:line | shape |
|---|---|---|
| `chan_eval` spot polynomials + accumulate | `dc/src/dc_pvr.c:902-914` | leftover after the FIPRs |
| texgen | `dc/src/dc_pvr.c:2461-2462` | 2 × dot-3 |
| `pack_argb` | `dc/src/dc_pvr.c:555-563` | 4 × (mul, add, 2 clamps, `ftrc`) per vertex |
| `lerp_vtx` | `dc/src/dc_pvr.c:2291-2335` | clip path only |
| `GXNormal3f32` | `dc/src/dc_gx.c:1156-1165` | 3 mul + 6 compares + 3 float→short |

---

## 2. What is already dead — do not re-derive

- **Swapping `mat_load` → `shz_xmtrx_load`.** Same instructions, different
  header. `kb/closed.md:246-249`.
- **`shz_sqrtf`.** It is `x · FSRRA(x)`, not FSQRT. FSRRA's bound is **±2⁻²¹**
  (Renesas, via <https://shared-ptr.com/sh_insns.html>), and multiplying by `x`
  compounds it. Worse: sh4zam guards only `x == 0.0f`, so **`shz_sqrtf(negative)`
  is garbage, not NaN**. `dc/src/dc_fmath.c` binds `sqrtf` to real FSQRT — keep it.
- **FSCA.** Only four live `sinf`/`cosf` sites (`src/game/m_camera2.c:87-90`);
  everything else is `sins()`/`coss()`, a table lookup at the same 16-bit
  resolution FSCA takes as input. `kb/closed.md:255-261`.
- **FIPR for `PSMTXMultVec`.** Measured, inside noise (`dc/src/dc_mtx.c:226-249`).
- **Composing `P·MV` in XMTRX** — costed and dropped, 67 µs of a 45 ms frame
  (`kb/RESUME.md:419`). ⚠️ But see G-G: making the *existing* fold WXYZ is a
  different, much cheaper change.
- ~~**The per-lit-vertex block is already optimal.**~~ **REOPENED — see §0a.**
- **`-ffast-math` on our tree.** Forbidden, `kb/closed.md:169-170`. ✅ **And it
  costs us nothing**: <https://sh4zam.com/tips.html> §"Selective Fast Math" —
  *"SH4ZAM is smart enough to fall-back to inline ASM for FSCA and FSRRA rather
  than relying on the compiler to emit them."* Confirmed in source;
  `shz_inv_sqrtf_fsrra_sh4` is unconditionally `asm("fsrra %0")`.

---

## 3. ✅ ANSWERED 2026-08-08 — the measurement that blocked all of this

**Run, not quoted.** Town, steady state, 430 `[EMU64H]` windows, averaged over
the last 60. `[EMU64H]` **doubled** (rule 1 below); `[PHASE]` **not**.

| | per logic tick (as printed) | **per presented frame** |
|---|---:|---:|
| `tot` | 16.41 ms | **32.82 ms** |
| `TRIN_INDEPEND` | 11.20 / 127 calls | **22.40 ms / 254 calls** |
| `gap` | 2.64 ms | 5.28 ms |
| `draw` / `skip` | — | 30.39 / 2.57 ms |
| `cull` / `xform` | — | 0.70 / 8.38 ms |
| `us/v` | — | 3.05 |

**Self-check `tot×2` vs `draw+skip`: 32.82 vs 32.97, −0.4 %.** `armed,
period=1`, no `DISABLED`, `[EMU64C] cum reinst=0`.

⭐ **`TRIN_INDEPEND` is 22.40 ms of a 30.39 ms draw — 73.7 %.** The old
*34.4 of 45.6, 75 %* is retired: the share barely moved, the absolute fell by a
third, and the frame is far faster than the `draw=49.9` this page used to cite,
because that predates session 9's fourth-iteration levers. `fps_p50` **25.9**.

**Inside TRIN: `cull 0.70 + xform 8.38 = 9.08` attributed, `13.31 ms` NOT —
43.8 % of the entire draw.**

### 🔴 2026-08-08 — G5 SPLIT `xform` AND IT RE-RANKS THIS WHOLE DOCUMENT

`-DDC_PVR_VTXSPLIT=16` (`dc/src/dc_pvr.c`). Town, `[SCENE_MODE] 18 -> 9`,
`us/v=3.24`, `xform=8.9 ms`, `v=2745 vlit=2613`. Per PRESENTED frame:

| stage | ms | of xform | what it is |
|---|---:|---:|---|
| **emit** | **2.15** | 27 % | near clip + perspective divide + `pvr_prim`'s 32-byte copy — **G-C** |
| **shade** | **2.03** | 26 % | `shade_vertex()`, the per-light loop |
| **memo** | **1.68** | 21 % | the vertex memo's hash + 12-field compare, **paid on every vertex** |
| tex | 0.62 | 8 % | `apply_texgen` + uv scale |
| **lit** | **0.58** | 7 % | ⭐ **the six FIPRs §0a is about** |
| post | 0.57 | 7 % | PT alpha, TEV const/fold, memo store |
| **xf** | **0.23** | 3 % | ⭐ **the position FTRV** |
| sum / xform | 7.87 / 8.9 | 88 % | the 1.0 ms residual is per-primitive loop overhead |

> ### 🔴 CORRECTION 2026-08-08 — THE PER-VERTEX CYCLE FIGURES BELOW ARE HALVED
>
> **`VS_MARK(VS_SHADE)` and every mark after `VS_MEMO` sit DOWNSTREAM of the
> memo-hit `continue`** (`dc_pvr.c`, the k-loop). A memo hit is charged to
> `memo` and to nothing else. So `xf`, `lit`, `tex`, `shade` and `post`
> accumulate over memo **MISSES**, not over all `v` vertices — and dividing
> them by `v` understates every one of them by the hit rate.
>
> With `samp=558095 memohit=835733` → 1,674,285 sampled corners, **49.9 % hit**:
>
> | stage | charged on | as published | **actual** |
> |---|---|---:|---:|
> | `memo` | every vertex (2745) | 122 cyc | 122 cyc ✔ |
> | `shade` | misses only (~1375) | 148 cyc | **~295 cyc** |
> | `lit` | lit ∧ miss (~1309) | 44 cyc | **~89 cyc** |
>
> **The paragraph immediately below is therefore wrong about WHY, and right
> about WHAT.** `lit` at 89 cycles is *not* "about what six FIPRs should cost";
> there is roughly 2x of headroom in that block. But it is still **0.58 ms of a
> 30 ms frame**, so the demotion stands — **on absolute milliseconds, not on
> the cycle argument.** Do not re-promote §0a on the strength of the 89 either:
> a perfect rewrite of the whole stage is still worth under 0.3 ms.
>
> `memo`'s 122 is unaffected — it is the one stage charged on every vertex.

⭐⭐ **§0a AND G-D ARE AIMED AT 0.58 ms OF A 30 ms FRAME — 1.9 %, and a
perfect FTRV rewrite takes maybe half of it.** ~~`lit` is 222 ns over 2,613 lit
vertices = **44 cycles**, which is about what six FIPRs, an FSRRA and a
normalize *should* cost.~~ (See the correction above: the divisor is misses,
not lit vertices, and the real figure is ~89.) The block is not the frame's
problem. **§0a is hereby DEMOTED from #2 to the bottom of the list**, on a
measurement rather than an argument — and the 2026-08-06 correction it
"reopened" was closer to right than the reopening was. The position FTRV is
**0.23 ms**: every matrix-unit idea in this document is chasing ~0.8 ms
combined.

⭐⭐⭐ **THE FRAME IS MEMORY-BOUND, NOT FPU-BOUND.** `memo` costs **122 cycles
per vertex** for a hash and a 12-field compare — that is not arithmetic, that
is the random access into `verts[s_vmemo_src[slot]]` missing the operand
cache. `emit` is `pvr_prim` copying 32 bytes per corner into the store queue.
Together with `shade`, the three memory-shaped stages are **5.86 ms, 75 % of
`xform`**, and the two FP stages are 0.81 ms. This is the same conclusion the
icache work reached from the other end (`tools/dcopt/icache_map.py`: the
12-symbol inner loop is 1.4x an 8 KB direct-mapped cache), and it means
**Flycast understates all of it** — it models no cache at all.

**The re-ranking that follows from this:**

1. **G-C (emit, 2.15 ms)** — now #1. `pvr_dr_*` writes the vertex straight into
   the store queue and deletes both the stack `pvr_vertex_t` and `sq_fast_cpy`'s
   read-back. ⚠️ Read §4e first, and note KOS 2.3 makes this easy: `pvr_dr_target()`
   is `pvr_dr_addr ^= 32` and `pvr_dr_commit` is `sq_flush`, while
   `pvr_list_begin` has already `sq_lock`ed the TA — so DR and `pvr_prim` share
   one QACR setup and can be mixed.
2. **`shade_vertex` (2.03 ms)** — never examined, never on this list. It is now
   the second largest stage in the vertex path.
3. **memo (1.68 ms)** — the cache is still net positive (~50 % hit rate saves
   ~4.0 ms of stages for 1.68 ms), but it is keyed on a 12-field content
   compare. **G-B would replace it structurally**: emu64's index stream already
   knows which corners share a vertex, so an index-keyed memo needs no hash, no
   compare and no random read, and would hit ~100 %.
4. **G-B (13.31 ms)** — still the largest single block in the project, and
   untouched by any of the above: it is emu64's expansion loop, upstream of
   everything measured here.
5. §0a / G-D / G-F — **0.58 + 0.23 + 0.70 ms.** Do these last, if ever.

### The ranking, re-costed against 30.39 ms

| gap | addressable | verdict |
|---|---:|---|
| **G-B** | **13.31 ms** | unchanged as the top lever, and now with a number |
| §0a / G-D | inside `xform` 8.38 ms | and only part of that is the six FTRV-able FIPRs |
| **G-F** | **0.70 ms**, 2.3 % of draw | ⚠️ **DEMOTED.** It was ranked on a pre-G3 2.0 ms. Cheap experiment, low value |

⚠️ **G3 ADDED cull calls, it did not replace them** — `cu_trin_indep` skips
emu64's handler only on the `return` path, so punt *and visible* both fall
through to `GXEnd` → the late cull. Per frame: `trin 254, cull 138, vis 59,
punt 57`. The scalar cull runs **254× from G3 plus ~116× on the late path, and
only the second set is inside the `cull=` bracket** — `dc_emu64_cull.cpp` has
zero `dc_time_us` reads. The `vcull` collapse 9,915 → 1,002 was a drop in the
late cull's *yield*, not its call count, and "visible" is its most expensive
answer because it has no early-out. **Any G-F costing needs a new bracket first.**

⚠️ **Two caveats on the run.** (1) No `-DDC_AUTOWALK` — the camera never moves
and all 60 windows are byte-identical (`v=2745 vsrc=2745 vcull=186`). Great
signal-to-noise, **one static town view**, not a walk. (2) `vmemo` here is
**54.40 %** (2,642,897/4,858,650), not `kb/perf-dc.md:481`'s 48.2-48.9 % —
and `[PHASE] vmemo=` is **cumulative**, unlike everything else on that line, so
it must be differenced between windows.

Corollaries that survive regardless:

1. `[EMU64H]` is per **logic tick** — double it. `[PHASE]` and `[GXSPLIT]` are
   per presented frame — do not.
2. `us/v` is the instrument, not FPS (`kb/perf-dc.md:149-153`).
3. Every FPS number ever produced came from **Flycast, which models no
   instruction cache**, against a 2,883,248 B `.text` on an 8 KB direct-mapped
   icache (`kb/RESUME.md:194-197`).
4. `kb/RESUME.md`'s `dc_pvr.c` line references have **drifted**. Current:
   `dc_pvr.c:3105-3124`, `:3012-3014`, `:3108-3119`, `:3096`, `:2168`,
   `dc_gx.c:570-601`.
5. **A better instrument exists and is reusable.** Xash3D DC ships an SH-4 PMCR
   profiler using `perf_cntr_start(PRFC1, PMCR_ELAPSED_TIME_MODE,
   PMCR_COUNT_CPU_CYCLES)` — *"Use PRFC1 to avoid interfering with KOS internal
   timing (PRFC0)"* — feeding an on-screen `r_speeds` breakdown. That
   PRFC0/PRFC1 split is directly applicable to our open "why is hardware slower
   than Flycast" question, where PMCR on a burn is the only instrument.

---

## 4. Showcase teardown

### 4a. The two architectures

**GLdc-based** (mk64-dc, sf64-dc, sm64-dc, DemoTek, early FDV): keep GLdc as the
TA layer, accept a **double vertex transform** — an XMTRX pass game-side purely
for clip outcodes and backface orientation, then GLdc does the real T&L — and
put the effort into *not submitting* triangles. ⚠️ **Do not copy this shape.**

**Native PVR** (QuakeSpasm DC, Xash3D DC, DMS-Engine, DKR, Doom 64 DC,
sh4zam's own examples): one fused transform → near-clip → 1/w → SQ or DMA-list
pass. QuakeSpasm states the reason it left GLdc, and it was **RAM, not speed**
(`Quake/pvr_local.h`):

> "GLdc accumulates the whole frame's OP/PT/TR vertex lists in main-RAM vectors
> that grow to a high-water mark and never shrink; on large maps that overruns
> the 16MB budget and starves the Quake hunk. Instead, this renderer submits
> geometry straight to the PVR Tile Accelerator through the store queues
> (pvr_dr_*), transforming and firing one small batch at a time. Render RAM
> stays at a fixed few hundred KB."

FDV's author was moving the same direction: *"The frame rate is concerning,
hopefully it will improve once I've made the switch to native PVR."*

### 4b. Per project

| project | source | sh4zam | what it is worth to us |
|---|---|---|---|
| **sh4zam `example/bruces_balls`** | in-repo | ✅ | The fused-DR reference + the **4.5 M poly/s** benchmark. WXYZ permutation, `SHZ_MEMORY_BARRIER_SOFT`, SQ warm-up, `shz_dcache_alloc_line`. No clipping — treat 4.5 M as a zero-clip upper bound |
| **sh4zam `example/pvr_dma`** (jnmartin64) | in-repo | ✅ | Pattern B. Extracted from **Doom 64 DC** `r_phase3.c` — production code. The **vismask jump-table near clip** |
| **Xash3D DC** `pvrrender` | ✅ GPL | ✅ | Best-documented. Studio path = G-B. World path is the anti-pattern, in the same repo — a natural A/B. PMCR profiler |
| **QuakeSpasm DC** | ✅ GPL-2.0 | ✅ | Full native-PVR module map, and every `pvr_*.c` header comment is a design doc |
| **DMS-Engine** | ⚠️ canonical URL 404s; history at `ianmicheal/DMS-Engine` | ✅ | Software-pipelined submit (G-E), 4-planes-in-1-FTRV cull (G-F), bone-palette skinning |
| **Simulant** | ✅ `next` branch | ✅ | `Mat4Scratch` RAII XMTRX ownership. **2-lights-per-renderable hard cap** |
| **mk64-dc / sf64-dc / sm64-dc** | ✅ | ✅ | F3DEX interpreters — structurally our emu64 problem. sf64's lighting-as-a-matrix-product; `shz_dcache_alloc_line`; `clip_rej[]` sized to one cache line |
| **Diddy Kong Racing** (Bruceleeto) | ✅ CC0 | ✅ | The surviving proof of his technique: `shz_sq_memcpy32_1` for headers, **`shz_sq_memcpy32_xmtrx`** for batched vertices |
| **Sonic Mania DC** | ❌ | ✅ | **Deleted vertex lighting entirely** to hit 60 FPS. Root cause was fixed-point math locking out the FPU |
| **The Cave / FDV** | ❌ | ✅ | The only published lighting budget: 4 point + 1 directional at ~60 FPS *in a tech test*; shipping scenes run **2 real-time lights** over baked AO/GI. Specular is a **sphere map**, not computed |
| **FFFFTT** | ✅ | ✅ | Uses `shz_fft` directly. 1024-pt, 22 kHz. `perf_cntr_timer_ns()` profiler |
| **Quake 2 DC**, **Monkey Ball DC**, **Meese** | ❌ gone/stub | claimed only | See 4d |
| **Gael Force**, **Audio Visualizer**, **DemoTek**, **Elias Daler** | ❌ | claimed | No published technical detail. Daler has **no** DC technical writing — the premise was wrong |

### 4c. Lighting, since we have no hardware T&L

Everyone's answer is the same and it is brutally simple: **a small fixed light
cap, chosen by distance, everything else baked.** Simulant's Dreamcast guide
states it outright — *"The Dreamcast renderer supports only 2 lights per
renderable"*, a compile-time hard limit; *"only the 2 closest lights affect each
object"*; compensate with lightmaps, vertex colours, and bright ambient. GLdc
caps at `MAX_GLDC_LIGHTS 4`. Nobody uses probes or per-frame amortisation.

GLdc's `GL/lighting.c` is the readable reference and every trick in it ports:
- **Per-light material products precomputed on state change**, not per vertex —
  the inner loop is three MACs per channel with no material math.
- **Three early-outs in cost order**, all before any normalise or dot:
  `MAX_LIGHT_RANGE 10.0f` distance cull → spot-cone `spotFactor <= 0` →
  `ATTENUATION_THRESHOLD 100.0f`.
- **Specular via a bit-trick `faster_pow`**, never `powf`; degenerates to a
  compare when shininess is 0.
- **Infinite viewer by default** — `computeViewVector()` returns literal
  `(0,0,1)` unless `GL_LIGHT_MODEL_LOCAL_VIEWER`, skipping a per-vertex negate
  and normalise.
- Normals packed to 8 bits/component, unpacked with one scale+offset.
- `PREFETCH(vertex + 1)` inside the per-vertex function.
- Lighting is a **separate pass** over an already-built vertex array, not fused
  into the transform — which is what makes G-D/§0a's FTRV lighting possible.

**Unexploited by everyone: PVR modifier volumes.** KOS's `dc/pvr.h` shows the
modifier vertex types carry *two complete parameter sets* — two copies of
colours, offset colours and texcoords, and the poly context has
`alpha`/`alpha2`, `fog_type`/`fog_type2`, `src`/`src2`, `dst`/`dst2`,
`txr`/`txr2` — outside vs inside the volume. A modifier volume is therefore a
per-pixel switch between two full shading configurations at **zero per-vertex
CPU cost**. `oargb` is the hardware's specular slot. [INFERRED] a point light
could be a modifier volume whose "inside" set is the lit material. **No project
in this survey published doing it.** Speculative, but it is the one genuinely
unexplored direction found.

### 4d. What could not be confirmed

- **Quake 2 DC** and **DMS-Engine**: canonical `Bruceleeto/*` URLs now 404.
  DMS history survives at `ianmicheal/DMS-Engine`. Quake 2 does not — the only
  datum is a community "consistently at 60fps" report.
- **Monkey Ball DC**: source not public; only a stripped ELF + asset toolchain
  (`Memorix101/mb-dc-packaging`). Strategically it is **not** a decomp-source
  port like ours — it abandoned building the GC executable and ported an
  extracted simulation library (`libmkb`). Its transferable pieces are the
  **TPL → PowerVR VQ** texture pipeline (`tools/vqenc.py`) and **MusyX → AICA
  ADPCM** conversion. It is the only project in the set using VQ.
- **Meese Engine**: repo is a README stub. But the question it was meant to
  answer is settled from sh4zam itself — **you do not wrap it.** `SHZ_BACKEND`
  auto-selects `SHZ_SH4` / `SHZ_SW` from `__DREAMCAST__`, and the SW backend
  fakes the register bank with a file-static matrix. Simulant independently
  confirms: sh4zam is compiled on every platform, `mat4.cpp` has **zero**
  `#ifdef __DREAMCAST__`.
- Cloudflare blocked all of `dcemulation.org` and `dreamcast-talk.com`,
  including threads named "Working with Lighting and Shadows on the DC" and
  "Bump Mapping sample" — [INFERRED] the richest untapped vein for the modifier-
  volume question. `x.com` returns 402 unauthenticated.

### 4e. Traps other people already paid for

- 🔴 **Never emit a poly header you might not follow with a vertex.** Xash3D:
  *"the TA can mis-parse the next header as a vertex, which manifests as gray
  screen + tiny quad and then no frames."* It runs an entire extra transform
  pass over a texture chain just to answer "will anything be emitted?"
- 🔴 **Unclipped `w<=0` hangs the TA — hard reboot.** QuakeSpasm `pvr_clip.c`.
  Our `DC_PVR_W_EPS` near clip is load-bearing, not a quality choice.
- 🔴 **`shz_sq_memcpy32_1_xmtrx` and `shz_fft` clobber XMTRX.** DMS pushes poly
  headers with it *before* loading the MVP. Get the order wrong and every vertex
  is transformed by a poly header. **74 in-memory `shz_mat4x4_*` routines also
  clobber XMTRX**, and `shz_xmtrx_apply_store_*` / `load_apply_store_*` leave
  XMTRX **garbage** despite the name — *"The result of the multiplication is not
  stored within XMTRX, despite it getting clobbered."* This matters to us
  because we hold `comb` in XMTRX across the whole vertex loop.
  ✅ **Confirmed safe** with a live matrix: all scalar, trig, vector and
  quaternion routines, and the non-`_xmtrx` memory routines (front bank only).
- ⚠️ **Two codebases independently reject FSRRA for the clip parameter.**
  Xash3D: *"near-plane clipping frequently involves negative denominators; using
  FSRRA-based reciprocal can introduce large errors and warp geometry."* Same
  conclusion we reached for the perspective divide (`dc/src/dc_pvr.c:176-181`).
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
  RAM on code size"* (<https://sh4zam.com/tips.html>). Nobody ships `-O0`.
  Vindicates the 2026-08-06 reversal.
- **Precise divide, not FSRRA, where precision decides geometry** — two
  codebases, same conclusion.
- **Per-batch XMTRX hold** is the right granularity — mk64's `gfx_sp_vertex`
  loads XMTRX once per `G_VTX` batch.
- **KOS preserves both FP banks across IRQs** — `irq_context` declares `fr[16]`
  *and* `frbank[16]`, and `entry.s` `frchg`s between them. Confirms the safety
  argument at `dc/src/dc_pvr.c:3038-3040`. [INFERRED] every IRQ pays that cost
  whether or not we use XMTRX, so holding a matrix resident adds nothing.
- **ARM7 is not a DSP target.** ~2.82 MHz, **no cache**. Community verdict:
  *"any amount of data transfer beyond minimal is pretty much out."* Consistent
  with `CLAUDE.md`'s existing rule.
- **jnmartin84's ports build KOS itself with `-O3 -flto`** and a hot-rodded
  `environ.sh`; Xash3D patches KOS to `-Os`. Either way, **KOS's own build flags
  are a lever nobody here has considered.** ⚠️ Costs one ~27 min SDK image
  rebuild (`kb/traps.md`).

---

## 5. Next actions, in order

✅ **1. G-A — DONE 2026-08-08.** §1 G-A carries the change and the verification.
✅ **2. Re-run `DC_EMU64_HIST` — DONE 2026-08-08.** §3 carries the numbers.
   ⚠️ Xash's PRFC1 profiler was **not** ported and is still the open instrument
   for the hardware-vs-Flycast question.

The remaining order, **revised by §3's re-cost**:

3. **G-B** — transform-once-per-unique-vertex, aimed at the **13.31 ms**
   unattributed block (43.8 % of draw). Promoted above G-F, which the re-cost
   demoted to 0.70 ms. Design before coding, and read §1 G-B's three
   mutation hazards: `set_position` is **not** a pure read of `vertices[]`
   (`emu64.c:2694-2708` flips `MTX_NONSHARED`, `:2712-2717` multiplies a normal
   with no idempotence latch, `:2724-2781` submits a round-tripped position).
   The distinct-vertex walk it needs **already exists** at
   `dc_emu64_cull.cpp:283-338` / `:369-390`.
4. **§0a + G-D** — the six FIPRs at `dc_pvr.c:3108-3118` against two FTRVs,
   inside `xform`'s 8.38 ms. Needs the pass split because XMTRX holds `comb`
   from `:3041` to `:3376`. ⚠️ A 32-vertex block must materialise `eye[32][3]`
   and `nrm[32][3]` — **768 B currently never stored at all** — plus
   `ClipVtx blk[32]`, `slot[32]` and a memo-hit bitmask: ~1796 B against today's
   112 B, into a 16 KB direct-mapped operand cache. And the loop indexes
   **primitives, not vertices** (`:3064`), so 32 vertices is 8 quads but 10⅔
   triangles. ⚠️ This overturns a "settled" record, and **matched screenshot
   frames are impossible** (no seed override exists; `shot_diff.py:42-45`), so
   it needs a same-build noise floor or a counting oracle instead.
5. **G-C** — the `dc_gx_backend_submit` rewrite. After 4; same code.
6. **G-F** — now a cheap 0.70 ms experiment, not a valuable one. If it is run,
   run the Gribb-Hartmann + positive-vertex form first and **add a bracket to
   `dc_emu64_cull.cpp` before costing anything** (§3).
7. **G-I** — read `kb/audio-cpu-cost.md` before writing anything.

⚠️ **None of this is a hardware verdict.** Every number above is Flycast, which
models **no instruction cache**, against 2,882,948 B of `.text` on an 8 KB
direct-mapped icache. §3 corollary 5 (Xash's PRFC1 profiler on a burn) remains
the only instrument that could answer why the console is slower.

**Screenshot rule applies to all of it**: `tools/dcqa/run_report.py` is the
floor and cannot see colour. Judge any renderer change on a matched screenshot
pair (`CLAUDE.md` §3).
