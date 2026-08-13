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

🔴 **The town has no villagers — AND THE REASON THIS FILE GAVE FOR IT IS
FALSIFIED (2026-08-09, S15).** The claim was "the VMU save path is unwired, so
`mNpc_SetNpcList` constructs none", and the prescribed fix was N2b. **Measured,
in the town, before anything was written:**

```
[DC/NPCSEED] pre ids=6 homed=6 | seeded id=8 home=8 | want=14 max=14
```

`pre ids=6 homed=6` — six villagers already had valid `id.npc_id` **and** valid
`home_info`, so `mNpc_SetNpcList` has been building a populated `npclist` all
along. Two separate things are now known to be wrong:

1. **`animals[]` is not loaded from the save on a new game — it is GENERATED**
   by `mNpc_DecideLivingNpcMax` (`m_npc.c:2422`), and `mSDI_StartInitNew`
   carries no `mFRm_CheckSaveData()` gate. **N2b would not have fixed this.**
2. **The homes were fine too**, so the FGDATA/`SIGN00..SIGN20` reserve-scan
   theory that replaced the save theory is *also* wrong.

**The real blocker is downstream of the list**: nothing constructs a villager
ACTOR. `aNPC_dma_draw_data_proc` still never runs — the same 600 s run printed
zero `[DC/NPCTEX]`/`[DC/NPCMDL]` lines **with `DC_NPCTEX_POOL=1
DC_NPCMDL_POOL=1`**, which is the first time that silence has meant anything
(at the old `=0` defaults both files compile to empty stubs, so the earlier
"zero lines" evidence was vacuous).

**Next step is the actor-construction path, not the save and not the field
data**: find what walks `Common_Get(npclist)` and spawns the NPC actors, and
why it spawns none. `dc/src/dc_npcseed.c` (`DC_NPC_SEED=1`) now guarantees a
14-entry roster, so that half is no longer a variable.

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

🔴 **THE PIVOT OF 2026-08-09 IS WITHDRAWN (2026-08-10). FPS IS NOT "GOOD ENOUGH
ON HARDWARE".** Standing human verdict: *"hardware does not run better than the
emulator, it runs much worse. the audio sounds good though."* The S14 burn
quote that this file read as "better than the emulator" was a comparison against
the **previous hardware build**; only the AUDIO result survives. **The FPS
deficit is on silicon and Flycast cannot see it** — `kb/RESUME.md` §1 and §6.
**Both workstreams are live: hardware FPS AND playability.**

⚠️ **"Eliminate stub loading" is not the lever** — a full
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

✅ **THE GARBLED TEXTURES ARE FIXED (2026-08-09, S15-8). The "generator half is
proved clean" claim that used to sit here was WRONG — twice over.**

The human report was right and reproducible: with T1 on, the title screen's
`Press START!` and copyright line rendered as **two bars of noise**; with
`DC_TEXPOOL_DEMAND=0` they were perfect. Two independent generator defects:

1. **S15-8, 154 rows.** T1's hook is the PVR upload, so it can only serve a
   texture whose **array address reaches the upload**. `gDPLoadTextureTile` /
   `gDPLoadTextureBlock*` go through emu64's **TMEM emulation**, which copies
   the texels out of the array into `texture_buffer_data` *before* any hook
   this port owns — so a stubbed array is read as its one byte and the garbage
   is baked in upstream. Now excluded from the demand path (91,072 B back to
   residency).
2. **S15-6, 2 rows.** `scan_asset_declarations()` let the `sorted()`-last file
   decide linkage, so two symbols that are `static` in `dia_win{,2,3}.c` and
   global in `lat_letter64_xk_tex.c` were stubbed to `u8 x[1]` with their
   loaders removed while their display lists still bound them.

⭐ **The instrument was `-DDC_TEXPOOL_TRACE=<N>` (S15-7), and it worked by
ABSENCE**: the rows that rendered correctly were listed; the broken ones were
never fetched at all. `fetch=133 fail=0` cannot distinguish a lookup failure
from a healthy run, which is why T1 looked clean while the screen was wrong.

⚠️ **And T1's own falsifier could never have caught either.** Under the loader
every row has `kept == 0`, so `lookup()`'s early-out makes `interior=` and
`mutated=` **incapable of incrementing** — every `interior=0 mutated=0` verdict
recorded for T1 was taken where those counters could not move. The only honest
probe config is `DC_TEXPOOL_PROBE=1 DC_TEXPOOL_DEMAND=0` + `keeplist-town.txt`.

🔴 **Still open: MISSING GEOMETRY**, which is a separate bisect and is not
explained by either fix. Most of it is the **904 model files (1,128,096 B) the
keep-list budget dropped**, equally absent before. For anything beyond that,
start with `-DDC_GX_NO_GHCULL` — the only change that can delete a whole batch.

Full numbers and the rollback contract: `kb/batch-s15.md`.

**RAM is no longer the binding constraint; RESIDENCY is.** 8,813,054 B of asset
destination arrays can never all be resident, so the keep list still decides what
exists. The opposite extreme is closed by a boot, not by arithmetic: a full
`DC_ASSET_STUB=0` image prints `margin=-781036 OVER`, fails a 15,638,528 B
contiguous malloc, and comes back with all 14,495 assets MISSING
(`kb/closed.md`).

---

## ⭐ P2 landed 2026-08-12 — hardware profiling is no longer a plan

The gprof flat profile works. `-pg` on the link line only, our `gprof_init()`
override wins in the map, 31,010 samples recovered from a Flycast run through
the console sink and symbolised by `sh-elf-gprof`. **The emulator half of the
§6 experiment is captured; the hardware half is a burn away**
(`AC-DC-20260812b-gprof-sd`, on the NAS with its ELF and sha256 sidecar).

⚠️ **Do not compare that build's FPS to anything here.** It is `keeplist-town`
(≈900 KB less resident geometry than shipping, to buy the 1.53 MB the gmon
buffers need) with **F5 off**. It is a MEASUREMENT build; its frame rate is a
different workload, not a result. A human read it as "runs noticeably better"
on hardware — that is the missing content, not a win.

Full result, the three traps it cost, and the two build lines:
`kb/RESUME.md` §6b, `kb/hardware-profiling.md`, `tools/dcprof/README.md`.

---

## 🔴 THE QUEUE IS RE-RANKED BY THE FIRST HARDWARE PROFILE (2026-08-12)

The evidence is `kb/RESUME.md` §6c. Three things moved:

1. ⭐⭐ **AUDIO IS NOW THE TOP PERF ITEM AND IT WAS BOTTOM OF THE QUEUE.**
   `RspStart` is **15.7 % of non-idle / ~21 % of work** on silicon and **2.18×
   its Flycast share**; audio as a group is ~25 % of work and 2.13×. It is the
   ONLY subsystem that measurably gets worse on hardware. Old ranked action 8
   (AICA stage B) was priced at nothing because no instrument had ever seen it.
   ⚠️ Its blockers are in **`kb/audio-engine.md` §3.5** — this file used to
   cite `kb/audio-cpu-cost.md` §3.5, **which does not exist**.
   ⭐ **AND THEY ARE NO LONGER "UNCHANGED": measured 2026-08-13,
   `kb/audio-aica-offload.md`.** Two of the four are ONE blocker (bank 153
   overflows *because* 19 of its samples exceed the channel limit; strip those
   and it is 49 % of usable), the 8-bit-PCM mitigation is **falsified** (4.9×
   usable), and the loop-discontinuity risk is 0 or ~163 samples depending on
   a single unmeasured hardware fact.
2. 🔴 **THE ICACHE PREDICTION FOR `dc_gx_backend_submit` IS FALSIFIED** — its
   share **shrank** 0.81× on hardware, as did the whole GX setter family. This
   does NOT clear the draw path of being icache-bound (a share cannot see a
   uniform stall); it kills one named suspect. `istall` on a PMCR burn is still
   the only instrument for the general claim.
3. ⭐ **G-B IS SMALLER THAN ADVERTISED, AND NOW SPLIT.** The 13.31 ms block is
   ~1.10 % of work index expansion and ~9.34 % setters/staging. G-B targets the
   staging half only. Re-size it in the TOWN before starting a multi-session
   rewrite.

**Shipped the same day, both with kill switches:** P3 (`DC_RSPSIM_NOFP`, the
bit-exact `f32`→`s32` in `A_CMD_ENVMIXER`, ~11 % of `RspStart`) and `dc_ctz32`
(`-DDC_NO_CTZ_LUT`, ~0.62 % of work).

🔴 **`DC_CONSOLE_MUTE=1` IS NOW A PRECONDITION FOR EVERY HARDWARE MEASUREMENT.**
`scif_*` is ~4.8 % of non-idle with no cable attached; Flycast reads it at
0.50 % and so understates it ~8× as a share of work.

⚠️ **THE PROFILE IS THE TITLE SCREEN'S DEMO SCENE**, not the walked town. It has
live actors, camera and music, so it transfers further than "title screen"
implies — but every vertex-load-dependent number in it (G-B's share, the
frustum test, `setup_1tri_2tri_1quad`) needs a town run to be final. The chord
(L+R+START) exists for exactly that and has now been used twice.

---

## Ranked next actions

⭐⭐ **USER DIRECTIVE 2026-08-09, end of session 15: the next two are VILLAGERS
and the TEV FIX. Everything below them is the perf queue and waits.**

**A. ✅ N3 HAS NOW RUN (2026-08-13) — READ `kb/villagers-n3-result.md` FIRST.**
Three of the five hypotheses are dead (`cloth=10/10`, `clip=5`,
`ut: calls=0`). **The break is NOT actor construction**: `aSNMgr_make_npc`
runs every tick and finds `make[]` empty (`mk: ent=12087 slot=0 called=0`).
The wall is `aSNMgr_chk_exist_and_appear_and_event`, which passes **0 times in
12,048 GUEST calls** while the plain `chk_exist_and_appear` passes fine in
REGULAR (`exist=28`). Two candidate causes remain and one line of extra
instrumentation separates them. Everything below is the pre-run text.

**A-old. 🔴 VILLAGERS — run the N3 diagnostic. ⚠️ THIS ITEM USED TO PRESCRIBE N2b
("wire the VMU save path"); THAT DIAGNOSIS IS FALSIFIED — see the top of this
file — AND N2b WOULD NOT HAVE FIXED IT.** The roster is fine and always was.
The break is **actor construction**, downstream of `npclist`.

**N3 is built and wired (2026-08-10): `DC_NPCDIAG=1`.** The chain from
`npclist` to a live actor is five functions with nine serial gates that say
nothing when they refuse, so N3 wraps every one in `dc_npcdiag_gate()` — which
returns its argument untouched, preserving every `&&` short-circuit — and
prints one cumulative `[DC/NPCDIAG]` line. **One town run is decisive**;
`dc/src/dc_npcdiag.c` carries the decision table mapping each printed shape to
its candidate.

Run line: `DC_NPCDIAG=1 DC_NPC_SEED=1 DC_NPCTEX_POOL=1 DC_NPCMDL_POOL=1`.

Live candidates, ranked (full chain and line numbers in `dc_npcdiag.c`):
1. **The REGULAR pass only runs on an acre transition** — `aSNMgr_actor_move`
   (`ac_set_npc_manager.c:1254-1276`) clears `make[]` on `mFI_WADE_NONE` before
   `set_proc` runs, and `aSNMgr_set_npc_regular` tails into `GUEST`. If
   `mFI_GetPlayerWade()` never leaves `WADE_NONE`, zero villagers ever.
2. **All ten cloth banks failed to reserve** — `aNPC_keep_cloth_data_area`
   (`ac_npc_cloth.c_inc:226-259`); every slot `mSC_BANK_NONE` makes
   `aNPC_setupNpc_check` FALSE forever, silently.
3. **`CLIP(npc_clip)` NULL** — set in exactly one place,
   `ac_npc_ctrl.c_inc:814`.
4. **`aSNMgr_get_safe_utnum` rejects every unit** — `mNpc_CheckNpcSet_fgcol`
   (`m_npc.c:4590`). Suspicious: the FGDATA reserve scan already finds nothing.
5. **Benign** — the run never entered a villager's acre. N3 logs the player's
   `next_block` against the roster's block numbers so this is distinguishable.

✅ **Candidate 6 is already RULED OUT with no run**: the missing `return` at
`ac_npc_ctrl.c_inc:519` is present in the built shrink tree.

⚠️ **`[DC/NPCTEX]`/`[DC/NPCMDL]` silence is a WEAKER signal than this file used
to claim** — `dc_npctex_ensure()` returns silently on four separate conditions
(`dc/src/dc_npctex.c:333-345`), so zero lines is consistent with actors
existing. Do not treat it as proof.

R2/R3 remain OFF and remain untestable until a villager exists; that ordering
was the one correct part of the old item. `dc/src/dc_npctex.c`,
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
2. ✅ **`cds=` — SUPERSEDED BY S15-1, AND THE 2.23 ms FIGURE HERE IS STALE
   (corrected 2026-08-10).** This item used to read "2.2 ms/frame of emu64 state
   work inside G3's cull, and nobody has ever costed it". **`cull_batch()` no
   longer calls `dirty_check` or `setup_1tri_2tri_1quad` at all** — S15-1
   replaced them with the lean refresh (`dc_emu64_cull.cpp:749-765`: the
   `EMU64_DIRTY_FLAG_PROJECTION_MTX` test plus one `GXSetCurrentMtx`), shipped
   default-ON 2026-08-09. `cds=` now brackets the lean pair, so the 2.23 ms is
   pre-S15-1 and **nobody has read the post-S15-1 value.**
   ⚠️ Two corrections to what this item asserted:
   - **`dirty_check()` (`emu64.c:3154-3442`) really is idempotent AND cheap** on
     a second call — every block is `IS_DIRTY(x) { CLEAR_DIRTY(x); … }`, so a
     repeat runs ~14 flag tests, the tile loop and one projection test, with **no
     calls**. The 2.23 ms was never this function's.
   - **`setup_1tri_2tri_1quad()` (`emu64.c:2854-2899`) is the one that was
     costly, and it is not dirty-guarded at all** — 8-11 unconditional
     out-of-line `GXClearVtxDesc`/`GXSetVtxDesc`/`GXSetVtxAttrFmt` calls per
     batch.
   **Action: no run of its own.** Read `cds=` and `lproj=` off the next
   `-DDC_PERF_PHASE` run. The handler's own copy cannot be elided from `dc/`
   (no flag exists and `src/` is not editable) — it disappears as a by-product
   of item 1, because a consumed batch never reaches the handler.
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
