/* dc_emu64_shadow.cpp — G2: emu64's display-list dispatch LOOP, at -O2, in dc/.
 *
 * =============================================================================
 * WHAT THIS IS, AND WHY IT NEEDED SIGN-OFF
 * =============================================================================
 * The town frame is ~77-80 % emu64 traversal plus game logic (kb/perf-dc.md).
 * That code is in src/, which is closed to editing, and compiler flags on it
 * are banned. This file does not edit src/ and does not recompile src/. It
 * installs trampolines in emu64's dispatch table — the same writable
 * `static dl_func dl_func_tbl[64]` (emu64.c:5702) that dc_emu64_hist.c already
 * swaps timing thunks into — and runs the PER-COMMAND LOOP out of dc/, which
 * has always been built at -O2.
 *
 * That is legal by the letter of CLAUDE.md §1 and the user signed off on it
 * knowing it walks near the spirit: it reimplements one src/ function in
 * optimised dc/ code. The mitigations are the ones that distinguish it from
 * the armhf -O1/-O2 disaster: it is ONE hand-vetted function, not 3,917 TUs;
 * it is off by default; it self-checks at three levels; and it is judged on a
 * screenshot pair, not on counters.
 *
 * Kill switch: DC_EMU64_SHADOW_LOOP=0 (the default) compiles this file to
 * nothing, and the image is then byte-identical to one built without it.
 *
 * =============================================================================
 * THE MECHANISM: A TAKEOVER TRAMPOLINE, NOT SYMBOL INTERPOSITION
 * =============================================================================
 * Do NOT try to objcopy --redefine-sym emu64_taskstart_r. Its only caller
 * (emu64.c:5914) is in the same object, so that reference can be resolved by
 * the assembler and the interposition would silently not take.
 *
 * Instead: install a trampoline in all 64 table slots. The -O0 loop runs its
 * own body exactly ONCE — for the first command of the traversal — and calls
 * our trampoline. The trampoline finishes that command by calling the original
 * -O0 handler, and then runs OUR -O2 loop for every remaining command. When we
 * return, the -O0 loop does its command_info increment (correct: that command
 * was its own), its gfx_p++ (harmless — nothing reads gfx_p after the loop and
 * emu64_taskstart_r:5773 overwrites it next frame), re-tests !end_dl, and
 * exits.
 *
 * Three properties this buys, all of which a "replace the whole function"
 * design would have to rebuild from scratch:
 *
 *   1. THE setjmp CRASH RECOVERY IS UNTOUCHED. emu64.c:5779-5804 arms a
 *      jmp_buf and, on a fault inside any handler, pops the DL stack and
 *      re-enters the -O0 loop — which re-enters this trampoline. We inherit it
 *      for free.
 *      ⚠️ DO NOT "FIX" THIS WITH A RE-ENTRANCY FLAG. A longjmp out of our loop
 *      would leave the flag stuck set, and every later frame would silently
 *      fall back to -O0 — a permanent perf cliff that looks like a random FPS
 *      dip. Re-entrancy cannot otherwise happen: nothing in src/ re-enters the
 *      dispatch loop (GXCallDisplayList, emu64.c:3501, goes to dc_gx.c).
 *   2. ARM/DISARM IS A 512-BYTE memcpy, so a single run can do a matched-frame
 *      A/B — armed on even ticks, disarmed on odd. That is what kb/RESUME.md
 *      §0b rule 5 asks for, and it is stronger evidence than any two-build
 *      comparison.
 *   3. Nothing in src/ ever writes dl_func_tbl (grep: emu64.c:5702, :5848,
 *      :5850 only), so the install cannot be undone underneath us.
 *
 * =============================================================================
 * ⚠️ THE HEADER'S MEMBER OFFSETS ARE WRONG FOR sh-elf. USE THE HEADER.
 * =============================================================================
 * emu64.hpp:723-871 annotates every member with a hex offset in a comment.
 * Those are the MWERKS/PowerPC offsets and they are WRONG here from `gfx` on —
 * verified against DWARF in the object we actually ship:
 *
 *     member            header    real (sh-elf)
 *     gfx               0x0048    0x0044
 *     gfx_cmd           0x0050    0x004C
 *     end_dl            0x0058    0x0054
 *     vertices          0x0E1C    0x1018
 *     dl_history        0x2038    0x2234
 *     sizeof(emu64)     ~0x207C   0x2278
 *
 * Two independent causes: `long long` aligns to 4 on sh-elf (so the Gfx union
 * lands at 0x44), and GXTexObj is 88 B in this port against 32 B on GameCube.
 *
 * THE CONSEQUENCE IS THE WHOLE REASON THIS FILE IS C++ AND NOT C: a hand-built
 * offset map or a mirrored struct would be wrong, and wrong in a way that
 * writes into the neighbouring member instead of faulting. We include the real
 * header and let the same compiler compute the same layout. dc/Makefile:790-792
 * already gives every dc/src C++ TU `-O2 -fpermissive -fno-exceptions
 * -fno-rtti`, so no Makefile change is needed to compile it.
 *
 * The static_assert below is the tripwire for any future drift.
 */
#include "dc_platform.h"

#if defined(DC_EMU64_SHADOW_LOOP) && DC_EMU64_SHADOW_LOOP > 0

/* emu64's members are all private (emu64.hpp:722). Access control has no
 * effect on layout, and this file is in dc/, not src/ — we are not editing the
 * class, we are reading the layout it already has. */
#define private   public
#define protected public

/* emu64.hpp:138-200 defines fastcast_float FOUR times WITHOUT `inline`.
 * emu64.c is today the only TU that includes this header (emu64_utility.c and
 * emu64_print.cpp are #include'd INTO it, at emu64.c:92 and :308), so a second
 * including TU is four duplicate definitions at link time. Rename them out of
 * the way — the same trick dc/include/dc_census_vtx.h already uses on
 * OSs16tof32, and the pattern dc_prelude.h §1a documents. */
#define fastcast_float dc_shadow_unused_fastcast_float

#include "libforest/emu64/emu64.hpp"

#undef fastcast_float
#undef protected
#undef private

extern "C" {

/* The opcode-mix counters. These are OURS (dc/src/dc_gx.c:90-95) and dc_vi.c
 * prints them as the [EMU64] line — the only instrument in the tree that says
 * what the command mix is. The shadow loop MUST keep feeding them or that line
 * silently reads as "the game stopped issuing commands". */
extern int pc_emu64_frame_cmds;
extern int pc_emu64_frame_noop_cmds;
extern int pc_emu64_frame_tri_cmds;
extern int pc_emu64_frame_vtx_cmds;
extern int pc_emu64_frame_dl_cmds;

void dc_emu64_shadow_init(void);
void dc_emu64_shadow_frame_open(unsigned int tick);
void dc_emu64_shadow_frame_close(void);
void dc_emu64_shadow_report(void);

} /* extern "C" */

/* ⚠️ sizeof is the cheap half of the tripwire. It catches a header edit or a
 * toolchain bump that moves members. It does NOT catch a change that keeps the
 * size and moves a member inside it — that is what the runtime triple-check in
 * shadow_selfcheck() is for. */
#if defined(__SH4__) || defined(__sh__)
static_assert(sizeof(emu64) == 0x2278,
              "emu64 layout drifted -- the shadow loop's member accesses are "
              "no longer the ones emu64.c compiles. Re-dump with "
              "llvm-dwarfdump on dc/build/obj/src/static/libforest/emu64/"
              "emu64.c.o before touching this number.");
#endif

#define SH_N        64          /* NUM_COMMANDS, emu64.hpp:26                */
#define SH_FIRSTCMD 0xCE        /* G_FIRST_CMD = G_SETTEXEDGEALPHA           */

/* The Itanium pointer-to-member the dispatcher reads: { u32 ptr; s32 adj; }.
 * dc_emu64_hist.c:38-50 records the disassembly that established this — the
 * virtual-case test is (ptr & 1), all 64 adj words are zero, and SH-4
 * instructions are 2-byte aligned, so the virtual path is unreachable and a
 * handler is exactly `void f(emu64 *this)` with `this` in r4. */
typedef struct { void (*fn)(void *self); int adj; } ShPmf;

/* Promoted from local to global by the objcopy step in dc/Makefile. */
extern "C" ShPmf _ZL11dl_func_tbl[SH_N];

/* Named only to verify the table is the one we think it is, before writing to
 * it. Ordinary global text symbols in emu64.c.o. */
extern "C" void _ZN5emu648dl_G_VTXEv(void *self);
extern "C" void _ZN5emu649dl_G_TRI1Ev(void *self);
extern "C" void _ZN5emu6411dl_G_CULLDLEv(void *self);

#define SH_IDX_VTX    51
#define SH_IDX_CULLDL 53
#define SH_IDX_TRI1   55

static ShPmf s_orig[SH_N];
static ShPmf s_tramp_tbl[SH_N];
static int   s_ready;
static int   s_armed;
static int   s_checked;         /* the first-entry data check has run        */
static unsigned int s_cmds;     /* commands taken by the shadow loop         */
static unsigned int s_entries;  /* traversals taken over                     */
static unsigned int s_punts;    /* traversals handed back to -O0             */

/* ---------------------------------------------------------------------------
 * The self-check that actually catches a layout error.
 *
 * sizeof() cannot see a member that moved without changing the total. This
 * can: the -O0 loop wrote gfx_p, gfx and gfx_cmd two instructions before
 * calling us, and they are related by construction. If our offsets are wrong,
 * at least one of these three reads lands somewhere else and the relation
 * breaks. Checked ONCE, on the first trampoline entry, so it costs nothing
 * steady-state.
 *
 * On failure: put the original table back and never re-arm. A wrong offset
 * here is not "the numbers are off", it is a write into a neighbouring member
 * of a 8,824-byte object — exactly the failure dc_emu64_hist.c:236-238 refuses
 * to risk.
 * ------------------------------------------------------------------------- */
static void shadow_disable(const char *why);

static int shadow_selfcheck(emu64 *self)
{
    const Gfx *p = self->gfx_p;
    unsigned char cmd_from_word;

    if (p == 0) { shadow_disable("gfx_p is NULL on entry"); return 0; }

    /* gfx is a copy of *gfx_p (emu64.c:5809) ... */
    if (((const u32 *)p)[0] != self->gfx.words.w0 ||
        ((const u32 *)p)[1] != self->gfx.words.w1) {
        shadow_disable("gfx != *gfx_p -- member offsets are wrong");
        return 0;
    }
    /* ... and gfx_cmd is its top byte (emu64.c:5810, gfx.dma.cmd). */
    cmd_from_word = (unsigned char)((self->gfx.words.w0 >> 24) & 0xFF);
    if (cmd_from_word != (unsigned char)self->gfx_cmd) {
        shadow_disable("gfx_cmd != gfx.dma.cmd -- member offsets are wrong");
        return 0;
    }
    return 1;
}

/* ---------------------------------------------------------------------------
 * THE LOOP. A transcription of emu64.c:5806-5875 with the dead work removed.
 *
 * WHAT MUST BE BIT-EXACT, and why (do not "clean up" any of these):
 *
 *   gfx_p        re-read from memory after EVERY handler call. dl_G_DL
 *                (:3453), dl_G_ENDDL (:3556), dl_G_CULLDL (:5300) and
 *                dl_G_BRANCH_Z (:5325) all WRITE it. Caching it in a register
 *                across the call would execute the wrong display list.
 *   FrameCansel  a global written from outside emu64 (jsyswrap.cpp:331), so it
 *                must be re-read every iteration. Never hoist it.
 *   dl_history   read by printInfo() (emu64.c:752), which panic() calls.
 *                Dropping it would cost the last-16-DL dump on every crash —
 *                a crash-triage regression to save three instructions.
 *   the counters pc_emu64_frame_* feed the [EMU64] line.
 *   the guards   cmd_index < 64 and fn != NULL. Dropping either is an
 *                unbounded indirect jump.
 *
 * WHAT IS DELIBERATELY DROPPED, with its reader:
 *
 *   command_info[idx].calls++     read only by emu64_taskstart:5934, which is
 *                                 gated on aflags[AFLAGS_PRINT_COMMAND_INFO].
 *                                 AFLAGS_MAX is 0 (emu64.hpp:20) because
 *                                 AFLAGS_DEBUG is defined nowhere in the tree,
 *                                 so aflags_c::operator[] is `return 0;` and
 *                                 that gate is never open. Zero behavioural
 *                                 cost; 4-5 instructions per command saved.
 *   EMU64_INFO/INFOF              already empty macros (emu64.hpp:558-559).
 *   the print_commands block      the four macros inside it are empty, but the
 *                                 FLAG is data-driven: dl_G_NOOP sets it from
 *                                 a G_TAG_INFO NOOP's param0 (emu64.c:4445).
 *                                 So we do not delete the test — we PUNT the
 *                                 whole traversal back to -O0 when it is set,
 *                                 which is both cheaper here and impossible to
 *                                 get subtly wrong.
 *
 * cmds_processed is kept. Its only readers are two never-compiled
 * PC_GX_VERBOSE printfs, but it is three instructions and it is the only
 * "how far did this traversal get" number available if a hang is ever debugged.
 * ------------------------------------------------------------------------- */
static void shadow_loop(emu64 *self)
{
    for (;;) {
        /* We were entered mid-body, so the first thing we owe the loop is the
         * tail increment of the command that just ran (emu64.c:5874). */
        self->gfx_p++;

        if (self->end_dl || FrameCansel) return;        /* :5806 */

        self->cmds_processed++;                          /* :5807 */
        self->gfx     = *self->gfx_p;                    /* :5809 */
        self->gfx_cmd = self->gfx.dma.cmd;               /* :5810 */

        self->dl_history[self->dl_history_start & (DL_HISTORY_COUNT - 1)] =
            self->gfx_p;                                 /* :5811 */
        self->dl_history_start =
            (self->dl_history_start + 1) & (DL_HISTORY_COUNT - 1);

        self->work_ptr = 0;                              /* :5814 */

        /* A G_TAG_INFO NOOP can arm this mid-traversal. Hand the rest back. */
        if (self->print_commands) { s_punts++; return; } /* :5816 */

        pc_emu64_frame_cmds++;                           /* :5826-5834 */
        switch (self->gfx_cmd) {
            case 0x00: pc_emu64_frame_noop_cmds++; break;
            case 0x01: pc_emu64_frame_vtx_cmds++;  break;
            case 0x05: case 0x06: case 0x07:
            case 0x09: case 0x0A: case 0x0D:
                       pc_emu64_frame_tri_cmds++;  break;
            case 0xDE: pc_emu64_frame_dl_cmds++;   break;
            default: break;
        }

        {
            unsigned int idx =
                (unsigned int)(unsigned char)(self->gfx_cmd - SH_FIRSTCMD);

            if (idx < (unsigned int)SH_N) {              /* :5847 */
                void (*fn)(void *) = s_orig[idx].fn;
                if (fn != 0) {                           /* :5848 */
                    s_cmds++;
                    fn(self);                            /* :5850 */
                }
            } else {
                /* :5856-5871. The original prints and then `break`s out of the
                 * loop with end_dl still FALSE.
                 *
                 * ⚠️ WE MUST SET end_dl. This is the one place the shadow is
                 * not a literal transcription, and omitting it is fatal: if we
                 * merely returned, the -O0 loop would do its gfx_p++, re-test
                 * (!end_dl && !FrameCansel) -> TRUE, and carry on executing
                 * whatever follows an unrecognised command. Setting it is
                 * observationally equivalent — nothing reads end_dl between
                 * that break and emu64_taskstart_r:5776, which resets it. */
                self->Printf0("予期しないコマンドがありました。中断します。(cmd=%02x)\n",
                              self->gfx_cmd);
                self->end_dl = 1;
                return;
            }
        }
    }
}

/* The 64 trampolines. Each finishes the command the -O0 loop staged, then
 * takes the traversal over. Written out flat for the same reason
 * dc_emu64_hist.c:139-143 gives: a clever base-N macro pair has to special-case
 * 8 and 9 (there is no `08` token) and the first draft of exactly that silently
 * redefined slots 50-57. */
/* ⚠️ THE SELF-CHECK MUST RUN *BEFORE* THE ORIGINAL HANDLER, AND THIS COST A RUN.
 *
 * The first version called s_orig[N].fn(p) first and checked afterwards, on the
 * reasoning that the check is cheap and the handler is what we are wrapping.
 * It came back `[EMU64S] DISABLED mid-run: gfx != *gfx_p -- member offsets are
 * wrong` on a build whose offsets were fine.
 *
 * The check relates gfx_p to the gfx the -O0 loop copied out of it -- but four
 * handlers legitimately REWRITE gfx_p as their whole purpose: dl_G_DL
 * (emu64.c:3453), dl_G_ENDDL (:3556), dl_G_CULLDL (:5300) and dl_G_BRANCH_Z
 * (:5325). Run the check after one of those and the two correctly disagree.
 * On entry, though, the -O0 loop has just executed `gfx = *gfx_p` two
 * statements earlier and nothing has had a chance to move it.
 */
#define SH_TRAMP(N)                                                     \
    static void dc_sh_t##N(void *p) {                                   \
        emu64 *self = (emu64 *)p;                                       \
        if (!s_checked) {                                               \
            s_checked = 1;                                              \
            if (!shadow_selfcheck(self)) { s_orig[N].fn(p); return; }   \
        }                                                               \
        s_orig[N].fn(p);                                                \
        if (!s_armed) return;   /* disabled by the check above */       \
        s_entries++;                                                    \
        shadow_loop(self);                                              \
    }

SH_TRAMP(0)  SH_TRAMP(1)  SH_TRAMP(2)  SH_TRAMP(3)
SH_TRAMP(4)  SH_TRAMP(5)  SH_TRAMP(6)  SH_TRAMP(7)
SH_TRAMP(8)  SH_TRAMP(9)  SH_TRAMP(10) SH_TRAMP(11)
SH_TRAMP(12) SH_TRAMP(13) SH_TRAMP(14) SH_TRAMP(15)
SH_TRAMP(16) SH_TRAMP(17) SH_TRAMP(18) SH_TRAMP(19)
SH_TRAMP(20) SH_TRAMP(21) SH_TRAMP(22) SH_TRAMP(23)
SH_TRAMP(24) SH_TRAMP(25) SH_TRAMP(26) SH_TRAMP(27)
SH_TRAMP(28) SH_TRAMP(29) SH_TRAMP(30) SH_TRAMP(31)
SH_TRAMP(32) SH_TRAMP(33) SH_TRAMP(34) SH_TRAMP(35)
SH_TRAMP(36) SH_TRAMP(37) SH_TRAMP(38) SH_TRAMP(39)
SH_TRAMP(40) SH_TRAMP(41) SH_TRAMP(42) SH_TRAMP(43)
SH_TRAMP(44) SH_TRAMP(45) SH_TRAMP(46) SH_TRAMP(47)
SH_TRAMP(48) SH_TRAMP(49) SH_TRAMP(50) SH_TRAMP(51)
SH_TRAMP(52) SH_TRAMP(53) SH_TRAMP(54) SH_TRAMP(55)
SH_TRAMP(56) SH_TRAMP(57) SH_TRAMP(58) SH_TRAMP(59)
SH_TRAMP(60) SH_TRAMP(61) SH_TRAMP(62) SH_TRAMP(63)

#define SH_TRAMP_REF(N) dc_sh_t##N

static void (*const s_tramps[SH_N])(void *) = {
    SH_TRAMP_REF(0),  SH_TRAMP_REF(1),  SH_TRAMP_REF(2),  SH_TRAMP_REF(3),
    SH_TRAMP_REF(4),  SH_TRAMP_REF(5),  SH_TRAMP_REF(6),  SH_TRAMP_REF(7),
    SH_TRAMP_REF(8),  SH_TRAMP_REF(9),  SH_TRAMP_REF(10), SH_TRAMP_REF(11),
    SH_TRAMP_REF(12), SH_TRAMP_REF(13), SH_TRAMP_REF(14), SH_TRAMP_REF(15),
    SH_TRAMP_REF(16), SH_TRAMP_REF(17), SH_TRAMP_REF(18), SH_TRAMP_REF(19),
    SH_TRAMP_REF(20), SH_TRAMP_REF(21), SH_TRAMP_REF(22), SH_TRAMP_REF(23),
    SH_TRAMP_REF(24), SH_TRAMP_REF(25), SH_TRAMP_REF(26), SH_TRAMP_REF(27),
    SH_TRAMP_REF(28), SH_TRAMP_REF(29), SH_TRAMP_REF(30), SH_TRAMP_REF(31),
    SH_TRAMP_REF(32), SH_TRAMP_REF(33), SH_TRAMP_REF(34), SH_TRAMP_REF(35),
    SH_TRAMP_REF(36), SH_TRAMP_REF(37), SH_TRAMP_REF(38), SH_TRAMP_REF(39),
    SH_TRAMP_REF(40), SH_TRAMP_REF(41), SH_TRAMP_REF(42), SH_TRAMP_REF(43),
    SH_TRAMP_REF(44), SH_TRAMP_REF(45), SH_TRAMP_REF(46), SH_TRAMP_REF(47),
    SH_TRAMP_REF(48), SH_TRAMP_REF(49), SH_TRAMP_REF(50), SH_TRAMP_REF(51),
    SH_TRAMP_REF(52), SH_TRAMP_REF(53), SH_TRAMP_REF(54), SH_TRAMP_REF(55),
    SH_TRAMP_REF(56), SH_TRAMP_REF(57), SH_TRAMP_REF(58), SH_TRAMP_REF(59),
    SH_TRAMP_REF(60), SH_TRAMP_REF(61), SH_TRAMP_REF(62), SH_TRAMP_REF(63),
};

static void shadow_disable(const char *why)
{
    int i;
    for (i = 0; i < SH_N; i++) _ZL11dl_func_tbl[i] = s_orig[i];
    s_armed = 0;
    s_ready = 0;
    DC_LOGE("[EMU64S] DISABLED mid-run: %s\n", why);
}

extern "C" void dc_emu64_shadow_init(void)
{
    int i;

    for (i = 0; i < SH_N; i++) {
        s_orig[i] = _ZL11dl_func_tbl[i];
        s_tramp_tbl[i].fn  = s_tramps[i];
        s_tramp_tbl[i].adj = 0;
    }

    /* Same discipline as dc_emu64_hist.c:240-249: refuse to instrument a table
     * we do not recognise. If the mangling, the table order or the PMF layout
     * ever changes, this is the difference between "the shadow is off" and
     * "the game jumps into hyperspace". */
    if (s_orig[SH_IDX_VTX].fn    != (void (*)(void *))_ZN5emu648dl_G_VTXEv ||
        s_orig[SH_IDX_TRI1].fn   != (void (*)(void *))_ZN5emu649dl_G_TRI1Ev ||
        s_orig[SH_IDX_CULLDL].fn != (void (*)(void *))_ZN5emu6411dl_G_CULLDLEv ||
        s_orig[SH_IDX_VTX].adj   != 0 ||
        s_orig[SH_IDX_TRI1].adj  != 0) {
        DC_LOGE("[EMU64S] DISABLED: dispatch table signature mismatch "
                "(vtx=%p tri1=%p culldl=%p adj=%d)\n",
                (void *)s_orig[SH_IDX_VTX].fn,
                (void *)s_orig[SH_IDX_TRI1].fn,
                (void *)s_orig[SH_IDX_CULLDL].fn,
                s_orig[SH_IDX_VTX].adj);
        return;
    }

    s_ready = 1;
    DC_LOGE("[EMU64S] armed, mode=%d (0=always, N>1 = every Nth frame)\n",
            (int)(DC_EMU64_SHADOW_LOOP));
}

/* DC_EMU64_SHADOW_LOOP == 1 arms permanently. A value N > 1 arms on one frame
 * in N, which is the matched-frame A/B: the same run produces armed and
 * unarmed windows a few frames apart, against the same camera and the same
 * geometry. kb/RESUME.md §0b rule 5 exists because estimates from unmatched
 * frames have been wrong here before. */
extern "C" void dc_emu64_shadow_frame_open(unsigned int tick)
{
    int i;
    if (!s_ready) return;
#if DC_EMU64_SHADOW_LOOP > 1
    if ((tick % (unsigned int)(DC_EMU64_SHADOW_LOOP)) != 0u) return;
#else
    (void)tick;
#endif
    for (i = 0; i < SH_N; i++) _ZL11dl_func_tbl[i] = s_tramp_tbl[i];
    s_armed = 1;
}

extern "C" void dc_emu64_shadow_frame_close(void)
{
    int i;
    if (!s_armed) return;
    for (i = 0; i < SH_N; i++) _ZL11dl_func_tbl[i] = s_orig[i];
    s_armed = 0;
}

extern "C" void dc_emu64_shadow_report(void)
{
    if (!s_ready && !s_entries) return;
    DC_LOGE("[EMU64S] traversals=%u cmds=%u punts=%u\n",
            s_entries, s_cmds, s_punts);
    s_entries = 0;
    s_cmds = 0;
    s_punts = 0;
}

#endif /* DC_EMU64_SHADOW_LOOP */
