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
 *  - PSVECDotProduct / PSVECMag / PSVECNormalize. SH-4 has FIPR (4-way dot)
 *    and FSRRA (reciprocal sqrt), but both are ~20-bit-mantissa APPROXIMATIONS
 *    where the GameCube used full single precision. These feed lighting
 *    normals and, through C_MTXLookAt, the camera basis. TODO(M3): measure
 *    whether the error is visible before enabling; DC_MTX_USE_FIPR turns them
 *    on for that experiment and is OFF by default.
 *  - PSMTXInverse. Not hot, and the cofactor expansion has no FTRV shape.
 *  - Everything gu* (N64 fixed-point). Called at scene setup, not per frame,
 *    and guMtxF2L is integer bit-twiddling anyway.
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

/* OFF by default — see "WHAT IS DELIBERATELY NOT ACCELERATED" above. */
#ifndef DC_MTX_USE_FIPR
#define DC_MTX_USE_FIPR 0
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
#endif /* DC_MTX_USE_XMTRX */

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
    dc_mtx_expand44(b, mb);
    mat_load((const matrix_t*)&mb);

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

void PSMTXMultVec(const MtxP m, const Vec* src, Vec* dst) {
#if DC_MTX_USE_XMTRX
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
#if DC_MTX_USE_XMTRX
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

    dc_mtx_transpose44(m, mt);
    mat_load((const matrix_t*)&mt);

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
 * FIPR/FSRRA are ~20-bit approximations. These feed lighting normals and the
 * C_MTXLookAt camera basis, so they stay full precision by default. */

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
