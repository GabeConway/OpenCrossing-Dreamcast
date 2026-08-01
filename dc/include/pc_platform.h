/* pc_platform.h - SHIM for the Dreamcast build.
 *
 * AUDITED 2026-08-01: verified against the tree, kept, corrections below.
 *
 * FIVE files under src/ include "pc_platform.h" directly — not six, and no
 * pc/src file that the DC build reuses (pc_assets.c, pc_save_bswap.c,
 * pc_m_card.c) includes it at all:
 *     src/game.c
 *     src/graph.c
 *     src/game/m_play.c
 *     src/game/m_actor.c
 *     src/static/libforest/emu64/emu64.c
 * The real pc/include/pc_platform.h pulls in SDL2 and glad, neither of which
 * exists on sh-elf.
 *
 * Rather than reopen those game files (design doc: keep TARGET_PC defined for
 * M1 so the link closes), the DC build puts dc/include AHEAD of pc/include on
 * the include path and this file wins. It forwards to dc_platform.h, which
 * declares every g_pc_ / pc_ prefixed symbol the game references.
 * (NB: do not write the prefixes with a trailing star here — the "star slash"
 *  sequence would terminate this comment block. That bug cost an hour.)
 *
 * COVERAGE CHECK — whole-word scan of the five files for pc_ / g_pc_ / PC_
 * identifiers, cross-checked against dc_platform.h. All satisfied:
 *     g_pc_running, g_pc_verbose, g_pc_fps_target, g_pc_frameskip_active,
 *     g_pc_widescreen_stretch, g_pc_window_w, g_pc_window_h, g_pc_model_viewer,
 *     pc_crash_protection_init, pc_crash_set_jmpbuf, pc_crash_get_addr,
 *     pc_crash_get_data_addr, pc_emu64_frame_ counters,
 *     pc_gx_draw_call_count, pc_gx_tlut_set_native_le,
 *     PC_GC_WIDTH, PC_GC_HEIGHT, PC_NOOP_WIDESCREEN_STRETCH and _OFF.
 * The rest of what those files name (g_pc_settings, pc_settings_cull_limit_xz,
 * pc_model_viewer_init, the pc_diag / pc_prof macros) comes from
 * pc/include/pc_settings.h, pc_model_viewer.h, pc_diag.h and pc_prof.h — none
 * of which touch SDL or GL, so they are reused unchanged.
 *
 * NOT forwarded, on purpose: pc_pad_get_controller() (its return type is
 * SDL_GameController) and pc_pad_dpad_as_stick_active(). Both are marked with
 * a star in the design doc's manifest, i.e. they appear nowhere in src/, and
 * dc_pad.c has a fixed maple mapping instead (PLAN §7).
 *
 * BUILD REQUIREMENT: dc/include MUST come before pc/include in the include
 * search order. If it does not, the SDL header is picked up and the build
 * fails with a missing <SDL.h>. That failure is loud, which is why this is a
 * note and not an #error.
 */
#ifndef PC_PLATFORM_H_DC_SHIM
#define PC_PLATFORM_H_DC_SHIM

/* Claim the real header's guard so a later #include "pc_platform.h" that
 * resolves to pc/include/ (e.g. from a referenced pc/src file compiled with a
 * different -I order) becomes a no-op instead of dragging in SDL. */
#ifndef PC_PLATFORM_H
#define PC_PLATFORM_H
#endif

#include "dc_platform.h"

/* Names the game files use that dc_platform.h spells differently. */
#define PC_GC_WIDTH       DC_GC_WIDTH
#define PC_GC_HEIGHT      DC_GC_HEIGHT
#define PC_SCREEN_WIDTH   DC_SCREEN_WIDTH
#define PC_SCREEN_HEIGHT  DC_SCREEN_HEIGHT
#define PC_MAIN_MEMORY_SIZE DC_MAIN_MEMORY_SIZE
#define PC_ARAM_SIZE      DC_ARAM_SIZE
#define PC_FIFO_SIZE      DC_FIFO_SIZE
#define PC_PI             DC_PI
#define PC_PIf            DC_PIf
#define PC_DEG_TO_RAD     DC_DEG_TO_RAD
#define PC_DEG_TO_RADf    DC_DEG_TO_RADf

/* There is no window on a console, but the real header defines this and a
 * stray reference should not become a build break. */
#ifndef PC_WINDOW_TITLE
#define PC_WINDOW_TITLE "Animal Crossing"
#endif

/* Defined in dc/src/dc_main.c. On DC the output mode is fixed at 640x480 with
 * no scaling layer, so it only re-derives the render size. */
void pc_platform_update_window_size(void);

/* --- POSIX bits the base port's #ifdef TARGET_PC additions expect -----------
 * src/static/libforest/emu64/emu64_utility.c:40 (compiled as part of emu64.c)
 * calls mincore() to ask the OS whether a page is mapped — the seg2k0 pointer
 * heuristic uses it to tell a real host pointer from an N64 segment address.
 * The real header for that is <sys/mman.h>, which the PC pc_platform.h pulls
 * in and sh-elf does not have.
 *
 * Dreamcast has no demand paging: main RAM is one flat, always-resident
 * 16 MB window. So "is this page committed" degenerates to "is this address
 * inside RAM", which we can answer without a syscall. Returning 0 means
 * mapped, -1 means not, matching mincore(2).
 *
 * Kept static inline so no dc/src .c file has to own it. */
#ifndef DC_MINCORE_DEFINED
#define DC_MINCORE_DEFINED
static __inline int mincore(void* addr, unsigned int length, unsigned char* vec) {
    unsigned int a = (unsigned int)addr;
    /* P1/P2 cached+uncached views of the 16 MB main RAM window. */
    int ok = ((a & 0x1FFFFFFFu) >= 0x0C000000u &&
              (a & 0x1FFFFFFFu) <  0x0C000000u + DC_TOTAL_RAM);
    (void)length;
    if (vec) *vec = (unsigned char)(ok ? 1 : 0);
    return ok ? 0 : -1;
}
#endif

#endif /* PC_PLATFORM_H_DC_SHIM */
