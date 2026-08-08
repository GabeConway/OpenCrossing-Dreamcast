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
#include "dc_pvr.h"     /* dc_pvr_report(): the renderer half of the PERF line */

/* ==========================================================================
 * DC_AUDIO_HEAPLOG — the falsifier for "audiomemory can be shrunk further"
 * ==========================================================================
 * OFF BY DEFAULT, and it is a measurement, not a feature.
 *
 * WHAT IT IS FOR. tools/dcstub/make_src_shrink.py's S4 rule already takes
 * `audiomemory` from 0x90000 to 0x76000 and its comment says DO NOT GO FURTHER,
 * with 2,176 B of margin computed from a hand-enumerated consumer list. The
 * next 400 KB of that heap is the single biggest untaken .bss lever left, and
 * nobody may take it on arithmetic alone: `Nas_WaveDmaNew` (internal/system.c:431)
 * `break`s out of its allocation loop on exhaustion and drops voices with NO
 * diagnostic, so an over-shrunk heap is silent. This prints the heap's own
 * bookkeeping instead of re-deriving it.
 *
 * HOW TO READ IT.
 *   * `waveloads` well under 3 x `chans` means the wave-load table is not full
 *     and the heap is NOT the binding constraint;
 *   * any ALHeap whose used/len is near 1.0 IS at its floor — do not shrink
 *     `audiomemory` while that holds;
 *   * ⚠️ non-zero `emem` or `data` usage at DC_AUDIO=0 FALSIFIES the S8
 *     argument in make_src_shrink.py's header. That argument says the whole
 *     per-frame chain is severed with the pump, so nothing past init should
 *     ever have allocated out of the external/data heaps. If they are non-zero,
 *     something reaches the sequencer by a path this trace did not find, and
 *     the S8 pool shrinks need re-deriving before they are trusted.
 *
 * WHY IT PRINTS UNCONDITIONALLY AT A FIXED TICK rather than waiting for a
 * heap to look initialised: kb/traps.md, "a probe that reports a digest must
 * report a population count next to it". A probe that stays silent when the
 * numbers are zero is indistinguishable from a probe that was never compiled
 * in, and zero here is a RESULT.
 *
 * It is gated on `s_logic_tick_count`, not `pc_frame_counter` — same file,
 * kb/traps.md: the frameskip early-out makes pc_frame_counter jump by the skip
 * factor and a `% N` test can stop landing on it entirely.
 *
 * ⚠️ This is the one place in dc/ that includes a decomp header.
 * dc/src/dc_audio.c deliberately forward-declares `pc_audio_process_frame`
 * rather than pull include/jaudio_NES in, and that judgement stands for shipping
 * code. Here there is no alternative that is not worse: `AG` is a 34 KB struct
 * whose fields this reads by name, and hand-copying its offsets (session_heap
 * at 0x2A20, misc_heap at 0x2A5C, …) would produce a probe that silently reads
 * the wrong words the moment audiostruct.h moves. Since the whole block is
 * behind a flag that no shipping build sets, a header collision here can only
 * ever break the build of someone who asked for the measurement.
 */
#ifdef DC_AUDIO_HEAPLOG
#include "jaudio_NES/audiowork.h"
/* Which logic tick to sample on. Must be after StartAudioThread()/Nas_InitAudio()
 * and after the game has had a chance to ask for sound. 600 ticks is ~25 s at
 * the town's measured rate — past the title screen, well inside a `--timeout 600`
 * harness run. */
#ifndef DC_AUDIO_HEAPLOG_TICK
#define DC_AUDIO_HEAPLOG_TICK 600
#endif
#endif

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

/* Audio's per-tick cost, for the [STUTTER] line below. NOT inside the
 * DC_PERF_PHASE block that follows: the phase externs are gated, the stutter
 * printer is not, so these have to live where the unguarded printer can reach
 * them. Defined unconditionally in dc_audio.c (0 at DC_AUDIO=0), so no #if
 * here either.
 *
 * WHY THE FIELD EXISTS: with DC_AUDIO=1 the game hitches ~2x/second on real
 * hardware — 210 [STUTTER] events in 420 s, against 14 in 600 s silent, 21x
 * more per second. The line named swap, gx and tex and left ~24 ms of a 67.3 ms
 * frame unattributed, so the cause had to be guessed, and two guesses were
 * already spent: disc-cache misses (4x the cache changed nothing on hardware)
 * and a multi-frame synthesis burst (DC_AUDIO_MAX_FRAMES=2 caps that near
 * 9.6 ms). `snd=`/`sndf=` make the instrument answer it instead. */
extern u32 dc_audio_tick_usec;
extern u32 dc_audio_tick_frames;
extern u32 dc_audio_tick_synth_max_us;

/* The voice census (dc_audio.c, -DDC_AUDIO_VOICELOG). Defined unconditionally
 * over there, so these need no #if and read 0 on a build without the flag —
 * which is why they can be APPENDED to the [STUTTER] format string once rather
 * than duplicating it behind an #ifdef. `vmax=` is the worst frame in the
 * window; `v@=`/`filt@=`/`comb@=` describe the SAME synthesis call that set
 * smax=, so the row carries a cost and its candidate cause together. */
extern u32 dc_audio_tick_voice_max;
extern u32 dc_audio_tick_voice_at_max;
extern u32 dc_audio_tick_filt_at_max;
extern u32 dc_audio_tick_comb_at_max;

/* ==========================================================================
 * Frame-phase attribution (-DDC_PERF_PHASE) — see kb/perf-dc.md
 * ==========================================================================
 * The [PERF] line reports FPS and `gx=`, and the gap between them was being
 * guessed at. It is large: at 10.3 FPS the frame is 97 ms and gx= is 27 ms, so
 * 72 % of the town frame was unattributed.
 *
 * The decomposition this measures, and why it is the right one:
 *
 *   graph.c:401-429 runs the game logic `ticks_per_visual` times per PRESENTED
 *   frame and sets g_pc_frameskip_active on all but the last. At the default
 *   g_pc_fps_target = 30 that is exactly TWO logic ticks per displayed frame,
 *   one of which is thrown away — emu64 still walks the whole display list and
 *   still drives every GX setter on the skipped tick; only dc_gx_flush_vertices
 *   and the present are elided.
 *
 *   VIWaitForRetrace() is called once per LOGIC TICK, at the end of it. So the
 *   interval from the previous call's exit to this call's entry is exactly one
 *   tick's game work, and g_pc_frameskip_active on entry says which kind it
 *   was. Accumulating those two intervals separately splits the frame into
 *   "work that produced pixels" and "work that was discarded" — a distinction
 *   no existing counter could make.
 *
 * Everything reported is per PRESENTED frame, averaged over the same 30-frame
 * window as [PERF], so the numbers add up to the [PERF] frame time:
 *
 *   [PHASE] draw=<ms in the presented tick, gx included>
 *           skip=<ms in discarded logic ticks>  nskip=<how many>
 *           cull=<ms in the whole-batch AABB cull>
 *           xform=<ms in dc_gx_backend_submit: SH-4 T&L + TA submit>
 *           vi=<ms in this function outside the ticks: swap, pace, probes>
 *           v=<vertices submitted>  vlit=<of which in lit batches>
 *           us/v=<microseconds per submitted vertex>
 *
 * Cost when enabled: four dc_time_us() reads per tick here plus two per batch
 * in dc_gx.c. Off by default so the shipping build is byte-identical. */
#ifdef DC_PERF_PHASE
extern u64 dc_gx_cull_time_us;
extern u64 dc_gx_submit_time_us;
extern unsigned int dc_gx_phase_verts;
extern unsigned int dc_gx_phase_verts_lit;
extern unsigned int dc_gx_phase_batches_lit;
extern unsigned int dc_gx_phase_src_verts;
extern unsigned int dc_gx_phase_culled_verts;
#ifndef DC_PVR_NO_VTXMEMO
/* The per-batch vertex memo cache in dc_pvr.c. `vmemo=hit/total` is the number
 * that says whether emu64's index expansion is really re-submitting shared
 * vertices — if the hit rate is near zero the cache is pure overhead and should
 * be compiled out, so it reports rather than asserts. Free-running totals, not
 * per-window: the ratio is what matters and it is stable. */
extern unsigned int dc_pvr_vmemo_hit;
extern unsigned int dc_pvr_vmemo_total;
#endif

static u64 s_ph_last_exit  = 0;   /* when the previous tick was released     */
static u64 s_ph_draw_us    = 0;   /* game work on presented ticks            */
static u64 s_ph_skip_us    = 0;   /* game work on discarded (frameskip) ticks*/
static u64 s_ph_vi_us      = 0;   /* time inside VIWaitForRetrace itself     */
static u64 s_ph_cull_us    = 0;
static u64 s_ph_xform_us   = 0;
static unsigned int s_ph_nskip = 0;
static unsigned int s_ph_verts = 0;
static unsigned int s_ph_vlit  = 0;
static unsigned int s_ph_vsrc  = 0;
static unsigned int s_ph_vcull = 0;
#endif

#ifdef DC_PERF_GXAPI
/* GX entry-point census — see the block in dc_gx.c. Reported next to [PHASE]
 * because the two answer one question together: [PHASE] says how many ms are in
 * display-list traversal, [GXAPI] says how many GX calls that traversal makes,
 * and the product bounds how much of it is editable at all. */
extern unsigned int dc_gx_api_pos, dc_gx_api_clr, dc_gx_api_tc;
extern unsigned int dc_gx_api_nrm, dc_gx_api_begin, dc_gx_api_dirty;
extern u64 dc_gx_api_vtx_us;
static u64 s_api_pos, s_api_clr, s_api_tc, s_api_nrm, s_api_begin, s_api_dirty;
static u64 s_api_vtx_us;
#endif

#if defined(DC_PERF_GXSPLIT) && (DC_PERF_GXSPLIT) > 0
/* G4 — the TRIN split. The block in dc_gx.c carries the whole argument; this is
 * only the window accumulator and the print.
 *
 * ⚠️ In its OWN #if, not inside the DC_PERF_PHASE block, for the reason
 * recorded ~30 lines below about [EMU64H]: an instrument whose arm sites and
 * whose report site are gated differently arms, pays for itself, and prints
 * nothing. The cross-check against cull=/xform= does need -DDC_PERF_PHASE, and
 * the print says so by reporting zeros. */
extern u64 dc_gx_gxs_pos_ns, dc_gx_gxs_gap_ns, dc_gx_gxs_begin_ns;
extern u64 dc_gx_gxs_end_ns, dc_gx_gxs_state_ns;
extern unsigned int dc_gx_gxs_drops;
/* Its OWN vertex count, not dc_gx_api_pos: that one is written only under
 * -DDC_PERF_GXAPI, and a probe= line that silently reads 0 because a second,
 * unrelated flag was absent is the same failure this instrument is documented
 * to avoid. */
extern unsigned int dc_gx_gxs_posn;
static u64 s_gxs_pos, s_gxs_gap, s_gxs_begin, s_gxs_end, s_gxs_state;
static u64 s_gxs_drops, s_gxs_posn;
#endif

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

#ifdef DC_PERF_PHASE
    /* FIRST statement in the function: everything between the previous exit and
     * here is the game's own work for the tick that just finished, and
     * g_pc_frameskip_active still holds that tick's flag. */
    {
        u64 ph_now = dc_time_us();
        if (s_ph_last_exit) {
            if (g_pc_frameskip_active) {
                s_ph_skip_us += ph_now - s_ph_last_exit;
                s_ph_nskip++;
            } else {
                s_ph_draw_us += ph_now - s_ph_last_exit;
            }
        }
        s_ph_last_exit = ph_now;   /* re-stamped on the way out */
    }
#endif

#if defined(DC_EMU64_HIST) && DC_EMU64_HIST > 0
    /* Close FIRST, before anything else in this function runs: the display-list
     * traversal that the histogram was measuring happened between the previous
     * VIWaitForRetrace and this one, so this is the instant it ended. Closing
     * later would charge the swap and the probes to whichever opcode ran last.
     * The frameskip early-out below also passes through here, which is what
     * keeps arm/disarm balanced on skipped ticks. */
    dc_emu64_hist_frame_close();
#endif

#if defined(DC_EMU64_SHADOW_LOOP) && DC_EMU64_SHADOW_LOOP > 0
    /* G2, disarmed at the same instant and for the same reason as G1 above:
     * the traversal this frame's shadow loop ran happened between the previous
     * VIWaitForRetrace and this one. Disarming here also means the table is
     * back to the untouched -O0 originals for everything that runs after —
     * so a crash anywhere else in the frame is not a crash "inside the
     * shadow", and the two cannot be confused during triage. */
    dc_emu64_shadow_frame_close();
#endif

    /* Always poll input, even on logic-only ticks. */
    if (!dc_platform_poll_events()) {
        g_pc_running = 0;
        return;
    }

    s_logic_tick_count++;

    /* Before the frameskip early-out below: audio must keep being produced on
     * logic-only ticks too, or the ring drains and the stream underruns exactly
     * when the game is already struggling. Budgeted internally (DC_AUDIO=0
     * removes it entirely). */
    dc_audio_pump();

#ifdef DC_AUDIO_HEAPLOG
    /* ONE SHOT. See the block at the top of this file for what the numbers
     * mean and which of them falsify what. Four lines, once, because SCIF is
     * boot time on real hardware even with no cable attached (kb/traps.md). */
    {
        static int heaplog_done = 0;

        if (!heaplog_done && s_logic_tick_count >= (DC_AUDIO_HEAPLOG_TICK)) {
            /* ALHeap is { u8* base; u8* current; int length; s32 count;
             * u8* last; } (include/jaudio_NES/audiostruct.h:74), so "used" is
             * current - base and there is no separate high-water mark. A heap
             * that was never set up reads 0/0, which is a reportable answer. */
#define DC_AGH_USED(h) ((long)((h).current - (h).base))
#define DC_AGH_LEN(h)  ((long)(h).length)
            DC_LOGE("[DC/AUDIOHEAP] tick=%d  init=%ld/%ld  session=%ld/%ld  "
                    "misc=%ld/%ld\n",
                    s_logic_tick_count,
                    DC_AGH_USED(AG.init_heap),    DC_AGH_LEN(AG.init_heap),
                    DC_AGH_USED(AG.session_heap), DC_AGH_LEN(AG.session_heap),
                    DC_AGH_USED(AG.misc_heap),    DC_AGH_LEN(AG.misc_heap));
            /* emem/external/data are the FALSIFIERS: at DC_AUDIO=0 the trace
             * in make_src_shrink.py's header says nothing past init allocates
             * from them, so anything non-zero here means that trace is wrong. */
            DC_LOGE("[DC/AUDIOHEAP] emem=%ld/%ld  external=%ld/%ld  "
                    "data=%ld/%ld  szdata=%ld/%ld  szauto=%ld/%ld\n",
                    DC_AGH_USED(AG.emem_heap),     DC_AGH_LEN(AG.emem_heap),
                    DC_AGH_USED(AG.external_heap), DC_AGH_LEN(AG.external_heap),
                    DC_AGH_USED(AG.data_heap),     DC_AGH_LEN(AG.data_heap),
                    DC_AGH_USED(AG.sz_data_heap),  DC_AGH_LEN(AG.sz_data_heap),
                    DC_AGH_USED(AG.sz_auto_heap),  DC_AGH_LEN(AG.sz_auto_heap));
            /* ⚠️ AG.cache_heap is NOT an ALHeap — it is DataHeapstrc
             * { size_t data_size; size_t auto_size; } (audiostruct.h:746), i.e.
             * the CONFIGURED cache sizes with no used/len pair to report. It is
             * printed as configuration, and the heaps it configures are
             * data/szdata/szauto on the line above. */
            DC_LOGE("[DC/AUDIOHEAP] cache_cfg data=%lu auto=%lu  "
                    "misc_cfg=%lu cache_cfg=%lu  heap=%ld B\n",
                    (unsigned long)AG.cache_heap.data_size,
                    (unsigned long)AG.cache_heap.auto_size,
                    (unsigned long)AG.audio_heap_info.misc_heap_size,
                    (unsigned long)AG.audio_heap_info.cache_heap_size,
                    (long)AG.audio_heap_size);
            /* THE headline. `3 * num_channels` is the wave-load table's own
             * ceiling; waveloads far below it means the heap is not what is
             * limiting voices, so S4's "DO NOT GO FURTHER" can be revisited
             * with a real experiment rather than an argument. */
            /* DC_AUDIO is echoed because every number above is only
             * interpretable against it, and the two lines end up in the same
             * console log as a hundred others. dc/Makefile passes -DDC_AUDIO=1
             * only in the ON case, so `defined()` is the test, not the value. */
            DC_LOGE("[DC/AUDIOHEAP] waveloads=%lu of 3*chans=%ld  "
                    "(count=%lu)  DC_AUDIO=%d\n",
                    (unsigned long)AG.num_waveloads,
                    (long)(3 * AG.num_channels),
                    (unsigned long)AG.waveload_count,
#if defined(DC_AUDIO) && DC_AUDIO
                    1);
#else
                    0);
#endif
#undef DC_AGH_USED
#undef DC_AGH_LEN
            heaplog_done = 1;
        }
    }
#endif

    if (vi_pre_callback) vi_pre_callback(retrace_count);

    /* Logic-only tick (frameskip): no present, no pacing. frame_start_us is
     * intentionally NOT reset, so the render tick at the end of the batch
     * measures the full batch duration. */
    if (g_pc_frameskip_active) {
        pc_gx_begin_frame();
        retrace_count++;
        pc_frame_counter++;
        if (vi_post_callback) vi_post_callback(retrace_count);
#ifdef DC_PERF_PHASE
        {
            u64 ph_out = dc_time_us();
            s_ph_vi_us += ph_out - s_ph_last_exit;
            s_ph_last_exit = ph_out;
        }
#endif
        /* ⚠️ THE DISPATCH-TABLE INSTRUMENTS ARM HERE TOO, AND THIS IS THE ARM
         * THAT ACTUALLY CATCHES ANYTHING. Measured 2026-08-04: G1's first real
         * run came back with every opcode bucket EMPTY and 100 % of the time in
         * `gap` — `tot=8.13ms gap=8.13ms |` and nothing after the bar.
         *
         * The cause is this early return. Arming only from the presented path
         * (below) means the window opens AFTER a present and closes at the
         * entry of the next tick — and at ticks_per_visual = 2 the next tick is
         * the LOGIC-ONLY one, which runs no display-list traversal at all. The
         * instrument was faithfully measuring the one tick per frame that has
         * nothing to measure.
         *
         * Arming at the end of EVERY tick makes each window exactly one tick,
         * so the drawing ticks are sampled like any other. Arming during a
         * logic-only tick is harmless: no commands dispatch, so the thunks
         * simply do not fire and the window lands in `gap`. */
#if defined(DC_EMU64_HIST) && DC_EMU64_HIST > 0
        {
            static unsigned int hist_tick_skip = 0;
            dc_emu64_hist_frame_open(hist_tick_skip++);
        }
#endif
#if defined(DC_EMU64_SHADOW_LOOP) && DC_EMU64_SHADOW_LOOP > 0
        {
            static unsigned int shadow_tick_skip = 0;
            dc_emu64_shadow_frame_open(shadow_tick_skip++);
        }
#endif
#if defined(DC_EMU64_CULL) && DC_EMU64_CULL > 0
        /* G3. NOT a table swap — it flips a boolean and re-checks the two slots
         * it owns. Cheap enough to run on the frameskipped tick too, and it has
         * to: that tick issues no display-list commands but the NEXT one does,
         * and the A/B period must count the same ticks G1's does or the two
         * instruments disagree about which frame was armed. */
        {
            static unsigned int cull_tick_skip = 0;
            dc_emu64_cull_frame_open(cull_tick_skip++);
        }
#endif
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

#ifdef DC_PERF_PHASE
    /* Must come after the snapshot: dc_gx.c publishes and clears its per-frame
     * accumulators there, so reading before it would double-count one frame and
     * miss the next. */
    s_ph_cull_us  += dc_gx_cull_time_us;
    s_ph_xform_us += dc_gx_submit_time_us;
    s_ph_verts    += dc_gx_phase_verts;
    s_ph_vlit     += dc_gx_phase_verts_lit;
    s_ph_vsrc     += dc_gx_phase_src_verts;
    s_ph_vcull    += dc_gx_phase_culled_verts;
#endif
#ifdef DC_PERF_GXAPI
    s_api_pos   += dc_gx_api_pos;    s_api_clr   += dc_gx_api_clr;
    s_api_tc    += dc_gx_api_tc;     s_api_nrm   += dc_gx_api_nrm;
    s_api_begin += dc_gx_api_begin;  s_api_dirty += dc_gx_api_dirty;
    s_api_vtx_us += dc_gx_api_vtx_us;
#endif
#if defined(DC_PERF_GXSPLIT) && (DC_PERF_GXSPLIT) > 0
    /* After the snapshot, same rule as [PHASE] above. */
    s_gxs_pos   += dc_gx_gxs_pos_ns;    s_gxs_gap   += dc_gx_gxs_gap_ns;
    s_gxs_begin += dc_gx_gxs_begin_ns;  s_gxs_end   += dc_gx_gxs_end_ns;
    s_gxs_state += dc_gx_gxs_state_ns;  s_gxs_drops += dc_gx_gxs_drops;
    s_gxs_posn  += dc_gx_gxs_posn;
#endif

    /* Adaptive stutter detection: only log frames well above the average. */
    {
        static double avg_frame_ms = 33.3;
        double thresh;
        if (frame_ms > 0.0)
            avg_frame_ms = avg_frame_ms * 0.95 + frame_ms * 0.05;
        thresh = avg_frame_ms * 1.5;
        if (thresh < 40.0) thresh = 40.0;
        if (frame_ms > thresh && g_pc_verbose) {
            /* snd/sndf are APPENDED, never inserted: tools/dcqa/run_report.py
             * greps this line, so the existing fields keep their order and
             * draws= stays last. */
            DC_LOG("[STUTTER] frame %u: total=%.1fms (avg=%.1fms) "
                   "swap=%.1fms gx=%.1fms tex=%.1fms snd=%.1fms sndf=%d "
                   "smax=%.1fms vmax=%u v@=%u filt@=%u comb@=%u draws=%d\n",
                   (unsigned)pc_frame_counter, frame_ms, avg_frame_ms,
                   (double)(t_after_swap - t_before_swap) / 1000.0,
                   (double)dc_gx_flush_time_us / 1000.0,
                   (double)dc_gx_texload_time_us / 1000.0,
                   (double)dc_audio_tick_usec / 1000.0,
                   (int)dc_audio_tick_frames,
                   (double)dc_audio_tick_synth_max_us / 1000.0,
                   (unsigned)dc_audio_tick_voice_max,
                   (unsigned)dc_audio_tick_voice_at_max,
                   (unsigned)dc_audio_tick_filt_at_max,
                   (unsigned)dc_audio_tick_comb_at_max,
                   pc_gx_draw_call_count);
        }
    }

    /* THE PER-TICK RESET, and it belongs here rather than in dc_audio.c so all
     * five numbers on the line describe ONE window. gx=/tex= are cleared inside
     * dc_gx_frame_timing_snapshot() (dc_gx.c:614-615), whose only call site is
     * ~30 lines above; no dc_audio_pump() runs between that call and this
     * statement — the pump is at the TOP of the tick (line ~300) — so clearing
     * after the print puts the audio window on the same frame boundary as the
     * gx pair, and on the same boundary as total=. Read before the print,
     * cleared after it: a non-stuttering frame is cleared too, so `snd=` is
     * never a stale sum from an earlier frame. */
    dc_audio_tick_usec = 0;
    dc_audio_tick_frames = 0;
    dc_audio_tick_synth_max_us = 0;
    /* Cleared with the rest of the window, and it MUST be: smax= and v@= are
     * paired by "cost >= the running max", so a stale smax carried into the next
     * window would stop v@= from ever being rewritten and freeze the pairing on
     * one old frame. The [DC/VOICE] histogram in dc_audio.c is deliberately NOT
     * reset — it is the whole-run distribution. */
    dc_audio_tick_voice_max = 0;
    dc_audio_tick_voice_at_max = 0;
    dc_audio_tick_filt_at_max = 0;
    dc_audio_tick_comb_at_max = 0;

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
            /* Paired with [PERF] on purpose: without a renderer census next
             * to the draw-call count, a black screen cannot be attributed
             * between "nothing submitted" and "submitted and discarded". */
#ifdef DC_PERF_PHASE
            /* Per PRESENTED frame, so it lines up with the FPS above: 30 is the
             * window size, not a magic number. `us/v` is the one number to
             * optimise against — it is xform time divided by the vertices that
             * actually reached the transform, and it is invariant to how much
             * geometry the scene happens to contain. */
            {
                double n = 30.0;
                DC_LOGE("[PHASE] draw=%.1f skip=%.1f (n=%u) vi=%.1f | "
                        "cull=%.1f xform=%.1f | v=%u vsrc=%u vlit=%u "
                        "vcull=%u us/v=%.2f"
#ifndef DC_PVR_NO_VTXMEMO
                        " vmemo=%u/%u"
#endif
                        "\n",
                        (double)s_ph_draw_us / 1000.0 / n,
                        (double)s_ph_skip_us / 1000.0 / n,
                        (unsigned)(s_ph_nskip),
                        (double)s_ph_vi_us / 1000.0 / n,
                        (double)s_ph_cull_us / 1000.0 / n,
                        (double)s_ph_xform_us / 1000.0 / n,
                        (unsigned)(s_ph_verts / 30u),
                        (unsigned)(s_ph_vsrc / 30u),
                        (unsigned)(s_ph_vlit / 30u),
                        (unsigned)(s_ph_vcull / 30u),
                        s_ph_verts ? (double)s_ph_xform_us / (double)s_ph_verts
                                   : 0.0
#ifndef DC_PVR_NO_VTXMEMO
                        , dc_pvr_vmemo_hit, dc_pvr_vmemo_total
#endif
                        );
                s_ph_draw_us = s_ph_skip_us = s_ph_vi_us = 0;
                s_ph_cull_us = s_ph_xform_us = 0;
                s_ph_nskip = s_ph_verts = s_ph_vlit = s_ph_vsrc = 0;
                s_ph_vcull = 0;
            }

            /* The opcode mix and emu64's OWN display-list cull, both of which
             * emu64 has been maintaining for free since the PC port
             * (emu64.c:5824-5834 and :5309-5315) and which nothing printed.
             * `cmds` alone cannot tell a vertex-bound frame from a
             * state-change-bound one, and `cullrej` is the current hit rate of
             * the G_CULLDL path that kb/research-fps-ideas.md F1 proposes to
             * inject more of -- a baseline for that idea costs this line.
             * Per FRAME, not per window: these are reset by emu64 every
             * emu64_taskstart_r, so the value here is the last frame of the
             * window, not a mean. */
            DC_LOGE("[EMU64] cmds=%d noop=%d vtx=%d tri=%d dl=%d | "
                    "cullvis=%d cullrej=%d\n",
                    pc_emu64_frame_cmds, pc_emu64_frame_noop_cmds,
                    pc_emu64_frame_vtx_cmds, pc_emu64_frame_tri_cmds,
                    pc_emu64_frame_dl_cmds,
                    pc_emu64_frame_cull_visible, pc_emu64_frame_cull_rejected);
#endif  /* DC_PERF_PHASE */

            /* ⚠️ DELIBERATELY OUTSIDE THE DC_PERF_PHASE BLOCK ABOVE, and it was
             * inside it until 2026-08-05. The dispatch-table instruments arm
             * from their own #if in this file, independently of the phase
             * timer — so a build with DC_EMU64_HIST=1 and no -DDC_PERF_PHASE
             * swapped in 64 thunks, paid a clock read on every one of ~2,867
             * commands per frame, and then PRINTED NOTHING, for the whole run.
             * The documented perf build line happens to carry -DDC_PERF_PHASE,
             * which is the only reason this never bit anyone.
             *
             * Read `tot=` against `[PHASE] draw=` minus `[PERF] gx=` for the
             * same window before believing any bucket — that self-check needs
             * -DDC_PERF_PHASE, but the histogram itself does not. */
#if defined(DC_EMU64_HIST) && DC_EMU64_HIST > 0
            dc_emu64_hist_report();
#endif
#if defined(DC_EMU64_SHADOW_LOOP) && DC_EMU64_SHADOW_LOOP > 0
            dc_emu64_shadow_report();
#endif
#if defined(DC_EMU64_CULL) && DC_EMU64_CULL > 0
            dc_emu64_cull_report();
#endif
#ifdef DC_PERF_GXAPI
            DC_LOGE("[GXAPI] pos=%u clr=%u tc=%u nrm=%u begin=%u dirty=%u "
                    "posms=%.2f\n",
                    (unsigned)(s_api_pos / 30u), (unsigned)(s_api_clr / 30u),
                    (unsigned)(s_api_tc / 30u), (unsigned)(s_api_nrm / 30u),
                    (unsigned)(s_api_begin / 30u),
                    (unsigned)(s_api_dirty / 30u),
                    (double)s_api_vtx_us / 1000.0 / 30.0);
            s_api_pos = s_api_clr = s_api_tc = s_api_nrm = 0;
            s_api_begin = s_api_dirty = 0; s_api_vtx_us = 0;
#endif
#if defined(DC_PERF_GXSPLIT) && (DC_PERF_GXSPLIT) > 0
            /* THE LINE THIS BUILD EXISTS FOR. All figures are ms per PRESENTED
             * frame (the /30 window), so they share a denominator with [PHASE]
             * and can be compared directly — unlike [EMU64H], which is per
             * LOGIC TICK and must be doubled (measurement rule 9).
             *
             * HOW TO READ IT:
             *   gxpos + gxbegin + gxend + gxstate  = OURS, dc_gx.c, editable
             *                                        without a trampoline
             *   gxgap                              = emu64's vertex loop:
             *                                        index decode, set_position,
             *                                        and the three cheap setters
             *   cull + xform                       = already attributed [PHASE],
             *                                        SUBTRACTED out of all five
             *                                        buckets above
             *
             * THE LEDGER, and it must close before any of this is acted on:
             *   [EMU64H] TRIN_INDEPEND x2  ~=  gxpos + gxgap + gxbegin + gxend
             *                                  + gxstate + cull + xform
             * A residual means a path is unbracketed, not that a bucket is
             * interesting. resid= prints it against [PHASE] draw= so the reader
             * does not have to do the arithmetic to find that out.
             *
             * probe= is the instrument's own cost — 2 TMU2 reads x posn, at
             * ~80 ns each — and it lands INSIDE gxpos. Subtract it before
             * quoting gxpos; do not subtract it from the total, because the
             * per-batch brackets add their own smaller share. */
            {
                double w = 30.0 * 1e6;   /* ns -> ms, over 30 presented frames */
                double p_pos = (double)s_gxs_pos / w;
                double p_gap = (double)s_gxs_gap / w;
                double p_beg = (double)s_gxs_begin / w;
                double p_end = (double)s_gxs_end / w;
                double p_st  = (double)s_gxs_state / w;
                double probe = (double)s_gxs_posn * 2.0 * 80.0 / w;
                DC_LOGE("[GXSPLIT] gxpos=%.2f gxgap=%.2f gxbegin=%.2f "
                        "gxend=%.2f gxstate=%.2f | ours=%.2f emu=%.2f | "
                        "posn=%u probe=%.2f drops=%u\n",
                        p_pos, p_gap, p_beg, p_end, p_st,
                        p_pos + p_beg + p_end + p_st - probe, p_gap,
                        (unsigned)(s_gxs_posn / 30u), probe,
                        (unsigned)s_gxs_drops);
                s_gxs_pos = s_gxs_gap = s_gxs_begin = 0;
                s_gxs_end = s_gxs_state = 0;
                s_gxs_posn = s_gxs_drops = 0;
            }
#endif
            dc_pvr_report();
            fps_start = now;
            fps_count = 0;
            logic_snap = s_logic_tick_count;
        }
    }

#if (defined(DC_FB_PROBE) && DC_FB_PROBE > 0) || \
    (defined(DC_ARENA_PROBE) && DC_ARENA_PROBE > 0) || \
    (defined(DC_ASSET_CENSUS) && DC_ASSET_CENSUS)
    /* MEASURED 2026-08-02: this used to gate on `pc_frame_counter % N`, and
     * that is wrong here. The frameskip path above returns early — after
     * incrementing pc_frame_counter — so this point is reached only on
     * *presented* frames, at a counter value that skips arbitrarily. A run
     * that presented 1,769 frames fired the probe three times, all in the
     * first two seconds, and then never again. Count presented frames locally
     * instead; the probes are meant to sample the rendered stream. */
    {
        static unsigned int probe_tick = 0;

#if defined(DC_FB_PROBE) && DC_FB_PROBE > 0
        /* Guest-side screenshot. Placed AFTER the present and BEFORE the next
         * frame opens, so the front buffer is the finished frame and not a
         * half-built one. */
        if ((probe_tick % (DC_FB_PROBE)) == 0)
            dc_pvr_fb_probe();
#endif
#if defined(DC_ARENA_PROBE) && DC_ARENA_PROBE > 0
        /* Same placement rationale: between frames, so the 1 KB-granule scan
         * of the whole arena never lands in the middle of a scene build. It
         * reads DC_MAIN_MEMORY_SIZE bytes, so keep the interval coarse
         * (60 = about once a second at the measured 29.3 FPS). */
        if ((probe_tick % (DC_ARENA_PROBE)) == 0)
            dc_arena_probe();
#endif
#if defined(DC_ASSET_CENSUS) && DC_ASSET_CENSUS
        /* Bounded burst per report, so the serial console is never the thing
         * that decides how much of the working set gets recorded. */
        if ((probe_tick % 30u) == 0)
            dc_asset_census_report();
#endif
        probe_tick++;
    }
#endif

#if defined(DC_EMU64_HIST) && DC_EMU64_HIST > 0
    /* Arm LAST, so the 512-byte table swap and everything above it are outside
     * the measured window. Counts PRESENTED frames locally for the reason in
     * the probe block above — never gate a periodic probe on pc_frame_counter
     * (kb/traps.md). */
    {
        static unsigned int hist_tick = 0;
        dc_emu64_hist_frame_open(hist_tick++);
    }
#endif

#if defined(DC_EMU64_SHADOW_LOOP) && DC_EMU64_SHADOW_LOOP > 0
    /* G2. Same placement rule as G1: arm LAST so the table swap is outside the
     * measured window, and count PRESENTED frames in a local — never gate a
     * periodic thing on pc_frame_counter (kb/traps.md: the retrace handler
     * returns early on frameskipped ticks AFTER incrementing it, so a
     * `% N == 0` test only ever samples presented frames at values that jump
     * by the skip factor). With DC_EMU64_SHADOW_LOOP=N>1 this tick is what
     * makes the matched-frame A/B alternate. */
    {
        static unsigned int shadow_tick = 0;
        dc_emu64_shadow_frame_open(shadow_tick++);
    }
#endif

#if defined(DC_EMU64_CULL) && DC_EMU64_CULL > 0
    /* G3, same placement and the same local tick counter as G1/G2 above. It
     * costs two compares, not a 512-byte table swap, so its position in this
     * block is about matching THEIR window rather than about its own cost. */
    {
        static unsigned int cull_tick = 0;
        dc_emu64_cull_frame_open(cull_tick++);
    }
#endif

    frame_start_us = dc_time_us();

    pc_gx_begin_frame();
    retrace_count++;
    pc_frame_counter++;
    if (vi_post_callback) vi_post_callback(retrace_count);
#ifdef DC_PERF_PHASE
    {
        u64 ph_out = dc_time_us();
        s_ph_vi_us += ph_out - s_ph_last_exit;
        s_ph_last_exit = ph_out;
    }
#endif
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
