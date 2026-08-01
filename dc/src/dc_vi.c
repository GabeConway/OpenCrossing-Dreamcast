/* dc_vi.c - video interface: frame pacing, retrace, present.
 *
 * Ported from pc/src/pc_vi.c. The frame-pacing and dynamic-FPS logic is
 * portable and kept; the SDL swap becomes the PVR scene finish + vblank wait.
 *
 * ORDERING (design doc §1, rule 5): VI callbacks are registered before the
 * first frame and VIWaitForRetrace() is first called from sound_initial2() —
 * i.e. BEFORE the game loop — so this path must be live very early in boot.
 */
#include "dc_platform.h"

#define VI_TVMODE_NTSC_INT    0
#define VI_TVMODE_NTSC_DS     1
#define VI_TVMODE_PAL_INT     4
#define VI_TVMODE_MPAL_INT    8
#define VI_TVMODE_EURGB60_INT 20

static u32 retrace_count = 0;
u32 pc_frame_counter = 0;

static void (*vi_pre_callback)(u32) = NULL;
static void (*vi_post_callback)(u32) = NULL;

static u64 frame_start_us = 0;
static int s_logic_tick_count = 0;

/* Dynamic-FPS controller (g_pc_fps_target == 6 selects it via settings). */
static double s_dyn_ema_us = 0.0;
static int    s_dyn_inited = 0;
static int    s_dyn_probe_countdown = 0;

void VIInit(void) {
    /* The display mode is set once in dc_platform_init(); nothing to do per
     * the game's VIInit. */
}

void VIConfigure(void* rm) { (void)rm; }
void VISetNextFrameBuffer(void* fb) { (void)fb; }
void VIFlush(void) { }

void pc_dynamic_fps_reset(void) {
    s_dyn_ema_us = 0.0;
    s_dyn_inited = 0;
    s_dyn_probe_countdown = 0;
}

static void dc_dynamic_fps_update(u64 work_us) {
    double fps_opt;

    /* EMA alpha=0.25: absorbs single-frame spikes, reacts to sustained load in
     * ~4 frames. Same constants as the base port, which validated them on a
     * 56 fps handheld — the DC target is 30, so the clamp changes below. */
    if (!s_dyn_inited) {
        s_dyn_ema_us = (double)work_us;
        s_dyn_inited = 1;
    } else {
        s_dyn_ema_us = 0.25 * (double)work_us + 0.75 * s_dyn_ema_us;
    }

    if (s_dyn_ema_us <= 0.0) return;
    fps_opt = 1000000.0 / s_dyn_ema_us;
    /* PLAN §1: 30 fps is the DESIGN TARGET, not degraded-60. Never chase
     * above it — the extra frames would come out of the game-logic budget the
     * M3 gate measures. */
    if (fps_opt > 30.0) fps_opt = 30.0;
    if (fps_opt < 10.0) fps_opt = 10.0;
    g_pc_fps_target = (int)(fps_opt + 0.5);

    /* Upward probe: the batch measurement is bistable (kb/perf.md — the
     * device symptom was an outdoor area locked at 30 until a cheap interior
     * visit reset it). Periodically assume headroom and let the EMA
     * re-converge. */
    if (g_pc_fps_target < 30) {
        if (--s_dyn_probe_countdown <= 0) {
            s_dyn_probe_countdown = 120;
            s_dyn_ema_us *= 0.5;
        }
    }
}

/* Wait until the target visual-frame period has elapsed. On DC the natural
 * quantum is the vblank (~16.68 ms NTSC), so a 30 fps target is exactly two
 * vblanks — far cheaper and steadier than a spin loop. Sub-vblank targets fall
 * back to a busy wait on the microsecond timer. */
static void dc_pace_frame(void) {
    u64 target_us, now, elapsed;

    if (g_pc_no_framelimit || g_pc_fps_target <= 0) return;
    if (!frame_start_us) return;

    target_us = 1000000ull / (u64)g_pc_fps_target;

#ifndef DC_HOST_STUB
    /* Sleep out whole vblanks first. VERIFY: KOS dc/video.h vid_waitvbl(). */
    for (;;) {
        now = dc_time_us();
        elapsed = now - frame_start_us;
        if (elapsed + 16680ull > target_us) break;
        vid_waitvbl();
    }
#endif
    for (;;) {
        now = dc_time_us();
        elapsed = now - frame_start_us;
        if (elapsed >= target_us) break;
    }
}

void VIWaitForRetrace(void) {
    u64 vi_enter, t_before_swap, t_after_swap;
    double frame_ms = 0.0;

    /* Always poll input, even on logic-only ticks. */
    if (!dc_platform_poll_events()) {
        g_pc_running = 0;
        return;
    }

    s_logic_tick_count++;

    if (vi_pre_callback) vi_pre_callback(retrace_count);

    /* Logic-only tick (frameskip): no present, no pacing. frame_start_us is
     * intentionally NOT reset, so the render tick at the end of the batch
     * measures the full batch duration. */
    if (g_pc_frameskip_active) {
        pc_gx_begin_frame();
        retrace_count++;
        pc_frame_counter++;
        if (vi_post_callback) vi_post_callback(retrace_count);
        return;
    }

    vi_enter = dc_time_us();
    if (frame_start_us)
        frame_ms = (double)(vi_enter - frame_start_us) / 1000.0;

    t_before_swap = dc_time_us();
    dc_gx_end_frame();          /* terminates the open batch, ends the scene */
    dc_platform_swap_buffers();
    t_after_swap = dc_time_us();

    if (g_pc_fps_target == 6 && frame_start_us)
        dc_dynamic_fps_update(t_after_swap - frame_start_us);

    dc_pace_frame();

    dc_gx_frame_timing_snapshot();

    /* Adaptive stutter detection: only log frames well above the average. */
    {
        static double avg_frame_ms = 33.3;
        double thresh;
        if (frame_ms > 0.0)
            avg_frame_ms = avg_frame_ms * 0.95 + frame_ms * 0.05;
        thresh = avg_frame_ms * 1.5;
        if (thresh < 40.0) thresh = 40.0;
        if (frame_ms > thresh && g_pc_verbose) {
            DC_LOG("[STUTTER] frame %u: total=%.1fms (avg=%.1fms) "
                   "swap=%.1fms gx=%.1fms tex=%.1fms draws=%d\n",
                   (unsigned)pc_frame_counter, frame_ms, avg_frame_ms,
                   (double)(t_after_swap - t_before_swap) / 1000.0,
                   (double)dc_gx_flush_time_us / 1000.0,
                   (double)dc_gx_texload_time_us / 1000.0,
                   pc_gx_draw_call_count);
        }
    }

    /* Periodic PERF line. The harness greps this; keep the shape stable. */
    {
        static u64 fps_start = 0;
        static int fps_count = 0;
        static int logic_snap = 0;
        if (fps_start == 0) { fps_start = dc_time_us(); logic_snap = s_logic_tick_count; }
        fps_count++;
        if (fps_count >= 30) {
            u64 now = dc_time_us();
            double secs = (double)(now - fps_start) / 1000000.0;
            if (secs > 0.0) {
                double render_fps = (double)fps_count / secs;
                double logic_tps = (double)(s_logic_tick_count - logic_snap) / secs;
                DC_LOGE("[PERF] %.1f FPS | %.0f%% speed | draws=%d "
                        "(q=%d t=%d s=%d f=%d o=%d) merged=%d culled=%d "
                        "cmds=%d crashes=%d gx=%.1fms tex=%.1fms\n",
                        render_fps, logic_tps / 60.0 * 100.0, pc_gx_draw_call_count,
                        pc_gx_prim_draws[0], pc_gx_prim_draws[1], pc_gx_prim_draws[2],
                        pc_gx_prim_draws[3], pc_gx_prim_draws[4],
                        pc_gx_merged_batches, pc_gx_culled_draws,
                        pc_emu64_frame_cmds, pc_emu64_frame_crashes,
                        (double)dc_gx_flush_time_us / 1000.0,
                        (double)dc_gx_texload_time_us / 1000.0);
            }
            fps_start = now;
            fps_count = 0;
            logic_snap = s_logic_tick_count;
        }
    }

    frame_start_us = dc_time_us();

    pc_gx_begin_frame();
    retrace_count++;
    pc_frame_counter++;
    if (vi_post_callback) vi_post_callback(retrace_count);
}

u32  VIGetRetraceCount(void) { return retrace_count; }
void VISetBlack(BOOL black) { (void)black; }
u32  VIGetTvFormat(void) { return 0; /* VI_NTSC */ }
u32  VIGetDTVStatus(void) { return 0; }

void* VISetPreRetraceCallback(void* cb) {
    void* old = (void*)vi_pre_callback;
    vi_pre_callback = (void (*)(u32))cb;
    return old;
}

void* VISetPostRetraceCallback(void* cb) {
    void* old = (void*)vi_post_callback;
    vi_post_callback = (void (*)(u32))cb;
    return old;
}

u32  VIGetCurrentLine(void) { return 0; }
void VISetNextXFB(void* xfb) { (void)xfb; }
