# Closed questions — do not re-propose any of these

Each entry cost a session or an agent to settle. **Read this before proposing
any RAM, size, or architecture idea.** Every item here was measured against the
real toolchain, not reasoned about.

Companion files: `kb/levers.md` (what is still live), `kb/traps.md` (toolchain
gotchas).

---

## The `-O0` directive — settled by the user, not by engineering

> "the optimizations cause problems and we cant use them without the port
> being broken"

**`-O1` / `-O2` / `-Os` / LTO are banned.** Not "risky" — banned, by user
decision. The armhf record is why: `-O2` gave a wild-pointer crash loop from
boot, `-O1` a hard SIGBUS on the intro train scene.

Do not propose or benchmark optimization as a size or speed lever. That
argument has been had and retired. If the levers do not close the gap, cutting
content or declaring a stock-16 MB build infeasible are the honest options;
quietly reopening this is not.

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
  display lists of 50+ vertices. The game uses the **5-bit** N-triangle index
  format exclusively, so no `gsSPVertex` anywhere in `src/data` exceeds **32**.
  Max is 32, p50 is 14, and chunks with n >= 33 number **zero**.
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

**Correction banked along the way:** `kb/perf-dc.md` §3.5 justifies the vertex
memo's 32 entries with "emu64's cache is `Vtx vertices[32]`". That premise is
false — `VTX_COUNT` is **128** (`emu64.hpp:33`). The memo stays correct (it is a
direct-mapped cache with a field-by-field compare), but the stated bound is not
a bound; what actually caps a batch at 32 distinct sources is the 5-bit
triangle index format.

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
