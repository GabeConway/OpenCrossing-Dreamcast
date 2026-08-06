# Fitting in 16 MB — the measured baseline and the `.bss` problem

The measured DC ELF section table, why every `.bss` byte is a heap byte under
KOS `sbrk`, and where the 13.5 MB of `.bss` actually is (§1); then the
GameCube inheritance and the standard never-allocate / lifetime-overlap /
demand-residency conversion (§4). Read first for any size work.
Part of `kb/research-size-reduction.md`, whose stub maps every § to its file.

Tags: **[M]** measured today against the real DC ELF, **[S]** sourced to a URL,
**[D]** derived arithmetic, **[?]/[UNVERIFIED]** not confirmed.

⚠️ `kb/levers.md` L3 re-costed every estimate in this document against
the real ELF: **every one was wrong, most by a lot, and two of the stated
mechanisms were impossible.** Use `kb/levers.md` for numbers; use this
document for the reasoning and the sources.

> ⚠️ **[STALE 2026-08-06] this document's `-O0` premise is VOID.** `src/` builds
> at `-Os` + a 14-TU `-O3` hot list (`DC_OPT_PROFILE=perf`); `dc/src` is `-O3`.
> Measured on the shipping town build: `.text` **5,506,964 → 2,753,700 B**
> (2,680,676 at flat `-Os`), `.data` **2,337,980 → 2,224,832 B**, `.bss`
> unchanged (3,945,356 → 3,945,484). **Codegen was worth ~2.75 MB of `.text`,
> roughly every `.bss` lever this project has landed put together.** The
> `.text`/`.rodata`/`.data` rows in §1's tables are `-O0`-era and are kept as
> history; they are also full-asset-image numbers, so do not substitute the four
> above into them. **§4 is unaffected** — `.bss` did not move (+128 B), so the
> demand-residency argument is exactly as valid as it was, just no longer the
> *only* lever. Evidence: the 2026-08-06 entry of `kb/state-log.md`.

## 1. The measured baseline

Subject: `/Users/gabe/Documents/GitHub/OpenCrossing-Dreamcast/dc/build/AnimalCrossing.elf`
(ELF32 little-endian, `Machine: Hitachi SH`, `Type: EXEC`, entry `0x8c010000`,
71,933,072 B unstripped, built 2026-08-01) and its 16.8 MB `.map`. Tools:
`llvm-readelf` / `llvm-nm` from `/opt/homebrew/opt/llvm/bin`. **[M]**

| Section | Addr | Bytes | Loads into RAM? |
|---|---|---:|---|
| `.text` | `0x8c010000` | 5,257,344 | yes |
| `.init` + `.fini` | `0x8c514000` | 96 | yes |
| `.rodata` | `0x8c514064` | 1,053,740 | yes |
| `.eh_frame` | `0x8c615490` | 7,228 | yes |
| `.gcc_except_table` | `0x8c6170cc` | 160 | yes |
| `.ctors` + `.dtors` | `0x8c6171ec` | 72 | yes |
| `.data` | `0x8c617240` | 2,638,256 | yes |
| `.got` | `0x8c89b5f0` | 12 | yes |
| **`.bss`** | `0x8c89b600` | **13,526,548** | **reserved, zeroed at startup** |
| `.ocram` | `0x7c001000` | 0 | no (NOLOAD, off-RAM) |
| `.debug_*`, `.symtab`, `.strtab`, `.comment` | — | 67,433,000 [D] | **no — not `SHF_ALLOC`** |

- **Image span** `0x8c010000` → `_end = 0x8d581c14` = **22,486,548 B (21.44 MiB)** **[M]**
- `_arch_mem_top` on a stock retail DC = `0x8d000000` (16 MB) — hard-coded in
  KOS `arch.h` **[S]**
- KOS `mm_sbrk()` refuses to grow past `_arch_mem_top - THD_KERNEL_STACK_SIZE`;
  `THD_KERNEL_STACK_SIZE = 64 * 1024`. So the **usable ceiling is `0x8cff0000`**. **[S]**
- **Overrun of the usable ceiling: 5,838,868 B, before a single heap byte exists.** **[D]**

### Why every `.bss` byte is a heap byte

KOS's heap is a plain `sbrk` bump starting at the ELF `end` symbol:

```c
/* kernel/mm/mm.c */
int mm_init(void) { sbrk_base = __align_up((uintptr_t)end, 4); return 0; }
void *mm_sbrk(ptrdiff_t increment) {
    ...
    if(new_base >= (_arch_mem_top - THD_KERNEL_STACK_SIZE)) { ...ENOMEM... }
```
— <https://github.com/KallistiOS/KallistiOS/blob/master/kernel/mm/mm.c>,
`_sbrk_r` → `mm_sbrk` in `kernel/libc/newlib/newlib_sbrk.c`. **[S]**

There is **no page table, no MMU use, no lazy commit**: KOS documents itself as
having "NOT: Memory protection" and notes "there is an MMU module for the DC
port, [but] nothing really uses it at this point"
(<https://kos-docs.dreamcast.wiki/md_doc_2README.html>). **[S]**
Consequently `.bss` is not "free until touched" the way it is on Linux —
`malloc` starts *above* it, so 13.5 MB of `.bss` is 13.5 MB of heap destroyed.
This is the single most important structural fact in the document.

### Where the 13.5 MB of `.bss` actually is **[M]**

Parsed from `AnimalCrossing.map`, attributed per input object (totals agree
with the section headers to within 0.01 %: `.text` 5,255,640 / `.bss`
13,515,687 of 13,526,548 — the gap is inter-object alignment padding).

| Source tree | `.text` | `.rodata` | `.data` | **`.bss`** | total |
|---|---:|---:|---:|---:|---:|
| `src/data` (generated asset TUs, 3,211 objects) | 84,296 | 74,233 | 2,248,965 | **8,519,191** | 10,926,685 |
| `src/game` | 1,675,606 | 19,687 | 99,822 | **1,548,194** | 3,343,309 |
| `src/actor` | 1,976,450 | 7,535 | 135,231 | **710,946** | 2,830,162 |
| `src/static` (jaudio_NES, emu64, JSystem) | 556,734 | 22,545 | 36,883 | **1,856,060** | 2,472,222 |
| `pc/src` (still linked) | 41,010 | **893,136** | 48 | 320,158 | 1,254,352 |
| `dc/src` | 30,692 | 17,897 | 860 | 370,230 | 419,679 |
| `src` (root) | 197,934 | 1,173 | 48,070 | 5,412 | 252,589 |
| `src/bg_item` | 186,960 | 394 | 49,044 | 4,700 | 241,098 |
| `src/effect` | 216,986 | 165 | 7,960 | 9,549 | 234,660 |
| `src/system` | 35,274 | 0 | 2,194 | 148,806 | 186,274 |
| all newlib/libgcc/libstdc++/libkallisti | ~180,000 | ~8,000 | ~2,000 | ~9,000 | ~199,000 |

Top single objects by `.bss` **[M]**:

| Bytes | Object | What it is |
|---:|---|---|
| 1,229,348 | `src/game/m_play.c` | `prbuf`, the EFB capture buffer `(2*320)*(2*240)*4` |
| 591,902 | `src/static/jaudio_NES/game/game64.c` | `audiomemory` (`0x90000`) |
| 562,121 | `src/static/libforest/emu64/emu64.c` | `texture_buffer_data` (0x80000) + class |
| 334,508 | `dc/src/dc_gx.c` | GX vertex staging |
| 320,150 | `pc/src/pc_m_card.c` | `l_keepSave`/`l_keepOriginal`/`l_keepMail`/`l_keepDiary` |
| 318,336 | `src/actor/ac_structure.c` | `aSTR_overlay` = 32 × 0x2400 |
| 277,580 | `src/static/jaudio_NES/internal/seqsetup.c` | `seq` |
| 187,328 | `src/game/m_common_data.c` | live save state (keep) |
| 137,864 | `src/actor/ac_boat_demo.c` | overlay staging |
| 132,104 | `src/system/sys_dynamic.c` | GX display-list build buffers (keep) |
| 102,400 | `src/data/model/lat_letter64_xk_tex.c` | *staged asset* — largest of 3,211 |

**63.0 % of `.bss` is `src/data` asset staging.** That is not "runtime state";
it is 16,343 assets that `gen_runtime_assets.py` turned into empty
`ATTRIBUTE_ALIGN(32)` arrays which `pc_assets_init()` fills at boot from the
decompressed REL. It is a static reservation of a thing that should be a cache.
Everything else in this document is a rounding error next to it.

⚠️ **[2026-08-06]** "everything else" no longer includes codegen, which was
excluded from this document by policy rather than measured out of it. At `-Os`
`.text` fell 2,826,288 B — the same order as this line item — so `src/data`
demand residency is now the largest of **two** levers, not the only one.
`kb/state-log.md`, 2026-08-06.

---

## 4. `.bss` — the actual problem, and the standard fix

13,526,548 B, 60.2 % of the image, zero codegen implications. It exists because
the game was written for a machine with **24 MB main + 16 MB ARAM = 40 MB**,
and because the armhf port's `gen_runtime_assets.py` converted compiled-in
asset arrays into *statically reserved* arrays that are filled at boot.

### 4.1 The GameCube inheritance, and why the DC has a real analogue

On GameCube the CPU cannot address ARAM at all; it moves blocks by DMA. Nintendo
shipped a paging library, and "ambitious developers used the 16MB of ARAM as
virtual memory by paging data in and out via Direct Memory Access … accessing
currently unpaged memory and triggering an exception, then their specially
programmed exception handler would map in memory from the ARAM."
<https://www.copetti.org/writings/consoles/gamecube/> **[S]**

Animal Crossing uses exactly this seam: `JKRAram::create(0x810000, 0x6A3780)`
= 8.45 MB sound + 6.96 MB graph, driven through `ARStartDMA`. `dc/src/dc_aram.c`
already models it as "ARAM addresses are OFFSETS, not pointers" with a resident
window (`DC_ARAM_WINDOW_SIZE`) and out-of-range zero-fill. **That header comment
is the correct architecture and it should be the template for the asset pool
too.** The GameCube's ARAM was never memory; it was a *cache tier with an
explicit DMA API*, and every byte of `src/data` `.bss` is a cache tier that got
flattened into a static array by the PC port.

### 4.2 The standard conversion, stated precisely

The technique is **not** "change `static u8 buf[N];` to `malloc(N)`". That is
net-negative: same bytes, plus allocator headers, plus fragmentation. Moving
`.bss` to the heap saves nothing on a machine with no MMU and an `sbrk` heap
that starts at `_end`.

The saving comes from exactly three things, and they must be named separately:

1. **Never-allocate.** The array is for a feature we do not ship (NES emulator,
   texture packs, model viewer, `pc/` GL caches). Delete the TU.
2. **Lifetime overlap.** Two or more arrays are provably never live at the same
   time → one shared arena sized to the max, not the sum. The
   `a*_overlay` / `a*_prg` / `a*_ldr` actor staging buffers
   (`aSTR_overlay` 294,912 + `aBTD_island_prg` 86,596 + `aNNW_client_prg`
   44,224 + `aNPC_k_overlay` 36,888 + `aGYO_overlay` 30,720 + `aINS_overlay`
   21,528 + …) are the textbook case: they are per-actor-class load buffers with
   disjoint residency. **This is a lifetime-analysis problem, not a compiler
   problem.**
3. **Demand residency.** The array is a staging area for content that lives on
   disc → replace with an ID-keyed LRU pool sized to the working set, not to the
   corpus. This is `src/data`, and it is 8.45 MB.

The `src/data` conversion in concrete terms (matching `kb/mem-budget.md` C6):

- 3,211 objects own 16,343 assets **[M]**. Median group ≈ 2.5 KB.
- `gen_runtime_assets.py` stops emitting `u16 foo[N] ATTRIBUTE_ALIGN(32);` and
  emits `extern u16 *foo;` plus a generated `{group_id, offset, size}` table.
  `.bss` cost drops from 8,519,191 B to 16,343 pointers ≈ 65 KB. **[D]**
- Host tool packs the assets into one 32-byte-aligned disc file with a sorted
  index; runtime pool warms per acre/room load and evicts LRU.
- Same change deletes the 15,640,056 B `foresta.rel.szs` boot transient
  (`kb/mem-budget.md` §1) — which on a 16 MB machine is not an optimisation,
  it is the difference between possible and impossible.
- **Net `.bss`: −8.45 MB. Cost: heap pool of ~1.5 MB (budgeted, bucket 7).**

---
