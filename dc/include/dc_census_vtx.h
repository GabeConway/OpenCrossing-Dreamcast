/* dc_census_vtx.h — the vertex half of the asset census, and the one seam that
 * makes it observable without touching src/.
 *
 * Force-included into exactly ONE translation unit,
 * src/static/libforest/emu64/emu64.c, and only when DC_ASSET_CENSUS is set.
 * dc/Makefile has the rule; nothing else includes this file.
 *
 * WHY A SHIM AT ALL
 * -----------------
 * dc_asset_census.c can see textures because a texture reaches the platform
 * layer as an argument: emu64 hands GXLoadTexObj a pointer into the asset. The
 * geometry never does. GXSetArray — the GX API's indexed-vertex entry point,
 * and the obvious place to look — recorded ZERO hits on the title screen, and
 * that is not a property of the title screen: the only GXSetArray call site in
 * the whole of src/ is GXInit.c:252's reset loop. This game draws no indexed
 * geometry. Every triangle comes from an N64 display list that emu64 walks
 * itself, and emu64::dl_G_VTX() dereferences the Vtx* inside src/, converting
 * it into emu64's own 32-entry `vertices[]` cache before any GX call happens.
 * By the time dc/src/dc_gx.c sees a vertex it is three floats, not an address.
 *
 * So the source pointer is visible to exactly one callable outside src/:
 *
 *     OSs16tof32(&vtx_p->n.ob[0..2], &emu_vtx_p->position.x)   emu64.c ~4674
 *
 * a static inline in include/dolphin/os/OSFastCast.h. Those three calls are the
 * ONLY three occurrences of OSs16tof32 in emu64.c (grep it), so wrapping it in
 * this TU sees the vertex stream and nothing else — no filtering needed, and no
 * risk of counting some unrelated fixed-point conversion as an asset. The other
 * two TUs that use OSs16tof32 are jaudio's, and they are not force-included.
 *
 * WHY THE RENAME DANCE
 * --------------------
 * A function-like `#define OSs16tof32(s,f) ...` cannot work: OSFastCast.h's own
 * `static inline void OSs16tof32(` is followed by `(`, so the macro would eat
 * the definition. The prelude's `link` trick (dc_prelude.h §1a) is the pattern
 * that does work — rename the vendored definition out of the way with an
 * object-like macro, pull the header in here so its include guard closes, take
 * the name back, and define the wrapper. emu64.c's own `#include
 * "dolphin/os/OSFastCast.h"` at line 9 then finds the guard already set.
 *
 * OBSERVATION ONLY. The wrapper's semantics are the vendored inline's, called
 * unconditionally, with one extra call in front. Nothing about what the game
 * computes changes; only how long it takes. With DC_ASSET_CENSUS unset this
 * header is not force-included at all and emu64.c compiles byte-identically.
 *
 * WHAT IT SEES, EXACTLY. Only ob[0..2] — bytes 0..5 of each 16 B Vtx. The flag,
 * texcoords, normal and colour are read with plain loads and no call. That is
 * enough: dl_G_VTX reads whole vertices, so seeing the first field of each one
 * reconstructs the batch's extent exactly. dc_asset_census.c rounds each batch
 * out to whole Vtx for that reason.
 *
 * FRAGILITY TO KNOW ABOUT: this depends on emu64.c reading vertex positions
 * through OSs16tof32 rather than a cast. If a future emu64 change replaces
 * that call, the census silently reports zero batches. BSUM's notes counter is
 * the canary — notes=0 on a frame that draws means the seam is gone, not that
 * the scene has no geometry.
 */
#ifndef DC_CENSUS_VTX_H
#define DC_CENSUS_VTX_H

#if defined(DC_ASSET_CENSUS) && DC_ASSET_CENSUS

#ifdef __cplusplus
extern "C" {
#endif
void dc_asset_census_vtx(const void* addr);
#ifdef __cplusplus
}
#endif

#define OSs16tof32 dc_census_OSs16tof32_raw
#include "dolphin/os/OSFastCast.h"
#undef OSs16tof32

static inline void OSs16tof32(s16* s, f32* f) {
    dc_asset_census_vtx(s);
    dc_census_OSs16tof32_raw(s, f);
}

#endif /* DC_ASSET_CENSUS */
#endif /* DC_CENSUS_VTX_H */
