# Budget premises §1 — the corrected budget and the honest fit test

§1 of `kb/research-budget-premises.md`, moved verbatim: why the old "image
budget" double-counted ~3.77 MB, the single-inequality restatement, the ~11.07 MB
required cut, and the binding `.bss` constraint. Read before quoting any RAM
total. **Contingent on bucket 6 = 4 MB, which is still [?]** — see
`kb/research-budget-bucket6.md`.

> ## ⚠️ [STALE 2026-08-06] — §1.4's binding constraint is VOID
>
> §1.4 concludes "**with `-O0` frozen `.text` cannot help**". `-O0` is no longer
> frozen. The directive was reversed on 2026-08-06: `src/` builds at `-Os` with
> a 14-TU `-O3` hot list (`DC_OPT_PROFILE=perf`, the default; `size` = `-Os`
> everywhere; `o0` = byte-identical revert); `dc/src` moved from `-O2` to
> `-O3`. Measured on matched town windows of the shipping build: `.text`
> **5,506,964 → 2,753,700 B** (2,680,676 at flat `-Os`), `.data` **2,337,980 →
> 2,224,832 B**, `.bss` unchanged (3,945,356 → 3,945,484 B).
>
> **`.text` helped, by 2,826,288 B at flat `-Os`** — more than twice the
> 1,349,044 B of `.bss` headroom §1.4 computes, and about as much as every
> `.bss` lever the project has landed put together. Concretely:
>
> - §1.4's `8,957,420 = .text-equivalent + .data` is `-O0` arithmetic. Both
>   terms fell. **The `.bss` headroom it derives is therefore much larger than
>   1,349,044 B — recompute it from a current link; do not patch the number
>   here, and do not subtract the shipping town figures from the full-asset
>   figures, which are different build lines.**
> - The **−89.1 % `.bss` requirement is likewise stale** for the same reason.
> - §1.3's ~11.07 MB required cut and §1.2's fit inequality are stale in the
>   same way. The *form* of the fit test — one inequality, never two pools — is
>   still right and still the rule.
> - Everything not derived from `.text`/`.data` stands: the bucket-1
>   double-count, buckets 9/10/11 being `.bss`, the `s_assets[]` line, and the
>   1,294,497 B of dead XFB/FIFO.
>
> Evidence: the 2026-08-06 entry of `kb/state-log.md`.

---

## 1. Corrected budget — the headline

### 1.1 The claimed budget was wrong in kind, not just in magnitude

The old derivation subtracted a list of "heap buckets" from 16 MB and called
the remainder an image budget. **Three of those buckets are not heap at all —
they are `.bss` inside the image — and a fourth double-counts bytes that are
already in the image.** Subtracting them a second time understated the budget
by ~3.77 MB.

| Input | Claimed | Corrected | Tag | Why |
|---|---:|---:|---|---|
| Physical RAM | 16,777,216 | 16,777,216 | [S] | `_arch_mem_top = 0x8d000000`, `arch.h:42` |
| Low RAM below load address | −65,536 | −65,536 | [S] | image loads at `0x8c010000`; `arch.h:297` rejects anything lower |
| Kernel stack hard reserve | −65,536 | −65,536 | [S] | `mm.c:52` refuses `sbrk` past `_arch_mem_top − THD_KERNEL_STACK_SIZE`; `stack.h:52` = 64 KB |
| **= usable for image + heap** | **16,646,144** | **16,646,144** | [D] | unchanged, and correct |
| Bucket 1 KOS | −1,000,000 | **−262,144** | [M]/[S] | **double-counted.** KOS+newlib+libstdc++ are *inside* our ELF: 304,829 B measured from the map. Only ~151 KB is genuinely additive heap. §3.2 |
| Bucket 6 JKRHeap/`__osMalloc` arena | −4,000,000 | −4,000,000 | **[?]** | genuinely heap-allocated. **True need still unmeasured** — but ≥1,294,497 B of it is provably dead. §2 |
| Bucket 7 asset pool | −1,500,000 | −1,500,000 | [D] | legitimate, but *future*: today those bytes are `.bss` in the image, so counting both is double-counting until the conversion lands. §3.4 |
| Bucket 8 ARAM graph window | −512,000 | −512,000 | **[?]** | genuinely heap-allocated (`dc_aram.c:49`). Size unmeasured. |
| Bucket 9 audio 700,000 | −700,000 | **−0** | [M] | **not heap.** The buffers are static `.bss`: `jaudio_NES` = 1,265,101 B, already in the image |
| Bucket 10 disc I/O 384,000 | −384,000 | **−0** | [M] | **not heap.** `dc_dvd.c.o` `.bss` = 13,320 B, in the image. No read-ahead ring exists yet |
| Bucket 11 PVR staging 384,000 | −384,000 | **−0** | [M] | **not heap.** `g_gx` = 334,764 B of `.bss` in `dc_gx.c.o`, in the image |
| Bucket 12 stacks 131,072 | −131,072 | −65,536 | [S] | KOS's 64 KB kernel stack is already subtracted above; idle/reaper stacks are 1,024 B of `.bss`. Reserve 64 KB for future app threads |
| **= image budget** | **8,035,072** | **10,306,464** | [D] | |

### 1.2 The honest fit test

Splitting one pool into "image budget" and "heap budget" is what caused the
error. State it as a single inequality instead:

```
(image span) + (genuinely additive heap) ≤ 16,646,144        [D]
```

| | bytes | tag |
|---|---:|---|
| image span today (`0x8c010000` → `_end 0x8d472814`) | **21,374,996** | [M] |
| additive heap today (KOS 262,144 + arena 4,000,000 + ARAM window 512,000 + threads 65,536) | **4,839,680** | [M]/[?] |
| usable | 16,646,144 | [D] |
| **over by** | **9,568,532** | [D] |

Adding the future 1.5 MB asset pool (the change that also removes 8.45 MB of
`.bss`, so it cannot be counted without its own credit):

| | bytes |
|---|---:|
| image budget with asset pool reserved | **10,306,464** [D] |
| image today | 21,374,996 [M] |
| **required cut** | **11,068,532** (10.56 MiB) [D] |

### 1.3 So: is the cut 14.45 MB, 12 MB, or 16 MB?

**It is ~11.07 MB, not 14.45 MB — and about 1.1 MB of that difference is
simply that the 14.45 MB figure is stale.**

| claim | bytes | status |
|---|---:|---|
| `kb/STATE.md` "image 22,486,548, cut 14,451,476" | 14,451,476 | **stale.** The linked ELF on disk is 21,374,996 — the §8.3 fixes already landed. Overstates by 1,111,552 [M] |
| same budget, current image | 13,339,924 | arithmetically right, **budget wrong** (§1.1) |
| **corrected** | **11,068,532** | [D], contingent on bucket 6 = 4 MB [?] |
| corrected, if the dead XFB/FIFO allocations in §2.2 are removed | **9,774,035** | [D] |

**≈3.4 MB was found, and none of it came from making anything smaller.** It
came from the budget double-counting bytes. That is the good news; §1.4 is the
bad news.

### 1.4 What did NOT improve, and it is the binding constraint ⚠️ [STALE 2026-08-06 — it improved; see the banner]

The coordinator's arithmetic is correct and survives this audit:

```
.text-equivalent (text+rodata+init/fini/ctors/eh_frame)   6,318,568  [M]
.data                                                      2,638,852  [M]
                                                      = 8,957,420
image budget (corrected)                                  10,306,464  [D]
⇒ headroom for ALL of .bss                                 1,349,044  [D]
.bss today                                                12,415,508  [M]
```

So the corrected budget buys **1,349,044 B of `.bss` headroom instead of a
negative number** — the port goes from arithmetically impossible to merely
very hard. But `.bss` must still fall from 12,415,508 to ~1.35 MB, i.e.
~~**−89.1 %**, and with `-O0` frozen `.text` cannot help.~~

⚠️ **[2026-08-06] `.text` can help and did.** `-Os` + a 14-TU `-O3` hot list
took `.text` from 5,506,964 to 2,753,700 B on the shipping town build, and
`.data` from 2,337,980 to 2,224,832 B. Both terms of the 8,957,420 B line above
fell, so the 1,349,044 B headroom and the −89.1 % `.bss` requirement are both
`-O0`-era. **Recompute them from a current link — do not mix the town-build
figures into this full-asset arithmetic.** `kb/state-log.md`, 2026-08-06.

A useful decomposition of the 11.07 MB cut against the ranked plan in
`kb/research-size-reduction.md` §6.2:

| lever | Δ | status |
|---|---:|---|
| `src/data` `.bss` demand residency | −8,449,191 | unbuilt; **76 % of the cut** |
| `s_assets[]` string pool → disc index | −888,853 | [M] confirmed exactly, §3.3 |
| `src/data` `.data` eviction (pointer-free first) | −948,688 [?] | figure not re-verified, §6.4 |
| dead XFB + GX FIFO (heap side, §2.2) | −1,294,497 | [M]/[D] new this pass |
| **subtotal** | **−11,581,229** | vs 11,068,532 required |

It closes with a 512,697 B margin (4.6 %) — thinner than it looks, because the
largest line is unbuilt and the second-largest is unverified.

⚠️ **[2026-08-06] this decomposition is missing its second-largest lever**,
because that lever was banned when it was written: `-Os` + a 14-TU `-O3` hot
list, worth 2,826,288 B of `.text` at flat `-Os` (2,753,700 B shipping figure
vs 5,506,964 B at `-O0`), plus 113,148 B of `.data`. It would rank second in
the table above, behind `src/data` demand residency and ahead of everything
else. The required-cut side of the comparison moved too, so **rebuild the whole
decomposition rather than inserting a row.** `kb/state-log.md`, 2026-08-06.
