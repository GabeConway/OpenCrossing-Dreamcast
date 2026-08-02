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
 * 1. ONE LIST, NOT THREE.  The PVR bins geometry into Opaque / Punch-Through /
 *    Translucent lists, and KOS is explicit that "lists can never be opened
 *    again within a single frame once they have been closed". The game hands
 *    us OP-ish and TR-ish batches finely interleaved in emu64 display-list
 *    order, so honouring three lists would mean buffering two of them in main
 *    RAM until frame end — and main RAM is the project's blocking problem
 *    (kb/STATE.md: the full image is ~7 MB over 16 MB).
 *
 *    So: everything goes into PVR_LIST_TR_POLY with autosort DISABLED. That
 *    turns the translucent list into a plain submission-ordered, Z-buffered
 *    rasteriser with per-polygon blend factors — which is exactly the GX
 *    semantics the game was written against. Opaque geometry is simply a poly
 *    with src=ONE dst=ZERO. Nothing is reordered, nothing is buffered, and the
 *    additive-heap cost of the whole renderer is zero bytes of main RAM.
 *
 *    The cost is the PVR's early-Z rejection, which the TR list does not do.
 *    That is a frame-rate problem, not a correctness problem, and it is the
 *    right trade for a bring-up. DC_PVR_LIST_MODE=1 puts everything in the
 *    opaque list instead (faster, no blending) for bisecting.
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

/* --- Viewport shadow -------------------------------------------------------
 * GXSetViewport hands us GameCube screen pixels. The PVR framebuffer is
 * 640x480 and Y-down, same as GX, so there is no flip — only a scale and
 * offset baked here so the per-vertex path is two multiply-adds. */
static float s_vp_cx = 320.0f, s_vp_cy = 240.0f;
static float s_vp_hw = 320.0f, s_vp_hh = 240.0f;

/* Set when a list is open inside the current scene. */
static int s_scene_open;
static int s_list_open;

/* The compiled poly header currently latched into the TA, and the state hash
 * it was compiled from. Recompiling costs ~100 cycles; comparing an int is
 * free, and the game re-sets identical state constantly. */
static pvr_poly_hdr_t s_hdr;
static unsigned int   s_hdr_key = 0xFFFFFFFFu;
static int            s_hdr_valid;

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
        case GX_BL_DSTALPHA:    return PVR_BLEND_DESTALPHA;
        case GX_BL_INVDSTALPHA: return PVR_BLEND_INVDESTALPHA;
        default:                return PVR_BLEND_ONE;
    }
}

/* pc_gx.c:1295 maps GX_CULL_BACK -> GL_BACK against GL's default CCW front
 * face, i.e. GX's front face is CCW in a Y-UP screen space. Our screen space
 * is Y-DOWN, which reverses the sign of every cross product, so GX-front
 * becomes CW here. Hence FRONT->CW and BACK->CCW, not the other way round.
 * If the world renders inside-out, -DDC_PVR_CULL_INVERT is the one-line test
 * before anything else gets blamed. */
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
        case GX_CULL_FRONT: return cw;
        case GX_CULL_BACK:  return ccw;
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
 * post-modelview position, so they are already in the same space. */
#ifndef DC_PVR_NO_LIGHTING
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
#endif /* !DC_PVR_NO_LIGHTING */

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

    rgba[0] = v->color0[0] * (1.0f / 255.0f);
    rgba[1] = v->color0[1] * (1.0f / 255.0f);
    rgba[2] = v->color0[2] * (1.0f / 255.0f);
    rgba[3] = v->color0[3] * (1.0f / 255.0f);

    return pack_argb(chan_component(0, 0, rgba, eye, nrm, 0),
                     chan_component(0, 0, rgba, eye, nrm, 1),
                     chan_component(0, 0, rgba, eye, nrm, 2),
                     chan_component(0, 1, rgba, eye, nrm, 3));
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
    k = (k * 33u) + (unsigned int)(uintptr_t)tex;
    if (tex) {
        k = (k * 33u) + tex->pvr_fmt;
        k = (k * 33u) + (unsigned int)(uintptr_t)tex->base;
    }
    return k ? k : 1u;
}

static void compile_header(const dc_pvr_tex_t* tex) {
    pvr_poly_cxt_t cxt;
    int list = (DC_PVR_LIST_MODE == 1) ? PVR_LIST_OP_POLY : PVR_LIST_TR_POLY;

    if (tex && tex->base)
        pvr_poly_cxt_txr(&cxt, list, (int)tex->pvr_fmt, tex->w, tex->h,
                         (pvr_ptr_t)tex->base, PVR_FILTER_BILINEAR);
    else
        pvr_poly_cxt_col(&cxt, list);

    cxt.gen.culling = cull_gx_to_pvr(g_gx.cull_mode);
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

    if (tex && tex->base) {
        /* MODULATEALPHA is px = ARGB(col) * ARGB(tex): the GX "modulate"
         * TEV config, which kb/tev-map.md shows dominating the 101 configs. */
        cxt.txr.env = PVR_TXRENV_MODULATEALPHA;
        cxt.txr.uv_clamp = PVR_UVCLAMP_NONE;
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
    pvr_prim(&s_hdr, sizeof(s_hdr));
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
    pvr_prim(&pv, sizeof(pv));
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
static void emit_triangle(const ClipVtx* a, const ClipVtx* b, const ClipVtx* c) {
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
    p.opb_sizes[4] = PVR_BINSIZE_0;
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

    dc_pvr_texture_init();

    DC_LOGE("[DC/PVR] backend up: list=%s autosort=off vertbuf=%d B "
            "opb=32 overflow=3\n",
            (DC_PVR_LIST_MODE == 1) ? "OP" : "TR", DC_PVR_VERTBUF_BYTES);
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
    pvr_scene_begin();
    s_scene_open = 1;
    s_list_open = 0;
    /* The header cache cannot survive a scene boundary: the TA latches state
     * per list, and a new list starts with nothing latched. */
    s_hdr_valid = 0;
    s_hdr_key = 0xFFFFFFFFu;

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
        pvr_list_begin((DC_PVR_LIST_MODE == 1) ? PVR_LIST_OP_POLY
                                               : PVR_LIST_TR_POLY);
        s_list_open = 1;
    }
    pvr_list_finish();
    s_list_open = 0;
    pvr_scene_finish();
    s_scene_open = 0;
    s_frames++;
}

void dc_gx_backend_submit(int prim, const DCGXVertex* verts, int count) {
    float comb[4][4];
    const float (*mv)[4];
    const float (*pr)[4];
    const float (*nm)[3];
    const dc_pvr_tex_t* tex;
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
        pvr_list_begin((DC_PVR_LIST_MODE == 1) ? PVR_LIST_OP_POLY
                                               : PVR_LIST_TR_POLY);
        s_list_open = 1;
        s_hdr_valid = 0;
        s_hdr_key = 0xFFFFFFFFu;
    }

    tex = dc_pvr_tex_get(g_gx.tex_handle[0]);
    ensure_header(tex);

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
            comb[i][j] = s;
        }
    }

    step = per_prim;
    for (i = 0; i + per_prim <= count; i += step) {
        ClipVtx cv[4];
        int k;

        for (k = 0; k < per_prim; k++) {
            const DCGXVertex* v = &verts[i + k];
            float ox = v->position[0], oy = v->position[1], oz = v->position[2];
            float eye[3], nrm[3], nl;

            cv[k].x = comb[0][0] * ox + comb[0][1] * oy + comb[0][2] * oz + comb[0][3];
            cv[k].y = comb[1][0] * ox + comb[1][1] * oy + comb[1][2] * oz + comb[1][3];
            cv[k].z = comb[2][0] * ox + comb[2][1] * oy + comb[2][2] * oz + comb[2][3];
            cv[k].w = comb[3][0] * ox + comb[3][1] * oy + comb[3][2] * oz + comb[3][3];

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

            apply_texgen(v, &cv[k].u, &cv[k].v);
            if (tex) {
                cv[k].u *= tex->u_scale;
                cv[k].v *= tex->v_scale;
            }
            cv[k].argb = shade_vertex(v, eye, nrm);
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

    s_batches++;
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
