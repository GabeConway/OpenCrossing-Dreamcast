# Fitting the image in 16 MB **without changing codegen**

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

---

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

---

## 2. Ranked technique table

Ordered by **saving ÷ (risk × cost)**. "Codegen?" = does it alter instruction
selection for `src/` decomp code.

| # | Technique | Codegen? | Expected saving | Cost | Risk |
|---|---|---|---:|---|---|
| 1 | **Demand-load `src/data` staged assets** (kill the static asset `.bss`) | **No** | **−8.45 MB `.bss`** | high (generator + pool + disc packer) | med |
| 2 | **Evict `src/data` `.data` tables to disc** (pointer-free first, then reloc pass) | **No** | −0.95 MB now, −2.05 MB total | med → high | low → med |
| 3 | **Move `s_assets[]` name-string pool out of the image** | **No** | **−0.89 MB `.rodata`** | low | low |
| 4 | **`prbuf` → PVR render-target in VRAM** | **No** | **−1.23 MB `.bss`** | med | med |
| 5 | **Place selected big arrays in VRAM via a `NOLOAD` linker section** | **No** | up to −2 MB `.bss` (see §5.1 caveats) | low-med | med-high |
| 6 | **`emu64 texture_buffer_data` → decode straight to VRAM** | **No** | −0.52 MB `.bss` | med | med |
| 7 | **`audiomemory` → AICA sound RAM + shrink** | **No** | −0.59 MB `.bss` | med | med |
| 8 | **Overlay staging arenas → one shared union arena** (`aSTR_overlay`, `ac_boat_demo`, `aNPC_*_overlay`, `aBTD_island_*`) | **No** | −0.5 … −0.7 MB `.bss` | med | med (lifetime analysis) |
| 9 | **`pc_m_card` save staging → heap, sized from `kb/save-budget.md`** | **No** | −0.28 MB `.bss` | low | low |
| 10 | **`dc_gx` vertex staging resize** | **No** | −0.25 MB `.bss` [?] | low | low |
| 11 | **Delete non-goal subsystems entirely** (NES emu + `famicom.arc`, texture packs, model viewer, remaining `pc/src`) | **No** | −0.1 MB `.bss`, −0.1 MB `.text` | low | low |
| 12 | **ScummVM-style SH-4 ELF code overlays** (`.text` paged from disc) | **No** | −0.5 … −1.0 MB `.text` realistically | **very high** | high |
| 13 | `.ocram` section (8 KB of the operand cache as off-RAM storage) | **No** | −8 KB | low | med (halves D-cache) |
| 14 | `--gc-sections` + `-ffunction-sections -fdata-sections` | **No** | **already applied — 0 further** | — | — |
| 15 | Strip `.debug_*` / `.symtab` from the shipped binary | **No** | **0 RAM** (67 MB disc only) | trivial | none |
| 16 | Compress `1ST_READ.BIN` | **No** | **0 RAM** (disc + load time only) | low | low |
| 17 | Avoid KOS `romdisk`; read from `/cd` | **No** | 0 (not currently used) — but *never* regress into it | — | — |
| 18 | Reclaim `0x8c000000`–`0x8c010000` by lowering `LOAD_OFFSET` | **No** | +64 KB | low | **high — BIOS/vector area** |
| 19 | `-Wl,--relax` / `-mrelax` | **YES** | ~1–3 % `.text` [?] | low | **DISQUALIFIED** |
| 20 | `-mspace`, `-Os`, `-O1`, `-O2`, LTO, `optimize` pragmas | **YES** | −48 % `.text` (2.5 MB) | low | **DISQUALIFIED by policy** |
| 21 | `-mbigtable` | **YES** | *negative* (default is already 16-bit) | — | **DISQUALIFIED** |
| 22 | `-mdalign`, `-mfmovd`, `-mpadstruct` | **YES** + ABI change | — | — | **DISQUALIFIED** |
| 23 | `-fno-exceptions` / `-fno-asynchronous-unwind-tables` | partly | ≤ 7.4 KB total | low | not worth it |
| 24 | Small-data / `-G` / `.sdata` | — | **does not exist on SH** | — | N/A |
| 25 | `--icf` identical code folding | — | **not available for `sh-elf`** | — | N/A |

---

## 3. Technique detail

### 3.1 `--gc-sections` — already spent, measured **[M]**

`dc/Makefile:407` already links with `-Wl,--gc-sections`, and `$KOS_CFLAGS`
already carries `-ffunction-sections -fdata-sections` (confirmed by inspecting
`dc/build/obj/src/famicom_emu.c.o`, which has 55 section headers of the form
`.text.my_alloc_init`, `.bss.famicom_done`, `.rodata.HZ`).

I parsed the `Discarded input sections` block of `AnimalCrossing.map`:

| Discarded | Bytes |
|---|---:|
| `.text` | 292,140 |
| `.bss` | 131,650 |
| `.rodata` | 83,464 |
| `.data` | 14,896 |
| **allocatable total** | **522,150** |
| `.debug_*` / `.group` / `.comment` (never allocatable anyway) | 1,298,283 |

**29,471 input sections discarded, 522 KB of RAM recovered — and that is all
there is.** Re-running with `--print-gc-sections` will produce a list, not more
savings. Anyone proposing `--gc-sections` as the answer should be shown this
table.

Two honest caveats about the flags, since they are already on:

- `-ffunction-sections` on SH-4 **can slightly enlarge `.text`**, because SH-4
  materialises 32-bit constants from PC-relative literal pools
  (`mov.l @(disp,PC)`), and per-function sections prevent pool sharing across
  functions. It does not change instruction *selection* — the same insns are
  chosen — but the emitted pool layout differs. Net effect here is clearly
  positive (522 KB recovered), so keep them. **[D]**
- At `-O0`, GCC's front end still does not emit **unreferenced `static`**
  functions or data at all, so `--gc-sections` only ever reaches *external*
  symbols. Verified on the host toolchain: a TU with `static int unused(){}` and
  `static const int unused_table[256]` produced neither symbol at `-O0`. **[M]**
  This is why 292 KB, not 2 MB — the decomp's dead code is mostly already gone.

Sources: <https://www.vidarholen.net/contents/blog/?p=729>,
<https://gitlab.com/simulant/simulant/-/issues/160>,
<https://fossies.org/linux/mruby/build_config/dreamcast_shelf.rb>

### 3.2 Toolchain knobs that change codegen — **all DISQUALIFIED**

I read `gcc/config/sh/sh.opt` from GCC master directly rather than trusting the
manual. **[S]** <https://github.com/gcc-mirror/gcc/blob/master/gcc/config/sh/sh.opt>

| Flag | `sh.opt` text | Verdict |
|---|---|---|
| `-mrelax` | "Shorten address references during linking." | **DISQUALIFIED.** It makes the assembler emit `.uses` pseudo-ops and lets `ld` synthesise `bsr` in place of literal-pool `jsr @Rn`. That is a link-time *instruction stream rewrite*. GCC's own SH machine description says "BSR is not generated by the compiler proper, but when relaxing, it generates .uses pseudo-ops that allow linker relaxation to create BSR." It is semantics-preserving in theory, but it is exactly the class of change the `-O0` rule exists to forbid, and `kb/design-shelf-hazards.md` §3.4 already flags that it "interacts badly with `--gc-sections` diagnosis." **Do not smuggle this in as "a linker flag".** [S] <https://gcc.gnu.org/onlinedocs/gcc/SH-Options.html> |
| `-mspace` | not present in modern `sh.opt`; historically an `-Os` alias | DISQUALIFIED (== `-Os`) |
| `-mbigtable` | "Generate 32-bit offsets in switch tables." | DISQUALIFIED **and it makes things bigger** — 16-bit is the default |
| `-misize` | "Annotate assembler instructions with estimated addresses." | Diagnostic only. Harmless, saves nothing. Useful for triage. |
| `-mpadstruct` | "Make structs a multiple of 4 bytes (warning: ABI altered)." | DISQUALIFIED — ABI change on UB-dependent decomp structs is suicidal |
| `-mdalign` / `-mfmovd` | 8-byte alignment + paired FP moves | DISQUALIFIED; also the exact ARM `LDRD` hazard class re-imported (`design-shelf-hazards.md` §3, §4.1) |
| `-G` / `-msdata` / small-data | **absent from `sh.opt` entirely** | **N/A — SH GCC has no small-data model.** The `.sdata`/`.sbss` output sections in the KOS linker script are inert boilerplate from the generic ELF template; the map shows 0 bytes in them. No addressing change to worry about, and no saving to be had. **[M]** |

### 3.3 `--icf` (identical code folding) — **not available for `sh-elf`**

ICF exists in `ld.gold` (`--icf=[none|all|safe]`) and in LLVM `lld`, not in GNU
`ld.bfd`. I fetched `gold/configure.tgt` from binutils master: the complete set
of `targ_obj` values is `aarch64, arm, i386, mips, powerpc, s390, sparc,
tilegx, x86_64`. **There is no SH backend in gold.** `lld` has no SH ELF port
either. **[M]/[S]**
<https://sourceware.org/git/?p=binutils-gdb.git;a=blob_plain;f=gold/configure.tgt;hb=HEAD>

Decomp code is a plausible ICF candidate (many near-identical actor
constructors), but it is simply not reachable on this target. Rebuilding a
gold SH backend is not a size-reduction project.

### 3.4 Debug info and symbol tables — **0 RAM, do it anyway for sanity**

The ELF is 71.9 MB but only 22.5 MB of it has `SHF_ALLOC`. `.debug_info` alone
is 48.8 MB. **[M]** `1ST_READ.BIN` is produced by `objcopy -O binary`, which
emits only allocatable content, so debug info has **never** cost a byte of
Dreamcast RAM. Anyone claiming "strip the binary to fit" is wrong. Keep `-g`
for the `.map`/`.elf` used by the harness; strip only what goes on the disc.

Same verdict for `.eh_frame` (7,228 B) + `.gcc_except_table` (160 B): a total
of 7.4 KB, i.e. 0.03 % of the problem. `-fno-asynchronous-unwind-tables` is a
codegen-neutral emission flag and is safe, but `-fno-exceptions` **is** a
codegen change for the C++ TUs (it deletes cleanup paths and changes call
sequences) and `JKRHeap.cpp` overrides `operator new`. Not worth the argument
for 7 KB.

### 3.5 Compressed / self-extracting `1ST_READ.BIN` — **0 RAM saving**

`1ST_READ.BIN` is loaded to `0x8c010000` by the BIOS, descrambled in place.
<https://mc.pp.se/dc/ip.bin.html>, <https://dreamcast.wiki/Boot_process> **[S]**

Be precise about what compression can and cannot do:

- It shrinks the **file on disc** and shortens the CD-R read at boot
  (~500 KB/s). Real, but a load-time win only.
- It **cannot** reduce resident RAM. A self-extracting stub must materialise
  the full `.text`+`.data` somewhere before executing it, so peak RAM is
  *≥* the uncompressed size, and during decompression it is
  *compressed + uncompressed* unless you decompress in place backwards.
- `.bss` — our actual problem — has **no on-disc representation at all**
  (`NOBITS`). Compression is arithmetically incapable of touching it.

Verdict: useful later for disc layout, irrelevant to this problem. I found no
Dreamcast-specific source that states this explicitly; the reasoning is from
the ELF `NOBITS` semantics and the BIOS load model, so treat the framing as
**[D]**, not [S].

### 3.6 KOS `romdisk` — a trap to stay out of

KOS's docs are blunt: "An embedded romdisk image is linked to your executable
and **cannot be evicted from system RAM**", "Mounted images will reside in
system RAM for as long as your program is running", and "the size of your
generated ROMFS image must be kept below 16MB, with 14MB being the maximum
recommended size, as your binary will also reside in RAM."
<https://kos-docs.dreamcast.wiki/group__vfs__romdisk.html> **[S]**

We currently link no romdisk (no `romdisk.o` appears in the map) **[M]**. The
correct pattern is `/cd` + our own read-ahead, which `dc/src/dc_dvd.c` is
already headed toward. Record it here so it never gets "helpfully" added.

### 3.7 Code overlays on Dreamcast — real, in production, and expensive

This is the technique the question was really asking about, and it does exist
on this platform with a shipping implementation.

**ScummVM's Dreamcast port has loaded engine code from disc since v0.7.0**
(2004). Each engine is a `.PLG` file; the launcher loads them one at a time.
Source: <https://consolemods.org/wiki/Dreamcast:ScummVM>, ScummVM docs
<https://docs.scummvm.org/en/latest/other_platforms/sega_dreamcast.html> **[S]**
The motivation is exactly ours: "The Dreamcast … only has 16MB of RAM"; and
"since the v1.2.1 release, the combined size of the engine plugins is larger
than the available memory."

The mechanism is a hand-written SH-4 ELF loader,
`backends/platform/dc/dcloader.cpp` (~12 KB), plus a plugin linker script
`backends/platform/dc/plugin.x`. **[S]**
<https://github.com/scummvm/scummvm/blob/master/backends/platform/dc/dcloader.cpp>
<https://github.com/scummvm/scummvm/blob/master/backends/platform/dc/plugin.x>

How it works, verbatim from the source:

- `plugin.x` links the module with `. = 0;` and a single `PT_LOAD` phdr, so the
  whole plugin is one position-zero segment with `.text .rodata .data .ctors
  .dtors .bss` laid out contiguously.
- `DLObject::load()` validates `e_type == 2`, `e_machine == 42` (EM_SH),
  `e_phnum == 1`, `p_vaddr == 0`, then `memalign(phdr.p_align, phdr.p_memsz)`
  and reads the segment in — i.e. **it mallocs the overlay out of the same heap
  we are fighting for**.
- `DLObject::relocate()` walks `Elf32_Rela` and handles exactly one relocation
  type — `R_SH_DIR32` (`r_info & 0xf == 1`) — adding the segment base. Anything
  else is a hard error.
- It then invalidates the I-cache (`purge_copyback()` writes the OC address
  array at `0xf4000000`; `flush_instruction_cache()`).

KOS also ships a first-party loader: `kos/elf.h` /
`elf_load()` / `elf_free()`, documented as "isn't necessarily meant for running
multiple processes, but more for **loadable library support within KOS**", with
SuperH relocation types handled.
<https://kos-docs.dreamcast.wiki/elf_8h.html> **[S]**

**Honest assessment for this project.** Overlays do not change codegen, so they
are legal under the rule. But:

- They buy `.text`, and `.text` is only 5.26 MB of a 22.5 MB image. Even a very
  aggressive carve (`src/actor/npc/*` 444 KB + museum 213 KB + jaudio_NES
  213 KB + emu64 31 KB, from `kb/mem-budget.md` §2) is under 1 MB, and only if
  none of it is hot.
- Every overlay's `.bss` and `.data` still get allocated when resident, so the
  peak is *not* the sum of the savings.
- The decomp is a single flat symbol namespace with dense cross-TU references.
  The GC-sections map shows the call graph is highly connected; finding a
  1 MB cut with a clean edge is a research task in itself.
- CD-R at ~500 KB/s means a 256 KB overlay is a ~0.5 s stall. Acceptable at a
  room transition, fatal mid-frame.

**Verdict: keep on the shelf.** It is the right answer if, after §7's plan
lands, `.text` is still the binding constraint. It is the wrong first move.

Precedent for the *other* half of the technique — carving content so it never
has to be resident — is the standard Dreamcast port answer: the DC ports of
Half-Life and Soldier of Fortune "required the levels to be cut up into
smaller, more manageable sections to load up in memory", and the Xash3D DC
effort describes "migrating static arrays in the engine to dynamic allocators
to save RAM" for retail 16 MB units.
<https://www.dreamcast-talk.com/forum/viewtopic.php?t=17755> **[S]**
(That thread is 403 to automated fetch; the quotes are as returned by search
indexing and are marked **[UNVERIFIED]** at the character level, though the
technique itself is not in doubt.)

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

## 5. What else the Dreamcast memory map offers

### 5.1 VRAM (8 MB) as a store — real, with sharp edges

**Addressing.** VRAM is physical area 1, `0x04000000`–`0x07FFFFFF`, 8 MB in two
independent 4 MB modules. Two aliases of the same silicon:
- **64-bit area** at `0x05000000` (P2/uncached: `0xA5000000`) — the two modules
  interleave every 4 bytes, so consecutive access alternates modules and is
  faster. This is where textures live.
- **32-bit area** at `0x04000000` (`0xA4000000`) — modules sequential; the
  framebuffer lives here.
- Mirrors at `0x06000000` / `0x07000000`.

<https://dreamcast.wiki/VRAM>, <https://mc.pp.se/dc/memory.html> **[S]**

**Rules you must obey.**
- "there is no restriction on what sizes may be used for read and write
  operations" in either area — but the widely repeated hardware rule is that
  **8-bit writes are not possible** (16-bit minimum). Treat any byte-granular
  array as unusable in VRAM. **[S]/[?]** — dreamcast.wiki states the "no
  restriction" line while the Sega hardware outline states the 16-bit-minimum
  write rule; I could not reconcile these two sources, so **assume 16-bit
  minimum** and design around it.
- Store queues write *to* VRAM efficiently: 32-byte units, 32-byte-aligned
  destination, 4-byte-aligned source (8 for best speed), size a multiple of 32.
  `pvr_sq_*` variants cannot be used concurrently with PVR DMA.
  <https://kos-docs.dreamcast.wiki/group__store__queues.html> **[S]**
- **Reads are the problem.** SH-4 reads from VRAM are consistently described as
  "much slower than accessing main RAM"; the VRAM bus is 64-bit @ 100 MHz
  (~800 MB/s aggregate, shared with the PVR's rasteriser, texture fetch, and
  display scan-out), and the SH-4 is a distant second-priority master on it.
  <https://www.copetti.org/writings/consoles/dreamcast/>,
  <https://dcemulation.org/phpBB/viewtopic.php?t=96364> **[S]**
  I could **not** find a citable measured SH-4↔VRAM read figure. Any number you
  see quoted, including "~520 MB/s SQ bandwidth", is **[UNVERIFIED]** — measure
  it at M1 before budgeting on it.

**How much is actually free.** Not 8 MB. The community answer is blunt: "when
you're using the PVR for rendering, about half of VRAM is off limits due to the
fact that it's needed for the PVR's buffers. Even when you're not using the
PVR, the framebuffers still reside in VRAM."
<https://dcemulation.org/phpBB/viewtopic.php?t=96364> **[S]**
Arithmetic for our case **[D]**: 2 × 640×480×16bpp framebuffers = 1,228,800 B,
plus TA vertex/OPB buffers (KOS `pvr_init` sizes these; typically 1–2 MB), plus
every texture the game needs. Animal Crossing is texture-heavy. **Budget zero
spare VRAM until `pvr_mem_available()` is measured on real hardware.**

**The mechanism, and it is clean.** You do not need a special API to place data
in VRAM — the KOS linker script already demonstrates the exact pattern for
off-main-RAM `NOLOAD` placement:

```ld
/* utils/ldscripts/shlelf.xc */
_end = .; PROVIDE (end = .);
.ocram 0x7c001000 (NOLOAD) :
{
  *(.ocram)
  . = . > 0x2000 ? 0x2000 : .;   /* error if > 8 KB of operand-cache RAM */
}
```
<https://github.com/KallistiOS/KallistiOS/blob/master/utils/ldscripts/shlelf.xc> **[S]**

A `.vram_bss 0xa5000000 (NOLOAD)` output section plus
`__attribute__((section(".vram_bss")))` on chosen arrays is **a placement
change, not a codegen change** — GCC emits identical instructions; only the
symbol's address differs. Two hard requirements: (a) the range must be carved
out of KOS's PVR allocator (`pvr_mem_malloc` hands out 32-byte-aligned blocks
from a dlmalloc pool initialised by `pvr_mem_initialize`
<https://kos-docs.dreamcast.wiki/group__pvr__mem__mgmt.html> **[S]**) or you
will get silent corruption; (b) the arrays must be 16-bit-or-wider access and
must tolerate uncached, high-latency reads.

**Best candidates** (write-mostly, read by hardware, never byte-indexed):
`prbuf` (1,228,800 B — but the *right* answer is a real PVR render target, not
a hand-placed buffer), `emu64 texture_buffer_data` (524,288 B — it is a texture
decode scratch buffer whose output goes to VRAM anyway). Worst candidates:
anything the game `memcpy`s out of every frame, anything with `u8` element type,
anything holding pointers the game dereferences.

### 5.2 AICA sound RAM (2 MB) — a real ARAM analogue, but DMA-only

`SPU_RAM_BASE = 0x00800000`, uncached alias `0xA0800000`. KOS exposes it as a
block device, not as memory: `spu_memload()`, `spu_memload_sq()`,
`spu_memload_dma()`, `spu_memread()`, `spu_memset()`, `spu_dma_transfer()`.
<https://github.com/KallistiOS/KallistiOS/blob/master/kernel/arch/dreamcast/include/dc/spu.h> **[S]**

Why it is not a `.bss` target. It sits behind the G2 bus, which KOS's own header
describes as "notoriously picky … You have to be careful to use the right access
size for whatever you're working with. Also you can't be doing PIO and DMA at
the same time. Finally, there's a FIFO to contend with when you're doing PIO
stuff as well. Generally, G2 is a pain in the rear."
<https://github.com/KallistiOS/KallistiOS/blob/master/kernel/arch/dreamcast/include/dc/g2bus.h> **[S]**
G2 is 16-bit @ 25 MHz with a real transfer rate around **40 MB/s (19 clk/32 B)**
per the Sega hardware outline
<https://segaretro.org/images/8/8b/Dreamcast_Hardware_Specification_Outline.pdf> **[S]**

So: **you cannot put a C array there and let the compiler address it.** You can
use it exactly the way the GameCube used ARAM — an explicit `ARStartDMA`-shaped
block store. That is precisely what `dc/src/dc_aram.c` is already modelling, and
its header already says the sound half (8.44 MB) "DIES — samples move to AICA
sound RAM / disc streaming."

Capacity check **[D]**: `audiorom.img` is 8,300,384 B, four times AICA RAM. AICA
can hold the *resident* sample set only; the rest streams from disc. There is no
spare AICA RAM for graph data. Budget **0 MB** of AICA for the size problem, and
count it as an audio-subsystem win instead.

### 5.3 Store queues — a transfer mechanism, not storage

Two 32-byte queues at `0xE0000000`/`0xE0000020`, committed with a `pref`. They
are how you *get* data into VRAM/AICA fast; they hold nothing. KOS API:
`sq_cpy`, `sq_fast_cpy`, `sq_set*`, `sq_lock`/`sq_unlock`, `pvr_sq_load`.
"DMA is faster for transactions which are consistently large; however, the store
queues tend to have better performance and have less configuration overhead when
bursting smaller chunks."
<https://kos-docs.dreamcast.wiki/group__store__queues.html> **[S]**
Relevant to §5.1 and to the asset pool's fill path; **saves 0 bytes on its own.**

### 5.4 Operand-cache RAM (8 KB) — real, tiny

Half the SH-4 data cache can be re-purposed as directly addressable OCRAM,
mapped at `0x7c001000`, and KOS's linker script already provides a `.ocram`
NOLOAD section with a build-time size assert (see §5.1 listing).
<https://dreamcast.wiki/Useful_programming_tips> **[S]**
8,192 B of genuinely off-main-RAM storage — 0.06 % of the deficit, and it costs
you half the D-cache. Mention it only so it is not proposed as a solution.

### 5.5 The low 64 KB (`0x8c000000`–`0x8c010000`)

`LOAD_OFFSET` is overridable in the KOS linker script
(`LOAD_OFFSET = DEFINED(LOAD_OFFSET) ? LOAD_OFFSET : 0x8c010000`) **[S]**, and
KOS's own `page_phys_base` is `0x8c010000` with
`arch.h:297` validating pointers as `ptr >= 0x8c010000 && ptr < _arch_mem_top`.
That region holds BIOS-installed syscall vectors and boot state. 64 KB for a
class of failure that will not reproduce in Flycast. **Do not.**

---

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

### 6.2 Where the 14.45 MB comes from, with `-O0` frozen

`.text` + `.rodata` + `.eh_frame` = 6,318,568 B and **it does not move**, except
for the ~890 KB of `pc_assets.c` strings that are data pretending to be rodata.

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
- `kb/mem-budget.md` — the 16 MB ledger, buckets 1–12, probes
- `kb/design-shelf-hazards.md` §3.4 — the `-O2` size measurement this document withdraws
- `dc/include/dc_mem_budget.h` — the ledger as constants
- `dc/src/dc_aram.c` — the correct architectural template for a demand-resident tier
