# MMU paging — precedent, what would have to change to reopen it, and sources [DEAD idea]

**VERDICT: DEAD.** MMU paging cannot close any part of the RAM overage: the
MMU cannot create memory, and the only backing store big enough is a 500 KB/s
CD-R (8.19 ms per 4 KB page ≈ 24.6 % of a frame). Nothing in this file reopens
it. Full verdict and the four independent secondary killers:
`kb/research-mmu-paging.md`.

§9, §10, §12 and the full source list of `kb/research-mmu-paging.md`, moved
verbatim: KOS/Flycast/commercial precedent, the four things that would all have
to change at once, what the research did not get to, and every citation used by
the other parts. Read *only* if you intend to reopen a settled-dead question.

---

## 9. Precedent [S / see agent notes]

- **KOS's MMU module is ported from KOS-MMU**, which the header itself says
  *"never reached a real phase of maturity and usefulness."* [M, `arch/mmu.h`]
  KOS's README notes *"there is an MMU module for the DC port, [but] nothing
  really uses it at this point."*
  <https://kos-docs.dreamcast.wiki/md_doc_2README.html> [S]
- The only in-tree users are the two toy examples,
  `examples/dreamcast/basic/mmu/{pvrmap,nullptr}` — a static VRAM remap and a
  null-pointer catcher. Neither does demand paging. Nothing else in KOS,
  kos-ports, or GLdc references `mmu_init`. [M]
- **Flycast 1.92.1 does implement the SH-4 MMU.** Its binary contains
  `core/hw/sh4/modules/mmu.cpp`, `CCN_MMUCR`, `ldtlb`, `MMU_init`,
  `mmu_instruction_lookup`, `ITLB_LRU_USE`, `MMU_TT_DWRITE`,
  `MmuError::TLB_MHIT`, and the log string **"Enabling Full MMU support"**. [M,
  `strings` on `/Applications/Flycast.app`] Emulator support is therefore *not*
  the blocker — see §10 for the caveat about iteration fidelity.
- Dreamcast's own commercial precedent for MMU use is **Windows CE titles**,
  which used it for OS address translation, not for paging content off the disc.
  Sega's native SDK (Katana/Shinobi) and every homebrew port examined — ScummVM,
  dcload, dca3 — run with the MMU off and solve 16 MB by *carving content*
  (ScummVM's `.PLG` overlays, `dcloader.cpp`) rather than paging it.
  See `kb/research-size-reduction.md` §3.7. [S]

---

## 10. What would have to change to reopen this

All four, together. Any one missing and it stays dead. [R]

1. A backing store of ≥8.5 MB with **≤100 µs random 4 KB access**. Nothing on a
   stock Dreamcast qualifies. (A 32 MB RAM mod would, and is permanently out of
   scope per `CLAUDE.md`.)
2. KOS patches for: cacheable dynamic pages, a `mmu_page_unmap`/eviction path,
   dirty tracking via a working initial-write handler, and 64 KB pages in the
   page table — i.e. finishing KOS-MMU.
3. A fault path that can perform I/O, meaning a polled GD-ROM driver plus a
   re-entrant exception save area.
4. An MMU-aware DMA path so PVR/G2 transfers out of paged arrays resolve real
   physical addresses.

That is an operating-system project, and at the end of it the page still comes
off a 500 KB/s CD. **Ship the `assets.pak` loader instead** — same backing
store, semantic granularity, sequential layout, zero seeks, and it is already
built and verified (`kb/asset-pack.md`).

If someone does reopen it, the cheap first experiment is a KOS test that calls
`mmu_init()`, maps one page via a custom callback, and reads through it under
`harness/dc/smoke.sh` — to confirm Flycast's UTLB path matches hardware before
any further investment. Not run here, because no result from it can move the
verdict.

---

## 12. What I did not get to — open questions and next steps

1. **Flycast MMU fidelity, in depth.** [M] confirms Flycast 1.92.1 contains a
   real MMU implementation (§9), but I did **not** establish: whether "Full
   MMU" is auto-enabled or per-game; whether the arm64 dynarec supports
   translation or falls back to the interpreter; and known accuracy bugs.
   *Next step:* read `core/hw/sh4/modules/mmu.cpp` and `core/hw/sh4/dyna/` at
   <https://github.com/flyinghead/flycast>, and search its issues for "MMU".
   A background agent was still running on this when work stopped.
   **This does not affect the verdict** — the verdict rests on backing-store
   arithmetic, not emulator support.
2. **No empirical MMU test was run.** *Next step, if reopened:* a KOS program
   calling `mmu_init()`, mapping one page via a custom `mmu_mapfunc_t`, and
   reading through it under `harness/dc/smoke.sh`, then on real hardware.
   Deliberately skipped — no result from it could move a verdict that is
   decided by the CD's 500 KB/s.
3. **SH-4↔VRAM read bandwidth is still unmeasured**, as
   `kb/research-size-reduction.md` §5.1 already flags. §6 here assumes VRAM
   reads are slow enough that a cacheable MMU alias would not rescue them;
   that assumption is **[R], not measured**.
4. **Uncached-access penalty not quantified.** §3.1 asserts the uncached
   forcing is a large framerate cost without a cycle number. *Next step:* a
   cached-vs-uncached `memcpy` microbenchmark on hardware. Only matters if
   someone reopens this.

---

## Sources

Read directly in `opencrossing-dc:sdk` (KOS 2.3.0, `1c6398f9`) [M]:
- `kernel/arch/dreamcast/kernel/mmu.c` — page table, fault handlers, static maps
- `kernel/arch/dreamcast/include/arch/mmu.h` — API, `mmupage_t`, KOS-MMU history
- `kernel/arch/dreamcast/kernel/entry.s` — vector table, `_irq_save_regs`, the
  commented-out `tlb_miss_hnd`, SR/IMASK manipulation
- `kernel/arch/dreamcast/kernel/itlb.s`, `irq.c`, `startup.S`
- `kernel/arch/dreamcast/hardware/sq.c` — MMU-aware store queues
- `kernel/arch/dreamcast/include/dc/memory.h` — P0/P1/P2/P4 areas
- `examples/dreamcast/basic/mmu/{pvrmap,nullptr}/`

External [S]:
- **SH7750 Series Hardware Manual, Rev. 6.0 (07/02)** — the authoritative doc,
  and the source of every SH-4 quote above.
  <https://docs.rs-online.com/ba59/0900766b80c2b5e1.pdf>
  Sections used: §3.1.1 p. 43 (page sizes, ITLB/UTLB counts) · §3.3.5 p. 70 &
  §3.6.2 p. 82 (software-only refill, hardware actions on miss) · §3.5.3 p. 78
  (LDTLB from P1/P2) · §3.5.4 p. 79 (hardware ITLB refill from UTLB) · §3.5.5
  p. 80 (synonym / VA[13:12] rule) · §4.7.3–4.7.6 pp. 122–125 (SQ under AT=1,
  QACR dead, SQ fault + corruption errata) · §5.5.3 p. 135 (BL=1 → reset) ·
  Table 5.2 pp. 130–132 (vectors, multiple-hit → reset) · §8 p. 206 and p. 215
  (no exception cycle count; LDTLB 1 cycle, RTE 5) · Table 1.1 p. 7 and §14.1.1
  p. 488 (DMAC is physical-address-only)
- SH-4 Hardware Manual (mc.pp.se Dreamcast mirror) — <https://mc.pp.se/dc/files/h14th002d2.pdf>
- NetBSD SH-4 asm TLB-miss fast path (41 instructions) —
  <https://github.com/NetBSD/src/blob/trunk/sys/arch/sh3/sh3/exception_vector.S>
- Linux SH-4 refill + MMUCR.RC initial-page-write erratum —
  <https://github.com/torvalds/linux/blob/master/arch/sh/mm/tlbex_32.c>,
  <https://github.com/torvalds/linux/blob/master/arch/sh/mm/tlb-sh4.c>
- KOS README, "nothing really uses it at this point" —
  <https://kos-docs.dreamcast.wiki/md_doc_2README.html>
- Flycast MMU module — <https://github.com/flyinghead/flycast> (`core/hw/sh4/modules/mmu.cpp`)

Internal:
- `kb/research-size-reduction.md` §5.1–5.3 (VRAM/AICA/SQ), §3.7 (overlays)
- `kb/asset-pack.md`, `kb/STATE.md` (the plan this would have replaced)
