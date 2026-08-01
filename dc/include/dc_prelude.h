/* dc_prelude.h — force-included (-include) into EVERY translation unit of the
 * Dreamcast build, decomp and platform alike.
 *
 * WHY THIS EXISTS (kb/design-shelf-hazards.md §2.3, §2.4)
 * ------------------------------------------------------
 * KallistiOS defines object-like macros whose names collide with struct field
 * names in the decomp. The collision is *unavoidable by include ordering*:
 *   include/dolphin/types.h -> <stdio.h> -> sys/types.h -> sys/_pthreadtypes.h
 *   -> sys/sched.h -> kos/thread.h -> arch/arch.h
 * i.e. every TU in the project drags arch/arch.h in transitively. So the fix
 * has to be "pull the system chain in FIRST, then #undef", which is exactly
 * what this header does. It must therefore be force-included, not #include'd
 * from a decomp header.
 *
 * DO NOT add game declarations here. This file is only:
 *   1. the system-header chain that defines the offending macros,
 *   2. the #undefs,
 *   3. the one genuine newlib gap (<string.h> not transitively pulled by
 *      newlib's C++ headers — breaks JUTFont.h/JFWDisplay.cpp).
 */
#ifndef DC_PRELUDE_H
#define DC_PRELUDE_H

/* 1. Pull, as narrowly as possible, the headers that define the identifiers we
 *    have to neutralise. Deliberately NOT <stdio.h>: on current KOS,
 *    <stdio.h> -> <sys/stdio.h> -> <unistd.h>, which declares
 *    `int link(const char*, const char*)` and collides with the decomp's
 *    `typedef struct link_ link;` (include/jaudio_NES/audiostruct.h:15).
 *    Pulling arch/arch.h directly gets page_count without that baggage. */
#include <arch/arch.h>

/* 1a. POSIX link() vs the decomp's `link` type.
 *     include/dolphin/types.h:64 pulls <stdio.h>, and on current KOS that is
 *     <stdio.h> -> <sys/stdio.h> -> <unistd.h> -> `int link(const char*,
 *     const char*)`. include/jaudio_NES/audiostruct.h:15 has
 *     `typedef struct link_ link;`. Whichever comes second errors with
 *     "'link' redeclared as different kind of symbol".
 *
 *     Renaming both uniformly (-Dlink=...) does NOT help — they would still
 *     collide under the new name. So rename ONLY the POSIX declaration: define
 *     the macro, pull <unistd.h> in here (first thing in every TU, so its
 *     include guard makes every later inclusion a no-op), then take the
 *     identifier back for the decomp.
 *
 *     Safe because nothing in src/ or dc/ calls POSIX link() — there is no
 *     filesystem-link concept anywhere in this game or in KOS's /cd. */
#define link __kos_posix_link
#include <unistd.h>
#undef link

/* 1b. KOS's dc/fmath.h:109 defines `static inline float fsqrt(float)` — an
 *     SH-4 `fsqrt` instruction wrapper. The decomp's include/libc64/math64.h:34
 *     does `#define fsqrt(x) sqrtf(x)` under TARGET_PC, which rewrites KOS's
 *     DEFINITION into `static inline float sqrtf(float)` and collides with
 *     newlib's non-static `float sqrtf(float)`.
 *     Including fmath.h here (before any decomp header can define the macro)
 *     means KOS's function is already declared; the decomp macro then only
 *     rewrites game-side CALL sites, which is exactly what the PC port wants.
 *     NOT covered by design-shelf-hazards §2.3 — that scan predates this
 *     toolchain image (GCC 15.2 / KOS master vs GCC 9.3 / KOS 525cbda). */
#include <dc/fmath.h>

/* 2. The one genuine libc gap on newlib (§6): glibc's C++ headers drag
 *    <string.h> in transitively, newlib's do not. JUTFont.h uses strlen and
 *    JFWDisplay.cpp uses memcpy without including it. */
#include <string.h>
#include <strings.h>   /* bcopy/bzero/index — newlib puts them here (LEGACY) */

/* 3. Macro-vs-field collisions.
 *
 *    page_count : arch/arch.h:34  `#define page_count ((16*1024*1024 - ...))`
 *                 vs include/m_notice_ovl.h:38 and include/m_address_ovl.h:42
 *                 `u8 page_count;`. Breaks m_notice_ovl.c, m_address_ovl.c,
 *                 m_editor_ovl.c, m_submenu_ovl.c. VERIFIED.
 *    direct     : sys/dir.h:8 `#define direct dirent` vs include/m_demo.h:117
 *                 `int direct;`. Latent — sys/dir.h is not currently on the
 *                 reachable path. Undef as insurance.
 *
 *    NOT undef'd: `errno`. sys/errno.h defines it as `(*__errno())` and
 *    include/libultra/osContPad.h:63 has a `u8 errno;` field, but all of
 *    libultra/ is excluded from the build and undef'ing errno would break any
 *    platform code that legitimately reads it. Revisit only if it bites.
 */
#undef page_count
#undef direct

#endif /* DC_PRELUDE_H */
