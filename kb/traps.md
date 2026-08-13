# Traps already paid for — do not re-discover these

Each entry cost real debugging time. **Read this before touching the build, the
harness, the prelude, or instrumentation.** These are mechanical gotchas, not
design decisions — for those see `kb/closed.md`.

## What Flycast does not model

The emulator is missing whole hardware mechanisms, and **its silence about one is
not evidence.** Before recording a refutation, ask what the instrument models.

- ⚠️ **NO PERFORMANCE COUNTERS — ALL ZERO, INCLUDING ELAPSED CYCLES
  (2026-08-08).** Measured twice over full runs: `perf_cntr_start(PRFC1, …)` +
  `perf_cntr_count(PRFC1)` returns **0 for all eight PMCR events**,
  `PMCR_ELAPSED_TIME_MODE` included. Not skewed — zero. A PMCR run in the
  emulator validates plumbing and nothing else. ⚠️ **A zero row is
  indistinguishable from "the build never armed"**, the failure mode that left G1
  unrun for two sessions, so `dc_pmcr.c` diagnoses itself: a zero elapsed-cycle
  window prints "PRFC1 is not counting … Burn it." Any instrument whose null
  result looks like a build mistake owes the reader the same line.
- ⚠️ **NO DISC TIMING. Never refute an I/O-timing hypothesis in it
  (2026-08-08).** `harness/dc/run-flycast.sh` passes `config:FastGDRomLoad=yes`;
  Flycast models **neither seek time nor transfer rate**, so `fs_read` is free.
  On 2026-08-06 "disc-cache misses cause the audio stutter" was recorded REFUTED
  on a Flycast A/B (ARAM cache 4 → 16 blocks: hit rate 83 → 97.9 %, disc reads
  3.54 → 0.77/s, stutter unchanged). On a real CD-R a seek is 100-200 ms *and
  then* the drive transfers at ~500 KB/s, against ~224 ms of audio cushion that
  nothing refills during a blocking read — the hypothesis was alive the whole
  time. Flycast is a fair proxy for loading *amounts* (hardware parity confirmed
  2026-08-04), **not** for loading *timing*.
- ⚠️ **UNDER-REPRODUCES THE AUDIO STUTTER BY ~10x (2026-08-06).** 15-16
  `[STUTTER]` events per 900 s in the emulator against **192 per 420 s on real
  hardware**. No audio verdict is final without a burn, in either direction.
- ⚠️ **NO ICACHE MODEL.** "It must be the cache" is never available as an
  explanation for a Flycast result, and a cache-layout win measured there is a
  floor, not the value.

## Measurement

**⭐ Split a bucket before optimising inside it — `xform` was 90 % not-maths
(2026-08-08).** `[PHASE] us/v` at 3.24 µs is **648 SH-4 cycles per vertex**
against roughly 60 cycles of vertex arithmetic, and a queue item
(`kb/research-sh4zam-gap.md` §0a) was about to rewrite six FIPRs as two FTRVs on
the assumption that the arithmetic was the cost. G5 (`-DDC_PVR_VTXSPLIT`) split
it: the FIPR block is **0.58 ms of a 30 ms frame**, the position FTRV **0.23 ms**,
and `memo` + `shade` + `emit` **5.86 ms** — `memo` at 122 cycles a vertex for a
hash and a 12-field compare is a cache miss, not maths. **Measurement rule 7 for
the third time** (`emu64_ms` per command, then `G_VTX`, now `xform`): an unsplit
bucket attracts optimisation aimed at whatever is easiest to imagine. **Split
first — G5 is ~40 lines and one sampled timer bracket per stage.**
⚠️ **Sample, do not bracket everything.** All seven stages of every vertex is
~19,000 timer reads a frame — at TMU2's ~80 ns, 1.5 ms of probe inside an 8.4 ms
measurement. One primitive in 16 costs ~450 reads and converges over a window.
Print the sample count so the scaling can be checked; 80 ns is a resolution floor,
so a `0.00` bucket means "below the noise", never "free".

**An average cost per command is not the cost of any command (2026-08-04,
corrected 2026-08-05).** `emu64_ms = 12.31 µs/cmd × cmds + 9.20 ms` (r = 0.954)
does NOT license pricing a SUBSET of commands at 12.31 µs: the fit is against
TOTAL `cmds`, which correlates with `vtx`, so the coefficient is dominated by
whichever opcode does the most work per command. Applied to the 2,094 state
commands per town frame it gives ~26 ms. ⚠️ **The fix made the same error again:**
"265 `G_VTX` carrying ~6,951 vertices at ~6.9 µs/vertex ≈ 48 ms" is wrong on both
inputs — that µs/vertex is itself a whole-command average, and the ~6,951 were
`GXPosition3f32` **references**, not loaded vertices (`G_VTX` loads ~3,601, the
rest are re-emissions). Measured: **`G_VTX` 5.40 ms over 149 calls;
`G_TRIN_INDEPEND` 22.25 ms over 146 calls, 63 % of dispatch.** A correction to an
averaging error must not itself be an average — `DC_EMU64_HIST` is the only thing
allowed to price an opcode.

**⭐ `[EMU64H]` is PER LOGIC TICK — double it (2026-08-06).** Every number G1
prints must be multiplied by `ticks_per_visual` before it can be compared with
`[PHASE] draw=`. G1 arms at the end of **every** tick (`dc_vi.c:405` frameskip,
`dc_vi.c:633` presented) and `s_frames` increments on every
`dc_emu64_hist_frame_close()`, so at `ticks_per_visual = 2` `[EMU64H] tot=` is a
**half-frame** figure: `tot 24.28 × 2 = 48.56` against `draw 45.6 + skip 2.9`.
**Two sessions quoted the halved numbers** — the `-O0` `G_TRIN_INDEPEND` was
44.5 ms of an 86.5 ms frame (51 %), not 22.25 of 78.3 (28 %). Measurement rule
**9**. ⚠️ **`probe=` is not subtracted from anything**: `dc_emu64_hist.c:300`
prints it and stops; it is inside `tot`, and the two probes per frame land inside
`gap` by construction. General rule: **an instrument sampling per logic tick and a
phase counter sampling per presented frame have different denominators**, and any
new counter must say which it is in its own output.

**⚠️ An instrument pointed at a subsystem that is not running measures the
subsystem not running (2026-08-06).** The `[DC/VOICE]` census reported the town at
0-4 concurrent voices and `filt=0 comb=0`, and two audio levers were declared dead
on that basis; both were retracted the same session because **the music was never
playing**. A voice count of zero and a voice count of "no sequence was ever
started" are the same number. **Establish that the thing being measured is RUNNING
before reading any counter about its cost**, and give every instrument a way to
report "idle" distinctly from "cheap".

**⚠️ A gate that cannot tell "the reference disagreed" from "the reference could
not answer" is not a gate (2026-08-08).** G3's gate (`-DDC_EMU64_CULL_VERIFY`)
never culls: it runs the entry test AND the original handler, then asks whether
`dc_gx.c`'s late cull agreed by watching `pc_gx_culled_draws` move. Two reachable
conditions stop that counter moving at all — `-DDC_NO_BATCH_CULL` compiles it to a
constant 0, and on a **frameskipped tick** `dc_gx_flush_vertices` returns at
`dc_gx.c:684` *before* the cull test — so every **correct** cull scores as a
`falsecull` and the gate reads as a catastrophe that is not happening. Fixed by
counting the unanswerable cases separately (`nocmp=`) and making the
`-DDC_NO_BATCH_CULL` combination a hard `#error`. **Enumerate the states in which
the ORACLE is silent, and make silence a third outcome rather than a failure.**

**`[DC/AUDIO]` mixes cumulative and windowed counters on one line (2026-08-06).**
`dc_audio.c:1031` resets **only** `s_pump_calls`, `s_pump_frames`,
`s_pump_budget_hits` and `s_pump_usec`, so `pump calls=` and `us/600=` are a
600-pump WINDOW while `cb=`, `pulled=`, `pollfail=` and `kick=` are **cumulative
since boot**. Dividing one by the other is wrong. **`synth_us` is a 3:1 EWMA
(`dc_audio.c:983`), not a mean**, understating the true mean
(`us/600 ÷ synth_frames`) by 22 %; using `sndf × synth_us` to "prove" a cost sat
outside synthesis is measurement rule 7 committed by the instrument itself. **An
EWMA cannot see a tail or a plateau** — the audio stutter was every call costing
4× the mean, which only a per-window MAX (`smax=`) could show.

**Cost a keep-list addition from TWO LINKS, never from summing arrays
(2026-08-06).** The gyroid set was costed at 155,360 B by summing its `Vtx`
arrays; the link said **432,160 B of span, 2.8× low**, because the files carry
textures and display lists too and keeping is per-FILE. Summing the arrays you
went looking for measures your search, not the image. Link with the addition, link
without, subtract `_end`.

**Measuring the image: the span is `_end - 0x8c010000`, NOT `sh-elf-size`'s `dec`
column.** `dec` sums section sizes: it omits every inter-section alignment gap and
counts `.ocram`, which is on-chip RAM at `0x7c001000` and not part of the image at
all. The two differ by hundreds of KB and move in opposite directions when section
sizes shift. **When it bit (2026-08-02):** a span was quoted as 18,997,600 from
`dec` when `_end` said 19,226,464, and the resulting "gap" was wrong by 228,864 B
in a document other work was planned against. `dec` is still right for "how many
bytes of section did this change remove" — P7 measured −246,064 by `dec` and
−238,048 by span. Say which one you mean.

**A full rebuild is 96 SECONDS (2026-08-06).** 3,926 TUs, `JOBS=8`, colima on an
M4; the `warnscan` variant is 132 s. Several plans in this kb are written as if a
rebuild were expensive enough to avoid — they were costing a guess.

**`shot_diff.py` cannot gate a change that alters the frame rate (2026-08-06).**
It scored an `-Os` build against an `-O0` build of the SAME source at 24-78 %
changed with every scene visually identical: the probe fires every N **presented**
frames and the game runs a variable number of logic ticks per presented frame, so
at probe index 60 the faster build is at a different point in the same camera pan.
Correct for a renderer change at a fixed frame rate; for an optimization change,
compare the SCENES by eye or fire the probe on a logic-tick count.

**Screenshots are the gate, not the counters (2026-08-03).** The alpha
texture-env fix (`-DDC_PVR_ALPHAENV`) came back with frames, deepest scene mode,
FPS, `ptdrop`, `LOST` and blank-texture count all within noise of its control —
and turned the train station canopy from textured beams into a flat teal slab.
**Judge a renderer change on a screenshot pair at the same probe index, always;**
`tools/dcqa/run_report.py --vs` is the *floor*, and it cannot see colour.
**Build the A/B out of ONE tree and ONE define** — both sides of that comparison
were the same commit differing only by `DC_XDEFS`; two separately edited trees
would have left the result arguable. **`DC_SCIF_FAST=1` is what makes this
affordable:** at 57,600 baud a 320x240 capture is ~35 s of wall clock, at
1,562,500 it is ~1.4 s, and a screenshot run at the default rate reaches a
fraction of the frames a plain run does, i.e. it is a different experiment.
⚠️ Emulator only — a real coder's cable will not sync at 1.5 Mbps.

## The console is an instrument that changes the run

KOS busy-waits on the SCIF TX FIFO **whether or not a cable is attached**, so
every logged byte is ~174 µs of dead time at the default 57,600 baud. Four
distinct traps come out of that.

**🔴 Muting the console at `main()` stops the game booting on hardware
(2026-08-08).** Paid for with a burn: `AC-DC-20260808f-pmcr.cdi` called
`dbgio_disable()` as the second statement of `main()`, and on the console the
PMCR table appeared and the game never did. The HUD is drawn from
`VIWaitForRetrace` (first reached from `sound_initial2()` inside `boot_main()`),
so a table on screen proves boot got *past* the asset load, *past* `pvr_init()`,
into game init — the failure is downstream of anything the boot log would show.
**The mechanism is timing and it is invisible in the emulator:** this port's boot
has always run with hundreds of milliseconds of SCIF delays in it, and removing
them at once changes the order things complete in during init. Flycast models
neither the FIFO timing nor, on that image, anything past the title (with no
`DC_AUTOSTART` it sits there forever), so the run that "passed" could not have
reached the failure. **Fix:** the mute is armed at `DC_CONSOLE_MUTE_FRAME`
**presented frames** (default 300, ~15 s) from `dc_console_mute_tick()`, inside
the game loop; `DC_CONSOLE_MUTE_FRAME=0` restores mute-at-`main()` and is kept
only to reproduce the failure.
⭐ **Generalise: a global change to how much the boot path BLOCKS is a change to
the boot path.** Anything that removes or adds waiting during init — console
rate, log volume, a sleep, a yield — must be armed after the game loop is up, or
proven on hardware.
⚠️ **An image whose only output channel is the screen must put a liveness line on
the screen.** That burn could not distinguish "the frame loop died" from "the loop
is alive and drawing nothing", so `dc_pmcr.c`'s HUD now leads with `f= t= d= c=`
(presented frames, logic ticks, GX draw calls, emu64 commands).

**⚠️ On hardware the console measures itself — mute it on a measuring burn
(2026-08-08).** `[PERF]`, `[PHASE]`, `[EMU64]`, `[EMU64C]`, five `[DC/PVR]`
lines, `[DC/TEX]`, `[DC/ARAM]` and every `[STUTTER]` all fire in **the same
30-frame window** — several hundred bytes, i.e. tens of milliseconds of stall
charged to the frames being reported. A logging burn measures the logging.
**`DC_CONSOLE_MUTE=1`** (`dc_main.c`, `dbgio_disable()` at the top of `main`)
removes all of it in one call: `DC_LOG`/`DC_LOGE`, the `printf`/`vprintf`
overrides, the game's `OSReport`, and anything KOS prints. Measured: 21 console
lines over a 240 s run, **0** `[PERF]`/`[PHASE]`/`[DC/PVR]`. ⚠️ It silences crash
dumps too — a triage burn must leave it off. Put the numbers on the TV instead
(`-DDC_PMCR_HUD`), and read the mute-at-`main()` entry above first.

**Any per-item log on a boot path is a hardware time bomb (2026-08-03).** The
per-asset `[DC/KEEP]` line printed 1,392 times = 86,357 B = **15.0 s of dead
boot** with nothing on screen — the exact window in which a human cannot tell
"loading" from "hung". The whole log was 51.4 s.

**`vprintf` is a third console sink, and it cost 8× the frame rate.**
- **A repeat-suppressor keyed on a table must REPLACE on miss, never give up when
  full.** The first `OSReport`/`printf` flood limiter was open-addressed and
  returned "print it" once every slot was taken; boot alone produces more than 32
  distinct call sites, so the table was full before the flood started and the
  limiter did nothing — 741 unsuppressed lines in the next run, with no symptom
  other than the flood it was written to stop. Direct-mapped with eviction is
  correct (`dc/src/dc_misc.c`).
- **`OSReport` is not the only sink.** `game64.c_inc`'s `[TRG_VOL]` and `[WALK]`
  lines call `printf` DIRECTLY and only become visible past the title screen, so
  a limiter written against the title-screen log looks complete and is not.
- **…and `printf` is not the only sink either.** emu64's `Printf0`
  (`emu64_print.cpp:18`) calls `vprintf` DIRECTLY, bypassing the `printf`
  override, gated on `g_pc_verbose` which `DC_ASSET_STUB` forces on
  (`dc_main.c:81`). In the town, `emu64.c:2690`'s
  `非シェアードの三角形群にシェアードの頂点が混ざっているので破綻しました!`
  fired **10,877 times in one 600 s run** — at 57,600 baud ~900 ms of every
  1-second frame. **The town ran at 1.1 FPS with `gx=35.1ms`: the renderer was
  4 % of the frame and the console was the rest.** Suppressing it: 10,877 → 18
  lines, **1.1 → 9.3 FPS**. If a scene is inexplicably slow and `gx=` does not
  account for it, count console lines before profiling anything else.
  ⚠️ After overriding `vprintf`, `dc_log_impl`/`dc_loge_impl` must call
  `vfprintf(stdout, …)` and the `printf` override must call `vfprintf` too —
  otherwise our own diagnostics get rate-limited (a suppressed `[DC/…]` line
  reads as "the thing did not happen") and `printf` charges every call site twice.

**`DC_LOG` is gated on verbose, NOT flood-limited.** ⚠️ Correction to a claim made
mid-session: `dc_misc.c:136` calls `vfprintf` directly, so the flood limiter never
touches it. It is gated on `g_pc_verbose`, which `DC_ASSET_STUB` forces on
(`dc_main.c:81`). If a `DC_TEX_LOG`/`DC_LOG` diagnostic prints nothing, the build
simply lacks the `-D`. Diagnostics behind their own flag should use `DC_LOGE` so
they are not double-gated.

## Instruments that install into emu64's dispatch table

**⚠️ The one that RE-installs must go first (2026-08-08).** G1
(`dc_emu64_hist.c:262`) and G2 (`dc_emu64_shadow.cpp:492`) `memcpy` **all 64**
dispatch slots on frame open and restore all 64 on close; G3
(`dc_emu64_cull.cpp`) installs **two** slots once at init and re-checks them per
frame. Run G3's frame-open *after* G1's and it overwrites G1's thunks for exactly
slots 59/60 — `G_TRIN` and `G_TRIN_INDEPEND`, the two opcodes G3 exists to fix —
so G1 reports `calls=0` for them AND silently bills their time to whichever opcode
ran before them, because `hist_enter()` charges elapsed time to `s_prev`
(`:125-131`). Order: **G3's frame-open first at both `dc_vi.c` sites, and
`dc_emu64_cull_init()` before both inits in `dc_main.c`**, so their snapshot
contains G3's trampolines. `reinst=` on the `[EMU64C]` line must read 0.

**Two instruments in the same table are not additive (2026-08-05).** G1
(`DC_EMU64_HIST`) and G2 (`DC_EMU64_SHADOW_LOOP`) both overwrite the table, and
building both produced a silently useless run: G2's trampolines replace G1's
thunks and G2's loop then calls `s_orig[]` directly, so G1 armed, cost a clock
read per command, and reported nothing. It is an `#error` now
(`dc/include/dc_platform.h:417`). **Any third installer has the same problem in a
worse form** (see the ordering rule above): a per-slot shadow must either arm
slot-wise or initialise strictly before them.

**An instrument's REPORT and its ARM SITES must live under the same guard.** G1's
`[EMU64H]` print was inside `#ifdef DC_PERF_PHASE` while the arming was not, so a
`DC_EMU64_HIST=1` build without `-DDC_PERF_PHASE` paid ~2,867 clock reads a frame
and printed nothing — indistinguishable from "the instrument is broken".

## Compile / link

- **`fsqrt` collision.** KOS `dc/fmath.h:109` defines
  `static inline float fsqrt(float)`; the decomp's `math64.h:34`
  `#define fsqrt(x) sqrtf(x)` rewrites KOS's *definition* into a static `sqrtf`
  that collides with newlib.
- **POSIX `link()` vs the decomp's `typedef struct link_ link`**, arriving via
  `<stdio.h>` → `<sys/stdio.h>` → `<unistd.h>`. A blanket `-Dlink=` does **not**
  work — it renames both sides. `dc/include/dc_prelude.h` renames only the POSIX
  declaration, then gives the identifier back.
- **`-fno-builtin` breaks the link** — see `kb/closed.md`.
- **3900 object paths exceed `execve`'s `ARG_MAX`.** The Makefile uses make's
  `$(file …)` to build a linker response file.
- **`char` is SIGNED by default** on this toolchain build, so `-fsigned-char` is
  belt-and-braces, not load-bearing.
- **`--wrap=<C name>` MATCHES NOTHING ON sh-elf, and ld does not tell you
  (2026-08-05).** This toolchain uses a leading-underscore user label prefix:
  `dc/build/dedup/syms.txt` has `8c35f384 00000318 T _mNpc_SetNpcList`, so
  `--wrap=mNpc_SetNpcList` wraps nothing — **silently**, because `--wrap` on an
  unknown symbol is not diagnosed. The build succeeds, the seam is absent, and
  the `__wrap_` function is dead code `--gc-sections` removes, so even a size
  check looks normal. Several kb proposals are written unprefixed
  (`kb/STATE.md`'s villager seam, `kb/research-ram-tiers.md`'s `--wrap=malloc`,
  the `--wrap=_RspStart2` audio probe). **Spell every `--wrap` with the
  underscore and verify with
  `sh-elf-nm dc/build/AnimalCrossing.elf | grep __wrap_`.**
- **⚠️ `--symbol-ordering-file` IS AN LLD FLAG; GNU ld wants
  `--section-ordering-file` (2026-08-08).** sh-elf uses GNU ld (2.45.1 in the SDK
  image): it has **`--section-ordering-file FILE`** and
  `--sort-section name|alignment`, and LLD's spelling is a hard error. The lever
  itself is real and free — `-ffunction-sections` is already in `GC_CFLAGS`.
  Check `sh-elf-ld --help` before costing layout work against a flag name from
  another toolchain.

**Per-TU make rules vs the scratch trees: a `$(OBJDIR)/src/…` per-TU rule stops
firing the moment a rewriter emits that source.** `stubify`/`shrinkify` change the
object path and make just skips the rule — no warning, no error. The two
`-Dmain=` renames are the dangerous instance, now written as
`$(OBJDIR)/$(call shrinkify,$(call stubify,src/main.c)).o`. **When it bit:**
`boot.c` moved into the stub tree, lost `-Dmain=boot_main`, and the image ended up
with two `main()`s; `-Wl,--allow-multiple-definition` (required for the 1,367
multiply-defined data symbols) swallowed the clash, the linker kept the wrong
`main`, and `--gc-sections` deleted everything the real entry chain reached —
`.text` 5,289,364 → 851,684, with `boot_main`/`ac_entry`/`graph_proc`/`mainproc`
absent from the ELF. **It linked, produced a CDI, and exited 0.** Detector, worth
running after any Makefile or rewriter change:
`sh-elf-nm build/AnimalCrossing.elf | grep -c ' _graph_proc$'` must be 1. Note the
**leading underscore** — `grep ' graph_proc$'` matches nothing even in a healthy
ELF and reads as the same failure.

**Killing a build mid-flight corrupts `objs.rsp` (2026-08-04).** `TaskStop` /
Ctrl-C during the link leaves `dc/build/objs.rsp` padded with NUL bytes, because
the link rule builds it with make's `$(file >>…)` one path at a time. The next
link reads a response file whose first ~1,500 bytes are `\0`, silently loses every
object those NULs replaced, and fails with **`undefined reference to 'main'`**
plus `__kos_romdisk` — i.e. it looks like `dc_main.c` vanished, not like a
truncated file. `od -c dc/build/objs.rsp | head` is the tell.
**`rm dc/build/objs.rsp` and relink.**

**The build tracks timestamps, not flags — FIXED, do not remove the fix.**
- **A flag change alone used to leave a stale image.** After a `DC_ASSET_STUB=1`
  build, a plain `bash dc/build-dc.sh` printed `make: Nothing to be done for
  'all'` and left the **stub** ELF in place, so `sh-elf-size` reported the stub's
  sections for what looked like a real build. Two causes: toggling the flag swaps
  2,521 sources for their stub twins and *both* sets of `.o` already exist and are
  older than the ELF, so nothing relinks; and any object whose source did not
  change keeps the `-D` set it was built with.
- **The fix is `dc/build/flags.stamp`**: it holds `DC_ASSET_STUB`, `DECOMP_OPT`,
  `DC_OPT` and `DEFINES`, is rewritten by `$(file …)` when any changes, and every
  object and the link depend on it. A flag change costs a full rebuild, the
  correct price.
- **`$(file …)` needs GNU make 4.0.** The container has 4.3; the macOS host has
  **3.81**, where `$(file …)` silently expands to nothing — which is why the stamp
  also has an ordinary recipe. The host only runs `make count` / `make sources`.
- **An `INCLUDES` change does not invalidate `flags.stamp`.** Adding
  `$(STUB_INCLUDES)` changed which `.c_inc` every kept TU sees and make rebuilt
  nothing — the link succeeded and the image silently still used the vendored
  copy. The `.d` files name the OLD path, so they cannot help. After changing an
  include path, delete the affected objects by hand.
- **When in doubt, `rm dc/build/AnimalCrossing.elf`** and re-link. A missing ELF
  cannot be stale.

## Burns and crash triage

**🔴 EVERY CRASH ON EVERY BURN THIS PROJECT EVER DID WAS UN-TRIAGEABLE, AND
NOBODY NOTICED (found and fixed 2026-08-09, S14).** `harness/dc/README.md`
§"ELF provenance sidecars" requires every CDI producer to write
`<image>.src.json` beside the image, and says in terms *"This binds
`dc/build-dc-docker.sh` too."* **It did not.** `dc/build-dc.sh` /
`dc/build-dc-docker.sh` wrote the CDI and stopped. A CDI holds a scrambled,
stripped `1ST_READ.BIN`, so a register dump out of one is hex; `crash.sh`
refuses to guess which ELF goes with an image, and refuses to symbolise against
one whose sha256 no longer matches — correctly, because a confidently wrong line
number is worse than no answer. So every burn-side crash report was dead on
arrival, silently, and the failure mode looks like "crash.sh is broken".

`dc/build-dc-docker.sh` now emits it. **The sha256 is the load-bearing field,
not the path** — `dc/build/AnimalCrossing.elf` is overwritten by the next build,
so a burn you keep needs its ELF kept beside it. **When you archive a CDI, copy
the `.elf` and the `.src.json` with it.**

⚠️ **General shape, and it is the third instance in this file: a rule written in
one document and never enforced by the code it binds.** Same as the
`DC_PERF_GXAPI` nesting below and the `DC_EMU64_HIST` forwarding trap. **If a
README says "every X must do Y", grep that every X actually does.**

## Knobs and flags

**🔴 AN INSTRUMENT WHOSE COUNT SITES AND PRINT SITE ARE GATED DIFFERENTLY ARMS,
PAYS FOR ITSELF, AND PRINTS NOTHING — AND `dc/src/dc_gx.c` HAS THIS BUG TODAY
(found 2026-08-09, S14; NOT FIXED).** The `#ifdef DC_PERF_PHASE` opened at
`dc_gx.c:141` does not close until `:296`, so the `DC_PERF_GXAPI` block
(`:162-172`) **and the whole `DC_PERF_GXSPLIT` block** (`:229-285`) are nested
inside it. Consequences, both silent until you hit them:

- `-DDC_PERF_GXAPI` **without** `-DDC_PERF_PHASE` fails to LINK — `dc_vi.c:188`
  externs `dc_gx_api_pos` and friends, which are then never defined.
- `-DDC_PERF_GXSPLIT=1` without it fails to COMPILE (`s_gxs_pos` is referenced
  in `dc_gx_frame_timing_snapshot`).
- ⚠️ **`dc_vi.c:199` explicitly documents the GXSPLIT block as being "in its OWN
  `#if`, not inside the DC_PERF_PHASE block". That is true of `dc_vi.c` and
  FALSE of `dc_gx.c`** — so the comment reads as a checked guarantee and is not
  one.

Same shape as the `DC_EMU64_HIST` trap below. **When adding a counter, put its
`#if` and its print site's `#if` on the same condition and check the nesting by
eye** — S14's `[GXVERIFY]` counters are in an independent `#if` for exactly this
reason. Line numbers here are pre-S14 and will drift; grep
`DC_PERF_GXAPI` and count the `#endif`s.

**⚠️ A knob that `dc/build-dc.sh` does not FORWARD is silently off (2026-08-04).**
`DC_EMU64_HIST` was never in the docker `-e` list, so G1 was unreachable from the
documented build line: `dc/Makefile` has `DC_EMU64_HIST ?= 0` and make only sees
what the container's environment carries, so `DC_EMU64_HIST=300 bash
dc/build-dc.sh` compiled the instrument out AND skipped the `objcopy` that
globalises the dispatch table, with no diagnostic. The run reached the town and
printed no `[EMU64H]` line, which reads as "the instrument is broken", not "it was
never built". **Any new `DC_*` knob needs a line in `ENVARGS`, and the check is
`tr ' ' '\n' < dc/build/flags.stamp | grep DC_YOURKNOB`.**
- ⚠️ **It must be the FORWARD-ONLY form**, `[ -n "${VAR+x}" ] && ENVARGS+=(-e
  VAR="$VAR")`, never a plain `-e VAR="${VAR:-}"`. The Makefile guard is
  `ifneq ($(VAR),0)` and **empty is not 0**, so an unset variable forwarded as
  empty turns the feature ON for every build. Same for knobs a *generator*
  consumes: `-e VAR=` expands `-DDC_NPCTEX_POOL=` into every TU, where the
  rewritten TUs' `#if defined(DC_NPCTEX_POOL) && !DC_NPCTEX_POOL` is a
  preprocessor error — a build failure, which is the good case; the bad case is
  the `ifneq` guard turning the feature on silently.
- **Verify the OBJECT, not just the exit code.** For a knob that gates a whole TU,
  `sh-elf-nm dc/build/obj/dc/src/<file>.o | grep -c <a symbol it defines>` must be
  non-zero. One G2 run was reported as "seems faster" before this check showed the
  shadow had never been compiled in — its FPS matched baseline because it *was*
  baseline.
- **`dc/build-dc.sh` and `dc/Makefile` each carry the knob's DEFAULT, and they
  drift (2026-08-05).** The script passes `--npctex-pool="${DC_NPCTEX_POOL:-0}"`
  to the generators while the Makefile has its own `DC_NPCTEX_POOL ?= 0` for the
  compile — two independent spellings of one default. They disagreed, caught only
  by an `#error` against a stale generated tree. **Change both, and give any
  generator-plus-compiler knob a hard `#error` on mismatch.**

**⭐ `DC_AUDIO=1` alone is SILENT — it needs `DC_ARAM_AUDIO_DROP=0`, and that was
never wired (2026-08-06, fixed 2026-08-08).** `DC_AUDIO=1` on its own produces a
live pipe carrying zeros and nothing warns you: `dc_aram.c:313-320` returns from
`aram_write()` *before* `dc_dvd_provenance()` at `:325` for every write below
`aram_audio_end`, so all 8,300,384 B of `audiorom.img` is streamed into ARAM and
discarded, the audio half of ARAM gets **zero extents**, and every jaudio sample
fetch lands on `dc_aram.c:401-409` and is `memset` to zero. `DC_ARAM_AUDIO_DROP=0`
was documented in `dc/Makefile:801` and `BUILDING-DC.md` as a manual `DC_XDEFS`
recipe and derived nowhere; it is derived from `DC_AUDIO=1` now, along with the two
peak-cost levers. **The tell is `[NEOS_OUT] … peak=0`** with the pump running and
the AICA pulling — `dc_audio.c:309-311` predicted this exact symptom in a comment.
Correct line: `ext=3/32`, `mapped=13282784`, `zero=0`, `peak != 0`.
⚠️ **`synth_us` measured on a silent run is meaningless** — synthesising zeros is
cheap. Two figures (1,353 and 3,208 µs) were quoted as "audio is affordable now"
before anyone checked `peak`.
⭐ **Rule: if a flag is mandatory for another flag to work, DERIVE it in the
Makefile with `?=`; a warning comment in two files is not wiring.**

**⚠️ `DC_OPT_O0_EXTRA` and `OPT_HOT_SRC` are SPACE-separated (2026-08-06).**
`DC_STUB_KEEP` is colon-separated and the habit carries over. A colon-joined list
here is taken as **one filename**, and `dc/Makefile:1214` correctly refuses the
build — but the message reads *"names 1 file(s) this build does not compile:"*
followed by the entire list, which looks like the list is wrong rather than the
separator. Read the "1".

**A test knob that fires in the wrong scene corrupts the run (2026-08-04).**
`DC_AUTOWALK` started at a fixed `PADRead` call number and drove the NAME-ENTRY
KEYBOARD cursor: garbage name, confirm prompt declined, run looped in the intro
forever. It happened to work on the run before, which is worse than failing every
time. Now gated on `sou_scene_mode` (`DC_AUTOWALK_SCENE`, default 9 = the town),
and the leg counter does not advance until the gate opens.
`u8 sou_scene_mode` (`game64.c_inc:504`) is an ordinary non-static global —
`8c900888 B _sou_scene_mode` in the linked ELF — so `dc/` can read the live scene
with no `src/` edit and no interposition. **A reusable seam.**

## Counting things in display lists — two ways to be wrong (2026-08-05)

- **`gsDPLoadTextureBlock_4b_Dolphin` expands to TWO `Gfx`, not one.** It is a
  comma pair at `include/libforest/gbi_extensions.h:1133`, and there are **1,619**
  of them in `src/data/npc/model/mdl/` alone. **Any tool that sizes a display list
  by counting macros is wrong by that much**, systematically in one direction.
- **A `gsSPNTriangles_5b` packet's top byte is vertex-index data, and it reads as
  `G_VTX` (0x01)** whenever `v11 == 0` and `v10` is 4..7 — ordinary geometry, not
  a corner case. **A display-list walker hunting for `G_VTX` will corrupt
  geometry.** This is why R3 patches from a generated table rather than walking
  display lists at runtime.
- The N-triangle **face count** in `G_TRIN_INDEPEND` is **7 bits** —
  `emu64.c:4814` is `n_faces = ((w0 >> 17) & 0x7F) + 1`, i.e. 1..128 faces. The
  "5 bits" everyone quotes is the per-vertex **index** width (`POLY_5b`,
  `gbi_extensions.h:64,69-86`), which caps a batch at 32 distinct source vertices.
  Both true; not the same fact.

## Renderer

- **KOS's `PVR_CULLING_CW`/`CCW` are DETERMINANT SIGNS, not winding names you can
  reason about in NDC.** `pvr_header.h:77-82`: `CCW = 2` = cull if the
  screen-space determinant is negative, `CW = 3` = cull if positive. Both GL and
  PVR name their modes in terms of the DISPLAYED image, so `emit_projected`
  negating Y on the way to screen coordinates **does not** need compensating for —
  do that and you have flipped it twice. **When it bit (2026-08-02):** every
  character in the train intro rendered inside-out ("everyone is standing
  backwards"), hidden for two sessions because the logo overlay draws with
  `GX_CULL_NONE`. Mapping is `GX_CULL_FRONT -> PVR_CULLING_CCW`,
  `GX_CULL_BACK -> PVR_CULLING_CW`; `-DDC_PVR_CULL_INVERT` restores the old one,
  `-DDC_PVR_NO_CULL` separates "culling is the axis" from "culling is irrelevant".
- **A comment that contradicts the code five lines below it will be believed.**
  `dc_pvr.c`'s viewport comment said "Y-down, same as GX, so there is no flip"
  while `emit_projected` right below it computed `cy - hh * y`. The cull bug above
  was derived from the comment, not the code.
- **The "recorded and never consumed" class has bitten FOUR times. Grep the
  CONSUMER, not the field, before believing any GX state is handled.** All four
  are `dc_gx.c` storing and `dc_pvr.c` ignoring:
  - **`TEXOBJ_WRAP_S/T` (fixed 2026-08-02).** Stored since M1 with
    `GXGetTexObjWrapS/T` exposed, so a grep for "wrap" finds a plumbed-looking
    wrap mode. Nothing read it — `dc_pvr.c` hardcoded
    `cxt.txr.uv_clamp = PVR_UVCLAMP_NONE`, so **every texture in the port
    repeated, `GX_CLAMP` included.** Symptom: the opening's spotlight cone drawn
    2.7 times across the frame at a fixed 117 px pitch, hard vertical seam on the
    tile boundary and the real 12 px falloff on the other edge. **A periodic seam
    is the signature; a clamped texture cannot produce one.** `-DDC_PVR_NO_UVCLAMP`
    reverts.
  - **`tev_colors[]`** (fixed 2026-08-02).
  - **`alpha_comp0/ref0/op/comp1/ref1`** and
    **`color_update_enable`/`alpha_update_enable`** (both still open as of
    2026-08-02). `GXSetAlphaCompare` has a five-field setter, a dedup path and a
    `DIRTY(DC_GX_DIRTY_ALPHA_CMP)` — it looks completely plumbed, yet
    `grep alpha_comp0 dc/src/dc_pvr.c` returns nothing, and 23 of the 101 TEV
    configs ask for an alpha test. **What it looks like:** alpha-tested cutout
    geometry (foliage, fences, hair) draws its fully-transparent texels at full
    opacity with `zw=1`, writing depth and occluding what is behind them
    ("textures are not layered properly"). The PT list is also compiled out
    (`p.opb_sizes[4] = PVR_BINSIZE_0`). Fix sketch in `kb/tev-map-alpha.md`.
- **`AA_ZB_TEX_EDGE2` is OPAQUE-WITH-HOLES, not foliage.** The obvious move —
  "alpha-tested geometry is see-through, so stop it writing depth" — breaks solid
  objects: the train door frame and leaf (`obj_romtrain_door.c:44,71`) and the
  tunnel (`rom_train_out.c:135`) are walls with alpha edges. With one
  submission-ordered list and autosort off, a batch that writes no depth is painted
  over by **everything submitted after it**, and all XLU window scenery is
  submitted after all OPA geometry. **When it bit (2026-08-02):** the passing trees
  and clouds drew through the closed train door. Split on what the game asked for:
  `GX_BM_NONE` + alpha test = opaque with holes, keep `depth.write`;
  `GX_BM_BLEND` + alpha test = real translucent cutout, drop it. ⚠️ That split is
  still not correct, and cannot be — see the next entry.
- **Without a real alpha test there is NO right answer for a punched hole, and both
  wrong answers have been observed.** `alpha_ref` (144 by default, `emu64.c:718`)
  is read only to detect that a test exists, never applied as a threshold, because
  the PVR has no alpha test outside the punch-through list. For a door with
  alpha-punched windows: `depth.write=true` makes the transparent holes write depth
  and **occlude the scenery behind them** ("the windows are missing");
  `depth.write=false` makes the door **fail to occlude anything** ("the trees draw
  through the door"). Both were tried on 2026-08-02 and both reported broken — do
  not oscillate. The fix is `PVR_LIST_PT_POLY`; `kb/RESUME.md` item 1 carries the
  constraints (PT is list 4, i.e. LAST, so cutouts must be buffered until the TR
  list closes).
- **Destination alpha does not exist (2026-08-02).** `GX_BL_DSTALPHA` /
  `GX_BL_INVDSTALPHA` must NOT map literally to `PVR_BLEND_DESTALPHA` /
  `PVR_BLEND_INVDESTALPHA`: **KOS renders into RGB565, so there is no stored
  destination alpha and the hardware reads 1.0.** emu64 uses the N64 two-pass
  memory-alpha decal idiom for every ground shadow (`emu64.c:2289`/`:2291`, RDP
  side `m_rcp.c:131`): pass A writes alpha with colour update off, pass B blends
  against it. With DESTALPHA reading 1.0, pass B collapses to `src*1 + dst*0` and
  the shadow paints **opaque** — presented as "the train station is very broken on
  the title screen with missing textures"; the textures were fine, a navy slab was
  painted over them. Substituting SOURCE alpha is exact, because both passes draw
  the same geometry with the same texture and prim alpha. Kill switch
  `-DDC_PVR_KEEP_DSTALPHA`.
- **A draw that binds no texture still got one.** `dc_pvr.c` bound
  `g_gx.tex_handle[0]` unconditionally and never consulted `tev_stages[0].tex_map`,
  while nothing ever clears that handle. The whole JSystem 2D path sets
  `GX_TEXMAP_NULL` + `GXSetNumTexGens(0)` (`J2DGrafContext.cpp:29-31`), and
  `GXPosition3f32` resets texcoord to (0,0) per vertex, so those panes sampled
  texel (0,0) of whatever emu64 last bound and `MODULATEALPHA` multiplied it into
  colour *and alpha*: an opaque-black texel blacked the pane out, a zero-alpha
  texel erased it, decided by that frame's draw order. Letterbox bars, dialogue
  frames, fade quads. Fixed 2026-08-02; `-DDC_PVR_NO_TEXNULL` reverts. ⚠️ Suppress
  the bind ONLY on an explicit `GX_TEXMAP_NULL` — `g_gx` is zero-initialised and
  `tex_map == 0` is `GX_TEXMAP0`, the "nobody called `GXSetTevOrder` yet" default,
  which must keep its texture.
- **libforest's `TEV_*` constants alias `GXTevColorArg` ON PURPOSE, and one alias
  is a trap.** `emu64.c:1423` casts the N64 combiner argument straight to
  `GXTevColorArg` because the tables line up: `TEV_PRIMITIVE` 4 == `GX_CC_C1`,
  `TEV_ENVIRONMENT` 6 == `GX_CC_C2`, `TEV_TEXEL0` 8 == `GX_CC_TEXC`, `TEV_SHADE`
  10 == `GX_CC_RASC` (`include/libforest/gbi_extensions.h:156-167`), and emu64
  writes the matching registers at `emu64.c:3171,3180`. **But `TEV_COMBINED` is 0
  and so is `GX_CC_CPREV`**, and they do NOT mean the same thing — `COMBINED` is
  the previous cycle's result, not a constant. Code that treats `GX_CC_CPREV` as a
  constant register silently blacks out the ~245 `(0, 0, 0, COMBINED)` cycle-0
  draws in `src/data/model/`. `tev_creg_of` in `dc_pvr.c` excludes it for this
  reason.
- **Wrap belongs to the BIND, not the upload.** `dc_pvr_texture.c` keys its cache
  on texel content, so one VRAM image is legitimately shared by GXTexObjs that wrap
  differently. Both `header_key()` in `dc_pvr.c` and the `dc_gx_state_dedup`
  early-return in `GXLoadTexObj` must include the wrap mode, or the second binding
  silently keeps the first one's header.

**The store queues have exactly one owner, and it is the open list.**
`emit_projected()` writes TA vertices through `pvr_dr_target()` /
`pvr_dr_commit()` (G-C, 2026-08-08). Four properties make that legal, all of them
someone else's code:
1. **`pvr_list_begin()` is what sets QACR** — it calls
   `sq_lock((void *)PVR_TA_INPUT)`, `pvr_list_finish()` calls `sq_unlock()`
   (`pvr_scene.c:198`, `:230`). DR is valid only *between* those two calls, which
   is exactly the window `emit_projected()` runs in.
2. **Only on the non-DMA arm.** `pvr_list_begin` takes the `sq_lock` path only when
   the list is not in DMA mode. This build sets `p.dma_enabled = 0`; flip that and
   the QACR setup silently disappears.
3. **`sq_flush()` clobbers `"memory"`** (`arch/sq.h:112-118`) — load-bearing and
   invisible: the eight field stores go through a pointer derived from an integer
   global, and an `asm volatile` *without* a memory clobber orders only against
   other volatile asm. Lose it and GCC may sink the stores past the `pref`, handing
   the TA the PREVIOUS primitive's 32 bytes — garbage geometry, no crash, no
   warning.
4. **QACR dies if the MMU is ever turned on** (`kb/research-mmu-paging.md`). MMU
   paging is already dead, but `dc/src/` now *depends* on that.

⚠️ **Do not "simplify" the punch-through producer to use DR.** A PT record is built
while the GENERAL list is open and must be held until list 4 can legally be opened;
the queue at that moment points at a different list. The replay loop in
`dc_gx_backend_frame_end()` could use DR — ~96 records a frame, not worth it.

**A per-vertex predicate is not a saving, however exact it is.** Session 12
replaced three int→float converts in `shade_vertex()` with a predicate that skips
them: exact, firing on essentially every lit vertex (`shade_a8 verts=12,543,600`),
and it **cost more than it saved** — `shade=` fell 0.15 ms while `xform − sum` rose
0.27 ms and `us/v` did not move. The predicate read three `g_gx` fields and
branched, **per vertex**, to avoid three converts per vertex; that moves work, it
does not remove it. All three fields are per-BATCH constants — every writer of
`chan_ctrl_*` calls `dc_gx_flush_if_begin_complete()` first
(`dc_gx.c:1879,1911,1935,1947,2065`).
⚠️ **AMENDED 2026-08-09 — the hoist this rule prescribed was measured and is
NEUTRAL, and the shortcuts behind it are still negative** (`shade_batch_mode()`,
`-DDC_PVR_NO_SHADE_HOIST`: hoist alone `us/v` 2.65 → 2.68; shortcuts on top
`shade` 1.82 → 1.89). **Why: with the shortcuts off, `need_rgb`/`need_a` were
compile-time CONSTANTS and GCC straight-lined the block; hoisting made them runtime
bitmask loads, i.e. a real branch with both arms emitted.** So: **hoisting a
predicate out of a loop buys nothing if it was already a constant IN the loop** —
check what the compiler already knows before moving the test. Settled-negative in
`kb/closed.md`.

## Assets, the stub tree and the keep list

**A KEEP-LIST ENTRY NO LONGER MEANS "RESIDENT" (2026-08-09, T1).** R1/R2/R3 each
switched a class off by removing whole FILES from the list, so "on the list ⇒
full size" held for two months. T1 cannot work that way — a texture array
normally shares its TU with the vertex arrays and display lists that must stay —
so `make_stub_data.py` grew a `DEMAND_STUB` set that `keep_symbol()` consults
and that **beats a keep-list entry at the symbol level**. Three consequences,
all of which bit during the implementation:
- the partial-keep path, which `keeplist-town.txt` never used (it carries **no**
  `#` prefix filters at all, despite `partial_file()`'s docstring describing the
  `#!obj_w_` season filter as if it were live), went from dead code to handling
  ~500 files in one commit;
- `emit_keep_inc()`'s symbol filter used to run only `if prefixes:`. Leaving it
  there would have emitted a loader call for a demand-stubbed array — a
  full-size `memcpy` into a `[1]` destination, which is the NEUTRALISE overrun
  class above, in a new place;
- the `.c_inc` rewrite loop lived only in the whole-file arm, so pushing files
  into the partial arm silently stopped rewriting their `.c_inc`. That is the
  reply-box bug (§ below) exactly. It is hoisted above the fork now.

**`gSPSegment`'s argument is not always a symbol — 24 textures are reached
through a POINTER TABLE (2026-08-09).** `TEXPOOL_SEGMENT_RE` matched only a
symbol written literally as the third argument, so `m_design_ovl.c`'s six
`tool_*_table[]` and `ac_shrine_draw.c_inc`'s `leaf_texture_table[]` slipped
past the exclusion and were demand-loaded. They happened to still work, because
every anime-segment placeholder in `gbi_extensions.h:36-47` is
`SEGMENT_ADDR(SEG, 0)` — **offset zero**, so the address the PVR is handed
equals the symbol's own. ⚠️ **That is a property of the game data, not of the
loader, and the failure it guards is the silent-and-pretty one**: a non-zero
offset would miss the lookup, fall back to hashing a one-byte array, and draw
whatever the linker put next. Fixed generally by
`scan_pointer_table_symbols()` — any eligible symbol appearing as a bare
element of an all-identifier brace initialiser is treated as segment-reached.
**Generalise: an exclusion regex that matches a CALL SITE only covers the
literal spelling; the indirect spelling is the one that gets away.**

**A BUDGET THAT LANDS 784 BYTES UNDER THE CEILING IS A COINCIDENCE, NOT A FIT
(2026-08-09).** The first `--full-model` keep list was sized by arithmetic to
1,300,000 B and linked to `_end - 0x8c010000 = 11,012,828` against a derived
ceiling of `11,013,612`. It is tempting to call that a pass. It is not: the
ceiling's libc-peak term was measured on 2026-08-04 against a much smaller keep
list and has never been re-derived (`kb/STATE.md` says so), and rule 6 already
says `MEMLEDGER FIT ... OK` is not a statement that the image boots. Budget cut
to 900,000 for ~460 KB of real margin. **Raise it against an OOM pair, never
against arithmetic.**

**`DC_ASSET_STUB` corrupts `.bss` unless every full-size pass is neutralised, not
just `pc_assets_init()`.** Shrinking a destination array does not shrink the loops
that write it: their bounds are compiled-in constants. `boot.c` runs four
endian-fixup passes immediately before the `HotStartEntry` loop and all four
overrun — `pc_bswap_house_pos_list()` writes 0x978 B into a `u8[1]` (2,423 B
over), `pc_bswap_u8_tlut_palettes()` 14 × 32 B into `u8[1]` (434 B),
`pc_bswap_raw_display_lists()` 112 B into three `u8[1]` (109 B), and
`mFM_InitActableEndian()` walks six actables looking for a sentinel that no longer
exists, so it is unbounded. **Symptom, measured:** `boot.c`'s own `HotStartEntry`
came back as `0x64b3418c`, the game jumped to it and died on an illegal
instruction at `PC=65000004`; it read as "the renderer broke the boot". The victim
symbol is ~3,000 B from the arrays being swapped and **moves whenever `.bss`
moves**, so the same bug is a silent hang in one build and a wild jump in the
next. Fixed by the `NEUTRALISE` table in `tools/dcstub/make_stub_data.py`, which
rewrites the four call sites under `#ifndef DC_ASSET_STUB` with an anchored,
hard-erroring match count. **Any new full-size pass over asset arrays needs an
entry there.** Corollary: in a stub image, a crash whose address changes when
unrelated `.bss` changes is an overrun — look for a loop bound that survived the
stubbing.

**A stubbed acre loses its VERTICES, not its textures (2026-08-04).** An unkept
`src/data/field/bg/acre/*` file renders NOTHING and it looks like a texture bug:
the acre `.c` stubs its vertex array under `TARGET_PC` — `grd_s_t_st1_2.c:15-16`
is `static Vtx grd_s_t_st1_2_v[0xF00 / sizeof(Vtx)]` — while the `Gfx` display
list is initialised data and is NOT stubbed. The list executes normally against
all-zero vertices, every triangle collapses to the origin, and the acre
contributes no pixels. Same shape for `src/data/model/obj_s_*` and for NPC models:
**Tom Nook rendered as a black spiky mess**, the same defect seen from the inside.
Two sessions went into `kb/station-bugs.md` §1's ground-texture indirection — a
real bug, and fixed — while most of the town was missing for this other reason.

**A census can NEVER produce a correct town keep list.** `src/system/sys_math.c:7`
seeds the town from `sqrand(osGetCount())`, and on DC `osGetCount()` is
boot-elapsed time — **every boot lays out a different town**, so `DC_ASSET_CENSUS`
names the acres ONE run happened to visit and a keep list built from it is wrong
for the next. `kb/station-bugs.md` §1 noticed the symptom on 2026-08-02 ("town
layout randomises the station column, so keep all three") without drawing the
general conclusion. `tools/dcstub/keeplist-town.txt` enumerates from the tree;
`keeplist-opening.txt` stays the censused list for title-screen and size work, and
the wide list is a UNION with it.

**On a `DC_ASSET_STUB` image, a missing TEXTURE looks exactly like a renderer
bug.** A stubbed texture array is `[1]` bytes, so its texels AND its palette read
as zeros; the decoder faithfully produces a fully transparent rectangle and the
geometry draws as a black silhouette. Nothing errors, nothing is rejected, and
`[DC/TEX] uploads/hits/evictions` all look healthy — the upload succeeded, it was
just an upload of nothing. **Before debugging a black model, run `DC_TEX_LOG=1`
and check `nonzero=`.** 2026-08-02: 77 of 117 uploads were blank, read for a whole
session as "the animal textures regressed"; the animals had never been kept.

**The census only ever sees the depth-0 branch (2026-08-04).** `DC_ASSET_CENSUS`
is sound; its DRIVER is the blind spot — `DC_AUTOSTART` presses A, every choice
menu defaults to index 0, and anything behind index 1 is invisible. That is why
`src/data/model/tim_win.c`, the whole clock/date screen, was missing for two
sessions until a human reported it: Rover's "is that right?" prompt puts the clock
behind `mChoice_CHOICE1` (`ac_npc_guide_move.c_inc:302-314`). Ruled out first
against the artifacts: not a capped table (`CENSUS SUM … overflow=0 full=0`), not
a bypassed draw path, and not "the run never got that far" (the census contains
`nam_win_*`/`mra_win_*`, which the guide opens *after* the clock). Same shape:
anything gated on `mEv_CheckFirstIntro() == FALSE` — post-intro HUD, START
inventory, START map, NPC spawning — is invisible to every census taken so far.
**`ASSET MISSING` does not cover this class:** it fires only when a *kept* asset
fails to load from disc (`pc/src/pc_assets.c:93`), a stubbed array is silently
zero, and there is no runtime detector for "a zero-filled asset was drawn" in
`dc/`. Building one is cheap and unbuilt — `dc_gx.c` mirrors the source pointer
into `g_gx.tex_obj_src[]`, so `census_resolve.py` would symbolise it for free.

**`.c_inc` files are invisible to the stub tooling, and the failure is silent.**
`make_stub_data.py` globs `*.c`, so a TU whose asset arrays and `_pc_load_src_*()`
loader live in an `#include`d `.c_inc` never enters `stub.list`, and
`census_keeplist.py` then dropped its symbols for "not being stubbable". Under
`DC_ASSET_STUB` that is the worst outcome available: the arrays are correctly
sized in `.bss`, nothing fills them, and the map looks right. `src/game/m_msg.c` →
`m_msg_data.c_inc` exposed it — the balloon behind every line of NPC dialogue was
missing. **The arrays are `static`**, so `dc_stub_keep.inc` cannot load them
directly (tried: eight undefined references at link); they must be filled from
inside the TU, i.e. the `.c_inc` gets `keep_file()`'d and shadowed on the include
path (`-I$(STUBDIR)/include`, the mechanism `DC_SRC_SHRINK` already uses).

**…and the `.c_inc` trap has a SECOND half, which cost the reply box
(2026-08-03).** `cinc_includes()` taught `make_stub_data.py` about
`.c_inc`-resident assets, but that handling sits **below** an early `continue`
that asks the wrong file: `if "#ifdef TARGET_PC" not in text: continue` tests the
**`.c`** and skips the whole TU when it has none — exactly the shape of a TU that
keeps *all* its asset code in the `.c_inc`. `src/game/m_choice.c` has zero
`TARGET_PC` guards and is the **only** one of the 193 keep-list entries with that
shape; `src/game/m_msg.c` survived the first fix only because it carries one.
**Symptom:** `[PC] ASSET MISSING: assets/con_waku_swaku3_tex.bin` and
`con_sentaku2_v.bin` — the choice window's only texture and its four vertices — so
the reply box was a transparent texture on a degenerate quad stretched into a pale
haze over the train interior ("the reply text boxes are messed up"). **Why it hid
for two days:** `dc_stub_keep.inc` declared *and called*
`_pc_load_src_game_m_choice_draw_c_inc()` either way, so the generated header
looked complete. That mismatch is now a hard error — a `.c_inc` loader call may
only be emitted for a `.c_inc` this run actually rewrote. **`grep 'ASSET MISSING'
<run>/console.log` must come back empty; the cheapest asset-side health check
there is.**

**Demand-loading a `bcopy` source — two things that bit R1 (2026-08-05).**
- **The game over-reads its own asset array, and the correct fix is to reproduce
  the over-read.** `l_bg_tex_common_dummy[15]` is a 2,048 B destination whose
  source `mFM_grd_s_beach_tex` is **1,024 B** (`pc_assets.c:22791`), so vanilla
  `bcopy`s 1,024 B past the end of the source on every call — on GameCube and on
  the PC port alike. A loader that reads the SOURCE size fills half the slot and
  the beach ground goes wrong; one that reads the **DEST** size reproduces the
  hardware byte-for-byte. R1 reads the dest size and logs the mismatch. **Any
  future demand-load of a `bcopy` source has to check for this shape.**
- **A demand load turns one resident array into a scattered disc seek, and the
  COUNT is what matters, not the payload.** R1 moved 27 `fs_seek`+`fs_read` pairs
  into `mFM_FieldInit`, and the same loop runs mid-scene on the island boat trip
  (`m_field_make.c:1745,1754`, from `ac_boat_demo_move.c_inc:92-102`). The payload
  is 33,632 B — ~67 ms at 500 KB/s — but `dc_main.c`'s sweep model prices a
  *scattered* seek at 20-100 ms, so 27 could be **0.5-2.7 s** [UNMEASURED]. Fix
  pattern: sort the requests and replay them through one window, i.e.
  `dc_keep_sweep()`.

**A two-pass traversal of the keep list SILENTLY DROPS ASSETS.** "Call
`dc_stub_keep_load()` once to count, once to record" is wrong: some rewritten
loaders keep the generator's load-once guard — `src/furniture/ac_radio_test.c` has
`static int radio_pal_loaded` — so the second traversal skips them. Record on the
single pass.

**⚠️ `MEMLEDGER FIT … OK` does not mean the image boots (2026-08-04).** `margin=`
IS libc's pool, and the ledger has no model of libc's demand. A wide-keep-list
build printed `MEMLEDGER FIT image_span=12681100 additive_heap=2358752
margin=1606292 **OK**` and then died on the splash at `trademark_init` with
`Out of memory. Requested sbrk_base 8d0be000, was 8cf5c000, diff 1449984`. `OK`
means the static side fits and nothing more. The pair of runs gives the number
that matters: libc peak ≈ margin + shortfall = 3,056,276, against a 3,202,932
margin on the build that boots, i.e. **~146 KB of real headroom, not 3.2 MB.**
`kb/heap-two-pools.md`.

**Census plumbing.**
- **Never union address ranges measured on a stub build.** Display lists are
  initialised `Gfx[]` data and are NOT stubbed, so
  `gsSPVertex(&obj_train1_1_v[93], 15, 0)` carries a real offset and count into a
  16-byte array: every read runs far past its own symbol and over its neighbours.
  Coalescing those spans merged unrelated arrays wholesale — 665,136 component
  reads collapsed to **ten** ranges, an undercount of unknown size with every
  identity but the first lost. Record contiguous *batches* keyed on base instead;
  the count comes from the display list and is real either way.
- **Interior pointers break nearest-symbol resolution.** `census_resolve.py` joins
  batches against the ~11,789 literal `gsSPVertex` sites in `src/` on
  `nm[symbol] + byte_offset == base`, an exact 32-bit equality.
- **A `[1]`-sized stub build still censuses correctly.** The addresses the GX layer
  is handed are link-time constants, so `DC_ASSET_CENSUS` names the same symbols in
  a stub image as in a full one; only the *sizes* are wrong.
  `tools/dcstub/census_resolve.py --sizes-from <full ELF>` turns that into real
  bytes — quoting the stub column as a working-set total understates it by ~20 %.
- **Never gate a periodic probe on `pc_frame_counter`.** `dc_vi.c`'s retrace
  handler returns early on every frameskipped tick — *after* incrementing
  `pc_frame_counter` — so a `pc_frame_counter % N == 0` test is evaluated only on
  presented frames, at counter values that jump by the skip factor. **When it
  bit:** a run that presented 1,769 frames fired the arena probe three times, all
  inside the first two seconds, then never again. Both probes now share one local
  `probe_tick` incremented where they are called.
- **A counter that only counts the failure you thought of proves nothing.** The
  ARAM pager's small-read fast path (`dc_aram.c`, `len <= ARAM_BLK`) `memset` a
  32 KB block to zero, called `dc_dvd_pager_read`, **ignored the return value**,
  bumped `c_r_disc++` (the SUCCESS counter) unconditionally, and cached the block
  as authoritative — so a failed or short read published ZEROS as real content
  while `zero=0` in the `[DC/ARAM] LRU` line, the exact counter you would check to
  rule it out. The slow path had the same shape in weaker form:
  `if (dc_dvd_pager_read(...) < 0)` treats a short read or `0` as success.
  `dc_dvd_pager_read` returns **bytes read** (`dc_dvd.c:228`), so the test must be
  `got != (int)n`. Both fixed 2026-08-02; the fast path now frees the block so the
  next read retries, and logs `SHORT READ`. ⚠️ Note *which* reads take the fast
  path: `len <= 32768`, so a silent zero-fill lands on exactly the message/string
  TABLE reads (64 B, `m_msg_main.c_inc:289`) and string bodies (~64-128 B) and
  never on the bulk archive reads — which is why "the dialogue body renders but the
  speaker name and the reply do not" was reachable with every pager counter looking
  clean. ⚠️ **It was NOT the cause of that symptom** — the fixed build reports
  `SHORT READ = 0`. Real bug, wrong suspect; the missing name/reply text is still
  open (`kb/RESUME.md` item 4).

## Disc and boot on real hardware (2026-08-03)

- **`pvr_init()` blanks the screen, and it used to run before the asset load.** It
  reprograms the display controller at its own buffers, so everything after it
  draws on black until the game's first frame. Splitting it out as
  `dc_gx_backend_start()` and calling it after the load turns the gap into a
  loading screen. The GX *state machine* still has to exist before the game's
  first GX call — boot-order rule 4 — but `pvr_init()` does not.
- **DMA is already in use for every disc read.** `fs_iso9660.c:279,829` pass
  `dma = true`; `CDROM_READ_PIO`/`CDROM_READ_DMA` are deprecated compat constants
  and `cdrom_read_sectors()` (no `_ex`) is not used by the VFS. Do not spend a
  session "switching to DMA".
- **KOS already does read-ahead, twice.** A 16 × 2048 B LRU sector cache per stream
  (`fs_iso9660.c:211`) and a drive-level `cdrom_stream_start` to end-of-file on any
  sector-aligned read (`:755-777`). The `3 × 128 KB` ring in `dc_dvd.c`'s TODO
  would duplicate it for 393,216 B. **What KOS cannot fix is request ORDER** — that
  is what `DC_KEEP_SWEEP` addresses.
- **An unaligned read costs two GD-ROM commands, not one.** A read that does not
  start on a 2048-byte boundary makes KOS serve the leading fragment through its
  single-sector cache, which itself calls `iso_abort_stream`. Every pager read of
  `forest_*.arc` is unaligned by construction — the RARC `dataoff` values are 1120
  and 1920, neither divisible by 2048.
- **`DC_CDI_PAD=1` does NOT push content to the outer edge.** Measured: padded and
  unpadded images put the archives at identical LBAs with a constant 4-sector
  delta; all ~684 MB of padding is appended AFTER the filesystem, so every game
  byte is on the innermost ~10 % either way. The old comment in
  `dc/build-dc-docker.sh` claimed otherwise and has been corrected.
- **`PADRead` samples once per LOGIC TICK, and the game's gates are EDGES.** At
  Flycast's 22-30 FPS a human tap always straddles a sample; at hardware's ~11 FPS
  (91 ms period) it can fall entirely between two and produce no edge.
  `DC_PAD_NO_LATCH` turns off the 60 Hz accumulator that fixes this. Related and
  free: `chkButton(BUTTON_L)` auto-advances dialogue under `TARGET_PC`
  (`m_msg_normal.c_inc:4`), so **holding the left trigger is a no-rebuild test for
  whether input reaches the game at all.**

## Harness / emulator

- **A short run is usually the HUMAN closing the emulator window, not a hang.** A
  479-frame run (vs 10,199 the run before) was diagnosed as an audio-thread
  deadlock and a kill-switch bisect built, then the user said they had closed it by
  accident. A run that stops mid-log with no crash dump and no `[DC/...]` error is
  ambiguous — **ask before bisecting.**
- **Say what an A/B build will break BEFORE handing it over.** A build made with
  `-DDC_PVR_NO_UVCLAMP` to test the train windows also turns off the fix that
  repaired K.K. Slider's spotlight the previous session, reported back as "kk
  slider is messed up major, regression" — correct behaviour for that switch,
  wasted round trip. Name the expected collateral in the same message as the build.
- **The harness writes `console.log` only when Flycast EXITS (2026-08-05).**
  Polling the run directory mid-run finds no log — or a stale one from the previous
  run — which reads exactly like a hang. **A 900 s run takes ~17-20 min of wall
  clock; wait for it.** To check a run is alive, check that the Flycast process
  still exists, not that the log has grown.
- **⚠️ `pgrep -f Flycast` matches your own wait loop (2026-08-08).**
  `until ! pgrep -f Flycast; do sleep 10; done` never exits — the shell running it
  has "Flycast" in its own command line. Match the binary path:
  `pgrep -f "MacOS/Flycast"`.
- **`-c config:LimitFPS=no` unlocks the frame limiter.** `harness/dc/smoke.sh`
  passes `-c` straight through to Flycast (`smoke.sh:97`). This is the user's own
  play-testing setting and gets far more game per wall-clock second.
- **The town is ~4,000 frames in — use `--timeout 600`.** A 240 s run stops in the
  train intro and will make you think progression regressed.
- **Build to a COPY of the CDI before a long run.** Flycast holds
  `dc/build/OpenCrossing.cdi` open for the whole run, so the next build cannot land
  while it plays. `cp` to the scratchpad and run the copy; builds and runs then
  overlap instead of serialising into 20-minute cycles.
- **Guest `scif_flush()` permanently kills the Flycast console. Never call it.**
  KOS's flush clears TEND and spins; Flycast never re-raises TEND on an idle TX
  FIFO; KOS latches `serial_enabled = 0`; a later crash then prints **nothing**.
  Bisected across 7 guest variants — raising baud is fine, the flush is the killer.
- **KOS 2.3 assertion text** is `*** ASSERTION FAILURE ***` / capital-A
  `Assertion "x" failed`. The documented lowercase regex never matched, so a failed
  `assert()` only ever surfaced as a timeout.
- **mkdcdisc padding**: default 740,083,145 B / 15.6 s vs `-N` 1,783,337 B /
  0.021 s. Use `-N` for every emulator run; `DC_CDI_PAD=1` only for burns and
  read-speed-realistic timing.
- **A smoke run of the game "fails" by construction.** The game never returns, so
  `run_reached_end_marker` / `mark_boot_ok` / `end_rc_zero` can never hold and
  `harness/dc/smoke.sh` exits 1 with `status=exited_early` even on a perfect run.
  For game images the console log is the artefact; read `[PERF]`, `[DC/PVR]` and
  the probe lines, not the exit code. The PASS/FAIL gate is meaningful for
  `selftest.cdi` and for anything that terminates.

**Framebuffer probing in Flycast.**
- **A framebuffer HASH is not a framebuffer TEST — count nonzero pixels.**
  `FBHASH bae41dc5` looks like a result and is the FNV-1a of 614,400 zero bytes.
  Two runs showing two different hashes were briefly read as "the framebuffer works
  now"; adding `FBNONZERO <n> of 307200` showed n = 0 every time. Any probe that
  reports a digest must report a population count next to it.
- **`config:rend.EmulateFramebuffer=yes` (`harness/dc/smoke.sh --fb-writeback`) is
  REQUIRED for any guest-side framebuffer read in Flycast.** ⚠️ This bullet
  previously said the opposite — that the flag "does NOT by itself make the guest
  see pixels" — and that was **falsified 2026-08-02** by an A/B on one image:
  without it every candidate surface reads `0 of 307200`; with it,
  `13711 of 307200`. The earlier negative was reached by reading the wrong address
  *and* omitting the flag, so neither variable was isolated. Its frame-rate cost is
  **unmeasured**: the old "24.8 → 16.8 FPS" did not reproduce (25.0 with, 16.5
  without) and no controlled pair exists, because every run dies at a different
  point in the title demo.
- **Flycast's 32-bit VRAM aperture repeats every 4 MB.** A sweep of all 8 MB
  reports each block twice, at N and N+64. That mirroring is what cross-checks a
  32-bit block index against its 64-bit counterpart (×2); otherwise it reads as
  twice as much resident data as exists.
- **`vram_s` is not the displayed surface once `pvr_init()` has run.** The PVR
  allocates its own buffers inside VRAM and programs the display controller at
  them: `PVR_FB_R_SOF1` (0xA05F8050) read **0x000E7480**, 947,840 bytes in, while
  the probe was hashing offset 0. Read the scanout register; never assume the
  framebuffer is at the base of VRAM. `SOF1` page-flips between `0x000e7480` and
  `0x004e7480`.
- **KOS's `pvr_get_front_buffer()` is not a usable framebuffer address.** It
  returns `addr * 2 + PVR_RAM_BASE`, mixing a 64-bit-area offset with the
  32-bit-area base; on the second buffer it points off the end of VRAM entirely.
  Use `0xA5000000 + FB_R_SOF1`.
- **"Hot VRAM blocks" are not evidence of a rendered frame.** A sweep that flags a
  64 KB block on one nonzero word counts the guest's own texture uploads. Reading
  "hot blocks grow 3 → 12 → 20 over a run" as writeback was exactly this mistake;
  the ten blocks the framebuffer occupies were empty the whole time.
- **A 16×12 thumbnail must box-filter, not point-sample.** The title logo covers a
  few per cent of a 640×480 frame, so a grid of 192 single pixels can report an
  all-black thumb off a frame that is not black. `dc_pvr_fb_probe()` averages whole
  cells now.

## Docker / SDK image

- **`bash -lc` inside the SDK image** re-runs `/etc/profile`, which drops
  `/opt/toolchains/dc/sh-elf/bin` from PATH. `sh-elf-addr2line` then vanishes and
  every address silently symbolises to `??`. **Use `bash -c`.**
- **Sourcing `environ.sh` under `set -u`** exits 127 with nothing on stderr.
- **Host has no BuildKit** — `DOCKER_BUILDKIT=0`, never pass `--progress`.
  `--platform linux/arm64` is not optional; without it an amd64 pull drops the
  build into qemu.
- **Do not rebuild `opencrossing-dc:sdk`** — ~27 min cold. It is already in the
  local Docker daemon.
- **"KOS 2.3" IS NOT A RELEASE, and treating it as one will send you to the wrong
  source (2026-08-05).** `include/kos/version.h` says 2.3.0 and every document in
  this tree calls the pinned SDK "KOS 2.3", but `git describe` on the pinned
  `KOS_SHA=1c6398f9` gives **`v2.2.0-946-g1c6398f9`** and the tags stop at v2.2.2 —
  2.3.0 is the in-development version number on master, not a tagged release. Two
  APIs this port cares about are **master-only and absent from every release tag**:
  `pvr_dr_addr` and `dcache_toggle_ocram()`. Release-tag docs and tarballs will
  disagree with the SDK image in both directions. **Read the pinned tree, never a
  release.** (`kb/toolchain.md`.)

## Disc content and the scratch-tree mechanism

- **`mkdcdisc -d DIR` puts DIR ITSELF on the disc**, so files land at
  `/cd/DIR/name` and every `DVDFastOpen` misses with no diagnostic. The flag you
  want is **`-D`** (contents, excluding the root). `dc_dvd.c:113` builds every path
  as `"/cd" + "/" + name`, flat, with no subdirectory.
- **Colima does not share `/private/tmp` with the VM.** A `-v` bind mount of a path
  under it is silently EMPTY inside the container — the build printed "0 files" and
  carried on. Stage disc content somewhere under `$HOME`.
- **`"${ARR[@]}"` on an empty array is an unbound-variable error under `set -u` in
  bash 3.2**, the macOS system bash. Every `dc/build-dc.sh` run without
  `DC_DISC_ROOT` died on it. Use `${ARR[@]+"${ARR[@]}"}`.
- **A quoted `#include` resolves against the INCLUDING FILE'S directory first**, so
  an `-I` shadow of a header in `include/` can never reach a consumer that pulls it
  in via a *sibling header* in `include/`. MEASURED: a shadow of
  `include/ac_structure.h` reaches `src/actor/ac_structure.c` but **not**
  `src/actor/npc/ac_npc.c` — a half-applied shadow, i.e. a silent ODR split that
  `--allow-multiple-definition` will not complain about. Header shadows are only
  safe for headers included directly by the TUs you care about; otherwise use a
  per-TU source swap confined to one TU, plus a compile-time assert pinning the
  unshrunk `sizeof`. `tools/dcstub/make_src_shrink.py` is built around this.
- **Every rewrite rule must hard-error on no-match.** A regex that silently matches
  nothing produces a build that looks fine and saves nothing, or worse, shrinks one
  of two consumers.

## Agent hygiene

- **Agents must not run git.** The main thread commits.
- **One build at a time.** `dc/build/` is a single shared object tree; two
  concurrent `make` runs corrupt it. Investigation agents get read-only
  `sh-elf-nm`/`objdump`/`readelf` over `docker run`, never `make`.
- **Never build while an agent has the tree.** Two builds in one session silently
  included another agent's in-flight edits to `dc_gx.c`/`dc_vi.c`, one of which had
  broken the `DC_FB_PROBE` hook — so every screenshot run returned zero framebuffer
  output and an "A/B" was never testing what it claimed. Concurrent `make` in
  `dc/build` also produced an `ld` bus error and a `flags.stamp` that reverted to
  another session's `DEFINES`, giving an image whose `DC_MAIN_MEMORY_SIZE`
  disagreed with its ledger and aborted at boot. **Verification builds go in a
  detached worktree at committed HEAD**, and ⚠️ **that worktree must live under
  `$HOME`** — colima cannot bind-mount `/private/tmp`, and a build from there fails
  with `bash: /work/dc/build-dc-docker.sh: No such file or directory`.
- **Always give absolute paths in scripts** — agents run from varying cwds.

## `--wrap` on sh-elf: the underscore theory is wrong, and it fails green

`--wrap=_Sym` with an `__asm__("__wrap__Sym")` label **matches nothing**. bfd
strips the `_` prefix itself, so `--wrap` on a name that already carries it is
ignored — with no diagnostic — the wrapper is left unreferenced, and
`--gc-sections` deletes it along with its `__real_*` reference so not even an
undefined symbol appears. The build is green and the interposition never
happened.

```
link:  -Wl,--wrap=Sym          NO leading underscore
code:  void __wrap_Sym(...)    plain C name, NO asm label
```

Proof that it bound: `___wrap_Sym` survives `--gc-sections` in the linked ELF.
Always ship a runtime counter too. Measured over a four-way matrix in the SDK
image, 2026-08-13; `kb/audio-aica-runtime.md` §3.
⚠️ `dc/src/dc_npctex.c`'s "WHY NOT `--wrap`" note and
`tools/dcstub/make_src_shrink.py:879` both assert the wrong version.

## `DEFINES :=` at dc/Makefile:599 discards every `+=` above it

A knob block placed earlier in the file builds, links and silently compiles its
feature out. Put `DEFINES +=` blocks BELOW that line. (Link-flag variables are
fine anywhere — they are consumed at the link recipe.)

## Never touch the store queues from the audio path — it corrupts the PICTURE

`spu_memload_sq()` → `sq_cpy()` → `sq_lock()` repoints **QACR0/QACR1** and
writes through **SQ0/SQ1**. `pvr_list_begin()` has already locked the SQ for the
TA, and `pvr_dr_target()`/`pvr_dr_commit()` alternate those same two queues
inside that window — so an audio-side SQ copy hands the TA its bytes as a
vertex. Reachable from anywhere, because `dc_audio_disc_yield()` fires from
inside any blocking disc read. Use `spu_memload()` (plain `g2_write_block_32`).
An audio-only change presenting as garbled geometry is the signature.

## Disc I/O from the audio path can re-enter fs_iso9660

`dc_audio_disc_yield()` runs the audio pump from *inside* somebody else's
blocking `fs_read`. Any read issued from there re-enters the CD driver with the
outer request in flight; nothing asserts and the OUTER read returns wrong bytes,
so the symptom is an asset that loaded "successfully" and draws as garbage.
`dc_audio_in_disc_yield` (dc_audio.c) is the guard flag.
