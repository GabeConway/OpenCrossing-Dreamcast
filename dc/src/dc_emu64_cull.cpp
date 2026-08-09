/* dc_emu64_cull.cpp — G3. Cull a triangle batch at G_TRIN_INDEPEND ENTRY,
 * before its vertex references are expanded through the GX attribute setters.
 *
 * =============================================================================
 * WHY THIS IS THE BIGGEST LEVER IN THE PROJECT
 * =============================================================================
 * MEASURED (kb/STATE.md, G1 re-run 2026-08-06 and G4 2026-08-06, both
 * x2-corrected for the per-logic-tick denominator, measurement rule 9):
 *
 *   G_TRIN_INDEPEND      34.4 ms of a 45.6 ms town frame        (75 %)
 *   of which cull+xform  10.8 ms   already attributed to dc/
 *   of which emu64's     ~30.9 ms  index expansion + set_position
 *   of which ours         ~8.4 ms  the GX* attribute setters
 *   vcull / v            5,250 / 2,899
 *
 * That last row is the whole argument: **75 % of the vertex references this
 * opcode expands are thrown away by dc_gx_batch_is_offscreen() one step
 * later** (dc_gx.c), AFTER emu64 has walked the index stream, after
 * set_position() has transformed and submitted every one of them, and after
 * our own GXPosition3f32/GXNormal3f32/GXColor4u8 have staged them. G4 priced
 * the reference at 0.536 us on OUR side alone, and the emu64 side is four
 * times that. The estimate for doing the same rejection one command earlier is
 * **5.4 ms floor / 19.2 ms ceiling** of a 45-69 ms frame.
 *
 * The test is not a new test. It is ONE IMPLEMENTATION, TWO INPUTS:
 * dc_gx_aabb_is_offscreen(), split out of dc_gx_batch_is_offscreen() for this
 * file, handed a box built from emu64's own vertex array instead of from our
 * staged vertex buffer. Two frustum tests that must agree is how a cull starts
 * deleting visible geometry; there is one implementation, and G3 feeds it.
 *
 * ⚠️ The two inputs coincide only because emu64.c:4935's GXEnd() — inside
 * `#ifdef TARGET_PC` — makes flush granularity exactly one TRIN. If that ever
 * stops being true, dc_gx.c's box spans batches ours does not, and BOTH the
 * claim above and the VERIFY signal below break with no tripwire to say so.
 *
 * =============================================================================
 * THE SHAPE: A TRAMPOLINE, NOT AN EDIT
 * =============================================================================
 * src/ is not edited (CLAUDE.md §1). emu64 dispatches through a 64-entry table
 * of pointers-to-member, `_ZL11dl_func_tbl`, promoted from file-local to global
 * by an objcopy step in dc/Makefile — the same mechanism G1's histogram
 * (dc/src/dc_emu64_hist.c) has been using since 2026-08-04. We replace slots
 * 59 (G_TRIN) and 60 (G_TRIN_INDEPEND) with our own functions, which either
 * consume the command themselves or call the original.
 *
 * ⚠️ THE TABLE IS SHARED WITH G1 AND G2, AND THEY REWRITE ALL 64 SLOTS.
 * dc_emu64_hist.c:262 and dc_emu64_shadow.cpp:492 each memcpy the whole table
 * on frame open and restore the whole table on frame close, from a snapshot
 * they took at THEIR init. Three consequences, and all three are load-bearing:
 *
 *   1. G3 installs ONCE, at init, slot-wise. It must never touch the table
 *      per frame — that would race their snapshot/restore.
 *   2. dc_emu64_cull_init() must run STRICTLY BEFORE dc_emu64_hist_init() and
 *      dc_emu64_shadow_init(), so that THEIR snapshot captures OUR trampoline
 *      and their restore puts it back. dc_main.c orders it that way.
 *   3. A tripwire re-checks slot 60 once per presented frame and re-installs
 *      if something replaced it, counting `reinst=`. A non-zero reinst on the
 *      log means someone reordered init — it is a diagnostic, not a fix.
 *
 * The A/B is therefore a BOOLEAN (s_active), never a table swap:
 * DC_EMU64_CULL=1 arms permanently, N>1 arms one presented frame in N.
 *
 * =============================================================================
 * WHY C++, AND WHY THE HEADER RATHER THAN AN OFFSET MAP
 * =============================================================================
 * Verbatim from dc_emu64_shadow.cpp:60-84 — emu64.hpp's offset comments are
 * MWERKS/PowerPC and wrong here from `gfx` onward (`vertices` is at 0x1018,
 * not the header's 0x0E1C; sizeof(emu64) is 0x2278). A hand-written offset map
 * writes into the neighbouring member instead of faulting. We include the real
 * header and let the same compiler compute the same layout, with a
 * static_assert as the drift tripwire.
 *
 * =============================================================================
 * KILL SWITCH AND VERDICT
 * =============================================================================
 * DC_EMU64_CULL=0 (the default) compiles this file to nothing — byte-identical
 * image. `-DDC_GX_NO_VTXID_DECAL` is a second, narrower switch that only
 * NARROWS G-B's side channel back to its pre-S14 reach; see its own section
 * below the includes.
 * The verdict on a run is `[EMU64C] falsecull=0` under
 * -DDC_EMU64_CULL_VERIFY, which needs no screenshot: in verify mode every
 * batch we would cull is ALSO handed to the original handler, and we count any
 * batch our entry test rejected that dc_gx.c's flush-time test did not.
 * A screenshot pair is still required for the shipping build (rule 2).
 */
#include "dc_platform.h"

#if defined(DC_EMU64_CULL) && DC_EMU64_CULL > 0

/* ⚠️ THE GATE CANNOT GATE WITHOUT THE THING IT COMPARES AGAINST. VERIFY's
 * whole verdict is "dc_gx.c's late cull agreed with us", read off
 * pc_gx_culled_draws — and -DDC_NO_BATCH_CULL makes that counter a constant 0,
 * at which point every correct cull scores as a falsecull and the gate reads
 * as a catastrophe. Refuse to build the combination rather than hand anyone
 * that log. */
#if defined(DC_EMU64_CULL_VERIFY) && defined(DC_NO_BATCH_CULL)
#error "DC_EMU64_CULL_VERIFY compares against dc_gx.c's late cull, which DC_NO_BATCH_CULL disables; the gate would report falsecull on every correct cull. Drop one of the two."
#endif

/* ⚠️ THIS TU DOES NOT SEE emu64.c's CONFIGURATION. `polygons++` is
 * unconditional on the 5-bit path but `#ifdef EMU64_FIXES` on the 7-bit one
 * (emu64.c:4886, :4899, :4917), and the walk below mirrors that with its own
 * #ifdef. The two agree today only because EMU64_FIXES is defined NOWHERE in
 * the tree. The day it arrives as anything other than a global -D reaching
 * both TUs, poly_add drifts by one per 7-bit face on every culled batch. Fail
 * the build instead: if it is ever defined here, someone has to come and check
 * that emu64.c sees it too. */
#ifdef EMU64_FIXES
#error "EMU64_FIXES is now visible to dc_emu64_cull.cpp. Confirm emu64.c sees the same definition, then delete this guard -- the 7-bit polygons++ replication depends on the two TUs agreeing."
#endif

/* emu64's members are all private (emu64.hpp:722). Access control has no
 * effect on layout, and this file is in dc/, not src/ — we are not editing the
 * class, we are reading the layout it already has. Same three defines, and the
 * same fastcast_float rename, as dc_emu64_shadow.cpp:89-102: emu64.hpp defines
 * fastcast_float four times WITHOUT `inline`, so a second including TU is four
 * duplicate definitions at link time. */
#define private   public
#define protected public
#define fastcast_float dc_cull_unused_fastcast_float

#include "libforest/emu64/emu64.hpp"

#undef fastcast_float
#undef protected
#undef private

extern "C" {

/* The frustum test, shared with dc_gx.c's flush-time cull. ⚠️ It reads g_gx,
 * so the caller owes it a current projection matrix and a current pos-matrix
 * slot — see the ordering rule in cull_batch() below. */
int dc_gx_aabb_is_offscreen(const float *mn, const float *mx);

/* dc_gx.c's own late cull, for the VERIFY build only: the delta across the
 * original handler tells us whether the reference path agreed with us.
 * ⚠️ THIS ONE AND NOT dc_gx_phase_culled_verts — that is a per-FRAME snapshot
 * published in dc_gx_frame_timing_snapshot() (dc_gx.c:764) and it does not
 * move between two batches. pc_gx_culled_draws is incremented live at the
 * rejection site (dc_gx.c:705) and reset per frame at :923, which is what a
 * per-batch delta needs. */
extern int pc_gx_culled_draws;

/* dc_gx.c:72-76. The late cull's kill switch, as a link-time constant. G3 is a
 * SECOND frustum cull, and -DDC_NO_BATCH_CULL used to mean "no culling at all";
 * without this it would mean "no LATE culling", which is not the switch anyone
 * reaching for it wants. */
extern const int dc_gx_batch_cull;

/* dc_gx.c returns early from dc_gx_flush_vertices on a frameskipped tick
 * (dc_gx.c:684) — BEFORE the cull test — so pc_gx_culled_draws cannot move
 * there and the VERIFY comparison below has to know. */
extern int g_pc_frameskip_active;

/* G-B. The vertex-index side channel into dc_gx.c's GXPosition3f32. See the
 * block comment above DCGXVertex in dc/include/dc_gx_internal.h; the condition
 * below is that header's, restated rather than pulling the whole GX state
 * struct into a C++ TU that only needs two function names. */
#if !defined(DC_GX_NO_VTXID) && !defined(DC_GX_FAT_VERTEX)
#define DC_GX_VTXID 1
void dc_gx_vtxid_arm(const unsigned char *ids, int n);
void dc_gx_vtxid_disarm(void);
#endif

void dc_emu64_cull_init(void);
void dc_emu64_cull_frame_open(unsigned int tick);
void dc_emu64_cull_report(void);

} /* extern "C" */

/* =============================================================================
 * DECAL-Z ARMING — G-B REACH: ARM THE DECAL-Z BATCHES, STILL NEVER CULL THEM
 * =============================================================================
 * ⭐ DEFAULT ON since 2026-08-09 (batch S14). Kill switch:
 * -DDC_GX_NO_VTXID_DECAL, which restores the old behaviour exactly, down to
 * the [EMU64C] log format. The old opt-IN spelling -DDC_GX_VTXID_DECAL is
 * still accepted and is now a no-op on top of the default.
 *
 * WHY THE DEFAULT FLIPPED. The gate had already passed on the OFF-by-default
 * build — `vidchk=15,835,845 vidbad=0 over=0` — and the reach it buys is
 * `vid=1800/61920 -> 2670/72810`, +48 % batches and +18 % refs. What kept it
 * off was that the expected `us/v` movement sits AT the ±2 % Flycast noise
 * floor. That is an argument about the EMULATOR's resolving power, not about
 * the change: what the side channel deletes is a random read into verts[] that
 * MISSES THE OPERAND CACHE, and Flycast models no cache at all. On hardware
 * the same change is worth strictly more than the floor Flycast can see, and
 * hardware is the target. Shipped ON; the kill switch is the retreat.
 *
 * THE ARGUMENT, PRESERVED HERE BECAUSE IT IS THE WHOLE JUSTIFICATION:
 *
 * The three punts in cull_batch() were all written for CULLING, and culling is
 * the strong operation — skipping a batch skips a draw AND skips the in-place
 * vertex mutations set_position() performs on the way, and it does so on the
 * strength of an AABB that may not even describe the geometry that would have
 * been drawn. ARMING SKIPS NOTHING. The original handler still runs, every
 * reference, in the same order, doing exactly what it did before; all the side
 * channel does is tell dc_gx.c which emu64 index each successive
 * GXPosition3f32 call belongs to. So arming clears a much weaker bar:
 *
 *     the same index must stage a BYTE-IDENTICAL DCGXVertex
 *     within this submit.
 *
 * Against that bar the three punts split two-to-one:
 *
 *   PUNT 1, decal-Z (emu64.c:2724-2783) — MEETS IT. set_position() there
 *     computes a TRANSIENT projected, z-biased position and hands it straight
 *     to GXPosition3f32; it never writes it back to emu_vtx->position. Every
 *     reference recomputes it from the same stored position through the same
 *     projection_mtx and position_mtx_stack[mtx_stack_size], none of which move
 *     inside a batch, so two references to one index stage identical vertices.
 *     It still FAILS the CULL's bar — our box is built from the raw positions
 *     and says nothing about where the reprojected geometry lands — so this
 *     path arms and is NEVER frustum-tested.
 *
 *   PUNT 2, G_TEXTURE_GEN (emu64.c:2710-2721) — FAILS IT. The normal is
 *     multiplied by model_view_mtx IN PLACE on EVERY reference, with no
 *     idempotence flag. Reference 2 of an index stages a different normal than
 *     reference 1. Stays punted whatever happens here — and measurement says it
 *     costs nothing anyway (ptgen=0).
 *
 *   PUNT 3, mixed MTX_NONSHARED (emu64.c:2693-2709) — FAILS IT. A disagreeing
 *     vertex is transformed in place on FIRST touch and its flag flipped, so
 *     again reference 1 and reference 2 stage different vertices. Stays punted,
 *     and it is RE-CHECKED on the decal path below: being a decal batch does
 *     not excuse being a mixed batch, and the two conditions are independent.
 *
 * WHAT IT IS WORTH. Measured over a 30-frame window, 2026-08-09:
 *     punt=1560 pdec=900 ptgen=0 pmix=660   against   vis=1770 armed batches.
 * PUNT 1 alone is ~900 batches, so lifting it takes armed batches 1770 -> 2670,
 * +51 % reach for the memo. `viddec=` on the report line is the measured half
 * of that claim; `pdec=` keeps counting the same thing it always did, so the
 * two are directly comparable across the A/B.
 */
#if defined(DC_GX_VTXID_DECAL) && !defined(DC_GX_VTXID)
#error "DC_GX_VTXID_DECAL widens the G-B side channel, but DC_GX_NO_VTXID or DC_GX_FAT_VERTEX has compiled that channel out of this build. Drop one of the two."
#endif
/* Arming is a property OF the side channel, so it can only exist where the
 * channel does: -DDC_GX_NO_VTXID / -DDC_GX_FAT_VERTEX take this with them,
 * silently and correctly, because DC_GX_VTXID is not defined in that build. */
#if defined(DC_GX_VTXID) && !defined(DC_GX_NO_VTXID_DECAL)
#define CU_DECAL_ARM 1
#endif

/* ⚠️ sizeof is the cheap half of the tripwire — it catches a header edit or a
 * toolchain bump that moves members, not a change that keeps the size and
 * moves a member inside it. The runtime signature check in
 * dc_emu64_cull_init() is the other half. */
#if defined(__SH4__) || defined(__sh__)
static_assert(sizeof(emu64) == 0x2278,
              "emu64 layout drifted -- G3's member accesses are no longer the "
              "ones emu64.c compiles. Re-dump with llvm-dwarfdump on "
              "dc/build/obj/src/static/libforest/emu64/emu64.c.o before "
              "touching this number.");
#endif

/* The Itanium pointer-to-member layout the dispatcher reads. Identical to
 * dc_emu64_hist.c:89; all 64 `adj` are 0, so a handler is exactly
 * void f(emu64*) with `this` in r4. */
typedef struct { void (*fn)(void *self); int adj; } CuPmf;

/* The mangled table, promoted to global by dc/Makefile's objcopy. Writing the
 * mangled name as a plain C identifier is correct on this toolchain — the
 * compiler adds the leading `_` itself. */
extern "C" CuPmf _ZL11dl_func_tbl[64];

/* The two originals, named to sanity-check the table before touching it. */
extern "C" void _ZN5emu649dl_G_TRINEv(void *self);
extern "C" void _ZN5emu6418dl_G_TRIN_INDEPENDEv(void *self);

/* Table order from dc_emu64_hist.c:186-203 (which derives it from
 * emu64.c:5703-5766): 59 = TRIN, 60 = TRIN_INDEPEND. */
#define CU_TRIN        59
#define CU_TRIN_INDEP  60

static CuPmf s_orig[64];
static int   s_ready  = 0;
/* ⚠️ STARTS DISARMED. With DC_EMU64_CULL=N>1 the sampling period is decided in
 * dc_emu64_cull_frame_open(); initialising this to 1 would cull every batch
 * dispatched before the first frame open regardless of the period, which is a
 * silently wrong A/B. frame_open runs on every tick, so the cost of starting
 * disarmed is at most one tick. */
static int   s_active = 0;

/* Counters. `s_trin..s_refs` are PER REPORT WINDOW and cleared by
 * dc_emu64_cull_report(); the three below the line are CUMULATIVE for the run,
 * deliberately — each of them is a "must be zero" and a per-window view of a
 * fault that fired once would hide it. The report labels them. */
static unsigned int s_trin, s_cull, s_vis, s_punt, s_refs;
static unsigned int s_reinst;
#ifdef DC_GX_VTXID
/* G-B. `vid=` batches armed / references handed to the side channel, per
 * window. `vid=0` on a town run means the channel is dead and every memo
 * lookup fell back to the hash — which is the free falsification. */
static unsigned int s_vid_armed, s_vid_refs;
/* Which punt costs the side channel its reach. The three are NOT equal under
 * the weaker bar arming has to clear -- see the note above each punt. */
static unsigned int s_punt_dec, s_punt_tgen, s_punt_mix;
/* Cumulative, and a must-be-zero: the reference list outgrew its structural
 * bound, so the batch went unarmed rather than desynced. */
static unsigned int s_vid_over;
#endif
#ifdef CU_DECAL_ARM
/* Per window, like s_vid_armed and s_punt_dec, so `pdec=` and `viddec=` can be
 * read against each other on one line. It is a SUBSET of s_vid_armed: a decal
 * batch that arms bumps both, which is why vid= may now exceed vis=. The
 * batches counted here are also still counted in punt=/pdec=, exactly once
 * each, so trin = cull + vis + punt and punt = pdec + ptgen + pmix both still
 * hold and the A/B against the OFF build stays honest. pdec= - viddec= is the
 * set of decal batches that reached the arm site and were refused there (mixed
 * MTX_NONSHARED, or the structural bound). */
static unsigned int s_vid_decal;
#endif
#ifdef DC_EMU64_CULL_VERIFY
static unsigned int s_falsecull, s_gfxp_bad, s_skipped;
#endif

/* ==========================================================================
 * CU_TIMED — the bracket this file has never had
 * ==========================================================================
 * ⚠️ THE GAP THIS CLOSES, STATED PLAINLY: until now this file contained ZERO
 * dc_time_us() reads. G3 does not REPLACE the late cull in dc_gx.c, it ADDS a
 * second one — a punted or visible batch falls through to GXEnd and is tested
 * again — and only the SECOND set is inside [PHASE]'s `cull=` bracket. So the
 * project's only published cull cost (0.70 ms of a ~30 ms draw) was the *late*
 * cull alone, and every re-costing of a frustum-test idea against it was
 * measuring the smaller half. Anything that changes dc_gx_aabb_is_offscreen()
 * — Gribb-Hartmann, an FTRV form, a plane cache — has to be judged against
 * `cus=`, not against `cull=`.
 *
 * "visible" is also this test's MOST expensive answer, because it is the one
 * with no early-out: every plane has to fail before the box is declared inside.
 * A cull-heavy view is therefore CHEAPER here than a cull-light one, and
 * comparing `cus=` across two different towns is comparing two different
 * workloads. Read it against `trin=` on the same line, never on its own.
 *
 * Under -DDC_PERF_PHASE only, which is the same gate dc_gx.c's `xform` bracket
 * uses; a shipping build compiles this to the bare call and the accumulator
 * away. Two dc_time_us() reads per TRIN batch is real overhead inside the
 * interval it is reporting — that is what makes it a perf-build instrument and
 * not a default. */
/* THE SPLIT, AND WHY IT IS NOT OPTIONAL. The first version of this bracket
 * timed cull_batch() whole and read 6.8 ms/frame — 23 % of a 29.4 ms draw —
 * which reads as "the frustum test is enormous" and is NOT what it says.
 * cull_batch() also makes two emu64 member calls (dirty_check,
 * setup_1tri_2tri_1quad) that are emu64's own state work, and the cross-check
 * that caught it is [PHASE]'s `cull=`: the SAME frustum test, wrapped in the
 * SAME two-dc_time_us pattern on the late path, costs 4.74 us a batch against
 * this site's 27.2. A bracket around a compound is a number nobody can act on.
 *
 *   cus= total inside cull_batch()      (the old, ambiguous number)
 *   cds= dirty_check + setup_1tri...    emu64 state work, NOT the cull
 *   fus= dc_gx_aabb_is_offscreen()      the frustum test alone
 *
 * cus - cds - fus is the index walk, the AABB build and the punts.
 *
 * ⚠️ THREE NESTED BRACKETS MEANS UP TO SIX dc_time_us() READS PER BATCH, and
 * the inner two are charged to the outer one. `fus=` and `cds=` are therefore
 * OVERSTATED by one read-pair each and `cus=` by three. That is acceptable for
 * apportioning a 6.8 ms block and is NOT acceptable for costing a change worth
 * a few hundred microseconds — for that, bracket one thing at a time. */
#ifdef DC_PERF_PHASE
static unsigned int s_cull_us;      /* per window, microseconds inside cull_batch */
static unsigned int s_setup_us;     /* ...of which: emu64's dirty_check + setup   */
static unsigned int s_frustum_us;   /* ...of which: the frustum test alone        */
#define CU_TIMED(expr)                                                        \
    ({                                                                        \
        u64 cu_t0_ = dc_time_us();                                            \
        int cu_r_ = (expr);                                                   \
        s_cull_us += (unsigned int)(dc_time_us() - cu_t0_);                   \
        cu_r_;                                                                \
    })
#define CU_SPLIT(acc, body)                                                   \
    do {                                                                      \
        u64 cs_t0_ = dc_time_us();                                            \
        body;                                                                 \
        (acc) += (unsigned int)(dc_time_us() - cs_t0_);                       \
    } while (0)
#else
#define CU_TIMED(expr) (expr)
#define CU_SPLIT(acc, body) do { body; } while (0)
#endif

/* ==========================================================================
 * THE INDEX WALK
 * ==========================================================================
 * A TRIN batch is a header Gfx followed by more Gfx words, all of them index
 * payload. The header word is ALSO the first index word (emu64.c:4823-4825
 * starts the loop at gfx_p == first_cmd).
 *
 *   n_faces  = ((w0 >> 17) & 0x7F) + 1          1..128 faces  (emu64.c:4814)
 *   width    = (w1 & POLY_BITMASK) == POLY_7b   RE-TESTED PER WORD (:4827)
 *   5-bit    3 faces on the first word, 4 after   (the `first_pass` branch)
 *   7-bit    2 faces on the first word, 3 after
 *
 * ⚠️ `first_pass` is shared across words and cleared exactly once, after the
 * first word's third (5b) or second (7b) face — so it is a property of the
 * BATCH, not of the word. Getting this wrong by a single word makes the
 * dispatcher execute half a triangle stream as opcodes.
 *
 * ⚠️ POLY_GET_V5_5b and POLY_GET_V4_7b STRADDLE w1 and w0. Use the macros.
 *
 * The walk ends with the local pointer one past the last payload word, and
 * emu64 leaves gfx_p at last_word - 1 (`gfx_p += n_faces - 1` at :4929 with
 * n_faces == 0 at every exit), because the dispatcher increments it again at
 * emu64.c:5874. So our culled path sets gfx_p = p - 1.
 */
#ifndef DC_GX_VTXID
#define CU_MARK(i)  (mark[(unsigned)(i) >> 5] |= 1u << ((unsigned)(i) & 31u))
#else
/* G-B. The same walk also records the reference SEQUENCE, which is the part
 * mark[] throws away: a bitset says which vertices the batch touches, and
 * GXPosition3f32 needs to know which one each successive call is for.
 *
 * The bound is structural and EXACT, not a guess: n_faces is a 7-bit field
 * plus one (emu64.c:4806) and every face marks exactly three indices, so a
 * TRIN command is at most 128 faces and 384 references. The store is still
 * bounds-tested and `nids` still counts past the cap, so an overrun writes
 * nothing and the arm site below simply declines to arm. A silently truncated
 * list would desync the cursor and hand vertices each other's transforms —
 * which is worth one compare per reference to make impossible. */
#define CU_IDS_MAX 384
static unsigned char s_ids[CU_IDS_MAX];
#define CU_MARK(i)  (mark[(unsigned)(i) >> 5] |= 1u << ((unsigned)(i) & 31u), \
                     (void)(nids < CU_IDS_MAX \
                                ? (s_ids[nids] = (unsigned char)(i)) : 0), \
                     (void)nids++)
#endif

/* ==========================================================================
 * cull_batch — returns 1 if the command was fully consumed here, 0 to punt
 * ==========================================================================
 * EVERY punt path must leave `this` exactly as it found it, because the
 * original handler runs immediately afterwards and must behave identically.
 * The walk therefore uses a LOCAL pointer and never writes this->gfx_p until
 * the cull is committed.
 */
static int cull_batch(emu64 *self)
{
    Gfx *first    = self->gfx_p;
    int  n_faces  = (int)((first->words.w0 >> 17) & 0x7F) + 1;
    int  is_7bit  = (first->words.w1 & POLY_BITMASK) == POLY_7b;
    int  first_vtx = is_7bit ? POLY_GET_V0_7b(first) : POLY_GET_V0_5b(first);
    unsigned int mark[4] = { 0u, 0u, 0u, 0u };
    Gfx *p = first;
    int  left = n_faces;
    int  first_pass = 1;
    int  poly_add = 0;
    int  want;
#ifdef CU_DECAL_ARM
    /* Set by PUNT 1 instead of returning; consumed by the decal arm block that
     * sits between the walk and dirty_check(). */
    int  decal = 0;
#endif
#ifdef DC_GX_VTXID
    int  nids = 0;

    /* Disarm FIRST, ahead of every punt below. At most one list is ever live,
     * and a batch that punts or culls falls back to the content-hashed memo
     * instead of inheriting whatever the last batch left behind. */
    dc_gx_vtxid_disarm();
#endif

    s_trin++;

    /* ---- PUNT 1: the decal-Z path (emu64.c:2724-2783) --------------------
     * set_position() does not submit emu_vtx->position there — it round-trips
     * the position through the projection, biases z, and comes back. Our box
     * is built from the raw positions, so it describes geometry that is not
     * what would be drawn. The divergence would present as z-fighting weeks
     * later, which is the worst possible failure mode for a cull. Punt, and
     * revisit only with a measurement of how many batches this is.
     *
     * ⚠️ THIS PUNT IS ABOUT CULLING AND MAY NOT APPLY TO G-B's SIDE CHANNEL.
     * Arming skips nothing — the original handler still runs every reference —
     * so all it needs is "same index ⇒ same staged vertex within this submit".
     * The decal path recomputes its transient position from the SAME
     * emu_vtx->position through the SAME matrices on every reference, so it
     * meets that bar even though it fails the cull's. Counted separately
     * (`pdec=`) so the reach this costs is a number before anyone acts on it.
     * ⭐ ACTED ON: -DDC_GX_VTXID_DECAL does exactly that, arming without ever
     * culling. The full argument is in the section above the static_assert. */
    if ((self->othermode_low & ZMODE_DEC) == ZMODE_DEC &&
        (self->geometry_mode & G_ZBUFFER) != 0 &&
        (self->geometry_mode & G_DECAL_EQUAL) == 0) {
        s_punt++;
#ifdef DC_GX_VTXID
        s_punt_dec++;
#endif
#ifdef CU_DECAL_ARM
        /* ARM, BUT DO NOT CULL. Both counters above fired exactly as they do
         * in the OFF build and exactly once for this batch, so punt= and pdec=
         * stay comparable; the new reach shows up in viddec=. Fall through to
         * the walk rather than returning.
         *
         * ⚠️ NON-NEGOTIABLE, AND THE ONE THING THAT MAKES THIS SAFE: this batch
         * must never reach dc_gx_aabb_is_offscreen(). The box we would hand it
         * is built from the raw positions and the decal path draws a
         * reprojected, z-biased position instead, so the answer would be
         * meaningless in BOTH directions — including the direction that deletes
         * visible geometry. The decal block after the walk returns 0
         * unconditionally and never falls into the box builder. */
        decal = 1;
#else
        return 0;
#endif
    }

    /* ---- PUNT 2: reflection mapping on a nonshared batch -----------------
     * emu64.c:2711-2721: with G_TEXTURE_GEN set, set_position() transforms
     * emu_vtx->normal IN PLACE and has no idempotence flag to stop it doing so
     * again on the next batch that touches the same vertex. Skipping the batch
     * therefore does not merely skip a draw, it changes how many times a
     * shared normal gets multiplied. Punt rather than reason about it.
     *
     * ⚠️ AND THIS ONE FAILS G-B's BAR TOO, for the same reason it fails the
     * cull's: the normal is re-multiplied on EVERY reference, so the second
     * reference to an index does not stage the same vertex as the first. It
     * must stay punted whatever happens to the decal case. */
    if ((self->geometry_mode & G_TEXTURE_GEN) != 0) {
#ifdef CU_DECAL_ARM
        /* ⚠️ THE ONE INTERACTION THE FALL-THROUGH CREATES. In the OFF build a
         * batch that is BOTH decal and G_TEXTURE_GEN returns at PUNT 1 and is
         * counted once, in punt= and pdec=. With the switch on it reaches this
         * test as well, so counting again here would inflate punt= and invent a
         * ptgen= the OFF build never had — the A/B would show a counter moving
         * that describes nothing. It was fully accounted one punt ago; just
         * leave. And it must NOT arm: G_TEXTURE_GEN re-multiplies the normal on
         * every reference, which fails the arming bar no matter what the ZMODE
         * bits say. Disarmed on entry, so leaving is enough. */
        if (decal) return 0;
#endif
        s_punt++;
#ifdef DC_GX_VTXID
        s_punt_tgen++;
#endif
        return 0;
    }

    /* ---- the walk. Marks EXACTLY the indices the handler would consume,
     * and counts the words, so gfx_p lands where emu64 would leave it. ---- */
    while (left > 0) {
        Gfx *g = p;
        int five = ((g->words.w1 & POLY_BITMASK) == POLY_5b);

        p++;

        if (five) {
            CU_MARK(POLY_GET_V0_5b(g)); CU_MARK(POLY_GET_V1_5b(g));
            CU_MARK(POLY_GET_V2_5b(g));
            poly_add++;                       /* emu64.c:4835, unconditional */
            if (--left == 0) break;

            CU_MARK(POLY_GET_V3_5b(g)); CU_MARK(POLY_GET_V4_5b(g));
            CU_MARK(POLY_GET_V5_5b(g));
            poly_add++;
            if (--left == 0) break;

            CU_MARK(POLY_GET_V6_5b(g)); CU_MARK(POLY_GET_V7_5b(g));
            CU_MARK(POLY_GET_V8_5b(g));
            poly_add++;
            if (--left == 0) break;

            if (first_pass) {
                first_pass = 0;               /* only 3 faces on the first */
            } else {
                CU_MARK(POLY_GET_V9_5b(g));  CU_MARK(POLY_GET_V10_5b(g));
                CU_MARK(POLY_GET_V11_5b(g));
                poly_add++;
                if (--left == 0) break;
            }
        } else {
            CU_MARK(POLY_GET_V0_7b(g)); CU_MARK(POLY_GET_V1_7b(g));
            CU_MARK(POLY_GET_V2_7b(g));
#ifdef EMU64_FIXES
            poly_add++;
#endif
            if (--left == 0) break;

            CU_MARK(POLY_GET_V3_7b(g)); CU_MARK(POLY_GET_V4_7b(g));
            CU_MARK(POLY_GET_V5_7b(g));
#ifdef EMU64_FIXES
            poly_add++;
#endif
            if (--left == 0) break;

            if (first_pass) {
                first_pass = 0;               /* only 2 faces on the first */
            } else {
                CU_MARK(POLY_GET_V6_7b(g)); CU_MARK(POLY_GET_V7_7b(g));
                CU_MARK(POLY_GET_V8_7b(g));
#ifdef EMU64_FIXES
                poly_add++;
#endif
                if (--left == 0) break;
            }
        }
    }

#ifdef CU_DECAL_ARM
    /* ---- THE DECAL ARM PATH. It arms, and it returns 0. That is all. ------
     *
     * PLACED DELIBERATELY BEFORE dirty_check() / setup_1tri_2tri_1quad(), and
     * this is a choice, not an accident. Those two calls exist here for exactly
     * one reason: g_gx.projection_mtx and g_gx.current_mtx are one command
     * stale until they run, and the FRUSTUM TEST reads both. A batch that is
     * never frustum-tested has no use for them. Making them anyway would be
     * harmless — they are idempotent, and the original handler makes the same
     * two calls at emu64.c:4812-4813 a moment later, which is precisely why the
     * existing punt paths can already leave them made or unmade — but NOT
     * making them keeps this path's observable effect on `this` byte-for-byte
     * identical to the OFF build's early return at PUNT 1, and that is the
     * property worth having when the switch is meant to be a clean A/B.
     * (setup_1tri_2tri_1quad also writes this->using_nonshared_mtx, emu64.c
     * :2862/:2866; the original handler sets it again from the same first_vtx
     * before any vertex is submitted, so skipping it here changes nothing that
     * is read before it is rewritten.)
     *
     * THE MIXED-MTX CHECK IS NOT SKIPPED. Decal and MTX_NONSHARED are
     * independent conditions, and a mixed batch fails the ARMING bar on its own
     * account: set_position() (emu64.c:2693-2709) transforms a disagreeing
     * vertex in place on first touch and flips its flag, so reference 2 of that
     * index stages a different vertex than reference 1 and the memo would hand
     * one of them the other's transform. So the same distinct-vertex ctz walk
     * over mark[] runs here, agreement-only — no box, no extrema, no frustum.
     * A decal batch that fails it simply does not arm and needs no counter of
     * its own: it is already in pdec=, and pdec= - viddec= is exactly that set.
     */
    if (decal) {
        int want_d = self->vertices[first_vtx].flag & MTX_NONSHARED;
        int agree  = 1;
        int w;

        for (w = 0; w < 4 && agree; w++) {
            unsigned int m = mark[w];
            while (m) {
                int b = __builtin_ctz(m);
                m &= m - 1u;
                if ((self->vertices[w * 32 + b].flag & MTX_NONSHARED)
                        != want_d) {
                    agree = 0;
                    break;
                }
            }
        }

        /* Same structural bound and same refusal-to-truncate as the visible
         * path below: a short list would desync the cursor, so decline instead.
         * cull_batch() already disarmed on entry, so declining leaves the
         * channel dead and every vertex of this batch falls back to the
         * content-hashed memo — which is what it did before this switch
         * existed. */
        if (agree) {
            if (nids <= CU_IDS_MAX) {
                dc_gx_vtxid_arm(s_ids, nids);
                s_vid_armed++;
                s_vid_refs += (unsigned int)nids;
                s_vid_decal++;
            } else {
                s_vid_over++;
            }
        }
        return 0;
    }
#endif /* CU_DECAL_ARM */

    /* ---- THE ORDERING RULE, AND IT IS THE ONE THAT DELETES GEOMETRY IF
     * BROKEN. g_gx.projection_mtx is refreshed only by GXSetProjection inside
     * dirty_check (emu64.c:3430-3436), and g_gx.current_mtx only by
     * setup_1tri_2tri_1quad (emu64.c:2861/2865). Test before either and the
     * frustum is one command stale — which culls things that are on screen.
     * These are exactly the two calls the original handler makes first
     * (emu64.c:4812-4813); on the punt path it simply makes them again, which
     * is idempotent (all dirty flags cleared, GXSetCurrentMtx dedups). */
    /* ⚠️ BRACKETED SEPARATELY, BECAUSE `cus=` WITHOUT THIS SPLIT IS AN
     * UNREADABLE NUMBER. These two are emu64's OWN state work, not the cull:
     * on a batch we cull they REPLACE the handler's copies, and on a batch we
     * do not cull the handler makes them again (the comment above says that is
     * idempotent — `cds=` is the first measurement of whether idempotent also
     * means cheap). Charging them to the frustum test made the cull look ~6x
     * more expensive than the identical test costs on the late path. */
    CU_SPLIT(s_setup_us, {
        self->dirty_check(self->texture_gfx.tile, self->texture_gfx.level, TRUE);
        self->setup_1tri_2tri_1quad((unsigned int)first_vtx);
    });

    /* ---- the box, over the DISTINCT referenced vertices ------------------
     * ⚠️ MIXED-FLAG BATCHES PUNT. A batch's matrix is chosen from the FIRST
     * vertex's flag alone (emu64.c:2856-2866), and set_position() then fixes
     * up disagreeing vertices ONE AT A TIME (:2687-2707) — transforming a
     * shared-flagged vertex in place and flipping its flag. Two things follow:
     * a disagreeing vertex is in a different space from the box we are
     * building, and skipping the batch skips a mutation the game expects. Both
     * say punt. */
    want = self->vertices[first_vtx].flag & MTX_NONSHARED;
    {
        float n0, n1, n2, x0, x1, x2;
        float mn[3], mx[3];
        int seeded = 0;
        int w;

        for (w = 0; w < 4; w++) {
            unsigned int m = mark[w];
            while (m) {
                int b = __builtin_ctz(m);
                Vertex *v = &self->vertices[w * 32 + b];
                float a, c, d;
                m &= m - 1u;

                /* ⚠️ ALSO FAILS G-B's BAR: set_position() transforms a
                 * disagreeing vertex in place on FIRST touch and flips its
                 * flag, so reference 1 and reference 2 of that index stage
                 * different vertices. Stays punted. */
                if ((v->flag & MTX_NONSHARED) != want) {
                    s_punt++;
#ifdef DC_GX_VTXID
                    s_punt_mix++;
#endif
                    return 0;
                }

                a = v->position.x; c = v->position.y; d = v->position.z;
                if (!seeded) {
                    n0 = x0 = a; n1 = x1 = c; n2 = x2 = d;
                    seeded = 1;
                } else {
                    /* Same else-if extrema form as dc_gx.c's fast AABB: after
                     * the seed both extrema start equal, so a value below the
                     * running minimum is necessarily below the running
                     * maximum, and NaN takes neither branch — which keeps the
                     * cull conservative. */
                    if (a < n0) n0 = a; else if (a > x0) x0 = a;
                    if (c < n1) n1 = c; else if (c > x1) x1 = c;
                    if (d < n2) n2 = d; else if (d > x2) x2 = d;
                }
            }
        }
        if (!seeded) { s_punt++; return 0; }

        mn[0] = n0; mn[1] = n1; mn[2] = n2;
        mx[0] = x0; mx[1] = x1; mx[2] = x2;

        /* The frustum test ALONE — the thing G-F and S14-5 actually change.
         * Compare `fus=` against [PHASE]'s `cull=`: same test, same 2-read
         * bracket pattern, two different call sites. */
        int off_screen;
        CU_SPLIT(s_frustum_us, { off_screen = dc_gx_aabb_is_offscreen(mn, mx); });
        if (!off_screen) {
            s_vis++;
#ifdef DC_GX_VTXID
            /* G-B. THE ONLY PLACE THE SIDE CHANNEL IS ARMED, and every
             * condition that makes it sound has already been checked above:
             * the batch is about to be DRAWN by the original handler, the
             * decal-Z and G_TEXTURE_GEN punts did not fire, and every
             * referenced vertex agreed on MTX_NONSHARED. Those three are
             * exactly the cases in which set_position() mutates vertices[]
             * mid-batch — so on this path "same index ⇒ byte-identical staged
             * vertex" is a property of emu64's own code, not an assumption.
             *
             * A batch we CULL never arms, because it emits no vertices at
             * all. `nids <= CU_IDS_MAX` is structurally guaranteed; if it ever
             * is not, the list is short and arming it would desync, so don't.
             */
            if (nids <= CU_IDS_MAX) {
                dc_gx_vtxid_arm(s_ids, nids);
                s_vid_armed++;
                s_vid_refs += (unsigned int)nids;
            } else {
                s_vid_over++;
            }
#endif
            return 0;
        }
    }

    /* ---- COMMITTED. Replicate the handler's non-drawing side effects. ----
     * GXBegin/GXEnd are deliberately NOT replicated: they are a matched pair
     * around zero submitted vertices, i.e. a no-op. `polygons` and
     * `rdp_pipe_sync_needed` are (emu64.c:4835, :4939) — `polygons` only feeds
     * EMU64_CAN_DRAW_POLYGON(), which is unconditionally true because
     * AFLAGS_MAX is 0, so this is belt-and-braces at the cost of two
     * instructions. */
    self->gfx_p = p - 1;
    self->polygons += (unsigned int)poly_add;
    self->rdp_pipe_sync_needed = true;

    s_cull++;
    s_refs += (unsigned int)(n_faces * 3);
    return 1;
}

/* ==========================================================================
 * The trampolines
 * ========================================================================== */
#ifdef DC_EMU64_CULL_VERIFY
/* VERIFY MODE: never actually cull. Run our test, then run the original
 * handler regardless, and check that dc_gx.c's flush-time cull agreed. Any
 * batch we would have dropped that the reference path drew is a `falsecull`,
 * and falsecull MUST be 0. Also checks that our walk lands gfx_p exactly where
 * emu64 does — one word wrong there is catastrophic, and silent.
 *
 * ⚠️ This mode is SLOWER than an uninstrumented build, by construction. It is
 * a correctness gate, never a perf run. */
static void cu_verify(emu64 *self, void (*orig)(void *))
{
    Gfx *before = self->gfx_p;
    int culled_before = pc_gx_culled_draws;
    unsigned int poly_before = self->polygons;
    int would_cull;
    Gfx *our_gfxp;
    /* ⚠️ SAMPLED BEFORE the trial. dl_G_TRIN runs on frameskipped ticks like
     * any other, but dc_gx_flush_vertices returns at dc_gx.c:684 BEFORE the
     * cull test on those ticks — so pc_gx_culled_draws CANNOT move and every
     * correct cull would be recorded as a falsecull. Counting those separately
     * is the difference between a gate and a number that is always nonzero. */
    int comparable = !g_pc_frameskip_active;

    /* Honour the sampling period even here: DC_EMU64_CULL=N>1 in a VERIFY
     * build must exercise the same batches an armed build would. */
    if (!s_active) { orig(self); return; }

    would_cull = cull_batch(self);
    our_gfxp = self->gfx_p;
    /* Undo the whole commit — the original handler re-walks from scratch and
     * must see exactly the state the dispatcher handed us. */
    self->gfx_p   = before;
    self->polygons = poly_before;

    orig(self);

    if (would_cull) {
        if (self->gfx_p != our_gfxp) s_gfxp_bad++;
        if (!comparable) {
            s_skipped++;
        } else if (pc_gx_culled_draws == culled_before) {
            /* The reference cull is a whole-batch decision taken at flush
             * time, which dl_G_TRIN reaches through its own GXEnd
             * (emu64.c:4935) before returning. No delta therefore means it
             * DREW what we would have dropped, which is the only failure that
             * matters. */
            s_falsecull++;
        }
    }
}
static void cu_trin(void *p)       { cu_verify((emu64 *)p, s_orig[CU_TRIN].fn); }
static void cu_trin_indep(void *p) { cu_verify((emu64 *)p, s_orig[CU_TRIN_INDEP].fn); }
#else
/* ⚠️ `dc_gx_batch_cull` IS IN THE CONDITION ON PURPOSE. -DDC_NO_BATCH_CULL has
 * always meant "turn the frustum cull off"; G3 is a second frustum cull, and
 * without this test that switch would quietly become "turn off only the LATE
 * half" — leaving the entry half culling with nothing to cross-check it. It is
 * a link-time const, so this folds away in every normal build. */
static void cu_trin(void *p) {
    emu64 *self = (emu64 *)p;
    if (s_active && dc_gx_batch_cull && CU_TIMED(cull_batch(self))) return;
    s_orig[CU_TRIN].fn(p);
}
static void cu_trin_indep(void *p) {
    emu64 *self = (emu64 *)p;
    if (s_active && dc_gx_batch_cull && CU_TIMED(cull_batch(self))) return;
    s_orig[CU_TRIN_INDEP].fn(p);
}
#endif

/* ==========================================================================
 * Install / arm / report
 * ========================================================================== */
extern "C" void dc_emu64_cull_init(void)
{
    int i;

    for (i = 0; i < 64; i++) s_orig[i] = _ZL11dl_func_tbl[i];

    /* Fail LOUDLY rather than rewrite a table we do not recognise. Same
     * discipline as dc_emu64_hist.c:236-249: if the mangling, the table order
     * or the PMF layout ever changes, this is the difference between "the cull
     * is off" and "the game jumps into hyperspace". */
    if (s_orig[CU_TRIN].fn       != (void (*)(void *))_ZN5emu649dl_G_TRINEv ||
        s_orig[CU_TRIN_INDEP].fn != (void (*)(void *))_ZN5emu6418dl_G_TRIN_INDEPENDEv ||
        s_orig[CU_TRIN].adj != 0 || s_orig[CU_TRIN_INDEP].adj != 0) {
        DC_LOGE("[EMU64C] DISABLED: dispatch table signature mismatch "
                "(trin=%p indep=%p adj=%d,%d)\n",
                (void *)s_orig[CU_TRIN].fn, (void *)s_orig[CU_TRIN_INDEP].fn,
                s_orig[CU_TRIN].adj, s_orig[CU_TRIN_INDEP].adj);
        return;
    }

    /* SLOT-WISE, ONCE, AND NEVER AGAIN — see the header comment. adj is
     * already 0 in both slots and we just asserted it. */
    _ZL11dl_func_tbl[CU_TRIN].fn       = cu_trin;
    _ZL11dl_func_tbl[CU_TRIN_INDEP].fn = cu_trin_indep;
    s_ready = 1;

    DC_LOGE("[EMU64C] armed, mode=%d%s\n", (int)(DC_EMU64_CULL),
#ifdef DC_EMU64_CULL_VERIFY
            " VERIFY (never culls; gate on falsecull=0)"
#else
            ""
#endif
           );
}

void dc_emu64_cull_frame_open(unsigned int tick)
{
    if (!s_ready) return;

    /* The A/B is a boolean read inside the trampoline, NOT a table swap: G1
     * and G2 memcpy all 64 slots per frame and a second writer would race
     * them. DC_EMU64_CULL=1 -> always armed; N>1 -> one frame in N. */
    s_active = ((DC_EMU64_CULL) <= 1) ||
               ((tick % (unsigned int)(DC_EMU64_CULL)) == 0u);

    /* The tripwire. If G1 or G2 initialised BEFORE us, their snapshot holds
     * the original handlers and their per-frame restore has just evicted us.
     * Re-install and count it; a non-zero reinst= is a build-order bug report,
     * not a repair. */
    if (_ZL11dl_func_tbl[CU_TRIN_INDEP].fn != cu_trin_indep) {
        _ZL11dl_func_tbl[CU_TRIN].fn       = cu_trin;
        _ZL11dl_func_tbl[CU_TRIN_INDEP].fn = cu_trin_indep;
        s_reinst++;
    }
}

void dc_emu64_cull_report(void)
{
    if (!s_ready) return;
    /* The first five are this window; everything after `|` is CUMULATIVE for
     * the run and must read zero. `nocmp=` is not a fault — it is the number
     * of culls taken on a frameskipped tick, where dc_gx.c cannot answer. */
    DC_LOGE("[EMU64C] trin=%u cull=%u vis=%u punt=%u refs=%u"
#ifdef DC_PERF_PHASE
            /* Microseconds spent INSIDE cull_batch() this window — G3's own
             * frustum test, which [PHASE]'s `cull=` has never included. Read
             * it against trin= on the same line. */
            " cus=%u cds=%u fus=%u"
#endif
#ifdef DC_GX_VTXID
            " vid=%u/%u over=%u pdec=%u"
#ifdef CU_DECAL_ARM
            /* Of the pdec= decal batches, how many the side channel reached.
             * Also a subset of vid=. Absent entirely when the switch is off, so
             * the default build's log line is unchanged. */
            " viddec=%u"
#endif
            " ptgen=%u pmix=%u"
#endif
            " | cum reinst=%u"
#ifdef DC_EMU64_CULL_VERIFY
            " falsecull=%u gfxp_bad=%u nocmp=%u"
#endif
            "\n",
            s_trin, s_cull, s_vis, s_punt, s_refs,
#ifdef DC_PERF_PHASE
            s_cull_us, s_setup_us, s_frustum_us,
#endif
#ifdef DC_GX_VTXID
            s_vid_armed, s_vid_refs, s_vid_over,
            s_punt_dec,
#ifdef CU_DECAL_ARM
            s_vid_decal,
#endif
            s_punt_tgen, s_punt_mix,
#endif
            s_reinst
#ifdef DC_EMU64_CULL_VERIFY
            , s_falsecull, s_gfxp_bad, s_skipped
#endif
           );
    s_trin = s_cull = s_vis = s_punt = s_refs = 0;
#ifdef DC_PERF_PHASE
    s_cull_us = s_setup_us = s_frustum_us = 0;
#endif
#ifdef DC_GX_VTXID
    s_vid_armed = s_vid_refs = 0;
    s_punt_dec = s_punt_tgen = s_punt_mix = 0;
#endif
#ifdef CU_DECAL_ARM
    s_vid_decal = 0;
#endif
}

#endif /* DC_EMU64_CULL */
