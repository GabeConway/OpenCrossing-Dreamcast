# SH-4 MMU demand paging as a RAM lever — feasibility

Researched 2026-08-01. Question: can SH-4 MMU demand paging close any part of
the 14,451,476 B overage, specifically by backing the 8,617,214 B of `src/data`
asset destination arrays with a virtual region?

Tags: **[M]** measured/read directly in this session, **[S]** sourced to a URL,
**[R]** reasoned from the above.

---

## VERDICT: **DEAD**

**The finding that kills it: the MMU cannot create memory, and we have no
backing store to page against.**

On SH-4 the *entire* 29-bit physical address space is already directly
addressable with the MMU off, via P1 (`0x8xxxxxxx`, cached) and P2
(`0xAxxxxxxx`, uncached) — main RAM, VRAM, AICA RAM, and every register file.
[S] The MMU therefore buys exactly two things on this machine:

1. **memory protection** — we do not need it; and
2. **oversubscription of virtual space against a backing store** — which
   requires a backing store.

Our only backing store with the capacity to hold 8.5 MB is the CD-R, at the
project's own settled ~500 KB/s. One 4 KB page is **8.19 ms of transfer with
zero seek**, against a 33.3 ms frame budget at 30 fps — **24.6 % of a frame per
page fault**. [R, from the settled 500 KB/s figure] The fault *mechanism* costs
~1.5–2.5 µs (§5). **The backing store costs ~4,000× the fault.** The MMU is not
the bottleneck and does nothing to relieve the thing that is.

And the comparison that settles it: the project's existing plan (`assets.pak`,
`kb/asset-pack.md` + `kb/STATE.md` levers 1–2) pages **the same bytes off the
same CD** — but at asset granularity, in real load order, pre-byte-swapped, with
82 backward reads of max reach 7,520 B so an 8 KB window yields **zero seeks**
across the whole 8.9 MB load. MMU paging replaces that with 4 KB faults at
arbitrary instruction boundaries, with no prefetch, no batching, no load-order
knowledge, and no ability to block on I/O (§4). **It is a strictly worse
implementation of a thing the project is already building.** [R]

Four independent secondary findings would each also sink it on their own:
KOS's dynamic mapper forces every paged page **uncached** (§3), TLB reach is
**253,952 B against 8.6 MB** (§2), KOS has **no eviction path at all** (§3), and
enabling the MMU makes the store queues fault-prone on SH7750 silicon in a way
Sega's own erratum documents (§7).

Nothing here depends on emulator support. Flycast's MMU emulation turns out to
be *fine* (§7) — it is simply not the deciding factor.

---

## This document was split (2026-08-02)

It was 585 lines. The verdict above stays here; the evidence lives in five
parts, each of which repeats the DEAD verdict in its own header. Section
numbers inside the parts are the original ones — use this table to resolve a
`§n` reference.

| part | original sections | contents |
|---|---|---|
| [`kb/research-mmu-kos-capability.md`](research-mmu-kos-capability.md) | §1, §2, §3, §3.1 | KOS ships an MMU driver, off by default; 4 KB-only page table; 253,952 B TLB reach; fill works, eviction/dirty-tracking absent; every paged page forced uncached |
| [`kb/research-mmu-fault-cost.md`](research-mmu-fault-cost.md) | §4, §4.1, §5 | interrupts masked in the fault handler ⇒ no CD-backed pager; BL=1 reset, UTLB multi-hit, synonym rule; ~2–4 µs per miss vs 8,192 µs per CD page |
| [`kb/research-mmu-hardware-tax.md`](research-mmu-hardware-tax.md) | §6, §7 | static remapping is redundant against P1/P2; SQ corruption errata make *any* MMU use a permanent tax |
| [`kb/research-mmu-game-impact.md`](research-mmu-game-impact.md) | §8, §11 | the five semantic breakages in `src/` (DMA bypasses the MMU); `.text` cannot page either, and does not need to |
| [`kb/research-mmu-reopening.md`](research-mmu-reopening.md) | §9, §10, §12, Sources | precedent (KOS-MMU, Flycast, WinCE), the four preconditions to reopen, unfinished threads, and all citations |
