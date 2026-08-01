/* pc_platform.h - SHIM for the Dreamcast build.
 *
 * SIX FILES UNDER src/ INCLUDE "pc_platform.h" DIRECTLY:
 *     src/game.c, src/graph.c, src/game/m_play.c, src/game/m_actor.c,
 *     src/static/libforest/emu64/emu64.c  (+ pc/ files that get referenced)
 * and the real pc/include/pc_platform.h pulls in SDL2 and glad, neither of
 * which exists on sh-elf.
 *
 * Rather than reopen those game files (design doc: keep TARGET_PC defined for
 * M1 so the link closes), the DC build puts dc/include AHEAD of pc/include on
 * the include path and this file wins. It forwards to dc_platform.h, which
 * declares every g_pc_*/pc_* symbol the game references.
 *
 * BUILD REQUIREMENT: dc/include MUST come before pc/include in the include
 * search order. If it does not, the SDL header is picked up and the build
 * fails with a missing <SDL.h>.
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
#define PC_PI             DC_PI
#define PC_PIf            DC_PIf
#define PC_DEG_TO_RAD     DC_DEG_TO_RAD
#define PC_DEG_TO_RADf    DC_DEG_TO_RADf

#endif /* PC_PLATFORM_H_DC_SHIM */
