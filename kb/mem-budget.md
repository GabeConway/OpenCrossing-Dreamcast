# RAM budget — 16 MB ledger

**This file is now an index.** The ledger was split so that the part that is
still true is loadable on its own and the part that is dead is quarantined.
⚠️ **The §4 ledger is void** — see `kb/mem-budget-void-ledger.md` for what
killed it and when. Live numbers are in `kb/STATE.md`; the live plan is
`kb/ram-plan.md` and `kb/levers.md`. Content below was moved verbatim; old
section numbers are kept so existing citations still resolve.

> ⚠️ **[STALE 2026-08-06] every part below was measured with `src/` at `-O0`,
> and `-O0` is gone.** `src/` builds at `-Os` + a 14-TU `-O3` hot list
> (`DC_OPT_PROFILE=perf`); `dc/src` is `-O3`. Measured on the shipping town
> build: `.text` **5,506,964 → 2,753,700 B** (2,680,676 at flat `-Os`), `.data`
> **2,337,980 → 2,224,832 B**, `.bss` unchanged (3,945,356 → 3,945,484).
> **Codegen was worth ~2.75 MB of `.text` — roughly every `.bss` lever this
> project has landed put together.** Every `.text` and `.data` line in the parts
> below is therefore `-O0`-era; `.bss` lines are unaffected. The four numbers
> here are shipping-stubbed-town numbers and must not be substituted into the
> full-asset tables below. Evidence: the 2026-08-06 entry of `kb/state-log.md`.

| file | old §§ | status | what it answers |
|---|---|---|---|
| `kb/mem-budget-m1-sh4.md` | §8 | **still true** | the real sh-elf link: section sizes, the `.bss` split, levers applied, dead ends, `--gc-sections`, ~~the rejected optimization table~~ **the optimization table — rejected 2026-08-01, ADOPTED 2026-08-06** — the boot attempt |
| `kb/mem-budget-armhf-working-set.md` | §§1, 3 | armhf-era, superseded in parts | the 65 MB start, the 15.6 MB REL boot transient, boot residency, disc contents, measurement provenance |
| `kb/mem-budget-armhf-binary-size.md` | §2 | armhf-era, superseded in parts | section totals, per-tree attribution, top `.bss` / `.text` symbols, evictable `.data` |
| `kb/mem-ledger-runtime-design.md` | §5 | design, partly landed | `dc/src/dc_mem_ledger.c` — making the budget a runtime object that fails loudly |
| `kb/mem-probe-plan.md` | §6 | unexecuted plan | `PC_MEMPROBE=1` probes for the real arena / ARAM / asset high-water marks |
| `kb/mem-budget-void-ledger.md` | §4, §7 | ⚠️ **VOID** | the 12-bucket ledger and the C1–C11 changes, plus the open-measurement list. Kept for its bucket numbering only |
