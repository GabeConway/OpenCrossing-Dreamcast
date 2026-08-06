# RAM budget — armhf-era working set and boot residency (§§1, 3)

Where the 65 MB address space comes from, the 15.6 MB boot-time REL transient,
what is resident at boot versus on demand, and the disc content table.
**Measured on the armhf build, 2026-08-01**, before any Dreamcast link existed;
superseded wherever it disagrees with `kb/mem-budget-m1-sh4.md`. Also carries
the measurement provenance (subjects and tools) for
`kb/mem-budget-armhf-binary-size.md`.

> ⚠️ **[STALE 2026-08-06] the `-O0` era ended.** `src/` builds at `-Os` + a
> 14-TU `-O3` hot list (`DC_OPT_PROFILE=perf`); `dc/src` is `-O3`. Measured on
> the shipping Dreamcast town build: `.text` **5,506,964 → 2,753,700 B**
> (2,680,676 at flat `-Os`), `.data` **2,337,980 → 2,224,832 B**, `.bss`
> unchanged (3,945,356 → 3,945,484). **Codegen was worth ~2.75 MB of `.text`,
> roughly every `.bss` lever this project has landed put together.** This file
> is almost all `.bss`, arena, transient and disc arithmetic, none of which
> optimization touches — the 15,640,056 B REL transient, the 68,108,104 B
> address space and the disc table below all still stand. Do not carry the
> armhf `.text`/`.data` figures forward as a DC size prediction, and do not
> substitute the four numbers above into any armhf table. Evidence: the
> 2026-08-06 entry of `kb/state-log.md`.

Measured 2026-08-01. Supersedes the sketch in `PLAN.md` §3.1; PLAN should be
updated to point here. Every number below is tagged **[M]** measured today,
**[D]** derived from measured numbers, or **[?]** unverified estimate that
needs a real measurement (each has a named owner milestone).

Measurement subjects:

- ELF: `/Users/gabe/Documents/GitHub/OpenCrossing-Anbernic/pc/build-armhf/bin/AnimalCrossing`
  (32-bit ARM EABI5 PIE, not stripped, 10,958,900 B, built 2026-07-29) —
  the DC repo has no build tree of its own, and its `pc/` + `src/` are
  byte-identical in the areas measured.
- 3,929 `.o` files under `pc/build-armhf/CMakeFiles/` — same build, so
  per-source attribution is exact, not inferred. Sums agree with the linked
  image to within 0.3 % (`.text` 3,327,876 vs 3,335,604; `.bss` 18,244,355 vs
  18,255,512 — the difference is crt/libgcc objects outside the CMake tree).
- The user's ISO (read-only, never copied): GAFE01 USA Rev 0.
- Tools: `llvm-readelf` / `llvm-nm` from `/opt/homebrew/opt/llvm/bin`.

---

## 1. The real starting number is 65 MB, not 45 MB

`PLAN.md` §3.1 says "~45 MB working set". The measured figure is worse.

| Region | Bytes | | Source |
|---|---:|---|---|
| ELF `PT_LOAD` #1 (text+rodata+relocs) | 4,895,852 | [M] | `readelf -l` |
| ELF `PT_LOAD` #2 memsz (data+bss) | 21,269,212 | [M] | `readelf -l` |
| Main arena (`PC_MAIN_MEMORY_SIZE`) | 25,165,824 | [M] | `pc/include/pc_platform.h:36` |
| Emulated ARAM (`PC_ARAM_SIZE`) | 16,777,216 | [M] | `pc/include/pc_platform.h:37` |
| **Total address space** | **68,108,104** (64.95 MiB) | [D] | |

And essentially all of it is *touched*, so RSS ≈ VM: `pc/src/pc_os.c:277`
`memset(arena_memory, 0, PC_MAIN_MEMORY_SIZE)` and `pc/src/pc_aram.c:12`
`memset(aram_base, 0, PC_ARAM_SIZE)` both fault in the whole reservation at
boot. **4.06× a stock Dreamcast's entire RAM.**

Two corrections to existing kb text while we are here:

- `kb/base-repo-map.md` lists "SystemHeapSize 22.8 MB (`0x16C7000`)". That
  constant (`src/static/jsyswrap.cpp:36`) is a **dead default** — it is
  overwritten at `jsyswrap.cpp:493` with
  `arena_hi - arena_lo - 0xD0` = 24 MiB − 0x3100 − 0xD0 = **25,153,072 B
  (23.99 MiB)** [M]. The system heap is the whole arena, not 22.8 MB.
- `gameheap_len` (`jsyswrap.cpp:37`, `0x380000` = 3.5 MB) is likewise dead.
  `jsyswrap.cpp:547` sets it to `JKRHeap_getFreeSize(systemHeap) - 0x10000`
  — i.e. **the game heap swallows the entire remaining system heap** and
  `MallocInit()` hands it to `__osMalloc`. So the port has *never* measured
  what the game actually needs; it just gives it everything. That is why
  bucket 6 below is the #1 open measurement.

### Boot-time transient (worse than steady state)

`pc_assets_init()` (`pc/src/pc_assets.c:29863`) decompresses the whole REL
into RAM before copying assets out of it:

| Transient | Bytes | | Source |
|---|---:|---|---|
| `foresta.rel.szs` Yaz0-decompressed | **15,640,056** | [M] | Yaz0 header in ISO |
| `main.dol` | 918,720 | [M] | DOL header in ISO |

15.6 MB held live, on a machine with 16 MB total. The current asset path is
not merely too big on DC — it is arithmetically impossible. It must become
seek-and-read-per-asset from the disc (§5, change **C6**).

---

## 3. What is resident at boot vs on demand (task 3 answer)

Boot order [M] (`pc/src/pc_main.c:519` → `:611` → `:614`):

1. `pc_platform_init()` → `pc_gx_init()` (`pc_main.c:308`): touches `g_gx`
   (3.15 MB) and builds `quad_index_buf` (196 KB).
2. `pc_check_disc_or_die()` → `pc_disc_init()` (`pc_main.c:475`): fills
   `g_fst_files` (270 KB).
3. `pc_assets_init()` (`pc_main.c:611`): extracts + Yaz0-decompresses the REL
   (**15.64 MB live**) and the DOL (0.92 MB), then **memcpy+byteswap all
   16,343 assets (8.77 MB) into BSS in one loop**, then frees the two blobs.
4. `ac_entry()`; `boot_main()` → `OSInit()` (24 MB arena, memset) →
   `JFWSystem::firstInit()` (`JFWSystem.cpp:115`) → `JKRExpHeap::createRoot` +
   `JKRExpHeap::create(sysHeapSize)` → `JFWSystem::init()` →
   `JKRAram::create(0x810000, 0x6A3780)` (`jsyswrap.cpp:499-500`; `pc_aram.c`
   mallocs and memsets 16 MB) → `JUTCreateFifo(0x10001)` (65,568 B from the
   system heap, `JUTGraphFifo.cpp:9`) → `JW_Init2` mounts `forest_1st.arc` and
   hands **all remaining system-heap free space** to `MallocInit`
   (`jsyswrap.cpp:547-549`) → `JW_Init3` mounts `forest_2nd.arc`.

**Nothing in the asset path is demand-loaded.** The only on-demand mechanism
that exists is `JKRAramArchive` → `ARStartDMA` (a `memcpy` in
`pc/src/pc_aram.c`), which is the one seam PLAN §3.1 correctly identifies.

Disc content, measured from the user's ISO [M]:

| File | Bytes | Note |
|---|---:|---|
| `audiorom.img` | 8,300,384 | → AICA sound RAM + disc stream, **0 main RAM** |
| `foresta.rel.szs` | 6,137,393 | Yaz0 → **15,640,056** decompressed |
| `foresta.map` | 4,849,144 | debug map, not shipped |
| `forest_2nd.arc` | 4,132,608 | RARC, uncompressed |
| `famicom.arc` | 1,699,904 | RARC — NES emu is a non-goal, drop |
| `main.dol` | 918,720 | text 681,440 + data 237,024, bss 1,272,157 |
| `forest_1st.arc` | 852,896 | RARC, uncompressed |
| others (`static.map`, `opening.bnr`, …) | 559,450 | |
| **total real content** | **26,531,779** (25.30 MiB) | the 1.46 GB image is padding |

Note `forest_1st.arc + forest_2nd.arc = 4,985,504 B` of *actual* graph-ARAM
content against a 6,961,536 B reservation — the reservation is 40 % slack.

---
