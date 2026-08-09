/* dc_pvr.c - the PowerVR backend: init, frame, SH-4 T&L, near clip, submit.
 *
 * This file is one half of the seam declared in dc_gx_internal.h; the other
 * half (texture decode -> VRAM) is dc_pvr_texture.c. Nothing above the seam
 * knows a tile exists, and nothing here knows what a GXTexObj is.
 *
 * ==========================================================================
 * THE FOUR DECISIONS THIS FILE MAKES, AND WHY
 * ==========================================================================
 *
 * 1. TWO LISTS: one general list, plus punch-through.  The PVR bins geometry
 *    into Opaque / Punch-Through / Translucent lists, and KOS is explicit that
 *    "lists can never be opened again within a single frame once they have
 *    been closed". The game hands us OP-ish and TR-ish batches finely
 *    interleaved in emu64 display-list order, so honouring OP *and* TR would
 *    mean buffering one of them in main RAM until frame end — and main RAM is
 *    the project's blocking problem (kb/STATE.md).
 *
 *    So: everything that is not a hard cutout goes into PVR_LIST_TR_POLY with
 *    autosort DISABLED. That turns the translucent list into a plain
 *    submission-ordered, Z-buffered rasteriser with per-polygon blend factors —
 *    which is exactly the GX semantics the game was written against. Opaque
 *    geometry is simply a poly with src=ONE dst=ZERO. Nothing is reordered.
 *
 *    The cost is the PVR's early-Z rejection, which the TR list does not do.
 *    That is a frame-rate problem, not a correctness problem, and it is the
 *    right trade for a bring-up. DC_PVR_LIST_MODE=1 puts everything in the
 *    opaque list instead (faster, no blending) for bisecting.
 *
 *    THE ONE EXCEPTION IS THE PUNCH-THROUGH LIST, added 2026-08-02. The PVR has
 *    no alpha test outside PVR_LIST_PT_POLY, so `alpha_ref` (144 by default,
 *    emu64.c:718) was read only to decide THAT a test existed and never applied
 *    as a threshold. For a train door with alpha-punched window openings that
 *    leaves no right answer: depth.write=true makes the transparent holes write
 *    depth and occlude the scenery behind them, depth.write=false makes the
 *    door occlude nothing and the passing trees paint through it. BOTH were
 *    built, both were watched by a human, both were wrong (kb/traps.md).
 *
 *    PVR_LIST_PT_POLY = 4 is LAST in the TA's list order, so cutouts cannot be
 *    interleaved: they are transformed at submission time into a small deferred
 *    record buffer (headers and vertices are both 32-byte TA words), and the
 *    buffer is replayed verbatim after the general list closes. The *render*
 *    order is unaffected by that — the ISP always rasterises OP, then PT, then
 *    TR — so a punched cutout still resolves against everything else through
 *    the depth buffer, with a real hardware alpha test doing the discarding.
 *    Kill switch: -DDC_PVR_NO_PUNCHTHRU restores the pre-2026-08-02 behaviour
 *    verbatim, including the blend-based cutout approximation below.
 *
 * 2. THE SH-4 DOES THE TRANSFORM.  There is no hardware T&L. Per batch we fold
 *    projection * posmtx into one 4x4, then per vertex do one matrix-vector
 *    multiply, a near-plane clip, a perspective divide and the viewport map.
 *    The PVR wants screen-space XY and 1/w as "z" — see dc_mtx.c:358.
 *
 * 3. NEAR-PLANE CLIPPING IS DONE HERE.  The PVR has no near clip. A triangle
 *    with a vertex behind the eye projects to garbage that smears across the
 *    screen, so each triangle is clipped against w > DC_PVR_W_EPS before the
 *    divide, emitting one or two triangles. Dropping such triangles outright
 *    would punch holes in the ground plane every time the camera is low.
 *
 * 4. TEV IS APPROXIMATED, DELIBERATELY.  kb/tev-map.md enumerates 101 TEV
 *    configurations. This pass implements the one that covers most of them:
 *    modulate the rasterised colour by the bound texture, with GX's blend
 *    factors mapped onto the PVR's (they share the "the other one's colour"
 *    naming quirk, so the mapping is 1:1). Konst-colour and multi-stage
 *    configs come out untinted rather than absent. That is a visible-but-wrong
 *    pixel, which is debuggable; a missing pixel is not.
 *
 * ==========================================================================
 * KILL SWITCHES (CLAUDE.md: "every optimization gets a kill switch")
 * ==========================================================================
 *   -DDC_PVR_BACKEND=0      compile the whole backend out; back to NONE/stub
 *   -DDC_PVR_LIST_MODE=1    opaque list instead of translucent
 *   -DDC_PVR_NO_CULL        never cull, whatever GXSetCullMode said
 *   -DDC_PVR_CULL_INVERT    swap CW/CCW if the winding derivation is wrong
 *   -DDC_PVR_NO_LIGHTING    skip the SH-4 lighting stage, use vertex colour
 *   -DDC_PVR_NO_NEARCLIP    drop straddling triangles instead of clipping
 *   -DDC_PVR_VERTBUF_BYTES  TA vertex buffer size in VRAM (default 768 KB)
 *   -DDC_PVR_NO_PUNCHTHRU   no PT list; cutouts go back to the TR blend hack
 *   -DDC_PVR_PT_ALL         route BLENDED cutouts to PT too (default: only
 *                           GX_BM_NONE cutouts, i.e. opaque-with-holes)
 *   -DDC_PVR_PT_ALPHA_REF   the global PT threshold, 0..255 (default 144)
 *   -DDC_PVR_PT_KEEP_VTXALPHA  let shade alpha multiply into the PT alpha test
 *                           (2026-08-02 behaviour; deletes the train door)
 *   -DDC_PVR_PT_BUF_RECS    deferred PT records, 32 B each (default 2048)
 *   -DDC_PVR_NO_TEVCONST_ALPHA  do not rescue the TEV constant ALPHA term
 *                           (leaves the speaker name and replies invisible)
 *   -DDC_PVR_NO_TEVCONST_ALPHA_MIRROR  match only the (ZERO,const,TEXA,ZERO)
 *                           spelling, not the 17x more common mirrored one
 *   -DDC_PVR_TEVCONST_ALPHA_WIDE  widen the mirror from PRIM_LOD_FRAC (A0,
 *                           134 sites, the default) to A0/A1/A2 (688). The
 *                           wide form broke the title-screen station.
 *   -DDC_PVR_NO_TEVCONST_COLOR_A  do not accept a flat constant colour written
 *                           in the `a` slot rather than `d` (5 sites)
 *   -DDC_PVR_NO_FOG         no hardware fog; byte-identical to pre-fog output
 *   -DDC_PVR_FOG_LOG=<N>    one fog state line every Nth frame
 *   -DDC_PVR_NO_FTRV        scalar C matrix-vector per vertex instead of the
 *                           SH-4 FTRV instruction (kb/perf-dc.md)
 *   -DDC_PVR_NO_SHADEFAST   restore the per-component lighting evaluation and
 *                           the unconditional eye/normal transform
 */

#include "dc_platform.h"
#include "dc_gx_internal.h"
#include "dc_pvr.h"
#include "dolphin/gx/GXEnum.h"

#ifndef DC_PVR_BACKEND
#define DC_PVR_BACKEND 1
#endif

#ifndef DC_PVR_LIST_MODE
#define DC_PVR_LIST_MODE 0      /* 0 = translucent list, 1 = opaque list */
#endif

#ifndef DC_PVR_VERTBUF_BYTES
#define DC_PVR_VERTBUF_BYTES (768 * 1024)
#endif

/* Vertices closer than this to the eye plane are clipped away. Not 0: a
 * vertex at exactly w = 0 divides to infinity and the TA latches a NaN. */
#ifndef DC_PVR_W_EPS
#define DC_PVR_W_EPS 0.001f
#endif

int dc_pvr_ready = 0;

/* Counters, dumped once per second by dc_pvr_report(). Cheap enough at -O0
 * that they stay in for now: without them a black screen is unattributable. */
static unsigned int s_frames;
static unsigned int s_batches;
static unsigned int s_tris_in;
static unsigned int s_tris_out;
static unsigned int s_tris_clipped;
static unsigned int s_tris_dropped;
static unsigned int s_prim_unsupported;
#if defined(DC_PVR_SHADE_ALPHA8) && !defined(DC_PVR_NO_LIGHTING) && \
    !defined(DC_PVR_NO_SHADEFAST)
/* Vertices whose alpha lane took the byte-copy shortcut in shade_vertex().
 * The falsification counter for S3: a 0 here on a lit town run means the
 * predicate stopped matching (an emu64 or dc_gx.c recording change) and any
 * timing movement is NOT this. */
static unsigned int s_shade_a8;
#endif
#if defined(DC_GX_VTXID) && defined(DC_GX_VTXID_VERIFY)
/* G-B's correctness gate. `vidbad` MUST be 0; `vidchk` says how much was
 * actually checked, so a zero that comes from the channel never arming is
 * distinguishable from a zero that means it is right. */
static unsigned int s_vid_checked, s_vid_bad;
#endif
#ifndef DC_PVR_NO_TEX1ALPHA
/* Batches that took the second-texture alpha fold. Reported next to the other
 * renderer counters so the feature says how often it fires rather than being
 * assumed to. */
static unsigned int s_tex1_batches;
#endif

#ifndef DC_PVR_NO_VTXMEMO
/* Defined out here, not inside the DC_PVR_BACKEND block, so dc_vi.c's [PHASE]
 * line still links when the backend is compiled out. */
unsigned int dc_pvr_vmemo_hit = 0;
unsigned int dc_pvr_vmemo_total = 0;
#endif

#if DC_PVR_BACKEND

#include <dc/pvr.h>
/* SH-4 FTRV: dc_gx_backend_submit() folds projection*modelview into ONE 4x4 and
 * then runs a matrix-vector multiply PER VERTEX. At -O0 that is 16 multiplies,
 * 12 adds and ~60 stack round-trips of C; FTRV is one instruction. The matrix
 * is loaded into XMTRX once per BATCH. See the DC_PVR_NO_FTRV block in
 * dc_gx_backend_submit() and kb/perf-dc.md. */
#include <dc/matrix.h>

/* SH-4 FSRRA / FIPR. Two instructions that replace the two shapes this file
 * runs thousands of times a frame, and neither is reachable from portable C:
 *
 *   frsqrt(x)  -> FSRRA, one instruction, ~1 cycle issue. The C it replaces is
 *                 `1.0f / sqrtf(x)`. $KOS_CFLAGS carries -mfsrra but NOT
 *                 -funsafe-math-optimizations, so GCC may not fold that form
 *                 itself; VERIFIED in the shipped object, which contains
 *                 `fsqrt` followed by up to three `fdiv` (dc_pvr.c.o+0x178).
 *                 On SH-4 FSQRT is ~13 cycles and FDIV ~13 more, each
 *                 non-pipelined, so a normalise costs ~50 cycles of latency
 *                 where FSRRA costs ~3.
 *   fipr(...)  -> FIPR, a four-component dot product in one instruction, vs
 *                 four FMUL and three FADD.
 *
 * PRECISION. FSRRA and FIPR are single-precision approximations: FSRRA carries
 * about 21 correct mantissa bits (~5e-7 relative) and FIPR accumulates without
 * intermediate rounding. Everything they feed here ends as an 8-bit colour
 * byte, an eye-space vector used only for a normalised direction, or a
 * texture coordinate — never as a depth value, which is why emit_projected()'s
 * `1.0f / c->w` is deliberately NOT converted: z-fighting is decided at a
 * precision this would disturb.
 *
 * Kill switch: -DDC_PVR_NO_FASTMATH restores the exact previous arithmetic.
 * Everything guarded by it is written as an if/else on a macro rather than a
 * rewrite, so the A/B is the same source. */
#ifndef DC_PVR_NO_FASTMATH
#include <dc/fmath.h>
#define DC_RSQRT(x)  frsqrt(x)
#define DC_DOT3(ax, ay, az, bx, by, bz)  fipr((ax), (ay), (az), 0.0f, \
                                              (bx), (by), (bz), 0.0f)
#define DC_DOT4(ax, ay, az, aw, bx, by, bz, bw) \
    fipr((ax), (ay), (az), (aw), (bx), (by), (bz), (bw))
#else
#define DC_RSQRT(x)  (1.0f / sqrtf(x))
#define DC_DOT3(ax, ay, az, bx, by, bz)  ((ax) * (bx) + (ay) * (by) + (az) * (bz))
#define DC_DOT4(ax, ay, az, aw, bx, by, bz, bw) \
    ((ax) * (bx) + (ay) * (by) + (az) * (bz) + (aw) * (bw))
#endif

/* --- Viewport shadow -------------------------------------------------------
 * GXSetViewport hands us GameCube screen pixels, baked here into a scale and
 * offset so the per-vertex path is two multiply-adds.
 *
 * ⚠️ This comment used to say "Y-down, same as GX, so there is no flip". That
 * is wrong about the code directly below it: emit_projected() computes
 *     pv.y = s_vp_cy - s_vp_hh * (y * inv_w)
 * — the NDC->screen Y negation IS there, and it is correct. Believing the
 * comment instead of the code is what produced the inverted cull mapping that
 * made every character render inside-out (see cull_gx_to_pvr below). */
static float s_vp_cx = 320.0f, s_vp_cy = 240.0f;
static float s_vp_hw = 320.0f, s_vp_hh = 240.0f;

/* Set when a list is open inside the current scene. */
static int s_scene_open;
static int s_list_open;

/* --- Per-batch state dump (-DDC_PVR_BATCH_LOG=<N>, every Nth frame) ---------
 * Pair it with DC_FB_PROBE=<N> at the SAME N and the log lines describe the
 * frame the screenshot captured, which is the only way to attribute a region
 * of a decoded PNG to the state that drew it. Two questions it exists to
 * answer, neither of which the [PERF]/[DC/PVR] counters can:
 *   - a draw call is counted, but does its geometry land on screen at all?
 *     (BBOX + Z)
 *   - which blend/tex/wrap state produced a given region? (the rest)
 * DC_LOG bypasses the dc_misc.c flood limiter, so identical-format lines are
 * NOT collapsed and the per-batch sequence survives intact. */
#ifdef DC_PVR_BATCH_LOG
static int   s_batch_log_now;
static int   s_bl_n;
static float s_bl_x0, s_bl_y0, s_bl_x1, s_bl_y1;
static float s_bl_z0, s_bl_z1;
static float s_bl_u0, s_bl_v0, s_bl_u1, s_bl_v1;
static unsigned int s_bl_argb;
#endif

/* The compiled poly header currently latched into the TA, and the state hash
 * it was compiled from. Recompiling costs ~100 cycles; comparing an int is
 * free, and the game re-sets identical state constantly. */
static pvr_poly_hdr_t s_hdr;
static unsigned int   s_hdr_key = 0xFFFFFFFFu;
static int            s_hdr_valid;

/* ==========================================================================
 * Hardware fog
 * ==========================================================================
 * emu64.c:3219 asks for GX_FOG_PERSP_LIN with live startz/endz/near/far and a
 * live colour whenever the N64 blender's P0 is G_BL_CLR_FOG and G_FOG is in
 * the geometry mode. dc_gx.c:1277 has recorded all six parameters since M1 and
 * nothing ever read them — the same "recorded, never consumed" bug family as
 * the wrap mode, the TEV constants and the colour mask.
 *
 * WHY TABLE FOG, AND WHY IT IS EXACT HERE
 * ---------------------------------------
 * PVR table fog indexes a 128-entry table by the per-pixel 1/w scaled by the
 * FOG_DENSITY register and clamped to [1.0, 259.999999]. Entry j stands for
 * the scaled value
 *     t(j) = 2^(j>>4) * ((j & 0xF) + 16) / 16,   j = 0..128,  t = 1 .. 256
 * (Simon Fenney's formula, quoted in KOS pvr_fog.c; it is exactly what
 * generates KOS's inverse_w_depth[]). Set FOG_DENSITY = endz and entry j sits
 * at the eye depth w(j) = endz / t(j) — known in closed form.
 *
 * emit_projected() already writes pv.z = 1/w, and w is the fourth row of the
 * GC projection, which GXSetProjection forces to (0,0,-1,0) for GX_PERSPECTIVE
 * (dc_gx.c:835-839). So w IS the positive eye-space depth — the same quantity
 * and the same units as emu64's startz/endz (emu64.c:3205-3208 negate
 * guMtxXFM1F_dol3, which returns eye z).
 *
 * GX_FOG_PERSP_LIN is therefore exactly representable: evaluate
 * f = clamp01((w - start) / (end - start)) at the w each entry really stands
 * for. GX's nearz/farz exist only to invert a depth-buffer value back to eye
 * z; we never left eye z, so they are not needed and are ignored — that is
 * why this mapping is exact rather than fitted. Residual error is (a) 129-knot
 * piecewise-linear interpolation of a curve smooth in 1/w, (b) 8 bits of fog
 * factor, (c) nearer than end/256 the factor freezes instead of falling to 0,
 * which only shows if start < end/256.
 *
 * Vertex fog is not an option: KOS's pvr_fog_vertex_color() is an
 * assert_msg(0, "not implemented") stub, and it would cost a per-vertex oargb
 * on the SH-4. Table fog costs nothing per vertex — pv.oargb stays 0 and
 * emit_projected() is untouched.
 *
 * WHEN THE REGISTERS ARE WRITTEN
 * ------------------------------
 * The fog table, colour and density are GLOBAL registers, not part of a poly
 * header, and pvr_fog.c is explicit that they must not be touched between
 * pvr_scene_begin() and pvr_scene_finish(). So the batch path only LATCHES
 * what it sees and dc_gx_backend_frame_begin() programs it right after
 * pvr_wait_ready() — the one point in the frame with no render in flight.
 * Consequence: a fog PARAMETER change lands one frame late, and if one frame
 * uses two different fog settings the last one latched wins for that frame.
 * Fog ON/OFF is per-poly (gen.fog_type) and has no latency.
 *
 * Main-RAM cost: 48 B of .bss here, ~344 B of KOS .text that --gc-sections is
 * currently discarding, 516 B of transient stack in fog_program(). The table
 * itself lives in PVR registers at 0xA05F8200 — zero main RAM, zero VRAM.
 * Using pvr_fog_table_custom() rather than pvr_fog_table_linear/exp keeps
 * KOS's own 2 KB of exp/inverse-w tables discarded as well.
 *
 * Kill switch: -DDC_PVR_NO_FOG. Diagnostic: -DDC_PVR_FOG_LOG=<N>.
 */
#ifndef DC_PVR_NO_FOG
/* What the last fogged batch asked for, latched during the frame. */
static int   s_fog_pend;
static float s_fog_pend_start, s_fog_pend_end, s_fog_pend_col[3];
/* What is actually in the registers right now. */
static int   s_fog_hw;
static float s_fog_hw_start, s_fog_hw_end, s_fog_hw_col[3];
#ifdef DC_PVR_FOG_LOG
static unsigned int s_fog_batches, s_fog_programs;
#endif

/* Does the CURRENT g_gx ask for fog we can actually render? */
static int fog_active(void) {
    if (g_gx.fog_type == GX_FOG_NONE) return 0;
    /* GX_FOG_ORTHO_* would need a different table build, and the 2D/UI path is
     * orthographic with w == 1, which would collapse every pixel into a single
     * table entry. Neither is worth guessing at. */
    if (g_gx.projection_type != GX_PERSPECTIVE) return 0;
    /* Degenerate parameters: emu64 emits (0,0,0,0) on its GX_FOG_NONE path and
     * the min/max arithmetic at emu64.c:3197 can invert. */
    if (!(g_gx.fog_end > g_gx.fog_start)) return 0;
    if (!(g_gx.fog_end > 0.0f)) return 0;
    return 1;
}

/* Program FOG_TABLE_COLOR + FOG_DENSITY + the 128 table registers. MUST be
 * called with no render in flight. Re-programs only on a real change. */
static void fog_program(void) {
    float table[129];
    float start, end;
    int j;

    if (!s_fog_pend) return;
    if (s_fog_hw &&
        s_fog_hw_start  == s_fog_pend_start &&
        s_fog_hw_end    == s_fog_pend_end &&
        s_fog_hw_col[0] == s_fog_pend_col[0] &&
        s_fog_hw_col[1] == s_fog_pend_col[1] &&
        s_fog_hw_col[2] == s_fog_pend_col[2])
        return;

    start = s_fog_pend_start;
    end   = s_fog_pend_end;
    if (start < 0.0f) start = 0.0f;

    /* Alpha 1.0 DELIBERATELY. KOS cannot set alpha in FOG_TABLE_COLOR, so it
     * fakes it by scaling every table entry by the alpha last handed to
     * pvr_fog_table_color() — which makes that argument a fog STRENGTH, not a
     * colour channel. g_gx.fog_color[3] is the N64 fog colour's alpha and is
     * not a strength; feeding it in would silently delete the fog whenever the
     * game left it at 0. Must be called BEFORE the table build: the table
     * builder reads the latched alpha. */
    pvr_fog_table_color(1.0f, s_fog_pend_col[0], s_fog_pend_col[1],
                        s_fog_pend_col[2]);
    pvr_fog_far_depth(end);

    for (j = 0; j <= 128; j++) {
        /* t(j) = the scaled 1/w this entry stands for; w(j) = end / t(j). */
        float t = (float)(1 << (j >> 4)) * (float)((j & 0xF) + 16) *
                  (1.0f / 16.0f);
        float w = end / t;
        float f = (w - start) / (end - start);
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        table[j] = f;              /* table[0] is farthest; 1.0 = full fog */
    }
    pvr_fog_table_custom(table);

    s_fog_hw = 1;
    s_fog_hw_start  = s_fog_pend_start;
    s_fog_hw_end    = s_fog_pend_end;
    s_fog_hw_col[0] = s_fog_pend_col[0];
    s_fog_hw_col[1] = s_fog_pend_col[1];
    s_fog_hw_col[2] = s_fog_pend_col[2];
#ifdef DC_PVR_FOG_LOG
    s_fog_programs++;
#endif
}

/* One call per batch from dc_gx_backend_submit(). NOT done inside
 * compile_header(): that only runs when header_key() changes, and the fog
 * PARAMETERS are deliberately absent from the key — they do not alter the
 * compiled header, only the global registers. */
static void fog_latch(void) {
    if (!fog_active()) return;
    s_fog_pend = 1;
    s_fog_pend_start  = g_gx.fog_start;
    s_fog_pend_end    = g_gx.fog_end;
    s_fog_pend_col[0] = g_gx.fog_color[0];
    s_fog_pend_col[1] = g_gx.fog_color[1];
    s_fog_pend_col[2] = g_gx.fog_color[2];
#ifdef DC_PVR_FOG_LOG
    s_fog_batches++;
#endif
}
#endif /* !DC_PVR_NO_FOG */

/* ==========================================================================
 * The punch-through list — a real hardware alpha test
 * ==========================================================================
 * See decision 1 in the file header for WHY. This block is the HOW.
 *
 * PVR_LIST_PT_POLY is 4, i.e. last in the TA's list enum (pvr_header.h:65),
 * and pvr_list_finish() latches `lists_closed` so a list "can never be opened
 * again within a single frame once closed" (pvr_scene.c). The frame therefore
 * has to be TR (or OP) first and PT last, which means cutout geometry must be
 * held somewhere until the general list closes.
 *
 * WHAT IS BUFFERED, AND WHY IT IS THE CHEAP THING. Not GX state and not source
 * vertices: the finished TA words. A pvr_poly_hdr_t is 32 bytes and a
 * pvr_vertex_t is 32 bytes (both asserted below), so one flat array of 32-byte
 * records holds the interleaved header/vertex stream in submission order and
 * the replay is a straight memcpy loop into the store queues. Transform,
 * lighting, near clip and texgen all still happen exactly once, at submission
 * time, in the order the game asked for. Nothing is re-derived at replay.
 *
 * SIZING. Cutouts were measured at 13.6 % of batches (316 of 2331) and the
 * renderer averages ~400 triangles per frame over a 600 s run, so the expected
 * steady-state load is on the order of 50-80 triangles = 150-240 records. 2048
 * records is ~10x that, and it costs 65,536 B of .bss — the one place in this
 * file that spends main RAM, so it is capped, counted, and reported rather
 * than grown on faith. Raise it with -DDC_PVR_PT_BUF_RECS and watch `pthi=` in
 * the [DC/PVR] line, which is the frame high-water mark against the cap.
 *
 * OVERFLOW IS NOT CORRUPTION. A full buffer drops whole TRIANGLES, never a
 * partial one: emit_triangle() marks the write position, and if any record of
 * that triangle did not fit the position is rolled back so the replayed stream
 * can never contain a strip that was cut off before its EOL vertex. Dropped
 * triangles are counted in `ptdrop=`; a nonzero value there means geometry is
 * missing from the screen and the buffer needs raising.
 *
 * THE THRESHOLD IS ONE GLOBAL REGISTER. PT_ALPHA_REF lives at 0xA05F811C and
 * is latched for the whole render, not per polygon — KOS 2.3 does not name it
 * at all (pvr_regs.h stops at PVR_UNK_0118 = 0x0118 and resumes at
 * PVR_TA_OPB_START = 0x0124), so it is written here by offset through KOS's
 * own PVR_SET. It is pinned to 144 to match emu64's `tex_edge_alpha` default
 * (emu64.c:718), which kb/tev-map-alpha.md §5.5 option (a) identifies as the
 * dominant reference; per-draw references that differ from it are approximated
 * rather than honoured, and that is a known, bounded wrong. */
#ifndef DC_PVR_NO_PUNCHTHRU

_Static_assert(sizeof(pvr_poly_hdr_t) == 32, "PT buffer record size");
_Static_assert(sizeof(pvr_vertex_t) == 32, "PT buffer record size");

/* The PT alpha comparison value. KOS has no symbol for it; this is the raw
 * register offset from 0xA05F8000, used with KOS's PVR_SET(). */
#define DC_PVR_REG_PT_ALPHA_REF 0x011c

#ifndef DC_PVR_PT_ALPHA_REF
#define DC_PVR_PT_ALPHA_REF 144
#endif

/* Honour the GX alpha combiner when picking the PVR texture env. ON by
 * default since 2026-08-03; -DDC_PVR_NO_ALPHAENV is the kill switch. The
 * reasoning, the A/B and the exclusion list are at alpha_env_texel_only(). */
#if !defined(DC_PVR_NO_ALPHAENV) && !defined(DC_PVR_ALPHAENV)
#define DC_PVR_ALPHAENV 1
#endif

#ifndef DC_PVR_PT_BUF_RECS
#define DC_PVR_PT_BUF_RECS 2048
#endif

/* The object-pointer bin size for the PT list. VRAM only — it comes out of the
 * PVR's own 4 MB half, not out of main RAM, and not out of the texture pool's
 * ceiling until that half is exhausted (it is not: two buffer sets of
 * ~1.7 MB each leave ~4.9 MB free against a 4 MB texture ceiling).
 *
 * Set to _32, matching the general list, rather than the _16 that 13.6 % of
 * batches would suggest: an OPB is indexed PER TILE, and PT's population is
 * exactly the large-area geometry — foliage canopies, the station roof, the
 * train door and tunnel — so its objects-per-tile count is not proportional to
 * its share of batch COUNT. Undersizing costs dropped geometry (the OPB
 * overflow allowance is finite) to save ~76 KB of a resource that is not
 * scarce. -DDC_PVR_PT_BINSIZE=16 is the A/B if VRAM ever does get tight. */
#ifndef DC_PVR_PT_BINSIZE
#define DC_PVR_PT_BINSIZE PVR_BINSIZE_32
#endif

typedef struct { unsigned int w[8]; } dc_pt_rec_t;

/* 32-byte aligned: pvr_prim() rejects anything not 8-byte aligned and the SQ
 * path copies in 32-byte units. */
static dc_pt_rec_t s_pt_buf[DC_PVR_PT_BUF_RECS] __attribute__((aligned(32)));
static unsigned int s_pt_n;         /* records written this frame            */
static int          s_pt_route;     /* the batch being submitted goes to PT  */
static int          s_pt_trunc;     /* a record was dropped since the mark   */

static unsigned int s_pt_batches;   /* batches routed to PT (cumulative)     */
static unsigned int s_pt_verts;     /* vertices replayed through PT          */
static unsigned int s_pt_recs;      /* records replayed through PT           */
static unsigned int s_pt_hi;        /* worst single-frame record count       */
static unsigned int s_pt_drop;      /* triangles dropped to buffer overflow  */
static int          s_pt_warned;    /* the one-shot overflow shout, below    */

#ifdef DC_PVR_ALPHAENV
/* The batch being submitted asks for alpha = TEXEL0.a alone, so its poly header
 * gets PVR_TXRENV_MODULATE instead of MODULATEALPHA. Decided once per batch in
 * dc_gx_backend_submit(), exactly like s_pt_route, so header_key() and
 * compile_header() can never disagree about it.
 *
 * ⚠️ OPT-IN, and it is off by default because it was MEASURED to regress. See
 * alpha_env_texel_only() below for the A/B. */
static int          s_alpha_env_texel;
static unsigned int s_env_texel_batches;  /* how often it fired (cumulative)  */
#endif

#ifdef DC_PVR_TEVP3
/* Whether THIS batch needs the PVR's offset colour, i.e. whether tev_p3_affine()
 * produced a non-zero offset. Same contract as s_pt_route and
 * s_alpha_env_texel: decided once per batch in dc_gx_backend_submit() BEFORE
 * ensure_header(), so header_key() and compile_header() cannot disagree.
 * s_p3_clamped counts batches where PRIM < ENV in some channel and the base
 * had to be clamped at 0 — the one place this fix is not exact (see
 * tev_p3_affine). */
static int          s_p3_specular;
static unsigned int s_p3_batches;
static unsigned int s_p3_clamped;
#endif

#if !defined(DC_PVR_NO_TEVCONST) && !defined(DC_PVR_NO_TEVCONST_ALPHA) && \
    !defined(DC_PVR_NO_TEVALPHA_LAST)
/* Batches whose LAST TEV stage was `APREV * <constant>` and so contributed a
 * constant factor to the vertex alpha. Same family as s_tex1_batches: without a
 * count, a screenshot pair cannot be attributed to this path rather than to
 * anything else that moved in the same build. */
static unsigned int s_tevalpha_last_batches;
#endif

/* One 32-byte TA record into the deferred stream. Records are uniform, so once
 * the buffer is full it stays full for the rest of the frame — which is what
 * makes "drop the whole triangle" implementable as a position rollback. */
static void pt_defer(const void* src) {
    if (s_pt_n >= (unsigned int)DC_PVR_PT_BUF_RECS) {
        s_pt_trunc = 1;
        return;
    }
    memcpy(&s_pt_buf[s_pt_n], src, 32);
    s_pt_n++;
}
#endif /* !DC_PVR_NO_PUNCHTHRU */

/* Every TA word in this file goes through here. Outside a PT batch it is
 * pvr_prim() unchanged; inside one it lands in the deferred buffer instead. */
static void submit_prim(const void* p, unsigned int size) {
#ifndef DC_PVR_NO_PUNCHTHRU
    if (s_pt_route) { pt_defer(p); return; }
#endif
    pvr_prim(p, size);
}

/* ==========================================================================
 * Small helpers
 * ========================================================================== */

static inline unsigned int pack_argb(float r, float g, float b, float a) {
    int ir, ig, ib, ia;
    ir = (int)(r * 255.0f + 0.5f); if (ir < 0) ir = 0; if (ir > 255) ir = 255;
    ig = (int)(g * 255.0f + 0.5f); if (ig < 0) ig = 0; if (ig > 255) ig = 255;
    ib = (int)(b * 255.0f + 0.5f); if (ib < 0) ib = 0; if (ib > 255) ib = 255;
    ia = (int)(a * 255.0f + 0.5f); if (ia < 0) ia = 0; if (ia > 255) ia = 255;
    return ((unsigned int)ia << 24) | ((unsigned int)ir << 16) |
           ((unsigned int)ig << 8) | (unsigned int)ib;
}

/* pack_argb with the alpha byte handed in already quantised, for the case
 * where the alpha channel is a straight copy of the vertex's own alpha byte.
 * See shade_vertex()'s DC_PVR_SHADE_ALPHA8 block for why that is exact
 * rather than an approximation. */
static inline unsigned int pack_rgb_a8(float r, float g, float b,
                                       unsigned int a8) {
    int ir, ig, ib;
    ir = (int)(r * 255.0f + 0.5f); if (ir < 0) ir = 0; if (ir > 255) ir = 255;
    ig = (int)(g * 255.0f + 0.5f); if (ig < 0) ig = 0; if (ig > 255) ig = 255;
    ib = (int)(b * 255.0f + 0.5f); if (ib < 0) ib = 0; if (ib > 255) ib = 255;
    return (a8 << 24) | ((unsigned int)ir << 16) |
           ((unsigned int)ig << 8) | (unsigned int)ib;
}

/* GX depth funcs compare "smaller is closer". The PVR compares 1/w, where
 * LARGER is closer, so every ordering predicate inverts. Equality does not. */
static int depth_gx_to_pvr(int func) {
    switch (func) {
        case GX_NEVER:   return PVR_DEPTHCMP_NEVER;
        case GX_LESS:    return PVR_DEPTHCMP_GREATER;
        case GX_EQUAL:   return PVR_DEPTHCMP_EQUAL;
        case GX_LEQUAL:  return PVR_DEPTHCMP_GEQUAL;
        case GX_GREATER: return PVR_DEPTHCMP_LESS;
        case GX_NEQUAL:  return PVR_DEPTHCMP_NOTEQUAL;
        case GX_GEQUAL:  return PVR_DEPTHCMP_LEQUAL;
        default:         return PVR_DEPTHCMP_ALWAYS;
    }
}

/* GX wrap mode -> the PVR's two separate U/V controls.
 *
 * The PVR splits what GX keeps in one enum. `uv_clamp` pins a coordinate at
 * 1.0; `uv_flip` mirrors it at every unit boundary; neither set is the plain
 * repeat GX calls GX_REPEAT, which is the hardware default. Clamp overrides
 * flip in hardware, so the two never have to be reconciled here.
 *
 * ⚠️ NPOT. dc_pvr_texture.c pads a non-power-of-two source up to a POT image
 * and hands back u_scale/v_scale < 1.0; emit_triangle scales the texcoords by
 * them. Clamping then pins at the edge of the PADDED image, not at the edge of
 * the real one, so a clamped NPOT texture bleeds into padding rather than
 * repeating into it. That is strictly less wrong than before and is NOT a fix
 * for NPOT — the real fix is to replicate the edge texel when padding.
 *
 * DC_PVR_NO_UVCLAMP restores the old unconditional PVR_UVCLAMP_NONE. */
static void wrap_gx_to_pvr(int wrap_s, int wrap_t, int* clamp, int* flip) {
#ifdef DC_PVR_NO_UVCLAMP
    (void)wrap_s; (void)wrap_t;
    *clamp = PVR_UVCLAMP_NONE;
    *flip  = PVR_UVFLIP_NONE;
#else
    int c = PVR_UVCLAMP_NONE, f = PVR_UVFLIP_NONE;
    /* PVR_UVCLAMP_U/_V and PVR_UVFLIP_U/_V are bit flags in the same order in
     * both enums (V = 1, U = 2), so the two axes OR together. */
    if (wrap_s == GX_CLAMP)       c |= PVR_UVCLAMP_U;
    else if (wrap_s == GX_MIRROR) f |= PVR_UVFLIP_U;
    if (wrap_t == GX_CLAMP)       c |= PVR_UVCLAMP_V;
    else if (wrap_t == GX_MIRROR) f |= PVR_UVFLIP_V;
    *clamp = c;
    *flip  = f;
#endif
}

/* GX and the PVR share the hardware quirk that a blend factor named after the
 * SOURCE colour means the DESTINATION colour when used in the source slot.
 * That makes this a straight numeric remap rather than a slot-dependent one.
 * GX_BL_DSTCLR aliases GX_BL_SRCCLR and GX_BL_INVDSTCLR aliases
 * GX_BL_INVSRCCLR in GXEnum.h, so 0x2/0x3 cover both spellings. */
static int blend_gx_to_pvr(int f) {
    switch (f) {
        case GX_BL_ZERO:        return PVR_BLEND_ZERO;
        case GX_BL_ONE:         return PVR_BLEND_ONE;
        case GX_BL_SRCCLR:      return PVR_BLEND_DESTCOLOR;
        case GX_BL_INVSRCCLR:   return PVR_BLEND_INVDESTCOLOR;
        case GX_BL_SRCALPHA:    return PVR_BLEND_SRCALPHA;
        case GX_BL_INVSRCALPHA: return PVR_BLEND_INVSRCALPHA;
        /* ⚠️ DESTINATION ALPHA DOES NOT EXIST ON THIS FRAMEBUFFER.
         *
         * emu64 uses the N64 two-pass memory-alpha decal idiom for every
         * ground shadow in the game:
         *   pass A  ZMODE_DEC|G_DECAL_GEQUAL|G_DECAL_SPECIAL
         *           -> GXSetBlendMode(GX_BM_NONE, ONE, ZERO)      emu64.c:2289
         *           -> GXSetColorUpdate(FALSE)/AlphaUpdate(TRUE)  emu64.c:2347
         *           writes the shadow's ALPHA into the framebuffer, paints
         *           nothing (already neutralised by the colour-mask block).
         *   pass B  ZMODE_DEC|G_DECAL_SPECIAL
         *           -> GXSetBlendMode(GX_BM_BLEND, DSTALPHA, INVDSTALPHA)
         *                                                          emu64.c:2291
         *           blends using the alpha pass A left behind.
         * RDP side: m_rcp.c:131, GBL_c2(G_BL_CLR_IN, G_BL_A_IN, G_BL_CLR_MEM,
         * G_BL_A_MEM).
         *
         * KOS renders into RGB565. There is NO stored destination alpha, so
         * PVR_BLEND_DESTALPHA reads 1.0 and pass B collapses to src*1 + dst*0
         * — the shadow paints OPAQUE. MEASURED 2026-08-02: batch 4241,
         * bm=1,6,7, argb=A4001E4B, bbox 133,251..804,449; sampled pixels there
         * read 00204A / 001C4A / 002052, the prim colour verbatim. A human
         * reported it as "the train station is very broken on the title screen
         * with missing textures" — the textures were fine; a navy slab was
         * painted over them.
         *
         * Substituting SOURCE alpha is EXACT here, not an approximation: both
         * passes draw the same geometry with the same texture and the same
         * prim alpha, so the destination alpha pass B reads back is by
         * construction its own source alpha. It would only break if some other
         * draw wrote framebuffer alpha for a LATER, DIFFERENT primitive to
         * read — and GXSetAlphaUpdate(TRUE) appears exactly once in emu64, in
         * pass A above.
         *
         * Kill switch: -DDC_PVR_KEEP_DSTALPHA restores the literal mapping. */
#ifdef DC_PVR_KEEP_DSTALPHA
        case GX_BL_DSTALPHA:    return PVR_BLEND_DESTALPHA;
        case GX_BL_INVDSTALPHA: return PVR_BLEND_INVDESTALPHA;
#else
        case GX_BL_DSTALPHA:    return PVR_BLEND_SRCALPHA;
        case GX_BL_INVDSTALPHA: return PVR_BLEND_INVSRCALPHA;
#endif
        default:                return PVR_BLEND_ONE;
    }
}

/* ⚠️ FIXED 2026-08-02, and the old reasoning is kept because it was seductive.
 *
 * This used to map FRONT->CW / BACK->CCW, arguing: "pc_gx.c:1295 maps
 * GX_CULL_BACK -> GL_BACK against GL's default CCW front face, and our screen
 * space is Y-DOWN, which reverses the sign of every cross product, so GX-front
 * becomes CW here." **That double-counts the flip.** Both APIs name their
 * winding modes in terms of the DISPLAYED image, so the intermediate
 * coordinate handedness cancels; the Y negation in emit_projected is already
 * what makes the displayed image upright.
 *
 * Worked, on one NDC triangle A(0,0) B(1,0) C(0,1) — visually CCW:
 *
 *   PC/GL   window coords (cx,cy) (cx+hw,cy) (cx,cy+hh) -> det +hw*hh
 *           -> GL_CCW -> FRONT -> glCullFace(GL_BACK) KEEPS it
 *   DC/PVR  screen coords (cx,cy) (cx+hw,cy) (cx,cy-hh) -> det -hw*hh
 *           -> PVR_CULLING_CCW is "cull if negative" -> CULLED
 *
 * Same triangle, opposite verdict. KOS's own header is explicit:
 * pvr_header.h:77-82, CCW = 2 "cull if counterclockwise" = cull if the
 * determinant is negative, CW = 3 = cull if positive.
 *
 * SYMPTOM WHEN IT WAS WRONG, observed by a human on the train intro: every
 * character rendered inside-out — you saw the far side of each closed mesh, so
 * "everyone is standing backwards". The title screen looked fine throughout,
 * which is exactly what hid it: the logo overlay draws with GX_CULL_NONE.
 *
 * -DDC_PVR_CULL_INVERT restores the old mapping; -DDC_PVR_NO_CULL disables
 * culling entirely (models look right but every interior surface is overdrawn,
 * which is how "culling is the axis" was separated from "culling is
 * irrelevant"). */
static int cull_gx_to_pvr(int mode) {
#ifdef DC_PVR_NO_CULL
    (void)mode;
    return PVR_CULLING_NONE;
#else
    int cw = PVR_CULLING_CW, ccw = PVR_CULLING_CCW;
#ifdef DC_PVR_CULL_INVERT
    cw = PVR_CULLING_CCW; ccw = PVR_CULLING_CW;
#endif
    switch (mode) {
        case GX_CULL_FRONT: return ccw;
        case GX_CULL_BACK:  return cw;
        default:            return PVR_CULLING_NONE;
    }
#endif
}

/* ==========================================================================
 * Lighting — GX channel evaluation on the SH-4
 * ==========================================================================
 * GX computes, per colour channel: mat * clamp(amb + sum(diffuse * atten)).
 * chan_ctrl_*[] is indexed [channel*2 + is_alpha], which is how GXSetChanCtrl
 * splits GX_COLOR0A0 into its colour and alpha halves (dc_gx.c).
 *
 * Light positions and directions are VIEW space in GX, and `eye` here is the
 * post-modelview position, so they are already in the same space.
 *
 * ⚠️ PERF, 2026-08-02 (kb/perf-dc.md). The original of this file evaluated ONE
 * COMPONENT PER CALL and shade_vertex() called it four times per vertex — so a
 * lit vertex ran the 8-light loop, its sqrtf and its spot attenuation FOUR
 * TIMES to produce four numbers that differ only in which `lights[li].color[]`
 * element they multiply. chan_eval() below inverts the nesting: one pass over
 * the lights, all the components of one channel control at a time. The
 * arithmetic is otherwise term for term identical.
 *
 * Kill switch: -DDC_PVR_NO_SHADEFAST restores the per-component form, which is
 * kept verbatim below rather than reconstructed, so the A/B is exact. */
#ifndef DC_PVR_NO_LIGHTING
#ifdef DC_PVR_NO_SHADEFAST
static float chan_component(int ci, int is_alpha,
                            const float* vtx_rgba, const float* eye,
                            const float* nrm, int comp) {
    int ctl = ci * 2 + is_alpha;
    float mat, amb, illum;
    int li;

    mat = (g_gx.chan_ctrl_mat_src[ctl] == GX_SRC_VTX)
              ? vtx_rgba[comp] : g_gx.chan_mat_color[ci][comp];

    if (!g_gx.chan_ctrl_enable[ctl])
        return mat;

    amb = (g_gx.chan_ctrl_amb_src[ctl] == GX_SRC_VTX)
              ? vtx_rgba[comp] : g_gx.chan_amb_color[ci][comp];
    illum = amb;

    for (li = 0; li < 8; li++) {
        float dx, dy, dz, d2, d, atten, ndl;
        if (!(g_gx.chan_ctrl_light_mask[ctl] & (1 << li)))
            continue;

        dx = g_gx.lights[li].pos[0] - eye[0];
        dy = g_gx.lights[li].pos[1] - eye[1];
        dz = g_gx.lights[li].pos[2] - eye[2];
        d2 = dx * dx + dy * dy + dz * dz;
        if (d2 < 1e-12f) continue;
        d = sqrtf(d2);
        dx /= d; dy /= d; dz /= d;

        /* Diffuse term. GX_DF_NONE means "no N.L factor at all", which is how
         * fullbright materials are expressed; it is not the same as N.L = 0. */
        switch (g_gx.chan_ctrl_diff_fn[ctl]) {
            case GX_DF_NONE:
                ndl = 1.0f;
                break;
            case GX_DF_SIGN:
                ndl = nrm[0] * dx + nrm[1] * dy + nrm[2] * dz;
                break;
            default: /* GX_DF_CLAMP */
                ndl = nrm[0] * dx + nrm[1] * dy + nrm[2] * dz;
                if (ndl < 0.0f) ndl = 0.0f;
                break;
        }

        /* attn = max(0, a . (1, cos, cos^2)) / (k . (1, d, d^2)), with cos the
         * angle off the light's own direction. GX_AF_NONE is a flat 1. */
        atten = 1.0f;
        if (g_gx.chan_ctrl_attn_fn[ctl] == GX_AF_SPOT) {
            float cosa = -(g_gx.lights[li].dir[0] * dx +
                           g_gx.lights[li].dir[1] * dy +
                           g_gx.lights[li].dir[2] * dz);
            float num = g_gx.lights[li].a0 +
                        g_gx.lights[li].a1 * cosa +
                        g_gx.lights[li].a2 * cosa * cosa;
            float den = g_gx.lights[li].k0 +
                        g_gx.lights[li].k1 * d +
                        g_gx.lights[li].k2 * d2;
            if (num < 0.0f) num = 0.0f;
            atten = (den > 1e-9f) ? (num / den) : 0.0f;
        }

        illum += g_gx.lights[li].color[comp] * ndl * atten;
    }

    if (illum < 0.0f) illum = 0.0f;
    if (illum > 1.0f) illum = 1.0f;
    return mat * illum;
}
#else /* the hoisted form — one light loop for a whole channel control */
/* Evaluate channel control ctl = ci*2+is_alpha for components [lo,hi] of one
 * vertex, writing out[lo..hi]. Everything inside the light loop except the
 * final multiply-accumulate is component-independent, which is the whole point:
 * the light vector, its length, the one sqrtf, the normalise, N.L and the spot
 * attenuation are computed once for the three colour components instead of
 * three times.
 *
 * ⚠️ ONE DELIBERATE FP DIFFERENCE. The old inner term was
 * `color[comp] * ndl * atten`, i.e. `(color*ndl)*atten`; this is
 * `color[comp] * (ndl*atten)`. Same value to within one ulp of a float that is
 * then clamped to [0,1] and quantised to 8 bits, so it cannot change a pixel —
 * but it is not bit-identical, and that is worth knowing before blaming a
 * one-LSB colour diff on something else. */
static void chan_eval(int ci, int is_alpha, int lo, int hi,
                      const float* vtx_rgba, const float* eye,
                      const float* nrm, float* out) {
    int ctl = ci * 2 + is_alpha;
    float mat[4], illum[4];
    int comp, li;
    /* Hoisted out of the light loop. These are GX state, so they cannot change
     * inside a batch let alone inside one vertex — but g_gx is a global and
     * -fno-strict-aliasing is on, so the compiler has to reload them on every
     * iteration unless they are named here. Eight iterations x three loads. */
    unsigned int mask    = g_gx.chan_ctrl_light_mask[ctl];
    int          diff_fn = g_gx.chan_ctrl_diff_fn[ctl];
    int          is_spot = (g_gx.chan_ctrl_attn_fn[ctl] == GX_AF_SPOT);

    for (comp = lo; comp <= hi; comp++)
        mat[comp] = (g_gx.chan_ctrl_mat_src[ctl] == GX_SRC_VTX)
                        ? vtx_rgba[comp] : g_gx.chan_mat_color[ci][comp];

    if (!g_gx.chan_ctrl_enable[ctl]) {
        for (comp = lo; comp <= hi; comp++) out[comp] = mat[comp];
        return;
    }

    for (comp = lo; comp <= hi; comp++)
        illum[comp] = (g_gx.chan_ctrl_amb_src[ctl] == GX_SRC_VTX)
                          ? vtx_rgba[comp] : g_gx.chan_amb_color[ci][comp];

    for (li = 0; li < 8 && (mask >> li) != 0; li++) {
        float dx, dy, dz, d2, d, atten, ndl, w;
        /* `(mask >> li) != 0` in the bound, not `li < 8` alone: emu64 always
         * writes (1 << num_lights) - 1 (emu64.c:3317), i.e. a DENSE low mask of
         * 1-3 bits, so the loop now ends at the last set bit instead of always
         * running eight iterations. The `li < 8` half stays because
         * g_gx.lights[] has exactly 8 entries and a stray high bit in the mask
         * would otherwise index off the end. Identical set of visited lights. */
        if (!(mask & (1u << li)))
            continue;

        dx = g_gx.lights[li].pos[0] - eye[0];
        dy = g_gx.lights[li].pos[1] - eye[1];
        dz = g_gx.lights[li].pos[2] - eye[2];
        d2 = DC_DOT3(dx, dy, dz, dx, dy, dz);
        if (d2 < 1e-12f) continue;
#ifdef DC_PVR_NO_FASTMATH
        d = sqrtf(d2);
        dx /= d; dy /= d; dz /= d;
#else
        /* One FSRRA where the line above is an FSQRT and three FDIVs. `d`
         * itself is only read by the spot denominator, so it is recovered as
         * d2 * (1/d) rather than by a second root. */
        {
            float rd = frsqrt(d2);
            d = d2 * rd;
            dx *= rd; dy *= rd; dz *= rd;
        }
#endif

        /* Diffuse term. GX_DF_NONE means "no N.L factor at all", which is how
         * fullbright materials are expressed; it is not the same as N.L = 0. */
        switch (diff_fn) {
            case GX_DF_NONE:
                ndl = 1.0f;
                break;
            case GX_DF_SIGN:
                ndl = DC_DOT3(nrm[0], nrm[1], nrm[2], dx, dy, dz);
                break;
            default: /* GX_DF_CLAMP */
                ndl = DC_DOT3(nrm[0], nrm[1], nrm[2], dx, dy, dz);
                if (ndl < 0.0f) ndl = 0.0f;
                break;
        }

        /* attn = max(0, a . (1, cos, cos^2)) / (k . (1, d, d^2)), with cos the
         * angle off the light's own direction. GX_AF_NONE is a flat 1. */
        atten = 1.0f;
        if (is_spot) {
            float cosa = -DC_DOT3(g_gx.lights[li].dir[0],
                                  g_gx.lights[li].dir[1],
                                  g_gx.lights[li].dir[2], dx, dy, dz);
            float num = g_gx.lights[li].a0 +
                        g_gx.lights[li].a1 * cosa +
                        g_gx.lights[li].a2 * cosa * cosa;
            float den = g_gx.lights[li].k0 +
                        g_gx.lights[li].k1 * d +
                        g_gx.lights[li].k2 * d2;
            if (num < 0.0f) num = 0.0f;
            atten = (den > 1e-9f) ? (num / den) : 0.0f;
        }

        w = ndl * atten;
        for (comp = lo; comp <= hi; comp++)
            illum[comp] += g_gx.lights[li].color[comp] * w;
    }

    for (comp = lo; comp <= hi; comp++) {
        float f = illum[comp];
        if (f < 0.0f) f = 0.0f;
        if (f > 1.0f) f = 1.0f;
        out[comp] = mat[comp] * f;
    }
}
#endif /* DC_PVR_NO_SHADEFAST */
#endif /* !DC_PVR_NO_LIGHTING */

/* --- TEV stage 0: constant colour inputs -----------------------------------
 *
 * `kb/tev-map.md` catalogues 101 TEV configurations and this backend
 * implements one of them: PVR_TXRENV_MODULATEALPHA, i.e. "texel times the
 * rasterised colour". That is the right answer whenever the stage's colour
 * really is the raster colour. It is the WRONG answer, in a way that is
 * invisible in the counters and obvious on screen, whenever the stage's colour
 * is a CONSTANT — a TEV register or a konst — because the constant is then
 * dropped and the texture's own RGB stands in for it.
 *
 * MEASURED 2026-08-02, the case that forced this. The opening's shade quad is
 * `src/data/field/bg/acre/grd_player_select/grd_player_select.c:69`:
 *
 *     gsDPSetCombineLERP(0, 0, 0, PRIMITIVE,  0, 0, 0, TEXEL0, ...)
 *     gsDPSetPrimColor(0, 255, 0, 0, 0, 255)          // PRIM = BLACK
 *     gsDPLoadTextureBlock_4b_Dolphin(rom_open_shade_tex, G_IM_FMT_I, ...)
 *
 * — colour is (0-0)*0 + PRIMITIVE = black, alpha is TEXEL0. An I-format
 * texture expands to (I,I,I,I) on GX, so modulating it by a WHITE vertex gave
 * a full-screen WHITE vignette where a BLACK one belongs: 27.9 % of the frame,
 * pure 0xFFFF. Folding the constant into the vertex colour fixes it exactly,
 * because MODULATEALPHA then computes rgb = 0 * I = 0 and a = 255 * I = I,
 * which is what the combiner asked for.
 *
 * SCOPE, deliberately narrow. Only the `a = b = c = ZERO, d = <constant>`
 * shape is recognised — the "flat colour, texture supplies alpha" idiom. Every
 * other configuration returns 0 and the caller keeps the existing raster path,
 * so this cannot regress a case it does not understand. Widening it is the
 * rest of N3.
 *
 * DC_PVR_NO_TEVCONST is the kill switch. */
#ifndef DC_PVR_NO_TEVCONST
/* Which of g_gx.tev_colors[] a colour arg names, or -1 if it is not a constant
 * TEV register. Index order is GX_TEVPREV, GX_TEVREG0..2, matching
 * GXSetTevColor's own `id` (dc_gx.c:1112).
 *
 * This is where the N64 side lands. libforest's TEV_* constants are laid out
 * to alias GXTevColorArg exactly, which is why emu64 casts them straight
 * across (emu64.c:1423): TEV_PRIMITIVE 4 == GX_CC_C1, TEV_ENVIRONMENT 6 ==
 * GX_CC_C2, TEV_TEXEL0 8 == GX_CC_TEXC, TEV_SHADE 10 == GX_CC_RASC
 * (include/libforest/gbi_extensions.h:156-167). And emu64 writes the matching
 * registers: primitive -> GX_TEVREG1, environment -> GX_TEVREG2
 * (emu64.c:3171,3180). So a `PRIMITIVE` combiner arg resolves to
 * tev_colors[2], which is the colour gsDPSetPrimColor set.
 *
 * ⚠️ GX_CC_CPREV is DELIBERATELY absent. It is 0, and so is libforest's
 * TEV_COMBINED (gbi_extensions.h:156) — the alias that makes the rest of the
 * table work bites here, because `COMBINED` means "the previous cycle's
 * result", not a constant. Accepting it would read whatever the last
 * GXSetTevColor(GX_TEVPREV) happened to leave — usually black — and black out
 * the ~245 `(0, 0, 0, COMBINED)` cycle-0 draws in src/data/model/. */
static int tev_creg_of(int arg) {
    switch (arg) {
        case GX_CC_C0: return 1;
        case GX_CC_C1: return 2;
        case GX_CC_C2: return 3;
        default:       return -1;
    }
}

/* Non-zero if stage 0's colour is a constant, and if so writes it to out[3].
 * GX_CC_KONST resolves through the stage's k_color_sel; only the four plain
 * "whole konst register" selectors are handled, since the swizzling ones
 * (GX_TEV_KCSEL_K0_R and friends) do not appear in this scene. */
static int tev_const_color(float* out) {
    const DCGXTevStage* ts;
    int creg;
    int arg;

    if (g_gx.num_tev_stages < 1)
        return 0;
    ts = &g_gx.tev_stages[0];

    /* Two spellings of the same flat colour. GX evaluates d + (1-c)*a + c*b,
     * so with b = c = ZERO both `d = <const>, a = ZERO` and
     * `a = <const>, d = ZERO` are exactly <const>. emu64 emits the second for
     * rom_train_out_shineglass_modelT (emu64.c:1777 — the train window's
     * shine, whose colour is the sun+ambient light colour PRIM) and for
     * act_ant / act_bee (emu64.c:1901). 5 sites in src/ in total.
     * Kill switch: -DDC_PVR_NO_TEVCONST_COLOR_A. */
    if (ts->color_b != GX_CC_ZERO || ts->color_c != GX_CC_ZERO)
        return 0;
    if (ts->color_a == GX_CC_ZERO) {
        arg = ts->color_d;
    }
#ifndef DC_PVR_NO_TEVCONST_COLOR_A
    else if (ts->color_d == GX_CC_ZERO) {
        arg = ts->color_a;
    }
#endif
    else {
        return 0;
    }

    creg = tev_creg_of(arg);
    if (creg >= 0) {
        out[0] = g_gx.tev_colors[creg][0];
        out[1] = g_gx.tev_colors[creg][1];
        out[2] = g_gx.tev_colors[creg][2];
        return 1;
    }
    if (arg == GX_CC_KONST) {
        int k = ts->k_color_sel;          /* GX_TEV_KCSEL_K0..K3 are 0xC..0xF */
        if (k < 0xC || k > 0xF) return 0;
        k -= 0xC;
        out[0] = g_gx.tev_k_colors[k][0];
        out[1] = g_gx.tev_k_colors[k][1];
        out[2] = g_gx.tev_k_colors[k][2];
        return 1;
    }
    return 0;
}

#ifdef DC_PVR_TEVP3
/* --- TEV class P3: the PVR's OFFSET COLOUR, which has never been wired ------
 *
 * WHAT IS BROKEN. The name-entry keyboard renders BLACK. 18 of its 26 display
 * lists (`mED_KeyDraw`, m_editor_ovl.c:2221-2258) are
 * `gsDPSetCombineLERP(PRIMITIVE, ENVIRONMENT, TEXEL0, ENVIRONMENT, 0,0,0,TEXEL0)`
 * = config #037, class P3 — and dc_pvr.c implements NO PART of P3, so both
 * constants are lost and the batch draws at `vtx.rgb * T0.rgb`. The DL clears
 * G_LIGHTING, so vtx.rgb is not a colour. P3 is 27 of the 101 configs.
 * This is not a stubbed asset: the textures are on both keep lists, and alpha
 * here is TEXEL0, so a zeroed texture would be INVISIBLE, not black.
 *
 * WHAT THE STAGE ACTUALLY IS, in GX terms — and note the slot rotation in
 * emu64::combine_auto (emu64.c:1219-1244) against tblc (emu64.c:325-346):
 * `color_b = tblc[N64_a][0]`, `color_a = tblc[N64_b][1]`, and so on. emu64
 * parks PRIM in GX_TEVREG1 and ENV in GX_TEVREG2, so #037 arrives here as the
 * single stage `(a,b,c,d) = (C2, C1, TEXC, ZERO)`. GX evaluates
 * `d + (1-c)a + c*b` = `(1-T0)*ENV + T0*PRIM` = **`ENV + (PRIM-ENV)*T0`**.
 * ⚠️ kb/tev-map-hard-cases.md §6.6 describes the same stage in N64 names
 * ("b/c are ENVIRONMENT and TEXEL0"); this is the GX spelling of it.
 *
 * WHY EVERY EXISTING PATH REFUSES IT:
 *   - tev_const_color() dies at its FIRST test (:994) — it requires
 *     `color_b == color_c == ZERO` and here they are C1 and TEXC.
 *   - tev_fold_color() dies inside tev_carg_affine(GX_CC_TEXC) (:1162, the
 *     default return). ⇒ **-DDC_PVR_TEVFOLD provably cannot fix this**, which
 *     is a free falsification test. Even if TEXC were accepted, a texture in
 *     `c` needs a third coefficient that function does not carry.
 *
 * THE FIX, and it is native — one pass, no extra geometry, no second batch.
 * The PVR has an OFFSET COLOUR that is ADDED to the texture-env result
 * (KOS 2.3, dc/pvr/pvr_header.h:121-122: "the offset color (aka. oargb) ... is
 * added to the result. Its alpha channel is ignored"), enabled by
 * `pvr_poly_cxt_t.gen.specular` (pvr.h:181) which compiles to PVR_TA_CMD_SPECULAR
 * = BIT(2) of the PCW (pvr.h:605, pvr_prim.c:45), and carried per vertex in
 * `pvr_vertex_t.oargb` (pvr.h:437). So:
 *
 *     vertex base colour = PRIM - ENV      (modulated by the texel)
 *     vertex oargb       = ENV             (added afterwards)
 *     => PVR computes  (PRIM-ENV)*T0 + ENV  == the GX result, exactly.
 *
 * Per-vertex cost is ZERO: pvr_vertex_t already has the field and
 * submit_prim() already sends all 32 bytes.
 *
 * ⚠️ THE ONE PLACE IT IS NOT EXACT, and it is live on the target widget.
 * argb/oargb are packed UNSIGNED bytes, so `PRIM - ENV` must be clamped at 0
 * here. Where PRIM < ENV in a channel the surface flattens to ENV and stops
 * responding to the texture in that channel — kb/tev-map-hard-cases.md §6.2
 * predicted exactly this. kai_sousa.c:520-521 is PRIM(65,95,165) /
 * ENV(125,45,225), i.e. base (-60,+50,-60): R and B clamp, and that element
 * will render flatter than GX **in the fixed build**. That is the clamp showing
 * itself, not a regression — and its presence is evidence the path fired.
 * s_p3_clamped counts it so the frequency is a measurement, not an argument.
 * The exact remedy (invert the texture, swap the roles) needs an offline asset
 * pass and is out of scope.
 * Top-end overflow cannot bite this predicate: with base = PRIM-ENV >= 0 and
 * offset = ENV, base + offset = PRIM <= 1 for any texel. The undocumented
 * gen.color_clamp bit (pvr.h:178, which pvr_header.h:285 calls fog_clamp) is
 * therefore MOOT here; -DDC_PVR_P3_CLRCLAMP exists to probe it later.
 *
 * SCOPE, honestly. Of the 27 P3 configs this predicate handles NINE exactly
 * (10, 35, 37, 43, 51, 52, 55, 63, 74 — all `C2 + (C1-C2)*T0`) plus the
 * `a == ZERO` arm below. The remaining 12 have base and/or offset scaled by
 * RASC, i.e. per-vertex — the terrain/day-night tint family — and need
 * tev_carg_affine() widened to `k0 + k1*RASC + k2*T0`. #077's texture term is
 * T1, not T0, and is never reachable this way. Do not read "P3 implemented".
 *
 * Returns 1 and fills base[3]/off[3] in 0..1 float. */
static int tev_p3_affine(float* base, float* off) {
    const DCGXTevStage* ts;
    int creg_b, creg_a, i;

    if (g_gx.num_tev_stages != 1)
        return 0;
    ts = &g_gx.tev_stages[0];

    if (ts->color_op != GX_TEV_ADD)
        return 0;
    /* The texture must be the LERP WEIGHT and nothing may bypass it. */
    if (ts->color_c != GX_CC_TEXC || ts->color_d != GX_CC_ZERO)
        return 0;

    creg_b = tev_creg_of(ts->color_b);
    if (creg_b < 0)
        return 0;

    /* The `a == ZERO` arm is the P2 shape `C1 * T0`, which needs no offset
     * colour at all and no specular bit. It is here rather than in a separate
     * function because it is THE SAME WIDGET: kai_sousa.c:387,396,406 draw
     * three more of the keyboard's pieces as
     * `gsDPSetCombineLERP(PRIMITIVE, 0, TEXEL0, 0, ...)`, which is equally
     * unimplemented today and equally black. -DDC_PVR_TEVP3_NOP2 refuses it, so
     * "P3 restored" stays separable from "P2's constant restored" in the A/B. */
    if (ts->color_a == GX_CC_ZERO) {
#ifdef DC_PVR_TEVP3_NOP2
        return 0;
#else
        for (i = 0; i < 3; i++) {
            base[i] = g_gx.tev_colors[creg_b][i];
            off[i]  = 0.0f;
        }
        return 1;
#endif
    }

    creg_a = tev_creg_of(ts->color_a);
    if (creg_a < 0)
        return 0;

    for (i = 0; i < 3; i++) {
        base[i] = g_gx.tev_colors[creg_b][i] - g_gx.tev_colors[creg_a][i];
        off[i]  = g_gx.tev_colors[creg_a][i];
    }
    return 1;
}
#endif /* DC_PVR_TEVP3 */

#ifdef DC_PVR_TEVFOLD
/* --- TEV stage 0, GENERALISED: fold the whole affine stage into the vertex --
 *
 * tev_const_color() above recognises exactly one shape — `b == c == ZERO` with
 * a constant in `a` or `d`. Everything else falls through and the batch draws
 * with the rasterised colour, silently dropping whatever the combiner asked
 * for. That blind spot is wide: `c != ZERO` covers every lerp and every
 * `const x RASC` modulate, which between them are 24 of the 34 single-texmap
 * multi-stage configs plus 14 single-stage ones in kb/tev-map-table.md.
 *
 * The observation that closes most of it: GX's stage output is
 * `d + (1-c)*a + c*b`. If none of {a,b,c,d} is a TEXTURE or a previous-stage
 * term — i.e. each is drawn from {ZERO, ONE, HALF, C0..C2, A0..A2, KONST,
 * RASC} — then the result is AFFINE in the rasterised colour:
 *
 *     stage0 = K0 + K1 * RASC          (per channel, K0/K1 CPU-known)
 *
 * and because RASC is shade_vertex()'s own per-vertex output, that folds into
 * the vertex colour exactly, with no offset colour and no second pass:
 *
 *     vtx.rgb := clamp(K0 + K1 * shade_rgb)
 *
 * PVR_TXRENV_MODULATE(ALPHA) then computes `(K0 + K1*RASC) * T0`, which is what
 * GX computes. When K1 == 0 this degenerates to "replace the vertex RGB with a
 * constant" — byte-for-byte the existing behaviour — which is why this runs
 * only AFTER tev_const_color() has declined, and can only add coverage.
 *
 * THE NAMED INSTANCE: the train window's scenery band, which is the thing a
 * human called "the mountains behind the train look messed up".
 * `rom_train_out_bgtree_modelT` (rom_train_out.c:99) is
 *     gsDPSetCombineLERP(PRIMITIVE, 0, PRIM_LOD_FRAC, ENVIRONMENT, 0,0,0,TEXEL0,
 *                        TEXEL1, 0, COMBINED, 0, 0,0,0, COMBINED)
 * and emu64's hand-written case (emu64.c:1753-1763) turns stage 0 into
 * `(a,b,c,d) = (ZERO, C1, A0, C2)` = **ENV + PRIM_LOD_FRAC * PRIM**, a pure
 * constant with no texture and no raster term. PRIM is literally the
 * time-of-day sun+ambient colour — `aTrainWindow_SetLightPrimColorDetail`
 * (ac_train_window.c:435-485) sets it from
 * `global_light.ambientColor + kankyo.base_light.sun_color` every frame — and
 * ENV is `gsDPSetEnvColor(60, 60, 35, 255)`, the darkening that makes the band
 * read as distant scenery. tev_const_color() rejects it at its very first test
 * (`color_b == GX_CC_C1`, not ZERO), so the port draws `vtx.cn * T0` instead:
 * no ENV darkening, no day/night, and a washed-out band.
 *
 * DELIBERATE LIMITS, each of which fails CLOSED:
 *   - `c` must be a pure constant (K1 == 0 for it). If `c` is RASC the product
 *     `c*b` is quadratic in the raster colour and no vertex fold exists.
 *   - `color_op` must be GX_TEV_ADD. combine_auto emits GX_TEV_SUB when
 *     `color_a != ZERO && color_a != color_d` (emu64.c:1237-1241); the field is
 *     recorded by dc_gx.c:1264-1278 and, until now, never read by anything.
 *   - any TEXC / TEXA / CPREV / APREV / RASA / unknown arg -> decline.
 *   - GX_CC_CPREV is 0 and so is TEV_COMBINED, which do NOT mean the same
 *     thing (kb/traps.md). tev_carg_affine() rejects it explicitly rather than
 *     treating it as a register.
 *
 * WHAT IT STILL GETS WRONG. Stage 1 is not read here any more than it was
 * before. For the P3 shapes whose second stage is `CPREV + TEXC*CPREV` — the
 * tree band among them — GX's answer is `K*(1 + T0)` and this produces `K*T0`,
 * i.e. correct hue and correct day/night response, about K too dark. That is a
 * different error from today's (wrong hue, no day/night at all), not a smaller
 * one by construction, so it is judged on a screenshot, not on this comment.
 *
 * ⚠️ MEASURED 2026-08-03, AND IT REGRESSES SOMETHING. A 600 s run with this on
 * (against an otherwise identical build, both reaching the town, both with
 * ASSET MISSING = 0) passes every counter — no ptdrop, no LOST, no new blank
 * textures, FPS within noise — and the train window's scenery band visibly
 * darkens, which is the ENV term correctly arriving. But **the train station
 * canopy renders as a flat teal slab with no texture at all**, where the same
 * build without this shows correctly textured beams.
 *
 * A flat, UNTEXTURED result is the clue and it does not fit "the vertex colour
 * was replaced by a constant" — that would still be modulated by the texel.
 * The likely shape is the one at dc_pvr.c's GX_TEXMAP_NULL guard: a config
 * whose stage 0 binds GX_TEXMAP_NULL and carries the texture on stage 1, so
 * `tex` is nulled, the batch is untextured, and the folded constant becomes the
 * whole colour. emu64.c:1764-1773 is exactly that shape and its true output is
 * `T0 * lerp(RASC, C1, A0)`. That guard reads only stage 0 and should ask
 * whether ANY stage binds a texmap. Fixing it is probably a precondition for
 * turning the fold on, not a separate job.
 *
 * So: this is off, it is not ready, and the next step is that guard plus the
 * oargb work above — not a wider fold.
 *
 * Kill switch: the whole thing is opt-in behind -DDC_PVR_TEVFOLD.
 * -DDC_PVR_TEVFOLD_NORASC narrows it to K1 == 0 (constants only, never scaling
 * the vertex colour), which separates "constants restored" from "18 configs now
 * modulate the shade" in a single build. */
static int tev_carg_affine(const DCGXTevStage* ts, int arg,
                           float* k0, float* k1) {
    int creg;

    k1[0] = k1[1] = k1[2] = 0.0f;

    switch (arg) {
        case GX_CC_ZERO: k0[0] = k0[1] = k0[2] = 0.0f; return 1;
        case GX_CC_ONE:  k0[0] = k0[1] = k0[2] = 1.0f; return 1;
        case GX_CC_HALF: k0[0] = k0[1] = k0[2] = 0.5f; return 1;
        case GX_CC_RASC:
            k0[0] = k0[1] = k0[2] = 0.0f;
            k1[0] = k1[1] = k1[2] = 1.0f;
            return 1;
        default: break;
    }

    /* C0/C1/C2 — a whole constant register. tev_creg_of() also refuses
     * GX_CC_CPREV for the aliasing reason in kb/traps.md. */
    creg = tev_creg_of(arg);
    if (creg >= 0) {
        k0[0] = g_gx.tev_colors[creg][0];
        k0[1] = g_gx.tev_colors[creg][1];
        k0[2] = g_gx.tev_colors[creg][2];
        return 1;
    }

    /* A0/A1/A2 — a register's ALPHA, broadcast to all three channels. These
     * are GX_CC_A0=3, A1=5, A2=7, interleaved with the colour selectors, so
     * the register index is (arg-1)/2 into the same tev_colors[] table
     * ([0] = GX_TEVPREV, [1..3] = GX_TEVREG0..2). This is the arg
     * tev_creg_of() returns -1 for, and it is exactly the PRIM_LOD_FRAC term
     * the tree band multiplies its sun colour by. */
    if (arg == GX_CC_A0 || arg == GX_CC_A1 || arg == GX_CC_A2) {
        int r = (arg - 1) / 2;
        k0[0] = k0[1] = k0[2] = g_gx.tev_colors[r][3];
        return 1;
    }

    if (arg == GX_CC_KONST) {
        int k = ts->k_color_sel;          /* GX_TEV_KCSEL_K0..K3 are 0xC..0xF */
        if (k < 0xC || k > 0xF) return 0;
        k -= 0xC;
        k0[0] = g_gx.tev_k_colors[k][0];
        k0[1] = g_gx.tev_k_colors[k][1];
        k0[2] = g_gx.tev_k_colors[k][2];
        return 1;
    }

    return 0;                             /* TEXC/TEXA/CPREV/APREV/RASA/... */
}

static int tev_fold_color(float* k0, float* k1) {
    const DCGXTevStage* ts;
    float a0[3], a1[3], b0[3], b1[3], c0[3], c1[3], d0[3], d1[3];
    int i;

    if (g_gx.num_tev_stages < 1)
        return 0;
    ts = &g_gx.tev_stages[0];

    if (ts->color_op != GX_TEV_ADD)
        return 0;

    if (!tev_carg_affine(ts, ts->color_a, a0, a1)) return 0;
    if (!tev_carg_affine(ts, ts->color_b, b0, b1)) return 0;
    if (!tev_carg_affine(ts, ts->color_c, c0, c1)) return 0;
    if (!tev_carg_affine(ts, ts->color_d, d0, d1)) return 0;

    /* A raster-dependent lerp weight makes the result quadratic in RASC. */
    if (c1[0] != 0.0f || c1[1] != 0.0f || c1[2] != 0.0f)
        return 0;

    for (i = 0; i < 3; i++) {
        k0[i] = d0[i] + (1.0f - c0[i]) * a0[i] + c0[i] * b0[i];
        k1[i] = d1[i] + (1.0f - c0[i]) * a1[i] + c0[i] * b1[i];
    }

#ifdef DC_PVR_TEVFOLD_NORASC
    if (k1[0] != 0.0f || k1[1] != 0.0f || k1[2] != 0.0f)
        return 0;
#endif
    return 1;
}
#endif /* DC_PVR_TEVFOLD */

/* --- TEV stage 0: the ALPHA half of the same idea -------------------------
 *
 * THE FONT BUG, 2026-08-02. Exactly two things in the dialogue balloon never
 * rendered: the speaker name (m_msg_draw_window.c_inc:48) and the reply
 * options (m_choice_draw.c_inc:146). They are the only text that goes through
 * mFont_SetLineStrings_AndSpace, which sets mFont_SENTENCE_FLAG_USE_POLY
 * unconditionally (m_font_main.c_inc:518) and so draws real geometry
 * (mFont_gppDrawCharPoly, m_font_main.c_inc:672-728) instead of a texture
 * rectangle. The body text takes the rect path and works.
 *
 * mFont_SetVertex_dol (m_font_main.c_inc:348-362) writes cn[0..3] = 0 into
 * every glyph vertex, and the font display list clears G_LIGHTING, so emu64
 * programs GXSetChanCtrl(GX_COLOR0A0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, ...)
 * (emu64.c:3327) — material source VTX. shade_vertex() therefore returns
 * 0x00000000 and PVR_TXRENV_MODULATEALPHA multiplies the glyph away.
 * Confirmed in a batch log before this was written: 47 balloon batches at
 * verts=6 zt=0 st=1 tm=0,255 reading argb=00001E00, alpha byte zero, against
 * body-text batches on the same frame reading argb=FFxxxxxx.
 *
 * On GX that zero is harmless: mFont_CC_FONT (m_font.c:17) is
 * `0, 0, 0, PRIMITIVE, PRIMITIVE, 0, TEXEL0, 0` — colour = PRIM, alpha =
 * PRIM.a * TEXEL0.a — and it never reads RASC or RASA. tev_const_color()
 * above already restores the RGB half; this restores the alpha half.
 *
 * The rect path escapes because emu64::draw_rectangle re-programs
 * GXSetChanCtrl(..., GX_SRC_REG, GX_SRC_REG, ...) (emu64.c:3076) and never
 * declares GX_VA_CLR0 (emu64.c:3119-3123), so its vertices shade to the
 * material register — which emu64 sets WHITE once (emu64.c:562) and never
 * touches again.
 *
 * SHAPE, deliberately narrow: a = ZERO, c = TEXA, d = ZERO, b = a constant
 * alpha register. Under GX's `d + (1-c)*a + c*b` that is exactly TEXA * const,
 * and it is the form emu64 emits — both replace_combine_to_tev
 * (emu64.c:1092-1096) and combine_auto (emu64.c:1245-1247) move the N64 `a`
 * term into GX's `b` slot and force GX's `a` to ZERO. RASA is not referenced
 * in this shape, so overriding the vertex alpha cannot discard anything the
 * combiner asked for. GX_MODULATE's default (c = RASA, dc_gx.c:1012) does not
 * match and is untouched.
 *
 * ⚠️ Blast radius, stated honestly: the `PRIMITIVE, 0, TEXEL0, 0` alpha shape
 * occurs 446 times in src/. Everywhere it fires the constant is the correct
 * answer and the current vertex alpha is the wrong one, but this reaches far
 * more than the font. -DDC_PVR_TEVCONST_ALPHA_RESCUE_ONLY narrows it to the
 * vertices that are currently fully invisible, which separates "the font came
 * back" from "hundreds of world batches changed alpha" in one build.
 *
 * Kill switch: -DDC_PVR_NO_TEVCONST_ALPHA. -DDC_PVR_NO_TEVCONST kills both
 * halves, as before. */
#ifndef DC_PVR_NO_TEVCONST_ALPHA
/* Non-zero if an alpha combiner arg names a CPU-known constant, and if so
 * writes it to *out.
 *
 * GX_CA_A0/A1/A2 are 1/2/3 and index tev_colors[] directly: [0] is GX_TEVPREV,
 * [1..3] are GX_TEVREG0..2. emu64 parks the N64 primitive colour in GX_TEVREG1
 * and the environment colour in GX_TEVREG2 (emu64.c:3171,3180), so a PRIMITIVE
 * alpha arg lands on tev_colors[2][3] — the alpha gDPSetPrimColor set.
 *
 * GX_CA_APREV is 0 and is NOT a constant; the `>= GX_CA_A0` test excludes it,
 * which is the alpha-side twin of the GX_CC_CPREV trap in tev_creg_of(). */
static int tev_aarg_const(const DCGXTevStage* ts, int arg, float* out) {
    if (arg >= GX_CA_A0 && arg <= GX_CA_A2) {
        *out = g_gx.tev_colors[arg][3];
        return 1;
    }
    if (arg == GX_CA_KONST) {
        int k = ts->k_alpha_sel;   /* GX_TEV_KASEL_K0_A..K3_A are 0x1C..0x1F */
        if (k < 0x1C || k > 0x1F) return 0;
        *out = g_gx.tev_k_colors[k - 0x1C][3];
        return 1;
    }
    return 0;
}

static int tev_const_alpha(float* out) {
    const DCGXTevStage* ts;
    int konst;

    if (g_gx.num_tev_stages < 1)
        return 0;
    ts = &g_gx.tev_stages[0];

    /* --- THE MIRRORED FORM, 2026-08-02: the train window's shine ----------
     *
     * The RDP alpha cycle is (a - b) * c + d, so `TEXEL0.a * PRIM.a` can be
     * written either way round, and this game writes it BOTH ways.
     * combine_auto moves the N64 `a` term into GX's `b` slot and the N64 `c`
     * term into GX's `c` slot (emu64.c:1226-1230), so the two spellings do
     * NOT collapse:
     *
     *   N64 (PRIMITIVE, 0, TEXEL0, 0) -> GX (ZERO, A1,   TEXA, ZERO)   40 sites
     *   N64 (TEXEL0, 0, PRIMITIVE, 0) -> GX (ZERO, TEXA, A1,   ZERO)  688 sites
     *
     * The original shape recognised only the first, which is why it fired on
     * the font and almost nowhere else. The second is 17x more common and it
     * carries the train window's day/night gate.
     *
     * (Counted semantically: every gsDPSetCombineLERP in src/ pushed through
     * emu64's real combine_auto tables and its 33 combine_manual cases, then
     * matched at GX stage 0 — which is what this function actually tests. An
     * earlier note in kb/RESUME.md said "446", but that was a raw text grep
     * for `PRIMITIVE, 0, TEXEL0, 0` which also hits COLOUR slots.)
     *
     * rom_train_out_shineglass_modelT (rom_train_out.c:114-116), the literal
     * "shine on the glass", is
     *     gsDPSetCombineLERP(0,0,0,PRIMITIVE, TEXEL0,0,TEXEL1,0,
     *                        0,0,0,COMBINED,  COMBINED,0,PRIM_LOD_FRAC,0)
     * and combine_manual (emu64.c:1775-1786) turns it into
     *     stage0 alpha = (ZERO, TEXA,  A0,   ZERO) = TEXEL0.a * PRIM_LOD_FRAC
     *     stage1 alpha = (ZERO, APREV, TEXA, ZERO) =           * TEXEL1.a
     * PRIM_LOD_FRAC is GX_CA_A0 = GX_TEVREG0.a, which emu64 loads out of
     * fill_tev_color (emu64.c:3238) whose .a is gsDPSetPrimColor's `l` field
     * (emu64.c:4377). The actor writes window->lod_factor there
     * (ac_train_window.c:534), and lod_factor IS the time of day: 0 before
     * 04:00, ramping to 160/255 at noon, back to 0 by 20:00
     * (ac_train_window.c:279-289).
     *
     * Dropping it made the glare permanent. rom_train_out_v[32..47], decoded
     * out of the retail foresta.rel, carry cn.a = 178 and 88, so
     * MODULATEALPHA drew the shine at a fixed 70 % / 35 % of its texel alpha
     * day AND night. shineglass has no gsDPSetRenderMode of its own — it
     * inherits ZB_XLU_SURF2 from bgtree — so it is a plain alpha-blended XLU
     * quad over a flat constant colour: its alpha is the whole of its
     * appearance.
     *
     * Kill switch for the mirror alone: -DDC_PVR_NO_TEVCONST_ALPHA_MIRROR.
     * -DDC_PVR_TEVCONST_ALPHA_LODONLY narrows it to PRIM_LOD_FRAC (GX_CA_A0),
     * 134 of the 688, which is enough for the window on its own. */
    if (ts->alpha_a != GX_CA_ZERO || ts->alpha_d != GX_CA_ZERO)
        return 0;

    /* One of b/c is the texel, the other is the constant. If both are TEXA
     * the first arm takes it and konst == GX_CA_TEXA falls through to 0. */
    if (ts->alpha_c == GX_CA_TEXA) {
        konst = ts->alpha_b;
    }
#ifndef DC_PVR_NO_TEVCONST_ALPHA_MIRROR
    else if (ts->alpha_b == GX_CA_TEXA) {
        konst = ts->alpha_c;
#ifndef DC_PVR_TEVCONST_ALPHA_WIDE
        /* ⚠️ NARROWED 2026-08-02, from a human report. Accepting all three
         * constant registers here is 688 sites, and turning that on broke the
         * title-screen train station with missing textures — an A1 (PRIM.a) or
         * A2 (ENV.a) constant that the game leaves low will make a whole
         * textured batch nearly transparent, which reads as a missing texture.
         *
         * A0 alone is PRIM_LOD_FRAC, 134 sites, and it is the register the
         * train window's day/night shine actually uses — so this keeps the fix
         * that motivated the mirror and drops the 554 sites of risk that came
         * with it. -DDC_PVR_TEVCONST_ALPHA_WIDE restores all three.
         *
         * Widening this again needs the per-register evidence that is missing
         * today: which A1/A2 sites have a constant alpha the game expects to
         * be honoured, versus one it expects the RDP to have ignored. */
        if (konst != GX_CA_A0) return 0;
#endif
    }
#endif
    else {
        return 0;
    }

    /* Register indexing and the GX_CA_KONST selector both live in
     * tev_aarg_const() above; this call is that code verbatim. */
    return tev_aarg_const(ts, konst, out);
}

/* --- The LAST TEV stage: `alpha = APREV * <constant>` ----------------------
 *
 * tev_const_alpha() above reads STAGE 0 and nothing else, so a constant that
 * the combiner applies at the END of the chain is dropped. The shape that
 * matters is `A = APREV * PRIM.a`: emu64 emits it as the final stage of seven
 * combine_manual cases (emu64.c:1574,1588,1602,1616,1630,1644,1658 — all
 * 3-stage, all `GXSetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_APREV,
 * GX_CA_A1, GX_CA_ZERO)`), and combine_auto reproduces it for any two-cycle
 * combiner whose cycle-1 alpha is `COMBINED, 0, PRIMITIVE, 0` and whose
 * cycle-0 alpha does not touch TEXEL1 (emu64.c:1306; the TEXEL1 reject that
 * sends the rest to combine_manual is emu64.c:1197-1207). Under GX's
 * `d + (1-c)*a + c*b` with a = d = ZERO that is exactly APREV * konst.
 *
 * COUNTED, not guessed: 28 of the 5,512 16-argument gsDPSetCombineLERP sites in
 * src/ end their alpha with `COMBINED, 0, PRIMITIVE, 0`, and 3 of those reach
 * combine_auto. So this is a narrow fix by construction, and the counter below
 * is how it is judged rather than this paragraph.
 *
 * PRIM.a is where the game keeps the per-frame fade of an effect. The named
 * instance is the outdoor character shadow: `ef_shadow_out.c:34-35` is
 *     gsDPSetCombineLERP(0,0,0,PRIMITIVE, TEXEL0,0,TEXEL1,0,
 *                        0,0,0,COMBINED,  COMBINED,0,PRIMITIVE,0)
 *     gsDPSetRenderMode(G_RM_FOG_SHADE_A, G_RM_ZB_CLD_SURF2)
 * — RGB is a flat dark PRIMITIVE with no texture at all, so the ALPHA is the
 * entire shape of the primitive, and gDPSetPrimColor's `a` is
 * play->kankyo.shadow_alpha (m_actor_shadow.c:54,262-271), the thing that makes
 * a shadow faint rather than opaque. Drop any alpha factor and a flat dark quad
 * is painted instead: kb/tev-map-table.md config #007.
 *
 * ⚠️ AND THIS DOES NOT FIRE ON #007. Read against emu64 rather than against the
 * combiner, #007 is TWO stages, not three (emu64.c:1888-1897):
 *     stage0 alpha = (ZERO, TEXA,  A1,    ZERO) = TEXEL0.a * PRIM.a
 *     stage1 alpha = (ZERO, TEXA,  APREV, ZERO) = APREV * TEXEL1.a
 * The constant is on stage 0 in the MIRRORED spelling, where tev_const_alpha()
 * above finds it and then discards it at its `konst != GX_CA_A0` narrowing —
 * and the last stage is APREV * TEXEL1.a, not APREV * konst, so
 * tex1_alpha_active()'s (ZERO, APREV, TEXA, ZERO) test misses it too. #007 is
 * therefore losing BOTH of its alpha factors today, and neither loss is this
 * function's to repair: the first wants -DDC_PVR_TEVCONST_ALPHA_WIDE (or a
 * narrower A1 arm), the second wants the mirrored shape in tex1_alpha_active().
 * Both are already-documented, already-regressed widenings and both want their
 * own screenshot pair.
 *
 * MULTIPLIES, never overwrites. GX chains the stages, so a stage-0 constant and
 * a last-stage constant are two factors of one product; taking the last one
 * alone would silently delete the first. When stage 0 declined, the constant
 * REPLACES the shade alpha, exactly as the stage-0 path already does, because
 * the shade alpha is not an opacity in this game (see the punch-through block
 * in dc_gx_backend_submit) — the one thing that would make that a loss is a
 * RASA anywhere earlier in the chain, and that is guarded below.
 *
 * DELIBERATE LIMITS, each failing CLOSED:
 *   - the last stage only, and only when there IS a last stage other than 0.
 *     APREV on stage 0 reads whatever GX_TEVPREV was left holding.
 *   - `(a,b,c,d) == (ZERO, APREV, <const>, ZERO)` and nothing else. The
 *     mirrored `(ZERO, <const>, APREV, ZERO)` is the same product but has zero
 *     sites (checked across all 33 combine_manual cases), so it is not
 *     accepted on speculation.
 *   - alpha_op must be GX_TEV_ADD. emu64::combine() resets every stage's op to
 *     ADD before each combine (emu64.c:1974-1976) and combine_auto re-arms SUB
 *     (emu64.c:1314), so a SUB here is a real subtract and not stale state.
 *   - no earlier stage may reference GX_CA_RASA. combine_manual has one such
 *     case (emu64.c:1552) and combine_auto can emit it from an N64 SHADE term
 *     (the tbla row at emu64.c:324); there the rasterised alpha this is about
 *     to overwrite is a term GX really wanted.
 *
 * NOT in header_key(): this changes the vertex colour bytes, not one field that
 * compile_header() reads, so a batch that takes this path and one that does not
 * compile to the same poly header and must keep sharing the cache entry.
 *
 * Kill switch: -DDC_PVR_NO_TEVALPHA_LAST restores today's behaviour exactly.
 * -DDC_PVR_NO_TEVCONST_ALPHA and -DDC_PVR_NO_TEVCONST still kill it with the
 * rest of the family. Counter: `tevalpha_last batches=` in dc_pvr_report(). */
#ifndef DC_PVR_NO_TEVALPHA_LAST
static int tev_const_alpha_last(float* out) {
    const DCGXTevStage* ts;
    int ns, si;

    ns = g_gx.num_tev_stages;
    if (ns < 2)                       /* stage 0 belongs to tev_const_alpha */
        return 0;
    /* Clamp to the ARRAY bound, not DC_GX_MAX_TEV_STAGES — that constant is an
     * observation (kb/tev-map-table.md §2), and indexing past 16 would be a
     * read out of g_gx.tev_stages[]. */
    if (ns > (int)(sizeof g_gx.tev_stages / sizeof g_gx.tev_stages[0]))
        return 0;

    ts = &g_gx.tev_stages[ns - 1];
    if (ts->alpha_op != GX_TEV_ADD)
        return 0;
    if (ts->alpha_a != GX_CA_ZERO || ts->alpha_d != GX_CA_ZERO)
        return 0;
    if (ts->alpha_b != GX_CA_APREV)
        return 0;
    if (!tev_aarg_const(ts, ts->alpha_c, out))
        return 0;

    for (si = 0; si < ns - 1; si++) {
        const DCGXTevStage* p = &g_gx.tev_stages[si];
        if (p->alpha_a == GX_CA_RASA || p->alpha_b == GX_CA_RASA ||
            p->alpha_c == GX_CA_RASA || p->alpha_d == GX_CA_RASA)
            return 0;
    }
    return 1;
}
#endif /* !DC_PVR_NO_TEVALPHA_LAST */
#endif /* !DC_PVR_NO_TEVCONST_ALPHA */
#endif /* !DC_PVR_NO_TEVCONST */

/* The vertex colour bytes, in the TA's ARGB word order. Three paths below
 * return exactly this and it used to be written out three times. */
static inline unsigned int pack_vtx_argb(const DCGXVertex* v) {
    return ((unsigned int)v->color0[3] << 24) |
           ((unsigned int)v->color0[0] << 16) |
           ((unsigned int)v->color0[1] << 8) | (unsigned int)v->color0[2];
}

/* --- THE SHADE PREDICATES ARE PER-BATCH, AND THEY USED TO BE PER-VERTEX -----
 *
 * Everything shade_vertex() branches on is g_gx channel state: num_chans and
 * chan_ctrl_{enable,mat_src,amb_src}[]. NONE of it can change inside one
 * dc_gx_backend_submit() call — GXSetChanCtrl is the only writer of the three
 * chan_ctrl_* arrays (dc_gx.c:1907) and it calls
 * dc_gx_flush_if_begin_complete() before every write, as do GXSetNumChans
 * (dc_gx.c:1878) and the rest of the lighting-state family. So a batch's
 * vertices provably all take the same branches.
 *
 * Session 12 measured the consequence of NOT knowing that: the LAZYRGBA and
 * ALPHA8 shortcuts below are exact, they fire on essentially every lit vertex
 * in the town, and they still measured NEGATIVE — `shade=` 2.09 -> 1.94 ms
 * while `xform - sum` went 0.44 -> 0.71 and `us/v` did not move. Three g_gx
 * loads and three branches per vertex to save three int->float converts is
 * moving work, not removing it. (kb/RESUME.md session 12, "what did NOT pay".)
 *
 * shade_batch_mode() therefore evaluates all of them ONCE, next to
 * `need_light`, and hands the answer down as a bitmask. Kill switch
 * -DDC_PVR_NO_SHADE_HOIST calls it per vertex instead, which is an exact
 * behavioural revert through the same code path. */
enum {
    SHADE_PASSTHRU = 1u << 0, /* the answer is the vertex colour, unchanged */
    SHADE_NEED_RGB = 1u << 1, /* build rgba[0..2] for chan_eval to read      */
    SHADE_NEED_A   = 1u << 2, /* build rgba[3]                               */
    SHADE_A8       = 1u << 3  /* the alpha lane is a byte copy               */
};

static unsigned int shade_batch_mode(void) {
#ifdef DC_PVR_NO_LIGHTING
    return SHADE_PASSTHRU;
#else
    unsigned int m = 0;

    /* numChans == 0 means the TEV takes no rasterised colour at all. Passing
     * the vertex colour through is wrong in principle but is what makes
     * untextured UI geometry visible instead of black. */
    if (g_gx.num_chans == 0)
        return SHADE_PASSTHRU;

#ifndef DC_PVR_NO_SHADEFAST
    /* THE PASS-THROUGH CASE, and it is the common one.
     *
     * With no light enabled on either half of the channel and both material
     * sources set to GX_SRC_VTX, GX's whole channel equation collapses to
     * "the vertex colour", and chan_component() was spending four float
     * divides, four multiplies, four clamps and four float->int conversions
     * arriving back at the byte it started from. Returning the bytes is EXACT,
     * not an approximation: pack_argb() computes (int)(b/255*255 + 0.5) and the
     * round-trip error is ~1e-5, four orders of magnitude below the 0.5 that
     * would change the answer.
     *
     * This is also what licenses dc_gx_backend_submit() to skip the eye-space
     * position and the normal transform entirely — see `need_light` there. The
     * two predicates are still written out separately and must not drift:
     * every path below that reads `eye` or `nrm` is guarded by
     * chan_ctrl_enable[], and SHADE_PASSTHRU implies !need_light. */
    if (!g_gx.chan_ctrl_enable[0] && !g_gx.chan_ctrl_enable[1] &&
        g_gx.chan_ctrl_mat_src[0] == GX_SRC_VTX &&
        g_gx.chan_ctrl_mat_src[1] == GX_SRC_VTX)
        return SHADE_PASSTHRU;
#endif

#ifndef DC_PVR_SHADE_LAZYRGBA
    m |= SHADE_NEED_RGB | SHADE_NEED_A;
#else
    if (g_gx.chan_ctrl_mat_src[0] == GX_SRC_VTX ||
        g_gx.chan_ctrl_amb_src[0] == GX_SRC_VTX)
        m |= SHADE_NEED_RGB;
    if (g_gx.chan_ctrl_mat_src[1] == GX_SRC_VTX ||
        g_gx.chan_ctrl_amb_src[1] == GX_SRC_VTX)
        m |= SHADE_NEED_A;
#endif

#ifdef DC_PVR_SHADE_ALPHA8
    if (!g_gx.chan_ctrl_enable[1] &&
        g_gx.chan_ctrl_mat_src[1] == GX_SRC_VTX) {
        m |= SHADE_A8;
        m &= ~(unsigned int)SHADE_NEED_A;
    }
#endif

    return m;
#endif
}

static unsigned int shade_vertex(const DCGXVertex* v, const float* eye,
                                 const float* nrm, unsigned int mode) {
#ifdef DC_PVR_NO_LIGHTING
    (void)eye; (void)nrm; (void)mode;
    return pack_vtx_argb(v);
#else
    float rgba[4];

    /* Both pass-through cases, decided once per batch in shade_batch_mode().
     * The caller hoists this test out of the vertex loop as well, so on a
     * pass-through batch shade_vertex() is not even called. */
    if (mode & SHADE_PASSTHRU)
        return pack_vtx_argb(v);

#ifdef DC_PVR_NO_SHADEFAST
    rgba[0] = v->color0[0] * (1.0f / 255.0f);
    rgba[1] = v->color0[1] * (1.0f / 255.0f);
    rgba[2] = v->color0[2] * (1.0f / 255.0f);
    rgba[3] = v->color0[3] * (1.0f / 255.0f);
    return pack_argb(chan_component(0, 0, rgba, eye, nrm, 0),
                     chan_component(0, 0, rgba, eye, nrm, 1),
                     chan_component(0, 0, rgba, eye, nrm, 2),
                     chan_component(0, 1, rgba, eye, nrm, 3));
#else
    {
        float out[4];
        /* --- S2: BUILD ONLY THE rgba LANES THAT ARE ACTUALLY READ ----------
         *
         * `chan_eval` reads `vtx_rgba[comp]` in exactly two places — the
         * material fetch and the ambient fetch — and each is guarded by its
         * channel control's own source being GX_SRC_VTX. In the town, emu64
         * programs the colour half with amb_src = mat_src = GX_SRC_REG
         * (emu64.c:3317), so `rgba[0..2]` were three byte loads, three
         * int->float converts and three multiplies producing values NOTHING
         * READ, on every lit vertex in the frame.
         *
         * The unread lanes are still WRITTEN, with 0.0f. That is deliberate:
         * leaving them uninitialised would make a future drift between this
         * predicate and chan_eval's guards read stack garbage — a much worse
         * failure than a wrong colour, and one that would not reproduce. A
         * store of zero is one instruction against the five it replaces.
         *
         * ⚠️ MEASURED 2026-08-08 AND DEFAULTED **OFF**, when the predicate was
         * evaluated here, per vertex. It is opt-in via
         * -DDC_PVR_SHADE_LAZYRGBA. Together with the alpha-byte shortcut below
         * it took `[VTXSPLIT] shade=` 2.09 -> 1.94 ms — and put MORE than that
         * back into the per-primitive loop: `xform - sum` went 0.44 -> 0.71 ms
         * and `us/v` did not move (2.65 -> 2.64).
         *
         * THE REASON WAS THE PREDICATE'S PLACEMENT, NOT THE IDEA — three g_gx
         * loads and three branches per vertex to save three int->float
         * converts. The predicate now lives in shade_batch_mode() and runs
         * once per batch; this block only reads the bit. */
        const int need_rgb = (mode & SHADE_NEED_RGB) != 0;
        const int need_a = (mode & SHADE_NEED_A) != 0;

        /* --- S3: THE ALPHA LANE IS A BYTE COPY, AND IT IS EXACT ------------
         *
         * When the alpha half is DISABLED and its material source is the
         * vertex — which is exactly what emu64 programs for every lit draw
         * (`GXSetChanCtrl(GX_ALPHA0, GX_FALSE, GX_SRC_REG, GX_SRC_VTX, ...)`,
         * emu64.c:3325) — chan_eval takes its early return and produces
         *
         *     out[3] = mat[3] = vtx_rgba[3] = v->color0[3] * (1/255)
         *
         * which pack_argb then turns back into `(int)(x*255 + 0.5)`, i.e. into
         * v->color0[3] again. The round-trip error is ~1e-5 against the 0.5
         * that would change the byte — the same argument this file already
         * makes for the whole-vertex pass-through above; it simply was never
         * applied to the alpha lane of the LIT path.
         *
         * So: emit the byte, and skip the second chan_eval call, its three
         * g_gx loads, the rgba[3] convert, and pack_argb's fourth lane.
         *
         * The predicate is a RUNTIME test of the same two fields chan_eval
         * would branch on, so it fails closed: any batch that does not match
         * takes the full path unchanged.
         *
         * ⚠️ ALSO DEFAULTED **OFF**, for the same reason as the block above —
         * opt-in via -DDC_PVR_SHADE_ALPHA8. The shortcut itself is exact and
         * it fires constantly (`shade_a8 verts=12,543,600` on a 600 s town
         * run), so what failed to pay was the PER-VERTEX predicate, not the
         * saving. The predicate is now per batch (shade_batch_mode(), which
         * also clears SHADE_NEED_A when it fires).
         *
         * Counter: `shade_a8=` in dc_pvr_report(). */
#ifdef DC_PVR_SHADE_ALPHA8
        const int a8 = (mode & SHADE_A8) != 0;
#endif

        if (need_rgb) {
            rgba[0] = v->color0[0] * (1.0f / 255.0f);
            rgba[1] = v->color0[1] * (1.0f / 255.0f);
            rgba[2] = v->color0[2] * (1.0f / 255.0f);
        } else {
            rgba[0] = rgba[1] = rgba[2] = 0.0f;
        }
        rgba[3] = need_a ? (v->color0[3] * (1.0f / 255.0f)) : 0.0f;

        chan_eval(0, 0, 0, 2, rgba, eye, nrm, out);   /* the colour half */
#ifdef DC_PVR_SHADE_ALPHA8
        if (a8) {
            s_shade_a8++;
            return pack_rgb_a8(out[0], out[1], out[2],
                               (unsigned int)v->color0[3]);
        }
#endif
        chan_eval(0, 1, 3, 3, rgba, eye, nrm, out);   /* the alpha half  */
        return pack_argb(out[0], out[1], out[2], out[3]);
    }
#endif
#endif
}

/* ==========================================================================
 * Poly header
 * ========================================================================== */

/* One integer that changes whenever the compiled header would. Cheaper than
 * memcmp-ing a pvr_poly_cxt_t, and a false HIT is impossible because every
 * field that feeds pvr_poly_compile() below is folded in. */
static unsigned int header_key(const dc_pvr_tex_t* tex) {
    unsigned int k = 0;
    k = (k * 33u) + (unsigned int)g_gx.blend_mode;
    k = (k * 33u) + (unsigned int)g_gx.blend_src;
    k = (k * 33u) + (unsigned int)g_gx.blend_dst;
    k = (k * 33u) + (unsigned int)g_gx.z_compare_enable;
    k = (k * 33u) + (unsigned int)g_gx.z_compare_func;
    k = (k * 33u) + (unsigned int)g_gx.z_update_enable;
    k = (k * 33u) + (unsigned int)g_gx.cull_mode;
    /* The alpha test rewrites blend and depth.write in compile_header, so it is
     * part of the header's identity. Leave it out and the dedup below latches
     * whichever variant compiled first and the fix silently does nothing for
     * every batch that shares the rest of the state. */
    k = (k * 33u) + (unsigned int)g_gx.alpha_comp0;
    k = (k * 33u) + (unsigned int)g_gx.alpha_ref0;
    k = (k * 33u) + (unsigned int)g_gx.alpha_comp1;
    k = (k * 33u) + (unsigned int)g_gx.alpha_ref1;
    k = (k * 33u) + (unsigned int)g_gx.color_update_enable;
#ifndef DC_PVR_NO_FOG
    /* Fog ON/OFF is a header bit (gen.fog_type below), so it belongs in the
     * key — leave it out and the first batch of a scene latches whichever
     * variant compiled first and every later batch inherits it. The fog
     * PARAMETERS must NOT be here: they live in global registers, do not
     * change the compiled header, and hashing them would churn the cache. */
    k = (k * 33u) + (unsigned int)g_gx.fog_type;
    k = (k * 33u) + (unsigned int)g_gx.projection_type;
#endif
#ifndef DC_PVR_NO_PUNCHTHRU
    /* The list type is baked into the compiled header, so a PT header and a TR
     * header are different objects even when every other input agrees. Folding
     * the routing decision in is belt-and-braces — it is derived from state
     * already hashed above — but a header latched into the wrong list is a
     * whole-frame corruption, not a wrong pixel. */
    k = (k * 33u) + (unsigned int)s_pt_route;
#endif
    k = (k * 33u) + (unsigned int)(uintptr_t)tex;
    if (tex) {
        k = (k * 33u) + tex->pvr_fmt;
        k = (k * 33u) + (unsigned int)(uintptr_t)tex->base;
        /* Wrap is a property of the BIND, not the upload: two GXTexObjs can
         * share one cached VRAM image and wrap differently, so the same `tex`
         * pointer does NOT imply the same header. */
        k = (k * 33u) + (unsigned int)g_gx.tex_obj_wrap_s[0];
        k = (k * 33u) + (unsigned int)g_gx.tex_obj_wrap_t[0];
#ifdef DC_PVR_ALPHAENV
        /* The texture env is baked into the compiled header, so two batches
         * that agree on everything else and disagree on the alpha combiner are
         * DIFFERENT headers. Leave this out and ensure_header() hits the cache,
         * every later batch inherits whichever env compiled first, and the fix
         * silently does nothing — the same trap already documented above for
         * the alpha test and for fog. One integer, not the four raw stage
         * fields, so the cache does not churn. */
        k = (k * 33u) + (unsigned int)s_alpha_env_texel;
#endif
#ifdef DC_PVR_TEVP3
        /* The specular/offset-colour bit lives in the PCW, which is baked into
         * the compiled header — so it MUST be part of the key. Leave it out and
         * ensure_header() hits the cache, every later batch inherits whichever
         * value compiled first, and the whole fix silently does nothing. That
         * trap is already documented three times in this function (alpha test,
         * fog, alpha env); this is the fourth instance of it. */
        k = (k * 33u) + (unsigned int)s_p3_specular;
#endif
    }
    return k ? k : 1u;
}

/* Is the game asking for a real alpha TEST (a cutout), as opposed to "always
 * pass"? emu64 expresses one as GX_GEQUAL with a reference taken from
 * blend_color.a / tex_edge_alpha, and "no test" as GX_ALWAYS with ref 0
 * (emu64.c:2310-2319). A GX_GEQUAL with ref 0 passes everything, so it is not a
 * test either. g_gx is zero-initialised, which makes the untouched state
 * GX_NEVER/0 — inert here, so a batch drawn before anyone calls
 * GXSetAlphaCompare keeps today's behaviour. */
static int alpha_test_active(void) {
    return (g_gx.alpha_comp0 == GX_GEQUAL && g_gx.alpha_ref0 > 0) ||
           (g_gx.alpha_comp1 == GX_GEQUAL && g_gx.alpha_ref1 > 0);
}

/* The general (non-PT) list this build submits everything else to. */
#define DC_PVR_BASE_LIST \
    ((DC_PVR_LIST_MODE == 1) ? PVR_LIST_OP_POLY : PVR_LIST_TR_POLY)

/* Does the batch about to be submitted belong in the punch-through list?
 *
 * Three exclusions, each of which would be a regression rather than a fix:
 *
 *  - No alpha test: nothing to punch. Straight to the general list.
 *  - GX_BM_BLEND + alpha test: kb/tev-map-alpha.md §5.3 B3, a genuinely
 *    TRANSLUCENT cutout. PT forces a passing fragment's alpha to 1.0, so
 *    routing it here would make a fading sprite pop to fully opaque. §5.4
 *    option (b) says route the outliers to TR, and the existing blended-cutout
 *    path already handles them acceptably; -DDC_PVR_PT_ALL overrides this and
 *    sends every alpha-tested batch to PT, which is the one-flag experiment if
 *    a blended cutout still looks wrong.
 *  - GXSetColorUpdate(FALSE): a depth-only pass, expressed here as
 *    src=ZERO dst=ONE (see the colour-mask block in compile_header). The PT
 *    list is the wrong place for a pass whose entire purpose is to not paint.
 *
 * Read on EVERY batch, not just on a header miss, because ensure_header()'s
 * cache can hit and the caller still has to know where the vertices go. */
static int pt_route_active(void) {
#ifdef DC_PVR_NO_PUNCHTHRU
    return 0;
#else
    if (!alpha_test_active())
        return 0;
    if (!g_gx.color_update_enable)
        return 0;
#ifndef DC_PVR_PT_ALL
    if (g_gx.blend_mode == GX_BM_BLEND)
        return 0;
#endif
    return 1;
#endif
}

#ifdef DC_PVR_ALPHAENV
/* DOES THIS BATCH'S COMBINER SAY "alpha = TEXEL0.a, alone"?
 *
 * ON by default since 2026-08-03, but only on the SECOND A/B — the first one
 * said regress. Read the two A/Bs at the bottom before touching this; the
 * lesson in them is bigger than the switch.
 *
 * The renderer programmed PVR_TXRENV_MODULATEALPHA on every textured batch
 * since M1 — px = ARGB(col) * ARGB(tex), i.e. final alpha = vertex alpha x
 * texel alpha. **The game almost never asks for that product.**
 *
 * Pushing every gsDPSetCombineLERP / gsDPSetCombineMode site in src/ through
 * emu64's real tables (the 8x2 tbla at emu64.c:322-325, the arg reorder at
 * emu64.c:1091-1105 and its combine_auto twin at :1245-1257, plus the 33
 * combine_manual cases) and matching at GX stage 0 gives 5,611 display-list
 * sites, of which **4,376 — 78 % — are exactly (a,b,c,d) = (ZERO, ZERO, ZERO,
 * TEXA)**, which under GX's `d + (1-c)*a + c*b` is TEXEL0.a and nothing else.
 * The vertex alpha byte on those draws is not an opacity at all: they run
 * G_RM_FOG_SHADE_A (5,795 occurrences in src/), where it is the per-vertex FOG
 * COEFFICIENT for the N64 blender. This port fogs in PVR hardware off 1/w
 * (fog_program()), so that byte has no consumer here — but MODULATEALPHA was
 * multiplying it into the result anyway, dimming or erasing geometry that GX
 * would have drawn at the texel's own alpha.
 *
 * That is the same defect the punch-through path already patches by hand
 * (`cv[k].argb |= 0xFF000000u` in dc_gx_backend_submit) after it deleted the
 * train door and the window's tunnel mask outright. The ⚠️ note left at that
 * fix — "MODULATEALPHA is wrong for this game's alpha combiner everywhere, not
 * only on PT" — is this function.
 *
 * PVR_TXRENV_MODULATE is `px = A(tex) + RGB(col) * RGB(tex)` (KOS 2.3,
 * dc/pvr/pvr_header.h:125, read out of the SDK image rather than assumed):
 * alpha straight from the texel, RGB still modulated by the shaded vertex
 * colour. That is a term-for-term match for this shape, and it needs no
 * vertex-alpha surgery, so near-clipped vertices and lerp_vtx() come along for
 * free.
 *
 * DELIBERATELY NARROW. Only the one shape, and only when the FINAL alpha is
 * stage 0's:
 *   - stage 1, if present, must be the identity pass-through (ZERO, ZERO,
 *     ZERO, APREV) = `d` = the previous stage. 5,158 of the 5,611 sites (92 %)
 *     are exactly that. Anything else means a later stage rewrites alpha and
 *     this decision is not ours to make.
 *   - three or more stages: bail. dc_pvr.c reads no stage past 1.
 * Everything unrecognised keeps MODULATEALPHA, which is what shipped.
 *
 * WHAT MUST NOT BE COLLAPSED INTO THIS. Nine display-list sites really do put
 * SHADE in the alpha combiner — G_ACMUX_SHADE maps to GX_CA_RASA at
 * emu64.c:324, so the claim "no display list does" is false, just rare
 * (0.16 %). Five are alpha = SHADE.a alone (m_rcp.c:66,170; m_fbdemo.c:15, the
 * screen wipe; int_sum_classicwardrope01.c:83; m_submenu_ovl.c:320) and four
 * mix SHADE with the texel or a constant (obj_museum5.c:160, obj_suisou1.c:117,
 * room_lightR.c:35, int_tak_lion.c:139). None of them match `d == GX_CA_TEXA`
 * with the other three ZERO, so all nine keep MODULATEALPHA. The 104 sites
 * whose stage-0 alpha is all-ZERO likewise do not match and are left alone
 * deliberately — their real alpha comes from a stage this backend cannot see.
 *
 * ⚠️ The site counts above are STATIC. They say how much of the data has this
 * shape, not how many batches per frame do; s_env_texel_batches in the
 * [DC/PVR] report is the runtime answer: **797,728 of 1,190,110 batches, 67 %**,
 * over a 600 s town-reaching run.
 *
 * ============ THE TWO A/Bs, 2026-08-03, AND WHY THE FIRST WAS WRONG ========
 *
 * Method both times: two 600 s runs of ONE tree differing only by this define,
 * both reaching the town, 320x240 screenshots every 400 frames. DC_SCIF_FAST
 * is what made that affordable — at KOS's default baud a capture costs ~35 s.
 *
 * A/B #1 said REGRESS, and the default was set to OFF on the strength of it:
 *   win  — the dialogue balloon: a grey block behind the body text disappears
 *          and the "Rover" nameplate goes from desaturated olive to the correct
 *          saturated yellow-green.
 *   loss — the train station canopy, much bigger on screen: textured
 *          orange-brown beams with the clock legible behind became a flat
 *          teal-green slab.
 * Counters passed on both sides — frames, ptdrop, LOST, blank textures and FPS
 * all within noise. **The counters would have shipped that regression.**
 *
 * A/B #2, after the reply box's missing assets were fixed in
 * tools/dcstub/make_stub_data.py, said the canopy is CORRECT with this ON. The
 * teal slab was never this switch on its own — it was this switch *plus* the
 * two zeroed assets. con_waku_swaku3_tex was an all-zero 4,096 B buffer, and
 * tex_content_hash() (dc_pvr_texture.c:317-322) hashes only the first and last
 * 256 bytes above 512 B, so an all-zero texture aliases any other texture with
 * zero ends: the reply panel shared a VRAM image with something it should not
 * have, and honouring texel alpha then painted it. (That aliasing is filed in
 * kb/issues.md in its own right.)
 *
 * With the assets present, MODULATE gives the correct balloon AND the correct
 * canopy, so it is on. The remaining risk list — the nine SHADE-in-alpha
 * display lists, the 104 all-ZERO stage-0 sites — is unchanged and none of
 * them match this shape.
 *
 * THE LESSON, which outlives the switch: a renderer A/B run against a build
 * that is missing assets does not measure the renderer. Check
 * `grep 'ASSET MISSING' <run>/console.log` is empty before believing any
 * visual comparison.
 *
 * Kill switch: -DDC_PVR_NO_ALPHAENV. */
static int alpha_env_texel_only(void) {
    const DCGXTevStage* ts;

    if (g_gx.num_tev_stages < 1)
        return 0;
    ts = &g_gx.tev_stages[0];
    if (ts->alpha_a != GX_CA_ZERO || ts->alpha_b != GX_CA_ZERO ||
        ts->alpha_c != GX_CA_ZERO || ts->alpha_d != GX_CA_TEXA)
        return 0;

    if (g_gx.num_tev_stages > 2)
        return 0;
    if (g_gx.num_tev_stages == 2) {
        const DCGXTevStage* t1 = &g_gx.tev_stages[1];
        if (t1->alpha_a != GX_CA_ZERO || t1->alpha_b != GX_CA_ZERO ||
            t1->alpha_c != GX_CA_ZERO || t1->alpha_d != GX_CA_APREV)
            return 0;
    }
    return 1;
}
#endif /* DC_PVR_ALPHAENV */

static void compile_header(const dc_pvr_tex_t* tex) {
    pvr_poly_cxt_t cxt;
#ifdef DC_PVR_NO_PUNCHTHRU
    int list = DC_PVR_BASE_LIST;
#else
    int list = s_pt_route ? PVR_LIST_PT_POLY : DC_PVR_BASE_LIST;
#endif
#ifndef DC_PVR_NO_ALPHATEST
    int cutout = alpha_test_active();
#endif

    if (tex && tex->base) {
        int filt = PVR_FILTER_BILINEAR;
#ifdef DC_PVR_PT_NEAREST
        /* The one-line test of kb/station-bugs.md §2 H2 — cutout-edge
         * ghosting. The palettes involved have NO mid-alpha entry (that was
         * read straight out of foresta.rel), but bilinear MANUFACTURES one at
         * every boundary between a transparent and an opaque texel, and the
         * punch-through comparator then straddles it at PT_ALPHA_REF. The
         * station roof's scalloped trim is all boundary.
         *
         * `list` is already folded into header_key() through s_pt_route, so
         * this needs no cache work. It is opt-in because it trades a frayed
         * edge for an aliased one on every leaf and fence in town — a
         * screenshot decision, not a counters decision.
         *
         * ⚠️ This is a TEST, not a fix for a confirmed cause: the roof
         * clip-through has never been reproduced in a captured frame. That
         * is what DC_AUTOWALK is for. */
        if (list == PVR_LIST_PT_POLY) filt = PVR_FILTER_NONE;
#endif
        pvr_poly_cxt_txr(&cxt, list, (int)tex->pvr_fmt, tex->w, tex->h,
                         (pvr_ptr_t)tex->base, filt);
    } else {
        pvr_poly_cxt_col(&cxt, list);
    }

    cxt.gen.culling = cull_gx_to_pvr(g_gx.cull_mode);
#ifndef DC_PVR_NO_FOG
    /* pvr_poly_cxt_col/txr already default this to PVR_FOG_DISABLE, so
     * -DDC_PVR_NO_FOG is byte-identical to the pre-fog build. Gated on
     * s_fog_hw: enabling table fog before the table registers have ever been
     * written would fog against whatever the PVR powered up holding.
     * s_hdr_valid is cleared at every frame boundary, so a 0->1 flip of
     * s_fog_hw can never be cached stale. */
    cxt.gen.fog_type = (s_fog_hw && fog_active()) ? PVR_FOG_TABLE
                                                  : PVR_FOG_DISABLE;
#endif
    cxt.depth.comparison = g_gx.z_compare_enable
                               ? depth_gx_to_pvr(g_gx.z_compare_func)
                               : PVR_DEPTHCMP_ALWAYS;
    cxt.depth.write = g_gx.z_update_enable ? true : false;

    if (g_gx.blend_mode == GX_BM_BLEND) {
        cxt.blend.src = blend_gx_to_pvr(g_gx.blend_src);
        cxt.blend.dst = blend_gx_to_pvr(g_gx.blend_dst);
    } else if (g_gx.blend_mode == GX_BM_SUBTRACT) {
        /* The PVR has no subtractive blend. Additive is the closest thing that
         * still shows the geometry; recorded here so it is a known wrong
         * colour rather than a mystery. */
        cxt.blend.src = PVR_BLEND_ONE;
        cxt.blend.dst = PVR_BLEND_ONE;
    } else { /* GX_BM_NONE, GX_BM_LOGIC */
        cxt.blend.src = PVR_BLEND_ONE;
        cxt.blend.dst = PVR_BLEND_ZERO;
    }

#ifndef DC_PVR_NO_ALPHATEST
    /* THE CUTOUT FIX.
     *
     * GX's alpha test discards a texel outright. The PVR has no alpha test
     * outside the punch-through list, and this backend deliberately runs a
     * single translucent list (see the header of this file), so 23 of the 101
     * TEV configs were asking for a test that silently did not happen.
     *
     * The damage was not subtle. emu64 draws cutouts — foliage, fences, grass,
     * hair, the town-gate lattice — with GX_BM_NONE, which lands in the branch
     * above as src=ONE dst=ZERO. So every FULLY TRANSPARENT texel of a cutout
     * texture was written at full opacity AND, with z_update on, wrote depth.
     * The result is a solid rectangle of garbage that also occludes everything
     * behind it: reported by a human as "textures are not layered properly" and
     * as missing textures, which are the same bug seen twice.
     *
     * Approximation, deliberately: alpha-blend the cutout so transparent texels
     * contribute nothing, and stop it writing depth so it cannot occlude. It is
     * still depth-TESTED, so opaque geometry in front still hides it correctly;
     * what is lost is cutout-vs-cutout ordering, which now follows submission
     * order. That is a visible-but-plausible error in place of an opaque block.
     * The exact fix is a real PVR punch-through list — see kb/tev-map-alpha.md.
     * It now exists; read on.
     *
     * ⚠️ SUPERSEDED FOR MOST CUTOUTS, 2026-08-02 (second fix). This whole block
     * is skipped when the batch is going to the punch-through list, where the
     * hardware does the real thing: the alpha test discards below-threshold
     * texels, depth.write stays exactly as the game asked, and the blend
     * factors stay src=ONE dst=ZERO. Applying the approximation there would
     * undo the fix — a PT poly that both alpha-blends and drops depth write is
     * the "trees draw through the door" build again, just with a hardware alpha
     * test bolted on. The block survives for the batches pt_route_active()
     * declines (GX_BM_BLEND cutouts) and for -DDC_PVR_NO_PUNCHTHRU builds.
     *
     * Kill switch: -DDC_PVR_NO_ALPHATEST restores the pre-2026-08-02 behaviour. */
    if (cutout && list != PVR_LIST_PT_POLY) {
        cxt.blend.src = PVR_BLEND_SRCALPHA;
        cxt.blend.dst = PVR_BLEND_INVSRCALPHA;
        /* ⚠️ Only surrender depth write when the GAME asked for blending.
         *
         * MEASURED 2026-08-02: dropping it unconditionally broke the train
         * door. `AA_ZB_TEX_EDGE2` is the game's ordinary OPAQUE-WITH-HOLES
         * mode — the door frame and leaf use it (obj_romtrain_door.c:44,71),
         * as does the tunnel (rom_train_out.c:135) — it is not a foliage-only
         * mode. Such a batch must still occlude: with one submission-ordered
         * list and autosort off, geometry that writes no depth is painted over
         * by everything submitted after it, and all the XLU window scenery is
         * submitted after all the OPA geometry. The passing trees and clouds
         * drew straight through the closed door.
         *
         * GX_BM_NONE + alpha test = opaque with punched holes -> keep depth.
         * GX_BM_BLEND + alpha test = a real translucent cutout -> drop it.
         *
         * The transparent-texel bug this whole block fixes is cured by the
         * blend factors above on their own; the depth-write change was an
         * over-correction. */
        if (g_gx.blend_mode == GX_BM_BLEND)
            cxt.depth.write = false;
    }
    /* ON THE REFERENCE VALUE. g_gx.alpha_ref0/ref1 is still not read as a
     * per-batch threshold, because the PVR does not have one: PT_ALPHA_REF is
     * a single global register latched for the whole render
     * (kb/tev-map-alpha.md §5.2). It is pinned to DC_PVR_PT_ALPHA_REF = 144,
     * emu64's tex_edge_alpha default (emu64.c:718), which §5.5 option (a)
     * expects to cover the overwhelming majority of draws. A draw that asked
     * for a different reference gets a slightly wrong cutoff — a texel or two
     * of edge — instead of no test at all, which is what it got before.
     *
     * For the batches that do NOT reach PT (blended cutouts, and every batch in
     * a -DDC_PVR_NO_PUNCHTHRU build) the old caveat still stands verbatim:
     * texels between 1 and the reference were discarded on GC and are drawn
     * semi-transparently here, so those cutout edges keep their faint halo. */
#endif

#ifndef DC_PVR_NO_COLORMASK
    /* A depth-only pass must not paint. GXSetColorUpdate(GX_FALSE) is how emu64
     * writes the decal mask (emu64.c:2347-2349, the G_DECAL_GEQUAL|SPECIAL
     * path): fill the depth buffer, touch no pixels. dc_gx.c has stored
     * color_update_enable since M1 and set DIRTY(DC_GX_DIRTY_COLOR_MASK) for it,
     * and nothing ever read it — so those passes drew SOLID geometry, usually
     * with GX_BM_NONE i.e. src=ONE dst=ZERO. Ground shadows, footprints and
     * puddle decals came out as opaque blobs over the scene.
     *
     * The PVR has no colour write mask, but it does not need one: src=ZERO
     * dst=ONE leaves the destination exactly as it was, while cxt.depth.write
     * still governs depth. That is precisely GX's colour-update-off semantics.
     *
     * Safe by construction: dc_gx.c:475 initialises color_update_enable to 1, so
     * this branch is only ever taken because the game explicitly asked for it.
     * Kill switch: -DDC_PVR_NO_COLORMASK. */
    if (!g_gx.color_update_enable) {
        cxt.blend.src = PVR_BLEND_ZERO;
        cxt.blend.dst = PVR_BLEND_ONE;
    }
#endif

    if (tex && tex->base) {
        int uv_clamp, uv_flip;
        /* MODULATEALPHA is px = ARGB(col) * ARGB(tex): the GX "modulate"
         * TEV config, which kb/tev-map.md shows dominating the 101 configs. */
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
#ifdef DC_PVR_ALPHAENV
        /* ...but its ALPHA half is wrong for 78 % of this game's draws. See
         * alpha_env_texel_only() above; s_alpha_env_texel is that predicate,
         * evaluated once per batch so header_key() agrees with this. */
        if (s_alpha_env_texel)
            cxt.txr.env = PVR_TXRENV_MODULATE;
#endif
#ifdef DC_PVR_TEVP3
        /* PVR_TA_CMD_SPECULAR, BIT(2) of the PCW (pvr.h:605, pvr_prim.c:45).
         * With it set the hardware ADDS pvr_vertex_t.oargb to the texture-env
         * result (pvr_header.h:121-122) — that add is what carries P3's ENV
         * term. It is independent of txr.env: MODULATEALPHA and MODULATE both
         * get the add, they differ only in what they do to alpha.
         *
         * Gated on tex && tex->base by the enclosing block, deliberately: KOS
         * documents offset colour for TEXTURED polygons and says nothing about
         * an untextured one. P3 always has a texture, so this costs nothing. */
        cxt.gen.specular = s_p3_specular ? true : false;
#ifdef DC_PVR_P3_MODULATE
        /* #037's alpha is TEXEL0 alone, and MODULATEALPHA multiplies it by the
         * vertex alpha — the same defect DC_PVR_ALPHAENV fixes globally and
         * which measured a REGRESSION when applied globally (BUILDING-DC.md).
         * This narrows the identical change to P3 batches: 27 configs instead
         * of every draw in the game. Requires the alpha predicate to agree, so
         * a P3 batch whose alpha is genuinely a product is left alone. */
        if (s_p3_specular && alpha_env_texel_only())
            cxt.txr.env = PVR_TXRENV_MODULATE;
#endif
#ifdef DC_PVR_P3_CLRCLAMP
        /* One-line probe of an UNDOCUMENTED bit: pvr.h:178 calls it
         * gen.color_clamp and pvr_header.h:285 calls the same bit fog_clamp,
         * and KOS documents the semantics of neither. Not needed by the
         * predicate in tev_p3_affine() — with base = PRIM-ENV >= 0 and
         * offset = ENV the sum is PRIM <= 1 for any texel, so it cannot
         * overflow. Here to be measured, not to be depended on. */
        cxt.gen.color_clamp = true;
#endif
#endif
        wrap_gx_to_pvr(g_gx.tex_obj_wrap_s[0], g_gx.tex_obj_wrap_t[0],
                       &uv_clamp, &uv_flip);
        cxt.txr.uv_clamp = (pvr_uv_clamp_t)uv_clamp;
        cxt.txr.uv_flip  = (pvr_uv_flip_t)uv_flip;
        cxt.gen.alpha = true;
    }

    pvr_poly_compile(&s_hdr, &cxt);
    s_hdr_valid = 1;
}

static void ensure_header(const dc_pvr_tex_t* tex) {
    unsigned int key = header_key(tex);
    if (s_hdr_valid && key == s_hdr_key)
        return;
    compile_header(tex);
    s_hdr_key = key;
    submit_prim(&s_hdr, sizeof(s_hdr));
}

/* ==========================================================================
 * Transform / clip / emit
 * ========================================================================== */

typedef struct {
    float x, y, z, w;   /* clip space */
    float u, v;
    unsigned int argb;
#ifdef DC_PVR_TEVP3
    /* The PVR offset colour, carried through the clipper and the vertex memo
     * exactly like argb. +4 B here is +512 B of .bss (s_vmemo_val[128]) and
     * +32 B of stack; TA bandwidth is unchanged, because pvr_vertex_t already
     * has the field and submit_prim() already sends all 32 bytes. */
    unsigned int oargb;
#endif
} ClipVtx;

/* ==========================================================================
 * G5 / [VTXSPLIT] — where the 610 cycles per vertex actually go
 * ==========================================================================
 * `-DDC_PVR_VTXSPLIT=<N>`, off by default, N = sample one primitive in N.
 *
 * WHY THIS EXISTS. `[PHASE] us/v` is the number this project optimises
 * against, and it sits at **3.05 us per submitted vertex — 610 SH-4 cycles at
 * 200 MHz.** The arithmetic in the loop it measures is one FTRV, six FIPRs and
 * a reciprocal square root: call it 60 cycles. **So ~90 % of `xform` is not
 * the vertex math, and nothing has ever said what it is.**
 *
 * That matters immediately, because `kb/research-sh4zam-gap.md` §0a proposes
 * replacing those six FIPRs with two FTRVs. Against 610 cycles that is a ~5 %
 * lever wearing the clothes of a headline. This splits the 610 first, for the
 * same reason G4 split `G_TRIN_INDEPEND` before anyone costed G3:
 * **measurement rule 7 — an average is not the cost of any part of it.**
 *
 * THE BUCKETS, in loop order:
 *   memo  the per-batch vertex memo: hash + the 12-field compare. Paid on
 *         EVERY vertex, hit or miss — it is the price of asking
 *   xf    the position transform (one FTRV, or 16 mul/12 add at -DDC_PVR_NO_FTRV)
 *   lit   eye + normal: the six FIPRs §0a wants to make two FTRVs, plus the
 *         normalize. Zero on an unlit batch, which is why `vlit` is next to it
 *   tex   apply_texgen + the u/v scale
 *   shade shade_vertex(): the per-light loop
 *   post  punch-through alpha, the TEV constant/fold overrides, memo store
 *   emit  emit_triangle x1 or x2: near clip, perspective divide, and
 *         pvr_prim's 32-byte copy into the store queue — G-C's target
 *
 * HOW IT SAMPLES, and why it must. Bracketing every stage of every vertex
 * would be ~19,000 timer reads a frame; at TMU2's ~80 ns that is 1.5 ms of
 * probe inside an 8.4 ms measurement. So it samples **one primitive in N** and
 * scales by the sample count. `prims=`/`samp=` are printed so the scaling can
 * be checked rather than trusted.
 *
 * ⚠️ TMU2 TICKS ARE 80 ns AND A STAGE IS ~100-600 ns. A single sample is
 * therefore worth ±1 tick and means nothing; only the window mean does. That
 * is a floor on what this can resolve, and a bucket reported as 0.00 means
 * "below the noise", never "free". The same clock and the same 80 ns tick as
 * [GXSPLIT] (dc_gx.c:230), on purpose — the two are comparable.
 *
 * ⚠️ It reads TCNT2 directly rather than calling a clock function: one
 * volatile load, no call, no us->ns conversion. TMU2 counts DOWN.
 */
#if defined(DC_PVR_VTXSPLIT) && (DC_PVR_VTXSPLIT) > 0
#define VS_TCNT2      (*(volatile unsigned int *)0xffd80024)
#define VS_TICK_NS    80u
#define VS_WRAP_GUARD 0x00400000u

enum { VS_MEMO = 0, VS_XF, VS_LIT, VS_TEX, VS_SHADE, VS_POST, VS_EMIT,
       VS_NBUCKET };
static u64 s_vs_acc[VS_NBUCKET];
static unsigned int s_vs_prims, s_vs_samples, s_vs_hits, s_vs_drops;
static const char* const s_vs_name[VS_NBUCKET] = {
    "memo", "xf", "lit", "tex", "shade", "post", "emit"
};

/* One sampled primitive's running mark. `on` is evaluated once per primitive
 * so the whole bracket set compiles to a single predictable branch. */
typedef struct { unsigned int t; int on; } VsMark;

static inline void vs_charge(VsMark* m, int bucket) {
    unsigned int now = VS_TCNT2;
    unsigned int d = m->t - now;      /* DOWN counter */
    m->t = now;
    if (d >= VS_WRAP_GUARD) { s_vs_drops++; return; }
    s_vs_acc[bucket] += d;
}

#define VS_DECL          VsMark vsm
#define VS_SAMPLE_BEGIN  do {                                                 \
        s_vs_prims++;                                                         \
        vsm.on = ((s_vs_prims % (unsigned int)(DC_PVR_VTXSPLIT)) == 0u);      \
        if (vsm.on) { s_vs_samples++; vsm.t = VS_TCNT2; }                     \
    } while (0)
#define VS_MARK(b)       do { if (vsm.on) vs_charge(&vsm, (b)); } while (0)
#define VS_HIT           do { if (vsm.on) s_vs_hits++; } while (0)
#define VS_SAMPLE_END    ((void)0)
#else
#define VS_DECL          ((void)0)
#define VS_SAMPLE_BEGIN  ((void)0)
#define VS_MARK(b)       ((void)0)
#define VS_HIT           ((void)0)
#define VS_SAMPLE_END    ((void)0)
#endif

/* ==========================================================================
 * G-C — Direct Rendering: build the vertex IN the store queue
 * ==========================================================================
 * MEASURED, 2026-08-08 (G5 / [VTXSPLIT]): `emit` is 2.15 ms of an 8.9 ms
 * `xform`, the largest of the seven stages, and the frame is memory-bound —
 * emit+shade+memo are 75 % of it while ALL the floating-point is 0.81 ms
 * (kb/research-sh4zam-gap.md §3).
 *
 * WHAT WAS COSTING. Every TA word went stack -> SQ:
 *   1. eight field stores into a stack `pvr_vertex_t pv`
 *   2. submit_prim() -> pvr_prim() -> sq_fast_cpy(), which READS those 32 bytes
 *      back as four 64-bit `fmov`s and writes them to the SQ, then `pref`s
 * i.e. a build pass, a read-back and a second store pass per corner, plus two
 * calls and an `fschg` pair.
 *
 * WHAT THIS DOES. `pvr_dr_target()` hands back the store-queue address itself,
 * so the same eight stores land directly in the SQ and `pvr_dr_commit()` is the
 * `pref`. The read-back, the second store pass and both calls disappear. The
 * arithmetic is untouched — same divide, same viewport map, same bytes.
 *
 * WHY THIS IS SAFE TO MIX WITH pvr_prim(), verified in the KOS 2.3 source in
 * the SDK image rather than assumed:
 *   - `pvr_dr_addr` is a plain global, statically initialised to MEM_AREA_SQ_BASE
 *     (kos/.../pvr/pvr_globals.c:21). `pvr_dr_init()` is a deprecated no-op in
 *     KOS 2.3 (pvr_legacy.h:282) — there is nothing to initialise and nothing
 *     this file has to call first.
 *   - `pvr_dr_target()` is `pvr_dr_addr ^= 32` and `pvr_dr_commit(a)` is
 *     `sq_flush(a)`, i.e. one `pref` (pvr.h:1001,1008).
 *   - ⭐ `sq_flush()` CLOBBERS "memory" — `__asm__ __volatile__("pref @%0" :
 *     : "r"(src) : "memory")` (arch/sq.h:112-118). This is the load-bearing
 *     one and it is the reason the eight plain stores below are ordered before
 *     the `pref` at all: an `asm volatile` without that clobber orders only
 *     against other volatile asm, and GCC would have been free to sink the
 *     stores past it and hand the TA the PREVIOUS primitive's 32 bytes. The
 *     old path never had the exposure because `sq_fast_cpy` did its stores
 *     inside the asm. If a future KOS ever drops the clobber, put a
 *     `__asm__ __volatile__("" ::: "memory")` in front of the commit — zero
 *     instructions, and it makes the guarantee local instead of borrowed.
 *   - `pvr_list_begin()` only takes the `sq_lock` arm when NOT in DMA mode.
 *     This build sets `p.dma_enabled = 0` (search `dma_enabled` below)
 *     — that is the precondition, not an accident.
 *   - The store queues need QACR0/QACR1, which stop working the moment the MMU
 *     is on (kb/research-mmu-paging.md). MMU paging is DEAD
 *     (kb/research-mmu-paging.md), so this is a dependency on a decision that
 *     is already made, but it is a dependency and it is now written down.
 *   - `pvr_list_begin()` has ALREADY done `sq_lock((void *)PVR_TA_INPUT)` for
 *     the open list (pvr_scene.c:198) and `pvr_list_finish()` does the
 *     `sq_unlock` (:230). QACR is therefore set up for the TA across exactly
 *     the window in which this function can run, and DR and `pvr_prim()` share
 *     it. `pvr_prim()` always targets SQ0; DR alternates SQ0/SQ1.
 *
 * ⚠️ THE PUNCH-THROUGH ROUTE KEEPS THE OLD PATH, AND THE REASON IS TEMPORAL,
 * NOT RESOURCE. A PT batch is not submitted here at all — it is memcpy'd into
 * s_pt_buf and replayed after the general list closes (see :11-47 and
 * dc_gx_backend_frame_end). The store queue exists and is locked at both
 * moments; what does not exist is permission to write list 4 yet. This
 * function runs while the GENERAL list is open, and a PT record must be HELD
 * until list 4 can legally be opened, so it has to go to RAM rather than to a
 * queue currently pointed at a different list. The replay loop itself could
 * legitimately use DR — 96-ish records a frame, so it is not worth it — but
 * the PRODUCER here could not.
 *
 * ⚠️ THE SQ IS WRITE-ONLY. Nothing may read a field back after storing it,
 * which is why the values are kept in locals and DC_PVR_BATCH_LOG reads those
 * rather than the emitted record.
 *
 * ⚠️ All 32 bytes are written every time. The SQ holds the PREVIOUS
 * primitive's words until overwritten, so a skipped field would emit stale
 * geometry, not a zero.
 *
 * Kill switch: -DDC_PVR_NO_DR. `dr` then folds to a compile-time 0 and the
 * whole DR arm is dead code the compiler removes, so the killed build is
 * byte-identical to the pre-G-C one AT ANY OPTIMISATION LEVEL THE `perf` AND
 * `size` PROFILES USE. Under DC_OPT_PROFILE=o0 it is not: `int dr = 0` and the
 * empty then-block survive as a live `mov #0 / tst / bt`. That profile exists
 * to bisect miscompiles, not to size .text, so the difference is stated rather
 * than engineered away. Counter: `dr=` in dc_pvr_report(). */
#ifndef DC_PVR_NO_DR
static unsigned int s_dr_verts;     /* vertices written straight into the SQ */
#endif

static void emit_projected(const ClipVtx* c, unsigned int flags) {
    float inv_w = 1.0f / c->w;
    float px = s_vp_cx + s_vp_hw * (c->x * inv_w);
    float py = s_vp_cy - s_vp_hh * (c->y * inv_w);
#ifdef DC_PVR_TEVP3
    unsigned int oargb = c->oargb;
#else
    unsigned int oargb = 0;
#endif
    int dr = 0;

#ifndef DC_PVR_NO_DR
    dr = 1;
#ifndef DC_PVR_NO_PUNCHTHRU
    if (s_pt_route) dr = 0;
#endif
#endif

    if (dr) {
#ifndef DC_PVR_NO_DR
        pvr_vertex_t* dv = (pvr_vertex_t*)pvr_dr_target();
        dv->flags = flags;
        dv->x = px;
        dv->y = py;
        dv->z = inv_w;
        dv->u = c->u;
        dv->v = c->v;
        dv->argb = c->argb;
        /* S14 — THE EIGHTH STORE THE HARDWARE THROWS AWAY.
         *
         * `oargb` is the PVR's OFFSET (specular) colour, and the hardware reads
         * it ONLY when PVR_TA_CMD_SPECULAR is set in the poly header's PCW.
         * That bit is written in exactly one place in this file —
         * `cxt.gen.specular = s_p3_specular` — which lives inside
         * `#ifdef DC_PVR_TEVP3`. So in the DEFAULT build the bit is never set,
         * every `oargb` this function has ever written was discarded by the
         * TA, and the store was one eighth of the per-vertex store-queue
         * traffic spent on nothing. In a TEVP3 build the bit is per batch, and
         * `s_p3_specular` is decided in dc_gx_backend_submit() BEFORE
         * ensure_header() (same contract as s_pt_route), so it is constant for
         * every vertex that reaches here and the test cannot disagree with the
         * header the TA is parsing against.
         *
         * ⚠️ THIS DOES NOT VIOLATE "ALL 32 BYTES ARE WRITTEN EVERY TIME". That
         * rule exists because the store queue RETAINS the previous primitive's
         * words, so a skipped field emits stale geometry rather than a zero —
         * and it is still true of the seven fields above. It does not bind
         * here: with the specular bit clear the TA does not read this word at
         * all, so what it retains is unobservable. Turn TEVP3 on and the store
         * comes back for exactly the batches whose header asks for it.
         *
         * Kill switch: -DDC_PVR_NO_OARGB_SKIP restores the unconditional
         * store. Under it the `oargb` local below is used on both arms exactly
         * as before, so the killed build is byte-identical. */
#ifdef DC_PVR_NO_OARGB_SKIP
        dv->oargb = oargb;
#elif defined(DC_PVR_TEVP3)
        if (s_p3_specular) dv->oargb = oargb;
#endif
        pvr_dr_commit(dv);
        s_dr_verts++;
#endif
    } else {
        /* The attribute RESTATES the type's own alignment; it does not add
         * one. `pvr_vertex_t` carries `alignas(32)` on its first member
         * (pvr.h:421), so this variable was always 32-aligned and the old code
         * was not getting away with anything — which also means the attribute
         * cannot move the stack frame and cannot threaten the kill switch's
         * byte-identity. It is here because the alignment is load-bearing and
         * invisible: sq_fast_cpy() reads the source as four 64-bit `fmov`s,
         * which fault on a 4-aligned address. */
        pvr_vertex_t pv __attribute__((aligned(32)));
        pv.flags = flags;
        pv.x = px;
        pv.y = py;
        pv.z = inv_w;
        pv.u = c->u;
        pv.v = c->v;
        pv.argb = c->argb;
        pv.oargb = oargb;
        submit_prim(&pv, sizeof(pv));
    }
#ifndef DC_PVR_NO_PUNCHTHRU
    if (s_pt_route) s_pt_verts++;
#endif

#ifdef DC_PVR_BATCH_LOG
    /* Accumulate what the TA was actually handed, not what we think it was.
     * "Submitted" and "on screen" are different claims and the draw-call
     * counter cannot tell them apart. Reads the LOCALS, never the emitted
     * record: under DR that record lives in the write-only store queue. */
    if (s_batch_log_now) {
        if (!s_bl_n) {
            s_bl_x0 = s_bl_x1 = px;
            s_bl_y0 = s_bl_y1 = py;
            s_bl_z0 = s_bl_z1 = inv_w;
            s_bl_u0 = s_bl_u1 = c->u;
            s_bl_v0 = s_bl_v1 = c->v;
        } else {
            if (px < s_bl_x0) s_bl_x0 = px;
            if (px > s_bl_x1) s_bl_x1 = px;
            if (py < s_bl_y0) s_bl_y0 = py;
            if (py > s_bl_y1) s_bl_y1 = py;
            if (inv_w < s_bl_z0) s_bl_z0 = inv_w;
            if (inv_w > s_bl_z1) s_bl_z1 = inv_w;
            if (c->u < s_bl_u0) s_bl_u0 = c->u;
            if (c->u > s_bl_u1) s_bl_u1 = c->u;
            if (c->v < s_bl_v0) s_bl_v0 = c->v;
            if (c->v > s_bl_v1) s_bl_v1 = c->v;
        }
        s_bl_n++;
        s_bl_argb = c->argb;
    }
#endif
}

/* ==========================================================================
 * The per-batch vertex memo cache
 * ==========================================================================
 * emu64 does NOT hand this backend a mesh. It hands it an expanded triangle
 * SOUP, and the expansion is lossy in exactly the way that matters here.
 *
 * The shape, read out of emu64.c:5100-5150 (the G_TRI1/G_TRI2 run collapser,
 * which the comment there says dominates this game's display lists): a
 * gsSPVertex has already loaded up to 32 vertices into `this->vertices[]`, and
 * each triangle command carries three INDICES into that cache. emu64 counts the
 * whole run of triangle commands, opens ONE GXBegin(GX_TRIANGLES, n_verts) —
 * n_verts = 3 per triangle, 6 per TRI2 — and then calls set_position3(v0,v1,v2)
 * per triangle, which re-emits GXColor / GXNormal / GXTexCoord / GXPosition for
 * each index. A vertex shared by six triangles is therefore pushed through this
 * file's transform, normal matrix, eight-light loop and texgen SIX TIMES, and
 * produces the same 28 bytes every time.
 *
 * It produces the same bytes because every other input is a per-BATCH constant:
 * the folded matrix, the modelview, the normal matrix, the light state, the TEV
 * constants, the PT route, the texture scale. Nothing inside the k-loop reads
 * anything that varies per vertex except `*v` itself. So memoising on the
 * source vertex bytes is EXACT, not an approximation — it cannot change a
 * single emitted byte, only how many times they are computed.
 *
 * Direct-mapped, 32 entries, because emu64's own vertex cache is 32 entries
 * (Vtx vertices[32]) and a batch can therefore never reference more than 32
 * distinct source vertices. The stored key is the source vertex's INDEX in
 * verts[], not a copy of it: verts[] is g_gx.vertex_buffer and is stable for
 * the whole call, so a hit is confirmed by comparing against the original
 * rather than against a copy that could disagree about padding bytes.
 *
 * ⚠️ The comparison must not read the struct's tail padding. DCGXVertex is 30
 * live bytes in a 32-byte aligned(8) shell (dc_gx_internal.h:116-128), and
 * those two bytes are never written by anything — struct assignment may or may
 * not carry them, so a plain 32-byte memcmp would compare uninitialised memory
 * and turn a legitimate hit into a miss (or, worse, be unstable run to run).
 * Fields are compared explicitly below.
 *
 * Kill switch: -DDC_PVR_NO_VTXMEMO. Counters: `vmemo=hit/total` in [PHASE]. */
#ifndef DC_PVR_NO_VTXMEMO
/* SIZING, and why it is 128 rather than emu64's own 32.
 *
 * The first version used 32 slots on the reasoning that emu64's vertex cache is
 * `Vtx vertices[32]`, so a batch can never reference more than 32 distinct
 * sources. That bounds the WORKING SET correctly and says nothing about
 * COLLISIONS: 32 keys into 32 direct-mapped slots collide constantly, and it
 * measured a 42.5 % hit rate against a mesh whose theoretical ceiling — six
 * triangles per shared vertex — is around 83 %.
 *
 * 128 slots is a 4x load factor. It costs 128 * (4 + 4 + 28) = 4,608 B of .bss
 * against 32's 1,152.
 *
 * The per-batch invalidation is a GENERATION STAMP, not a memset, precisely
 * because the table got bigger: clearing 128 entries per batch, ~96 batches a
 * frame, would have spent more than the extra hits are worth. `s_vmemo_gen`
 * wraps at 2^32, and a wrap can only ever produce a FALSE HIT if a stale entry
 * survives 4 billion batches AND lands on the same slot AND its recorded source
 * index still points at a byte-identical vertex — the vmemo_same() compare
 * below is what makes that safe rather than merely unlikely. */
#define VMEMO_SLOTS 128
static int          s_vmemo_src[VMEMO_SLOTS];   /* index into verts[] */
static unsigned int s_vmemo_tag[VMEMO_SLOTS];   /* the batch it belongs to */
static unsigned int s_vmemo_gen = 0;

/* ==========================================================================
 * S14 — THE MEMO VALUE'S STRIDE IS A CACHE-LINE BUG, AND IT IS THE SAME BUG
 * DCGXVertex's aligned(32) FIXED
 * ==========================================================================
 * ClipVtx is 28 bytes (x,y,z,w,u,v + argb; 32 under -DDC_PVR_TEVP3) and its
 * members are floats, so `ClipVtx s_vmemo_val[128]` has alignment 4 and a
 * stride of 28. The SH-4's operand cache has 32-BYTE LINES. At a 28-byte
 * stride, entry k begins at byte 28k, and 28k mod 32 cycles 0,28,24,20,…: only
 * one entry in eight starts on a line boundary and SEVEN IN EIGHT STRADDLE TWO
 * LINES. The lookup index is a vertex id, i.e. effectively random within the
 * batch, so this is two line fills per memo HIT — on the path that exists to
 * be cheap, in a frame this project has measured as memory-bound.
 *
 * Rounding the stride to 32 makes every entry exactly one line, always. It
 * costs 128 * 4 = 512 B of .bss (3,584 -> 4,096) and changes no field offset
 * inside ClipVtx, so it cannot alter a single emitted byte — only how many
 * line fills reading one costs.
 *
 * ⚠️ THE SoA LAYOUT STAYS, DELIBERATELY. Folding tag/vid/val into one struct
 * looks like the bigger version of this fix and is NOT: s_vmemo_tag is 512 B
 * and s_vmemo_vid 256 B — 24 lines between them, in a 16 KB (512-line) operand
 * cache — so both are effectively always resident and cost nothing to touch.
 * Only the 3.5 KB value array is big enough to miss. An AoS at a 64-byte
 * stride would make the table 8 KB, quadruple the tag array's footprint, and
 * still straddle two lines per value. Measured reasoning, not taste.
 *
 * ⚠️ Under -DDC_PVR_TEVP3 ClipVtx is already 32 B and this is a no-op on
 * stride — it still fixes the ARRAY BASE alignment, which is 4 without it.
 *
 * Kill switch: -DDC_PVR_NO_MEMO_ALIGN restores the bare array verbatim. */
#ifndef DC_PVR_NO_MEMO_ALIGN
typedef struct { ClipVtx v; } __attribute__((aligned(32))) DCVMemoVal;
/* A hard error rather than a silent regression if ClipVtx ever outgrows a
 * line: at 33+ bytes this would round to 64 and quietly double the table. */
typedef char dc_vmemo_stride_check[(sizeof(DCVMemoVal) == 32) ? 1 : -1];
static DCVMemoVal   s_vmemo_valv[VMEMO_SLOTS];
#define VMEMO_VAL(s) (s_vmemo_valv[(s)].v)
#else
static ClipVtx      s_vmemo_val[VMEMO_SLOTS];
#define VMEMO_VAL(s) (s_vmemo_val[(s)])
#endif
#ifdef DC_GX_VTXID
/* G-B. The (epoch<<8 | emu64 index) this slot was published for, or
 * DC_GX_VTXID_NONE when the hash path published it. 256 B, and it is the
 * second half of the id lookup — see dc_gx.c's dc_gx_vtxid_arm() for why the
 * generation tag alone is not enough once GXBegin merges two TRINs. */
static unsigned short s_vmemo_vid[VMEMO_SLOTS];
#endif

static inline unsigned int vmemo_hash(const DCGXVertex* v) {
    /* Position dominates: two vertices of one model almost never share it,
     * while colour and normal collide constantly on flat-shaded geometry. */
    const unsigned int* w = (const unsigned int*)(const void*)v;
    unsigned int h = w[0] ^ (w[1] * 3u) ^ (w[2] * 5u) ^ w[5];
    h ^= h >> 15;
    return h & (VMEMO_SLOTS - 1);
}

/* THE COMPARE, AS 8 LOADS PER SIDE AND ONE BRANCH
 * ==========================================================================
 * The field-by-field form below (kept as the kill switch) is 12 compares and
 * 12 DEPENDENT CONDITIONAL BRANCHES — `&&` is a sequence point, so each one
 * is a T-bit test the SH-4 cannot fold or reorder. Estimated at 50-65 of the
 * memo's measured 122 cycles per vertex, which makes it the largest single
 * block in the stage.
 *
 * THE LIVE BYTES ARE 30, NOT 28. DCGXVertex is position[3] (0-11) +
 * texcoord[2] (12-19) + color0[4] (20-23) + normal[3] (24-29), in a 32-byte
 * shell whose last two bytes are never written by anything
 * (dc_gx_internal.h:116-128). So the exact compare is SEVEN uint32 words plus
 * ONE uint16 — a plain 32-byte memcmp would read the two dead bytes, and a
 * 7-word compare would silently drop `normal[2]` and mis-light any two corners
 * that differ only in it.
 *
 * OR-then-test rather than compare-then-branch: the eight XORs are
 * independent, so they pipeline, and there is exactly one branch at the end.
 *
 * ⚠️ TWO FLOAT-SEMANTIC DELTAS, both harmless, both stated rather than
 * discovered later:
 *   NaN vs the same NaN bits — `==` says different (miss), bitwise says same
 *     (hit). Identical bits go through FTRV/FIPR to identical outputs, so this
 *     is one FEWER recompute for the same emitted bytes.
 *   +0.0 vs -0.0 — `==` says same (hit), bitwise says different (miss). One
 *     lost hit; the vertex is recomputed and emits the same bytes.
 * Neither can change a pixel. Both change `vmemo=` by a rounding, which is
 * why the falsification below is the SPLIT and not the hit rate.
 *
 * Kill switch: -DDC_PVR_NO_VMEMO_WORDCMP restores the 12-field form verbatim.
 * Also disabled automatically under -DDC_GX_FAT_VERTEX, whose 40-byte layout
 * these offsets do not describe. */
#if !defined(DC_PVR_NO_VMEMO_WORDCMP) && !defined(DC_GX_FAT_VERTEX)
/* A hard error rather than a silent wrong answer if the struct ever moves. */
typedef char dc_vmemo_layout_check[(sizeof(DCGXVertex) == 32) ? 1 : -1];

static inline int vmemo_same(const DCGXVertex* a, const DCGXVertex* b) {
    const unsigned int* wa = (const unsigned int*)(const void*)a;
    const unsigned int* wb = (const unsigned int*)(const void*)b;
    /* bytes 28-29: normal[2]. Bytes 30-31 are the dead tail and are NOT read. */
    const unsigned short* ha = (const unsigned short*)(const void*)&a->normal[2];
    const unsigned short* hb = (const unsigned short*)(const void*)&b->normal[2];
    unsigned int d = (wa[0] ^ wb[0]) | (wa[1] ^ wb[1]) | (wa[2] ^ wb[2]) |
                     (wa[3] ^ wb[3]) | (wa[4] ^ wb[4]) | (wa[5] ^ wb[5]) |
                     (wa[6] ^ wb[6]) |
                     (unsigned int)((unsigned int)*ha ^ (unsigned int)*hb);
    return d == 0u;
}
#else
static inline int vmemo_same(const DCGXVertex* a, const DCGXVertex* b) {
    return a->position[0] == b->position[0] &&
           a->position[1] == b->position[1] &&
           a->position[2] == b->position[2] &&
           a->texcoord[0] == b->texcoord[0] &&
           a->texcoord[1] == b->texcoord[1] &&
           a->color0[0] == b->color0[0] && a->color0[1] == b->color0[1] &&
           a->color0[2] == b->color0[2] && a->color0[3] == b->color0[3] &&
           a->normal[0] == b->normal[0] && a->normal[1] == b->normal[1] &&
           a->normal[2] == b->normal[2];
}
#endif
#endif /* !DC_PVR_NO_VTXMEMO */

static void lerp_vtx(ClipVtx* out, const ClipVtx* a, const ClipVtx* b, float t) {
    out->x = a->x + (b->x - a->x) * t;
    out->y = a->y + (b->y - a->y) * t;
    out->z = a->z + (b->z - a->z) * t;
    out->w = a->w + (b->w - a->w) * t;
    out->u = a->u + (b->u - a->u) * t;
    out->v = a->v + (b->v - a->v) * t;
    {
        unsigned int ca = a->argb, cb = b->argb;
        unsigned int r = 0;
        int s;
        for (s = 0; s < 32; s += 8) {
            float x0 = (float)((ca >> s) & 0xFF);
            float x1 = (float)((cb >> s) & 0xFF);
            int   xi = (int)(x0 + (x1 - x0) * t + 0.5f);
            if (xi < 0) xi = 0;
            if (xi > 255) xi = 255;
            r |= ((unsigned int)xi) << s;
        }
        out->argb = r;
    }
#ifdef DC_PVR_TEVP3
    /* NOT optional. A near-clipped triangle emits lerped vertices, and leaving
     * oargb uninitialised here would push whatever was on the stack into the
     * TA as an additive colour — a bug that appears only on geometry crossing
     * the near plane, i.e. exactly the geometry the pvr_dropped counter is
     * already known to track by camera position. Interpolated rather than
     * copied because base and offset must stay a matched pair across the seam.
     * Three channels: the hardware ignores oargb's alpha (pvr_header.h:122). */
    {
        unsigned int ca = a->oargb, cb = b->oargb;
        unsigned int r = 0;
        int s;
        for (s = 0; s < 24; s += 8) {
            float x0 = (float)((ca >> s) & 0xFF);
            float x1 = (float)((cb >> s) & 0xFF);
            int   xi = (int)(x0 + (x1 - x0) * t + 0.5f);
            if (xi < 0) xi = 0;
            if (xi > 255) xi = 255;
            r |= ((unsigned int)xi) << s;
        }
        out->oargb = r;
    }
#endif
}

/* Emit one triangle, clipping it against w > DC_PVR_W_EPS first. The PVR has
 * no near plane; without this a vertex behind the eye divides by a negative w
 * and streaks across the whole framebuffer. Sutherland-Hodgman on three
 * vertices yields at most four, i.e. at most two triangles. */
static void emit_triangle_raw(const ClipVtx* a, const ClipVtx* b,
                              const ClipVtx* c) {
    const ClipVtx* in[3];
    ClipVtx out[4];
    int n_out = 0;
    int i, inside_count = 0;

    in[0] = a; in[1] = b; in[2] = c;
    for (i = 0; i < 3; i++)
        if (in[i]->w > DC_PVR_W_EPS) inside_count++;

    if (inside_count == 0) {
        s_tris_dropped++;
        return;
    }

    if (inside_count == 3) {
        emit_projected(a, PVR_CMD_VERTEX);
        emit_projected(b, PVR_CMD_VERTEX);
        emit_projected(c, PVR_CMD_VERTEX_EOL);
        s_tris_out++;
        return;
    }

#ifdef DC_PVR_NO_NEARCLIP
    s_tris_dropped++;
    return;
#else
    for (i = 0; i < 3; i++) {
        const ClipVtx* cur = in[i];
        const ClipVtx* nxt = in[(i + 1) % 3];
        int cur_in = cur->w > DC_PVR_W_EPS;
        int nxt_in = nxt->w > DC_PVR_W_EPS;

        if (cur_in)
            out[n_out++] = *cur;
        if (cur_in != nxt_in) {
            float t = (DC_PVR_W_EPS - cur->w) / (nxt->w - cur->w);
            lerp_vtx(&out[n_out++], cur, nxt, t);
        }
    }

    s_tris_clipped++;
    if (n_out < 3) {
        s_tris_dropped++;
        return;
    }
    /* Fan the clipped polygon. n_out is 3 or 4 by construction. */
    for (i = 2; i < n_out; i++) {
        emit_projected(&out[0], PVR_CMD_VERTEX);
        emit_projected(&out[i - 1], PVR_CMD_VERTEX);
        emit_projected(&out[i], PVR_CMD_VERTEX_EOL);
        s_tris_out++;
    }
#endif
}

/* All-or-nothing against the PT record buffer.
 *
 * The replayed stream is read by the TA as a command sequence, so a strip that
 * stops before its EOL vertex is not "a missing triangle", it is a malformed
 * list — the next poly header would be consumed as if it were the strip's
 * continuation. Marking the write position and rewinding it if any record of
 * this triangle did not fit makes overflow a clean, countable loss instead.
 *
 * Only vertices are written between the mark and here (ensure_header() runs
 * once per batch, before the triangle loop), so the s_pt_verts adjustment is
 * exact. s_tris_out is deliberately NOT rewound: it is a census of what the
 * transform stage produced, and ptdrop= is the census of what the buffer then
 * refused. Two different questions. */
static void emit_triangle(const ClipVtx* a, const ClipVtx* b, const ClipVtx* c) {
#ifndef DC_PVR_NO_PUNCHTHRU
    if (s_pt_route) {
        unsigned int mark = s_pt_n;
        s_pt_trunc = 0;
        emit_triangle_raw(a, b, c);
        if (s_pt_trunc) {
            s_pt_verts -= (s_pt_n - mark);
            s_pt_n = mark;
            s_pt_drop++;
        }
        return;
    }
#endif
    emit_triangle_raw(a, b, c);
}

/* Texgen. Slot ids are GX_TEXMTX0..9 stepping by 3 (GXEnum.h:294); GX_IDENTITY
 * and anything else means "use the texcoord as given". dc_gx.c stores the 3x4
 * matrix row-major, so a 2x4 texgen is rows 0 and 1. */
static int texmtx_slot(int id) {
    if (id < GX_TEXMTX0 || id >= GX_IDENTITY) return -1;
    return (id - GX_TEXMTX0) / 3;
}

/* `gen` is the GX texcoord generator index. 0 is the one the PVR's single
 * texture unit actually uses; 1 exists because a two-stage TEV binds a second
 * texture through GX_TEXCOORD1 with its own matrix (emu64.c:2673-2676 loads
 * GX_TEXMTX1 and points GX_TEXCOORD1 at it, sourced from the SAME GX_TG_TEX0
 * vertex attribute), and the tex1-alpha fold below has to reproduce it on the
 * CPU. */
static void apply_texgen_n(const DCGXVertex* v, int gen, float* u, float* v_out) {
    int slot;
    const float (*tm)[4];

    *u = v->texcoord[0];
    *v_out = v->texcoord[1];

    if (g_gx.num_tex_gens <= gen)
        return;
    /* Only GX_TG_TEX0-sourced 2x4/3x4 matrix texgens are handled. BUMP and
     * SRTG texgens, and POS/NRM sources, fall through to the raw texcoord —
     * wrong mapping, still visible. TODO(M3): kb/tev-map.md census. */
    if (g_gx.tex_gen_src[gen] != GX_TG_TEX0)
        return;
    slot = texmtx_slot(g_gx.tex_gen_mtx[gen]);
    if (slot < 0 || slot >= 10)
        return;

    tm = (const float (*)[4])g_gx.tex_mtx[slot];
    *u     = tm[0][0] * v->texcoord[0] + tm[0][1] * v->texcoord[1] + tm[0][3];
    *v_out = tm[1][0] * v->texcoord[0] + tm[1][1] * v->texcoord[1] + tm[1][3];
}

static void apply_texgen(const DCGXVertex* v, float* u, float* v_out) {
    apply_texgen_n(v, 0, u, v_out);
}

#ifndef DC_PVR_NO_TEX1ALPHA
/* ==========================================================================
 * The second texture's alpha
 * ==========================================================================
 * THE DEFECT. The PVR has one texture unit. When a TEV alpha combiner is
 * TEXEL0.a * TEXEL1.a this backend binds texmap0 and silently drops the second
 * factor, so an effect whose whole shape is the PRODUCT of two ramps collapses
 * to whatever texmap0 contributes on its own.
 *
 * THE CASE THAT FORCED IT, decoded from the retail foresta.rel and then
 * confirmed in a batch log. `rom_train_out_shineglass_modelT`
 * (rom_train_out.c:114) is the light shaft through the train window:
 *
 *   combiner  colour = PRIMITIVE,  alpha = TEXEL0.a * TEXEL1.a * PRIM_LOD_FRAC
 *   TEXEL0    rom_train_shine_tex, I4 64x8  — a pure HORIZONTAL ramp
 *   TEXEL1    rom_train_glass_tex, I4 16x16 — a pure VERTICAL ramp
 *
 * Neither texture is a light shaft. The soft 2-D falloff IS the product, one
 * 1-D ramp per texture unit. And all four of the shine quad's vertices carry
 * the same s (168.0 texels, GX_CLAMP), so once texmap1 is dropped TEXEL0
 * contributes a single constant over the whole quad. The batch log says
 * exactly that:
 *
 *   BATCH ... 64x8 ... st=2 tm=0,1 t1=1 argb=88FFFFFF bbox=-144,-165..337,494
 *
 * flat white, one alpha (0x88) for every vertex, over a screen-sized wedge —
 * which is what a human reported as "the light on Rover should be subtle, not
 * this rectangle".
 *
 * THE FIX, and its honest limits. The second texture's alpha is sampled on the
 * CPU per VERTEX, through texgen 1, out of the 8x8 map dc_pvr_texture.c builds
 * at upload, and multiplied into the vertex alpha. MODULATEALPHA then
 * multiplies by TEXEL0 in hardware, so the emitted alpha is
 * vtx.a * T1.a(vertex) * T0.a(pixel) — the right product, but with the T1
 * factor interpolated between vertices instead of sampled per pixel.
 *
 * That is exact for a linear ramp across a quad, which is what this idiom is
 * always used for, and it is wrong for a second texture carrying high
 * frequency detail. It is applied ONLY where texmap1 binds a genuinely
 * different image from texmap0 — measured at 220 of 1231 two-stage batches;
 * the other 1009 point both texmaps at the same tile for N64 LOD
 * interpolation, which the PVR does in hardware and which is free to drop.
 *
 * Kill switch: -DDC_PVR_NO_TEX1ALPHA, which also removes the 32 KB of .bss the
 * alpha maps cost. */
static int tex1_alpha_active(const dc_pvr_tex_t* tex0) {
    const DCGXTevStage* t1;

    if (g_gx.num_tev_stages < 2) return 0;
    if (!tex0 || !tex0->base) return 0;
    /* A genuinely SECOND image, not the same tile bound twice. */
    if (g_gx.tex_handle[1] == 0 || g_gx.tex_handle[1] == g_gx.tex_handle[0])
        return 0;
    t1 = &g_gx.tev_stages[1];
    if (t1->tex_map == GX_TEXMAP_NULL) return 0;
    /* GX's alpha op is d + (1-c)*a + c*b, so (ZERO, APREV, TEXA, ZERO) is
     * exactly APREV * TEXEL1.a — the multiply we are missing. Nothing else is
     * claimed; any other shape keeps today's behaviour. */
    return t1->alpha_a == GX_CA_ZERO && t1->alpha_b == GX_CA_APREV &&
           t1->alpha_c == GX_CA_TEXA && t1->alpha_d == GX_CA_ZERO;
}

/* Nearest-cell lookup into the 8x8 map. Nearest and not bilinear on purpose:
 * this is already sampled at vertex rate and then linearly interpolated by the
 * rasteriser, so a second interpolation inside one cell buys nothing that the
 * hardware is not about to do anyway. `wrap` follows the GX wrap mode of
 * texmap1 — clamping a ramp is the whole point of this idiom. */
static unsigned int tex1_alpha_sample(const unsigned char* prof, int wrap_s,
                                      int wrap_t, float u, float v) {
    int iu, iv;

    if (wrap_s == GX_REPEAT || wrap_s == GX_MIRROR) {
        u = u - (float)(int)u;
        if (u < 0.0f) u += 1.0f;
    }
    if (wrap_t == GX_REPEAT || wrap_t == GX_MIRROR) {
        v = v - (float)(int)v;
        if (v < 0.0f) v += 1.0f;
    }
    iu = (int)(u * (float)DC_PVR_APROF_DIM);
    iv = (int)(v * (float)DC_PVR_APROF_DIM);
    if (iu < 0) iu = 0;
    if (iu >= DC_PVR_APROF_DIM) iu = DC_PVR_APROF_DIM - 1;
    if (iv < 0) iv = 0;
    if (iv >= DC_PVR_APROF_DIM) iv = DC_PVR_APROF_DIM - 1;
    return prof[iv * DC_PVR_APROF_DIM + iu];
}
#endif /* !DC_PVR_NO_TEX1ALPHA */

/* ==========================================================================
 * The seam
 * ========================================================================== */

void dc_gx_backend_init(void) {
    pvr_init_params_t p;
    int rc;

    memset(&p, 0, sizeof(p));
    /* Bins go opaque, opaque-mod, translucent, translucent-mod, punch-thru.
     * Only the list we actually submit to gets a bin; a zero-length bin
     * disables the list and costs no VRAM. */
    p.opb_sizes[0] = (DC_PVR_LIST_MODE == 1) ? PVR_BINSIZE_32 : PVR_BINSIZE_0;
    p.opb_sizes[1] = PVR_BINSIZE_0;
    p.opb_sizes[2] = (DC_PVR_LIST_MODE == 1) ? PVR_BINSIZE_0 : PVR_BINSIZE_32;
    p.opb_sizes[3] = PVR_BINSIZE_0;
#ifdef DC_PVR_NO_PUNCHTHRU
    p.opb_sizes[4] = PVR_BINSIZE_0;
#else
    /* Enabling list 4 costs VRAM only — an OPB lives in the PVR's own 4 MB
     * half (pvr_buffers.c: pvr_allocate_buffers), not in main RAM. At
     * PVR_BINSIZE_32 that is opb_sizes[4]*4 * 20*15 tiles * (1 + overflow 3) =
     * 153,600 B per buffer set, 307,200 B total, against ~4.9 MB of free VRAM
     * and a 4 MB texture ceiling. A zero-length bin disables the list, which is
     * exactly what -DDC_PVR_NO_PUNCHTHRU restores. */
    p.opb_sizes[4] = DC_PVR_PT_BINSIZE;
#endif
    p.vertex_buf_size = DC_PVR_VERTBUF_BYTES;
    p.dma_enabled = 0;
    p.fsaa_enabled = 0;
    /* THE LOAD-BEARING LINE. Autosort makes the translucent list reorder
     * polygons per pixel; the game is a submission-ordered Z-buffered
     * renderer and reordering it produces wrong-but-plausible output that is
     * very hard to attribute. Disabled = plain Z-buffer semantics. */
    p.autosort_disabled = 1;
    p.opb_overflow_count = 3;
    p.vbuf_doublebuf_disabled = 0;

    rc = pvr_init(&p);
    if (rc < 0) {
        DC_LOGE("[DC/PVR] pvr_init FAILED (rc=%d). Video mode not 3D-capable, "
                "or the PVR was already up. Rendering stays dark.\n", rc);
        dc_pvr_ready = 0;
        return;
    }
    dc_pvr_ready = 1;

    pvr_set_zclip(0.0f);
    pvr_set_bg_color(0.0f, 0.0f, 0.0f);

#ifndef DC_PVR_NO_PUNCHTHRU
    PVR_SET(DC_PVR_REG_PT_ALPHA_REF, (unsigned int)(DC_PVR_PT_ALPHA_REF) & 0xFFu);
#endif

    dc_pvr_texture_init();

#ifdef DC_PVR_NO_PUNCHTHRU
    DC_LOGE("[DC/PVR] backend up: list=%s autosort=off vertbuf=%d B "
            "opb=32 overflow=3 pt=off\n",
            (DC_PVR_LIST_MODE == 1) ? "OP" : "TR", DC_PVR_VERTBUF_BYTES);
#else
    DC_LOGE("[DC/PVR] backend up: list=%s autosort=off vertbuf=%d B "
            "opb=32 overflow=3 pt=on binsize=%d ref=%d buf=%d recs/%u B\n",
            (DC_PVR_LIST_MODE == 1) ? "OP" : "TR", DC_PVR_VERTBUF_BYTES,
            (int)DC_PVR_PT_BINSIZE, (int)DC_PVR_PT_ALPHA_REF,
            (int)DC_PVR_PT_BUF_RECS, (unsigned int)sizeof(s_pt_buf));
#endif
}

void dc_gx_backend_shutdown(void) {
    if (!dc_pvr_ready) return;
    dc_pvr_texture_shutdown();
    pvr_shutdown();
    dc_pvr_ready = 0;
}

void dc_gx_backend_frame_begin(void) {
    if (!dc_pvr_ready) return;

    /* RE-ENTRY GUARD. `pc_gx_begin_frame()` has TWO callers, and both fire every
     * frame: dc_vi.c's retrace handler, and JW_BeginFrame
     * (src/static/jsyswrap.cpp:326, under TARGET_PC). MEASURED in a batch log —
     * 52 `BATCHLOG BEGIN` lines against 27 `BATCHLOG END` — so this function was
     * running pvr_wait_ready() + pvr_scene_begin() twice per scene.
     *
     * Nothing visibly broke, because in every logged frame the two calls were
     * adjacent with no batch between them. The hazard is that they need not be:
     * VIWaitForRetrace() is also reached from padmgr.c:410, JKRFile.cpp:41,
     * graph.c:267,321 and jsyswrap.cpp:233, and a second scene_begin AFTER
     * geometry has been submitted resets s_hdr_valid, s_hdr_key and — the one
     * that loses pixels — s_pt_n, discarding every buffered punch-through
     * record. KOS's pvr_scene_begin() also clears lists_closed and sets
     * list_reg_open = PVR_LIST_NONE under the open list.
     *
     * Returning early is exactly right: the scene the second caller wants is
     * the one already open. Kill switch: -DDC_PVR_NO_FRAMEBEGIN_GUARD. */
#ifndef DC_PVR_NO_FRAMEBEGIN_GUARD
    if (s_scene_open) return;
#endif

    pvr_wait_ready();
#ifndef DC_PVR_NO_FOG
    /* Between pvr_wait_ready() and pvr_scene_begin() is the only point in the
     * frame with no render in flight. KOS pvr_fog.c: "You should only call
     * these functions outside of the pvr_scene_begin, pvr_scene_finish. If you
     * call to change fog parameters while the pvr is rendering the scene you
     * will get artifacts in the image." */
    fog_program();
#ifdef DC_PVR_FOG_LOG
    /* ask=0 means the game never asked for fog at all — that is the single
     * question this diagnostic exists to answer. hw=1 means the table is
     * programmed; batches is how many batches wanted fog in the last frame. */
    if ((s_frames % (unsigned int)(DC_PVR_FOG_LOG)) == 0)
        DC_LOGE("[FOG] frame=%u ask=%d hw=%d batches=%u start=%.2f end=%.2f "
                "rgb=%.2f,%.2f,%.2f progs=%u\n",
                s_frames, s_fog_pend, s_fog_hw, s_fog_batches,
                (double)s_fog_hw_start, (double)s_fog_hw_end,
                (double)s_fog_hw_col[0], (double)s_fog_hw_col[1],
                (double)s_fog_hw_col[2], s_fog_programs);
    s_fog_batches = 0;
#endif
#endif
    pvr_scene_begin();
    s_scene_open = 1;
    s_list_open = 0;
#ifndef DC_PVR_NO_PUNCHTHRU
    /* Re-assert the threshold every frame. It is one uncached 32-bit store and
     * the register is global, undocumented in KOS, and written by nobody else
     * in this build — but "nobody else" is an assumption about every library in
     * the image, and a silently reset PT_ALPHA_REF would present as cutouts
     * losing their holes again, i.e. as this whole fix having been reverted. */
    PVR_SET(DC_PVR_REG_PT_ALPHA_REF, (unsigned int)(DC_PVR_PT_ALPHA_REF) & 0xFFu);
    s_pt_n = 0;
    s_pt_route = 0;
    s_pt_trunc = 0;
#endif
    /* The header cache cannot survive a scene boundary: the TA latches state
     * per list, and a new list starts with nothing latched. */
    s_hdr_valid = 0;
    s_hdr_key = 0xFFFFFFFFu;

#ifdef DC_PVR_BATCH_LOG
    s_batch_log_now = ((s_frames % (unsigned int)(DC_PVR_BATCH_LOG)) == 0);
    if (s_batch_log_now)
        DC_LOG("BATCHLOG BEGIN frame=%u\n", s_frames);
#endif

#ifdef DC_PVR_DEBUG_BG
    /* Bring-up only. A black frame has two very different causes — "the PVR
     * never presented anything" and "it presented, but every polygon is black
     * or degenerate" — and the framebuffer hash cannot tell them apart. A
     * forced non-black background separates them in one run: if the probe
     * still reports all-zero, presentation itself is broken and the geometry
     * is not the suspect. */
    pvr_set_bg_color(0.0f, 0.15f, 0.45f);
#else
    pvr_set_bg_color(g_gx.clear_color[0], g_gx.clear_color[1],
                     g_gx.clear_color[2]);
#endif
}

void dc_gx_backend_frame_end(void) {
    if (!dc_pvr_ready) return;
    if (!s_scene_open) return;

    /* Open the list even if nothing was drawn: KOS submits a blank one to
     * satisfy the hardware, and skipping it leaves the TA waiting. */
    if (!s_list_open) {
        pvr_list_begin(DC_PVR_BASE_LIST);
        s_list_open = 1;
    }
    pvr_list_finish();
    s_list_open = 0;

#ifndef DC_PVR_NO_PUNCHTHRU
    /* THE REPLAY. The general list is closed, so list 4 is now the only one
     * that may still be opened this frame (lists must be submitted in strictly
     * increasing order; PT is last). The buffer already holds a valid
     * header/vertex command stream in submission order, so this is a straight
     * copy — no state is recompiled and no geometry is re-derived.
     *
     * Skipping the open when the buffer is empty is safe: pvr_scene_finish()
     * submits a blank poly header for every enabled-but-unused list
     * (pvr_scene.c), which is exactly what an empty PT list needs. */
    if (s_pt_n) {
        unsigned int i;
        pvr_list_begin(PVR_LIST_PT_POLY);
        for (i = 0; i < s_pt_n; i++)
            pvr_prim(&s_pt_buf[i], 32);
        pvr_list_finish();
        s_pt_recs += s_pt_n;
        if (s_pt_n > s_pt_hi) s_pt_hi = s_pt_n;
        s_pt_n = 0;
    }
    /* Say it ONCE, loudly, the first time it happens. The periodic [DC/PVR]
     * line carries ptdrop= forever after, but a full buffer means cutouts are
     * silently absent from the screen — and "a model is missing" is exactly the
     * symptom this whole change exists to stop being unattributable. The
     * default 2048 records was sized from an ambiguous measurement (316 of 2331
     * cutout batches, over a sample whose frame count is not recorded), so this
     * is the line that turns the guess into a number. */
    if (s_pt_drop && !s_pt_warned) {
        s_pt_warned = 1;
        DC_LOGE("[DC/PVR] PT RECORD BUFFER FULL: %u triangles dropped, cap %d "
                "records (%u B). Cutout geometry is MISSING from the frame. "
                "Rebuild with a larger -DDC_PVR_PT_BUF_RECS and watch pthi= "
                "in the periodic [DC/PVR] pt line.\n",
                s_pt_drop, (int)DC_PVR_PT_BUF_RECS,
                (unsigned int)sizeof(s_pt_buf));
    }
#endif

    pvr_scene_finish();
    s_scene_open = 0;
#ifdef DC_PVR_BATCH_LOG
    if (s_batch_log_now)
        DC_LOG("BATCHLOG END frame=%u\n", s_frames);
#endif
    s_frames++;
}

void dc_gx_backend_submit(int prim, const DCGXVertex* verts, int count) {
    /* 32-byte aligned for mat_load(): KOS requires 8 and wants 32. Under
     * -DDC_PVR_NO_FTRV nothing reads it through mat_load and the alignment is
     * merely harmless. */
    float comb[4][4] __attribute__((aligned(32)));
    const float (*mv)[4];
    const float (*pr)[4];
    const float (*nm)[3];
    const dc_pvr_tex_t* tex;
#ifndef DC_PVR_NO_TEX1ALPHA
    const unsigned char* tex1_prof = NULL;
    int tex1_wrap_s = GX_CLAMP, tex1_wrap_t = GX_CLAMP;
#endif
#ifndef DC_PVR_NO_TEVCONST
    float tevconst[3];
    int   have_tevconst = 0;
#ifdef DC_PVR_TEVFOLD
    float foldk0[3], foldk1[3];
    int   have_tevfold = 0;
#endif
#endif
#ifdef DC_PVR_TEVP3
    float p3base[3], p3off[3];
    int   have_p3 = 0;
#endif
#ifndef DC_PVR_NO_TEVCONST
#ifndef DC_PVR_NO_TEVCONST_ALPHA
    float tevalpha = 1.0f;
    int   have_tevalpha = 0;
#endif
#endif
#if !defined(DC_PVR_NO_LIGHTING) && !defined(DC_PVR_NO_SHADEFAST)
    /* Does anything this batch draws actually need the eye-space position and
     * the transformed, renormalised normal? Both are per-VERTEX and neither is
     * cheap — the normal alone costs a 3x3 multiply, a dot product, a sqrtf and
     * a divide — and shade_vertex() reads them only from inside chan_eval()'s
     * `chan_ctrl_enable[ctl]` branch. So when no channel enables a light the
     * whole of both is dead code that ran anyway, on every vertex of the town.
     *
     * The predicate must stay identical to the one shade_vertex() uses to take
     * its pass-through path; it is written out in both places on purpose,
     * because a mismatch would mean reading uninitialised eye[]/nrm[] rather
     * than producing a wrong colour, and that is a much worse failure. */
    int need_light;
#endif
    /* Every predicate shade_vertex() branches on, decided once. See
     * shade_batch_mode(). -DDC_PVR_NO_SHADE_HOIST moves the call back into the
     * vertex loop, which is an exact revert. */
    unsigned int shade_mode = 0;
    int i, j, step, per_prim;

    if (!dc_pvr_ready || !s_scene_open || count <= 0) return;

    /* GXBegin has already rewritten strips and fans into independent
     * triangles (dc_gx.c:556), so only these two shapes can arrive with
     * geometry in them. Points and lines are counted and dropped: nothing in
     * the title path emits them, and a wrong-looking line is not worth the
     * quad expansion right now. */
    if (prim == GX_TRIANGLES)      per_prim = 3;
    else if (prim == GX_QUADS)     per_prim = 4;
    else { s_prim_unsupported++; return; }

    if (count < per_prim) return;

    if (!s_list_open) {
        pvr_list_begin(DC_PVR_BASE_LIST);
        s_list_open = 1;
        s_hdr_valid = 0;
        s_hdr_key = 0xFFFFFFFFu;
    }

    /* A draw that asks for NO texture must not get one. GXSetTevOrder's tex_map
     * is GX_TEXMAP_NULL for the whole JSystem 2D path — J2DGrafContext::setup2D
     * (J2DGrafContext.cpp:29-31) sets GX_PASSCLR + GX_TEXMAP_NULL +
     * GXSetNumTexGens(0) — but this used to bind g_gx.tex_handle[0]
     * unconditionally, and nothing ever clears that handle. Since
     * GXPosition3f32 resets texcoord to (0,0) per vertex (dc_gx.c:654), such a
     * pane sampled texel (0,0) of whatever emu64 happened to bind last and
     * MODULATEALPHA multiplied it into both colour AND alpha: an opaque-black
     * texel blacked the pane out, a zero-alpha texel made it vanish, and which
     * one you got depended on draw order that frame. Letterbox bars, dialogue
     * frames and fade quads all live on this path.
     *
     * Only GX_TEXMAP_NULL suppresses the bind. g_gx is zero-initialised, so
     * tex_map == 0 == GX_TEXMAP0 is the "nobody called GXSetTevOrder" default
     * and must keep its texture. Kill switch: -DDC_PVR_NO_TEXNULL. */
    tex = dc_pvr_tex_get(g_gx.tex_handle[0]);
#ifndef DC_PVR_NO_TEXNULL
#ifdef DC_PVR_TEXNULL_STAGE0_ONLY
    if (g_gx.tev_stages[0].tex_map == GX_TEXMAP_NULL) tex = NULL;
#else
    /* ⚠️ 2026-08-04: this test used to read STAGE 0 ONLY, and that is wrong
     * for a config that parks GX_TEXMAP_NULL on stage 0 and carries the
     * texture on stage 1. Such a batch got tex = NULL and drew untextured.
     *
     * The blast radius was pinned by enumerating the PRODUCER rather than
     * guessing from the TEV table. `GXSetTevOrder(GX_TEVSTAGE0,
     * GX_TEXCOORD_NULL, GX_TEXMAP_NULL, ...)` has five sites in emu64; four
     * (emu64.c:1949, :1968, :3000, :3006) are genuinely textureless, and
     * exactly one — emu64.c:1771-1772, the combine_manual case for
     * gsDPSetCombineLERP(PRIMITIVE, SHADE, PRIM_LOD_FRAC, SHADE, 0,0,0,TEXEL0,
     * TEXEL1,0,COMBINED,0, 0,0,0,COMBINED) — puts the image on stage 1.
     * That combiner has six display-list sites in all of src/, and every one
     * of them is in src/data/model/obj_s_shop1.c. So this bug is precisely
     * "Tom Nook's shop draws untextured". combine_auto can never produce the
     * shape: it assigns GX_TEXMAP0 to stage 0 unconditionally (emu64.c:1265).
     *
     * The correct test is "does ANY active stage bind a texmap". The JSystem
     * 2D path is unaffected because it sets GX_TEXMAP_NULL on ALL its stages.
     * Both of shop1's stages resolve to GX_TEXMAP0, so tex_handle[0] is still
     * the right handle to bind — there is no second handle to chase here.
     *
     * Kill switch: -DDC_PVR_TEXNULL_STAGE0_ONLY restores the old test
     * verbatim; -DDC_PVR_NO_TEXNULL keeps its existing meaning (bind
     * unconditionally, the pre-2026-08-02 behaviour). */
    {
        int ns = g_gx.num_tev_stages;
        int si, any_map = 0;
        /* Clamp to the ARRAY bound (16), not DC_GX_MAX_TEV_STAGES (3).
         * That constant is the measured max across the 101 configs, i.e. an
         * observation, and using an observation as an array bound is how a
         * 17-stage config would silently read as textureless. */
        if (ns <= 0) ns = 1;
        if (ns > (int)(sizeof g_gx.tev_stages / sizeof g_gx.tev_stages[0]))
            ns = (int)(sizeof g_gx.tev_stages / sizeof g_gx.tev_stages[0]);
        for (si = 0; si < ns; si++) {
            if (g_gx.tev_stages[si].tex_map != GX_TEXMAP_NULL) { any_map = 1; break; }
        }
        if (!any_map) tex = NULL;
    }
#endif
#endif

#ifndef DC_PVR_NO_PUNCHTHRU
    /* Decide the destination list BEFORE the header is compiled or looked up:
     * header_key() folds it in and compile_header() bakes it into the TA
     * command word, and every submit_prim() below this line reads it. */
    s_pt_route = pt_route_active();
    if (s_pt_route) s_pt_batches++;
#endif

#ifdef DC_PVR_ALPHAENV
    /* Same rule as s_pt_route: decided BEFORE header_key()/compile_header(),
     * once, so the two cannot disagree about which env this batch wants. */
    s_alpha_env_texel = (tex && tex->base) ? alpha_env_texel_only() : 0;
    if (s_alpha_env_texel) s_env_texel_batches++;
#endif

#ifdef DC_PVR_TEVP3
    /* Third member of the same family, and it MUST be before ensure_header()
     * for the same reason as the two above. Note the ordering against
     * have_tevconst below: P3 is strictly narrower (it requires TEXC in the
     * lerp weight, which tev_const_color() rejects outright), so the two
     * predicates cannot both fire and the arm below is an `else if` only for
     * readability. */
    have_p3 = (tex && tex->base) ? tev_p3_affine(p3base, p3off) : 0;
    if (have_p3) {
        int ch, clamped = 0;
        s_p3_batches++;
        for (ch = 0; ch < 3; ch++) if (p3base[ch] < 0.0f) clamped = 1;
        if (clamped) s_p3_clamped++;
        /* An all-zero offset is the P2 arm: it needs the vertex base colour but
         * NOT the specular bit, and asking for the bit anyway would compile a
         * second header for no reason and churn the header cache. */
        s_p3_specular = (p3off[0] != 0.0f || p3off[1] != 0.0f ||
                         p3off[2] != 0.0f);
    } else {
        s_p3_specular = 0;
    }
#endif

#ifndef DC_PVR_NO_FOG
    /* Remember what this batch wants; frame_begin programs it. Must run every
     * batch, not only on a header-key change — the fog parameters are not in
     * the key. */
    fog_latch();
#endif

    ensure_header(tex);

#ifndef DC_PVR_NO_TEX1ALPHA
    /* Per batch: the TEV stages and both texture bindings are fixed inside one
     * batch, so the predicate and the map pointer are resolved once. */
    tex1_prof = tex1_alpha_active(tex) ? dc_pvr_tex_aprof(g_gx.tex_handle[1])
                                       : NULL;
    if (tex1_prof) {
        tex1_wrap_s = g_gx.tex_obj_wrap_s[1];
        tex1_wrap_t = g_gx.tex_obj_wrap_t[1];
        s_tex1_batches++;
    }
#endif

#ifndef DC_PVR_NO_TEVCONST
    /* Per-batch, not per-vertex: the TEV stage and its registers cannot change
     * inside a batch, only between them. */
    have_tevconst = tev_const_color(tevconst);
#ifdef DC_PVR_TEVFOLD
    /* Only where the narrow shape declined, so every batch the old code
     * handled keeps its exact result and this can only add coverage. */
    if (!have_tevconst)
        have_tevfold = tev_fold_color(foldk0, foldk1);
#endif
#ifndef DC_PVR_NO_TEVCONST_ALPHA
    have_tevalpha = tev_const_alpha(&tevalpha);
#ifndef DC_PVR_NO_TEVALPHA_LAST
    {
        float klast;
        if (tev_const_alpha_last(&klast)) {
            /* PRODUCT, not replacement. GX chains its stages, so a stage-0
             * constant and a last-stage constant are two factors of one alpha;
             * assigning here instead of multiplying would delete the first.
             * Both are per-batch — the TEV stages and the TEV registers cannot
             * change inside one batch. */
            tevalpha = have_tevalpha ? (tevalpha * klast) : klast;
            have_tevalpha = 1;
            s_tevalpha_last_batches++;
        }
    }
#endif
#endif
#endif

#ifdef DC_PVR_BATCH_LOG
    /* Zero the extents too, not just the count: a batch whose triangles are
     * all dropped never reaches emit_projected, and printing an uninitialised
     * bbox would invent geometry. verts=0 is the honest answer. */
    s_bl_n = 0;
    s_bl_x0 = s_bl_y0 = s_bl_x1 = s_bl_y1 = 0.0f;
    s_bl_z0 = s_bl_z1 = 0.0f;
    s_bl_u0 = s_bl_v0 = s_bl_u1 = s_bl_v1 = 0.0f;
    s_bl_argb = 0;
#endif

    /* Fold projection * posmtx once per batch. The modelview is GX's 3x4
     * row-major affine matrix, so its implicit fourth row is (0,0,0,1) and the
     * fourth column of the product is just the projection's own translation
     * column plus the modelview's. */
    mv = (const float (*)[4])g_gx.pos_mtx[g_gx.current_mtx];
    pr = (const float (*)[4])g_gx.projection_mtx;
    nm = (const float (*)[3])g_gx.nrm_mtx[g_gx.current_mtx];
    for (i = 0; i < 4; i++) {
        for (j = 0; j < 4; j++) {
            float s = pr[i][0] * mv[0][j] + pr[i][1] * mv[1][j] +
                      pr[i][2] * mv[2][j];
            if (j == 3) s += pr[i][3];
#ifdef DC_PVR_NO_FTRV
            comb[i][j] = s;
#else
            /* TRANSPOSED on purpose. mat_load() copies 16 consecutive floats
             * into XF0..XF15 in order (KOS matrix.s: eight `fmov @r4+, xdN`),
             * and FTRV computes fr0' = XF0*x + XF4*y + XF8*z + XF12*w — i.e. it
             * reads the loaded array COLUMN-major. Writing the fold transposed
             * is free here (it is one index swap in a 16-iteration loop that
             * runs once per batch) and saves transposing 3,000 times a frame. */
            comb[j][i] = s;
#endif
        }
    }
#ifndef DC_PVR_NO_FTRV
    /* XMTRX is a single global hardware register bank, so this is only safe
     * because NOTHING in the vertex loop below can touch it: apply_texgen,
     * shade_vertex, emit_triangle and pvr_prim are all plain C or integer
     * asm, and dc_mtx.c's PSMTX* — the only other XMTRX user in the image —
     * is not reachable from here. Interrupts and thread switches ARE safe:
     * KOS's entry.s saves and restores BOTH floating-point banks
     * (kernel/entry.s, the `frchg` pair around eight `fmov drN,@-r0`). */
    mat_load((const matrix_t*)comb);
    /* We have just taken XMTRX away from dc_mtx.c's residency cache
     * (kb/research-fps-ideas.md F2). It has no other way to know: it keys on
     * the source matrix pointer and contents, neither of which changed. */
    dc_mtx_xmtrx_invalidate();
#endif

#if !defined(DC_PVR_NO_LIGHTING) && !defined(DC_PVR_NO_SHADEFAST)
    need_light = (g_gx.num_chans > 0) &&
                 (g_gx.chan_ctrl_enable[0] || g_gx.chan_ctrl_enable[1]);
#endif

#ifndef DC_PVR_NO_SHADE_HOIST
    shade_mode = shade_batch_mode();
#endif

#ifndef DC_PVR_NO_VTXMEMO
    /* Invalidate on entry, not on exit: everything the memo depends on that is
     * NOT the source vertex — the folded matrix, mv, nm, the light state, the
     * TEV constants, s_pt_route, tex->u_scale — is established above and is
     * constant from here to the end of this call, and changes between calls.
     * One increment, because the table is 128 entries and clearing it per batch
     * would cost more than the hits it buys. */
    s_vmemo_gen++;
#endif

    step = per_prim;
    for (i = 0; i + per_prim <= count; i += step) {
        ClipVtx cv[4];
        int k;
        VS_DECL;

        VS_SAMPLE_BEGIN;
        for (k = 0; k < per_prim; k++) {
            const DCGXVertex* v = &verts[i + k];
            float ox, oy, oz;
            float eye[3], nrm[3], nl;

            /* S14 — pull the NEXT source vertex in while this one is being
             * transformed. The SH-4 has no hardware prefetcher, so a
             * sequentially walked array still takes a full miss per line; the
             * only defence is the `pref` instruction, issued far enough ahead
             * to overlap something. Since session 12 DCGXVertex is exactly 32
             * bytes and 32-byte aligned, so verts[n+1] is exactly the next
             * cache line and one `pref` covers the whole vertex.
             *
             * ONE LINE AHEAD, NOT FOUR. The only quantitative memory statement
             * in the sh4zam repo is that the SH-4 has ONE prefetch in flight at
             * a time, ~10-12 cycles to complete, and that overlapping
             * prefetches STALL the pipeline. This loop body is hundreds of
             * cycles on a memo miss and tens on a hit, so a single line of
             * lookahead is already covered and a deeper distance would only
             * issue prefetches that collide with each other.
             *
             * ⚠️ UNCONDITIONAL, INCLUDING ONE PAST THE LAST VERTEX. `pref`
             * cannot fault: the MMU is off (CLAUDE.md hard rule), all of RAM is
             * mapped, and verts is g_gx.vertex_buffer — a static object — so
             * &verts[count] is an address in .bss either way. Paying a compare
             * and a branch per vertex to avoid one harmless line fill per
             * PRIMITIVE would cost more than it saves.
             *
             * The asm is deliberately NOT sh4zam's SHZ_PREFETCH even though it
             * is the identical instruction: sh4zam's headers carry alignment
             * and FP-mode assert()s that are only killed by the -DNDEBUG the
             * Makefile scopes to sh4zam's own TUs ($(SHZ_OBJS): TU_DEFS), so
             * including one HERE would compile those asserts into the vertex
             * loop. That trap is written up in kb/research-sh4zam-gap.md G-A.
             *
             * Kill switch: -DDC_PVR_NO_PREFETCH. */
#if !defined(DC_PVR_NO_PREFETCH) && (defined(__SH4__) || defined(__sh__))
            __asm__ __volatile__("pref @%0" : : "r" (&verts[i + k + 1]));
#endif
            ox = v->position[0]; oy = v->position[1]; oz = v->position[2];
#ifndef DC_PVR_NO_VTXMEMO
            unsigned int slot;
            int memo_hit;
#ifdef DC_GX_VTXID
            /* G-B. WHEN emu64's OWN INDEX IS ON THE VERTEX, THE LOOKUP IS TWO
             * SEQUENTIAL LOADS. `slot` is the index itself — emu64's vertex
             * cache is 128 entries and so is this table — so it is injective
             * within a TRIN and there is no hash, no 30-byte compare, and
             * above all NO RANDOM READ INTO verts[]. That read is what made
             * this stage 122 cycles a vertex: it is an operand-cache miss, not
             * arithmetic (kb/RESUME.md session 11b).
             *
             * The stamp compare is the epoch half — GXBegin can merge two TRIN
             * commands into one submit, so `tag == gen` alone is NOT
             * sufficient. See dc_gx.c's dc_gx_vtxid_arm().
             *
             * No id/hash mixing is possible inside one batch: the side channel
             * is armed for a whole TRIN or not at all (dc_emu64_cull.cpp), so
             * every vertex of a given TRIN either carries a stamp or none
             * does. A merged submit CAN mix an armed TRIN with an unarmed one,
             * which is exactly why the two lookups stay separate and the
             * unarmed side keeps its content compare. */
            const unsigned int vid = v->vtxid;
            if (vid != DC_GX_VTXID_NONE) {
                slot = vid & (VMEMO_SLOTS - 1u);
                memo_hit = (s_vmemo_tag[slot] == s_vmemo_gen) &&
                           (s_vmemo_vid[slot] == vid);
#ifdef DC_GX_VTXID_VERIFY
                /* THE GATE. A desynced side channel does not fail, it returns
                 * another vertex's transform — so on every id hit, run the
                 * compare the fast path just skipped and count disagreements.
                 * `vidbad=` MUST read 0. This build is slower by construction;
                 * it is a correctness run, never a perf run. */
                s_vid_checked++;
                if (memo_hit && !vmemo_same(v, &verts[s_vmemo_src[slot]]))
                    s_vid_bad++;
#endif
            } else
#endif
            {
                slot = vmemo_hash(v);
                memo_hit = (s_vmemo_tag[slot] == s_vmemo_gen) &&
                           vmemo_same(v, &verts[s_vmemo_src[slot]]);
            }

            dc_pvr_vmemo_total++;
            if (memo_hit) {
                cv[k] = VMEMO_VAL(slot);
                dc_pvr_vmemo_hit++;
                VS_MARK(VS_MEMO);
                VS_HIT;
                continue;
            }
            VS_MARK(VS_MEMO);
#endif

#ifdef DC_PVR_NO_FTRV
            cv[k].x = comb[0][0] * ox + comb[0][1] * oy + comb[0][2] * oz + comb[0][3];
            cv[k].y = comb[1][0] * ox + comb[1][1] * oy + comb[1][2] * oz + comb[1][3];
            cv[k].z = comb[2][0] * ox + comb[2][1] * oy + comb[2][2] * oz + comb[2][3];
            cv[k].w = comb[3][0] * ox + comb[3][1] * oy + comb[3][2] * oz + comb[3][3];
#else
            {
                /* One FTRV against the matrix loaded above. mat_trans_nodiv is
                 * KOS's no-perspective-divide form: this backend needs the raw
                 * clip-space w for the near-plane clip and does its own divide
                 * in emit_projected(). */
                float tx = ox, ty = oy, tz = oz, tw = 1.0f;
                mat_trans_nodiv(tx, ty, tz, tw);
                cv[k].x = tx; cv[k].y = ty; cv[k].z = tz; cv[k].w = tw;
            }
#endif
            VS_MARK(VS_XF);

#if !defined(DC_PVR_NO_LIGHTING) && !defined(DC_PVR_NO_SHADEFAST)
            if (need_light)
#endif
            {
            /* Six FIPRs where this was 21 multiplies and 15 adds. The 3x4
             * modelview row dotted with (ox,oy,oz,1) is exactly FIPR's shape;
             * the 3x3 normal matrix uses a zero fourth term. */
            eye[0] = DC_DOT4(mv[0][0], mv[0][1], mv[0][2], mv[0][3], ox, oy, oz, 1.0f);
            eye[1] = DC_DOT4(mv[1][0], mv[1][1], mv[1][2], mv[1][3], ox, oy, oz, 1.0f);
            eye[2] = DC_DOT4(mv[2][0], mv[2][1], mv[2][2], mv[2][3], ox, oy, oz, 1.0f);

            {
                float nx = v->normal[0] * (1.0f / DC_GX_NRM_SCALE);
                float ny = v->normal[1] * (1.0f / DC_GX_NRM_SCALE);
                float nz = v->normal[2] * (1.0f / DC_GX_NRM_SCALE);
                nrm[0] = DC_DOT3(nm[0][0], nm[0][1], nm[0][2], nx, ny, nz);
                nrm[1] = DC_DOT3(nm[1][0], nm[1][1], nm[1][2], nx, ny, nz);
                nrm[2] = DC_DOT3(nm[2][0], nm[2][1], nm[2][2], nx, ny, nz);
                nl = DC_DOT3(nrm[0], nrm[1], nrm[2], nrm[0], nrm[1], nrm[2]);
                if (nl > 1e-12f) {
                    nl = DC_RSQRT(nl);
                    nrm[0] *= nl; nrm[1] *= nl; nrm[2] *= nl;
                }
            }
            }
            VS_MARK(VS_LIT);

            apply_texgen(v, &cv[k].u, &cv[k].v);
            if (tex) {
                cv[k].u *= tex->u_scale;
                cv[k].v *= tex->v_scale;
            }
            VS_MARK(VS_TEX);
#ifdef DC_PVR_NO_SHADE_HOIST
            shade_mode = shade_batch_mode();
#endif
            /* The pass-through test is lifted out of the call as well: on a
             * pass-through batch the whole of shade_vertex() is four byte
             * loads and three shifts, and paying a function call for that was
             * most of what `shade` cost. */
            cv[k].argb = (shade_mode & SHADE_PASSTHRU)
                             ? pack_vtx_argb(v)
                             : shade_vertex(v, eye, nrm, shade_mode);
            VS_MARK(VS_SHADE);
#ifndef DC_PVR_NO_PUNCHTHRU
#ifndef DC_PVR_PT_KEEP_VTXALPHA
            /* SHADE ALPHA IS NOT AN OPACITY, AND IT MUST NOT REACH THE PT
             * COMPARATOR.
             *
             * cxt.txr.env is PVR_TXRENV_MODULATEALPHA, so the alpha the
             * hardware tests is vertex_alpha * texel_alpha. On the N64 the two
             * were never multiplied: every alpha-tested display list in this
             * game runs G_RM_FOG_SHADE_A, where the vertex alpha byte is the
             * per-vertex FOG COEFFICIENT for the blender, and the alpha half of
             * the colour combiner is (0,0,0,TEXEL0) — texel alpha, alone.
             *
             * DECODED out of the retail foresta.rel, then CONFIRMED in a batch
             * log before this was written:
             *   obj_romtrain_door_v[0..7]  (the door LEAF, obj_romtrain_door.c:70)
             *       cn = (230,220,255,0) (190,180,220,0) (140,130,200,0)
             *            (90,80,150,0)              -> alpha 0
             *   rom_train_out_v[8..15]     (the window's TUNNEL MASK,
             *                               rom_train_out.c:130)
             *       cn = (50,50,50,50)              -> alpha 50
             * and in the log, every frame: `pt=1 ... argb=32323232` on a 64x32
             * with a window-sized bbox, and `pt=1 ... argb=005A5096` on a 32x64
             * door-shaped one. Both are AA_ZB_TEX_EDGE2, so both route here,
             * and 0 and 50 are both below the 144 threshold — so EVERY texel of
             * both models failed the test and both vanished outright. Reported
             * by a human as "the train door has disappeared entirely, the glass
             * is there" (the glass is verts 8..11 at alpha 255 and is XLU, so
             * it never comes here; the door's third joint, verts 12..14, is a
             * degenerate zero-area triangle in the shipped data and was never
             * visible) and as "a big weird light texture" in the window, which
             * is the bgsky/bgcloud/bgtree scenery the deleted tunnel mask
             * exists to cover.
             *
             * Forcing the vertex alpha opaque makes the tested alpha exactly
             * the texel alpha, which is what the RDP combiner produced. RGB is
             * untouched, so shade and lighting still modulate the colour.
             *
             * TEXTURED batches only. An UNTEXTURED punch-through poly has no
             * texel alpha at all, and there the vertex alpha genuinely is the
             * alpha GX would have tested — forcing it would turn a legitimate
             * discard into a paint.
             *
             * Runs before lerp_vtx() so near-clipped vertices inherit it, and
             * before the TEV-const override, which masks with 0xFF000000 and
             * therefore preserves it.
             *
             * ⚠️ MODULATEALPHA is wrong for this game's alpha combiner
             * everywhere, not only on PT — no display list read so far puts
             * SHADE in the alpha combiner. The non-PT cutout path has the same
             * defect. That is a wider change and wants its own measured pass.
             *
             * Kill switch: -DDC_PVR_PT_KEEP_VTXALPHA. -DDC_PVR_NO_PUNCHTHRU
             * still removes this with the rest of the PT path. */
            if (s_pt_route && tex && tex->base)
                cv[k].argb |= 0xFF000000u;
#endif
#endif
#ifndef DC_PVR_NO_TEVCONST
            /* Replace the RGB only. Alpha keeps coming from the lit/vertex
             * path so MODULATEALPHA still multiplies it by the texel alpha,
             * which is the half of the combiner that was already correct. */
            if (have_tevconst) {
                int cr = (int)(tevconst[0] * 255.0f + 0.5f);
                int cg = (int)(tevconst[1] * 255.0f + 0.5f);
                int cb = (int)(tevconst[2] * 255.0f + 0.5f);
                if (cr < 0) cr = 0;
                if (cr > 255) cr = 255;
                if (cg < 0) cg = 0;
                if (cg > 255) cg = 255;
                if (cb < 0) cb = 0;
                if (cb > 255) cb = 255;
                cv[k].argb = (cv[k].argb & 0xFF000000u) |
                             ((unsigned int)cr << 16) |
                             ((unsigned int)cg << 8) | (unsigned int)cb;
            }
#ifdef DC_PVR_TEVFOLD
            /* K0 + K1 * shade, per channel. K1 == 0 reduces to the replace
             * above; K1 == 1 with K0 == 0 leaves the shaded colour untouched,
             * which is what config #089 asks for. */
            else if (have_tevfold) {
                unsigned int sr = (cv[k].argb >> 16) & 0xFFu;
                unsigned int sg = (cv[k].argb >> 8) & 0xFFu;
                unsigned int sb = cv[k].argb & 0xFFu;
                int cr = (int)(foldk0[0] * 255.0f + foldk1[0] * (float)sr + 0.5f);
                int cg = (int)(foldk0[1] * 255.0f + foldk1[1] * (float)sg + 0.5f);
                int cb = (int)(foldk0[2] * 255.0f + foldk1[2] * (float)sb + 0.5f);
                if (cr < 0) cr = 0;
                if (cr > 255) cr = 255;
                if (cg < 0) cg = 0;
                if (cg > 255) cg = 255;
                if (cb < 0) cb = 0;
                if (cb > 255) cb = 255;
                cv[k].argb = (cv[k].argb & 0xFF000000u) |
                             ((unsigned int)cr << 16) |
                             ((unsigned int)cg << 8) | (unsigned int)cb;
            }
#endif
#endif
#ifdef DC_PVR_TEVP3
            /* CLASS P3: split the stage across the two hardware colours.
             * Base carries PRIM-ENV, which the texture env modulates; oargb
             * carries ENV, which the specular bit adds afterwards. Together
             * they reproduce GX's `ENV + (PRIM-ENV)*T0` exactly.
             *
             * ⚠️ oargb is written on EVERY path, not only when have_p3 — the
             * vertex memo caches the whole ClipVtx and emit_projected reads the
             * field unconditionally, so a path that skipped it would push stack
             * garbage into the TA. The zero default is also the correct value:
             * with the specular bit clear the hardware ignores it anyway.
             *
             * Alpha is untouched, deliberately. #037's alpha IS TEXEL0 alone,
             * which MODULATEALPHA gets wrong by the vertex-alpha factor — the
             * same defect DC_PVR_ALPHAENV addresses globally and which measured
             * a regression when applied globally. -DDC_PVR_P3_MODULATE narrows
             * that fix to P3 batches only, so its blast radius is 27 configs
             * rather than every draw in the game. Off by default: the RGB half
             * is the black-panel fix and does not need it. */
            cv[k].oargb = 0;
            if (have_p3) {
                int br = (int)(p3base[0] * 255.0f + 0.5f);
                int bg = (int)(p3base[1] * 255.0f + 0.5f);
                int bb = (int)(p3base[2] * 255.0f + 0.5f);
                int orr = (int)(p3off[0] * 255.0f + 0.5f);
                int og = (int)(p3off[1] * 255.0f + 0.5f);
                int ob = (int)(p3off[2] * 255.0f + 0.5f);
                /* The clamp that makes this inexact where PRIM < ENV. Counted
                 * per batch in s_p3_clamped, not silently swallowed. */
                if (br < 0) br = 0;
                if (br > 255) br = 255;
                if (bg < 0) bg = 0;
                if (bg > 255) bg = 255;
                if (bb < 0) bb = 0;
                if (bb > 255) bb = 255;
                if (orr < 0) orr = 0;
                if (orr > 255) orr = 255;
                if (og < 0) og = 0;
                if (og > 255) og = 255;
                if (ob < 0) ob = 0;
                if (ob > 255) ob = 255;
                cv[k].argb = (cv[k].argb & 0xFF000000u) |
                             ((unsigned int)br << 16) |
                             ((unsigned int)bg << 8) | (unsigned int)bb;
                cv[k].oargb = ((unsigned int)orr << 16) |
                              ((unsigned int)og << 8) | (unsigned int)ob;
            }
#endif
#ifndef DC_PVR_NO_TEVCONST
#ifndef DC_PVR_NO_TEVCONST_ALPHA
            /* MODULATEALPHA computes a = vtx.a * tex.a, so writing the
             * combiner's constant into the vertex alpha makes the hardware
             * evaluate exactly PRIM.a * TEXEL0.a — what mFont_CC_FONT asked
             * for and what the raster path cannot supply, because the glyph
             * vertices carry alpha 0 by design.
             *
             * -DDC_PVR_TEVCONST_ALPHA_RESCUE_ONLY narrows this to the vertices
             * that are currently fully invisible. Use it to separate "the font
             * came back" from "hundreds of world batches changed alpha" in one
             * build, without giving up the fix.
             *
             * `tevalpha` is now the PRODUCT of stage 0's constant and the last
             * stage's, either of which may be absent — tev_const_alpha_last()
             * above carries why they multiply. Nothing here changes: one
             * constant or two, the vertex alpha it writes is still a single
             * CPU-known scalar. */
            /* ⚠️ NEVER on a punch-through batch. The PT block above forces the
             * vertex alpha opaque so the comparator sees the texel alpha
             * alone; letting a constant land here afterwards would put a
             * product back in front of the 144 threshold and re-delete any
             * cutout whose constant alpha is below ~0.56 — which is exactly
             * the train door that was just recovered. GX really did test the
             * product, but this backend has already, deliberately, stopped
             * doing that for PT (see the block above); doing it by halves is
             * worse than either. Blended/opaque batches are unaffected, and
             * the train window's shine is XLU, so the fix this guard protects
             * still lands where it is needed. */
            if (have_tevalpha
#ifndef DC_PVR_NO_PUNCHTHRU
                && !s_pt_route
#endif
#ifdef DC_PVR_TEVCONST_ALPHA_RESCUE_ONLY
                && (cv[k].argb & 0xFF000000u) == 0u
#endif
               ) {
                int ca = (int)(tevalpha * 255.0f + 0.5f);
                if (ca < 0) ca = 0;
                if (ca > 255) ca = 255;
                cv[k].argb = (cv[k].argb & 0x00FFFFFFu) |
                             ((unsigned int)ca << 24);
            }
#endif
#endif
#ifndef DC_PVR_NO_TEX1ALPHA
            /* LAST of the alpha writers, deliberately. Every block above
             * decides what the alpha WOULD be with one texture; this one
             * applies the second texture's factor to whatever they produced,
             * which is what the GX combiner chain does (stage 1 multiplies
             * stage 0's result). Putting it earlier would let the TEV-constant
             * alpha overwrite it.
             *
             * Skipped on punch-through batches for the same reason the
             * TEV-constant alpha is: the PT comparator has already been given
             * the texel alpha alone on purpose, and reintroducing a product in
             * front of the 144 threshold re-deletes cutouts. */
            if (tex1_prof
#ifndef DC_PVR_NO_PUNCHTHRU
                && !s_pt_route
#endif
               ) {
                float u1, v1;
                unsigned int a1, a0;
                apply_texgen_n(v, 1, &u1, &v1);
                a1 = tex1_alpha_sample(tex1_prof, tex1_wrap_s, tex1_wrap_t,
                                       u1, v1);
                a0 = (cv[k].argb >> 24) & 0xFFu;
                /* (a * b + 127) / 255, the exact 8-bit product, as a multiply
                 * and two shifts — the same form dc_q() uses in the texture
                 * decoder and for the same reason: SH-4 at this call rate must
                 * not take a libgcc divide. */
                a0 = (a0 * a1 + 127u);
                a0 = (a0 + (a0 >> 8)) >> 8;
                cv[k].argb = (cv[k].argb & 0x00FFFFFFu) | (a0 << 24);
            }
#endif
#ifndef DC_PVR_NO_VTXMEMO
            /* Published only here, at the very bottom of the k-loop, so the
             * cached ClipVtx is the FINAL one — after the PT vertex-alpha
             * override and after both TEV-constant overrides. Storing it any
             * earlier would memoise a half-built vertex. */
            s_vmemo_src[slot] = i + k;
            s_vmemo_tag[slot] = s_vmemo_gen;
            VMEMO_VAL(slot) = cv[k];
#ifdef DC_GX_VTXID
            /* An unarmed vertex publishes NONE, so a later id lookup landing
             * on this slot cannot mistake it for its own; the armed case
             * publishes the stamp it was found by. s_vmemo_src stays written
             * on BOTH paths — the hash path still needs it, and the VERIFY
             * gate reads it to content-check an id hit. */
            s_vmemo_vid[slot] = (unsigned short)v->vtxid;
#endif
#endif
            VS_MARK(VS_POST);
        }

        if (per_prim == 3) {
            s_tris_in++;
            emit_triangle(&cv[0], &cv[1], &cv[2]);
        } else {
            /* A GX quad is a loop v0 v1 v2 v3. Two triangles, same winding. */
            s_tris_in += 2;
            emit_triangle(&cv[0], &cv[1], &cv[2]);
            emit_triangle(&cv[0], &cv[2], &cv[3]);
        }
        VS_MARK(VS_EMIT);
        VS_SAMPLE_END;
    }

#ifdef DC_PVR_BATCH_LOG
    if (s_batch_log_now) {
        /* `src=` is the SOURCE pointer the GX layer was handed, which is the
         * same quantity DC_ASSET_CENSUS resolves to a symbol name
         * (tools/dcstub/census_resolve.py). Without it a batch log line can
         * be joined to a screen region but never to a source symbol, and
         * every texture investigation so far has had to guess which batch is
         * which from its bbox. `vram=` joins the same line to DC_TEX_LOG.
         * Called out as a tooling gap in kb/station-bugs.md §2. */
        DC_LOG("BATCH b=%u %s n=%d verts=%d tex=%d src=%p vram=%p %dx%d fmt=0x%X a=%d "
               "us=%.3f wrap=%d,%d bm=%d,%d,%d cull=%d zt=%d zf=%d zw=%d "
               "chans=%d argb=%08X ac=%d/%d,%d/%d cut=%d pt=%d cu=%d,%d tm=%d,%d "
               "st=%d t1=%d bbox=%.1f,%.1f..%.1f,%.1f z=%.5f..%.5f "
               "uv=%.2f,%.2f..%.2f,%.2f\n",
               s_batches, (per_prim == 4) ? "QUAD" : "TRI", count, s_bl_n,
               tex ? 1 : 0,
               (void*)(uintptr_t)g_gx.tex_obj_src[0],
               tex ? tex->base : (void*)0,
               tex ? (int)tex->w : 0, tex ? (int)tex->h : 0,
               tex ? (unsigned)tex->pvr_fmt : 0u,
               tex ? (int)tex->has_alpha : 0,
               tex ? (double)tex->u_scale : 0.0,
               g_gx.tex_obj_wrap_s[0], g_gx.tex_obj_wrap_t[0],
               g_gx.blend_mode, g_gx.blend_src, g_gx.blend_dst,
               g_gx.cull_mode, g_gx.z_compare_enable, g_gx.z_compare_func,
               g_gx.z_update_enable, g_gx.num_chans, s_bl_argb,
               /* ac = the alpha compare the game asked for, cut = whether we
                * treated it as a cutout, pt = whether it went to the
                * punch-through list (cut=1 pt=0 is a cutout the PT router
                * declined — a blended cutout, or a colour-masked pass, or a
                * -DDC_PVR_NO_PUNCHTHRU build), cu = colour/alpha update (a
                * `0,1` is a pass that GX would have made invisible), tm =
                * stage 0/1 texmap (0xFF is GX_TEXMAP_NULL), st = TEV stage
                * count (>1 means the combine is being collapsed to stage 0). */
               g_gx.alpha_comp0, g_gx.alpha_ref0,
               g_gx.alpha_comp1, g_gx.alpha_ref1, alpha_test_active(),
               pt_route_active(),
               g_gx.color_update_enable, g_gx.alpha_update_enable,
               g_gx.tev_stages[0].tex_map, g_gx.tev_stages[1].tex_map,
               g_gx.num_tev_stages,
               /* t1: is texmap1's bound texture a DIFFERENT image from
                * texmap0's? 52 % of batches request two texmaps, but the PVR
                * has one texture unit and this backend binds tex_handle[0]
                * only. If t1=0 the second bind is the same tile (N64 2-cycle
                * LOD interpolation, which the PVR does in hardware anyway and
                * which is therefore free to drop); if t1=1 a genuinely second
                * image is being discarded and the material really is losing a
                * layer. This single bit decides whether multi-texture is worth
                * a two-pass implementation. */
               (g_gx.tex_handle[1] != 0 &&
                g_gx.tex_handle[1] != g_gx.tex_handle[0]) ? 1 : 0,
               (double)s_bl_x0, (double)s_bl_y0, (double)s_bl_x1,
               (double)s_bl_y1, (double)s_bl_z0, (double)s_bl_z1,
               (double)s_bl_u0, (double)s_bl_v0, (double)s_bl_u1,
               (double)s_bl_v1);
    }
#endif

    s_batches++;
#ifndef DC_PVR_NO_PUNCHTHRU
    s_pt_route = 0;
#endif
}

void dc_gx_backend_set_viewport(int x, int y, int w, int h,
                                float nearz, float farz) {
    (void)nearz; (void)farz;   /* folded into 1/w; the PVR has no depth range */
    if (w <= 0 || h <= 0) return;
    s_vp_hw = (float)w * 0.5f;
    s_vp_hh = (float)h * 0.5f;
    s_vp_cx = (float)x + s_vp_hw;
    s_vp_cy = (float)y + s_vp_hh;
}

void dc_gx_backend_set_scissor(int x, int y, int w, int h) {
    /* TODO(M3): PVR user clipping is per-tile and 32-pixel granular
     * (PVR_USERCLIP_INSIDE + a clip header per list). Ignoring it draws
     * outside the intended rect, which is visible and correctable; emulating
     * it wrongly would silently delete geometry. */
    (void)x; (void)y; (void)w; (void)h;
}

void dc_pvr_report(void) {
    DC_LOGE("[DC/PVR] frames=%u batches=%u tris in=%u out=%u clipped=%u "
            "dropped=%u unsupported_prims=%u\n",
            s_frames, s_batches, s_tris_in, s_tris_out, s_tris_clipped,
            s_tris_dropped, s_prim_unsupported);
#if defined(DC_PVR_VTXSPLIT) && (DC_PVR_VTXSPLIT) > 0
    /* G5. Free-running totals scaled back up by the sample rate, then divided
     * by frames — so this is **ms per PRESENTED frame**, the same denominator
     * as [PHASE] and [GXSPLIT], and it should sum to roughly [PHASE] xform=.
     *
     * A shortfall against xform= is not a bucket being interesting: it is
     * per-primitive loop overhead that no bracket covers, and it is printed as
     * `sum=` so the reader does not have to do that subtraction to find out. */
    {
        int b;
        double scale = (double)(DC_PVR_VTXSPLIT);
        double fr = s_frames ? (double)s_frames : 1.0;
        double sum = 0.0;
        char line[256];
        int n = 0;
        for (b = 0; b < VS_NBUCKET; b++) {
            double ms = (double)s_vs_acc[b] * (double)VS_TICK_NS * scale
                        / 1e6 / fr;
            sum += ms;
            n += snprintf(line + n, sizeof(line) - (size_t)n, "%s=%.2f ",
                          s_vs_name[b], ms);
            if (n < 0 || (size_t)n >= sizeof(line)) break;
        }
        DC_LOGE("[VTXSPLIT] %s| sum=%.2f prims=%u samp=%u memohit=%u "
                "drops=%u 1in%u\n",
                line, sum, s_vs_prims, s_vs_samples, s_vs_hits, s_vs_drops,
                (unsigned int)(DC_PVR_VTXSPLIT));
    }
#endif
#ifndef DC_PVR_NO_DR
    /* G-C's falsification counter. `dr=` must be within a rounding of
     * tris out*3 minus the punch-through verts; a 0 here on a run that drew
     * anything means the DR arm never ran and any timing change is something
     * else. See emit_projected(). */
    DC_LOGE("[DC/PVR] dr verts=%u of %u emitted (pt takes the rest)\n",
            s_dr_verts, s_tris_out * 3u);
#endif
#if defined(DC_PVR_SHADE_ALPHA8) && !defined(DC_PVR_NO_LIGHTING) && \
    !defined(DC_PVR_NO_SHADEFAST)
    DC_LOGE("[DC/PVR] shade_a8 verts=%u\n", s_shade_a8);
#endif
#if defined(DC_GX_VTXID) && defined(DC_GX_VTXID_VERIFY)
    DC_LOGE("[DC/PVR] vtxid vidchk=%u vidbad=%u\n", s_vid_checked, s_vid_bad);
#endif
#ifndef DC_PVR_NO_TEX1ALPHA
    DC_LOGE("[DC/PVR] tex1alpha batches=%u of %u\n", s_tex1_batches, s_batches);
#endif
#if !defined(DC_PVR_NO_TEVCONST) && !defined(DC_PVR_NO_TEVCONST_ALPHA) && \
    !defined(DC_PVR_NO_TEVALPHA_LAST)
    /* How many batches had a constant recovered from their LAST TEV stage. 0
     * means the shape stopped matching (a dc_gx.c recording change, or emu64
     * taking a different combine path) and a screenshot change is NOT this. */
    DC_LOGE("[DC/PVR] tevalpha_last batches=%u of %u\n",
            s_tevalpha_last_batches, s_batches);
#endif
#ifndef DC_PVR_NO_PUNCHTHRU
    /* Emitted next to [PERF] (dc_vi.c calls this straight after it, every 30
     * presented frames). ptdrop MUST be 0: any other value is geometry that
     * was transformed and then refused by the record buffer, i.e. cutouts
     * missing from the screen, and the fix is -DDC_PVR_PT_BUF_RECS. pthi is
     * the worst single frame's record count against the cap, which is the
     * number to size the buffer from rather than from this comment. */
    DC_LOGE("[DC/PVR] pt batches=%u verts=%u recs=%u pthi=%u/%d ptdrop=%u "
            "ref=%d buf=%u B\n",
            s_pt_batches, s_pt_verts, s_pt_recs, s_pt_hi,
            (int)DC_PVR_PT_BUF_RECS, s_pt_drop, (int)DC_PVR_PT_ALPHA_REF,
            (unsigned int)sizeof(s_pt_buf));
#endif
#ifdef DC_PVR_ALPHAENV
    /* The runtime answer to alpha_env_texel_only()'s static 78 %: how many
     * textured batches actually asked for alpha = TEXEL0.a and so got
     * PVR_TXRENV_MODULATE instead of MODULATEALPHA. A drop to 0 means the
     * predicate stopped matching (a dc_gx.c recording change); it is not
     * expected to move otherwise. */
    DC_LOGE("[DC/PVR] alphaenv texel_only=%u of %u batches\n",
            s_env_texel_batches, s_batches);
#endif
#ifdef DC_PVR_TEVP3
    /* batches= is the falsification counter: if it is 0 on a run that reached
     * the name-entry keyboard, the predicate never matched and the diagnosis in
     * kb/tev-map-hard-cases.md §6.6 is wrong — no screenshot needed to know it.
     * clamped= is how often PRIM < ENV forced the base to 0 (tev_p3_affine),
     * i.e. how often this fix is approximate rather than exact. */
    DC_LOGE("[DC/PVR] tevp3 batches=%u clamped=%u of %u\n",
            s_p3_batches, s_p3_clamped, s_batches);
#endif
    dc_pvr_texture_report();
}

/* ==========================================================================
 * Framebuffer probe — how this port is checked WITHOUT a human at the screen
 * ==========================================================================
 * Host-side screen capture is blocked on the dev machine (harness/dc/
 * screenshot.sh explains why: Screen Recording / Accessibility TCC are not
 * granted, so Flycast's own F12 path cannot be driven unattended), and
 * watching a window by hand is not a test anyway — a stalled emulator and a
 * correctly-rendered black frame look identical.
 *
 * So the guest reports its own framebuffer. harness/dc/screenshot.sh already
 * speaks this protocol; nothing in the game build emitted it until now:
 *
 *   MARK:FRAME <n>
 *   FBSRC <which surface> addr=<where>
 *   FBNONZERO <lit pixels> of 307200
 *   FBHASH <fnv1a-32 over the whole front buffer>
 *   FBTHUMB 16x12 <base64 RGB565>
 *
 * FBNONZERO is the assertion; the hash is only the regression golden
 * (byte-identical across runs, so a hex string can be checked in) and cannot
 * on its own tell "black" from "reading the wrong surface" — kb/traps.md. The
 * 16x12 thumbnail is the part a human — or I — can actually read: enough to
 * tell "black", "sky above grass" and "logo on dark" apart, for ~256 chars
 * instead of a 614 KB dump.
 *
 * Reading 614,400 B back out of VRAM is slow, which is exactly why this is
 * gated and periodic rather than per-frame. DC_FB_PROBE is the interval in
 * frames; 0 (the default) compiles it out.
 *
 * ==========================================================================
 * THE ANSWER, MEASURED 2026-08-02 — READ THIS BEFORE DEBUGGING A BLACK PROBE
 * ==========================================================================
 * This probe reported FBNONZERO 0 for two sessions while a human could see
 * the title logo, and three wrong explanations were tried before the right
 * one. All of them were addressing theories, and all of them were wrong:
 *
 *   - "nothing is drawn"              — the renderer census said otherwise
 *   - "vram_s is the wrong offset"    — true but not sufficient; the display
 *                                       scans out from PVR_FB_R_SOF1, and
 *                                       reading there was still 0
 *   - "the SOF registers are in the
 *      64-bit aperture's terms"       — no. KOS's own vid_set_start() writes
 *                                       FB_R_SOF1 and sets vram_s to
 *                                       0xa5000000|base from the same value,
 *                                       so A5 + SOF1 was right all along
 *
 * The answer is that **Flycast's hardware renderer never writes the rendered
 * frame back into emulated VRAM**, and the guest was reading real, writable,
 * genuinely empty memory. Two measurements settle it and both are produced by
 * this function in a single run:
 *
 *   FBWTEST — writes a pattern into each candidate aperture and reads it
 *             back. All four SOF-derived apertures returned writable=1, so
 *             "the probe is reading a hole" is dead.
 *   FBMAP32 / FBMAP64 — 8 MB of VRAM as one density character per 64 KB.
 *             Without --fb-writeback the only nonzero blocks in either
 *             aperture are the guest's own texture uploads; the ten blocks
 *             the framebuffer occupies are empty. With --fb-writeback those
 *             ten blocks light up and A5+SOF1 reads 13,711 of 307,200.
 *
 * So `harness/dc/smoke.sh --fb-writeback` is REQUIRED for this probe, not an
 * optimisation. It costs frame rate (Flycast has to emulate the framebuffer),
 * which is why it stays opt-in rather than being on for every run.
 */
#ifndef DC_FB_PROBE
#define DC_FB_PROBE 0
#endif

#if DC_FB_PROBE > 0
static const char s_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* The PVR's memory-mapped register block. Only the display/framebuffer
 * registers are used here; KOS does not expose them all. */
#define DC_PVR_REG(off) (*(volatile unsigned int*)(0xA05F8000u + (off)))
#define DC_FB_R_CTRL       DC_PVR_REG(0x044)  /* pixel depth, enable, vclk    */
#define DC_FB_W_CTRL       DC_PVR_REG(0x048)  /* format the TA renders into   */
#define DC_FB_W_LINESTRIDE DC_PVR_REG(0x04C)  /* render-target stride / 8     */
#define DC_FB_R_SOF1       DC_PVR_REG(0x050)  /* scanout base, odd field      */
#define DC_FB_R_SOF2       DC_PVR_REG(0x054)  /* scanout base, even field     */
#define DC_FB_R_SIZE       DC_PVR_REG(0x05C)  /* x words-1 | y lines-1 | mod  */
#define DC_FB_W_SOF1       DC_PVR_REG(0x060)  /* where the PVR WRITES, field 1*/
#define DC_FB_W_SOF2       DC_PVR_REG(0x064)

/* The two windows onto the same 8 MB of VRAM. 0xA5000000 is the 32-bit
 * sequential area (bank 0 then bank 1); 0xA4000000 is the 64-bit
 * bank-interleaved area. Which one the SOF registers are expressed in is
 * exactly what this probe is here to settle, so both are tried. */
#define DC_VRAM32 0xA5000000u
#define DC_VRAM64 0xA4000000u
#define DC_VRAM_BYTES (8u * 1024u * 1024u)

#define DC_FB_PIXELS ((unsigned int)(DC_SCREEN_WIDTH * DC_SCREEN_HEIGHT))
#define DC_FB_BYTES  (DC_FB_PIXELS * 2u)

/* Return `window + off` only if a whole 640x480x16 frame fits inside VRAM
 * from there. A candidate that would read off the end of the aperture is not
 * a candidate; reading it would fault or alias and neither is informative. */
static const unsigned short* dc_fb_at(unsigned int window, unsigned int off) {
    off &= ~3u;
    if (off > DC_VRAM_BYTES || off + DC_FB_BYTES > DC_VRAM_BYTES) return 0;
    return (const unsigned short*)(window + off);
}

/* Same guard for a pointer KOS handed us rather than one we computed. A bogus
 * pvr_get_front_buffer() must be reported as out-of-range, not dereferenced —
 * an unhandled TLB miss here would look like a renderer crash. */
static const unsigned short* dc_fb_ptr(const void* p) {
    unsigned int a = (unsigned int)(unsigned long)p;
    if ((a & 0xFF000000u) == DC_VRAM32) return dc_fb_at(DC_VRAM32, a - DC_VRAM32);
    if ((a & 0xFF000000u) == DC_VRAM64) return dc_fb_at(DC_VRAM64, a - DC_VRAM64);
    return 0;
}

/* One pass over a candidate surface: FNV-1a over every 16-bit word (stride 1
 * — sampling would make the hash blind to exactly the small sprites this is
 * meant to catch) plus the nonzero-pixel population count.
 *
 * The count is the assertion and the hash is only the golden. kb/traps.md:
 * "a framebuffer HASH is not a framebuffer TEST". */
static unsigned int dc_fb_scan(const unsigned short* fb, unsigned int* hash_out) {
    unsigned int hash = 2166136261u;
    unsigned int nonzero = 0;
    unsigned int i;

    for (i = 0; i < DC_FB_PIXELS; i++) {
        unsigned short px = fb[i];
        hash = (hash ^ (px & 0xFF)) * 16777619u;
        hash = (hash ^ (px >> 8)) * 16777619u;
        if (px) nonzero++;
    }
    *hash_out = hash;
    return nonzero;
}

/* 8 MB of VRAM as 128 characters, one per 64 KB block, through ONE window.
 *
 * The previous sweep printed only a hot/not count and the first four hot
 * block indices, and that was not enough to act on: a framebuffer is a *run*
 * of ~10 consecutive DENSE blocks (614,400 B / 65,536), while an uploaded
 * texture is one or two blocks that are mostly zero. A density character per
 * block separates those by eye in one line:
 *
 *   '.' = every word zero      '0' = nonzero but under 10% of words
 *   '1'..'9' = 10%..90% dense  '#' = every word nonzero
 *
 * So the framebuffer, wherever it is and whichever window it is visible
 * through, shows up as a contiguous smear of digits. */
static void dc_fb_map(const char* tag, unsigned int window) {
    const unsigned int* vram = (const unsigned int*)window;
    const unsigned int block_words = 65536u / 4u;
    const unsigned int blocks = DC_VRAM_BYTES / 65536u;
    char map[129];
    unsigned int b, w, hot = 0;

    for (b = 0; b < blocks; b++) {
        const unsigned int* p = vram + b * block_words;
        unsigned int n = 0;
        for (w = 0; w < block_words; w++)
            if (p[w]) n++;
        if (n) hot++;
        map[b] = (n == 0)              ? '.'
               : (n >= block_words)    ? '#'
               : (char)('0' + (n * 10u / block_words));
    }
    map[blocks] = '\0';
    DC_LOGE("FBMAP%s hot=%u/%u %s\n", tag, hot, blocks, map);
}

/* Is this address real, writable VRAM at all?
 *
 * Without this, a zero read has two causes that need opposite fixes — "the
 * probe is pointed at the wrong place" and "the place is right but the
 * emulator never writes the rendered frame back into it" — and no amount of
 * re-reading distinguishes them. Writing a pattern and reading it back does:
 * a successful round-trip proves the aperture is live memory, so a zero read
 * there is the emulator's silence, not our arithmetic.
 *
 * Eight pixels in the top-left corner, restored immediately. If the surface
 * is the one being scanned out this is a single-frame red speck in the
 * corner, which is harmless and, on a human-watched run, is itself a
 * confirmation that the address is the visible one. */
static int dc_fb_write_test(const unsigned short* fb) {
    volatile unsigned short* p = (volatile unsigned short*)fb;
    unsigned short save[8];
    int k, ok = 1;

    for (k = 0; k < 8; k++) save[k] = p[k];
    for (k = 0; k < 8; k++) p[k] = (unsigned short)(0xF800u | (unsigned)k);
    for (k = 0; k < 8; k++)
        if (p[k] != (unsigned short)(0xF800u | (unsigned)k)) ok = 0;
    for (k = 0; k < 8; k++) p[k] = save[k];
    return ok;
}

/* --------------------------------------------------------------------------
 * DC_FB_IMAGE=<1|2|4> — dump the WHOLE frame, not a 16x12 thumbnail.
 *
 * WHY THIS EXISTS. The 16x12 thumbnail can tell "black" from "not black" and
 * nothing else. Every rendering question past that — is this model inside-out,
 * is this texture the right one, is the sky where the ground should be — has
 * been answered by a human watching Flycast and typing what they saw, which is
 * slow, serialises on that human, and cannot be diffed between two builds.
 * Host-side capture is blocked by macOS TCC (harness/dc/screenshot.sh
 * documents both dead ends), so the guest has to send the picture out itself.
 *
 * Streamed ROW BY ROW on purpose: a 640x480 RGB565 buffer plus its base64 is
 * ~1.4 MB, and holding either in .bss would put a debug knob on the critical
 * RAM budget. One output row of accumulators is 640 * 3 * 4 B of stack and
 * nothing is retained between rows.
 *
 * Protocol (tools/dcfb/fbimg_to_png.py turns it into a PNG):
 *     FBIMG BEGIN <w> <h> rgb565 frame=<n>
 *     FBROW <y> <base64 of w RGB565 pixels, big-endian>
 *     FBIMG END
 *
 * The factor is a BOX filter, not point sampling — same reason the thumbnail
 * box-filters (kb/traps.md). Off by default: at factor 1 this is ~820 KB of
 * console per probe, so pair it with a large DC_FB_PROBE.
 * -------------------------------------------------------------------------- */
#if defined(DC_FB_PROBE) && defined(DC_FB_IMAGE)
static void dc_pvr_fb_dump_image(const unsigned short* fb) {
    static unsigned int image_no = 0;
    static const char b64[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const int f = (DC_FB_IMAGE) < 1 ? 1 : (DC_FB_IMAGE);
    const int w = DC_SCREEN_WIDTH / f;
    const int h = DC_SCREEN_HEIGHT / f;
    unsigned int acc_r[DC_SCREEN_WIDTH], acc_g[DC_SCREEN_WIDTH],
                 acc_b[DC_SCREEN_WIDTH];
    unsigned char row[DC_SCREEN_WIDTH * 2];
    char out[((DC_SCREEN_WIDTH * 2 + 2) / 3) * 4 + 1];
    int cy, cx, sy, sx, i, o;
    unsigned int npx = (unsigned int)(f * f);

    DC_LOGE("FBIMG BEGIN %d %d rgb565 frame=%u\n", w, h, image_no++);

    for (cy = 0; cy < h; cy++) {
        for (cx = 0; cx < w; cx++) { acc_r[cx] = acc_g[cx] = acc_b[cx] = 0; }

        for (sy = cy * f; sy < (cy + 1) * f; sy++) {
            const unsigned short* src = fb + (unsigned int)sy * DC_SCREEN_WIDTH;
            for (cx = 0; cx < w; cx++) {
                for (sx = cx * f; sx < (cx + 1) * f; sx++) {
                    unsigned short px = src[sx];
                    acc_r[cx] += (px >> 11) & 0x1F;
                    acc_g[cx] += (px >> 5) & 0x3F;
                    acc_b[cx] += px & 0x1F;
                }
            }
        }

        for (cx = 0; cx < w; cx++) {
            unsigned int px = ((acc_r[cx] / npx) << 11) |
                              ((acc_g[cx] / npx) << 5) |
                               (acc_b[cx] / npx);
            row[cx * 2]     = (unsigned char)(px >> 8);
            row[cx * 2 + 1] = (unsigned char)(px & 0xFF);
        }

        for (i = 0, o = 0; i < w * 2; i += 3) {
            unsigned int v = (unsigned int)row[i] << 16;
            if (i + 1 < w * 2) v |= (unsigned int)row[i + 1] << 8;
            if (i + 2 < w * 2) v |= (unsigned int)row[i + 2];
            out[o++] = b64[(v >> 18) & 0x3F];
            out[o++] = b64[(v >> 12) & 0x3F];
            out[o++] = (i + 1 < w * 2) ? b64[(v >> 6) & 0x3F] : '=';
            out[o++] = (i + 2 < w * 2) ? b64[v & 0x3F] : '=';
        }
        out[o] = '\0';
        DC_LOGE("FBROW %d %s\n", cy, out);
    }

    DC_LOGE("FBIMG END\n");
}

/* Dump an arbitrary linear RGB565 640x480 surface, for callers that have one
 * and are not the PVR. The only one is dc_main.c's splash, which runs BEFORE
 * pvr_init() and therefore owns vram_s outright — dc_pvr_fb_probe() cannot help
 * it, because that reads PVR_FB_R_SOF1 and the display controller has not been
 * reprogrammed yet. Without this the splash is the one thing in the port that
 * could not be checked with a screenshot. */
void dc_pvr_fb_dump_surface(const unsigned short* fb) {
    dc_pvr_fb_dump_image(fb);
}
#else
#define dc_pvr_fb_dump_image(fb) ((void)(fb))
void dc_pvr_fb_dump_surface(const unsigned short* fb) { (void)fb; }
#endif

void dc_pvr_fb_probe(void) {
    static unsigned int probe_no = 0;
    /* Once a surface has produced a nonzero frame the aperture question is
     * answered, and re-answering it costs ten 614,400 B scans plus two 8 MB
     * sweeps per probe. So the full diagnostic runs only while the answer is
     * unknown — the first probe, and any later probe whose surface has gone
     * black again (which is itself the signal that something moved). */
    static int s_settled = 0;
    static int s_src = 0;
    int full;
    /* Candidate surfaces, in the order they are reported. Ten of them because
     * ten hypotheses were live at once and a run costs a build plus two
     * minutes of a machine whose window steals focus: scanning them all costs
     * ten passes over 614,400 B and settles in ONE run which — if any — the
     * guest can actually read. See kb/STATE.md 2026-08-02. */
    enum { DC_FB_NCAND = 10 };
    static const char* const cand_name[DC_FB_NCAND] = {
        "A5+R  ", "A4+R  ", "A4+2R ", "A5+2R ", "A5+W  ",
        "A4+2W ", "A5+0  ", "A4+0  ", "KOSFRNT", "KOSBACK"
    };
    const unsigned short* cand[DC_FB_NCAND];
    unsigned int cand_nz[DC_FB_NCAND];
    unsigned int cand_hash[DC_FB_NCAND];
    unsigned int rsof1, rsof2, wsof1;
    int best = -1;
    const unsigned short* fb;
    unsigned int hash = 0;
    unsigned short thumb[16 * 12];
    unsigned char raw[16 * 12 * 2];
    char out[((16 * 12 * 2 + 2) / 3) * 4 + 1];
    /* Box-filter accumulators, one per thumbnail cell. Point sampling was
     * wrong: the title logo covers a few per cent of a 640x480 frame, so a
     * 16x12 grid of single pixels reported an all-black thumbnail off a frame
     * whose hash proved it was not black. Averaging the whole cell cannot miss
     * content that a single sample steps over. */
    unsigned int acc_r[16 * 12], acc_g[16 * 12], acc_b[16 * 12];
    unsigned int cell_px;
    unsigned int nonzero = 0;
    int x, y, i, o;

    if (!dc_pvr_ready) return;

    /* ASK THE HARDWARE WHERE IT IS SCANNING OUT FROM. Do not assume.
     *
     * MEASURED 2026-08-02. `vram_s` is the base of VRAM and is NOT the
     * displayed surface once pvr_init() has run: the PVR allocates its own
     * buffers inside VRAM and programs the display controller at them.
     * PVR_FB_R_SOF1 read 0x000E7480 — 947,840 bytes in — and page-flips to
     * 0x004E7480 and back, while this probe was reading offset 0. Reading
     * SOF1 was necessary; it was not sufficient (see the header block:
     * Flycast also has to be told to write frames back at all).
     *
     * SOF1 is the odd-field / progressive base; SOF2 is one line further on,
     * so both fields live in the same 640x480 buffer. FB_W_SOF1 is a separate
     * register: it is where the PVR *renders*, which need not be the surface
     * being displayed this instant, and is the other place a finished frame
     * can be found. Both are reported.
     *
     * WHY THERE IS A `2 *` HERE. Read from the KOS 2.3 source in the SDK
     * image, not guessed:
     *
     *   pvr_misc.c:pvr_sync_view()  -> vid_set_start(frame_buffers[i].frame)
     *   video.c:vid_set_start(base) -> PVR_SET(PVR_FB_ADDR, base);
     *                                  vram_s = (uint16_t*)(PVR_RAM_BASE|base)
     *
     * so FB_R_SOF1 is a byte offset in the 32-bit *sequential* aperture
     * (PVR_RAM_BASE = 0xa5000000), and `A5 + SOF1` is the textbook-correct
     * read. But pvr_misc.c:pvr_get_frame_buffer() returns
     *
     *   (pvr_ptr_t)(addr * 2 + PVR_RAM_BASE)
     *
     * with the comment "convert its address to make it addressable from the
     * 64-bit memory" — the doubling is the 32-bit-offset -> 64-bit-offset
     * conversion for bank 0, but the base added is the 32-BIT one. Either KOS
     * has an off-by-one-aperture bug there or the doubling means something
     * else; both readings are cheap to test, so both `A4 + 2*SOF1` (the
     * doubling applied to the interleaved aperture, which is what the comment
     * describes) and `A5 + 2*SOF1` (verbatim what KOS returns) are candidates.
     *
     * VERDICT, 2026-08-02: neither doubled candidate ever carried a frame, and
     * once the display's own second buffer is in use (SOF1 = 0x004E7480) both
     * of them fall off the end of the 8 MB aperture entirely — which is itself
     * evidence that pvr_get_front_buffer() cannot be trusted as a framebuffer
     * address. `A5 + SOF1` is the correct read and is what FBSRC reports. The
     * candidates are kept because they cost one pass each on the first probe
     * only, and because "we checked" is worth more in a log than in a
     * comment. */
    rsof1 = DC_FB_R_SOF1;
    rsof2 = DC_FB_R_SOF2;
    wsof1 = DC_FB_W_SOF1;

    cand[0] = dc_fb_at(DC_VRAM32, rsof1);
    cand[1] = dc_fb_at(DC_VRAM64, rsof1);
    cand[2] = dc_fb_at(DC_VRAM64, rsof1 * 2u);
    cand[3] = dc_fb_at(DC_VRAM32, rsof1 * 2u);
    cand[4] = dc_fb_at(DC_VRAM32, wsof1);
    cand[5] = dc_fb_at(DC_VRAM64, wsof1 * 2u);
    cand[6] = dc_fb_at(DC_VRAM32, 0);
    cand[7] = dc_fb_at(DC_VRAM64, 0);
    cand[8] = dc_fb_ptr(pvr_get_front_buffer());
    cand[9] = dc_fb_ptr(pvr_get_back_buffer());

    for (i = 0; i < DC_FB_NCAND; i++) { cand_nz[i] = 0; cand_hash[i] = 0; }

    /* Bounded, not "until it works". MEASURED: a run without
     * `--fb-writeback` never settles, and an unbounded full diagnostic then
     * costs ten 614,400 B scans plus two 8 MB sweeps on EVERY probe forever —
     * which halves the frame rate of the very run whose frame rate is being
     * reported, and buries the console in identical maps. Three probes is
     * enough to see the aperture picture twice and watch it not change. */
    full = !s_settled && probe_no < 3u;

    if (!s_settled && probe_no == 3u)
        DC_LOGE("FBPROBE giving up on the aperture sweep: no surface has "
                "produced a pixel in 3 probes, and every candidate was "
                "writable. That is Flycast not writing frames back to VRAM, "
                "not an address — re-run with `smoke.sh --fb-writeback`.\n");

    if (!full) {
        /* Settled: one scan of the surface that has been producing pixels.
         * The index is what is remembered, not the address, because the two
         * page-flipped buffers swap every frame and cand[] is rebuilt from
         * the live registers each time. */
        best = s_src;
        if (!cand[best]) { full = 1; s_settled = 0; }
        else cand_nz[best] = dc_fb_scan(cand[best], &cand_hash[best]);
    }

    if (full) {
        /* Registers first, raw. Decoding them host-side beats guessing at
         * their meaning in guest code: FB_R_CTRL bits 3:2 are the display
         * depth (1 = RGB565) and FB_R_SIZE packs x words-1 | y lines-1 |
         * modulus, so a frame that is not 640x480x16 is visible in the log
         * rather than silently mis-strided by this probe.
         *
         * MEASURED 2026-08-02: rctl=00000005 (enabled, RGB565),
         * rsize=1413bd3f (x=320 32-bit words = 640 px, y=239+1 = 240 lines
         * per field, modulus 321 = one line skipped between them, i.e. an
         * interlaced 640x480), and rsof1 alternating 000e7480 / 004e7480 as
         * KOS page-flips. So the assumed 640x480x16 geometry is correct and
         * this probe's flat 640*480 read of SOF1 sees both fields. */
        DC_LOGE("FBREGS rctl=%08x rsize=%08x rsof1=%08x rsof2=%08x "
                "wctl=%08x wstride=%08x wsof1=%08x wsof2=%08x "
                "kosfront=%08x kosback=%08x\n",
                DC_FB_R_CTRL, DC_FB_R_SIZE, rsof1, rsof2,
                DC_FB_W_CTRL, DC_FB_W_LINESTRIDE, wsof1, DC_FB_W_SOF2,
                (unsigned int)(unsigned long)pvr_get_front_buffer(),
                (unsigned int)(unsigned long)pvr_get_back_buffer());

        for (i = 0; i < DC_FB_NCAND; i++) {
            int dup = -1;
            if (!cand[i]) {
                DC_LOGE("FBCAND %s addr=--------  (out of range)\n",
                        cand_name[i]);
                continue;
            }
            for (o = 0; o < i; o++)
                if (cand[o] == cand[i]) { dup = o; break; }
            if (dup >= 0) {
                cand_nz[i] = cand_nz[dup];
                cand_hash[i] = cand_hash[dup];
            } else {
                cand_nz[i] = dc_fb_scan(cand[i], &cand_hash[i]);
            }
            DC_LOGE("FBCAND %s addr=%08x nz=%u of %u hash=%08x%s\n",
                    cand_name[i], (unsigned int)(unsigned long)cand[i],
                    cand_nz[i], DC_FB_PIXELS, cand_hash[i],
                    dup >= 0 ? " (same as an earlier candidate)" : "");
        }

        /* WHICH SURFACE TO REPORT. Not "whichever scored highest" — that
         * heuristic misfired the first time it ran: on a probe taken before
         * any frame had been written back, A4+0 scored 18 nonzero pixels (a
         * corner of the guest's own texture pool, read as if it were a frame)
         * and beat the real framebuffer's 0, so FBSRC and FBHASH changed
         * surface between probes of the same run and the golden was worthless.
         *
         * A5+R is the surface the display controller is scanning out. It is
         * the answer by definition, and FBWTEST proves it is live memory, so
         * report it unless another candidate is not merely larger but plainly
         * a DIFFERENT SURFACE: at least 1/64 of the frame lit and at least 8x
         * A5+R's count. That bar is far above any texture-pool false positive
         * and far below a real frame (measured: 13,711 of 307,200 at the
         * title screen). */
        best = cand[0] ? 0 : -1;
        for (i = 0; i < DC_FB_NCAND; i++) {
            if (!cand[i]) continue;
            if (best < 0) { best = i; continue; }
            if (cand_nz[i] >= DC_FB_PIXELS / 64u &&
                cand_nz[i] > cand_nz[best] * 8u)
                best = i;
        }

        /* The two block maps. If a run of dense blocks appears in FBMAP64 and
         * not in FBMAP32, the SOF registers are in 64-bit-area terms and the
         * window was the bug; if it appears in FBMAP32 at a block the
         * candidates do not cover, the offset is the bug; if neither map
         * shows a dense run at all, nothing ever wrote a frame into VRAM and
         * the fix is not addressing at all. One run, three verdicts.
         *
         * MEASURED 2026-08-02 — this is what settled N2. Without
         * `--fb-writeback` the ONLY nonzero blocks in all 8 MB, through either
         * aperture, are the guest's own texture uploads (FBMAP32 blocks 0,
         * 12, 14, 23-26, mirrored at +64 because Flycast's 32-bit aperture
         * repeats every 4 MB); the ten blocks the framebuffer occupies are
         * '.' throughout. With `--fb-writeback` blocks 20-23 light up and
         * A5+R reads 13,711 nonzero pixels. So the black frame was never an
         * addressing bug: Flycast's hardware renderer simply does not write
         * the rendered frame back into emulated VRAM unless asked to. */
        dc_fb_map("32", DC_VRAM32);
        dc_fb_map("64", DC_VRAM64);

        /* Round-trip the four SOF-derived apertures. All four came back
         * writable=1, which is what turned "the probe is reading a hole" from
         * a live hypothesis into a dead one. */
        for (i = 0; i < 4; i++)
            if (cand[i])
                DC_LOGE("FBWTEST %s writable=%d\n", cand_name[i],
                        dc_fb_write_test(cand[i]));
    }

    /* Report the winner through the unchanged MARK:FRAME / FBNONZERO /
     * FBHASH / FBTHUMB protocol that harness/dc/screenshot.sh already parses,
     * plus FBSRC so a golden hash is never compared across two different
     * source surfaces. With every candidate at zero this still emits, from
     * A5+R, so "all black" remains a reportable result rather than silence. */
    if (best < 0 || !cand[best]) { s_settled = 0; return; }
    fb = cand[best];
    nonzero = cand_nz[best];
    hash = cand_hash[best];
    /* A surface that produced pixels is the surface; one that has gone black
     * re-opens the question on the next probe rather than reporting a
     * confident zero forever. */
    s_settled = (nonzero > 0);
    s_src = best;

    memset(acc_r, 0, sizeof(acc_r));
    memset(acc_g, 0, sizeof(acc_g));
    memset(acc_b, 0, sizeof(acc_b));

    /* Box filter only — the hash and the population count already came from
     * dc_fb_scan() over the same surface, and recomputing the hash here once
     * cost a second full pass for a number that must agree by construction. */
    for (y = 0; y < DC_SCREEN_HEIGHT; y++) {
        unsigned int cell_row = (unsigned int)(y * 12 / DC_SCREEN_HEIGHT) * 16u;
        for (x = 0; x < DC_SCREEN_WIDTH; x++) {
            unsigned short px = fb[y * DC_SCREEN_WIDTH + x];
            unsigned int c = cell_row + (unsigned int)(x * 16 / DC_SCREEN_WIDTH);
            acc_r[c] += (px >> 11) & 0x1F;
            acc_g[c] += (px >> 5) & 0x3F;
            acc_b[c] += px & 0x1F;
        }
    }

    cell_px = ((unsigned int)DC_SCREEN_WIDTH / 16u) *
              ((unsigned int)DC_SCREEN_HEIGHT / 12u);
    for (i = 0; i < 16 * 12; i++)
        thumb[i] = (unsigned short)(((acc_r[i] / cell_px) << 11) |
                                    ((acc_g[i] / cell_px) << 5) |
                                     (acc_b[i] / cell_px));
    memcpy(raw, thumb, sizeof(raw));

    for (i = 0, o = 0; i < (int)sizeof(raw); i += 3) {
        unsigned int v = (unsigned int)raw[i] << 16;
        if (i + 1 < (int)sizeof(raw)) v |= (unsigned int)raw[i + 1] << 8;
        if (i + 2 < (int)sizeof(raw)) v |= (unsigned int)raw[i + 2];
        out[o++] = s_b64[(v >> 18) & 0x3F];
        out[o++] = s_b64[(v >> 12) & 0x3F];
        out[o++] = (i + 1 < (int)sizeof(raw)) ? s_b64[(v >> 6) & 0x3F] : '=';
        out[o++] = (i + 2 < (int)sizeof(raw)) ? s_b64[v & 0x3F] : '=';
    }
    out[o] = '\0';

    dc_pvr_fb_dump_image(fb);

    DC_LOGE("MARK:FRAME %u\n", probe_no++);
    DC_LOGE("FBSRC %s addr=%08x\n", cand_name[best],
            (unsigned int)(unsigned long)fb);
    /* The count is the part a script can assert on without decoding anything:
     * "0 of 307200" is a black frame, full stop, and any other number means
     * the guest can see its own output. The hash alone cannot distinguish
     * "black" from "the probe is reading the wrong surface". */
    DC_LOGE("FBNONZERO %u of %u\n", nonzero, DC_FB_PIXELS);
    DC_LOGE("FBHASH %08x\n", hash);
    DC_LOGE("FBTHUMB 16x12 %s\n", out);
}
#else
void dc_pvr_fb_probe(void) { }
/* Outside the `#if DC_FB_PROBE > 0` block on purpose: dc_main.c's splash calls
 * this unconditionally, so a build with no framebuffer probe must still link.
 * The inner `#if defined(DC_FB_PROBE) && defined(DC_FB_IMAGE)` further up only
 * chooses between the real dump and a no-op WITHIN a probe build. */
void dc_pvr_fb_dump_surface(const unsigned short* fb) { (void)fb; }
#endif

#else /* !DC_PVR_BACKEND — the original do-nothing seam, kept verbatim. */

void dc_gx_backend_init(void) {
    DC_LOGE("[DC/GX] backend: NONE (stub, DC_PVR_BACKEND=0). Geometry is "
            "accumulated, culled and counted, but nothing is drawn.\n");
}
void dc_gx_backend_shutdown(void) { }
void dc_gx_backend_frame_begin(void) { s_frames++; }
void dc_gx_backend_frame_end(void) { }
void dc_gx_backend_submit(int prim, const DCGXVertex* verts, int count) {
    (void)prim; (void)verts; (void)count;
}
void dc_gx_backend_set_viewport(int x, int y, int w, int h, float nz, float fz) {
    (void)x; (void)y; (void)w; (void)h; (void)nz; (void)fz;
}
void dc_gx_backend_set_scissor(int x, int y, int w, int h) {
    (void)x; (void)y; (void)w; (void)h;
}
void dc_pvr_report(void) {
    DC_LOGE("[DC/PVR] backend compiled out (DC_PVR_BACKEND=0)\n");
}
void dc_pvr_fb_probe(void) { }
void dc_pvr_fb_dump_surface(const unsigned short* fb) { (void)fb; }

#endif /* DC_PVR_BACKEND */
