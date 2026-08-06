# sh4zam — vendored, not a kos-ports dependency

    upstream   https://github.com/gyrovorbis/sh4zam
    commit     d4c648f4c0b7068a1f1a48533af383337869a321 (2026-07-30)
    licence    MIT (LICENSE, copied verbatim)
    vendored   2026-08-06

## Why vendored rather than installed

`kb/traps.md`: taking it from kos-ports forces a **~27 minute Docker SDK image
rebuild**, which is the single most expensive operation in this toolchain. The
library needs none of that — it includes **nothing from KOS** (verified: no
`#include <` outside its own tree), it is C11 plus `typeof`, and everything
this port uses from it is `SHZ_INLINE` in a header.

## What was copied

`include/` and `source/` verbatim, plus `LICENSE`. Nothing else: no CMake, no
Makefile, no tests, no examples, no docs. **Do not edit these files.** A local
fix goes in `dc/src/`, wrapping the library — so the next version bump is a
re-copy and not a merge.

## How it is built here

`dc/Makefile` compiles `source/*.c` as part of `$(DC_C)`'s flag set — our own
code, `$(DC_OPT)` (`-O3`), `-std=gnu11` — and puts `include/` on the include
path. It is NOT compiled with the decomp flag set: it is not vendored decomp,
it is a library we chose.

⚠️ **Warnings are suppressed for this tree only** (`-w` in its per-object rule).
It is upstream code held at a pinned commit; a warning in it is not
actionable here, and letting it into the log would bury warnings in `dc/src/`,
which we do act on.

## Why it is here at all — read this before "simplifying" it away

`kb/closed.md` passed on sh4zam in an earlier session and the reasoning is
still mostly right: **swapping KOS `mat_*`/`fipr`/`frsqrt` calls for `shz_*`
equivalents is a no-op**, because both emit the same SH-4 instructions. If
that is all a change does, it is not worth the dependency.

What is NOT a no-op, and is why this is vendored, is the part KOS has no API
for at all:

  * `shz_xmtrx_apply_*` — compose a matrix product **inside XMTRX**, so a
    compound transform never round-trips through RAM. KOS has `mat_apply`, but
    not the transpose/reverse forms, and this port's matrices arrive in the
    wrong orientation for FTRV (`dc_pvr.c` folds `P·MV` transposed by hand,
    16 iterations of scalar multiply-add per batch, precisely to work around
    that).
  * `shz_xmtrx_apply_transpose_4x4` / `apply_reverse_4x4` — the two forms that
    make the orientation fix free instead of a hand-written fold.
  * `shz_vec4_dot` / the FIPR wrappers — used to give `PSMTXMultVec` a vector
    path at all. Its FTRV path is dead code in this image (the residency kind
    it needs is only claimed by `PSMTXMultVecArray`, which `--gc-sections`
    discards), so it runs 9 multiplies and 9 adds per call, twice per vertex.

`shz_sqrtf` is deliberately **not** used: it is `shz_inv_sqrtf_fsrra(x) * x`,
an FSRRA approximation (~2^-21), not a correctly-rounded square root.
`dc/src/dc_fmath.c` binds `sqrtf` to the real FSQRT and that stays.
