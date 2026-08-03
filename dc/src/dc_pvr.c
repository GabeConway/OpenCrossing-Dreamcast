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

#if DC_PVR_BACKEND

#include <dc/pvr.h>
/* SH-4 FTRV: dc_gx_backend_submit() folds projection*modelview into ONE 4x4 and
 * then runs a matrix-vector multiply PER VERTEX. At -O0 that is 16 multiplies,
 * 12 adds and ~60 stack round-trips of C; FTRV is one instruction. The matrix
 * is loaded into XMTRX once per BATCH. See the DC_PVR_NO_FTRV block in
 * dc_gx_backend_submit() and kb/perf-dc.md. */
#include <dc/matrix.h>

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

    for (li = 0; li < 8; li++) {
        float dx, dy, dz, d2, d, atten, ndl, w;
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

    /* GX_CA_A0/A1/A2 are 1/2/3 and index tev_colors[] directly: [0] is
     * GX_TEVPREV, [1..3] are GX_TEVREG0..2. emu64 parks the N64 primitive
     * colour in GX_TEVREG1 and the environment colour in GX_TEVREG2
     * (emu64.c:3171,3180), so a PRIMITIVE alpha arg lands on tev_colors[2][3]
     * — the alpha gDPSetPrimColor set. */
    if (konst >= GX_CA_A0 && konst <= GX_CA_A2) {
        *out = g_gx.tev_colors[konst][3];
        return 1;
    }
    if (konst == GX_CA_KONST) {
        int k = ts->k_alpha_sel;   /* GX_TEV_KASEL_K0_A..K3_A are 0x1C..0x1F */
        if (k < 0x1C || k > 0x1F) return 0;
        *out = g_gx.tev_k_colors[k - 0x1C][3];
        return 1;
    }
    return 0;
}
#endif /* !DC_PVR_NO_TEVCONST_ALPHA */
#endif /* !DC_PVR_NO_TEVCONST */

static unsigned int shade_vertex(const DCGXVertex* v, const float* eye,
                                 const float* nrm) {
#ifdef DC_PVR_NO_LIGHTING
    (void)eye; (void)nrm;
    return ((unsigned int)v->color0[3] << 24) | ((unsigned int)v->color0[0] << 16) |
           ((unsigned int)v->color0[1] << 8) | (unsigned int)v->color0[2];
#else
    float rgba[4];

    /* numChans == 0 means the TEV takes no rasterised colour at all. Passing
     * the vertex colour through is wrong in principle but is what makes
     * untextured UI geometry visible instead of black. */
    if (g_gx.num_chans == 0)
        return ((unsigned int)v->color0[3] << 24) |
               ((unsigned int)v->color0[0] << 16) |
               ((unsigned int)v->color0[1] << 8) | (unsigned int)v->color0[2];

#ifndef DC_PVR_NO_SHADEFAST
    /* THE PASS-THROUGH CASE, and it is the common one.
     *
     * With no light enabled on either half of the channel and both material
     * sources set to GX_SRC_VTX, GX's whole channel equation collapses to
     * "the vertex colour", and chan_component() below was spending four
     * float divides, four multiplies, four clamps and four float->int
     * conversions arriving back at the byte it started from. Returning the
     * bytes is EXACT, not an approximation: pack_argb() computes
     * (int)(b/255*255 + 0.5) and the round-trip error is ~1e-5, four orders of
     * magnitude below the 0.5 that would change the answer.
     *
     * This is also what licenses dc_gx_backend_submit() to skip the eye-space
     * position and the normal transform entirely — see `need_light` there. The
     * predicate is duplicated deliberately and the two must not drift: every
     * path below that reads `eye` or `nrm` is guarded by chan_ctrl_enable[]. */
    if (!g_gx.chan_ctrl_enable[0] && !g_gx.chan_ctrl_enable[1] &&
        g_gx.chan_ctrl_mat_src[0] == GX_SRC_VTX &&
        g_gx.chan_ctrl_mat_src[1] == GX_SRC_VTX)
        return ((unsigned int)v->color0[3] << 24) |
               ((unsigned int)v->color0[0] << 16) |
               ((unsigned int)v->color0[1] << 8) | (unsigned int)v->color0[2];
#endif

    rgba[0] = v->color0[0] * (1.0f / 255.0f);
    rgba[1] = v->color0[1] * (1.0f / 255.0f);
    rgba[2] = v->color0[2] * (1.0f / 255.0f);
    rgba[3] = v->color0[3] * (1.0f / 255.0f);

#ifdef DC_PVR_NO_SHADEFAST
    return pack_argb(chan_component(0, 0, rgba, eye, nrm, 0),
                     chan_component(0, 0, rgba, eye, nrm, 1),
                     chan_component(0, 0, rgba, eye, nrm, 2),
                     chan_component(0, 1, rgba, eye, nrm, 3));
#else
    {
        float out[4];
        chan_eval(0, 0, 0, 2, rgba, eye, nrm, out);   /* the colour half */
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
 * ⚠️⚠️ OFF BY DEFAULT. THIS WAS BUILT, RUN, AND MEASURED TO REGRESS.
 * Read the A/B at the bottom of this comment before turning it on. The
 * analysis below is correct as far as it goes; the screenshots are not
 * negotiable.
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
 * ================= WHY IT IS OFF: THE MEASURED A/B, 2026-08-03 =============
 *
 * Two 600 s runs of ONE tree differing only by this define, both reaching the
 * town, screenshots at 320x240 every 400 frames (DC_SCIF_FAST made that
 * affordable). Counters were clean either way — frames 10,349 vs 10,269,
 * ptdrop 0, LOST 0, no new blank textures, FPS within noise. **The counters
 * would have passed this change.** The screenshots did not:
 *
 *   WIN  — the dialogue balloon. With MODULATEALPHA a grey block sits behind
 *          the body text and the "Rover" nameplate is desaturated olive; with
 *          MODULATE the balloon is a clean cream oval and the nameplate is the
 *          correct saturated yellow-green.
 *   LOSS — the train station canopy, and it is much bigger on screen. Correct
 *          (MODULATEALPHA): textured orange-brown wooden beams over the
 *          platform, clock legible behind. With MODULATE: a flat teal-green
 *          slab covering the whole structure.
 *
 * The loss is the larger, more central object, so the default stays as it
 * shipped. NOT diagnosed, and worth knowing before anyone re-opens this: for a
 * GX_BM_NONE batch the framebuffer is RGB565 with src=ONE dst=ZERO, so alpha
 * cannot affect the result at all, and for a punch-through batch the vertex
 * alpha is ALREADY forced to 255 a few hundred lines below — so this switch
 * can only change blended batches and punch-through routing. The canopy draws
 * through `_texture_z_light_fog_prim_npc` (`ac_station_draw.c_inc:61`) =
 * `G_RM_FOG_SHADE_A | G_RM_AA_ZB_TEX_EDGE2`, which should land on the PT path
 * where this is a no-op. It visibly does not. **Something else is being
 * revealed here** — most likely a batch that was invisible only because its
 * vertex alpha was 0 and is now painted at its texel alpha, over the canopy.
 * The next step is one run with -DDC_PVR_ALPHAENV -DDC_PVR_BATCH_LOG=400 at
 * the same probe interval, reading the batch whose bbox covers the slab.
 *
 * Turn on with -DDC_PVR_ALPHAENV. */
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

    if (tex && tex->base)
        pvr_poly_cxt_txr(&cxt, list, (int)tex->pvr_fmt, tex->w, tex->h,
                         (pvr_ptr_t)tex->base, PVR_FILTER_BILINEAR);
    else
        pvr_poly_cxt_col(&cxt, list);

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
} ClipVtx;

static void emit_projected(const ClipVtx* c, unsigned int flags) {
    pvr_vertex_t pv;
    float inv_w = 1.0f / c->w;

    pv.flags = flags;
    pv.x = s_vp_cx + s_vp_hw * (c->x * inv_w);
    pv.y = s_vp_cy - s_vp_hh * (c->y * inv_w);
    pv.z = inv_w;
    pv.u = c->u;
    pv.v = c->v;
    pv.argb = c->argb;
    pv.oargb = 0;
    submit_prim(&pv, sizeof(pv));
#ifndef DC_PVR_NO_PUNCHTHRU
    if (s_pt_route) s_pt_verts++;
#endif

#ifdef DC_PVR_BATCH_LOG
    /* Accumulate what the TA was actually handed, not what we think it was.
     * "Submitted" and "on screen" are different claims and the draw-call
     * counter cannot tell them apart. */
    if (s_batch_log_now) {
        if (!s_bl_n) {
            s_bl_x0 = s_bl_x1 = pv.x;
            s_bl_y0 = s_bl_y1 = pv.y;
            s_bl_z0 = s_bl_z1 = pv.z;
            s_bl_u0 = s_bl_u1 = pv.u;
            s_bl_v0 = s_bl_v1 = pv.v;
        } else {
            if (pv.x < s_bl_x0) s_bl_x0 = pv.x;
            if (pv.x > s_bl_x1) s_bl_x1 = pv.x;
            if (pv.y < s_bl_y0) s_bl_y0 = pv.y;
            if (pv.y > s_bl_y1) s_bl_y1 = pv.y;
            if (pv.z < s_bl_z0) s_bl_z0 = pv.z;
            if (pv.z > s_bl_z1) s_bl_z1 = pv.z;
            if (pv.u < s_bl_u0) s_bl_u0 = pv.u;
            if (pv.u > s_bl_u1) s_bl_u1 = pv.u;
            if (pv.v < s_bl_v0) s_bl_v0 = pv.v;
            if (pv.v > s_bl_v1) s_bl_v1 = pv.v;
        }
        s_bl_n++;
        s_bl_argb = pv.argb;
    }
#endif
}

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

static void apply_texgen(const DCGXVertex* v, float* u, float* v_out) {
    int slot;
    const float (*tm)[4];

    *u = v->texcoord[0];
    *v_out = v->texcoord[1];

    if (g_gx.num_tex_gens <= 0)
        return;
    /* Only GX_TG_TEX0-sourced 2x4/3x4 matrix texgens are handled. BUMP and
     * SRTG texgens, and POS/NRM sources, fall through to the raw texcoord —
     * wrong mapping, still visible. TODO(M3): kb/tev-map.md census. */
    if (g_gx.tex_gen_src[0] != GX_TG_TEX0)
        return;
    slot = texmtx_slot(g_gx.tex_gen_mtx[0]);
    if (slot < 0 || slot >= 10)
        return;

    tm = (const float (*)[4])g_gx.tex_mtx[slot];
    *u     = tm[0][0] * v->texcoord[0] + tm[0][1] * v->texcoord[1] + tm[0][3];
    *v_out = tm[1][0] * v->texcoord[0] + tm[1][1] * v->texcoord[1] + tm[1][3];
}

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
#ifndef DC_PVR_NO_TEVCONST
    float tevconst[3];
    int   have_tevconst = 0;
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
    if (g_gx.tev_stages[0].tex_map == GX_TEXMAP_NULL) tex = NULL;
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

#ifndef DC_PVR_NO_FOG
    /* Remember what this batch wants; frame_begin programs it. Must run every
     * batch, not only on a header-key change — the fog parameters are not in
     * the key. */
    fog_latch();
#endif

    ensure_header(tex);

#ifndef DC_PVR_NO_TEVCONST
    /* Per-batch, not per-vertex: the TEV stage and its registers cannot change
     * inside a batch, only between them. */
    have_tevconst = tev_const_color(tevconst);
#ifndef DC_PVR_NO_TEVCONST_ALPHA
    have_tevalpha = tev_const_alpha(&tevalpha);
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
#endif

#if !defined(DC_PVR_NO_LIGHTING) && !defined(DC_PVR_NO_SHADEFAST)
    need_light = (g_gx.num_chans > 0) &&
                 (g_gx.chan_ctrl_enable[0] || g_gx.chan_ctrl_enable[1]);
#endif

    step = per_prim;
    for (i = 0; i + per_prim <= count; i += step) {
        ClipVtx cv[4];
        int k;

        for (k = 0; k < per_prim; k++) {
            const DCGXVertex* v = &verts[i + k];
            float ox = v->position[0], oy = v->position[1], oz = v->position[2];
            float eye[3], nrm[3], nl;

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

#if !defined(DC_PVR_NO_LIGHTING) && !defined(DC_PVR_NO_SHADEFAST)
            if (need_light)
#endif
            {
            eye[0] = mv[0][0] * ox + mv[0][1] * oy + mv[0][2] * oz + mv[0][3];
            eye[1] = mv[1][0] * ox + mv[1][1] * oy + mv[1][2] * oz + mv[1][3];
            eye[2] = mv[2][0] * ox + mv[2][1] * oy + mv[2][2] * oz + mv[2][3];

            {
                float nx = v->normal[0] * (1.0f / DC_GX_NRM_SCALE);
                float ny = v->normal[1] * (1.0f / DC_GX_NRM_SCALE);
                float nz = v->normal[2] * (1.0f / DC_GX_NRM_SCALE);
                nrm[0] = nm[0][0] * nx + nm[0][1] * ny + nm[0][2] * nz;
                nrm[1] = nm[1][0] * nx + nm[1][1] * ny + nm[1][2] * nz;
                nrm[2] = nm[2][0] * nx + nm[2][1] * ny + nm[2][2] * nz;
                nl = nrm[0] * nrm[0] + nrm[1] * nrm[1] + nrm[2] * nrm[2];
                if (nl > 1e-12f) {
                    nl = 1.0f / sqrtf(nl);
                    nrm[0] *= nl; nrm[1] *= nl; nrm[2] *= nl;
                }
            }
            }

            apply_texgen(v, &cv[k].u, &cv[k].v);
            if (tex) {
                cv[k].u *= tex->u_scale;
                cv[k].v *= tex->v_scale;
            }
            cv[k].argb = shade_vertex(v, eye, nrm);
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
             * build, without giving up the fix. */
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
    }

#ifdef DC_PVR_BATCH_LOG
    if (s_batch_log_now) {
        DC_LOG("BATCH b=%u %s n=%d verts=%d tex=%d %dx%d fmt=0x%X a=%d "
               "us=%.3f wrap=%d,%d bm=%d,%d,%d cull=%d zt=%d zf=%d zw=%d "
               "chans=%d argb=%08X ac=%d/%d,%d/%d cut=%d pt=%d cu=%d,%d tm=%d,%d "
               "st=%d t1=%d bbox=%.1f,%.1f..%.1f,%.1f z=%.5f..%.5f "
               "uv=%.2f,%.2f..%.2f,%.2f\n",
               s_batches, (per_prim == 4) ? "QUAD" : "TRI", count, s_bl_n,
               tex ? 1 : 0,
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
#else
#define dc_pvr_fb_dump_image(fb) ((void)(fb))
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

#endif /* DC_PVR_BACKEND */
