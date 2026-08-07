# Session state — resume here

## ⭐⭐⭐ 2026-08-06 (session 7) — THE MUSIC NEVER PLAYS, AND IT IS THE AUDIO
## COMMAND QUEUE, NOT THE SYNTHESISER

**Human, on a running build: "the music isn't working though, only the talking
sound."** Root cause identified with evidence already in the logs:

- `Na_GameFrame` pushes ~55 commands/frame into a 256-entry ring; the ONLY
  consumer is `CreateAudioTask` (`sub_sys.c:733-736`), which on DC runs **only
  while the scene gate is armed** (`dc_audio.c:1104`). `DC_AUDIO_SCENES=3,9`
  leaves scenes **4 and 18 DISARMED**, the 64-slot queue fills,
  `Nap_SendStart` stops advancing `read_pos`, and `Nap_PortSet` overwrites one
  slot forever. Observed: **`SendStart::Mesg Full Queue`, 6 events**.
- ⭐ **Why only MUSIC dies:** BGM's `START_SEQ` is issued **once** per scene
  (`game64.c_inc:1511`); SFX re-issues from 8 sites and VOICE from every
  dialogue message. **A dropped command is invisible on SFX and permanent on
  BGM.** `grp->flags.enabled` never goes TRUE (`system.c:822`).
- Ruled out: sample data (`aram audio=8300384 LOST=0 zero=0 ext=3/32`), the
  sequencer chain (speech proves it runs), the jaudio heaps (they fit).
- **FIX:** drain the port queue every tick regardless of arm state — gate
  **synthesis**, not command processing. **Free confirmation: one rebuild with
  `DC_AUDIO_SCENES=all`.**

### ⚠️ RETRACTED THE SAME SESSION — the voice census measured a broken state

`[DC/VOICE]` ran while BGM was absent. **"The voice cap is dead because the town
never exceeds 0-4 concurrent voices" and "FIR/comb never run" are both
WITHDRAWN** — that was the music not playing. Both levers are **untested, not
dead**, and L2's mix-rate numbers are SFX-only. **An instrument pointed at a
subsystem that is not running measures the subsystem not running**; a human ear
caught in one sentence what three runs did not.

## ⭐⭐ 2026-08-06 (session 7) — G4 RAN. THE ~23.6 ms IS FOUR-FIFTHS emu64's

Queue item 1 is CLOSED. `-DDC_PERF_GXSPLIT=1`, town, 187 matched windows,
per PRESENTED frame (⚠️ **not** per tick — unlike `[EMU64H]`):

| | ms |
|---|---:|
| `[PHASE] draw` | 69.3 |
| `G_TRIN_INDEPEND` ×2 | **55.5** |
| `cull` + `xform` (already attributed) | **14.0** |
| **OURS — `dc_gx.c` setters, probe-subtracted** | **8.4** |
| **emu64 — `gxgap` + uncharged intervals** | **~30.9** |

- ⭐ **The work belongs in emu64 (G3), not in our renderer.**
- ⭐ **G3's "net loss" case is REFUTED.** `gxpos` is one call per vertex by
  construction: **0.536 µs/ref** against 1.29 ms over ~300 commands. At the
  measured **75 %** cull rate G3 saves **5.4 ms floor / 19.2 ms ceiling**.
- ✅ Instrument self-validated: `posn=13,197` vs `v+vcull=13,329`, counted by
  different code in different files. `probe=2.11 ms`, subtracted.

**TEV P3 is implemented and compile-verified, NOT run** (`-DDC_PVR_TEVP3`).
Wires `pvr_vertex_t.oargb` + `gen.specular`, which this port had never used.
**9 of 27 P3 configs exact**, not "P3 implemented". `[DC/PVR] tevp3 batches=0`
on a run reaching the keyboard falsifies the diagnosis without a screenshot.

### Ranked next actions (2026-08-06, session 7) — supersedes the list below

1. 🔴 **Drain the audio command queue every tick.** The BGM bug above. One
   rebuild with `DC_AUDIO_SCENES=all` confirms it first, for free.
2. ⭐ **G3 — cull at TRIN entry.** Now costed from measurement, not estimate:
   **5.4-19.2 ms** of a 45-69 ms frame. Design (trampoline, index walk, the
   `dirty_check`-first ordering rule) is in `kb/research-fps-ideas.md`.
3. **Re-run the audio census WITH music playing.** Every audio number in this
   file is SFX-only until then — including L1, L2 and L4.
4. **TEV P3 screenshot pair** — the code is in and off.
5. **A hardware burn.** Flycast under-reproduces the stutter 10× (15/900 s here
   vs 192/420 s on console) and models no instruction cache.

Updated 2026-08-06 (session 6). This file is **short on purpose**: only what is
true *right now*, plus what to do next. Everything else is one hop away.
**A fresh context should read `kb/RESUME.md` first** — that is the handoff and
it carries the build lines and the **nine** measurement rules; this file is the
numbers and the queue.

## ⭐⭐⭐ 2026-08-06 (session 6, later) — AUDIO WORKS ON HARDWARE. IT WAS ONE
## UNWIRED FLAG, AND THE STUTTER IT LEFT IS A BUDGET, NOT A BUG

**`DC_AUDIO=1` needs `-DDC_ARAM_AUDIO_DROP=0` and nothing wires it.**
`dc_aram.c:313-320` drops every write below `aram_audio_end` *before*
`dc_dvd_provenance()` at `:325`, so all 8,300,384 B of `audiorom.img` streamed
in and was discarded, the audio half got **zero** extents, every jaudio sample
fetch hit `dc_aram.c:401-409` ("no extent covers this offset") and was
`memset` to zero, and the mixer faithfully mixed silence. `dc/Makefile:801`
*comments* about the flag; `BUILDING-DC.md:126,163` documents it as a manual
recipe. **Human-verified working on real hardware once set.**

```
[DC/ARAM] LRU w:mapped=13282784 ... zero=0(0 B) | ext=3/32   (was ext=2/32, zero=123)
[NEOS_OUT] peak 0 -> 3851
```

⚠️ **THE `-O0` AUDIO DOCTRINE IS DEAD.** "One DAC frame costs 19.8 ms, 113 % of
the machine, so AICA Stage B is worth 6-10 weeks" was measured at `-O0`.
Measured now: **mean 3,777 µs** against 17.49 ms of audio per frame. Software
synthesis is affordable; Stage B is not needed for cost reasons.

**THE STUTTER (~2/s on hardware, 192 `[STUTTER]`/420 s vs 14 silent) IS A
PLATEAU, NOT A TAIL.** New counters `snd=` / `sndf=` / `smax=` on the
`[STUTTER]` line: `4 × smax == snd` on every spike, so **every** synthesis call
costs ~10 ms during a stutter against a 3.78 ms mean. jaudio is **bimodal**
(~2.5 ms / ~10 ms, almost certainly voice count), and the budget is:

| | cheap | expensive |
|---|---:|---:|
| per DAC frame | ~2.5 ms | **~10 ms** |
| × 2.57 frames per ~45 ms game frame | ~14 % | **~57 %** |

At 57 % the frame collapses, the ring falls behind, and the pump hits its
4-frame ceiling (2 ticks × `DC_AUDIO_MAX_FRAMES=2`) catching up.

**FOUR HYPOTHESES REFUTED BY MEASUREMENT** — disc-cache misses (ARAM 4→16
blocks took the hit rate 83→97.9 % and disc reads 3.54→0.77/s; **hardware
stutter unchanged**); a multi-frame burst (`sndf=4` is 2 ticks × 2, not a burst);
`snd_stream_poll`/G2/the 10 ms scheduler quantum (`DC_AUDIO_MAX_FRAMES=0` keeps
the whole KOS DMA/semaphore path live at **0.1 ms** and drops stutters to 13);
and jaudio's mean cost (`-O3` on rspsim/driver/system/aictrl: 192→174 events,
mean −1.4 %, where `-Os` bought the decomp draw path 41 %). The `-O3` promotion
is kept anyway; `jammain_2.c` is deliberately NOT promoted with it.

**The remaining lever is PEAK per-frame synthesis cost** — voice count, effects,
or rspsim at 22 kHz (`kb/audio-cpu-cost.md` A0-A4). That is a product decision.
**`DC_AUDIO_SCENES=3` is the shippable config today**: the K.K. scene is 888
verts at the frame cap and can afford 57 %; the town has nothing spare.

## ⭐⭐ 2026-08-06 (session 6) — G1 RE-RUN: EVERY `[EMU64H]` NUMBER WAS HALF THE
## TRUTH, AND RAM HAS STOPPED BEING THE BINDING CONSTRAINT

⚠️ **`[EMU64H]` is per LOGIC TICK, not per presented frame — DOUBLE IT.** G1
arms at the end of every tick (`dc_vi.c:405` frameskip, `dc_vi.c:633` presented)
and `s_frames` counts ticks. Proof: `tot 24.28 × 2 = 48.56` against
`draw 45.6 + skip 2.9 = 48.5`, and the `-O0` run's `42.86 × 2 = 85.7` against
`78.3 + 8.2 = 86.5`. **Two sessions quoted the halved numbers.** Measurement
rule **9** (`kb/RESUME.md` §0b), `kb/traps.md`.

Run `smoke-oc-dc-g1b-20260806-164033-15671`, town, probe-free, medians over 47
windows, **×2-corrected**:

| | `-O0` | **`-Os` + `-O3`** |
|---|---:|---:|
| `draw` | 78.3 | **45.6** |
| `G_TRIN_INDEPEND` | 44.5 / 292 | **34.4 ms / 306 calls = 112.5 µs, 75 % of the frame** |
| `G_VTX` | 10.8 | **1.84** |
| `G_MTX` | 4.36 | **1.72** |
| `G_TEXRECT` | 4.34 | **2.88** |
| `gap` | 15.8 | **5.96** |
| `[EMU64H] tot` | 85.7 | **48.56** |

`[PHASE] draw=45.6 skip=2.9 vi=0.4 | cull=2.0 xform=8.8 | v=2899 vlit=2689
vcull=5250 us/v=3.06`, `cmds=3562`.

- ⭐ **~23.6 ms is unattributed and it is the largest block in the project.** Of
  TRIN's 34.4 ms, `cull 2.0 + xform 8.8 = 10.8` is measured `dc/`. The rest is
  **emu64's `dl_G_TRIN` index expansion PLUS our own `GX*` attribute setters in
  `dc_gx.c` — and those two have never been separated.** 52 % of the frame.
  **Nothing below is costed until that split lands** (queue item 1).
- ⭐ **G3 is the biggest lever in the project**, and bigger than its old
  4.5-7.0 ms estimate: `vcull=5250` vs `v=2899` means **64 % of vertex
  references are fully expanded and pushed through the GX setters before the
  batch is rejected.**
- ✅ **`gap` IS ATTRIBUTED — CLOSED.** It is emu64's own dispatch-loop overhead:
  slot `HIST_GAP=64` (`dc_emu64_hist.c:87`) accumulated in `hist_enter()`
  (`:124-131`) when `s_prev == HIST_GAP`, i.e. `emu64_taskstart_r`'s loop
  control (`emu64.c:5807-5824`, `:5847-5855`, `:5874`) plus frame
  prologue/epilogue. Confirmed by 15.8 → 5.96 when that loop went `-O3`.
  ⚠️ `probe=` is **not** subtracted from `tot` or `gap` (`dc_emu64_hist.c:300`
  only prints it) and both probes land inside `gap` by construction.
- ❌ **G2 is DEAD** — its target *was* `gap`, now 5.96 ms and already `-O3`.
  Delete `dc_emu64_shadow.cpp` after one A/B: it costs nothing when off (all
  inside `#if DC_EMU64_SHADOW_LOOP > 0`, 20 KB only when on) **but it blocks G1**
  via the `#error` at `dc_platform.h:417`. `kb/closed.md`.
- ✅ **`G_VTX` is finished as a topic.** 10.8 → 1.84 ms from a compiler flag.
- ✅ **Every state opcode is ≤ 0.5 ms.** Stripping state commands stays worth
  nothing (F8).

### RAM: the problem is no longer "does it fit"

Committed `296a1d2`. Three links:

| | shipping | + interiors/winter | **+ gyroids (now)** |
|---|---:|---:|---:|
| `.text` | 2,753,700 | 2,793,284 | **2,854,108** |
| `.bss` | 3,945,484 | 4,428,076 | **4,791,884** |
| image span | 8,926,124 | 9,446,380 | **9,878,540** |
| `margin` | 6,061,268 | 5,541,012 | **5,109,364** |

Real headroom (`margin` − 3,056,276 libc peak, rule 6) went **~146 KB on
2026-08-04 → ~2.05 MB**, after spending 952,416 B on content. `MEMLEDGER OK`,
`ASSET MISSING 0`, `aram LOST 0`, `deepest_scene 18`, `run_report --vs` no
regression, town `us/v` 3.07 → 3.09, **gyroids confirmed rendering by a human**.

**The old line "the full image still does not fit, and that is the only thing
between here and a playable build" is VOID.** What binds now is **residency**:
8,813,054 B of asset destination arrays can never all be resident, so the keep
list still decides what exists. ✅ And the other extreme is closed by
experiment — a full `DC_ASSET_STUB=0` image boots to
`MEMLEDGER … margin=-781036 OVER`, fails a **15,638,528 B contiguous malloc**,
and comes back with **all 14,495 assets MISSING** (`kb/closed.md`).

⚠️ **Cost a keep-list addition from two links.** The gyroids were estimated at
155,360 B by summing `Vtx` arrays and cost **432,160 B** of span — 2.8× low,
because the files carry textures and display lists too (`kb/traps.md`).

### Two more results, both with their own file

- **T1 is designed and much cheaper than its concept note** — the seam is
  already ours (`GXLoadTexObj` → `dc_gx_backend_texture_upload`), **no pool is
  needed** (the PVR's VRAM LRU already holds every texture; main RAM is read
  only to compute the cache key), so **phase 1 is −579,248 B** and **phase 2
  buys 5,685 textures / 2,782,080 B of content for +68,000 B**.
  `kb/levers.md` **L10**.
- **A whole TEV class is unimplemented and it is visible.** The name-entry
  keyboard renders black because 18 of its 26 display lists are **config #037,
  class P3** — and `dc_pvr.c` implements no part of P3: `pv.oargb` is hardcoded
  `0` at `:1988`. **27 of the 101 configs.** Not a stub, ruled out statically
  and by argument. `kb/tev-map-hard-cases.md` §6.6.

### Ranked next actions (2026-08-06) — supersedes every earlier list in this file

**`kb/RESUME.md` §5 carries the same list and the two must agree.**

1. ⭐ **Split the ~23.6 ms TRIN remainder.** One build, one run: a
   `dc_time_us()` bracket around the `GX*` attribute setters in `dc_gx.c`. It
   decides whether the work goes into emu64 (G3) or into our own renderer.
   **Nothing below should be costed until it lands.**
2. ⭐ **G3 — cull at TRIN entry.** 64 % of vertex references are wasted.
3. **TEV P3 / `oargb`** — fixes the black keyboard and 27 configs. Needs a kill
   switch and a screenshot pair. `kb/tev-map-hard-cases.md` §6.6.
4. **AABB cull via XMTRX.** `dc_gx_batch_is_offscreen` (`dc_gx.c:435`, math at
   `:495-519`) runs two scalar stages per corner — MV 3 dots (`:503-505`), P 4
   dots (`:506-509`) = **200 mults/batch** — and never touches the matrix unit,
   while `dc_pvr.c:2782-2799` builds the same `P·MV` one line later but **after**
   the cull (`:559` cull, `:591` submit). Est **0.4-0.8 ms** of 45.6. Kill
   switch `-DDC_GX_NO_FTRV_CULL`. ⚠️ Must call `dc_mtx_xmtrx_invalidate()` as
   `dc_pvr.c:2812` does.
5. **`chan_eval`'s light loop** (`dc_pvr.c:837-902`) — 3 FIPRs per light per
   vertex, ~35-70k FIPR/frame. ⚠️ **Correction to the record: the per-lit-vertex
   block at `dc_pvr.c:2868` is ALREADY OPTIMAL** — `mv`/`nm` are hoisted per
   batch at `:2779-2781` and the seven ops at `:2875-2886` are already `fipr()`
   via `DC_DOT4`/`DC_DOT3` (`:187-191`). Holding `nm` in XMTRX **loses**, because
   `comb` needs XMTRX for the position FTRV at `:2863`.
6. **T1 phase 1 (−579 KB), then phase 2** (every texture for +68 KB).
   `kb/levers.md` L10 — run the `DC_TEXPOOL_PROBE` falsifier first.
7. **N2b — wire the VMU save path.** Unchanged, and still the only way to get a
   villager into the town.
8. **A hardware burn.** Nothing this session ran on silicon, and Flycast models
   no instruction cache against a 2.83 MB `.text` cut.
9. **Re-cost the audio.** The 19.8 ms/DAC-frame figure that drove the whole
   AICA-vs-software verdict was `-O0`; jaudio is `-Os` now and has never been
   re-measured. ⚠️ `jammain_2.c` is the first quarantine suspect the moment
   `DC_AUDIO=1` — C++ TU, missing return, 22 uninitialised reads
   (`dc/opt-lists.mk`).

## ⭐ 2026-08-06 — THE `-O0` BAN IS REVERSED. 2.8 MB OF `.text` AND +8 FPS
## FROM ONE FLAG, AND THE `dc/` CONTROL PHASE DID NOT MOVE

`src/` builds at **`-Os`, with 14 hot TUs at `-O3`** (`DC_OPT_PROFILE=perf`,
the default). `DC_OPT_PROFILE=o0` is a byte-identical revert. Lists and
justifications: `dc/opt-lists.mk`. Evidence: `kb/state-log.md`, top entry.

Matched town windows, same acre, `v` within 2 %:

| | `-O0` | `-Os` | **`-Os` + `-O3` hot** |
|---|---:|---:|---:|
| `.text` | 5,506,964 | 2,680,676 | **2,729,152** |
| town FPS | 11.6 | 18.5 | **20.0** |
| `draw` ms | 79.1 | 50.3 | **46.8** |
| logic tick ms | 6.6 | 3.3 | **2.8** |
| `xform` ms (`dc/`, `-O2` throughout — THE CONTROL) | 13.1 | 12.9 | 12.4 |
| whole-run FPS p50 | 24.5 | 29.8 | 29.8 (capped) |

- **`.text` −2,826,288 B** — bigger than every `.bss` lever this project has
  landed, combined. Optimization is now a RAM lever as well as an FPS lever.
- Screenshot-gated: two 900 s `DC_AUTOWALK` runs differing only in
  `DC_OPT_PROFILE`, frame-matched at the train, the town, the house tour and
  K.K. — same image. `ASSET MISSING 0`, `crashes=0`, `run_report --vs` clean.
- ⚠️ **`shot_diff.py` cannot gate an optimization change** — the probe fires per
  presented frame and the faster build is at a different point in the same
  camera pan. Compare scenes, not pixels. (`kb/state-log.md`.)
- ⚠️ **Nothing here has run on hardware.** And the emulator UNDERSTATES this
  change: Flycast models no instruction cache, and this removed 2.8 MB of
  `.text`.

**Why it stood for five weeks:** the ban was armhf evidence, never reproduced
on SH-4, never isolated from a simultaneous NEON/cpu-target change, and its
most likely real cause was a missing `JUTRomFont::spFontHeader_` definition —
a LINK bug. `kb/closed.md` carries the full post-mortem.

**The old FPS arithmetic below is now wrong in its absolute numbers.** The
78.3 ms frame it decomposes is 46.8 ms today, and the `src/`-vs-`dc/` split
has moved sharply toward `dc/`: `xform` was 15.6 % of the draw phase and is now
~27 %. ⚠️ **G1 must be re-run before any FPS plan is costed again** — every
per-opcode number in this file was measured at `-O0`.

## ⭐ 2026-08-05 — G1 RAN. ONE OPCODE IS 28 % OF THE TOWN FRAME, and it is not
## the one every plan was costing against

⚠️ **[SUPERSEDED 2026-08-06 — every `[EMU64H]` figure in this section is HALF
its real value.]** The histogram is per logic tick; at `ticks_per_visual = 2`
these must be doubled. `G_TRIN_INDEPEND` was **44.5 ms of an 86.5 ms frame
(51 %)**, not 22.25 of 78.3 (28 %); `G_VTX` was 10.8, `gap` 15.8. The section
above carries the re-run at `-Os`/`-O3`. **`gap` is CLOSED** — it is
`emu64_taskstart_r`'s loop control. Kept for the sequence and for the `G_VTX`
correction, which stands.

Run `smoke-G1-20260805-160640-92325`, town, probe-free, 11.5-12.1 FPS:

```
[PHASE]  draw=78.3 skip=8.2 vi=0.4 | cull=2.2 xform=12.2 | v=3002 vcull=6042 us/v=4.07
[EMU64H] tot=42.86ms gap=7.92ms | TRIN_INDEPEND 22.25/146  VTX 5.40/149  MTX 2.18/113  TEXRECT 2.17/25
```

- **`G_TRIN_INDEPEND`: 22.25 ms over 146 calls = 152 µs/call — 63 % of emu64
  dispatch, 28 % of the whole frame.** Handler `emu64.c:4798` → `dl_G_TRIN`.
- **`G_VTX` is 5.40 ms, not the ~48 ms four documents costed G3 against.** That
  figure applied a whole-command average to a subset (rule 7, in the sentence
  that states rule 7) *and* counted `GXPosition3f32` references as loaded
  vertices. Corrected in place everywhere.
- **65 % of TRIN's 152 µs is already `dc/` at `-O2`** (`GXEnd` at
  `emu64.c:4935` → the AABB cull ~15 µs + `dc_gx_backend_submit` ~81 µs). Only
  ~53 µs is `src/` at `-O0`, so a G2-shaped rewrite is capped at ~8.3 ms and
  realistically buys **2-4 ms** [ESTIMATED] — not 25-35.
- **The biggest addressable block in the frame is ours:
  `dc_gx_backend_submit`, 12.2 ms = 15.6 % of the draw phase**
  (`dc/src/dc_pvr.c:2448`). No trampoline, no sign-off.
- ~~**`gap=7.92 ms` (18 % of `tot`) is inside the draw phase, outside any emu64
  command, and unexplained. OPEN.**~~ ✅ **CLOSED 2026-08-06** — it is emu64's
  own dispatch-loop overhead (`HIST_GAP=64`, `dc_emu64_hist.c:87`,
  `hist_enter()` `:124-131`), and it was 15.8 ms, not 7.92. See the top section.
- **`vcull=6042` against `v=3002`: 66.8 % of vertices are culled after emu64
  paid full `-O0` price.** ⚠️ That is **not** worth 14.9 ms — culled vertices
  already cost `xform` nothing. An ideal cull at TRIN entry is
  **4.5-7.0 ms, central ~6.0** [ESTIMATED] ⇒ 11.5 → **12.2-12.6 FPS**.
- **OPEN: the vertex memo's ceiling is contested** — 48.2 % (at its ceiling,
  worth zero) vs ~60 % (11 points of headroom), depending on whether total
  staged references are the old 6,951 or the newly measured `v + vcull` =
  9,044. Settle it with a direct count of distinct vertex references per batch.
  `dc_gx.c:870`'s 6,951 comment is stale.

**R1 landed: acre ground textures are demand-loaded.** 96 `mFM_grd_*` symbols
(150,880 B) stop being resident; `dc/src/dc_bgtex.c` + a `make_src_shrink.py`
rewrite of one `bcopy`, `src/` untouched, kill switch `DC_BGTEX_DEMAND=0`.
**`.bss` 4,027,212 → 3,945,356 (−81,856). `margin` 3,103,956 → 3,191,348
(+87,392).** No OOM, `ASSET MISSING` 0, `aram LOST` 0, `deepest_scene` 18
unchanged, `fps_p50` 24.1 → 24.2, and **screenshot-verified on a
`DC_BGTEX_DEMAND=0` vs `=1` pair** — so this one clears measurement rule 2, not
just the counters.

**R2 and R3 landed too, and are DEFAULTED OFF** — the villager texture and model
pools, 16 slots each (`dc/src/dc_npctex.c`, `dc/src/dc_npcmdl.c`,
`DC_NPCTEX_POOL=1` / `DC_NPCMDL_POOL=1`). They are **content restoration, not a
saving** (see the corrected pool section below), and they are off because
**the port constructs ZERO villager NPCs**: `mNpc_SetNpcList` populates the town
from the save's `Animal_c animals[]` (`m_start_data_init.c:559`), the VMU path is
unwired, `[PC] No save file found`. Two 900 s runs that reach scene 9 and walk
printed not one `[DC/NPCTEX]`/`[DC/NPCMDL]` line, because
`aNPC_dma_draw_data_proc` (`ac_npc_ctrl.c_inc:687`) never runs. **Wiring the save
(N2b) is now a PREREQUISITE for testing them.**

**SH-4 math: FTRV/FIPR/FSRRA were already live; two real gaps were not.**
`DC_MTX_USE_FIPR` now defaults to 1 (`-DDC_MTX_NO_FIPR` reverts), and
**272 `sqrtf` sites in `src/` were linking newlib's SOFTWARE
`__ieee754_sqrtf`** — `dc/src/dc_fmath.c` binds them to FSQRT
(`-DDC_NO_FSQRT` reverts), bit-identical to newlib for every normal input.
**sh4zam stays a PASS** and `kb/closed.md` carries the corrected reasons.
One RAM find: `dc/src/dc_misc.c:421` calls **double** `sin()` to build a startup
table and drags in **5,940 B** of libm — 44 % of the image's libm, in our own
code, removable without touching `src/` (`kb/levers.md`).

### Ranked next actions (2026-08-05) — ⚠️ SUPERSEDED. Use the 2026-08-06 list at the top of this file.

Kept for the reasoning only. Items 1-3 were costed against halved `[EMU64H]`
numbers and an `-O0` frame; item 6's two levers are still the right ones.

1. **`dc_gx_backend_submit` — 12.2 ms, ours, `-O2`, no gate.** First step is
   free: print the vertex-memo hit ratio and a per-batch `count` histogram from
   `dc_gx_flush_vertices`. It decides whether this is a 3 ms or an 8 ms lever
   and settles the memo-ceiling question above.
2. **Per-slot shadow of `G_MTX`** (2.18 ms / 113 calls) as proof the per-slot
   machinery works — est 0.9-1.4 ms. ⚠️ `dc_emu64_hist.c:262` and
   `dc_emu64_shadow.cpp:492` both `memcpy` **all 64 slots** on frame open and
   restore all 64 on close; a third installer must arm slot-wise or initialise
   strictly first.
3. **Slot-60 `TRIN_INDEPEND` cull-only shadow** — AABB over the index window at
   entry, `dl_G_TRIN` kept as the fallback. Est 4.5-7.0 ms.
4. **Wire the VMU save path (N2b)** — now blocking, because it is the only way
   to test R2/R3.
5. **The audio sequencer floor:** `-Wl,--wrap=_RspStart2` (note the leading
   underscore) + an empty `__wrap_` deletes exactly the work AICA would absorb.
   Read `synth_us=` against 19,840: ≤2,000 ⇒ AICA Stage B is worth 6-10 weeks;
   ≥8,000 ⇒ an `-O2` rspsim shadow outranks it.
6. **The TEV shadow-alpha fix** — config #007 currently loses BOTH alpha
   factors and draws at `vtx.a * T0.a`, i.e. the fog coefficient: the black
   wedges. `kb/RESUME.md` §4 item 6 has the two levers, both needing a
   screenshot pair.

Still open behind those: the `gap=7.92 ms`; a sorted batch helper for R1's 27
scattered seeks (0.5-2.7 s [UNMEASURED]); and the acre pool — the one class
where "a pool frees RAM" is still true, **815,024 B of summer acre vertex arrays
measured resident**. Evidence for all of it: `kb/state-log.md`, top entry.

## ⭐ 2026-08-04 session 3 — EVERY SUMMER STRUCTURE NOW RENDERS, and it cost
## nothing, because the keep list had been buying WINTER

**The headline: all 84 `obj_s_*` town structures are in the image.** Nook's
shop, the museum, the tailor, the shrine, the police box, the notice board —
they were 71-of-84 stubbed, i.e. black spiky messes, and they are now drawn.
`.bss` 3,917,324 → **4,025,644 (+108,320 B)**, and the run is clean:

```
no "Out of memory", ASSET MISSING 0, LOST 0, deepest_scene unchanged (18)
fps_p50 13.8 -> 13.6 (screenshot build, within noise)   pvr_dropped 1,314 -> 0
VERDICT: no regression detected
```

**Why it was affordable, and this is the part worth remembering: every
`src/data/model/obj_s_*.c` carries BOTH seasons.** `obj_s_house1.c` is 42,624 B
of `obj_s_house1_*` and **42,720 B of `obj_w_house1_*`**. Keeping is per-FILE,
so the 13 structures the old list kept were buying **101,216 B of winter that a
summer town can never draw** (the season is picked at `ac_shop.c:92-94`).
`make_stub_data.py` now takes a per-symbol filter — `path.c#!obj_w_` keeps the
file minus its winter arrays — so all 84 summer structures cost **+109,936 B**
against the ~146-181 KB of real headroom, instead of +332 KB.

⚠️ **Use the EXCLUSION form (`#!obj_w_`), never the inclusion form
(`#obj_s_`)**: 3,680 B across nine of those files are season-NEUTRAL and named
neither (`obj_kanban_pal`, `hakushi_tex`, `obj_shop4_grass_tex_pic_i4` …), and a
stubbed palette does not fail loudly — it renders its model in garbage colours.

⚠️ **DATED TIME BOMB — REDUCED 2026-08-05, not cleared.** The winter *ground*
half is fixed: R1 makes all 41 `mFM_grd_w_*` textures loadable on demand
(`kb/RESUME.md` §4 item 3). The 84 `obj_w_*` structures are **still absent**, so
a winter town still draws every building as a black spiky mess. Needs a
`DC_SEASON=winter` build.

### The near-miss: regenerating the keep list DELETED Tom Nook

`keeplist-town.txt` had 25 entries **typed in by hand** that the generator never
emitted — the START map overlay, the clock/date HUD, and `rcn/rcc/rcd/rcf/rcs/
tuk _1` (Tom Nook and the raccoons). Regenerating silently dropped all 25, and
it was caught only because a human said Nook looked wrong. They now live in
`EXTRA_SOURCES` in `tools/dcstub/make_keeplist_town.py`, and the regenerated
list is a strict superset of what shipped. **Verify that property whenever this
file is regenerated**, it is two lines of python.

### Interiors are deferred, on purpose

The generator's acre glob also sweeps in building interiors and the developers'
scratch rooms (`rom_*`, `room01`, `tmp*`, `myr_etc`, `grd_post_office`,
`grd_yamishop`) — **+269,312 B**, which does not fit next to the structures.
They are excluded by default; `--interiors` turns them on. ⚠️ **Do not wait for
"the villager pool to free the room" — it does not free any.** R2 is roughly
break-even and R3 costs +115,424 B (see the corrected pool section below).
Interiors need either their own pool, costed against the 269,312 B of *keeping*
them, or headroom from somewhere else.

### The RAM plan from here is a POOL, not a bigger keep list

⚠️ **CORRECTED 2026-08-05 — a pool does not FREE bytes in a stubbed image, and
this section used to say it did.** The rule, which governs every remaining pool
idea (acres, structures, interiors):

> **In a stubbed image, an asset class's resident cost is what the KEEP LIST
> kept, not what the class totals.** `DC_ASSET_STUB` already dropped the rest —
> an unkept asset is a 1-byte `.bss` symbol with its load suppressed. A pool is
> worth building when it **delivers content the keep list cannot afford**, not
> when it "frees" bytes the stub system dropped long ago. Cost a pool against
> the *alternative* (keeping the class), never against the class total.

Measured, and this is what the villager pools actually did:

| | non-stub total | resident before | pool | net `.bss` | content delivered |
|---|---:|---:|---:|---:|---|
| **R2** villager textures | 1,154,944 | **90,464** (21 of 236 sets) | 78,872 (16 × 4,832) | **~−4,700** | 21 species with textures → **236** |
| **R3** villager models | 438,640 | **5,536** (`cbr_1` only) | 120,956 (16 × 7,552) | **+115,424** | 1 species with geometry → **32** |

⚠️ **"Content delivered" is what the pools WOULD deliver — both default to 0,
and nothing has been delivered yet**, because the town constructs no villagers
at all (see the top of this file). Read that column as the case for turning them
on after N2b, not as a description of the shipping image.

R3 **spends** bytes. It is justified because the only other way to get those 31
species is keeping all 32 `mdl/*.c` at 194,400 B, so the pool is 73,568 B
cheaper than its content. Follow-up lever: 16 max-sized slots waste 15,248 B
against the 16 largest packed (105,584 B) — a bump arena, or just cut
`DC_NPCMDL_SLOTS` / `DC_NPCTEX_SLOTS`.

The mechanism notes that survive the correction:

- villager **textures need no patching at all** — they are bound through
  segment registers at draw time (`ac_npc_draw.c_inc:269-278`), so a load is 16
  pointer writes into `npc_draw_data_tbl[]`, which is an ordinary global;
- villager **vertices need ~29 patched `Gfx` words** per species (933 across the
  32, min 21, max 37), reachable from the global skeleton `cKF_bs_r_<species>`,
  and the display lists are in `.data`, i.e. writable;
- the seam is `mNpc_SetNpcList` (`m_npc.c:2799`) or just polling
  `common_data.npclist[]`. ⚠️ **NOT `--wrap`** — sh-elf prefixes user labels
  with `_`, so the symbol is `_mNpc_SetNpcList` and `--wrap=mNpc_SetNpcList`
  matches nothing, without a diagnostic (`kb/traps.md`).
- **there are 32 distinct villager models, not 72 and not 236.** 382
  `npc_draw_data_tbl[]` rows → 72 skeletons: 32 villager-only, 40 special-only,
  zero shared; the 236 villager texture sets share those 32 models.

⚠️ **`mFM_DecideAcre` DOES NOT EXIST.** Four kb files cite it. The real
generator is `mRF_MakeRandomField` (`m_random_field.c:9`), and it writes the
layout **into the save**, not per boot — the port re-rolls every boot only
because the VMU path is unwired. "A census cannot make a town keep list" stays
true; the reason is "per player", not "per boot".

### Instruments: two were unreachable, and one measured the wrong tick

- **`DC_EMU64_HIST` was never forwarded into the build container**, so G1 was
  unreachable from the documented build line — that is why it sat "in the tree,
  never run". Fixed, along with the same hole for `DC_EMU64_SHADOW_LOOP`.
  `kb/traps.md` has the rule and the verification command.
- **G1 armed on the wrong edge.** It sampled the frameskipped logic tick, which
  issues no display-list commands, so its first real run returned every opcode
  bucket empty and 100 % of the time in `gap`. Both instruments now arm at the
  end of *every* tick (`dc_vi.c`). **The histogram still has to be re-run** —
  G1 has not yet produced a per-opcode number, so nothing in F1/F8/G2/G3 is
  costed yet.

### G2 WORKS — first measurement, and it is small as predicted

`dc/src/dc_emu64_shadow.cpp` — emu64's dispatch LOOP at `-O2` in `dc/`, via the
same trampoline mechanism G1 uses. Off by default (`DC_EMU64_SHADOW_LOOP=0`).
User-approved 2026-08-04 along with G3.

```
[EMU64S] armed, mode=1
[EMU64S] traversals=28 cmds=65729 punts=0      <- the town, one 30-frame window
fps_p50 24.0 -> 24.8      deepest_scene 18 (unchanged)      no crash, no punts
```

The shadow really is running the traversal (65,729 commands in one window), the
self-check never tripped, and nothing was handed back to `-O0`. **Treat +0.8 FPS
as a first datum, not a verdict** — it is a whole-run p50 across scenes, and the
town-only number has not been separated out.

⚠️ **THE SELF-CHECK MUST RUN BEFORE THE ORIGINAL HANDLER, and getting this
backwards cost a run.** The first version checked `gfx == *gfx_p` *after*
calling the handler and disabled itself with "member offsets are wrong" on a
build whose offsets were fine: `dl_G_DL`, `dl_G_ENDDL`, `dl_G_CULLDL` and
`dl_G_BRANCH_Z` all rewrite `gfx_p` as their entire purpose, so the two
correctly disagree afterwards. The mechanism behaved exactly as designed —
it refused to run rather than corrupt — but it was refusing for a bogus reason.

⚠️ **`pvr_dropped` is CLOSED, and it was never noise in the statistical sense
(2026-08-06).** `s_tris_dropped` (`dc_pvr.c:134`) increments **only** on
near-plane geometry — all three vertices behind (`:2149`, `w <= 0.001f`), a
straddle under `-DDC_PVR_NO_NEARCLIP` (`:2162`), or Sutherland-Hodgman emitting
`< 3` vertices (`:2181`) — so it is **purely data-dependent on camera
position**. The 0 / 1,305 / 1,314 / 0 spread across four identical-code runs and
the `1,300 → 0` on the `dc/src` `-O3` change were both *where the camera was
standing*, not what the build did. `run_report.py --vs` will keep calling it a
REGRESSION and keep being wrong. `kb/closed.md`.

⚠️ **Its documented 7-14 ms/frame estimate is above its own ceiling.**
`emu64_taskstart_r` is 0x480 B of `.text` and SH-4 instructions are 2 bytes, so
the whole function is **576 instructions** — running all of it once per command
at 2,867 cmds/frame and IPC 1.0 is 8.3 ms, and the loop body is a fraction of
that. **Expect 2-5 ms.** G3 likewise re-estimated 15-25 ms, not 25-35.

⚠️ **`emu64.hpp`'s member-offset comments are PowerPC and WRONG for sh-elf.**
`sizeof(emu64)` is **0x2278**, `gfx` is at 0x44 not 0x48, `vertices` at 0x1018
not 0x0E1C — `long long` aligns to 4 here and `GXTexObj` is 88 B, not 32. That
is why the shadow is a C++ TU that includes the real header: a hand-written
offset map would write into the neighbouring member instead of faulting.

⚠️ **`kb/research-fps-ideas.md`'s `emu64_set_aflags()` seam DOES NOT EXIST.**
`AFLAGS_MAX` is 0, so its guard is `idx > 0 && idx < 0` and `aflags_c::set()` is
an empty body. The "acre_render-shaped lever" is unreachable that way. The
upside: every `aflags` test folds to a constant, so a shadow can delete the
wireframe and one-triangle paths outright.

### New tool: the visual regression gate is mechanical now

`tools/dcfb/shot_diff.py BASE/console.log CAND/console.log --out DIR` pairs two
runs' framebuffer dumps by frame index and reports changed% / meanabs / maxabs
**plus the nonzero-pixel count on each side** (so "both frames went black" can
never score as a perfect match), and writes base|cand|diff×4 contact sheets.
⚠️ The town is not deterministic across boots, so establish the same-build noise
floor before reading a number as a verdict; indoor and title scenes are the
sharp instrument.

### Still open, ranked

1. ✅ **DONE 2026-08-05 — G1 ran.** See the 2026-08-05 section at the top of
   this file; it moved the answer from `G_VTX` to `G_TRIN_INDEPEND`.
2. **Measure G2** — built, self-check fixed, run not yet read. ⚠️ Its ceiling is
   now known: ~8.3 ms, expect 2-4 ms.
3. ✅ **DONE 2026-08-05 — the villager pools shipped** (R2 textures, R3 models).
   ⚠️ Read the corrected pool section above before quoting what they bought:
   they restore content, they do not free RAM.
4. **Tom Nook's apron is missing.** Not a keep-list gap: his draw data points
   only at `tuk_1_tmem_txt` + palette + eyes, all kept, and all five of his
   model parts are in the kept TU. Next step is a `DC_TEX_LOG=1` run to see
   whether that texture uploads non-zero.
5. ✅ **DIAGNOSED 2026-08-05, not fixed — the large black wedges are TEV config
   #007** (`ef_shadow_out.c:34-35`, two stages, not three). It loses *both*
   alpha factors and draws at `vtx.a * T0.a`, i.e. the `G_RM_FOG_SHADE_A` fog
   coefficient. The suspicion "opaque shadow decals" was right about the
   geometry and wrong about the mechanism. Two levers, both widenings needing a
   screenshot pair: `kb/RESUME.md` §4 item 6.
6. `aram zero=7` still non-zero.

## ⭐ 2026-08-04 session 2 — three things changed, and one of them was a
## misdiagnosis the project had been carrying for two sessions

### ⭐ RESOLVED LATER THE SAME DAY — the acre fix LANDED, and here is what paid

Item 1 below says the wide keep list does not fit. It does now.
`DC_ARENA_BYTES` 1,900,000 → **1,200,000** on the strength of the first-ever
TOWN arena measurement (`[DC/ARENA] zelda used=289536 free=1124944`) plus the
jaudio `.bss` shrink (−450,368 B, keyed to `DC_AUDIO=0`) bought ~1.15 MB, and
`tools/dcstub/keeplist-town.txt` now keeps all 371 summer acre TUs, the map
overlay, the date/time HUD, Tom Nook and the raccoon NPCs, `obj_s_house1` and
`obj_s_myhome1`. Shipping image: `image_span=11749436 additive_heap=1658752
margin=3237956 OK`, no OOM, `ASSET MISSING 0`.

Human verdict: *"the mountain texture works now"*, *"the textures overall look
excellent, significantly improved"*. Read `kb/RESUME.md` §3 for the mechanism
and §4 for what is still stubbed. ⚠️ That list said "60 villager models"; the
real figure was **31 of 32 villager species**, and R2/R3 have since restored
them out of pools (74 structures and winter still stand).

1. **"Missing and weird textures" is mostly MISSING GEOMETRY — and the obvious
   fix does not fit.** The keep list covers 18 of 268 acres and 11 of 84
   summer structures, and an acre `.c` stubs its **vertex** array, not just its
   textures — so an unkept acre draws its unstubbed display list against
   all-zero vertices, every triangle collapses to the origin, and the acre
   renders **nothing**. Censusing harder cannot fix it either
   (`mFM_DecideAcre` builds the town from the save's seed).
   `tools/dcstub/keeplist-town.txt` enumerates all of them from the tree —
   **and the resulting image dies on the splash screen.** See §"the ~146 KB"
   below. The list is checked in as a measurement artefact and an aspiration;
   `keeplist-opening.txt` is what boots.

### ⚠️ THE REAL HEADROOM IS ~146 KB, AND `MEMLEDGER FIT` SAYS `OK` ANYWAY

The wide keep list built clean and reported
`MEMLEDGER FIT image_span=12681100 additive_heap=2358752 margin=1606292 **OK**`
— then died at `trademark_init` with
`Out of memory. Requested sbrk_base 8d0be000, was 8cf5c000, diff 1449984`.

`kb/heap-two-pools.md` exactly: **the margin the ledger prints IS libc's pool,
and the ledger does not model libc's demand.** "OK" means the image and the
fixed reserves fit, not that the program runs.

```
libc peak demand    ≈ 1,606,292 + 1,449,984 = 3,056,276
margin at the opening keep list                3,202,932
⇒ headroom for ANY .bss growth               ≈   146,656 B
```

So the working image has **~146 KB of slack, not 3.2 MB**, and that number —
not `margin=` — is the one to plan `.bss` work against. Two ways to raise it:
cut `DC_ARENA_BYTES` by the measured town arena high-water (unmeasured until
now; a `DC_ARENA_PROBE` run is in flight), or S4.
2. **The harness could not walk.** `DC_AUTOSTART` presses buttons only, so
   every unattended run reached the town and then stood still for 600 s. That
   is why the station roof clip-through has never appeared in a captured frame.
   `DC_AUTOWALK=<N>` synthesises a deterministic 8-direction stick walk;
   verified on a 600 s run with progression intact.
3. **The opcode mix is now printed, and it is state-dominated.** Per town
   frame: `cmds=2867 noop=1 vtx=265 tri=258 dl=250 | cullvis=6 cullrej=3`.
   ⚠️ **Do not price the 2,094 state commands at 12.31 µs/cmd** — that
   coefficient is a fit against TOTAL `cmds`, which correlates with `vtx`, so it
   belongs to the heaviest opcode and to no other.
   ⚠️ **CORRECTED 2026-08-05.** This block used to continue "265 `G_VTX`
   carrying ~6,951 vertices at ~6.9 µs each is ~48 ms, i.e. most of the emu64
   budget on its own." **G1 measured `G_VTX` at 5.40 ms.** The ~48 ms committed
   rule 7's error a second time (a whole-command average applied to a subset)
   and counted `GXPosition3f32` references as loaded vertices. The heavy opcode
   is `G_TRIN_INDEPEND` at 22.25 ms — see the 2026-08-05 section at the top.

Also: `bench_mem` finally builds and runs — and Flycast cannot answer it
(read == write == 114.3 MB/s at every size, in both VRAM windows). It is a
hardware task now, de-risked. `kb/research-ram-tiers.md` has the numbers.

## ⭐ 2026-08-04 — the headline, and it reframes the whole FPS problem

**Current, probe-free, audio off:** FPS p50 **24.3**, p1 11.6, min 10.7. The
town (scene 9) p50 **14.9** and it never exceeds 14.9. `us/v` p50 **3.81 µs**
(was 4.71), `xform` p50 **10.0 ms** (was 12.6), vertex-memo hit rate **48.2 %**.

**THE ARITHMETIC EVERY FPS PLAN MUST START FROM.** The town frame is
**19-27 % renderer** (`gx=`, in `dc/`, editable) and **77-80 % emu64 traversal +
game logic** (in `src/`, closed to editing, compiler flags banned) — at the
median AND at the 1 % lows alike. **Deleting the renderer entirely takes the
worst frames from 11.8 to 15.2 FPS.** The 1 % lows are not a separate problem;
14 of the 17 worst windows are the town, which is the same wall, deeper.

**AUDIO WORKS, AND COSTS 45 % OF THE FRAME RATE.** One jaudio DAC frame is
~19.8 ms of SH-4 for **17.49 ms** of audio, so synthesis runs at **0.88× real
time and needs ~113 % of the machine** to stay level. FPS p50 23.5 off vs 13.0
on. ⚠️ **CORRECTED 2026-08-05** — this line said "~35 ms of audio … ~57 % of the
machine". `DAC_SIZE * 2` (`aictrl.c:292`) is 2,240 **bytes** = 560 stereo pairs
at `JAC_DAC_RATE = 32028.5` = 17.49 ms; the 2× error had been steering audio
decisions. `kb/audio-cpu-cost.md`. **`DC_AUDIO` therefore defaults to 0**;
`-DDC_AUDIO=1` turns it on. No budget setting fixes this — budgeting made it
*worse* (10.9). Root cause of the silence was that the AICA ARM7 **ran and then
wedged** on a Timer-A FIQ that is never delivered.

**⚠️ NEVER build a perf run with `DC_FB_PROBE`** — the dump costs 1,506 ms into
the `vi` bucket and dragged p1 from 11.56 to 8.50 in numbers that had already
been quoted. Screenshot runs and perf runs are different experiments again.

**The open decision is `kb/RESUME.md` §4** — interposing on emu64's dispatch
table (legal by the letter, near the spirit of the `-O0` directive, worth
25-35 ms) versus **F1**, offline bbox-CULLDL injection (no sign-off needed,
10-20 ms, costs 60-120 KB). New research: `kb/research-fps-ideas.md`,
`kb/research-ram-tiers.md`.

| file | read it when |
|---|---|
| `kb/state-log.md` | you need the evidence — what was observed running, when, and what it cost |
| `kb/heap-two-pools.md` | **before touching `DC_ARENA_BYTES` / `DC_ARAM_WINDOW`** or anything that allocates at boot |
| `kb/plan-stages.md` | the agreed S1→S5 RAM plan and the reasoning behind each step |
| `kb/levers.md` | planning any size/RAM work — the ranked ledger of what's left |
| `kb/closed.md` | **before proposing** any RAM/size/architecture idea — what is already dead and why |
| `kb/traps.md` | before touching the build, harness, prelude, or instrumentation |

`CLAUDE.md` is the index to everything else.

## Where the port is

### ⭐ 2026-08-04 — HARDWARE LOADING IS AT PARITY WITH THE EMULATOR

Human report on the `AC-DC-20260804` burn (committed `dev` @ `2fcaa80`,
`DC_CDI_PAD=1`, no `DC_SCIF_FAST`, no autostart): **"loading times have
significantly improved on the hardware… I would say it's at parity with
emulator."**

That **closes the second of the three 2026-08-03 hardware candidates below**.
"The CD-R is simply slow, because the ARAM pager reads at ~500 KB/s with real
seeks while every emulator run has `FastGDRomLoad=yes`" is no longer a live
explanation for anything. What is now known: an emulator timing result about
*loading* transfers to hardware, so the harness is a fair proxy for disc work
and the read-ahead question can stop being hedged.

⚠️ **This does NOT mean the frame rate transfers.** Flycast models no cache and
the town is CPU-bound (`kb/research-fps-ideas.md` F5); the only hardware FPS
figure in the project is still "~11 FPS in the town", and `DC_EMU64_HIST`'s
histogram has never been read on a console. Parity on I/O is not parity on
compute — do not quote this line as evidence for either.

What plausibly earned it, none of it isolated by an A/B (so this is
attribution, not measurement): the disc-backed ARAM pager replacing the window
(2026-08-02), the per-asset `[DC/KEEP]` line being removed from the boot path —
1,392 lines = 86,357 B = **15.0 s of dead boot at 57,600 baud**, with nothing on
screen — and `pvr_init()` being split out as `dc_gx_backend_start()` so the load
happens behind a real progress bar instead of behind a blanked screen.

### 2026-08-03 — IT BOOTS ON REAL HARDWARE, and stops at the K.K. scene

A padded CD-R burn of `dev` boots on the user's retail Dreamcast. Every
observation before today came from Flycast. It reaches the title screen and
stops at the K.K. Slider / player-select scene; three candidates (input never
arriving — that burn had `DC_AUTOSTART` unset; ~~the CD-R simply being slow~~
**[CLOSED 2026-08-04 — see above]**; or a real hardware-only hang) are written
up with the disc built to separate them in `kb/RESUME.md` §1 and
`kb/state-log.md`.

⚠️ **Never compile `DC_SCIF_FAST` into a hardware build** — a coder's cable
will not sync at 1.5 Mbps and the console, crash dumps included, is lost.
⚠️ **`DC_CDI_PAD=1` for burns.** The 740 MB CDI is 314,663 raw 2352-byte
sectors = 69.9 minutes and fits a 74-min CD-R, despite what the byte count
suggests.

### 2026-08-03 — two visible bugs fixed, and the measurement rules that found them

- **The reply box now renders.** It had no assets: `make_stub_data.py` skipped
  `src/game/m_choice.c` because the *`.c`* has no `#ifdef TARGET_PC`, so its
  `.c_inc` — which holds the panel's only texture and its four vertices — was
  never rewritten. `ASSET MISSING` 2 → 0, blank uploads 2/306 → **0/306**.
- **The alpha texture-env fix is in and ON.** 78 % of display-list sites ask for
  alpha = TEXEL0.a alone; the port multiplied by the vertex alpha, which on
  those draws is the `G_RM_FOG_SHADE_A` fog coefficient. The dialogue balloon is
  now a solid cream oval instead of transparent.
- **`tools/dcqa/run_report.py`** is the regression gate; `DC_SCIF_FAST=1` makes
  320x240 screenshots cost ~1.4 s instead of ~35 s, so a screenshot run reaches
  the town like any other.
- **Three rules this cost:** `ASSET MISSING` must be empty before any visual
  comparison is believed; judge a renderer change on a screenshot pair, not on
  counters; and total frames is not a progression metric (the town is 11 FPS and
  the train intro 30, so arriving sooner *lowers* the count — 10,499 vs 7,979 on
  one build).


⚠️ **Updated 2026-08-02 (session 2). The port now REACHES THE TOWN.**
`[SCENE_MODE] 0 → 3 → 4 → 18 → 9`; mode 9 is `mFI_FIELD_FG` +
`mEv_CheckFirstIntro()` TRUE (`m_field_make.c:1292`) = SCENE_FG, the outdoor
field. Title → player-select → train intro with Rover → name-entry keyboard →
town, unattended, in one 600 s run. What unblocked it was **input**, not memory:
`DC_AUTOSTART` alternated START/A 1:1 and past the title almost nothing takes
START. Latest full run: **16,889 frames / 600 s**, town ~12 FPS,
`image_span` 10,699,616 B, margin 3,588,448 B, fit OK.

Human verdict on the current build: K.K. Slider correct, Rover correct in the
train, scrolling trees and glass present. **Still wrong: the train door, and the
scrolling window texture sits entirely above the window.** Both are tracked in
`kb/RESUME.md` §5, items 1-2. The door needs the PVR punch-through list and
cannot be fixed by toggling depth write — both settings were tried and both are
visibly broken, for opposite reasons.

**M0 and M1 are met. M2 has real pixels and is not complete.** The renderer is a
real PowerVR backend (`dc/src/dc_pvr.c` + `dc_pvr_texture.c`). Four renderer
bugs of one family — **GX state recorded by `dc_gx.c` and never consumed by
`dc_pvr.c`** — have now been found and fixed (wrap mode, TEV constants,
`GX_TEXMAP_NULL`, alpha compare, colour update). Sweeping every `g_gx` field for
a *consumer* is the standing technique; fog is the largest one still unread.

- **3917 / 3917 translation units compile and link for sh-elf**, zero
  exclusions. `src/` carries only **four** `#if defined(TARGET_DC)` branches;
  every compat fix lives in `dc/include/dc_prelude.h` as a force-include.
- **The harness works and is verified against real CDIs**, not asserted.
- ~~**The full image still does not fit. That is the only thing between here and
  a playable build**~~ ⚠️ **VOID 2026-08-06.** Real headroom is ~2.05 MB and the
  binding constraint is **residency**, not fit — see the top of this file. And
  the opposite extreme is closed by experiment: a full non-stub image fails a
  15,638,528 B contiguous malloc and loads **nothing** (`kb/closed.md`). The
  renderer, the platform layer and the boot path are all observed working.

The build that renders (2026-08-02, session 2 — supersedes the older line):

```bash
DC_STUB_KEEP="$(grep -v '^#' tools/dcstub/keeplist-opening.txt | paste -sd: -)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=131072 DC_ARENA_BYTES=1900000 DC_AUTOSTART=300 \
  bash dc/build-dc.sh
bash harness/dc/smoke.sh dc/build/OpenCrossing.cdi --timeout 600 -c config:LimitFPS=no
```

**600 s and `LimitFPS=no`, not 180 s.** The town is ~4,000 frames in; a short
run stops in the train intro and reads as a progression regression. Build to a
*copy* of the CDI before a long run — Flycast holds the file open for the whole
run.

⚠️ A game smoke run **always** exits 1 with `status=exited_early` — the game
never returns, so the end-marker checks cannot pass. The console log is the
artefact. See `kb/traps.md`.

## The one inequality

State the fit as **one inequality, never two pools**. Splitting it into an
"image budget" and a "heap budget" has already produced two wrong numbers
(14,451,476 and 11,068,532).

```
(image span) + (genuinely additive heap) ≤ 16,646,144

  image span        18,993,020   post-P7 + ARAM-pager (+4,604), 2026-08-02
                                 pre-pager 18,988,416: text 5,804,776 /
                                 data 2,337,976 / bss 10,837,376, _end 0x8d22bd80
  additive heap      2,358,752   KOS 262,144 + arena 1,900,000
                                 + ARAM LRU 131,072 + threads 65,536
  ⇒ over by          4,705,628
```

At the *policy* arena (2,705,504; ARAM is now fixed at 131,072) it is over by
5,511,132. These supersede the 6,999,924 / 6,424,032 / 5,421,920 that older
docs and earlier versions of this block quote.

⚠️ **Correction, 2026-08-02:** an earlier version of this block said the
pre-P7 span was 18,997,600 and the gap 5,431,104. That was an arithmetic slip
— `0x8d265f60 - 0x8c010000` is **19,226,464**, which is what the older docs
said all along. The `size` "dec" column is not the span: it omits inter-section
alignment and it counts `.ocram`, which lives at `0x7c001000` and is not in the
image at all. **Take the span from `_end` minus `0x8c010000`, never from
`dec`.**

`.text` + `.data` = 8,142,752 B and neither can shrink — `-O0` is mandatory, so
`.text` can only be *relocated*. The lever big enough is demand-loading the
8,771,358 B of asset destination arrays (`kb/levers.md` L1), **but the pool it
loads into is additive heap**, which is what makes S4's pool size the binding
constraint. `dc_mem_ledger.c` prints this line at boot as `MEMLEDGER FIT …`.

⚠️ **Measure only against a clean rebuild.** `dc/build/flags.stamp` now forces
one when flags change; before that fix a stale `dc_main.c.o` made a non-stub
ELF read 356,776 B too small.

⚠️ **The ARAM debt is paid. `DC_ARAM_WINDOW=131072`.** PLAN §3.1's disc-backed
pager landed 2026-08-02 (`dc/src/dc_aram.c`, `dc/include/dc_aram_lru.h`, the
provenance ring in `dc_dvd.c`, kill switch `DC_ARAM_LRU=0`). The window is no
longer a window: an extent map learned from the write stream maps ARAM ranges
onto byte ranges of `/cd` files, and a read miss is one `fs_read` into the
caller's buffer. The pool is a 4 × 32 KB LRU cache. **−720,896 B of additive
heap, +4,604 B of image span, net −716,292 B off the gap**; `MEMLEDGER FIT`
additive_heap 3,079,648 → **2,358,752**, verified in this tree. The old 851,968
"floor" rested on a wrong measurement — `forest_1st.arc` arrives as 26 transfers
of ≤32,768 B over one *contiguous* 851,744 B extent, not as one transfer.

## Latest measurements (2026-08-02) — full narrative in `kb/state-log.md`

- **Arena, first real measurement.** `DC_ARENA_PROBE=<frames>` reports the
  game's own allocator. At the title screen: **used 256,192 B of a 1,412,704 B
  zelda arena**, inside a 1,900,000 B knob, while libc had taken 2,666,496 B
  from sbrk. The arena is not where the pressure is. **Title scene only** — a
  loaded town is unmeasured, so this licenses a smaller *bring-up* arena, not a
  smaller shipping one.
- **Asset working set, textures.** `DC_ASSET_CENSUS=1` +
  `tools/dcstub/census_resolve.py`: the title screen touches **50 symbols /
  111,136 B** of real texture bytes — the logo glyphs, all seven `obj_train1_t*`
  textures, `grl_1_*`, and the animals' eye/mouth TA textures — against the
  4.6 MB of texture destinations the image keeps in `.bss`. Strongest evidence
  yet for `kb/research-creative-ram.md` T1.
- **The vertex/model half is measured: the whole title-screen working set is
  93,312 B** — 30,688 B of models (58 `gsSPVertex` batches, 18,720 B actually
  read) plus 62,624 B of textures and palettes, against **8,771,358 B** of asset
  destination arrays in `.bss`. `GXSetArray` is **dead in this game**, not just
  quiet: its only call site anywhere in `src/` is `GXInit.c:252`'s own reset
  loop. The seam that works is `OSs16tof32()`, whose only three calls in the
  emu64 TU are the three components of a source vertex; `dc/include/dc_census_vtx.h`
  wraps it and `dc/Makefile` force-includes it into that one TU.
- **The framebuffer probe is attributed, not fixed.** `PVR_FB_R_SOF1` reads
  `0x000E7480`: the display scans out 947,840 B into VRAM, and `vram_s` has
  never been the displayed surface. `FBNONZERO` is the assertion to trust; a
  hash cannot tell black from wrong-address.

## ⭐ 2026-08-02 (latest) — the button got pressed. The port is in the train intro.

`kb/boot-blockers.md`'s three cheap wins all landed, and the first was worth
much more than "an unattended START": **the game leaves the title screen and
reaches the player-select scene — the train intro, with Rover, dialogue frames
and real textures.** Confirmed both in the console census and by a human
watching Flycast. Narrative and numbers: `kb/state-log.md`, top entry.

```bash
python3 tools/dcstub/census_resolve.py <run>/console.log \
    --sizes-from dc/build/nonstub/AnimalCrossing.elf --top 0 > /tmp/census.txt
DC_STUB_KEEP="$(python3 tools/dcstub/census_keeplist.py /tmp/census.txt \
                 --with-default --colon)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=131072 DC_ARENA_BYTES=1900000 DC_AUTOSTART=300 \
  bash dc/build-dc.sh
```

- **`DC_AUTOSTART=<N>`** (`dc/src/dc_pad.c`) synthesises START/A pulses from
  `PADRead` call N. ⚠️ **`DC_AUTOSTART_PERIOD=24` is WORSE than the default 90**
  — measured: pressing every 0.8 s stalled the run at 38 draws instead of
  reaching the train. Dialogue needs press/release edges, not a held button.
- **Console flood limiter** (`dc/src/dc_misc.c`, a `printf` override + the same
  table from `OSReport`): `SendStart::Mesg Full Queue` 741 → 15 lines.
  `DC_CONSOLE_LIMIT=0` reverts. Two ways to get this wrong are in `kb/traps.md`.
- **`OSGetSoundMode()` → stereo** (`dc_stubs.c`). It returned mono and
  `src/audio.c:147` hard-locked the game to mono off that.
- **The cull mapping was inverted** (`dc_pvr.c`) — `GX_CULL_BACK` mapped to
  `PVR_CULLING_CCW`, which KOS defines as "cull if the screen-space determinant
  is negative", and `emit_projected` already negates Y. So the port culled
  exactly the faces it should keep and every character rendered inside-out. The
  title screen was unaffected the whole time because the logo draws with
  `GX_CULL_NONE`. See `kb/traps.md` → Renderer.
- **The keep list is now generated, not written.** `tools/dcstub/census_keeplist.py`
  joins a census to the linked map and emits `DC_STUB_KEEP` (66 files,
  `dc_stub_keep.inc` 546 rows / 390,848 B; `.bss` +322,112, `.text` +37,280).
- **`DC_FB_IMAGE=<1|2|4>`** dumps the whole framebuffer as base64 rows;
  `tools/dcfb/fbimg_to_png.py` decodes a run into PNGs. Built because every
  rendering question past "is it black" was being answered by a human watching
  the emulator. **Now run end to end** — the port has screenshots.
- **The GX wrap mode is honoured** (`dc_pvr.c`, `dc_gx.c`). It was stored and
  never read, so every texture repeated. The opening's spotlight was drawn 2.7
  times across the frame; it is now one cone over a legible floor with a
  silhouette in it. Same keep list, same draw counts (96/49, q/t unchanged) —
  a clean A/B. Kill switch `DC_XDEFS='-DDC_PVR_NO_UVCLAMP'`.
- **TEV constant colours reach the vertex** (`dc_pvr.c`, N3's first slice).
  `g_gx.tev_colors[]`/`tev_k_colors[]` were stored and never read, so a
  combiner whose colour is `PRIMITIVE` or `ENVIRONMENT` got the texture's own
  RGB instead. The opening's shade quad is
  `gsDPSetCombineLERP(0,0,0,PRIMITIVE, 0,0,0,TEXEL0)` with `PRIM = BLACK`
  (`grd_player_select.c:69`), so a black vignette rendered as a **white** one:
  27.9 % of the frame at pure `0xFFFF`, now **0.0 %**. Only the
  `a=b=c=ZERO, d=<const>` shape is recognised; everything else keeps the raster
  path. Kill switch `DC_XDEFS='-DDC_PVR_NO_TEVCONST'`.
- **`DC_XDEFS`** passes raw `-D` flags through `dc/build-dc.sh` into the
  Makefile, so the renderer kill switches are reachable from a command line
  instead of by hand-editing. Verified reaching 3920 compile lines.
- **The dialogue balloon renders.** Its arrays live in `m_msg_data.c_inc`, and
  `make_stub_data.py` globbed `*.c` only, so that TU never entered `stub.list`
  and `census_keeplist.py` dropped it. Both tools now handle `.c_inc`:
  `cinc_includes()` rewrites and shadows them (`-I$(STUBDIR)/include`, the
  mechanism `DC_SRC_SHRINK` already used) and the keep list no longer drops
  non-stubbable sources. The balloon textures went from **0/8192 non-zero
  texels to 6475/8192**.
- **The keep list was re-censused and the animals have textures.** The old
  31-file list covered 90 asset loads; the regenerated 76-file list covers
  **779**, and blank texture uploads fell from **77/117 to 11/119**. The
  "animal textures used to work and now don't" report was not a renderer
  regression at all — those textures were never in the image, and a stubbed
  asset decodes to a transparent rectangle that draws as a black silhouette.
  The scene now renders K.K. Slider, his guitar, the stage floor and readable
  dialogue. `image_span` 10,239,776 → 10,622,368 B, margin 3,665,024 B, fit
  still OK, 29.3 FPS unchanged. List checked in at
  `tools/dcstub/keeplist-opening.txt` with the command to reproduce it.
  The residual 15 are outdoor acre/scenery textures this indoor scene loads but
  never draws.
- **`DC_TEX_LOG=1`** logs what each texture upload actually decoded to
  (non-zero texel count, value range, distinct values, palette head). This is
  what separated "missing asset" from "renderer bug" — see `kb/traps.md`.
- **`DC_PVR_BATCH_LOG=<N>`** dumps every batch's state — tex/wrap/blend/cull/z
  plus the screen bbox and z range of what was actually emitted — on every Nth
  frame. Pair it with `DC_FB_PROBE` at the same N and a region of a decoded PNG
  can be attributed to the state that drew it. This is what found both bugs
  above; the `[PERF]` draw counters cannot distinguish "submitted" from "on
  screen".

### A correction to `kb/boot-blockers.md` item 5

`SCENE_PLAYERSELECT` does **not** gate on the memory card.
`aNPS_setup_game_start` waits on `mCD_InitGameStart_bg()`, and while
`src/game/m_card.c:5096` is a 10-step card state machine,
**`pc/src/pc_m_card.c:1188` overrides that symbol** (the link carries
`--allow-multiple-definition`) and returns `mCD_TRANS_ERR_NONE`
unconditionally. The gate is the dialogue FSM and a 440-frame timer — i.e.
input. `[PC] toNextLand: keepSave not set, aborting` is likewise not a blocker;
it is the town-to-town transfer path with no save.

### The number the plan was waiting on — answered

**93,312 B.** A title-screen-complete build does not need S4. Extending
`DC_STUB_KEEP` with the censused models and textures is the next concrete step
(N1 item 2 below).

## ⭐ Ranked next actions — SUPERSEDED (2026-08-02, session 2)

**Read the top of this file, or `kb/RESUME.md` §5.** That list predated the town
being reachable, and all four of its items (punch-through, the window scroll's
UV offset, fog, the speaker-name text) have since landed. Its audio note is
superseded by `kb/audio-plan-of-record.md` and by the 2026-08-05 DAC-frame
correction above.

## Ranked next actions (2026-08-02, session 1) — the list before parking

The S1→S5 plan in `kb/plan-stages.md` is still the RAM strategy and is not
superseded. These are the concrete next moves now that pixels exist.

### N1. Get the town to draw. [reframed 2026-08-02 — the texture half is measured, the vertex half is not]

The keep-list-by-hand plan is dead: the title demo's acres and animals are
named by index and profile ID, so there is no static list to extend from.
`DC_ASSET_CENSUS=1` replaces it and already answered the texture side (50
symbols, 111,136 B — see "Latest measurements" above). Two follow-ups, in order:

1. ✅ **The vertex side is censused** — 93,312 B for the whole title screen, no
   `src/` edit needed. See "Latest measurements".
2. **Extend `DC_STUB_KEEP` from the measured list.** 93 KB fits trivially, so a
   **title-screen-complete build should land without S4**. The census names the
   models: `boy_1_v`/`grl_1_v`, `mnk_1_v`, `wol_1_v`, `dog_1_v`, the three
   `obj_train*_v`, five `logo_us_*_v`, `ef_hanabira01_00_v`, `ef_shadow_out_v`.
   ⚠️ Animal species attribution is **not decidable on a stub image** — every
   `xxx_1_v` is 16 B apart and all species share a rig, so 4 of 58 batches are
   ambiguous and some "certain" animal hits may be aliases. Keeping the named
   models gives them real sizes and spacing, which makes the join exact: do that
   first, then re-run the census to confirm before trusting the list.
3. Then re-run on a **town** scene — that is what actually sizes S4's pool, and
   the batch table caps at 1,024 (`full=0` on the title screen; a town run must
   check that counter).

### N2. ✅ DONE 2026-08-02 — the unattended visual gate works.

```bash
bash harness/dc/smoke.sh dc/build/OpenCrossing.cdi --fb-golden 25789d43
```

`fb_saw_pixels` and `fb_golden` come back in the JSON. The golden is the stub
title screen at `DC_ARAM_WINDOW=851968 DC_ARENA_BYTES=1900000`;
`FBNONZERO 13711 of 307200`, reproduced across four runs. The decoded 16×12
thumbnail shows two centred text bands in the lower third over black — "PRESS
START" above the copyright line, which is what a human reported seeing.

**`--fb-writeback` is required, not optional**, and my earlier note in this file
saying otherwise was wrong: without it every candidate surface reads zero, with
it the scanout surface reads real pixels. `0xA5000000 + FB_R_SOF1` was the right
address all along; the 64-bit-aperture hypothesis was wrong and Flycast's
32-bit aperture merely mirrors every block at +4 MB. The frame-rate cost of the
flag is **unmeasured** — the old 24.8 → 16.8 FPS did not reproduce and no
controlled pair exists.

### N2b. Wire the game's save path to the VMU. [the backend is real; nothing calls it]

`dc/src/dc_card.c` is a working KOS `vmufs`/`vmu_pkg` backend, proven in Flycast
and re-verified host-side out of the flash image. **But the game never calls
it:** `pc/src/pc_m_card.c` does its I/O with `<stdio.h>` against the relative
path `save/card_a/DobutsunomoriP_MURA.gci`, so of the 29 `CARD*` entry points
only `CARDInit()` is on its path and `[PC] No save file found` is a failed
`stat()`. `pc_card_scan_for_gci()` deliberately still returns 0 — returning a
path would make the game print "found" and then fail to `fopen` it.

The designed fix is `kb/save-plan.md` §7.8: a KOS `vfs_handler_t` mounted at
`/dcsave` plus an `fs_chdir()` from `dc_main.c`, committing to the VMU on the
`->rename` callback — which is exactly `pc_save_write_gci_ex`'s last step, so
it is a free atomic commit point with no edit to `pc/`.

**Measured and load-bearing:** a VMU block costs **84.6 ms** to write
(`write ≈ 0.678 s + 84.6 ms/block`, within 1.3% of the KOS-source ceiling), so a
150-block save is **13.4 s**. Incremental writes are mandatory and the shipped
chunking does *not* deliver them — `vmufs_write()` rewrites the whole file. The
format is byte-stable; the writer needs a block-diffing pass. Deflate-6 on SH-4
is 295,910 B → 99,657 B in 0.129 s, i.e. compression is free next to the flash
cost. ⚠️ Every compression ratio so far is against **synthetic** data; a real
`.gci` is the top open item.

### N3. Correct the TEV mapping. [the logo renders; is it renders *right*?]

`dc_pvr.c` implements exactly one TEV configuration — modulate texture by
rasterised colour — against the 101 in `kb/tev-map.md`. Konst-colour and
multi-stage configs currently come out untinted. Now that something is on
screen, this is measurable for the first time: instrument which of the 101
configs the title screen and the field actually request, and implement by
frequency rather than by enum order.

### N4. Measure bucket 6 properly. [PARTLY DONE 2026-08-02 — title scene measured, gameplay is not]

`DC_ARENA_PROBE=60` reports the game's own allocator every 60 presented frames.
At the title screen: **used 256,192 B of a 1,412,704 B zelda arena**, inside a
1,900,000 B arena knob, while libc had taken 2,666,496 B. No bisect needed and
no arena-side OOM is reachable from here.

What is left is the part that decides the shipping number: **the same probe on
a scene that has a town loaded**, which does not exist yet. Until it does, do
not cut `DC_MAIN_MEMORY_SIZE` on the strength of the title figure — cut it for
bring-up images if libc needs the room, and re-run the probe the moment S4
loads real field data.

### N5. Then S4 — the asset loader. [unchanged, still the critical path]

`kb/plan-stages.md` S4 still applies in full — read it before starting. Two
things changed since it was written: the pool must be sized against the
**libc** side of the split, not treated as a free-floating extent
(`kb/heap-two-pools.md`); and the S3 remainder is smaller than billed — P6
measured −598,424 B, not −821,569.

### Also worth knowing

- `SendStart::Mesg Full Queue` spams the console ~1,000 times per run. It is
  jaudio, it is not fatal, and it makes logs hard to read. Worth silencing.
- **The ARAM window no longer thrashes** — `rebases=14` → the concept is gone.
  Verified in this tree: all 4,982,400 B of graph-half writes mapped
  (`forest_1st` + `forest_2nd` exactly), **`LOST=0`, 0 reads zero-filled** (was
  ≥5,121), 2 extents, `pin_peak=0`. Grep `[DC/ARAM] LRU` — `LOST=` must be 0.
  ⚠️ **Real archive content now reaches the renderer, and it costs: 29.3 →
  12.6 FPS**, `cmds` 851 → ~3,600, textures 78/301,312 B → 173/676,608 B.
  Measured *not* to be disc — at the same frame index under `--no-fast-gdrom`
  the frame is 100.6 vs 66.5 ms with `gx` only 29.0 vs 26.0, so ~31 ms/frame is
  SH-4/emu64 work on content the port used to throw away. **That is the next
  performance question and it is a new one.**
  ⚠️ `pin_peak=0` only because the save path never runs. `m_card.c:1607`'s three
  ARAM save blocks are 147,782 B of writes with no disc provenance; raise
  `DC_ARAM_WINDOW` to 262,144 when the VMU write path is wired up (N2b).


## Toolchain

`opencrossing-dc:sdk` in the local Docker daemon — **do not rebuild, ~27 min
cold**. sh-elf GCC 15.2.0, newlib 4.6.0.20260123, binutils 2.45.1, KOS 2.3.0
(`1c6398f9`), kos-ports (`f4faacc4`), GLdc (`a1cd80a8`), mkdcdisc (`3c2ef63a`),
`-m4-single`, thread model kos. Clean build ≈ 97 s for 3917 TUs + link + CDI at
`-j4`. Entry points, every env knob and the flag assembly: `BUILDING-DC.md`.
Gotchas: `kb/traps.md`.

## Standing constraints

`CLAUDE.md` §1 is authoritative and must not be restated differently here.
The short version: stock 16 MB (the 32 MB mod must never become a requirement),
**`src/` at `-Os` with a reviewed `-O3` hot list — `DC_OPT_PROFILE`,
`dc/opt-lists.mk`, and `DC_OPT_PROFILE=o0` is the byte-identical revert**, never
edit `src/`, never commit ROM material or disc images, every optimization gets a
kill switch, agents do not run git — the main thread commits.

⚠️ **The two sentences that used to close this file said `-O0` was mandatory and
that "quietly reopening the optimization question" was dishonest. Both are
reversed** (2026-08-06); the post-mortem is in `kb/closed.md`, and its lesson is
that a directive can settle a preference but not a fact about a compiler nobody
had run.

**Be honest in reporting.** A negative result is a result: the non-stub boot
that proved "just put the assets in RAM" impossible is worth more than the
estimate it replaced. If a lever does not deliver, cutting content
(`kb/levers.md` L5 — the user's call, not engineering's) is an honest option;
quoting an unmeasured number as if it were measured is not.
