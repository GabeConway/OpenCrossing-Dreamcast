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

## 1. Correcting the record

`kb/research-size-reduction.md` §1 and `kb/STATE.md` state "no MMU, no lazy
commit" as a structural fact. **That is right about KOS's default and wrong
about the hardware.** The correct statement is:

> KOS ships a complete SH-4 MMU driver, off by default and opt-in. `arch_main`
> never calls `mmu_init()`; `startup.S` explicitly disables the MMU and
> invalidates the TLB on exit. [M] The MMU is available. It just cannot help
> with *this* problem.

`kb/STATE.md`'s "Dead ends" list should gain this document by reference. The
underlying claim — that `.bss` bytes destroy heap bytes because `mm_sbrk()`
starts at `end` — remains true and unaffected.

---

## 2. Page size and TLB reach — 253,952 B [M]

SH-4 supports 1 KB / 4 KB / 64 KB / 1 MB pages, a 4-entry ITLB and a 64-entry
fully-associative UTLB — *SH7750 Series Hardware Manual* Rev. 6.0 Table 1.1
p. 4 and §3.1.1 p. 43, *"Supports multiple page sizes: 1 kbyte, 4 kbytes,
64 kbytes, 1 Mbyte … 4-entry fully-associative TLB for instructions …
64-entry fully-associative TLB for instructions and operands."*
<https://docs.rs-online.com/ba59/0900766b80c2b5e1.pdf> [S]

TLB refill is **100 % software** — there is no hardware page-table walker.
§3.6.2 p. 82: *"Software is responsible for searching the external memory page
table and assigning the necessary page table entry."* The TTB register has no
hardware meaning at all (p. 63: *"This register can be freely used by
software."*), so the page-table format is entirely ours. The only hardware walk
is UTLB→ITLB. [S] That part is good news, and it is why a custom
`mmu_mapfunc_t` is architecturally sufficient in principle (§3).

**KOS's page-table path supports only 4 KB**, hardcoded in two places in
`kernel/arch/dreamcast/kernel/mmu.c`:

```c
/* mmu_page_map_single(), line 279 — BUILD_PTEL(PA, V, SZ, PR, C, D, SH, WT) */
page->ptel = BUILD_PTEL(page->physical << PAGESIZE_BITS, 1, 1, page->prkey,
                        page->cache, page->dirty, page->shared, page->wthru);
/*                                          ^ SZ = 1 = 4 KB, literal        */
```

and structurally in the two-level table itself — `MMU_IND_BITS 12`,
`MMU_TOP_MASK GENMASK(30, 21)`, `MMU_BOT_MASK GENMASK(20, 12)`. The 12-bit
split is baked into `map_virt()`, `mmu_page_map_single()`, `mmu_copyin()` and
`mmu_copyv()`. Larger pages are reachable **only** through
`mmu_page_map_static()`, which is a different thing entirely (§6). [M]

Usable UTLB entries, from `mmu_init_basic()`: it burns entries 63 and 62 on the
store queues and leaves `URB = 61`, so random replacement cycles entries 0–61.
**62 entries × 4,096 B = 253,952 B of reach**, against 8,617,214 B of asset
arrays — **2.9 %**. [M/R]

There is no configuration that gives large-page demand paging. You get either
62 × 4 KB of *faultable* reach, or up to 62 × 1 MB of *pinned, never-faulting*
static remapping (§6). Getting 64 KB or 1 MB demand pages means rewriting KOS's
page table, not configuring it.

---

## 3. Is the KOS fault path complete enough? — it can fill, it cannot evict [M]

**Yes, a custom `mmu_mapfunc_t` can synchronously return a PTE.**
`mmu_gen_tlb_miss()` reads the faulting address from TEA, calls `map_func`, and
`ldtlb`s whatever comes back:

```c
void mmu_gen_tlb_miss(const char *what, irq_t source, irq_context_t *context) {
    addr = *tea;
    ...
    page = map_func(mmu_cxt_current, addr >> PAGESIZE_BITS);
    if(!page) { dbgio_printf(...); unhandled_mmu(source, context); }
    ...
    ptehv = page->pteh | mmu_cxt_current->asid;
    ptelv = page->ptel;
    mmu_ldtlb_quick(ptehv, ptelv);
}
```

So the *fill* half is genuinely there, and `examples/dreamcast/basic/mmu/
nullptr/nullptr.c` demonstrates a custom callback. [M] But everything else a
pager needs is missing:

| Required | State in KOS 2.3.0 | Evidence [M] |
|---|---|---|
| Fill a PTE on fault | **present** | `mmu_gen_tlb_miss()` above |
| **Unmap / evict a page** | **absent — no API at all** | no `mmu_page_unmap`; nothing ever clears `page->valid`; only `mmu_reset_itlb()` exists and it clears the *ITLB* (4 entries), not the UTLB |
| **Dirty-bit tracking** | **absent** | `page->dirty = 1;` unconditionally, with `/* XXX Initial-write exception not called */` |
| Write-fault (for dirty/COW) | **panic stub** | `initial_page_write()` → `dbgio_printf` + `unhandled_mmu` → `arch_panic` |
| Protection violation recovery | **panic stubs** | `itlb_pv`, `dtlb_pv_read`, `dtlb_pv_write` — all `arch_panic` |
| Cacheable paged memory | **forced off** (§3.1) | all four `page_cache_t` cases set `page->cache = 0` |
| `mmu_phys_to_virt()` | **declared, never defined** | in `arch/mmu.h:255`, absent from `mmu.c` and from `mmu.o`'s symbol table |

Without eviction and dirty tracking there is no pager — only a one-way
populate. To evict you would hand-write the UTLB address array at
`0xF6000000` yourself. [R]

Two latent bugs found while reading, worth recording regardless of verdict:

- `mmu_gen_tlb_miss()` guards `mmu_cxt_current == NULL` **only when
  `map_func == map_virt`**, then unconditionally dereferences
  `mmu_cxt_current->asid`. A custom callback without an installed context is a
  NULL dereference *inside the fault handler*. [M]
- `mmu_page_map_static()` alignment check is
  `if(virt & phys & page_mask[page_size])` — `&` where `|` was meant; a
  misaligned `virt` passes as long as `phys` is aligned. [M]

### 3.1 Every dynamically mapped page is uncached [M]

`mmu_page_map_single()` ignores its `page_cache_t` argument:

```c
switch(cache) {
    case MMU_NO_CACHE:   page->cache = 0; break;
    case MMU_CACHE_BACK: page->cache = 0; page->wthru = 0; break;  /* XXX tmp */
    case MMU_CACHE_WT:   page->cache = 0; page->wthru = 1; break;
    default:             page->cache = 0; page->wthru = 0; break;
}
```

`BUILD_PTEL` places this in PTEL bit 3 (C), so C=0 → non-cacheable on every
path, including `MMU_CACHEABLE`. KOS's own `pvrmap` example passes
`MMU_NO_CACHE`, so this has never been exercised. [M]

Consequence: the 8.5 MB of model/texture data — read every frame by the emu64
GBI interpreter and the GX layer — would be read **uncached**, losing both the
32-byte line fill and all spatial locality on data that is scanned
sequentially. This is a large framerate cost on a machine that has no headroom,
and it is unfixable without patching KOS. [R]

---

## 4. Fault context: interrupts are masked — CD-backed paging is impossible [M]

Two facts combine into a hard structural block.

**(a) TLB misses take the full, slow exception path.** The dedicated fast
handler `tlb_miss_hnd` in `entry.s` is **commented out of the vector table**:

```
_vma_table_400:		! TLB miss exceptions (MMU)
	nop
!	bra	tlb_miss_hnd
!	nop
	bra	_irq_save_regs
	mov	#2,r4			! Set exception code
```

(This is a mercy — the fast path saves only R15 and clobbers PR via its
`jsr`, on a 256-byte global stack, with SR.BL still set.) [M]

**(b) `_irq_save_regs` masks all interrupts.** After saving context it sets SR
with `and 0xefffff0f` then `or 0x000000f0` — clearing BL (bit 28) so nested
*exceptions* are legal, and setting IMASK to 15, **masking every interrupt**.
The source comment says so: *"re-enable exceptions (but not interrupts) so we
can still debug inside handlers."* [M]

Therefore the fault handler — and any `mmu_mapfunc_t` it calls — runs with all
interrupts masked. It cannot:

- wait on a GD-ROM read (KOS's driver completes on interrupt/thread);
- wait on a DMA completion IRQ;
- take a KOS mutex or semaphore, or call `malloc()` (which takes one) — note
  `mmu_page_map_single()` **calls `malloc()`** to lazily allocate subcontexts,
  so subcontexts must be pre-populated;
- block or yield in any way.

**A CD-backed pager must fetch its page from inside this handler. It cannot.**
Short of writing a polled, interrupt-free GD-ROM driver and spinning ~8 ms with
interrupts off — which stalls audio and vblank — there is no route. [R]

There is also a reentrancy hazard: `_irq_save_regs` uses a **single global
register-save table** (`_irq_srt_addr`) and a **single 4 KB static kernel
stack** (`krn_stack`), justified in-source by *"only one thread will ever
actually be sitting inside the kernel code."* A TLB miss taken inside an
interrupt handler is a nested exception that overwrites the outer saved
context. With 8.5 MB of paged data, any IRQ handler touching it corrupts the
interrupted thread. [M/R]

---

### 4.1 Three hardware hazards that make a hand-rolled pager worse than it looks

All from the *SH7750 Series Hardware Manual* Rev. 6.0. [S]

- **A fault inside the TLB-miss handler is a silent reset, not a nested
  exception.** Hardware sets SR.BL=1 on exception entry, and §5.5.3 p. 135:
  *"When the BL bit in SR is 1 and an exception other than a user break is
  generated, the CPU's internal registers … are set to their states following a
  manual reset, and the CPU branches to the same address as in a reset
  (H'A000 0000)."* KOS survives this because `_irq_save_regs` writes only into
  P1 memory before clearing BL — but it means the handler, its literal pool,
  its stack and everything it dereferences must be untranslated. [S/R]
- **A UTLB multiple hit is also a reset** (Table 5.2, EXPEVT H'140 → vector
  H'A000 0000), and SH-4 has an erratum that makes duplicates easy to create:
  on an initial-page-write exception it *"does not set MMUCR.RC to the
  corresponding TLB entry … we need to flush it in order to avoid potential
  TLB entry duplication"* — Linux `arch/sh/mm/tlbex_32.c`.
  <https://github.com/torvalds/linux/blob/master/arch/sh/mm/tlbex_32.c> [S]
  KOS's `initial_page_write` handler is a panic stub (§3), so this is unhandled.
- **Cacheable aliases must agree on VA bits [13:12].** §3.5.5 "Avoiding Synonym
  Problems" p. 80: *"When address translation information whereby a number of
  4-kbyte page UTLB entries are translated into the same physical address is
  recorded in the UTLB, ensure that the VPN [13:12] values are the same."* A
  pager that recycles physical frames across different virtual pages violates
  this constantly and must flush the operand cache on every remap — which
  cancels the benefit of caching them in the first place. Moot while KOS forces
  C=0 (§3.1), but fatal to the obvious "just patch KOS to allow caching" fix.
  64 KB and 1 MB pages are immune, and KOS cannot demand-page those (§2). [S]

KOS does comply with the two `LDTLB` sequencing rules — §3.5.3 p. 78 (*"ensure
that it is issued by a program in the P1 or P2 area"*; `mmu.c` links into P1)
and §3.6.2 p. 82 (*"The RTE instruction should be issued at least one
instruction after the LDTLB instruction"*; KOS's C epilogue is far longer than
one instruction). [S/M]

---

## 5. Cost per TLB miss — ~2–4 µs, and it is *not* the problem [M/R]

Measured instruction counts in the actual path, from
`sh-elf-objdump -d entry.o`: [M]

| Segment | Instructions |
|---|---:|
| `_irq_save_regs` (incl. 16 `fmov` double-stores across both FP banks, `movca.l` cache pre-allocation loop) | **75** |
| `_save_regs_finish` → `rte` | **48** |
| `irq_handle_exception` (dispatcher, `0x14c` B ≈ 166 insns, partially executed) | ≤166 |

Plus `dtlb_miss_read` → `mmu_gen_tlb_miss` → callback → `ldtlb` → `RTE`
(5 cycles, p. 215 [S]). Round trip **~400–800 cycles ≈ 2–4 µs at 200 MHz.** [R]
Side cost: the `movca.l` loop pre-allocates 8 cache lines and the 240-byte
context write lands in D-cache — ~1.5–3 % of the 16 KB D-cache displaced per
fault. [R]

For calibration: **the SH-4 manuals publish no exception-entry cycle count at
all** (searched both; §8 p. 206 only says *"the number of penalty cycles … is
largely dependent on the user's memory subsystems"*). The best external
reference point is NetBSD's hand-written SH-4 asm fast path,
`sh4_vector_tlbmiss` — **41 instructions** including `LDTLB`, its mandatory
padding `nop`, and `RTE`, of which 2 are serially dependent page-table loads.
<https://github.com/NetBSD/src/blob/trunk/sys/arch/sh3/sh3/exception_vector.S>
[S] **KOS's path is ~3× that in instruction count** and additionally goes
through a C dispatcher, so the estimate above is if anything optimistic. Linux's
SH-4 refill is slower still — it has no asm fast path and walks a 5-level page
table in C. [S]

Put against the alternatives, this is the honest ranking:

| Per 4 KB page | Cost | vs 33.3 ms frame |
|---|---:|---:|
| TLB miss, page already resident | ~2 µs | 0.006 % |
| Fetch from VRAM (if spare existed) | ~30 µs [R, unmeasured] | 0.09 % |
| **Fetch from CD-R, zero seek** | **8,192 µs** | **24.6 %** |
| Fetch from CD-R with a seek | +tens of ms [R] | >100 % |

**The fault mechanism is affordable. The only backing store big enough is
4,000× more expensive than the fault.** Adding an MMU to a CD-bound problem
does not make the CD faster.

---

## 6. Static remapping (`mmu_page_map_static`) adds nothing over P2 [M/R]

The one MMU mode that *is* mature — `mmu_init_basic()` + `mmu_page_map_static()`,
pinned 1 MB entries, no exception handlers, no faults — was worth checking
separately, because it could in principle expose VRAM or AICA RAM as ordinary
linker-placeable memory.

It adds nothing, because **SH-4 P1/P2 already do this with the MMU off**:

| Target | Physical | Already addressable without MMU | What static MMU mapping would add |
|---|---|---|---|
| VRAM 64-bit area | `0x05000000` | `0xA5000000` (P2, uncached) | only a *cacheable* alias — incoherent with PVR writes, and the project budgets **0 MB spare VRAM** until `pvr_mem_available()` is measured |
| AICA RAM | `0x00800000` | `0xA0800000` (P2) | nothing — the MMU cannot fix G2's FIFO and access-size rules, and 2 MB < 8.5 MB anyway |

`kb/research-size-reduction.md` §5.1 already documents the correct mechanism
for VRAM placement — a `.vram_bss 0xa5000000 (NOLOAD)` output section, modelled
on KOS's own `.ocram` — which achieves the identical result with **zero TLB
entries, zero runtime cost, and no MMU**. [S, that doc] The MMU is strictly
redundant here. §5.2's conclusion that AICA RAM cannot host a C array stands,
and is not an MMU-addressable limitation.

---

## 7. Store queues — KOS handles the switch; the silicon is the problem

**The API side is fine.** §4.7.3 pp. 122–123 confirms exactly why
`mmu_set_sq_addr` exists: *"When MMU is on — the SQ area (H'E000 0000 to
H'E3FF FFFF) is set in VPN of the UTLB, and the transfer destination external
memory address in PPN … When MMU is off — external memory address bits [28:26]
… are generated from the QACR0/1 registers."* **QACR0/1 stop working the moment
MMUCR.AT=1**, which silently breaks every piece of Dreamcast SQ code written
against the MMU-off model. [S]

KOS 2.3.0 already handles this correctly and transparently in
`kernel/arch/dreamcast/hardware/sq.c`:

```c
with_mmu = mmu_enabled();
mask = with_mmu ? 0x000fffe0 : 0x03ffffe0;
if(with_mmu) mmu_set_sq_addr(dest);
else         SET_QACR_REGS(dest, dest);
```

`mmu_init_basic()` reserves UTLB entries 62/63 as two 1 MB pages at
`0xe0000000`/`0xe0100000`, and `mmu_set_sq_addr()` rewrites their PPNs directly
in the UTLB data array. `sq_cpy()` is aware of the resulting constraint —
*"Transfer maximum 1 MiB at once. This is because when using the MMU the SQ area
is 2 MiB, and the destination address may not be on a page boundary."*
`pvr_scene.c:198` goes through `sq_lock((void *)PVR_TA_INPUT)`, so the PVR
submission path is covered. [M] Cost: 2 of 64 UTLB entries permanently gone, and
2 UTLB data-array writes per `sq_lock()`. Our `dc/src/` uses no SQ or PVR DMA
yet, so nothing is affected today. [M]

**But the silicon adds two hazards that KOS cannot paper over**, both in
§4.7.4–4.7.6 pp. 124–125: [S]

- SQ accesses are subject to normal translation, so *"a TLB miss exception,
  protection violation exception, or initial page write exception is
  generated"* on SQ writes and on the `PREF` that flushes them — **and
  *"if an exception occurs in an SQ write, the SQ contents may be corrupted"*
  on SH7750/SH7750S.** The Dreamcast is SH7750-class.
- §4.7.6 documents an erratum where *"if an exception occurs within the three
  instructions preceding an instruction that writes to an SQ … a branch may be
  made to the exception handling routine after execution of the SQ write that
  should be suppressed."*

So enabling the MMU converts the renderer's hottest path into one where a
stray TLB miss can silently corrupt a vertex burst. With the MMU off, none of
this can happen. This is a real, permanent tax on **any** MMU use on this
target — including the static-mapping mode of §6 — and it is why "just turn the
MMU on, it's free" is wrong even for the uses that would otherwise be harmless.
[S/R]

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
