# RESUME — the handoff. A fresh context starts here.

Then `kb/STATE.md` for the numbers and the queue, `kb/state-log.md` for the
evidence behind any figure, `kb/closed.md` before proposing anything, and
`kb/traps.md` before touching the build, the harness or an instrument.

Rewritten 2026-08-09 in the kb audit. Everything session-by-session that used to
live in this file is in `kb/state-log.md`, newest first — this file is what is
TRUE and what is OPEN, not what happened.

---

## 1. Where the port is

**It walks the town, with music.** It boots from a burned CD-R on a retail
Dreamcast with loading **at parity with the emulator** (human verdict, the
`AC-DC-20260804` burn). In Flycast it reaches the town, walks it, meets Tom Nook
and is taken to the houses. All 3,936 objects in the link compile and link for
sh-elf with zero exclusions.

🔴 **HARDWARE IS MUCH SLOWER THAN THE EMULATOR. THE AUDIO IS FIXED; THE FRAME
RATE IS NOT.** Human verdict, restated 2026-08-10 and **authoritative**:
*"hardware does not run better than the emulator, it runs much worse. the audio
sounds good though."*

⚠️ **THIS CORRECTS A CLAIM THIS FILE CARRIED FROM 2026-08-09 TO 2026-08-10.**
The S14 burn verdict *"definitely runs better on real hardware"* was a
comparison against **the previous hardware build**, not against Flycast, and
this file promoted it to "better than the emulator measured". It is not. What
S14 actually banked, and what still holds:

- ✅ **the audio tail** — *"sound is perfect, no skipping"*, *"music doesn't cut
  out at all or stutter"*, against `[STUTTER] 65 / 900 s` in Flycast.
- 🔴 **the frame rate — NOT banked.** *"the FPS is still definitely worse than
  emulator"* (2026-08-09) and *"much worse"* (2026-08-10).

**Measurement rule 12 survives on the audio half alone** and is unchanged in
mechanism: Flycast scored S14 a wash (`us/v` 2.51 → 2.48, inside the noise
floor) and the burn fixed the stutter, so an emulator "no change" still cannot
falsify a locality claim. What must not be repeated is reading that as an FPS
result. `kb/batch-s14.md` §7.

⭐ **CONSEQUENCE — THE PIVOT OF 2026-08-09 WAS BUILT ON THIS AND IS WITHDRAWN.**
"FPS is good enough on hardware, the workstream is now playability" rested on
the misread verdict. **FPS is not good enough, the deficit is on silicon, and
Flycast cannot see it** (§6). The only hardware FPS FIGURE the project has ever
had is still the old "~11 FPS in the town", and nothing has attributed the gap
to a specific cause. **§6 is no longer an open question filed for later — it is
the FPS workstream.**
`AC-DC-20260809c-nof5.cdi` isolates F5 (same objects, only the link order
differs); `AC-DC-20260809a-pmcr.cdi` gives `istall`. **Every FPS number in this
kb is still Flycast's, and Flycast models no instruction cache, no operand cache
and no disc seek time.** See §6.

Current numbers: `kb/STATE.md`.

---

## 2. The build lines

### The shipping config

```bash
DC_STUB_KEEP="$(grep -v '^#' tools/dcstub/keeplist-full.txt | paste -sd: -)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=1048576 DC_ARENA_BYTES=1200000 \
DC_AUDIO_SCENES=all DC_AUDIO_DISC_FRAMES=8 DC_AUDIO_VOICES=12 \
DC_CDI_PAD=1 bash dc/build-dc.sh
```

⚠️ **The keep list is `keeplist-FULL.txt` since 2026-08-09**, not
`keeplist-town.txt`. It is the town list plus 650 more `src/data/model/` files
that T1's freed bytes pay for. `keeplist-town.txt` is still the right list for a
T1-off build, and the two are NOT interchangeable: `keeplist-full` with
`DC_TEXPOOL_DEMAND=0` is ~11.4 MB of span and does not fit.

⭐ **Everything else that matters is a DEFAULT and needs no flag**:
`DC_EMU64_CULL=1` (G3), the vertex-index side channel, `pvr_dr_*` emit,
`DCGXVertex` `aligned(32)`, the branch-free memo compare, the shade-predicate
hoist; **and since 2026-08-09 the S14 batch** — the 32-byte memo stride, the
dropped `oargb` store, the source-vertex `pref`, the Gribb-Hartmann cull,
decal-Z arming, and F5's `DC_SECTION_ORDER`
(**`kb/batch-s14.md` is the rollback contract — one line reverts all of it**).
⚠️ **`-DDC_GX_NRMSKIP` is the one S14 item that is OFF**: its own gate proved it
a strict no-op on day one, because emu64 already guards the `GXNormal*` call on
`G_LIGHTING` (`kb/batch-s14.md` §2b). Do not turn it on.
And inside the `DC_AUDIO=1` block `DC_ARAM_AUDIO_DROP=0`,
`DC_AUDIO_MIXRATE=24000`, `DC_AUDIO_SUBDELAY=0`, `DC_AUDIO_MAX_FRAMES=6`. Every
one is `?=`, so naming it explicitly still wins, and each has a documented
revert. **This was deliberate: a result that lives only in a command line is one
unset environment variable away from being lost.**

### The run variants

Add to the prefix above, then run the harness:

| run | add | then |
|---|---|---|
| **perf** | `DC_AUTOSTART=300 DC_SCIF_FAST=1 DC_XDEFS='-DDC_PERF_PHASE'` | `bash harness/dc/smoke.sh <copy>.cdi --timeout 600 -c config:LimitFPS=no` |
| **vertex split** | the above **+** `DC_PVR_VTXSPLIT=16` | same |
| **screenshot** | `DC_FB_PROBE=150 DC_FB_IMAGE=2 DC_XDEFS='-DDC_PERF_PHASE -DDC_AUTOWALK=1'` | `… --timeout 900 --fb-writeback …` then `python3 tools/dcfb/fbimg_to_png.py <run>/console.log --out /tmp/shots` |
| **opcode histogram (G1)** | `DC_EMU64_HIST=1 DC_XDEFS='-DDC_PERF_PHASE'` | same as perf |
| **burn** | drop `DC_SCIF_FAST`, `DC_AUTOSTART`, `DC_AUTOWALK` and every probe; keep `DC_CDI_PAD=1`; **add `DC_CONSOLE_MUTE=1` for a PLAY burn** | burn the CDI |

⭐ **`DC_CONSOLE_MUTE=1` IS WORTH ~5 % OF THE FRAME ON A PLAY BURN, AND EVERY
BURN THIS PROJECT HAS EVER MADE PAID IT.** Measured 2026-08-13 by
phase-subtracting the two hardware gprof runs in `dc/build/gprof-runs`
(1,889-frame title demo minus the 69-frame boot arm):

| | busy/frame | `scif_*` | % of busy |
|---|---:|---:|---:|
| boot-dominated (69f) | 120.6 ms | 16.09 ms/f | **13.34 %** |
| title demo (1958f) | 76.4 ms | 4.37 ms/f | 5.71 % |
| **steady state** | **74.8 ms** | **3.92 ms/f** | **5.25 %** |

KOS busy-waits on the SCIF TX FIFO **with no cable attached**, at ~174 µs per
logged byte. It is boot-weighted but does not go away. ⚠️ **Flycast measures
the same code at 0.50 % and understates it ~8×** — rule 12; no emulator A/B can
settle it. `dc/build-dc.sh` now warns when `DC_CDI_PAD=1` is built without an
explicit choice.
⚠️ **It is NOT the default and must not become one**: it silences crash dumps
(so a *triage* burn wants `DC_CONSOLE_MUTE=0`), and it would blind
`run_report.py` mid-run on any smoke, since the mute arms at frame 300.

**Verdict on any run:**
`python3 tools/dcqa/run_report.py <run>/console.log --vs <baseline>/console.log`

- ⚠️ **`DC_SCIF_FAST` on hardware loses the console** — a coder's cable will not
  sync at 1.5 Mbps, crash dumps included.
- ⚠️ **Build to a COPY of the CDI before a long run** — Flycast holds the file
  open for the whole run.
- ⚠️ **The harness writes `console.log` only when Flycast EXITS.** Polling
  mid-run finds nothing and reads exactly like a hang. A 900 s run is ~17-20 min
  of wall clock.
- ⚠️ **A game smoke run ALWAYS exits 1** with `status=exited_early` — the game
  never returns. `run_report.py` is the verdict, not the exit code.
- ⚠️ **Never build a perf run with `DC_FB_PROBE`** — rule 4.
- ⚠️ **G1 and G2 are mutually exclusive** and it is an `#error`
  (`dc/include/dc_platform.h:454`): both install into emu64's dispatch table.

---

## 3. THE MEASUREMENT RULES — twelve, each paid for

1. **`grep 'ASSET MISSING' <run>/console.log` must be empty** before you believe
   any visual comparison.
2. **Judge a renderer change on a screenshot pair, not on counters.**
   `tools/dcqa/run_report.py` is the floor, not the verdict — it cannot see
   colour.
3. **Total frames is NOT a progression metric.** Use `deepest_scene`. The town
   is ~11 FPS and the train intro 30, so arriving *sooner* lowers the count.
4. ⚠️ **NEVER build a perf run with `DC_FB_PROBE`.** The dump costs **1,506 ms**
   into the `vi` bucket and dragged p1 from 11.56 FPS to 8.50. Screenshot runs
   and perf runs are different experiments.
5. ⚠️ **Estimate from a matched-frame A/B, not from instruction counts.**
6. ⚠️ **`MEMLEDGER FIT … OK` IS NOT A STATEMENT THAT THE IMAGE BOOTS.**
   `margin=` *is* libc's pool and the ledger has no model of libc's demand. A
   build reported `margin=1606292 OK` and died on the splash with
   `Out of memory … diff 1449984`. Derive real headroom from an OOM pair, never
   from `margin=`. `kb/heap-two-pools.md`.
7. ⚠️ **An AVERAGE cost per command is not the cost of ANY command.**
   `emu64_ms = 12.31 µs/cmd × cmds + 9.20 ms` is a fit against TOTAL `cmds`,
   which correlates with `vtx`, so the coefficient belongs to the heaviest
   opcode and to no other. This rule was broken twice, once *inside* its own
   text. **Only the histogram (G1) may price an opcode.**
8. ⚠️ **IN A STUBBED IMAGE AN ASSET CLASS COSTS WHAT THE KEEP LIST KEPT — not
   what the class totals.** `DC_ASSET_STUB` already dropped the rest; an unkept
   asset is a 1-byte `.bss` symbol with its load suppressed. Measured: villager
   textures are 1,154,944 B on paper and **90,464 B resident**; villager models
   438,640 B on paper and **5,536 B resident**. **Cost a pool against the
   alternative — keeping the class — never against the class total.** A pool is
   worth building when it delivers content the keep list cannot afford, not when
   it "frees" bytes the stub system dropped long ago.
9. ⚠️ **STATE THE DENOMINATOR. `[EMU64H]` IS PER LOGIC TICK — DOUBLE IT.** G1
   arms at the end of *every* tick (`dc_vi.c:490` the frameskip path,
   `dc_vi.c:839` the presented path) and `s_frames` counts ticks. Proof both ways: `tot 24.28 × 2 =
   48.56` vs `draw 45.6 + skip 2.9 = 48.5`. **Two sessions quoted the halved
   numbers.** `[GXSPLIT]`, `[PHASE]`, `[VTXSPLIT]` and `[PMCR]` are per
   PRESENTED frame — do NOT double those. ⚠️ `probe=` is not subtracted from
   `tot` or `gap`; both probes land inside `gap` by construction.
10. 🔴 **STATE THE DENOMINATOR *WITHIN* AN INSTRUMENT TOO — `[VTXSPLIT]`'s SEVEN
    BUCKETS DO NOT SHARE ONE.** `memo` is charged on every vertex; `xf`, `lit`,
    `tex`, `shade`, `post` only on memo **MISSES** (their `VS_MARK`s sit
    downstream of the memo-hit `continue`); `emit` is per PRIMITIVE. A
    per-vertex figure for the middle five must divide by `v × (1 − hit_rate)`.
    At a ~50 % hit rate that is a factor of two, and **two documents published
    the halved figures**. The ms-per-frame figures the buckets print are correct
    as printed; only the per-vertex derivation was wrong.
11. 🔴 **THE NOISE FLOOR ON `us/v` IS ~±2 %. ONE A/B PAIR CANNOT RESOLVE A SMALL
    CHANGE.** Measured over five 600 s runs on one build line: the same change
    read **+1.1 %** against one baseline and **−2.0 %** against another — the
    sign flipped. Cause: the town reseeds per boot (`sys_math.c:7` →
    `sqrand(osGetCount())`), and `us/v` normalises for vertex COUNT but not for
    WHICH vertices. **Under ~4 %: run each arm 2-3 times and compare groups, or
    report it as inside the floor. Never pick the favourable pair.** Session
    12's wins were 8-18 % and were safe on one run each; that does not license a
    2 % claim.

12. ⭐⭐⭐ **A FLYCAST "NO CHANGE" IS NOT EVIDENCE AGAINST A CHANGE WHOSE
    MECHANISM IS CACHE — PROVEN 2026-08-09.** Batch S14 measured as a **wash**
    in Flycast (`us/v` 2.51 → 2.48, inside the ±2 % floor, every `[VTXSPLIT]`
    bucket within 0.03 ms) and came back from a burned CD-R with *"sound
    perfect, no skipping"* against `[STUTTER] 65 / 900 s` in the emulator.
    Flycast models **no instruction cache and no operand cache**, so a change
    made of locality is invisible there BY CONSTRUCTION. **The emulator can
    falsify an instruction-count claim; it can NEVER falsify a locality claim**
    — and the converse trap is that a Flycast *win* on such a change is
    understated, not absent.
    ⚠️ **The FPS half of this rule's original evidence is WITHDRAWN (2026-08-10).**
    The burn's *"definitely runs better on real hardware"* was against the
    previous **hardware** build, not against Flycast; the standing human verdict
    is that hardware is **much worse** than the emulator. The rule rests on the
    audio result. **Do not cite rule 12 as evidence that any FPS change landed
    on silicon — nothing has ever been measured there.** §1.
    ⚠️ **The audio half measures the TAIL, not the median.**
    `DC_AUDIO_MAX_FRAMES` caps production at
    `MAX_FRAMES × 17.49 ms × 2 ticks` **per PRESENTED frame** (§5 audio rule 4),
    but at 6 the sustained floor is ~4.8 FPS, which this port cleared long ago.
    `[STUTTER]` fires on frames that individually blow the budget, so "no
    skipping" says **the p99 frame time came down and says nothing about p50**.
    Confirmed by the same human in the same session: *"music doesn't cut out at
    all or stutter on hardware, but the FPS is still definitely worse than
    emulator."* **Tail fixed, median still short.** `kb/batch-s14.md` §7.

---

## 4. The instruments

| instrument | knob | denominator | notes |
|---|---|---|---|
| `[PHASE]` | `-DDC_PERF_PHASE` | presented frame | `draw`/`skip`/`vi`, `cull`/`xform`, `v`/`vlit`/`vcull`/`us/v` |
| `[VTXSPLIT]` | `-DDC_PVR_VTXSPLIT=<N>` | **mixed — rule 10** | splits `xform` into 7 stages, sampling 1 primitive in N |
| `[EMU64H]` | `DC_EMU64_HIST=<N>` | **logic tick — rule 9** | per-opcode timing; the only thing allowed to price an opcode |
| `[EMU64C]` | `DC_EMU64_CULL=1` | 30-frame window | G3's own counters: `trin`/`cull`/`vis`/`punt`/`pdec`/`ptgen`/`pmix`; and under `-DDC_PERF_PHASE` the **S14 timing split `cus`/`cds`/`fus`** — total `cull_batch()`, emu64's `dirty_check`+`setup` inside it, and the frustum test alone. ⚠️ **The three NEST**, so `cds`/`fus` carry one extra `dc_time_us()` pair each and `cus` three: fine for apportioning 3 ms, not for costing a 200 µs change |
| `[PMCR]` | `DC_PMCR=1`, `DC_PMCR_HUD=1` | presented frame | ⚠️ **burn-only — Flycast returns 0 for all 8 events** |
| framebuffer | `DC_FB_PROBE` / `DC_FB_IMAGE` | — | ⚠️ never in a perf run (rule 4) |
| `icache_map.py` | host-side | — | sizes icache pressure; only `istall` on a burn prices it |

**Gates** (they never change behaviour, they only check it):
`-DDC_EMU64_CULL_VERIFY` (runs both paths, counts a batch we would have dropped
that the reference drew), `-DDC_GX_VTXID_VERIFY` (content-checks every
vertex-id hit). ⚠️ **`-DDC_EMU64_CULL_VERIFY` cannot certify the vertex-id side
channel** — it only checks batches we cull, and a decal batch never culls.

---

## 5. The subsystems, as they stand

### G3 — the cull at `G_TRIN_INDEPEND` entry (`dc/src/dc_emu64_cull.cpp`)

Installs trampolines into emu64's dispatch table for slots 59 (`G_TRIN`) and 60
(`G_TRIN_INDEPEND`), walks the packed index stream, builds an AABB over the
referenced vertices in emu64's own `vertices[]`, and consumes the command when
the box is fully outside the frustum — so vertex references that `dc_gx.c` used
to reject at flush time never reach `set_position` or our `GX*` setters.
**−19.9 ms of a 69.8 ms town frame when it shipped; 61 % of TRIN batches culled;
`vcull` 9,915 → 1,002.** Gate passed `falsecull=0 gfxp_bad=0 reinst=0` over 473
windows. Since 2026-08-09 it also **records the index sequence** its AABB walk
already visits and arms `dc_gx.c` with it — the source half of the vertex-index
side channel.

- **The frustum test is one implementation with two inputs.**
  `dc_gx_aabb_is_offscreen()` is shared; the two callers coincide only because
  `emu64.c:4935`'s `GXEnd()` makes flush granularity exactly one TRIN.
- **Three punts, each load-bearing for CULLING**: decal-Z (`set_position`
  submits a projection round-trip, not `position`), `G_TEXTURE_GEN` (transforms
  `normal` in place with no idempotence flag), and any batch whose referenced
  vertices disagree on `MTX_NONSHARED`. **ARMING is a weaker bar than culling** —
  see `kb/STATE.md` action 1.
- ⚠️ **G3's tripwire must run BEFORE `dc_emu64_hist_frame_open()`** at both
  `dc_vi.c` sites. G1/G2 rewrite all 64 slots per sampled frame; G3 re-installs
  its two. Wrong order and G3 evicts G1's thunks for exactly the two opcodes G3
  exists to fix. `reinst=` is the tripwire. `kb/traps.md`.
- ⚠️ **`-DDC_NO_BATCH_CULL` also disables G3** — it means "no frustum cull", not
  "no *late* frustum cull".
- **Still open:** the screenshot pair (rule 2), and on the visible path
  `dirty_check` + `setup_1tri_2tri_1quad` run twice, which is not separately
  measured.

### The vertex-index side channel (`dc_gx.c` + `dc_pvr.c`, 2026-08-09)

`GXPosition3f32` walks the recorded sequence with a cursor and stamps
`(epoch << 8) | index` into **`DCGXVertex` bytes 30-31, which were dead
padding** — `sizeof` stays 32, the `aligned(32)` is untouched. The memo then
keys on the stamp: no hash, no 30-byte compare, and **no random read into
`verts[]`**, which was the operand-cache miss that made `memo` 122 cycles a
vertex. **`us/v` 2.68 → 2.51 (−6.3 %), hit rate 50.9 → 53.7 %.** Kill
`-DDC_GX_NO_VTXID`. Gate ran `vidchk=15,538,941 vidbad=0 over=0`.

- **The epoch is load-bearing.** `GXBegin` merges batches
  (`pc_gx_merged_batches`): one submit can hold two TRIN commands and emu64
  **reloads `vertices[]` between them**, so a bare index would hand the second
  TRIN the first one's transforms.
- ⚠️ **The win lands in `shade`/`lit`/`tex`/`post`, not in `memo`** — those are
  charged on memo misses (rule 10), and the miss-count drop is ~¾ of the gain.
- 🔴 **This is NOT the 13.31 ms block.** That is `dl_G_TRIN`'s index expansion
  plus our own `GX*` setters; the side channel removes no setter. Several kb
  files conflated the two — do not add to that.

### T1 — every display-list texture comes off the disc (`dc/src/dc_texpool.c`)

Landed 2026-08-09. The 6,068 texture arrays the display lists name by ADDRESS
are stubbed to `[1]` and read off `/cd/foresta.rel` into ONE 4,096 B staging
buffer at the moment the PVR first binds them. **The row index is the cache
key**, so `tex_content_hash()` — up to 512 B of main RAM read on every one of
~109 binds a frame — is gone for those rows, and an array that is not read on a
bind does not have to be resident. **−841,888 B on a matched link.** Kill
`DC_TEXPOOL_DEMAND=0`. Full numbers and the runtime counters: `kb/levers.md` L10.

- **Its keep-list half is the first PER-SYMBOL rule in this project.** R1/R2/R3
  each removed whole FILES; T1 cannot, because a texture array shares its TU
  with vertex arrays that must stay. `make_stub_data.py`'s `DEMAND_STUB` beats a
  keep-list entry, which turned the partial-keep path from dead code into the
  common one. `kb/traps.md` has the three things that broke on the way.
- ⚠️ **24 rows are EXCLUDED** — reached through a pointer table and then
  `gSPSegment`, which the old exclusion regex could not see. They stay resident;
  their six owner files are pinned in `make_keeplist_town.py`'s `MODEL_REQUIRED`.
- 🔴 **T1 SHIPPED TWO GARBLED-TEXTURE BUGS AND BOTH ARE NOW FIXED — read this
  before adding anything to the demand path.** `kb/batch-s15.md` S15-6/S15-8.
  **T1 can only serve a texture whose ARRAY ADDRESS reaches
  `dc_gx_backend_texture_upload()`.** `gDPLoadTextureTile` /
  `gDPLoadTextureBlock*` go through emu64's TMEM emulation, which copies the
  texels out of the array into `texture_buffer_data` *before* any hook this
  port owns — so a stubbed array is read as its one byte and the garbage is
  baked in upstream (154 rows; the title screen's `Press START!` was two bars
  of noise). And `scan_asset_declarations()` let the `sorted()`-last file
  decide linkage, stubbing two arrays that are `static` in one file and global
  in another (2 rows).
- ⚠️ **T1'S FALSIFIER CANNOT FALSIFY IT.** Under the loader every row has
  `kept == 0`, so `lookup()`'s early-out makes `interior=` and `mutated=`
  **incapable of incrementing**. Every `interior=0 mutated=0` verdict on record
  was taken where the counters could not move. Honest config:
  `DC_TEXPOOL_PROBE=1 DC_TEXPOOL_DEMAND=0` + `keeplist-town.txt`.
  ⭐ **The instrument that actually worked is `-DDC_TEXPOOL_TRACE=<N>`, and it
  works by ABSENCE** — a texture that renders as noise and is not in the trace
  was never fetched, which is a LOOKUP failure, not a loader failure.
- ✅ **Signed off visually 2026-08-09**: title screen text correct, human
  verdict on the town *"visually looks pretty good"*. The old "lavender ground"
  entry here was the paved plaza in two differently-seeded towns.

### The shared read-ahead window (`dc/src/dc_assetwin.c`) — ⭐ **DEFAULT OFF**

Every mid-scene demand read goes through it: T1's, R1's, R2/R3's, and
`dc_stub_keep_load_one()`'s post-boot path (gated on `dc_stub_boot_done`, so the
boot sweep is untouched). At `DC_ASSETWIN_B=0`, the default, that is a plain
seek+read — byte-for-byte the pre-T1 path.

🔴 **IT WAS BUILT, MEASURED FOUR WAYS, AND TURNED OFF. Do not re-propose it
without reading this.** One 900 s town run, same build otherwise:

| refill policy | reads | bytes off disc | `[STUTTER]` |
|---|---:|---:|---:|
| 32 KB always | 148 | 4,849,664 | 94 |
| sectors + sequential | 218 | 2,177,024 | 147 |
| **OFF (default)** | 271 | **325,184** | **109** |

Baseline without T1 is 90 stutters. **325,184 B is the true payload; every
window variant paid 6.7-15× it.** The premise was L10's "median gap 512 B, 863
of 905 gaps under 32 KB" — **which describes those offsets SORTED**.
`dc_keep_sweep()` earns clustering by sorting the whole request list first; a
loader driven by the renderer's bind order cannot, and inheriting the conclusion
without the sort was the error.

⚠️ **It is OFF, not deleted, because RULE 12 applies.** On a CD-R, 271 seeks is
5.4-27 s and 4.85 MB of read-ahead is ~9.7 s — the same order, and Flycast
models neither. Its FastGDRomLoad makes a byte free, which is exactly why the
emulator ranked the 32 KB variant best. **A burn settles it; a smoke run cannot.**
⚠️ And judge it on `reads=` AND `bytes=` together — `reads=` alone is what hid
the 4.8 MB for a whole iteration.

### Audio — the four rules from four burns

Every burn falsified the previous fix's **scope**, never its mechanism. The
mechanism was right the first time; the location was wrong three times.

1. **There is exactly ONE place a disc read may block —
   `dc_dvd_read_yielding()` (`dc/src/dc_dvd.c`). Add new `fs_read` calls THERE,
   never at a call site.** Five sites route through it.
2. **Yield BEFORE the seek**, not only between chunks. A 100-200 ms head
   movement cannot be subdivided by any chunk size; the only defence is entering
   it with a full ring.
3. **The re-entrant case is "poll, do not synthesise", not "do nothing".**
   `snd_stream_poll()` touches no jaudio state and is safe at any depth;
   re-entering `pc_audio_process_frame` is not.
4. ⭐ **`DC_AUDIO_MAX_FRAMES` IS AN FPS CONSTANT, NOT AN AUDIO ONE.** Production
   is capped at `MAX_FRAMES × 17.49 ms × 2 ticks` per presented frame. At the
   old value of 2 that is 70 ms/frame — **the audio cannot keep up below
   ~14 FPS however cheap synthesis becomes.** Now 6 (~4.8 FPS).

⭐ **The peak audio cost is VOICE COUNT**, measured with the music actually
playing: `cost ≈ 2,332 µs + ~265 µs per voice-update`, monotonic across every
bucket. The "bimodal 2.5 / 10 ms" chased for two sessions was **SFX-only versus
music-playing**, and `filt@=0 comb@=0` on every `[STUTTER]` row retires the
FIR/comb suspicion. Peak is 14-15 concurrent voices; `DC_AUDIO_VOICES=12` bounds
the worst frame linearly.

⭐ **And the largest single win was not code: `DC_ARAM_WINDOW` 131072 →
1048576.** Matched 420 s runs: disc reads 4,183 → **106**, bytes off disc
137.9 MB → **4.3 MB**, evictions 4,173 → **68**. It sat at 131072 because that
was measured when RAM was the binding constraint, which stopped being true on
2026-08-06, and nobody re-costed it. **Generalise: when a constraint is lifted,
re-cost everything that was sized under it.**

---

## 6. 🔴 THE HEADLINE OPEN QUESTION — hardware is not the emulator

**Expected in DIRECTION, unmeasured in MAGNITUDE.** Flycast models no
instruction cache; the SH-4's is **8 KB direct-mapped** against a 2,876,648 B
`.text`+`.rodata`. `tools/dcopt/icache_map.py` sizes the pressure host-side:
the town frame's hot symbols are **16.40×** the icache and the innermost draw
loop **2.62×**.
⚠️ **The 11.9× / 1.4× pair quoted here until 2026-08-09 is FALSIFIED**: the
tool's hot-set regexes matched `^_dl_G_` / `^_emu64`, but emu64 is C++ and every
handler is mangled, so the interpreter — most of the draw — was never in the
measurement (`.text._ZN5emu64*` = 105 map sections, `.text.dl_G_*` = 0).
The pressure is worse than believed AND F5's ceiling is lower: at 2.62× the
inner loop can be made contiguous but never resident. `kb/batch-s14.md` §5.
⭐ **F5 SHIPPED 2026-08-09** (`dc/section-order.txt`, `DC_SECTION_ORDER=0` to
kill): `dc_gx_backend_submit` moved `0x8c221118 → 0x8c022c20`, i.e. from 2 MB
away from the interpreter to directly behind it. **No emulator can price this.**

⚠️ **NO AMOUNT OF FLYCAST WORK CAN ANSWER THIS.** Same trap as the disc-timing
refutation, one layer up: do not "A/B it in the emulator".

The instrument exists — **P1, `dc/src/dc_pmcr.c`**, SH7750 performance counters
on **PRFC1** (KOS owns PRFC0), rotating through 8 events one per 30-frame
window, bracketed into `[PHASE]`'s draw/skip/vi plus `audio` and `xform`.
`DC_PMCR_HUD=1` draws the table on the TV. It is **blocked on a burn**:
`AC-DC-20260808g-pmcr.cdi`, built and unburned.

- ⚠️ **`DC_CONSOLE_MUTE=1` is not optional on a measuring burn** — KOS
  busy-waits on the SCIF FIFO with or without a cable, and a perf build puts
  ~10 lines into every window, so the log would be measuring itself.
- 🔴 **But muting at `main()` stops the game booting.** The `-f` burn showed the
  HUD and never booted; this port's boot has always run with hundreds of ms of
  implicit console delay in it. The mute now arms at `DC_CONSOLE_MUTE_FRAME`
  presented frames (default 300), inside the game loop. The HUD leads with a
  liveness line `f= t= d= c=` so a repeat failure is readable off the screen.
  `kb/traps.md`.
- ⚠️ **`--symbol-ordering-file` is an LLD flag and we use GNU ld** — 2.45.1
  wants **`--section-ordering-file`**, which does the same job because
  `-ffunction-sections` is already on.
- **What to photograph:** the town, standing still, ~12 s after boot (rows read
  `--` until their event has had a window). **`cyc` (does `ms` ≈ `wall`?),
  `istall`, `dstall`.**

---

## 6b. ⭐ P2 — THE INSTRUMENT §6 ASKED FOR NOW EXISTS AND HAS RUN (2026-08-12)

`dc/src/dc_profdump.c` + the `DC_GPROF` block in `dc/Makefile` + the nine knobs
forwarded by `dc/build-dc.sh`. **`-pg` on the LINK LINE ONLY, zero TUs** — the
optimized code stays byte-identical, and the map proves our strong
`gprof_init()` wins over `libgprof.a(gmon.o)`'s. Validated end to end in
Flycast, console sink:

```
[GPROF] END lines=28 raw=612433 enc=1585 crc32=2fd07100
612,433 B gmon.out recovered; 31,010 samples in 470 non-empty bins at 100 Hz
94.76% thd_idle_task | 0.66 vid_waitvbl | 0.51 dc_gx_backend_submit
0.50 scif_write | 0.38 RspStart | 0.20 emu64::set_position | 0.18 cull_batch
```

⭐ **The z0 zero-run RLE is what makes it affordable: 612,433 → 1,585 B → 28
console lines**, not the ~145 s of serial the naive estimate predicted.
🔴 **IDLE IS 94.76 % — COMPARE NON-IDLE SHARES, NEVER ABSOLUTE SECONDS.**
⚠️ Samples accrue per RESCHEDULE, not per timer tick, so `dc_dvd_read_yielding()`
is over-represented. Relative shares only.

**It is TWO IMAGES and that is forced by hardware, not a compromise**: Flycast
has no SD adapter, and on hardware the adapter occupies the serial port so there
is no console. `DC_GPROF_SD=0` is the emulator half, `=1` the hardware half. The
diff survives it — gprof symbolises against each run's own ELF and `-pg` changes
no game code.

Three things this cost, all now written down where they will be found again:

- 🔴 **`fopen(...,"a")` IS UNUSABLE ON `fs_fat`.** `O_APPEND` is `0x08`, inside
  KOS's `O_MODE_MASK` `0x0f`, so `fs_fat_write()` returns `EBADF` on every
  append write — while `fs_fat_open()` does **not** check the mode, so the open
  succeeds. libgprof writes gmon.out with `"a"`. A naive `/sd` sink comes home
  from a burn with a **20-byte** file and no error anywhere. Fixed by never
  letting gmon.c open the card: it writes to `/prof` all run and `prof_write()`
  forwards into a file this port opened in `"w"` and holds open.
- 🔴 **PROBING FOR AN ABSENT SD CARD WEDGES.** KOS's `sci_spi_rw_byte` waits on
  RDRF with **no cycle cap** (every TDRE wait in `sci.c` has one; the four RDRF
  waits in the SPI helpers do not), and SCSSR1's reset value passes every
  bounded gate, so `sd_init_ex()` pins forever. SCIF is bounded but
  `acmd41_loop()` is 5000 × two commands × a 500 ms ceiling. **`DC_GPROF_SD_IF`
  now defaults to 0 (SCIF only)** and the "one CDI works with or without a card"
  claim is **falsified**.
- ⚠️ **A wedge behind a muted console is indistinguishable from a game hang.**
  It cost four runs and a wrong diagnosis (twice: "the sampler is slow", then
  "K.K. Slider hangs" — the screen just happened to show K.K.). The dump now
  emits `[GPROF] dump: muting console, probing SD` **before** `dbgio_disable()`.

✅ **Cleared under suspicion:** the sampler is NOT expensive —
`histogram_callback` is ~20 instructions per reschedule, `thd_poll` parks the
caller rather than spinning, and `[PERF]` read 24-28 FPS with it armed.

🔴 **F5 MUST BE OFF FOR A keeplist-town BUILD.** `dc/section-order.txt` was
generated from a **keeplist-full** link; against a keeplist-town link the image
hangs in `maple_wait_scan()` — inside `arch_auto_init`, before `main()`. Proven
by a four-build matrix. ⚠️ **Hardware boots it anyway**, so this is Flycast-only
— but F5 is not inert when the link it describes changes. `kb/hardware-profiling.md`,
`tools/dcprof/README.md`.

---

## 6c. 🔴 WHAT THE FIRST HARDWARE PROFILE ACTUALLY SAID (2026-08-12)

**The scene was the TITLE SCREEN RUNNING ITS DEMO** — a live town with actors,
camera and music, not a static card (human confirmed). So these shares transfer
to gameplay far better than "title screen" suggests, but the vertex load is not
the walked town's. 1,958 presented frames, 100 Hz, F5 OFF, console UNMUTED.

🔴🔴 **THE IDLE SHARE IS NOT A SHARE — IT IS A WRAPPED 16-BIT COUNTER, AND SO IS
`thd_block_now`. NEVER QUOTE EITHER.** Verified against KOS source and against a
disassembly of `hw.elf`, 2026-08-12:

- `HIST_COUNTER_TYPE` (`gmon.c`) is a **uint16**. With the profiler armed the
  histogram thread sits in `STATE_POLLING` forever, so `thd_idle_task`'s
  `thd_has_polls()` branch takes `thd_pass()` instead of `arch_sleep()` — **idle
  SPINS**. It self-samples at order 10^5/s into one bin that **wraps every
  0.2-0.5 s of idle**. "65.78 %" is modular residue. Proof: the `sleep`
  instruction's bin has **zero** samples in all three runs while the
  `thd_block_now` call site one bin away is saturated.
- `thd_block_now` saves **PR as the "PC"** (`thdswitch.s`, `sts.l pr,@-r4`), so
  its bin is context-switch accounting, not blocking time. At 100 Hz, ticks
  landing in that 5-instruction window would need ~10^9 switches.
- ⚠️ **AND THE BIAS RUNS THE OTHER WAY FROM WHAT THIS KB SAID.** The callback
  only counts if the outgoing thread is still `STATE_RUNNING`, so **voluntarily
  blocked threads are NEVER sampled** and blockers are **under**-represented.
  Device-IRQ reschedules never sample either. That is why
  `dc_dvd_read_yielding()` has **zero** samples — not because it is cheap.

✅ **WHAT SURVIVES IS THE USEFUL HALF.** The non-idle, non-`thd_block_now` bins
are **honest 100 Hz timer-IRQ samples of real CPU time** — they cannot wrap
(that would need >10 min of CPU in one 8-byte bin) and no game code yields while
runnable. Quote those, in **ms/frame**, and say "busy" not "non-idle".

⭐⭐ **THE NUMBER THIS PROJECT NEVER HAD: 13,580 busy ticks / 1,889 frames =
71.9 ms of CPU PER PRESENTED FRAME. The title/demo is capped at ~14 FPS by CPU
alone, before any waiting.**

| family | ms/frame | % of busy |
|---|---:|---:|
| **audio** (`RspStart` + Nas/Jac/snd/spu) | **17.2** | **23.9** |
| emu64 (incl. `cull_batch`) | 15.4 | 21.4 |
| `dc_gx`/`dc_pvr` backend | 15.3 | 21.2 |
| GX shim (`GX*`/`PSMTX*`) | 7.0 | 9.7 |
| game logic (`cKF_*`, `mCoBG_*`, actors) | 5.9 | 8.3 |
| `mem*` (incl. newlib's hidden `L_*` labels) | 4.8 | 6.7 |
| **console/serial — removable** | **3.9** | **5.4** |

⭐ **Steady-state disc work is 0.02 ms/frame.** Whatever makes the TITLE slow on
hardware, it is not I/O. (Says nothing about the town with T1 demand loading.)

🔴 **THE INSTRUMENT DISTORTS THE MACHINE, IN A FLYCAST-INVISIBLE WAY.** Armed,
idle spins the full context save/restore (~464 B through the operand cache per
iteration, `movca.l` allocating lines) instead of sleeping — every idle episode
cache-scrubs the next busy period. Flycast models no cache, so this perturbation
exists **only on the machine P2 exists to measure**.

⭐ **THE FIX IS ONE LINE**, in `gmon.c:histogram_callback`:
`if(!irq_inside_int()) return cxt->running_thread ? 0 : 1;` — tick-driven
scheduler entries run inside the timer IRQ, voluntary passes do not. That makes
P2 a true 100 Hz time-proportional sampler and idle/seconds/wall-clock/FPS all
become real. Needs a patched `libgprof.a` in the SDK image. **Do this before the
next profiling burn.**

⚠️ The `%ni` / `%work` renormalisations quoted in the first writeup of this
session are superseded by the ms/frame table above.

### ⭐⭐ THE ICACHE PREDICTION IS FALSIFIED, AND AUDIO IS THE REAL OUTLIER

`kb/hardware-profiling.md` §7 predicted in writing that `dc_gx_backend_submit`
(10,036 B = 1.24× the icache) is "where the hardware run's share grows".

| symbol / family | Flycast | hardware | HW/FC |
|---|---:|---:|---:|
| `RspStart` | 7.20 | **15.69** | **2.18** |
| `dc_gx_backend_submit` | 9.79 | 7.96 | **0.81** |
| `GXPosition3f32` | 3.26 | 1.21 | 0.37 |
| `vid_waitvbl` | 12.62 | 1.51 | **0.12** |
| **audio as a group** | 9.05 | **19.26** | **2.13** |

⚠️ These are shares of a denominator that includes the wrapped idle bin, so read
them as **ratios between the two platforms** (which is what they are for) and
not as fractions of the frame. The honest per-frame figures are the ms/frame
table above. Audio's growth also survives in ms/frame: it is the largest busy
family at 17.2 ms of a 71.9 ms frame, against 9.4 % of Flycast's busy time.

**It shrank.** So does the whole GX setter family. ⚠️ **Read this precisely: a
SHARE only detects DIFFERENTIAL slowdown.** A uniformly icache-starved draw path
would leave every share unchanged, so "the draw path is icache-bound" is
UNTOUCHED and `dc_pmcr.c`'s `istall` is still its only instrument. What is dead
is "`dc_gx_backend_submit` is the icache outlier."

⭐ **`vid_waitvbl` collapsing 12.6 → 1.5 % is the quantitative form of the
standing human verdict.** Per presented frame: Flycast 0.68 samples, hardware
0.13. **The hardware run essentially never waits for vblank — the frame has no
slack.**

### ⭐ AUDIO IS THE LARGEST ACTIONABLE SUBSYSTEM ON SILICON

`RspStart` (`src/static/jaudio_NES/internal/rspsim.c`, the software N64 RSP
microcode simulator) is **14.1 ms/frame of a 71.9 ms frame — the #1 symbol on
the machine**, and audio as a group is **17.2 ms/frame, 23.9 % of busy CPU**.
Its hot bins cluster at `RspStart+0x138..+0x158` (~250 ticks in one 6,248 B
function), which is where an objdump should start. Source-line attribution
through the histogram's 8-byte bins splits it: ADPCM decode 29 %, ENVMIXER 26 %,
RESAMPLE 21 %, `Jac_Resample16` 11 %, MIXER 11 %.
⚠️ Part of the 2.18× is confounded — the hardware run had more music playing
(`Nas_MainCtrl` 2/1624 vs 75/17382). The ABSOLUTE share is not confounded.
🔴 **`DC_AUDIO_VOICES=12`, `DC_AUDIO_MIXRATE=24000` and `DC_AUDIO_SUBDELAY=0`
were ALREADY APPLIED in this build — they are spent, not available wins.**
⭐ **This re-prices `kb/STATE.md` ranked action 8 (AICA stage B) from a
content/RAM project to a ~20 %-of-CPU project.** Its blockers (the ADPCM
step-index discontinuity, the 65,536-sample channel limit, bank 153's
1,971,016 B) are unchanged — better justification does not make them cheaper.

### ⭐ THE 13.31 ms BLOCK SPLITS, AND IT IS ~1 : 12

`kb/STATE.md` has said for weeks that the block is "`dl_G_TRIN`'s index
expansion PLUS our own `GX*` setters, never separated". A flat profile separates
them by construction:

| half | %work |
|---|---:|
| `dl_G_TRIN` + `TRIN_INDEPEND` + `TRI1` + `TRI2` — index expansion | **1.10** |
| setters + `set_position` + `dc_gx_commit_vertex` — the staging half | **9.34** |

**G-B targets the 9.34 % and can touch almost none of the 1.10 %.**
⚠️ Quote `dl_G_TRIN*` as 145 samples TOGETHER — `dl_G_TRIN_INDEPEND` alone shows
3, which for the town's dominant opcode means it inlined into `dl_G_TRIN` in the
`-O3` `emu64.c` TU. ⚠️ And G1's "TRIN is 73 % of the draw" is **inclusive**;
gprof is **self**. Label it or it will be misread.

### Free money, already banked or one flag away

- 🔴 **`scif_write` is 4.17 %ni + `scif_flush` 0.54 % — ~4.8 % of non-idle in
  the serial console, with NO CABLE ATTACHED.** `DC_CONSOLE_MUTE=1` (armed at
  `DC_CONSOLE_MUTE_FRAME`, never at `main()`) is now a **precondition for every
  hardware measurement**, not an optimization. Flycast measures it at 0.50 % and
  therefore understates it ~8× as a share of work.
- ✅ **P3 SHIPPED** — `A_CMD_ENVMIXER`'s four `f32` accumulators retyped to
  `s32` via `make_src_shrink.py`. **Bit-exact and provably so**: the terms are
  `>>16/18/19` of an s16 × u16, so the sum is < 46,000 and the final add < 2^24,
  which f32 represents exactly — truncation is the identity. ~11 % of
  `RspStart`. Kill switch `DC_RSPSIM_NOFP=0`.
- ✅ **`dc_ctz32` SHIPPED** — `__ctzsi2` was 0.62 % of work because SH-4 has no
  CTZ and every `__builtin_ctz` was a `jsr` into a 96-byte libgcc loop, on the
  per-referenced-vertex mark walks in `cull_batch` and in `dc_gx_mark_dirty`.
  De Bruijn LUT in `dc_platform.h`. Kill switch `-DDC_NO_CTZ_LUT`. Changes no
  logic, so the gate is `[EMU64C]` counters byte-identical.

### Still on the table, priced, NOT done

- **The texture-bind hit path is ~3.65 % of work and still hashes.**
  `tlut_content_hash()` walks up to 512 B byte-at-a-time on EVERY bind (~109 a
  frame) — the exact cost T1 removed for textures and left for palettes — plus
  a ~132 B `memset` of which only 28 B is the key, plus `tlut_is_be()`'s 16-slot
  scan. 🔴 **Not attempted: `probe` is NOT lookup-only** (`memcpy(e, &probe,
  sizeof …)` on the miss path, and `probe.aprof` is written), so the memset is
  load-bearing and any key change risks aliasing — which is how T1 shipped two
  garbled-texture bugs. Needs a screenshot pair.
- **The reverb bus is sized by a constant, not the mixrate.** `DMEM_2CH_SIZE` =
  416 samples, sized for 48 kHz; at `DC_AUDIO_MIXRATE=24000` the engine makes
  96. **It mixes 2.17× more samples than exist.** Another instance of "re-cost
  what a lifted constraint sized". ~0.9 %, medium confidence — the attribution
  of `A_CMD_MIXER` to the reverb bus is inferred, not measured.
- **`MAC.W` the resampler FIRs** — up to ~2.7 %, medium. ⚠️ Do NOT extend it to
  the ADPCM chain without a proven bound: `sp9C[l] << temp_r19` with
  `temp_r19 ∈ 0..15` can exceed s16.

### Corrections this profile forces on the kb

1. `kb/hardware-profiling.md` §8 calls `dc_dvd_read_yielding()` "the worst
   offender" for reschedule over-representation. **It has ZERO samples.** A
   function that calls a blocking primitive accrues no self time — the
   primitive does. The real artefact is `thd_block_now`.
2. `kb/STATE.md`'s `fus=` figure put the frustum test at 4.5 % of G3. Hardware
   says `dc_gx_aabb_offscreen_gh` is **~30 % of G3** (1.69 % of work). The
   conclusion (G-F retired) probably survives at 1.69 %; the number does not.
   ⚠️ `fus=` was measured pre-S14 on a different implementation.
3. `dc_pvr_texture.c`'s comment "512 × 68 (sizeof `dc_tex_entry_t`)" is **stale
   by ~64 B** — it predates `aprof[64]`.
4. `icache_map.py`'s `DEFAULT_HOT` does **not** include `^_RspStart`, so the
   "16.40× the icache" hot-set figure omits a 6,248 B function that is 20 % of
   the work. ⚠️ And ordering cannot save it: at 6,248 B in an 8 KB
   direct-mapped cache it aliases 76 % of the sets wherever it is placed.
5. 🔴 **VILLAGER ACTOR PROCS HAVE SAMPLES.** ~22 distinct `aNPC_*` functions
   (`_actor_move`, `_think_main_proc`, `_anime_proc`, `_ctrl_umbrella`,
   `_circleRangeCheck`, …) form a coherent think→move→draw tree, with
   `aNPC_dma_draw_data_proc` at **zero**. §7.1 and `kb/STATE.md` §A both say
   nothing constructs a villager actor. This is the first evidence the actors
   may EXIST and the break is INSIDE DRAW — a different branch of
   `dc_npcdiag.c`'s decision table. ⚠️ It may be the demo's town rather than the
   player's. **Run N3 before more construction work.**

---

## 7. Still broken, ranked

1. 🔴 **THE TOWN HAS NO VILLAGERS. IT IS NOT A SAVE BUG AND IT IS NOT A FIELD
   DATA BUG — BOTH DIAGNOSES ARE FALSIFIED (2026-08-09, S15).**
   ⚠️ **This item used to read "it is a save bug, not an asset bug … the VMU
   path is unwired … N2b is the prerequisite". Do not act on that.** The
   instrument that settled it is `dc/src/dc_npcseed.c`, which logs the roster
   BEFORE it writes anything:

   ```
   [DC/NPCSEED] pre ids=6 homed=6 | seeded id=8 home=8 | want=14 max=14
   ```

   Six villagers already had valid ids **and** valid homes, so `npclist` has
   been populated all along. `animals[]` is **generated** on a new game by
   `mNpc_DecideLivingNpcMax` (`m_npc.c:2422`) under `mNpc_InitNpcAllInfo`, and
   `mSDI_StartInitNew` has no `mFRm_CheckSaveData()` gate — only
   `StartInitFrom`/`NewPlayer`/`Pak` do.

   ⚠️ **The old evidence could never have supported the old claim.** "Two 900 s
   runs printed zero `[DC/NPCTEX]`/`[DC/NPCMDL]` lines" was collected at the
   documented defaults `DC_NPCTEX_POOL=0 DC_NPCMDL_POOL=0`, where both files
   compile to empty stubs and the load seam is not inserted at all — silence
   was guaranteed either way. **A counter that cannot move is not a
   measurement.**

   **What is actually broken is ACTOR CONSTRUCTION, downstream of the list.**
   The S15 run had the pools ON, a 14-entry roster, and still printed zero
   `[DC/NPCTEX]`/`[DC/NPCMDL]` lines, i.e. `aNPC_dma_draw_data_proc`
   (`ac_npc_ctrl.c_inc:687`) never runs. **The open question is what walks
   `Common_Get(npclist)` and spawns NPC actors, and why it spawns none.**
2. 🔴 **The name-entry keyboard renders black. The fix is written, HAS NOW BEEN
   RUN (2026-08-09), and its PREDICATE IS TOO WIDE.** 18 of the keyboard's 26
   display lists are config **#037, class P3** — 27 of the 101 configs. The
   implementation is behind **`-DDC_PVR_TEVP3`, default OFF** (`dc/src/dc_pvr.c`,
   ~11 sites): vertex base colour `= PRIM − ENV`, per-vertex `oargb = ENV`,
   `PVR_TXRENV_MODULATE` + the offset-colour bit.
   🔴 **Switched on in a town build it printed `tevp3 batches=20305
   clamped=6941` and recoloured the town** — purple/pink ground, trunks and
   mailboxes — **in a run that never opened the keyboard.** 26 display lists in
   one widget cannot be 20,305 batches: the predicate is matching a shape #037
   merely shares, not #037. It also cost ~10 % (`us/v` 2.83 vs 2.57).
   **The maths in §6.6 is right; the recogniser is wrong. Fix the predicate
   first — log the combine words of the first N distinct matches — and only
   then take a screenshot pair.** `kb/tev-map-hard-cases.md` §6.6a.
   ⚠️ **Still untested: the panel is black but the 40 key caps are correctly
   coloured.** If the caps are black too, the whole diagnosis is wrong. Needs a
   run that actually reaches the name-entry screen — `DC_AUTOSTART` presses
   index 0 and never gets there (§8).
3. **The large black wedges are TEV config #007 losing BOTH alpha factors** —
   diagnosed, not fixed. `ef_shadow_out.c:34-35` records as **two** stages, not
   three; the batch draws at `vtx.a * T0.a`, where `vtx.a` is the
   `G_RM_FOG_SHADE_A` fog coefficient. The two levers are widening
   `tev_const_alpha()`'s A1 arm and adding the mirrored shape to
   `tex1_alpha_active()`; both are widenings, widenings in this family have
   regressed before, so **both need a screenshot pair**.
   ⚠️ `tev_const_alpha_last()` (`-DDC_PVR_NO_TEVALPHA_LAST`) does **not** fix
   this. Reference copy: `kb/tev-map-alpha.md` §5.6.
   **Note the relation to item 2: #007 loses both ALPHA factors, #037 loses both
   COLOUR constants.**
4. **The other 74 `obj_s_*` structures** — 333 KB, one building silhouette each.
5. ✅ **The winter time bomb is CLEARED on paper (2026-08-09), never verified.**
   R1 already made all 41 `mFM_grd_w_*` ground textures demand-loadable, and
   `keeplist-full.txt` now keeps **all 43 `obj_w_*.c` structure files** — they
   cost only 19,024 B after T1, which is why they are pinned first in
   `make_keeplist_town.py`'s `MODEL_PRIORITY_PREFIXES`. ⚠️ **Nobody has seen a
   winter town.** Verifying still needs a season-forcing build and **no such
   knob exists** — `DC_SEASON` has been written up as if it did.
6. **The station roof clip-through.** Reproducible for the first time —
   `DC_AUTOWALK` can walk a character under it, `DC_PVR_BATCH_LOG` prints
   `src=`/`vram=` so a batch joins to a symbol, and `-DDC_PVR_PT_NEAREST` is a
   one-line test of H2. Never run. `kb/station-bugs.md`.
7. **`aram zero=7 (2016 B)`** in the walking run; must be 0. Small, unexplained.

---

## 8. Two systemic blind spots in the census

⚠️ **A census can NEVER produce a town keep list.** `src/system/sys_math.c:7`
seeds the town from `sqrand(osGetCount())` and on DC `osGetCount()` is
boot-elapsed time, so **every boot lays out a different town**. The real
generator is `mRF_MakeRandomField` (`m_random_field.c:9`), which writes the
layout **into the save** — so the true reason is "per player", and the port
re-rolls per boot only because the VMU path is unwired.
⚠️ **`mFM_DecideAcre` DOES NOT EXIST.** Several kb files used to cite it.
`tools/dcstub/keeplist-town.txt` enumerates from the tree instead.

⚠️ **The census only ever sees the depth-0 branch of every decision.**
`DC_AUTOSTART` presses A, every choice menu defaults to index 0, and the clock
screen lives behind index **1** (`ac_npc_guide_move.c_inc:302-314`) — which is
why `tim_win.c` was missing for two sessions and a human had to report it.
Anything gated on `mEv_CheckFirstIntro() == FALSE` is invisible for the same
reason. **The mechanism is sound; its driver is the blind spot.** Two cheap
fixes are specced and unbuilt: a zero-asset detector at texture upload, and a
`DC_AUTOCHOICE` knob that presses Down-then-A on the Nth choice — **neither
exists in the tree today.**

⚠️ **Regenerating `keeplist-town.txt` once DELETED TOM NOOK.** 25 entries were
hand-typed and the generator never emitted them (the START map overlay, the
clock/date HUD, and `rcn/rcc/rcd/rcf/rcs/tuk _1`). They live in `EXTRA_SOURCES`
in `tools/dcstub/make_keeplist_town.py` now. **Verify the regenerated list is a
strict superset of what shipped whenever it is regenerated** — two lines of
python.

---

## 9. Renderer facts already settled — do not re-investigate

- **The recorded-but-never-consumed family.** Five bugs of one shape: GX state
  written by `dc_gx.c` and never read by `dc_pvr.c` (wrap mode, TEV constants,
  `GX_TEXMAP_NULL`, alpha compare, colour update; and the font's zeroed vertex
  colours). **Sweeping every `g_gx` field for a consumer is the standing
  technique**; fog was the largest one and is now read.
- **Punch-through.** `cxt.txr.env` was `PVR_TXRENV_MODULATEALPHA`, so the alpha
  the PT comparator tested was `vertex_alpha × texel_alpha` — but every
  alpha-tested display list runs `G_RM_FOG_SHADE_A`, where the vertex alpha byte
  is the **fog coefficient**. Fixed by forcing vertex alpha opaque on textured
  PT batches (`-DDC_PVR_PT_KEEP_VTXALPHA` reverts). **A/B settled: PT stays ON.**
- **Fog is hardware and the mapping is exact** — `PVR_FOG_TABLE` with a
  129-entry table derived in closed form. Vertex fog was never an option: KOS's
  `pvr_fog_vertex_color()` is an `assert_msg(0, "not implemented")` stub.
  `-DDC_PVR_NO_FOG`.
- **`pvr_dropped` has no speed mechanism.** `s_tris_dropped` (`dc_pvr.c:134`)
  fires only on near-plane geometry, so it tracks **where the camera is
  standing**. `run_report.py --vs` will keep calling it a REGRESSION and keep
  being wrong.
- **`GX_AF_SPOT` is unreachable in the whole tree** — `emu64.c:3316` gates it on
  `G_LIGHTING_POSITIONAL`, which nothing ever sets. The town runs **exactly 2 lights** (sun + moon, unconditional, in
  `m_kankyo.c`), not 8.
- **K.K.'s strum is slaved to the music** — `Na_StaffRollStart` latches on every
  BGM start, `Na_GetStaffRollInfo` returns `PART_START` forever when the
  sequencer does not tick, and `current_frame` then reads an uninitialised stack
  value. Fixed by the audio actually running.

---

## 10. Working hygiene

- **The main thread commits; agents do not run git.**
- ⚠️ **Never edit the tree while a build runs**, and never run two builds at
  once — killing a build mid-flight corrupts `objs.rsp`.
- ⚠️ **A short run is usually the human closing the emulator window, not a
  hang.** Ask before bisecting one.
- ⚠️ **A knob that `dc/build-dc.sh` does not FORWARD is silently off.** This is
  why G1 sat "in the tree, never run" for two sessions.
- **Every optimization gets a kill switch**, and the default is the good build.
