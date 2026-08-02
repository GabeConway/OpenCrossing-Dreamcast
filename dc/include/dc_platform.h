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
 * Kept behind DC_HOST_STUB so the individual dc/src .c files can also be
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
/* 4,000,000 was a round number nobody measured. The XFB double buffer
 * (1,228,800 B) and the GX FIFO (65,697 B) are now killed at their source on
 * DC (JUTXfb.cpp / jsyswrap.cpp, both #if TARGET_DC), so the arena is cut by
 * exactly what they used to consume: __osMalloc's usable pool is UNCHANGED at
 * ~2.6 MB while 1,294,496 B of real RAM comes back. 32-byte aligned.
 *
 * ⚠️ THIS ARENA IS NOT THE ONLY HEAP, AND GROWING IT CAN MAKE THINGS WORSE.
 * There are TWO pools and they are not independent:
 *
 *   1. this arena  — __osMalloc / zelda_malloc / JKRHeap, carved ONCE at boot
 *   2. KOS's sbrk  — plain libc malloc(), which is what graph_proc's
 *                    `malloc(dlftbl->alloc_size)` and the scene loaders use
 *
 * Both come out of the SAME region: everything between the end of .bss and
 * `_arch_mem_top` (0x8d000000 on a stock 16 MB machine). This arena is carved
 * from that region with memalign, so every byte added here is a byte libc can
 * never hand out.
 *
 * MEASURED 2026-08-02, first frame-running boot. With arena = 2,705,504 the
 * title-demo scene (scene 33) reached play_init and died with
 *
 *     Out of memory. Requested sbrk_base 8d0ee000, was 8cec5000, diff 2265088
 *
 * That message is KOS's sbrk, NOT this arena — note `8d0ee000` is past the top
 * of physical RAM. libc had 1,290,240 B of headroom left and the scene asked
 * for 2,265,088, so the shortfall is ~975 KB of LIBC heap.
 *
 * Raising the arena to 4,980,736 to "fix" it made the run get LESS far — it
 * stopped at trademark_init instead of reaching play_main — because the extra
 * 2.27 MB came straight out of libc's share. Read that as the rule: when the
 * sbrk OOM fires, the lever is to SHRINK this number, or the ARAM window, or
 * the image, and never to grow this.
 *
 * Overridable so a bring-up image can be tuned without the full build
 * inheriting the value: -DDC_MAIN_MEMORY_SIZE=<bytes>, plumbed as
 * DC_ARENA_BYTES in dc/Makefile. DC_BUDGET_JKRHEAP tracks it automatically
 * (dc_os.c static-asserts they are equal).
 *
 * Bucket 6's own high-water mark is STILL unmeasured — no arena-side OOM has
 * ever been observed, which is weak evidence that 2,705,504 is adequate, not
 * proof. kb/research-budget-premises.md §2.4 still has the real recipe. */
#ifndef DC_MAIN_MEMORY_SIZE
#define DC_MAIN_MEMORY_SIZE     2705504u
#endif
#define DC_SYSTEM_HEAP_SIZE     (DC_MAIN_MEMORY_SIZE - DC_ARENA_LOW_RESERVE)

/* ARAM is an ADDRESS SPACE, not an allocation. dc_aram.c never reserves
 * 16 MB — the sound half is gone (AICA + streaming) and the graph half is a
 * disc-backed LRU window (PLAN §3.1 / §3.4). The size is kept only so that
 * ARGetSize()/ARAlloc() hand out the same offsets the game expects. */
#define DC_ARAM_SIZE            (16u * 1024u * 1024u)

/* Graph-ARAM resident window, mem-budget bucket 8.
 *
 * MEASURED 2026-08-01, first boot with real disc content: the window was
 * anchored at ARAM offset 0 and every archive access missed it. JKRAram splits
 * the address space audio-first — jsyswrap.cpp:487 asks for 0x810000 of sound
 * ARAM and JKRAram::JKRAram allocates that BEFORE the 0x6A3780 graph half — so
 * graph offsets start at 8,454,144 and a window over [0, 512000) is entirely
 * inside jaudio's region. `forest_1st.arc` mounted, wrote 851,744 B to ARAM
 * offset 8,454,144, and all 257 writes were counted out-of-window and dropped.
 * dc_aram.c now anchors the window on first use; see the comment there.
 *
 * 1,048,576 rather than 512,000 because the boot archive alone is 851,744 B
 * and mounting it is the first thing JW_Init2 does. Still UNMEASURED as a
 * steady-state figure — probe 3 owes a real high-water mark. */
#ifndef DC_ARAM_WINDOW_SIZE
#define DC_ARAM_WINDOW_SIZE     1048576u
#endif

/* GXInit is handed a JKRHeap-allocated FIFO buffer we do not need; recorded
 * only so dc_gx.c can report how much it is giving back. */
#define DC_FIFO_SIZE            (0x10001u)

/* --- KILL SWITCH: DC_CARD_KEEP_STATIC --------------------------------------
 * kb/levers.md L3, row "pc_m_card": 308,242 B of .bss in pc/src/pc_m_card.c —
 * the sole definition of mCD_toNextLand & co. on this target, since
 * dc/Makefile's PC_REUSE_C compiles it in and excludes src/game/m_card.c.
 *
 * Four cuts, all of them layout, none of them codegen:
 *   A  l_keepMail / l_keepOriginal / l_keepDiary deleted     -147,782 B
 *      They were a pure double buffer of l_aram_block_p_table[]; the GCI read
 *      now lands in the ARAM blocks directly.
 *   B  l_keepSave: `Save` -> `Save_t` (drops dead memory-card sector padding)
 *      and moved from .bss to a zelda_malloc() out of the arena this header
 *      already reserves, for the one play scene it is live
 *                                                            -155,644 B
 *   C  l_mcd_foreigner_file: drop the sector alignment padding  -4,576 B
 *      (the passport is never written to a card file on this port)
 *   D  l_card_b_gci_path[300] -> [64]                             -236 B
 *      (the only DC value is "/vmu/aN/ANIMAL_CROSSING", 23 chars)
 *
 * DEFINE THIS to revert all four: every buffer goes back to a file-scope
 * array and pc_save_read_gci_to_keep() goes back to the double-buffer path.
 * The switch lives inside four helpers in pc_m_card.c, so the call sites are
 * identical either way and the logic is not duplicated.
 *
 * NB none of this is exercised yet: dc_vmu_write_file() (dc/src/dc_card.c) is
 * still DC_UNIMPLEMENTED, so nothing on this target saves or travels. The cut
 * is therefore free of behavioural regression today — and correspondingly
 * untested at runtime. Re-read the CUT A/B comments in pc_m_card.c when the
 * VMU write path is wired up. */
/* #define DC_CARD_KEEP_STATIC */

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

/* --- Asset working-set census (kb/STATE.md N1) ------------------------------
 * Records the asset addresses the renderer is handed and prints them for
 * tools/dcstub/census_resolve.py to turn back into symbols. This is the only
 * way to learn which acres and NPCs a scene touches: the title demo names both
 * indirectly, so no static trace can see them.
 *
 *   _note()  one address per asset. 'T' texture image (GXLoadTexObj),
 *            'P' palette (GXLoadTlut), 'V' indexed vertex array (GXSetArray —
 *            measured dead, this game never uses indexed fetch).
 *   _vtx()   one address per vertex COMPONENT, from emu64's dl_G_VTX via the
 *            dc/include/dc_census_vtx.h shim. Grouped into contiguous batches
 *            internally, because a point table would need a slot per vertex
 *            and a coalesced range table gives a wrong answer on a stub image
 *            (dc_asset_census.c explains why).
 *
 * Compiled to empty functions unless -DDC_ASSET_CENSUS=1. */
void dc_asset_census_note(const void* addr, int kind);
void dc_asset_census_vtx(const void* addr);
void dc_asset_census_report(void);

/* --- Arena high-water probe (kb/STATE.md N4) --------------------------------
 * Compiled to an empty function unless -DDC_ARENA_PROBE=<frames> (DC_ARENA_PROBE
 * in dc/Makefile). Prints touched/peak arena bytes and the current libc break,
 * so the arena's real demand can be read off one run instead of bisected over
 * one full build per data point. dc_os.c documents what the number does and
 * does not mean. */
void dc_arena_probe(void);

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
