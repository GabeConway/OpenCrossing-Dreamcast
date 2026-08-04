/* dc_emu64_hist.c — G1: per-opcode time histogram over emu64's dispatch table.
 *
 * WHY. The [EMU64] line (dc_vi.c) says 2,094 of 2,867 town display-list
 * commands — 73 % — are neither vertex nor triangle nor display-list, i.e.
 * they are pure RDP/RSP state. At the measured 12.31 us/cmd (kb/perf-dc.md)
 * that is ~26 ms of an ~85 ms frame, and nothing else in this tree can say
 * WHICH opcodes those are. `[GXAPI]` cannot: it counts what emu64 hands the GX
 * layer, not what emu64 spends getting there.
 *
 * emu64 already keeps a free per-opcode CALL count (`command_info[i].calls`,
 * emu64.c:5854) but no usable TIME. Its time half is dead three ways over:
 * both EMU64_TIMED_SEGMENT_* macros expand to nothing without EMU64_DEBUG
 * (emu64.hpp:584-585); the EMU64_DEBUG forms do not compile (`this->#stat` is
 * a stringify operator outside a stringify context, emu64.hpp:578); and
 * emu64.c:5940 computes the mean as `time / time`. Nor is command_info[] ever
 * reset, so it could only ever give a since-boot total.
 *
 * ==========================================================================
 * WHY THIS IS LEGAL — read before assuming it violates CLAUDE.md §1
 * ==========================================================================
 * src/ is neither edited nor compiled differently. emu64's dispatch table is
 *
 *     static dl_func dl_func_tbl[NUM_COMMANDS] = { ... };   emu64.c:5702
 *
 * a NON-CONST file-static array of pointer-to-member in .data, and there is no
 * MMU write protection anywhere in this port. We save the 64 entries, install
 * thunks that time and then call the untouched -O0 originals, and put the
 * originals back. The only BUILD change is one objcopy --globalize-symbol on
 * an already-existing local symbol so that dc/ can name it — a symbol BINDING
 * change, which alters not one byte of loaded image and reselects not one
 * instruction. Layout, not codegen.
 *
 * This is G1 from kb/research-fps-ideas.md, the row marked "risk ~0 — no
 * policy question, do it". G2 and G3 on that page are different animals and
 * need the user's sign-off; this file does not touch them.
 *
 * ==========================================================================
 * THE ABI, verified by disassembling the shipped emu64.c.o
 * ==========================================================================
 * `typedef void (emu64::* dl_func)(void);` (emu64.hpp:873) is an Itanium
 * pointer-to-member: { u32 ptr; s32 adj; }, 8 bytes, 512 B for the 64 entries.
 * emu64_taskstart_r +0x322..+0x354 loads ptr and adj, tests (ptr & 1) for the
 * virtual case, computes r4 = this + adj, and does `jsr @ptr`. All 64 adj
 * words are zero and SH-4 instructions are 2-byte aligned, so the virtual
 * branch is never taken. A handler is therefore exactly
 *
 *     void f(emu64 *this)     // sh-elf ELF ABI, `this` in r4, void return
 *
 * and a thunk with adj = 0 is called the same way. The 64 handlers themselves
 * are global symbols (`T`), unlike the table.
 *
 * ==========================================================================
 * HEISENBERG — the rule this instrument exists under
 * ==========================================================================
 * kb/RESUME.md §0b rule 4 and rule 5, and kb/perf-dc.md §4: `dc_time_us()`
 * costs ~0.43 us (~86 cycles) per read, and the [GXAPI] census once reported
 * 2.44 ms of which 2.17 ms was the probe. Two dc_time_us() per command at
 * 2,600-3,800 commands/frame would add 2.2-3.3 ms/frame AND land it
 * disproportionately on the cheapest opcodes — exactly the number rule 5
 * exists to prevent. Three stacked mitigations:
 *
 *   1. ONE clock read per command, not two. Time is charged to the PREVIOUS
 *      opcode on entry to the next thunk. No unattributed gaps: the
 *      dispatcher's own loop overhead lands in a bucket instead of vanishing.
 *   2. The RAW TMU2 counter, not dc_time_us(). One uncached 32-bit read
 *      against dc_time_us()'s four reads plus a timespec conversion and 64-bit
 *      mul/div. It is the same register KOS's own clock reads, so it is
 *      emulated by construction — every FPS number in this project comes
 *      through it.
 *   3. Sample 1 frame in N by swapping the whole 512-byte table in and out.
 *      Non-sampled frames are bit-identical to an uninstrumented build.
 *
 * And then PRINT the calibration (`probe=`), so a reader can subtract
 * probe x calls from a bucket instead of arguing about it.
 *
 * SELF-CHECK: `tot=` must land near [PHASE] `draw=` minus `gx=` for the same
 * window. If it does not, the histogram is wrong and nothing built on it is
 * trustworthy. Read that before reading the buckets.
 *
 * Kill switch: DC_EMU64_HIST=0 (the default) compiles this file to nothing.
 */
#include "dc_platform.h"

#if defined(DC_EMU64_HIST) && DC_EMU64_HIST > 0

#define HIST_N     64           /* NUM_COMMANDS (emu64.hpp:26)              */
#define HIST_GAP   64           /* sentinel: time outside a traversal       */
#define HIST_SLOTS 65

/* The Itanium pointer-to-member layout the dispatcher reads. */
typedef struct { void (*fn)(void *self); int adj; } DcPmf;

/* The C++-mangled table, promoted from local to global by the objcopy step in
 * dc/Makefile. Writing the mangled name as a plain C identifier is correct on
 * this toolchain: the compiler adds the leading `_` itself, so
 * `_ZL11dl_func_tbl` here emits an undefined `__ZL11dl_func_tbl`, which is
 * exactly what objcopy globalises. Verified with sh-elf-nm both ways. */
extern DcPmf _ZL11dl_func_tbl[HIST_N];

/* Three handlers named only to sanity-check the table before touching it.
 * These are ordinary global text symbols in emu64.c.o. */
extern void _ZN5emu648dl_G_VTXEv(void *self);
extern void _ZN5emu649dl_G_TRI1Ev(void *self);
extern void _ZN5emu6411dl_G_CULLDLEv(void *self);

/* TMU2, free-running at Pck/4 = 12,468,720 Hz (80.2 ns/tick), reloaded once a
 * second by KOS's underflow ISR. It is a DOWN counter, so a delta is
 * prev - now, and it wraps at the reload — one sample in ~40,000, discarded by
 * the range test in hist_enter(). */
#define HIST_TCNT2   (*(volatile unsigned int *)0xffd80024)
#define HIST_TICK_NS 80u
#define HIST_WRAP_GUARD 0x00400000u

static DcPmf        s_orig[HIST_N];
static DcPmf        s_thunk_tbl[HIST_N];
static unsigned int s_ticks[HIST_SLOTS];
static unsigned int s_calls[HIST_SLOTS];
static unsigned int s_prev = HIST_GAP;
static unsigned int s_last;
static unsigned int s_frames;
static unsigned int s_probe_ns10;   /* per-thunk overhead, ns x 10 */
static int          s_armed;
static int          s_ready;

static inline void hist_enter(unsigned int op) {
    unsigned int t = HIST_TCNT2;
    unsigned int d = s_last - t;            /* down counter */
    if (d < HIST_WRAP_GUARD) s_ticks[s_prev] += d;
    s_calls[s_prev]++;
    s_prev = op;
    s_last = t;
}

/* 64 thunks. hist_enter() inlines and the original call is in tail position,
 * so at -O2 each is one uncached read, a handful of ALU ops and an indirect
 * jump — no frame. */
#define DC_THUNK(N) \
    static void dc_hist_t##N(void *self) { hist_enter(N); s_orig[N].fn(self); }

/* Written out flat, 0..63, deliberately. A clever base-N macro pair here
 * would have to spell 8 and 9 as a special case (there is no `08` token) and
 * the first draft of exactly that silently redefined slots 50-57. A table
 * that must be one-to-one with a 64-entry hardware-order array is worth
 * seeing in full. */
DC_THUNK(0)  DC_THUNK(1)  DC_THUNK(2)  DC_THUNK(3)
DC_THUNK(4)  DC_THUNK(5)  DC_THUNK(6)  DC_THUNK(7)
DC_THUNK(8)  DC_THUNK(9)  DC_THUNK(10) DC_THUNK(11)
DC_THUNK(12) DC_THUNK(13) DC_THUNK(14) DC_THUNK(15)
DC_THUNK(16) DC_THUNK(17) DC_THUNK(18) DC_THUNK(19)
DC_THUNK(20) DC_THUNK(21) DC_THUNK(22) DC_THUNK(23)
DC_THUNK(24) DC_THUNK(25) DC_THUNK(26) DC_THUNK(27)
DC_THUNK(28) DC_THUNK(29) DC_THUNK(30) DC_THUNK(31)
DC_THUNK(32) DC_THUNK(33) DC_THUNK(34) DC_THUNK(35)
DC_THUNK(36) DC_THUNK(37) DC_THUNK(38) DC_THUNK(39)
DC_THUNK(40) DC_THUNK(41) DC_THUNK(42) DC_THUNK(43)
DC_THUNK(44) DC_THUNK(45) DC_THUNK(46) DC_THUNK(47)
DC_THUNK(48) DC_THUNK(49) DC_THUNK(50) DC_THUNK(51)
DC_THUNK(52) DC_THUNK(53) DC_THUNK(54) DC_THUNK(55)
DC_THUNK(56) DC_THUNK(57) DC_THUNK(58) DC_THUNK(59)
DC_THUNK(60) DC_THUNK(61) DC_THUNK(62) DC_THUNK(63)

#define DC_THUNK_REF(N) dc_hist_t##N

static void (*const s_thunks[HIST_N])(void *) = {
    DC_THUNK_REF(0),  DC_THUNK_REF(1),  DC_THUNK_REF(2),  DC_THUNK_REF(3),
    DC_THUNK_REF(4),  DC_THUNK_REF(5),  DC_THUNK_REF(6),  DC_THUNK_REF(7),
    DC_THUNK_REF(8),  DC_THUNK_REF(9),  DC_THUNK_REF(10), DC_THUNK_REF(11),
    DC_THUNK_REF(12), DC_THUNK_REF(13), DC_THUNK_REF(14), DC_THUNK_REF(15),
    DC_THUNK_REF(16), DC_THUNK_REF(17), DC_THUNK_REF(18), DC_THUNK_REF(19),
    DC_THUNK_REF(20), DC_THUNK_REF(21), DC_THUNK_REF(22), DC_THUNK_REF(23),
    DC_THUNK_REF(24), DC_THUNK_REF(25), DC_THUNK_REF(26), DC_THUNK_REF(27),
    DC_THUNK_REF(28), DC_THUNK_REF(29), DC_THUNK_REF(30), DC_THUNK_REF(31),
    DC_THUNK_REF(32), DC_THUNK_REF(33), DC_THUNK_REF(34), DC_THUNK_REF(35),
    DC_THUNK_REF(36), DC_THUNK_REF(37), DC_THUNK_REF(38), DC_THUNK_REF(39),
    DC_THUNK_REF(40), DC_THUNK_REF(41), DC_THUNK_REF(42), DC_THUNK_REF(43),
    DC_THUNK_REF(44), DC_THUNK_REF(45), DC_THUNK_REF(46), DC_THUNK_REF(47),
    DC_THUNK_REF(48), DC_THUNK_REF(49), DC_THUNK_REF(50), DC_THUNK_REF(51),
    DC_THUNK_REF(52), DC_THUNK_REF(53), DC_THUNK_REF(54), DC_THUNK_REF(55),
    DC_THUNK_REF(56), DC_THUNK_REF(57), DC_THUNK_REF(58), DC_THUNK_REF(59),
    DC_THUNK_REF(60), DC_THUNK_REF(61), DC_THUNK_REF(62), DC_THUNK_REF(63),
};

/* Names in TABLE ORDER (emu64.c:5703-5766). Index i is opcode (u8)(i + 0xCE);
 * G_FIRST_CMD is G_SETTEXEDGEALPHA = 0xCE (gbi_extensions.h:54,59). The many
 * NOOP slots are real — the table is sparse by design. */
static const char *const s_name[HIST_N] = {
    "SETTEXEDGEALPHA", "SETCOMBINE_NOTEV", "SETCOMBINE_TEV", "noop",
    "SETTILE_DOLPHIN", "noop", "noop", "SPECIAL_1",
    "noop", "TEXTURE", "POPMTX", "GEOMETRYMODE",
    "MTX", "MOVEWORD", "MOVEMEM", "LOAD_UCODE",
    "DL", "ENDDL", "SPNOOP", "RDPHALF_1",
    "SETOTHERMODE_L", "SETOTHERMODE_H", "TEXRECT", "noop",
    "RDPLOADSYNC", "RDPPIPESYNC", "RDPTILESYNC", "RDPFULLSYNC",
    "noop", "noop", "noop", "SETSCISSOR",
    "SETPRIMDEPTH", "RDPSETOTHERMODE", "LOADTLUT", "noop",
    "SETTILESIZE", "LOADBLOCK", "LOADTILE", "SETTILE",
    "FILLRECT", "SETFILLCOLOR", "SETFOGCOLOR", "SETBLENDCOLOR",
    "SETPRIMCOLOR", "SETENVCOLOR", "SETCOMBINE", "SETTIMG",
    "SETZIMG", "SETCIMG", "noop", "VTX",
    "MODIFYVTX", "CULLDL", "BRANCH_Z", "TRI1",
    "TRI2", "QUAD", "LINE3D", "TRIN",
    "TRIN_INDEPEND", "noop", "noop", "QUADN",
};

/* Measure the thunk's own cost so the report can publish it instead of
 * leaving every reader to guess. Not a null handler call — that would time
 * the call too — just hist_enter() itself, which is the part that is added to
 * every command. */
static void hist_calibrate(void) {
    const unsigned int iters = 20000u;
    unsigned int i, t0, t1, d;

    s_prev = HIST_GAP;
    s_last = HIST_TCNT2;
    t0 = HIST_TCNT2;
    for (i = 0; i < iters; i++)
        hist_enter(HIST_GAP);
    t1 = HIST_TCNT2;

    d = t0 - t1;
    if (d < HIST_WRAP_GUARD)
        s_probe_ns10 = (unsigned int)(((u64)d * HIST_TICK_NS * 10u) / iters);

    for (i = 0; i < HIST_SLOTS; i++) { s_ticks[i] = 0; s_calls[i] = 0; }
}

void dc_emu64_hist_init(void) {
    int i;

    for (i = 0; i < HIST_N; i++) {
        s_orig[i] = _ZL11dl_func_tbl[i];
        s_thunk_tbl[i].fn  = s_thunks[i];
        s_thunk_tbl[i].adj = 0;
    }

    /* Fail LOUDLY rather than instrument a table we do not recognise. If the
     * mangling, the table order or the PMF layout ever changes, this is the
     * difference between "the histogram is off" and "the game jumps into
     * hyperspace". Indices from the table above: 51 VTX, 55 TRI1, 53 CULLDL. */
    if (s_orig[51].fn != _ZN5emu648dl_G_VTXEv ||
        s_orig[55].fn != _ZN5emu649dl_G_TRI1Ev ||
        s_orig[53].fn != _ZN5emu6411dl_G_CULLDLEv ||
        s_orig[51].adj != 0 || s_orig[55].adj != 0) {
        DC_LOGE("[EMU64H] DISABLED: dispatch table signature mismatch "
                "(vtx=%p tri1=%p culldl=%p adj=%d)\n",
                (void*)s_orig[51].fn, (void*)s_orig[55].fn,
                (void*)s_orig[53].fn, s_orig[51].adj);
        return;
    }

    hist_calibrate();
    s_ready = 1;
    DC_LOGE("[EMU64H] armed, period=%d frames, probe=%u.%u ns/cmd\n",
            (int)(DC_EMU64_HIST), s_probe_ns10 / 10u, s_probe_ns10 % 10u);
}

void dc_emu64_hist_frame_open(unsigned int tick) {
    int i;
    if (!s_ready || (tick % (unsigned)(DC_EMU64_HIST)) != 0u) return;
    s_prev = HIST_GAP;
    s_last = HIST_TCNT2;
    for (i = 0; i < HIST_N; i++) _ZL11dl_func_tbl[i] = s_thunk_tbl[i];
    s_armed = 1;
}

void dc_emu64_hist_frame_close(void) {
    int i;
    if (!s_armed) return;
    for (i = 0; i < HIST_N; i++) _ZL11dl_func_tbl[i] = s_orig[i];
    hist_enter(HIST_GAP);      /* close the last command's bucket */
    s_armed = 0;
    s_frames++;
}

void dc_emu64_hist_report(void) {
    unsigned int order[HIST_N];
    unsigned int n = 0, i, j, tot_ticks = 0;
    double fdiv;

    if (!s_ready || s_frames == 0) return;
    fdiv = (double)s_frames;

    for (i = 0; i < HIST_SLOTS; i++) tot_ticks += s_ticks[i];
    for (i = 0; i < HIST_N; i++)
        if (s_calls[i] != 0) order[n++] = i;

    /* Insertion sort by ticks, descending. n is at most 64 and this runs once
     * per 30 presented frames. */
    for (i = 1; i < n; i++) {
        unsigned int k = order[i];
        for (j = i; j > 0 && s_ticks[order[j - 1]] < s_ticks[k]; j--)
            order[j] = order[j - 1];
        order[j] = k;
    }

    DC_LOGE("[EMU64H] f=%u tot=%.2fms gap=%.2fms probe=%u.%uns |",
            s_frames,
            (double)tot_ticks * HIST_TICK_NS / 1000000.0 / fdiv,
            (double)s_ticks[HIST_GAP] * HIST_TICK_NS / 1000000.0 / fdiv,
            s_probe_ns10 / 10u, s_probe_ns10 % 10u);
    /* Capped at 12: the whole point is the head of the distribution, and an
     * unbounded line would fight the console flood limiter. */
    for (i = 0; i < n && i < 12u; i++) {
        unsigned int b = order[i];
        DC_LOGE(" %s %.2f/%.0f", s_name[b],
                (double)s_ticks[b] * HIST_TICK_NS / 1000000.0 / fdiv,
                (double)s_calls[b] / fdiv);
    }
    DC_LOGE("\n");

    for (i = 0; i < HIST_SLOTS; i++) { s_ticks[i] = 0; s_calls[i] = 0; }
    s_frames = 0;
}

#endif /* DC_EMU64_HIST */
