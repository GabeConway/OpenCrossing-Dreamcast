# Budget premises — audit of the numbers the RAM plan rests on

Written 2026-08-01. **Two of six questions are only partly answered.** §6 lists
exactly what is missing and how to finish it.

Purpose: `kb/mem-budget.md` and `kb/research-size-reduction.md` derive an
"image budget" of **8,035,072 B** and a "required cut" of **14,451,476 B**.
Several inputs to that subtraction had never been measured. This document
re-derives it from measurements.

Tags: **[M]** measured today · **[S]** read from source (KOS/SDK/decomp, cited
file:line) · **[D]** derived arithmetic · **[?]** still unverified.

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
