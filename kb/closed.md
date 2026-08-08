# Closed questions — do not re-propose any of these

Each entry cost a session or an agent to settle. **Read this before proposing
any RAM, size, or architecture idea.** Every item here was measured against the
real toolchain, not reasoned about.

Companion files: `kb/levers.md` (what is still live), `kb/traps.md` (toolchain
gotchas).

## 🛑 REOPENED 2026-08-08 — "DISC-CACHE MISSES ARE NOT THE STUTTER" WAS AN ARTEFACT OF FLYCAST

It was killed on 2026-08-06 by taking the ARAM cache 4 → 16 blocks: hit rate
83 → 97.9 %, disc reads 3.54 → 0.77/s, **stutter unchanged**. That A/B ran in
Flycast, whose harness sets `FastGDRomLoad=yes` and which models **neither seek
time nor transfer rate**. It measured a machine where a disc read is free.

On silicon a CD-R seek is 100-200 ms and then transfers at ~500 KB/s, against
~224 ms of total audio cushion (96 ms ring + 128 ms SPU) that **nothing refills
during a blocking `fs_read`**. A human on a burn: *"the stutter almost
perfectly lines up with laser load sounds."*

✅ Fixed by chunking the read and synthesising between chunks
(`dc_audio_disc_yield()`, `DC_DVD_READ_CHUNK`), **not** by a deeper buffer — a
deeper buffer buys the fix with permanent latency. ⚠️ **Awaiting hardware
confirmation.** What generalises, and it is the reusable half: **a hypothesis
about I/O TIMING cannot be refuted in an emulator that does not model I/O
timing.** Check what the instrument models before recording a refutation.

---

## ✅ G3 IS BUILT, GATED AND SHIPPED — STOP TREATING IT AS A PROPOSAL (2026-08-08)

`dc/src/dc_emu64_cull.cpp`, `DC_EMU64_CULL ?= 1`. Correctness gate
`-DDC_EMU64_CULL_VERIFY` ran clean (`falsecull=0 gfxp_bad=0 reinst=0`, 473
windows, to the town). Measured **−19.9 ms of a 69.8 ms town frame**, `fps_p50`
19.5 → 23.2, and the late cull's `vcull` collapsed **9,915 → 1,002**. The
5.4-19.2 ms G4 predicted came in at the top of its own range.

**What this closes with it:** the "interpose on emu64's dispatch table" decision
gate is spent. G1 measured, G2 is dead, G3 shipped. Do not re-litigate any of
the three. The remaining unattributed work inside TRIN is emu64's index
expansion on the ~39 % of batches that survive the cull.

## ✅ THE "BIMODAL 2.5 / 10 ms" AUDIO MYSTERY IS CLOSED — IT WAS VOICE COUNT (2026-08-08)

⚠️ **ONE OF THE FOUR IS REOPENED — see the entry below on disc-cache misses.**

Four hypotheses died for this between 2026-08-06 and 2026-08-08 (disc-cache
misses, multi-frame bursts, `snd_stream_poll`/G2/the scheduler quantum, and
`-O3` on the jaudio tree). The answer, once the music actually played, is
linear and boring: **`cost ≈ 2,332 us + ~265 us per voice-update`**, monotonic
across all eight census buckets. The two modes are SFX-only and music-playing.

**Also genuinely dead now, for the right reason this time:** the conditional FIR
and comb stages. `filt@=0 comb@=0` on **every** `[STUTTER]` row of a run with
BGM playing — which is the measurement session 7 correctly said it did not have.
L4 is closed. ⚠️ **L1 (`DC_AUDIO_VOICES`) is NOT closed** — it is priced,
correct, and simply unused.

## ⚠️ THE `-O0` DIRECTIVE IS REOPENED AND REVERSED (2026-08-06)

**This entry is kept, struck through, because it is the largest thing this file
ever got wrong and the shape of the error is worth keeping.**

> ~~"the optimizations cause problems and we cant use them without the port
> being broken"~~
>
> ~~**`-O1` / `-O2` / `-Os` / LTO are banned.** Not "risky" — banned, by user
> decision. The armhf record is why: `-O2` gave a wild-pointer crash loop from
> boot, `-O1` a hard SIGBUS on the intro train scene. Do not propose or
> benchmark optimization as a size or speed lever. That argument has been had
> and retired.~~

**What is true now:** `src/` builds at `-Os`, with a 14-TU hot list at `-O3`.
`DC_OPT_PROFILE` selects it, `dc/opt-lists.mk` holds the lists,
`DC_OPT_PROFILE=o0` is a byte-identical revert. Measured on this tree
(`kb/state-log.md`, 2026-08-06):

| | `-O0` | `-Os` | `-Os` + `-O3` hot |
|---|---:|---:|---:|
| `.text` | 5,506,964 | 2,680,676 | 2,729,152 |
| town FPS (matched window) | 11.6 | 18.5 | **20.0** |
| `draw` ms | 79.1 | 50.3 | **46.8** |

**Why the ban stood for five weeks, and the three lessons:**

1. **It was ARM evidence applied to SH-4.** The whole record traces to one
   armhf session on 2026-07-13 and survives only as a comment block in
   `pc/CMakeLists.txt:21-29`. No log, no commit, no test case. It was never
   reproduced on this target — and `kb/design-shelf-flags.md` §9 had already
   said so ("Achievable, and probably mandatory"), in a document this file
   overrode.
2. **The armhf failure was never isolated.** `-O2` was changed together with
   `-mcpu=cortex-a53 -mfpu=neon-vfpv4` (`pc/build-armhf-docker.sh:14`), so
   auto-vectorisation and 64-bit VFP load/store were in the same experiment.
   And upstream's own "compile everything at -O2" commit needed one line —
   a missing definition of `JUTRomFont::spFontHeader_` — which is a *link*
   bug that would present exactly as "wild pointer from boot". ⚠️ That symbol
   is still undefined in this tree; it is absent from both ELFs because
   `--gc-sections` drops its callers, and it is deliberately left undefined so
   that an optimized build which starts referencing it fails LOUDLY at link
   rather than dereferencing NULL.
3. **"Settled" was doing work that "measured" should have done.** The entry
   forbade *benchmarking*, which is what closed the question in 96 seconds the
   day someone tried it. A user decision can settle a preference; it cannot
   settle a fact about a compiler nobody had run.

**What is still true from the old entry:** raising optimization is not free and
not automatically safe. The decomp's UB is real — 35 missing returns, 99
uninitialised reads, and heavy type-punning on the per-vertex path
(`kb/state-log.md` has the scan). The guard set in `dc/Makefile` (`UB_GUARDS` +
`OPT_GUARDS`) is what makes `-Os` legal, and the quarantine list is how a TU
that miscompiles gets handled — not a tree-wide retreat.

## `-DTARGET_PC` is non-negotiable and must stay

It means "not GameCube", not "PC": it guards the base port's little-endian
correctness fixes — byte-wise texconv in `emu64.c`, swapped `u16` pair ordering
in `sys_matrix.c`, overlap-safe `Jac_bcopy` in `sample.c`. `-DTARGET_DC` is
added *alongside* it for genuinely DC-only branches.

See also `kb/levers.md` L1: the non-`TARGET_PC` branch cannot build at all
(no `src/data/**/assets/*.inc` tree exists), so "revert to the GameCube path"
was never available even in principle.

## Why `.bss` is not free — the fact everything turns on

**KOS's `mm_sbrk()` starts at the ELF `end` symbol. No MMU is enabled, no lazy
commit. Every `.bss` byte literally destroys a heap byte.** This is why
compression and debug-stripping are worth exactly zero (`.bss` is `NOBITS` —
there is nothing in the file to compress).

---

## Linker and toolchain levers — all measured, all dead

- **`--gc-sections` is mandatory, not an optimization.** `DC_GC_SECTIONS=0`
  **does not link**: the decomp has genuinely undefined symbols whose
  referencing sections GC removes (`JKRTask::searchBlank()`, `vtable for
  JSUOutputStream`, `JSURandomOutputStream::getAvailable()/skip`) plus KOS's
  `__kos_romdisk`. Its recovery is already spent: 522,150 B (map-based,
  authoritative).
- **`--icf`** — no SH backend in gold, no ICF in `ld.bfd`, no SH port of `lld`.
  (Source-level dedup in the *generator* is still open — `kb/levers.md` L6.)
- **SH GCC has no small-data model** — no `-G`/`-msdata` in `sh.opt`; the KOS
  script's `.sdata`/`.sbss` are inert, 0 bytes in the map.
- **`-g0` / strip saves exactly 0** — no debug section carries the `A` flag or
  appears in any `PT_LOAD`; `objcopy -O binary` never emitted it.
- **Compressing `1ST_READ.BIN` saves 0 RAM** — `.bss` is `NOBITS`.
- **`-fno-builtin` breaks the link.** `m_select.c:936,993` then call a real
  `alloca` newlib does not provide, and there is no `-fbuiltin-alloca`.
  `kb/design-shelf-hazards.md` marked it "(VERIFIED)" as KOS convention; that
  was **false for this image**.

## `-mfsrra` and `-mfsca` are INERT in this build (2026-08-05)

They are in `$KOS_CFLAGS` (`kb/toolchain-components.md`), so a grep finds them
and they look live. **GCC will not act on either without
`-funsafe-math-optimizations`** — plus `-ffinite-math-only` for `fsrra` — and
neither flag appears anywhere in this tree. So no `sqrtf`, `1/x`, `sinf` or
`cosf` in the image was ever folded into FSRRA/FSCA by the compiler.

Corroborated, not just read off the flags: `kb/perf-dc.md` §3.6 disassembled the
shipped `dc_pvr.c.o` and found `fsqrt` at +0x178 followed by three `fdiv` — the
light normalise compiled literally, on the hot per-vertex path.

**The consequence is already banked and does not need redoing:** the renderer
calls KOS's `frsqrt()`/`fipr` intrinsics explicitly (`kb/perf-dc.md` §3.6). Do
not "discover" the flags again and do not add
`-funsafe-math-optimizations` — that is codegen, which §1 bans.

## sh4zam — PASS for this port (2026-08-05, re-verified against the library's own source)

The user asked about **sh4zam** (`https://sh4zam.com/`,
`https://github.com/gyrovorbis/sh4zam`, MIT) after community advice that GCC
does not emit the SH-4's T&L instructions. It is also flagged as a follow-up in
`kb/toolchain-components.md` §4.3 and `kb/toolchain-decision.md` item 6.
**Verdict: do not adopt it.** The community premise is true in general and
false here:

1. **The port already emits the instructions it exists to emit**, through KOS
   `dc/fmath.h`: FTRV at `dc_pvr.c:2666`, `:2721` and `dc_mtx.c:246-427`, FIPR
   at `dc_pvr.c:188-191`, FSRRA at `dc_pvr.c:187`.
2. **Swapping KOS `mat_*` / `fipr` / `frsqrt` for sh4zam's equivalents is a
   NO-OP** — the same instructions, from a different header. The only
   measurable delta is one `jsr`/`rts` per `mat_load` where KOS inlines, ~15 µs
   against a 78.3 ms frame.
3. **`shz_sqrtf` is NOT FSQRT.** `shz_scalar.inl.h:315-325` defines it as
   `shz_inv_sqrtf_fsrra(x) * x`, i.e. an **FSRRA approximation** (~2^-21), not
   a correctly-rounded square root. The one real gap the audit found —
   272 `sqrtf` sites binding newlib's software `__ieee754_sqrtf` — is closed by
   `dc/src/dc_fmath.c` calling KOS `fsqrt()`, and sh4zam would have closed it
   *wrongly*.
4. **Its API is `inline` in headers (`SHZ_INLINE` = `inline static`), so its
   codegen is the including TU's.** Included from `src/` that is `-O0` — the
   library would be compiled badly exactly where the frame time is.
5. **`kb/perf-dc.md` §3.7 already measured this class of change at exactly
   zero**: four renderer micro-optimisations predicted at 1.5-3 ms/frame came
   back at +0.4 % across 215 counter-matched windows.

**If it is ever adopted, VENDOR it — do not take it from kos-ports.** It is
header-only for everything relevant, depends on nothing from KOS, and keeps
**no shadow copy of XMTRX** (so it cannot desynchronise from
`dc_mtx.c`'s residency cache). `dc/third_party/sh4zam/` costs nothing; a
kos-ports dependency forces a **~27 min Docker SDK image rebuild**, which is the
single most expensive thing in this toolchain.

Stated fairly, because the reason is "no room", not "bad library": sh4zam is
MIT, actively developed, and shipping in DCA3 and SM64-DC. If a future rewrite
moves the per-vertex math into a `dc/`-owned `-O2` TU, reason 4 stops applying
and this is worth ten minutes — but not before.

### ⚠️ REOPENED 2026-08-06 — reason 4 is dead, and the library was never the point

**"Not before" arrived the same week.** Reason 4 was "its API is `inline` in
headers, so its codegen is the including TU's, and it would be included from
`src/` at `-O0`". There is no `-O0` any more: `src/` is `-Os`/`-O3` and
`dc/src` is `-O3`. **That objection is void.**

Reasons 1-3 and 5 still stand as stated, and they are all about *swapping
calls*: the port already emits FTRV/FIPR/FSRRA through KOS, so exchanging
`mat_*` for `shz_*` is the same instructions from a different header, and
`shz_sqrtf` is an FSRRA approximation rather than a correctly-rounded FSQRT.

**But the maintainer's own guidance (2026-08-06) is not "swap the calls" — it
is a different shape of code**, and that part is genuinely untested here:

> All matrix operations are performed within XMTRX registers, rather than
> within memory. We directly initialise XMTRX into the first transform rather
> than identity. We use apply operations when a transform only needs to be
> applied over a submatrix. We directly set the translational component rather
> than applying it as a transform.

i.e. `shz_xmtrx_init_rotation_xyz` / `apply_scale` / `set_translation` /
`store_4x4` build a compound transform without ever round-tripping the matrix
through memory. `dc/src/dc_mtx.c` does the opposite today: it keeps a residency
cache *because* it assumes matrices live in RAM and XMTRX is a scarce resource
(`DC_MTX_NO_XMTRX_CACHE` reverts it, and F2's premise was already measured
FALSE — `dl_G_VTX` alternates two matrices per vertex, so the cache misses
every call).

**So the open question is not "is sh4zam faster than KOS", it is "should
`dc_mtx.c` stop staging matrices in memory at all".** That is a rewrite of our
own code, which sh4zam would then be the natural vocabulary for.

**Cost of finding out, now that a rebuild is 96 s:** vendor into
`dc/third_party/sh4zam/` (NOT kos-ports — that forces a ~27 min SDK image
rebuild), and A/B `us/v`, which is already the right instrument: it moved
4.05 → 3.11 on the `DC_OPT=-O3` change alone, so it is sensitive enough to
price this.

⚠️ **Do NOT re-run the "swap `mat_load` for `shz_xmtrx_load`" experiment and
report the result as a verdict on sh4zam.** That experiment is reasons 1-2, it
was already done, and it measures nothing.

### Two corollaries banked with it, so they are not re-derived

- **FSQRT needs no precision screenshot.** KOS sets `FPSCR = 0x00040000` at
  `startup.S:74-85` (DN=1, RM=00 = round-to-nearest-even, every exception
  enable clear), so FSQRT is the correctly-rounded IEEE-754 square root and is
  **bit-identical to newlib's software routine for every normal input**. The
  three divergences are all consequences of that same word: `x < 0` gives qNaN
  without setting `errno` (nothing in the tree reads `errno` after a `sqrtf`),
  `sqrt(-0) = -0` on both paths, and DN=1 flushes denormal inputs to zero.
- **FSCA is not an FPS lever here.** There are only **four** live `sinf`/`cosf`
  sites in the image (`m_camera2.c:87-90`); everything else goes through
  `sins()`/`coss()`, which is a table lookup at the same 16-bit angular
  resolution FSCA takes as its input. There is nothing to convert.
- **The "camera basis precision" worry has no mechanism in this build.**
  `PSVECDotProduct`, `PSVECMag`, `PSVECCrossProduct`, `C_MTXLookAt` and
  `PSMTXMultVecArray` are all in the map's *Discarded input sections* — **not
  in the image**. Only `PSVECNormalize` is live (`0x8c4cdde4`), with one caller
  (`emu64.c:4696`) gated on texture-gen batches. ⚠️ This contradicts
  `dc_mtx.c`'s own older comment about `PSMTXMultVecArray` being "where XMTRX
  actually pays"; the map is the authority.

## SH-4 MMU demand paging — VERDICT: DEAD

Full writeup: `kb/research-mmu-paging.md`. Read it before ever reconsidering.

The killer: **the MMU cannot create memory, and we have no backing store to
page against.** On SH-4 the entire 29-bit physical space is already directly
addressable with the MMU off via P1/P2, so the MMU buys only protection (don't
need it) and oversubscription against a backing store (don't have one).

The only store big enough for 8.5 MB is the CD-R at ~500 KB/s: one 4 KB page is
**8.19 ms of transfer against a 33.3 ms frame budget — 24.6% of a frame per
fault** — while the fault mechanism itself costs ~1.5–2.5 µs. **The backing
store costs ~4,000× the fault.**

The comparison that settles it: `assets.pak` already pages the same bytes off
the same CD, but at asset granularity, in load order, pre-swapped, with zero
seeks. MMU paging would replace that with 4 KB faults at arbitrary instruction
boundaries with no prefetch, batching, or load-order knowledge — *a strictly
worse implementation of something the project is already building.*

Four secondary findings each sink it independently: KOS's dynamic mapper forces
every paged page **uncached**; TLB reach is 253,952 B against 8.6 MB; KOS has
**no eviction path at all**; and MMU-on makes store queues fault-prone on
SH7750 silicon.

## "JUST PUT ALL THE ASSETS IN RAM" — REFUTED BY A BOOT (2026-08-06)

The RAM picture moved far enough this session that the question deserved an
experiment instead of an estimate, so a full `DC_ASSET_STUB=0` image was linked
and run (`smoke-oc-dc-nostub-20260806-165321-16857`).

```
text 3,050,152  data 2,224,820  bss 10,493,196   _end 0x8cf19b6c
MEMLEDGER FIT image_span=15768428 additive_heap=1658752 usable=16646144
              margin=-781036 OVER          <- the first OVER the ledger has printed
Out of memory. Requested sbrk_base 8d016000, was 8cf36000, diff 917504    (main.dol)
Out of memory. Requested sbrk_base ...                   diff 15638528    (foresta.rel)
```

Both allocations fail, so `rom_src=0` never resolves and **all 14,495 asset rows
come back MISSING — the non-stub image has LESS content than the stubbed one.**

**The structural reason, which no lever can move:** the non-stub path asks libc
for **one contiguous 15,638,528 B buffer**. A 15.6 MB `malloc` cannot fit in
16 MB next to a 15.8 MB image, at any margin.

```
span 15,768,428 + additive 1,658,752 + libc peak 3,056,276 = 20,483,456
                                              usable         16,646,144
                                                short by       3,837,312
```

Two corrections banked with it, both of which were live misconceptions:

- **S6 is NOT a demand loader.** It deletes `s_assets[]`'s `path` field and
  14,495 string literals (`make_src_shrink.py:467`) — 598,648 B of `.rodata` —
  and nothing else. The rewritten loader still does
  `memcpy(dest, rom + rom_off, size)` against the whole resident REL. **The real
  demand loader is `dc_stub_keep_assets()` / `dc_stub_keep_load_one()`
  (`dc_main.c:885-1135`), entirely inside `#ifdef DC_ASSET_STUB`.**
- **`rom_src=0` means `SRC_REL`**, not "row 0".

## AICA's 2 MB cannot hold a C array

DMA-only over a 16-bit 25 MHz G2 bus. Still viable as a *destination* for
specific buffers (audio — `kb/levers.md` L3), never as general storage.

## emu64 is NOT an N64 emulator, and there is no emulated RDRAM anywhere

Verified independently twice. Full writeup: `kb/research-n64-origin.md`.

emu64 is a GBI display-list interpreter emitting GX. `emu64.hpp:750`'s
`u32 segments[16]` is 64 bytes of real GameCube pointers; `seg2k0()`
bounds-checks `0x80000000..0x83000000` because that is GameCube MEM1, not
because it is an emulated image's extent. Game logic is *ported* — `src/`
carries the same TUs as the N64 decomp and `src/static/libultra/` reimplements
the N64 OS API on Dolphin OS. The only genuinely emulated memory in the build
is rspsim's 4 KB `DMEM[0x1000]`. Whole emu64 tree = 562,374 B of `.bss`.

So: **the 22.5 MB image is not an emulation artefact**, and there is no
emulated RAM image to delete.

## `foresta.map` / `static.map` are droppable — 5,402,023 B of disc

Only reader is `JUTException::queryMapAddress_single` on the
`OSSetErrorHandler` path, which returns false outside `0x80000000..0x82FFFFFF`.
No SH-4 address qualifies.

## Second-tier memory (VRAM / AICA) probing — deprioritised

`kb/research-second-tier-memory.md` is a **salvaged fragment, not a real doc** —
the agent died before writing. Recovered: a complete but **never-compiled,
never-run** benchmark at `harness/dc/bench/bench_mem.c` (probes every
main-RAM↔VRAM and main-RAM↔AICA path both directions with checksum
verification), plus uncited community bandwidth figures (SQ→RAM 495 MB/s,
cacheline read+writeback 223.5 MB/s). **No VRAM *read* figure** — that gap is
still open.

Low priority now: with MMU paging dead, VRAM and AICA are only interesting as
destinations for specific buffers (`texture_buffer_data`, audio), not as
general storage.

## NPOT + `GX_REPEAT` textures — the bug is real and has ZERO instances

Filed as an open renderer defect in `kb/RESUME.md` §5 item 2: a non-power-of-two
texture is padded up to POT in VRAM and its UVs are scaled by
`u_scale = w / pot_w` (`dc_pvr_texture.c:1211`, applied `dc_pvr.c:2017-2021`).
That is exact for `GX_CLAMP` and structurally wrong for `GX_REPEAT`, because the
PVR wraps at the padded boundary, so tile *n* starts at `n·u_scale` instead of
`n`. `GX_MIRROR` is wrong the same way.

**Censused exhaustively, 2026-08-03. `src/data` is 3,212 files and every display
list in the game is enumerable as source.** All eight Dolphin-path macro
spellings parsed with their per-macro argument orders (note
`gDPSetTextureImage_Dolphin` swaps `h` and `w` relative to `gsDP…`,
`gbi_extensions.h:1102-1107`), each `SetTextureImage` paired with the next
`SetTile` in file order — **12,108 texture binds**:

| class | binds | share |
|---|---:|---:|
| POT + REPEAT/MIRROR — fine | 6,958 | 57.5 % |
| mixed: NPOT axis CLAMPed, POT axis wrapped — fine | 765 | 6.3 % |
| NPOT + CLAMP — fine (edge-pad is correct there) | 408 | 3.4 % |
| POT + CLAMP — fine | 3,977 | 32.9 % |
| **NPOT + REPEAT or MIRROR — the bug** | **0** | **0.0 %** |

The artists were disciplined: every NPOT axis in the shipped data carries
`GX_CLAMP` **on that axis**. The 765 "mixed" binds (e.g. 16×48
`GX_MIRROR,GX_CLAMP`) are the pattern that looks dangerous and is not. All 972
binds at `w = 48` clamp the 48 axis. The N64 tile path cannot produce the bug at
all: `emu64.c:2195-2223` computes GX wrap from `cs/ms/ct/mt` **only** for a
dimension in `{4,8,16,32,64,128,256,512}` and its `default:` arm forces
`GX_CLAMP`.

Also evaluated and rejected, so nobody re-derives them:

- **Period replication when padding** is exact only when `pot/real` is an
  integer, and for any genuinely NPOT `w` that ratio is strictly between 1 and 2
  — so it is exact for *no* NPOT size this game ships.
- **CPU-side UV folding** is wrong in the common case, not the rare one: folding
  is per vertex and the hardware interpolates linearly between folded values, so
  a triangle spanning a wrap boundary plays the texture backwards. The logged
  train-window batch (`uv=-2.80,0.02..-1.80,1.02`) crosses an integer boundary
  *inside* the primitive.
- **Resampling to POT** would need a resampler `dc_pvr_texture.c` does not have,
  cost a bilinear pass per upload on SH-4 at `-O0`, and blur every font sheet and
  UI element — all of which are `GX_CLAMP` and exact today.

**Residual, unverified and sub-8 only.** `next_pot()` floors at
`DC_PVR_TEX_MIN_DIM = 8` (`dc_pvr_texture.c:942-946`), so a GX dimension of 4 —
POT to GX, NPOT to us — gets `u_scale = 0.5`. `emu64.c:2196,2227` both list
`case 4:`, so a 4-wide N64 tile with `cs==0 && ms==0` can reach `GX_REPEAT` at
runtime. No static call site exhibits it. Detector rather than patch: assert no
`DC_TEX_LOG` upload has `w < 8 || h < 8` with `wrap=1|2` on the matching
`BATCH` line. If one ever appears, the narrowed fix is periodic replication for
sub-8 only, where the ratio *is* an exact power of two.

---

## F1 — offline bbox-CULLDL injection. NOT RECOMMENDED (2026-08-04)

`kb/research-fps-ideas.md` F1 proposed splitting acre and object display lists
into chunks, each prefixed with 8 synthetic AABB corner vertices and a
`gsSPCullDisplayList`, so emu64 skips geometry before paying `-O0` price for it.
A full design pass killed it on arithmetic, not on taste:

- **Its RAM cost is 594 KB of `.data`** for the town keep-list scope (3,804
  chunks x 160 B), not the 60-120 KB it claimed. 1.83 MB for all of `src/data`.
- **Its own cost cap selects nothing.** F1 proposed bounding injection to
  display lists of 50+ vertices. No `gsSPVertex` anywhere in `src/data` exceeds
  **32** vertices: max is 32, p50 is 14, and chunks with n >= 33 number
  **zero**. ⚠️ **Wording corrected 2026-08-05 — the verdict is unchanged, the
  stated reason was not.** The 5 bits are the per-vertex **index** width
  (`POLY_5b`, `include/libforest/gbi_extensions.h:64,69-86`), which is what caps
  a batch at 32 distinct sources. They are *not* the N-triangle count:
  `emu64.c:4814` reads `n_faces = ((w0 >> 17) & 0x7F) + 1`, i.e. **7 bits,
  1..128 faces per `G_TRIN_INDEPEND`**. Do not quote "5-bit N-triangle format".
- **The bboxes cannot be computed without the ROM.** 2,112 files source their
  `Vtx` from `assets/*.inc` files that do not exist in the repo; under
  `TARGET_PC` every vertex array is uninitialised storage filled at runtime
  from `main.dol`/`foresta.rel`. So the bbox table is ROM-derived data that may
  not be committed (CLAUDE.md §1) and cannot be regenerated in the container.
  A runtime AABB pass removes that but keeps every byte.
- **G3 dominates it**: 25-35 ms against F1's ~18 ms, for **0 bytes**, covering
  runtime-built display lists too, and needing no per-symbol reachability proof.
  Spending 594 KB against a 4.7 MB deficit to buy a smaller version of a free
  win is bad arithmetic.

⚠️ Also note for anyone measuring an F1-shaped change: **`cmds` goes UP**, on
every frame, even when the frame gets faster — a cull hit skips 2 commands and
adds 4, and the win is entirely per-vertex work. That breaks `kb/perf-dc.md`
§6's matched-frame recipe, which matches on `cmds`.

The surviving variant, if the G3 sign-off is ever refused: model-granularity
injection scoped to the town keep list, 1,355 boxes, **212 KB**. It should still
wait for `DC_EMU64_HIST` to run.

**Correction banked along the way:** `kb/perf-dc.md` §3.5 used to justify the
vertex memo's 32 entries with "emu64's cache is `Vtx vertices[32]`". That
premise is false — `VTX_COUNT` is **128** (`emu64.hpp:33`). The memo stays
correct (it is a direct-mapped cache with a field-by-field compare), but the
stated bound is not a bound; what actually caps a batch at 32 distinct sources
is the 5-bit per-vertex **index** width, not any triangle count (see the
correction above). **The memo itself is 128 slots as of 2026-08-04**
(`dc_pvr.c:1955`), hit rate 48.2-48.9 %.

## F8 — stripping RDP/RSP state commands. ANSWERED BY G1, worth ~nothing (2026-08-05)

F8 was reopened on 2026-08-04 on the strength of "2,094 of 2,867 town commands
per frame — 73 % — are pure state, and at 12.31 µs/cmd that is ~26 ms". The
12.31 µs was an average (measurement rule 7) and the histogram priced the same
opcodes directly:

```
MOVEMEM 0.55/207 · SETTILE_DOLPHIN 0.32/109 · SETCOMBINE 0.28/58 ·
SETTIMG 0.25/112 · ENDDL 0.18/131 · LOADTLUT 0.13/42
```

**Every state opcode is ≤ 0.55 ms/frame.** A large *count* of near-free commands
is worth nothing to strip. **Do not build a strip rule** — not against the
static count, and now not against the runtime mix either. The frame is in
`G_TRIN_INDEPEND` (22.25 ms) and in `gap` (7.92 ms, still unexplained).

## G2 — reimplementing emu64's dispatch LOOP. DEAD (2026-08-06)

G2 was "reimplement `emu64_taskstart_r` in `dc/` at `-O2`, because `src/` is
stuck at `-O0`". Two things killed it, and the second is a measurement:

1. `emu64.c` is on the `-O3` hot list (`dc/opt-lists.mk`). **The compiler did
   G2's job, in the right place, with no interposition and no sign-off.**
2. **G2's target is exactly `gap`, and `gap` is now 5.96 ms of a 45.6 ms
   frame** — down from 15.8, on the strength of that same flag (`kb/state-log.md`
   2026-08-06; note the ×2 rule before comparing it to anything older).

**Recommendation: delete `dc/src/dc_emu64_shadow.cpp` after one A/B.** ⚠️ It
costs nothing when off — the whole body sits inside
`#if DC_EMU64_SHADOW_LOOP > 0` and it is 20 KB only when on — so the reason to
remove it is not bytes: **it blocks G1** through the `#error` at
`dc_platform.h:417`, and G1 is the only instrument allowed to price an opcode.

## `pvr_dropped` — a data-dependent counter with no speed mechanism (2026-08-06)

`s_tris_dropped` (`dc_pvr.c:134`) increments **only** on near-plane geometry:
all three vertices behind the plane (`:2149`, `w <= 0.001f`), a straddle under
`-DDC_PVR_NO_NEARCLIP` (`:2162`), or Sutherland-Hodgman emitting fewer than
three vertices (`:2181`). It is a function of **where the camera is standing**,
nothing else.

So the `1,300 → 0` that `run_report --vs` reported across the `dc/src` `-O3`
change was the camera, not the flag — and the older 0 / 1,305 / 1,314 / 0 spread
across four identical-code runs was the same thing. **Do not read this counter
as a regression, a win, or evidence about the renderer.** It remains a
correctness tripwire for the clipper and nothing more; `run_report.py --vs` will
keep calling it a REGRESSION and keep being wrong about it.

## A census cannot produce a town keep list (2026-08-04)

Do not propose "just re-run the census on a town scene" for missing acres,
structures or villagers. `src/system/sys_math.c:7` seeds the whole town from
`sqrand(osGetCount())`, which on DC is boot-elapsed time, so **every boot lays
out a different town**. A census names what ONE run walked into. Enumerate from
the tree (`tools/dcstub/make_keeplist_town.py`), or land S4.

Separately, the census only ever observes the **depth-0 branch of every
decision** — see `kb/traps.md`. It is a working-set tool, not a coverage tool.

## Running bench_mem in Flycast (2026-08-04)

`harness/dc/bench/bench_mem.c` now builds, runs and passes every checksum — and
the emulator cannot answer the question it exists to ask. CPU read == write ==
**114.3 MB/s** at every size in both the 32-bit and 64-bit VRAM windows, and the
DMA rows come off 0-2,240 ns samples. Flycast models neither VRAM access latency
nor Holly bus contention. **Do not re-run it in an emulator and quote the
numbers.** It is a CD-R burn task now, at 57,600 baud.

## Caveat on the wider `kb/`

The first session's deliverables were written by agents whose **adversarial
verifiers all died**, so they are unreviewed. Treat their numbers as claims
until confirmed. Falsified so far by contact with the real toolchain:

1. `-fno-builtin` ("VERIFIED", actually breaks the link).
2. The header-collision scan — measured GCC 9.3 / KOS `525cbda`, not our GCC
   15.2 / KOS 2.3, and missed **both** collisions that actually bit us.
3. "No MMU" — true of KOS's default config, false of the hardware.
