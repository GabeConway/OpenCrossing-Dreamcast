/* dc_gx.c - GX API for Dreamcast: the renderer-agnostic CPU half.
 *
 * This is pc/src/pc_gx.c with the OpenGL half amputated. What survives is
 * exactly the part kb/design-platform-api.md §3.6 says must be reproduced, and
 * it is also precisely what a tile-based deferred GPU wants:
 *
 *   - the GX state machine and its dirty flags
 *   - state dedup (the decomp re-sets identical state constantly)
 *   - deferred vertex commit with COLOR0 CARRIED FORWARD
 *   - batch merging across state that did not actually change
 *     (measured on the base port: 491-600 draws/frame -> 114.6)
 *   - strip/fan -> independent triangles at accumulation time
 *   - whole-batch CPU frustum cull (rejects 60-80 % of batches, ~286/frame)
 *
 * WHAT IS NOT HERE, ON PURPOSE:
 *   The PowerVR backend. PLAN §3.3 stages it (A = GLdc, M2; B = direct KOS PVR
 *   with store queues and FTRV T&L, M4) and the TEV classification that decides
 *   the OP/PT/TR list mapping is still being measured (tev_map.md, M2). Every
 *   submission call in here funnels through dc_gx_backend_*() at the bottom of
 *   the file, which is the ONLY seam the renderer attaches to.
 *
 * Also folded in: the GXTexObj / GXTlutObj surface that lives in
 * pc/src/pc_gx_texture.c on the PC side. The object layouts are copied
 * byte-for-byte (the GAME allocates those structs) but the decode+upload path
 * is a stub — DC wants twiddled 16-bit / paletted / VQ output into VRAM, not
 * linear RGBA8, and that is an M2 deliverable.
 */
#include "dc_gx_internal.h"
#include "dc_pvr.h"        /* dc_pvr_tex_get(): detects an evicted handle */
#include "dc_mem_ledger.h"
#include <dolphin/gx/GXEnum.h>

typedef struct { u8 r, g, b, a; } GXColor;

/* Declared here rather than included: GXTev.h uses enum types where the port
 * uses u32 (same reason pc_gx.c does it). */
void GXSetTevColorIn(u32 stage, u32 a, u32 b, u32 c, u32 d);
void GXSetTevAlphaIn(u32 stage, u32 a, u32 b, u32 c, u32 d);
void GXSetTevColorOp(u32 stage, u32 op, u32 bias, u32 scale, GXBool clamp, u32 out_reg);
void GXSetTevAlphaOp(u32 stage, u32 op, u32 bias, u32 scale, GXBool clamp, u32 out_reg);
void GXSetTevIndirect(u32 stage, u32 ind_stage, u32 fmt, u32 bias_sel,
                      u32 mtx_sel, u32 wrap_s, u32 wrap_t, GXBool add_prev,
                      GXBool ind_lod, u32 alpha_sel);

/* ==========================================================================
 * State
 * ========================================================================== */
DCGXState g_gx;

/* Kill switches. Compile-time on DC (no env vars); PLAN requires every
 * optimization to have one so a regression can be bisected on hardware.
 *
 * ⚠️ THEY ARE `const`, and that is load-bearing, not tidiness. These were plain
 * `int` globals read by ~35 state setters on their hot path; `-fno-strict-
 * aliasing` is on, so GCC had to RELOAD each one from memory before every
 * compare and could not fold the branch. They are already decided at compile
 * time by the #ifdefs below — nothing anywhere writes them (verified: the only
 * references in the tree are these definitions, the declarations in
 * dc_gx_internal.h, and the reads inside this file) — so making that explicit
 * lets the branch fold away entirely. The kill switch is unchanged: it was
 * always a -D, never a runtime knob. */
#ifdef DC_NO_STATE_DEDUP
const int dc_gx_state_dedup = 0;
#else
const int dc_gx_state_dedup = 1;
#endif
#ifdef DC_NO_STRIP_CONVERT
const int dc_gx_strip_convert = 0;
#else
const int dc_gx_strip_convert = 1;
#endif
#ifdef DC_NO_BATCH_CULL
const int dc_gx_batch_cull = 0;
#else
const int dc_gx_batch_cull = 1;
#endif
#ifdef DC_NO_DRAW_MERGE
const int dc_gx_draw_merge = 0;
#else
const int dc_gx_draw_merge = 1;
#endif

/* Per-frame diagnostics. Names kept from the PC port: game code and the
 * harness both read them. */
int pc_gx_draw_call_count = 0;
int pc_gx_prim_draws[5];
int pc_gx_merged_batches;
int pc_gx_culled_draws;
int pc_gx_flush_reason[18];
int pc_emu64_frame_cmds = 0;
int pc_emu64_frame_crashes = 0;
int pc_emu64_frame_noop_cmds = 0;
int pc_emu64_frame_tri_cmds = 0;
int pc_emu64_frame_vtx_cmds = 0;
int pc_emu64_frame_dl_cmds = 0;
int pc_emu64_frame_cull_visible = 0;
int pc_emu64_frame_cull_rejected = 0;

u64 dc_gx_flush_time_us = 0;
u64 dc_gx_texload_time_us = 0;
static u64 s_flush_time_acc = 0;
u64 dc_gx_texload_acc = 0;   /* non-static: also written by the texture path */

/* ---- Frame-phase attribution (-DDC_PERF_PHASE) ----------------------------
 * kb/perf-dc.md. `gx=` in the [PERF] line is the WHOLE of dc_gx_flush_vertices,
 * which is three very different jobs bolted together: the whole-batch AABB
 * frustum cull, the SH-4 transform/light/emit in dc_pvr.c, and the bookkeeping
 * around both. A single number cannot say which one to attack, and the two that
 * matter are affected by opposite changes — cheaper culling means MORE geometry
 * reaches the transform.
 *
 * These are published unconditionally (so dc_vi.c can print them without an
 * #ifdef of its own) and only WRITTEN under -DDC_PERF_PHASE: two extra
 * dc_time_us() reads per batch is ~600 timer reads/frame in the town, which is
 * measurable against the thing being measured. Zero means "not instrumented",
 * which is distinguishable from "measured as zero" because verts= is zero too.
 *
 * verts_lit is the number that decides whether the lighting path is worth
 * optimising at all: it counts vertices in batches whose channel control
 * actually enables a light, i.e. the vertices for which chan_component()'s
 * 8-light loop can run. The test is deliberately the same one dc_pvr.c's
 * chan_component() applies, so the two cannot drift apart silently. */
u64 dc_gx_cull_time_us = 0;
u64 dc_gx_submit_time_us = 0;
unsigned int dc_gx_phase_verts = 0;
unsigned int dc_gx_phase_verts_lit = 0;
unsigned int dc_gx_phase_batches_lit = 0;
unsigned int dc_gx_phase_src_verts = 0;
#ifdef DC_PERF_PHASE
static u64 s_cull_time_acc = 0;
static u64 s_submit_time_acc = 0;
static unsigned int s_phase_verts = 0;
static unsigned int s_phase_verts_lit = 0;
static unsigned int s_phase_batches_lit = 0;
/* ---- GX API census (-DDC_PERF_GXAPI) --------------------------------------
 * The phase split says ~58 % of a town frame is display-list traversal: the
 * game's draw code plus emu64 plus THIS FILE's GX entry points. Only the last
 * of those three is editable (CLAUDE.md forbids touching src/), so the ranking
 * question for any further work is "how much of that 58 ms is the GX API?".
 *
 * Counting answers it without the observer effect a per-call timer would have:
 * one increment is a handful of cycles against a function whose body is tens of
 * instructions at -O0, whereas two dc_time_us() reads per call would cost more
 * than the calls being measured. `vtxus` is the one aggregate timer, and only
 * on GXPosition3f32 — the single hottest entry point and the one that owns the
 * 32-byte staging copy — so its overhead is bounded and attributable.
 *
 * -DDC_PERF_GXAPI, off by default. */
unsigned int dc_gx_api_pos = 0, dc_gx_api_clr = 0, dc_gx_api_tc = 0;
unsigned int dc_gx_api_nrm = 0, dc_gx_api_begin = 0, dc_gx_api_dirty = 0;
u64 dc_gx_api_vtx_us = 0;
#ifdef DC_PERF_GXAPI
static unsigned int s_api_pos, s_api_clr, s_api_tc, s_api_nrm,
                    s_api_begin, s_api_dirty;
static u64 s_api_vtx_us;
#define DC_GXAPI(counter) do { (counter)++; } while (0)
#else
#define DC_GXAPI(counter) do { } while (0)
#endif

/* Vertices the GAME handed us for the batch currently staged, against the
 * `count` the backend is then asked to transform. They differ only because
 * GXBegin() rewrites GX_TRIANGLESTRIP/GX_TRIANGLEFAN into independent
 * triangles: an N-vertex strip becomes 3*(N-2) buffered vertices, so the
 * ratio of the two is exactly the transform work a native PVR strip would
 * save. It cannot be inferred from the [PERF] counters — prim_draws[2] is
 * zero precisely BECAUSE the conversion already happened. */
static unsigned int s_batch_src_verts = 0;
static unsigned int s_phase_src_verts = 0;
#endif

/* Set when a flush actually broke a batch; the next state change claims it,
 * attributing the break to its cause. */
static int s_flush_pending_attr = 0;

/* Strip/fan conversion state */
static int s_conv_active = 0;
static int s_conv_is_fan = 0;
static int s_conv_src_count = 0;
static int s_conv_src_expected = 0;
static DCGXVertex s_conv_v0, s_conv_v1;

/* Vertices emitted into the CURRENT GXBegin, counting ones already flushed out
 * of the staging buffer by a mid-batch split. g_gx.current_vertex_idx is only
 * the *staged* count now, so it can no longer serve as the batch-completion
 * counter (§3.6 behaviour 1 terminates batches on a counted total, because
 * emu64 never calls GXEnd). */
static int s_emit_total = 0;

/* One-shot warning, now reachable only under -DDC_NO_VERTEX_SPLIT. With
 * splitting on — the default — overflow cannot happen and no geometry is
 * ever dropped. */
static int s_overflow_warned = 0;

/* ---- Mid-batch split (kill switch: -DDC_NO_VERTEX_SPLIT) ------------------
 * DC_GX_MAX_VERTS used to be a HARD CEILING: past it dc_gx_commit_vertex()
 * dropped vertices and printed once. That is the only reason the buffer was
 * sized defensively at 8192. It is a flush point now — when the next primitive
 * would not fit, the staged vertices are submitted as their own draw and
 * accumulation restarts at index 0.
 *
 * Why that is safe:
 *   - Topology. DC_GX_MAX_VERTS is a multiple of 12, and GXBegin() below
 *     converts GX_TRIANGLESTRIP/GX_TRIANGLEFAN to GX_TRIANGLES *before* any
 *     vertex reaches the buffer, so every primitive the buffer holds is
 *     independent: 3 verts, or 4 for GX_QUADS. A split on a multiple of 12
 *     always lands on a primitive boundary. Strips/fans can only reach the
 *     buffer unconverted under -DDC_NO_STRIP_CONVERT, and splitting those WOULD
 *     lose the bridging vertices — dc_gx_split_ok() excludes them and they keep
 *     the old drop behaviour.
 *   - Cull. dc_gx_batch_is_offscreen() takes the AABB of what is staged. The
 *     AABB of each half is contained in the AABB of the whole, so splitting can
 *     only ever reject less. Strictly more conservative, never wrong.
 *   - State. Both halves are drawn with identical g_gx state, because every GX
 *     state setter flushes the open batch first (§3.6 behaviour 3), so state
 *     cannot change mid-batch.
 *
 * Consequence: DC_GX_MAX_VERTS is now a pure performance knob. Too small costs
 * draw calls; it can never cost a triangle. */
#ifdef DC_NO_VERTEX_SPLIT
static const int dc_gx_vertex_split = 0;
/* Without splitting, a merged batch must still fit the buffer. */
#define DC_GX_MERGE_CAP DC_GX_MAX_VERTS
#define DC_GX_STAGE_CAP DC_GX_MAX_VERTS
#else
static const int dc_gx_vertex_split = 1;
/* The split point, rounded DOWN to a multiple of 12 so that it always lands on
 * a primitive boundary. DC_GX_MAX_VERTS is already 2040 = 12 x 170, so this is
 * a no-op at the default; it exists so the -DDC_GX_MAX_VERTS=<n> kill switch
 * stays safe for any n (e.g. the old 8192 -> 8184). */
#define DC_GX_STAGE_CAP ((DC_GX_MAX_VERTS / 12) * 12)
/* With splitting the staging buffer no longer bounds a batch, so merging is
 * limited only by the counters: GXBegin takes a u16 nverts and strip
 * conversion multiplies by at most 3. */
#define DC_GX_MERGE_CAP 262144
#endif

/* A quad is the widest primitive that can sit in the buffer; anything below
 * that cannot stage even one. */
typedef char dc_gx_stage_cap_check[(DC_GX_STAGE_CAP >= 12) ? 1 : -1];

/* Splitting preserves topology only for primitives whose vertices are
 * independent. Everything here is post-conversion (see GXBegin). */
static int dc_gx_split_ok(void) {
    return dc_gx_vertex_split &&
           (g_gx.current_primitive == GX_TRIANGLES ||
            g_gx.current_primitive == GX_QUADS ||
            g_gx.current_primitive == GX_POINTS ||
            g_gx.current_primitive == GX_LINES);
}

/* ==========================================================================
 * Small helpers, ported verbatim
 * ========================================================================== */
static void dc_unpack_rgba8f(u32 packed, float* out_rgba) {
    /* Shift-based: colors packed as (r<<24|g<<16|b<<8|a) from N64 DL data. */
    out_rgba[0] = ((packed >> 24) & 0xFF) / 255.0f;
    out_rgba[1] = ((packed >> 16) & 0xFF) / 255.0f;
    out_rgba[2] = ((packed >> 8) & 0xFF) / 255.0f;
    out_rgba[3] = (packed & 0xFF) / 255.0f;
}

/* Byte-based: reads RGBA in memory order. For GXColor structs, NOT DL data.
 * SH-4 is little-endian exactly like the base targets, so this stays correct. */
static void dc_unpack_gxcolor_f(u32 color_as_u32, float* out_rgba) {
    const u8* bytes = (const u8*)&color_as_u32;
    out_rgba[0] = bytes[0] / 255.0f;
    out_rgba[1] = bytes[1] / 255.0f;
    out_rgba[2] = bytes[2] / 255.0f;
    out_rgba[3] = bytes[3] / 255.0f;
}

/* Map a tex-matrix ID to a slot: raw 0..9, GX enum 30..57 (stride 3), 60=id. */
static int dc_tex_mtx_id_to_slot(int id) {
    if (id == GX_IDENTITY) return -1;
    if (id >= 0 && id < 10) return id;
    if (id >= GX_TEXMTX0 && id < GX_IDENTITY) return (id - GX_TEXMTX0) / 3;
    return -1;
}

void dc_gx_mark_dirty(unsigned int flag) {
    unsigned int m;
#ifdef DC_PERF_GXAPI
    DC_GXAPI(s_api_dirty);
#endif
    if (s_flush_pending_attr) {
        pc_gx_flush_reason[__builtin_ctz(flag)]++;
        s_flush_pending_attr = 0;
    }
    g_gx.dirty |= flag;
    m = flag & DC_GX_DIRTY_UNIFORM_MASK;
    while (m) {
        g_gx.group_gen[__builtin_ctz(m)]++;
        m &= m - 1;
    }
}

static void dc_gx_dirty_all(void) {
    int b;
    g_gx.dirty = DC_GX_DIRTY_ALL;
    for (b = 0; b < DC_GX_STATE_GROUP_COUNT; b++)
        g_gx.group_gen[b]++;
}

/* ==========================================================================
 * Vertex accumulation
 * ==========================================================================
 * §3.6 behaviour 2: a GXPosition* call COMMITS the previous vertex and starts a
 * new one; the reset clears normal and texcoords (and color1, when the fat
 * vertex is compiled in) but CARRIES COLOR0 FORWARD. Break that and vertex
 * colours break.
 *
 * Converting batches (strip/fan -> triangle list) buffer the first two source
 * vertices, then emit one triangle per subsequent vertex. Strips alternate
 * winding; fans pivot on v0. Doing it here rather than at draw time is what
 * makes them mergeable with plain triangle batches.
 */
/* Make room for `need` more vertices. Returns 1 if they will fit. With
 * splitting enabled this submits the staged batch and restarts at index 0
 * (see the DC_NO_VERTEX_SPLIT block above for why that is safe); with it
 * disabled this is just the old capacity test. */
static int dc_gx_reserve_verts(int need) {
    if (g_gx.current_vertex_idx + need <= DC_GX_STAGE_CAP) return 1;
    if (!dc_gx_split_ok()) return 0;
    dc_gx_flush_vertices();     /* resets current_vertex_idx to 0 */
    return need <= DC_GX_STAGE_CAP;
}

static void dc_gx_commit_vertex(void) {
    int i;

#ifdef DC_PERF_PHASE
    s_batch_src_verts++;   /* one call == one vertex the game emitted */
#endif
    if (!s_conv_active) {
        if (dc_gx_reserve_verts(1)) {
            g_gx.vertex_buffer[g_gx.current_vertex_idx] = g_gx.current_vertex;
            g_gx.current_vertex_idx++;
            s_emit_total++;
        } else if (!s_overflow_warned) {
            s_overflow_warned = 1;
            DC_LOGE("[DC/GX] vertex buffer overflow: DC_GX_MAX_VERTS=%d is too "
                    "small, geometry is being DROPPED (DC_NO_VERTEX_SPLIT)\n",
                    DC_GX_MAX_VERTS);
        }
        return;
    }

    i = s_conv_src_count++;
    if (i == 0) { s_conv_v0 = g_gx.current_vertex; return; }
    if (i == 1) { s_conv_v1 = g_gx.current_vertex; return; }

    if (dc_gx_reserve_verts(3)) {
        DCGXVertex* dst = &g_gx.vertex_buffer[g_gx.current_vertex_idx];
        if (s_conv_is_fan || (i & 1) == 0) {
            dst[0] = s_conv_v0;
            dst[1] = s_conv_v1;
        } else {
            dst[0] = s_conv_v1;
            dst[1] = s_conv_v0;
        }
        dst[2] = g_gx.current_vertex;
        g_gx.current_vertex_idx += 3;
        s_emit_total += 3;
    } else if (!s_overflow_warned) {
        s_overflow_warned = 1;
        DC_LOGE("[DC/GX] vertex buffer overflow during strip conversion\n");
    }

    if (s_conv_is_fan) {
        s_conv_v1 = g_gx.current_vertex;
    } else {
        s_conv_v0 = s_conv_v1;
        s_conv_v1 = g_gx.current_vertex;
    }
}

static void dc_gx_commit_pending_and_flush(void) {
    if (!g_gx.in_begin) return;
    if (g_gx.vertex_pending) {
        dc_gx_commit_vertex();
        g_gx.vertex_pending = 0;
    }
    g_gx.in_begin = 0;
    s_conv_active = 0;
    if (g_gx.current_vertex_idx > 0)
        dc_gx_flush_vertices();
}

/* §3.6 behaviour 1: emu64 NEVER calls GXEnd. Batches terminate when the
 * declared vertex count from GXBegin is reached. Converting batches complete on
 * their SOURCE count, because the emitted count is 3*(n-2). */
void dc_gx_flush_if_begin_complete(void) {
    int pend;
    if (!g_gx.in_begin) return;
    pend = g_gx.vertex_pending ? 1 : 0;
    if (s_conv_active) {
        if (s_conv_src_expected <= 0) return;
        if (s_conv_src_count + pend < s_conv_src_expected) return;
    } else {
        if (g_gx.expected_vertex_count <= 0) return;
        /* s_emit_total, NOT current_vertex_idx: a mid-batch split has already
         * drained part of the batch out of the staging buffer. */
        if (s_emit_total + pend < g_gx.expected_vertex_count) return;
    }
    dc_gx_commit_pending_and_flush();
}

/* ==========================================================================
 * Whole-batch CPU frustum cull
 * ==========================================================================
 * Object-space AABB of the batch, 8 corners through the exact transform
 * (clip = P * MV * pos). If all corners are outside the same clip half-space,
 * the whole convex hull is (the test is linear in homogeneous coords), so
 * nothing rasterizes and the batch can be skipped entirely.
 *
 * On the base port this rejected 60-80 % of submitted batches. On a tile-based
 * deferred GPU it is worth even more: a rejected batch costs no TA bandwidth,
 * no binning, no store-queue traffic. Conservative: NaN and mixed-plane cases
 * draw. Merged batches share one matrix by construction (merging requires
 * dirty == 0).
 */
static int dc_gx_batch_is_offscreen(int count) {
    const DCGXVertex* v = g_gx.vertex_buffer;
    float mn[3], mx[3];
    float (*mv)[4];
    float (*pr)[4];
    int outside[6];
#ifdef DC_GX_NO_FAST_AABB
    int i, c;
#endif
    int ci, p;

#ifdef DC_GX_NO_FAST_AABB
    mn[0] = mx[0] = v[0].position[0];
    mn[1] = mx[1] = v[0].position[1];
    mn[2] = mx[2] = v[0].position[2];
    for (i = 1; i < count; i++) {
        for (c = 0; c < 3; c++) {
            float pv = v[i].position[c];
            if (pv < mn[c]) mn[c] = pv;
            if (pv > mx[c]) mx[c] = pv;
        }
    }
#else
    /* ⚠️ PERF, 2026-08-02 (kb/perf-dc.md). MEASURED at 4.1 ms of a 100 ms town
     * frame, over every vertex of every batch — including the 71 % that this
     * function then rejects, which is the point of it.
     *
     * The loop above is arithmetically identical, but at -O0 the `[c]` inner
     * loop costs its own counter, and `v[i].position[c]` recomputes
     * base + i*sizeof(DCGXVertex) + c*4 with a multiply on every one of the
     * three components. Walking a pointer and holding the six extrema in named
     * scalars removes all of that address arithmetic; nothing about the
     * comparison changes.
     *
     * `else if` is not a shortcut that can bite: after the seed both extrema
     * start equal, so a value below the running minimum is necessarily below
     * the running maximum, and NaN takes neither branch in either form —
     * exactly as before, which keeps the cull conservative.
     *
     * Kill switch: -DDC_GX_NO_FAST_AABB restores the indexed form verbatim. */
    {
        const DCGXVertex* p = v;
        const DCGXVertex* end = v + count;
        float n0, n1, n2, x0, x1, x2;
        n0 = x0 = p->position[0];
        n1 = x1 = p->position[1];
        n2 = x2 = p->position[2];
        for (++p; p < end; ++p) {
            float a = p->position[0];
            float b = p->position[1];
            float d = p->position[2];
            if (a < n0) n0 = a; else if (a > x0) x0 = a;
            if (b < n1) n1 = b; else if (b > x1) x1 = b;
            if (d < n2) n2 = d; else if (d > x2) x2 = d;
        }
        mn[0] = n0; mn[1] = n1; mn[2] = n2;
        mx[0] = x0; mx[1] = x1; mx[2] = x2;
    }
#endif

    mv = g_gx.pos_mtx[g_gx.current_mtx];
    pr = g_gx.projection_mtx;
    for (p = 0; p < 6; p++) outside[p] = 0;

    for (ci = 0; ci < 8; ci++) {
        float ox = (ci & 1) ? mx[0] : mn[0];
        float oy = (ci & 2) ? mx[1] : mn[1];
        float oz = (ci & 4) ? mx[2] : mn[2];
        float e0 = mv[0][0] * ox + mv[0][1] * oy + mv[0][2] * oz + mv[0][3];
        float e1 = mv[1][0] * ox + mv[1][1] * oy + mv[1][2] * oz + mv[1][3];
        float e2 = mv[2][0] * ox + mv[2][1] * oy + mv[2][2] * oz + mv[2][3];
        float cx = pr[0][0] * e0 + pr[0][1] * e1 + pr[0][2] * e2 + pr[0][3];
        float cy = pr[1][0] * e0 + pr[1][1] * e1 + pr[1][2] * e2 + pr[1][3];
        float cz = pr[2][0] * e0 + pr[2][1] * e1 + pr[2][2] * e2 + pr[2][3];
        float cw = pr[3][0] * e0 + pr[3][1] * e1 + pr[3][2] * e2 + pr[3][3];
        if (cx < -cw) outside[0]++;
        if (cx >  cw) outside[1]++;
        if (cy < -cw) outside[2]++;
        if (cy >  cw) outside[3]++;
        if (cz < -cw) outside[4]++;
        if (cz >  cw) outside[5]++;
    }
    for (p = 0; p < 6; p++)
        if (outside[p] == 8) return 1;
    return 0;
}

/* ==========================================================================
 * Flush -> backend
 * ========================================================================== */
void dc_gx_flush_vertices(void) {
    int count = g_gx.current_vertex_idx;
    u64 t0;

    if (count == 0) return;
    t0 = dc_time_us();
#ifdef DC_PERF_PHASE
    /* Snapshot-and-clear on EVERY exit path below, so the source count always
     * belongs to the batch whose `count` it is being compared against. A
     * mid-batch split (dc_gx_reserve_verts) flushes here too, and both halves
     * then carry their own share. */
    { unsigned int src_here = s_batch_src_verts; s_batch_src_verts = 0;
#define DC_PH_SRC src_here
#else
#define DC_PH_SRC 0u
#endif

    /* Frameskip: no submission. dirty stays set so the next surviving flush
     * applies everything that changed during the skipped ticks. */
    if (g_pc_frameskip_active) {
        s_flush_time_acc += dc_time_us() - t0;
        g_gx.current_vertex_idx = 0;
        return;
    }

    {
        int cullable = dc_gx_batch_cull &&
            (g_gx.current_primitive == GX_QUADS ||
             g_gx.current_primitive == GX_TRIANGLES ||
             g_gx.current_primitive == GX_TRIANGLESTRIP ||
             g_gx.current_primitive == GX_TRIANGLEFAN);
        int off;
#ifdef DC_PERF_PHASE
        u64 tc = dc_time_us();
        off = cullable && dc_gx_batch_is_offscreen(count);
        s_cull_time_acc += dc_time_us() - tc;
#else
        off = cullable && dc_gx_batch_is_offscreen(count);
#endif
        if (off) {
            pc_gx_culled_draws++;
            s_flush_pending_attr = 1;
            s_flush_time_acc += dc_time_us() - t0;
            g_gx.current_vertex_idx = 0;
            return;
        }
    }

    pc_gx_draw_call_count++;
    switch (g_gx.current_primitive) {
        case GX_QUADS:         pc_gx_prim_draws[0]++; break;
        case GX_TRIANGLES:     pc_gx_prim_draws[1]++; break;
        case GX_TRIANGLESTRIP: pc_gx_prim_draws[2]++; break;
        case GX_TRIANGLEFAN:   pc_gx_prim_draws[3]++; break;
        default:               pc_gx_prim_draws[4]++; break;
    }

    /* ---- THE SEAM ---- */
#ifdef DC_PERF_PHASE
    {
        u64 ts = dc_time_us();
        int lit = (g_gx.num_chans > 0) &&
                  (g_gx.chan_ctrl_enable[0] || g_gx.chan_ctrl_enable[1]);
        dc_gx_backend_submit(g_gx.current_primitive, g_gx.vertex_buffer, count);
        s_submit_time_acc += dc_time_us() - ts;
        s_phase_verts += (unsigned int)count;
        s_phase_src_verts += DC_PH_SRC;
        if (lit) { s_phase_batches_lit++; s_phase_verts_lit += (unsigned int)count; }
    }
#else
    dc_gx_backend_submit(g_gx.current_primitive, g_gx.vertex_buffer, count);
#endif

    g_gx.dirty = 0;
    s_flush_pending_attr = 1;
    g_gx.current_vertex_idx = 0;
    s_flush_time_acc += dc_time_us() - t0;
#ifdef DC_PERF_PHASE
    }
#endif
#undef DC_PH_SRC
}

void dc_gx_frame_timing_snapshot(void) {
    dc_gx_flush_time_us = s_flush_time_acc;
    dc_gx_texload_time_us = dc_gx_texload_acc;
    s_flush_time_acc = 0;
    dc_gx_texload_acc = 0;
#ifdef DC_PERF_PHASE
    dc_gx_cull_time_us = s_cull_time_acc;
    dc_gx_submit_time_us = s_submit_time_acc;
    dc_gx_phase_verts = s_phase_verts;
    dc_gx_phase_verts_lit = s_phase_verts_lit;
    dc_gx_phase_batches_lit = s_phase_batches_lit;
    dc_gx_phase_src_verts = s_phase_src_verts;
    s_phase_src_verts = 0;
#endif
#ifdef DC_PERF_GXAPI
    dc_gx_api_pos = s_api_pos;     s_api_pos = 0;
    dc_gx_api_clr = s_api_clr;     s_api_clr = 0;
    dc_gx_api_tc = s_api_tc;       s_api_tc = 0;
    dc_gx_api_nrm = s_api_nrm;     s_api_nrm = 0;
    dc_gx_api_begin = s_api_begin; s_api_begin = 0;
    dc_gx_api_dirty = s_api_dirty; s_api_dirty = 0;
    dc_gx_api_vtx_us = s_api_vtx_us; s_api_vtx_us = 0;
#endif
#ifdef DC_PERF_PHASE
    s_cull_time_acc = 0;
    s_submit_time_acc = 0;
    s_phase_verts = 0;
    s_phase_verts_lit = 0;
    s_phase_batches_lit = 0;
#endif
}

/* ==========================================================================
 * Init / frame
 * ========================================================================== */
void dc_gx_init(void) {
    int i;

    memset(&g_gx, 0, sizeof(g_gx));

    /* The vertex buffer is still the largest single object in the platform
     * layer: DC_GX_MAX_VERTS * sizeof(DCGXVertex). It lives in BSS (like the PC
     * port) rather than in a ledger extent, so tell the ledger about it
     * explicitly. NOTE bucket 11 is retired to 0 in dc_mem_budget.h — these
     * bytes are already counted once as .bss inside the image span, and this
     * note exists for the per-bucket report, not for the fit inequality. */
    dc_mem_note(DCMEM_PVR_STAGING,
                (ptrdiff_t)(DC_GX_MAX_VERTS * (int)sizeof(DCGXVertex)));

    /* The live configuration lands in every smoke log; `split` is the one that
     * makes DC_GX_MAX_VERTS a perf knob rather than a correctness ceiling. */
    DC_LOGE("[DC/GX] verts=%d x %uB = %u B; dedup=%d strip=%d cull=%d merge=%d "
            "split=%d\n",
            DC_GX_MAX_VERTS, (unsigned)sizeof(DCGXVertex),
            (unsigned)(DC_GX_MAX_VERTS * sizeof(DCGXVertex)),
            dc_gx_state_dedup, dc_gx_strip_convert,
            dc_gx_batch_cull, dc_gx_draw_merge, dc_gx_vertex_split);

    g_gx.projection_type = GX_PERSPECTIVE;
    g_gx.num_tev_stages = 1;
    g_gx.num_chans = 1;
    g_gx.num_tex_gens = 0;
    g_gx.cull_mode = GX_CULL_NONE;
    g_gx.z_compare_enable = 1;
    g_gx.z_compare_func = GX_LEQUAL;
    g_gx.z_update_enable = 1;
    g_gx.color_update_enable = 1;
    g_gx.alpha_update_enable = 1;
    g_gx.blend_mode = GX_BM_NONE;
    g_gx.blend_src = GX_BL_ONE;
    g_gx.blend_dst = GX_BL_ZERO;
    g_gx.clear_color[3] = 0.0f;
    g_gx.clear_depth = 1.0f;

    for (i = 0; i < 4; i++)
        g_gx.projection_mtx[i][i] = 1.0f;

    for (i = 0; i < 10; i++) {
        g_gx.pos_mtx[i][0][0] = 1.0f;
        g_gx.pos_mtx[i][1][1] = 1.0f;
        g_gx.pos_mtx[i][2][2] = 1.0f;
        g_gx.nrm_mtx[i][0][0] = 1.0f;
        g_gx.nrm_mtx[i][1][1] = 1.0f;
        g_gx.nrm_mtx[i][2][2] = 1.0f;
    }

    for (i = 0; i < 4; i++) {
        g_gx.tev_swap_table[i].r = 0;
        g_gx.tev_swap_table[i].g = 1;
        g_gx.tev_swap_table[i].b = 2;
        g_gx.tev_swap_table[i].a = 3;
    }

    for (i = 0; i < 2; i++) {
        g_gx.chan_mat_color[i][0] = 1.0f;
        g_gx.chan_mat_color[i][1] = 1.0f;
        g_gx.chan_mat_color[i][2] = 1.0f;
        g_gx.chan_mat_color[i][3] = 1.0f;
    }

#ifdef DC_NO_LATE_PVR
    dc_gx_backend_init();
#endif
    dc_gx_dirty_all();
}

/* Bring the PVR up. Split out of dc_gx_init() so the boot splash survives the
 * asset load — pvr_init() reprograms the display controller at its own buffers
 * (kb/traps.md: "vram_s is not the displayed surface once pvr_init() has run"),
 * so calling it early blanks the screen and every second of disc I/O after it
 * happens on black. The GX STATE MACHINE still comes up at the old point, which
 * is what boot-order rule 4 actually requires; nothing between there and here
 * makes a GX call.
 *
 * Idempotent, so a second call is harmless.
 * Kill switch: -DDC_NO_LATE_PVR restores pvr_init() inside dc_gx_init(). */
void dc_gx_backend_start(void) {
#ifndef DC_NO_LATE_PVR
    static int started = 0;
    if (started) return;
    started = 1;
    dc_gx_backend_init();
#endif
}

void dc_gx_shutdown(void) {
    dc_gx_backend_shutdown();
}

/* Called by dc_vi.c once per retrace, and by game code under its pc_ name. */
void pc_gx_begin_frame(void) {
    pc_emu64_frame_cmds = 0;
    pc_emu64_frame_crashes = 0;
    pc_emu64_frame_noop_cmds = 0;
    pc_emu64_frame_tri_cmds = 0;
    pc_emu64_frame_vtx_cmds = 0;
    pc_emu64_frame_dl_cmds = 0;
    pc_emu64_frame_cull_visible = 0;
    pc_emu64_frame_cull_rejected = 0;
    pc_gx_draw_call_count = 0;
    memset(pc_gx_prim_draws, 0, sizeof(pc_gx_prim_draws));
    memset(pc_gx_flush_reason, 0, sizeof(pc_gx_flush_reason));
    pc_gx_merged_batches = 0;
    pc_gx_culled_draws = 0;
    s_flush_pending_attr = 0;
    g_pc_widescreen_stretch = 0;

    if (g_pc_frameskip_active) {
        g_gx.dirty = DC_GX_DIRTY_ALL;
        return;
    }

    dc_gx_backend_frame_begin();
}

void dc_gx_end_frame(void) {
    dc_gx_commit_pending_and_flush();
    if (!g_pc_frameskip_active)
        dc_gx_backend_frame_end();
}

/* ==========================================================================
 * Vertex submission
 * ========================================================================== */
void GXBegin(u32 primitive, u32 vtxfmt, u16 nverts) {
    u32 eff_prim = primitive;
#ifdef DC_PERF_GXAPI
    DC_GXAPI(s_api_begin);
#endif
    int conv = 0, conv_fan = 0;
    int emit_count;

    if (dc_gx_strip_convert &&
        (primitive == GX_TRIANGLESTRIP || primitive == GX_TRIANGLEFAN)) {
        conv = 1;
        conv_fan = (primitive == GX_TRIANGLEFAN);
        eff_prim = GX_TRIANGLES;
    }
    emit_count = conv ? (nverts >= 3 ? ((int)nverts - 2) * 3 : 0) : (int)nverts;

    /* §3.6 behaviour 3: every GX state setter flushes the open batch, so
     * arriving here with a COMPLETE batch still open means nothing changed
     * since it was built — the two draws share identical state and can be
     * concatenated. Only list primitives merge safely (strips/fans would
     * bridge geometry across batches). */
    if (dc_gx_draw_merge && g_gx.in_begin && g_gx.dirty == 0 &&
        (g_gx.current_primitive == GX_QUADS || g_gx.current_primitive == GX_TRIANGLES) &&
        eff_prim == (u32)g_gx.current_primitive &&
        vtxfmt == (u32)g_gx.current_vtxfmt &&
        g_gx.expected_vertex_count > 0) {
        int pend = g_gx.vertex_pending ? 1 : 0;
        int prev_complete = s_conv_active
            ? (s_conv_src_count + pend == s_conv_src_expected)
            : (s_emit_total + pend == g_gx.expected_vertex_count);
        /* DC_GX_MERGE_CAP is the staging buffer only when splitting is off;
         * with splitting on a merged batch may exceed it and simply becomes
         * several draws. */
        if (prev_complete &&
            g_gx.expected_vertex_count + emit_count <= DC_GX_MERGE_CAP) {
            if (g_gx.vertex_pending) {
                dc_gx_commit_vertex();
                g_gx.vertex_pending = 0;
            }
            g_gx.expected_vertex_count += emit_count;
            s_conv_active = conv;
            s_conv_is_fan = conv_fan;
            s_conv_src_count = 0;
            s_conv_src_expected = nverts;
            pc_gx_merged_batches++;
            memset(&g_gx.current_vertex, 0, sizeof(DCGXVertex));
            g_gx.current_vertex.color0[0] = 255;
            g_gx.current_vertex.color0[1] = 255;
            g_gx.current_vertex.color0[2] = 255;
            g_gx.current_vertex.color0[3] = 255;
            return;
        }
    }

    dc_gx_commit_pending_and_flush();
    if (s_flush_pending_attr) {
        pc_gx_flush_reason[16]++;
        s_flush_pending_attr = 0;
    }

    g_gx.current_primitive = eff_prim;
    g_gx.current_vtxfmt = vtxfmt;
    g_gx.expected_vertex_count = emit_count;
    g_gx.current_vertex_idx = 0;
    s_emit_total = 0;           /* new batch: nothing emitted or split out yet */
    g_gx.in_begin = 1;
    g_gx.vertex_pending = 0;
    s_conv_active = conv;
    s_conv_is_fan = conv_fan;
    s_conv_src_count = 0;
    s_conv_src_expected = nverts;
    memset(&g_gx.current_vertex, 0, sizeof(DCGXVertex));
    g_gx.current_vertex.color0[0] = 255;
    g_gx.current_vertex.color0[1] = 255;
    g_gx.current_vertex.color0[2] = 255;
    g_gx.current_vertex.color0[3] = 255;
}

/* emu64 omits this. Kept because a handful of non-emu64 call sites use it. */
void GXEnd(void) { dc_gx_commit_pending_and_flush(); }

void GXPosition3f32(f32 x, f32 y, f32 z) {
#ifdef DC_PERF_GXAPI
    u64 t_api = dc_time_us();
    DC_GXAPI(s_api_pos);
#endif

    if (g_gx.vertex_pending)
        dc_gx_commit_vertex();

    /* CARRY COLOR0 FORWARD — by not touching it.
     *
     * This used to read the four colour bytes into locals and store them
     * straight back, four instructions each way, 6,951 times a frame in the
     * town. It was a provable no-op: dc_gx_commit_vertex() above only ever
     * READS g_gx.current_vertex (the struct copy at :327, the s_conv_v0/v1
     * saves at :340-341 and the three dst[] stores at :346-352), so nothing
     * between the read and the write-back could change color0. The contract in
     * the old comment — "everything else resets, colour carries" — is satisfied
     * by resetting only the fields that reset.
     *
     * Kept as a comment rather than deleted silently because "why does colour
     * survive a GXPosition when normal and texcoord do not" is a real question
     * about this API, and the answer is emu64's §3.6 behaviour 2. */
#ifdef DC_GX_NO_FASTPATH
    {
        u8 cr = g_gx.current_vertex.color0[0];
        u8 cg = g_gx.current_vertex.color0[1];
        u8 cb = g_gx.current_vertex.color0[2];
        u8 ca = g_gx.current_vertex.color0[3];
        g_gx.current_vertex.normal[0] = 0;
        g_gx.current_vertex.normal[1] = 0;
        g_gx.current_vertex.normal[2] = 0;
        g_gx.current_vertex.color0[0] = cr;
        g_gx.current_vertex.color0[1] = cg;
        g_gx.current_vertex.color0[2] = cb;
        g_gx.current_vertex.color0[3] = ca;
    }
#else
    g_gx.current_vertex.normal[0] = 0;
    g_gx.current_vertex.normal[1] = 0;
    g_gx.current_vertex.normal[2] = 0;
#endif /* DC_GX_NO_FASTPATH */
#ifdef DC_GX_FAT_VERTEX
    g_gx.current_vertex.color1[0] = 0;
    g_gx.current_vertex.color1[1] = 0;
    g_gx.current_vertex.color1[2] = 0;
    g_gx.current_vertex.color1[3] = 0;
#endif
    g_gx.current_vertex.texcoord[0] = 0.0f;
    g_gx.current_vertex.texcoord[1] = 0.0f;

    g_gx.current_vertex.position[0] = x;
    g_gx.current_vertex.position[1] = y;
    g_gx.current_vertex.position[2] = z;
    g_gx.vertex_pending = 1;
#ifdef DC_PERF_GXAPI
    s_api_vtx_us += dc_time_us() - t_api;
#endif
}

void GXPosition3u16(u16 x, u16 y, u16 z) { GXPosition3f32((f32)x, (f32)y, (f32)z); }
void GXPosition3s16(s16 x, s16 y, s16 z) { GXPosition3f32((f32)x, (f32)y, (f32)z); }
void GXPosition3u8(u8 x, u8 y, u8 z)     { GXPosition3f32((f32)x, (f32)y, (f32)z); }
void GXPosition3s8(s8 x, s8 y, s8 z)     { GXPosition3f32((f32)x, (f32)y, (f32)z); }
void GXPosition2f32(f32 x, f32 y)        { GXPosition3f32(x, y, 0.0f); }
void GXPosition2u16(u16 x, u16 y)        { GXPosition3f32((f32)x, (f32)y, 0.0f); }
void GXPosition2s16(s16 x, s16 y)        { GXPosition3f32((f32)x, (f32)y, 0.0f); }
void GXPosition2u8(u8 x, u8 y)           { GXPosition3f32((f32)x, (f32)y, 0.0f); }
void GXPosition2s8(s8 x, s8 y)           { GXPosition3f32((f32)x, (f32)y, 0.0f); }

/* Indexed attribute fetch. GXSetArray stored a raw GameCube-era 32-bit
 * pointer; it has to survive the seg2k0 range heuristic (PLAN §11.6). Source
 * layout is assumed f32[3] / f32[2], as on the base port. */
void GXPosition1x16(u16 index) {
    if (g_gx.array_base[GX_VA_POS]) {
        const u8* base = (const u8*)g_gx.array_base[GX_VA_POS];
        const f32* pos = (const f32*)(base + index * g_gx.array_stride[GX_VA_POS]);
        GXPosition3f32(pos[0], pos[1], pos[2]);
    }
}
void GXPosition1x8(u8 index) { GXPosition1x16(index); }

void GXNormal3f32(f32 x, f32 y, f32 z) {
#ifdef DC_PERF_GXAPI
    DC_GXAPI(s_api_nrm);
#endif
    /* Stored as s16 fixed point; SH-4 lighting consumes it that way.
     *
     * ⚠️ A "one magnitude test instead of six clamps" fast path was written
     * here and REVERTED: measured across 215 counter-matched windows it moved
     * nothing (see the note on GXPosition3f32 above). The six clamps stay
     * because they are the obvious code and they cost nothing measurable. */
    float sx = x * DC_GX_NRM_SCALE, sy = y * DC_GX_NRM_SCALE, sz = z * DC_GX_NRM_SCALE;
    if (sx >  32767.0f) sx =  32767.0f;
    if (sx < -32768.0f) sx = -32768.0f;
    if (sy >  32767.0f) sy =  32767.0f;
    if (sy < -32768.0f) sy = -32768.0f;
    if (sz >  32767.0f) sz =  32767.0f;
    if (sz < -32768.0f) sz = -32768.0f;
    g_gx.current_vertex.normal[0] = (short)sx;
    g_gx.current_vertex.normal[1] = (short)sy;
    g_gx.current_vertex.normal[2] = (short)sz;
}
void GXNormal3s16(s16 x, s16 y, s16 z) {
#ifdef DC_PERF_GXAPI
    DC_GXAPI(s_api_nrm);
#endif
    g_gx.current_vertex.normal[0] = x;
    g_gx.current_vertex.normal[1] = y;
    g_gx.current_vertex.normal[2] = z;
}
void GXNormal3s8(s8 x, s8 y, s8 z) {
    GXNormal3f32(x / 127.0f, y / 127.0f, z / 127.0f);
}
void GXNormal1x16(u16 index) {
    if (g_gx.array_base[GX_VA_NRM]) {
        const u8* base = (const u8*)g_gx.array_base[GX_VA_NRM];
        const f32* nrm = (const f32*)(base + index * g_gx.array_stride[GX_VA_NRM]);
        GXNormal3f32(nrm[0], nrm[1], nrm[2]);
    }
}
void GXNormal1x8(u8 index) { GXNormal1x16(index); }

void GXColor4u8(u8 r, u8 g, u8 b, u8 a) {
#ifdef DC_PERF_GXAPI
    DC_GXAPI(s_api_clr);
#endif
    g_gx.current_vertex.color0[0] = r;
    g_gx.current_vertex.color0[1] = g;
    g_gx.current_vertex.color0[2] = b;
    g_gx.current_vertex.color0[3] = a;
}
void GXColor3u8(u8 r, u8 g, u8 b) { GXColor4u8(r, g, b, 255); }
void GXColor1u32(u32 clr) {
    GXColor4u8((clr >> 24) & 0xFF, (clr >> 16) & 0xFF, (clr >> 8) & 0xFF, clr & 0xFF);
}
void GXColor1u16(u16 clr) { GXColor1u32((u32)clr << 16); }
void GXColor1x16(u16 index) {
    if (g_gx.array_base[GX_VA_CLR0]) {
        const u8* base = (const u8*)g_gx.array_base[GX_VA_CLR0];
        const u8* clr = base + index * g_gx.array_stride[GX_VA_CLR0];
        GXColor4u8(clr[0], clr[1], clr[2], clr[3]);
    }
}
void GXColor1x8(u8 index) { GXColor1x16(index); }
void GXColor4f32(float r, float g, float b, float a) {
    GXColor4u8((u8)(r * 255.0f + 0.5f), (u8)(g * 255.0f + 0.5f),
               (u8)(b * 255.0f + 0.5f), (u8)(a * 255.0f + 0.5f));
}

/* Channel 0 only — emu64 emits one texcoord; multi-tex uses matrix transforms. */
void GXTexCoord2f32(f32 s, f32 t) {
#ifdef DC_PERF_GXAPI
    DC_GXAPI(s_api_tc);
#endif
    g_gx.current_vertex.texcoord[0] = s;
    g_gx.current_vertex.texcoord[1] = t;
}
void GXTexCoord2u16(u16 s, u16 t) { GXTexCoord2f32((f32)s, (f32)t); }
/* No frac scaling — emu64 already provides pre-scaled texcoords. */
void GXTexCoord2s16(s16 s, s16 t) { GXTexCoord2f32((f32)s, (f32)t); }
void GXTexCoord2u8(u8 s, u8 t)    { GXTexCoord2f32((f32)s, (f32)t); }
void GXTexCoord2s8(s8 s, s8 t)    { GXTexCoord2f32((f32)s, (f32)t); }
void GXTexCoord1f32(f32 s, f32 t) { GXTexCoord2f32(s, t); }
void GXTexCoord1u16(u16 s, u16 t) { GXTexCoord2f32((f32)s, (f32)t); }
void GXTexCoord1s16(s16 s, s16 t) { GXTexCoord2s16(s, t); }
void GXTexCoord1u8(u8 s, u8 t)    { GXTexCoord2f32((f32)s, (f32)t); }
void GXTexCoord1s8(s8 s, s8 t)    { GXTexCoord2f32((f32)s, (f32)t); }
void GXTexCoord1x16(u16 index) {
    if (g_gx.array_base[GX_VA_TEX0]) {
        const u8* base = (const u8*)g_gx.array_base[GX_VA_TEX0];
        const f32* tc = (const f32*)(base + index * g_gx.array_stride[GX_VA_TEX0]);
        GXTexCoord2f32(tc[0], tc[1]);
    }
}
void GXTexCoord1x8(u8 index) { GXTexCoord1x16(index); }

/* ==========================================================================
 * Vertex descriptor / indexed arrays
 * ========================================================================== */
void GXSetVtxDesc(u32 attr, u32 type) {
    if (attr < DC_GX_MAX_ATTR) g_gx.vtx_desc[attr] = type;
}
void GXSetVtxDescv(const void* list) {
    const u32* p = (const u32*)list;
    while (p[0] != GX_VA_NULL) {
        GXSetVtxDesc(p[0], p[1]);
        p += 2;
    }
}
void GXClearVtxDesc(void) { memset(g_gx.vtx_desc, 0, sizeof(g_gx.vtx_desc)); }

void GXSetVtxAttrFmt(u32 vtxfmt, u32 attr, u32 cnt, u32 type, u8 frac) {
    (void)cnt; (void)type;
    if (vtxfmt < GX_MAX_VTXFMT) {
        if (attr >= GX_VA_TEX0 && attr <= GX_VA_TEX7) {
            int tc = (int)attr - GX_VA_TEX0;
            g_gx.vtx_fmt[vtxfmt].has_texcoord[tc] = 1;
            g_gx.vtx_fmt[vtxfmt].texcoord_frac[tc] = frac;
        }
    }
}

void GXSetArray(u32 attr, const void* data, u32 size, u8 stride) {
    (void)size;
    /* Working-set census (dc_asset_census.c). MEASURED 2026-08-02: this records
     * ZERO hits, and not just on the title screen — the only GXSetArray call
     * site anywhere in src/ is GXInit.c:252's reset loop, so this game draws no
     * indexed geometry at all and this hook can never see an asset. Kept
     * because it costs nothing and a future J3D path would use it; the vertex
     * census that actually works is the OSs16tof32 shim in
     * dc/include/dc_census_vtx.h. Do not re-derive this — it cost a search. */
    dc_asset_census_note(data, 'V');
    if (attr < GX_VA_MAX_ATTR) {
        g_gx.array_base[attr] = data;
        g_gx.array_stride[attr] = stride;
    }
}

void GXInvalidateVtxCache(void) { }

void GXGetVtxAttrFmt(u32 idx, u32 attr, u32* compCnt, u32* compType, u8* shift) {
    (void)idx; (void)attr;
    if (compCnt) *compCnt = 0;
    if (compType) *compType = 0;
    if (shift) *shift = 0;
}

/* ==========================================================================
 * Transforms
 * ========================================================================== */
void GXSetProjection(const void* mtx, u32 type) {
    static float s_proj_in[12];
    static u32   s_proj_type;
    static int   s_proj_valid = 0;

    if (dc_gx_state_dedup && s_proj_valid && type == s_proj_type &&
        memcmp(mtx, s_proj_in, sizeof(s_proj_in)) == 0)
        return;
    memcpy(s_proj_in, mtx, sizeof(s_proj_in));
    s_proj_type = type;
    s_proj_valid = 1;

    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_PROJECTION);
    g_gx.projection_type = type;
    memcpy(g_gx.projection_mtx, mtx, sizeof(float) * 12);

    /* GX stores only 3 rows; the 4th is implicit from the projection type.
     * NOTE FOR THE PVR BACKEND: this builds a GameCube-convention projection
     * (z in [-1, 0]). PVR wants its own 1/w convention — check this before
     * debugging "everything is inside out". */
    if (type == GX_PERSPECTIVE) {
        g_gx.projection_mtx[3][0] = 0.0f;
        g_gx.projection_mtx[3][1] = 0.0f;
        g_gx.projection_mtx[3][2] = -1.0f;
        g_gx.projection_mtx[3][3] = 0.0f;
    } else { /* GX_ORTHOGRAPHIC */
        g_gx.projection_mtx[3][0] = 0.0f;
        g_gx.projection_mtx[3][1] = 0.0f;
        g_gx.projection_mtx[3][2] = 0.0f;
        g_gx.projection_mtx[3][3] = 1.0f;
    }
}

/* 41 % of all batch breaks are modelview loads — the single biggest batching
 * obstacle. A CPU pre-transform pass would help DC even more than it helps the
 * base port (PLAN §3.3). */
void GXLoadPosMtxImm(const void* mtx, u32 id) {
    int slot = id / 3;
    if (slot >= 10) return;
    if (dc_gx_state_dedup && memcmp(g_gx.pos_mtx[slot], mtx, sizeof(float) * 12) == 0)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_MODELVIEW);
    memcpy(g_gx.pos_mtx[slot], mtx, sizeof(float) * 12);
}

void GXLoadNrmMtxImm(const void* mtx, u32 id) {
    int slot = id / 3;
    const float* src;
    float (*d)[3];
    if (slot >= 10) return;
    src = (const float*)mtx;
    d = g_gx.nrm_mtx[slot];
    /* Upper-left 3x3 of a 3x4 row-major Mtx (stride 4, not contiguous).
     *
     * ⚠️ Three 12-byte block moves, not nine scalar float compares and nine
     * scalar stores. The rows ARE contiguous on both sides — d[i][0..2] is 12 B
     * and src[0..2] / src[4..6] / src[8..10] are 12 B each — only the stride
     * between them differs, so memcmp/memcpy are byte-for-byte identical here.
     * Measured from the linked map, the scalar form compiled to 552 B / 276
     * instructions, against 148 B / 74 for GXLoadPosMtxImm doing the SAME job
     * on MORE data (a full 3x4) with memcmp/memcpy. 3.7x the cost for 75 % of
     * the payload, on a function emu64 calls once per G_MTX command
     * (emu64.c:4603). */
#ifdef DC_GX_NO_FASTPATH
    if (dc_gx_state_dedup &&
        d[0][0] == src[0] && d[0][1] == src[1] && d[0][2] == src[2] &&
        d[1][0] == src[4] && d[1][1] == src[5] && d[1][2] == src[6] &&
        d[2][0] == src[8] && d[2][1] == src[9] && d[2][2] == src[10])
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_MODELVIEW);
    d[0][0] = src[0]; d[0][1] = src[1]; d[0][2] = src[2];
    d[1][0] = src[4]; d[1][1] = src[5]; d[1][2] = src[6];
    d[2][0] = src[8]; d[2][1] = src[9]; d[2][2] = src[10];
#else
    if (dc_gx_state_dedup &&
        memcmp(d[0], src + 0, sizeof(float) * 3) == 0 &&
        memcmp(d[1], src + 4, sizeof(float) * 3) == 0 &&
        memcmp(d[2], src + 8, sizeof(float) * 3) == 0)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_MODELVIEW);
    memcpy(d[0], src + 0, sizeof(float) * 3);
    memcpy(d[1], src + 4, sizeof(float) * 3);
    memcpy(d[2], src + 8, sizeof(float) * 3);
#endif
}

void GXLoadTexMtxImm(const void* mtx, u32 id, u32 type) {
    int slot = dc_tex_mtx_id_to_slot((int)id);
    (void)type;
    if (slot < 0 || slot >= 10) return;
    if (dc_gx_state_dedup && memcmp(g_gx.tex_mtx[slot], mtx, sizeof(float) * 12) == 0)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_TEXGEN);
    memcpy(g_gx.tex_mtx[slot], mtx, sizeof(float) * 12);
}

void GXSetCurrentMtx(u32 id) {
    u32 slot = id / 3;
    if (slot >= 10) return;
    if (dc_gx_state_dedup && g_gx.current_mtx == (int)slot) return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_MODELVIEW);
    g_gx.current_mtx = (int)slot;
}

/* Shadowed exactly as on PC: re-setting an identical viewport skips both the
 * flush and the backend call. A REAL change must flush first, because the
 * viewport applies immediately and a buffered batch would draw with the new
 * one. */
static int   s_vp[4];
static float s_vp_depth[2];
static int   s_vp_valid = 0;
static int   s_sc[4];
static int   s_sc_valid = 0;

static void dc_gx_invalidate_vp_shadow(void) {
    s_vp_valid = 0;
    s_sc_valid = 0;
}

void GXSetViewport(f32 left, f32 top, f32 wd, f32 ht, f32 nearz, f32 farz) {
    int x, y, w, h;

    g_gx.viewport[0] = left;
    g_gx.viewport[1] = top;
    g_gx.viewport[2] = wd;
    g_gx.viewport[3] = ht;
    g_gx.viewport[4] = nearz;
    g_gx.viewport[5] = farz;
    if (g_pc_frameskip_active) return;

    /* GX is Y-down; the PVR framebuffer is Y-down too, so unlike the GL path
     * there is no flip here. If the image comes out upside down, this is the
     * line — not the projection matrix. */
    x = (int)left;
    y = (int)top;
    w = (int)wd;
    h = (int)ht;

    if (dc_gx_state_dedup && s_vp_valid &&
        x == s_vp[0] && y == s_vp[1] && w == s_vp[2] && h == s_vp[3] &&
        nearz == s_vp_depth[0] && farz == s_vp_depth[1])
        return;

    dc_gx_flush_if_begin_complete();
    if (s_flush_pending_attr) {
        pc_gx_flush_reason[17]++;
        s_flush_pending_attr = 0;
    }
    dc_gx_backend_set_viewport(x, y, w, h, nearz, farz);
    s_vp[0] = x; s_vp[1] = y; s_vp[2] = w; s_vp[3] = h;
    s_vp_depth[0] = nearz; s_vp_depth[1] = farz;
    s_vp_valid = 1;
}

void GXSetViewportJitter(f32 left, f32 top, f32 wd, f32 ht,
                         f32 nearz, f32 farz, u32 field) {
    (void)field;
    GXSetViewport(left, top, wd, ht, nearz, farz);
}

void GXSetScissor(u32 left, u32 top, u32 wd, u32 ht) {
    int x, y, w, h;

    g_gx.scissor[0] = (int)left;
    g_gx.scissor[1] = (int)top;
    g_gx.scissor[2] = (int)wd;
    g_gx.scissor[3] = (int)ht;
    if (g_pc_frameskip_active) return;

    x = (int)left; y = (int)top; w = (int)wd; h = (int)ht;

    if (dc_gx_state_dedup && s_sc_valid &&
        x == s_sc[0] && y == s_sc[1] && w == s_sc[2] && h == s_sc[3])
        return;

    dc_gx_flush_if_begin_complete();
    if (s_flush_pending_attr) {
        pc_gx_flush_reason[17]++;
        s_flush_pending_attr = 0;
    }
    dc_gx_backend_set_scissor(x, y, w, h);
    s_sc[0] = x; s_sc[1] = y; s_sc[2] = w; s_sc[3] = h;
    s_sc_valid = 1;
}

void GXSetScissorBoxOffset(s32 x, s32 y) { (void)x; (void)y; }
void GXSetClipMode(u32 mode) { (void)mode; }

void GXGetProjectionv(f32* p) {
    if (p) memcpy(p, g_gx.projection_mtx, sizeof(float) * 16);
}

/* ==========================================================================
 * TEV
 * ==========================================================================
 * This is the surface tev_map.md (M2) must classify: 101 unique configurations
 * were harvested from a full playthrough on the base port, max 3 stages. That
 * is a complete, finite spec of what Animal Crossing actually asks for. Every
 * setter below records state and marks dirty; the mapping to PVR fixed-function
 * modes happens in the backend, once.
 */
void GXSetNumTevStages(u8 nStages) {
    if (dc_gx_state_dedup && g_gx.num_tev_stages == (int)nStages) return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_TEV_STAGES);
    g_gx.num_tev_stages = nStages;
}

void GXSetTevOp(u32 stage, u32 mode) {
    /* No flush here — the GXSetTev* calls below flush iff they change state. */
    if (stage >= 16) return;
    /* TEV formula: out = (d + ((1-c)*a + c*b) + bias) * scale */
    switch (mode) {
    case GX_MODULATE:
        GXSetTevColorIn(stage, GX_CC_ZERO, GX_CC_TEXC, GX_CC_RASC, GX_CC_ZERO);
        GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
        break;
    case GX_DECAL:
        GXSetTevColorIn(stage, GX_CC_RASC, GX_CC_TEXC, GX_CC_TEXA, GX_CC_ZERO);
        GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
        break;
    case GX_BLEND:
        GXSetTevColorIn(stage, GX_CC_ONE, GX_CC_RASC, GX_CC_TEXC, GX_CC_ZERO);
        GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_TEXA, GX_CA_RASA, GX_CA_ZERO);
        break;
    case GX_REPLACE:
        GXSetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_TEXC);
        GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_TEXA);
        break;
    case GX_PASSCLR:
        GXSetTevColorIn(stage, GX_CC_ZERO, GX_CC_ZERO, GX_CC_ZERO, GX_CC_RASC);
        GXSetTevAlphaIn(stage, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_RASA);
        break;
    default:
        return;
    }
    GXSetTevColorOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
    GXSetTevAlphaOp(stage, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_TRUE, GX_TEVPREV);
}

void GXSetTevColorIn(u32 stage, u32 a, u32 b, u32 c, u32 d) {
    DCGXTevStage* ts;
    if (stage >= 16) return;
    ts = &g_gx.tev_stages[stage];
    if (dc_gx_state_dedup &&
        ts->color_a == (int)a && ts->color_b == (int)b &&
        ts->color_c == (int)c && ts->color_d == (int)d)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_TEV_STAGES);
    ts->color_a = (int)a; ts->color_b = (int)b;
    ts->color_c = (int)c; ts->color_d = (int)d;
}

void GXSetTevAlphaIn(u32 stage, u32 a, u32 b, u32 c, u32 d) {
    DCGXTevStage* ts;
    if (stage >= 16) return;
    ts = &g_gx.tev_stages[stage];
    if (dc_gx_state_dedup &&
        ts->alpha_a == (int)a && ts->alpha_b == (int)b &&
        ts->alpha_c == (int)c && ts->alpha_d == (int)d)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_TEV_STAGES);
    ts->alpha_a = (int)a; ts->alpha_b = (int)b;
    ts->alpha_c = (int)c; ts->alpha_d = (int)d;
}

void GXSetTevColorOp(u32 stage, u32 op, u32 bias, u32 scale, GXBool clamp, u32 out_reg) {
    DCGXTevStage* ts;
    if (stage >= 16) return;
    ts = &g_gx.tev_stages[stage];
    if (dc_gx_state_dedup &&
        ts->color_op == (int)op && ts->color_bias == (int)bias &&
        ts->color_scale == (int)scale && ts->color_clamp == (int)clamp &&
        ts->color_out == (int)out_reg)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_TEV_STAGES);
    ts->color_op = (int)op; ts->color_bias = (int)bias;
    ts->color_scale = (int)scale; ts->color_clamp = (int)clamp;
    ts->color_out = (int)out_reg;
}

void GXSetTevAlphaOp(u32 stage, u32 op, u32 bias, u32 scale, GXBool clamp, u32 out_reg) {
    DCGXTevStage* ts;
    if (stage >= 16) return;
    ts = &g_gx.tev_stages[stage];
    if (dc_gx_state_dedup &&
        ts->alpha_op == (int)op && ts->alpha_bias == (int)bias &&
        ts->alpha_scale == (int)scale && ts->alpha_clamp == (int)clamp &&
        ts->alpha_out == (int)out_reg)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_TEV_STAGES);
    ts->alpha_op = (int)op; ts->alpha_bias = (int)bias;
    ts->alpha_scale = (int)scale; ts->alpha_clamp = (int)clamp;
    ts->alpha_out = (int)out_reg;
}

void GXSetTevOrder(u32 stage, u32 coord, u32 map, u32 color) {
    DCGXTevStage* ts;
    if (stage >= 16) return;
    ts = &g_gx.tev_stages[stage];
    if (dc_gx_state_dedup &&
        ts->tex_coord == (int)coord && ts->tex_map == (int)map &&
        ts->color_chan == (int)color)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_TEV_STAGES | DC_GX_DIRTY_TEXTURES);
    ts->tex_coord = (int)coord;
    ts->tex_map = (int)map;
    ts->color_chan = (int)color;
}

void GXSetTevColor(u32 id, u32 color_packed) {
    float tmp[4];
    if (id >= GX_MAX_TEVREG) return;
    /* TEVREG0 comes from GXColor fields (byte unpack); the rest come from
     * EmuColor.raw (shift unpack). */
    if (id == GX_TEVREG0) dc_unpack_gxcolor_f(color_packed, tmp);
    else                  dc_unpack_rgba8f(color_packed, tmp);
    if (dc_gx_state_dedup && memcmp(tmp, g_gx.tev_colors[id], sizeof(tmp)) == 0)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_TEV_COLORS);
    memcpy(g_gx.tev_colors[id], tmp, sizeof(tmp));
}

void GXSetTevColorS10(u32 id, s16 r, s16 g, s16 b, s16 a) {
    float tmp[4];
    if (id >= GX_MAX_TEVREG) return;
    tmp[0] = r / 255.0f; tmp[1] = g / 255.0f;
    tmp[2] = b / 255.0f; tmp[3] = a / 255.0f;
    if (dc_gx_state_dedup && memcmp(tmp, g_gx.tev_colors[id], sizeof(tmp)) == 0)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_TEV_COLORS);
    memcpy(g_gx.tev_colors[id], tmp, sizeof(tmp));
}

void GXSetTevKColor(u32 id, u32 color_packed) {
    float tmp[4];
    if (id >= 4) return;
    dc_unpack_rgba8f(color_packed, tmp);
    if (dc_gx_state_dedup && memcmp(tmp, g_gx.tev_k_colors[id], sizeof(tmp)) == 0)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_KONST);
    memcpy(g_gx.tev_k_colors[id], tmp, sizeof(tmp));
}

void GXSetTevKColorSel(u32 stage, u32 sel) {
    if (stage >= 16) return;
    if (dc_gx_state_dedup && g_gx.tev_stages[stage].k_color_sel == (int)sel) return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_TEV_STAGES);
    g_gx.tev_stages[stage].k_color_sel = (int)sel;
}

void GXSetTevKAlphaSel(u32 stage, u32 sel) {
    if (stage >= 16) return;
    if (dc_gx_state_dedup && g_gx.tev_stages[stage].k_alpha_sel == (int)sel) return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_TEV_STAGES);
    g_gx.tev_stages[stage].k_alpha_sel = (int)sel;
}

void GXSetTevSwapMode(u32 stage, u32 ras_sel, u32 tex_sel) {
    DCGXTevStage* ts;
    if (stage >= 16) return;
    ts = &g_gx.tev_stages[stage];
    if (dc_gx_state_dedup &&
        ts->ras_swap == (int)ras_sel && ts->tex_swap == (int)tex_sel)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_TEV_STAGES);
    ts->ras_swap = (int)ras_sel;
    ts->tex_swap = (int)tex_sel;
}

void GXSetTevSwapModeTable(u32 table, u32 red, u32 green, u32 blue, u32 alpha) {
    DCGXTevSwapTable* st;
    if (table >= 4) return;
    st = &g_gx.tev_swap_table[table];
    if (dc_gx_state_dedup &&
        st->r == (int)red && st->g == (int)green &&
        st->b == (int)blue && st->a == (int)alpha)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_SWAP_TABLES);
    st->r = (int)red; st->g = (int)green;
    st->b = (int)blue; st->a = (int)alpha;
}

/* ==========================================================================
 * Alpha / blend / depth / cull
 * ==========================================================================
 * GXSetAlphaCompare + GXSetBlendMode together drive the PVR
 * Opaque / Punch-Through / Translucent list classification — the most
 * consequential single mapping decision in the renderer. The backend reads the
 * recorded state; nothing is decided here.
 */
void GXSetAlphaCompare(u32 comp0, u8 ref0, u32 op, u32 comp1, u8 ref1) {
    if (dc_gx_state_dedup &&
        g_gx.alpha_comp0 == (int)comp0 && g_gx.alpha_ref0 == (int)ref0 &&
        g_gx.alpha_op == (int)op &&
        g_gx.alpha_comp1 == (int)comp1 && g_gx.alpha_ref1 == (int)ref1)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_ALPHA_CMP);
    g_gx.alpha_comp0 = (int)comp0;
    g_gx.alpha_ref0 = (int)ref0;
    g_gx.alpha_op = (int)op;
    g_gx.alpha_comp1 = (int)comp1;
    g_gx.alpha_ref1 = (int)ref1;
}

void GXSetBlendMode(u32 type, u32 src, u32 dst, u32 logic_op) {
    if (dc_gx_state_dedup &&
        g_gx.blend_mode == (int)type && g_gx.blend_src == (int)src &&
        g_gx.blend_dst == (int)dst && g_gx.blend_logic_op == (int)logic_op)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_BLEND);
    g_gx.blend_mode = (int)type;
    g_gx.blend_src = (int)src;
    g_gx.blend_dst = (int)dst;
    g_gx.blend_logic_op = (int)logic_op;
}

void GXSetZMode(GXBool compare_enable, u32 func, GXBool update_enable) {
    if (dc_gx_state_dedup &&
        g_gx.z_compare_enable == (int)compare_enable &&
        g_gx.z_compare_func == (int)func &&
        g_gx.z_update_enable == (int)update_enable)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_DEPTH);
    g_gx.z_compare_enable = (int)compare_enable;
    g_gx.z_compare_func = (int)func;
    g_gx.z_update_enable = (int)update_enable;
}

void GXSetColorUpdate(GXBool enable) {
    if (dc_gx_state_dedup && g_gx.color_update_enable == (int)enable) return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_COLOR_MASK);
    g_gx.color_update_enable = (int)enable;
}

void GXSetAlphaUpdate(GXBool enable) {
    if (dc_gx_state_dedup && g_gx.alpha_update_enable == (int)enable) return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_COLOR_MASK);
    g_gx.alpha_update_enable = (int)enable;
}

void GXSetZCompLoc(GXBool before_tex) { (void)before_tex; }
void GXSetDither(GXBool dither) { (void)dither; }
void GXSetDstAlpha(GXBool enable, u8 alpha) { (void)enable; (void)alpha; }
void GXSetFieldMask(GXBool odd, GXBool even) { (void)odd; (void)even; }
void GXSetFieldMode(GXBool field_mode, GXBool half_aspect) {
    (void)field_mode; (void)half_aspect;
}
void GXSetPixelFmt(u32 pix_fmt, u32 z_fmt) { (void)pix_fmt; (void)z_fmt; }

void GXSetCullMode(u32 mode) {
    int final_mode = g_pc_model_viewer_no_cull ? GX_CULL_NONE : (int)mode;
    if (dc_gx_state_dedup && g_gx.cull_mode == final_mode) return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_CULL);
    /* NOTE for the backend: GX cull sense is opposite to GL's in places, and
     * PVR has its own culling-mode enum again. Verify against the PC
     * implementation before trusting any direct mapping. */
    g_gx.cull_mode = final_mode;
}
void GXSetCoPlanar(GXBool enable) { (void)enable; }

/* PVR table fog is a near-native fit for GX fog (PLAN §3.3). */
void GXSetFog(u32 type, f32 startz, f32 endz, f32 nearz, f32 farz, GXColor color) {
    float fc[4];
    fc[0] = color.r / 255.0f; fc[1] = color.g / 255.0f;
    fc[2] = color.b / 255.0f; fc[3] = color.a / 255.0f;
    if (dc_gx_state_dedup &&
        g_gx.fog_type == (int)type &&
        g_gx.fog_start == startz && g_gx.fog_end == endz &&
        g_gx.fog_near == nearz && g_gx.fog_far == farz &&
        memcmp(fc, g_gx.fog_color, sizeof(fc)) == 0)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_FOG);
    g_gx.fog_type = (int)type;
    g_gx.fog_start = startz;
    g_gx.fog_end = endz;
    g_gx.fog_near = nearz;
    g_gx.fog_far = farz;
    memcpy(g_gx.fog_color, fc, sizeof(fc));
}

void GXInitFogAdjTable(void* table, u16 width, f32 projmtx[4][4]) {
    (void)table; (void)width; (void)projmtx;
}
void GXSetFogRangeAdj(GXBool enable, u16 center, void* table) {
    (void)enable; (void)center; (void)table;
}

/* ==========================================================================
 * Lighting
 * ==========================================================================
 * GameCube does 8 lights with angular + distance attenuation in the transform
 * unit. The DC has no T&L, so this runs on SH-4 in the backend (PLAN §3.3:
 * bake what is static, FIPR the rest, clamp the light count to measured usage).
 * All the attenuation-coefficient math below is Dolphin's own and is pure
 * scalar C — it ports unchanged.
 */
static int dc_gx_chan_index(u32 chan) {
    switch (chan) {
        case GX_COLOR0: case GX_ALPHA0: case GX_COLOR0A0: return 0;
        case GX_COLOR1: case GX_ALPHA1: case GX_COLOR1A1: return 1;
        default: return -1;
    }
}

void GXSetNumChans(u8 nChans) {
    if (dc_gx_state_dedup && g_gx.num_chans == (int)nChans) return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_LIGHTING);
    g_gx.num_chans = nChans;
}

static int dc_gx_chan_ctrl_same(int k, GXBool enable, u32 amb_src, u32 mat_src,
                                u32 light_mask, u32 diff_fn, u32 attn_fn) {
    return g_gx.chan_ctrl_enable[k] == (int)enable &&
           g_gx.chan_ctrl_amb_src[k] == (int)amb_src &&
           g_gx.chan_ctrl_mat_src[k] == (int)mat_src &&
           g_gx.chan_ctrl_light_mask[k] == (int)light_mask &&
           g_gx.chan_ctrl_diff_fn[k] == (int)diff_fn &&
           g_gx.chan_ctrl_attn_fn[k] == (int)attn_fn;
}

void GXSetChanCtrl(u32 chan, GXBool enable, u32 amb_src, u32 mat_src,
                   u32 light_mask, u32 diff_fn, u32 attn_fn) {
    int idx = dc_gx_chan_index(chan);
    int is_combined, is_alpha;
    if (idx < 0) return;
    is_combined = (chan >= GX_COLOR0A0);
    is_alpha = (chan == GX_ALPHA0 || chan == GX_ALPHA1);

    if (dc_gx_state_dedup &&
        ((is_alpha && !is_combined) ||
         dc_gx_chan_ctrl_same(idx * 2, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn)) &&
        ((!is_alpha && !is_combined) ||
         dc_gx_chan_ctrl_same(idx * 2 + 1, enable, amb_src, mat_src, light_mask, diff_fn, attn_fn)))
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_LIGHTING);

    if (!is_alpha || is_combined) {
        g_gx.chan_ctrl_enable[idx * 2] = (int)enable;
        g_gx.chan_ctrl_amb_src[idx * 2] = (int)amb_src;
        g_gx.chan_ctrl_mat_src[idx * 2] = (int)mat_src;
        g_gx.chan_ctrl_light_mask[idx * 2] = (int)light_mask;
        g_gx.chan_ctrl_diff_fn[idx * 2] = (int)diff_fn;
        g_gx.chan_ctrl_attn_fn[idx * 2] = (int)attn_fn;
    }
    if (is_alpha || is_combined) {
        g_gx.chan_ctrl_enable[idx * 2 + 1] = (int)enable;
        g_gx.chan_ctrl_amb_src[idx * 2 + 1] = (int)amb_src;
        g_gx.chan_ctrl_mat_src[idx * 2 + 1] = (int)mat_src;
        g_gx.chan_ctrl_light_mask[idx * 2 + 1] = (int)light_mask;
        g_gx.chan_ctrl_diff_fn[idx * 2 + 1] = (int)diff_fn;
        g_gx.chan_ctrl_attn_fn[idx * 2 + 1] = (int)attn_fn;
    }
}

void GXSetChanAmbColor(u32 chan, u32 color_packed) {
    int idx = dc_gx_chan_index(chan);
    float tmp[4];
    if (idx < 0 || idx >= 2) return;
    dc_unpack_gxcolor_f(color_packed, tmp);
    if (dc_gx_state_dedup && memcmp(tmp, g_gx.chan_amb_color[idx], sizeof(tmp)) == 0)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_LIGHTING);
    memcpy(g_gx.chan_amb_color[idx], tmp, sizeof(tmp));
}

void GXSetChanMatColor(u32 chan, u32 color_packed) {
    int idx = dc_gx_chan_index(chan);
    float tmp[4];
    if (idx < 0 || idx >= 2) return;
    dc_unpack_gxcolor_f(color_packed, tmp);
    if (dc_gx_state_dedup && memcmp(tmp, g_gx.chan_mat_color[idx], sizeof(tmp)) == 0)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_LIGHTING);
    memcpy(g_gx.chan_mat_color[idx], tmp, sizeof(tmp));
}

/* GXLightObj internal layout, from GXPriv.h. The GAME allocates these. */
typedef struct {
    u32 padding[3];
    u32 color;
    f32 a0, a1, a2;
    f32 k0, k1, k2;
    f32 px, py, pz;
    f32 nx, ny, nz;
} DCGXLightObjInternal;

void GXInitLightSpot(void* lt, f32 cutoff, u32 spot_func) {
    DCGXLightObjInternal* l = (DCGXLightObjInternal*)lt;
    f32 a0, a1, a2, r, cr, d;

    if (cutoff <= 0.0f || cutoff > 90.0f) spot_func = GX_SP_OFF;

    r = DC_PIf * cutoff / 180.0f;
    cr = cosf(r);
    switch (spot_func) {
    case GX_SP_FLAT:  a0 = -1000.0f * cr; a1 = 1000.0f; a2 = 0.0f; break;
    case GX_SP_COS:   a0 = -cr / (1.0f - cr); a1 = 1.0f / (1.0f - cr); a2 = 0.0f; break;
    case GX_SP_COS2:  a0 = 0.0f; a1 = -cr / (1.0f - cr); a2 = 1.0f / (1.0f - cr); break;
    case GX_SP_SHARP:
        d = (1.0f - cr) * (1.0f - cr);
        a0 = (cr * (cr - 2.0f)) / d; a1 = 2.0f / d; a2 = -1.0f / d;
        break;
    case GX_SP_RING1:
        d = (1.0f - cr) * (1.0f - cr);
        a0 = (-4.0f * cr) / d; a1 = (4.0f * (1.0f + cr)) / d; a2 = -4.0f / d;
        break;
    case GX_SP_RING2:
        d = (1.0f - cr) * (1.0f - cr);
        a0 = 1.0f - ((2.0f * cr * cr) / d); a1 = (4.0f * cr) / d; a2 = -2.0f / d;
        break;
    case GX_SP_OFF:
    default: a0 = 1.0f; a1 = 0.0f; a2 = 0.0f; break;
    }
    l->a0 = a0; l->a1 = a1; l->a2 = a2;
}

void GXInitLightDistAttn(void* lt, f32 ref_dist, f32 ref_bright, u32 dist_func) {
    DCGXLightObjInternal* l = (DCGXLightObjInternal*)lt;
    f32 k0, k1, k2;

    if (ref_dist < 0.0f) dist_func = GX_DA_OFF;
    if (ref_bright <= 0.0f || ref_bright >= 1.0f) dist_func = GX_DA_OFF;

    switch (dist_func) {
    case GX_DA_GENTLE:
        k0 = 1.0f; k1 = (1.0f - ref_bright) / (ref_bright * ref_dist); k2 = 0.0f;
        break;
    case GX_DA_MEDIUM:
        k0 = 1.0f;
        k1 = (0.5f * (1.0f - ref_bright)) / (ref_bright * ref_dist);
        k2 = (0.5f * (1.0f - ref_bright)) / (ref_bright * ref_dist * ref_dist);
        break;
    case GX_DA_STEEP:
        k0 = 1.0f; k1 = 0.0f;
        k2 = (1.0f - ref_bright) / (ref_bright * ref_dist * ref_dist);
        break;
    case GX_DA_OFF:
    default: k0 = 1.0f; k1 = 0.0f; k2 = 0.0f; break;
    }
    l->k0 = k0; l->k1 = k1; l->k2 = k2;
}

void GXInitLightPos(void* lt, f32 x, f32 y, f32 z) {
    DCGXLightObjInternal* l = (DCGXLightObjInternal*)lt;
    l->px = x; l->py = y; l->pz = z;
}
void GXInitLightDir(void* lt, f32 nx, f32 ny, f32 nz) {
    DCGXLightObjInternal* l = (DCGXLightObjInternal*)lt;
    l->nx = nx; l->ny = ny; l->nz = nz;
}
void GXInitLightColor(void* lt, u32 color) {
    ((DCGXLightObjInternal*)lt)->color = color;
}
void GXInitLightAttn(void* lt, f32 a0, f32 a1, f32 a2, f32 k0, f32 k1, f32 k2) {
    DCGXLightObjInternal* l = (DCGXLightObjInternal*)lt;
    l->a0 = a0; l->a1 = a1; l->a2 = a2;
    l->k0 = k0; l->k1 = k1; l->k2 = k2;
}
void GXInitLightAttnA(void* lt, f32 a0, f32 a1, f32 a2) {
    DCGXLightObjInternal* l = (DCGXLightObjInternal*)lt;
    l->a0 = a0; l->a1 = a1; l->a2 = a2;
}
void GXInitLightAttnK(void* lt, f32 k0, f32 k1, f32 k2) {
    DCGXLightObjInternal* l = (DCGXLightObjInternal*)lt;
    l->k0 = k0; l->k1 = k1; l->k2 = k2;
}

void GXLoadLightObjImm(void* lt, u32 light) {
    DCGXLightObjInternal* l = (DCGXLightObjInternal*)lt;
    float col[4];
    int slot = -1, i;

    for (i = 0; i < 8; i++)
        if (light == (1u << i)) { slot = i; break; }
    if (slot < 0) return;

    dc_unpack_gxcolor_f(l->color, col);
    if (dc_gx_state_dedup &&
        g_gx.lights[slot].pos[0] == l->px && g_gx.lights[slot].pos[1] == l->py &&
        g_gx.lights[slot].pos[2] == l->pz &&
        g_gx.lights[slot].dir[0] == l->nx && g_gx.lights[slot].dir[1] == l->ny &&
        g_gx.lights[slot].dir[2] == l->nz &&
        g_gx.lights[slot].a0 == l->a0 && g_gx.lights[slot].a1 == l->a1 &&
        g_gx.lights[slot].a2 == l->a2 &&
        g_gx.lights[slot].k0 == l->k0 && g_gx.lights[slot].k1 == l->k1 &&
        g_gx.lights[slot].k2 == l->k2 &&
        memcmp(col, g_gx.lights[slot].color, sizeof(col)) == 0)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_LIGHTING);

    g_gx.lights[slot].pos[0] = l->px;
    g_gx.lights[slot].pos[1] = l->py;
    g_gx.lights[slot].pos[2] = l->pz;
    g_gx.lights[slot].dir[0] = l->nx;
    g_gx.lights[slot].dir[1] = l->ny;
    g_gx.lights[slot].dir[2] = l->nz;
    g_gx.lights[slot].a0 = l->a0;
    g_gx.lights[slot].a1 = l->a1;
    g_gx.lights[slot].a2 = l->a2;
    g_gx.lights[slot].k0 = l->k0;
    g_gx.lights[slot].k1 = l->k1;
    g_gx.lights[slot].k2 = l->k2;
    memcpy(g_gx.lights[slot].color, col, sizeof(col));
}

void GXGetLightPos(void* lt, f32* x, f32* y, f32* z) {
    DCGXLightObjInternal* l = (DCGXLightObjInternal*)lt;
    *x = l->px; *y = l->py; *z = l->pz;
}
void GXGetLightColor(void* lt, void* color) {
    DCGXLightObjInternal* l = (DCGXLightObjInternal*)lt;
    memcpy(color, &l->color, 4);
}

/* ==========================================================================
 * Texture coordinate generation
 * ========================================================================== */
void GXSetNumTexGens(u8 n) {
    if (dc_gx_state_dedup && g_gx.num_tex_gens == (int)n) return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_TEXGEN);
    g_gx.num_tex_gens = n;
}

void GXSetTexCoordGen2(u32 dst, u32 func, u32 src, u32 mtx,
                       GXBool normalize, u32 postmtx) {
    (void)normalize; (void)postmtx;
    if (dst >= 8) return;
    if (dc_gx_state_dedup &&
        g_gx.tex_gen_type[dst] == (int)func &&
        g_gx.tex_gen_src[dst] == (int)src &&
        g_gx.tex_gen_mtx[dst] == (int)mtx)
        return;
    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_TEXGEN);
    g_gx.tex_gen_type[dst] = (int)func;
    g_gx.tex_gen_src[dst] = (int)src;
    g_gx.tex_gen_mtx[dst] = (int)mtx;
}

void GXSetLineWidth(u8 width, u32 texOffsets) { (void)width; (void)texOffsets; }
void GXSetPointSize(u8 size, u32 texOffsets)  { (void)size; (void)texOffsets; }
void GXEnableTexOffsets(u32 coord, GXBool line, GXBool point) {
    (void)coord; (void)line; (void)point;
}
void GXSetTexCoordScaleManually(u32 coord, GXBool enable, u16 ss, u16 ts) {
    (void)coord; (void)enable; (void)ss; (void)ts;
}
void GXSetTexCoordBias(u32 coord, u8 s, u8 t) { (void)coord; (void)s; (void)t; }

/* ==========================================================================
 * Framebuffer / copy
 * ========================================================================== */
void GXSetCopyClear(GXColor clear_clr, u32 clear_z) {
    g_gx.clear_color[0] = clear_clr.r / 255.0f;
    g_gx.clear_color[1] = clear_clr.g / 255.0f;
    g_gx.clear_color[2] = clear_clr.b / 255.0f;
    g_gx.clear_color[3] = clear_clr.a / 255.0f;
    g_gx.clear_depth = clear_z / (float)0x00FFFFFF;
}

/* The frame is presented from dc_vi.c (VIWaitForRetrace), not here — same
 * split as the base port. GXCopyDisp only terminates the open batch. */
void GXCopyDisp(void* dest, GXBool clear) {
    (void)dest; (void)clear;
    dc_gx_commit_pending_and_flush();
}

void GXSetDispCopyGamma(u32 gamma) { (void)gamma; }
void GXSetDispCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
    g_gx.copy_src[0] = left; g_gx.copy_src[1] = top;
    g_gx.copy_src[2] = wd;   g_gx.copy_src[3] = ht;
}
void GXSetDispCopyDst(u16 wd, u16 ht) { g_gx.copy_dst[0] = wd; g_gx.copy_dst[1] = ht; }
f32  GXGetYScaleFactor(u16 efbHeight, u16 xfbHeight) {
    return (f32)xfbHeight / (f32)efbHeight;
}
u32  GXSetDispCopyYScale(f32 vscale) { return (u32)(vscale * 256.0f); }
u16  GXGetNumXfbLines(u16 efbHeight, f32 yScale) { return (u16)(efbHeight * yScale); }
void GXSetCopyFilter(GXBool aa, const void* pattern, GXBool vf, const void* vfilter) {
    (void)aa; (void)pattern; (void)vf; (void)vfilter;
}
void GXAdjustForOverscan(void* rmin, void* rmout, u16 hor, u16 ver) {
    (void)hor; (void)ver;
    memcpy(rmout, rmin, sizeof(u32) * 16);
}

void GXSetTexCopySrc(u16 left, u16 top, u16 wd, u16 ht) {
    g_gx.tex_copy_src[0] = left; g_gx.tex_copy_src[1] = top;
    g_gx.tex_copy_src[2] = wd;   g_gx.tex_copy_src[3] = ht;
}
void GXSetTexCopyDst(u16 wd, u16 ht, u32 fmt, GXBool mipmap) {
    g_gx.tex_copy_dst[0] = wd;
    g_gx.tex_copy_dst[1] = ht;
    g_gx.tex_copy_fmt = fmt;
    g_gx.tex_copy_mipmap = mipmap ? 1 : 0;
}

/* EFB capture. PLAN §3.3: PVR render-to-texture exists but is costly;
 * enumerate the actual call sites in AC first (the NES/TV screen and a few
 * transition effects) and handle them per-case. Until then this is a loud
 * no-op — the affected surfaces render stale/blank rather than crashing. */
void GXCopyTex(void* dest, GXBool clear) {
    (void)dest; (void)clear;
    dc_gx_commit_pending_and_flush();
    DC_UNIMPLEMENTED_NOTE("EFB->texture capture: enumerate AC call sites, then "
                          "PVR render-to-texture or pre-rendered assets");
}
void GXSetCopyClamp(u32 clamp) { (void)clamp; }

/* ==========================================================================
 * Init / sync
 * ========================================================================== */
void* GXInit(void* base, u32 size) {
    /* Called from JUTGraphFifo.cpp with a JKRHeap-allocated 0x10001-byte FIFO
     * buffer (JW_Init sets FifoBufSize). There is no command FIFO on DC, so
     * that allocation is dead weight we can reclaim later — logged so the
     * number shows up in the M2 memory work. */
    DC_LOG("[DC/GX] GXInit(base=%p size=%u) — FIFO unused on PVR\n",
           base, (unsigned)size);
    /* §1 ordering rule 4: GXInit happens INSIDE JFWSystem::init(), i.e. after
     * the heap exists. dc_gx_init() runs earlier, from dc_platform_init(). */
    return base;
}

void GXSetMisc(u32 token, u32 val) { (void)token; (void)val; }
/* Maps to nothing on a deferred tile renderer. */
void GXFlush(void) { }
void GXResetWriteGatherPipe(void) { }
void GXAbortFrame(void) { }
void GXSetDrawSync(u16 token) { (void)token; }
u16  GXReadDrawSync(void) { return 0; }
void GXSetDrawDone(void) { }
void GXWaitDrawDone(void) { }
/* No-op on PC; on DC this becomes a real pvr_wait_ready() scene sync point
 * once the backend lands. */
void GXDrawDone(void) { }
void GXPixModeSync(void) { }
void GXTexModeSync(void) { }
void* GXSetDrawSyncCallback(void* cb) { (void)cb; return NULL; }
void* GXSetDrawDoneCallback(void* cb) { (void)cb; return NULL; }

/* ==========================================================================
 * FIFO — the GameCube command FIFO has no DC analogue. All inert, as on PC.
 * ========================================================================== */
typedef struct { u8 pad[128]; } GXFifoObj;
void GXInitFifoBase(GXFifoObj* f, void* base, u32 size) { (void)f; (void)base; (void)size; }
void GXInitFifoPtrs(GXFifoObj* f, void* rp, void* wp) { (void)f; (void)rp; (void)wp; }
void GXInitFifoLimits(GXFifoObj* f, u32 hi, u32 lo) { (void)f; (void)hi; (void)lo; }
void GXSetCPUFifo(GXFifoObj* f) { (void)f; }
void GXSetGPFifo(GXFifoObj* f) { (void)f; }
void GXSaveCPUFifo(GXFifoObj* f) { (void)f; }
void GXSaveGPFifo(GXFifoObj* f) { (void)f; }
void GXGetGPStatus(GXBool* a, GXBool* b, GXBool* c, GXBool* d, GXBool* e) {
    if (a) *a = 0; if (b) *b = 0; if (c) *c = 1; if (d) *d = 1; if (e) *e = 0;
}
void GXGetFifoStatus(GXFifoObj* f, GXBool* a, GXBool* b, u32* c,
                     GXBool* d, GXBool* e, GXBool* g) {
    (void)f;
    if (a) *a = 0; if (b) *b = 0; if (c) *c = 0;
    if (d) *d = 0; if (e) *e = 0; if (g) *g = 0;
}
void GXGetFifoPtrs(GXFifoObj* f, void** rp, void** wp) {
    (void)f; if (rp) *rp = NULL; if (wp) *wp = NULL;
}
void* GXGetFifoBase(GXFifoObj* f) { (void)f; return NULL; }
u32   GXGetFifoSize(GXFifoObj* f) { (void)f; return 0; }
void  GXGetFifoLimits(GXFifoObj* f, u32* hi, u32* lo) {
    (void)f; if (hi) *hi = 0; if (lo) *lo = 0;
}
void* GXSetBreakPtCallback(void* cb) { (void)cb; return NULL; }
void  GXEnableBreakPt(void* bp) { (void)bp; }
void  GXDisableBreakPt(void) { }
void* GXSetCurrentGXThread(void) { return NULL; }
void* GXGetCurrentGXThread(void) { return NULL; }
GXFifoObj* GXGetCPUFifo(void) { static GXFifoObj f; return &f; }
GXFifoObj* GXGetGPFifo(void)  { static GXFifoObj f; return &f; }
u32 GXGetOverflowCount(void) { return 0; }
u32 GXResetOverflowCount(void) { return 0; }
volatile void* GXRedirectWriteGatherPipe(void* ptr) { return ptr; }
void GXRestoreWriteGatherPipe(void) { }
int  IsWriteGatherBufferEmpty(void) { return 1; }

/* ==========================================================================
 * Display lists
 * ==========================================================================
 * The PC port records only the EFB-copy ops into a private mini-format and
 * replays them. With GXCopyTex stubbed there is nothing worth recording yet,
 * so recording is inert and GXEndDisplayList returns 0 bytes — which the game
 * treats as "empty list" and skips. Revisit together with EFB capture.
 */
void GXBeginDisplayList(void* list, u32 size) {
    (void)list; (void)size;
    DC_UNIMPLEMENTED_NOTE("display-list recording (only EFB-copy ops on PC); "
                          "revisit with GXCopyTex");
}
u32  GXEndDisplayList(void) { return 0; }
void GXCallDisplayList(void* list, u32 nbytes) { (void)list; (void)nbytes; }

/* ==========================================================================
 * Indirect textures — PVR has no equivalent. Recorded so the M2 census can
 * report whether AC uses them at all (open question PLAN §11.3).
 * ========================================================================== */
void GXSetTevDirect(u32 stage) {
    GXSetTevIndirect(stage, 0, 0, 0, 0, 0, 0, 0, 0, 0);
}
void GXSetNumIndStages(u8 n) {
    DIRTY(DC_GX_DIRTY_INDIRECT);
    g_gx.num_ind_stages = n;
}
void GXSetIndTexMtx(u32 mtx_sel, const void* offset, s8 scale) {
    const float* mtx;
    int id;
    DIRTY(DC_GX_DIRTY_INDIRECT);
    switch (mtx_sel) {
        case 1: case 2: case 3:   id = (int)mtx_sel - 1; break;
        case 5: case 6: case 7:   id = (int)mtx_sel - 5; break;
        case 9: case 10: case 11: id = (int)mtx_sel - 9; break;
        default: return;
    }
    if (id < 0 || id >= 3) return;
    mtx = (const float*)offset;
    g_gx.ind_mtx[id][0][0] = mtx[0];
    g_gx.ind_mtx[id][0][1] = mtx[1];
    g_gx.ind_mtx[id][0][2] = mtx[2];
    g_gx.ind_mtx[id][1][0] = mtx[3];
    g_gx.ind_mtx[id][1][1] = mtx[4];
    g_gx.ind_mtx[id][1][2] = mtx[5];
    g_gx.ind_mtx_scale[id] = scale;
}
void GXSetIndTexOrder(u32 ind_stage, u32 tex_coord, u32 tex_map) {
    DIRTY(DC_GX_DIRTY_INDIRECT);
    if (ind_stage >= 4) return;
    g_gx.ind_order[ind_stage].tex_coord = (int)tex_coord;
    g_gx.ind_order[ind_stage].tex_map = (int)tex_map;
}
void GXSetTevIndirect(u32 stage, u32 ind_stage, u32 fmt, u32 bias_sel,
                      u32 mtx_sel, u32 wrap_s, u32 wrap_t, GXBool add_prev,
                      GXBool ind_lod, u32 alpha_sel) {
    DCGXTevStage* s;
    DIRTY(DC_GX_DIRTY_INDIRECT);
    if (stage >= 16) return;
    s = &g_gx.tev_stages[stage];
    s->ind_stage = (int)ind_stage;
    s->ind_format = (int)fmt;
    s->ind_bias = (int)bias_sel;
    s->ind_mtx = (int)mtx_sel;
    s->ind_wrap_s = (int)wrap_s;
    s->ind_wrap_t = (int)wrap_t;
    s->ind_add_prev = (int)add_prev;
    s->ind_lod = (int)ind_lod;
    s->ind_alpha = (int)alpha_sel;
}
void GXSetTevIndWarp(u32 stage, u32 ind_stage, GXBool signed_ofs,
                     GXBool replace, u32 mtx_sel) {
    u32 wrap = replace ? 6u : 0u;      /* GX_ITW_0 : GX_ITW_OFF  */
    u32 bias = signed_ofs ? 7u : 0u;   /* GX_ITB_STU : GX_ITB_NONE */
    GXSetTevIndirect(stage, ind_stage, 0, bias, mtx_sel, wrap, wrap, 0, 0, 0);
}
void GXSetIndTexCoordScale(u32 ind_stage, u32 scale_s, u32 scale_t) {
    DIRTY(DC_GX_DIRTY_INDIRECT);
    if (ind_stage >= 4) return;
    g_gx.ind_order[ind_stage].scale_s = (int)scale_s;
    g_gx.ind_order[ind_stage].scale_t = (int)scale_t;
}
void __GXSetIndirectMask(u32 mask) { (void)mask; }
void GXSetZTexture(u32 op, u32 fmt, u32 bias) { (void)op; (void)fmt; (void)bias; }

void GXDrawSphere(u8 numMajor, u8 numMinor) { (void)numMajor; (void)numMinor; }

void GXReadXfRasMetric(u32* xf_wait_in, u32* xf_wait_out, u32* ras_busy, u32* clocks) {
    if (xf_wait_in) *xf_wait_in = 0;
    if (xf_wait_out) *xf_wait_out = 0;
    if (ras_busy) *ras_busy = 0;
    if (clocks) *clocks = 0;
}
void  GXSetVerifyLevel(u32 level) { (void)level; }
void* GXSetVerifyCallback(void* cb) { (void)cb; return NULL; }

/* ==========================================================================
 * GXTexObj / GXTlutObj
 * ==========================================================================
 * Layout copied EXACTLY from pc_gx_texture.c: the game allocates these structs
 * and the offsets are part of the contract. 22 u32 for a GXTexObj, 4 for a
 * GXTlutObj. Do not reorder.
 */
#define TEXOBJ_IMAGE_PTR   0
#define TEXOBJ_WIDTH       1
#define TEXOBJ_HEIGHT      2
#define TEXOBJ_FORMAT      3
#define TEXOBJ_WRAP_S      4
#define TEXOBJ_WRAP_T      5
#define TEXOBJ_MIPMAP      6
#define TEXOBJ_MIN_FILTER  7
#define TEXOBJ_MAG_FILTER  8
#define TEXOBJ_MIN_LOD     9
#define TEXOBJ_MAX_LOD     10
#define TEXOBJ_LOD_BIAS    11
#define TEXOBJ_BIAS_CLAMP  12
#define TEXOBJ_EDGE_LOD    13
#define TEXOBJ_MAX_ANISO   14
#define TEXOBJ_BACKEND_TEX 15   /* was TEXOBJ_GL_TEX */
#define TEXOBJ_CI_FORMAT   16
#define TEXOBJ_TLUT_NAME   17
#define TEXOBJ_SIZE        22

#define TLUTOBJ_DATA       0
#define TLUTOBJ_FORMAT     1
#define TLUTOBJ_N_ENTRIES  2

void GXInitTexObj(void* obj, void* image_ptr, u16 width, u16 height, u32 format,
                  u32 wrap_s, u32 wrap_t, u8 mipmap) {
    u32* o = (u32*)obj;
    memset(o, 0, TEXOBJ_SIZE * sizeof(u32));
    o[TEXOBJ_IMAGE_PTR] = (u32)(uintptr_t)image_ptr;
    o[TEXOBJ_WIDTH] = width;
    o[TEXOBJ_HEIGHT] = height;
    o[TEXOBJ_FORMAT] = format;
    o[TEXOBJ_WRAP_S] = wrap_s;
    o[TEXOBJ_WRAP_T] = wrap_t;
    o[TEXOBJ_MIPMAP] = mipmap;
    o[TEXOBJ_MIN_FILTER] = 1;  /* GX_LINEAR */
    o[TEXOBJ_MAG_FILTER] = 1;
}

void GXInitTexObjCI(void* obj, void* image_ptr, u16 width, u16 height, u32 format,
                    u32 wrap_s, u32 wrap_t, u8 mipmap, u32 tlut_name) {
    u32* o;
    GXInitTexObj(obj, image_ptr, width, height, format, wrap_s, wrap_t, mipmap);
    o = (u32*)obj;
    o[TEXOBJ_CI_FORMAT] = format;
    o[TEXOBJ_TLUT_NAME] = tlut_name;
}

void GXInitTexObjData(void* obj, void* image_ptr) {
    ((u32*)obj)[TEXOBJ_IMAGE_PTR] = (u32)(uintptr_t)image_ptr;
}

void GXInitTexObjLOD(void* obj, u32 min_filt, u32 mag_filt, f32 min_lod, f32 max_lod,
                     f32 lod_bias, GXBool bias_clamp, GXBool edge_lod, u32 max_aniso) {
    u32* o = (u32*)obj;
    o[TEXOBJ_MIN_FILTER] = min_filt;
    o[TEXOBJ_MAG_FILTER] = mag_filt;
    memcpy(&o[TEXOBJ_MIN_LOD], &min_lod, sizeof(f32));
    memcpy(&o[TEXOBJ_MAX_LOD], &max_lod, sizeof(f32));
    memcpy(&o[TEXOBJ_LOD_BIAS], &lod_bias, sizeof(f32));
    o[TEXOBJ_BIAS_CLAMP] = bias_clamp;
    o[TEXOBJ_EDGE_LOD] = edge_lod;
    o[TEXOBJ_MAX_ANISO] = max_aniso;
}

void GXInitTexObjWrapMode(void* obj, u32 s, u32 t) {
    u32* o = (u32*)obj;
    o[TEXOBJ_WRAP_S] = s;
    o[TEXOBJ_WRAP_T] = t;
}

void GXDestroyTexObj(void* obj) {
    u32* o = (u32*)obj;
    if (o[TEXOBJ_BACKEND_TEX]) {
        dc_gx_backend_texture_release(o[TEXOBJ_BACKEND_TEX]);
        o[TEXOBJ_BACKEND_TEX] = 0;
    }
}
void GXDestroyTlutObj(void* obj) { (void)obj; }

GXBool GXGetTexObjMipMap(const void* obj) { return ((const u32*)obj)[TEXOBJ_MIPMAP] != 0; }
u32    GXGetTexObjFmt(const void* obj)    { return ((const u32*)obj)[TEXOBJ_FORMAT]; }
u16    GXGetTexObjHeight(const void* obj) { return (u16)((const u32*)obj)[TEXOBJ_HEIGHT]; }
u16    GXGetTexObjWidth(const void* obj)  { return (u16)((const u32*)obj)[TEXOBJ_WIDTH]; }
u32    GXGetTexObjWrapS(const void* obj)  { return ((const u32*)obj)[TEXOBJ_WRAP_S]; }
u32    GXGetTexObjWrapT(const void* obj)  { return ((const u32*)obj)[TEXOBJ_WRAP_T]; }
void*  GXGetTexObjData(const void* obj) {
    return (void*)(uintptr_t)((const u32*)obj)[TEXOBJ_IMAGE_PTR];
}

static int dc_gc_format_bpp(u32 format) {
    switch (format) {
        case GX_TF_I4:   case GX_TF_C4:   case GX_TF_CMPR: return 4;
        case GX_TF_I8:   case GX_TF_IA4:  case GX_TF_C8:   return 8;
        case GX_TF_IA8:  case GX_TF_RGB565: case GX_TF_RGB5A3:
        case GX_TF_C14X2: return 16;
        case GX_TF_RGBA8: return 32;
        default: return 16;
    }
}

u32 GXGetTexBufferSize(u16 width, u16 height, u32 format, GXBool mipmap, u8 max_lod) {
    u32 bpp = (u32)dc_gc_format_bpp(format);
    u32 total = ((u32)width * (u32)height * bpp) / 8;
    if (mipmap) {
        u32 w = width, h = height;
        u8 lod;
        for (lod = 1; lod <= max_lod && (w > 1 || h > 1); lod++) {
            if (w > 1) w >>= 1;
            if (h > 1) h >>= 1;
            total += (w * h * bpp) / 8;
        }
    }
    return total;
}

void GXInvalidateTexAll(void) { }
void GXInvalidateTexRegion(void* region) { (void)region; }
void GXInitTexCacheRegion(void* region, GXBool is_32b, u32 tmem_even, u32 size_even,
                          u32 tmem_odd, u32 size_odd) {
    (void)region; (void)is_32b; (void)tmem_even;
    (void)size_even; (void)tmem_odd; (void)size_odd;
}
void* GXSetTexRegionCallback(void* callback) { (void)callback; return NULL; }
void  GXInitTlutRegion(void* region, u32 tmem_addr, u32 tlut_size) {
    (void)region; (void)tmem_addr; (void)tlut_size;
}

void GXInitTlutObj(void* obj, void* lut, u32 fmt, u16 n_entries) {
    u32* o = (u32*)obj;
    memset(o, 0, 4 * sizeof(u32));
    o[TLUTOBJ_DATA] = (u32)(uintptr_t)lut;
    o[TLUTOBJ_FORMAT] = fmt;
    o[TLUTOBJ_N_ENTRIES] = n_entries;
}

void GXLoadTlut(void* obj, u32 idx) {
    u32* o;
    dc_gx_flush_if_begin_complete();
    if (idx >= 16) return;
    o = (u32*)obj;

    /* Working-set census, the bind rather than the GXInitTlutObj — same rule as
     * GXLoadTexObj below. Palettes are real asset bytes and emu64 passes the
     * source pointer straight through on this target: the 32-byte-alignment
     * truncation at emu64.c:3872 is #ifndef TARGET_PC, so `aligned_addr ==
     * tlut_addr` here, and a TLUT is always named by its symbol base rather
     * than an interior pointer — which is what makes a point record correct in
     * a stub image. No-op unless DC_ASSET_CENSUS. */
    dc_asset_census_note((const void*)(uintptr_t)o[TLUTOBJ_DATA], 'P');

    g_gx.tlut[idx].data = (const void*)(uintptr_t)o[TLUTOBJ_DATA];
    g_gx.tlut[idx].format = (int)o[TLUTOBJ_FORMAT];
    g_gx.tlut[idx].n_entries = (int)o[TLUTOBJ_N_ENTRIES];
    g_gx.tlut[idx].is_be = 1;   /* default BE (ROM/JSystem data) */
}

/* TLUT upload-dedup cache. emu64.c:3820 declares this `extern "C" u16
 * s_tlut_first_word[16]` and uses it to skip re-uploading a palette whose
 * first word is unchanged (emu64.c:3851,3879,3908). On PC it lives in
 * pc_gx_texture.c:17; that file is a rewrite-from-scratch on DC
 * (kb/design-platform-api.md §2c), so the array has to be owned here or the
 * link does not close. Value semantics are pure cache — zero means "nothing
 * uploaded yet", which is the correct cold state. */
u16 s_tlut_first_word[16];

/* emu64's tlutconv produces native-LE palettes; this marks the slot. The
 * distinction carries to DC unchanged (§3.6). */
void pc_gx_tlut_set_native_le(unsigned int idx) {
    if (idx < 16) g_gx.tlut[idx].is_be = 0;
}

/* Binding a texture to a TEV stage. The decode + VRAM upload lives in
 * dc_pvr_texture.c: twiddled ARGB1555/4444/RGB565 in the 8 MB of VRAM, decoded
 * with pc_gx_texture.c's tiling logic unchanged.
 *
 * RE-UPLOAD AFTER EVICTION. Texture VRAM is a bounded LRU cache, so a handle
 * stored in a GXTexObj can go stale while the object itself lives on for the
 * rest of the run. The check below is what keeps that from being permanent:
 * without it, the first eviction of a texture leaves its GXTexObj untextured
 * FOREVER, because the upload path only fires on a zero handle. Clearing the
 * stale handle here sends it back through the uploader on the next bind. */
void GXLoadTexObj(void* obj, u32 id) {
    u32* o;
    if (id >= 8) return;
    o = (u32*)obj;

    /* Census the bind, not the GXInitTexObj: a texture object that is built
     * and never bound is not part of the working set. No-op unless
     * DC_ASSET_CENSUS. */
    dc_asset_census_note((const void*)(uintptr_t)o[TEXOBJ_IMAGE_PTR], 'T');

    if (o[TEXOBJ_BACKEND_TEX] && !dc_pvr_tex_get(o[TEXOBJ_BACKEND_TEX]))
        o[TEXOBJ_BACKEND_TEX] = 0;

    /* The wrap mode is part of the dedup key. GXInitTexObjWrapMode() can
     * change it on an object that is already bound, and the backend header is
     * compiled from it; without these two terms that change would be dropped
     * and the texture would keep the previous object's wrapping. */
    if (dc_gx_state_dedup &&
        g_gx.tex_handle[id] == o[TEXOBJ_BACKEND_TEX] &&
        g_gx.tex_obj_w[id] == (int)o[TEXOBJ_WIDTH] &&
        g_gx.tex_obj_h[id] == (int)o[TEXOBJ_HEIGHT] &&
        g_gx.tex_obj_wrap_s[id] == (int)o[TEXOBJ_WRAP_S] &&
        g_gx.tex_obj_wrap_t[id] == (int)o[TEXOBJ_WRAP_T] &&
        o[TEXOBJ_BACKEND_TEX] != 0)
        return;

    dc_gx_flush_if_begin_complete();
    DIRTY(DC_GX_DIRTY_TEXTURES);

    if (!o[TEXOBJ_BACKEND_TEX]) {
        int tlut_name = (int)o[TEXOBJ_TLUT_NAME];
        const void* tlut = NULL;
        int tlut_fmt = 0, tlut_n = 0;
        if (tlut_name >= 0 && tlut_name < 16) {
            tlut = g_gx.tlut[tlut_name].data;
            tlut_fmt = g_gx.tlut[tlut_name].format;
            tlut_n = g_gx.tlut[tlut_name].n_entries;
        }
        o[TEXOBJ_BACKEND_TEX] = dc_gx_backend_texture_upload(
            (const void*)(uintptr_t)o[TEXOBJ_IMAGE_PTR],
            (int)o[TEXOBJ_WIDTH], (int)o[TEXOBJ_HEIGHT], (int)o[TEXOBJ_FORMAT],
            (int)o[TEXOBJ_CI_FORMAT], tlut, tlut_fmt, tlut_n);
    }

    g_gx.tex_handle[id] = o[TEXOBJ_BACKEND_TEX];
    g_gx.tex_obj_w[id] = (int)o[TEXOBJ_WIDTH];
    g_gx.tex_obj_h[id] = (int)o[TEXOBJ_HEIGHT];
    g_gx.tex_obj_fmt[id] = (int)o[TEXOBJ_FORMAT];
    g_gx.tex_obj_wrap_s[id] = (int)o[TEXOBJ_WRAP_S];
    g_gx.tex_obj_wrap_t[id] = (int)o[TEXOBJ_WRAP_T];
}

/* ==========================================================================
 * ============================ BACKEND SEAM ================================
 * ==========================================================================
 * The seam moved out of this file on the M2 pass. The seven backend functions
 * now live in:
 *
 *   dc/src/dc_pvr.c          init / frame / SH-4 T&L / near clip / submit
 *   dc/src/dc_pvr_texture.c  GC texture decode -> twiddled 16-bit VRAM
 *
 * Nothing above this line changed to make that happen — the accumulator, the
 * state machine, the dedup and the whole-batch cull are the same code that ran
 * against the NONE backend. Building with -DDC_PVR_BACKEND=0 restores the old
 * do-nothing behaviour for bisecting.
 */

/* Kept so anything that grabbed the PC entry point still links. */
void pc_gx_blit_to_screen(void) { }
void dc_gx_invalidate_viewport_shadow(void) { dc_gx_invalidate_vp_shadow(); }
