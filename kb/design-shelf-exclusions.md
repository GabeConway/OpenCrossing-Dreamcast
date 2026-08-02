# sh-elf compile hazards — what fails to compile, and why

⚠️ **Two of this document's instructions violate `CLAUDE.md` §1 and must not be
followed as written.** §8 Step 0 says to add a definition to
`src/static/JSystem/JUtility/JUTFont.cpp`, and §2.4 says to add an `#include`
to `include/JSystem/JUtility/JUTFont.h`. **`src/` is never edited to make
something compile** — every compat fix goes in `dc/include/dc_prelude.h`, which
is force-included, and all 3917 TUs build that way today with zero exclusions.
Read those steps as "here is the collision", not as "here is the fix".
Similarly, §3.4 and §9 recommend building at `-O2`; codegen flags are banned by
user directive. (Noted 2026-08-02 while splitting this document.)

The headline compile result (§0), the full reading of `pc/CMakeLists.txt` and
its two `-O2` confounds (§1), and the exact DC exclusion/fix list — the 12 TUs
that do not build (§2). Read before touching the source exclusion list or the
per-TU C-standard rules. Part of `kb/design-shelf-hazards.md`, whose stub maps every § to its file.

Written 2026-08-01. **Everything marked VERIFIED below was measured**, not
reasoned about: `sh-elf-gcc 9.3.0` from the `einsteinx2/dcdev-kos-toolchain`
image (KOS `525cbda`, newlib 3.3.0), run against this repo's actual `src/`
tree. Items marked UNVERIFIED say so explicitly.

> **Toolchain caveat.** All compiler measurements used **GCC 9.3.0**. PLAN §8
> targets dc-chain **GCC 14+** per sm64-dc. ABI facts (`__BIGGEST_ALIGNMENT__`,
> `__SIZEOF_DOUBLE__`, char signedness, absence of 64-bit load/store) are
> stable across GCC versions. Codegen-detail claims (store merging, memcpy
> inlining) must be re-checked once the real M0 container exists.

> ## ⚠️ Superseded in part — read this before trusting anything below
>
> **Updated 2026-08-01.** The real M0 container now exists and is *not* the
> image this document measured: `opencrossing-dc:sdk` is **GCC 15.2.0 /
> newlib 4.6.0.20260123 / KOS 2.3.0 (`1c6398f9`)**, versus GCC 9.3.0 / newlib
> 3.3.0 / KOS `525cbda` here. All 3917 TUs compile and link against it with
> **zero exclusions and no edits to `src/`**, so this document's exclusion
> lists are pessimistic. Known divergences:
>
> - **`-fno-builtin` (§3.1) is falsified** — it was marked "(VERIFIED)" as KOS
>   convention and it breaks the link. See the flag table.
> - **The §2.3 header-collision scan missed the two collisions that actually
>   bit us**, because newer KOS/newlib headers introduce them: KOS
>   `dc/fmath.h:109`'s `static inline float fsqrt(float)` versus the decomp's
>   `math64.h:34` `#define fsqrt(x) sqrtf(x)`, and POSIX `link()` versus the
>   decomp's `typedef struct link_ link` arriving through `<stdio.h>` →
>   `<sys/stdio.h>` → `<unistd.h>`. Both are fixed in
>   `dc/include/dc_prelude.h`; the second cannot be fixed with a blanket
>   `-Dlink=`, which renames both sides.
> - **Char signedness (§2.x):** this image defaults to **SIGNED**, so
>   `-fsigned-char` is belt-and-braces, not load-bearing.
> - **Optimization flags anywhere below are moot** — `src/` builds at `-O0`
>   by user directive (CLAUDE.md, PLAN §3.2).
>
> Treat the ABI facts as durable and every codegen-detail claim as unverified
> until re-measured on GCC 15.2.

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
