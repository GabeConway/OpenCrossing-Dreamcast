/* dc_main.c - Dreamcast entry point, platform lifecycle, crash recovery.
 *
 * Replaces pc/src/pc_main.c (SDL2 + GL + POSIX signals). This file owns:
 *   - the real main() and the KOS init flags
 *   - the boot ordering contract (kb/design-platform-api.md §1)
 *   - the g_pc_* platform globals the 84 TARGET_PC files in src/ reference
 *   - crash protection: SH-4 exception handler + longjmp recovery, the DC
 *     analogue of the base port's SIGSEGV/SIGBUS handler
 *
 * ==========================================================================
 * BOOT ORDER — design doc §1. Do not reorder without reading it.
 * ==========================================================================
 *   1. dc_mem_ledger_init()   FIRST. Measures the image, publishes
 *                             pc_image_base/_end for the seg2k0 heuristic
 *                             (PLAN §11.6), and must precede every allocation
 *                             or the ledger under-reports.
 *   2. pc_settings_load()     Must precede display setup (it decides the
 *                             fps target, cull knobs and verbosity).
 *   3. dc_platform_init()     Video mode, then dc_gx_init(). §1 rule 4:
 *                             GXInit itself happens LATER, inside
 *                             JFWSystem::init(), so the GX state machine has
 *                             to be alive before the game's first GX call.
 *   4. disc check             /cd must be readable before assets load.
 *   5. pc_assets_init()       §1 rule 1: fills every linked .inc asset array
 *                             BEFORE any game code runs.
 *   6. ac_entry()             src/main.c's main(); sets HotStartEntry.
 *   7. boot_main(argc, argv)  src/static/boot.c's main(). Inside it:
 *                             osGetTime() runs BEFORE OSInit() (§1 rule 6) —
 *                             dc_os.c's time source is safe that early —
 *                             then OSInit() creates the arena and writes the
 *                             0x28 memory-size word (§1 rule 2), then
 *                             JFWSystem::init() -> ARInit -> GXInit -> PADInit,
 *                             then the game loop. It normally does not return.
 *   8. shutdown + the unimplemented-stub roll-up.
 *
 * The main loop itself is NOT here. Frame pacing, present and input polling
 * all hang off VIWaitForRetrace() in dc_vi.c, which the game calls; boot.c's
 * sound_initial2() spins on it before the game loop even starts (§1 rule 5).
 */
#include "dc_platform.h"
#include "dc_mem_ledger.h"
#include "pc_settings.h"   /* pc_settings_load/apply — see dc_misc.c */

/* The game's main() is renamed with -Dmain=ac_entry / -Dmain=boot_main. If
 * that define ever leaks onto this TU we would silently lose the real entry
 * point and boot to a black screen. */
#ifdef main
#error "dc_main.c must be compiled WITHOUT -Dmain=... (it owns the real main)"
#endif

#ifndef DC_HOST_STUB
#include <kos/init.h>

/* INIT_DEFAULT = IRQ + preemptive threads + all virtual filesystems +
 * INIT_DEFAULT_ARCH (INIT_MAPLE_ALL | INIT_CDROM). That gives us, for free:
 * the maple controller driver (dc_pad.c), the VMU + vmufs (dc_card.c) and the
 * auto-mounted iso9660 /cd (dc_dvd.c). Nothing else is wanted:
 *   - INIT_NET      would cost RAM we do not have (16 MB, CLAUDE.md).
 *   - INIT_QUIET    would kill dbgio, which is our only console on hardware.
 *   - INIT_MALLOCSTATS is tempting for the memory ledger but adds per-malloc
 *     bookkeeping in a build whose whole point is measuring the 16 MB budget
 *     honestly; dc_mem_ledger.c wraps malloc instead. */
KOS_INIT_FLAGS(INIT_DEFAULT);
#endif

/* ==========================================================================
 * Platform globals
 * ==========================================================================
 * These keep their pc_ / g_pc_ names deliberately. The DC build keeps
 * TARGET_PC defined (design doc, "The platform surface is NOT only pc/"), so
 * 84 conditionally-compiled files under src/ reference these exact spellings.
 * Renaming any of them re-opens every one of those files.
 */
int   g_pc_running          = 1;
/* Every OSReport in the game is gated on this (dc_os.c), so with it off a
 * bring-up run is blind: the game prints its own progress and its own failure
 * reasons through OSReport and nothing else. A burned CD-R passes no argv, so
 * --verbose is unreachable there; DC_VERBOSE=1 is the compile-time way in, and
 * the stub build turns it on by default because seeing why the game stops is
 * the entire purpose of that build. */
#if defined(DC_VERBOSE) || defined(DC_ASSET_STUB)
int   g_pc_verbose          = 1;
#else
int   g_pc_verbose          = 0;   /* dbgio at 57600 baud destroys frame time */
#endif
int   g_pc_no_framelimit    = 0;
int   g_pc_time_override    = -1;  /* -1 = use the battery-backed RTC       */
int   g_pc_min_override     = -1;
int   g_pc_sec_override     = -1;
int   g_pc_window_w         = DC_SCREEN_WIDTH;
int   g_pc_window_h         = DC_SCREEN_HEIGHT;
int   g_pc_render_w         = DC_SCREEN_WIDTH;
int   g_pc_render_h         = DC_SCREEN_HEIGHT;
int   g_pc_widescreen_stretch = 0;
int   g_pc_frameskip_active = 0;
int   g_pc_fps_target       = 30;  /* PLAN §1: 30 is the DESIGN TARGET      */
int   g_pc_scale_mode       = 0;
int   g_pc_menu_open        = 0;
float g_pc_zoom             = 1.0f;

/* Dropped dev tool; the flags exist because graph.c/m_game_dlftbls.c read
 * them unconditionally. dc_stubs.c stubs the functions. */
int   g_pc_model_viewer         = 0;
int   g_pc_model_viewer_start   = 0;
int   g_pc_model_viewer_no_cull = 0;

/* Image range for the seg2k0 pointer heuristic: emu64 uses it to tell a real
 * pointer from an N64 segment address. Filled by dc_mem_ledger_init() from the
 * linker symbols (exact on DC, unlike the PC port's dladdr()). */
unsigned int pc_image_base = 0;
unsigned int pc_image_end  = 0;

/* ==========================================================================
 * Crash protection
 * ==========================================================================
 * The base port catches SIGSEGV/SIGBUS and longjmps back to a per-frame
 * recovery point; emu64's display-list interpreter depends on it, because a
 * malformed DL in the vendored data is a routine event, not an emergency
 * (graph.c sets the recovery point, emu64 counts pc_emu64_frame_crashes).
 *
 * On SH-4 the equivalent seam is a KOS exception handler. The recovery trick:
 * an irq_hdl_t receives the faulting thread's saved irq_context_t, and KOS
 * restores that context on return. Rewriting CONTEXT_PC() therefore resumes
 * the thread at a trampoline of our choosing instead of re-executing the
 * faulting instruction. The trampoline runs in ordinary thread context (the
 * exception return has already restored SR/SP), so calling longjmp() from it
 * is a normal call, NOT a longjmp-out-of-a-signal-handler.
 *
 * PLAN §3.2 also wants this for -O2 triage: SH-4 raises a real exception on
 * unaligned access, where ARM raised SIGBUS on the base port. The address is
 * in TEA and the faulting instruction in context->pc, which is exactly the
 * per-TU triage signal the Anbernic repo used.
 *
 * KILL SWITCH: -DDC_NO_CRASH_PROTECTION reverts to "let KOS panic", which is
 * the right setting if the redirect below is ever suspected of masking a real
 * bug or of looping.
 */
#ifndef DC_NO_CRASH_PROTECTION

static jmp_buf* s_crash_jmpbuf = NULL;
static volatile unsigned int s_crash_addr = 0;       /* faulting instruction */
static volatile unsigned int s_crash_data_addr = 0;  /* faulting data address */
static int s_crash_installed = 0;
static int s_crash_count = 0;

#ifndef DC_HOST_STUB
/* SH-4 TEA (TLB Exception Address). Holds the effective address of the access
 * that faulted — the direct analogue of siginfo_t::si_addr. Not exposed by
 * KOS, so read the control register. */
#define DC_SH4_TEA (*(volatile uint32_t*)0xFF00000CU)

static volatile unsigned int s_crash_code = 0;

/* Resumed-into by the handler below via the rewritten context PC. Never
 * returns. Runs on the faulting thread's own stack in ORDINARY thread context,
 * which is why all the logging lives here and not in the handler: newlib's
 * printf is not reentrant and the handler runs on the interrupt path. */
static void dc_crash_resume(void) {
    jmp_buf* buf = s_crash_jmpbuf;
    s_crash_jmpbuf = NULL;          /* one-shot, exactly like the PC handler */

    /* First few only: on hardware this goes out over dbgio at 57600 baud and
     * a fault storm would be slower than the faults it is reporting. */
    if (s_crash_count <= 8) {
        DC_LOGE("[DC/EXC] recovered code=0x%04X pc=0x%08X tea=0x%08X (#%d)\n",
                s_crash_code, s_crash_addr, s_crash_data_addr, s_crash_count);
    }

    if (buf) longjmp(*buf, 1);

    /* Unreachable: the handler only redirects here when a jmpbuf was set. */
    DC_LOGE("[DC/EXC] recovery trampoline with no jmp_buf — halting\n");
    dc_unimpl_dump();
    arch_exit();
}

/* Reached when there is no recovery point. Cannot just return from the
 * handler: every exception hooked below is [REEXEC], so returning re-runs the
 * faulting instruction and spins forever. */
static void dc_crash_fatal(void) {
    DC_LOGE("[DC/EXC] FATAL code=0x%04X pc=0x%08X tea=0x%08X "
            "(no recovery point)\n",
            s_crash_code, s_crash_addr, s_crash_data_addr);
    dc_unimpl_dump();
    dc_mem_report(1);
    arch_exit();
}

/* Interrupt context: record and redirect only. No printf, no allocation. */
static void dc_crash_handler(irq_t code, irq_context_t* ctx, void* data) {
    (void)data;

    s_crash_code      = (unsigned int)code;
    s_crash_addr      = (unsigned int)CONTEXT_PC(*ctx);
    s_crash_data_addr = (unsigned int)DC_SH4_TEA;
    s_crash_count++;

    CONTEXT_PC(*ctx) = s_crash_jmpbuf
        ? (uint32_t)(uintptr_t)&dc_crash_resume
        : (uint32_t)(uintptr_t)&dc_crash_fatal;
}
#endif /* !DC_HOST_STUB */

void pc_crash_protection_init(void) {
    if (s_crash_installed) return;
    s_crash_installed = 1;

#ifndef DC_HOST_STUB
    /* SH-4 general exceptions worth catching. Note EXC_INSTR_ADDRESS and
     * EXC_DATA_ADDRESS_READ share code 0x0E0 — one registration covers both.
     * Unaligned loads/stores land in the two DATA_ADDRESS codes, which is the
     * -O2 alignment class PLAN §3.2 cares about. */
    irq_set_handler(EXC_DATA_ADDRESS_READ,  dc_crash_handler, NULL);
    irq_set_handler(EXC_DATA_ADDRESS_WRITE, dc_crash_handler, NULL);
    irq_set_handler(EXC_ILLEGAL_INSTR,      dc_crash_handler, NULL);
    irq_set_handler(EXC_SLOT_ILLEGAL_INSTR, dc_crash_handler, NULL);
    DC_LOGE("[DC/EXC] SH-4 exception recovery armed\n");
#endif
}

void pc_crash_set_jmpbuf(jmp_buf* buf) { s_crash_jmpbuf = buf; }
jmp_buf* pc_crash_get_jmpbuf(void)     { return s_crash_jmpbuf; }
unsigned int pc_crash_get_addr(void)      { return s_crash_addr; }
unsigned int pc_crash_get_data_addr(void) { return s_crash_data_addr; }

#else /* DC_NO_CRASH_PROTECTION */

void pc_crash_protection_init(void) {
    DC_LOGE("[DC/EXC] crash protection DISABLED at compile time\n");
}
void pc_crash_set_jmpbuf(jmp_buf* buf) { (void)buf; }
jmp_buf* pc_crash_get_jmpbuf(void)     { return NULL; }
unsigned int pc_crash_get_addr(void)      { return 0; }
unsigned int pc_crash_get_data_addr(void) { return 0; }

#endif /* DC_NO_CRASH_PROTECTION */

/* ==========================================================================
 * Input / event pump
 * ==========================================================================
 * There are no window events on a console. What remains of pc_platform_poll_events
 * is (a) the quit path and (b) the harness frame limit.
 *
 * The quit path is the Dreamcast convention: A+B+X+Y+Start aborts to the BIOS.
 * KOS does NOT install that by default, so we register it. The callback fires
 * from the maple IRQ, hence flag-only — the game leaves through
 * boot.c/OSResetSystem exactly as it would on GameCube.
 */
#ifndef DC_HOST_STUB
static void dc_abort_combo(uint8_t addr, uint32_t btns) {
    (void)addr; (void)btns;
    g_pc_running = 0;
}
#endif

/* Harness hook: -DDC_BOOT_FRAME_LIMIT=N leaves the loop after N presented
 * frames so harness/dc/smoke.sh can boot a CDI, capture the console and
 * assert without a human. 0 (default) = run forever, which is what ships. */
#ifndef DC_BOOT_FRAME_LIMIT
#define DC_BOOT_FRAME_LIMIT 0
#endif

int dc_platform_poll_events(void) {
#if DC_BOOT_FRAME_LIMIT > 0
    if (pc_frame_counter >= (u32)DC_BOOT_FRAME_LIMIT) {
        DC_LOGE("[DC] frame limit %d reached, leaving the loop\n",
                DC_BOOT_FRAME_LIMIT);
        g_pc_running = 0;
        return 0;
    }
#endif
    return g_pc_running;
}

/* ==========================================================================
 * Present
 * ==========================================================================
 * On PC this was SDL_GL_SwapWindow. On the Dreamcast the PVR presents as a
 * side effect of pvr_scene_finish(), which dc_gx.c's backend owns and calls
 * from dc_gx_end_frame() — i.e. one statement before dc_vi.c calls us. There
 * is deliberately NOTHING to do here.
 *
 * Do not add vid_waitvbl() here: dc_vi.c's dc_pace_frame() already sleeps out
 * whole vblanks to hit the fps target, and doing it twice would halve the
 * frame rate in the exact place that is hardest to notice.
 */
void dc_platform_swap_buffers(void) {
}

/* Kept because pc_platform.h declares it and the ✱ pc-internal callers are
 * cheap to satisfy. The DC output mode is fixed at 640x480; there is no
 * window and no scaling layer, so this only re-derives the render size. */
void pc_platform_update_window_size(void) {
    g_pc_window_w = DC_SCREEN_WIDTH;
    g_pc_window_h = DC_SCREEN_HEIGHT;
    g_pc_render_w = DC_SCREEN_WIDTH;
    g_pc_render_h = DC_SCREEN_HEIGHT;
    g_pc_scale_mode = 0;
}

/* ==========================================================================
 * Lifecycle
 * ========================================================================== */
void dc_platform_init(void) {
    DC_LOGE("[DC] OpenCrossing-Dreamcast  build %s %s\n", __DATE__, __TIME__);
    DC_LOGE("[DC] image 0x%08X-0x%08X  target %dx%d @ %d fps\n",
            pc_image_base, pc_image_end,
            DC_SCREEN_WIDTH, DC_SCREEN_HEIGHT, g_pc_fps_target);

#ifndef DC_HOST_STUB
    /* 640x480 NTSC, RGB565. The game renders in GameCube coordinates and the
     * DC output mode is the same 640x480, so unlike the handheld port there is
     * no scaling layer at all.
     *
     * TODO (M2): the PVR itself is NOT initialised here. pvr_init_defaults()
     * belongs to the renderer, and dc_gx.c's backend seam
     * (dc_gx_backend_init/frame_begin/frame_end) is where it lands so that the
     * PVR lifetime and the scene lifetime stay in one file. Until then this is
     * a plain framebuffer mode and nothing is drawn — which is exactly what
     * the M1 gate measures (boot + console, no pixels). */
    vid_set_mode(DM_640x480, PM_RGB565);

    cont_btn_callback(0,
                      CONT_A | CONT_B | CONT_X | CONT_Y | CONT_START,
                      dc_abort_combo);
#endif

    pc_platform_update_window_size();

    /* §1 rule 4: the GX state machine must exist before the game's first GX
     * call. GXInit() itself arrives much later, from inside JFWSystem::init(),
     * and on this port it is nearly a no-op (there is no command FIFO). */
    dc_gx_init();
}

void pc_platform_shutdown(void) {
    extern void PADCleanup(void);

    pc_audio_shutdown();
    pc_audio_mq_shutdown();
    PADCleanup();
    dc_gx_shutdown();

    DC_LOGE("[DC] platform shutdown\n");
}

/* ==========================================================================
 * Entry point
 * ==========================================================================
 * The game's own entry points, renamed by the build:
 *   src/main.c        main() -> ac_entry   (sets HotStartEntry = &entry)
 *   src/static/boot.c main() -> boot_main  (full init, then the game loop)
 */
extern void ac_entry(void);
extern int  boot_main(int argc, const char** argv);
extern void pc_assets_init(void);

/* Cheap sanity check that the disc actually carries the extracted payload.
 * NOT fatal: the M1 gate wants a boot log even from a half-built disc, and
 * pc_disc_extract_dol()/_rel() already shout by name when a file is missing.
 * This exists so the FIRST line of the log says whether /cd is even there,
 * instead of that showing up 30 seconds later as an asset failure. */
static void dc_check_disc(void) {
#ifndef DC_HOST_STUB
    file_t d = fs_open("/cd", O_RDONLY | O_DIR);
    if (d == FILEHND_INVALID) {
        DC_LOGE("[DC] /cd NOT MOUNTED — no disc, or KOS's iso9660 driver did "
                "not attach. Every asset load will fail.\n");
        return;
    }
    fs_close(d);
    DC_LOGE("[DC] /cd mounted\n");
#endif
}

int main(int argc, char* argv[]) {
    /* 1. The ledger goes first: it measures .text/.data/.bss from the linker
     *    symbols and publishes pc_image_base/_end, which emu64's seg2k0
     *    pointer heuristic reads. Any allocation before this is invisible to
     *    the 16 MB budget. */
    dc_mem_ledger_init();

    /* 2. Settings before display: they carry the fps target, the cull knobs
     *    and the verbosity flag. There is no ini file on a read-only disc, so
     *    this is compile-time defaults for now (dc_misc.c). */
    pc_settings_load();
    pc_settings_apply();

    /* Command line, when there is one. dcload passes argv; a burned CD-R does
     * not, so every one of these has a working default. */
    {
        int i;
        for (i = 1; i < argc; i++) {
            if (strcmp(argv[i], "--verbose") == 0 || strcmp(argv[i], "-v") == 0) {
                g_pc_verbose = 1;
            } else if (strcmp(argv[i], "--no-framelimit") == 0) {
                g_pc_no_framelimit = 1;
            } else if (strcmp(argv[i], "--time") == 0 && i + 1 < argc) {
                int h = -1, m = -1, s = -1;
                sscanf(argv[i + 1], "%d:%d:%d", &h, &m, &s);
                if (h >= 0 && h <= 23) g_pc_time_override = h;
                if (m >= 0 && m <= 59) g_pc_min_override = m;
                if (s >= 0 && s <= 59) g_pc_sec_override = s;
                i++;
            }
        }
    }

    /* 3. Display + GX state machine. */
    dc_platform_init();

    /* 4. Disc. */
    dc_check_disc();

    /* 5. §1 rule 1: every linked-in .inc asset array is EMPTY until this
     *    returns, and game code must not run before it does. */
#ifdef DC_ASSET_STUB
    /* S1 bring-up build: the destination arrays are one element each, so the
     * central table in pc_assets.c would memcpy megabytes over them. Skipping
     * the load is the whole point — this image exists to prove the trampoline,
     * KOS init, the console and the ledger run, not to render anything. */
    DC_LOGE("[DC] DC_ASSET_STUB: skipping pc_assets_init() — assets are [1]\n");
#else
    pc_assets_init();
#endif

    /* 6/7. Hand over to the game. boot_main() runs OSInit() (which builds the
     *      arena and writes the 0x28 memory-size word — §1 rule 2),
     *      JFWSystem::init() (ARAM, GXInit, PADInit — rules 3 and 4), the
     *      2.5 s sound_initial() wait, then sound_initial2()'s
     *      VIWaitForRetrace() loop (rule 5) and finally the game loop. It
     *      normally does not return. */
    DC_LOGE("[DC] entering the game\n");
    ac_entry();
    boot_main(argc, (const char**)argv);

    /* 8. Only reached via OSResetSystem/osShutdownStart or the abort combo. */
    pc_platform_shutdown();
    dc_unimpl_dump();
    dc_mem_report(1);
    DC_LOGE("[DC] exit\n");
    return 0;
}
