# SH-4 MMU demand paging — VERDICT: DEAD

Researched 2026-08-01, five evidence files. Those files were deleted 2026-08-09
in the kb audit — the verdict is settled and nothing acted on the detail. Recover
them from git history (`git log -- kb/research-mmu-*.md`) only if a precondition
below is ever met.

## Why it is dead

**The MMU cannot create memory, and we have no backing store to page against.**

On SH-4 the entire 29-bit physical address space is already directly addressable
with the MMU **off**, through P1 (`0x8xxxxxxx`, cached) and P2 (`0xAxxxxxxx`,
uncached) — main RAM, VRAM, AICA RAM, every register file. The MMU therefore
buys only (1) memory protection, which we do not need, and (2) oversubscription
against a backing store, which requires a backing store.

The only store big enough is the CD-R at ~500 KB/s. One 4 KB page is **8.19 ms
of transfer with zero seek** against a 33.3 ms frame — **24.6 % of a frame per
fault**, while the fault mechanism itself costs ~1.5–2.5 µs. **The backing store
costs ~4,000× the fault.**

And `assets.pak` (`kb/asset-pack.md`) already pages the same bytes off the same
disc at *asset* granularity, in load order, pre-byte-swapped. MMU paging is a
strictly worse implementation of something already built.

Four secondary findings would each sink it alone:

- KOS's dynamic mapper forces every paged page **uncached**;
- TLB reach is **253,952 B** against 8.6 MB of assets;
- KOS has **no eviction path at all** — fill only;
- enabling the MMU makes the store queues fault-prone on SH7750 silicon (Sega's
  own erratum). ⚠️ **`dc/src/` now depends on QACR for `pvr_dr_*` (G-C), so this
  tax is no longer hypothetical — the MMU staying off is load-bearing.**

Flycast's MMU emulation is fine. Emulator support was never the deciding factor.

## The four preconditions to reopen

All four, not any one:

1. a backing store faster than the CD (a real RAM disc, or flash);
2. an eviction path in KOS's mapper, or our own;
3. cached paged pages;
4. a store-queue path that does not go through QACR, or an erratum-free part.
