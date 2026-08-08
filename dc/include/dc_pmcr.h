/* dc_pmcr.h — SH7750 performance-counter profiler (P1). API + no-op macros.
 *
 * WHY THIS EXISTS. Every FPS number this project has ever produced came out of
 * Flycast, which models NO instruction cache, against a 2.88 MB `.text` on an
 * 8 KB DIRECT-MAPPED icache. A human has reported the console is materially
 * slower than the emulator and nobody has measured why. No amount of Flycast
 * work can answer that (kb/RESUME.md §0h); the SH-4's own PMCR counters can,
 * and they are the only instrument that can.
 *
 * DESIGN, in one paragraph. KOS owns PRFC0 (it is `perf_cntr_timer_ns`'s
 * clock), so this uses **PRFC1 only** — the split Xash3D DC uses, for the same
 * reason. The SH-4 has exactly TWO counters and one is already spoken for, so
 * only ONE event can be counted at a time: this rotates PRFC1 through a table
 * of events, one event per report window, and prints a running table. A full
 * rotation is DC_PMCR_NMODE windows.
 *
 * WHAT IT BRACKETS. The same decomposition [PHASE] already uses, so the two
 * lines can be read side by side:
 *   draw  = counter delta over a PRESENTED logic tick's game work
 *   skip  = ditto over a frameskipped tick
 *   vi    = inside VIWaitForRetrace: end-of-frame, swap, pace, probes
 *   audio = inside dc_audio_pump()      (a sub-bucket of draw/skip)
 *   xform = inside dc_gx_backend_submit (a sub-bucket of draw)
 *
 * DENOMINATOR (measurement rule 9 — state it, from the code, always):
 * everything [PMCR] prints is **per PRESENTED frame**, like [PHASE] and
 * [GXSPLIT] and unlike [EMU64H]. `draw`/`skip` are sums over the whole window
 * divided by the window's presented-frame count.
 *
 * OFF BY DEFAULT. With DC_PMCR unset every macro below is `((void)0)` and
 * dc_pmcr.c compiles to nothing, so a shipping build is byte-identical.
 */
#ifndef DC_PMCR_H
#define DC_PMCR_H

#if defined(DC_PMCR) && DC_PMCR > 0

/* Bracket slots. Kept as small integers rather than separate entry points so
 * the call sites are one instruction's worth of argument and the accumulator
 * table can be indexed instead of branched on. */
enum {
    DC_PMCR_SLOT_AUDIO = 0,   /* dc_audio_pump()                              */
    DC_PMCR_SLOT_XFORM = 1,   /* dc_gx_backend_submit(): SH-4 T&L + TA submit */
    DC_PMCR_SLOT_COUNT = 2
};

void dc_pmcr_init(void);
void dc_pmcr_tick_enter(int was_frameskip);
void dc_pmcr_tick_exit(void);
void dc_pmcr_enter(int slot);
void dc_pmcr_exit(int slot);
void dc_pmcr_report(unsigned int presented_frames, double window_us);
void dc_pmcr_hud(void);

#define DC_PMCR_INIT()          dc_pmcr_init()
#define DC_PMCR_TICK_ENTER(s)   dc_pmcr_tick_enter(s)
#define DC_PMCR_TICK_EXIT()     dc_pmcr_tick_exit()
#define DC_PMCR_ENTER(s)        dc_pmcr_enter(s)
#define DC_PMCR_EXIT(s)         dc_pmcr_exit(s)
#define DC_PMCR_REPORT(n, us)   dc_pmcr_report((n), (us))
#define DC_PMCR_HUD_DRAW()      dc_pmcr_hud()

#else  /* !DC_PMCR */

#define DC_PMCR_INIT()          ((void)0)
#define DC_PMCR_TICK_ENTER(s)   ((void)0)
#define DC_PMCR_TICK_EXIT()     ((void)0)
#define DC_PMCR_ENTER(s)        ((void)0)
#define DC_PMCR_EXIT(s)         ((void)0)
#define DC_PMCR_REPORT(n, us)   ((void)0)
#define DC_PMCR_HUD_DRAW()      ((void)0)

#endif /* DC_PMCR */

#endif /* DC_PMCR_H */
