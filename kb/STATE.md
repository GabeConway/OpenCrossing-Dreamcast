# STATE — the numbers and the queue, right now

**Short by design.** Only what is true today, plus what to do next. The handoff
and the measurement rules are `kb/RESUME.md`; the evidence and the narrative for
every figure here are `kb/state-log.md`, newest first. If a section here starts
growing a history, that history belongs in the log.

Last flush: **2026-08-09 (session 14b — S14 burned, pivot to playability)**. Audited 2026-08-09 — every
number below was re-derived or re-sourced; the pre-session-9 material that used
to sit in this file is in `kb/state-log.md`.

---

## Where the port is

**It walks the town, with music, on real hardware.** It boots from a burned CD-R
on a retail Dreamcast with **loading at parity with the emulator**; in Flycast it
reaches the town, walks around it, meets Tom Nook and is taken to the houses
(`[SCENE_MODE] 0 → 3 → 4 → 18 → 9`). Every summer acre is in the image, plus the
interiors, the winter set and the gyroids. The BGM plays.

⚠️ **The town has no villagers** — the VMU save path is unwired, so
`mNpc_SetNpcList` constructs none. Nothing on the NPC path can be tested until
N2b lands (`kb/RESUME.md` §"still broken").

---

## The renderer, measured — 2026-08-09, Flycast

One build line, five 600 s runs, town, static camera:
`DC_ASSET_STUB=1`, `keeplist-town.txt`, `DC_ARAM_WINDOW=1048576`,
`DC_ARENA_BYTES=1200000`, `DC_AUDIO_SCENES=all`, `DC_AUDIO_DISC_FRAMES=8`,
`DC_AUDIO_VOICES=12`, `DC_AUTOSTART=1`, `DC_PVR_VTXSPLIT=16`, `-DDC_PERF_PHASE`.

```
build                        us/v  draw xform  sum  memo shade  lit  tex post emit   xf     v  hit%
ctrl (neither change)        2.65  29.1   7.5  6.66  1.27  1.99 0.58 0.57 0.58 1.45 0.23  2820  50.5
shade hoist only             2.68  28.3   7.3  6.65  1.26  1.98 0.61 0.62 0.56 1.40 0.22  2739  50.9
G-B + hoist  (SHIPPING)      2.51  29.2   6.9  6.31  1.20  1.82 0.54 0.57 0.55 1.40 0.22  2745  53.7
G-B + hoist + shortcuts      2.54  28.5   7.0  6.33  1.18  1.89 0.53 0.56 0.57 1.39 0.20  2745  53.7
G-B, hoist OFF               2.56  27.8   7.0  6.45  1.26  1.91 0.55 0.59 0.55 1.40 0.20  2754  54.1
```

All five: `ASSET MISSING 0`, `reinst=0`, `dropped=0`.

⚠️ **`us/v` is the instrument; `draw` is NOT.** Every run drew a different town
(`v` 2739–2820 — the seed is per boot), and `draw` wanders 27.8–29.2 in both
directions across a change worth −6 % on `us/v`.

⚠️ **The `[VTXSPLIT]` buckets do not share a denominator** (measurement rule 10):
`memo` is per vertex, `emit` is per primitive, and the middle five are charged on
memo **MISSES** only.

⚠️ **Flycast models no cache. Every figure here is a floor.**

### A different experiment — the whole-run, audio-on figure

`fps_p50` **23.2**, town `draw` **49.9 ms**, `[STUTTER]` **65 / 900 s**
(2026-08-08, session 9, 900 s `DC_AUTOWALK`, no `VTXSPLIT` probe). **Do not
compare it against the table above** — different run length, different camera,
different probe set. It is the number to quote for "how does it play".

### The trajectory, for context

| | 2026-08-06 | session 11b | session 12 | session 13 | **session 14 (S14)** |
|---|---:|---:|---:|---:|---:|
| `us/v` | 3.06 | 3.24 | 2.65 | 2.51 | **2.48** |
| `xform` ms | 8.8 | 8.9 | 7.2 | 6.9 | **7.0** |

⚠️ **S14 is a WASH in Flycast and that was predicted** — four of its seven
changes pay in cache misses and Flycast models no cache. −1.2 % on `us/v` is
inside the ±2 % floor. **Do not read the 2.51 → 2.48 column as a result in
either direction.** ⭐ **The burn answered it: *"definitely runs better on real
hardware"*, *"music doesn't cut out at all or stutter"* — but *"the FPS is still
definitely worse than emulator"*. TAIL FIXED, MEDIAN STILL SHORT**, and the
median gap has never been measured on silicon. `kb/batch-s14.md` §7.

### Where G3's cull time actually goes — measured 2026-08-09, first time ever

| bucket | wraps | ms/frame | share |
|---|---|---:|---:|
| `cus=` | all of `cull_batch()` | 3.05 | 100 % |
| **`cds=`** | emu64's `dirty_check` + `setup_1tri_2tri_1quad` | **2.23** | **73 %** |
| **`fus=`** | `dc_gx_aabb_is_offscreen()` — the frustum test | **0.139** | **4.5 %** |

⭐ **The frustum test is under 0.5 % of the frame. G-F is retired in both
shapes.** The cost is emu64 state work G3 must trigger to get a live matrix.
Ranked action 2.

---

## What binds: the frame is MEMORY-BOUND

`us/v = 2.51` is ~500 SH-4 cycles per vertex against ~60 cycles of vertex
arithmetic. The memory-shaped stages (`memo 1.20 + emit 1.40 + shade 1.82`) are
**4.42 ms — 70 % of the 6.31 ms the split accounts for**; the two
floating-point stages (`lit 0.54 + xf 0.22`) are **0.76 ms of a ~29 ms frame,
2.6 %.** Corroborated from the other end by
`tools/dcopt/icache_map.py`: the inner draw loop is **2.62×** an 8 KB
direct-mapped icache, and the whole frame's hot set is **16.40×** it.
⚠️ **The 1.4× / 11.9× pair quoted here until 2026-08-09 is FALSIFIED** — the
tool's hot-set regexes missed every emu64 handler, because emu64 is C++ and they
are all mangled (`.text._ZN5emu64*` = 105 sections in the map, `.text.dl_G_*` =
0). The interpreter was absent from the measurement of its own cache pressure.
`kb/batch-s14.md` §5.

**Consequence: every matrix-unit / FTRV / FIPR idea is aimed at ~2.6 % of the
frame.** `kb/research-sh4zam-gap.md` is ranked around this.

🔴 **The largest single block in the project is still unattributed and still
untouched: 13.31 ms of the draw is `dl_G_TRIN`'s index expansion PLUS our own
`GX*` attribute setters, never separated from each other.** Session 13's G-B
made the vertex memo cheap; it removed no setter and expanded no fewer indices.
Do not read one as the other.

---

## The fit — one inequality, never two pools

Derived 2026-08-09 from the linked ELF in `dc/build` (`dc/build/flags.stamp`:
`DC_ASSET_STUB=1 DC_SRC_SHRINK=1 DC_AUDIO=1 DC_OPT_PROFILE=perf`,
arena 1,200,000, ARAM window 1,048,576):

```
(image span) + (genuinely additive heap) ≤ 16,646,144

  image span        10,514,524   _end 0x8ca1705c − 0x8c010000   [T1 + keeplist-full]
                                 .text+.rodata 3,014,196
                                 + .data 2,225,024 + .bss 5,274,332
  additive heap      2,576,256   KOS 262,144 + arena 1,200,000
                                 + ARAM LRU 1,048,576 + threads 65,536
  ⇒ margin           3,555,364
  ⇒ REAL headroom  ≈   499,088   margin − the 3,056,276 B libc peak (rule 6)
```

⭐ **`.bss` MOVED +25,312 B WHILE 650 MODEL FILES BECAME REAL.** T1 paid for
almost the whole keep-list expansion: it took 885,984 B of texture arrays out
and the new content put 899,640 B of geometry back. What grew is `.text` +
`.rodata` (+137,548), which is the 6,068-row map plus 318 more keep-list loader
calls. The pre-T1 baseline for comparison is `.bss` 5,249,020, span 10,375,116.

⚠️ **`margin=` is not headroom** — it *is* libc's pool, and `MEMLEDGER FIT … OK`
does not mean the image boots. `kb/heap-two-pools.md`.

⚠️ **The "~2.05 MB of headroom" figure quoted since 2026-08-06 is STALE.** It
predates `DC_ARAM_WINDOW` going 131,072 → 1,048,576 (+917,504 B of additive
heap) and `DC_AUDIO=1` disabling the S8 jaudio `.bss` shrink (+455,848 B of
span). Both were deliberate and both were worth it — but headroom is now
**~649 KB**, not 2 MB. ⚠️ **The 3,056,276 B libc peak itself was measured on
2026-08-04 at the opening keep list and has never been re-derived.** The honest
next step is an OOM pair on the current config, not more arithmetic.

⚠️ **Take the span from `_end` minus `0x8c010000`, never from `size`'s `dec`
column** — `dec` omits inter-section alignment and counts `.ocram`, which lives
at `0x7c001000` and is not in the image.

⭐ **THE PIVOT, 2026-08-09: FPS is "good enough on hardware"; the workstream is
now PLAYABILITY.** ⚠️ **"Eliminate stub loading" is not the lever** — a full
`DC_ASSET_STUB=0` image is refuted by a boot and has LESS content than the
stubbed one (below). **The stub system already IS the demand loader**
(`dc_stub_keep_load_one()` / `dc_keep_sweep()`); what limits the game is that
the **keep list is static.**

✅ **T1 LANDED THE SAME DAY (`kb/levers.md` L10).** All 6,068 display-list
texture arrays are read off `/cd/foresta.rel` at bind time and are no longer in
`.bss`: **−841,888 B measured on a matched link**, `fail=0`, `evictions=0`,
`ASSET MISSING 0`, deepest scene 9. The freed bytes were spent on
`tools/dcstub/keeplist-full.txt` — 650 more `src/data/model/` files, 899,640 B
of geometry that rendered as NOTHING before, priority-ordered so the shops,
room interiors, player tools, HUD windows and all 43 winter structures come
first. Shipping span **10,553,116 B**, ~460 KB under the ceiling.

⚠️ **NOT SIGNED OFF, and the reason is a human report the counters do not
show.** A matched baseline (T1 off, `keeplist-town`, same probe set) was run and
the frames compare CLEAN — the "lavender ground" that looked like a regression
is the paved plaza, identical in both, and the two runs simply seeded different
towns and walked to different places. ⚠️ **That mistake was made twice in one
session: `v=3488` (baseline) vs `v=3001` (T1) also looks like a 16 % win and is
not one.** Rule 11 applies to SCREENSHOTS as much as to `us/v`.

What the counters DO show, matched runs, same probe set:

| | baseline | T1 + `keeplist-full` |
|---|---:|---:|
| `us/v` | 2.69 | **2.57** |
| FPS | 14.6 | **16.4** |
| `draw` ms | 52.3 | **47.4** |
| `[STUTTER]` | 90 | **147** |

⭐ **T1 plus 650 extra model files is FASTER than baseline.** The one real
regression is stutters, and the time is in neither `gx` nor `snd` (mean
unaccounted 84 ms baseline → 157 ms), i.e. it is the demand reads.

✅ **T1 IS VISUALLY CLEAN — MATCHED-FRAME A/B, RULE 2 SATISFIED.** `t1only`
(T1 on, `keeplist-town`, everything else identical to baseline) drew
**v=3507 against baseline's 3488 — 0.5 % apart, i.e. the same town** — and
frame 90 of each is the same scene, pixel-comparable, with no difference.
That is the first genuinely matched screenshot pair this session produced;
every earlier "regression" was two different towns.

| | baseline | t1only | T1 + `keeplist-full` |
|---|---:|---:|---:|
| `v` | 3488 | 3507 | 4007 |
| `us/v` | 2.69 | 2.67 | 2.61 |
| FPS | 14.6 | 15.5 | 16.2 |
| `[STUTTER]` | 90 | 110 | 109 |

**T1 costs nothing per vertex and adds ~20 stutters** — the demand reads, 271 of
them for 325,184 B. That is the whole price.

🔴 **STILL OPEN: a human reports wrong/garbled textures and missing geometry
that no captured frame reproduces**, including the matched pair above. The
generator half is proved clean — T1 stubs exactly 1,379 extra symbols, every one
a T1 texture row, and un-stubs nothing; `keeplist-full` is a strict superset of
`keeplist-town`. So missing geometry is the **904 model files (1,128,096 B) the
budget dropped**, equally absent before today. What remains unexplained is a
class the 900 s autowalk never binds — the shops, house interiors and menus it
never opens are the obvious candidates.

**RAM is no longer the binding constraint; RESIDENCY is.** 8,813,054 B of asset
destination arrays can never all be resident, so the keep list still decides what
exists. The opposite extreme is closed by a boot, not by arithmetic: a full
`DC_ASSET_STUB=0` image prints `margin=-781036 OVER`, fails a 15,638,528 B
contiguous malloc, and comes back with all 14,495 assets MISSING
(`kb/closed.md`).

---

## Ranked next actions

⭐⭐ **USER DIRECTIVE 2026-08-09, end of session 15: the next two are VILLAGERS
and the TEV FIX. Everything below them is the perf queue and waits.**

**A. 🔴 N2b — wire the VMU save path, then turn R2/R3 on.** This is the whole
villager problem and it is a SAVE bug, not an asset bug: `mNpc_SetNpcList`
populates the town from the save's `Animal_c animals[]`
(`m_start_data_init.c:559`), the VMU path is unwired, so `[PC] No save file
found` and **not one villager actor is ever constructed** — measured, two 900 s
runs to scene 9 printed zero `[DC/NPCTEX]`/`[DC/NPCMDL]` lines. R2 (236 villager
texture sets) and R3 (32 species, with its 933-word gsSPVertex relocation table)
are BUILT, tested to compile, and **defaulted OFF for exactly this reason** —
nothing on the NPC path can be exercised until a villager exists. Order: save
path → villager appears → turn R2/R3 on → then their pools can finally be
measured. `kb/save-plan.md`, `dc/src/dc_card.c`, `dc/src/dc_npctex.c`,
`dc/src/dc_npcmdl.c`.

**B. 🔴 TEV P3 — the predicate, not the maths.** `-DDC_PVR_TEVP3` was run for
the first time on 2026-08-09 and printed `tevp3 batches=20305 clamped=6941` in a
town run that **never opened the name-entry keyboard**, recolouring the town and
costing ~10 %. 26 display lists in one widget cannot be 20,305 batches. The
`PRIM − ENV` / `oargb` derivation in `kb/tev-map-hard-cases.md` §6.6 is right;
the recogniser is matching a shape #037 merely shares. **Log the combine words
of the first N distinct matches, narrow it, THEN take a screenshot pair** — and
the free falsification (`is the panel black while the 40 caps are correct?`)
still needs a run that actually reaches the keyboard, which `DC_AUTOSTART`
never does. §6.6a.

---

⭐ **2026-08-09 — batch S14 landed eight changes, all ON by default, each with a
kill switch: `kb/batch-s14.md` (the rollback contract).** It collected the old
item 1 (decal-Z arming), G-J's unlit-`GXNormal` skip, G-F's cheap shape
(Gribb-Hartmann), F5 (linker section ordering), and three cache-layout /
store-removal fixes in `dc_pvr.c`. ⚠️ **Part of it — F5 above all — is
UNMEASURABLE in Flycast by construction, so item 3 below is now load-bearing
rather than optional.**

1. 🔴 **The full indexed-submit rewrite — the 13.31 ms block.** Transform each
   unique vertex once, index into it, delete the setters. Unstarted,
   multi-session, and still the largest single block in the project.
   `kb/research-sh4zam-gap.md` G-B. ⚠️ S14 deliberately did **not** touch it —
   a multi-session rewrite inside a bundled A/B tells you nothing about the
   other seven changes.
2. 🔴 **`cds=` — 2.2 ms/frame of emu64 state work inside G3's cull, and NOBODY
   HAS EVER COSTED IT.** Found 2026-08-09 by S14-8's split bracket:
   `cull_batch()` is 3.05 ms/frame, of which the frustum test is **0.139** and
   emu64's `dirty_check` + `setup_1tri_2tri_1quad` are **2.23 (73 %)**. They are
   there because the frustum test reads `g_gx.projection_mtx` and
   `g_gx.current_mtx` **live**, so G3 must make emu64 refresh them before it can
   test anything — and in a typical window only **175 of 2,572** TRIN batches
   are culled, so ~93 % of that cost is on batches whose original handler then
   calls the same two functions again. The in-file comment asserts the second
   call is "idempotent"; **idempotent is not cheap and it has never been
   measured.** Measure the handler's second call first, then decide whether the
   ordering rule can be satisfied without a full `dirty_check`.
   ⭐ **This retired G-F entirely** — see `kb/research-sh4zam-gap.md`.
3. 🔴 **The S14 PMCR burn.** Was "the hardware PMCR burn"
   (`AC-DC-20260808g-pmcr.cdi`); rebuild it on the S14 tree instead, because
   **F5 has no verdict at all without it**. Every number in this file is a
   Flycast floor and **Flycast structurally cannot answer the hardware gap**: it
   implements no PMCR (all 8 events read 0) and models no cache of either kind.
   The three numbers to photograph off the TV are `cyc`, `istall`, `dstall` —
   and `istall` is what prices F5. `kb/RESUME.md` §6 carries the burn's traps
   (`DC_CONSOLE_MUTE=1` is not optional; muting at `main()` stops the boot).
4. **Screenshot pairs still owed** — for G3 (`DC_EMU64_CULL=0` vs `=1`) and now
   for S14-4 / S14-5, the two S14 changes that can alter what is drawn. Their
   VERIFY gates are the stronger instrument, but measurement rule 2 is not
   formally satisfied by a counter.
5. **TEV P3 / `oargb`** — in the tree since session 7, compile-verified, still
   never run. `-DDC_PVR_TEVP3`. Fixes the black name-entry keyboard and 27 of
   the 101 TEV configs. Free falsification: `[DC/PVR] tevp3 batches=0` on a run
   that reaches the keyboard.
6. **N2b — wire the VMU save path.** Still the only way to get a villager into
   the town, and therefore still the gate on testing the R2/R3 pools.
0. 🔴 **THE LAVENDER TOWN GROUND — the one thing blocking T1's sign-off.**
   Reproduce with `scratchpad/t1v2.cdi`, and settle "is it new" with
   `base.cdi` (T1 off, `keeplist-town.txt`). If new, the discriminator after
   that is `nowin.cdi` (T1 on, `DC_ASSETWIN_B=0`), which restores the exact
   pre-change read path for R1 while keeping T1 — the window is the only thing
   T1 changed about how R1 gets its bytes. ⚠️ **`console.log` appears only when
   Flycast exits, so "no log yet" is the normal state for most of a 900 s run.
   Do not infer a dead run from `ps` — `grep "[f]lycast"` is case-sensitive and
   the binary is `Flycast`; that misreading cost a killed run today.**

7. ✅ **T1 — DONE, see `kb/levers.md` L10 and item 0 above.** Original entry
   kept below because its numbers were wrong in an instructive way. (user directive
   2026-08-09: *"the FPS is now good enough on hardware … make the full game
   playable"*). Phase 1 frees 579,248 B; phase 2 makes **4,711 stubbed textures
   / 2,132,352 B** loadable — content that renders as nothing today. ⚠️ **The
   kb's "5,685 / 2,782,080 B" was ~30 % high**; the figures here are measured
   (`[DC/TEXPOOL] map=6092 resident=1381/885984 stubbed=4711/2132352`).
   ✅ **ITS FALSIFIER IS NOW PASSED, WITH THE TOWN EXERCISED**:
   `interior=0 mutated=0 oversize=0 aliased=0` over **2,074,009 binds / 127
   distinct textures**, deepest scene 9 (`smoke-texprobe3-20260809-142619`).
   The design is cleared as specified — one ~24,576 B staging buffer, no N-slot
   pool. ⚠️ An earlier run read `interior=4318` and that was a **probe bug**
   (it charged a stubbed row's neighbours to it); fixed, and every `interior=`
   printed before the fix is void. `kb/levers.md` L10.
   ⚠️ **The seek risk is the playability risk**: ~306 seeks = 6-30 s of
   hitching on CD-R unless it uses `dc_keep_sweep()`'s read-ahead window, which
   R1 also still does not use.
8. **AICA offload (stage B)** — the ~265 µs per voice-update term is exactly
   what the 64 hardware ADPCM channels do. Needs an offline VADPCM →
   AICA-ADPCM converter and a residency manager for 8.3 MB in ~1.8 MB.
