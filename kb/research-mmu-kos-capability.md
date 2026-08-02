# MMU paging — what KOS's MMU can and cannot do [DEAD idea]

**VERDICT: DEAD.** MMU paging cannot close any part of the RAM overage: the
MMU cannot create memory, and the only backing store big enough is a 500 KB/s
CD-R (8.19 ms per 4 KB page ≈ 24.6 % of a frame). Nothing in this file reopens
it. Full verdict and the four independent secondary killers:
`kb/research-mmu-paging.md`.

§1–§3 of `kb/research-mmu-paging.md`, moved verbatim: correcting "no MMU" to
"MMU off by default", page sizes and the 253,952 B TLB reach, and the fault
path — it can fill a PTE, it cannot evict. Read only to answer "does KOS
support X"; the answer does not revive the idea.

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
