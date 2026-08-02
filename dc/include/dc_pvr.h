/* dc_pvr.h - contract between the two halves of the PowerVR backend.
 *
 * The backend seam declared in dc_gx_internal.h is implemented by two files:
 *
 *   dc/src/dc_pvr.c          init / frame / SH-4 T&L / near-clip / submit
 *   dc/src/dc_pvr_texture.c  GC texture decode -> twiddled 16-bit VRAM
 *
 * They meet here and nowhere else. dc_pvr.c never decodes a texel and
 * dc_pvr_texture.c never emits a vertex.
 *
 * HANDLES. dc_gx_backend_texture_upload() returns an opaque non-zero
 * unsigned int that dc_gx.c stores in the GXTexObj (TEXOBJ_BACKEND_TEX) and
 * mirrors into g_gx.tex_handle[]. dc_pvr.c turns it back into something the
 * TA can consume with dc_pvr_tex_get(). Handle 0 means "no texture" and
 * dc_pvr_tex_get(0) MUST return NULL.
 *
 * WHY A LOOKUP AND NOT A POINTER. The handle also has to survive being
 * stashed in a u32 slot of the game's own GXTexObj, and a released texture
 * must be detectable as stale rather than dangling — a pointer would give the
 * TA a freed VRAM address with no way to notice. The indirection is one array
 * index on the hot path.
 */
#ifndef DC_PVR_H
#define DC_PVR_H

#include "dc_platform.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Resolved texture record. `base` is a VRAM address from pvr_mem_malloc();
 * `pvr_fmt` is the PVR_TXRFMT_* word to OR into the poly context, ALREADY
 * carrying the twiddle/stride/palette bits. w/h are the PVR-side power-of-two
 * dimensions, which may exceed the GX dimensions when a NPOT source had to be
 * padded — u_scale/v_scale then map GX texcoords onto the padded image and are
 * 1.0f when no padding happened. */
typedef struct {
    void*        base;
    unsigned int pvr_fmt;
    unsigned short w, h;
    float        u_scale, v_scale;
    unsigned char has_alpha;   /* 1 if any texel alpha < 255: drives OP/TR */
    unsigned char twiddled;
} dc_pvr_tex_t;

/* NULL for handle 0 or any handle that is not currently live. */
const dc_pvr_tex_t* dc_pvr_tex_get(unsigned int handle);

/* Called from dc_gx_backend_init()/shutdown() in dc_pvr.c, in that order,
 * AFTER pvr_init() has succeeded (VRAM allocation needs the PVR up). */
void dc_pvr_texture_init(void);
void dc_pvr_texture_shutdown(void);

/* One-line census for the smoke log: uploads, bytes resident, evictions. */
void dc_pvr_texture_report(void);

/* One line of renderer census: frames, batches, triangles in/out/clipped.
 * Safe to call before the backend is up. */
void dc_pvr_report(void);

/* Emit MARK:FRAME / FBHASH / FBTHUMB for the current front buffer, the
 * protocol harness/dc/screenshot.sh already parses. Compiled out unless
 * -DDC_FB_PROBE=<interval in frames>. This is how the port is verified with
 * nobody watching the emulator window — see the long comment in dc_pvr.c. */
void dc_pvr_fb_probe(void);

/* Non-zero once pvr_init() has succeeded. dc_pvr_texture.c must refuse to
 * allocate VRAM before that. Defined in dc_pvr.c. */
extern int dc_pvr_ready;

#ifdef __cplusplus
}
#endif

#endif /* DC_PVR_H */
