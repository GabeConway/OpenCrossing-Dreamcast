# S14 — the hardware-shaped batch, and how to take any of it back

Landed 2026-08-09 in one pass, on a user directive. **Seven changes shipped; an
eighth (S14-4) was proven a no-op by its own gate the same day and reverted —
§2b.** The directive was: *"do everything in the
backlog in one shot, then test it all in one big bunch, make sure it can be
rolled back … the goal is making this game work smooth on real hardware, which
we can't test in Flycast anyway."*

**This file is the rollback contract.** Every row below is independently
revertible with one `-D`, and the whole batch is revertible with one line. If
you are reading this because something looks wrong on a TV, go straight to
§4 and bisect — do not start by reading renderer source.

Numbers: `kb/STATE.md`. Evidence and narrative: `kb/state-log.md`.

---

## 1. The directive was aimed at the wrong 3 %, and this is why the batch
## looks nothing like "use sh4zam"

The ask was to focus on sh4zam. That was re-costed against this tree's own
`[VTXSPLIT]` split before any code was written, and it does not survive:

- **Every floating-point stage of the vertex path is ~0.76 ms of a ~29 ms
  frame — 2.6 %.** `xf`, the position FTRV, is **0.22 ms**. A *perfect* matrix
  rewrite cannot pay more than that.
- **sh4zam already contributes zero instructions to the shipping image**
  (`sh-elf-nm`), and we already emit FTRV / FIPR / FSRRA / FSQRT — through KOS.
  The gap was never "we are not using the vector unit".
- The one sh4zam-flavoured idea whose *mechanism* fits a memory-bound frame is
  G-E: overlapping FTRV latency with **stores**, not doing less arithmetic.

So the batch is aimed where the milliseconds are: **memory traffic, cache
layout and removed work.** ⭐ **The single most likely explanation for "hardware
is much worse than the emulator" is the instruction cache — 8 KB,
direct-mapped, against ~2.5 MB of `.text` — and Flycast models no cache at all,
which is exactly why nobody has ever seen it.** F5 attacks that directly.

⚠️ **This is a re-costing, not a refutation of sh4zam.** G-E and G-G are still
live and still unbuilt (`kb/research-sh4zam-gap.md` §5).

---

## 2. What landed

| # | change | file | mechanism | kill switch |
|---|---|---|---|---|
| **S14-1** | memo value array gets a **32-byte stride** | `dc/src/dc_pvr.c` | `ClipVtx` is 28 B, so **7 of 8 memo entries straddled two cache lines** on every hit. Costs 512 B of `.bss` | `-DDC_PVR_NO_MEMO_ALIGN` |
| **S14-2** | drop the per-vertex `oargb` store | `dc/src/dc_pvr.c` | the TA reads offset colour **only** when `PVR_TA_CMD_SPECULAR` is set, which only `-DDC_PVR_TEVP3` ever does. One of eight store-queue words, discarded by hardware | `-DDC_PVR_NO_OARGB_SKIP` |
| **S14-3** | `pref` the next source vertex | `dc/src/dc_pvr.c` | SH-4 has **no hardware prefetcher**; `DCGXVertex` is exactly one 32-byte line since session 12, so one `pref` covers a whole vertex. One line ahead, because only one prefetch may be in flight | `-DDC_PVR_NO_PREFETCH` |
| ~~**S14-4**~~ | ~~skip `GXNormal*` on unlit batches~~ 🔴 **REVERTED THE DAY IT SHIPPED — PROVEN A NO-OP, see §2b** | `dc/src/dc_gx.c` | — | now opt-**in**: `-DDC_GX_NRMSKIP` |
| **S14-5** | Gribb-Hartmann frustum planes + positive-vertex test | `dc/src/dc_gx.c` | ~200 unconditional multiplies with no early-out → one 48-multiply fold plus 3 multiplies per plane, returning on the first rejection | `-DDC_GX_NO_GHCULL` |
| **S14-6** | decal-Z arming default **ON** | `dc/src/dc_emu64_cull.cpp` | +48 % armed batches for the G-B side channel. Gate had already passed; what kept it off was a *Flycast* noise floor, and the thing it deletes is a cache miss Flycast does not model | `-DDC_GX_NO_VTXID_DECAL` |
| **S14-7** | **F5** — linker section ordering | `dc/section-order.txt`, `dc/Makefile` | packs the innermost draw loop contiguously so the interpreter and the GX layer stop **conflict**-evicting each other | `DC_SECTION_ORDER=0` |
| **S14-8** | `cus=` — a timing bracket on G3's cull | `dc/src/dc_emu64_cull.cpp` | instrument, not an optimization. See §5 | `DC_PERF_PHASE` only |

---

## 2a. WHAT FLYCAST SAID: A WASH — AND THAT IS THE EXPECTED RESULT

600 s, town, static camera, `DC_PVR_VTXSPLIT=16`, `-DDC_PERF_PHASE`,
`smoke-s14-20260809-130522`:

| | session 13 (pre-S14) | **S14** |
|---|---:|---:|
| `us/v` | 2.51 | **2.48** |
| `xform` ms | 6.9 | 7.0 |
| memo hit | 53.7 % | **54.2 %** |
| `memo / shade / emit` ms | 1.20 / 1.82 / 1.40 | 1.23 / 1.82 / 1.41 |
| `vid` armed | 1800 / 61920 | **2670 / 73080** |

**−1.2 % on `us/v` is INSIDE the ±2 % floor (rule 11), and every `[VTXSPLIT]`
bucket is within 0.03 ms.** Read that as "no resolvable change in the
emulator", not as "no change".

⭐ **This was predicted for four of the seven and it is not a defence for the
rest.** S14-1, -3, -6 and -7 all pay in cache misses, and **Flycast models
no cache of either kind** — the same reason G-B(1) is documented as a floor
there. The two counters that CAN move in an emulator both did, in the right
direction and by the right amount: the arming reach hit `vid=2670/73080`
against a prediction of `2670/72810`, and the memo hit rate rose 0.5 points.

⚠️ **AND TWO ITEMS TURNED OUT TO BE AIMED AT NOTHING MUCH. Both were ranked in
the kb without anyone checking the denominator — one of them at nothing at
all:**

1. **The lit fraction is VIEW-DEPENDENT, and one static view is not a
   workload.** ⚠️ **This was measured as S14-4's reach and that framing is WRONG
   — see §2b: emu64 gates the call on a DIFFERENT predicate and S14-4 is dead.**
   The distribution is kept because it is a real property of the town that
   anything keying on lit-vs-unlit will need:

   | run | windows | min | p50 | max | **mean** |
   |---|---:|---:|---:|---:|---:|
   | `smoke-s14` (600 s) | 478 | 0.0 % | **93.4 %** | 96.2 % | 75.7 % |
   | `smoke-s14b` (240 s) | 176 | 0.0 % | **54.4 %** | 96.1 % | 61.3 % |

   ⚠️ **A p50 of 93.4 % and a p50 of 54.4 % on the same build is the lesson**:
   the town reseeds per boot AND the camera decides, so `vlit/v` must be quoted
   as a distribution, never as a number. (An earlier draft of this file said
   "the town is 93.4 % lit" off the first run alone. It is not.)
2. 🔴 **S14-5's target is 0.14 ms.** See §5 item 2 — the `cds=`/`fus=` split
   retired it, and G-F with it.

**The batch's real verdict is a burn** (§7). Nothing above is evidence against
it, and nothing above is evidence for it either.

---

## 2b. 🔴 S14-4 IS DEAD — THE GATE KILLED IT, AND THAT IS THE GATE WORKING

`-DDC_GX_NRMSKIP_VERIFY`, 360 s town, `smoke-gate-20260809-132911`:

```
[GXVERIFY] nrmskip=0 nrmskipchk=427327 nrmskipbad=0 ghcullchk=1576071 ghcullbad=0
```

**427,327 batches offered a normal and NOT ONE was skippable.** The reason, found
in the decomp *after* the counter said so — **emu64 already guards the call**
(`src/static/libforest/emu64/emu64.c:2785-2787`):

```c
/* If geometry mode lighting is enabled, write vertex normals */
if ((this->geometry_mode & G_LIGHTING) != 0) {
    GXNormal3f32(emu_vtx->normal.x, emu_vtx->normal.y, emu_vtx->normal.z);
}
```

An unlit batch never reaches `GXNormal*` at all. The work this was written to
remove had already been removed **upstream, in `src/`, by the original
developers**, and all the switch could do is add three `g_gx` loads and a branch
per call to reach a `return` that never happens. **Default flipped to OFF; it is
now opt-in via `-DDC_GX_NRMSKIP`, and there is no reason to.**

⚠️ **`kb/research-sh4zam-gap.md` G-J ranked this wrong — not by degree, but
entirely.** It said: *"emu64 calls `GXNormal3f32` thousands of times a frame to
fill a field the backend discards when `need_light` is false. Skipping it for
unlit batches is legal — that is a work-removal idea and it outranks every
arithmetic idea in this table."* The premise was never checked against the call
site. **Check the CALLER before costing a callee's skip.**

⚠️ **And it invalidates §2a item 1's framing too.** The `vlit/v` distribution is
real and worth knowing, but it was the wrong denominator for this change: the
predicate that matters is emu64's `G_LIGHTING` geometry-mode bit, not `dc_gx`'s
channel state. Two different predicates, and only one of them gates the call.

**The code and its gate are kept, not deleted.** They are the evidence, and the
counter is what anyone re-proposing this idea should be shown first.

---

## 3. The gates — run these before believing any of it

Three of the eight can be **silently wrong** rather than visibly broken, so
each carries a counter that must read zero. They are separate builds and every
one of them is **slower by construction**; never quote `us/v` off a gate build.

```
DC_XDEFS='-DDC_PERF_PHASE -DDC_GX_NRMSKIP_VERIFY -DDC_GX_GHCULL_VERIFY -DDC_GX_VTXID_VERIFY'
```

| counter | where | must read | what a non-zero means |
|---|---|---|---|
| `ghcullbad=` | `[GXVERIFY]` | **0** | S14-5 disagreed with the 8-corner oracle — geometry is being culled or drawn wrongly |
| `nrmskipbad=` | `[GXVERIFY]` | **0** | channel state changed **between** staging a vertex with a skipped normal and submitting it. S14-4 is unsound in this build |
| `vidbad=` / `over=` | `[EMU64C]` | **0** | the side channel desynced — a vertex got **another vertex's** transform. Applies to S14-6's widened reach |
| `nrmskipchk=` / `ghcullchk=` | `[GXVERIFY]` | **large** | ⚠️ a `bad=0` next to a `chk=0` proves nothing — it means the gate never ran |
| `reinst=` | `[EMU64C]` | **0** | G1/G2 evicted G3's trampolines; a build-order bug, not a repair |

⚠️ **`-DDC_EMU64_CULL_VERIFY` cannot certify S14-6** — it only checks batches we
*cull*, and a decal batch never culls. `-DDC_GX_VTXID_VERIFY` is its gate.

⚠️ **Counters are the floor, not the verdict** (measurement rule 2). S14-4 and
S14-5 both change what is *drawn* if they are wrong, and `tools/dcqa/run_report.py`
cannot see colour. A screenshot pair is still owed.

---

## 4. Rollback

**The whole batch, one line** — every switch off, back to the pre-S14 renderer:

```bash
DC_SECTION_ORDER=0 \
DC_XDEFS='-DDC_PVR_NO_MEMO_ALIGN -DDC_PVR_NO_OARGB_SKIP -DDC_PVR_NO_PREFETCH \
          -DDC_GX_NO_GHCULL -DDC_GX_NO_VTXID_DECAL' \
bash dc/build-dc.sh
```

**Bisecting a visual fault**, cheapest first — this is the order to try, and the
reasoning is "how much can this change what reaches the TA":

1. `-DDC_GX_NO_GHCULL` — the only change that can delete a whole batch.
2. `-DDC_GX_NO_VTXID_DECAL` — the only change that can hand a vertex another
   vertex's transform, and decal-Z is z-biased geometry, so look at ground
   decals, shadows and text on surfaces.
3. `-DDC_PVR_NO_OARGB_SKIP` — only reachable at all in a `-DDC_PVR_TEVP3` build.
4. `DC_SECTION_ORDER=0`, `-DDC_PVR_NO_MEMO_ALIGN`, `-DDC_PVR_NO_PREFETCH` —
   **cannot** change a rendered byte. Layout and cache only. If one of these
   "fixes" a visual fault, the fault is a latent bug being reshuffled, not
   caused, and that is a much more serious finding than a bad optimization.

⚠️ **`DC_SECTION_ORDER=0` relinks without recompiling 3,900 TUs**, by design —
it is keyed to `dc/build/link.stamp`, not to `flags.stamp`. Everything else in
the table above is a `DC_XDEFS` change and therefore a **full rebuild**.

---

## 5. Three things this batch found that were WRONG in the kb

Recorded here because each was believed, quoted, and load-bearing.

1. 🔴 **The i-cache pressure figure excluded the entire interpreter.**
   `tools/dcopt/icache_map.py`'s default hot set matched `^_dl_G_`, `^_emu64`
   and `^_cu_trin` — but emu64 is **C++ and every handler is mangled**. Verified
   against the linked map: `.text._ZN5emu64*` sections **105**,
   `.text.dl_G_*` sections **0**. So the published **11.9×** and "inner loop
   1.4×" were computed with emu64 — most of the draw — absent.
   **Corrected: 16.40× the icache, inner loop 2.62×.** The pressure is worse
   than believed *and* F5's ceiling is lower: at 2.62× the inner loop can be
   made contiguous but can never be made resident.
2. 🔴🔴 **G3's cull had never been timed, and when it was, THE FRUSTUM TEST WAS
   NOT THE COST — emu64's own state calls were.** `dc_emu64_cull.cpp` contained
   **zero** `dc_time_us()` reads. S14-8 added `cus=` and it read **6.8 ms/frame,
   23 % of a 29.4 ms draw** — which reads as "the frustum test is enormous" and
   is not what it says. Splitting the bracket three ways settled it
   (`smoke-s14b`, 176 windows):

   | bucket | what it wraps | ms/frame (all windows) | ms/frame (cull-heavy) | share of `cus` |
   |---|---|---:|---:|---:|
   | `cus=` | all of `cull_batch()` | 3.05 | 7.26 | 100 % |
   | **`cds=`** | **emu64's `dirty_check` + `setup_1tri_2tri_1quad`** | **2.23** | **5.26** | **73 %** |
   | **`fus=`** | **`dc_gx_aabb_is_offscreen()` alone** | **0.139** | 0.292 | **4.5 %** |

   ⭐ **THE FRUSTUM TEST IS 0.14 ms OF A ~29 ms FRAME — UNDER 0.5 %.** That is
   the entire target of G-F *and* of S14-5, measured at last. **G-F is retired
   in both shapes** (the FTRV form too): there is nothing there to win.
   ⭐ **What IS there is `cds=` — 2.2 ms/frame, 73 % of G3's entry cost, and
   nobody has ever costed it.** It is the price of the ordering rule: the
   frustum test reads `g_gx.projection_mtx` and `g_gx.current_mtx` live, so G3
   must make emu64 refresh them *before* it can test anything. And in a typical
   window only **175 of 2,572** TRIN batches are culled, so ~93 % of that
   2.2 ms is spent on batches whose original handler then calls the same two
   functions again. The in-file comment asserts the second call is "idempotent";
   **idempotent is not the same as cheap, and that has never been measured.**
   → the next real renderer lever after G-B(2), and 15x G-F's.

   ⚠️ Read `cus=` against `trin=` on the same line: "visible" is the frustum
   test's most expensive answer (no early-out), so a cull-heavy view is cheaper
   per batch, and two towns are two workloads.
   ⚠️ The three brackets nest, so `cds=`/`fus=` each carry one extra
   `dc_time_us()` pair and `cus=` three. Fine for apportioning 3 ms; **not** fine
   for costing a change worth a few hundred microseconds.
3. **A skipped normal reads ZERO, not stale.** `GXPosition3f32` clears
   `normal[0..2]` for the next vertex, so S14-4 leaves a constant-zero field
   rather than a leftover one. Strictly safer — and it should *raise* the memo
   hit rate on unlit batches, because vertices differing only in an unused
   normal now collide, and collide exactly.

4. **`dc/build-dc.sh` NEVER WROTE THE ELF PROVENANCE SIDECAR — fixed 2026-08-09.**
   `harness/dc/README.md` §"ELF provenance sidecars" states the rule and says in
   terms *"This binds `dc/build-dc-docker.sh` too. Without the sidecar, crash
   triage on the game build is dead on arrival"* — and it was. A CDI holds a
   scrambled, stripped `1ST_READ.BIN`, so a register dump out of one is just
   hex, and `harness/dc/crash.sh` **refuses** to guess which ELF goes with an
   image or to symbolise against one whose sha256 no longer matches. **Every
   crash on every burn this project has ever done was un-triageable, silently.**
   `dc/build-dc-docker.sh` now writes `<image>.cdi.src.json` with the ELF's
   sha256, size and build time. The sha is the load-bearing field: the ELF at
   that path is overwritten by the next build.

⚠️ **And one PRE-EXISTING bug, found but deliberately not fixed here** (it is
not this batch's scope and folding it in would confound the A/B): in
`dc/src/dc_gx.c` the `#ifdef DC_PERF_PHASE` opened at `:141` does not close
until `:296`, so the `DC_PERF_GXAPI` and `DC_PERF_GXSPLIT` blocks are **nested
inside it**. `-DDC_PERF_GXAPI` without `-DDC_PERF_PHASE` fails to LINK
(`dc_vi.c` externs `dc_gx_api_*`, never defined); `-DDC_PERF_GXSPLIT=1` without
it fails to compile. `dc_vi.c:199` documents the GXSPLIT block as being in its
own `#if` — true of `dc_vi.c`, **false of `dc_gx.c`**. Same shape as the
`DC_EMU64_HIST` trap in `kb/traps.md`.

---

## 6. What was deliberately NOT bundled

- 🔴 **G-B(2), the indexed-submit rewrite — 13.31 ms, the largest single block
  in the project.** Bigger than everything in §2 combined. It deletes emu64's
  index expansion **and** our own `GX*` setters, and it has three vertex
  mutation hazards in `set_position` (`kb/research-sh4zam-gap.md` G-B(2)).
  **Reason for excluding it: a multi-session architectural rewrite inside a
  batch that is meant to be A/B'd as one unit means a single broken build tells
  you nothing about the other seven changes.** It wants its own pass.
- **TEV P3** (`-DDC_PVR_TEVP3`) — still default OFF. It is a *correctness* fix
  (the black name-entry keyboard), not a perf one, and turning it on **adds**
  the per-vertex `oargb` store that S14-2 just removed. Landing it here would
  confound the measurement in both directions.
- **§0a's FTRV lighting / G-D's pass split.** XMTRX holds `comb` across the
  whole vertex loop, so lighting-by-FTRV needs the loop split into passes — and
  a 32-vertex block must materialise ~1,796 B of scratch against today's ~112 B,
  into a **16 KB** operand cache. Spending cache to save arithmetic is the wrong
  direction in a memory-bound frame. Sub-1 ms either way.
- **Building KOS itself at `-O3 -flto`.** Costs a ~27 min SDK image rebuild and
  points the wrong way for an i-cache-bound target; `-Os` would be the arm worth
  testing, not `-O3`.
- **F6, OCRAM for the memo.** Speculative, hardware-only, and its premise moved:
  the side channel already removed the random read into `verts[]` that made the
  memo expensive, and S14-1 just fixed its stride. Re-argue it after a PMCR burn.

---

## 7. ⭐⭐⭐ THE HARDWARE VERDICT: IT RUNS BETTER ON REAL SILICON

**2026-08-09, human verdict on a burned CD-R of `AC-DC-20260809b.cdi`:
*"definitely runs better on real hardware."***

⭐⭐⭐ **THIS IS THE FIRST TIME THIS PROJECT HAS BANKED A WIN THE EMULATOR COULD
NOT SEE, AND IT IS THE METHODOLOGICAL RESULT OF THE SESSION.** Flycast measured
the same batch as a **wash** — `us/v` 2.51 → 2.48, inside the ±2 % floor, every
`[VTXSPLIT]` bucket within 0.03 ms (§2a). The console disagrees. The two are not
in conflict: they are the predicted consequence of the fact this kb has repeated
for three sessions without ever having acted on it —

> **Flycast models no instruction cache and no operand cache, so every figure it
> produces is a FLOOR, and a change whose entire mechanism is cache behaviour is
> invisible there BY CONSTRUCTION.**

Four of the seven changes — S14-1 (memo stride), S14-3 (prefetch), S14-6 (decal
arming reach) and above all S14-7 (F5 section ordering) — pay only in cache
misses. **`us/v` 2.48 was never the result; it was the floor.**

### ⭐ AND THE AUDIO CORROBORATES IT MECHANICALLY — IT IS NOT A SECOND OPINION

Same burn, same session: ***"sound is perfect. no skipping."*** In Flycast the
comparable run booked **`[STUTTER]` 65 / 900 s**.

**It is a frame-time measurement wearing a different hat — but of the TAIL, not
the median.** The mechanism is in `kb/RESUME.md` §5 audio rule 4:

> **`DC_AUDIO_MAX_FRAMES` IS AN FPS CONSTANT, NOT AN AUDIO ONE.** Production is
> capped at `MAX_FRAMES × 17.49 ms × 2 ticks` **per PRESENTED frame** — the
> audio cannot keep up below a floor FPS *however cheap synthesis becomes*.

Audio production budget is proportional to presented frames, so a faster build
gets proportionally more synthesis budget and the stutter is the first thing to
go.

🔴 **BUT BE PRECISE ABOUT WHAT IT BOUNDS, BECAUSE AN EARLIER DRAFT OF THIS FILE
WAS NOT.** At `DC_AUDIO_MAX_FRAMES=6` the sustained floor is **~4.8 FPS** — a
bound this port cleared long ago. `[STUTTER]` does not fire on the median frame;
it fires on the frames that individually blow the budget. **So "no skipping" is
a statement about the WORST frames, not the average one: it says the p99 frame
time came down, and it says nothing about p50.**

⭐ **That is corroborated directly by the human in the same session: *"music
doesn't cut out at all or stutter on hardware, but the FPS is still definitely
worse than emulator."* Tail fixed, median still short.** The two halves of that
sentence are consistent and they measure different things — which is exactly why
the audio is worth reading as an instrument and exactly why it cannot stand in
for one.

⚠️ **It does not attribute, and it does not size the median gap.** Nothing here
replaces `istall`.

### What this does NOT establish

⚠️ **It is a perceptual verdict, not a number, and it does not attribute.** It
says the batch helped. It does not say which change, or by how much, and it
cannot rule out that one of the seven is a small regression masked by the
others. Two things fix that, in order of cost:

1. ⭐ **The F5 A/B, and it is CHEAP.** `DC_SECTION_ORDER=0` is keyed to
   `dc/build/link.stamp`, not `flags.stamp`, so it **relinks without
   recompiling 3,900 TUs** — a second burnable image in ~2 minutes.
   `AC-DC-20260809c-nof5.cdi` is that image: identical objects, identical
   flags, **only the linker's section order differs.** Burn it, play the same
   spot, and the difference is F5 alone. No other change in this project has
   ever had an isolation this clean.
2. **`AC-DC-20260809a-pmcr.cdi`** for the numbers — `istall` is what prices
   F5, and it is the one counter that can turn "feels better" into a figure.

### The general rule this earns

⭐ **A change whose mechanism is CACHE must be judged on hardware, and a Flycast
"no change" is not evidence against it.** The converse also holds and is the
trap: a Flycast *win* on a cache-shaped change would be understated, not
absent. Add this to the measurement rules — the emulator can falsify an
instruction-count claim and can never falsify a locality claim.

---

## 7a. The instrument that is still owed

🔴 **The batch now has a direction but no magnitude, and Flycast structurally
cannot give one.** It models no instruction cache, no operand cache and no disc seek
time. **S14-7 (F5) is unmeasurable in the emulator by construction**, and
S14-1/-3/-6 are all understated there for the same reason — what they remove is
cache misses.

**The instrument is `dc/src/dc_pmcr.c` on a burned CD-R**: `DC_PMCR=1`,
`DC_PMCR_HUD=1`, `DC_CONSOLE_MUTE=1`, no probes, no `DC_SCIF_FAST`. The three
numbers to photograph off the TV, town, standing still, ~12 s after boot, are
**`cyc`, `istall`, `dstall`** — and `istall` is the one that prices F5.
`kb/RESUME.md` §6 carries the burn's own traps.

### The images, built 2026-08-09 and waiting on a CD-R

On the NAS at `/Volumes/Gabe/AC-DC/` (Jupiter), same convention as the
2026-08-08 set. ⚠️ **Both are the post-S14 tree with `-DDC_GX_NRMSKIP` OFF**, so
neither carries the dead branch §2b describes.

| image | config | what it is for |
|---|---|---|
| `AC-DC-20260809a-pmcr.cdi` | `DC_PMCR=1 DC_PMCR_HUD=1 DC_CONSOLE_MUTE=1 -DDC_PERF_PHASE`, padded | **the instrument.** Photograph `cyc`/`istall`/`dstall` off the HUD |
| `AC-DC-20260809b.cdi` | shipping config, padded, no probes, no mute, no PMCR | **the play test.** "does it feel smoother than the 0808 burn" |

⭐ **Each now has a `.elf` and a `.cdi.src.json` beside it** — the ELF
provenance sidecar `harness/dc/crash.sh` needs and that no build in this project
had ever written (§5 item 4). **Keep the three files together**: the ELF in
`dc/build/` is overwritten by the next build, and without a matching sha256 a
crash dump off that disc is un-symbolisable hex.

⚠️ **`DC_CONSOLE_MUTE` arms at `DC_CONSOLE_MUTE_FRAME` (default 300) presented
frames, NOT at `main()`** — muting at `main()` stopped the boot on the `-f`
burn. The HUD leads with a liveness line `f= t= d= c=` so a repeat failure is
readable off the screen rather than silent.
