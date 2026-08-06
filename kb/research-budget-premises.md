# Budget premises — audit of the numbers the RAM plan rests on

Written 2026-08-01. **Two of six questions are only partly answered.** §6 lists
exactly what is missing and how to finish it.

Purpose: `kb/mem-budget.md` and `kb/research-size-reduction.md` derive an
"image budget" of **8,035,072 B** and a "required cut" of **14,451,476 B**.
Several inputs to that subtraction had never been measured. This document
re-derives it from measurements.

Tags: **[M]** measured today · **[S]** read from source (KOS/SDK/decomp, cited
file:line) · **[D]** derived arithmetic · **[?]** still unverified.

> ⚠️ **[STALE 2026-08-06] a premise this audit did NOT audit has since been
> falsified: `-O0`.** The directive was reversed on 2026-08-06. `src/` builds at
> `-Os` with a 14-TU `-O3` hot list (`DC_OPT_PROFILE=perf`, the default; `size`
> = `-Os` everywhere; `o0` = byte-identical revert); `dc/src` moved from `-O2`
> to `-O3`. Measured on matched town windows of the shipping build: `.text`
> **5,506,964 → 2,753,700 B** (2,680,676 at flat `-Os`), `.data` **2,337,980 →
> 2,224,832 B**, `.bss` unchanged (3,945,356 → 3,945,484 B). **Codegen was worth
> ~2.75 MB of `.text` — roughly every `.bss` lever this project has landed put
> together.**
>
> This audit took "`.text` + `.data` are frozen at `-O0`" as given and derived
> the required cut and the `.bss` headroom from it. **Every such derivation in
> `-corrected.md` §1.4 and `-actions.md` §4/§7 is now stale.** The parts that
> stand on their own — bucket 1's double-count, buckets 9/10/11 being `.bss`,
> the `s_assets[]` 888,853 B figure, the 1,294,497 B of dead XFB/FIFO — are
> unaffected. The four numbers above are from the *shipping stubbed town* build
> line and must not be substituted into this document's full-asset arithmetic.
> Evidence: the 2026-08-06 entry of `kb/state-log.md`.

---

## This document was split (2026-08-02)

It was 582 lines. The content is unchanged and lives in five parts. Section
numbers inside the parts are the original ones — use this table to resolve a
`§n` reference. **§2.4 (the bucket-6 measurement recipe) and §6 (what is
unfinished) are cited by name from `kb/levers.md` and `kb/ram-plan.md`; both
keep their numbering.**

| part | original sections | contents |
|---|---|---|
| [`kb/research-budget-corrected.md`](research-budget-corrected.md) | §1 | the corrected budget, the single-inequality fit test, the ~11.07 MB cut, the `.bss` constraint |
| [`kb/research-budget-bucket6.md`](research-budget-bucket6.md) | §2, incl. **§2.2** and **§2.4** | the 4 MB arena: what it is, the 1,294,497 B of dead XFB/FIFO, why the peak is unknown, and **§2.4 the measurement recipe** |
| [`kb/research-budget-evidence.md`](research-budget-evidence.md) | §3, incl. **§3.6** | KOS memory model, bucket 1 double-count, GLdc, buckets 9/10/11 are `.bss`, archives, `s_assets[]` 888,853 B, `.bss` composition |
| [`kb/research-budget-actions.md`](research-budget-actions.md) | §4, §5, §7 | revised ledger for `dc_mem_budget.h`, ranked cheapest actions, bottom line |
| [`kb/research-budget-unfinished.md`](research-budget-unfinished.md) | §6, incl. **§6.2** | the six open questions and their next steps |
