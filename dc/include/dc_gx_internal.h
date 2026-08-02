/* dc_gx_internal.h - GX state machine for the PowerVR backend.
 *
 * This is pc/include/pc_gx_internal.h with every GL object removed. The state
 * machine, dirty flags, batch accumulation and cull are IDENTICAL by design —
 * design doc §3.6 lists the four behaviours that must be reproduced exactly:
 *
 *   1. GXEnd is never called (emu64 omits it). Batches terminate on a counted
 *      vertex total, not an explicit end.
 *   2. GXPosition* commits the PREVIOUS vertex and resets the working one,
 *      but CARRIES COLOR0 FORWARD.
 *   3. GXBegin merges into an already-open complete batch when dirty == 0.
 *   4. State dedup returns early on a no-op set; whole-batch frustum cull
 *      rejects 60-80 % of submitted batches.
 *
 * The PVR backend attaches at ONE seam: dc_gx_backend_submit(). Nothing else
 * in dc_gx.c knows what a tile is.
 */
#ifndef DC_GX_INTERNAL_H
#define DC_GX_INTERNAL_H

#include "dc_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* --- Dirty flags: identical bit assignment to pc_gx_internal.h -------------
 * The bit numbers are load-bearing: pc_gx_flush_reason[] is indexed by them
 * and kb/perf.md quotes "breaks: mv=41 %, tex=30 %" against these indices. */
#define DC_GX_DIRTY_PROJECTION  (1u << 0)
#define DC_GX_DIRTY_MODELVIEW   (1u << 1)
#define DC_GX_DIRTY_TEV_COLORS  (1u << 2)
#define DC_GX_DIRTY_TEV_STAGES  (1u << 3)
#define DC_GX_DIRTY_SWAP_TABLES (1u << 4)
#define DC_GX_DIRTY_KONST       (1u << 5)
#define DC_GX_DIRTY_ALPHA_CMP   (1u << 6)
#define DC_GX_DIRTY_LIGHTING    (1u << 7)
#define DC_GX_DIRTY_TEXGEN      (1u << 8)
#define DC_GX_DIRTY_TEXTURES    (1u << 9)
#define DC_GX_DIRTY_INDIRECT    (1u << 10)
#define DC_GX_DIRTY_FOG         (1u << 11)
#define DC_GX_DIRTY_DEPTH       (1u << 12)
#define DC_GX_DIRTY_COLOR_MASK  (1u << 13)
#define DC_GX_DIRTY_CULL        (1u << 14)
#define DC_GX_DIRTY_BLEND       (1u << 15)
#define DC_GX_DIRTY_ALL         0xFFFFu
#define DC_GX_DIRTY_UNIFORM_MASK  0x0FFFu
#define DC_GX_STATE_GROUP_COUNT   12

void dc_gx_mark_dirty(unsigned int flag);
#define DIRTY(flag) dc_gx_mark_dirty(flag)

/* --- Sizing ----------------------------------------------------------------
 * THE BUFFER STAGES ONE FLUSH, NOT ONE FRAME. dc_gx_flush_vertices() resets
 * current_vertex_idx on every exit path, so the only thing this has to hold is
 * the largest run of vertices that can arrive between two flushes.
 *
 * The provable per-GXBegin ceiling is 512 vertices, from emu64's display-list
 * decoder:
 *   emu64.c:4954 dl_G_QUADN  n_faces = ((w0 >> 17) & 0x7F) + 1  <= 128, x4 = 512
 *   emu64.c:4814 dl_G_TRIN   same 7-bit field,                  x3 = 384
 *   everything else (G_TRI1/G_TRI2 singles, sprites, rects)     <= 6
 * kb/perf.md §14 measured merged=0 across a whole session, so GXBegin's
 * batch-merge path never inflates that in practice.
 *
 * 2040 is a ~4x margin over 512. It is deliberately NOT 1024: the G_TRI1/G_TRI2
 * run coalescer (emu64.c:5103-5118) counts commands until a non-tri opcode and
 * emits one GXBegin for the whole run — it is the one path with no encoding
 * bound on nverts. 2040 rather than 2048 because the cap MUST BE A MULTIPLE OF
 * 12: a mid-batch split is only topology-preserving if it lands on a primitive
 * boundary, and 12 = lcm(3 triangles, 4 quads, 2 lines, 1 point).
 *
 * The old value was 8192. It was not derived from anything: it is
 * pc/include/pc_gx_internal.h:50 PC_GX_MAX_VERTS 65536 — which is itself just
 * the width of GXBegin's `u16 nverts` argument, not a measurement — divided by
 * 8. Do not treat 8192 as a measured floor.
 *
 * Since dc_gx_commit_vertex() now FLUSHES AND CONTINUES instead of dropping,
 * this is a pure performance knob: too small costs extra draw calls, never a
 * dropped triangle. Kill switch: -DDC_GX_MAX_VERTS=8192 (and
 * -DDC_NO_VERTEX_SPLIT to get the old drop-on-overflow behaviour back). */
#ifndef DC_GX_MAX_VERTS
#define DC_GX_MAX_VERTS       2040
#endif
#define DC_GX_MAX_ATTR        26
#define DC_GX_MAX_VTXFMT      8
#define DC_GX_MAX_TEV_STAGES  3     /* measured max across all 101 configs */

/* Staging vertex. 32 bytes.
 *
 * It used to be 40. Three of those fields were dead weight:
 *   color1[4]   — NEVER WRITTEN. Only zeroed in GXPosition3f32. No call site in
 *                 src/ ever does GXSetVtxDesc(GX_VA_CLR1, ...) — emu64 declares
 *                 exactly POS, NRM, CLR0, TEX0 (emu64.c:2885-2896); the only
 *                 GX_VA_CLR1 mentions in the tree are switch arms inside
 *                 src/static/dolphin/gx/GXAttr.c and GXVerifXF.c. NOTE: the
 *                 `.oargb` in kb/tev-map.md is a PER-BATCH constant derived from
 *                 the TEV konst colours, not a per-vertex attribute — it does
 *                 not resurrect this field.
 *   _pad, _reserved — never touched. They existed to pad the struct to a
 *                 multiple of 8 for SH-4 FMOV pairs; a 32 B struct already is
 *                 one, and the aligned(8) below makes that a guarantee rather
 *                 than a hope about where the linker puts g_gx.
 * position/texcoord/color0 are live. normal[3] is kept even though nothing
 * reads it yet — SH-4 lighting (PLAN §3.3) consumes it as s16 fixed point, and
 * the GXNormal3f32/3s16/3s8/1x16 setters already write it. DC_GX_NRM_SCALE
 * converts.
 *
 * Kill switch: -DDC_GX_FAT_VERTEX restores the byte-identical 40 B layout.
 *
 * CAVEAT, recorded so nobody banks it twice: this saving is real .bss today,
 * but GLdc (M2, stage A) will bring its own vertex and command buffers, which
 * are ADDITIVE HEAP that no bucket currently reserves. This change does not
 * pay for those. */
#define DC_GX_NRM_SCALE 32767.0f
typedef struct {
    float position[3];      /*  0 */
    float texcoord[2];      /* 12 */
    unsigned char color0[4];/* 20 */
#ifdef DC_GX_FAT_VERTEX
    unsigned char color1[4];/* 24 -- dead: never written, see above */
#endif
    short normal[3];        /* 24 (28 when fat) */
#ifdef DC_GX_FAT_VERTEX
    unsigned short _pad;    /* 34 */
    float _reserved;        /* 36 -> 40 */
#endif
} __attribute__((aligned(8))) DCGXVertex;   /* 32 B lean, 40 B fat */

typedef struct {
    int color_a, color_b, color_c, color_d;
    int alpha_a, alpha_b, alpha_c, alpha_d;
    int color_op, color_bias, color_scale, color_clamp, color_out;
    int alpha_op, alpha_bias, alpha_scale, alpha_clamp, alpha_out;
    int tex_coord, tex_map, color_chan;
    int k_color_sel, k_alpha_sel;
    int ras_swap, tex_swap;
    int ind_stage, ind_format, ind_bias, ind_mtx, ind_wrap_s, ind_wrap_t;
    int ind_add_prev, ind_lod, ind_alpha;
} DCGXTevStage;

typedef struct { int r, g, b, a; } DCGXTevSwapTable;

typedef struct {
    int has_position, has_normal, has_color0, has_color1;
    int has_texcoord[8];
    int texcoord_frac[8];
    int position_size, color_size, texcoord_size, stride;
} DCGXVertexFormat;

typedef struct {
    /* Primitive assembly */
    int current_primitive;
    int current_vtxfmt;
    int expected_vertex_count;
    int in_begin;
    int current_vertex_idx;
    DCGXVertex vertex_buffer[DC_GX_MAX_VERTS];
    DCGXVertex current_vertex;
    int vertex_pending;

    /* Vertex descriptor / indexed arrays */
    int vtx_desc[DC_GX_MAX_ATTR];
    DCGXVertexFormat vtx_fmt[DC_GX_MAX_VTXFMT];
    const void*   array_base[DC_GX_MAX_ATTR];
    unsigned char array_stride[DC_GX_MAX_ATTR];

    /* Transforms */
    float projection_mtx[4][4];
    int   projection_type;
    float pos_mtx[10][3][4];
    float nrm_mtx[10][3][3];
    float tex_mtx[10][3][4];
    int   current_mtx;

    /* Viewport & scissor (GC coordinates, Y-down) */
    float viewport[6];
    int   scissor[4];

    /* TEV */
    int num_tev_stages;
    DCGXTevStage tev_stages[16];
    float tev_colors[4][4];
    float tev_k_colors[4][4];
    DCGXTevSwapTable tev_swap_table[4];

    /* Texture binding: opaque backend handles, filled by dc_gx_texture_bind */
    int num_tex_gens;
    int tex_gen_type[8];
    int tex_gen_src[8];
    int tex_gen_mtx[8];
    unsigned int tex_handle[8];   /* backend texture id, 0 = none */
    int tex_obj_w[8];
    int tex_obj_h[8];
    int tex_obj_fmt[8];
    /* GX_CLAMP / GX_REPEAT / GX_MIRROR, mirrored out of the bound GXTexObj.
     * These belong to the BIND, not to the upload: the texture cache is keyed
     * on texel content, so one VRAM image is legitimately shared by objects
     * that wrap differently. dc_pvr.c turns them into cxt.txr.uv_clamp/uv_flip
     * and folds them into header_key(). */
    int tex_obj_wrap_s[8];
    int tex_obj_wrap_t[8];

    /* Lighting (runs on SH-4; PLAN §3.3) */
    int num_chans;
    float chan_amb_color[2][4];
    float chan_mat_color[2][4];
    int chan_ctrl_enable[4];
    int chan_ctrl_amb_src[4];
    int chan_ctrl_mat_src[4];
    int chan_ctrl_light_mask[4];
    int chan_ctrl_diff_fn[4];
    int chan_ctrl_attn_fn[4];
    struct {
        float pos[3];
        float dir[3];
        float color[4];
        float a0, a1, a2;
        float k0, k1, k2;
    } lights[8];

    /* Raster state */
    int blend_mode, blend_src, blend_dst, blend_logic_op;
    int z_compare_enable, z_compare_func, z_update_enable;
    int color_update_enable, alpha_update_enable;
    int alpha_comp0, alpha_ref0, alpha_op, alpha_comp1, alpha_ref1;
    int cull_mode;

    /* Fog — PVR table fog is a near-native fit */
    int fog_type;
    float fog_start, fog_end, fog_near, fog_far;
    float fog_color[4];

    /* TLUT: per-slot is_be flag. ROM palettes are big-endian, emu64/EFB ones
     * native LE. This distinction carries to DC unchanged (§3.6). */
    struct {
        const void* data;
        int format;
        int n_entries;
        int is_be;
    } tlut[16];

    /* Indirect textures (PVR has no equivalent; recorded for the census) */
    int num_ind_stages;
    struct { int tex_coord, tex_map, scale_s, scale_t; } ind_order[4];
    float ind_mtx[3][2][3];
    int   ind_mtx_scale[3];

    float clear_color[4];
    float clear_depth;

    /* EFB copy */
    int copy_src[4];
    int copy_dst[2];
    int tex_copy_src[4];
    int tex_copy_dst[2];
    unsigned int tex_copy_fmt;
    int tex_copy_mipmap;

    unsigned int dirty;
    unsigned int group_gen[DC_GX_STATE_GROUP_COUNT];
} DCGXState;

extern DCGXState g_gx;

/* Kill switches (PLAN: "every optimization gets a kill switch"). Compile-time
 * on DC — there are no env vars. */
extern int dc_gx_state_dedup;   /* DC_NO_STATE_DEDUP   */
extern int dc_gx_strip_convert; /* DC_NO_STRIP_CONVERT */
extern int dc_gx_batch_cull;    /* DC_NO_BATCH_CULL    */
extern int dc_gx_draw_merge;    /* DC_NO_DRAW_MERGE    */

void dc_gx_flush_vertices(void);
void dc_gx_flush_if_begin_complete(void);

/* ==========================================================================
 * PVR BACKEND SEAM — the ONLY place the renderer attaches.
 * ==========================================================================
 * Everything above this line is renderer-agnostic and finished. Everything
 * below is stubbed in dc_gx.c for this pass and is the M2 (GLdc, stage A) /
 * M4 (direct PVR, stage B) deliverable.
 *
 * dc_gx_backend_submit() receives a complete, culled, state-consistent batch
 * of `count` vertices of primitive `prim` (already converted to independent
 * triangles or quads) together with the full g_gx state. Its job:
 *   - classify into the PVR Opaque / Punch-Through / Translucent list from
 *     GXSetAlphaCompare + GXSetBlendMode (design doc: "the most consequential
 *     single mapping decision in the renderer"),
 *   - transform + light on SH-4 (no hardware T&L),
 *   - emit pvr_vertex_t records through the store queues.
 * Until tev_map.md (M2) classifies all 101 TEV configs, this is a stub.
 */
void dc_gx_backend_init(void);
void dc_gx_backend_shutdown(void);
void dc_gx_backend_frame_begin(void);
void dc_gx_backend_frame_end(void);
void dc_gx_backend_submit(int prim, const DCGXVertex* verts, int count);
void dc_gx_backend_set_viewport(int x, int y, int w, int h, float nearz, float farz);
void dc_gx_backend_set_scissor(int x, int y, int w, int h);
/* Returns an opaque handle for a decoded texture, 0 on failure. */
unsigned int dc_gx_backend_texture_upload(const void* data, int w, int h, int fmt,
                                          int ci_fmt, const void* tlut,
                                          int tlut_fmt, int tlut_count);
void dc_gx_backend_texture_release(unsigned int handle);

#ifdef __cplusplus
}
#endif

#endif /* DC_GX_INTERNAL_H */
