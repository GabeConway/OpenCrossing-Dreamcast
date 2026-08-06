# Fitting in 16 MB — does it add up, the recommended plan, and sources

The honest arithmetic (the target is ~13.5 MB, not 6.5 MB), the per-bucket
closing table and what is fragile about it (§6); the ordered plan, steps 0–5
plus what is held in reserve (§7); and the full source index for every part
(§8). Part of `kb/research-size-reduction.md`, whose stub maps every § to its file.

Tags: **[M]** measured today against the real DC ELF, **[S]** sourced to a URL,
**[D]** derived arithmetic, **[?]/[UNVERIFIED]** not confirmed.

⚠️ `kb/levers.md` L3 re-costed every estimate in this document against
the real ELF: **every one was wrong, most by a lot, and two of the stated
mechanisms were impossible.** Use `kb/levers.md` for numbers; use this
document for the reasoning and the sources.

> ## ⚠️ [STALE 2026-08-06] — §6.2's load-bearing sentence is VOID
>
> "**`.text` + `.rodata` + `.eh_frame` = 6,318,568 B and it does not move**" was
> true only because `-O0` was mandated. **That directive was reversed on
> 2026-08-06.** `src/` builds at `-Os` with a 14-TU `-O3` hot list
> (`DC_OPT_PROFILE=perf`, the default; `size` = `-Os` everywhere; `o0` =
> byte-identical revert); `dc/src` moved to `-O3`. Measured on matched town
> windows of the shipping build: `.text` **5,506,964 → 2,753,700 B**
> (2,680,676 at flat `-Os`), `.data` **2,337,980 → 2,224,832 B**, `.bss`
> unchanged (3,945,356 → 3,945,484 B).
>
> `.text` moved by 2,826,288 B at flat `-Os` — **more than any single line in
> §6.2's closing table except #1, and more than every `.bss` lever the project
> has landed put together.** Consequences, in order:
>
> - §6.2's `.text` row (−107,440 B "delete NES/texpack/viewer") is now the
>   smallest part of what `.text` actually gave up. The table's total does not
>   close as written; it is `-O0` arithmetic. **Do not patch the total — rebuild
>   it from a current link.**
> - §6.3's "there is no second technique of that magnitude available without
>   touching codegen" is exactly right, and the answer was to touch codegen.
> - §7's reserve item #12 (ScummVM-style overlays, "if `.text` becomes binding")
>   is much further away than it was.
> - §7 Step 0's three unknowns (KOS baseline, VRAM headroom, `__osMalloc` peak)
>   are **still unmeasured** and still gate everything.
>
> ⚠️ The four numbers above are shipping-stubbed-town numbers; §6 is
> full-asset-image arithmetic. Do not substitute one into the other. Evidence:
> the 2026-08-06 entry of `kb/state-log.md`.

## 6. Does it add up? — the honest arithmetic

### 6.1 The target is not 6.5 MB. It is ~13.5 MB.

6,500,000 B of cuts leaves the image at 15,986,548 B, ending at `0x8cd42...`,
i.e. it **links and boots with roughly 1.2 MB of heap** — less than the game's
`JUTCreateFifo(0x10001)` plus one archive mount. It is a build-green milestone,
not a playable one.

The real target, from `dc/include/dc_mem_budget.h` (which transcribes
`kb/mem-budget.md` §4):

```
RAM                                        16,777,216
− low reserved 0x8c000000..0x8c010000          65,536
− KOS kernel + newlib + drivers [?]         1,000,000
− KOS kernel stack below mem_top               65,536
= available for image + heap               15,646,144   [D]

heap the ledger wants (buckets 6–12):
  JKRHeap/__osMalloc  4,000,000
  asset pool          1,500,000
  ARAM graph window     512,000
  audio work RAM        700,000
  disc read-ahead       384,000
  PVR staging           384,000
  thread stacks         131,072
                    = 7,611,072

⇒ image budget                              8,035,072   [D]
   image today                              22,486,548  [M]
   REQUIRED CUT                            −14,451,476  [D]  (13.78 MiB)
```

### 6.2 Where the 14.45 MB comes from, with `-O0` frozen ⚠️ [STALE 2026-08-06 — `-O0` is not frozen; see the banner]

~~`.text` + `.rodata` + `.eh_frame` = 6,318,568 B and **it does not move**~~,
except for the ~890 KB of `pc_assets.c` strings that are data pretending to be
rodata. **It moved: `.text` 5,506,964 → 2,753,700 B in the shipping profile.**
The table below is `-O0` arithmetic, kept as history.

| Bucket | Now | After | Δ | Technique |
|---|---:|---:|---:|---|
| `.text` + `.init`/`.fini` | 5,257,440 | 5,150,000 | −107,440 | #11 delete NES/texpack/viewer/`pc/` |
| `.rodata` | 1,053,740 | 165,000 | **−888,740** | #3 `s_assets[]` strings → disc index |
| `.eh_frame` + `.gcc_except_table` | 7,388 | 7,388 | 0 | not worth it |
| `.data` | 2,638,340 | 700,000 | **−1,938,340** | #2 evict `src/data` tables (948 KB pointer-free now + ~990 KB via reloc pass) |
| `.bss` — `src/data` staging | 8,519,191 | 70,000 | **−8,449,191** | **#1 demand residency** |
| `.bss` — `prbuf` | 1,229,348 | 0 | **−1,229,348** | #4 PVR render target |
| `.bss` — `emu64 texture_buffer` | 562,121 | 40,000 | −522,121 | #6 decode straight to VRAM |
| `.bss` — `audiomemory` + jaudio | 1,052,834 | 400,000 | −652,834 | #7 AICA + shrink |
| `.bss` — actor overlay arenas | ~710,000 | ~250,000 | −460,000 | #8 shared union arena |
| `.bss` — `pc_m_card` | 320,150 | 40,000 | −280,150 | #9 → heap, `kb/save-budget.md` sized |
| `.bss` — `dc_gx` | 334,508 | 90,000 | −244,508 | #10 resize |
| `.bss` — everything else (keep) | ~800,000 | ~800,000 | 0 | `common_data`, `sys_dynamic`, libs |
| **TOTAL** | **22,486,548** | **7,712,388** | **−14,774,160** | |

**It adds up — with 323 KB to spare against a 14.45 MB requirement, i.e. a 2 %
margin on a plan whose largest line item is unbuilt.** That is not comfortable.
Read that as "the plan closes on paper," not "the plan is safe."

### 6.3 Honest statement of what is fragile

- **#1 is 57 % of the entire cut.** If the `src/data` demand-residency
  conversion lands at only half effectiveness, the whole thing fails. There is
  no second technique of that magnitude available without touching codegen.
  ⚠️ **[2026-08-06] and touching codegen is what happened.** `-Os` + a 14-TU
  `-O3` hot list took `.text` from 5,506,964 to 2,753,700 B. That is the second
  technique of that magnitude, and this bullet named it correctly — it just was
  not allowed at the time. `kb/state-log.md`, 2026-08-06.
- **Bucket 6 (`JKRHeap` + `__osMalloc`, 4.0 MB) is still unmeasured.**
  `kb/mem-budget.md` §4.2 calls it "the single biggest unknown". Today the game
  hands the *entire* remaining system heap to `MallocInit`
  (`jsyswrap.cpp:547`), so nobody knows the real peak. If it is 6 MB, the plan
  above is 2 MB short and technique #12 (code overlays) becomes mandatory.
- **The KOS baseline (1.0 MB) is unmeasured** [?]. Measure
  `(uintptr_t)_arch_mem_top - (uintptr_t)&end` after `pvr_init` in a KOS
  hello-world with GLdc, as `kb/mem-budget.md` bucket 1 already demands.
- **VRAM headroom is unmeasured.** #4, #5 and #6 all spend VRAM. If
  `pvr_mem_available()` after texture residency is under ~1.5 MB, #4/#6 must
  find main-RAM answers instead and the plan loses ~1.75 MB.
- If all three unknowns land badly, the shortfall is 4–6 MB and the only
  remaining codegen-free lever is **#12, ScummVM-style overlays**, at very high
  cost.

---

## 7. Recommended plan

**Do these in this order. Do not reorder to chase easy wins first.**

**Step 0 — measure the three unknowns before writing any code (1 day).**
KOS+GLdc baseline RAM; `pvr_mem_available()` after a representative texture
load; `__osMalloc` peak via the probe in `kb/mem-budget.md` §5 "Probe 1". These
three numbers decide whether the plan in §6.2 is a plan or a wish. Everything
below is contingent on them.

**Step 1 — `src/data` demand residency (#1 + #2 + #3 together).**
This is one project, not three, because they share the disc packer and index:
- host tool packs 16,343 assets + the 2.25 MB of `src/data` `.data` tables into
  one aligned disc file with a sorted `{group_id, off, size}` index;
- `gen_runtime_assets.py` emits `extern T *sym;` + generated ID table instead of
  arrays;
- runtime LRU pool (bucket 7, 1.5 MB) keyed by group, warmed per acre/room;
- `s_assets[]` name strings never enter the image.

**Expected: −8.45 MB `.bss`, −0.95 MB `.data` (phase 1), −0.89 MB `.rodata`
= −10.3 MB.** It also deletes the 15.64 MB `foresta.rel.szs` boot transient,
which is independently a hard blocker. **This is the single highest-value thing
to do, and nothing else comes close.**

**Step 2 — `prbuf` → PVR render target (#4).** −1.23 MB, one buffer, one owner
(`src/game/m_play.c:54`), and it is the correct architecture on PVR anyway.

**Step 3 — the audio/emu64/card/gx cluster (#6, #7, #9, #10, #11).** −1.7 MB
across five independent, individually revertible changes. Good parallel work
for a second agent; each gets its own kill switch per CLAUDE.md.

**Step 4 — actor overlay union arena (#8).** −0.46 MB. Needs lifetime proof per
actor class; do it last among the `.bss` items because it is the one that can
silently corrupt.

**Step 5 — `src/data` `.data` phase 2, the REL-style reloc pass (#2 cont.).**
−0.99 MB. Only 403,972 B of the 2.05 MB pointer-bearing set is actual pointer
content (`kb/mem-budget.md` §2), so a load-time fix-up table is cheap. Do it
only if §6.2's margin has eroded.

**Held in reserve, in this order:** VRAM `NOLOAD` section for selected arrays
(#5) once VRAM headroom is measured; then ScummVM-style SH-4 ELF overlays (#12)
if `.text` becomes binding.

**Explicitly not on the table, ever, without the user reopening it:** anything
in rows 19–22 of §2, and the 32 MB RAM mod.

---

## 8. Source index

KallistiOS
- `kernel/mm/mm.c` — `sbrk` starts at `end` — <https://github.com/KallistiOS/KallistiOS/blob/master/kernel/mm/mm.c>
- `kernel/libc/newlib/newlib_sbrk.c` — <https://github.com/KallistiOS/KallistiOS/blob/master/kernel/libc/newlib/newlib_sbrk.c>
- `kernel/arch/dreamcast/include/arch/arch.h` — `_arch_mem_top`, `HW_MEM_16` — <https://github.com/KallistiOS/KallistiOS/blob/master/kernel/arch/dreamcast/include/arch/arch.h>
- `include/kos/thread.h` — `THD_KERNEL_STACK_SIZE = 64K` — <https://github.com/KallistiOS/KallistiOS/blob/master/include/kos/thread.h>
- `utils/ldscripts/shlelf.xc` — `LOAD_OFFSET`, `.ocram` NOLOAD pattern — <https://github.com/KallistiOS/KallistiOS/blob/master/utils/ldscripts/shlelf.xc>
- `dc/spu.h` — <https://github.com/KallistiOS/KallistiOS/blob/master/kernel/arch/dreamcast/include/dc/spu.h>
- `dc/g2bus.h` — G2 access rules — <https://github.com/KallistiOS/KallistiOS/blob/master/kernel/arch/dreamcast/include/dc/g2bus.h>
- ELF loader API — <https://kos-docs.dreamcast.wiki/elf_8h.html>
- Romdisk ("cannot be evicted from system RAM") — <https://kos-docs.dreamcast.wiki/group__vfs__romdisk.html>
- Store queues — <https://kos-docs.dreamcast.wiki/group__store__queues.html>
- PVR memory allocator — <https://kos-docs.dreamcast.wiki/group__pvr__mem__mgmt.html>
- README (no memory protection, MMU unused) — <https://kos-docs.dreamcast.wiki/md_doc_2README.html>

Toolchain
- GCC `gcc/config/sh/sh.opt` — <https://github.com/gcc-mirror/gcc/blob/master/gcc/config/sh/sh.opt>
- GCC SH options manual — <https://gcc.gnu.org/onlinedocs/gcc/SH-Options.html>
- binutils `gold/configure.tgt` (no SH target) — <https://sourceware.org/git/?p=binutils-gdb.git;a=blob_plain;f=gold/configure.tgt;hb=HEAD>
- `-ffunction-sections` explained — <https://www.vidarholen.net/contents/blog/?p=729>
- Simulant DC toolchain flags — <https://gitlab.com/simulant/simulant/-/issues/160>
- mruby `dreamcast_shelf.rb` KOS flag set — <https://fossies.org/linux/mruby/build_config/dreamcast_shelf.rb>

Dreamcast hardware
- Memory map — <https://mc.pp.se/dc/memory.html>
- IP.BIN / 1ST_READ.BIN — <https://mc.pp.se/dc/ip.bin.html>
- Boot process, load address `0x8c010000` — <https://dreamcast.wiki/Boot_process>
- VRAM 32/64-bit areas — <https://dreamcast.wiki/VRAM>
- OCRAM, cache instructions — <https://dreamcast.wiki/Useful_programming_tips>
- Bus/bandwidth analysis — <https://www.copetti.org/writings/consoles/dreamcast/>
- Sega hardware spec outline (G2 40 MB/s) — <https://segaretro.org/images/8/8b/Dreamcast_Hardware_Specification_Outline.pdf>
- VRAM-as-storage thread (403 to fetch; via search index) — <https://dcemulation.org/phpBB/viewtopic.php?t=96364>
- DreamHAL — <https://github.com/sega-dreamcast/dreamhal>, <https://dreamcast.wiki/DreamHAL>

Precedent ports
- ScummVM DC ELF loader — <https://github.com/scummvm/scummvm/blob/master/backends/platform/dc/dcloader.cpp>
- ScummVM DC plugin linker script — <https://github.com/scummvm/scummvm/blob/master/backends/platform/dc/plugin.x>
- ScummVM DC plugins overview — <https://consolemods.org/wiki/Dreamcast:ScummVM>
- ScummVM DC docs (16 MB limit) — <https://docs.scummvm.org/en/latest/other_platforms/sega_dreamcast.html>
- Xash3D DC (static arrays → dynamic allocators) — <https://www.dreamcast-talk.com/forum/viewtopic.php?t=17755>
- GameCube ARAM as paged virtual memory — <https://www.copetti.org/writings/consoles/gamecube/>

Internal
- `kb/state-log.md` — **2026-08-06 entry: the `-O0` reversal and the measured `.text`/`.data`/`.bss`/FPS table. Read it before quoting anything in §6**
- `kb/mem-budget.md` — the 16 MB ledger, buckets 1–12, probes
- `kb/design-shelf-hazards.md` §3.4 — the `-O2` size measurement this document withdraws. ⚠️ **[2026-08-06] the withdrawal is withdrawn — §3.4 was right, and its recommendation is now the shipping build**
- `dc/include/dc_mem_budget.h` — the ledger as constants
- `dc/src/dc_aram.c` — the correct architectural template for a demand-resident tier
