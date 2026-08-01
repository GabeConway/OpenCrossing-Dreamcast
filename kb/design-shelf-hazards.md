# sh-elf compile hazards for `src/` (M1 recon)

Written 2026-08-01. **Everything marked VERIFIED below was measured**, not
reasoned about: `sh-elf-gcc 9.3.0` from the `einsteinx2/dcdev-kos-toolchain`
image (KOS `525cbda`, newlib 3.3.0), run against this repo's actual `src/`
tree. Items marked UNVERIFIED say so explicitly.

> **Toolchain caveat.** All compiler measurements used **GCC 9.3.0**. PLAN §8
> targets dc-chain **GCC 14+** per sm64-dc. ABI facts (`__BIGGEST_ALIGNMENT__`,
> `__SIZEOF_DOUBLE__`, char signedness, absence of 64-bit load/store) are
> stable across GCC versions. Codegen-detail claims (store merging, memcpy
> inlining) must be re-checked once the real M0 container exists.

---

## 0. Headline result

**3900 translation units were compiled for sh-elf. 3888 succeeded. 12 failed.**
The same 12 — no more, no fewer, and zero ICEs — fail identically at `-O0` and
at `-O2` with the full guard-flag set. VERIFIED (six chunked runs, whole tree,
both optimization levels).

The compile side of M1 is a ~12-file problem, not a 4765-file problem.
The *runtime* side (alignment) is the real risk and is analysed in §4.

---

## 1. What `pc/CMakeLists.txt` actually does (read in full)

### 1.1 The optimization-history comment block (lines 9–31)

Reproduced because it is the single most load-bearing artifact in the base repo:

```
# Decomp game code optimization level. History on the armhf device build:
#   -O2 → wild-pointer CRASH loop from boot (recovered per frame, black
#         screen with music);
#   -O1 → hard SIGBUS on the intro train scene (unaligned LDRD/VFP access
#         the decomp's pointer-punning provokes once GCC merges loads);
#   no -O (default) → stable. Decomp code stays UNOPTIMIZED until specific
# translation units are proven safe; pc/ platform sources are unaffected
# (they carry per-source -O2/-O3 below). Do not raise this without device-
# testing a full new-game intro (KK Slider → train → town arrival).
```

`CMAKE_C_FLAGS_RELEASE` / `CMAKE_CXX_FLAGS_RELEASE` therefore carry **no `-O`
at all**, only `-DNDEBUG -fno-delete-null-pointer-checks -fno-lifetime-dse
-fno-aggressive-loop-optimizations -fno-strict-overflow`.
Global (line 95): `-fno-strict-aliasing -fwrapv`.
`emu64.c` + `emu64_utility.c` are the only decomp TUs at `-O2` (line 540),
with a stated DEVICE-TEST GATE. `pc/` sources are `-O2`; `pc_gx_texture.c` is
`-O3`.

### 1.2 Two confounds in that history — read before trusting it

**Confound A — the missing static member.** Upstream `flyngmt/ACGC-PC-Port`
commit `4f4282766de0f2be482b087207474a7e15beba3c` ("Compile everything at
-O2", 2026-07-10) is **9 lines in CMakeLists + 4 lines in JUTFont.cpp**
(VERIFIED — fetched `.patch`):

```diff
     add_compile_options(
+        -O2
         -fno-strict-aliasing
         -fwrapv
     )
```
```diff
+/* Static member has no definition in the decomp; -O0 never emitted a
+   reference to it but -O2 does (JFWSystem.cpp). */
+OSFontHeader* JUTRomFont::spFontHeader_;
```

**This repo does not have that definition.** VERIFIED:
`include/JSystem/JUtility/JUTFont.h:155` declares
`static OSFontHeader* spFontHeader_;` and `grep -rn 'JUTRomFont::spFontHeader_'
src/` returns nothing. Combined with the link line's
`--allow-multiple-definition` (CMakeLists:602), an `-O2` build that starts
emitting references to an undefined/zero-resolved static pointer is an
extremely good match for *"wild-pointer CRASH loop from boot, black screen with
music, addr=0xDC08093A"* (kb/perf.md). **The ARM `-O2` failure may have nothing
to do with ARM.** Apply upstream's one-line fix before drawing any conclusion
about `-O2` on SH-4.

**Confound B — the `-O2` attempt was not isolated.** kb/perf.md #4: the change
was *"-O2 in RELEASE flags **+ `-mcpu=cortex-a53 -mfpu=neon-vfpv4`** in
build-armhf-docker.sh"*. VERIFIED in `pc/build-armhf-docker.sh:14`:
`TUNE="-fsigned-char -mcpu=cortex-a53 -mfpu=neon-vfpv4 -mfloat-abi=hard"`.
Enabling NEON simultaneously with `-O2` turns on auto-vectorization and
64-bit VFP load/store forms — which is also the most likely origin of the
separate `-O1` SIGBUS attributed to "unaligned LDRD/VFP". Neither `-O1` nor
`-O2` was ever tested on ARM *without* the FPU/CPU change.

### 1.3 The exclusion filters (all 35, verbatim regex → 920 files removed)

Reproduced programmatically and cross-checked against the tree: **4765 `.c` +
55 `.cpp` = 4820 total → 911 `.c` + 9 `.cpp` excluded → 3854 `.c` + 46 `.cpp` =
3900 kept** (VERIFIED).

Excluded by count: `src/static/dolphin/*` 96, `MSL_C.PPCEABI.bare.H` 38,
`libultra` 33, `TRK_MINNOW_DOLPHIN` 22, `furniture/` 696, `Runtime.PPCEABI.H` 6,
`actor/npc/*_anime.c` 6, `Famicom` 4, plus singletons.

### 1.4 Per-TU C standard handling

| Selector | Flags |
|---|---|
| all decomp `.c` (default) | `-w -std=gnu89 -fpermissive` |
| `jaudio_NES/game/*.c`, `jaudio_NES/internal/*.c`, `libforest/emu64/*.c` | `-w -std=gnu11 -fpermissive` |
| `emu64.c`, `ja_calc.c`, `jammain_2.c`, `game64.c` | `LANGUAGE CXX`, `-w -fpermissive` |
| all decomp `.cpp` | `-w -fpermissive` |
| `pc/` sources | `-O2 -std=gnu11 -Wall -Wextra` + suppressions |

`CMAKE_C_STANDARD` is deliberately **not** used (comment at line 4): CMake
appends the standard flag *after* `COMPILE_OPTIONS`, which would defeat the
per-TU gnu89 override.

Warning suppression: `-w`, `-fpermissive`, `-Wno-return-type`,
`-Wno-error=return-type`, `-fno-stack-protector`; Clang additionally
`-Wno-c99-designator -Wno-initializer-overrides`.
`add_compile_options($<$<COMPILE_LANGUAGE:C>:-Dnullptr=NULL>)` — C only.

### 1.5 The `main` renames

* `src/main.c` → `COMPILE_DEFINITIONS "main=ac_entry"`
* `src/static/boot.c` → `COMPILE_DEFINITIONS "main=boot_main"`

Both are still required on Dreamcast (KOS owns `main`).

---

## 2. Exact source exclusion list for a DC build

Start from the 35 PC filters **unchanged** (§1.3) — every one of them is
PC-generic, not GLES/SDL-specific — then add the following **DC-only**
exclusions/fixes. The list is complete with respect to the whole-tree compile:
these are literally the only 12 files that do not build.

### 2.1 Must be excluded or given a DC platform header (5 files)

These decomp TUs were modified by the base port to `#include "pc_platform.h"`,
which pulls `<SDL.h>` and `<sys/mman.h>`. VERIFIED failure.

```
src/graph.c
src/game.c
src/game/m_play.c
src/game/m_actor.c
src/static/libforest/emu64/emu64.c      (compiled as C++)
```

**Fix, not exclusion**: provide `dc/include/dc_platform.h` exposing the same
symbols and make these five include a neutral shim. They are all core TUs;
excluding them is not an option.

### 2.2 SDL used directly in decomp code (1 file)

```
src/static/jaudio_NES/internal/os.c      # `#include <SDL.h>`, SDL_mutex/SDL_Delay
```
VERIFIED: compiles cleanly for sh-elf against a 6-line stub SDL header, so the
port is mechanical — swap `SDL_CreateMutex/LockMutex/UnlockMutex/Delay` for
KOS `mutex_t` + `thd_sleep`.

### 2.3 KOS macro collision — `page_count` (4 files)

VERIFIED root cause:
`/opt/toolchains/dc/kos/kernel/arch/dreamcast/include/arch/arch.h:34`
```c
#define page_count      ((16*1024*1024 - 0x10000) / PAGESIZE)
```
and the decomp has struct fields with that name:
* `include/m_notice_ovl.h:38` — `u8 page_count;`
* `include/m_address_ovl.h:42` — `u8 page_count;`

Breaks `src/game/m_notice_ovl.c`, `m_address_ovl.c`, `m_editor_ovl.c`,
`m_submenu_ovl.c`.

This is **unavoidable by include ordering**: `include/dolphin/types.h:64`
includes `<stdio.h>` → `sys/types.h` → `sys/_pthreadtypes.h` → `sys/sched.h` →
`kos/thread.h` → `arch/arch.h`. Every TU in the project sees the macro.
Fix: `#undef page_count` in a DC prelude header force-included after the system
chain (`-include dc/include/dc_prelude.h`), or rename the two fields.

**Two further latent collisions found by a full macro-vs-field scan** (VERIFIED
scan, not yet observed breaking anything):

| Identifier | Decomp site | Macro source |
|---|---|---|
| `errno` | `include/libultra/osContPad.h:63,70,77,85` (`u8 errno;`) | `sh-elf/include/sys/errno.h:14` `#define errno (*__errno())` |
| `direct` | `include/m_demo.h:117` (`int direct;`) | `sh-elf/include/sys/dir.h:8` `#define direct dirent` |

Neither header is currently on the reachable include path; both belong in the
`dc_prelude.h` `#undef` block as insurance.

### 2.4 C++ TU missing transitive libc declarations (1 file)

```
src/static/JSystem/JFramework/JFWDisplay.cpp
  include/JSystem/JUtility/JUTFont.h:61,69 — 'strlen' was not declared
  JFWDisplay.cpp:378                      — 'memcpy' was not declared
```
glibc's C++ headers drag `<string.h>` in transitively; newlib's do not.
Fix: add `#include <string.h>` to `JUTFont.h` (upstreamable).

### 2.5 Harness-only (not a real defect)

`ja_calc.c` / `jammain_2.c` failed in my run with *"'NULL' was not declared"* —
that is my harness applying `-Dnullptr=NULL` to C files that CMake compiles as
C++. The CMake generator expression already prevents this. Keep the C-only
guard on the DC build.

### 2.6 Additional DC exclusions expected but NOT compile-blocking

`src/static/GBA2/JoyBoot.c` (GBA link cable), `src/static/dvderr.c`, the
`famicom_emu.c` shim — these compile fine but are dead weight; drop them for
binary size once `dc/` stubs settle. Not urgent.

---

## 3. Recommended flag set, justified per flag

All flags below were **individually VERIFIED to be accepted by `sh-elf-gcc
9.3.0`** (compile test, rc=0).

### 3.1 Global, every TU

| Flag | Why |
|---|---|
| `-ml -m4-single-only` | Mandatory. KOS's own `environ_dreamcast.sh` sets exactly this (VERIFIED), and libc/libm/libkallisti are built with it. Deviating breaks ABI compatibility with the prebuilt libraries. |
| `-fno-strict-aliasing` | The decomp type-puns constantly (`*(u64*)&this->gfx`, `*(float*)&int`). Upstream `4f428276` proves this is one of the two flags that made `-O2` viable on x86. Already global in the base repo. |
| `-fwrapv` | Signed-overflow UB. Second half of upstream's `-O2` fix. |
| `-fno-delete-null-pointer-checks` | Base-repo guard: decomp dereferences then null-checks; GCC would delete the check. |
| `-fno-lifetime-dse` | Base-repo guard: dead stores into objects about to die are load-bearing in JSystem ctors/dtors. |
| `-fno-aggressive-loop-optimizations` | Base-repo guard: decomp loops index past declared array bounds. |
| `-fno-strict-overflow` | Base-repo guard: pointer-arithmetic overflow. |
| `-fno-stack-protector` | Base-repo flag; also KOS has no `__stack_chk_fail` guarantee for game TUs. |
| `-fno-builtin` | KOS convention (`KOS_CFLAGS`, VERIFIED). Also stops GCC recognizing hand-rolled `Jac_bcopyfast`-style loops and replacing them with `memcpy` under *stronger* alignment assumptions. |
| `-ffunction-sections -fdata-sections` (+ `-Wl,--gc-sections`) | KOS default. Directly attacks PLAN §3.1's ≤4 MB binary target. |
| `-fsigned-char` | **Not strictly required.** VERIFIED: `sh-elf-gcc` defaults to **signed** char (`(char)-1 < 0` is true; `__CHAR_UNSIGNED__` is undefined). This **corrects PLAN §8 and kb/base-repo-map.md**, both of which state SH-4 GCC chars are unsigned. Keep the flag anyway — zero cost, documents intent, survives a toolchain swap. |
| `-DTARGET_PC` **and** `-DTARGET_DC` | **Critical.** `#ifdef TARGET_PC` in `src/` guards the base port's little-endian correctness fixes — byte-wise texconv in `emu64.c:840-864`, the swapped `u16` pair ordering in `sys_matrix.c:_MtxF_to_Mtx`/`Matrix_MtxtoMtxF`, the overlap-safe `Jac_bcopy` in `sample.c:33`. `TARGET_PC` here means "not GameCube", not "PC". Dropping it silently reintroduces big-endian assumptions. Add `TARGET_DC` for genuinely DC-specific branches. |

### 3.2 Explicitly NOT used

| Flag | Why not |
|---|---|
| `-mdalign` | Aligns doubles to 64 bits and **changes calling conventions** — GCC's own docs warn the standard library must be rebuilt. Prebuilt newlib/KOS are not. And `double` is 32-bit anyway (§5.2), so it buys nothing. |
| `-mfmovd` | Enables 64-bit `fmov.d` register-pair moves, which **require 8-byte alignment on a 4-byte-max-alignment ABI**. This is precisely the ARM LDRD hazard class, voluntarily re-imported. Off by default (VERIFIED); keep it off. |
| `-ffast-math` | Breaks the exact 32-bit float semantics the GameCube code depends on (same reason the PC build forces `-msse2 -mfpmath=sse` on x86). |
| `-mno-unaligned-access` | **Does not exist on SH.** VERIFIED against the full `--help=target` output. SH is unconditionally `STRICT_ALIGNMENT`. |
| `-mrelax` | Link-time address shortening; interacts badly with `--gc-sections` diagnosis during triage. Revisit at M4 for size. |

### 3.3 Per-TU, mirroring `pc/CMakeLists.txt`

```
decomp C (default) : -w -std=gnu89
jaudio_NES/*, emu64/*  : -w -std=gnu11        (+ -Dnullptr=NULL, C only)
emu64.c, ja_calc.c, jammain_2.c, game64.c : compiled as C++, -w -fpermissive
decomp C++        : -w -fpermissive -fno-exceptions -fno-rtti -fno-operator-names
src/main.c        : -Dmain=ac_entry
src/static/boot.c : -Dmain=boot_main
```

`-fno-exceptions -fno-rtti` (KOS_CPPFLAGS) are safe: VERIFIED that no kept
`.cpp` or C++-compiled `.c` uses `throw`, `try`, `catch`, `typeid`, or
`dynamic_cast`. `JKRHeap.cpp:321-350` overrides global `operator new`/`delete`
onto JKRHeap — with KOS this is fine (no interposition problem, so the base
repo's `version_script.lds` hack is unnecessary on DC).

### 3.4 Optimization level — recommendation

**Ship `-O2` for game code from day one, `-Os` for `src/data/` and cold TUs.**
Rationale beyond speed — VERIFIED size measurement on a 648-TU representative
sample (1/6 of the tree, includes `src/data`):

| | `.text` | `.data` | `.bss` |
|---|---|---|---|
| no `-O` | 945,214 | 348,436 | 469,750 |
| `-O2`   | **488,474 (−48.3 %)** | 314,671 | 469,586 |
| `-Os`   | **431,076 (−54.4 %)** | 314,671 | 469,586 |

Extrapolated ×6 for the whole tree: `.text` ≈ 5.7 MB at `-O0` → **≈ 2.9 MB at
`-O2` → ≈ 2.6 MB at `-Os`**. On a 16 MB machine, `-O0` costs roughly **3 MB of
RAM** on top of being 2–3× slower. `-O0` is not an option on Dreamcast the way
it was on a 1 GB handheld. This reframes PLAN §3.2: `-O2` is not an
optimization, it is a *budget requirement*.

---

## 4. Alignment hazards — the actual risk

### 4.1 SH-4 vs ARM: the exposure is *different*, and larger

VERIFIED compiler facts (probe programs, `sh-elf-gcc -O2 -S`):

* `__BIGGEST_ALIGNMENT__` is **4**. `__alignof__(long long)` = 4,
  `__alignof__(double)` = 4. `offsetof(struct{char c; long long ll;}, ll)` = 4.
* **SH-4 has no 64-bit integer load/store.** `*(u64*)a != *(u64*)b` on a
  `Gfx`-shaped union compiles to *two* `mov.l` at offsets 0 and 4 — never a
  paired 8-byte access. The **ARM LDRD class simply does not exist here.**
* `sizeof(Gfx)` = 8 but `__alignof__(Gfx)` = **4** (VERIFIED against this
  repo's `include/PR/gbi.h`; the `long long force_structure_alignment` member
  no longer forces 8). `sizeof(Vtx)` = 16, align 4, `offsetof(Vtx_t,cn)` = 12.
* GCC **does not** merge four adjacent `mov.b` stores into a `mov.l`, and does
  not merge two adjacent `mov.w` loads into a `mov.l`, at `-O2`. It respects
  `STRICT_ALIGNMENT`. (ARM's store-merging under NEON is where the base port's
  `-O1` trouble plausibly came from.)
* `__builtin_memcpy(d,s,4)` with unknown alignment emits an out-of-line
  `memcpy` call — byte-safe.
* `__attribute__((packed))` and `__attribute__((aligned(1)))` produce
  **byte-wise load/shift/or sequences** — i.e. GCC SH *does* have a safe
  unaligned-access idiom, it just needs to be asked.

**But**: ARMv7-A with `SCTLR.A=0` (the Linux default, hence the base port)
services unaligned `LDR`/`LDRH`/`STR`/`STRH` **in hardware, silently**. x86
tolerates everything. SH-4 raises a CPU address error on **any** misaligned
`mov.w` or `mov.l`. There is no `movua.l` on SH-4 (that is SH-4A).

> **Therefore: every 16- and 32-bit unaligned access in this codebase has
> never been exercised by any existing port.** The base repo's clean ARM record
> is *not* evidence that these sites are aligned. This is the single largest
> unknown in M1/M2, larger than the `-O2` question.

### 4.2 Triage hook — VERIFIED available

KOS exposes exactly what is needed:

```c
/* kernel/arch/dreamcast/include/arch/irq.h */
#define EXC_DATA_ADDRESS_READ   0x00e0
#define EXC_DATA_ADDRESS_WRITE  0x0100
typedef void (*irq_handler)(irq_t source, irq_context_t *context);
int irq_set_handler(irq_t source, irq_handler hnd);
int irq_set_global_handler(irq_handler hnd);
#define CONTEXT_PC(c)  ((c).pc)      /* also .pr, .r[0..15], .sr */
```
The faulting address is in the SH-4 **TEA** register at `0xff00000c` — KOS
already reads it (`kernel/arch/dreamcast/kernel/mmu.c:27`,
`static volatile uint32 * const tea = (uint32 *)(0xff00000c);`). KOS's default
behaviour is `dbglog(DBG_DEAD, "Unhandled exception: PC %08lx, code %d, evt
%04x")` then `arch_panic("unhandled IRQ/Exception")` — so an unhandled
misalignment is loud, not silent. Good.

**Day-one deliverable for `dc/`:** an address-error handler that logs
`PC`, `PR`, `TEA`, and `r0..r15`, then either resumes past the instruction or
panics. Feed `PC` to `sh-elf-addr2line -e OpenCrossing.elf` → file:line →
the offending TU, with **no bisection needed**. This is strictly better than
the base repo's SIGBUS handler because SH-4 gives you the faulting address.

UNVERIFIED: whether **Flycast** faithfully raises the SH-4 address error for
misaligned loads, or quietly services them like a host CPU. Flycast's issue
tracker shows plenty of "Unhandled SH4 exception" reports but nothing
confirming *alignment* fidelity specifically. **Assume Flycast is permissive
until proven otherwise** — that would make Flycast unable to catch this entire
bug class, and would make real-hardware CD-R testing mandatory earlier than
PLAN §8 assumes. **Resolve this at M0** with a deliberate `*(u32*)(buf+1)`
test in the hello-world.

### 4.3 Ranked hazard list

Ranked by (execution frequency) × (probability the address is not naturally
aligned). All line numbers verified in this tree.

| # | Site | Frequency | Risk | Note |
|---|---|---|---|---|
| 1 | `src/static/jaudio_NES/internal/rspsim.c:635` — `sp120 = (s16*)cmdLo;` | per audio frame | **HIGH** | `cmdLo` is a raw address decoded from the RSP command word, pointing into ADPCM/wave-bank data at arbitrary byte offsets. Nothing forces even alignment. |
| 2 | `rspsim.c:62,120,244,245,418,419,432,471-477,606,638` — `(s16*)&DMEM[ofs]` | per audio frame, hundreds/frame | **HIGH** | `DMEM` itself is `ATTRIBUTE_ALIGN(32)` (`rspsim.c:8`) ✅, but `ofs` is `cmdLo & 0xFFFF` / `cmdLo >> 16` — command-supplied. N64 microcode convention keeps these 8/16-byte aligned; nothing *enforces* it. |
| 3 | `src/static/libforest/emu64/emu64.c:906,908,916` — `*(u32*)(converted_addr+n) = *(u32*)(addr+ofs);` | every texture upload (inner loop) | **HIGH** | `ofs = this->tmem_swap(x_ofs, line_siz)`; the 4-bit path at :916 computes `x_ofs = (blk_x + y_ofs*wd)/2`, which is **not** guaranteed ≡0 mod 4 for odd `wd`. Note the sibling 16-bit path at :848-863 was *already* rewritten byte-wise under `#ifdef TARGET_PC` — the u32 paths were not. |
| 4 | `emu64.c:890,891,898` — `u32* src = (u32*)(addr + ofs);` (IA8 path) | every IA8 texture upload | **HIGH** | same mechanism as #3. |
| 5 | `emu64.c:4715` — `emu_vtx_p->color.raw = *(u32*)(&vtx_p->v.cn[0]);` | **per vertex** | MED-HIGH | `offsetof(Vtx_t,cn)`=12, `sizeof(Vtx)`=16 (VERIFIED) → safe iff the `Vtx*` base is 4-aligned. Bases come from N64 segment-resolved addresses. Highest execution count in the whole list; a 1-in-10⁶ misalignment is still a crash every few seconds. |
| 6 | `emu64.c:3850,3879,3907,3919,3938` — `*(u16*)tlut_addr`, `(u16*)addr` | per TLUT load | MED | 16-bit access; needs only even alignment, but TLUT addresses are segment-derived. |
| 7 | `src/static/jaudio_NES/internal/system.c:30,36,46,1063,1094` — `swap32_inplace((u32*)p)`, `BANK_ENTRY` macro `(((u32*)((u32)ctrl)) + idx)` | per audio-bank load | MED | The base port's **added** LE-conversion code, walking `u8*` bank data with `u32*` casts. On DC these run over disc-streamed bank images whose in-file offsets are not our choice. |
| 8 | `system.c:983-985,1437-1439,2056` — `((u16*)rom_addr)[0..2]`, `*(u32*)wavetable` | per bank load | MED | same class. |
| 9 | `src/static/libc64/__osMalloc.c:143` — `u32* e = (u32*)(OS_MALLOC_BLOCK2DATA(block) + block->size);` | every guarded alloc/free | MED | `block->size` is a caller-supplied byte count with **no rounding**; `+ size` lands at arbitrary alignment. `:142` (base only) is fine. |
| 10 | `src/game/m_player_lib.c:1132,1201,1222,1253,1363,1368` — `(u16*)obj_ex->banks[n].ram_start`, `bcopy((u16*)pal_rom_p, …)`, `mPlib_ByteSwapPlayerPalette((u16*)pal_p)` | per player-model / palette load | MED | palette byte-swapping over ROM-sourced pointers. |

Second tier (lower frequency or lower misalignment probability, still worth
auditing):

* `src/static/JSystem/JKernel/JKRDecomp.cpp:206,214` — `*(u32*)(src_buffer+4)`,
  `*(u32*)src_buffer` (Yaz0 header; base is normally 32-aligned).
* `src/static/JSystem/JFramework/JFWSystem.cpp:38,46,47` —
  `*(u32*)(data+0x0C)`, `*(u32*)(block+0x00/0x04)` (bootdata block walk).
* `src/static/JSystem/JKernel/JKRHeap.cpp:69` — `*(u32*)((start + 0x28))`.
* `src/game/m_home.c:242,243` — `*((u32*)&home->floors[…].layer_main.ftr_switch + 1) = 0;`
  (source comment: *"ftr_switch might be a union?"* — it is a punned write).
* `src/game/m_home.c:245,251,435-438,591` — `(u16*)…layer_main.items`.
* `src/static/jaudio_NES/internal/cmdstack.c:57,78,82,124,141,143` —
  `((int*)p)[…]`, `((int (*)(int)) * (int*)(p + 0x14))(…)` — a **function
  pointer loaded from a raw byte offset**; misalignment here jumps to garbage.
* `src/static/jaudio_NES/game/melody.c:646,678` — `((u16*)AG.groups[0].seq_data)[2]`,
  `(u16*)(dst + 4)`.
* `src/game/m_camera2.c:2537` — `*(u16*)(kk_save_area + 2)`.
* `src/game/m_island.c:173,193,239,248,257` — `mISL_int(u32*,u32*)`,
  `mISL_u64(u64*,u64*)` over save/GBA transfer structs (u64 variant is two
  `mov.l` on SH-4, so 4-alignment suffices).
* `src/effect/ef_sandsplash.c:54,55,62,63,71,72` — `*(s16*)ct_arg` where
  `ct_arg` is a generic `void*` actor argument.
* `src/actor/ac_snowman.c:83,92,157,182` — `(f32*)mEv_get_common_area(...)`
  — float read out of a byte-addressed event-common area.
* `src/static/boot.c:618,621` — `(u32*)var1`, `(u32*)var2->stackEnd + 1`.

**Assessed SAFE after inspection** (listed so nobody re-audits them):
`src/system/sys_matrix.c:534,634,635` (`(u16*)&Mtx.m[0][0]` — `Mtx` is 4-aligned,
`sizeof`=64 VERIFIED, all accesses at even offsets);
`src/static/libc64/qrand.c:19,25` (`*(float*)&__qrand_itemp` — a static `u32`,
always aligned; pure aliasing, handled by `-fno-strict-aliasing`);
all `emu64.c` `*(u64*)&this->gfx` sites (1496, 3949-4001, 5122, 5349-5350,
5853) — two `mov.l` at a 4-aligned member, VERIFIED codegen.

### 4.4 The fix idiom

Do **not** sprinkle `memcpy`. Add one header, `dc/include/dc_unaligned.h`:

```c
typedef __attribute__((aligned(1))) unsigned int  u32_ua;
typedef __attribute__((aligned(1))) unsigned short u16_ua;
#define LD32U(p)     (*(const u32_ua *)(p))
#define ST32U(p, v)  (*(u32_ua *)(p) = (v))
```
VERIFIED: `aligned(1)` makes GCC SH emit the byte-wise sequence, no library
call, no alignment fault. Cost is ~6 instructions vs 1. Apply **only** at sites
the exception handler actually reports — do not pre-emptively pessimise #5
(per-vertex) without measuring.

---

## 5. Architecture-specific code inventory

### 5.1 Inline assembly — VERIFIED: **zero reachable sites**

39 files in the tree contain CodeWarrior `asm` blocks (PPC), but after the §1.3
filters only two survive, and neither is live:

* `src/static/JSystem/JKernel/JKRHeap.cpp:390` — a comment.
* `src/static/JSystem/JUtility/JUTException.cpp:627` —
  `asm u32 JUTException::getFpscr()` is inside `#ifndef TARGET_PC`; the
  `#else` branch is `return 0;`. Keeping `-DTARGET_PC` (see §3.1) keeps this
  disabled.

All real PPC asm (`OSContext.c`, `OSCache.c`, `mtx.c`, `GXFifo.c`,
`ReconfigBATs.c`, `ks_nes_core.cpp`, …) is already excluded.

### 5.2 `double` is 32 bits — the sleeper issue

VERIFIED: with `-m4-single-only` (which KOS mandates),
`__SIZEOF_DOUBLE__` is **4**. `double dsum(double,double)` compiles to
`fmov fr4,fr0; fadd fr5,fr0` — single precision. `f64` in this codebase is
`typedef double f64` (`include/types.h:65`, `include/dolphin/types.h:14`,
`include/PR/ultratypes.h:58`), so **every `f64` in the decomp silently becomes
`f32` on Dreamcast**, unlike GameCube (Gekko has real 64-bit doubles).

Blast radius is small and enumerable — VERIFIED: exactly **7 kept TUs** touch
`f64`/`double`:

| File | occurrences | verdict |
|---|---|---|
| `src/system/sys_matrix.c` | 32 | **PROBLEM** |
| `src/graph.c` | 4 | audit |
| `src/static/libc64/__osMalloc.c` | 2 | benign (printf) |
| `src/static/jaudio_NES/internal/system.c` | 2 | audit |
| `src/game/m_debug_mode.c` | 2 | benign |
| `src/static/jaudio_NES/internal/driver.c` | 1 | audit |
| `src/game/m_select.c` | 1 | benign |

`sys_matrix.c:Matrix_MtxtoMtxF` (line 633+) is the real one:
```c
dest->xx = ((m1[1] << 0x10) | m2[1]) * (1 / (f64)0x10000);
```
That is a full 32-bit fixed-point 16.16 value multiplied by 1/65536. With a
24-bit mantissa the low 8 bits are lost → roughly 8 fractional bits survive
instead of 16, i.e. ~0.004 quantisation on matrix elements. On GameCube this
was exact. **Expect visible geometry/animation jitter.** Fix by computing in
integer/fixed-point, or splitting into a `(hi * 1.0f) + (lo * (1.0f/65536))`
form. Log this against PLAN §11 as a new open question; it is an M2 correctness
item, not an M1 blocker.

### 5.3 `__attribute__` usage — VERIFIED safe

7664 uses across the tree; in kept files, **only two kinds**: `aligned` (7664
via `ATTRIBUTE_ALIGN(n)` in `include/types.h:105`) and one `visibility("hidden")`
(`JKRHeap.cpp:316`, a Linux-interposition workaround that becomes a no-op).
VERIFIED that `__attribute__((aligned(32)))` on statics works on sh-elf —
`.bss` gets `2**5` alignment and the symbol lands at offset 0x20 in a test
object. `__BIGGEST_ALIGNMENT__ = 4` limits *implicit* alignment only; explicit
`aligned(n)` is honoured. **No `packed` anywhere in the codebase** (VERIFIED —
the only `packed` hits are variable names and comments).

The `visibility("hidden")` + `version_script.lds` machinery for
`operator new`/`delete` is unnecessary on KOS (no dynamic symbol interposition);
drop it in `dc/`.

### 5.4 CodeWarrior `#pragma`s — VERIFIED harmless

In kept files: `force_active` ×10, `dont_inline` ×4, `opt_propagation` ×2,
`function_align` ×2, `push`/`pop`, `pool_data`, `inline_max_size`,
`inline_depth`, `fallthrough`. GCC ignores unknown pragmas; `-w` suppresses the
warning. **Note `#pragma dont_inline` is silently ignored** — if a TU depended
on a function *not* being inlined for correctness, `-O2` will inline it. Worth
remembering during triage.

### 5.5 Endianness and struct layout

SH-4 is little-endian ILP32, identical to the base port's targets. Every
`#ifdef TARGET_PC` byte-swap fix transfers verbatim (see §3.1's `-DTARGET_PC`
note — this is why that define is non-negotiable). Bitfield allocation order is
the same little-endian scheme GCC uses on ARM/x86, so the base port's bitfield
work carries over. `#pragma reverse_bitfields` appears only in excluded files.

### 5.6 `register` keyword

6 kept files use `register` on parameters (`m_debug_hayakawa.c`,
`m_debug_mode.c`, `m_island.c`, `track.c`, `jammain_2.c`, `JUTException.cpp`).
These are CodeWarrior ABI hints; GCC treats them as ordinary `register` and
they compiled clean. No action.

---

## 6. libc / newlib gap list

**VERIFIED: there is effectively no gap.** Method: extracted every identifier
used in call position across all 3900 kept TUs, intersected against the union of
defined symbols in `libc.a`, `libm.a`, and `libkallisti.a` (2371 symbols).

The decomp's entire libc surface is:

```
bcopy bzero index memcpy memmove memset
strcat strcmp strcpy strlen  sprintf vsnprintf printf fprintf
malloc free  exit  setjmp
acos atan2 cos cosf sin sinf sqrt sqrtf fabsf powf isinf isnan
fclose fgets fopen  clock time  tolower
```

All present. Specifics:

* `malloc/free/calloc/realloc/memalign/abort/__assert_func` come from
  **libkallisti**, not newlib (VERIFIED: absent from `libc.a`, present as
  `_malloc` etc. in `libkallisti.a`). Ordinary link order handles this.
* `bcopy`, `bzero`, `index`, `rindex` are declared in newlib's `<strings.h>`
  (VERIFIED, marked LEGACY). Argument order matches the decomp's usage
  (`bcopy(src, dst, n)` — see `src/actor/ac_flag.c:176`). No shim needed.
* **`fsqrt` is a non-issue on Dreamcast.** The base repo carries a comment that
  `_GNU_SOURCE` must not be global because glibc exposes `fsqrt(double)`
  conflicting with the decomp's `f32 fsqrt(f32)` (`src/static/libc64/math64.c:12`).
  VERIFIED: newlib 3.3.0's `math.h` declares no `fsqrt`, and `libm.a` defines
  no `_fsqrt`. The conflict is glibc-specific; drop the workaround.
* `open/close/read/write/remove` matched only C++ member functions
  (`JKRAramArchive::open`, `JKRDvdFile::close`, `JSUInputStream::read`, …),
  not POSIX calls (VERIFIED by inspecting every site). No syscall glue needed
  from the decomp side.
* `pc/include/*.h` dependencies: **24 kept decomp files** include a `pc_*.h`
  header — `pc_bswap.h` ×12, `pc_settings.h` ×8, `pc_platform.h` ×5,
  `pc_prof.h` ×3, `pc_diag.h` ×3, `pc_model_viewer.h` ×2. Only the
  `pc_platform.h` five are compile-blocking (§2.1); the other headers are
  portable C and compiled clean for sh-elf as-is. `dc/` must either provide
  equivalents or the build must keep `pc/include` on the include path.
* `glibc_compat.c` (`__isoc23_*` symver shims) is `#ifdef __arm__`-guarded and
  irrelevant here.

**Genuine gap: `<string.h>` is not transitively included by newlib's C++
headers** — the `JFWDisplay.cpp` failure in §2.4. That is the whole list.

---

## 7. SH-4 GCC findings (consolidated, all VERIFIED unless noted)

```
sh-elf-gcc 9.3.0, --print-multi-lib = ".;"   (single multilib: m4-single-only)
-m4, -m4-single, -m4-nofpu  → "not supported by this configuration"
__BYTE_ORDER__       = __ORDER_LITTLE_ENDIAN__
__BIGGEST_ALIGNMENT__= 4
__SIZEOF_DOUBLE__    = 4          __SIZEOF_LONG_LONG__ = 8
__SH4_SINGLE_ONLY__, __SH_FPU_ANY__, __sh__
__CHAR_UNSIGNED__    undefined  →  char is SIGNED by default
sizeof(void*)=4, alignof(long long)=4, alignof(double)=4
```

UB-guard flag acceptance (each compiled rc=0):
`-fno-strict-aliasing`, `-fwrapv`, `-fno-delete-null-pointer-checks`,
`-fno-lifetime-dse`, `-fno-aggressive-loop-optimizations`,
`-fno-strict-overflow`, `-fsigned-char`, `-fno-stack-protector`, `-Os/-O2/-O3`.
`-fpermissive` warns "valid for C++/ObjC++ but not for C" but does not error.

Alignment-trap semantics: SH-4 raises a CPU address error on any misaligned
`mov.w`/`mov.l`/`fmov.s`. KOS surfaces this as `EXC_DATA_ADDRESS_READ 0x00e0`
and `EXC_DATA_ADDRESS_WRITE 0x0100`, hookable with `irq_set_handler` /
`irq_set_global_handler`; unhandled → `dbglog(DBG_DEAD, ...)` + `arch_panic`.
Faulting address in TEA (`0xff00000c`).

No `-mno-unaligned-access` equivalent; SH is unconditionally strict.
`movua.l` (SH-4A unaligned load) is not emitted for `-m4-single-only`; the
compiler's only safe-unaligned idiom is `packed` / `aligned(1)` (byte-wise) or
an out-of-line `memcpy`.

UNVERIFIED: GCC 14 codegen may differ (newer store-merging and SLP passes).
Re-run the §4.1 probe set against the M0 container.

---

## 8. Triage procedure for a miscompile

You are not bisecting 4765 files. You are bisecting at most 3900, and in the
common case you are not bisecting at all.

### Step 0 — before anything, apply the two known upstream fixes
1. Add `OSFontHeader* JUTRomFont::spFontHeader_;` to
   `src/static/JSystem/JUtility/JUTFont.cpp` (upstream `4f428276`). Without
   this, `-O2` is *expected* to produce a wild-pointer crash and you will
   waste a week bisecting a one-line bug.
2. Add `#include <string.h>` to `include/JSystem/JUtility/JUTFont.h`.

### Step 1 — make faults self-identifying (cost: ~1 hour, saves days)
Install the address-error handler at boot, before anything else:

```c
static void addr_err(irq_t src, irq_context_t *ctx) {
    volatile uint32 *tea = (uint32 *)0xff00000c;
    dbglog(DBG_DEAD, "ALIGN %s pc=%08lx pr=%08lx tea=%08lx\n",
           src == EXC_DATA_ADDRESS_WRITE ? "ST" : "LD",
           CONTEXT_PC(*ctx), ctx->pr, *tea);
    for (int i = 0; i < 16; i++) dbglog(DBG_DEAD, " r%d=%08lx", i, ctx->r[i]);
    arch_panic("alignment");
}
irq_set_handler(EXC_DATA_ADDRESS_READ,  addr_err);
irq_set_handler(EXC_DATA_ADDRESS_WRITE, addr_err);
```
Then `sh-elf-addr2line -f -e OpenCrossing.elf <pc>` → function + file:line.
**Build with `-g` always** (KOS does; `-g` costs nothing at runtime and the
ELF never ships on the disc — only `1ST_READ.BIN` does). This turns the entire
§4.3 hazard list from "audit 30 sites" into "wait for the report".

Also validate the handler at M0 with a deliberate `*(u32*)(buf+1)` — and use
that same test to answer the open question of whether **Flycast** traps it at
all (§4.2). If Flycast does not, alignment triage must happen on hardware.

### Step 2 — classify the failure
* **Address-error panic** → Step 1 named the site. Fix with §4.4's `LD32U`
  idiom. No bisection.
* **Fault at a plausible-but-garbage PC / wild pointer** → suspect a missing
  definition or an inlined-away `#pragma dont_inline`. Diff the `-O0` and
  `-O2` symbol tables (`sh-elf-nm -u`) for symbols that appear only at `-O2`;
  that is exactly how `spFontHeader_` presents.
* **Silent wrong behaviour** → Step 3.

### Step 3 — bisect the *flag set*, not the files, first
Cheaper than any file bisection because each step is one full build:
`-O2` → `-O1` → `-O2 -fno-tree-vectorize` → `-O2 -fno-ipa-sra` →
`-O2 -fno-strict-aliasing -fno-schedule-insns2` → `-Os`.
6 builds ≈ 45 min total (measured: 650 TUs ≈ 67 s at `-P4` under colima, so a
full 3900-TU build ≈ 7 min).

### Step 4 — bisect the *set of TUs at -O2*, by directory
Drive it from a plain text file of TU paths with a per-TU `-O` override, so
one variable controls the partition. Bisect over these ~20 groups first, in
descending suspicion order:

```
src/static/libforest/emu64/   (6.4k LOC — already proven -O2-safe on ARM)
src/static/jaudio_NES/        (61 files, 33k LOC — rspsim is hazard #1/#2)
src/static/JSystem/           (heaps, archives, Yaz0)
src/game/                     src/actor/       src/effect/
src/system/                   src/bg_item/     src/static/libc64/
src/static/libjsys/           src/static/libu64/   src/data/  (inert tables)
src/*.c (top level)
```
`log2(20) ≈ 5` builds ≈ 35 min to pin the directory. Then binary-search within
it: `log2(600) ≈ 10` builds ≈ 70 min. **Worst case ≈ 2 hours from "it's broken
at -O2" to "this TU".** Compare: naive per-file bisection over 3900 is
`log2(3900) ≈ 12` builds — barely different, so just do the directory version,
it produces a more useful intermediate answer.

### Step 5 — per-TU fallback, permanently
Mirror the base repo's mechanism exactly: a list of TUs pinned to a lower `-O`,
with a comment naming the symptom and the date. The base repo's inverse
(`emu64.c` pinned *up* to `-O2`, with a DEVICE-TEST GATE comment) is the same
pattern and should be preserved in spirit: **any change to a TU's optimization
level requires a full new-game intro run** (KK Slider → train → town arrival),
because that is the sequence that historically exposed the alignment class.

### Step 6 — feed fixes upstream
Every misaligned cast fixed at the source is upstreamable to
`flyngmt/ACGC-PC-Port` and benefits every port. The `#ifdef TARGET_PC`
byte-wise rewrite already applied to `emu64.c:840-864` is the model: keep the
original expression under `#else` so the decomp stays matching.

---

## 9. Risk verdict on `-O2`

**Achievable, and probably mandatory.** Confidence: moderate-high on
"it compiles and links"; moderate on "it runs correctly without per-TU
fallbacks".

Arguments for:
1. All 3900 TUs compile at `-O2` with the full guard set, zero ICEs (VERIFIED).
2. The **ARM `-O2` evidence is confounded twice** (§1.2) — a missing static
   member definition that upstream had to add for exactly this reason, and a
   simultaneous `-mfpu=neon-vfpv4` switch. Neither confound exists on SH-4.
3. The specific mechanism blamed for the ARM `-O1` SIGBUS — GCC merging loads
   into 64-bit `LDRD`/VFP accesses — **cannot occur on SH-4**: max alignment is
   4, there are no 64-bit integer loads, and GCC was measured *not* merging
   adjacent byte/half stores at `-O2` (VERIFIED).
4. Upstream runs `-O2` in production on x86 with only
   `-fno-strict-aliasing -fwrapv`; we carry four additional guards.
5. `-O2` cuts `.text` by 48 % (VERIFIED). On 16 MB that is ~3 MB of RAM. `-O0`
   is arguably not affordable regardless of speed.

Arguments against / residual risk:
1. **The 16/32-bit alignment class is entirely untested by every prior port.**
   ARM services those in hardware; x86 always has. SH-4 will not. This risk is
   *independent of `-O2`* — it bites at `-O0` too — but `-O2` increases the
   chance of hitting it (more aggressive reassociation of address expressions,
   more inlining exposing cross-function alignment assumptions). Hazards #1–#5
   in §4.3 are in per-frame or per-vertex paths.
2. `#pragma dont_inline` is silently ignored by GCC; any TU relying on it for
   correctness changes behaviour at `-O2`.
3. GCC 9.3.0 was measured; GCC 14's store-merging and SLP passes are more
   aggressive and could reintroduce a merged-access hazard. Re-verify.
4. If Flycast does not trap misalignment, the feedback loop for this bug class
   runs at CD-R burn speed, not emulator speed. **This is the schedule risk,
   not the technical one.**

**Recommendation:** build `-O2` from M1, with the address-error handler in from
M0 and a per-TU downgrade list ready. Do not repeat the base repo's decision to
pin everything at `-O0` — on this hardware that trades a solvable correctness
problem for an unsolvable budget problem.

---

## 10. Corrections to existing project docs

* **PLAN.md §8** and **kb/base-repo-map.md §"Build system / flags"** state
  `-fsigned-char` is required because "SH-4 GCC defaults to unsigned char".
  **VERIFIED false** — `sh-elf-gcc` defaults to *signed* char. Keep the flag
  for explicitness, but the stated reason is wrong.
* **PLAN.md §3.2** says "SH-4 raises a CPU exception on unaligned access —
  install an exception handler". Correct, and now concrete: `EXC_DATA_ADDRESS_READ`
  / `EXC_DATA_ADDRESS_WRITE`, `irq_set_handler`, TEA at `0xff00000c`.
* **PLAN.md §3.2** frames the ARM `-O2`/`-O1` history as evidence about
  optimization on strict-alignment ISAs. §1.2 above shows both data points are
  confounded; treat them as *unexplained*, not as *explained by alignment*.
* **New open question for PLAN §11:** `double` is 32-bit under
  `-m4-single-only`; `sys_matrix.c:Matrix_MtxtoMtxF` loses ~8 bits of
  fixed-point precision as a result (§5.2).
* **New open question for PLAN §11:** does Flycast trap SH-4 address errors?
  Determines whether alignment triage can happen in the emulator at all (§4.2).
