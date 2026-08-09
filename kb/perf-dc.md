# Where the town frame goes — the METHOD, and the one block still unattributed

> 🔴 **THIS FILE NO LONGER CARRIES LIVE NUMBERS.** It carries the instrument, the
> applied changes with their kill switches, and what is ruled out. Every absolute
> millisecond it used to state was measured at `-O0`, or halved by a per-tick
> denominator, or against a frame since re-cut by G3, the audio work and sessions
> 12-13. Those tables were deleted rather than re-stamped.
>
> **Live numbers: `kb/RESUME.md`** (sessions 12-13). **Evidence:
> `kb/state-log.md`.** **Headline / fit: `kb/STATE.md`.** **The ranked renderer
> queue: `kb/research-sh4zam-gap.md` §3.**

Companion to `kb/STATE.md` (FPS) and `kb/levers.md` (RAM, not time). **Read
`kb/closed.md` first** — it carries the `-O0` post-mortem.

Numbers here are **emulated guest time**, measured by the guest with
`timer_us_gettime64()` under Flycast. Guest milliseconds are **insensitive to host
load**, which is what makes them usable for "did this change make it worse". They
are the wrong instrument for an absolute hardware answer: **Flycast models no
instruction cache, no operand cache, no bus contention, no store-queue stalls and
no disc seek timing. Every figure it produces is a FLOOR.** Only
`dc/src/dc_pmcr.c` on a burn prices the real machine.

---

## 1. The instrument

| knob | emits | cost when on |
|---|---|---|
| `-DDC_PERF_PHASE` | `[PHASE]` next to `[PERF]`, every 30 presented frames: `draw= skip= vi= \| cull= xform= \| v= vsrc= vlit= vcull= us/v=` | 4 `dc_time_us()` per logic tick + 2 per batch |
| (always on) | `vmemo=hit/total` inside `[PHASE]` — the vertex memo's hit rate | two increments per vertex |
| `-DDC_PERF_GXAPI` | `[GXAPI] pos= clr= tc= nrm= begin= dirty= posms=` | 1 increment per GX vertex call + 2 `dc_time_us()` in `GXPosition3f32` |
| `-DDC_PERF_GXSPLIT=1` | **G4 — `[GXSPLIT]`**: `gxpos= gxgap= gxbegin= gxend= gxstate= \| ours= emu= \| posn= probe= drops=`. Brackets `GXPosition3f32`/`GXBegin`/`GXEnd`/the per-batch state setters with **raw TMU2** (`dc_emu64_hist.c:110`), charging the interval between one vertex's exit and the next one's entry to `gxgap` — emu64's index decode + `set_position` + the cheap setters. Each bracket subtracts the `dc_gx_flush_vertices` time inside it, so `cull`/`xform` are never double-counted. ⚠️ `gxgap` also absorbs non-GX work between two GX calls | 2 TMU2 reads/vertex ≈ 1.5 ms/frame, printed as `probe=` |
| `-DDC_PVR_VTXSPLIT=<N>` | **G5 — `[VTXSPLIT]`**: splits `[PHASE] xform=` into `memo / xf / lit / tex / shade / post / emit`, sampling 1 primitive in N | sampled |
| `DC_EMU64_HIST=<N>` | **G1 — `[EMU64H]`**, the per-opcode histogram (`dc/src/dc_emu64_hist.c`). Thunks swapped into emu64's dispatch table at runtime; `src/` untouched. **The only thing allowed to price an opcode** | sampled |
| `DC_PMCR=1` (+ `DC_PMCR_HUD=1`) | **P1 — SH7750 counters on PRFC1** (`dc/src/dc_pmcr.c`). ⚠️ **Flycast returns 0 for every event** — burn-only, and `DC_CONSOLE_MUTE=1` goes with it (`kb/traps.md`) | rotates 8 events, one per window |

**How the split is taken, so it can be re-derived.** `graph.c` runs the logic
`ticks_per_visual` times per presented frame and sets `g_pc_frameskip_active` on
all but the last; at `fps_target = 30` that is two ticks, one discarded.
`VIWaitForRetrace()` is called once per logic tick, at the end of it
(`src/graph.c:265-270` calls it directly on the skipped path), so **the interval
from one call's exit to the next call's entry is exactly one tick's game work**,
and the frameskip flag at entry says which kind of tick it was.
`dc/src/dc_vi.c` accumulates the two intervals separately; `dc/src/dc_gx.c`
brackets the AABB cull and `dc_gx_backend_submit()` inside the flush. Everything
is per **presented** frame over the same 30-frame window `[PERF]` uses, so the
parts sum to the frame time.

### The five rules this instrument has cost people

1. ⚠️ **`us/v` is the number to optimise against — not FPS, not `draw`.** It is
   transform time over the vertices that reached the transform, so it is invariant
   to scene size. **The town reseeds every boot** (`sys_math.c:7` seeds from
   `sqrand(osGetCount())`), so `draw` wanders in both directions across a change
   that is real on `us/v`.
2. ⚠️ **The `us/v` noise floor is ~±2 %** (rule 11, 2026-08-09). A change worth
   less than ~4 % cannot be resolved by one A/B pair. Run each arm 2-3 times or
   report it as inside the floor.
3. ⚠️ **`[EMU64H]` is per LOGIC TICK — double it** (rule 9). `[PHASE]`,
   `[GXSPLIT]` and `[VTXSPLIT]` are per presented frame — do not.
4. ⚠️ **`[VTXSPLIT]`'s buckets do not share a denominator** (rule 10). `memo` is
   per vertex; `emit` is per primitive; `xf`/`lit`/`tex`/`shade`/`post` sit
   downstream of the memo-hit `continue` and are charged on memo **MISSES** only.
5. ⚠️ **`[PHASE] vmemo=` is CUMULATIVE**, unlike every other field on that line —
   difference it between two windows.

---

## 2. The frame's shape, and the one live headline

The ordering that has survived every re-measurement: **display-list traversal
(emu64 + our GX layer) dominates; the SH-4 transform/light/submit block (`xform`)
is second; the AABB cull, VI and texture upload are noise.** Texture decode has
measured `tex=0.0ms` in every window of every run ever taken. All absolute shares
that used to live here are deleted; current figures are in `kb/RESUME.md`.

⭐ **STILL LIVE: `G_TRIN_INDEPEND` is the dominant opcode, and 13.31 ms of the
draw is `dl_G_TRIN`'s index expansion PLUS our own `GX*` attribute setters —
never separated from each other. It is the largest unattributed block in the
project and it is still open.**

- `G_TRIN_INDEPEND = 0x0A` (`include/libforest/gbi_extensions.h:52`), dispatch
  slot 60, forwarding to `dl_G_TRIN`. One command carries 1..128 faces —
  `emu64.c:4814` reads `n_faces = ((w0 >> 17) & 0x7F) + 1`, i.e. **7 bits**. Do
  not quote "5-bit N-triangle format"; the 5 bits are the per-vertex index width.
- **`GXEnd()` is live inside `dl_G_TRIN`** (`emu64.c:4935`), so everything
  `dc_gx_flush_vertices` triggers — the AABB cull and `dc_gx_backend_submit` — is
  billed to this opcode. A large fraction of the "emu64" bucket is code we own.
  `cull + xform` is the attributed part; the remainder is the 13.31 ms.
- ⚠️ **Session 13's vertex-index side channel did NOT touch this block.** It makes
  the vertex memo cheap; it removes no setter and expands no fewer indices. The
  **indexed-submit rewrite** that addresses the 13.31 ms is unstarted:
  `kb/research-sh4zam-gap.md` §1 G-B splits the two explicitly.
- ✅ Settled along the way: **`G_VTX` is NOT the budget**; **every state opcode is
  ≤ 0.55 ms** (`kb/closed.md` §F8); **`gap` is CLOSED** — it is emu64's own
  dispatch-loop overhead, slot `HIST_GAP = 64` (`dc_emu64_hist.c:87`), accumulated
  in `hist_enter()` (`:125`). ⚠️ `probe=` is **not** subtracted from `tot`/`gap`.

---

## 3. What was applied, and what it bought

All `dc/` code, all still in the tree, **each with a kill switch**. The mechanisms
are the durable content; the before/after milliseconds are removed because they
were taken against `-O0` neighbours and a `dc/src` since moved `-O2` → `-O3`.

⚠️ **§3.1 and §3.4 justify a hand optimisation by what the compiler was "failing
to do at `-O0`".** At `-O3` it does the strength reduction and scalar promotion
itself, so **any future optimisation of that shape needs a matched A/B, never
"the compiler will not do this".** The algorithmic mechanisms — the memo,
FSRRA/FIPR/FTRV, skipping unlit work — are things no optimizer can invent.

A/B is always against **counter-matched frames**: identical `draws`, `culled`,
`cmds` and `v` in both runs.

### 3.1 FTRV instead of a scalar 4x4 (`-DDC_PVR_NO_FTRV`)

Projection × modelview is folded into one 4x4 per batch (`mat_load`,
`dc/src/dc_pvr.c:3479`) and the matrix-vector product is one `mat_trans_nodiv()`
per vertex (`:3584`) instead of 16 multiplies and 12 adds. `mat_trans_nodiv` is
the no-perspective-divide form, which is what this backend needs — it keeps raw
clip-space `w` for the near clip.

The fold is written **transposed** (`:3455-3470`): `mat_load()` copies 16
consecutive floats into `XF0..XF15` in order and FTRV computes
`fr0' = XF0*x + XF4*y + XF8*z + XF12*w`, i.e. it reads that array column-major.
Transposing is one index swap in a per-batch loop.

**Why it is safe, checked not assumed:** XMTRX is a single global bank, so it
survives the vertex loop only if nothing else writes it — and nothing in the loop
can (`apply_texgen`, `shade_vertex`, `emit_projected`, `pvr_prim` are plain C or
integer asm, and `dc_mtx.c`'s `PSMTX*`, the only other XMTRX user in the image, is
unreachable from there). Interrupts and thread switches are safe because **KOS's
`kernel/entry.s` saves and restores BOTH FP banks** — read out of the SDK image.

### 3.2 One light loop per channel instead of four (`-DDC_PVR_NO_SHADEFAST`)

`chan_component()` evaluated **one colour component per call** and
`shade_vertex()` called it four times — so a lit vertex ran the whole light loop,
including its root, four times to produce four numbers differing in one
multiplicand. `chan_eval()` (`dc/src/dc_pvr.c:851`) inverts the nesting: one pass
per channel control. The alpha half is a separate control (`ctl = ci*2+1`) and
costs a second pass only when enabled.

⚠️ One deliberate FP difference: `(color*ndl)*atten` became `color*(ndl*atten)`.
Same to within an ulp of a float that is clamped and quantised to 8 bits, so it
cannot change a pixel — but it is not bit-identical, which is worth knowing before
blaming a one-LSB colour diff on something else.

### 3.3 Skip the eye position and the normal when nothing is lit (same switch)

The eye-space position and the normal transform were computed for **every**
vertex, and `shade_vertex()` reads them only inside the `chan_ctrl_enable[ctl]`
branch. `need_light` is computed once per batch (`dc/src/dc_pvr.c:3487`, tested at
`:3591`) and both blocks are skipped when false. `shade_vertex()` also
short-circuits to the raw vertex bytes when no light is enabled and both material
sources are `GX_SRC_VTX` — **exact**, not an approximation: `pack_argb` computes
`(int)(b/255*255 + 0.5)` and the round-trip error is ~1e-5 against the 0.5 that
would change the answer.

⚠️ The batch-constant predicates now live in `shade_batch_mode()`
(`dc/src/dc_pvr.c:1668`, kill `-DDC_PVR_NO_SHADE_HOIST`). **Kept because it
single-sources a predicate that was written out twice — not because it is fast:
it measured neutral, inside the ±2 % floor** (`kb/closed.md`).

### 3.4 The AABB cull loop (`-DDC_GX_NO_FAST_AABB`)

`dc_gx_batch_is_offscreen()` (`dc/src/dc_gx.c:604`) runs over every vertex of
every batch, including the ones it rejects — which is the point of it. The loop
was arithmetically fine but indexed; it now walks a pointer and holds the six
extrema in named scalars. Its clip half is split out as
`dc_gx_aabb_is_offscreen()` (`:571`) and shared with G3
(`dc/src/dc_emu64_cull.cpp`). `else if` between the min and max tests is safe:
after the seed both extrema are equal, and NaN takes neither branch in either
form — the cull stays conservative.

### 3.5 The vertex memo cache (`-DDC_PVR_NO_VTXMEMO`)

**This came from noticing what emu64's output actually is.** emu64 does not hand
the backend a mesh: its TRI run collapser opens **one** `GXBegin`, then calls
`set_position3(v0, v1, v2)` per triangle, where the `v` are **indices into emu64's
own vertex array** and `set_position3` re-emits `GXColor`/`GXNormal`/`GXTexCoord`/
`GXPosition` for each. A vertex shared by six triangles goes through the fold, the
normal matrix, the light loop and texgen **six times** for identical bytes.

Memoising on the source vertex is **exact**: every other input to the per-vertex
block is a per-BATCH constant (the folded matrix, `mv`, `nm`, the light state, the
TEV constants, `s_pt_route`, `tex->u_scale`). **128 slots** (`VMEMO_SLOTS`,
`dc/src/dc_pvr.c:2640`), direct-mapped, invalidated at the top of every
`dc_gx_backend_submit()` (`:3213`).

⚠️ **Sizing correction, banked:** the table was once 32 slots "because emu64's
cache is `Vtx vertices[32]`". Both halves were wrong — emu64's array is
`Vertex vertices[128]` (`emu64.hpp:33`, `VTX_COUNT`), and 32 bounds the WORKING
SET, not the COLLISIONS. Sizing a direct-mapped table from its working set instead
of its load factor cost about six points of hit rate.

Two later changes, both ON by default (`kb/RESUME.md` §12-13): **`vmemo_same()` is
branch-free** (`-DDC_PVR_NO_VMEMO_WORDCMP`), and the memo now keys on a
**vertex-index stamp** (`s_vmemo_vid`, `:2650`, kill `-DDC_GX_NO_VTXID`) rather
than hashing and comparing 30 bytes — deleting the random read into `verts[]` that
made the memo a cache miss rather than arithmetic.

### 3.6 FSRRA and FIPR (`-DDC_PVR_NO_FASTMATH`)

`$KOS_CFLAGS` carries `-mfsrra -mfsca` but **not**
`-funsafe-math-optimizations`, so **both are inert in this build**
(`kb/closed.md`). The shipped object settled it: `sh-elf-objdump -d dc_pvr.c.o`
had `fsqrt` followed by three `fdiv` — the light normalise, compiled literally,
both non-pipelined on SH-4. `frsqrt()` (one FSRRA, behind `DC_RSQRT`,
`dc/src/dc_pvr.c:201`) replaces it; `d` — read only by the spot attenuation
denominator — is recovered as `d2 * (1/d)` rather than by a second root. The
eye-space position and normal transform become `DC_DOT4`/`DC_DOT3` FIPRs
(`:3597-3608`).

⚠️ **`emit_projected()`'s `1.0f / c->w` is deliberately NOT converted**
(`:188-194`, `:2495`). That is the depth written to the TA, and z-fighting between
near-coplanar polygons is decided at a precision FSRRA's ~21 mantissa bits would
disturb. Two other Dreamcast codebases reached the same conclusion independently
(`kb/research-sh4zam-gap.md` §4e).

Also hoisted out of `chan_eval`'s light loop: the light mask, the diffuse function
and the spot test (`-fno-strict-aliasing` is on and `g_gx` is a global, so all
three were reloaded every iteration). The loop terminates at the last set bit,
since emu64 writes a dense `(1 << num_lights) - 1` (`emu64.c:3317`). ⚠️
**`GX_AF_SPOT` is unreachable in the whole tree, and the town runs exactly 2
lights, not 8** (`kb/RESUME.md` §12).

### 3.7 The GX entry-point micro-wins — MEASURED ZERO

A code-size census of `dc_gx.c` predicted 1.5-3 ms/frame from tightening the
per-vertex GX entry points. Four changes went in behind `-DDC_GX_NO_FASTPATH`:
`GXPosition3f32`'s COLOR0 save/restore deleted (a provable no-op);
`GXLoadNrmMtxImm`'s nine float compares + nine stores → three `memcmp(12)` /
`memcpy(12)`; the four kill-switch globals made `const`; `GXNormal3f32`'s six
clamps → one magnitude test.

**RESULT: nothing.** 215 counter-matched windows, `draw` and `gx` flat, FPS p50
unmoved. The first three are kept because they are strictly simpler code; the
sixth-clamp one was reverted as the only one that added complexity. **The lesson:
instruction-count estimates off the linked map did not survive contact with a
matched-frame A/B, and the A/B cost two builds and two 600 s runs — do the A/B
first next time.**

### 3.8 Later wins — kill switches for the record

| change | kill switch | where |
|---|---|---|
| G3 — AABB cull at `G_TRIN_INDEPEND` entry, ON | `DC_EMU64_CULL=0`; gate `-DDC_EMU64_CULL_VERIFY` | `dc/src/dc_emu64_cull.cpp`, `kb/RESUME.md` §0d |
| G-C — `pvr_dr_*` store-queue emit, ON | `-DDC_PVR_NO_DR` | `kb/RESUME.md` §12 |
| `DCGXVertex` `aligned(32)`, ON | `-DDC_GX_NO_VTXALIGN` | `dc/include/dc_gx_internal.h` |
| `lights[]` cache-line reorder, ON | `-DDC_GX_LIGHT_LAYOUT_LEGACY` | `dc/include/dc_gx_internal.h` |
| vertex-index side channel, ON | `-DDC_GX_NO_VTXID`; gate `-DDC_GX_VTXID_VERIFY`; decal-arm lift `-DDC_GX_VTXID_DECAL` (default OFF) | `kb/RESUME.md` §13 |

---

## 4. Ruled out, with evidence

Each was a plausible target before it was measured. Recorded so they are not
proposed again.

- **Texture upload/decode. Not a cost at all.** `tex=0.0ms` in every window of
  every run, cache warm and static in the town, `evictions=0`. Do not spend time
  in `dc_pvr_texture.c` for frame rate.
- **Strip/fan → triangle expansion. Does not happen in this game.** Measured with
  a `vsrc=` counter (vertices the game handed us) against `v=` (vertices the
  backend transformed): **`vsrc == v` exactly, in every window of every run.**
  emu64 emits `GX_TRIANGLES` and `GX_QUADS` only. **Native strip submission is
  worth zero here.**
- **The batch-merge path (`merged=0`). Dead, and not worth reviving.** It requires
  `g_gx.in_begin` at `GXBegin` time, but `dc_gx_flush_if_begin_complete()` has
  already cleared it, so the branch can never be taken. It would only save
  poly-header compiles, which `header_key()` already dedups.
- **The GX API surface in `dc_gx.c`. Small — and this needed a calibration to
  see.** `[GXAPI]`'s `posms=` reads as "`GXPosition3f32` is 9 ms". It is not: at a
  matched frame the census build is ~2.3 ms/frame slower than the same build
  without it, pricing `dc_time_us()` at **≈0.43 µs (~86 cycles) per read** — so
  the two bracketing reads account for almost all of what is reported.
  ⚠️ **Any future `dc_time_us()` on a path called thousands of times per frame
  must be calibrated the same way before its number is quoted.**
- **The frameskip tick is not the waste it looks like.** Two logic ticks run per
  presented frame and one is discarded, but the discarded one is an order of
  magnitude cheaper — `src/graph.c:265-270` skips the whole draw traversal on it.
  Forcing `ticks_per_visual = 1` buys it back at the cost of a quarter of the
  game's logic rate: a gameplay-speed trade the user owns, not an optimisation.
- **`pvr_dropped` has no speed mechanism.** `s_tris_dropped`
  (`dc/src/dc_pvr.c:134`, incremented at `:2787`, `:2800`, `:2819`) fires only on
  near-plane geometry, so it tracks where the camera is standing. Never read it as
  a regression.
- 🔴 **`-O0` — the cautionary example, and this file got it wrong.** A bullet here
  used to read *"`-O0` is not reopened; the town is interpreter-bound and that is
  a content/architecture problem, not a codegen one."* It was reopened on
  2026-08-06 and was the largest single result the project has had: `.text`
  −2.75 MB, town FPS 11.6 → 20.6, no `src/` edit. The bullet's own words identify
  the tell — "exactly the shape that invites it" — and it was not acted on because
  `kb/closed.md` was treated as settling a question it had only ever settled **on
  armhf evidence**, never on SH-4. **A "closed" entry whose evidence was gathered
  on a different architecture is a claim, not a verdict.**

---

## 5. What is left, ranked — POINTER ONLY

The old ranking was derived from an `-O0` frame and is deleted. **The live ranking
is `kb/research-sh4zam-gap.md` §3**; `kb/STATE.md` carries the top action. In one
line: **the 13.31 ms indexed-submit rewrite (§2) is the largest single item,
every stage inside `xform` is now under 2 ms, and every matrix/FTRV idea is aimed
at ~0.8 ms of a ~30 ms frame.**

---

## 6. Reproducing any of this

⚠️ **Pass `DC_OPT_PROFILE=` explicitly.** Default is `perf` (`-Os` + the `-O3` hot
list, `dc/opt-lists.mk`); `o0` is a byte-identical revert and the control for any
A/B against a pre-2026-08-06 number; `size` is flat `-Os`. **A full rebuild is 96
seconds.** See `BUILDING-DC.md`.

```bash
DC_STUB_KEEP="$(grep -v '^#' tools/dcstub/keeplist-town.txt | paste -sd: -)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=1048576 DC_ARENA_BYTES=1200000 \
DC_AUDIO_SCENES=all DC_AUDIO_DISC_FRAMES=8 DC_AUDIO_VOICES=12 \
DC_AUTOSTART=1 DC_XDEFS='-DDC_PERF_PHASE -DDC_PVR_VTXSPLIT=16' \
bash dc/build-dc.sh
cp dc/build/OpenCrossing.cdi /tmp/A.cdi
bash harness/dc/smoke.sh /tmp/A.cdi --timeout 600 -c config:LimitFPS=no

# one kill switch per applied change
DC_XDEFS='-DDC_PERF_PHASE -DDC_PVR_NO_FTRV'
DC_XDEFS='-DDC_PERF_PHASE -DDC_PVR_NO_SHADEFAST'
DC_XDEFS='-DDC_PERF_PHASE -DDC_GX_NO_FAST_AABB'
DC_XDEFS='-DDC_PERF_PHASE -DDC_PVR_NO_DR'
DC_XDEFS='-DDC_PERF_PHASE -DDC_GX_NO_VTXID'
```

Then **compare `us/v`, not `draw` and not FPS** (§1's five rules). Use a
counter-matched window (identical `draws`/`culled`/`cmds`/`v` in both logs) where
one exists; otherwise `us/v` is the only comparable figure, and a change under
~4 % needs repeated runs.

⚠️ **Build to a private object tree if anything else may be building.** Two
concurrent `make` runs in `dc/build` corrupt it; this work lost two builds to it
(one `ld` bus error, and a `flags.stamp` that kept reverting to another session's
`DEFINES`, silently producing an image whose `DC_MAIN_MEMORY_SIZE` did not match
its ledger and aborted at boot). The stub/shrink trees are content-idempotent and
can be shared:

```bash
make -C /work/dc -j4 BUILD=/work/dc/build-perf \
     STUBDIR=/work/dc/build/stubsrc SHRINKDIR=/work/dc/build/shrinksrc all
```

⚠️ **Never run a perf run with `DC_FB_PROBE`.** The framebuffer dump costs ~1.5 s,
smears over the following 30-frame window and lands in the `vi` bucket; it dragged
p1 from 11.56 FPS to 8.50 in one run. Screenshot runs and perf runs are different
experiments. ⚠️ And `shot_diff.py` cannot gate a change that alters the frame
rate — judge the SCENE, not the pixels.
