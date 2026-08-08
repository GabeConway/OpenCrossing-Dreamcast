/* dc_pmcr.c — P1, the SH7750 performance-counter profiler.
 *
 * THE QUESTION IT EXISTS TO ANSWER, stated so a later reader cannot mistake
 * it: the console is materially slower than Flycast (human report,
 * kb/RESUME.md §0h) and nobody knows by how much or why. The leading
 * hypothesis is the instruction cache — 8 KB, DIRECT-MAPPED, against a
 * 2.88 MB `.text` — and Flycast models no instruction cache at all, so the
 * emulator structurally cannot answer it. The SH-4's own PMCR counters can.
 *
 * ⚠️ THIS IS AN INSTRUMENT, NOT AN OPTIMIZATION. Nothing here makes the game
 * faster. It exists so that steps 2-5 of the hardware-perf plan are
 * falsifiable on the machine they are about, instead of on the one that has
 * a perfect icache.
 *
 * =========================================================================
 * WHY PRFC1 AND WHY ONLY ONE EVENT AT A TIME
 * =========================================================================
 * The SH7750 has two 48-bit counters. KOS drives PRFC0 as the clock behind
 * `perf_cntr_timer_ns()` (perfctr.c:perf_cntr_timer_enable), and
 * `perf_cntr_start(PRFC0, ...)` would silently steal it. Xash3D DC's profiler
 * hits the same wall and answers it the same way — *"Use PRFC1 to avoid
 * interfering with KOS internal timing (PRFC0)"*. So exactly ONE event is
 * countable, and the design consequence is unavoidable: the event ROTATES.
 * Each report window counts one event; a full picture is DC_PMCR_NMODE
 * windows (~12 s at 20 FPS with the default 30-frame window).
 *
 * ⚠️ THE ROTATION IS A DENOMINATOR TRAP AND IT IS HANDLED, NOT IGNORED. Two
 * different modes are measured over two different windows, i.e. over two
 * different stretches of gameplay. Ratios BETWEEN modes (istall / cyc) are
 * therefore only as good as the scene's stationarity: take them from a static
 * camera, or from a long run where the windows interleave. Ratios WITHIN one
 * mode (audio vs draw) share a window and are exact. `wall=` is printed on
 * every line for exactly this reason — it is the one figure common to all
 * windows and it says whether two windows saw the same workload.
 *
 * =========================================================================
 * DENOMINATOR (measurement rule 9)
 * =========================================================================
 * Everything [PMCR] prints is **per PRESENTED frame**. It is reported from
 * dc_vi.c's 30-presented-frame block, the same one that prints [PERF] and
 * [PHASE], and every accumulator is divided by the frames that block counted.
 * Do NOT double it. ([EMU64H] is the only per-logic-tick instrument here.)
 *
 * =========================================================================
 * WHAT THE COUNTERS DO AND DO NOT SEE
 * =========================================================================
 *  - They count on the CPU, globally: interrupts and other threads land in
 *    whichever bracket is open. That is a feature for `draw`/`vi` and a
 *    caveat for `audio`.
 *  - ⚠️ They stop while the CPU sleeps. `vid_waitvbl()` in dc_pace_frame()
 *    can sleep, so on a frame-limited build `cyc x 5 ns` is LESS than `wall`
 *    by exactly the idle time. That difference is a result, not an error: if
 *    cyc*5ns ≈ wall the CPU never idled and the frame is compute-bound.
 *  - PMCR_ELAPSED_TIME_MODE counts 1 per cycle at PMCR_COUNT_CPU_CYCLES,
 *    i.e. 5 ns per count at 200 MHz (KOS perfctr.c: NS_PER_CYCLE 5).
 *
 * =========================================================================
 * COST
 * =========================================================================
 * One `perf_cntr_count()` is three uncached P4 register reads. The brackets
 * are: 2 per logic tick, 2 per dc_audio_pump(), and 2 per SUBMITTED BATCH —
 * that last one is the expensive one (hundreds of batches a frame). The
 * probe therefore MEASURES ITS OWN COST: dc_pmcr_init() times 1,000 reads
 * and every report prints `rd=` (reads in the window) and `rdns=` (ns per
 * read), so `rd x rdns` is the instrument's own contribution and can be
 * subtracted. kb/traps.md's rule — a probe that cannot price itself is not
 * an instrument — is the reason this is not left as a comment.
 *
 * KILL SWITCH: DC_PMCR unset (the default) removes every call site via the
 * macros in dc_pmcr.h and compiles this file to nothing.
 */
#include "dc_platform.h"
#include "dc_pmcr.h"

#if defined(DC_PMCR) && DC_PMCR > 0 && !defined(DC_HOST_STUB)

#include <dc/perfctr.h>
#include <dc/biosfont.h>

/* Windows spent on each event before rotating to the next. 1 window is 30
 * presented frames (dc_vi.c's [PERF] cadence). */
#ifndef DC_PMCR_DWELL
#define DC_PMCR_DWELL 1
#endif

/* ==========================================================================
 * The event table
 * ==========================================================================
 * Chosen to answer three questions and no more:
 *
 *   1. "How much of a hardware frame is cache stall?"
 *      -> istall / dstall, both CYCLE modes, directly comparable to cyc.
 *         PIPELINE_FREEZE_BY_ICACHE_MISS is the number the icache hypothesis
 *         lives or dies on: it is cycles the pipeline was frozen, not misses.
 *   2. "Is it the icache or the operand cache?"
 *      -> imiss / omiss (counts) and ifill / ofill (cycles of fill traffic).
 *         A miss COUNT without a stall figure is not a cost; both are here so
 *         neither has to be inferred from the other.
 *   3. "Is the CPU even issuing?"
 *      -> iissue, so cycles-per-instruction can be formed against cyc.
 *
 * `is_cyc` drives the ms/% column: only cycle modes convert. A quantity mode
 * printed as milliseconds would be a fabricated number. */
typedef struct {
    perf_cntr_event_t ev;
    const char*       name;
    unsigned char     is_cyc;
} dc_pmcr_mode_t;

static const dc_pmcr_mode_t s_modes[] = {
    { PMCR_ELAPSED_TIME_MODE,                    "cyc",    1 },
    { PMCR_PIPELINE_FREEZE_BY_ICACHE_MISS_MODE,  "istall", 1 },
    { PMCR_PIPELINE_FREEZE_BY_DCACHE_MISS_MODE,  "dstall", 1 },
    { PMCR_INSTRUCTION_CACHE_MISS_MODE,          "imiss",  0 },
    { PMCR_OPERAND_CACHE_MISS_MODE,              "omiss",  0 },
    { PMCR_INSTRUCTION_CACHE_FILL_MODE,          "ifill",  1 },
    { PMCR_OPERAND_CACHE_FILL_MODE,              "ofill",  1 },
    { PMCR_INSTRUCTION_ISSUED_MODE,              "iissue", 0 }
};
#define DC_PMCR_NMODE ((int)(sizeof(s_modes) / sizeof(s_modes[0])))

/* Per-mode last-known result, per presented frame. This is what the HUD draws
 * and what makes a rotating instrument readable: the table fills in over one
 * rotation and then keeps refreshing in place, so a photograph of the screen
 * carries the whole picture rather than one event. */
typedef struct {
    unsigned int draw, skip, vi, audio, xform, hud;
    double       wall_ms;      /* the window's wall frame time, for stationarity */
    unsigned char valid;
} dc_pmcr_row_t;

/* ==========================================================================
 * ⚠️ THE CONSOLE IS NOT FREE ON HARDWARE, AND THIS INSTRUMENT IS FOR HARDWARE
 * ==========================================================================
 * KOS busy-waits on the SCIF TX FIFO whether or not a cable is attached, so
 * at the default 57,600 baud every logged byte is ~174 us of DEAD FRAME
 * (kb/traps.md: 86,357 B of per-asset logging = 15.0 s of boot). A [PMCR]
 * line is ~140 B — ~24 ms per window — and it is emitted right next to
 * [PERF], [PHASE], [EMU64], [EMU64C] and six [DC/*] lines, which together are
 * far more. A burn that reports over the console MEASURES THE CONSOLE.
 *
 * So: when the HUD is compiled in, the HUD is the channel and the console
 * line is suppressed by default. -DDC_PMCR_LOG=1 forces it back on (an
 * emulator run wants both), and without the HUD the console is the only
 * channel there is, so it stays on. */
static int dc_pmcr_console_on(void) {
#if defined(DC_PMCR_LOG)
    return (DC_PMCR_LOG) != 0;
#elif defined(DC_PMCR_HUD) && DC_PMCR_HUD > 0
    return 0;
#else
    return 1;
#endif
}

static dc_pmcr_row_t s_row[DC_PMCR_NMODE];

static int          s_mode = 0;
static int          s_dwell = 0;
static int          s_inited = 0;
static int          s_dead_said = 0;
static double       s_read_ns = 0.0;

static u64 s_last;              /* counter at the previous tick boundary   */
static int s_have_last = 0;
static u64 s_acc_draw, s_acc_skip, s_acc_vi, s_acc_hud;
static u64 s_acc_slot[DC_PMCR_SLOT_COUNT];
static u64 s_enter[DC_PMCR_SLOT_COUNT];
static int s_open[DC_PMCR_SLOT_COUNT];
static unsigned int s_reads;
static unsigned int s_bad;      /* deltas that went backwards — see below  */

static u64 dc_pmcr_rd(void) {
    s_reads++;
    return perf_cntr_count(PRFC1);
}

/* A delta must never be negative. It can be, in exactly one situation: the
 * mode rotation calls perf_cntr_start(), which CLEARS the counter, so any
 * bracket straddling that instant would wrap. The rotation is placed inside
 * the report, i.e. between a tick's exit re-stamp and the next tick's entry,
 * so no bracket can straddle it — and `bad=` is the tripwire that says so
 * rather than a comment claiming it. A non-zero `bad=` invalidates the
 * window, it does not merely round it. */
static u64 dc_pmcr_delta(u64 now, u64 then) {
    if (now < then) { s_bad++; return 0; }
    return now - then;
}

void dc_pmcr_init(void) {
    volatile u64 sink = 0;
    u64 t0, t1;
    int i;

    if (s_inited) return;
    s_inited = 1;

    perf_cntr_start(PRFC1, s_modes[0].ev, PMCR_COUNT_CPU_CYCLES);

    /* Price the instrument. 1,000 reads timed in microseconds gives the cost
     * of one read in nanoseconds directly — no scaling, no assumed clock.
     * `sink` is volatile so the loop cannot be optimized away at -O3. */
    t0 = dc_time_us();
    for (i = 0; i < 1000; i++) sink += perf_cntr_count(PRFC1);
    t1 = dc_time_us();
    s_read_ns = (double)(t1 - t0);

    /* Discard the calibration's own reads: they are boot, not a window. */
    s_reads = 0;

    DC_LOGE("[PMCR] armed: PRFC1, %d events, dwell=%d window(s), "
            "read=%.0fns (PRFC0 left to KOS)\n",
            DC_PMCR_NMODE, (int)(DC_PMCR_DWELL), s_read_ns);

    for (i = 0; i < DC_PMCR_NMODE; i++) s_row[i].valid = 0;
}

/* FIRST statement of VIWaitForRetrace, exactly like [PHASE]'s block: the
 * interval from the previous tick's exit to here is that tick's game work,
 * and g_pc_frameskip_active still holds which kind of tick it was. */
void dc_pmcr_tick_enter(int was_frameskip) {
    u64 now;

    if (!s_inited) dc_pmcr_init();
    now = dc_pmcr_rd();
    if (s_have_last) {
        u64 d = dc_pmcr_delta(now, s_last);
        if (was_frameskip) s_acc_skip += d; else s_acc_draw += d;
    }
    s_last = now;
    s_have_last = 1;
}

/* LAST statement of VIWaitForRetrace: everything since tick_enter is this
 * function's own work — end-of-frame, swap, pace, probes, and the HUD. */
void dc_pmcr_tick_exit(void) {
    u64 now = dc_pmcr_rd();
    if (s_have_last) s_acc_vi += dc_pmcr_delta(now, s_last);
    s_last = now;
    s_have_last = 1;
}

void dc_pmcr_enter(int slot) {
    if (!s_inited) return;
    s_enter[slot] = dc_pmcr_rd();
    s_open[slot] = 1;
}

void dc_pmcr_exit(int slot) {
    if (!s_inited || !s_open[slot]) return;
    s_acc_slot[slot] += dc_pmcr_delta(dc_pmcr_rd(), s_enter[slot]);
    s_open[slot] = 0;
}

/* Called from dc_vi.c's 30-presented-frame block, alongside [PERF]/[PHASE].
 * `window_us` is that block's own wall measurement, so [PMCR] never has to
 * invent a clock and the two lines are guaranteed to describe one window. */
void dc_pmcr_report(unsigned int presented_frames, double window_us) {
    const dc_pmcr_mode_t* m = &s_modes[s_mode];
    dc_pmcr_row_t* r = &s_row[s_mode];
    double n, wall_ms, total, ms, pct;

    if (!s_inited || presented_frames == 0) return;

    n = (double)presented_frames;
    wall_ms = window_us / 1000.0 / n;

    /* u32 is safe: the largest per-frame figure is elapsed cycles at ~5 ns,
     * i.e. ~10 M for a 50 ms frame. Nothing here approaches 4 G, and %u keeps
     * the format string inside what newlib-nano's printf is known to do. */
    r->draw  = (unsigned int)((double)s_acc_draw     / n);
    r->skip  = (unsigned int)((double)s_acc_skip     / n);
    r->vi    = (unsigned int)((double)s_acc_vi       / n);
    r->audio = (unsigned int)((double)s_acc_slot[DC_PMCR_SLOT_AUDIO] / n);
    r->xform = (unsigned int)((double)s_acc_slot[DC_PMCR_SLOT_XFORM] / n);
    r->hud   = (unsigned int)((double)s_acc_hud / n);
    r->wall_ms = wall_ms;
    r->valid = 1;

    total = (double)r->draw + (double)r->skip + (double)r->vi;
    ms  = m->is_cyc ? total * 5.0 / 1000000.0 : 0.0;   /* 5 ns per count */
    pct = (m->is_cyc && wall_ms > 0.0) ? ms / wall_ms * 100.0 : 0.0;

    /* ⚠️ ZERO IS AMBIGUOUS AND THE INSTRUMENT MUST SAY SO ITSELF. Flycast
     * implements no performance counters: a full run there prints 0 for every
     * event INCLUDING elapsed cycles, which is indistinguishable from "the
     * build never armed" to anyone reading the log later. Diagnose it once,
     * on the mode that cannot legitimately be zero. */
    if (m->ev == PMCR_ELAPSED_TIME_MODE && total == 0.0 && !s_dead_said) {
        s_dead_said = 1;
        DC_LOGE("[PMCR] ⚠️ elapsed-cycle count is ZERO over a whole window. "
                "PRFC1 is not counting: that is EXPECTED under Flycast (it "
                "models no PMCR) and means this run cannot answer anything "
                "about the hardware. Burn it.\n");
    }

    /* ONE line, stable field order, greppable. `kind=` is what stops a
     * quantity being read as a duration three documents later. `hud=` is
     * INSIDE `vi=`, not beside it — it is the instrument's own on-screen
     * report, and it is broken out so a HUD build's `vi=` can be corrected
     * rather than quietly believed. */
    if (dc_pmcr_console_on()) {
        DC_LOGE("[PMCR] mode=%s ev=0x%02x kind=%s | draw=%u skip=%u vi=%u "
                "audio=%u xform=%u hud=%u | ms=%.2f wall=%.2f pct=%.1f "
                "rd=%u rdns=%.0f bad=%u\n",
                m->name, (unsigned int)m->ev, m->is_cyc ? "cyc" : "num",
                r->draw, r->skip, r->vi, r->audio, r->xform, r->hud,
                ms, wall_ms, pct, s_reads / presented_frames, s_read_ns,
                s_bad);
    }

    s_acc_draw = s_acc_skip = s_acc_vi = s_acc_hud = 0;
    s_acc_slot[DC_PMCR_SLOT_AUDIO] = 0;
    s_acc_slot[DC_PMCR_SLOT_XFORM] = 0;
    s_reads = 0;

    /* Rotate. perf_cntr_start() clears PRFC1, so the tick accumulator's
     * anchor must be re-taken AFTER it — dc_pmcr_tick_exit() runs later in
     * the same function and does exactly that, but this is not left to luck:
     * s_have_last is dropped so the very next boundary re-anchors even if the
     * call order ever changes. */
    if (++s_dwell >= (DC_PMCR_DWELL)) {
        s_dwell = 0;
        s_mode = (s_mode + 1) % DC_PMCR_NMODE;
        perf_cntr_start(PRFC1, s_modes[s_mode].ev, PMCR_COUNT_CPU_CYCLES);
        s_have_last = 0;
        s_open[DC_PMCR_SLOT_AUDIO] = s_open[DC_PMCR_SLOT_XFORM] = 0;
    }
}

/* ==========================================================================
 * The on-screen table (-DDC_PMCR_HUD)
 * ==========================================================================
 * ⚠️ THE BURN CANNOT USE THE CONSOLE. DC_SCIF_FAST=1 loses the console on real
 * hardware (a coder's cable will not sync at 1.5 Mbps), and at KOS's default
 * 57,600 baud SCIF is BOOT TIME whether a cable is attached or not
 * (kb/traps.md) — so a burn that reports over the console changes the thing it
 * is measuring. The answer has to be on the TV.
 *
 * HOW IT DRAWS. The game owns the PVR, so there is no free scene to submit a
 * quad into; this writes straight into the scanned-out surface instead. The
 * address is READ FROM THE HARDWARE (FB_R_SOF1), never assumed: `vram_s` is
 * the base of VRAM and is NOT the displayed surface once pvr_init() has run —
 * that cost a debug cycle already and is written up at dc_pvr.c:3842. The
 * measured geometry is a flat 640x480 RGB565 buffer at 0xA5000000 + SOF1.
 *
 * WHY ONE BUFFER AND NOT BOTH. KOS page-flips between two surfaces, so the
 * obvious worry is that painting one of them makes the text flicker at half
 * rate. It does not: FB_R_SOF1 is the SCANOUT register, so it always names
 * the surface being displayed at this instant, and each surface is repainted
 * while it is the one on screen. Painting both would double the cost to
 * protect against a flip-timing case that reading the register already rules
 * out.
 *
 * COST, and it is real: ~10 rows x <=30 chars x 12x24 px of opaque 16-bit
 * stores to UNCACHED VRAM — ~75,000 stores a frame. It is measured, not
 * estimated: the HUD brackets itself and reports `hud=` on the [PMCR] line.
 * `hud=` is INSIDE `vi=`; subtract it before quoting `vi=` from a HUD build.
 * A HUD build is a hardware-readout build, not a perf build.
 */
#if defined(DC_PMCR_HUD) && DC_PMCR_HUD > 0

#define DC_PVR_REG(off) (*(volatile unsigned int*)(0xA05F8000u + (off)))
#define DC_FB_R_SOF1    DC_PVR_REG(0x050)
#define DC_VRAM32       0xA5000000u
#define DC_VRAM_BYTES   (8u * 1024u * 1024u)

#define HUD_X  8
#define HUD_Y  8
#define HUD_FG 0xFFFFu   /* RGB565 white — bfont_draw_str_ex at bpp 16 takes */
#define HUD_BG 0x0000u   /* the colour verbatim, so these are NOT ARGB8888.  */

static unsigned short* s_hud_fb;

static void dc_pmcr_hud_line(int row, const char* s) {
    bfont_draw_str_ex(s_hud_fb +
                          (size_t)(HUD_Y + row * BFONT_HEIGHT) *
                              DC_SCREEN_WIDTH + HUD_X,
                      DC_SCREEN_WIDTH, HUD_FG, HUD_BG, 16, true, s);
}

void dc_pmcr_hud(void) {
    /* 40 chars of text at BFONT_THIN_WIDTH is 480 px; the buffer is 640 wide,
     * so a row can never run off the right edge and into the next line. */
    char line[40];
    unsigned int off;
    u64 t0;
    int i, row = 0;

    if (!s_inited) return;

    /* ASK THE HARDWARE, EVERY FRAME. The pair of surfaces is KOS's business
     * and the scanout register is the only thing that knows which one is live
     * right now. Out of range is reported by drawing nothing — never
     * dereferenced (dc_pvr.c:3614's rule). */
    off = DC_FB_R_SOF1 & ~3u;
    if (off + (unsigned int)(DC_SCREEN_WIDTH * DC_SCREEN_HEIGHT * 2) >
        DC_VRAM_BYTES)
        return;
    s_hud_fb = (unsigned short*)(DC_VRAM32 + off);

    t0 = dc_pmcr_rd();

    snprintf(line, sizeof(line), "PMCR /frame   now=%s", s_modes[s_mode].name);
    dc_pmcr_hud_line(row++, line);

    for (i = 0; i < DC_PMCR_NMODE; i++) {
        const dc_pmcr_mode_t* m = &s_modes[i];
        const dc_pmcr_row_t* r = &s_row[i];
        if (!r->valid) {
            snprintf(line, sizeof(line), "%-6s        --", m->name);
        } else if (m->is_cyc) {
            /* ms, then the same figure split: d = the game's own tick,
             * a = audio inside it, x = T&L + TA submit inside it. */
            double tot = (double)r->draw + (double)r->skip + (double)r->vi;
            snprintf(line, sizeof(line), "%-6s%6.1f d%5.1f a%4.1f x%5.1f",
                     m->name, tot * 5.0 / 1000000.0,
                     (double)r->draw  * 5.0 / 1000000.0,
                     (double)r->audio * 5.0 / 1000000.0,
                     (double)r->xform * 5.0 / 1000000.0);
        } else {
            /* Counts, in thousands — a per-frame miss count runs to six
             * digits and the column has to stay narrow. */
            snprintf(line, sizeof(line), "%-6s%6uk d%5uk a%4uk x%5uk",
                     m->name,
                     (r->draw + r->skip + r->vi) / 1000u,
                     r->draw / 1000u, r->audio / 1000u, r->xform / 1000u);
        }
        dc_pmcr_hud_line(row++, line);
    }

    snprintf(line, sizeof(line), "wall%6.1fms hud%5.1f bad=%u",
             s_row[s_mode].wall_ms,
             (double)s_row[s_mode].hud * 5.0 / 1000000.0, s_bad);
    dc_pmcr_hud_line(row++, line);

    /* The HUD's own cost, in whatever the current event counts. Accumulated
     * separately AND left inside `vi` rather than subtracted here, so the
     * buckets still sum to the frame and the correction is the reader's to
     * make, visibly. */
    s_acc_hud += dc_pmcr_delta(dc_pmcr_rd(), t0);
}

#else   /* !DC_PMCR_HUD */
void dc_pmcr_hud(void) { }
#endif  /* DC_PMCR_HUD */

#endif  /* DC_PMCR */
