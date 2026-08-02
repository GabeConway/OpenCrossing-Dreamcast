# Fitting in 16 MB — the ranked technique table and per-technique detail

The 25-row ranked table with a codegen verdict per row (§2), then the detail:
`--gc-sections` already spent, the disqualified toolchain knobs, `--icf`,
debug info, compression, romdisk, and ScummVM-style code overlays (§3). Read
before proposing any size technique. Part of `kb/research-size-reduction.md`, whose stub maps every § to its file.

Tags: **[M]** measured today against the real DC ELF, **[S]** sourced to a URL,
**[D]** derived arithmetic, **[?]/[UNVERIFIED]** not confirmed.

⚠️ `kb/levers.md` L3 re-costed every estimate in this document against
the real ELF: **every one was wrong, most by a lot, and two of the stated
mechanisms were impossible.** Use `kb/levers.md` for numbers; use this
document for the reasoning and the sources.
Sources cited here are indexed in `kb/research-size-plan.md` §8.

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
