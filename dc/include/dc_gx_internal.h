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
 * PC_GX_MAX_VERTS was 65536 x 48 B = 3.1 MB of static BSS. kb/mem-budget.md
 * §4.1 shrinks this to 8192 verts; the real peak per town frame is UNMEASURED
 * (probe 4). If a batch exceeds this the surplus vertices are DROPPED and a
 * one-shot warning is printed — see dc_gx_commit_vertex(). */
#define DC_GX_MAX_VERTS       8192
#define DC_GX_MAX_ATTR        26
#define DC_GX_MAX_VTXFMT      8
#define DC_GX_MAX_TEV_STAGES  3     /* measured max across all 101 configs */

/* Staging vertex. 40 bytes, not the 32 B mem-budget quotes: that figure
 * assumes stage-B's quantized positions (dca3 meshlets + FTRV decode), which
 * is an M4 change. 8192 x 40 = 327,680 B, inside bucket 11 (384,000 B).
 * Normals are s16-normalized because SH-4 lighting will consume them as
 * fixed point; DC_GX_NRM_SCALE converts. */
#define DC_GX_NRM_SCALE 32767.0f
typedef struct {
    float position[3];      /*  0 */
    float texcoord[2];      /* 12 */
    unsigned char color0[4];/* 20 */
    unsigned char color1[4];/* 24 -- PVR "offset" (specular) color candidate */
    short normal[3];        /* 28 */
    unsigned short _pad;    /* 34 */
    float _reserved;        /* 36 -> 40, keeps 8-byte alignment for SH-4 FMOV pairs */
} DCGXVertex;

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
