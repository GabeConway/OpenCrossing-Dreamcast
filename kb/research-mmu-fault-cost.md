# MMU paging — fault context, hardware hazards, and the cost per TLB miss [DEAD idea]

**VERDICT: DEAD.** MMU paging cannot close any part of the RAM overage: the
MMU cannot create memory, and the only backing store big enough is a 500 KB/s
CD-R (8.19 ms per 4 KB page ≈ 24.6 % of a frame). Nothing in this file reopens
it. Full verdict and the four independent secondary killers:
`kb/research-mmu-paging.md`.

§4, §4.1 and §5 of `kb/research-mmu-paging.md`, moved verbatim: interrupts are
masked in the fault handler so a CD-backed pager is structurally impossible,
three SH7750 hazards, and the ~2–4 µs miss cost that is *not* the problem.
Read when someone claims the fault mechanism is the bottleneck. It is not.

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
