# Budget premises §4, §5, §7 — the revised ledger, the cheapest actions, the bottom line

§4, §5 and §7 of `kb/research-budget-premises.md`, moved verbatim: the proposed
replacement for `dc/include/dc_mem_budget.h`, the ranked cheap wins, and the
summary. Read when editing the ledger header or picking the next size cut.
**Do not apply the ledger blind — bucket 6 is still [?]**
(`kb/research-budget-bucket6.md`).

> ## ⚠️ [STALE 2026-08-06] — bucket 3 is no longer "unachievable, fixed at `-O0`"
>
> The `-O0` directive was reversed on 2026-08-06. `src/` builds at `-Os` with a
> 14-TU `-O3` hot list (`DC_OPT_PROFILE=perf`, the default; `size` = `-Os`
> everywhere; `o0` = byte-identical revert); `dc/src` moved from `-O2` to
> `-O3`. Measured on matched town windows of the shipping build: `.text`
> **5,506,964 → 2,753,700 B** (2,680,676 at flat `-Os`), `.data` **2,337,980 →
> 2,224,832 B**, `.bss` unchanged (3,945,356 → 3,945,484 B). **Codegen was
> worth ~2.75 MB of `.text` — roughly every `.bss` lever this project has landed
> put together.**
>
> Two things in this file are void as a result:
>
> 1. **§4 bucket 3** — "unachievable as budgeted. Fixed at `-O0`". It is not
>    fixed and the 6,318,568 B proposal is `-O0`-era.
> 2. **§7's bottom line** — "with `-O0` frozen, `.text` + `.data` = 8,957,420 B
>    leaves 1,349,044 B for all of `.bss`", and "there is no second lever within
>    a factor of eight of `src/data` demand residency". **There is: codegen, at
>    ~2.75 MB.** Within a factor of about three, not eight.
>
> §5's ranked cheap actions (kill the XFB, kill the FIFO, fix bucket 1, delete
> buckets 9/10/11) are unaffected — none of them was a codegen argument, and all
> four are still unspent as far as this document knows. ⚠️ The four numbers above
> are shipping-stubbed-town figures; §4/§7 are full-asset arithmetic. Do not
> substitute. Evidence: the 2026-08-06 entry of `kb/state-log.md`.

---

## 4. Revised ledger (proposed replacement for `dc/include/dc_mem_budget.h`)

Do not apply blind — bucket 6 is still `[?]`.

| # | bucket | current | proposed | tag | note |
|---:|---|---:|---:|---|---|
| 1 | KOS | 1,000,000 | **262,144** | [M] | runtime heap only; image bytes are in buckets 3–5 |
| 2 | low RAM | 65,536 | 65,536 | [S] | not spendable |
| 3 | image `.text`+`.rodata` | 2,600,000 | ~~**6,318,568**~~ **[STALE 2026-08-06]** | [M] | ~~**unachievable as budgeted.** Fixed at `-O0`~~ — **not fixed: `-Os` + `-O3` hot list gives `.text` 2,753,700 B on the shipping build (5,506,964 at `-O0`). Re-derive this row from a current link**; the 888,853 B string-pool lever is independent and still live |
| 4 | image `.data` | 1,800,000 | 700,000 | [?] | needs §6.4 |
| 5 | image `.bss` | 1,950,000 | 1,950,000 | [?] | absorbs buckets 9–11 if they stay static |
| 6 | JKRHeap/`__osMalloc` arena | 4,000,000 | **[?] 2,750,000–5,500,000** | **[?]** | §2. −1,294,497 available free |
| 7 | asset pool | 1,500,000 | 1,500,000 | [?] | future; must be paired with the `.bss` credit |
| 8 | ARAM graph window | 512,000 | 512,000 | [?] | unmeasured |
| 9 | audio | 700,000 | **delete** | [M] | it is `.bss` — fold into 5 |
| 10 | disc I/O | 384,000 | **delete** | [M] | it is `.bss` — fold into 5 |
| 11 | PVR staging | 384,000 | **delete** | [M] | it is `.bss` — fold into 5 |
| 12 | stacks | 131,072 | 65,536 | [S] | KOS's 64 KB is already in bucket 2's line |

Also fix the header's own contract: `dc_mem_ledger.c` should assert
`image_span + additive_heap ≤ 16,646,144`, not
`sum(all buckets) ≤ 16,777,216` — the latter cannot detect the double-count
this audit found.

---

## 5. Cheapest actions, ranked

1. **Kill the XFB allocation** — `TARGET_DC` branch in `JUTXfb::initiate`
   returning a dummy non-NULL pointer. **−1,228,800 B of bucket 6.** No
   measurement needed; the buffers are provably never read (§2.2).
2. **Kill / reclaim the GX FIFO** — **−65,697 B.** §2.2(b).
3. **Fix bucket 1** — **−737,856 B of phantom reservation.** §3.2.
4. **Delete buckets 9/10/11** — **−1,468,000 B of phantom reservation.** §3.4.
5. **Drop `-lGL`** from `dc/Makefile:441` until M2 — cosmetic (0 bytes), but
   it stops the next reader budgeting for GLdc twice.
6. **Then measure bucket 6** (§2.4) before anything else is planned on it.

Items 1–4 are ~3.5 MB of the ~3.4 MB improvement in §1.3 and none of them
requires touching `src/` codegen. ⚠️ **[2026-08-06]** "requires no codegen" is
no longer a selling point — codegen is allowed, and it is the biggest `.text`
lever there is. These four are still worth doing; they are just not privileged
by that argument any more. `kb/state-log.md`, 2026-08-06.

---

## 7. Bottom line

- The required cut is **~11.07 MB**, not 14.45 MB — and **~1.1 MB of that gap
  was just staleness**, not analysis. Roughly 2.3 MB more is real: the budget
  was double-counting.
- **A further 1.29 MB of bucket 6 is provably dead weight** (dead framebuffers
  and a dead GX FIFO) and needs no measurement to remove.
- **But bucket 6's actual requirement is still unknown**, and the only
  historical evidence found — a dead `0x380000` default — points at ~3.5 MB,
  i.e. *against* the hoped-for 1.5 MB. **Do not budget on the optimistic
  reading.**
- ~~The binding constraint is unchanged and unforgiving: with `-O0` frozen,
  `.text` + `.data` = 8,957,420 B leaves **1,349,044 B for all of `.bss`**,
  which is 12,415,508 B today. **`src/data` demand residency is still ~76 % of
  the cut and there is no second lever within a factor of eight of it.**~~
  ⚠️ **[VOID 2026-08-06.]** `-O0` is not frozen. `-Os` + a 14-TU `-O3` hot list
  took `.text` from 5,506,964 to 2,753,700 B and `.data` from 2,337,980 to
  2,224,832 B on the shipping town build, with `.bss` unchanged. Both terms of
  the 8,957,420 B line fell, so the 1,349,044 B headroom is stale — **recompute
  it from a current link.** And **there is now a second lever, at ~2.75 MB**:
  codegen. `src/data` demand residency is still the largest single lever, but it
  is no longer alone by a factor of eight. Evidence: the 2026-08-06 entry of
  `kb/state-log.md`.
