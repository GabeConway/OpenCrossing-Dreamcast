/* dc_fmath.c - libm symbols this port defines itself so newlib's are never
 * linked.
 *
 * ==========================================================================
 * WHY THIS IS NOT A CODEGEN CHANGE (CLAUDE.md §1)
 * ==========================================================================
 * CLAUDE.md bans -O1/-O2/-Os/LTO on src/ and bans proposing optimization as a
 * lever. This file changes neither: src/ is compiled with the same flags and
 * emits the same `bsr sqrtf` it always did. What changes is which object the
 * LINKER binds that call to. Layout is fair game; codegen is not.
 *
 * The mechanism is archive extraction, not symbol overriding: the link line
 * (dc/Makefile:1224) lists our objects before kos-c++ appends
 * `-Wl,--start-group -lkallisti -lm -lc -lgcc -Wl,--end-group`, so by the time
 * libm.a is scanned `sqrtf` is already defined and libm_a-wf_sqrt.o is never
 * pulled. ef_sqrt.o follows it out, because wf_sqrt.o was its only referrer
 * (AnimalCrossing.map:391-392).
 *
 * ==========================================================================
 * WHAT IT IS WORTH
 * ==========================================================================
 * MEASURED against dc/build/AnimalCrossing.map, the pre-change image:
 *
 *   libm_a-wf_sqrt.o   84 B   sqrtf, the errno wrapper
 *   libm_a-ef_sqrt.o  240 B   __ieee754_sqrtf, the software shift-and-subtract
 *                     ----
 *                     324 B   of .text this file reclaims
 *
 * The speed side is the one that matters. __ieee754_sqrtf is a bit-twiddling
 * loop over the mantissa; FSQRT is one instruction. The hottest caller is
 * guMtxNormalize (emu64_utility.c:265, #included into emu64.c:92, so it links
 * as _Z14guMtxNormalizePA4_f, 392 B at -O0), which runs three of them per
 * call. [EMU64H] puts the dl_G_MTX handler at 2.18 ms over 113 calls per
 * frame, i.e. 339 sqrtf calls per frame, all of them at -O0.
 *
 * ==========================================================================
 * PRECISION: THIS IS NOT AN APPROXIMATION
 * ==========================================================================
 * Do not confuse FSQRT with the SH-4's approximation instructions. FSRRA
 * (KOS frsqrt(), dc_pvr.c:187) and FIPR are ~2^-21 approximations. FSQRT is
 * not: it is the IEEE-754 square root, correctly rounded under the current
 * FPSCR rounding mode.
 *
 * KallistiOS sets FPSCR to 0x00040000 at startup — DN=1, RM=00, every
 * exception enable clear (KOS kernel/arch/dreamcast/kernel/startup.S:74-85 at
 * the pinned SHA 1c6398f9). RM=00 is round-to-nearest-even, which is the mode
 * newlib's software routine also targets. So for every normal input this
 * returns the SAME BITS newlib returned. There is nothing to A/B on a
 * screenshot.
 *
 * The three input classes where the two differ, all consequences of that same
 * FPSCR word:
 *
 *   x < 0     FSQRT signals invalid; with the enable bit clear the result is
 *             qNaN. newlib also returned NaN, and additionally set errno =
 *             EDOM. Nothing in src/ or dc/ reads errno after a sqrtf — the
 *             decomp has no errno handling at all — so the NaN is the whole
 *             observable contract and it is unchanged.
 *   -0.0      IEEE-754 requires sqrt(-0) = -0 and both paths honour it.
 *   denormal  DN=1 flushes denormal inputs to zero, so this returns +0 where
 *             newlib returned ~1e-22. Below any coordinate this game holds.
 *
 * ==========================================================================
 * WHAT IS DELIBERATELY LEFT ALONE
 * ==========================================================================
 *  - double sqrt(). src/game/m_collision_bg.c pulls libm_a-w_sqrt.o (180 B)
 *    and libm_a-e_sqrt.o (652 B) — 832 B, and a far slower routine per call
 *    than the float one. Reaching it needs FSQRT in double-precision mode,
 *    which means toggling FPSCR.PR around the instruction from inside an asm
 *    block while GCC believes PR is 0. That is a mode-switch hazard, not a
 *    one-liner, and collision math is the wrong place to find out. Left for a
 *    stage that can measure it.
 *  - sinf/cosf. FSCA is a genuinely different accuracy story (its input is a
 *    16-bit fixed-point angle), and its four live call sites are all in
 *    src/game/m_camera2.c:87-90. Separate stage, separate argument.
 *
 * Kill switch (CLAUDE.md): -DDC_NO_FSQRT compiles this file to nothing and
 * newlib's sqrtf comes back on the next link.
 */
#include "dc_platform.h"

#ifndef DC_NO_FSQRT
#define DC_USE_FSQRT 1
#else
#define DC_USE_FSQRT 0
#endif

/* The host-stub build has no SH-4 and must keep the host's libm. */
#if defined(DC_HOST_STUB)
#undef  DC_USE_FSQRT
#define DC_USE_FSQRT 0
#endif

#if DC_USE_FSQRT

#include <dc/fmath.h>   /* fsqrt() -> __fsqrt() -> one `fsqrt` instruction */

/* Matches newlib's prototype exactly; anything else and the calls in src/,
 * which are already compiled, bind to a signature we do not implement.
 *
 * KOS's fsqrt() is `static inline __pure`, so this is one instruction plus the
 * function prologue — the call from src/ survives, only its body shrinks. It
 * is deliberately NOT __builtin_sqrtf(): GCC is free to lower that back to a
 * call to this very function, and the recursion would be silent. */
float sqrtf(float x) {
    return fsqrt(x);
}

#endif /* DC_USE_FSQRT */
