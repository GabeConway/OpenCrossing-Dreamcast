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
#if !defined(DC_HOST_STUB) && !defined(DC_NO_SPLASH)
#include <dc/biosfont.h>
#include <dc/video.h>
#include "dc_pvr.h"        /* dc_pvr_fb_dump_surface — the splash screenshot */

/* The author's splash, shown between the Sega licence screen and the game.
 *
 * WHERE IT GOES, and why it is here and not in the renderer: at this point in
 * dc_platform_init() the PVR has NOT been initialised — pvr_init_defaults()
 * belongs to dc_gx.c's backend seam and runs much later — so vid_set_mode()
 * has just given us a plain linear RGB565 framebuffer at vram_s. Drawing is
 * therefore a memset and a bfont call, with no scene, no lists and no texture
 * cache to disturb. Doing the same thing after the PVR is up would mean
 * building a textured quad and a font atlas for two lines of text.
 *
 * Cost: zero RAM (bfont lives in the Dreamcast BIOS, not in our image) and
 * DC_SPLASH_MS of boot time, skippable with any button.
 *
 * ⚠️ It must run BEFORE dc_gx_init(). The splash owns the framebuffer only
 * until the PVR takes it over, and pvr_init() reprograms the display
 * controller at its own buffers (kb/traps.md: "vram_s is not the displayed
 * surface once pvr_init() has run").
 *
 * Kill switch: -DDC_NO_SPLASH. Duration: -DDC_SPLASH_MS=<n>. */
#ifndef DC_SPLASH_MS
#define DC_SPLASH_MS 2000
#endif

#define DC_SPLASH_TEXT "TechProGabe Presents..."

/* ==========================================================================
 * The splash, drawn straight into the raw framebuffer
 * ==========================================================================
 * Everything below is a CPU write loop over a linear RGB565 640x480 surface.
 * That is the whole reason it is safe: no PVR, no lists, no texture cache, no
 * assets, no VRAM allocation, and nothing that has to be undone before
 * pvr_init() takes the display. The only persistent memory it costs is the
 * 840-byte glyph mask below.
 *
 * Three pieces, each independently removable:
 *   1. the text at 2x    (-DDC_SPLASH_NO_SCALE)  bfont is 12x24 thin, which
 *      reads as a debug print; doubled it reads as a title card.
 *   2. a progress bar    (-DDC_SPLASH_NO_BAR)    driven by the REAL keep-list
 *      load, which is the 15 s window in which a human cannot tell "loading"
 *      from "hung" (kb/traps.md).
 * Plus a vertical gradient instead of flat black, and a fade-in.
 *
 * A shine sweep across the letters was built and then turned OFF at the
 * author's request — "remove the shimmer from the text, but everything else is
 * good". It is kept behind -DDC_SPLASH_SHINE rather than deleted, because the
 * per-pixel recolour it does is also what animates the band during the load,
 * and someone may want that back.
 *
 * ⚠️ There is no back buffer here. vram_s IS the scanout surface, so every
 * write is immediately visible and the redraw is band-limited (552x48 plus the
 * bar) rather than a full-screen clear, or the text would flicker. */
#define SPL_CHARS   ((int)(sizeof(DC_SPLASH_TEXT) - 1))
#define SPL_SRC_W   (SPL_CHARS * BFONT_THIN_WIDTH)      /* 276 */
#define SPL_SRC_H   BFONT_HEIGHT                        /* 24  */
#ifndef DC_SPLASH_NO_SCALE
#define SPL_ZOOM    2
#else
#define SPL_ZOOM    1
#endif
#define SPL_W       (SPL_SRC_W * SPL_ZOOM)
#define SPL_H       (SPL_SRC_H * SPL_ZOOM)
#define SPL_X       ((DC_SCREEN_WIDTH  - SPL_W) / 2)
#define SPL_Y       ((DC_SCREEN_HEIGHT - SPL_H) / 2 - 16)
#define SPL_BAR_X   120
#define SPL_BAR_W   400
#define SPL_BAR_H   8
#define SPL_BAR_Y   (SPL_Y + SPL_H + 28)
#define SPL_MASK_STRIDE ((SPL_SRC_W + 7) / 8)            /* 35 bytes */

/* 1 bit per source pixel: 35 x 24 = 840 B. A byte-per-pixel mask would be
 * 6,624 B, and this file is compiled into an image that is already over its
 * RAM budget. */
static unsigned char s_spl_mask[SPL_MASK_STRIDE * SPL_SRC_H];
static int           s_spl_ready = 0;

static inline unsigned short spl_rgb(int r, int g, int b) {
    if (r < 0) r = 0;
    if (r > 255) r = 255;
    if (g < 0) g = 0;
    if (g > 255) g = 255;
    if (b < 0) b = 0;
    if (b > 255) b = 255;
    return (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

/* Vertical gradient, evaluated per row. Dark navy at the top fading to near
 * black at the bottom — enough to stop the screen reading as "off" without
 * competing with the text. */
static inline unsigned short spl_bg(int y) {
    int t = (y * 255) / (DC_SCREEN_HEIGHT - 1);
    return spl_rgb(6 + ((14 - 6) * (255 - t)) / 255,
                   10 + ((22 - 10) * (255 - t)) / 255,
                   28 + ((52 - 28) * (255 - t)) / 255);
}

static void spl_fill_rows(int y0, int y1) {
    int y, x;
    for (y = y0; y < y1; y++) {
        unsigned short c = spl_bg(y);
        unsigned short* row = vram_s + (size_t)y * DC_SCREEN_WIDTH;
        for (x = 0; x < DC_SCREEN_WIDTH; x++) row[x] = c;
    }
}

/* Redraw the text band. `phase` is the shine position in [0,1] as a fraction of
 * a sweep that starts off the left edge and ends off the right; `fade` is 0..255
 * and scales the glyph colour so the first frames ramp in. */
static void spl_draw_text(int phase_x, int fade) {
    int sy, sx;
#ifndef DC_SPLASH_SHINE
    (void)phase_x;
#endif

    /* Repaint only the band. See the no-back-buffer warning above. */
    spl_fill_rows(SPL_Y, SPL_Y + SPL_H);

    for (sy = 0; sy < SPL_SRC_H; sy++) {
        const unsigned char* mrow = s_spl_mask + (size_t)sy * SPL_MASK_STRIDE;
        for (sx = 0; sx < SPL_SRC_W; sx++) {
            int r, g, b, zy, zx;
            unsigned short c;
            if (!(mrow[sx >> 3] & (0x80u >> (sx & 7)))) continue;

            /* Base is a cool white; the sweep adds a warm core so the highlight
             * reads as a specular pass rather than as a brightness change. */
            r = 198; g = 204; b = 226;
#ifdef DC_SPLASH_SHINE
            {
                int dx = sx * SPL_ZOOM - phase_x;
                if (dx < 0) dx = -dx;
                if (dx < 110) {
                    int t = 110 - dx;        /* 0..110 */
                    int w = (t * t) / 110;   /* quadratic falloff, 0..110 */
                    r += (255 - r) * w / 110;
                    g += (250 - g) * w / 110;
                    b += (215 - b) * w / 110;  /* pulls warm at the core */
                }
            }
#endif
            r = r * fade / 255; g = g * fade / 255; b = b * fade / 255;
            c = spl_rgb(r, g, b);

            for (zy = 0; zy < SPL_ZOOM; zy++) {
                unsigned short* row = vram_s +
                    (size_t)(SPL_Y + sy * SPL_ZOOM + zy) * DC_SCREEN_WIDTH +
                    SPL_X + sx * SPL_ZOOM;
                for (zx = 0; zx < SPL_ZOOM; zx++) row[zx] = c;
            }
        }
    }
}

#ifndef DC_SPLASH_NO_BAR
static void spl_draw_bar(unsigned int done, unsigned int total) {
    int y, x, filled;
    const unsigned short frame = spl_rgb(40, 60, 110);
    const unsigned short fill  = spl_rgb(120, 190, 255);

    if (total == 0) total = 1;
    if (done > total) done = total;
    filled = (int)(((unsigned long long)done * (unsigned long long)(SPL_BAR_W - 2))
                   / total);

    for (y = 0; y < SPL_BAR_H; y++) {
        unsigned short* row = vram_s + (size_t)(SPL_BAR_Y + y) * DC_SCREEN_WIDTH
                            + SPL_BAR_X;
        for (x = 0; x < SPL_BAR_W; x++) {
            int edge = (y == 0 || y == SPL_BAR_H - 1 ||
                        x == 0 || x == SPL_BAR_W - 1);
            row[x] = edge ? frame
                          : ((x - 1 < filled) ? fill : spl_bg(SPL_BAR_Y + y));
        }
    }
}
#endif

/* Called from the keep-list sweep so the bar tracks the REAL load, and so the
 * shine keeps moving while the disc is being read. Both are no-ops until the
 * splash has actually drawn, which is what s_spl_ready guards: this runs on the
 * boot path and must never write vram_s before vid_set_mode(), nor after
 * pvr_init() has reprogrammed the display controller at its own buffers. */
void dc_splash_progress(unsigned int done, unsigned int total) {
#if !defined(DC_HOST_STUB) && !defined(DC_NO_SPLASH)
    if (!s_spl_ready) return;
#ifdef DC_SPLASH_SHINE
    {
        /* Tie the sweep to load progress rather than to a clock, so the
         * animation IS the progress indication even before the bar is read. */
        static unsigned int tick = 0;
        int span = SPL_W + 220;
        spl_draw_text(((int)(++tick * 14) % span) - 110, 255);
    }
#endif
#ifndef DC_SPLASH_NO_BAR
    spl_draw_bar(done, total);
#else
    (void)done; (void)total;
#endif
#else
    (void)done; (void)total;
#endif
}

static void dc_splash(void) {
    uint64_t t0;
    int sy, sx, shot = 0;

    /* STEP 1 — capture the glyph shapes.
     *
     * bfont draws straight into vram_s and there is no way to ask it for a
     * bitmap, so the text is rendered once into the top-left corner, read back
     * into the 1-bit mask, and then buried under the gradient before the hold
     * loop starts. The scratch draw is on screen for well under a frame and is
     * never held, which is why no back buffer is needed for it. */
    memset(vram_s, 0, (size_t)DC_SCREEN_WIDTH * DC_SCREEN_HEIGHT * 2);
    bfont_set_foreground_color(0xFFFFFFFF);
    bfont_set_background_color(0x00000000);
    bfont_draw_str_vram_fmt(0, 0, false, "%s", DC_SPLASH_TEXT);

    memset(s_spl_mask, 0, sizeof(s_spl_mask));
    for (sy = 0; sy < SPL_SRC_H; sy++) {
        const unsigned short* row = vram_s + (size_t)sy * DC_SCREEN_WIDTH;
        for (sx = 0; sx < SPL_SRC_W; sx++)
            if (row[sx])
                s_spl_mask[sy * SPL_MASK_STRIDE + (sx >> 3)] |= 0x80u >> (sx & 7);
    }

    /* Count what bfont actually wrote. kb/traps.md: "a framebuffer HASH is not
     * a framebuffer TEST — count nonzero pixels". Zero here means the mask is
     * empty and every frame below will draw an empty band, which is otherwise
     * indistinguishable from a splash nobody looked at. */
    {
        unsigned int lit = 0;
        int i, n = SPL_MASK_STRIDE * SPL_SRC_H, k;
        for (i = 0; i < n; i++)
            for (k = 0; k < 8; k++)
                if (s_spl_mask[i] & (1u << k)) lit++;
        DC_LOGE("[DC] splash: \"%s\" mask %u px of %d, %dx%d at %d,%d, %d ms\n",
                DC_SPLASH_TEXT, lit, SPL_SRC_W * SPL_SRC_H,
                SPL_W, SPL_H, SPL_X, SPL_Y, (int)DC_SPLASH_MS);
    }

    /* STEP 2 — the field. This also erases the scratch draw. */
    spl_fill_rows(0, DC_SCREEN_HEIGHT);
    s_spl_ready = 1;

    /* STEP 3 — hold and animate. Any button skips; the pad is polled through
     * maple, which KOS drives from the vblank IRQ, so this needs no scheduler.
     * vid_waitvbl() paces it at 60 Hz and keeps the band redraw off the
     * scanout's own read of that region. */
    t0 = timer_ms_gettime64();
    for (;;) {
        uint64_t el = timer_ms_gettime64() - t0;
        int fade, phase, span;
        maple_device_t* dev;
        cont_state_t* st;

        if (el >= (uint64_t)(DC_SPLASH_MS)) break;

        fade  = (el < 400u) ? (int)((el * 255u) / 400u) : 255;
        span  = SPL_W + 220;
        /* One full sweep across the title per DC_SPLASH_MS, offset so it enters
         * from off the left edge. */
        phase = (int)((el * (uint64_t)span) / (uint64_t)(DC_SPLASH_MS)) - 110;
        spl_draw_text(phase, fade);
#ifndef DC_SPLASH_NO_BAR
        spl_draw_bar(0, 1);        /* the empty frame, so it does not pop in */
#endif

        /* One screenshot, at the sweep's midpoint, so the splash is judged on a
         * picture like every other renderer change (kb/traps.md, "screenshots
         * are the gate, not the counters"). Compiled out unless the image has
         * DC_FB_PROBE and DC_FB_IMAGE. */
        if (!shot && el >= (uint64_t)(DC_SPLASH_MS) / 2u) {
            shot = 1;
            dc_pvr_fb_dump_surface(vram_s);
        }

        dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        st  = dev ? (cont_state_t*)maple_dev_status(dev) : NULL;
        if (st && st->buttons) break;
        vid_waitvbl();
    }

    /* Deliberately NOT cleared. dc_gx_backend_start() is deferred until after
     * the keep-list load (see dc_platform.h), so leaving the title up — now
     * with a bar that dc_splash_progress() advances — turns what used to be a
     * black "is it hung?" gap into a real loading screen. The PVR wipes it when
     * it finally takes the display. DC_SPLASH_MS is a MINIMUM on-screen time,
     * not the total. */
}
#else
static void dc_splash(void) { }
void dc_splash_progress(unsigned int done, unsigned int total) {
    (void)done; (void)total;
}
#endif

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

    /* Between the Sega licence screen and the game. Must be before
     * dc_gx_init(), which is where the PVR eventually takes the display. */
    dc_splash();

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
    /* List the root. ISO9660 mangles names — uppercasing, 8.3 truncation, a
     * ";1" version suffix — and which of those survive depends on whether the
     * Joliet tree is present and whether KOS attached to it. Every DVD open in
     * this port is a literal lowercase name (dc_dvd.c), so a mismatch shows up
     * as "miss:" on every file with no hint why. Print what is actually there
     * once, at boot; it costs one directory read. */
    {
        dirent_t* e;
        int n = 0;
        while ((e = fs_readdir(d)) != NULL) {
            DC_LOGE("[DC] /cd/%s  %d B\n", e->name, e->size);
            n++;
        }
        DC_LOGE("[DC] /cd mounted, %d entries\n", n);
    }
    fs_close(d);
#endif
}

/* ==========================================================================
 * DC_ASSET_STUB keep list — real asset bytes inside an otherwise-stubbed image
 * ==========================================================================
 * DC_ASSET_STUB shrinks 16,281 asset destination arrays to [1] so the image
 * fits. pc_assets_init() must NOT run in that build: it walks s_assets[] and
 * memcpy()s the full-size asset over every one of those [1] destinations,
 * scribbling over whatever the linker put next. That skip stays.
 *
 * But renderer bring-up needs *something* real on screen. So
 * tools/dcstub/make_stub_data.py takes an allowlist ($DC_STUB_KEEP, default =
 * the title-logo overlay) of sources it leaves at FULL size, and generates
 * dc/build/stubsrc/dc_stub_keep.inc: a dc_stub_keep_load() that touches those
 * destinations and only those. Included below; resolved through the Makefile's
 * -I$(ROOT). Setting DC_STUB_KEEP= empty makes it an empty function, which is
 * exactly the old behaviour.
 *
 * The loads cannot go through pc_load_asset(). That function wants
 * g_rel_data / g_dol_data resident — all 15,640,056 B of foresta.rel in RAM at
 * once (see dc_dvd.c's C6 warning) — and its .bin fallback is a host-relative
 * fopen() that resolves to nothing on a read-only /cd. dc_stub_keep_load_one()
 * below is the Dreamcast answer: seek to rom_off in the ROM image on the disc,
 * read exactly `size` bytes straight into the destination, byte-swap in place.
 * Peak transient is zero. The generator rewrites the identifier
 * `pc_load_asset` to `dc_stub_keep_load_one` inside kept TUs, which is how the
 * generator's own per-file `_pc_load_src_*()` initialisers (the only way to
 * reach a file-static destination) end up here too.
 */
#ifdef DC_ASSET_STUB

/* These mirror pc/src/pc_assets.c's enums, which are file-static there. If the
 * generator's numbering ever changes, this is the other end that must move. */
enum { DC_STUB_SRC_REL = 0, DC_STUB_SRC_DOL = 1, DC_STUB_SRC_NONE = 2 };
enum { DC_STUB_SWAP_NONE = 0, DC_STUB_SWAP_U16 = 1,
       DC_STUB_SWAP_VTX  = 2, DC_STUB_SWAP_U32 = 3 };

/* Non-static in pc_assets.c and small; referencing them here is what keeps
 * --gc-sections from dropping them along with pc_assets_init(). */
extern void pc_bswap_asset_u16(void* data, unsigned int size);
extern void pc_bswap_asset_vtx(void* data, unsigned int size);
extern void pc_bswap_asset_u32(void* data, unsigned int size);

static int dc_stub_keep_ok;
static int dc_stub_keep_bad;
static unsigned int dc_stub_keep_bytes;

#ifndef DC_HOST_STUB
/* Indexed by rom_src. Held open across the whole burst: a CD-R seek is ~200 ms
 * and re-opening per asset would cost more than the read itself. */
static file_t dc_stub_rom_fd[2] = { FILEHND_INVALID, FILEHND_INVALID };
static const char* const dc_stub_rom_path[2] = {
    "/cd/foresta.rel",   /* DC_STUB_SRC_REL — already Yaz0-decompressed */
    "/cd/main.dol"       /* DC_STUB_SRC_DOL */
};
#endif

void dc_stub_keep_load_one(const char* bin_path, void* dest, unsigned int size,
                           unsigned int rom_off, int rom_src, int swap);

/* ==========================================================================
 * DC_KEEP_SWEEP — read the keep list in DISC ORDER, not in source order
 * ==========================================================================
 * The generated dc_stub_keep.inc calls the loader below 1,392 times in the
 * order the keep list happens to be written, and each call is an fs_seek plus
 * an fs_read. Measured from a real run's [DC/KEEP] lines:
 *
 *   874,736 useful bytes, offsets 3,012,288 .. 14,399,968 of a 15.6 MB file
 *   1,235 of 1,392 reads are < 2048 B; 932 are < 512 B
 *   64 backward jumps; 255 MB of total seek distance travelled
 *   only 559 distinct 2048-byte sectors are actually wanted, in 112 runs
 *
 * KOS's iso9660 layer has a 16-block x 2048 B LRU sector cache
 * (fs_iso9660.c:211) and starts a drive-level stream on a sector-aligned read
 * (fs_iso9660.c:755-777), so it already does read-ahead — which is why this
 * does NOT add a buffer ring. What it cannot do is fix the ORDER. Modelling
 * that cache against the observed request order gives ~578 single-sector
 * GD-ROM commands, each preceded by a seek to a scattered offset.
 *
 * Sorting the same requests by (rom_src, rom_off) and sweeping them with one
 * window buffer, modelled on the same trace:
 *
 *     policy                       GD-ROM commands    bytes moved
 *     today (source order)                    578        ~1.18 MB
 *     sorted + 8 KB window                    193         1.61 MB
 *     sorted + 16 KB window                   129         2.12 MB
 *     sorted + 32 KB window                    89         2.92 MB
 *
 * 16 KB is the knee: -449 commands for +0.94 MB. At ~500 KB/s the extra bytes
 * cost 1.9 s and the removed seeks save 449 x T_seek, which is 9 s at an
 * optimistic 20 ms and 45 s at 100 ms. On Flycast with FastGDRomLoad the win
 * is invisible; this is a hardware-only optimisation and the COMMAND COUNT is
 * what to A/B, not the wall clock.
 *
 * WHY ONE PASS AND NOT TWO. The obvious shape is "call dc_stub_keep_load()
 * once to count, once to record". That is WRONG and would silently drop
 * assets: some rewritten loaders carry the generator's load-once guard —
 * src/furniture/ac_radio_test.c has `static int radio_pal_loaded` — so the
 * second traversal skips them. The loader therefore RECORDS on its single
 * pass into a table that grows by realloc, and the sweep runs afterwards.
 *
 * RAM: transient only, and freed before OSInit carves the arena (boot order
 * step 5 vs step 7, see the header of this file). ~28 KB of table plus the
 * window. It is charged to libc's sbrk share while it lives — see
 * kb/heap-two-pools.md — so the sweep reports its own peak rather than
 * leaving anyone to guess.
 *
 * Kill switch: -DDC_KEEP_SWEEP=0 restores the old behaviour exactly (the
 * loader does its own seek+read inline, as it always did).
 * Window size: -DDC_KEEP_SWEEP_WIN=<bytes>, default 16384.
 */
#if !defined(DC_HOST_STUB) && !defined(DC_KEEP_SWEEP)
#define DC_KEEP_SWEEP 1
#endif
#ifndef DC_KEEP_SWEEP_WIN
#define DC_KEEP_SWEEP_WIN 16384u
#endif

#if defined(DC_KEEP_SWEEP) && DC_KEEP_SWEEP
#define DC_KEEP_SECTOR 2048u

typedef struct {
    const char*  who;
    void*        dest;
    unsigned int size;
    unsigned int rom_off;
    short        rom_src;
    short        swap;
} dc_keep_rec_t;

static dc_keep_rec_t* s_keep_rec;
static unsigned int   s_keep_n;
static unsigned int   s_keep_cap;
static int            s_keep_recording;   /* 1 while the .inc is traversing */

/* Sort by source file, then by offset: one forward sweep per ROM. */
static int dc_keep_cmp(const void* a, const void* b) {
    const dc_keep_rec_t* x = (const dc_keep_rec_t*)a;
    const dc_keep_rec_t* y = (const dc_keep_rec_t*)b;
    if (x->rom_src != y->rom_src) return x->rom_src - y->rom_src;
    if (x->rom_off < y->rom_off) return -1;
    if (x->rom_off > y->rom_off) return 1;
    return 0;
}

static int dc_keep_record(const char* who, void* dest, unsigned int size,
                          unsigned int rom_off, int rom_src, int swap) {
    if (!s_keep_recording) return 0;
    if (s_keep_n == s_keep_cap) {
        unsigned int cap = s_keep_cap ? s_keep_cap * 2u : 256u;
        dc_keep_rec_t* p = (dc_keep_rec_t*)realloc(s_keep_rec,
                                                   cap * sizeof(*p));
        if (!p) {
            /* Out of room: fall back to loading this one inline. Correctness
             * before speed — a dropped record is a zeroed asset. */
            DC_LOGE("[DC/KEEP] sweep table realloc to %u failed; "
                    "loading inline from here\n", cap);
            s_keep_recording = 0;
            return 0;
        }
        s_keep_rec = p;
        s_keep_cap = cap;
    }
    s_keep_rec[s_keep_n].who     = who;
    s_keep_rec[s_keep_n].dest    = dest;
    s_keep_rec[s_keep_n].size    = size;
    s_keep_rec[s_keep_n].rom_off = rom_off;
    s_keep_rec[s_keep_n].rom_src = (short)rom_src;
    s_keep_rec[s_keep_n].swap    = (short)swap;
    s_keep_n++;
    return 1;
}
#endif /* DC_KEEP_SWEEP */

void dc_stub_keep_load_one(const char* bin_path, void* dest, unsigned int size,
                           unsigned int rom_off, int rom_src, int swap)
{
#ifdef DC_HOST_STUB
    (void)bin_path; (void)dest; (void)size;
    (void)rom_off;  (void)rom_src; (void)swap;
#else
    const char* who = bin_path ? bin_path : "(anon)";
    file_t fd;

    if (dest == NULL || size == 0) return;

#if defined(DC_KEEP_SWEEP) && DC_KEEP_SWEEP
    /* Recording pass: remember it and return. The sweep below does the I/O in
     * disc order. Validation of rom_src stays BELOW this point on purpose —
     * a bad rom_src is diagnosed once, at sweep time, not twice. */
    if (dc_keep_record(who, dest, size, rom_off, rom_src, swap))
        return;
#endif

    if (rom_src != DC_STUB_SRC_REL && rom_src != DC_STUB_SRC_DOL) {
        /* SRC_NONE means "the .bin file is the only source". There is no
         * assets/ tree on the disc, so this cannot be served. Say so — a
         * silently zeroed texture looks like a renderer bug for hours. */
        DC_LOGE("[DC/KEEP] %s: rom_src=%d has no on-disc source, left zeroed\n",
                who, rom_src);
        dc_stub_keep_bad++;
        return;
    }

    fd = dc_stub_rom_fd[rom_src];
    if (fd == FILEHND_INVALID) {
        fd = fs_open(dc_stub_rom_path[rom_src], O_RDONLY);
        if (fd == FILEHND_INVALID) {
            DC_LOGE("[DC/KEEP] %s MISSING — every kept asset from it stays "
                    "zeroed\n", dc_stub_rom_path[rom_src]);
            dc_stub_rom_fd[rom_src] = FILEHND_INVALID;
            dc_stub_keep_bad++;
            return;
        }
        dc_stub_rom_fd[rom_src] = fd;
        DC_LOGE("[DC/KEEP] opened %s (%d B)\n", dc_stub_rom_path[rom_src],
                (int)fs_total(fd));
    }

    if (fs_seek(fd, (off_t)rom_off, SEEK_SET) < 0) {
        DC_LOGE("[DC/KEEP] %s: seek to %u failed\n", who, rom_off);
        dc_stub_keep_bad++;
        return;
    }
    if (fs_read(fd, dest, size) != (ssize_t)size) {
        DC_LOGE("[DC/KEEP] %s: short read of %u B at %u\n", who, size, rom_off);
        dc_stub_keep_bad++;
        return;
    }

    switch (swap) {
        case DC_STUB_SWAP_U16: pc_bswap_asset_u16(dest, size); break;
        case DC_STUB_SWAP_VTX: pc_bswap_asset_vtx(dest, size); break;
        case DC_STUB_SWAP_U32: pc_bswap_asset_u32(dest, size); break;
        default: break;
    }

    dc_stub_keep_ok++;
    dc_stub_keep_bytes += size;
#ifdef DC_KEEP_TRACE
    /* ⚠️ FIFTEEN SECONDS OF BOOT, ON HARDWARE, PER RUN.
     *
     * This prints once per kept asset and the opening keep list has 1,392 of
     * them: measured 86,357 bytes of console in one 600 s run. KOS busy-waits
     * on the SCIF TX FIFO whether or not a cable is attached (the same
     * mechanism that cost 8x the frame rate in kb/traps.md), so at the KOS
     * default 57,600 baud that is 5,760 B/s = **15.0 s of dead boot time** with
     * nothing on screen — which is exactly the window in which a human cannot
     * tell "it is loading" from "it has hung". The whole log is 51.4 s.
     *
     * The per-asset detail has never answered a question the summary below
     * could not: dc_stub_keep_bad and the SHORT READ line already name any
     * asset that fails. So it is off by default and -DDC_KEEP_TRACE brings it
     * back for the one case that wants it — auditing which assets a new keep
     * list actually pulled. */
    DC_LOG("[DC/KEEP] %s %u B @ %u\n", who, size, rom_off);
#else
    (void)who; (void)rom_off;
#endif
#endif
}

#include "dc/build/stubsrc/dc_stub_keep.inc"

#if defined(DC_KEEP_SWEEP) && DC_KEEP_SWEEP
/* Replay the recorded table in disc order through one window buffer. */
static void dc_keep_sweep(void) {
    unsigned char* win = NULL;
    unsigned int   win_cap = (unsigned int)(DC_KEEP_SWEEP_WIN);
    int            win_src = -1;         /* which ROM the window holds       */
    unsigned int   win_off = 0, win_len = 0;
    unsigned int   i, reads = 0, disc_bytes = 0;
    unsigned int   t0 = (unsigned int)timer_ms_gettime64();

    if (s_keep_n == 0) return;

    qsort(s_keep_rec, s_keep_n, sizeof(*s_keep_rec), dc_keep_cmp);

    win = (unsigned char*)malloc(win_cap);
    if (!win) {
        DC_LOGE("[DC/KEEP] sweep window malloc(%u) failed; per-asset reads\n",
                win_cap);
        win_cap = 0;
    }

    for (i = 0; i < s_keep_n; i++) {
        const dc_keep_rec_t* r = &s_keep_rec[i];
        file_t fd;

        /* The loading screen, driven by the real work. Every 32nd asset, not
         * every one: this repaints a 552x48 band plus the bar, and 1,392 full
         * repaints would show up next to a load that is otherwise ~130 disc
         * reads. 32 gives ~43 updates, which is smooth at this duration. */
        if ((i & 31u) == 0u) dc_splash_progress(i, s_keep_n);

        if (r->rom_src != DC_STUB_SRC_REL && r->rom_src != DC_STUB_SRC_DOL) {
            DC_LOGE("[DC/KEEP] %s: rom_src=%d has no on-disc source, "
                    "left zeroed\n", r->who, (int)r->rom_src);
            dc_stub_keep_bad++;
            continue;
        }

        fd = dc_stub_rom_fd[r->rom_src];
        if (fd == FILEHND_INVALID) {
            fd = fs_open(dc_stub_rom_path[r->rom_src], O_RDONLY);
            if (fd == FILEHND_INVALID) {
                DC_LOGE("[DC/KEEP] %s MISSING — every kept asset from it "
                        "stays zeroed\n", dc_stub_rom_path[r->rom_src]);
                dc_stub_keep_bad++;
                continue;
            }
            dc_stub_rom_fd[r->rom_src] = fd;
            DC_LOGE("[DC/KEEP] opened %s (%d B)\n",
                    dc_stub_rom_path[r->rom_src], (int)fs_total(fd));
        }

        /* Served by the window? */
        if (win_cap && r->rom_src == win_src &&
            r->rom_off >= win_off &&
            r->rom_off + r->size <= win_off + win_len) {
            memcpy(r->dest, win + (r->rom_off - win_off), r->size);
        } else if (win_cap && r->size <= win_cap) {
            /* Refill, starting on a SECTOR boundary. An unaligned start makes
             * KOS serve the leading fragment from its single-sector cache
             * (fs_iso9660.c:250-302) and abort the drive stream, costing an
             * extra GD-ROM command on every single refill. */
            ssize_t got;
            unsigned int base = r->rom_off - (r->rom_off % DC_KEEP_SECTOR);
            unsigned int want = win_cap;

            if (base + want < r->rom_off + r->size)
                want = (r->rom_off + r->size) - base;   /* cannot exceed cap */

            if (fs_seek(fd, (off_t)base, SEEK_SET) < 0) {
                DC_LOGE("[DC/KEEP] %s: seek to %u failed\n", r->who, base);
                dc_stub_keep_bad++;
                continue;
            }
            got = fs_read(fd, win, want);
            if (got < (ssize_t)(r->rom_off - base + r->size)) {
                DC_LOGE("[DC/KEEP] %s: short window read at %u (%d of %u)\n",
                        r->who, base, (int)got, want);
                dc_stub_keep_bad++;
                win_src = -1; win_len = 0;
                continue;
            }
            win_src = r->rom_src;
            win_off = base;
            win_len = (unsigned int)got;
            reads++;
            disc_bytes += (unsigned int)got;
            memcpy(r->dest, win + (r->rom_off - win_off), r->size);
        } else {
            /* Bigger than the window: straight into the destination. */
            if (fs_seek(fd, (off_t)r->rom_off, SEEK_SET) < 0) {
                DC_LOGE("[DC/KEEP] %s: seek to %u failed\n",
                        r->who, r->rom_off);
                dc_stub_keep_bad++;
                continue;
            }
            if (fs_read(fd, r->dest, r->size) != (ssize_t)r->size) {
                DC_LOGE("[DC/KEEP] %s: short read of %u B at %u\n",
                        r->who, r->size, r->rom_off);
                dc_stub_keep_bad++;
                continue;
            }
            reads++;
            disc_bytes += r->size;
            win_src = -1; win_len = 0;   /* the stream moved; window is stale */
        }

        switch (r->swap) {
            case DC_STUB_SWAP_U16: pc_bswap_asset_u16(r->dest, r->size); break;
            case DC_STUB_SWAP_VTX: pc_bswap_asset_vtx(r->dest, r->size); break;
            case DC_STUB_SWAP_U32: pc_bswap_asset_u32(r->dest, r->size); break;
            default: break;
        }
        dc_stub_keep_ok++;
        dc_stub_keep_bytes += r->size;
#ifdef DC_KEEP_TRACE
        DC_LOG("[DC/KEEP] %s %u B @ %u\n", r->who, r->size, r->rom_off);
#endif
    }

    dc_splash_progress(s_keep_n, s_keep_n);   /* land the bar on full */

    /* reads= is the number to A/B. It is host-independent, so Flycast proves
     * the improvement even though FastGDRomLoad hides the wall clock. */
    DC_LOGE("[DC/KEEP] sweep: %u assets, %u disc reads, %u B from disc, "
            "win %u B, table %u B, %u ms\n",
            s_keep_n, reads, disc_bytes, win_cap,
            (unsigned int)(s_keep_cap * sizeof(dc_keep_rec_t)),
            (unsigned int)timer_ms_gettime64() - t0);

    free(win);
    free(s_keep_rec);
    s_keep_rec = NULL;
    s_keep_n = s_keep_cap = 0;
}
#endif /* DC_KEEP_SWEEP */

/* The narrow stand-in for pc_assets_init(). */
static void dc_stub_keep_assets(void) {
#if defined(DC_KEEP_SWEEP) && DC_KEEP_SWEEP
    s_keep_recording = 1;
    dc_stub_keep_load();          /* records; does no I/O */
    s_keep_recording = 0;
    dc_keep_sweep();              /* one forward pass per ROM */
#else
    dc_stub_keep_load();
#endif
#ifndef DC_HOST_STUB
    {
        int i;
        for (i = 0; i < 2; i++) {
            if (dc_stub_rom_fd[i] != FILEHND_INVALID) {
                fs_close(dc_stub_rom_fd[i]);
                dc_stub_rom_fd[i] = FILEHND_INVALID;
            }
        }
    }
#endif
    DC_LOGE("[DC] DC_ASSET_STUB keep list: %d assets, %u B loaded, %d failed\n",
            dc_stub_keep_ok, dc_stub_keep_bytes, dc_stub_keep_bad);
}
#endif /* DC_ASSET_STUB */

#if !defined(DC_HOST_STUB) && defined(DC_SCIF_FAST)
#include <dc/scif.h>
#include <kos/dbgio.h>

/* RAISE THE CONSOLE BAUD — EMULATOR RUNS ONLY.
 *
 * KOS's default SCIF rate is 57,600 baud = ~5.8 KB/s, and KOS busy-waits on the
 * TX FIFO, so console output is charged to the guest as real frame time. That
 * is not a footnote here: emu64's shared-vertex warning printed 10,877 times in
 * one 600 s town run and cost 8x the frame rate (kb/traps.md), and a single
 * DC_FB_IMAGE screenshot is ~205 KB of base64 — about 35 SECONDS of wall clock
 * at the default rate, which is why a screenshot run reaches a fraction of the
 * frames a plain run does and must never be read as a progression run.
 *
 * Flycast models the SCIF divisor faithfully, and the harness's own selftest
 * has run at 1,562,500 baud (~150 KB/s) since M0 — see
 * harness/dc/selftest/selftest.c:134 and harness/dc/console.sh's "two
 * guest-side rules". Same call here, so the game build gets the same ~27x.
 *
 * ⚠️ EMULATOR ONLY, and that is why it is opt-in rather than default: a real
 * coder's cable will not sync at 1.5 Mbps, so a hardware run with this compiled
 * in loses its console entirely — including any crash dump. `dc/build-dc.sh`
 * does not set it; `DC_SCIF_FAST=1` does.
 *
 * ⚠️ Still NEVER call scif_flush() (kb/traps.md): Flycast does not re-raise
 * TEND on an idle TX FIFO, KOS latches serial_enabled = 0, and the console dies
 * permanently. scif_init() after set_parameters is the documented sequence and
 * does not flush. */
static void dc_scif_fast_init(void) {
    scif_set_parameters(1562500, 1);
    scif_init();
    dbgio_dev_select("scif");
}
#else
static void dc_scif_fast_init(void) { }
#endif

int main(int argc, char* argv[]) {
    /* 0. Console speed, before anything can print. */
    dc_scif_fast_init();

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
     * KOS init, the console and the ledger run, not to render anything.
     *
     * dc_stub_keep_assets() is the exception, and only the exception: it fills
     * the $DC_STUB_KEEP allowlist, whose destinations the generator left at
     * full size. See the block above dc_stub_keep_load_one(). */
    DC_LOGE("[DC] DC_ASSET_STUB: skipping pc_assets_init() — assets are [1]\n");
    dc_stub_keep_assets();
#else
    pc_assets_init();
#endif

    /* 5b. NOW bring the PVR up. Everything above — the disc check and the
     *     whole keep-list load — ran with the splash still on screen, because
     *     pvr_init() is what blanks it. See dc_gx_backend_start(). */
    dc_gx_backend_start();

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
