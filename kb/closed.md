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

---

## Caveat on the wider `kb/`

The first session's deliverables were written by agents whose **adversarial
verifiers all died**, so they are unreviewed. Treat their numbers as claims
until confirmed. Falsified so far by contact with the real toolchain:

1. `-fno-builtin` ("VERIFIED", actually breaks the link).
2. The header-collision scan — measured GCC 9.3 / KOS `525cbda`, not our GCC
   15.2 / KOS 2.3, and missed **both** collisions that actually bit us.
3. "No MMU" — true of KOS's default config, false of the hardware.
