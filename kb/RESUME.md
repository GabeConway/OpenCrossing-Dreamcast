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

⭐⭐⭐ **AND SINCE THE S14 BURN (2026-08-09) IT RUNS BETTER ON HARDWARE THAN THE
EMULATOR MEASURED.** Human verdict on `AC-DC-20260809b.cdi`: *"definitely runs
better on real hardware"*, *"sound is perfect, no skipping"* — against
`[STUTTER] 65 / 900 s` in Flycast, which had scored the same batch as a **wash**
(`us/v` 2.51 → 2.48, inside the noise floor). **That is measurement rule 12**:
four of S14's seven changes pay only in cache misses and Flycast models no
cache, so its number was the floor, not the result. `kb/batch-s14.md` §7.

⚠️ **This SUPERSEDES the older verdict** *"on hardware the game runs super
stable, fps and audio is worse for sure … the emulator runs buttery smooth"* —
that was the pre-S14 build. The only hardware FPS FIGURE the project has ever
had is still the old "~11 FPS in the town": **the new verdict is a direction,
not a magnitude, and nothing has attributed it to a specific change.**
`AC-DC-20260809c-nof5.cdi` isolates F5 (same objects, only the link order
differs); `AC-DC-20260809a-pmcr.cdi` gives `istall`. **Every FPS number in this
kb is still Flycast's, and Flycast models no instruction cache, no operand cache
and no disc seek time.** See §6.

Current numbers: `kb/STATE.md`.

---

## 2. The build lines

### The shipping config

```bash
DC_STUB_KEEP="$(grep -v '^#' tools/dcstub/keeplist-town.txt | paste -sd: -)" \
DC_DISC_ROOT=~/.cache/oc-dc-discroot DC_ASSET_STUB=1 \
DC_ARAM_WINDOW=1048576 DC_ARENA_BYTES=1200000 \
DC_AUDIO_SCENES=all DC_AUDIO_DISC_FRAMES=8 DC_AUDIO_VOICES=12 \
DC_CDI_PAD=1 bash dc/build-dc.sh
```

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
| **burn** | drop `DC_SCIF_FAST`, `DC_AUTOSTART`, `DC_AUTOWALK` and every probe; keep `DC_CDI_PAD=1` | burn the CDI |

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
    bucket within 0.03 ms) and came back from a burned CD-R as *"definitely runs
    better on real hardware"* with *"sound perfect, no skipping"* against
    `[STUTTER] 65 / 900 s` in the emulator. Flycast models **no instruction
    cache and no operand cache**, so a change made of locality is invisible
    there BY CONSTRUCTION. **The emulator can falsify an instruction-count
    claim; it can NEVER falsify a locality claim** — and the converse trap is
    that a Flycast *win* on such a change is understated, not absent.
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

## 7. Still broken, ranked

1. 🔴 **THE TOWN HAS NO VILLAGERS, and it is a save bug, not an asset bug.**
   `mNpc_SetNpcList` populates the town from the save's `Animal_c animals[]`
   (`m_start_data_init.c:559`); the VMU path is unwired, so `[PC] No save file
   found` and **not one villager actor is ever constructed**. Measured: two
   900 s runs that reach scene 9 and walk printed zero
   `[DC/NPCTEX]`/`[DC/NPCMDL]` lines, because `aNPC_dma_draw_data_proc`
   (`ac_npc_ctrl.c_inc:687`) never runs. **R2/R3 are DEFAULTED OFF for exactly
   this reason.** N2b is the prerequisite for testing either.
2. 🔴 **The name-entry keyboard renders black — a whole TEV class, and the fix
   is WRITTEN BUT NEVER RUN.** 18 of its 26 display lists are config **#037,
   class P3**, which is **27 of the 101 configs**. The implementation exists
   behind **`-DDC_PVR_TEVP3`, default OFF** (`dc/src/dc_pvr.c`, ~11 sites):
   vertex base colour `= PRIM − ENV`, per-vertex `oargb = ENV`,
   `PVR_TXRENV_MODULATE` + the offset-colour bit, with `oargb` carried through
   the clipper and the vertex memo exactly like `argb`. With the switch off
   `emit_projected()` sets `oargb = 0` and every P3 batch loses both colour
   constants. **Compile-verified, never run on a frame** — it handles 9 of the
   27 P3 configs exactly.
   ⚠️ **Free falsification before any screenshot: `[DC/PVR] tevp3 batches=0` on
   a run that reaches the keyboard** kills the diagnosis for nothing.
   ⚠️ **Second falsifiable prediction: the panel is black but the 40 key caps
   are correctly coloured.** If the caps are black too, the diagnosis is wrong.
   `kb/tev-map-hard-cases.md` §6.6.
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
5. ⚠️ **The winter time bomb is REDUCED, NOT CLEARED.** R1 made all 41
   `mFM_grd_w_*` ground textures demand-loadable, but the 84 `obj_w_*`
   structures are still stubbed, so a winter town still draws every building as
   a black spiky mess. Verifying it needs a season-forcing build, and **no such
   knob exists yet** — `DC_SEASON` has been written up as if it did.
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
