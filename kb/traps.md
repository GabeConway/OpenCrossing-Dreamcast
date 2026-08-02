# Traps already paid for — do not re-discover these

Each one cost real debugging time. **Read this before touching the build, the
harness, or the prelude.** These are mechanical gotchas, not design decisions —
for those see `kb/closed.md`.

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
- **`char` is SIGNED by default** on this toolchain build, so `-fsigned-char`
  is belt-and-braces, not load-bearing.

## `DC_ASSET_STUB` — full-size passes over `[1]`-sized destinations

- **The stub build corrupts `.bss` unless every full-size pass is neutralised,
  not just `pc_assets_init()`.** Shrinking a destination array does not shrink
  the loops that write it: their bounds are compiled-in constants. `boot.c`
  runs four endian-fixup passes immediately before the `HotStartEntry` loop and
  all four overrun —
  `pc_bswap_house_pos_list()` writes 0x978 B into a `u8[1]` (2,423 B over),
  `pc_bswap_u8_tlut_palettes()` 14 × 32 B into `u8[1]` (434 B),
  `pc_bswap_raw_display_lists()` 112 B into three `u8[1]` (109 B), and
  `mFM_InitActableEndian()` walks six actables looking for a sentinel that no
  longer exists, so it is unbounded.
  **Symptom, measured:** `boot.c`'s own `HotStartEntry` came back as
  `0x64b3418c`, the game jumped to it and died on an illegal instruction at
  `PC=65000004`. The victim symbol is ~3,000 B from the arrays being swapped
  and **moves whenever `.bss` moves**, so the same bug presents as a silent
  hang in one build and a wild jump in the next. Before this was found it read
  as "the renderer broke the boot".
  Fixed by the `NEUTRALISE` table in `tools/dcstub/make_stub_data.py`, which
  rewrites the four call sites under `#ifndef DC_ASSET_STUB` with an anchored,
  hard-erroring match count. **Any new full-size pass over asset arrays needs
  an entry there.**
- Corollary for debugging: in a `DC_ASSET_STUB` image, a crash whose address
  changes when unrelated `.bss` changes is an overrun, not a logic bug. Look
  for a loop bound that survived the stubbing.

## Per-TU make rules vs the scratch trees

- **A `$(OBJDIR)/src/…` per-TU rule stops firing the moment a rewriter emits
  that source.** `stubify`/`shrinkify` change the object path, and make just
  skips the rule — no warning, no error. The two `-Dmain=` renames are the
  dangerous instance, and they are now written as
  `$(OBJDIR)/$(call shrinkify,$(call stubify,src/main.c)).o`.
  **What it looked like when it bit:** `boot.c` moved into the stub tree, lost
  `-Dmain=boot_main`, and the image ended up with two `main()`s.
  `-Wl,--allow-multiple-definition` (required for the 1,367 multiply-defined
  data symbols) swallowed the clash, the linker kept the wrong `main`, and
  `--gc-sections` then deleted everything the real entry chain reached: `.text`
  5,289,364 → 851,684, and `boot_main`/`ac_entry`/`graph_proc`/`mainproc` were
  simply absent from the ELF. **It linked, produced a CDI, and exited 0.**
  Cheap detector, worth running after any Makefile or rewriter change:
  `sh-elf-nm build/AnimalCrossing.elf | grep -c ' _graph_proc$'` must be 1.
  Note the **leading underscore** — this toolchain prefixes every C symbol, so
  `grep ' graph_proc$'` matches nothing even in a healthy ELF and reads as the
  same failure.

## Measuring the image

- **The image span is `_end - 0x8c010000`. It is NOT `sh-elf-size`'s `dec`
  column.** `dec` sums the section sizes: it omits every inter-section
  alignment gap and it counts `.ocram`, which is on-chip RAM at `0x7c001000`
  and is not part of the image at all. The two differ by hundreds of KB and
  they move in opposite directions when section sizes shift. **What it looked
  like when it bit (2026-08-02):** a span was quoted as 18,997,600 from the
  `dec` column when `_end` said 19,226,464, and the resulting "gap" was wrong
  by 228,864 B in a document that other work was being planned against.
- Related: `dec` is still the right number for "how many bytes of section did
  this change remove" — P7 measured −246,064 by `dec` and −238,048 by span,
  and both are correct answers to different questions. Say which one you mean.

## Renderer

- **KOS's `PVR_CULLING_CW`/`CCW` are DETERMINANT SIGNS, not winding names you
  can reason about in NDC.** `pvr_header.h:77-82`: `CCW = 2` = cull if the
  screen-space determinant is negative, `CW = 3` = cull if positive. Both GL
  and PVR name their modes in terms of the DISPLAYED image, so the fact that
  `emit_projected` negates Y on the way to screen coordinates **does not** need
  compensating for in the cull mapping — do that and you have flipped it twice.
  **What it looked like when it bit (2026-08-02):** every character in the train
  intro rendered inside-out — a human watching Flycast reported "everyone is
  standing backwards". The title screen looked perfect the whole time, which is
  what hid it for two sessions: the logo overlay draws with `GX_CULL_NONE`.
  The mapping is `GX_CULL_FRONT -> PVR_CULLING_CCW`, `GX_CULL_BACK ->
  PVR_CULLING_CW`; `-DDC_PVR_CULL_INVERT` restores the old one and
  `-DDC_PVR_NO_CULL` separates "culling is the axis" from "culling is
  irrelevant" in one build.
- **The "recorded and never consumed" class has now bitten FOUR times. Grep the
  consumer before believing any GX state is handled.** The four, all in
  `dc_gx.c` storing and `dc_pvr.c` ignoring: `TEXOBJ_WRAP_S/T` (fixed
  2026-08-02), `tev_colors[]` (fixed 2026-08-02), **`alpha_comp0/ref0/op/
  comp1/ref1`** and **`color_update_enable`/`alpha_update_enable`** (both still
  open as of 2026-08-02). `GXSetAlphaCompare` has a five-field setter, a
  dedup path and a `DIRTY(DC_GX_DIRTY_ALPHA_CMP)` — it looks completely plumbed,
  and `dc_gx.c:1195-1198` even calls the mapping "the most consequential single
  mapping decision in the renderer". `grep alpha_comp0 dc/src/dc_pvr.c` returns
  nothing. 23 of the 101 TEV configs ask for an alpha test; all are dropped.
  **What it looks like:** alpha-tested cutout geometry (foliage, fences, hair)
  has its fully-transparent texels drawn at full opacity with `zw=1`, so they
  write depth and occlude whatever is behind them — reported by a human as
  "textures are not layered properly". The PT list is also compiled out
  (`p.opb_sizes[4] = PVR_BINSIZE_0`), so there is currently nowhere for
  punch-through geometry to go. Fix sketch in `kb/tev-map-alpha.md`.
- **`AA_ZB_TEX_EDGE2` is OPAQUE-WITH-HOLES, not foliage.** When implementing the
  alpha test, the obvious move — "alpha-tested geometry is see-through, so stop
  it writing depth" — breaks solid objects. The train door frame and leaf
  (`obj_romtrain_door.c:44,71`) and the tunnel (`rom_train_out.c:135`) all use
  `AA_ZB_TEX_EDGE2`; they are walls with alpha edges. With one
  submission-ordered list and autosort off, a batch that writes no depth is
  painted over by **everything submitted after it**, and all XLU window scenery
  is submitted after all OPA geometry. **What it looked like when it bit
  (2026-08-02):** the passing trees and clouds drew straight through the closed
  train door. Split on what the game asked for: `GX_BM_NONE` + alpha test =
  opaque with holes, keep `depth.write`; `GX_BM_BLEND` + alpha test = real
  translucent cutout, drop it.
  ⚠️ **That split is still not correct, and cannot be** — see the next entry.
- **Without a real alpha test there is NO right answer for a punched hole, and
  both wrong answers have been observed.** `alpha_ref` (144 by default,
  `emu64.c:718`) is read only to detect that a test exists, never applied as a
  threshold, because the PVR has no alpha test outside the punch-through list.
  For a door with alpha-punched window openings: `depth.write=true` makes the
  transparent holes write depth and **occlude the scenery behind them**
  ("the windows are missing"); `depth.write=false` makes the door **fail to
  occlude anything** ("the trees draw through the door"). Do not oscillate
  between these two — both were tried on 2026-08-02 and both were reported
  broken by a human. The fix is `PVR_LIST_PT_POLY`; `kb/RESUME.md` item 1
  carries the constraints (PT is list 4, i.e. LAST, so cutouts must be buffered
  until the TR list closes).
- **A draw that binds no texture still got one.** `dc_pvr.c` bound
  `g_gx.tex_handle[0]` unconditionally and never consulted
  `tev_stages[0].tex_map`, while nothing ever clears that handle. The whole
  JSystem 2D path sets `GX_TEXMAP_NULL` + `GXSetNumTexGens(0)`
  (`J2DGrafContext.cpp:29-31`), and `GXPosition3f32` resets texcoord to (0,0)
  per vertex, so those panes sampled texel (0,0) of whatever emu64 last bound
  and `MODULATEALPHA` multiplied it into colour *and alpha*: an opaque-black
  texel blacked the pane out, a zero-alpha texel erased it, and which one you
  got depended on that frame's draw order. Letterbox bars, dialogue frames and
  fade quads. Fixed 2026-08-02; `-DDC_PVR_NO_TEXNULL` reverts.
  ⚠️ Suppress the bind ONLY on an explicit `GX_TEXMAP_NULL` — `g_gx` is
  zero-initialised and `tex_map == 0` is `GX_TEXMAP0`, i.e. the "nobody called
  `GXSetTevOrder` yet" default, which must keep its texture.
- **State that is recorded and never consumed reads exactly like state that is
  handled.** `dc_gx.c` has stored `TEXOBJ_WRAP_S/T` since M1 and exposes
  `GXGetTexObjWrapS/T`, so a grep for "wrap" finds a plumbed-looking wrap mode.
  Nothing read it: `dc_pvr.c` hardcoded `cxt.txr.uv_clamp = PVR_UVCLAMP_NONE`,
  so **every texture in the port repeated, `GX_CLAMP` included**, until
  2026-08-02. Grep for the *consumer*, not the field.
  **What it looked like when it bit:** the opening's spotlight cone drawn 2.7
  times across the frame — one shape at a fixed 117 px pitch, hard vertical
  seam on one edge (the tile boundary, no gradient) and the real 12 px texture
  falloff on the other. A periodic seam is the signature; a clamped texture
  cannot produce one. `-DDC_PVR_NO_UVCLAMP` restores the old behaviour.
- **`.c_inc` files are invisible to the stub tooling, and the failure is
  silent.** `make_stub_data.py` globs `*.c`, so a TU whose asset arrays and
  `_pc_load_src_*()` loader live in an `#include`d `.c_inc` never enters
  `stub.list`; `census_keeplist.py` then dropped its symbols for "not being
  stubbable". Under `DC_ASSET_STUB` that is the worst outcome available: the
  arrays are correctly sized in `.bss`, nothing fills them, and the map looks
  right. `src/game/m_msg.c` → `m_msg_data.c_inc` is the case that exposed it —
  the balloon behind every line of NPC dialogue was missing.
  **The arrays are `static`**, so `dc_stub_keep.inc` cannot load them directly
  (tried: eight undefined references at link). They have to be filled from
  inside the TU, which means the `.c_inc` itself gets `keep_file()`'d and
  shadowed on the include path — `-I$(STUBDIR)/include` in `dc/Makefile`,
  the same mechanism `DC_SRC_SHRINK` already uses.
- **An `INCLUDES` change does not invalidate `dc/build/flags.stamp`.** Adding
  `$(STUB_INCLUDES)` changed which `.c_inc` every kept TU sees and make
  rebuilt nothing — the link succeeded and the image was silently still using
  the vendored copy. The `.d` files name the OLD path, so they cannot help.
  After changing an include path, delete the affected objects by hand.
- **On a `DC_ASSET_STUB` image, a missing asset looks exactly like a renderer
  bug.** A stubbed texture array is `[1]` bytes, so its texels AND its palette
  read as zeros; the decoder faithfully produces a fully transparent rectangle
  and the geometry draws as a black silhouette. Nothing errors, nothing is
  rejected, and `[DC/TEX] uploads/hits/evictions` all look healthy — the upload
  succeeded, it was just an upload of nothing. **Before debugging a black
  model, run `DC_TEX_LOG=1` and check `nonzero=`.** 2026-08-02: 77 of 117
  uploads were blank, which read for a whole session as "the animal textures
  regressed"; the animals had simply never been in the keep list. See
  `tools/dcstub/keeplist-opening.txt`.
- **libforest's `TEV_*` constants alias `GXTevColorArg` ON PURPOSE, and one of
  the aliases is a trap.** `emu64.c:1423` casts the N64 combiner argument
  straight to `GXTevColorArg` because the tables were built to line up:
  `TEV_PRIMITIVE` 4 == `GX_CC_C1`, `TEV_ENVIRONMENT` 6 == `GX_CC_C2`,
  `TEV_TEXEL0` 8 == `GX_CC_TEXC`, `TEV_SHADE` 10 == `GX_CC_RASC`
  (`include/libforest/gbi_extensions.h:156-167`), and emu64 writes the matching
  registers at `emu64.c:3171,3180`. **But `TEV_COMBINED` is 0 and so is
  `GX_CC_CPREV`**, and those two do NOT mean the same thing — `COMBINED` is the
  previous cycle's result, not a constant. Any code that treats `GX_CC_CPREV`
  as a constant register silently blacks out the ~245 `(0, 0, 0, COMBINED)`
  cycle-0 draws in `src/data/model/`. `tev_creg_of` in `dc_pvr.c` excludes it
  for exactly this reason.
- **Wrap belongs to the BIND, not the upload.** `dc_pvr_texture.c` keys its
  cache on texel content, so one VRAM image is legitimately shared by GXTexObjs
  that wrap differently. Both `header_key()` in `dc_pvr.c` and the
  `dc_gx_state_dedup` early-return in `GXLoadTexObj` therefore have to include
  the wrap mode, or the second binding silently keeps the first one's header.
- **A comment that contradicts the code five lines below it will be believed.**
  `dc_pvr.c`'s viewport comment said "Y-down, same as GX, so there is no flip"
  while `emit_projected` right below it computed `cy - hh * y`. The cull bug
  above was derived from the comment, not from the code.

## Instrumentation

- **A repeat-suppressor keyed on a table must REPLACE on miss, never give up
  when full.** The first `OSReport`/`printf` flood limiter was open-addressed
  and returned "print it" once every slot was taken. Boot alone produces more
  than 32 distinct call sites, so the table was full before the flood started
  and the limiter did nothing — 741 unsuppressed lines in the next run, with no
  symptom other than the flood it was written to stop. Direct-mapped with
  eviction is correct: a flooding site re-claims its slot forever, an evicted
  one-shot line simply prints again. (`dc/src/dc_misc.c`.)
- **`OSReport` is not the only console sink.** `game64.c_inc`'s `[TRG_VOL]` and
  `[WALK]` lines call `printf` DIRECTLY, and they only become visible once the
  title screen is passed — so a limiter written against the title-screen log
  looks complete and is not. The sink is a `printf` override in DC-owned code.
- **…and `printf` is not the only sink either. `vprintf` needs the same
  override, and this one cost 8× the frame rate.** emu64's `Printf0`
  (`emu64_print.cpp:18`) calls `vprintf` DIRECTLY, bypassing the `printf`
  override entirely. It is gated on `g_pc_verbose`, which **`DC_ASSET_STUB`
  forces on** (`dc_main.c:81`) — so every stub build had it. In the town,
  `emu64.c:2690`'s
  `非シェアードの三角形群にシェアードの頂点が混ざっているので破綻しました!`
  fired **10,877 times in one 600 s run**; at 57600 baud that is ~900 ms of every
  1-second frame. **The town ran at 1.1 FPS with `gx=35.1ms` — i.e. the renderer
  was 4 % of the frame and the console was the rest.** Suppressing it: 10,877 →
  18 lines, **1.1 → 9.3 FPS**. If a scene is inexplicably slow and `gx=` does
  not account for it, count console lines before profiling anything else.
  ⚠️ After overriding `vprintf`, `dc_log_impl`/`dc_loge_impl` must call
  `vfprintf(stdout, …)` and the `printf` override must call `vfprintf` too —
  otherwise our own diagnostics get rate-limited (a suppressed `[DC/…]` line
  reads as "the thing did not happen") and `printf` charges every call site
  twice.
- **A counter that only counts the failure you thought of proves nothing.** The
  ARAM pager's small-read fast path (`dc_aram.c`, `len <= ARAM_BLK`) `memset` a
  32 KB block to zero, called `dc_dvd_pager_read`, **ignored the return value**,
  bumped `c_r_disc++` (the SUCCESS counter) unconditionally, and cached the
  block as authoritative. A failed or short read therefore published ZEROS as
  real content while `zero=0` in the `[DC/ARAM] LRU` line — the exact counter
  you would check to rule it out. The neighbouring slow path had the same shape
  in weaker form: `if (dc_dvd_pager_read(...) < 0)` treats a short read or `0`
  as success. `dc_dvd_pager_read` returns **bytes read** (`dc_dvd.c:228`), so
  the test must be `got != (int)n`. Both fixed 2026-08-02; the fast path now
  frees the block so the next read retries, and logs `SHORT READ`.
  ⚠️ Note *which* reads take the fast path: `len <= 32768`. The message/string
  TABLE reads are 64 B (`m_msg_main.c_inc:289`) and string bodies are ~64-128 B,
  so a silent zero-fill lands on exactly the strings and never on the bulk
  archive reads — which is why "the dialogue body renders but the speaker name
  and the reply do not" was reachable with every pager counter looking clean.
  ⚠️ **It was NOT the cause of that symptom** — the fixed build reports
  `SHORT READ = 0`. Real bug, wrong suspect; the missing name/reply text is
  still open (`kb/RESUME.md` item 4).
- **Never gate a periodic probe on `pc_frame_counter`.** `dc_vi.c`'s retrace
  handler returns early on every frameskipped tick — *after* incrementing
  `pc_frame_counter` — so a `pc_frame_counter % N == 0` test is evaluated only
  on presented frames, at counter values that jump by the skip factor.
  **What it looked like when it bit:** a run that presented 1,769 frames fired
  the arena probe three times, all inside the first two seconds, then never
  again; the counter simply stopped landing on a multiple of 60. Both probes
  now share one local `probe_tick` incremented where they are called.
- **Never union address ranges measured on a stub build.** Display lists are
  initialised `Gfx[]` data and are NOT stubbed, so `gsSPVertex(&obj_train1_1_v[93],
  15, 0)` carries a real offset and a real count into a 16-byte array: every read
  runs far past its own symbol and over its neighbours. Coalescing those spans
  merged unrelated arrays wholesale — 665,136 component reads collapsed to **ten**
  ranges, an undercount of unknown size with every identity but the first lost.
  Record contiguous *batches* keyed on base instead; the count comes from the
  display list and is real either way.
- **Interior pointers break nearest-symbol resolution.** `census_resolve.py`
  joins batches against the ~11,789 literal `gsSPVertex` sites in `src/` on
  `nm[symbol] + byte_offset == base`, which is an exact 32-bit equality.
- **A `[1]`-sized stub build still censuses correctly.** The addresses the GX
  layer is handed are link-time constants, so `DC_ASSET_CENSUS` names the same
  symbols in a stub image as it would in a full one; only the *sizes* are
  wrong. `tools/dcstub/census_resolve.py --sizes-from <full ELF>` is what
  turns that into real bytes — quoting the stub column as a working-set total
  understates it by about 20%.

## Harness / emulator

- **A short run is usually the HUMAN closing the emulator window, not a hang.**
  A 479-frame run (vs 10,199 the run before) was diagnosed as an audio-thread
  deadlock, a kill-switch bisect was built and launched, and then the user said
  "your changes didnt end the run early i did by accident". A run that stops
  mid-log with no crash dump and no `[DC/...]` error is ambiguous — **ask before
  bisecting.**
- **Say what an A/B build will break BEFORE handing it over.** A build made with
  `-DDC_PVR_NO_UVCLAMP` to test one hypothesis about the train windows also
  turns off the fix that repaired K.K. Slider's spotlight the previous session.
  The user reported "kk slider is messed up major, regression" — correct
  behaviour for that switch, wasted round trip. Name the expected collateral in
  the same message as the build.
- **`-c config:LimitFPS=no` unlocks the frame limiter.** `smoke.sh` passes `-c`
  straight through to Flycast (`smoke.sh:97`). This is the user's own
  play-testing setting and gets far more game per wall-clock second.
- **The town is ~4,000 frames in — use `--timeout 600`.** A 240 s run stops in
  the train intro and will make you think progression regressed.
- **Build to a COPY of the CDI before a long run.** Flycast holds
  `dc/build/OpenCrossing.cdi` open for the whole run, so the next build cannot
  land while it plays. `cp` to the scratchpad and run the copy; then builds and
  runs overlap instead of serialising into 20-minute cycles.
- **Guest `scif_flush()` permanently kills the Flycast console. Never call it.**
  KOS's flush clears TEND and spins; Flycast never re-raises TEND on an idle TX
  FIFO; KOS latches `serial_enabled = 0`; a later crash then prints **nothing**.
  Bisected across 7 guest variants — raising baud is fine, the flush is the
  killer.
- **KOS 2.3 assertion text** is `*** ASSERTION FAILURE ***` / capital-A
  `Assertion "x" failed`. The documented lowercase regex never matched, so a
  failed `assert()` only ever surfaced as a timeout.
- **mkdcdisc padding**: default 740,083,145 B / 15.6 s vs `-N` 1,783,337 B /
  0.021 s. Use `-N` for every emulator run; `DC_CDI_PAD=1` only for burns and
  read-speed-realistic timing.
- **A framebuffer HASH is not a framebuffer TEST — count nonzero pixels.**
  `FBHASH bae41dc5` looks like a result and is the FNV-1a of 614,400 zero
  bytes. Two runs showing two different hashes were briefly read as "the
  framebuffer works now"; adding `FBNONZERO <n> of 307200` showed n = 0 every
  time. Any probe that reports a digest must report a population count next to
  it, or the digest will eventually be mistaken for content.
- **`config:rend.EmulateFramebuffer=yes` (`smoke.sh --fb-writeback`) is
  REQUIRED for any guest-side framebuffer read in Flycast.** ⚠️ This bullet
  previously said the opposite — that the flag "does NOT by itself make the
  guest see pixels" — and that was **falsified 2026-08-02** by an A/B on one
  image: without it every candidate surface reads `0 of 307200`; with it,
  `13711 of 307200`. The earlier negative was reached by reading the wrong
  address *and* omitting the flag, so neither variable was isolated.
  Its frame-rate cost is **unmeasured**: the old "24.8 → 16.8 FPS" did not
  reproduce (25.0 with, 16.5 without) and no controlled pair exists, because
  every run dies at a different point in the title demo.
- **Flycast's 32-bit VRAM aperture repeats every 4 MB.** A sweep of all 8 MB
  therefore reports each block twice, at N and N+64. That mirroring is what
  cross-checks a 32-bit block index against its 64-bit counterpart (×2), and
  it will otherwise read as twice as much resident data as exists.
- **`vram_s` is not the displayed surface once `pvr_init()` has run.** The PVR
  allocates its own buffers inside VRAM and programs the display controller at
  them: `PVR_FB_R_SOF1` (0xA05F8050) read **0x000E7480**, i.e. 947,840 bytes
  in, while the probe was hashing offset 0. Read the scanout register; never
  assume the framebuffer is at the base of VRAM. `SOF1` page-flips between
  `0x000e7480` and `0x004e7480`.
- **KOS's `pvr_get_front_buffer()` is not a usable framebuffer address.** It
  returns `addr * 2 + PVR_RAM_BASE`, mixing a 64-bit-area offset with the
  32-bit-area base; on the second buffer it points off the end of VRAM
  entirely. Use `0xA5000000 + FB_R_SOF1`.
- **"Hot VRAM blocks" are not evidence of a rendered frame.** A sweep that
  flags a 64 KB block on one nonzero word counts the guest's own texture
  uploads. An earlier reading of "hot blocks grow 3 → 12 → 20 over a run" as
  writeback was exactly this mistake; the ten blocks the framebuffer occupies
  were empty the whole time.
- **A 16×12 thumbnail must box-filter, not point-sample.** The title logo
  covers a few per cent of a 640×480 frame, so a grid of 192 single pixels can
  report an all-black thumb off a frame that is not black. `dc_pvr_fb_probe()`
  averages whole cells now.
- **A smoke run of the game "fails" by construction.** The game never returns,
  so `run_reached_end_marker` / `mark_boot_ok` / `end_rc_zero` can never hold
  and `smoke.sh` exits 1 with `status=exited_early` even on a perfect run. For
  game images the console log is the artefact; read `[PERF]`, `[DC/PVR]` and
  the probe lines, not the exit code. The PASS/FAIL gate is meaningful for
  `selftest.cdi` and for anything that terminates.

## Docker / SDK image

- **`bash -lc` inside the SDK image** re-runs `/etc/profile`, which drops
  `/opt/toolchains/dc/sh-elf/bin` from PATH. `sh-elf-addr2line` then vanishes
  and every address silently symbolises to `??`. **Use `bash -c`.**
- **Sourcing `environ.sh` under `set -u`** exits 127 with nothing on stderr.
- **Host has no BuildKit** — `DOCKER_BUILDKIT=0`, never pass `--progress`.
  `--platform linux/arm64` is not optional; without it an amd64 pull drops the
  build into qemu.
- **Do not rebuild `opencrossing-dc:sdk`** — ~27 min cold. It is already in the
  local Docker daemon.

## The build tracks timestamps, not flags — FIXED, do not remove the fix

- **A flag change alone used to leave a stale image.** After a
  `DC_ASSET_STUB=1` build, a plain `bash dc/build-dc.sh` printed
  `make: Nothing to be done for 'all'` and left the **stub** ELF in place, so
  `sh-elf-size` reported the stub's sections for what looked like a real build.
  Two causes: toggling the flag swaps 2,521 sources for their stub twins and
  *both* sets of `.o` already exist and are older than the ELF, so nothing
  relinks; and any object whose source did not change keeps the `-D` set it was
  built with, which would have shipped a non-stub image whose `dc/src` objects
  still skipped `pc_assets_init()`.
- **The fix is `dc/build/flags.stamp`**: it holds `DC_ASSET_STUB`,
  `DECOMP_OPT`, `DC_OPT` and `DEFINES`, is rewritten by `$(file …)` when any of
  them changes, and every object and the link depend on it. Changing a flag now
  costs a full rebuild, which is the correct price.
- **`$(file …)` needs GNU make 4.0.** The container has 4.3; the macOS host has
  **3.81**, where `$(file …)` silently expands to nothing. That is why the
  stamp also has an ordinary recipe. The host only ever runs `make count` /
  `make sources`, never a compile.
- **When in doubt, `rm dc/build/AnimalCrossing.elf`** and re-link. A missing
  ELF cannot be stale.

## Disc content and the scratch-tree mechanism

- **`mkdcdisc -d DIR` puts DIR ITSELF on the disc**, so files land at
  `/cd/DIR/name` and every `DVDFastOpen` misses with no diagnostic. The flag
  you want is **`-D`** (contents, excluding the root). `dc_dvd.c:113` builds
  every path as `"/cd" + "/" + name`, flat, with no subdirectory.
- **Colima does not share `/private/tmp` with the VM.** A `-v` bind mount of a
  path under it is silently EMPTY inside the container — the build printed
  "0 files" and carried on. Stage disc content somewhere under `$HOME`.
- **`"${ARR[@]}"` on an empty array is an unbound-variable error under `set -u`
  in bash 3.2**, the macOS system bash. Every `dc/build-dc.sh` run without
  `DC_DISC_ROOT` died on it. Use `${ARR[@]+"${ARR[@]}"}`.
- **A quoted `#include` resolves against the INCLUDING FILE'S directory first**,
  so an `-I` shadow of a header in `include/` can never reach a consumer that
  pulls it in via a *sibling header* in `include/`. MEASURED: a shadow of
  `include/ac_structure.h` reaches `src/actor/ac_structure.c` but **not**
  `src/actor/npc/ac_npc.c` — a half-applied shadow, i.e. a silent ODR split that
  `--allow-multiple-definition` will not complain about. Header shadows are only
  safe for headers included directly by the TUs you care about; otherwise use a
  per-TU source swap confined to one TU, plus a compile-time assert pinning the
  unshrunk `sizeof`. `tools/dcstub/make_src_shrink.py` is built around this.
- **Every rewrite rule must hard-error on no-match.** A regex that silently
  matches nothing produces a build that looks fine and saves nothing, or worse,
  shrinks one of two consumers.

## Agent hygiene

- **Agents must not run git.** The main thread commits.
- **One build at a time.** `dc/build/` is a single shared object tree; two
  concurrent `make` runs corrupt it. Investigation agents get read-only
  `sh-elf-nm`/`objdump`/`readelf` over `docker run`, never `make`.
- **Always give absolute paths in scripts** — agents run from varying cwds.
