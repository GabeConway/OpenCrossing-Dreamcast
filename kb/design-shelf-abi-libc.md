# sh-elf compile hazards — ABI, language surface, and the newlib gap

⚠️ **Two of this document's instructions violate `CLAUDE.md` §1 and must not be
followed as written.** §8 Step 0 says to add a definition to
`src/static/JSystem/JUtility/JUTFont.cpp`, and §2.4 says to add an `#include`
to `include/JSystem/JUtility/JUTFont.h`. **`src/` is never edited to make
something compile** — every compat fix goes in `dc/include/dc_prelude.h`, which
is force-included, and all 3917 TUs build that way today with zero exclusions.
Read those steps as "here is the collision", not as "here is the fix".

⭐ **2026-08-06 — the `-O2` ban is REVERSED.** The sentence that used to end the
paragraph above read *"Similarly, §3.4 and §9 recommend building at `-O2`;
codegen flags are banned by user directive."* `src/` now builds at `-Os` + a
14-TU `-O3` hot list; `.text` **5,506,964 → 2,753,700**, town FPS **11.6 →
20.6**. Two consequences inside this part: §5.4's `#pragma dont_inline` warning
is now a real exposure rather than a note, and §10's third bullet (the ARM
history is confounded, not explained) is the finding the reversal rests on.
Evidence: `kb/state-log.md`, 2026-08-06 entry. The `src/`-editing half of the
warning still stands in full.

Inline asm, 32-bit `double`, `__attribute__`, CodeWarrior pragmas, endianness
and `register` (§5); the libc/newlib gap list (§6); and the corrections this
recon made to other project docs (§10). Read when a language or libc feature
behaves differently than on the PC port. Part of `kb/design-shelf-hazards.md`, whose stub maps every § to its file.

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
> - ~~**Optimization flags anywhere below are moot** — `src/` builds at `-O0`
>   by user directive (CLAUDE.md, PLAN §3.2).~~ **[STALE 2026-08-06 — the
>   opposite is now true.]** `src/` builds at `-Os` plus a 14-TU `-O3` hot
>   list. `kb/state-log.md` 2026-08-06.
>
> Treat the ABI facts as durable and every codegen-detail claim as unverified
> until re-measured on GCC 15.2.

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

> ⚠️ **This is now a live exposure, and it is the one open item the 2026-08-06
> reversal did NOT address.** `src/` builds at `-Os`/`-O3` today, so those four
> `#pragma dont_inline` sites are being inlined. Nothing has misbehaved
> (`crashes=0` across the optimized runs in `kb/state-log.md` 2026-08-06), but
> nothing has *checked* either. If an optimized image misbehaves in a way the
> warnscan classes do not explain, find the four sites and quarantine their
> TUs with `DC_OPT_O0_EXTRA` before bisecting anything broader.

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
  ✅ **ACCEPTED 2026-08-06 — this correction was right and PLAN §3.2 has been
  rewritten around it.** The ban it argued against is gone; `src/` builds at
  `-Os` + a 14-TU `-O3` hot list. `kb/state-log.md` 2026-08-06.
* **New open question for PLAN §11:** `double` is 32-bit under
  `-m4-single-only`; `sys_matrix.c:Matrix_MtxtoMtxF` loses ~8 bits of
  fixed-point precision as a result (§5.2).
* **New open question for PLAN §11:** does Flycast trap SH-4 address errors?
  Determines whether alignment triage can happen in the emulator at all (§4.2).
