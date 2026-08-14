# Session log — what was observed running, in order

**This file is the ARCHIVE and the evidence.** Newest first. Since the
2026-08-09 kb audit it is the *only* place the session-by-session narrative
lives — `kb/STATE.md` and `kb/RESUME.md` used to carry a second and third copy
of it and no longer do. Entries are dated snapshots: a number here was true when
it was written and is **not** a claim about today. For what is true now, read
`kb/STATE.md`.

## 2026-08-12 (session 16b) — THE PROFILE READ: AUDIO IS THE FRAME, THE ICACHE
## SUSPECT IS CLEARED, AND THE INSTRUMENT'S IDLE NUMBER IS A WRAPPED COUNTER

Four agents read the two hardware `gmon.out`s. Two of them went under gprof and
parsed the ELF, the DWARF line table and the raw 8-byte bins directly.

⭐⭐ **THE NUMBER THIS PROJECT NEVER HAD.** 13,580 busy ticks / 1,889 presented
frames = **71.9 ms of CPU per frame — the title/demo is capped at ~14 FPS by CPU
alone**, before any waiting.

| family | ms/frame | % of busy |
|---|---:|---:|
| **audio** | **17.2** | **23.9** |
| emu64 | 15.4 | 21.4 |
| `dc_gx`/`dc_pvr` | 15.3 | 21.2 |
| GX shim | 7.0 | 9.7 |
| game logic | 5.9 | 8.3 |
| `mem*` | 4.8 | 6.7 |
| console (removable) | 3.9 | 5.4 |

🔴 **THE IDLE SHARE IS AN ARTEFACT AND SO IS `thd_block_now`.** `HIST_COUNTER_TYPE`
is a **uint16**; with the profiler armed the histogram thread sits in
`STATE_POLLING`, so `thd_idle_task` spins on `thd_pass()` instead of
`arch_sleep()` and self-samples ~10^5/s into a bin that **wraps every 0.2-0.5 s**.
Proof: the `sleep` instruction's bin has zero samples in all three runs.
`thd_block_now` saves **PR as the "PC"**. And the callback only counts a thread
still `STATE_RUNNING`, so **blocked threads are never sampled** — blockers are
UNDER-represented, the opposite of what `kb/hardware-profiling.md` §8 claimed,
which is why `dc_dvd_read_yielding()` shows zero samples.
⚠️ **Two readings were built on the bad half before this was found**, mine
included: "hardware blocks 5× more than Flycast" (it is boot, and 85 % of those
samples were banked by frame 69) and the `%ni` renormalisations. The busy-side
bins are honest 100 Hz timer samples and everything above rests only on those.
⭐ **One line fixes it** — `if(!irq_inside_int()) return …` in
`gmon.c:histogram_callback`. Needs a patched `libgprof.a` in the SDK image.

🔴 **THE ICACHE PREDICTION IS FALSIFIED.** `kb/hardware-profiling.md` §7 said
`dc_gx_backend_submit` (1.24× the icache) is "where the hardware run's share
grows". It **shrank** — 9.79 → 7.96 (0.81×), and the whole GX setter family with
it (`GXPosition3f32` 0.37×). The one grower is audio, 2.13× as a family.
⚠️ A share only sees DIFFERENTIAL slowdown, so "the draw path is icache-bound"
is untouched; one named suspect died. `istall` is still its only instrument.
⭐ `vid_waitvbl` collapsed 12.62 → 1.51 % (0.68 → 0.13 samples/frame): **the
hardware frame never waits for vblank.**

⭐ **THE 13.31 ms BLOCK SPLITS ~1 : 12** — index expansion 1.10 % of work, the
setters/staging half 9.34 %. G-B targets the staging half and can touch almost
none of the expansion.

**Shipped the same evening, both with kill switches and both compile-verified:**
P3 (`DC_RSPSIM_NOFP`) retypes `A_CMD_ENVMIXER`'s four `f32` accumulators to
`s32` through `make_src_shrink.py` — **bit-exact**, because the terms are
`>>16/18/19` of an s16 × u16 so the sum is < 46,000 and the final add < 2^24,
which f32 represents exactly; ~11 % of `RspStart`. And `dc_ctz32`
(`-DDC_NO_CTZ_LUT`), a de Bruijn LUT replacing `__builtin_ctz`, which on SH-4
was a `jsr` into a 96-byte libgcc loop on the per-referenced-vertex mark walks —
0.62 % of work, changes no logic.

**Priced and deliberately NOT attempted:** the texture-bind hit path (~3.65 % of
work — `tlut_content_hash()` still walks up to 512 B per bind, the cost T1
removed for textures and left for palettes). `probe` is **not** lookup-only
(`memcpy(e, &probe, …)` on the miss path), so the oversized `memset` is
load-bearing and any key change risks the aliasing that shipped two garbled-
texture bugs in S15. Needs a screenshot pair.

🔴 **VILLAGER ACTOR PROCS HAVE SAMPLES** — ~22 distinct `aNPC_*` functions in a
coherent think→move→draw tree, with `aNPC_dma_draw_data_proc` at **zero**. First
evidence the actors may EXIST and the break is inside DRAW. Run N3.

⚠️ **The scene was the title screen's DEMO** (human confirmed) — live actors,
camera and music, so it transfers further than "title screen" implies, but every
vertex-load-dependent figure needs a town run.

---

## 2026-08-12 (session 16) — P2 WORKS. THE §6 INSTRUMENT PRODUCED A REAL FLAT
## PROFILE, AFTER THREE BUGS THAT ALL FAILED SILENTLY

User directive: *"i have the dreamcast sd card reader in hand, get the iso ready
for the hardware test with logging like we need."*

**Starting state: the C was written and NOTHING was wired.** `dc/Makefile` and
`dc/build-dc.sh` contained zero occurrences of `gprof`, `-pg`, `DC_GPROF` or
`kosfat`, so `tools/dcprof/README.md`'s published build line was a **silent
no-op** — the exact `DC_EMU64_HIST` failure mode `build-dc.sh` documents in its
own comments. Any P2 "result" predating today is void.

**What landed.** The `DC_GPROF` block in `dc/Makefile` (nine knobs, `-pg` +
`-lkosfat` on the `$(TARGET)` recipe, keyed into `$(LINKSTATE)` and NOT
`$(FLAGSTATE)` — `-pg` changes no object, so keying it into the compile stamp
would rebuild ~3,900 TUs to relink one ELF); the matching forwarding in
`dc/build-dc.sh`; and in `dc/src/dc_profdump.c` the SD sink, the L+R+START dump
chord, and the six `g0`…`g5` stage markers.

**Link verified from `AnimalCrossing.map`:** our `gprof_init` at `0x8c2305c4`
from `dc_profdump.c.o` **wins**; `libgprof.a(gmon.o)`'s copy is discarded at
`0x0`. `monstartup`, `_mcleanup`, `fs_fat_mount`, `sd_init_ex` all resolved.

**THE RESULT — Flycast, console sink, town build, 2026-08-12:**

```
[GPROF] arming: range 8c010000..8c266062, predicted alloc 1506528 B, hz=100
[GPROF] Total memory allocated: 1506528 bytes     <- gmon.c's own line
[GPROF] END lines=28 raw=612433 enc=1585 crc32=2fd07100
```

`decode_gmon.py` → 612,433 B gmon.out, 306,190 bins, **31,010 samples in 470
non-empty bins**; `sh-elf-gprof -b -p` symbolised it:

| % | self s | symbol |
|---:|---:|---|
| 94.76 | 293.86 | `thd_idle_task` |
| 0.66 | 2.05 | `vid_waitvbl` |
| 0.51 | 1.59 | `dc_gx_backend_submit` |
| 0.50 | 1.56 | `scif_write` |
| 0.38 | 1.17 | `RspStart` |
| 0.20 | 0.63 | `emu64::set_position(unsigned int)` |
| 0.18 | 0.55 | `cull_batch(emu64*)` |

⭐ **z0 RLE is what makes it affordable: 612,433 → 1,585 B → 28 console lines.**
🔴 **Idle is 94.76 % — non-idle shares only, never absolute seconds.**
⚠️ `scif_write` at 1.56 s is the console tax appearing in its own profile.

**The three bugs, all of which failed silently, in the order they cost time:**

1. 🔴 **`fopen(…,"a")` is unusable on `fs_fat`.** `O_APPEND` `0x08` sits inside
   `O_MODE_MASK` `0x0f`, so `fs_fat_write()` returns `EBADF` on every append
   write — and `fs_fat_open()` does not check the mode, so the open succeeds.
   libgprof writes gmon.out with `"a"`. **Caught by an adversarial review before
   the burn, not by a run**; it would have produced a 20-byte file and a receipt
   saying "20 bytes". Fixed by never letting gmon.c open the card.
2. 🔴 **F5's `section-order.txt` is link-specific.** Generated from a
   keeplist-full link; against keeplist-town the image hangs in
   `maple_wait_scan()`, before `main()`. Four-build matrix: full+F5 boots,
   town+F5 hangs (with and without `DC_GPROF`), town+F5-off boots. ⚠️ **Hardware
   boots the town+F5 image** — the user played it — so this is Flycast-only.
3. 🔴 **Probing for an absent SD card wedges, behind a muted console.** KOS's
   `sci_spi_rw_byte` waits on RDRF with no cycle cap while every TDRE wait in
   the same file has one; SCIF's `acmd41_loop` is 5000 × 2 commands × 500 ms.
   `DC_GPROF_SD_IF` now defaults to 0 and "one CDI works with or without a card"
   is **falsified** — it is two images.

**Two wrong diagnoses this produced, both mine, both from the same root cause**
(a wedge behind a muted console looks exactly like a hang): *"`DC_GPROF_HZ=1000`
is crushing the frame rate"* — refuted, `[PERF]` read 24-28 FPS with the sampler
armed and `histogram_callback` is ~20 instructions; and *"K.K. Slider hangs"* —
refuted, the dump wedged while K.K. happened to be on screen. **Every gprof run
this session died exactly one 30-frame report short of its configured dump
frame** (299/300, 389/400, 599/600); one cause explained all of them, and the
pattern was visible in the logs long before it was read. The fix is a marker
line emitted *before* `dbgio_disable()`.

⚠️ **The build that went to hardware is `keeplist-town` with F5 off.** ~900 KB
less resident geometry than shipping, to buy the 1.53 MB the gmon buffers need.
A human read it as *"running noticeably better"* — **that is the missing
content, not a win**, and it must not be recorded as an FPS result.

Artefacts: `AC-DC-20260812b-gprof-sd.{cdi,elf,cdi.src.json}` on the NAS.

---

## 2026-08-09 (session 15) — T1 BUILT: −841,888 B, SPENT ON 650 MODEL FILES.
## ONE VISUAL FAULT UNRESOLVED, AND TWO OF MY OWN CHANGES CAUSED REGRESSIONS

User directive: *"pick up on the ram problem, using dynamic loading or whatever,
i want it so the whole game is loaded now. trying to get it playable past nook."*

**What landed.** `dc/src/dc_texpool.c` (T1, `DC_TEXPOOL_DEMAND=0` reverts),
`dc/src/dc_assetwin.c` (the shared mid-scene read window, `DC_ASSETWIN_B=0`
reverts), `make_stub_data.py --texpool-demand` with its `DEMAND_STUB` per-symbol
keep rule, and `tools/dcstub/keeplist-full.txt` from
`make_keeplist_town.py --full-model`.

**The RAM result, matched links, `_end − 0x8c010000`:**

| build | span |
|---|---:|
| baseline (T1 off, `keeplist-town`) | 10,375,116 |
| T1 on, same keep list | **9,533,228** (−841,888) |
| T1 on + `keeplist-full` @ 900,000 budget | 10,553,116 |

Runtime, 900 s town run: `fetch=181 staged=181 fail=0 toobig=0`,
`uploads=392 evictions=0`, `ASSET MISSING 0`, deepest scene 9.

**THREE THINGS I GOT WRONG, IN THE ORDER THEY BIT.**

1. **I turned on `-DDC_PVR_TEVP3` alongside the RAM work.** It was never part of
   the ask, it had never been run, and its default was OFF for that reason. It
   fired on **20,305 batches with `clamped=6941`** and recoloured the world.
   Removing it also took `us/v` 2.83 → 2.57 — it was costing ~10 % as well.
   **Lesson: do not bundle an unproven renderer flag into a memory change; the
   A/B stops being readable and the human sees one fault, not two.**

2. **The read-ahead window's first shape was a large net loss.** Built on
   `kb/levers.md` L10's "offsets are clustered, median gap 512 B, 863 of 905
   gaps ≤ 32 KB" — **which describes the offsets SORTED.** `dc_keep_sweep()`
   earns that by sorting the whole request list first; a bind-order demand
   loader cannot. Cost: **4,849,664 B off disc for ~190 KB of texture**, 148
   refills of 32 KB, hit rate 123/271, 94 `[STUTTER]` frames at ~320 ms with the
   time in neither `gx` nor `snd`. Now reads ahead only when a request continues
   where the last ended: 2,177,024 B, `narrow=169`. **Lesson: a distribution
   measured over a SORTED set is not a statement about arrival order.**

3. **The first `--full-model` budget landed 784 bytes under the ceiling** and I
   nearly called that a fit. The ceiling's libc-peak term dates from 2026-08-04
   and has never been re-derived. Cut to 900,000 for ~460 KB of real margin.

**THE PERF RESULT, matched runs, same probe set** (baseline = T1 off,
`keeplist-town`):

| | baseline | T1 + `keeplist-full` |
|---|---:|---:|
| `us/v` | 2.69 | **2.57** |
| FPS | 14.6 | **16.4** |
| `draw` ms | 52.3 | **47.4** |
| `v` | 3488 | 3001 |
| `[STUTTER]` | 90 | **147** |

⭐ **T1 plus 650 extra model files is FASTER than baseline**, which was not the
expected direction — removing `tex_content_hash()` from ~109 binds a frame is
apparently worth more than the extra geometry costs. The one real regression is
stutters; the time is in neither `gx` nor `snd` (mean unaccounted 84 → 157 ms),
so it is the demand reads.

4. 🔴 **I CALLED A SCREENSHOT PAIR A REGRESSION AND IT WAS THE TOWN SEED.** The
   T1 runs showed a wide lavender cobble ground where the baseline showed green
   grass, and I wrote "confirmed regression" on the strength of two frames. It
   was the paved plaza: the cobble is the SAME shade in both, grass is green in
   both, and the two runs seeded different towns and walked to different places.
   **Rule 11 is written about `us/v` and it applies just as hard to
   SCREENSHOTS** — `v=3488` vs `v=3001` across the same pair looks like a 16 %
   geometry win and is equally meaningless. To compare frames, compare the same
   acre, or compare many frames, or do not compare.

✅ **AND THEN A GENUINELY MATCHED PAIR SETTLED T1's VISUALS.** `t1only` — T1 on,
`keeplist-town`, everything else identical to the baseline — drew **v=3507
against baseline's 3488, 0.5 % apart, i.e. the same town** — and frame 90 of
each is the same scene with no visible difference. Rule 2 satisfied, for T1 at
least. Four-way table:

| | baseline | t1only | T1 + `keeplist-full` (win off) |
|---|---:|---:|---:|
| `v` | 3488 | 3507 | 4007 |
| `us/v` | 2.69 | 2.67 | 2.61 |
| FPS | 14.6 | 15.5 | 16.2 |
| `[STUTTER]` | 90 | 110 | 109 |

**T1 is free per vertex and costs ~20 stutters.** ⚠️ The lesson from #4 above
cuts both ways: this pair is trustworthy BECAUSE `v` matched to 0.5 %. Check
that before believing any frame comparison in this project.

**STILL OPEN, WHICH IS WHY NOTHING WAS COMMITTED.** A human reports wrong/
garbled textures and missing geometry that no captured frame reproduces —
including the matched pair above.
Everything checkable statically is clean: no full-size load survives into a
`[1]` array (462 rewritten files); no decoder over-read at any of 7,680 resolved
operand sites; `dc_dvd_read_yielding()` seeks once then reads sequentially; the
`goto swapped` reroute still byte-swaps; **T1 stubs exactly 1,379 extra symbols
and every one is a T1 texture row, un-stubbing nothing**; and `keeplist-full` is
a strict superset of `keeplist-town`. So the missing geometry is the **904 model
files / 1,128,096 B the budget dropped**, which were equally absent before
today. The live suspect is a wrong texture on a class the autowalk never binds;
`t1only.cdi` vs `base.cdi` is the experiment that isolates it.

🔴 **IT IS NOT KNOWN WHETHER THE LAVENDER GROUND IS NEW.** No screenshot of this
port's town predates today, so there is nothing to diff against. `base.cdi`
(T1 off, old keep list) is the reference and `nowin.cdi` (T1 on,
`DC_ASSETWIN_B=0`) is the discriminator; both are built.

⚠️ **A SELF-INFLICTED DETOUR WORTH RECORDING, because it is a whole class of
mistake.** I concluded twice that a run had died with its window closed, and
both times I was wrong: `ps aux | grep -c "[f]lycast"` is CASE-SENSITIVE and the
binary is `/Applications/Flycast.app/Contents/MacOS/**F**lycast`, so the check
returned 0 for every healthy run. On that false reading I killed a baseline run
that was working, told the user runs were being destroyed, and left an orphan
Flycast process behind. **`console.log` really is written only at exit
(`kb/RESUME.md` §2), so "no log yet" is the NORMAL state for most of a 900 s
run — it is not evidence of anything.** Use `grep -i`, and prefer waiting on the
log file to inferring liveness from a process list.

**A number I reported too confidently and am correcting:** I told the user the
bigger keep list draws "32 % more vertices" from `v=3640` against a `v=2745`
baseline. Two boots of the same new build measured `v=3001` and `v=3640`. **The
town reseeds every boot (rule 11), so those are different towns and the
comparison is indicative at best, not a measurement.**

## 2026-08-09 (session 14b) — THE PIVOT TO PLAYABILITY, AND T1's PROBE WAS
## MEASURING ITS OWN STUB PADDING

User directive after the S14 burn: *"the FPS is now good enough on hardware.
let's see if we can eliminate stub loading to make the full game playable on
hardware if possible."*

### "Eliminate stub loading" is the wrong lever, and the right goal

`DC_ASSET_STUB=0` is refuted by a boot and fails in the OPPOSITE direction:
`margin=-781036 OVER`, a failed 15,638,528 B contiguous malloc, and **all
14,495 assets MISSING** — the non-stub image has LESS content than the stubbed
one (`kb/closed.md`). **The stub system already IS the demand loader**
(`dc_stub_keep_load_one()` / `dc_keep_sweep()` live inside `#ifdef
DC_ASSET_STUB`). What limits the game is that the **keep list is static**: 765
entries chosen at build time. The town prints `ASSET MISSING 0` because its list
is complete; the missing content is in the scenes the list does not cover.

So the goal is **"stop letting a build-time list decide what exists"**, and the
biggest lever for it is **T1** (`kb/levers.md` L10).

### T1's falsifier ran twice and taught a lesson about instruments

| run | scenes | verdict |
|---|---|---|
| 300 s | `0 → 3 → 4` — **never reached the town** | `interior=0 mutated=0 oversize=0 aliased=0`, 726,570 binds |
| 700 s | `0 → 3 → 4 → 18 → 9` | 🔴 `interior=4318`, 2,187,050 binds |

🔴 **The 4,318 are a PROBE ARTIFACT.** All of them named one symbol,
`ef_doyon01_00`, at +68/+324/+480. It is not in `keeplist-town.txt`, so the stub
tree gives it `u8 ef_doyon01_00[1]` while the probe's map still carries its real
1,024 B — and the linker packed ~50 other small symbols into that window
(0x8c684xxx: `dna_win_*_pal`, `ef_ame02_*_v`, `ef_anahikari01_*`). Every bind to
a NEIGHBOUR was charged to `ef_doyon01_00` as an interior pointer.

⭐ **`dc_texpool.c`'s own header had already stated the premise and the code did
not implement it.** Fixed: a row with `kept == 0` is one byte, so anything past
its base is `unmapped`. Every `interior=` figure printed before the fix is void.

### ✅ AND THE RE-RUN CLEARS T1

`smoke-texprobe3-20260809-142619`, **deepest scene 9**, 17,609 frames:

```
[DC/TEXPOOL] VERDICT interior=0 mutated=0 oversize=0 aliased=0
[DC/TEXPOOL] binds=2074009 mapped=560487 interior=0 unmapped=1513522 distinct=127
```

**All four killers zero over 2.07 M binds with the town exercised.** A synthetic
identity key is a legal substitute for the content hash ⇒ **T1 needs ONE
~24,576 B staging buffer, not an N-slot pool.** The remaining risk is not
correctness but **seeks**: ~306 per run = 6-30 s of mid-scene hitching on CD-R
unless it uses `dc_keep_sweep()`'s read-ahead window — which **R1 still does not
use either**, so the shared helper is the first piece of work, not the loader.

⚠️ **A SHORT RUN IS NOT A CHEAP RUN, IT IS A DIFFERENT RUN.** The 300 s pass
returned a clean all-four-zero verdict purely because it never entered the town
— the same blind spot `kb/RESUME.md` §8 records for the census. **Check
`deepest_scene` before believing any "all clear".**

### The measured population, and a sixth kb figure corrected

```
[DC/TEXPOOL] map=6092 rows resident=1381/885984 B stubbed=4711/2132352 B
```
`kb/levers.md` L10 phase 2 claimed "5,685 textures / 2,782,080 B". Measured
against the real build and keep list: **4,711 rows / 2,132,352 B** — ~30 % less
content on offer than advertised, though still the best bytes-per-content ratio
in the file.

### The hardware picture, refined

Human, same session: ***"music doesn't cut out at all or stutter on hardware,
but the FPS is still definitely worse than emulator."*** ⚠️ **This CORRECTS an
over-claim made earlier the same day**: the audio was written up as
corroborating the median frame rate. It does not. At `DC_AUDIO_MAX_FRAMES=6` the
sustained floor is ~4.8 FPS — cleared long ago — and `[STUTTER]` fires on frames
that individually blow the budget. **"No skipping" certifies the p99 frame time
came down and says nothing about p50. Tail fixed, median still short.**

**The median gap has still never been measured on hardware.** Only
`AC-DC-20260809a-pmcr.cdi` can, and it splits the diagnosis three ways: `istall`
high ⇒ icache, F5 was right, more layout work pays (and
`AC-DC-20260809c-nof5.cdi` sizes F5 alone); `dstall` high ⇒ the data side;
neither, just `cyc` ⇒ raw work, i.e. G-B(2)'s 13.31 ms.

---

## ⭐⭐⭐ 2026-08-09 (session 14) — BATCH S14: EIGHT CHANGES IN ONE PASS, A WASH
## IN FLYCAST BY CONSTRUCTION, AND THE FRUSTUM TEST TURNED OUT TO BE 0.14 ms

Full write-up and the rollback contract: **`kb/batch-s14.md`**. Runs:
`smoke-s14-20260809-130522` (600 s) and `smoke-s14b-20260809-131916` (240 s,
the split-bracket build). Build line as session 13, plus `-DDC_PERF_PHASE` and
`DC_PVR_VTXSPLIT=16`.

**The directive was "focus sh4zam".** It was re-costed first and does not
survive: every FP stage of the vertex path is ~0.76 ms of a ~29 ms frame, `xf`
is 0.22 ms, and sh4zam already contributes zero instructions to the image while
we emit FTRV/FIPR/FSRRA through KOS. The batch was aimed at memory traffic,
cache layout and removed work instead — above all at the **instruction cache**,
which is the one plausible explanation for "hardware is much worse than the
emulator" that no emulator run can ever see.

### What shipped, all ON by default, each with a kill switch

32-byte memo stride · `oargb` store dropped · source-vertex `pref` ·
`GXNormal*` skipped on unlit batches · Gribb-Hartmann frustum cull ·
decal-Z arming default ON · **F5 linker section ordering** · a timing bracket on
G3's cull. Table, switches and the one-line revert: `kb/batch-s14.md` §2/§4.

### ⭐⭐⭐ THE HARDWARE VERDICT — IT RUNS BETTER, AND THE AUDIO PROVES IT MECHANICALLY

Human verdict on a burned CD-R of `AC-DC-20260809b.cdi`, same day:
***"definitely runs better on real hardware"*** and ***"sound is perfect. no
skipping"*** (the comparable Flycast run booked `[STUTTER] 65 / 900 s`).

⭐⭐⭐ **FIRST WIN THIS PROJECT HAS EVER BANKED THAT THE EMULATOR COULD NOT SEE.**
Flycast measured the same batch as a wash (below). The two do not conflict —
four of the seven changes pay only in cache misses and **Flycast models neither
cache**, so `us/v` 2.48 was never the result, it was the floor.

⭐ **The audio is an instrument, not a second opinion — but of the TAIL.**
`DC_AUDIO_MAX_FRAMES` is an FPS constant (`kb/RESUME.md` §5 audio rule 4):
production is capped at `MAX_FRAMES × 17.49 ms × 2 ticks` **per PRESENTED
frame**. ⚠️ **At 6, the sustained floor is ~4.8 FPS — a bound this port cleared
long ago — so "no skipping" does NOT mean "median FPS rose".** `[STUTTER]` fires
on frames that individually blow the budget, so what it certifies is that the
**p99 frame time came down.** The human confirmed exactly that split later the
same session: ***"music doesn't cut out at all or stutter on hardware, but the
FPS is still definitely worse than emulator."* Tail fixed, median still short**,
and the two halves measure different things. (An earlier draft of this entry
read the audio as corroborating the median. It does not.)

⚠️ **Direction, not magnitude, and NO attribution.** It does not say which of
the seven did it, and cannot rule out one of them being a small regression
masked by the others. `AC-DC-20260809c-nof5.cdi` exists for exactly that: same
objects, same flags, **only the linker's section order differs**
(`DC_SECTION_ORDER=0` is keyed to `link.stamp`, so it relinked with **0**
recompiles). Burning it isolates F5 alone — the cleanest A/B this project has
ever had. `AC-DC-20260809a-pmcr.cdi` gives the number, via `istall`.

⭐ **NEW MEASUREMENT RULE EARNED: a Flycast "no change" is not evidence against
a change whose mechanism is CACHE.** The emulator can falsify an
instruction-count claim; it can never falsify a locality claim.

### The Flycast verdict: NO RESOLVABLE CHANGE

`us/v` **2.51 → 2.48** (inside the ±2 % floor, rule 11); every `[VTXSPLIT]`
bucket within 0.03 ms; memo hit 53.7 → **54.2 %**; arming reach
`vid=1800/61920 → 2670/73080` against a prediction of `2670/72810`.

**That is the expected result for four of the eight** — S14-1/-3/-6/-7 all pay
in cache misses and **Flycast models no cache of either kind**. The two counters
an emulator *can* move both moved, in the right direction, by the predicted
amount. ⚠️ **It is not evidence for the batch either.** The verdict is a burn.

### 🔴 THE REAL FINDING: `cull_batch()` IS 3.05 ms/frame AND THE FRUSTUM TEST IS 0.14

`dc_emu64_cull.cpp` had **zero** `dc_time_us()` reads, so G3's own cull had never
been timed — only `dc_gx.c`'s *late* cull (0.70 ms) ever had. The first bracket
read **6.8 ms/frame, 23 % of the draw**, which reads as "the frustum test is
enormous" and is not what it says. Splitting it three ways (176 windows):

| bucket | wraps | ms/frame | cull-heavy | share |
|---|---|---:|---:|---:|
| `cus=` | all of `cull_batch()` | 3.05 | 7.26 | 100 % |
| **`cds=`** | **emu64's `dirty_check` + `setup_1tri_2tri_1quad`** | **2.23** | **5.26** | **73 %** |
| **`fus=`** | **`dc_gx_aabb_is_offscreen()`** | **0.139** | 0.292 | **4.5 %** |

⭐ **G-F IS RETIRED IN BOTH SHAPES** — the cheap Gribb-Hartmann one shipped as
S14-5 and the FTRV one will never be worth writing: the whole test is under
0.5 % of the frame.
⭐ **`cds=` is the new lever: 2.2 ms/frame that nobody has costed.** It is the
price of the ordering rule — the frustum test reads `projection_mtx` and
`current_mtx` live, so G3 must make emu64 refresh them before it can test
anything. In a typical window only **175 of 2,572** TRIN batches are culled, so
~93 % of it is spent on batches whose original handler then calls the same two
functions again. The in-file comment calls the second call "idempotent";
**idempotent is not cheap, and that has never been measured.**

### 🔴 THE GATE KILLED ONE OF THE EIGHT — S14-4 IS A STRICT NO-OP

`-DDC_GX_NRMSKIP_VERIFY`, 360 s town (`smoke-gate-20260809-132911`):

```
[GXVERIFY] nrmskip=0 nrmskipchk=427327 nrmskipbad=0 ghcullchk=1576071 ghcullbad=0
[EMU64C]   ... vidchk=14065122 vidbad=0 over=0 ... reinst=0
```

`ghcullbad=0` over **1,576,071** checks and `vidbad=0` over **14,065,122**
certify S14-5 and S14-6 on target. But **`nrmskip=0`**: the unlit-`GXNormal`
skip never fired once in 427,327 batches that offered a normal. The reason,
found in the decomp only after the counter said so — **emu64 already guards the
call** (`emu64.c:2785-2787`, `if ((this->geometry_mode & G_LIGHTING) != 0)`).
An unlit batch never reaches `GXNormal*` at all; the work had been removed
upstream in `src/` by the original developers years ago.

**Default flipped to OFF the same day** (now opt-in `-DDC_GX_NRMSKIP`); the code
and its gate are kept as the evidence. `kb/research-sh4zam-gap.md` G-J ranked it
as *"work removal, and it outranks every arithmetic idea in this table"* without
checking the call site. **Check the CALLER before costing a callee's skip** — and
note the two predicates were not even the same one (emu64's `G_LIGHTING`
geometry-mode bit vs `dc_gx`'s channel-control state).

⭐ **This is the gate doing its job on the first day, which is the argument for
building the gate at all.** The change was harmless, compiled, passed every
correctness check, and did nothing — the only instrument that could tell the
difference was a counter that says how often the fast path was taken.

### Three kb figures falsified

1. **i-cache pressure was quoted as 11.9× / inner loop 1.4×.** The tool's
   hot-set regexes (`^_dl_G_`, `^_emu64`, `^_cu_trin`) matched **nothing** —
   emu64 is C++ and every handler is mangled. Map check: `.text._ZN5emu64*` =
   **105** sections, `.text.dl_G_*` = **0**. The interpreter, most of the draw,
   was absent from the measurement of its own cache pressure. Real:
   **16.40× / 2.62×**. Worse pressure, *lower* F5 ceiling — at 2.62× the inner
   loop can be made contiguous but never resident.
2. **`--section-ordering-file` takes a linker-script `SECTIONS` fragment**, not
   a list of names, and it **must open with a bare `*(.text)`** or `start` is
   hoisted off `0x8c010000` and the image does not boot.
3. **"a skipped normal is stale"** — no, it is **zero**: `GXPosition3f32` clears
   `normal[0..2]` per vertex. Safer, and it should raise the memo hit rate.

### And a denominator nobody had checked

`vlit/v` — the reach of the unlit-`GXNormal` skip — is **view-dependent**:
p50 **93.4 %** lit on one run, **54.4 %** on another, mean 75.7 % / 61.3 %.
Quote it as a distribution or not at all. The 600 s `us/v` run sat in a 93.4 %
view, which is exactly why it could not see S14-4.

### Not bundled, deliberately

**G-B(2), the indexed-submit rewrite (13.31 ms)** — a multi-session
architectural change inside a bundled A/B tells you nothing about the other
seven. Also TEV P3 (a correctness fix that would *add back* the `oargb` store
S14-2 removed), §0a/G-D, KOS at `-O3 -flto`, and F6/OCRAM.

---

## ⭐⭐⭐ 2026-08-09 (session 13) — THE VERTEX-INDEX SIDE CHANNEL SHIPPED
## (`us/v` 2.68 → 2.51), THE SHADE HOIST IS NEUTRAL, AND THE SHORTCUTS ARE DEAD
## FOR A REASON THAT IS NOW UNDERSTOOD

Four measured runs, **one build line for all of them**, 600 s each, Flycast:

```
town, keeplist-town.txt, DC_ASSET_STUB=1, DC_ARAM_WINDOW=1048576,
DC_ARENA_BYTES=1200000, DC_AUDIO_SCENES=all, DC_AUDIO_DISC_FRAMES=8,
DC_AUDIO_VOICES=12, DC_AUTOSTART=1, DC_PVR_VTXSPLIT=16, -DDC_PERF_PHASE
```

```
build                        us/v  draw xform  sum  memo shade  lit  tex post emit   xf     v  hit%
ctrl (neither change)        2.65  29.1   7.5  6.66  1.27  1.99 0.58 0.57 0.58 1.45 0.23  2820  50.5
shade hoist only             2.68  28.3   7.3  6.65  1.26  1.98 0.61 0.62 0.56 1.40 0.22  2739  50.9
G-B + hoist                  2.51  29.2   6.9  6.31  1.20  1.82 0.54 0.57 0.55 1.40 0.22  2745  53.7
G-B + hoist + shortcuts      2.54  28.5   7.0  6.33  1.18  1.89 0.53 0.56 0.57 1.39 0.20  2745  53.7
G-B, hoist OFF               2.56  27.8   7.0  6.45  1.26  1.91 0.55 0.59 0.55 1.40 0.20  2754  54.1
```

All five: **`ASSET MISSING 0`, `reinst=0`, `dropped=0`.**

🔴 **THE FIFTH RUN GAVE THIS SESSION ITS MOST REUSABLE RESULT: THE NOISE FLOOR
ON `us/v` IS ~±2 %, AND NOBODY HAD MEASURED IT.** The shade hoist reads
**+1.1 %** against `ctrl` (2.65 → 2.68) and **−2.0 %** against `G-B, hoist OFF`
(2.56 → 2.51). **The sign flips.** Grouped, the five runs are 2.65 / 2.68
without G-B and 2.51 / 2.54 / 2.56 with it — the groups separate cleanly, the
members within a group do not. `us/v` normalises for vertex COUNT but not for
WHICH vertices, and the lit / textured / punch-through mix moves with the town.

**Consequences, and they are general:**
- **The hoist is INSIDE THE NOISE and its sign is NOT determined by this data.**
  Any claim that it helped or hurt is unsupported. It is kept ON because it
  single-sources a predicate that was written out twice, not because it is fast.
- **G-B's −6.3 % is ~3x the floor**, and what carries it is the two-GROUP
  separation, not any single pair.
- ⚠️ **A change worth less than ~4 % CANNOT be resolved by one A/B pair.**
  Session 12's wins were 8-18 % and were safe on one run each; that precedent
  does NOT license a 2 % claim. Run each arm 2-3 times, or report the result as
  inside the floor — do not quietly pick the favourable pair.

⚠️ **Each run drew a DIFFERENT TOWN** (`v = 2820 / 2739 / 2745 / 2745 / 2754`; the seed
is `sqrand(osGetCount())` per boot, `sys_math.c:7`). **`us/v` is the instrument;
`draw` is not** — `draw` moves 28.3-29.2 across rows that differ by a change
worth −6 % on `us/v`, in both directions, which is exactly what a different town
looks like.

### 1. ⭐ G-B — THE VERTEX-INDEX SIDE CHANNEL. `us/v` 2.68 → 2.51, −6.3 %. SHIPPED, ON BY DEFAULT

⚠️ **FRAMING, BEFORE THE MECHANISM: this is NOT the 13.31 ms block.** That
figure is `dl_G_TRIN`'s index expansion **plus our own `GX*` attribute
setters**, and this change removes neither. It makes the **memo** cheap. **The
indexed-submit rewrite — transform each unique vertex once, index into it, delete
the setters — is still open and is still the largest single block in the
project.** Several kb files already blur the two; do not add to that.

**What it is.** `dc/src/dc_emu64_cull.cpp`'s AABB walk already visits the batch's
indices **in exactly the order `set_position3()` will replay them**, so it now
*records that sequence* and hands it to `dc/src/dc_gx.c` (`dc_gx_vtxid_arm`).
`GXPosition3f32` consumes the sequence with a cursor and stamps
`(epoch << 8) | index` into `DCGXVertex` **bytes 30-31, which were dead
padding** — `sizeof(DCGXVertex)` stays 32 and session 12's `aligned(32)` is
untouched. `dc/src/dc_pvr.c`'s vertex memo then keys on that stamp:

- no hash,
- no 30-byte content compare,
- ⭐ and above all **no random read into `verts[]`** — which is the
  operand-cache miss that made `memo` **122 cycles a vertex** (session 11b).

Kill switch **`-DDC_GX_NO_VTXID`**.

**The correctness gate RAN.** `-DDC_GX_VTXID_VERIFY` content-checks **every** id
hit against the old compare: **`vidchk=15,538,941 vidbad=0 over=0`**
(`smoke-gbverify-20260809-100644-39747`). Not one disagreement in 15.5 M checks,
and `over=0` says the cursor never ran past the recorded sequence.

**The epoch is load-bearing, not defensive.** `GXBegin` **merges** batches
(`pc_gx_merged_batches`), so one submit can hold two TRIN commands, and emu64
**reloads `vertices[]` between them** — a bare index would hand the second TRIN
the first TRIN's transformed vertex. The epoch makes a stale id miss.

**Reach: 100 % of what is legal to arm.** Armed only where the batch survives the
frustum test AND none of `dc_emu64_cull.cpp`'s three punts fired. Per 30-frame
window: **`vid=1770/61470` against `vis=1770`** — i.e. every visible TRIN batch,
61,470 stamped vertex references.

⚠️ **WHERE THE WIN LANDED IS NOT WHERE IT WAS AIMED.** `memo` itself moved only
**1.26 → 1.20 ms**. The gain is in `shade`/`lit`/`tex`/`post`
(3.77 → 3.48 ms combined), because **those stages are charged on memo MISSES**
(measurement rule 10) and the **hit rate rose 50.9 → 53.7 %**. Misses per frame
fall ~5.5 %, the five miss-charged stages fall ~7.3 %; the miss-count drop is
about three-quarters of it and the remainder is consistent with the deleted
random read no longer evicting the lines the rest of the loop wants.

⚠️ **AND FLYCAST UNDERSTATES THIS BY CONSTRUCTION.** The thing the side channel
deletes is *a random read that misses the operand cache*, and **Flycast models no
cache**. **2.51 is a FLOOR**, not the hardware figure.

### 2. THE SHADE-PREDICATE HOIST IS NEUTRAL AND THE SHORTCUTS ARE DEAD — AND THE MECHANISM IS A COMPILER ONE

`kb/RESUME.md` session 12 said the `LAZYRGBA`/`ALPHA8` shortcuts lost because
their predicates ran **per vertex**, and told this session to hoist them next to
`need_light`. **That was done** — `shade_batch_mode()`, kill switch
`-DDC_PVR_NO_SHADE_HOIST`. Both halves of the prediction failed:

| | `us/v` | `shade` |
|---|---:|---:|
| ctrl | 2.65 | 1.99 |
| **hoist alone** | **2.68** | 1.98 |
| G-B + hoist | 2.51 | 1.82 |
| **+ shortcuts on top** | **2.54** | **1.89** |

The hoist alone is **no better** (+1.1 %, i.e. noise-to-slightly-worse). The
shortcuts on top of the hoist are **WORSE** — `shade` 1.82 → 1.89, `us/v` 2.51 →
2.54 — while the shortcut fired **`shade_a8 verts=10,184,262`** times. So it is
not that the fast path is rare; it is that having a fast path costs more than it
saves, twice, measured two different ways.

⭐ **The mechanism, which is the part worth keeping.** With the shortcuts OFF,
`need_rgb` / `need_a` are **compile-time constants** and GCC straight-lines the
block. **Hoisting turns them into runtime variables loaded from a bitmask**, so
`if (need_rgb)` becomes a real branch with **both arms emitted**. The predicate
got cheaper to compute and the loop got harder to schedule — fewer evaluations of
a test, worse code around it.

**This generalises the existing rule** ("a per-vertex predicate is not a saving",
`kb/traps.md`): **moving a predicate out of a loop does not help if it was
already a constant IN the loop.** Recorded as settled-negative in
`kb/closed.md`; do not propose a third variant of this without a new mechanism.

### 3. THE PUNT SPLIT IS MEASURED, AND IT RE-OPENS ONE PUNT

```
[EMU64C] trin=6990 cull=3660 vis=1770 punt=1560 pdec=900 ptgen=0 pmix=660
```

Per 30-frame window: **52.4 % culled, 25.3 % visible, 22.3 % punted.** Of the
punts, **the decal-Z punt is 900 — 58 % of all punts — and `G_TEXTURE_GEN` is
ZERO.**

⭐ **The three punts exist because CULLING a batch changes semantics. ARMING
skips nothing** — it only needs the weaker property *"the same index means a
byte-identical staged vertex within this submit"*.

- **Decal-Z MEETS the weaker bar.** It recomputes a transient position from the
  same `emu_vtx->position` through the same matrices on every reference.
- **`G_TEXTURE_GEN` and mixed `MTX_NONSHARED` do NOT.** Both *mutate*
  `vertices[]` — per reference, and on first touch, respectively.

**Lifting the decal-Z punt FOR ARMING ONLY takes armed batches 1770 → 2670,
+51 % reach.** ⚠️ **NOT DONE, NOT MEASURED.** It is the top ranked next action.

### Ranked next actions (2026-08-09)

1. ⭐ **The decal-Z arming lift — WRITTEN, GATE PASSED, PERF NOT MEASURED.**
   `-DDC_GX_VTXID_DECAL`, **default OFF**. Decal batches now arm but are still
   never frustum-tested and never culled. Gate with it ON:
   **`vidchk=15,835,845 vidbad=0 over=0`**; reach `vid=1800/61920` →
   `2670/72810` (**+48 % batches, +18 % refs**), `viddec=900` of `pdec=1020`.
   ⚠️ Refs only +18 % (decal batches are ~12 refs, not ~35), so the `us/v`
   effect sits AT the ±2 % floor and needs REPEATED runs (rule 11). Four were in
   flight at flush — read them before defaulting it ON.
   ⚠️ `-DDC_EMU64_CULL_VERIFY` cannot certify it (decal batches never cull);
   `-DDC_GX_VTXID_VERIFY` is the gate.
2. 🔴 **The indexed-submit rewrite — the 13.31 ms block.** Still the largest
   single block in the project, still untouched by this session, still a
   multi-session change. The side channel is *evidence for* it (a stamped
   sequence already exists) but it is not it.
3. **The hardware PMCR burn** — unchanged, and still the only instrument that
   can price what Flycast hides. Every number above is a floor.

### Runs

| run | what |
|---|---|
| `smoke-ctrl-20260809-094257-38338` | ctrl, neither change |
| `smoke-cand-20260809-095257-38954` | shade hoist only |
| `smoke-gbverify-20260809-100644-39747` | `-DDC_GX_VTXID_VERIFY` gate — `vidchk=15,538,941 vidbad=0 over=0` |
| `smoke-gb-20260809-101618-39854` | G-B + hoist |
| `smoke-shade2-20260809-103932-40220` | G-B + hoist + shade shortcuts |

⏸ **A fifth run is in flight: G-B with `-DDC_PVR_NO_SHADE_HOIST`**, isolating the
hoist from the side channel. Until it lands, "G-B is −6.3 %" is measured against
the hoisted build, and the hoist's own contribution is bounded by the ctrl row at
±1 %.

## ⭐⭐⭐ 2026-08-08 (session 12) — G-C SHIPPED, AND THE MEMORY-BOUND READING
## PAID: `us/v` 3.24 → 2.65, THREE MEASURED RUNS, ONE CHANGE EACH

Session 11b said the frame is memory-bound and ranked the queue by it. This
session spent the queue. **Every number below is a run, not an estimate**, all
on the same build line (town, `DC_PVR_VTXSPLIT=16`, `-DDC_PERF_PHASE`, static
camera, 600 s), and each row differs from the row above it by **one change**.

```
                     memo    xf   lit   tex shade  post  emit |  sum  xform  us/v  draw   FPS
baseline (all off)   1.74  0.23  0.62  0.62  2.13  0.57  2.20 | 8.11   8.9   3.24  30.7  25.2-26.5
+ G-C (pvr_dr_*)     1.77  0.23  0.64  0.63  2.19  0.58  1.45 | 7.49   8.2   2.89  29.3  26.6-27.4
+ align + wordcmp    1.26  0.22  0.61  0.61  2.09  0.53  1.43 | 6.76   7.2   2.65  27.7  27.7-28.0
```

**`us/v` −18.2 %. `xform` −1.7 ms. `draw` −3.0 ms. ~+2.5 FPS.**

⚠️ **The three runs drew DIFFERENT TOWNS** (`v=2745 / 2826 / 2739`) — the town
is reseeded from `sqrand(osGetCount())` every boot (`sys_math.c:7`), so matched
frames are impossible and `us/v` is the instrument precisely because it
normalises. Where a per-vertex figure is quoted below it is computed against
that run's own `v`.

### What each change was, and what proves it was that change

**G-C — `emit` 2.20 → 1.45 ms, −34 %** (normalised: 0.801 → 0.513 µs/vertex,
**−36 %**, i.e. it did 3 % more vertices in 34 % less time).
`emit_projected()` now writes the eight TA words straight into
`pvr_dr_target()` and `pref`s via `pvr_dr_commit()`, deleting the stack
`pvr_vertex_t` build pass, `sq_fast_cpy`'s read-back of it, and two calls per
corner. Counter `[DC/PVR] dr verts=30,386,676 of 37,204,221` = **81.7 %**,
which is exactly the non-punch-through share — PT keeps the old path because a
PT record must be *held* until list 4 can legally be opened. Kill switch
`-DDC_PVR_NO_DR`.

**`DCGXVertex` → `aligned(32)` + the branch-free memo compare — `memo` 1.77 →
1.26 ms, −29 %**, against the −0.48 ms predicted for the compare alone.
- The vertex was 32 bytes at `aligned(8)`, landing at `0x8c993088` (`&31 == 8`),
  so **every vertex straddled two 32-byte operand-cache lines**, split across
  the field boundary that matters: pos+texcoord+color in one, `normal` in the
  next. Both memo sides and the lighting path paid it.
- `vmemo_same()` was 12 compares and **12 dependent conditional branches**;
  it is now 8 XOR-OR loads per side and one branch. The live bytes are **30**,
  not 28 — `normal[2]` is live — so it is 7 × `uint32` **plus** a `uint16`.
- ⭐ **The check that matters: the hit rate did not move** — 51.0 % → 51.5 %.
  The compare changed the *cost of asking*, not the *answer*, so the two
  float-semantic deltas (NaN bits, ±0.0) cost nothing real.
Kill switches `-DDC_GX_NO_VTXALIGN`, `-DDC_PVR_NO_VMEMO_WORDCMP`.

### The kill switches, and an honest note about the `.text` gate

All four revert flags **compile and link together**:
`-DDC_PVR_NO_DR -DDC_GX_NO_VTXALIGN -DDC_PVR_NO_VMEMO_WORDCMP
-DDC_GX_LIGHT_LAYOUT_LEGACY` → `.text = 2,882,980`.

⚠️ **That is 32 B ABOVE the 2,882,948 the gate quotes, and I did not prove
where the 32 B went.** It is not a live feature — every switch is off — so the
suspect is the restructure of `emit_projected()` itself: the DR arm needed the
projected values in locals (`px`, `py`, `oargb`) before the branch, where the
old code stored them straight into `pv`'s fields, and that is enough to change
scheduling. **The claim "the killed build is byte-identical" is therefore NOT
made.** The check that would settle it — build pre-session HEAD with this exact
flag set and diff — was attempted and blocked, and is left as the first small
task next session. What IS established: the switches exist, they compile, and
they put `.text` back to within 0.001 %.

### Two corrections this session forced

1. 🔴 **`[VTXSPLIT]`'s per-vertex cycle figures were HALVED for five of seven
   stages.** `VS_MARK` for `xf`/`lit`/`tex`/`shade`/`post` is downstream of the
   memo-hit `continue`, so those stages are charged over memo **MISSES**, not
   over `v`. At 49.9 % hit: `shade` is **~295** cycles/vertex, not 148; `lit`
   is **~89**, not 44. `memo` — charged on every vertex — is unaffected at 122.
   **`kb/research-sh4zam-gap.md` §3's "44 cycles, about what six FIPRs should
   cost, so the block was never slow" is retired.** The demotion of §0a/G-D
   survives, but on absolute milliseconds (0.58 ms of 30) rather than on that
   argument.
2. **`GX_AF_SPOT` is unreachable in the entire tree.** `emu64.c:3316` gates it
   on `G_LIGHTING_POSITIONAL` (0x400000), which has exactly three references —
   a name table and its two readers — and no display list or runtime call ever
   sets it. So `chan_eval`'s spot branch, including its per-light FDIV, is dead
   weight. The town runs **exactly 2 lights** (sun + moon, unconditional,
   `m_kankyo.c:1282-1290`), not 8.

## 🔴🔴 2026-08-08 (session 11b) — G5 SPLIT `xform`, AND THE FRAME IS
## MEMORY-BOUND. EVERY sh4zam MATRIX IDEA IS AIMED AT 0.8 ms OF A 30 ms FRAME.

`[PHASE] us/v` — the number this project optimises against — is **3.24 µs per
submitted vertex, i.e. 648 SH-4 cycles at 200 MHz**, against maybe 60 cycles of
actual vertex arithmetic. Nobody had ever asked where the other ~590 went, and
`kb/research-sh4zam-gap.md` §0a was about to spend a session rewriting six
FIPRs as two FTRVs on the assumption that the vertex math was the cost.

`-DDC_PVR_VTXSPLIT=16` (G5) brackets seven stages of the vertex loop, sampling
one primitive in 16. Town, `[SCENE_MODE] 18 -> 9`, `xform=8.9`, `v=2745
vlit=2613`, `drops=9` of 558,095 samples. Per PRESENTED frame:

```
[VTXSPLIT] memo=1.68 xf=0.23 lit=0.58 tex=0.62 shade=2.03 post=0.57 emit=2.15
           | sum=7.87 prims=8929522 samp=558095 memohit=835733 drops=9 1in16
```

**The ledger closes: `sum 7.87` against `xform 8.9` — 88 % attributed**, the
1.0 ms residual being per-primitive loop overhead no bracket covers. Three
consecutive windows printed identical figures to 0.01 ms, so this is not noise.

### What it says, and it is not what the queue assumed

| | ms | share of `xform` |
|---|---:|---:|
| **emit** (near clip, divide, `pvr_prim`'s 32 B copy) | **2.15** | 27 % |
| **shade** (`shade_vertex`, the per-light loop) | **2.03** | 26 % |
| **memo** (hash + 12-field compare, on EVERY vertex) | **1.68** | 21 % |
| tex / post | 0.62 / 0.57 | 15 % |
| **lit** — ⭐ *the six FIPRs §0a wants to replace* | **0.58** | 7 % |
| **xf** — ⭐ *the position FTRV* | **0.23** | 3 % |

⭐⭐ **§0a and G-D are aimed at 0.58 ms of a 30 ms frame — 1.9 %** — and a
perfect rewrite takes maybe half of that. `lit` is 222 ns over 2,613 lit
vertices = **44 cycles**, about what six FIPRs, an FSRRA and a normalize ought
to cost. The block is not slow. The 2026-08-06 note that called it "already
optimal" was closer to right than the 2026-08-08 reopening of it, and this
is the measurement that settles the exchange rather than another argument.
**Every matrix-unit idea in that document is chasing ~0.8 ms combined.**

⭐⭐⭐ **THE FRAME IS MEMORY-BOUND.** `memo` costs **122 cycles per vertex** for
a hash and a 12-field compare — that is not arithmetic, it is the random read
of `verts[s_vmemo_src[slot]]` missing the operand cache. `emit` is a 32-byte
copy per corner into the store queue. The three memory-shaped stages are
**5.86 ms, 75 % of `xform`**; the two FP stages are 0.81 ms.

Two independent measurements now say the same thing from opposite ends: this
one, and `tools/dcopt/icache_map.py` finding the 12-symbol inner draw loop is
1.4x an 8 KB direct-mapped instruction cache. **And Flycast models neither
cache**, so both are understatements of the hardware.

### The re-ranking

1. **G-C (emit, 2.15 ms)** — `pvr_dr_*` instead of `pvr_prim`. KOS 2.3 makes it
   cheap: `pvr_dr_target()` is `pvr_dr_addr ^= 32`, `pvr_dr_commit` is
   `sq_flush`, and `pvr_list_begin` has already `sq_lock`ed the TA, so DR and
   `pvr_prim` share one QACR setup and may be mixed.
2. **`shade_vertex` (2.03 ms)** — has never been on any list. Now second.
3. **memo (1.68 ms)** — net positive today (~50 % hit rate saves ~4.0 ms for
   1.68 ms) but keyed on a content compare. G-B replaces it structurally.
4. **G-B (13.31 ms)** — still the largest block in the project and untouched by
   all of the above; it is emu64's expansion, upstream of this loop.
5. §0a / G-D / G-F — 0.58 + 0.23 + 0.70 ms. Last, if ever.

⚠️ **Static camera, one town view** — same caveat as the §3 run it extends.
⚠️ TMU2's tick is 80 ns and a stage is 100-600 ns, so a single sample is worth
±1 tick; only the window mean is meaningful and a 0.00 bucket would mean
"below the noise", not "free".

## ⭐⭐ 2026-08-08 (session 11) — P1: THE INSTRUMENT FOR THE HARDWARE GAP IS
## BUILT, AND THE FREE HOST-SIDE CHECK ALREADY SAYS THE ICACHE CANNOT HOLD THE
## INNER LOOP

Nothing here makes the game faster. Everything here makes the hardware gap
falsifiable, which it has not been for the whole life of the project.

### P1 — `dc/src/dc_pmcr.c`, the SH7750 performance counters

`DC_PMCR=1`. PRFC1 only, because KOS owns PRFC0 as `perf_cntr_timer_ns`'s
clock — the same split Xash3D DC uses and for the same stated reason. The
SH-4 has two counters and one is spoken for, so **exactly one event is
countable and the event ROTATES**: one per 30-frame report window, eight
events, a full table in ~12 s.

Events: elapsed cycles, **pipeline-freeze-by-icache-miss**,
pipeline-freeze-by-dcache-miss, icache misses, operand-cache misses, icache
fill cycles, operand-cache fill cycles, instructions issued. Buckets: `draw`
/ `skip` / `vi` (the same decomposition `[PHASE]` uses, bracketed at the same
two sites in `dc_vi.c`, so the two lines read together), plus `audio`
(`dc_audio_pump`) and `xform` (`dc_gx_backend_submit`) as sub-buckets.

**Per PRESENTED frame** — reported from the same 30-frame block as `[PERF]`,
and handed that block's own wall clock rather than reading a second one. Do
not double it (measurement rule 9).

It prices itself: `dc_pmcr_init()` times 1,000 counter reads, so `rd=` and
`rdns=` on every line bound the instrument's own cost. `bad=` is the tripwire
for a delta that went backwards, which is the only way the mode rotation
(which clears the counter) could corrupt a window. **Ran: `bad=0` throughout.**

### ⚠️ FLYCAST IMPLEMENTS NO PERFORMANCE COUNTERS AT ALL

Two full runs: **every event reads 0, including `PMCR_ELAPSED_TIME_MODE`.**
Not "small", not "approximate" — zero. So Flycast cannot validate even the
arithmetic, and a later reader must not mistake a zero row for a build that
never armed. The instrument diagnoses itself now:

```
[PMCR] ⚠️ elapsed-cycle count is ZERO over a whole window. PRFC1 is not
counting: that is EXPECTED under Flycast (it models no PMCR) and means this
run cannot answer anything about the hardware. Burn it.
```

What Flycast DID validate: the plumbing runs, the rotation advances through
all eight events, `bad=0`, `ASSET MISSING 0`, the run reaches the town at
19.5-20.6 FPS, and **the HUD renders legibly** — screenshot-verified, not
assumed (`/tmp/shots-pmcr/frame-0062.png`, 10 rows over the town).

### The HUD, and why the report cannot go over the console on a burn

`DC_PMCR_HUD=1` draws the table straight into the scanned-out framebuffer:
address read from **FB_R_SOF1 every frame**, never `vram_s` (that is the base
of VRAM and is not the displayed surface once `pvr_init()` has run —
`dc_pvr.c:3842` paid for that already). One buffer, not two: SOF1 is the
scanout register, so it always names the surface currently on screen, and each
surface is repainted while it is showing.

⚠️ **`DC_CONSOLE_MUTE=1` is the other half and it is not optional for a burn.**
KOS busy-waits on the SCIF TX FIFO whether or not a cable is attached. A perf
build emits `[PERF]`, `[PHASE]`, `[EMU64]`, `[EMU64C]`, five `[DC/PVR]` lines,
`[DC/TEX]`, `[DC/ARAM]` and every `[STUTTER]` **into the same 30-frame window**
— hundreds of bytes, tens of milliseconds of stall, charged to the frames being
measured. `dbgio_disable()` at the top of `main()` removes all of it in one
call, including the game's own `OSReport` and anything KOS prints, with no
per-call-site change and nothing to miss. **Verified:** the muted image booted,
ran 240 s without crashing, and printed **0** `[PERF]`/`[PHASE]`/`[DC/PVR]`
lines against 21 total console lines (KOS's own boot banner).
⚠️ It silences crash dumps too — that is the trade, and a triage burn should
leave it off. `-DDC_PMCR_LOG=1` puts the `[PMCR]` line back on the console for
emulator runs.

### 🔴 THE FREE HOST-SIDE FINDING: THE HOT SET IS 11.9x THE INSTRUCTION CACHE

`tools/dcopt/icache_map.py`, run against this tree's ELF. The SH7750 icache is
**8 KB, direct-mapped, 32-byte lines** — 256 lines, index = address bits
[12:5].

| hot set | bytes | vs 8 KB icache |
|---|---:|---:|
| the town frame (emu64 dispatch + `dl_G_*` + our `GX*`/`dc_gx_`/`dc_pvr_` + G3's cull + `Nas_*`) | **97,504** | **11.9x** |
| **the innermost draw loop only** — 12 symbols: `dl_G_TRIN*`, `emu64_taskstart_r`, `dc_gx_flush_vertices`, `dc_gx_backend_submit`, the `GXPosition/Color/TexCoord/Normal` setters, `cu_trin_*`, `set_position` | **11,648** | **1.4x** |

⭐ **Even the twelve functions of the inner loop do not fit in the instruction
cache**, and `dc_gx_backend_submit` — the single biggest addressable block in
the frame — shares cache lines with six of the `GX*` attribute setters it calls
per vertex (lines 91, 94, 98, 99, 100, 101). Every FPS number this project has
ever produced came from a machine where that costs exactly nothing.

⚠️ **This sizes the pressure; it does not price it.** The tool has no call
profile, so a collision between two functions that never share a frame costs
nothing, and capacity pressure is not the same as a measured stall. The number
that settles it is `istall` from P1 on a burn. But the direction is no longer a
guess: at 1.4x on twelve functions, the inner loop *cannot* be resident.

### The ordering flag, corrected before anyone spends a session on it

The plan said `--symbol-ordering-file`. **That is an LLD flag and sh-elf uses
GNU ld.** GNU ld 2.45.1 in our SDK image has **`--section-ordering-file FILE`**
instead, which does the same job because `-ffunction-sections` is already on
(`GC_CFLAGS`). Same lever, different spelling; `--sort-section name|alignment`
is also there and is not what this wants.

### What is staged

`AC-DC-20260808f-pmcr.cdi` (740,090,153 B, padded, burnable):
shipping config + `DC_PMCR=1 DC_PMCR_HUD=1 DC_CONSOLE_MUTE=1`, no
`DC_SCIF_FAST`, no `DC_AUTOSTART`, no probes. **It is a measurement image, not
a play image** — the HUD covers the top-left of the screen and the console is
dead.

## ⭐ 2026-08-08 (session 10) — G-A LANDED, AND THE DENOMINATOR EVERY sh4zam
## PROPOSAL WAS RANKED AGAINST IS NOW MEASURED RATHER THAN QUOTED

Two things happened. One is mechanical and shipped; the other retires a number
this project has been quoting since 2026-08-06.

### G-A — sh4zam could not link, now it can. Byte-neutral so far.

`dc/Makefile` globbed `source/*.c`, top level only, so **five out-of-line
symbols had no definition** and anything calling `shz_fft`, `shz_sq_memcpy32`,
`shz_memcpy128`, `shz_memset8` or the `shz_xmtrx_load_apply_store_*` forms
could not have linked. Nothing did, which is why nothing broke.

Fixed by naming `source/sh4/shz_complex_sh4.c` in the glob, adding `SHZ_S` for
the two `.s` files, and adding a `$(OBJDIR)/%.s.o: $(ROOT)/%.s` rule.
`source/sw/` stays out — `shz_xmtrx_sw.c` defines a conflicting `xmtrx_state_`,
so a *recursive* glob here is a build break, not a slowdown. `-DNDEBUG` is
scoped to `$(SHZ_OBJS)` only (sh4zam's alignment/FP-mode asserts are on by
default and would sit in the vertex loop).

Mechanical notes worth keeping: lowercase `.s` means gcc runs **no cpp**, so
that rule carries no `-MMD`, no prelude and no `$(DEFINES)`, and `DEPS` filters
the would-be `.d` out rather than `-include`ing a file that never appears.

**Verified, not assumed.** All five symbols are `T` in the objects, and a
throwaway TU calling every one of them links clean under `kos-cc -DNDEBUG`.
⚠️ **`sh-elf-nm` on `AnimalCrossing.elf` finds ZERO of them** — nothing in `dc/`
calls them yet, so `--gc-sections` drops every one. **G-A buys capability, not
speed.** `.text` = 2,882,948 B on the town build line; do NOT read the 300 B
against this log's earlier 2,883,248 as a saving, the two were never built
back-to-back.

### 🔴 THE RE-MEASUREMENT. "34.4 ms of a 45.6 ms frame, 75 %" IS DEAD.

`G_TRIN_INDEPEND` had not been measured since G3 shipped. Everything in
`kb/research-sh4zam-gap.md` §1 was ranked against a **pre-G3, silent** build.

Run `run-g1`, town, steady state, 430 `[EMU64H]` windows, averaged over the
last 60. `[EMU64H]` **doubled** (rule 9); `[PHASE]` **not**.

| | per logic tick (as printed) | **per presented frame** |
|---|---:|---:|
| `tot` | 16.41 ms | **32.82 ms** |
| `TRIN_INDEPEND` | 11.20 / 127 calls | **22.40 ms / 254 calls** |
| `gap` | 2.64 ms | 5.28 ms |
| `draw` / `skip` | — | 30.39 / 2.57 ms |
| `cull` / `xform` | — | 0.70 / 8.38 ms |
| `us/v` | — | 3.05 |

**Self-check `tot×2` vs `draw+skip`: 32.82 vs 32.97, −0.4 %.** The instrument
agrees with itself. `[EMU64H] armed, period=1`, no `DISABLED`, `[EMU64C] cum
reinst=0` — so G1 did not lose slots 59/60 to G3.

**The new statement: `TRIN_INDEPEND` is 22.40 ms of a 30.39 ms draw — 73.7 %.**
The *share* barely moved. The *absolute* fell by a third, and the frame is far
faster than the 49.9 ms this log records two entries up, because that figure
predates session 9's fourth-iteration levers (`DC_ARAM_WINDOW` 1 MB,
`DC_AUDIO_VOICES=12`, `DC_AUDIO_DISC_FRAMES=8`, `DC_AUDIO_MAX_FRAMES=6`).
`fps_p50` **25.9** against the logged 23.2, `deepest_scene 18`, scene edges
`0 → 3 → 4 → 18 → 9`, `aram LOST 0`, `dropped/unsup 0/0`, `tex rejects 0`.

**Inside TRIN: `cull 0.70 + xform 8.38 = 9.08` attributed, and 13.31 ms is
NOT — 43.8 % of the entire draw.** That is `dl_G_TRIN`'s index expansion plus
our own `GX*` setters, still never separated, and still the largest single
block in the project. It is what G-B aims at.

**This re-ranks the sh4zam list**, against a 30.39 ms draw:

| gap | addressable | note |
|---|---:|---|
| **G-B** | **13.31 ms** | the unattributed block |
| §0a / G-D | inside `xform` 8.38 ms | and only a fraction of it is the six FTRV-able FIPRs |
| **G-F** | **0.70 ms** (2.3 % of draw) | ⚠️ the doc ranked it on a **pre-G3 2.0 ms**. Cheap, no longer valuable |

⚠️ **G3 ADDED cull calls, it did not replace them.** `cu_trin_indep` skips
emu64's handler only on the `return` path; punt *and visible* both fall through
to `GXEnd` → the late cull. `[EMU64C]` per frame: `trin 254, cull 138, vis 59,
punt 57`, so the scalar cull runs 254× from G3 **plus** ~116× on the late path,
and **only the second set is inside the `cull=` bracket**. `dc_emu64_cull.cpp`
contains zero `dc_time_us` reads. The `vcull` collapse 9,915 → 1,002 was a drop
in the late cull's *yield*, not its call count — and "visible" is its most
expensive answer, since it has no early-out.

### Two caveats on the run, both real

1. **No `-DDC_AUTOWALK`, so the camera never moves.** All 60 windows are
   byte-identical (`v=2745 vsrc=2745 vcull=186`). Excellent signal-to-noise,
   **one static town view**, not a walk. Anything that varies with what is
   on screen needs a walking run before it is believed.
2. **`vmemo` hit rate here is 54.40 %** (2,642,897 / 4,858,650 over 59 windows),
   not the 48.2-48.9 % in `kb/perf-dc.md:481`. Different scene composition.
   ⚠️ `[PHASE] vmemo=` is **cumulative**, unlike every other field on that line —
   it must be differenced between windows, which is why a single line reads
   wrong.

### Corrections this session forced

- **`kb/RESUME.md` carries two disagreeing build lines.** §0i (`:217-223`) is
  authoritative; the §42 line (`:51-56`) and the §2 prefix (`:661-664`) are both
  stale at `DC_ARAM_WINDOW=524288` with no audio knobs. Two builds were thrown
  away here before this was noticed.
- **`DC_ARAM_WINDOW=1048576` is redundant** — exactly the header default
  (`dc_platform.h:168-169`). Dropping it is byte-identical.
- **`dc/Makefile:1467`'s `DC_EMU64_HIST` doc-string is wrong about its own
  denominator.** It calls the value a period "in presented frames"; the gate is
  `tick % N` over two per-**logic-tick** counters (`dc_emu64_hist.c:259`,
  `dc_vi.c:470,798`). At `N=1` it is moot, which is what every real run has used.
- **`chan_eval`'s four FIPRs are NOT FTRV-shaped**, contrary to §0a. `:864` and
  `:3119` are self-dots; `:887`/`:890`'s constant operand `nrm` is **per-vertex**;
  `:899`'s is per-**light**. Converting them needs an XMTRX reload per vertex.
  Only the six at `:3108-3110` / `:3116-3118` have a batch-constant operand.
- **emu64's vertex cache is 128 entries, not 32** (`emu64.hpp:33`, `VTX_COUNT`;
  indices are 5-bit *or* 7-bit). `dc_pvr.c:2226-2229` and `kb/perf-dc.md:471`
  both say 32. `dc_emu64_cull.cpp:249` already sizes `mark[4]` = 128 bits, i.e.
  the cull agrees with the header and the comments do not.
- **Matched screenshot frames are impossible today.** No seed override exists
  anywhere in the tree — `sqrand(osGetCount())` (`sys_math.c:6-8`), wall-clock
  (`dc_os.c:189`). `shot_diff.py:42-45` says so itself. Step 4's "own screenshot
  pair" needs a same-build noise floor first, or a counting oracle instead.

## ⭐ 2026-08-08 (session 9, fourth iteration) — THE PUMP COULD NOT KEEP UP
## BELOW 14 FPS, AND THE CONSOLE IS NOT THE EMULATOR

**Human, on hardware:** *"sound right on kk scene, title screen because of the
low fps it sounds choppy"* … *"the fps isn't as good on the hardware and I
suspect it's audio related"* … *"on hardware the game runs super stable, fps
and audio is worse for sure … the emulator runs buttery smooth."*

⭐ **`DC_AUDIO_MAX_FRAMES=2` WAS AN FPS TRAP, AND THE ARITHMETIC IS EXACT.** One
DAC frame is 17.49 ms of audio; the pump runs once per LOGIC tick; there are 2
ticks per presented frame. So production is capped at
`MAX_FRAMES × 17.49 × 2` per frame — **70 ms at the old value, i.e. the audio
cannot keep up below ~14 FPS however cheap synthesis becomes.** That is
precisely "the title screen sounds choppy because the fps is low", and it is
not a synthesis-cost problem at all. The comment on the constant claimed it
"binds only on the first pump after an arm" — true of a 30 FPS scene, false of
every scene that struggles, which are the ones that needed it. Raised to 6
(210 ms/frame, level to ~4.8 FPS); the loop's real bound is the ring filling,
which this cap sits in front of, so raising it cannot overshoot.

**`DC_ARAM_WINDOW` to 1 MB** (the header default all along) — matched 420 s
runs: disc reads **4,183 → 358 → 106**, bytes off disc **137.9 → 12.6 →
4.3 MB**, evictions **4,173 → 336 → 68**. `MEMLEDGER margin=3,705,420`, no OOM.

**L1 applied, `DC_AUDIO_VOICES=12`.** Per-voice cost is exactly linear and the
town peaked at 14-15 concurrent voices, so the cap binds on the worst frames and
nowhere else: the `v>=56` bucket disappears and the worst DAC frame goes
**15,891 → 11,191 us**. Degrades to priority-ordered note stealing
(`__Nas_GetLowerPrio`), not to silence.

### 🔴 AND THE FINDING THAT OUTRANKS ALL OF IT: THE CONSOLE IS NOT THE EMULATOR

Flycast models **no instruction cache**. The SH-4's is **8 KB, direct-mapped**,
against **2,883,248 B of `.text`**. Every FPS figure this project has ever
produced — 11.6, 20.0, 23.2 — is from a machine with a perfect icache, and a
human has now reported the gap directly.

⚠️ **No Flycast experiment can size it.** That is the disc-timing trap one layer
up, and this file now carries three instances of the same error shape in one
week: a census pointed at silent audio, a gate whose oracle could not answer,
and an emulator with no drive. **Ask what the instrument models.**

**The experiment: SH7750 PMCR counters via KOS `perfctr`**, bracketing the
presented frame and `pc_audio_process_frame()`, over SCIF on a burn (~50 lines).
It answers in one run how much of a hardware frame is cache stall, whether audio
really is the console's FPS cost, and whether the audio cost has a memory
component. **A free host-side pre-check exists**: `sh-elf-nm` the ELF and look
for hot symbols colliding mod 8192 along one call chain — no collisions kills
the direct-mapped-aliasing hypothesis without spending a disc.

## ⭐ 2026-08-08 (session 9, third iteration) — THE STUTTER TOOK THREE BURNS,
## AND THE BIGGEST FIX WAS A CONSTANT NOBODY HAD RE-COSTED

Each burn falsified the previous fix's SCOPE and never its mechanism.

**Burn `-b` (pager chunked): "same problem but less laser thrash … actually I
think it's still there."** The chunking worked and was in the wrong function.
`dc_dvd_pager_read()` is the ARAM pager; the game's own asset loading goes
`DVDRead` → `dc_dvd_read_impl()`, and the `DC_ASSET_STUB` keep-list loader had
three more `fs_seek`+`fs_read` pairs. Most of the bytes a scene load reads were
never chunked. Fixed by making **one** function the only place a disc read may
block — `dc_dvd_read_yielding()` — with all five sites calling it, plus two
rules the burn taught: **yield before the seek** (a 100-200 ms head movement
cannot be subdivided, so the only defence is entering it with a full ring) and
**a short first chunk** (2 KB, so the yield lands as soon as the head arrives).

**Burn `-c` (every read routed): "certain parts of the music are REPEATING."**
⭐ That word is the diagnosis. An empty ring produces SILENCE —
`dc_audio_stream_cb()` zero-fills. Repeating means the AICA channel was never
handed new bytes, i.e. `snd_stream_poll()` did not run, and snd_stream keeps one
channel keyed on with a looping buffer. And one path guaranteed it: jaudio's own
sample fetch (`pc_audio_process_frame → Nas_WaveDmaCallBack → Nas_StartDma →
ARStartDMA → the pager → a disc read → dc_audio_disc_yield`) hit the
`s_audio_busy` guard and returned. **The reads most likely to happen WHILE MUSIC
PLAYS were the only ones in the program getting no service at all.** The guard
is right about synthesis and wrong about the poll: `snd_stream_poll()` touches
no jaudio state and is safe at any depth. The re-entrant case polls now
(`pollonly=` on the yield line, non-zero in the smoke).

### ⭐ AND THE LARGEST WIN WAS NOT CODE — `DC_ARAM_WINDOW` 131072 → 524288

Matched 420 s Flycast runs, only the window differing:

| | 128 KB | **512 KB** |
|---|---:|---:|
| disc reads | 4,183 | **358** |
| bytes off disc | 137,943,392 | **12,597,984** |
| evictions | 4,173 | **336** |
| cache hits | 15,605 | 15,930 |

**11.7× fewer reads, i.e. 11.7× fewer SEEKS on hardware**, for 384 KB of a
~2 MB budget (`MEMLEDGER margin=4,229,708`, no OOM, `ASSET MISSING 0`, scenes
0 → 3 → 4 → 18 → 9). ⚠️ With the LRU pager on, this knob is a **disc-seek**
lever and not merely a RAM lever, and the "floor 851,968" note applies only
with `DC_ARAM_LRU=0`.

**Why it sat at 131072: it was measured when RAM was the binding constraint.**
That ended on 2026-08-06. **Generalise: when a constraint is lifted, re-cost
everything that was sized under it** — the value was not wrong when it was set,
it was wrong the moment the reason for it went away. Historical build lines in
this file and in `kb/perf-dc.md` / `kb/heap-two-pools.md` still read 131072 and
are LEFT that way: they record runs that really did use it.

## ⏸ 2026-08-08 (session 9, later) — A BURN SAYS THE REMAINING STUTTER IS THE
## LASER, AND FLYCAST HAD REFUTED THAT ON A MACHINE WITH NO DRIVE

**Human, on `AC-DC-20260808.cdi` running on the retail console:** *"the loading
is much improved, though it still stutters but it's better. I can hear it's
stuttering on disk load. The stutter almost perfectly lines up with laser load
sounds."*

**The arithmetic agrees, and it is not close.** `dc_dvd_pager_read()` blocks in
a single `fs_read` on the game thread; `dc_audio_pump()` runs once per logic
tick. So for the entire read, nothing refills either buffer:

```
our ring   RING_BUF_SAMPLES - DC_AUDIO_HEADROOM = 6,144 s16 = 3,072 pairs =  96 ms
SPU buffer DC_AUDIO_STREAM_BYTES 8,192 B/channel                          = 128 ms
                                                                   total  ~224 ms
```

against a CD-R seek of 100-200 ms **followed by** ~500 KB/s of transfer — a
256 KB archive read is ~500 ms. The AICA runs dry and repeats its last
fragment. It lines up with the laser because it **is** the laser.

⚠️ **AND THE PROJECT HAD ALREADY "REFUTED" THIS.** On 2026-08-06 the ARAM cache
went 4 → 16 blocks (hit rate 83 → 97.9 %, disc reads 3.54 → 0.77/s) and the
stutter did not move, so disc I/O was struck off. **That A/B ran in Flycast,
which the harness starts with `FastGDRomLoad=yes` and which models neither seek
time nor transfer rate.** It measured a machine where the read is free. Third
instance in three days of the same error shape — a census pointed at silent
audio, a gate whose oracle could not answer, an emulator with no drive — and
`kb/traps.md` now carries all three together.

**THE FIX IS DELIBERATELY NOT A BIGGER BUFFER.** 500 ms of cushion is 500 ms of
latency on every footstep and UI blip, permanently, to hide an event that
happens a few times a minute. Instead `dc_dvd_pager_read()` chunks its read at
16 KB (~32 ms of drive time) and calls `dc_audio_disc_yield()` between chunks,
which synthesises up to 4 DAC frames (~70 ms of audio) and polls the stream —
so the ring comes **out** of a long read fuller than it went in, and
steady-state latency is untouched. Chunking a sequential read on an already-open
fd adds no seeks. Kill switch `DC_DVD_READ_CHUNK=0`.

⚠️ **Reentrancy is real:** synthesis fetches samples, so
`pc_audio_process_frame → Nas_WaveDmaCallBack → Nas_StartDma → ARStartDMA →
dc_aram.c → dc_dvd_pager_read` lands straight back in the yield.
`s_audio_busy` guards both directions.

**What the Flycast smoke could and could not say.** It cannot show the win — by
construction the read it models is free. It showed the absence of harm: the
yield fires and produces audio (`[DC/AUDIO] yield calls=41 frames=15` in one
window), bytes per **logical** pager read 34,003 against a 32,916 baseline (the
counter counts logical reads, so chunking did not inflate it), `ASSET MISSING
0`, `aram LOST 0`, no assert, scenes 0 → 3 → 4 before the human ended the run.

⏸ **PARKED HERE FOR A HARDWARE VERDICT.** `AC-DC-20260808b.cdi` is built,
padded and unburned; `AC-DC-20260808.cdi` is kept beside it as the
without-the-fix control. `kb/RESUME.md` §0f carries what to do for each of the
three possible answers.

## ⭐⭐⭐ 2026-08-08 (session 9) — THE MUSIC PLAYS, THE STUTTER IS VOICE COUNT,
## AND G3 TOOK 19.9 ms OUT OF THE TOWN FRAME

Five runs, all 900 s (one ended early — the human closed the window), town keep
list, `DC_AUTOWALK`, probe-free, `-DDC_PERF_PHASE`.

### Run 1 — the free confirmation, and it reframed the audio problem

`DC_AUDIO_SCENES=all`, unmodified tree plus the `DC_ARAM_AUDIO_DROP`
derivation. Session 7 predicted BGM would appear with `all` and it did:
**`SendStart::Mesg Full Queue` = 0**, `[NEOS_OUT] peak=6629`,
`[DC/ARAM] audio=8300384 LOST=0 zero=0`.

⭐ **And with the music finally playing, `DC_AUDIO_VOICELOG` answered the
question three earlier runs could not.** Whole-run distribution, bucketed by
voice-updates summed over the frame's four updates (bucket N = N/4 concurrent):

```
[DC/VOICE] v>= 0 n=8741  mean=2332us max= 8284us
[DC/VOICE] v>= 8 n=16085 mean=3907us max= 8194us
[DC/VOICE] v>=16 n=7985  mean=5701us max=10380us
[DC/VOICE] v>=24 n=6061  mean=7583us max=12327us
[DC/VOICE] v>=32 n=1794  mean=9355us max=15131us
[DC/VOICE] v>=40 n=480  mean=11415us max=17022us
[DC/VOICE] v>=48 n=460  mean=13533us max=18388us
[DC/VOICE] v>=56 n=303  mean=17285us max=22214us
```

**Monotonic. `cost ≈ 2,332 us fixed + ~265 us per voice-update`.** Every
`[STUTTER]` row reads `filt@=0 comb@=0`, so the conditional FIR and comb stages
are refuted in the same run. **The "bimodal ~2.5 ms / ~10 ms" the project has
chased since 2026-08-06 is nothing more exotic than SFX-only versus
music-playing** — a stutter row shows `snd=43.4ms sndf=4 smax=10.8ms vmax=40`,
and bucket `v>=40` has mean 11,415 us. It reconciles exactly.

This is measurement rule 1's cousin, paid for twice now: **session 7 was right
to retract the census that "refuted" voice count. It had measured silence.**
Cost: `fps_p50` **17.4** against 29.8 silent, town `draw` **65.4 ms**, **192**
`[STUTTER]` events. ⚠️ **Flycast's documented ~10× under-reproduction of the
hardware stutter is now suspect** — 192/900 s here against 192/420 s on console
is the same order, and the emulator figure that founded the claim was taken
while the music was silent.

### Run 2 — the two levers, and a human ear

`-DDC_AUDIO_MIXRATE=24000 -DDC_AUDIO_SUBDELAY=0`, plus the port-queue drain and
S8e. Against run 1, matched build line:

| | run 1 | run 2 |
|---|---:|---:|
| `[STUTTER]` events | 192 | **70** |
| `fps_p50` | 17.4 | **19.5** |
| `synth_us` | 2,732 | **1,202** |
| audio's phase (`vi`) | 13.0 ms | **6.8 ms** |
| worst DAC frame (`v>=56`) | 17,285 us | **11,440 us** |

`Full Queue 0`, `[DC/AUDIO] drain msgs=600` per 600 pumps — i.e. our drain now
consumes every port window and `CreateAudioTask` finds nothing, which is the
intended shape. Every voice bucket fell ~34 %; the fixed term fell 27 %.

**Human, on the running build: "audiolev sounds pretty ok, its a little choppy
when the fps dips but it is not stuttering as bad."** That sentence is what
moved the remaining problem from audio to frame rate.

### Run 3 — G3's correctness gate, and it is exact

`DC_EMU64_CULL=1 -DDC_EMU64_CULL_VERIFY`, audio off (a correctness run, and
VERIFY is slower than an uninstrumented build by construction — it runs our
entry test AND the original handler on every batch).

```
[EMU64C] armed, mode=1 VERIFY (never culls; gate on falsecull=0)
[EMU64C] trin=16200 cull=9848 vis=3052 punt=3300 refs=273600 reinst=0 falsecull=0 gfxp_bad=0
```

**473 report windows, every one `falsecull=0 gfxp_bad=0 reinst=0`, through to
scene 9.** Not one batch we would have dropped was drawn by the reference path,
and not once did our index walk disagree with `dl_G_TRIN` about where `gfx_p`
lands. Town rate: **61 % of TRIN batches culled, ~9,120 vertex references
skipped per presented frame** — against `[PHASE] vcull ≈ 9,963`, which is the
instrument validating itself: two different pieces of code in two different
files counting the same geometry.

### Run 4 — G3 armed. −19.9 ms, the top of the predicted range

`DC_EMU64_CULL=1` plus the audio levers. Against run 2:

```
[PHASE] draw=49.9 skip=3.8 vi=8.2 | cull=1.3 xform=11.1 | v=3501 vcull=1002 us/v=3.18
[EMU64C] trin=16020 cull=9344 vis=3556 punt=3120 refs=260400 | cum reinst=0
```

| | run 2 | run 4 |
|---|---:|---:|
| town `draw` | 69.8 ms | **49.9 ms** |
| `fps_p50` | 19.5 | **23.2** |
| `vcull` | 9,915 | **1,002** |
| `cull` phase | 3.9 ms | 1.3 ms |
| `[STUTTER]` | 70 | **65** |

`run_report --vs`: **VERDICT no regression detected.** ⭐ **`vcull` collapsing
9,915 → 1,002 is the mechanism showing its work** — the late cull has almost
nothing left to reject, because G3 rejected it before `set_position` and the
`GX*` setters ever saw it. The 5.4-19.2 ms G4 predicted came in at **19.9**.

⚠️ **The screenshot pair was NOT taken.** The VERIFY gate is a stronger and
exact instrument and it passed, but measurement rule 2 is not formally
satisfied and the next session should close it.

### The adversarial review, and the two findings that mattered

The walk, the `gfx_p` accounting, the punt predicates and the box all came back
clean against `dl_G_TRIN` — including `n_faces == 1` and a partially-consumed
last word. **The damage was to the instruments meant to police them:**

1. **G3's tripwire ran AFTER `dc_emu64_hist_frame_open()`.** G1 and G2 rewrite
   **all 64** dispatch slots per sampled frame; G3 re-installs its two. In that
   order G3 evicts G1's thunks for **exactly the two opcodes G3 exists to fix**,
   and `hist_enter()` charges their time to whichever opcode preceded them — so
   a combined run would have reported `calls=0` for TRIN and quietly inflated
   its neighbour. G3's frame-open now runs first at both `dc_vi.c` sites.
2. **`falsecull=0` was not a gate.** `dc_gx_flush_vertices` returns at
   `dc_gx.c:684` on a frameskipped tick **before** the cull test, so
   `pc_gx_culled_draws` cannot move there and every correct cull would score as
   a false positive. Counted separately as `nocmp=` now, and
   `-DDC_NO_BATCH_CULL` + VERIFY is a hard `#error`.

Generalised, and it belongs with rule 9: **a gate that cannot distinguish "the
reference disagreed" from "the reference could not answer" is not a gate.**

### The latent overrun that only a music-playing build could reach

`max_audio_cmds = num_channels*20*updates_per_frame + _09*30 + 400` = **2,350**
at 24 voices (`memory.c:1029`). On N64 that sizes `AG.abi_cmd_bufs[]`
(`:1061`). **The DC path does not use those buffers** — `Neos_Update` calls
`CreateAudioTask(pc_task_buf[cur], …)` (`neosthread.c:35`) into a fixed
`Acmd pc_task_buf[2][1600]` (`:26`) — and `max_audio_cmds` appears nowhere else
in the tree, so `Nas_smzAudioFrame` writes with no bound at all: **750 Acmds =
6,000 B past the end** at a full voice load. Never observed because the command
count scales with active voices and the music had never started. `S8e` in
`make_src_shrink.py` grows it to 2,432 (+13,312 B of `.bss`) **before** the
first music-playing run rather than after a corruption hunt.

### The defaults moved, on purpose

`DC_EMU64_CULL ?= 1`; and inside the `DC_AUDIO=1` block
`DC_ARAM_AUDIO_DROP ?= 0`, `DC_AUDIO_MIXRATE ?= 24000`,
`DC_AUDIO_SUBDELAY ?= 0`. All `?=`, all with documented reverts. Verified by
building with none of them named and reading `dc/build/flags.stamp` back.
**A result that lives only in a command line is one unset environment variable
away from being lost** — the human asked for exactly this ("make sure we dont
regress past this point").

### Scoped, not built: the AICA question

Asked directly: *"can we not use the dreamcast audio chip to offload?"* The
answer is stage B (`kb/audio-stage-b-aica.md` §5), and the new measurement is
the best argument it has ever had — the ~265 us/voice-update term is precisely
what 64 hardware ADPCM channels do, while the sequencer stays inside the 2.3 ms
fixed term. What still stands in the way is unchanged: jaudio ships **N64
VADPCM** and AICA wants **Yamaha 4-bit ADPCM**, so every sample needs
transcoding with loop points remapped and 24 over-length samples split, and
8.3 MB of `audiorom.img` has to live in ~1.8 MB of usable sound RAM behind a
per-sequence residency manager. A **BGM-only** variant was scoped as the cheap
version — music is the many-voice case, SFX and voice are 0-4 voices — and
would need no per-voice register driving, no envelope mapping and no LRU.
**Decision taken: finish G3 first.** Nothing was built.


## 2026-08-08 (session 8) — THE SPLASH NOW CREDITS THE PEOPLE, AND THE README
## HAS A HALL OF HEROES

Cosmetic, at the maintainer's direction, and recorded only because the boot
screen is the version of the credits most people will ever see.

**What changed.** `DC_SPLASH_TEXT` is untouched — "TechProGabe Presents..."
still owns the middle of the screen at 2x. Added under the progress bar, at
1x in a dim slate (`0xFF8C96B4`) so it reads as a footer:

```
                 SPECIAL THANKS TO
         ACreTeam  -  Cuyler36  -  Dia2809
            flyngmt  -  Falco Girgis
```

`README.md`'s `## Credits` became `## Hall of Heroes` carrying the same five
names in the same order. The maintainer's instruction was **people, not
projects** — the OpenCrossing-Anbernic and KallistiOS-community bullets came
out, and Cuyler36 and Falco Girgis went in.

**Why it is free.** bfont lives in the Dreamcast BIOS, so the three lines cost
94 B of `.rodata` and no RAM. They are drawn **once**, in a new STEP 2b right
after the gradient fill, and they sit entirely below the repainted text band
(`SPL_Y..SPL_Y+SPL_H`) and below the bar — so neither the hold loop nor
`dc_splash_progress()` ever writes over them. That is what buys a second block
of text with no second glyph mask and no flicker on a surface that has no back
buffer. Kill switch `-DDC_SPLASH_NO_THANKS`; knob documented in
`BUILDING-DC.md`.

**Verified on a picture, not on counters** (`kb/traps.md`). Screenshot build,
Flycast, `--fb-writeback`: `frame-0000` of the run shows all three lines
centred, legible over the gradient, nothing clipped, title unchanged.
`.text` 2,871,792 B — the same size class as before. Console still prints
`[DC] splash: "TechProGabe Presents..." mask 1206 px of 6624, 552x48 at
44,200, 2000 ms`, and the run reaches the town at **29.8 FPS / 99 %**, so
nothing on the boot path moved.

⚠️ The screen and the README are **one list in two places**. Change one and
change the other; the comment above `s_spl_thanks[]` says so.

## ⭐⭐⭐ 2026-08-06 (session 7, later) — THE MUSIC NEVER PLAYS, AND IT IS THE
## AUDIO COMMAND QUEUE. THE VOICE CENSUS MEASURED A BROKEN STATE

**Human, on a running build: "audio sounds right … the music isn't working
though, only the talking sound."** That single sentence reframes everything
below it and retracts two lever deaths from earlier in this session.

**THE MECHANISM, and the evidence is in logs that already existed.**

| fact | evidence |
|---|---|
| the audio command queue overruns | `SendStart::Mesg Full Queue` — **6** events in the `-Os` run, **2** in the `-O0` run |
| the gate is disarmed for whole scenes | `DC_AUDIO_SCENES=3,9`, and the run visits scenes **3, 4, 9, 18**. `scene 4: DISARMED` |
| sample data is NOT the problem | `[DC/ARAM] audio=8300384 LOST=0 zero=0(0 B) ext=3/32 drop=0` — no sequence, bank or wave fetch was ever zero-filled |
| the sequencer chain runs | speech and SFX are audible, and they traverse the identical `dc_audio_pump → … → Nas_MySeqMain` path |

`Na_GameFrame` pushes ~55 commands per game frame into a **256-entry** ring
(`sub_sys.c:215-225`) and posts one message per frame into a **64-slot** queue.
The **only** consumer is `CreateAudioTask` (`sub_sys.c:733-736`), which on DC
runs **only while the scene gate is armed** (`dc_audio.c:1104`). Across a
disarmed scene the queue fills, `Z_osSendMesg` returns −1 (`os.c:51-56`),
`Nap_SendStart` stops advancing `AG.thread_cmd_read_pos` (`sub_sys.c:274`), and
`Nap_PortSet` then hits `if (write_pos == read_pos) write_pos--` and
**overwrites the same slot forever**.

⭐ **Why that silences MUSIC and nothing else.** BGM's `START_SEQ` is issued
**exactly once** per scene / per in-game hour (`game64.c_inc:1511`; once
`mBGMPsComp_main_req_start` sets `mBGMPs_FLAG_EXECUTE` at `m_bgm.c:1543-1545`
it never re-fires). SFX re-issues `START_SEQ(SE_GROUP,242,0)` from **eight**
sites, and VOICE re-issues from `Na_SpecChange` on **every dialogue message**.
**So a mechanism that silently drops one command is invisible on SFX and
permanent on BGM.** `__Nas_StartSeq` never runs, `grp->flags.enabled` stays
FALSE (`system.c:822`, gate at `track.c:2129`), and the sequencer ticks over a
group that was never armed.

**This port already measured the same overrun and did not connect it to audio:**
`dc_os.c:606-609` records *"3,374 of 3,956 console lines — 85.3 % — are jaudio's
`SendStart::Mesg Full Queue` … because the audio command queue is never
drained."*

**THE FIX SHAPE:** drain the port queue every tick regardless of arm state — the
gate should skip **synthesis**, not command processing, which is what
`dc_audio.c:1006-1044` already says it intends. **The free confirmation is one
rebuild: `DC_AUDIO_SCENES=all`.** If BGM appears with `all` and not with `3,9`,
it is proved.

### ⚠️ TWO LEVER DEATHS FROM EARLIER TODAY ARE RETRACTED

The `[DC/VOICE]` census was run **while BGM was absent**, and nobody noticed
until a human listened. Its shape is still real, but two conclusions drawn from
it are not:

- ❌ **"The voice cap (L1) is dead because the town runs at 0-4 concurrent
  voices and never approaches the 24 ceiling."** RETRACTED. 0-4 voices was
  **the music not playing**, not a quiet town. A sequenced BGM track uses many
  channels. **L1 is untested, not dead.**
- ❌ **"Cutting FIR and comb (L4) is dead, `filt@=0 comb@=0`."** RETRACTED for
  the same reason — those stages are set by sequence commands
  (`sub_sys.c:571-595`), and no sequence was running.
- ⚠️ **L2's mix-rate numbers are also SFX-only** and will not hold once music
  plays. They are recorded below as what they are.

**The lesson, and it belongs with measurement rule 1:** `ASSET MISSING` empty is
not the only precondition for believing a measurement. **An instrument pointed
at a subsystem that is not running measures the subsystem not running.** The
census had no way to say "no sequence is playing", and every counter it fed was
therefore describing silence. A human ear caught in one sentence what four
hypotheses and three runs did not.

### L2 — internal mix rate 48000 → 24000, measured (SFX only, see above)

`DC_AUDIO_MIXRATE=24000` → `samples/update 200 → 96`. Sound still produced
(`[NEOS_OUT] peak=6406`), `ASSET MISSING 0`, scene 18 → 9 reached.

| voice-updates | mean before | mean after | |
|---|---:|---:|---|
| 0-7 | 1,863 µs | **1,591** | −15 % |
| 8-15 | 3,702 | **2,786** | −25 % |
| 16-23 | 5,345 | **3,853** | −28 % |
| 24+ | 6,547 | **5,039** | −23 % |
| max observed | 9,522 | **6,891** | −28 % |

Real, and cheap, and **not a fix** — it is a tax cut, and it costs the high
band. Hold it until the BGM bug is closed, or it pays for someone else's bug.

### The `-Os` regression hypothesis — NOT settled, and probably not the story

The timeline is real: `210660d` "sound comes out" (2026-08-04) was at **`-O0`**;
`9688c42` moved all of `src/` including jaudio to **`-Os`** (2026-08-06); every
stutter report is after it. Session 6 tested `-Os → -O3` (−1.4 %), never
`-O0 → -Os`. A build with all 49 compiled jaudio TUs forced back to `-O0` via
`DC_OPT_O0_EXTRA` ran clean and a human called it *"sounds right"* — **but the
same build still has no music**, and the queue-full count merely fell 6 → 2,
which is what a slower build does to a queue that fills on a timing race.
**Treat the `-O0` result as unexplained, not as a fix.** The BGM bug above is
the live thread.

⚠️ **`DC_OPT_O0_EXTRA` is SPACE-separated, not colon-separated.** A colon-joined
list is taken as ONE filename and the guard at `dc/Makefile:1214` reports
"names 1 file(s) this build does not compile" with the whole list as the name.
The guard is right and saved the build; the error text just reads oddly.

⚠️ **Flycast UNDER-REPRODUCES the stutter by an order of magnitude** — 15-16
events per 900 s here against 192 per 420 s on hardware. No audio verdict from
this emulator is final without a burn.

## ⭐⭐⭐ 2026-08-06 (session 7) — G4 RAN. THE ~23.6 ms IS SPLIT, AND IT IS
## FOUR-FIFTHS emu64's. G3 IS THE WORK, AND ITS "NET LOSS" CASE IS REFUTED

**Queue item 1 is closed.** `dc/src/dc_gx.c` + `dc/src/dc_vi.c` now carry
**G4**, a raw-TMU2 bracket around the `GX*` attribute setters, reached with
`DC_XDEFS=-DDC_PERF_GXSPLIT=1`. Run
`smoke-oc-gxsplit-20260806-201255-35591`, town keep list, `DC_AUTOWALK`,
900 s, **187 town windows** (`[PHASE] draw > 55 ms`) out of 563, medians:

| per PRESENTED frame | ms |
|---|---:|
| `[PHASE] draw` | 69.3 |
| `G_TRIN_INDEPEND` ×2 (rule 9) | **55.5** |
| `cull` + `xform` — already-attributed `dc/` | 3.3 + 10.7 = **14.0** |
| **`gxpos` + `gxbegin` + `gxend` + `gxstate` − `probe` = OURS** | **8.37** |
| **`gxgap` = emu64's vertex loop + the three cheap setters** | **18.34** |
| ledger residual against TRIN×2 | **−12.6** |

```
[GXSPLIT] gxpos=9.19 gxgap=18.34 gxbegin=0.14 gxend=0.84 gxstate=0.31
          | ours=8.37 emu=18.34 | posn=13197 probe=2.11 drops=2
```

- ⭐ **THE ANSWER: the remainder is ~1/5 ours and ~4/5 emu64's.** Of TRIN's
  55.5 ms, 14.0 is `cull`+`xform`, **8.4 ms is `dc_gx.c`**, and the other
  ~30.9 ms (`gxgap` 18.3 + the 12.6 ms residual, which is emu64 interval time
  by construction — see below) is emu64. **So the next piece of work belongs in
  emu64 (G3), not in our own renderer**, which is the decision this build
  existed to make.
- ⭐ **G3's degenerate case is REFUTED, and that mattered.** The design pass
  flagged that if the remainder were entirely per-COMMAND rather than
  per-VERTEX, G3 would be a **net loss**. `gxpos` is unambiguously per-vertex
  (one call per vertex, by construction) and is **9.19 ms/frame over 13,197
  references = 0.536 µs each**, against `gxbegin+gxend+gxstate = 1.29 ms` over
  ~300 commands. The cost is per-vertex. G3 is worth building.
- ⭐ **G3's measured payoff.** The cull rate in these windows is **75.0 %**
  (`vcull` 9,993 of `v+vcull` 13,329). Skipping those references saves
  **5.4 ms at the floor** (our setters alone, 0.536 µs/ref) and **19.2 ms at
  the ceiling** (if `gxgap` is per-vertex too, 1.926 µs/ref). The floor alone
  is 7.8 % of the draw phase; the ceiling is 28 %.
- ✅ **The instrument validated itself.** `posn=13,197` against
  `v + vcull = 13,329` — 1.0 % apart, and they are counted by different code in
  different files. `probe=2.11 ms` is 3.0 % of the frame and is subtracted, not
  argued about. `drops=2` per window from the wrap guard.

⚠️ **`[GXSPLIT]` IS PER PRESENTED FRAME — do NOT double it.** Unlike
`[EMU64H]`, its accumulators are published and cleared in
`dc_gx_frame_timing_snapshot()`, whose only call site (`dc_vi.c:480`) is
**below** the frameskip early return at `:423`. So one publish covers every tick
in the batch. It shares a denominator with `[PHASE]`, and only `[EMU64H]` needs
the ×2. Rule 9 generalised: **state the denominator for every new counter** —
this one was checked against the code, not assumed.

⚠️ **THE −12.6 ms RESIDUAL WAS A HOLE IN MY OWN INSTRUMENT, AND IT IS FIXED.**
The first draft charged the gap only at `GXPosition3f32` entry, so the intervals
`GXEnd → GXBegin` and `GXBegin → first vertex` — emu64's `dirty_check`,
`setup_1tri_2tri_1quad` and every non-TRIN opcode — were charged to **nothing**
and vanished. `gxs_open()` now charges the gap from every bracket
(`dc_gx.c`, `gxs_gap_charge`). The numbers above are from the run BEFORE that
fix, so **`gxgap` 18.34 is a floor and the emu64 share is if anything larger** —
the residual is emu64 interval time by construction, which is why the
four-fifths conclusion is safe even with the ledger open. A re-run should close
it; nothing above depends on that.

⚠️ **This frame is 69.3 ms, not the 45.6 ms of the 2026-08-06 baseline**, for
two reasons that must not be conflated: **both** probes were on (G1's thunks
plus G4's 2.11 ms), **and** the geometry is heavier (`v` 3,336 vs 2,899,
`vcull` 9,993 vs 5,250, cull rate 75 % vs 64 %) because `DC_AUTOWALK` was
standing somewhere else. **Read the ratios, not the absolutes** — that is why
every figure above is quoted as a share of TRIN or per vertex reference.

**Build line:**
```bash
DC_STUB_KEEP="$(grep -v '^#' tools/dcstub/keeplist-town.txt | paste -sd: -)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=131072 DC_ARENA_BYTES=1200000 DC_AUTOSTART=300 DC_SCIF_FAST=1 \
DC_EMU64_HIST=1 \
DC_XDEFS='-DDC_PERF_PHASE -DDC_PERF_GXSPLIT=1 -DDC_AUTOWALK=1' bash dc/build-dc.sh
```

## ⭐⭐⭐ 2026-08-06 (session 6, later) — AUDIO WORKS ON REAL HARDWARE, AND THE
## STUTTER IT LEFT KILLED FOUR HYPOTHESES

**The fix was one flag nobody had wired.** `DC_AUDIO=1` alone drops all
8,300,384 B of `audiorom.img` at `dc_aram.c:317-320`, *before*
`dc_dvd_provenance()` at `:325`, so the audio half of ARAM had **zero** extents
and every sample fetch was `memset` to zero. `-DDC_ARAM_AUDIO_DROP=0` fixes it:
`mapped` 4,982,400 → **13,282,784** (exactly +8,300,384), `ext=3/32`, `zero=0`,
`[NEOS_OUT] peak` 0 → **3851**, and a human confirmed sound on the console.
Costs no pool bytes — `dc_aram.c:344` skips block allocation for mapped writes.

⚠️ **Two `synth_us` figures (1,353 and 3,208 µs) were quoted as "audio is
affordable now" before anyone checked `peak`.** Both were measured on silent
runs, and synthesising zeros is cheap. Withdrawn. The honest figure, with sound
actually playing, is a **mean of 3,777 µs** per 17.49 ms DAC frame — which does
still retire the `-O0` doctrine (19.8 ms, "113 % of the machine, AICA Stage B is
worth 6-10 weeks"), just for a reason that had to be earned twice.

**THE STUTTER: ~2/s on hardware, and it reproduces at 192 `[STUTTER]`/420 s
against 14 silent.** Four hypotheses, each killed by a measurement:

| # | hypothesis | how it died |
|---|---|---|
| 1 | disc-cache misses | ARAM 4→16 blocks: hit rate 83→97.9 %, disc reads 3.54→0.77/s. **Hardware stutter unchanged.** Kept anyway — right on its own evidence |
| 2 | multi-frame synthesis burst | `DC_AUDIO_MAX_FRAMES` is 2, capping a pump at ~9.6 ms. `sndf=4` is 2 TICKS × 2 — the per-logic-tick denominator again, same day as rule 9 |
| 3 | `snd_stream_poll` / G2 / the 10 ms scheduler quantum | `DC_AUDIO_MAX_FRAMES=0` keeps the entire KOS path live (`cb=5692`, `pulled=46,628,864 B`, same fills, same DMA, same semaphore) and costs **0.1 ms**; stutters fall to 13. The quantum fit was coincidence |
| 4 | jaudio's mean cost | `-O3` on rspsim/driver/system/aictrl: 192→174 events, mean 3,832→3,777 µs. **1.4 %**, where `-Os` bought the decomp draw path 41 %. Not compiler-bound |

**What it actually is, from `smax=`:** `4 × smax == snd` on every spike
(`snd=39.6 sndf=4 smax=9.9`; `snd=28.2 sndf=4 smax=7.0`). Not one pathological
call among three cheap ones — **every** call costs ~10 ms during a stutter
against a 3.78 ms mean. jaudio is **bimodal**, and the budget explains the rest:
2.57 DAC frames per ~45 ms game frame is ~14 % of the frame at 2.5 ms and
**~57 % at 10 ms**. At 57 % the frame collapses and the pump hits its 4-frame
ceiling catching up.

The instruments that did it, and they were cheap: `snd=`/`sndf=` (ten minutes)
killed hypotheses 2 and 3; `smax=` (five lines) killed 4 and produced the
answer. Every counter that already existed reported an **average**, and the bug
was a plateau — invisible to all of them.

Runs: `smoke-oc-dc-audio-20260806-174020` (silent, `peak=0`),
`-audio-o0-174509` (the `-O0` control that exonerated the optimizer),
`-audiofix-175714` (`peak=3851`), `-aramwin-183021` (16 blocks),
`-sndattr-185418` (192), `-nosynth-191545` (13), `-audioO3-192616` (174),
`-smax-193504` (191).

**Not fixed. The lever is PEAK per-frame synthesis cost** — voice count,
effects, or rspsim at 22 kHz (`kb/audio-cpu-cost.md` A0-A4) — and those are
product decisions. `DC_AUDIO_SCENES=3` is the shippable configuration today.

## ⭐⭐ 2026-08-06 (session 6) — G1 WAS RE-RUN, AND EVERY `[EMU64H]` NUMBER THIS
## PROJECT HAS EVER QUOTED IS HALF OF THE TRUTH

Run `smoke-oc-dc-g1b-20260806-164033-15671`, town, probe-free,
`DC_EMU64_HIST=1 DC_XDEFS='-DDC_PERF_PHASE'`. It was launched to re-cost the
opcode mix at `-Os`/`-O3` — which it did — but the first thing it returned was
a correction to the instrument.

### The ×2. Read this before quoting any histogram figure, new or old

⚠️ **`[EMU64H]` reports per LOGIC TICK, not per presented frame.** G1 arms at
the end of EVERY tick — `dc_vi.c:405` is the frameskip path, `dc_vi.c:633` the
presented path — and `s_frames` increments on every
`dc_emu64_hist_frame_close()`. At `ticks_per_visual = 2` every printed number
must be **doubled** before it can be set against `[PHASE] draw=`.

Proof, from two runs a day apart, in both directions:

| | `tot` as printed | ×2 | `draw` + `skip` |
|---|---:|---:|---:|
| this run, `-Os` + `-O3` | 24.28 | **48.56** | 45.6 + 2.9 = **48.5** |
| 2026-08-05, `-O0` | 42.86 | **85.7** | 78.3 + 8.2 = **86.5** |

**Two sessions quoted the halved numbers.** `G_TRIN_INDEPEND` was never 28 % of
the `-O0` frame; it was **44.5 ms of an 86.5 ms frame, i.e. 51 %**. This is
measurement rule **9** now (`kb/RESUME.md` §0b) and an entry in `kb/traps.md`.

### The histogram, corrected to per presented frame

```
[EMU64H] f=60 tot=20.27ms gap=2.78ms probe=79.9ns | TRIN_INDEPEND 14.75/126
         VTX 0.77/132 MTX 0.65/94 MOVEMEM 0.25/207 TRI2 0.25/2 DL 0.12/124
         SETCOMBINE 0.11/47 ENDDL 0.08/116 LOADTLUT 0.07/40
         SETTILE_DOLPHIN 0.07/64 SETTIMG 0.06/68 MOVEWORD 0.06/78
[PHASE]  draw=45.6 skip=2.9 vi=0.4 | cull=2.0 xform=8.8 |
         v=2899 vlit=2689 vcull=5250 us/v=3.06        cmds=3562
```

(the `[EMU64H]` line above is the raw per-tick print; the medians below are
over 47 windows and are **×2-corrected**)

| | `-O0`, ×2 corrected | `-Os`/`-O3`, ×2 corrected |
|---|---:|---:|
| `draw` | 78.3 | **45.6** |
| `G_TRIN_INDEPEND` | 44.5 ms / 292 calls | **34.4 ms / 306 calls, 112.5 µs/call, 75 % of the frame** |
| `G_VTX` | 10.8 | **1.84** |
| `G_MTX` | 4.36 | **1.72** |
| `G_TEXRECT` | 4.34 | **2.88** |
| `gap` | 15.8 | **5.96** |
| `[EMU64H] tot` | 85.7 | **48.56** |

### What the corrected histogram decides

- ⭐ **The largest unattributed block in the project is ~23.6 ms, and it is
  inside one opcode.** Of `G_TRIN_INDEPEND`'s 34.4 ms, `cull 2.0 + xform 8.8 =
  10.8 ms` is measured `dc/` code. **The other ~23.6 ms — 52 % of the whole
  frame — is emu64's `dl_G_TRIN` index expansion PLUS our own `GX*` attribute
  setters in `dc_gx.c`, and those two have never been separated from each
  other.** Splitting them is now the top of the queue: one `dc_time_us()`
  bracket, one build, one run.
- ⭐ **G3 (cull at TRIN entry) is the biggest lever in the project, and bigger
  than its old estimate.** `vcull=5250` against `v=2899` means **64 % of vertex
  references are fully expanded and pushed through the GX setters before the
  batch is rejected.** The old 4.5-7.0 ms was costed against a halved frame.
- **G2 is DEAD.** Its target was the dispatch loop, which is exactly `gap` —
  5.96 ms, and already `-O3`. Recommend deleting `dc_emu64_shadow.cpp` after one
  A/B. ⚠️ It costs **nothing** when off (the whole body is inside
  `#if DC_EMU64_SHADOW_LOOP > 0`; 20 KB only when on) but it **does block G1**
  through the `#error` at `dc_platform.h:417`, which is the real reason to
  remove it.
- **`G_VTX` is finished as a topic.** 10.8 → 1.84 ms from a compiler flag. Four
  documents once costed G3 against "~48 ms of `G_VTX`".
- **Every state opcode is ≤ 0.5 ms.** F8 stays answered: stripping state
  commands is worth nothing.

### `gap` IS ATTRIBUTED — it was open since 2026-08-05

`gap` is emu64's own dispatch-loop overhead, and it is not a mystery bucket:
slot `HIST_GAP = 64` (`dc_emu64_hist.c:87`) is accumulated in `hist_enter()`
(`:124-131`) whenever `s_prev == HIST_GAP` — i.e. `emu64_taskstart_r`'s loop
control (`emu64.c:5807-5824` prologue, `:5847-5855` dispatch guard, `:5874`
`gfx_p++`) plus the frame prologue and epilogue. Confirmed by its own response
to the flag: **15.8 → 5.96 ms when that loop went `-O3`.**

⚠️ **`probe=` is NOT subtracted from `tot` or from `gap`** —
`dc_emu64_hist.c:300` only prints it — and two probes per frame land inside
`gap` by construction. The `gap unexplained / OPEN` items in `kb/STATE.md` and
`kb/RESUME.md` are **CLOSED**.

### `pvr_dropped` is CLOSED — there is no speed mechanism behind it

`s_tris_dropped` (`dc_pvr.c:134`) increments on near-plane geometry and nothing
else: all three vertices behind the plane (`:2149`, `w <= 0.001f`), a straddle
under `-DDC_PVR_NO_NEARCLIP` (`:2162`), or Sutherland-Hodgman emitting fewer
than three vertices (`:2181`). It is **purely data-dependent on camera
position**. The `1,300 → 0` that `run_report --vs` reported on the `dc/src`
`-O3` change was **where the camera was, not what `-O3` did**. Goes to
`kb/closed.md`; the "noisy counter" warning in `kb/STATE.md` now cites the
mechanism instead of just the observation.

### "PUT ALL THE ASSETS IN RAM" IS REFUTED BY A BOOT, NOT BY ARITHMETIC

A full `DC_ASSET_STUB=0` image was linked and run —
`smoke-oc-dc-nostub-20260806-165321-16857` — because the RAM picture had moved
far enough that the question deserved an experiment rather than an estimate.

```
text 3,050,152   data 2,224,820   bss 10,493,196   _end 0x8cf19b6c
MEMLEDGER FIT image_span=15768428 additive_heap=1658752 usable=16646144
              margin=-781036 OVER            <- the first OVER ever printed
Out of memory. Requested sbrk_base 8d016000, was 8cf36000, diff 917504
Out of memory. Requested sbrk_base ...                   diff 15638528
```

`diff 917504` is `main.dol`; `diff 15638528` is the whole of `foresta.rel`. Both
fail, so `rom_src=0` and **all 14,495 asset rows come back MISSING** — the
non-stub image contains LESS content than the stubbed one it was meant to
replace.

**The structural reason it can never work, at any margin:** the non-stub path
asks libc for **one contiguous 15,638,528 B buffer**. No lever in `kb/levers.md`
makes a 15.6 MB `malloc` fit in 16 MB next to a 15.8 MB image.

```
image span 15,768,428 + additive 1,658,752 + libc peak 3,056,276 = 20,483,456
usable                                                             16,646,144
                                                        short by    3,837,312
```

⚠️ **Two live misconceptions corrected while proving this:**

- **S6 is NOT a demand loader.** It deletes `s_assets[]`'s `path` field and
  14,495 string literals (`make_src_shrink.py:467`) — 598,648 B of `.rodata`,
  and nothing else. The rewritten loader still does
  `memcpy(dest, rom + rom_off, size)` against the whole resident REL. **The real
  demand loader is `dc_stub_keep_assets()` / `dc_stub_keep_load_one()`
  (`dc_main.c:885-1135`), which lives entirely inside `#ifdef DC_ASSET_STUB`.**
- **`rom_src=0` means `SRC_REL`**, not "row 0".

### THE RAM PICTURE, RE-COSTED — and RAM is no longer the binding constraint

Committed today as `296a1d2`. Runs `smoke-oc-dc-wide-20260806-165816-17270`
(the interiors/winter step) and `smoke-oc-dc-full-20260806-171552-18975` (the
current image):

| | shipping | + interiors/winter | + gyroids (now) |
|---|---:|---:|---:|
| `.text` | 2,753,700 | 2,793,284 | **2,854,108** |
| `.bss` | 3,945,484 | 4,428,076 | **4,791,884** |
| image span | 8,926,124 | 9,446,380 | **9,878,540** |
| `margin` | 6,061,268 | 5,541,012 | **5,109,364** |

Real headroom — `margin` minus the 3,056,276 B libc peak, which is the only
honest form (rule 6) — went from **~146 KB on 2026-08-04 to ~2.05 MB**, *after*
spending 952,416 B on content. `MEMLEDGER OK`, `ASSET MISSING 0`, `aram LOST 0`,
`deepest_scene 18`, `run_report --vs` no regression, town `us/v` 3.07 → 3.09,
and **a human confirmed the gyroids render**.

**Say it plainly, because four documents are still written the other way: the
RAM problem is no longer the binding constraint.** `kb/STATE.md`'s old line —
"the full image still does not fit, that is the only thing between here and a
playable build" — is **void**. What remains is **residency**: 8,813,054 B of
asset destination arrays can never all be resident at once, so the keep list
still decides what exists. That is a content question, not a fit question.

⚠️ **Method error worth more than the bytes.** The gyroid set was first costed
at **155,360 B** by summing its `Vtx` arrays. The link says **432,160 B** of
span — **2.8× low**, because those files also carry textures and display lists.
**Cost a keep-list addition from two links, never from summing the arrays you
went looking for.** (`kb/traps.md`.)

### T1 IS DESIGNED, AND IT IS MUCH CHEAPER THAN THE CONCEPT NOTE SAID

`kb/levers.md` T1 has been the highest-value open idea in the
project since 2026-08-01. Designed against the tree today, it is smaller and
better-placed than its own write-up:

- **The seam is already ours.** `GXLoadTexObj` (`dc_gx.c:2288`) →
  `dc_gx_backend_texture_upload` (`dc_gx.c:2333` → `dc_pvr_texture.c:1060`).
  **No `src/` rewrite, no `make_src_shrink.py` rule, no `--wrap`** — cheaper
  than R1's seam, which needed a rewriter.
- **No N-slot pool is needed, and this is the part the concept note missed.**
  The PVR already holds every texture twiddled in VRAM behind a content-keyed
  LRU (`uploads=306 hits=894442 evictions=0`). The main-RAM array is read on
  every bind **only to compute the cache key** (`tex_content_hash`,
  `dc_pvr_texture.c:1092`, ~109 binds/frame). Replace that with a synthetic key
  built from the asset row and the array is needed **only on a miss** ⇒ one
  24,576 B staging buffer, ~38,800 B of fixed cost in total.
- **Inventory** (method: `make_stub_data.py`'s own `IFDEF_RE`/`DECL_RE`,
  cross-checked because it reproduces `kb/levers.md`'s independent acre figure
  of 815,024 B **exactly**): whole asset population **8,813,054 B / 16,341
  syms**, of which **1,885,176 B / 1,742 syms resident**. Textures **5,053,824 B
  total, 752,640 B resident**. Max texture 4,096 B with one outlier
  (`FONT_nes_tex_font1`, 24,576 B); **99.4 % are ≤ 2,048 B**. Every texture is
  `rom_src=0`, `swap=0` — a pure `pread`, no byte-swap.

| | resident before | cost | net | delivers |
|---|---:|---:|---:|---|
| **T1 phase 1** — 669 case-1 textures (excl. NPC, segment-bound, file-static) | 618,048 | ~38,800 | **−579,248** | same content |
| **T1 phase 2** — extend the map to all 6,354 eligible | — | **+68,000** | +68,000 | **5,685 textures / 2,782,080 B that render as nothing today** |

Phase 1 is **a real saving, not a rule-8 content swap** — the first lever since
R1 that frees bytes rather than converting MISSING into PRESENT.

**The hazard it found, and its mitigation.** 27 of 8,761
`gsDPSetTextureImage_Dolphin` sites use pointer arithmetic. All 27 are in
`src/data/model/hnw_model.c`, all of the form `anime_4_txt + 0x…` — and
`anime_4_txt` is `SEGMENT_ADDR(0x0B,0)`, not a `.bss` symbol, so it resolves
through `gSPSegment` and is **safe**. Mitigation: exclude the 14
`gSPSegment`-argument symbols (1 resident, 512 B) and all of `src/data/npc/**`
(R2's domain).

**Falsification experiment: one build, one run, ZERO behaviour change.**
`DC_TEXPOOL_PROBE=1` with counters `interior` / `mutated` / `oversize`. **Any
one of them non-zero kills the design as specified.**

⚠️ **Seek risk, and the fix already exists in the tree.** T1 issues one seek per
distinct texture — ~306 per run — which is **6-30 s of seeks on hardware,
concentrated as mid-scene hitches**. Resident texture ROM offsets are clustered
(median gap 512 B; 863 of 905 gaps ≤ 32 KB), so a 32 KB read-ahead window
collapses ~306 seeks to ~40. **`dc_keep_sweep()` (`dc_main.c:977-1108`) already
implements exactly that window discipline, and R1 does not use it** — R1 is
still paying 27 unbatched seeks per acre load. ⚠️ **Do NOT reach for a wholesale
sorted prefetch instead:** resident texture offsets span 10.9 MB, which is ~22 s
of linear read.

### A WHOLE TEV CLASS IS UNIMPLEMENTED, IT IS VISIBLE, AND IT IS NOT A STUB

A human reported the name-entry keyboard rendering **BLACK**. Root-caused, and
for once it is not the keep list:

- The widget draws at `m_editor_ovl.c:2221-2258` (`mED_KeyDraw`), 26 display
  lists. Its assets — `kai_sousa.c`, `kai_sousa2.c`, `lat_sp.c` — are on **both**
  keep lists and in the generated loader. Stub ruled out **statically and by
  argument**: alpha here is `TEXEL0`, so a zeroed texture would be INVISIBLE,
  not black.
- **18 of 26 DLs**, including every structural piece (`mojibanT`,
  `controllerT`, `controller2T`, `shitaT`, `controllpadT`, `cursorT`, `3DT` and
  the button caps) use
  `gsDPSetCombineLERP(PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, 0,0,0,TEXEL0)`
  — **TEV config #037, class P3**, `kb/tev-map-table.md` §4 row 37.
- **`dc_pvr.c` implements no part of P3.** `tev_const_color()` rejects it at its
  first test (`:1000` needs `b`/`c` == ZERO; here they are ENVIRONMENT and
  TEXEL0). `tev_fold_color()` rejects it too — `tev_carg_affine()` falls through
  at `:1162` for `GX_CC_TEXC` — so **`-DDC_PVR_TEVFOLD` will not fix it**, which
  is a free falsification test. And `pv.oargb` is a **hardcoded 0** at `:1988`:
  **PVR offset colour has never been wired at all.**
- The DL clears `G_LIGHTING`, so the material source is VTX — the same mechanism
  as `mFont_SetVertex_dol` (`dc_pvr.c:1209-1216`). The result is
  `rgb = vtx.rgb * T0.rgb`, and **both PRIM and ENV are lost**. 21 of the 26 DLs
  are wrong; the 5 that survive use the flat `(0,0,0,PRIMITIVE)` shape that
  `tev_const_color()` does recognise.
- **P3 is 27 of the 101 configs.** The native PVR fix needs no second pass:
  vertex base colour = `PRIM − ENV`, per-vertex `oargb` = `ENV`,
  `PVR_TXRENV_MODULATE` plus the offset-colour bit. `pv.oargb` is already
  stored, so the per-vertex cost is ~zero. Needs its own kill switch and a
  screenshot pair — **widenings in this family have regressed before**.
- **Falsifiable prediction:** the panel is black but the 40 key caps are
  correctly coloured (`m_editor_ovl.c:2092` uses the recognised shape). **If the
  caps are black too, the diagnosis is wrong.**
- Relationship to the open #007 bug: **#007 loses both ALPHA factors, #037 loses
  both COLOUR constants.** Same family, opposite halves.

### The queue this session leaves

Rewritten in `kb/STATE.md` and `kb/RESUME.md` §5, which must agree. The old list
was `-O0`-era and its top two items are now wrong. In order: split the ~23.6 ms
TRIN remainder → G3 → TEV P3/`oargb` → the AABB cull via XMTRX → `chan_eval` →
T1 phase 1 then 2 → N2b → a hardware burn → the audio re-cost.

⚠️ **One correction to the record, item 5's neighbour:** the per-lit-vertex
block at `dc_pvr.c:2868` is **ALREADY OPTIMAL** and should stop being listed as
an sh4zam candidate. `mv`/`nm` are hoisted per batch at `:2779-2781`, and the
seven ops at `:2875-2886` are already `fipr()` through `DC_DOT4`/`DC_DOT3`
(`:187-191`). Holding `nm` in XMTRX **loses**, because `comb` needs XMTRX for
the position FTRV at `:2863`. The live light-loop candidate is `chan_eval`
(`dc_pvr.c:837-902`) — 3 FIPRs per light per vertex, ~35-70k FIPR/frame.


## 2026-08-06 (later) — sh4zam vendored; the FIPR experiment measures FLAT

Falco's direction, followed and measured. Recorded because the NEGATIVE result
is the useful one.

**The experiment.** `PSMTXMultVec`/`SR` through `shz_vec4_dot3` — three FIPRs
sharing one pinned left vector, which the KOS `fipr()` macro cannot express.
The compiled function really did become **3 FIPRs and nothing else** (objdump,
down from 9 `fmul` + 9 `fadd`). The frame did not care:

| | `-O3` baseline | + FIPR MultVec |
|---|---:|---:|
| `us/v` | 3.11 | 3.12 |
| `draw` ms | 45.4 | 45.5 |
| town FPS | 20.6 | 20.4 |

`smoke-oc-dc-shz-20260806-133743`. **DEFAULT 0**, and the reason is precision
rather than speed: FIPR carries ~2^-21 relative error against four
correctly-rounded scalar operations, which is a real risk on VERTEX POSITIONS
bought for nothing. (Lighting keeps the same trade, `dc_pvr.c:188-191`, where
the error cannot move a pixel.)

⚠️ **HOW THE ESTIMATE WENT WRONG — rule 5, again.** It was built from
instruction counts: ~6,951 G_VTX source vertices × 2 calls × ~90 cycles ⇒
"~6 ms/frame". The backend only ever sees ~3,000 vertices, and `-O3` had
already made the scalar body cheap. **Only a matched-frame A/B may price a
function.**

**What the trip was worth.** The residency branch at the top of both functions
is **dead code**: `dc_xm_claim(..., DC_XM_KIND_TRANSPOSE)` has one call site,
inside `PSMTXMultVecArray`, which `--gc-sections` discards — and `dc_mtx.c`'s
own header already said so while the code went on probing for it twice per
vertex at 12 loads + 12 compares a call. Probe removed. Measured 21.3 FPS /
`draw` 43.7 ms against 20.6 / 45.4, but that window carried less geometry
(`cmds` 3611 vs 3728), so it is logged as **no regression, direction
positive** — not as a win.

**Structural, so it is not re-proposed:** XMTRX can never serve that path.
`dl_G_VTX` calls it twice per vertex with two different matrices
(`emu64.c:4709`, `:4711`), alternating every iteration, and SH-4 has one XF
bank.

**Costed and NOT tried:** composing the per-batch `P·MV` inside XMTRX. The
hand fold it would replace is ~96 batches × ~140 instructions ≈ **67 µs of a
45 ms frame**. Doing the arithmetic first is what rule 5 is for.

**Still open, and cheap now that sh4zam is vendored:**
`dc_gx_batch_is_offscreen` (`dc_gx.c:495`) puts 8 AABB corners through two
scalar matrix stages per batch — ~2.0 ms/frame — and never touches the matrix
unit, despite using the same `P·MV` product `dc_pvr.c` builds a line later.
~~And the per-lit-vertex block (`dc_pvr.c:2868`) re-reads `mv`/`nm` from RAM per
vertex for 6-7 FIPRs, on ~2,900 of ~3,100 town vertices.~~
⚠️ **[CORRECTED 2026-08-06, session 6] That second candidate does not exist.**
`mv`/`nm` are **hoisted per batch** at `:2779-2781`, and the seven ops at
`:2875-2886` are already `fipr()` via `DC_DOT4`/`DC_DOT3` (`:187-191`) — the
block is optimal as written, and holding `nm` in XMTRX *loses*, because `comb`
needs XMTRX for the position FTRV at `:2863`. **The live light-loop candidate is
`chan_eval` (`dc_pvr.c:837-902`)**: 3 FIPRs per light per vertex,
~35-70k FIPR/frame.

⚠️ **sh4zam currently contributes ZERO bytes to the image** — its four `.c`
files gc-section away and the one path that used a header is off by default.
It is vendored (`dc/third_party/sh4zam`, MIT, pinned `d4c648f`) so those two
experiments need no setup, not because anything ships from it today.


## ⭐ 2026-08-06 — THE `-O0` BAN WAS ARM EVIDENCE, AND IT COST 2.8 MB AND 8 FPS

**Session 5, and the largest single result the project has had.** The user
reversed the `-O0` directive on advice from the KOS/sh4zam maintainer: GCC's
SH-4 output at `-O0` is not "unoptimized" but pathological, and no DC port
ships that way. Every number below is from this tree, sh-elf GCC 15.2, the
shipping build line (town keep list, `DC_ASSET_STUB=1`, `DC_SRC_SHRINK=1`,
`DC_ARENA_BYTES=1200000`, audio off).

### The measurement, in one table

Matched town windows — same acre, `v` within 2 %, `cmds` within 3 %:

| | `-O0` | `-Os` | `-Os` + `-O3` hot | **+ `dc/src` `-O3`** |
|---|---:|---:|---:|---:|
| `.text` | 5,506,964 | 2,680,676 | 2,729,152 | **2,753,700** |
| `.data` | 2,337,980 | 2,224,832 | 2,224,832 | 2,224,832 |
| `.bss` | 3,945,356 | 3,945,484 | 3,945,484 | 3,945,484 |
| town FPS | 11.6 / 11.4 | 18.5 / 17.9 | 20.0 / 19.7 | **20.6 / 20.2** |
| `draw` ms | 79.1 / 80.8 | 50.3 / 51.9 | 46.8 / 47.5 | **45.4 / 46.3** |
| logic tick (`skip`) ms | 6.6 / 6.5 | 3.3 / 3.4 | 2.8 / 2.7 | 2.8 / 2.8 |
| `xform` ms (`dc/`) | 13.1 / 14.4 | 12.9 / 14.3 | 12.4 / 13.6 | **9.9 / 10.9** |
| `us/v` | 4.05 | 4.05 | 4.05 | **3.11** |
| whole-run FPS p50 | 24.5 | 29.8 | 29.8 | 28.7 |

⚠️ **The fourth column is a SEPARATE change** — `DC_OPT`, which governs
`dc/src` and was never part of the `-O0` directive — landed the same day and
screenshot-gated separately (`smoke-oc-dc-shot-dcO3-20260806-131303`). It is in
this table because every later document quotes the end-to-end figure, and a
table that stopped at column three would make those look wrong. Its own
`run_report --vs` also shows `pvr_dropped 1,300 -> 0` on the autowalk path,
which is unexplained and recorded, not claimed.

**`xform` is the control and it did not move.** That is what proves the win is
decomp code and not measurement drift: the phase this work could not touch —
`dc/src/dc_pvr.c`, `-O2` before and after — stayed inside 4 % while the phase
it did touch fell 41 %.

**`.text` −2,826,288 B is the same size as roughly every `.bss` lever this
project has landed put together.** It is RAM, not just speed (`kb/closed.md`:
every image byte destroys a heap byte).

Runs: `smoke-oc-dc-Os-20260806-121509-8620` (flat `-Os`),
`smoke-oc-dc-perfprof-20260806-122605-10356` (`-Os` + `-O3` hot list),
against `smoke-sh4math-20260805-204911-6201` (`-O0` baseline).
`ASSET MISSING 0`, `crashes=0`, `ASSERT 0`, `ptdrop 0`, `LOST 0` on all three.
`run_report.py --vs`: **no regression detected** on either.

### The screenshot gate (rule 2 — counters cannot see colour)

Two 900 s `DC_AUTOWALK` runs built from the SAME source with only
`DC_OPT_PROFILE` differing: `smoke-oc-dc-shot-perf-20260806-123722-10646` and
`smoke-oc-dc-shot-o0-20260806-123744-10710`, 108 and 84 probes decoded with
`fbimg_to_png.py`. Frame-matched pairs at the train (Porter), the town (Tom
Nook outside a house), the house tour, and the K.K. scene are the same image:
same geometry, same textures, same balloon text, same lighting, and the same
pre-existing black shadow wedges (TEV #007, still unfixed, unchanged).

⚠️ **`shot_diff.py` IS NOT A VALID GATE ACROSS TWO OPTIMIZATION LEVELS, and
this cost a confusing five minutes.** It scored the pairs at 24-78 % changed.
The probe fires every N *presented* frames, but the game runs a variable number
of logic ticks per presented frame, so the faster build is at a different point
in the same camera pan at the same probe index. The pixels are not comparable;
the SCENES are. Judge these by eye, or write a probe that fires on a logic-tick
count. `run_report --vs` confirms the counter half:
`pvr_dropped 1,314 → 1,300` and `aram_zero 7 → 7` — both pre-existing on the
autowalk path, neither caused by optimization.

### What the ban actually rested on

Audited this session, and it does not survive:

1. **It was never reproduced on SH-4.** The entire record is one armhf session
   (2026-07-13) surviving as a comment in `pc/CMakeLists.txt:21-29`. No log,
   no commit, no test case. `kb/traps.md` §9 had already called
   `-O2` "achievable, and probably mandatory" on this target; `kb/closed.md`
   overrode it with the ARM story.
2. **The armhf `-O2` was never isolated** — it shipped together with
   `-mcpu=cortex-a53 -mfpu=neon-vfpv4` (`pc/build-armhf-docker.sh:14`), i.e.
   auto-vectorisation and 64-bit VFP load/store in the same change. The `-O1`
   SIGBUS was attributed to "unaligned LDRD/VFP", which **cannot happen on
   SH-4**: max alignment is 4 and there are no 64-bit integer loads.
3. **Upstream's own "compile everything at -O2" commit needed one line** — a
   definition for `JUTRomFont::spFontHeader_`, missing from the decomp. An
   `-O2` build that starts referencing an undefined static pointer is an
   extremely good match for "wild-pointer crash loop from boot". ⚠️ **That
   symbol is still undefined in this tree** — absent from both ELFs because
   `--gc-sections` drops its callers — and it is being left that way
   deliberately: if an optimized build ever emits a reference, the linker
   fails loudly instead of the game dereferencing NULL.
4. **`-O2` on `emu64.c` was device-verified SAFE on armhf** (`kb/perf-dc.md` #8:
   train passes, crashes=0) — the same TU, and the same intro train scene the
   `-O1` SIGBUS was blamed on.

### What the decomp's UB actually looks like, measured

`DC_TARGET=warnscan bash dc/build-dc.sh` recompiles all 3,926 TUs at `-O2`
with the decomp's `-w` removed (132 s). 64,729 warnings; reduced by
`tools/dcopt/warnscan_report.py` to the classes an optimizer can act on:

```
return-type=35 (in 30 files)   uninit=99   bounds=8   sequence=3   aliasing=0
```

- ✅ **`emu64.c` — the hot file, and one of the four TUs compiled as C++ — has
  ZERO return-type warnings.** That is the single most reassuring line in the
  scan: in C++ a missing return is UB that G++ turns into
  `__builtin_unreachable` and deletes the path outright.
- ⚠️ **`jammain_2.c` is the one file where both halves meet**: C++ *and* a
  missing return *and* 22 uninitialised reads, the most in the tree. Not
  quarantined, because at `DC_AUDIO=0` it never ticks — but it is the first
  file to suspect the day the audio work starts.
- The other 28 are ordinary C, where a missing return costs the caller a
  garbage value rather than a deleted path, and the port walks the whole town
  with all 35 outstanding.

### What was built to make this safe rather than lucky

| thing | what it is |
|---|---|
| `DC_OPT_PROFILE=perf\|size\|o0` | `perf` = `-Os` + `-O3` on the hot list (default); `size` = `-Os` everywhere, the lever for when the image will not fit; **`o0` = byte-identical revert** |
| `dc/opt-lists.mk` | the `-O3` hot list (14 TUs) and the `-O0` quarantine list (empty), each entry with its evidence. A stale path is a **hard error**, not a silent no-op |
| `OPT_GUARDS` | `-fno-isolate-erroneous-paths-dereference` (stops a tolerated NULL deref becoming a trap — the "wild pointer" shape), `-fno-ipa-sra` (the decomp calls 968 K&R `()` functions, some WITH arguments), `-fno-store-merging` (SH-4 traps any misaligned store), `-fno-ipa-icf` (keeps crash addresses unambiguous) |
| `DC_AUTOVAR_INIT=zero` | the uninitialised-read A/B. If a symptom disappears under it, the bug is in the 99 |
| `make warnscan` + `tools/dcopt/warnscan_report.py` | the scan above, and its reducer |
| `tools/dcopt/bisect_o0.sh` + `predicate_town.sh` | binary search for a miscompiling TU via `DC_OPT_O0_EXTRA`, with both sanity gates (the failure must reproduce with nothing quarantined, and must vanish with everything quarantined) |

### The hot list, and the arithmetic that keeps it short

`-O3` on 14 TUs costs **+48,476 B** of `.text` over flat `-Os` and bought
**3.5 ms** (18.5 → 20.0 FPS). It is short on purpose: the frameskipped tick
runs ALL of `game_main` and skips only the draw, and it costs 2.8 ms against
the drawn tick's 46.8 — so `emu64.c` is most of the frame and **every other
`src/` TU in the port shares those 2.8 ms**. `m_player.c` (114,350 B of
`.text`, the largest object in the image) and `m_collision_bg.c` are
deliberately NOT on the list for that reason.

⚠️ **`-O3` on `emu64.c` is unproven anywhere in this port's history; `-O2` on
it is device-verified on armhf.** If an optimized image misbehaves in the
display list, `DC_OPT_O0_EXTRA=src/static/libforest/emu64/emu64.c` is the first
experiment, and `DECOMP_HOT_OPT=-O2` is the second.

### What this does NOT change

- The town still has zero villagers (save path unwired, N2b).
- The black shadow wedges are still there (TEV config #007).
- `gap=7.92 ms` is still unexplained.
- **Nothing here has run on hardware.** Flycast models no cache and no bus
  contention, and `-Os` shrinks `.text` by 2.8 MB — which on real hardware also
  means far better instruction-cache behaviour than the emulator can show. The
  hardware number could be better than 20 FPS or worse; it is unmeasured.


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
section, `kb/levers.md` L1, `kb/levers.md` P1/P2, `kb/plan-stages.md` S4 and
`kb/levers.md` T1 all read "villager textures 1,154,944 B",
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
`kb/levers.md`, `kb/levers.md`, `kb/plan-stages.md`,
`kb/levers.md` and `kb/RESUME.md`.

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
the strongest evidence yet for `kb/levers.md` T1.

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

