/* dc_platform.h - Dreamcast/KallistiOS platform layer: types, memory map,
 * global state, logging. The DC analogue of pc/include/pc_platform.h.
 *
 * Read kb/design-platform-api.md before touching anything in here. The
 * numbered references below (§3.1 etc.) are sections of that document.
 *
 * HARDWARE CONTRACT (CLAUDE.md): stock Dreamcast, 16 MB main RAM, 8 MB VRAM,
 * 2 MB AICA sound RAM, SH-4 @ 200 MHz single core, PowerVR CLX2 with no
 * shaders and no hardware T&L. Nothing in dc/ may assume the 32 MB RAM mod.
 */
#ifndef DC_PLATFORM_H
#define DC_PLATFORM_H

/* --- 32-bit assertion (same contract as pc_platform.h) ---------------------
 * The decomp (JSystem, emu64, seg2k0 pointer heuristics) casts pointers to u32
 * everywhere. SH-4 is ILP32 little-endian, so this holds — but assert it so a
 * host-side build of these files fails loudly instead of corrupting silently. */
#include <stdint.h>
#if UINTPTR_MAX != 0xFFFFFFFFu
#error "OpenCrossing-Dreamcast must be compiled 32-bit (sh-elf); pointer size != 4"
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <stddef.h>
#include <math.h>
#include <setjmp.h>

/* --- KallistiOS ------------------------------------------------------------
 * Kept behind DC_HOST_STUB so the individual dc/src/*.c files can also be
 * syntax-checked on the host without a sh-elf toolchain. The real build never
 * defines DC_HOST_STUB. */
#ifndef DC_HOST_STUB
#include <kos.h>          /* pulls arch/arch.h, kos/thread.h, dc/maple.h, ... */
#include <arch/arch.h>    /* arch_exit, arch_reboot, _arch_mem_top           */
#include <arch/cache.h>   /* dcache_flush_range, dcache_inval_range, ...     */
#include <arch/timer.h>   /* timer_ms_gettime64, timer_us_gettime64          */
#include <arch/irq.h>     /* irq_disable, irq_restore                        */
#include <arch/rtc.h>     /* rtc_unix_secs                                   */
#include <kos/fs.h>       /* fs_open/fs_read/fs_seek on /cd                  */
#include <kos/thread.h>   /* thd_sleep, thd_create                           */
#include <dc/video.h>     /* vid_set_mode, vid_waitvbl                       */
#include <dc/pvr.h>       /* pvr_init, pvr_scene_begin/finish                */
#include <dc/maple.h>
#include <dc/maple/controller.h>
#endif

/* --- Types -----------------------------------------------------------------
 * Byte-for-byte the same set pc/include/pc_types.h defines, and using the SAME
 * include guard, so a TU that pulls in both headers (e.g. a referenced pc/ file)
 * sees exactly one definition. Do not "improve" these to stdint types: the
 * decomp relies on u32 == unsigned long for some format strings. */
#ifndef PC_TYPES_H
#define PC_TYPES_H
typedef unsigned char      u8;
typedef unsigned short     u16;
typedef unsigned long      u32;
typedef unsigned long long u64;
typedef signed char        s8;
typedef signed short       s16;
typedef signed long        s32;
typedef signed long long   s64;
typedef float              f32;
typedef double             f64;
typedef int                BOOL;
#ifndef TRUE
#define TRUE 1
#endif
#ifndef FALSE
#define FALSE 0
#endif
#endif /* PC_TYPES_H */

#ifdef __cplusplus
extern "C" {
#endif

/* --- Display ---------------------------------------------------------------
 * The game renders in GameCube coordinates (640x480); the DC output mode is
 * 640x480 too, so unlike the handheld port there is no scaling layer. */
#define DC_GC_WIDTH       640
#define DC_GC_HEIGHT      480
#define DC_SCREEN_WIDTH   DC_GC_WIDTH
#define DC_SCREEN_HEIGHT  DC_GC_HEIGHT

/* --- Memory map ------------------------------------------------------------
 * kb/mem-budget.md §4 is the authority. The numbers here MUST agree with
 * dc/include/dc_mem_budget.h; dc_mem_ledger.c static-asserts the total.
 *
 * The arena (§3.1 option A, the recommended one): OSPhysicalToCached(p) maps to
 * dc_arena_base + p, exactly as the proven PC port does, so JKRHeap's
 *      mMemorySize = *(u32*)(OSPhysicalToCached(0) + 0x28)
 * lands inside our own block and never touches KOS low memory at 0x8C000000.
 * The first DC_ARENA_LOW_RESERVE bytes are the fake GameCube boot-info block. */
#define DC_TOTAL_RAM            (16u * 1024u * 1024u)   /* stock DC, never more */
#define DC_ARENA_LOW_RESERVE    0x3100u                 /* mirrors pc_os.c arena_lo offset */
/* The WHOLE arena block. Must equal DC_BUDGET_JKRHEAP (dc_os.c static-asserts
 * it) because JW_Init computes SystemHeapSize = arenaHi - arenaLo - 0xD0: the
 * arena size IS the system heap size, there is no separate knob (§3.1). */
#define DC_MAIN_MEMORY_SIZE     4000000u
#define DC_SYSTEM_HEAP_SIZE     (DC_MAIN_MEMORY_SIZE - DC_ARENA_LOW_RESERVE)

/* ARAM is an ADDRESS SPACE, not an allocation. dc_aram.c never reserves
 * 16 MB — the sound half is gone (AICA + streaming) and the graph half is a
 * disc-backed LRU window (PLAN §3.1 / §3.4). The size is kept only so that
 * ARGetSize()/ARAlloc() hand out the same offsets the game expects. */
#define DC_ARAM_SIZE            (16u * 1024u * 1024u)

/* Graph-ARAM resident window, mem-budget bucket 8. UNMEASURED — probe 3. */
#define DC_ARAM_WINDOW_SIZE     512000u

/* GXInit is handed a JKRHeap-allocated FIFO buffer we do not need; recorded
 * only so dc_gx.c can report how much it is giving back. */
#define DC_FIFO_SIZE            (0x10001u)

#define DC_PI  3.14159265358979323846
#define DC_PIf 3.14159265358979323846f
#define DC_DEG_TO_RAD  (DC_PI / 180.0)
#define DC_DEG_TO_RADf (DC_PIf / 180.0f)

/* --- GameCube hardware clocks ----------------------------------------------
 * §3.3: DO NOT CHANGE THESE. Every timeout, animation delta and RTC
 * computation in the game is denominated in 40.5 MHz GC ticks. Only the
 * SOURCE of the ticks changes on Dreamcast, never the rate. */
#define GC_BUS_CLOCK          162000000u
#define GC_CORE_CLOCK         486000000u
#define GC_TIMER_CLOCK        (GC_BUS_CLOCK / 4)     /* 40,500,000 Hz */
#define GC_UNIX_EPOCH_DIFF    946684800LL            /* 2000-01-01 in Unix seconds */

/* --- Logging ---------------------------------------------------------------
 * DC_LOG   : gated on g_pc_verbose. On hardware this goes out over dbgio
 *            (dcload/serial) at 57600 baud — leave OFF by default or frame
 *            time dies (§5, OSReport row).
 * DC_LOGE  : always printed. Boot errors, budget overruns, fatals.
 * DC_UNIMPLEMENTED : the point of this pass. Every stub announces itself ONCE
 *            and then continues, so the first boot attempt produces a to-do
 *            list instead of a black screen. */
extern int g_pc_verbose;

void dc_log_impl(const char* fmt, ...);
void dc_loge_impl(const char* fmt, ...);
void dc_unimpl_report(const char* fn, const char* file, int line);
int  dc_unimpl_count(void);
void dc_unimpl_dump(void);

#define DC_LOG(...)  do { if (g_pc_verbose) dc_log_impl(__VA_ARGS__); } while (0)
#define DC_LOGE(...) do { dc_loge_impl(__VA_ARGS__); } while (0)

#define DC_UNIMPLEMENTED()                                                    \
    do {                                                                      \
        static unsigned char dc_unimpl_seen_ = 0;                             \
        if (!dc_unimpl_seen_) {                                               \
            dc_unimpl_seen_ = 1;                                              \
            dc_unimpl_report(__func__, __FILE__, __LINE__);                   \
        }                                                                     \
    } while (0)

/* Same, but with a note explaining what the real implementation must do. */
#define DC_UNIMPLEMENTED_NOTE(note)                                           \
    do {                                                                      \
        static unsigned char dc_unimpl_seen_ = 0;                             \
        if (!dc_unimpl_seen_) {                                               \
            dc_unimpl_seen_ = 1;                                              \
            dc_unimpl_report(__func__, __FILE__, __LINE__);                   \
            dc_loge_impl("[DC/TODO]     %s\n", (note));                       \
        }                                                                     \
    } while (0)

/* --- Global state ----------------------------------------------------------
 * These keep their pc_ / g_pc_ names on purpose: the DC build keeps TARGET_PC
 * defined (design doc §"The platform surface is NOT only pc/"), so the 84
 * conditionally-compiled files under src/ reference these exact spellings.
 * Renaming them would re-open every one of those files. */
extern int g_pc_running;
extern int g_pc_no_framelimit;
extern int g_pc_time_override;
extern int g_pc_min_override;
extern int g_pc_sec_override;
extern int g_pc_window_w;
extern int g_pc_window_h;
extern int g_pc_widescreen_stretch;
extern int g_pc_frameskip_active;
extern int g_pc_fps_target;
extern int g_pc_render_w;
extern int g_pc_render_h;
extern int g_pc_scale_mode;
extern int g_pc_menu_open;
extern int g_pc_model_viewer;
extern int g_pc_model_viewer_start;
extern int g_pc_model_viewer_no_cull;
extern float g_pc_zoom;

#define PC_ZOOM_MIN  0.5f
#define PC_ZOOM_MAX  2.0f
#define PC_ZOOM_STEP 0.02f

/* Widescreen NOOP markers emu64 reads out of POLY_OPA (see pc_platform.h). */
#define PC_NOOP_WIDESCREEN_STRETCH     0xAC5701u
#define PC_NOOP_WIDESCREEN_STRETCH_OFF 0xAC5700u

/* Arena bounds, exported for the seg2k0 pointer heuristic (PLAN §11.6). */
extern u8* pc_arena_base;
extern u8* pc_arena_end;

/* Image range, same purpose: distinguish real pointers from N64 segment
 * addresses. On DC these come from the linker symbols, not dladdr. */
extern unsigned int pc_image_base;
extern unsigned int pc_image_end;

/* --- Platform lifecycle ---------------------------------------------------- */
void dc_platform_init(void);
void pc_platform_shutdown(void);
void dc_platform_swap_buffers(void);
int  dc_platform_poll_events(void);

/* --- Crash protection ------------------------------------------------------
 * The PC port catches SIGSEGV/SIGBUS and longjmps back to a per-frame recovery
 * point; emu64 display-list processing depends on it. On SH-4 the equivalent
 * seam is a KOS exception handler (see dc_os.c __OSSetExceptionHandler).
 * PLAN §3.2 wants this for -O2 alignment-fault triage. */
void     pc_crash_protection_init(void);
void     pc_crash_set_jmpbuf(jmp_buf* buf);
jmp_buf* pc_crash_get_jmpbuf(void);
unsigned int pc_crash_get_addr(void);
unsigned int pc_crash_get_data_addr(void);

/* --- Per-frame diagnostics (read by game code and dc_vi.c) ----------------- */
extern int pc_emu64_frame_cmds;
extern int pc_emu64_frame_crashes;
extern int pc_emu64_frame_noop_cmds;
extern int pc_emu64_frame_tri_cmds;
extern int pc_emu64_frame_vtx_cmds;
extern int pc_emu64_frame_dl_cmds;
extern int pc_emu64_frame_cull_visible;
extern int pc_emu64_frame_cull_rejected;
extern int pc_gx_draw_call_count;
extern int pc_gx_prim_draws[5];
extern int pc_gx_merged_batches;
extern int pc_gx_culled_draws;
extern int pc_gx_flush_reason[18];
extern u32 pc_frame_counter;

/* --- Audio ---------------------------------------------------------------- */
extern int pc_save_loaded;
int  pc_audio_get_buffer_fill(void);
int  pc_audio_is_active(void);
void pc_audio_shutdown(void);
void pc_audio_start_producer_thread(void);
void pc_audio_mq_init(void);
void pc_audio_mq_shutdown(void);
void pc_audio_update_volumes(void);

/* --- GX entry points the rest of the platform layer calls ------------------ */
void dc_gx_init(void);
void dc_gx_shutdown(void);
void pc_gx_begin_frame(void);       /* game code calls this by its pc_ name */
void dc_gx_end_frame(void);
void dc_gx_frame_timing_snapshot(void);
void pc_gx_tlut_set_native_le(unsigned int idx);

extern u64 dc_gx_flush_time_us;
extern u64 dc_gx_texload_time_us;

/* --- Timing helper shared by dc_os.c / dc_vi.c / dc_mem_ledger.c ----------- */
u64 dc_time_us(void);   /* free-running microseconds, safe before OSInit() */

#ifdef __cplusplus
}
#endif

#endif /* DC_PLATFORM_H */
