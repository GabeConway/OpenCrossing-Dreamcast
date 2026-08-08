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
 * The test is not a new test. It is the SAME frustum test dc_gx.c already
 * runs — dc_gx_aabb_is_offscreen(), split out of dc_gx_batch_is_offscreen()
 * for this file — handed a box built from emu64's own vertex array instead of
 * from our staged vertex buffer. Two frustum tests that must agree is how a
 * cull starts deleting visible geometry; there is one, and G3 feeds it.
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
 * image. The verdict on a run is `[EMU64C] falsecull=0` under
 * -DDC_EMU64_CULL_VERIFY, which needs no screenshot: in verify mode every
 * batch we would cull is ALSO handed to the original handler, and we count any
 * batch our entry test rejected that dc_gx.c's flush-time test did not.
 * A screenshot pair is still required for the shipping build (rule 2).
 */
#include "dc_platform.h"

#if defined(DC_EMU64_CULL) && DC_EMU64_CULL > 0

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

void dc_emu64_cull_init(void);
void dc_emu64_cull_frame_open(unsigned int tick);
void dc_emu64_cull_report(void);

} /* extern "C" */

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
static int   s_active = 1;

/* Counters. Printed as one [EMU64C] line, cleared per report window. */
static unsigned int s_trin, s_cull, s_vis, s_punt, s_refs, s_reinst;
#ifdef DC_EMU64_CULL_VERIFY
static unsigned int s_falsecull, s_gfxp_bad;
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
#define CU_MARK(i)  (mark[(unsigned)(i) >> 5] |= 1u << ((unsigned)(i) & 31u))

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

    s_trin++;

    /* ---- PUNT 1: the decal-Z path (emu64.c:2724-2783) --------------------
     * set_position() does not submit emu_vtx->position there — it round-trips
     * the position through the projection, biases z, and comes back. Our box
     * is built from the raw positions, so it describes geometry that is not
     * what would be drawn. The divergence would present as z-fighting weeks
     * later, which is the worst possible failure mode for a cull. Punt, and
     * revisit only with a measurement of how many batches this is. */
    if ((self->othermode_low & ZMODE_DEC) == ZMODE_DEC &&
        (self->geometry_mode & G_ZBUFFER) != 0 &&
        (self->geometry_mode & G_DECAL_EQUAL) == 0) {
        s_punt++;
        return 0;
    }

    /* ---- PUNT 2: reflection mapping on a nonshared batch -----------------
     * emu64.c:2711-2721: with G_TEXTURE_GEN set, set_position() transforms
     * emu_vtx->normal IN PLACE and has no idempotence flag to stop it doing so
     * again on the next batch that touches the same vertex. Skipping the batch
     * therefore does not merely skip a draw, it changes how many times a
     * shared normal gets multiplied. Punt rather than reason about it. */
    if ((self->geometry_mode & G_TEXTURE_GEN) != 0) {
        s_punt++;
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

    /* ---- THE ORDERING RULE, AND IT IS THE ONE THAT DELETES GEOMETRY IF
     * BROKEN. g_gx.projection_mtx is refreshed only by GXSetProjection inside
     * dirty_check (emu64.c:3430-3436), and g_gx.current_mtx only by
     * setup_1tri_2tri_1quad (emu64.c:2861/2865). Test before either and the
     * frustum is one command stale — which culls things that are on screen.
     * These are exactly the two calls the original handler makes first
     * (emu64.c:4812-4813); on the punt path it simply makes them again, which
     * is idempotent (all dirty flags cleared, GXSetCurrentMtx dedups). */
    self->dirty_check(self->texture_gfx.tile, self->texture_gfx.level, TRUE);
    self->setup_1tri_2tri_1quad((unsigned int)first_vtx);

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

                if ((v->flag & MTX_NONSHARED) != want) { s_punt++; return 0; }

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

        if (!dc_gx_aabb_is_offscreen(mn, mx)) { s_vis++; return 0; }
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

    would_cull = cull_batch(self);
    our_gfxp = self->gfx_p;
    /* Undo the whole commit — the original handler re-walks from scratch and
     * must see exactly the state the dispatcher handed us. */
    self->gfx_p   = before;
    self->polygons = poly_before;

    orig(self);

    if (would_cull) {
        if (self->gfx_p != our_gfxp) s_gfxp_bad++;
        /* The reference cull is a whole-batch decision taken at flush time,
         * which dl_G_TRIN reaches through its own GXEnd (emu64.c:4935) before
         * returning. No delta therefore means it DREW what we would have
         * dropped, which is the only failure that matters. */
        if (pc_gx_culled_draws == culled_before) s_falsecull++;
    }
}
static void cu_trin(void *p)       { cu_verify((emu64 *)p, s_orig[CU_TRIN].fn); }
static void cu_trin_indep(void *p) { cu_verify((emu64 *)p, s_orig[CU_TRIN_INDEP].fn); }
#else
static void cu_trin(void *p) {
    emu64 *self = (emu64 *)p;
    if (s_active && cull_batch(self)) return;
    s_orig[CU_TRIN].fn(p);
}
static void cu_trin_indep(void *p) {
    emu64 *self = (emu64 *)p;
    if (s_active && cull_batch(self)) return;
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
    DC_LOGE("[EMU64C] trin=%u cull=%u vis=%u punt=%u refs=%u reinst=%u"
#ifdef DC_EMU64_CULL_VERIFY
            " falsecull=%u gfxp_bad=%u"
#endif
            "\n",
            s_trin, s_cull, s_vis, s_punt, s_refs, s_reinst
#ifdef DC_EMU64_CULL_VERIFY
            , s_falsecull, s_gfxp_bad
#endif
           );
    s_trin = s_cull = s_vis = s_punt = s_refs = 0;
}

#endif /* DC_EMU64_CULL */
