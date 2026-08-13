/* dc_profdump.h — P2, the gprof-over-serial dump. API + no-op macros.
 *
 * WHY THIS EXISTS. P1 (dc_pmcr.c) answers "WHAT is slow on hardware" — icache
 * stalls, operand-cache fills, issue rate — but it cannot say WHERE. It
 * brackets five hand-placed regions and that is all it will ever see. This is
 * the other half: a gprof FLAT PROFILE, per symbol, produced by the SAME image
 * on a burned CD-R and in Flycast, so the two can be diffed function by
 * function. That diff is the project's #1 open question (kb/RESUME.md §0h).
 *
 * THE ONE IDEA. KOS's libgprof histogram sampler (`histogram_callback`,
 * addons/libgprof/gmon.c) is a SCHEDULER-DRIVEN PC SAMPLER: it reads
 * `thd_current`'s saved PC out of `thd_poll` and does NOT depend on `mcount`.
 * So NO translation unit is compiled `-pg` — `-pg` goes on the LINK LINE ONLY.
 * Every optimized object stays byte-identical: no trapa at function entry, no
 * -fno-omit-frame-pointer, no -fno-inline. We lose the call graph and do not
 * want it; a flat profile is what a Flycast-vs-hardware diff reads.
 *
 * THE ONE PROBLEM THIS FILE SOLVES. gmon.c writes gmon.out through `fopen`,
 * and its default path is `/pc/gmon.out` — the dcload host filesystem, which
 * DOES NOT EXIST ON A BURNED CD-R. The console's only channel off the machine
 * is SCIF, which the harness already captures. So this file is a write-only
 * KOS VFS that STORES NOTHING: every byte handed to it is RLE'd, base64'd and
 * streamed straight out of dbgio as framed console lines, and a host script
 * (tools/dcprof/decode_gmon.py) recovers gmon.out from console.log.
 *
 * ⚠️ IT IS ALSO WHERE `monstartup()` IS CALLED FROM, AND THAT IS NOT OPTIONAL.
 * KOS calls `gprof_init()` from arch_main BEFORE constructors and before
 * main() (kernel/arch/dreamcast/kernel/init.c:309). libgprof's own
 * `gprof_init()` (gmon.c:549) would run `monstartup()` over the whole .text at
 * that moment — before any code of ours could mount /prof — and its opening
 * `fopen(path,"w")` would fail, latching GMON_PROF_ERROR for the whole run.
 * dc_profdump.c therefore defines a STRONG `gprof_init()` of its own that
 * mounts the sink first and only then calls monstartup(). Our objects precede
 * `-lgprof` on the link line and the link already carries
 * `-Wl,--allow-multiple-definition` (dc/Makefile, the $(TARGET) recipe), so
 * ours is the definition that wins — the same mechanism dc_misc.c's `printf`
 * override relies on.
 *
 * DENOMINATOR (measurement rule 9 — state it, from the code, always): the
 * histogram is in SAMPLES, one per reschedule that found the profiled thread
 * running, at `thd_get_hz()` nominal. It is NOT per frame and NOT per logic
 * tick. gprof turns it into seconds using the profrate written in the header.
 *
 * OFF BY DEFAULT. With DC_GPROF unset every macro below is `((void)0)`,
 * dc_profdump.c compiles to nothing, no `gprof_init` is defined and the image
 * is byte-identical to a shipping build.
 */
#ifndef DC_PROFDUMP_H
#define DC_PROFDUMP_H

#if defined(DC_GPROF) && DC_GPROF > 0 && !defined(DC_HOST_STUB)

/* Called once per PRESENTED frame from dc_vi.c. One increment and one compare
 * after it fires. At DC_GPROF_DUMP_FRAME it calls _mcleanup() — which writes
 * the histogram through the /prof sink — and then closes the console stream.
 *
 * ⚠️ IT OWNS ITS OWN FRAME COUNTER rather than taking one as an argument. Two
 * reasons: the call site then needs no `static` of its own and so compiles to
 * literally nothing when DC_GPROF is unset; and the counter physically cannot
 * be wired to `pc_frame_counter`, which is the standing trap here — dc_vi.c's
 * frameskip path returns early AFTER incrementing it, so anything gated on it
 * samples at values that jump by the skip factor (kb/traps.md). Called from
 * the presented path only, this counts presented frames by construction. */
void dc_profdump_frame_tick(void);

/* Force the dump now, from anywhere (a pad chord, a crash handler). Safe to
 * call twice: the second call is a no-op. */
void dc_profdump_flush_now(void);

#define DC_GPROF_TICK()    dc_profdump_frame_tick()
#define DC_GPROF_FLUSH()   dc_profdump_flush_now()

#else  /* !DC_GPROF */

#define DC_GPROF_TICK()    ((void)0)
#define DC_GPROF_FLUSH()   ((void)0)

#endif /* DC_GPROF */

#endif /* DC_PROFDUMP_H */
