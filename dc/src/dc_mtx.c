/* dc_mtx.c - PSMTX* / C_MTX* / gu* matrix and vector math for Dreamcast.
 *
 * Ported from pc/src/pc_mtx.c (which is itself a scalar-C replacement for the
 * GameCube's PPC paired-singles routines). The scalar bodies are copied
 * VERBATIM and are the reference semantics; the SH-4 fast paths below must
 * produce the same results.
 *
 * ==========================================================================
 * CONVENTIONS — read this before touching the XMTRX code
 * ==========================================================================
 * GameCube `Mtx` is 3x4 ROW-MAJOR with a COLUMN-VECTOR convention:
 *
 *      dst = M * v          dst_j = sum_k M[j][k]*v_k + M[j][3]
 *
 * i.e. the translation lives in the 4th COLUMN, M[0..2][3], and the implied
 * bottom row is (0,0,0,1).
 *
 * The SH-4's FTRV instruction is the opposite convention. With XMTRX loaded
 * from memory by KOS's mat_load() (which copies 16 consecutive floats into
 * XF0..XF15 in order), the SH-4 manual's definition
 *
 *      out_i = sum_k XF[i + 4k] * v_k
 *
 * becomes, for a row-major memory matrix P,
 *
 *      out_i = sum_k v_k * P[k][i]           i.e.  out = v_row * P
 *
 * a ROW-VECTOR times the matrix. So FTRV computes the TRANSPOSED product.
 * Everything below either transposes on the way in or picks the operand order
 * that makes the transpose cancel. The two cases actually used:
 *
 *   PSMTXConcat(a, b, r):  r = a*b.  Row i of r is (row i of a) * b, which is
 *      exactly FTRV with P = b (NO transpose) applied to a's rows.
 *
 *   PSMTXMultVec(m, s, d): d = m*s.  This needs P = transpose(m44), built
 *      explicitly. Note transpose(m44) has m's translation column as its
 *      LAST ROW, which is what a row-vector convention wants.
 *
 * Getting this backwards produces geometry that is subtly wrong (transposed
 * rotations look like sheared/mirrored models) rather than obviously broken,
 * so DC_MTX_USE_XMTRX exists to A/B it against the scalar path in one build
 * flag (CLAUDE.md: every optimization gets a kill switch).
 *
 * ==========================================================================
 * WHAT IS DELIBERATELY *NOT* ACCELERATED
 * ==========================================================================
 *  - PSMTXInverse. Not hot, and the cofactor expansion has no FTRV shape.
 *  - Everything gu* (N64 fixed-point). Called at scene setup, not per frame,
 *    and guMtxF2L is integer bit-twiddling anyway.
 *
 * ==========================================================================
 * THE PSVEC* TRIO — the M3 TODO, answered from the map
 * ==========================================================================
 * This block used to say PSVECDotProduct / PSVECMag / PSVECNormalize were held
 * at full precision because they "feed lighting normals and, through
 * C_MTXLookAt, the camera basis", with a TODO(M3) to measure the error first.
 * Two thirds of that sentence is false for this image. Read the map:
 *
 *   PSVECDotProduct   AnimalCrossing.map:37256   DISCARDED, never called
 *   PSVECMag          AnimalCrossing.map:37258   DISCARDED, never called
 *   PSVECCrossProduct AnimalCrossing.map:37254   DISCARDED, never called
 *   C_MTXLookAt       AnimalCrossing.map:37264   DISCARDED, never called
 *   PSMTXMultVecArray AnimalCrossing.map:37248   DISCARDED, never called
 *
 * All five are inside the map's "Discarded input sections" block, which starts
 * at line 733 and ends at 42895. --gc-sections deleted them. THE CAMERA BASIS
 * DOES NOT GO THROUGH THIS FILE AT ALL, so there was never a camera-drift risk
 * to weigh; and PSMTXMultVecArray, which this file's own comments call "where
 * XMTRX actually pays", is not linked either.
 *
 * Exactly one member of the trio survives:
 *
 *   PSVECNormalize    AnimalCrossing.map:92577   0x8c4cdde4, 0x44 B
 *
 * with exactly one caller in the whole image, emu64.c:4696, gated on
 * `aflags[AFLAGS_VTX_NORMAL_MODIFY_TYPE] == 0 && (geometry_mode &
 * G_TEXTURE_GEN) != 0` — a per-vertex call, but only on texture-gen batches.
 * (The other two textual call sites, mtx.c:290 and :482-484, are in
 * src/static/dolphin/mtx/, which dc/Makefile:783 excludes from the build.)
 *
 * So the question the TODO deferred is narrow: does a ~2^-21 relative error on
 * ONE normalized vertex normal show up in the frame? Work it forward. The
 * normal is unit length, so the absolute error is ~5e-7 per component. That
 * normal is multiplied by the model-view matrix and consumed as a lighting
 * input whose output is an 8-bit colour channel. One channel step is 1/255 =
 * 3.9e-3, nearly four orders of magnitude larger than the error. There is no
 * arrangement of lights that turns 5e-7 into a visible band, and unlike a
 * camera basis this error does not accumulate across frames — every vertex is
 * renormalized from its source data every time.
 *
 * That is the whole argument, and it is why this now defaults ON. Turn it back
 * off with -DDC_MTX_NO_FIPR and diff a screenshot pair if you disbelieve it;
 * the shot to take is a texture-gen surface, because a batch without
 * G_TEXTURE_GEN never reaches this code — kb/station-bugs.md's shiny train
 * station interior is the cheapest one to frame.
 *
 * Note what this is NOT worth: with dc/src/dc_fmath.c linked, the scalar path's
 * sqrtf is already one FSQRT instruction, so the FIPR path is buying a
 * 3-mul-2-add-1-div sequence collapsed into FIPR + FSRRA. Tens of cycles on a
 * subset of vertices. This stage is cheap and safe, not large.
 *
 * XMTRX is a global CPU resource shared with anything else that uses the SH-4
 * matrix unit (GLdc, KOS's own mat_* helpers). Every fast path here loads it
 * immediately before use and never assumes it survived a call. KOS's
 * irq_context_t saves fr[16] AND frbank[16], so a preemption mid-routine is
 * safe.
 */
#include "dc_platform.h"

/* Kill switch (CLAUDE.md). 1 = SH-4 matrix unit, 0 = scalar C reference. */
#ifndef DC_MTX_USE_XMTRX
#define DC_MTX_USE_XMTRX 1
#endif

/* ON by default — see "THE PSVEC* TRIO" above for why the M3 TODO that held
 * this at 0 was resolved rather than measured. -DDC_MTX_NO_FIPR is the A/B. */
#ifndef DC_MTX_USE_FIPR
#ifdef DC_MTX_NO_FIPR
#define DC_MTX_USE_FIPR 0
#else
#define DC_MTX_USE_FIPR 1
#endif
#endif

#if defined(DC_HOST_STUB)
#undef  DC_MTX_USE_XMTRX
#define DC_MTX_USE_XMTRX 0
#undef  DC_MTX_USE_FIPR
#define DC_MTX_USE_FIPR 0
#endif

#if DC_MTX_USE_XMTRX
#include <dc/matrix.h>     /* mat_load, mat_trans_nodiv, matrix_t */
#endif
#if DC_MTX_USE_FIPR
#include <dc/fmath.h>      /* fipr, frsqrt, fsqrt */
/* Vendored, pinned, MIT — dc/third_party/sh4zam/VENDORING.md says why it is
 * here rather than taken from kos-ports. Used for shz_vec4_dot3 only: three
 * FIPRs sharing one pinned left vector, which the KOS fipr() macro cannot
 * express. */
#include <sh4zam/shz_vector.h>
#endif

/* Types, copied from pc_mtx.c. Deliberately local to this translation unit,
 * exactly as on the base port: dolphin/mtx.h is excluded from the build and
 * the game's own declarations are what the prototypes must match. */
typedef f32 Mtx34[3][4];
typedef f32 Mtx44[4][4];
typedef f32 (*MtxP)[4];

typedef struct { f32 x, y, z; } Vec;

typedef long long int Mtx_t[4][2];
typedef union {
    Mtx_t m;
    long long int forc_align;
} Mtx;

#define FTOFIX32(x) (long)((x) * (float)0x00010000)

/* ==========================================================================
 * SH-4 helpers
 * ========================================================================== */
#if DC_MTX_USE_XMTRX
/* mat_load() requires 8-byte alignment and prefers 32. matrix_t already
 * carries aligned(8); ask for a cache line so the fmov pairs never straddle. */
typedef matrix_t dc_mtx44 __attribute__((aligned(32)));

/* Expand a GameCube 3x4 into the 4x4 the matrix unit wants, appending the
 * implied bottom row (0,0,0,1). */
static void dc_mtx_expand44(const MtxP src, dc_mtx44 dst) {
    memcpy(&dst[0][0], &src[0][0], 12 * sizeof(f32));
    dst[3][0] = 0.0f; dst[3][1] = 0.0f; dst[3][2] = 0.0f; dst[3][3] = 1.0f;
}

/* transpose(src44). Feeding this to FTRV turns the row-vector product back
 * into the column-vector product GX means. */
static void dc_mtx_transpose44(const MtxP src, dc_mtx44 dst) {
    dst[0][0] = src[0][0]; dst[0][1] = src[1][0]; dst[0][2] = src[2][0]; dst[0][3] = 0.0f;
    dst[1][0] = src[0][1]; dst[1][1] = src[1][1]; dst[1][2] = src[2][1]; dst[1][3] = 0.0f;
    dst[2][0] = src[0][2]; dst[2][1] = src[1][2]; dst[2][2] = src[2][2]; dst[2][3] = 0.0f;
    dst[3][0] = src[0][3]; dst[3][1] = src[1][3]; dst[3][2] = src[2][3]; dst[3][3] = 1.0f;
}

/* ==========================================================================
 * XMTRX RESIDENCY (kb/research-fps-ideas.md F2)
 * ==========================================================================
 * emu64's dl_G_VTX calls PSMTXMultVec TWICE PER SHARED VERTEX
 * (emu64.c:4708-4712) with the same matrix pointer every call, and every one
 * of those calls used to rebuild the 4x4 transpose in memory and re-run
 * mat_load's eight `fmov @r4+, xdN`. That is ~28 memory operations to restore
 * a register bank that already held exactly the right sixteen floats.
 *
 * A residency token skips the reload when XMTRX provably still holds this
 * matrix. "Provably" is doing real work in that sentence, and there are two
 * distinct ways to be wrong, so the guard has two halves:
 *
 *   1. SOMEONE ELSE LOADED XMTRX. It is one global register bank and
 *      dc_pvr.c loads its own folded projection*modelview once per batch
 *      (dc_pvr.c:2604). Pointer/content equality cannot see that, so every
 *      other writer in the image must call dc_mtx_xmtrx_invalidate(). There
 *      are exactly two XMTRX writers -- this file and that one.
 *
 *   2. THE SAME POINTER NOW HOLDS DIFFERENT NUMBERS. emu64 owns a matrix
 *      stack and rewrites entries in place, so pointer equality alone is the
 *      documented failure mode for this idea: geometry transformed by a stale
 *      matrix snaps to the wrong place. Hence the full 12-word content
 *      compare. It costs 12 integer compares on a hit -- still far less than
 *      the transpose and the load it replaces -- and it makes the cache
 *      correct by construction rather than by an invalidation audit.
 *
 * The tag deliberately does NOT distinguish PSMTXMultVec from
 * PSMTXMultVecSR: both load transpose44(m) and differ only in the `w` they
 * pass to FTRV, which is a register argument, not part of the matrix.
 * PSMTXConcat loads the OTHER form (expand44, untransposed), so it carries a
 * different kind and the two can never alias.
 *
 * Kill switch: -DDC_MTX_NO_XMTRX_CACHE restores an unconditional reload.
 */
#ifndef DC_MTX_NO_XMTRX_CACHE
#define DC_MTX_XMTRX_CACHE 1
#else
#define DC_MTX_XMTRX_CACHE 0
#endif

/* ==========================================================================
 * DC_MTX_FIPR_MULTVEC — the MultVec family through FIPR. MEASURED FLAT.
 *                       DEFAULT 0. Turn on with -DDC_MTX_FIPR_MULTVEC=1.
 * ==========================================================================
 * ⚠️ THE RESULT FIRST, because the reasoning below is a good argument for a
 * change that does not pay: matched town windows, 2026-08-06, this flag the
 * only difference —
 *
 *      us/v      3.11 -> 3.12        draw   45.4 -> 45.5 ms
 *      town FPS  20.6 -> 20.4        (run smoke-oc-dc-shz-20260806-133743)
 *
 * i.e. inside noise, and if anything slightly worse. The compiled function
 * really did become 3 FIPRs and nothing else (verified with objdump: zero
 * fmul, zero fadd, down from 9 and 9) — it simply is not where the frame is.
 * The estimate that motivated it assumed ~13,900 calls/frame from the G_VTX
 * source-vertex count; the backend only ever sees ~3,000 vertices, and at -O3
 * the scalar form was already cheap.
 *
 * ⚠️ SO IT IS OFF, and the reason is precision, not speed: FIPR accumulates
 * four products with a single final rounding and carries up to ~2^-21
 * relative error where the scalar path is four correctly-rounded operations.
 * That is a real risk on VERTEX POSITIONS, and a real risk buys nothing here.
 * (The renderer accepts the same trade for LIGHTING at dc_pvr.c:188-191,
 * where an error of that size cannot move a pixel.)
 *
 * KEPT REGARDLESS, and this is the part that was worth the trip: the
 * residency branch this replaced is DEAD CODE, and the #else path below no
 * longer pays a 12-word compare to discover that. Traced against
 * dc/build/AnimalCrossing.map —
 *
 *   dc_xm_claim(..., DC_XM_KIND_TRANSPOSE) has exactly ONE call site, inside
 *   PSMTXMultVecArray — which this file's own header already lists as
 *   DISCARDED by --gc-sections, at address 0x00000000. Nothing in this game
 *   calls it, and the header noted that while the code went on probing for it.
 *   Therefore dc_xm_resident(m, DC_XM_KIND_TRANSPOSE) could never return 1,
 *   the FTRV at the top of PSMTXMultVec had never once executed, and every
 *   call ran 12 loads + 12 compares + a branch to reach the scalar fallback.
 *
 * AND WHY NOT XMTRX HERE, EVER. dl_G_VTX calls this twice per vertex with TWO
 * DIFFERENT matrices — position_mtx at emu64.c:4709, model_view_mtx at :4711
 * — alternating every iteration. SH-4 has one XF bank, so any resident design
 * reloads it twice per vertex. The scalar path was the right call all along;
 * only the compare in front of it was waste.
 */
#ifndef DC_MTX_FIPR_MULTVEC
#define DC_MTX_FIPR_MULTVEC 0
#endif

#define DC_XM_KIND_NONE       0
#define DC_XM_KIND_TRANSPOSE  1   /* transpose44(m) — the MultVec family */
#define DC_XM_KIND_EXPAND     2   /* expand44(b)    — PSMTXConcat        */

#if DC_MTX_XMTRX_CACHE
static const void *s_xm_src  = NULL;
static int         s_xm_kind = DC_XM_KIND_NONE;
static u32         s_xm_copy[12];

static int dc_xm_resident(const MtxP m, int kind) {
    const u32 *a;
    int i;
    if (s_xm_kind != kind || s_xm_src != (const void *)m)
        return 0;
    a = (const u32 *)(const void *)&m[0][0];
    for (i = 0; i < 12; i++)
        if (a[i] != s_xm_copy[i])
            return 0;
    return 1;
}

static void dc_xm_claim(const MtxP m, int kind) {
    const u32 *a = (const u32 *)(const void *)&m[0][0];
    int i;
    s_xm_src  = (const void *)m;
    s_xm_kind = kind;
    for (i = 0; i < 12; i++)
        s_xm_copy[i] = a[i];
}
#endif /* DC_MTX_XMTRX_CACHE */
#endif /* DC_MTX_USE_XMTRX */

/* Always defined, so dc_pvr.c can call it without knowing which switches are
 * set. Under -DDC_MTX_NO_XMTRX_CACHE or the scalar path it is a no-op and
 * -O2 deletes the call. */
void dc_mtx_xmtrx_invalidate(void) {
#if DC_MTX_USE_XMTRX && DC_MTX_XMTRX_CACHE
    s_xm_src  = NULL;
    s_xm_kind = DC_XM_KIND_NONE;
#endif
}

/* ==========================================================================
 * Dolphin PS* matrix functions (3x4 row-major, column-vector convention)
 * ========================================================================== */

void PSMTXIdentity(MtxP m) {
    m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = 0.0f;
    m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; m[2][3] = 0.0f;
}

void C_MTXIdentity(MtxP m) {
    PSMTXIdentity(m);
}

void PSMTXCopy(const MtxP src, MtxP dst) {
    memcpy(dst, src, 12 * sizeof(f32));
}

/* THE hot one. 41 % of all GX batch breaks are modelview loads (design doc
 * §3.6), every one of which is preceded by a concat chain. */
void PSMTXConcat(const MtxP a, const MtxP b, MtxP result) {
    f32 tmp[3][4];
    int i;

#if DC_MTX_USE_XMTRX
    dc_mtx44 mb;

    /* Row i of the answer is (row i of a, as a 4-vector) * b. `b` goes into
     * XMTRX UNTRANSPOSED — see the header comment. Both operands are copied
     * out before `result` is written, so a/b may alias result. */
#if DC_MTX_XMTRX_CACHE
    if (!dc_xm_resident(b, DC_XM_KIND_EXPAND)) {
        dc_mtx_expand44(b, mb);
        mat_load((const matrix_t*)&mb);
        dc_xm_claim(b, DC_XM_KIND_EXPAND);
    }
#else
    dc_mtx_expand44(b, mb);
    mat_load((const matrix_t*)&mb);
#endif

    for (i = 0; i < 3; i++) {
        float x = a[i][0], y = a[i][1], z = a[i][2], w = a[i][3];
        mat_trans_nodiv(x, y, z, w);
        tmp[i][0] = x; tmp[i][1] = y; tmp[i][2] = z; tmp[i][3] = w;
    }
#else
    for (i = 0; i < 3; i++) {
        tmp[i][0] = a[i][0]*b[0][0] + a[i][1]*b[1][0] + a[i][2]*b[2][0];
        tmp[i][1] = a[i][0]*b[0][1] + a[i][1]*b[1][1] + a[i][2]*b[2][1];
        tmp[i][2] = a[i][0]*b[0][2] + a[i][1]*b[1][2] + a[i][2]*b[2][2];
        tmp[i][3] = a[i][0]*b[0][3] + a[i][1]*b[1][3] + a[i][2]*b[2][3] + a[i][3];
    }
#endif
    memcpy(result, tmp, 12 * sizeof(f32));
}

/* Scalar only: cofactor expansion has no FTRV shape and this is not hot. */
void PSMTXInverse(const MtxP src, MtxP inv) {
    f32 det, invDet;
    f32 tmp[3][4];

    det = src[0][0] * (src[1][1]*src[2][2] - src[1][2]*src[2][1])
        - src[0][1] * (src[1][0]*src[2][2] - src[1][2]*src[2][0])
        + src[0][2] * (src[1][0]*src[2][1] - src[1][1]*src[2][0]);

    if (fabsf(det) < 1e-25f) {
        PSMTXIdentity(inv);
        return;
    }

    invDet = 1.0f / det;

    tmp[0][0] = (src[1][1]*src[2][2] - src[1][2]*src[2][1]) * invDet;
    tmp[0][1] = (src[0][2]*src[2][1] - src[0][1]*src[2][2]) * invDet;
    tmp[0][2] = (src[0][1]*src[1][2] - src[0][2]*src[1][1]) * invDet;
    tmp[1][0] = (src[1][2]*src[2][0] - src[1][0]*src[2][2]) * invDet;
    tmp[1][1] = (src[0][0]*src[2][2] - src[0][2]*src[2][0]) * invDet;
    tmp[1][2] = (src[0][2]*src[1][0] - src[0][0]*src[1][2]) * invDet;
    tmp[2][0] = (src[1][0]*src[2][1] - src[1][1]*src[2][0]) * invDet;
    tmp[2][1] = (src[0][1]*src[2][0] - src[0][0]*src[2][1]) * invDet;
    tmp[2][2] = (src[0][0]*src[1][1] - src[0][1]*src[1][0]) * invDet;

    tmp[0][3] = -(tmp[0][0]*src[0][3] + tmp[0][1]*src[1][3] + tmp[0][2]*src[2][3]);
    tmp[1][3] = -(tmp[1][0]*src[0][3] + tmp[1][1]*src[1][3] + tmp[1][2]*src[2][3]);
    tmp[2][3] = -(tmp[2][0]*src[0][3] + tmp[2][1]*src[1][3] + tmp[2][2]*src[2][3]);

    memcpy(inv, tmp, 12 * sizeof(f32));
}

/* ⚠️ CORRECTION to kb/research-fps-ideas.md F2, 2026-08-04.
 *
 * F2 was written as "dl_G_VTX calls PSMTXMultVec twice per shared vertex with
 * the SAME matrix every call, so a residency token turns the second call into
 * one FTRV". Read the call site and that premise is false:
 *
 *     emu64.c:4708   PSMTXMultVec(position_mtx,         pos,    pos);
 *     emu64.c:4710   PSMTXMultVec(this->model_view_mtx, normal, normal);
 *
 * TWO DIFFERENT MATRICES, alternating once per vertex. A single-slot residency
 * cache — and XMTRX is physically a single slot — therefore misses on EVERY
 * call in the loop that matters. F2 as specified is worth zero here.
 *
 * What the call site does say is that the single-vector entry points should
 * never have been on the XMTRX path at all. One FTRV is cheap; getting the
 * matrix into XMTRX is not, and for one vector it is the whole cost:
 *
 *   XMTRX : transpose44 = 12 loads + 16 stores, mat_load = 8 double-fmov
 *           plus the frchg pair, then 1 FTRV.        ~50+ cycles
 *   scalar: 12 loads, 9 fmul, 9 fadd, 3 stores, no
 *           memory round-trip and no bank switch.    ~30-40 cycles
 *
 * The file's own header already said this — "PSMTXMultVecArray: this is where
 * XMTRX actually pays" — and then loaded XMTRX per call anyway.
 *
 * So the shape here is: FTRV when the matrix provably still IS resident (which
 * PSMTXMultVecArray and PSMTXConcat do arrange, for their own callers), and
 * otherwise plain scalar. The miss path deliberately does NOT touch XMTRX,
 * which is what makes it safe to interleave with a bulk transform: a scalar
 * miss cannot evict somebody else's resident matrix.
 *
 * Kill switch: -DDC_MTX_NO_XMTRX_CACHE restores the unconditional reload. */
void PSMTXMultVec(const MtxP m, const Vec* src, Vec* dst) {
#if DC_MTX_FIPR_MULTVEC
    /* src and dst alias on every emu64 call site — compute all three
     * components before storing any. */
    const shz_vec3_t r = shz_vec4_dot3(
        shz_vec4_init(src->x, src->y, src->z, 1.0f),
        shz_vec4_init(m[0][0], m[0][1], m[0][2], m[0][3]),
        shz_vec4_init(m[1][0], m[1][1], m[1][2], m[1][3]),
        shz_vec4_init(m[2][0], m[2][1], m[2][2], m[2][3]));
    dst->x = r.x; dst->y = r.y; dst->z = r.z;
#elif DC_MTX_USE_XMTRX && DC_MTX_XMTRX_CACHE
    /* No residency probe: DC_XM_KIND_TRANSPOSE is never claimed in this image
     * (see the block above). The probe could only ever fail, at 12 loads and
     * 12 compares a call, twice per vertex. */
    {
        f32 x = m[0][0]*src->x + m[0][1]*src->y + m[0][2]*src->z + m[0][3];
        f32 y = m[1][0]*src->x + m[1][1]*src->y + m[1][2]*src->z + m[1][3];
        f32 z = m[2][0]*src->x + m[2][1]*src->y + m[2][2]*src->z + m[2][3];
        dst->x = x; dst->y = y; dst->z = z;
    }
#elif DC_MTX_USE_XMTRX
    dc_mtx44 mt;
    float x = src->x, y = src->y, z = src->z, w = 1.0f;

    dc_mtx_transpose44(m, mt);
    mat_load((const matrix_t*)&mt);
    mat_trans_nodiv(x, y, z, w);
    dst->x = x; dst->y = y; dst->z = z;
#else
    f32 x = m[0][0]*src->x + m[0][1]*src->y + m[0][2]*src->z + m[0][3];
    f32 y = m[1][0]*src->x + m[1][1]*src->y + m[1][2]*src->z + m[1][3];
    f32 z = m[2][0]*src->x + m[2][1]*src->y + m[2][2]*src->z + m[2][3];
    dst->x = x; dst->y = y; dst->z = z;
#endif
}

/* Scale/Rotate only — the translation column is skipped. Same XMTRX, w = 0. */
void PSMTXMultVecSR(const MtxP m, const Vec* src, Vec* dst) {
#if DC_MTX_FIPR_MULTVEC
    const shz_vec3_t r = shz_vec4_dot3(
        shz_vec4_init(src->x, src->y, src->z, 0.0f),
        shz_vec4_init(m[0][0], m[0][1], m[0][2], m[0][3]),
        shz_vec4_init(m[1][0], m[1][1], m[1][2], m[1][3]),
        shz_vec4_init(m[2][0], m[2][1], m[2][2], m[2][3]));
    dst->x = r.x; dst->y = r.y; dst->z = r.z;
#elif DC_MTX_USE_XMTRX && DC_MTX_XMTRX_CACHE
    /* Same as PSMTXMultVec: the probe is provably dead here too. */
    {
        f32 x = m[0][0]*src->x + m[0][1]*src->y + m[0][2]*src->z;
        f32 y = m[1][0]*src->x + m[1][1]*src->y + m[1][2]*src->z;
        f32 z = m[2][0]*src->x + m[2][1]*src->y + m[2][2]*src->z;
        dst->x = x; dst->y = y; dst->z = z;
    }
#elif DC_MTX_USE_XMTRX
    dc_mtx44 mt;
    float x = src->x, y = src->y, z = src->z, w = 0.0f;

    dc_mtx_transpose44(m, mt);
    mat_load((const matrix_t*)&mt);
    mat_trans_nodiv(x, y, z, w);
    dst->x = x; dst->y = y; dst->z = z;
#else
    f32 x = m[0][0]*src->x + m[0][1]*src->y + m[0][2]*src->z;
    f32 y = m[1][0]*src->x + m[1][1]*src->y + m[1][2]*src->z;
    f32 z = m[2][0]*src->x + m[2][1]*src->y + m[2][2]*src->z;
    dst->x = x; dst->y = y; dst->z = z;
#endif
}

/* Bulk transform. This is where XMTRX actually pays: the matrix is loaded
 * ONCE and every vertex costs one FTRV instead of 9 mul + 9 add.
 *
 * NOTE: deliberately NOT KOS's mat_transform(). That reads 4-float vector_t
 * records; `Vec` is 3 floats, so mat_transform would over-read the last
 * element of the array and (with stride 12) misinterpret w. */
void PSMTXMultVecArray(const MtxP m, const Vec* srcBase, Vec* dstBase, u32 count) {
    u32 i;
#if DC_MTX_USE_XMTRX
    dc_mtx44 mt;

#if DC_MTX_XMTRX_CACHE
    if (!dc_xm_resident(m, DC_XM_KIND_TRANSPOSE)) {
        dc_mtx_transpose44(m, mt);
        mat_load((const matrix_t*)&mt);
        dc_xm_claim(m, DC_XM_KIND_TRANSPOSE);
    }
#else
    dc_mtx_transpose44(m, mt);
    mat_load((const matrix_t*)&mt);
#endif

    for (i = 0; i < count; i++) {
        float x = srcBase[i].x, y = srcBase[i].y, z = srcBase[i].z, w = 1.0f;
        mat_trans_nodiv(x, y, z, w);
        dstBase[i].x = x; dstBase[i].y = y; dstBase[i].z = z;
    }
#else
    for (i = 0; i < count; i++)
        PSMTXMultVec(m, &srcBase[i], &dstBase[i]);
#endif
}

void PSMTXScale(MtxP m, f32 sx, f32 sy, f32 sz) {
    m[0][0] = sx;   m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = 0.0f;
    m[1][0] = 0.0f; m[1][1] = sy;   m[1][2] = 0.0f; m[1][3] = 0.0f;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = sz;   m[2][3] = 0.0f;
}

void PSMTXTrans(MtxP m, f32 tx, f32 ty, f32 tz) {
    m[0][0] = 1.0f; m[0][1] = 0.0f; m[0][2] = 0.0f; m[0][3] = tx;
    m[1][0] = 0.0f; m[1][1] = 1.0f; m[1][2] = 0.0f; m[1][3] = ty;
    m[2][0] = 0.0f; m[2][1] = 0.0f; m[2][2] = 1.0f; m[2][3] = tz;
}

void PSMTXTransApply(const MtxP src, MtxP dst, f32 tx, f32 ty, f32 tz) {
    if (src != dst) PSMTXCopy(src, dst);
    dst[0][3] += tx;
    dst[1][3] += ty;
    dst[2][3] += tz;
}

void PSMTXScaleApply(const MtxP src, MtxP dst, f32 sx, f32 sy, f32 sz) {
    int j;
    for (j = 0; j < 4; j++) {
        dst[0][j] = src[0][j] * sx;
        dst[1][j] = src[1][j] * sy;
        dst[2][j] = src[2][j] * sz;
    }
}

/* --- Vector ops -----------------------------------------------------------
 * Of the four below, only PSVECNormalize is linked; the other three are in the
 * map's discarded block. See "THE PSVEC* TRIO" in the header for the map lines
 * and for why DC_MTX_USE_FIPR now defaults to 1. */

/* The one live caller is emu64.c:4696, per vertex on texture-gen batches only.
 * The mag2 > 0 guard is load-bearing in BOTH paths and for different reasons:
 * the scalar path would divide by zero, and FSRRA of +0 is +inf, which would
 * turn a degenerate normal into inf/NaN and poison the lit colour rather than
 * producing the zero vector the GameCube produced. */
void PSVECNormalize(const Vec* src, Vec* dst) {
#if DC_MTX_USE_FIPR
    float mag2 = fipr_magnitude_sqr(src->x, src->y, src->z, 0.0f);
    if (mag2 > 0.0f) {
        float inv = frsqrt(mag2);
        dst->x = src->x * inv;
        dst->y = src->y * inv;
        dst->z = src->z * inv;
    } else {
        dst->x = dst->y = dst->z = 0.0f;
    }
#else
    f32 mag = sqrtf(src->x*src->x + src->y*src->y + src->z*src->z);
    if (mag > 0.0f) {
        f32 inv = 1.0f / mag;
        dst->x = src->x * inv;
        dst->y = src->y * inv;
        dst->z = src->z * inv;
    } else {
        dst->x = dst->y = dst->z = 0.0f;
    }
#endif
}

void PSVECCrossProduct(const Vec* a, const Vec* b, Vec* dst) {
    f32 x = a->y * b->z - a->z * b->y;
    f32 y = a->z * b->x - a->x * b->z;
    f32 z = a->x * b->y - a->y * b->x;
    dst->x = x; dst->y = y; dst->z = z;
}

f32 PSVECDotProduct(const Vec* a, const Vec* b) {
#if DC_MTX_USE_FIPR
    return fipr(a->x, a->y, a->z, 0.0f, b->x, b->y, b->z, 0.0f);
#else
    return a->x*b->x + a->y*b->y + a->z*b->z;
#endif
}

f32 PSVECMag(const Vec* v) {
#if DC_MTX_USE_FIPR
    return fsqrt(fipr_magnitude_sqr(v->x, v->y, v->z, 0.0f));
#else
    return sqrtf(v->x*v->x + v->y*v->y + v->z*v->z);
#endif
}

/* ==========================================================================
 * C_MTX* — 4x4 projection matrices
 * ==========================================================================
 * These build GAMECUBE-convention projections: clip z in [-1, 0] (not GL's
 * [-1, 1]), and a column-vector convention like the PS* matrices above.
 *
 * TODO (M2, PLAN §3.3): the PVR does not consume a projection matrix at all —
 * it wants screen-space XY plus 1/W per vertex, and its depth test runs on
 * that W. The SH-4 transform stage in dc_gx.c has to apply P*MV itself and
 * emit 1/W. Do NOT "fix" the matrices here to a PVR convention: dc_gx.c reads
 * them back through GXGetProjectionv and the game does its own frustum math
 * with them. The convention change belongs in the vertex stage, once.
 */

void C_MTXFrustum(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
    f32 tmp;
    tmp     = 1.0f / (r - l);
    m[0][0] = (2.0f * n) * tmp;
    m[0][1] = 0.0f;
    m[0][2] = (r + l) * tmp;
    m[0][3] = 0.0f;
    tmp     = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = (2.0f * n) * tmp;
    m[1][2] = (t + b) * tmp;
    m[1][3] = 0.0f;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    tmp     = 1.0f / (f - n);
    m[2][2] = -(n) * tmp;
    m[2][3] = -(f * n) * tmp;
    m[3][0] = 0.0f;
    m[3][1] = 0.0f;
    m[3][2] = -1.0f;
    m[3][3] = 0.0f;
}

void C_MTXPerspective(Mtx44 m, f32 fovY, f32 aspect, f32 n, f32 f) {
    f32 angle = 0.5f * fovY * DC_DEG_TO_RADf;
    f32 cot = 1.0f / tanf(angle);
    f32 tmp = 1.0f / (f - n);
    m[0][0] = cot / aspect;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = 0.0f;
    m[1][0] = 0.0f;
    m[1][1] = cot;
    m[1][2] = 0.0f;
    m[1][3] = 0.0f;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = -n * tmp;
    m[2][3] = -(f * n) * tmp;
    m[3][0] = 0.0f;
    m[3][1] = 0.0f;
    m[3][2] = -1.0f;
    m[3][3] = 0.0f;
}

void C_MTXOrtho(Mtx44 m, f32 t, f32 b, f32 l, f32 r, f32 n, f32 f) {
    f32 tmp;
    tmp     = 1.0f / (r - l);
    m[0][0] = 2.0f * tmp;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = -(r + l) * tmp;
    tmp     = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = 2.0f * tmp;
    m[1][2] = 0.0f;
    m[1][3] = -(t + b) * tmp;
    tmp     = 1.0f / (f - n);
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = -1.0f * tmp;
    m[2][3] = -n * tmp;
    m[3][0] = 0.0f;
    m[3][1] = 0.0f;
    m[3][2] = 0.0f;
    m[3][3] = 1.0f;
}

void C_MTXLookAt(MtxP m, const Vec* camPos, const Vec* camUp, const Vec* target) {
    Vec look, right, up;

    look.x = camPos->x - target->x;
    look.y = camPos->y - target->y;
    look.z = camPos->z - target->z;
    PSVECNormalize(&look, &look);

    PSVECCrossProduct(camUp, &look, &right);
    PSVECNormalize(&right, &right);

    PSVECCrossProduct(&look, &right, &up);

    m[0][0] = right.x; m[0][1] = right.y; m[0][2] = right.z;
    m[0][3] = -(camPos->x*right.x + camPos->y*right.y + camPos->z*right.z);
    m[1][0] = up.x;    m[1][1] = up.y;    m[1][2] = up.z;
    m[1][3] = -(camPos->x*up.x + camPos->y*up.y + camPos->z*up.z);
    m[2][0] = look.x;  m[2][1] = look.y;  m[2][2] = look.z;
    m[2][3] = -(camPos->x*look.x + camPos->y*look.y + camPos->z*look.z);
}

void C_MTXLightPerspective(MtxP m, f32 fovY, f32 aspect, f32 scaleS, f32 scaleT,
                           f32 transS, f32 transT) {
    f32 angle = 0.5f * fovY * DC_DEG_TO_RADf;
    f32 cot = 1.0f / tanf(angle);

    m[0][0] = (cot / aspect) * scaleS;
    m[0][1] = 0.0f;
    m[0][2] = -transS;
    m[0][3] = 0.0f;
    m[1][0] = 0.0f;
    m[1][1] = cot * scaleT;
    m[1][2] = -transT;
    m[1][3] = 0.0f;
    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = -1.0f;
    m[2][3] = 0.0f;
}

void C_MTXLightOrtho(MtxP m, f32 t, f32 b, f32 l, f32 r, f32 scaleS, f32 scaleT,
                     f32 transS, f32 transT) {
    f32 tmp;
    tmp = 1.0f / (r - l);
    m[0][0] = 2.0f * tmp * scaleS;
    m[0][1] = 0.0f;
    m[0][2] = 0.0f;
    m[0][3] = (-(r + l) * tmp) * scaleS + transS;

    tmp = 1.0f / (t - b);
    m[1][0] = 0.0f;
    m[1][1] = 2.0f * tmp * scaleT;
    m[1][2] = 0.0f;
    m[1][3] = (-(t + b) * tmp) * scaleT + transT;

    m[2][0] = 0.0f;
    m[2][1] = 0.0f;
    m[2][2] = 0.0f;
    m[2][3] = 1.0f;
}

/* ==========================================================================
 * N64 / libultra fixed-point matrix functions
 * ==========================================================================
 * Scene-setup frequency, not per-frame, and guMtxF2L is integer work. Left
 * scalar on purpose — note the ROW-VECTOR convention here (translation in
 * mf[3][0..2]), which is the opposite of the PSMTX / C_MTX block above and is
 * NOT a bug: libultra and Dolphin genuinely disagree.
 */

void guMtxIdentF(float mf[4][4]) {
    int i, j;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            mf[i][j] = (i == j) ? 1.0f : 0.0f;
}

void guMtxF2L(float mf[4][4], Mtx* m) {
    int i, j;
    int e1, e2;
    int *ai, *af;

    ai = (int*)&m->m[0][0];
    af = (int*)&m->m[2][0];

    for (i = 0; i < 4; i++)
        for (j = 0; j < 2; j++) {
            e1 = FTOFIX32(mf[i][j*2]);
            e2 = FTOFIX32(mf[i][j*2+1]);
            *(ai++) = (e1 & 0xffff0000) | ((e2 >> 16) & 0xffff);
            *(af++) = ((e1 << 16) & 0xffff0000) | (e2 & 0xffff);
        }
}

void guMtxIdent(Mtx* m) {
    float mf[4][4];
    guMtxIdentF(mf);
    guMtxF2L(mf, m);
}

void guOrthoF(float mf[4][4], float l, float r, float b, float t,
              float n, float f, float scale) {
    int i, j;
    guMtxIdentF(mf);
    mf[0][0] = 2.0f / (r - l);
    mf[1][1] = 2.0f / (t - b);
    mf[2][2] = -2.0f / (f - n);
    mf[3][0] = -(r + l) / (r - l);
    mf[3][1] = -(t + b) / (t - b);
    mf[3][2] = -(f + n) / (f - n);
    mf[3][3] = 1.0f;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            mf[i][j] *= scale;
}

void guOrtho(Mtx* m, float l, float r, float b, float t,
             float n, float f, float scale) {
    float mf[4][4];
    guOrthoF(mf, l, r, b, t, n, f, scale);
    guMtxF2L(mf, m);
}

void guPerspectiveF(float mf[4][4], u16* perspNorm, float fovy, float aspect,
                    float near, float far, float scale) {
    float cot;
    int i, j;
    guMtxIdentF(mf);
    fovy *= DC_DEG_TO_RADf;
    cot = cosf(fovy / 2.0f) / sinf(fovy / 2.0f);
    mf[0][0] = cot / aspect;
    mf[1][1] = cot;
    mf[2][2] = (near + far) / (near - far);
    mf[2][3] = -1.0f;
    mf[3][2] = (2.0f * near * far) / (near - far);
    mf[3][3] = 0.0f;
    for (i = 0; i < 4; i++)
        for (j = 0; j < 4; j++)
            mf[i][j] *= scale;
    if (perspNorm != NULL) {
        if (near + far <= 2.0f) {
            *perspNorm = (u16)0xFFFF;
        } else {
            *perspNorm = (u16)((2.0f * 65536.0f) / (near + far));
            if (*perspNorm <= 0)
                *perspNorm = (u16)0x0001;
        }
    }
}

void guPerspective(Mtx* m, u16* perspNorm, float fovy, float aspect,
                   float near, float far, float scale) {
    float mf[4][4];
    guPerspectiveF(mf, perspNorm, fovy, aspect, near, far, scale);
    guMtxF2L(mf, m);
}

void guLookAtF(float mf[4][4], float xEye, float yEye, float zEye,
               float xAt, float yAt, float zAt,
               float xUp, float yUp, float zUp) {
    float len, xLook, yLook, zLook, xRight, yRight, zRight;

    guMtxIdentF(mf);

    xLook = xAt - xEye;
    yLook = yAt - yEye;
    zLook = zAt - zEye;

    len = sqrtf(xLook*xLook + yLook*yLook + zLook*zLook);
    len = -1.0f / len;
    xLook *= len;
    yLook *= len;
    zLook *= len;

    xRight = yUp * zLook - zUp * yLook;
    yRight = zUp * xLook - xUp * zLook;
    zRight = xUp * yLook - yUp * xLook;
    len = sqrtf(xRight*xRight + yRight*yRight + zRight*zRight);
    len = 1.0f / len;
    xRight *= len;
    yRight *= len;
    zRight *= len;

    xUp = yLook * zRight - zLook * yRight;
    yUp = zLook * xRight - xLook * zRight;
    zUp = xLook * yRight - yLook * xRight;
    len = sqrtf(xUp*xUp + yUp*yUp + zUp*zUp);
    len = 1.0f / len;
    xUp *= len;
    yUp *= len;
    zUp *= len;

    mf[0][0] = xRight;
    mf[1][0] = yRight;
    mf[2][0] = zRight;
    mf[3][0] = -(xEye * xRight + yEye * yRight + zEye * zRight);

    mf[0][1] = xUp;
    mf[1][1] = yUp;
    mf[2][1] = zUp;
    mf[3][1] = -(xEye * xUp + yEye * yUp + zEye * zUp);

    mf[0][2] = xLook;
    mf[1][2] = yLook;
    mf[2][2] = zLook;
    mf[3][2] = -(xEye * xLook + yEye * yLook + zEye * zLook);

    mf[0][3] = 0.0f;
    mf[1][3] = 0.0f;
    mf[2][3] = 0.0f;
    mf[3][3] = 1.0f;
}

void guLookAt(Mtx* m, float xEye, float yEye, float zEye,
              float xAt, float yAt, float zAt,
              float xUp, float yUp, float zUp) {
    float mf[4][4];
    guLookAtF(mf, xEye, yEye, zEye, xAt, yAt, zAt, xUp, yUp, zUp);
    guMtxF2L(mf, m);
}

typedef struct {
    unsigned char col[3];  char pad1;
    unsigned char colc[3]; char pad2;
    signed char   dir[3];  char pad3;
} pc_Light_t;
typedef union { pc_Light_t l; long long int force_align[2]; } pc_Light;
typedef struct { pc_Light l[2]; } pc_LookAt;
typedef struct { int x1, y1, x2, y2; } pc_Hilite_t;
typedef union { pc_Hilite_t h; long long int force_align[2]; } pc_Hilite;

#define FTOFRAC8(x) ((int)((x) * 128.0f < 127.0f ? (x) * 128.0f : 127.0f) & 0xff)
#define THRESH2 0.1f

void guLookAtHilite(Mtx* m, void* lv, void* hv,
                    float xEye, float yEye, float zEye,
                    float xAt, float yAt, float zAt,
                    float xUp, float yUp, float zUp,
                    float xl1, float yl1, float zl1,
                    float xl2, float yl2, float zl2,
                    int twidth, int theight) {
    pc_LookAt* l = (pc_LookAt*)lv;
    pc_Hilite* h = (pc_Hilite*)hv;
    float mf[4][4];
    float len, xLook, yLook, zLook, xRight, yRight, zRight;
    float xHilite, yHilite, zHilite;

    guMtxIdentF(mf);

    xLook = xAt - xEye;
    yLook = yAt - yEye;
    zLook = zAt - zEye;

    len = -1.0f / sqrtf(xLook*xLook + yLook*yLook + zLook*zLook);
    xLook *= len; yLook *= len; zLook *= len;

    xRight = yUp * zLook - zUp * yLook;
    yRight = zUp * xLook - xUp * zLook;
    zRight = xUp * yLook - yUp * xLook;
    len = 1.0f / sqrtf(xRight*xRight + yRight*yRight + zRight*zRight);
    xRight *= len; yRight *= len; zRight *= len;

    xUp = yLook * zRight - zLook * yRight;
    yUp = zLook * xRight - xLook * zRight;
    zUp = xLook * yRight - yLook * xRight;
    len = 1.0f / sqrtf(xUp*xUp + yUp*yUp + zUp*zUp);
    xUp *= len; yUp *= len; zUp *= len;

    len = 1.0f / sqrtf(xl1*xl1 + yl1*yl1 + zl1*zl1);
    xl1 *= len; yl1 *= len; zl1 *= len;

    xHilite = xl1 + xLook;
    yHilite = yl1 + yLook;
    zHilite = zl1 + zLook;
    len = sqrtf(xHilite*xHilite + yHilite*yHilite + zHilite*zHilite);
    if (len > THRESH2) {
        len = 1.0f / len;
        xHilite *= len; yHilite *= len; zHilite *= len;
        h->h.x1 = twidth * 4 +
            (int)((xHilite*xRight + yHilite*yRight + zHilite*zRight) * twidth * 2);
        h->h.y1 = theight * 4 +
            (int)((xHilite*xUp + yHilite*yUp + zHilite*zUp) * theight * 2);
    } else {
        h->h.x1 = twidth * 2;
        h->h.y1 = theight * 2;
    }

    len = 1.0f / sqrtf(xl2*xl2 + yl2*yl2 + zl2*zl2);
    xl2 *= len; yl2 *= len; zl2 *= len;

    xHilite = xl2 + xLook;
    yHilite = yl2 + yLook;
    zHilite = zl2 + zLook;
    len = sqrtf(xHilite*xHilite + yHilite*yHilite + zHilite*zHilite);
    if (len > THRESH2) {
        len = 1.0f / len;
        xHilite *= len; yHilite *= len; zHilite *= len;
        h->h.x2 = twidth * 4 +
            (int)((xHilite*xRight + yHilite*yRight + zHilite*zRight) * twidth * 2);
        h->h.y2 = theight * 4 +
            (int)((xHilite*xUp + yHilite*yUp + zHilite*zUp) * theight * 2);
    } else {
        h->h.x2 = twidth * 2;
        h->h.y2 = theight * 2;
    }

    l->l[0].l.dir[0] = FTOFRAC8(xRight);
    l->l[0].l.dir[1] = FTOFRAC8(yRight);
    l->l[0].l.dir[2] = FTOFRAC8(zRight);
    l->l[1].l.dir[0] = FTOFRAC8(xUp);
    l->l[1].l.dir[1] = FTOFRAC8(yUp);
    l->l[1].l.dir[2] = FTOFRAC8(zUp);
    l->l[0].l.col[0] = 0x00; l->l[0].l.col[1] = 0x00;
    l->l[0].l.col[2] = 0x00; l->l[0].l.pad1 = 0x00;
    l->l[0].l.colc[0] = 0x00; l->l[0].l.colc[1] = 0x00;
    l->l[0].l.colc[2] = 0x00; l->l[0].l.pad2 = 0x00;
    l->l[1].l.col[0] = 0x00; l->l[1].l.col[1] = 0x80;
    l->l[1].l.col[2] = 0x00; l->l[1].l.pad1 = 0x00;
    l->l[1].l.colc[0] = 0x00; l->l[1].l.colc[1] = 0x80;
    l->l[1].l.colc[2] = 0x00; l->l[1].l.pad2 = 0x00;

    mf[0][0] = xRight; mf[1][0] = yRight; mf[2][0] = zRight;
    mf[3][0] = -(xEye * xRight + yEye * yRight + zEye * zRight);
    mf[0][1] = xUp;    mf[1][1] = yUp;    mf[2][1] = zUp;
    mf[3][1] = -(xEye * xUp + yEye * yUp + zEye * zUp);
    mf[0][2] = xLook;  mf[1][2] = yLook;  mf[2][2] = zLook;
    mf[3][2] = -(xEye * xLook + yEye * yLook + zEye * zLook);
    mf[0][3] = 0; mf[1][3] = 0; mf[2][3] = 0; mf[3][3] = 1;

    guMtxF2L(mf, m);
}

void guScale(Mtx* m, float x, float y, float z) {
    float mf[4][4];
    guMtxIdentF(mf);
    mf[0][0] = x; mf[1][1] = y; mf[2][2] = z; mf[3][3] = 1.0f;
    guMtxF2L(mf, m);
}

void guTranslate(Mtx* m, float x, float y, float z) {
    float mf[4][4];
    guMtxIdentF(mf);
    mf[3][0] = x; mf[3][1] = y; mf[3][2] = z;
    guMtxF2L(mf, m);
}

void guRotateF(float mf[4][4], float a, float x, float y, float z) {
    float s, c, t;
    float len = sqrtf(x*x + y*y + z*z);
    if (len > 0.0f) { x /= len; y /= len; z /= len; }

    a *= DC_DEG_TO_RADf;
    s = sinf(a);
    c = cosf(a);
    t = 1.0f - c;

    guMtxIdentF(mf);
    mf[0][0] = t*x*x + c;    mf[0][1] = t*x*y + s*z; mf[0][2] = t*x*z - s*y;
    mf[1][0] = t*x*y - s*z;  mf[1][1] = t*y*y + c;   mf[1][2] = t*y*z + s*x;
    mf[2][0] = t*x*z + s*y;  mf[2][1] = t*y*z - s*x; mf[2][2] = t*z*z + c;
}

void guRotate(Mtx* m, float a, float x, float y, float z) {
    float mf[4][4];
    guRotateF(mf, a, x, y, z);
    guMtxF2L(mf, m);
}

void guNormalize(float* x, float* y, float* z) {
    float norm = sqrtf(*x * *x + *y * *y + *z * *z);
    if (norm > 0.0f) {
        norm = 1.0f / norm;
        *x *= norm;
        *y *= norm;
        *z *= norm;
    }
}

/* Silence "defined but not used" for the typedefs the decomp headers would
 * normally consume; keeping them here documents the layouts this TU assumes. */
typedef char dc_mtx_layout_assert_[
    (sizeof(Mtx) == 64 && sizeof(Mtx34) == 48 && sizeof(Mtx44) == 64) ? 1 : -1];
