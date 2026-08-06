# Where the town frame actually goes — measured, 2026-08-02

> ## 🔴 [2026-08-06, session 6] G1 WAS RE-RUN — AND §2b's NUMBERS ARE HALF THEIR
> ## REAL VALUE, INDEPENDENTLY OF THE `-O0` PROBLEM BELOW
>
> ⚠️ **`[EMU64H]` reports per LOGIC TICK, not per presented frame.** G1 arms at
> the end of every tick (`dc_vi.c:405` frameskip, `dc_vi.c:633` presented) and
> `s_frames` counts ticks, so at `ticks_per_visual = 2` every §2b figure must be
> **doubled**. Proof: `tot 24.28 × 2 = 48.56` vs `draw 45.6 + skip 2.9 = 48.5`,
> and this document's own `42.86 × 2 = 85.7` vs `78.3 + 8.2 = 86.5`.
> **So §2b's `G_TRIN_INDEPEND` was 44.5 ms of an 86.5 ms frame — 51 % of the
> frame, not the 28 % the table states.** Measurement rule 9, `kb/traps.md`.
>
> Re-run `smoke-oc-dc-g1b-20260806-164033-15671`, town, probe-free, medians over
> 47 windows, ×2-corrected, against the `-O0` figures also ×2-corrected:
>
> | | `-O0` | **`-Os` + `-O3`** |
> |---|---:|---:|
> | `draw` | 78.3 | **45.6** |
> | `G_TRIN_INDEPEND` | 44.5 / 292 | **34.4 ms / 306 = 112.5 µs, 75 % of the frame** |
> | `G_VTX` | 10.8 | **1.84** |
> | `G_MTX` / `G_TEXRECT` | 4.36 / 4.34 | **1.72 / 2.88** |
> | `gap` | 15.8 | **5.96** |
> | `[EMU64H] tot` | 85.7 | **48.56** |
>
> `[PHASE] draw=45.6 skip=2.9 vi=0.4 | cull=2.0 xform=8.8 | v=2899 vlit=2689
> vcull=5250 us/v=3.06`, `cmds=3562`.
>
> **Three things this does to §2b:**
>
> 1. ✅ **`gap` is CLOSED, and §2b's "do not describe it as dispatch overhead"
>    is now answered: it IS dispatch overhead.** Slot `HIST_GAP = 64`
>    (`dc_emu64_hist.c:87`), accumulated in `hist_enter()` (`:124-131`) when
>    `s_prev == HIST_GAP` — `emu64_taskstart_r`'s loop control
>    (`emu64.c:5807-5824` prologue, `:5847-5855` dispatch guard, `:5874`
>    `gfx_p++`) plus frame prologue/epilogue. Confirmed by 15.8 → 5.96 ms when
>    that loop went `-O3`. ⚠️ `probe=` is **not** subtracted from `tot` or from
>    `gap` (`dc_emu64_hist.c:300` only prints it), and both probes land inside
>    `gap` by construction.
> 2. ⭐ **The 65/35 split §2b reading 2 records is superseded by a better
>    decomposition.** Of TRIN's 34.4 ms, `cull 2.0 + xform 8.8 = 10.8 ms` is
>    measured `dc/`. **The other ~23.6 ms — 52 % of the frame — is `dl_G_TRIN`'s
>    index expansion PLUS our own `GX*` attribute setters in `dc_gx.c`, and
>    those two have never been separated.** That is the largest unattributed
>    block in the project and the next measurement to take.
> 3. ⭐ **The cull rate section below stands, and the lever grew.** `vcull=5250`
>    against `v=2899` = **64 %**, and it is now known that those references are
>    fully expanded and pushed through the GX setters before rejection — so the
>    4.5-7.0 ms range in that table was costed against a halved frame and is
>    low.
>
> ✅ Also closed here: **`pvr_dropped` has no speed mechanism.** `s_tris_dropped`
> (`dc_pvr.c:134`) fires only on near-plane geometry (`:2149` all-behind,
> `:2162` straddle under `-DDC_PVR_NO_NEARCLIP`, `:2181` Sutherland-Hodgman
> emitting `< 3`), so it tracks camera position and nothing else.
>
> Evidence: `kb/state-log.md`, top entry, 2026-08-06 (session 6).

> ## ⚠️ [STALE 2026-08-06] EVERY FRAME NUMBER BELOW WAS MEASURED AT `-O0`.
>
> **The `-O0` directive was reversed on 2026-08-06.** `src/` builds at `-Os`
> with a 14-TU `-O3` hot list (`DC_OPT_PROFILE=perf`, the default); `dc/src`
> moved from `-O2` to `-O3`. Evidence: `kb/state-log.md`, top entry, 2026-08-06.
>
> **The line that used to stand here — "the `-O0` directive is settled, so
> nothing in this document is, or may become, a compiler flag" — is now FALSE,
> and it was the single most consequential wrong sentence in this file.** A
> compiler flag was the largest FPS win the project has ever taken:
>
> | matched town window | `-O0` | `-Os` | `-Os` + `-O3` hot | + `dc/src` `-O3` |
> |---|---:|---:|---:|---:|
> | town FPS | 11.6 | 18.5 | 20.0 | **20.6** |
> | `draw` ms | 79.1 | 50.3 | 46.8 | **45.4** |
> | logic tick ms | 6.6 | 3.3 | 2.8 | **2.8** |
> | `xform` ms (`dc/`) | 13.1 | 12.9 | 12.4 | **9.9** |
> | `us/v` | 4.05 | 4.05 | 4.05 | **3.11** |
> | whole-run FPS p50 | 24.5 | 29.8 | 29.8 | 28.7 |
>
> **What this invalidates, structurally:**
>
> - The §2 phase table and its shares are an `-O0` frame. The *ordering* of the
>   phases survives; the milliseconds do not, and neither do the percentages.
> - **The §2 conclusion "that is what `-O0` costs on a large interpreter switch,
>   and `src/` may not be edited, so it is a wall, not a lever" is void.** The
>   wall was the flag. `draw` fell 79.1 → 45.4 ms without one line of `src/`
>   changing.
> - The §2b per-opcode histogram (`TRIN_INDEPEND` 22.25 ms of a 78.3 ms frame)
>   is an `-O0` profile. **It has NOT been re-run at `-Os`+`-O3`, and the split
>   inside it almost certainly moved**: the `src/` half was optimised and the
>   `dc/` half was already `-O2`, so the *share* that is ours went UP.
>   Re-run `DC_EMU64_HIST` before costing anything against it.
> - §3's applied wins are still real — they are `dc/` code and the mechanisms
>   (FTRV, the memo cache, FSRRA) are unaffected — but their *before/after
>   milliseconds* were taken against `-O0` neighbours and are not the current
>   frame's arithmetic.
> - §4's "`-O0` is not reopened here" bullet is reversed; see §4.
>
> Nothing below has been re-measured. **Treat every millisecond in this document
> as a historical `-O0` figure until a run at `DC_OPT_PROFILE=perf` replaces
> it.** The re-measurement is cheap now: a full rebuild is 96 seconds and
> `DC_OPT_PROFILE=o0` is a byte-identical revert, so this whole document can be
> regenerated as a matched A/B.

Companion to `kb/STATE.md` (which reports FPS) and `kb/levers.md` (which is
about RAM, not time). **Read `kb/closed.md` first** — it now carries the `-O0`
post-mortem rather than the `-O0` directive.

Every number is **emulated guest time**, measured by the guest with
`timer_us_gettime64()` under Flycast. `harness/dc/perf.sh` explains why that is
the right instrument for "did this change make it worse" and the wrong one for
an absolute hardware answer: Flycast models no cache, no bus contention and no
store-queue stalls. Guest-measured milliseconds are **insensitive to host
load**, which matters here because another Flycast was running on the same
machine throughout — it changes how far a run gets in 600 s, not what a frame
costs.

---

## 1. The instrument

Two build-time knobs, both off by default, both in files this work owns.

| knob | emits | cost when on |
|---|---|---|
| `-DDC_PERF_PHASE` | `[PHASE]` next to `[PERF]`, every 30 presented frames | 4 `dc_time_us()` per logic tick + 2 per batch |
| (always on) | `vmemo=hit/total` inside `[PHASE]` — the vertex memo cache's hit rate, §3.5 | two increments per vertex |
| `-DDC_PERF_GXAPI` | `[GXAPI]`, same cadence | 1 increment per GX vertex call + 2 `dc_time_us()` in `GXPosition3f32` |

```
[PERF]  10.0 FPS | 33% speed | draws=96 ... culled=232 cmds=3765 gx=26.1ms tex=0.0ms
[PHASE] draw=91.4 skip=7.3 (n=30) vi=1.6 | cull=4.1 xform=21.8 | v=2869 vsrc=2869 vlit=2695 us/v=7.58
[GXAPI] pos=9485 clr=9485 tc=9485 nrm=9101 begin=325 dirty=1354 posms=9.15
```

**How the split is taken, so it can be re-derived.** `graph.c:401-429` runs the
game logic `ticks_per_visual` times per presented frame and sets
`g_pc_frameskip_active` on all but the last; at the default `fps_target = 30`
that is exactly two ticks, one discarded. `VIWaitForRetrace()` is called once
per logic tick, at the end of it (`graph.c:265-270` calls it directly on the
skipped path), so **the interval from one call's exit to the next call's entry
is exactly one tick's game work**, and the frameskip flag at entry says which
kind of tick it was. `dc_vi.c` accumulates those two intervals separately;
`dc_gx.c` brackets the AABB cull and `dc_gx_backend_submit()` inside the flush.
Everything is reported per **presented** frame over the same 30-frame window
`[PERF]` uses, so the parts sum to the frame time: `draw + skip + vi = 100.3 ms`
against 10.0 FPS.

⚠️ **`us/v` is the number to optimise against, not FPS.** It is transform time
divided by the vertices that reached the transform, so it is invariant to how
much geometry the scene happens to contain — which matters because two runs of
the same build reach different camera positions and `[PERF]` alone cannot be
compared across them.

---

## 2. The answer: the renderer is not the problem

⚠️ **[STALE 2026-08-06] This entire section is an `-O0` frame.** The shares are
what `src/` cost when `src/` was unoptimised. The qualitative claim — the
renderer is a minority of the frame — is the one part likely to have survived,
and it survived in the WRONG direction: `dc/`'s share went *up*, because `src/`
got 41 % cheaper while `xform` (already `-O2`) barely moved. Do not quote a
percentage from this table. Evidence: `kb/state-log.md`, 2026-08-06.

Town, steady state, baseline build (`~/.cache/oc-dc-harness/runs/perfA2`):

| phase | ms/frame | share | whose code |
|---|---:|---:|---|
| game logic (`game_main`, measured on the discarded tick) | 7.3 | 7 % | `src/` |
| **display-list traversal + emu64 + GX API** | **58.0** | **58 %** | `src/` (mostly) |
| SH-4 transform / light / TA submit (`dc_gx_backend_submit`) | 21.8 | 22 % | `dc/src/dc_pvr.c` |
| whole-batch AABB frustum cull | 4.1 | 4 % | `dc/src/dc_gx.c` |
| flush bookkeeping | 0.2 | 0 % | `dc/src/dc_gx.c` |
| VI: swap, pacing, probes | 1.6 | 2 % | `dc/src/dc_vi.c` |
| texture decode/upload | 0.0 | 0 % | — |
| **total** | **~100** | | 10.0 FPS |

Rows 3-5 are the only ones this work could touch. After §3 they are 21.8 → 15.6
and 4.1 → 2.2, i.e. **~8 ms off a 100 ms frame**; the other 65 ms did not move
and cannot be moved from `dc/`.

Derivation of row 2: the drawn tick is 91.4 ms and the discarded tick — which
runs `game_main` and skips `graph_draw_finish` + `graph_task_set00` entirely
(`src/graph.c:265-270`) — is 7.3 ms. So the whole draw traversal is
91.4 − 7.3 = 84.1 ms, of which `gx=` accounts for 26.1. **58.0 ms is emu64
walking the display list and the game building it.**

At `cmds=1561` in a matched, quieter frame the same subtraction gives 20.7 ms,
i.e. **≈13.3 µs — about 2,650 SH-4 cycles — per emu64 GBI command.** ~~That is
what `-O0` costs on a large interpreter switch, and `src/` may not be edited
(CLAUDE.md §1), so it is a wall, not a lever.~~

> 🔴 **[VOID 2026-08-06] — THE PREMISE WAS THE ERROR, NOT THE ARITHMETIC.**
> The sentence above is the clearest statement in the project of the belief that
> `-O0` was a floor. It was not a wall; it was a flag. Recompiling the same,
> unedited `src/` at `-Os` + a 14-TU `-O3` hot list took `draw` from **79.1 ms
> to 45.4 ms** and town FPS from **11.6 to 20.6** — no `src/` edit, no content
> cut, no renderer change. **13.3 µs per GBI command was an `-O0` figure and has
> not been re-measured.** What replaces the claim: emu64's dispatch cost is a
> *codegen-sensitive* quantity, it is now the thing the `-O3` hot list exists to
> attack (`dc/opt-lists.mk`), and the remaining wall — if there is one — has not
> been located yet. ⚠️ `-O3` on `emu64.c` is unproven anywhere in this port's
> history; `-O2` on it is device-verified on armhf. See `kb/state-log.md`,
> 2026-08-06.

### The consequence for expectations

⚠️ **[STALE 2026-08-06]** — the arithmetic below is against the `-O0` frame.

~~The renderer is 26 % of the town frame. **Deleting it entirely would take
10.0 FPS to 13.5.**~~ The *shape* of the argument still holds — a renderer
optimisation alone is not a route to 30 FPS in the town — but the renderer's
share is larger now than 26 %, because `src/` shrank around it. The current
`xform` is 9.9 ms (`dc/src` at `-O3`) against a 45.4 ms `draw`, and that ratio
is the one to reason from, not the one below. **Re-derive the split before
quoting it in any plan.**

---

## 2b. Per-OPCODE, at last — MEASURED 2026-08-05 (G1) — ⚠️ AT `-O0`

⚠️ **[STALE 2026-08-06] This histogram is an `-O0` profile and has NOT been
re-run.** Its findings divide into two kinds:

- **Structural findings that survive**, because they are about what emu64 *does*
  rather than what it costs: `G_TRIN_INDEPEND` is the heavy opcode; `G_VTX` is
  NOT the budget (reading 1, which killed a figure four documents were costed
  against); `GXEnd` is live inside `dl_G_TRIN`, so `dc/` work is charged to an
  emu64 opcode; 66.8 % of vertices are culled *after* emu64 pays for them.
- **Every millisecond and every share**, which do not survive. The `src/` half
  of each bucket was recompiled; the `dc/` half was already `-O2` and is now
  `-O3`. So the *proportion of `G_TRIN_INDEPEND` that is ours* went UP, and the
  "only ~53 µs is `src/` at `-O0`" split below is the number most likely to have
  moved. **Re-run `DC_EMU64_HIST=<N>` at `DC_OPT_PROFILE=perf` before costing
  any idea against this section** — that is now a 96-second rebuild away.

Evidence for the reversal: `kb/state-log.md`, top entry, 2026-08-06.

§2 answers "which phase", and for a year that was as far as the instrument went.
`DC_EMU64_HIST=<N>` answers "which opcode". Run
`smoke-G1-20260805-160640-92325`, town, probe-free, 11.5-12.1 FPS:

```
[PHASE]  draw=78.3 skip=8.2 (n=30) vi=0.4 | cull=2.2 xform=12.2 | v=3002 vsrc=3002 vlit=2517 vcull=6042 us/v=4.07
[EMU64]  cmds=3458 noop=1 vtx=298 tri=295 dl=276 | cullvis=7 cullrej=2
[EMU64H] tot=42.86ms gap=7.92ms probe=79.9ns | TRIN_INDEPEND 22.25/146 VTX 5.40/149 MTX 2.18/113
         MOVEMEM 0.55/207 TEXRECT 2.17/25 SETTILE_DOLPHIN 0.32/109 DL 0.29/138 SETCOMBINE 0.28/58
         SETTIMG 0.25/112 TRI2 0.19/2 ENDDL 0.18/131 LOADTLUT 0.13/42
```

| opcode | ms / calls | µs/call | share of the 78.3 ms frame |
|---|---|---:|---:|
| **`G_TRIN_INDEPEND`** | **22.25 / 146** | **152** | **28 %** |
| `G_VTX` | 5.40 / 149 | 36 | 7 % |
| `G_MTX` | 2.18 / 113 | 19 | 3 % |
| `G_TEXRECT` | 2.17 / 25 | 87 | 3 % |
| all other opcodes | ≤ 0.55 each | — | ~2 % total |
| **`gap`** | **7.92** | — | **10 % — OPEN** |

`G_TRIN_INDEPEND = 0x0A` (`include/libforest/gbi_extensions.h:52`), dispatch
table index 60, handler `emu64.c:4798` forwarding to `dl_G_TRIN`
(`emu64.c:4802-4940`). One command carries 1..128 faces —
`emu64.c:4814` is `n_faces = ((w0 >> 17) & 0x7F) + 1`, i.e. **7 bits**.

Three readings, in order of how much they change plans:

1. **`G_VTX` is NOT the budget.** Four documents costed G3 against "~48 ms of
   `G_VTX`". Measured: **5.40 ms**. That figure applied a whole-command average
   to a subset and counted `GXPosition3f32` *references* as loaded vertices.
   See `kb/traps.md`, "An average cost per command…", which now records that
   the correction to the averaging error was itself an averaging error.
2. **65 % of the heavy opcode is ALREADY ours, at `-O2`.** `GXEnd` is live at
   `emu64.c:4935`, so the 152 µs charged to `G_TRIN_INDEPEND` includes
   everything `dc_gx_flush_vertices` triggers: the per-batch AABB cull
   (~15 µs) and `dc_gx_backend_submit` (~81 µs). ~~**Only ~53 µs is `src/` at
   `-O0`.** So "reimplement the TRIN loop in `dc/` at `-O2`" is bounded above
   by **~8.3 ms** and realistically recovers **2-4 ms** [ESTIMATED].~~
   ⚠️ **[VOID 2026-08-06]** — the structural point (most of this bucket is
   already `dc/` code) survives and is now *more* true; the split does not.
   `src/` is no longer `-O0` and `dc/src` is no longer `-O2`, so both halves
   of the 65/35 moved in opposite directions. **The "reimplement the TRIN loop
   in `dc/` at `-O2`" idea has lost most of its remaining premise: the `src/`
   half it was going to replace is now compiled at `-O3` (`emu64.c` is on the
   hot list, `dc/opt-lists.mk`).** Re-measure before reviving it.
3. **`gap = 7.92 ms` is 18 % of `tot`** — time inside the draw phase but
   outside any emu64 command. Nothing in this run attributes it. **OPEN. Do not
   describe it as dispatch overhead; it has not been measured as anything.**

### The cull rate is a quantity now — and it is worth less than it looks

New counter `dc_gx_phase_culled_verts` (`dc/src/dc_gx.c`, incremented on the
reject path), printed as `vcull=`. **`vcull=6042` against `v=3002`: 66.8 % of
vertices are thrown away by `dc_gx_batch_is_offscreen()` after emu64 has paid
full `-O0` price.** The previous "60 %" was derived across two
differently-instrumented builds, and `pc_gx_culled_draws` counts BATCHES.

⚠️ **A cull is NOT worth 22.25 × 0.668 = 14.9 ms.** `dc_gx.c` returns *before*
`dc_gx_backend_submit`, so culled vertices already contribute **zero** to
`xform`. What an ideal cull at TRIN entry actually removes:

| term | ms | tag |
|---|---:|---|
| staging 6,042 vertices that are then rejected | 5.0-5.6 | ESTIMATED |
| the per-vertex AABB scan they cause (`cull=2.2`) | 2.1 | MEASURED |
| *minus* a new O(window) visibility test | −0.25…−0.40 | ESTIMATED |
| *minus* the irreducible per-call cost on culled calls | not separated | — |
| **defensible range** | **4.5-7.0, central ~6.0** | ESTIMATED |

⇒ **11.5 FPS becomes 12.2-12.6.** Quote the range and the reasoning, never the
14.9.

---

## 3. What was applied, and what it bought

⚠️ **[STALE 2026-08-06] The changes are still in the tree and still correct; the
before/after numbers are not current.** All of §3 was A/B'd against builds whose
`src/` was `-O0`, and `dc/src` has since gone `-O2` → `-O3`, which moved `xform`
13.1 → 9.9 ms and `us/v` 4.05 → 3.11 on its own. Two consequences worth stating
plainly:

- **Several sub-sections justify a hand optimisation by what the compiler was
  failing to do "at `-O0`"** (§3.1's stack round-trips, §3.4's recomputed
  `base + i*sizeof + c*4`). ⚠️ Those justifications are written as if the code
  in question were unoptimised. Whether that was ever true of `dc/src` —
  which these documents elsewhere describe as `-O2` in the same period — is
  **UNRESOLVED, and it is not resolved here.** At `-O3` the compiler now does
  the strength reduction and the scalar promotion by itself, so any *future*
  hand optimisation of this shape must be justified by a matched A/B, never by
  "the compiler will not do this".
- The mechanisms that are algorithmic — the memo cache (§3.5), FSRRA/FIPR/FTRV
  (§3.1, §3.6), skipping unlit work (§3.3) — are things no optimizer can
  invent, and they are unaffected.

Evidence for the reversal: `kb/state-log.md`, top entry, 2026-08-06.

Four changes: three are per-vertex work in `dc_gx_backend_submit()` /
`shade_vertex()` (`dc/src/dc_pvr.c`), one is the batch cull (`dc/src/dc_gx.c`).
A/B is against **counter-matched frames** — the same `draws`, `culled`, `cmds`
and `v` in both runs, found by scanning the console logs for identical counter
tuples, so the scene is provably the same geometry.

**Run A** = baseline + `-DDC_PERF_PHASE`. **Run B** = A + §3.1-3.3.
**Run C** = B + `-DDC_PERF_GXAPI`. **Run D** = C + §3.4.

Matched frame `draws=81 culled=2 cmds=1561 v=2437 vlit=1395` — identical
counters in all four runs, so provably the same geometry:

| | A (baseline) | B (+FTRV, +shade) | D (+AABB, has census) |
|---|---:|---:|---:|
| FPS | 22.8 | **25.5** | 24.3 |
| `gx=` whole flush | 17.1 ms | 12.5 ms | **12.0 ms** |
| `xform` transform+light+submit | 15.9 ms | 11.4 ms | 11.3 ms |
| `cull` AABB | 1.10 ms | 1.10 ms | **0.60 ms** |
| `us/v` | 6.53 | 4.66 | 4.63 |

D's FPS is *lower* than B's despite doing strictly less work: D carries
`-DDC_PERF_GXAPI`, which costs 2.3 ms/frame here and ~8 ms in the deep town.
Compare D against C (same census), not against B.

Isolates:

- **§3.1-3.3, A → B:** `xform` **15.9 → 11.4 ms (−28.3 %)**, `us/v`
  6.53 → 4.66, FPS **22.8 → 25.5 (+11.8 %)**. Two more matched frames agree:
  `draws=88 cmds=1652` 20.8 → 22.0, `draws=102 cmds=1694` 22.1 → 23.3.
- **§3.4, C → D:** `cull` **1.10 → 0.60 ms (−45 %)** with `xform` unchanged at
  11.3 — a clean isolate. Per batch, 13.25 → 7.23 µs; town median 10.43 → 6.00.

Town-wide medians over every window with `v > 2000` (199 A windows, 219 D):
`us/v` **6.42 → 4.61 µs (−28.2 %)**, `gx=` 19.0 → 13.8 ms, `cull` 1.40 → 0.80,
FPS 19.9 → 20.7 (D still paying the census).

Deepest town windows (`cmds > 3000`, where the 10.3 FPS report came from):
`us/v` **7.61 → 5.99 (−21 %)**, `cull` 4.00 → 1.90 ms, and with the census off
(run B + the §3.4 delta) FPS ≈ **9.6 → 11.0**. ⚠️ Those windows are not
counter-matched between runs — `v` is 3817 in A and 3165-3301 later — so treat
the FPS there as indicative and the per-vertex figures as the measurement.

A second matched pair at the opening scene (`draws=38 cull=0 cmds=896 v=889`)
gives `xform` 6.8 → 5.4 ms and `us/v` 7.69 → 6.09; FPS is unchanged there at
28.8 because that scene is frame-limited, which is a useful control — the change
moves work, not the clock.

### 3.1 FTRV instead of a scalar 4x4 (`-DDC_PVR_NO_FTRV`)

`dc_pvr.c` folded projection × modelview into one 4x4 per batch and then ran the
matrix-vector product in C **per vertex**: 16 multiplies, 12 adds and, at `-O0`,
a stack round-trip for every intermediate. SH-4 has `FTRV`, which does the whole
thing in one instruction against the `XMTRX` bank, and KOS exposes it as
`mat_load()` + `mat_trans_nodiv()` (`dc/matrix.h`). The matrix is loaded once per
batch; `mat_trans_nodiv` is the no-perspective-divide form, which is what this
backend needs because it keeps raw clip-space `w` for the near-plane clip.

The fold is written **transposed**: `mat_load()` copies 16 consecutive floats
into `XF0..XF15` in order (KOS `matrix.s`, eight `fmov @r4+, xdN`) and FTRV
computes `fr0' = XF0*x + XF4*y + XF8*z + XF12*w`, i.e. it reads that array
column-major. Transposing costs one index swap in a 16-iteration per-batch loop.

**Why this is safe, checked rather than assumed:** `XMTRX` is a single global
register bank, so it survives the vertex loop only if nothing else writes it.
Nothing in the loop can — `apply_texgen`, `shade_vertex`, `emit_triangle` and
`pvr_prim` are plain C or integer asm, and `dc_mtx.c`'s `PSMTX*` (the only other
`XMTRX` user in the image) is not reachable from there. Interrupts and thread
switches are safe because **KOS's `kernel/entry.s` saves and restores BOTH
floating-point banks** — the `frchg` pair around eight `fmov drN,@-r0` — which
was read out of the SDK image, not inferred from `irq_context_t` having an
`frbank[16]` field.

### 3.2 One light loop per channel instead of four (`-DDC_PVR_NO_SHADEFAST`)

`chan_component()` evaluated **one colour component per call** and
`shade_vertex()` called it four times. Everything inside its 8-light loop — the
light vector, its length, the `sqrtf`, the normalise, N·L and the spot
attenuation — is component-independent; only `lights[li].color[comp]` is not.
So a lit vertex ran that loop, and its `sqrtf` per light, **four times** to
produce four numbers differing in one multiplicand.

`chan_eval()` inverts the nesting: one pass over the lights for a whole channel
control. RGB now costs one loop; the alpha half is a separate control
(`ctl = ci*2+1`) and costs a second only when enabled.

This is the dominant term of the three. **`vlit` is 94 % of vertices in the
town** (2695 of 2869) — the lighting path is not a corner case, it is the
common case.

One deliberate FP difference: the old inner term was `color*ndl*atten`, i.e.
`(color*ndl)*atten`; the new one is `color*(ndl*atten)`. Same to within an ulp
of a float that is then clamped to [0,1] and quantised to 8 bits, so it cannot
change a pixel — but it is not bit-identical, and that is worth knowing before
blaming a one-LSB colour diff on something else.

### 3.3 Skip the eye position and the normal when nothing is lit (same switch)

The eye-space position (a 3x4 multiply) and the normal (a 3x3 multiply, a dot
product, a `sqrtf` and a divide) were computed for **every** vertex, and
`shade_vertex()` reads them only from inside the `chan_ctrl_enable[ctl]` branch.
`need_light` is now computed once per batch and both blocks are skipped when it
is false. `shade_vertex()` additionally short-circuits to the raw vertex bytes
when no light is enabled and both material sources are `GX_SRC_VTX` — which is
**exact**, not an approximation: `pack_argb` computes `(int)(b/255*255 + 0.5)`
and the round-trip error is ~1e-5 against the 0.5 that would change the answer.

The predicate is written out in both places on purpose. A mismatch would mean
reading uninitialised `eye[]`/`nrm[]`, which is a much worse failure than a
wrong colour; the comment at each site says so.

In the town this fires on only 6 % of vertices, but in the train/dialogue
scenes `vlit` is 57 % of `v`, and there it is worth about a third of the win.

### 3.5 THE VERTEX MEMO CACHE (`-DDC_PVR_NO_VTXMEMO`) — 2026-08-04

**This is the largest single win found so far, and it comes from noticing what
emu64's output actually is.**

emu64 does not hand the backend a mesh. `emu64.c:5100-5150` — the G_TRI1/G_TRI2
run collapser, whose own comment says it dominates this game's display lists —
walks a run of triangle commands, counts `n_verts` (3 per TRI1, 6 per TRI2),
opens **one** `GXBegin(GX_TRIANGLES, n_verts)`, and then calls
`set_position3(v0, v1, v2)` per triangle. Those `v` are **indices into emu64's
own 32-entry vertex cache**, and `set_position3` re-emits `GXColor`,
`GXNormal`, `GXTexCoord` and `GXPosition` for each one. A vertex shared by six
triangles therefore goes through the fold, the normal matrix, the eight-light
loop and texgen **six times** and produces the same 28 bytes each time.

Memoising on the source vertex is **exact**, not an approximation: every other
input to the per-vertex block is a per-BATCH constant (the folded matrix, `mv`,
`nm`, the light state, the TEV constants, `s_pt_route`, `tex->u_scale`).
Nothing in the `k` loop reads anything that varies per vertex except `*v`.

**128 slots** (`dc_pvr.c:1955`), direct-mapped, invalidated at the top of every
`dc_gx_backend_submit()`.

⚠️ **CORRECTED 2026-08-05 — this paragraph used to read "32 entries … 32 because
emu64's cache is `Vtx vertices[32]`, so one batch can never reference more
distinct sources than that", with a 42.5 % hit rate.** Both halves were wrong.
32 bounds the WORKING SET and says nothing about COLLISIONS, and sizing a
direct-mapped table from its working set instead of from its load factor cost
about six points of hit rate. The table has been 128 slots since 2026-08-04.

**MEASURED HIT RATE: 48.2-48.9 %** (the 42.5 % above was the 32-slot table).
`vmemo=hit/total` is in the `[PHASE]` line.

⚠️ **OPEN: whether that is at the ceiling.** Two derivations disagree, entirely
on one input — total staged vertex references per frame. Using the older 6,951
(the figure in `dc_gx.c:870`'s comment, now **stale**) against ~3,601 loaded
vertices gives 1.93 refs/vertex and a **48.2 % ceiling**, i.e. the memo is done.
Using the newly measured `v + vcull` = 3,002 + 6,042 = **9,044** gives 2.51
refs/vertex and a **~60 % ceiling**, i.e. ~11 points of headroom. The 9,044
rests on the `vcull` counter added 2026-08-05 and is measured; the 6,951 is
older. **Do not record either as fact.** The experiment that settles it is a
direct count of distinct vertex references per batch.

⚠️ The stored key is the source vertex's **index**, not a copy, and the
comparison is field-by-field. `DCGXVertex` is 30 live bytes in a 32-byte
`aligned(8)` shell and **nothing ever writes the two tail padding bytes**, so a
plain 32-byte `memcmp` would compare uninitialised memory — turning legitimate
hits into misses, unstably.

### 3.6 FSRRA and FIPR (`-DDC_PVR_NO_FASTMATH`) — 2026-08-04

`$KOS_CFLAGS` carries `-mfsrra -mfsca` but **not**
`-funsafe-math-optimizations` (nor `-ffinite-math-only`, which `fsrra`
additionally requires), so **both flags are inert in this build** — closed
2026-08-05, `kb/closed.md`. The shipped object settles what GCC did with them: `sh-elf-objdump -d dc_pvr.c.o` has `fsqrt` at +0x178 followed by three
`fdiv` at +0x180/182/186 — the light normalise, compiled literally. Both are
non-pipelined on SH-4, so that is ~50 cycles of latency per light per lit
vertex, and `vlit` is 94 % of town vertices.

`frsqrt()` (KOS `dc/fmath.h`, one `FSRRA`) replaces it at ~3 cycles. `d` — read
only by the spot attenuation denominator — is recovered as `d2 * (1/d)` rather
than by a second root. The eye-space position and the normal transform become
six `FIPR`s instead of 21 multiplies and 15 adds.

⚠️ **`emit_projected()`'s `1.0f / c->w` is deliberately NOT converted.** That
is the depth value written to the TA, and z-fighting between near-coplanar
polygons is decided at a precision FSRRA's ~21 mantissa bits would disturb.
Everything that was converted ends as an 8-bit colour byte or a normalised
direction.

Also hoisted out of `chan_eval`'s light loop: the light mask, the diffuse
function and the spot test. `-fno-strict-aliasing` is on and `g_gx` is a
global, so the compiler had to reload all three on each of eight iterations.
The loop now terminates at the last set bit, since emu64 always writes a dense
`(1 << num_lights) - 1` (`emu64.c:3317`).

### Combined result of 3.5 + 3.6

Two 600 s runs at the same commit, same keep list, same build line, differing
only by the patch:

| | before | after |
|---|---:|---:|
| `us/v` p50 | 4.71 µs | **3.92 µs** (−17 %) |
| `us/v` p90 | 6.03 µs | **4.45 µs** (−26 %) |
| `xform` p50 | 12.6 ms | **10.3 ms** |
| `xform` p90 | 16.4 ms | **11.9 ms** |
| FPS p50 | 22.6 | **23.6** |

`tools/dcqa/run_report.py --vs`: no regression. Screenshot pairs at probe
indices 0-18 are pixel-identical or within FSRRA's last-bit colour noise;
19-25 differ only because the faster build is at a different point in the same
scene, which is rule 3 in `kb/RESUME.md` doing its job.

### 3.4 The AABB cull loop (`-DDC_GX_NO_FAST_AABB`, `dc_gx.c`)

`dc_gx_batch_is_offscreen()` runs over every vertex of every batch — including
the 71 % it then rejects, which is the point of it — and cost 4.1 ms/frame in the deep town.
The loop was arithmetically fine but indexed: at `-O0` the `[c]` inner loop
carries its own counter and `v[i].position[c]` recomputes
`base + i*sizeof(DCGXVertex) + c*4` with a multiply on each of the three
components. It now walks a pointer and holds the six extrema in named scalars.

`else if` between the min and max tests is safe: after the seed both extrema are
equal, so a value below the running minimum is necessarily below the running
maximum, and NaN takes neither branch in either form — which keeps the cull
conservative, exactly as before.

---

### 3.7 The GX entry-point micro-wins — MEASURED ZERO, kept only where simpler

2026-08-04. A code-size census of `dc_gx.c` (from `.text.<fn>` sizes in the
linked map) predicted 1.5-3 ms/frame from tightening the per-vertex GX entry
points. Four were applied behind one define, `-DDC_GX_NO_FASTPATH`:

- `GXPosition3f32`'s COLOR0 save/restore deleted — it was a **provable no-op**
  (`dc_gx_commit_vertex()` only ever reads `g_gx.current_vertex`), 6,951 times a
  frame.
- `GXLoadNrmMtxImm`'s nine float compares + nine stores → three `memcmp(12)` /
  `memcpy(12)`. Same semantics; the scalar form compiled to 552 B against 148 B
  for `GXLoadPosMtxImm` doing the same job on *more* data.
- The four kill-switch globals made `const`, so ~35 setters fold the branch
  instead of reloading a global under `-fno-strict-aliasing`.
- `GXNormal3f32`'s six clamps → one magnitude test. **Reverted.**

**RESULT: nothing. 215 counter-matched windows, mean `draw` 31.93 → 32.07 ms
(+0.4 %), mean `gx` 9.49 → 9.51, FPS p50 23.9 → 23.9, p1 10.8 → 10.6.** The
prediction was wrong — either the code-size model over-prices these paths, or
the win is below what run-to-run variation can resolve at this frame time.

The first three are kept because they are strictly simpler code; the sixth-clamp
one was reverted because it was the only one that added complexity. **The lesson
worth keeping: instruction-count estimates off the linked map did not survive
contact with a matched-frame A/B here, and the A/B cost two builds and two
600 s runs — do the A/B first next time.**

## 4. Ruled out, with evidence

Each of these was a plausible target before it was measured. They are recorded
so they are not proposed again.

- **Texture upload/decode. Not a cost at all.** `tex=0.0ms` in every window of
  every run, with `uploads=306 hits=894442 evictions=0` — the cache is warm and
  static in the town. Do not spend time in `dc_pvr_texture.c` for frame rate.

- **Strip/fan → triangle expansion. Does not happen in this game.** The natural
  suspicion was that `GXBegin()` rewriting `GX_TRIANGLESTRIP`/`GX_TRIANGLEFAN`
  into independent triangles inflates an N-vertex strip into 3(N−2) transformed
  vertices, which would make native PVR strips a 2-3x lever. Measured with a
  `vsrc=` counter (vertices the game handed us) against `v=` (vertices the
  backend transformed): **`vsrc == v` exactly, in every window of every run.**
  emu64 emits `GX_TRIANGLES` and `GX_QUADS` only. `s=0 f=0` in `[PERF]` said the
  same thing and was misread as "conversion already happened" rather than
  "there is nothing to convert". **Native strip submission is worth zero here.**

- **The batch-merge path (`merged=0`). Dead, and not worth reviving.** It
  requires `g_gx.in_begin` at `GXBegin` time, but `dc_gx_flush_if_begin_complete()`
  has already cleared `in_begin` by then, so the branch can never be taken. It
  would only save poly-header compiles, and those are already deduped by
  `header_key()` — 96 draws/frame against 2,869 vertices is not where the time
  is.

- **The GX API surface in `dc_gx.c`. Small — and this needed a calibration to
  see.** `[GXAPI]` reports `pos=9485 posms=9.15` in the town, which reads as
  "`GXPosition3f32` is 9 ms". It is not. At a **matched frame** the census build
  is 2.3 ms/frame slower than the same build without it (`dr` 35.5 vs 33.2 at
  `pos=2521`), which prices `dc_time_us()` at **≈0.43 µs (~86 cycles) per read**
  — so the two reads bracketing each call account for 2.17 ms of the 2.44 ms
  reported there. **True `GXPosition3f32` cost is ~0.3 ms/frame at that window
  and ~1 ms in the town.** ⚠️ Any future use of `dc_time_us()` on a path called
  thousands of times per frame must be calibrated the same way before its number
  is quoted.

- **The frameskip tick is not the waste it looks like.** Two logic ticks run per
  presented frame and one is discarded, but the discarded one costs **7.3 ms
  against the drawn tick's 91.4** — `graph.c` skips the whole draw traversal on
  it. Forcing `ticks_per_visual = 1` (i.e. `fps_target = 60`) would buy ~7 ms of
  frame time at the cost of a quarter of the game's logic rate. It is a
  gameplay-speed trade the user owns, not an optimisation, and it is small.

- 🔴 ~~**`-O0` is not reopened here.** The 58 ms of emu64 is exactly the shape
  that invites it. `kb/closed.md` settles it; the honest statement is that the
  town is interpreter-bound at `-O0` and that this is a content/architecture
  problem, not a codegen one.~~
  **[REVERSED 2026-08-06 — this bullet was wrong, and it belongs at the top of
  the "ruled out" list as the cautionary example.]** It was reopened, on
  2026-08-06, on the KOS/sh4zam maintainer's advice, and it was the largest
  single result the project has had: `draw` 79.1 → 45.4 ms, town FPS
  11.6 → 20.6, `.text` −2.75 MB. This bullet's own words identify the tell —
  "exactly the shape that invites it" — and the reason it was not acted on was
  that `kb/closed.md` was treated as settling a question it had only ever
  settled *on armhf evidence*, never on SH-4. The town was not
  "interpreter-bound at `-O0`"; it was **`-O0`-bound**, and it was a codegen
  problem after all. See `kb/closed.md`'s `-O0` post-mortem and
  `kb/state-log.md`, 2026-08-06. **The general lesson: a "closed" entry whose
  evidence was gathered on a different architecture is a claim, not a
  verdict.**

---

## 5. What is left, ranked

> ⚠️ **[STALE 2026-08-06] This ranking was built on an `-O0` frame, and the
> item that used to be #0 — "recompile `src/`" — was not on it at all, because
> the whole list assumed codegen was banned.** That item has since been taken
> and is the largest win the project has recorded (`draw` 79.1 → 45.4 ms, town
> 11.6 → 20.6 FPS). **The ranking below has not been re-derived against the
> current frame.** Re-run `[PHASE]` and `DC_EMU64_HIST` at
> `DC_OPT_PROFILE=perf` before acting on the order. Evidence:
> `kb/state-log.md`, 2026-08-06.
>
> **The new #0, and it is free:** the `-O3` hot list is 14 TUs and cost
> +48,476 B of `.text` for 3.5 ms. `dc/opt-lists.mk` is the file, a rebuild is
> 96 s, and `DC_OPT_PROFILE=o0` is a byte-identical control — so "which TU
> belongs on the hot list" is now a cheap, repeatable experiment rather than a
> forbidden one. It is bounded, though: the frameskipped tick runs ALL of
> `game_main` and costs 2.8 ms against the drawn tick's 46.8, so every `src/`
> TU *other than* `emu64.c` is sharing those 2.8 ms and adding it to the hot
> list buys ~nothing while costing `.text` (= heap, `kb/closed.md`).

1. ~~**The emu64 + draw traversal. `src/`, and therefore closed to editing**~~
   **`src/` IS NOT CLOSED TO OPTIMISING — only to EDITING.** That distinction
   did not exist when this item was written and it is the whole point of the
   2026-08-06 result. CLAUDE.md §1 still forbids editing `src/`; it no longer
   forbids compiling it well, and `emu64.c` is on the `-O3` hot list.
   §2b says *which part* of the traversal is heavy, and it is smaller than this
   item assumed. ⚠️ Its numbers are `-O0`: `G_TRIN_INDEPEND` was 22.25 ms of a
   78.3 ms frame, of which **65 % was already `dc/` code at `-O2`** (`GXEnd` →
   the cull + submit); the "only ~53 µs of its 152 µs/call is `src/`" split is
   void, since both halves were recompiled. The levers are now: **tune the
   `-O3` hot list** (new, cheap, measurable), make emu64 execute *fewer*
   commands (scene/content work, `kb/levers.md` L5, the user's call), cull
   *before* the staging cost (4.5-7.0 ms [ESTIMATED at `-O0`], §2b), or speed
   up the `dc/` code inside the bucket — item 1b. **Stop quoting "58 ms of
   emu64" as one indivisible wall; it is now neither 58 ms nor indivisible nor
   a wall.**

1b. **`dc_gx_backend_submit` — ⚠️ 12.2 ms is an `-O0`-era figure — and it is
   ours.** `dc/src/dc_pvr.c:2448`. `dc/src` moved to `-O3` on 2026-08-06, which
   took `xform` 12.4 → 9.9 ms and `us/v` 4.05 → 3.11 by recompilation alone, so
   this block is **already ~20 % cheaper than the number above** and its share
   of the draw phase has moved in both directions at once (it got cheaper; the
   frame got much cheaper). It needs no trampoline, no
   `objcopy --globalize-symbol` and no sign-off, which is still more than can be
   said for anything in `kb/research-fps-ideas.md`'s G-series. §3.5/§3.6 already
   took ~28 % out of it; items 3, 4 and 5 below are the remaining named ideas,
   and each of them is now competing against an `-O3` compiler that may already
   be doing part of the job — **A/B each one, do not assume.**

2. ✅ **DONE 2026-08-04 — §3.5 and §3.6.** `us/v` is now 3.92 µs p50, i.e.
   ~784 cycles, down from 4.71. The two levers that paid were **not** the ones
   listed here: the big one was that **48.2-48.9 %** of the vertices reaching
   this code were duplicates of one already computed in the same batch (42.5 %
   at the original 32-slot table), and the second was that `sqrtf` + `fdiv` had
   been compiled literally where the SH-4 has FSRRA.
   What is left inside the per-vertex cost, still unmeasured: `apply_texgen`,
   the near-clip test, the `pvr_prim` copy (item 3 below), and the ~51 % of
   vertices that still miss the memo. ⚠️ **Raising the memo's hit rate is no
   longer safe to call "the cheapest remaining idea" — whether there is ANY
   headroom left is an open question as of 2026-08-05** (§3.5: the ceiling is
   either 48.2 % or ~60 % depending on a reference count nobody has taken).

3. **`pvr_dr_target()` / `pvr_dr_commit()` instead of `pvr_prim()`.**
   `emit_projected()` builds a 32-byte `pvr_vertex_t` on the stack and
   `pvr_prim()` then copies it through `sq_fast_cpy` (KOS `pvr_scene.c:259`).
   Direct Rendering writes the vertex straight into the store queue, removing
   one 32-byte copy and one call per vertex — perhaps 0.5-1.5 ms/frame.
   ✅ **UNBLOCKED 2026-08-05 — REOPENED.** This item used to say "not attempted
   deliberately: KOS 2.3 exposes `pvr_dr_addr` as a bare global with no
   `pvr_dr_init()` in the header, so who sets `QACR` for the TA is unresolved."
   Read out of the pinned KOS tree, which is what that note asked for:
   **`pvr_list_begin()` calls `sq_lock((void*)PVR_TA_INPUT)`, and that is what
   sets QACR0/QACR1**; `pvr_list_finish()` calls `sq_unlock()`.
   `pvr_dr_init`/`pvr_dr_finish` survive only as **deprecated no-ops** in
   `pvr_legacy.h`, which is why the header looked incomplete. **DR is safe
   inside the `pvr_list_begin`/`pvr_list_finish` bracket the code already
   has.** ⚠️ `pvr_dr_addr` is master-only — see `kb/traps.md`, "KOS 2.3 is not a
   release".

4. **Direct-slot vertex staging in `dc_gx.c`.** `GXPosition3f32` fills
   `g_gx.current_vertex` and `dc_gx_commit_vertex()` then copies 32 bytes into
   the staging buffer. Writing attributes straight into the buffer slot and
   letting commit be an `idx++` removes that copy. Bounded by the calibration in
   §4: the whole vertex API is ~1-3 ms/frame, so this is worth **at most ~1 ms**
   and it has to interact correctly with strip conversion and the mid-batch
   split. **Low priority; the number does not justify the risk.**

5. **`GXNormal3f32` for unlit batches.** emu64 calls it 9,101 times/frame
   (`src/static/libforest/emu64/emu64.c:2787`) and it does three multiplies, six
   clamps and three float→short conversions to fill a field that
   `dc_gx_backend_submit()` now discards when `need_light` is false. Legal to
   skip because §3.6 behaviour 3 guarantees GX state cannot change mid-batch, so
   `chan_ctrl_enable[]` is already this batch's state. Worth ~0.5 ms in the town
   (6 % of vertices unlit) and more in dialogue scenes (43 %).

---

## 6. Reproducing any of this

⚠️ **[2026-08-06] Add `DC_OPT_PROFILE=` to every command below.** The build line
here predates the optimization profiles and therefore silently reproduces
whatever the current default is (`perf`), not the `-O0` builds these numbers
came from. `DC_OPT_PROFILE=o0` is a byte-identical revert to the `-O0` tree and
is the control for any A/B against this document; `size` is flat `-Os`. A full
rebuild is 96 seconds, so re-measuring a section here is cheaper than arguing
about it. See `kb/state-log.md`, 2026-08-06, and `BUILDING-DC.md`.

```bash
# baseline + instrumentation
DC_STUB_KEEP="$(grep -v '^#' tools/dcstub/keeplist-opening.txt | paste -sd: -)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=131072 DC_ARENA_BYTES=1900000 DC_AUTOSTART=300 \
DC_XDEFS='-DDC_PERF_PHASE' bash dc/build-dc.sh
cp dc/build/OpenCrossing.cdi /tmp/A.cdi
bash harness/dc/smoke.sh /tmp/A.cdi --timeout 600 -c config:LimitFPS=no

# the kill switches, one per applied change
DC_XDEFS='-DDC_PERF_PHASE -DDC_PVR_NO_FTRV'
DC_XDEFS='-DDC_PERF_PHASE -DDC_PVR_NO_SHADEFAST'
DC_XDEFS='-DDC_PERF_PHASE -DDC_GX_NO_FAST_AABB'
```

Then match frames rather than eyeballing the tail: scan both console logs for
`[PERF]`/`[PHASE]` pairs and compare only windows with **identical**
`draws`/`culled`/`cmds`/`v`. Two runs of the same build end up at different
camera positions, so an unmatched tail comparison is worthless — and `us/v` is
comparable even when no matched window exists.

⚠️ **Build to a private object tree if anything else may be building.** Two
concurrent `make` runs in `dc/build` corrupt it; this work lost two builds to it
(one `ld` bus error, and a `flags.stamp` that kept reverting to another
session's `DEFINES`, which silently produced an image whose `DC_MAIN_MEMORY_SIZE`
did not match its ledger and aborted at boot). The fix is a command-line
override, and the stub/shrink trees are content-idempotent so they can be
shared:

```bash
make -C /work/dc -j4 BUILD=/work/dc/build-perf \
     STUBDIR=/work/dc/build/stubsrc SHRINKDIR=/work/dc/build/shrinksrc all
```
