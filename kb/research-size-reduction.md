# Fitting the image in 16 MB **without changing codegen** — split index

Researched 2026-08-01. Constraint that frames everything below:
**`-O0` is mandatory and non-negotiable.** `-O1`/`-O2`/`-Os`/LTO/per-function
optimize pragmas are ruled out by the user (armhf history: `-O2` = wild-pointer
crash loop from boot, `-O1` = SIGBUS on the intro train). Every technique in
this document is therefore judged first on **"does it change instruction
selection?"** — if yes it is marked **DISQUALIFIED** and reported anyway so
nobody re-discovers it.

This document **supersedes `kb/design-shelf-hazards.md` §3.4** ("ship `-O2`
from day one … `-O0` is not an option on Dreamcast"). That recommendation is
withdrawn as a matter of project policy, not of fact — its measurement (`.text`
−48.3 % at `-O2`) is still correct and is exactly the 3 MB we are choosing not
to take. The consequence is that the whole burden shifts onto `.bss` and
`.data`, and the required cut is **bigger than 6.5 MB**. See §7.

Tags: **[M]** measured today against the real DC ELF, **[S]** sourced to a URL,
**[D]** derived arithmetic, **[?]/[UNVERIFIED]** not confirmed.

⚠️ `kb/levers.md` L3 re-costed every estimate in this document against the real
ELF: **every one was wrong, most by a lot, and two of the stated mechanisms
were impossible.** Take numbers from `kb/levers.md`; take reasoning, mechanism
and sources from here.

| part | sections | contents |
|---|---|---|
| `kb/research-size-baseline.md` | §1, §4 | the measured ELF baseline, why `.bss` is heap under KOS `sbrk`, where the 13.5 MB is, and the three-way conversion that fixes it |
| `kb/research-size-techniques.md` | §2, §3 | the 25-row ranked table with a codegen verdict per row, and the detail behind each (`--gc-sections`, disqualified knobs, `--icf`, strip, compression, romdisk, overlays) |
| `kb/research-size-memory-map.md` | §5 | VRAM, AICA sound RAM, store queues, operand-cache RAM, the low 64 KB |
| `kb/research-size-plan.md` | §6, §7, §8 | the closing arithmetic and what is fragile, the ordered plan, and the full source index |
