# MMU on Dreamcast — static remapping adds nothing, and the store-queue tax [DEAD idea]

**VERDICT: DEAD.** MMU paging cannot close any part of the RAM overage: the
MMU cannot create memory, and the only backing store big enough is a 500 KB/s
CD-R (8.19 ms per 4 KB page ≈ 24.6 % of a frame). Nothing in this file reopens
it. Full verdict and the four independent secondary killers:
`kb/research-mmu-paging.md`.

§6 and §7 of `kb/research-mmu-paging.md`, moved verbatim: why
`mmu_page_map_static()` buys nothing over P1/P2 with the MMU off, and why
enabling the MMU *at all* — including the harmless-looking static mode — taxes
the store queues on SH7750 silicon. Read before proposing any MMU use, paging
or not.

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

> ⚠️ **NO LONGER TRUE, 2026-08-08.** G-C made `dc_pvr.c`'s `emit_projected()`
> write vertices straight into the store queue via `pvr_dr_target()` /
> `pvr_dr_commit()`. `dc/src/` now has a **hard dependency on QACR0/QACR1**, and
> therefore on the MMU staying off — which is what the rest of this page already
> concludes anyway. The kill switch is `-DDC_PVR_NO_DR`. [M]

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
