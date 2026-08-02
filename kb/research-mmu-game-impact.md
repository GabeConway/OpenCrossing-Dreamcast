# MMU paging — what it would do to `src/`, and whether `.text` can leave RAM [DEAD idea]

**VERDICT: DEAD.** MMU paging cannot close any part of the RAM overage: the
MMU cannot create memory, and the only backing store big enough is a 500 KB/s
CD-R (8.19 ms per 4 KB page ≈ 24.6 % of a frame). Nothing in this file reopens
it. Full verdict and the four independent secondary killers:
`kb/research-mmu-paging.md`.

§8 and §11 of `kb/research-mmu-paging.md`, moved verbatim: the five places a
P0-mapped `src/data` breaks semantically (DMA first), and why code paging is
*more* dead than data paging — plus the finding that `.text` proper (5,257,344 B)
does not have to leave RAM. Read if you are tempted to relocate a section.

---

## 8. Could the game code stay untouched? — mechanically yes, semantically no

**Mechanically yes.** A linker-script rule placing `*src/data/*(.bss .bss.*)`
into an output section at a P0 VMA is a layout change, not a codegen change, and
needs no edit to `src/`. It is legal under the `-O0` directive. [R]

**Semantically it breaks in five places**, in rough order of severity: [R]

1. **DMA bypasses the MMU — cited, not assumed.** The SH-4's own DMAC is listed
   as a *"Physical address DMA controller"* (Table 1.1 p. 7), with *"Physical
   address space"* as a headline feature (§14.1.1 p. 488), and SAR/DAR are raw
   29-bit external addresses (p. 497). There is no TLB path for DMA at all. [S]
   The Dreamcast's PVR/TA, AICA and G2 devices are external bus masters and sit
   even further outside the CPU's MMU. [R] KOS derives DMA physical addresses by
   masking a P1/P2 pointer with `MEM_AREA_CACHE_MASK` (`0x1fffffff`); masking a
   P0 virtual pointer yields the virtual address, not the physical one. Texture
   upload out of these very arrays is exactly the affected path. Silent
   corruption, not a crash. Any page that is a live DMA target would have to be
   **pinned and hand-flushed**, which for 8.5 MB of texture source data means
   pinning most of it — i.e. no paging.
2. **Everything is uncached** (§3.1), on the hottest data in the game.
3. **Interrupt-context faults** corrupt the single global exception save area
   (§4).
4. **`emu64`'s `seg2k0()` bounds check** is `0x80000000..0x83000000`
   (`kb/STATE.md` open question 2). It already fails for KOS's `0x8c......`; a
   P0 address fails it too.
5. **Low P0 addresses make NULL valid.** KOS's `pvrmap` example maps VRAM to
   virtual 0 deliberately. Doing that here would make every `if(ptr)` check in
   3,900 TUs of UB-dependent decomp meaningless. Avoidable by basing the region
   at e.g. `0x10000000`, but it must be a deliberate choice.

---

## 11. Can `.text` leave RAM this way? — no, and it probably doesn't need to

Raised mid-research: `.text`+`.rodata`+`.eh_frame` = 6,318,568 B and
`+ .data` = 8,957,420 B **already exceeds the 8,035,072 B image budget with
`.bss` at zero**, so if `-O0` freezes `.text`, does code have to leave RAM?

**Two answers, and the second one matters more.**

**(a) MMU paging cannot move `.text` either.** [R] Same blocker, worse case:

- Instruction fetch through a translated region uses the **4-entry ITLB**
  backed by the 62 usable UTLB entries (§2), so code and data would *share*
  253,952 B of reach against a 5.26 MB `.text`.
- The backing store is still the CD at 8.19 ms per 4 KB page (§5), and code
  has far worse locality than bulk asset data — it faults at arbitrary call
  sites, not at scene boundaries.
- The fault handler still cannot perform I/O (§4).

Code paging is *more* dead than data paging, not less. The right lever for
`.text` remains **ScummVM-style ELF overlays** — already documented in
`kb/research-size-reduction.md` §3.7 with a shipping SH-4 implementation
(`backends/platform/dc/dcloader.cpp`, `R_SH_DIR32`). Note the shared principle:
overlays and `assets.pak` both work because they fetch at **semantic
boundaries in large sequential reads**. Every technique that survives contact
with a 500 KB/s CD has that shape; MMU paging is the one that doesn't.

**(b) The premise overstates the problem — `.text` proper is 5,257,344 B, which
fits.** [M, from `kb/research-size-reduction.md` §1] The 8,957,420 B figure
counts `.rodata` (1,053,740) and `.data` (2,638,852), and the existing plan
already evicts **2,827,080 B of exactly those** — `s_assets[]` name strings
−888,740 (§6.2 #3) and `src/data` tables to disc −1,938,340 (#2), neither of
which is a codegen change. The plan's post-cut targets are `.text` 5,150,000 +
`.rodata` 165,000 + `.data` 700,000 + `.eh_frame` 7,388 = **6,022,388 B**,
leaving ~2.01 MB of the image budget for all remaining `.bss`.

So the correct statement is: **`.text` does not have to leave RAM; `.rodata`
and `.data` do, and the plan already moves them.** Overlays stay on the shelf
as the reserve lever if the `.bss` work lands short — which is exactly where
`kb/research-size-reduction.md` §6.3 already puts them. This finding does not
change the port's viability assessment. [R]
