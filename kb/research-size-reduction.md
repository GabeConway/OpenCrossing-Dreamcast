# Fitting the image in 16 MB **without changing codegen** — split index

> ## ⚠️ [STALE 2026-08-06] — THE TITLE OF THIS DOCUMENT IS NOW A CONSTRAINT WE NO LONGER HAVE
>
> **The `-O0` directive was reversed on 2026-08-06.** `src/` builds at `-Os`
> with a 14-TU `-O3` hot list (`DC_OPT_PROFILE=perf`, the default; `size` =
> `-Os` everywhere; `o0` = byte-identical revert); `dc/src` moved from `-O2` to
> `-O3`. Measured on matched town windows of the shipping build:
>
> | | `-O0` | `-Os` | shipping (`-Os` + `-O3` hot) |
> |---|---:|---:|---:|
> | `.text` | 5,506,964 | 2,680,676 | 2,753,700 |
> | `.data` | 2,337,980 | 2,224,832 | 2,224,832 |
> | `.bss` | 3,945,356 | 3,945,484 | 3,945,484 |
>
> **`.text` fell 2,826,288 B at flat `-Os` (−2,753,264 B in the shipping
> profile). `.data` fell 113,148 B. `.bss` did not move (+128 B).** The lever
> this whole document was written to route *around* turned out to be worth
> roughly as much as every `.bss` lever the project has landed put together.
>
> So read this document for its **mechanisms and sources**, which are unchanged,
> and read the "DISQUALIFIED — changes codegen" verdicts as **history**. Nothing
> below has been deleted. ⚠️ The four numbers above are from the *shipping
> stubbed town* build line; the tables below are full-asset-image numbers. Do
> not substitute one into the other — take the reversal of the argument, not the
> arithmetic. Evidence: the 2026-08-06 entry of `kb/state-log.md`.

Researched 2026-08-01. Constraint that framed everything below — **[VOID as of
2026-08-06, see the banner]**:
~~**`-O0` is mandatory and non-negotiable.**~~ `-O1`/`-O2`/`-Os`/LTO/per-function
optimize pragmas were ruled out by the user (armhf history: `-O2` = wild-pointer
crash loop from boot, `-O1` = SIGBUS on the intro train). Every technique in
this document is therefore judged first on **"does it change instruction
selection?"** — if yes it is marked **DISQUALIFIED** and reported anyway so
nobody re-discovers it. That test no longer disqualifies anything by itself;
the armhf evidence behind it was audited on 2026-08-06 and did not survive
(never reproduced on SH-4, never isolated from `-mcpu`/NEON).

This document said it **superseded `kb/design-shelf-hazards.md` §3.4** ("ship
`-O2` from day one … `-O0` is not an option on Dreamcast"). **That withdrawal is
itself withdrawn.** §3.4 was right: it was overridden as a matter of project
policy, not of fact, and the policy has now been reversed. Its measurement
(`.text` −48.3 % at `-O2`) was the 3 MB we were choosing not to take; we now
take it. The consequence stated here — "the whole burden shifts onto `.bss` and
`.data`, and the required cut is bigger than 6.5 MB" — **no longer holds**: the
`.text`+`.data` side of the image moved by megabytes on its own, without any
`.bss` work at all.

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
