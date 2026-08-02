/* dc_pvr_texture.c - the texture half of the PowerVR backend.
 *
 * Contract: dc/include/dc_pvr.h. This file decodes a GameCube texture out of
 * the game's own memory into twiddled 16-bit PVR-native texels, uploads it to
 * VRAM, and hands dc_pvr.c an opaque handle. It never emits a vertex; dc_pvr.c
 * never decodes a texel.
 *
 * =========================================================================
 * WHERE THE DECODERS CAME FROM
 * =========================================================================
 * The ten GC format decoders below are ports of pc/src/pc_gx_texture.c, whose
 * tile de-swizzling has been endianness-audited against the real ISO and is
 * the part that is easy to get subtly wrong. The 4x4 / 8x4 / 8x8 block walks,
 * the bounds checks on partial edge blocks, the two-pass AR/GB layout of
 * RGBA8, the BE reads and the CMPR sub-block order are COPIED VERBATIM. Three
 * things changed, and only these three:
 *
 *   1. The NEON paths are gone (this is SH-4). Only the scalar paths survive,
 *      byte-for-byte.
 *   2. The destination store changed. PC writes four bytes of linear RGBA8
 *      (`dst[idx] = r; dst[idx+1] = g; ...`); we write ONE 16-bit PVR texel
 *      through EMIT(), which packs into whichever of RGB565 / ARGB1555 /
 *      ARGB4444 the alpha survey picked for this texture.
 *   3. The destination pitch is the POWER-OF-TWO padded width, not `w`. The
 *      loops still bound against the GX `w`/`h`; only the row stride differs,
 *      so a NPOT source decodes into the top-left of a padded image.
 *
 * =========================================================================
 * FORMAT SELECTION — one survey, three targets
 * =========================================================================
 * The PVR has no 32-bit texture format worth using and no per-texel alpha
 * beyond 4 bits, so every source collapses into one of three 16-bit formats.
 * Rather than hardcode a per-source-format mapping, a cheap linear pre-pass
 * (survey_alpha(); it reads the raw source bytes, not the decoded image, so it
 * needs no tiling logic) classifies the texture's ALPHA into:
 *
 *   class 0  every texel opaque              -> PVR_TXRFMT_RGB565    has_alpha 0
 *   class 1  alpha is only ever 0 or 255     -> PVR_TXRFMT_ARGB1555  has_alpha 1
 *   class 2  anything in between             -> PVR_TXRFMT_ARGB4444  has_alpha 1
 *
 * That lands exactly where the hand mapping would: RGB565 for I4/I8/RGB565
 * sheets that are fully lit, ARGB1555 for CMPR punch-through and for the
 * bit-15 case of RGB5A3, ARGB4444 for IA4/IA8/RGB5A3-with-graded-alpha/RGBA8.
 * dc_pvr.c reads `has_alpha` to choose the PVR list and blend mode.
 *
 * NOTE ON I4/I8. GX intensity formats expand to (I,I,I,I) — the intensity IS
 * the alpha, which is why pc_gx_texture.c writes `dst[idx+3] = i`. We keep
 * that: a JUTResFont glyph sheet is I4, and dropping its alpha would render
 * text as opaque black boxes. Consequence: a font sheet surveys as class 2 and
 * therefore reports has_alpha, which is correct but does mean intensity
 * textures are not automatically opaque-list candidates.
 *
 * =========================================================================
 * SCRATCH: 131,072 B, and why that number
 * =========================================================================
 * Main RAM is the project's blocking constraint (kb/STATE.md: the image is
 * ~7 MB over 16 MB today), so there is no per-texture malloc and no big
 * permanent staging buffer. Decode runs through ONE static scratch buffer:
 *
 *   DC_PVR_TEX_SCRATCH_TEXELS = 65,536 texels = 131,072 B  (16-bit texels)
 *
 * A texture is accepted iff its POWER-OF-TWO-PADDED area fits, i.e.
 * pot_w * pot_h <= 65,536, with each side <= 1024. That admits 256x256,
 * 512x128, 128x512, 1024x64 and everything smaller — which covers every
 * texture the two real producers emit:
 *
 *   - emu64 (src/static/libforest/emu64/emu64.c:2265,3387) converts N64 tiles,
 *     and an N64 tile is bounded by 4 KB of TMEM, so its GXInitTexObj[CI]
 *     dimensions are small by construction.
 *   - JSystem font/UI sheets (JUTResFont.cpp:438) are 256-wide class assets.
 *
 * The ONE known rejection is the NES emulator's CHR sheet
 * (src/static/Famicom/ks_nes_draw.cpp:414,419 — 1024x256 GX_TF_I8, 262,144
 * texels). Accepting it would cost a permanent 512 KB of .bss to make an
 * unlockable minigame textured, on a target that is already 7 MB over. It is
 * rejected loudly via DC_LOGE and drawn untextured. Raise
 * -DDC_PVR_TEX_SCRATCH_TEXELS=262144 if that minigame ever matters more than
 * 384 KB of RAM.
 *
 * Fixed .bss cost of this file, for the ledger. MEASURED with sh-elf-size on
 * the object built with the Makefile's dc/src flag set:
 *   scratch                                                      131,072 B
 *   entry table   512 * 68 (sizeof dc_tex_entry_t, sh-elf ILP32)  34,816 B
 *   hash index    1024 * 2                                         2,048 B
 *   counters / decode cursor                                          36 B
 *                                                              ------------
 *                                                                 167,972 B
 * plus 18,383 B of .text. Both tables are knobs (DC_PVR_TEX_MAX_ENTRIES,
 * DC_PVR_TEX_SCRATCH_TEXELS) if the ledger needs the space back, and
 * -DDC_PVR_NO_TEXTURES takes the whole file to 318 B of .text and zero .bss.
 *
 * =========================================================================
 * KILL SWITCH
 * =========================================================================
 * -DDC_PVR_NO_TEXTURES compiles this whole file down to "always return 0",
 * so a build can be bisected against untextured rendering.
 */

#include "dc_pvr.h"
#include "dc_gx_internal.h"
#include <dolphin/gx/GXEnum.h>

/* --- KOS surface -----------------------------------------------------------
 * dc_platform.h already pulls <dc/pvr.h> behind #ifndef DC_HOST_STUB; the
 * include below is the same pattern, stated explicitly so this file's
 * dependency is visible. The DC_HOST_STUB arm exists only so the file can be
 * syntax-checked on a host with no sh-elf toolchain — the real build never
 * defines it. */
#ifndef DC_HOST_STUB
#include <dc/pvr.h>
#else
typedef void* pvr_ptr_t;
#define PVR_TXRFMT_VQ_DISABLE   (0u << 30)
#define PVR_TXRFMT_ARGB1555     (0u << 27)
#define PVR_TXRFMT_RGB565       (1u << 27)
#define PVR_TXRFMT_ARGB4444     (2u << 27)
#define PVR_TXRFMT_TWIDDLED     (0u << 26)
#define PVR_TXRFMT_POW2_STRIDE  (0u << 25)
#define PVR_TXRLOAD_16BPP       0x03
static pvr_ptr_t pvr_mem_malloc(size_t sz) { (void)sz; return 0; }
static void pvr_mem_free(pvr_ptr_t p) { (void)p; }
static void pvr_txr_load_ex(const void* s, pvr_ptr_t d, unsigned int w,
                            unsigned int h, unsigned int f) {
    (void)s; (void)d; (void)w; (void)h; (void)f;
}
#endif

/* ==========================================================================
 * Tunables
 * ========================================================================== */

/* Texture VRAM ceiling. 4 MB of the 8 MB total: the other 4 MB is the frame
 * buffers, the Z/tile accumulation buffers and the TA vertex/OPB pool that
 * dc_pvr.c hands to pvr_init(). This is a SOFT ceiling enforced by LRU
 * eviction; the hard one is whatever pvr_mem_malloc() actually has left, and
 * both are honoured. */
#ifndef DC_PVR_TEX_VRAM_BYTES
#define DC_PVR_TEX_VRAM_BYTES  (4u * 1024u * 1024u)
#endif

/* See the header comment. 65,536 texels = 131,072 B. */
#ifndef DC_PVR_TEX_SCRATCH_TEXELS
#define DC_PVR_TEX_SCRATCH_TEXELS  65536
#endif

/* Live texture records. 512 * 4 KB average is well past the 4 MB ceiling, so
 * VRAM runs out before the table does; the table is sized so a table-full
 * eviction is not the common case. */
#ifndef DC_PVR_TEX_MAX_ENTRIES
#define DC_PVR_TEX_MAX_ENTRIES  512
#endif

/* Open-addressed content index. Power of two, >= 2 * MAX_ENTRIES. */
#define DC_PVR_TEX_INDEX_SIZE   1024

/* PVR minimum texture side. Anything smaller pads up to this. */
#define DC_PVR_TEX_MIN_DIM      8
#define DC_PVR_TEX_MAX_DIM      1024

#ifndef DC_PVR_NO_TEXTURES

/* ==========================================================================
 * Alpha classes
 * ========================================================================== */
#define DC_TEX_OPAQUE  0    /* every texel alpha == 255      -> RGB565   */
#define DC_TEX_BINARY  1    /* alpha only ever 0 or 255      -> ARGB1555 */
#define DC_TEX_GRADED  2    /* anything else                 -> ARGB4444 */

/* ==========================================================================
 * Cache entry
 * ========================================================================== */
typedef struct {
    dc_pvr_tex_t  rec;          /* what dc_pvr.c consumes                    */

    /* content key — an identical tuple must reuse the upload */
    unsigned int  data_ptr;
    unsigned int  data_hash;
    unsigned int  tlut_ptr;
    unsigned int  tlut_hash;
    unsigned short gx_w, gx_h;
    unsigned short fmt, ci_fmt;
    unsigned short tlut_fmt, tlut_count;

    unsigned int  bytes;        /* VRAM footprint                            */
    unsigned int  lru;          /* s_clock stamp of last touch               */
    unsigned short gen;         /* bumped on release/evict -> stale handles  */
    unsigned short refs;        /* GXTexObjs sharing this upload             */
    unsigned char  live;
} dc_tex_entry_t;

static dc_tex_entry_t  s_tex[DC_PVR_TEX_MAX_ENTRIES];
static unsigned short  s_index[DC_PVR_TEX_INDEX_SIZE];  /* entry_idx + 1     */
static unsigned int    s_clock;
static unsigned int    s_bytes_resident;

static unsigned int    s_stat_uploads;
static unsigned int    s_stat_hits;
static unsigned int    s_stat_evictions;
static unsigned int    s_stat_rejects;

/* The one staging buffer. 8-byte aligned so pvr_txr_load_ex's 16-bit reads
 * never straddle awkwardly. */
static unsigned short s_scratch[DC_PVR_TEX_SCRATCH_TEXELS]
    __attribute__((aligned(8)));

/* Decode target: EMIT() writes s_out[py * s_pitch + px]. */
static unsigned short* s_out;
static int             s_pitch;
static int             s_class;

/* ==========================================================================
 * Texel packing
 * ========================================================================== */
static unsigned short dc_pack(u8 r, u8 g, u8 b, u8 a) {
    switch (s_class) {
        case DC_TEX_OPAQUE:
            return (unsigned short)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
        case DC_TEX_BINARY:
            return (unsigned short)(((a >= 128) ? 0x8000u : 0u) |
                                    ((unsigned)(r >> 3) << 10) |
                                    ((unsigned)(g >> 3) << 5) |
                                    (unsigned)(b >> 3));
        default:
            return (unsigned short)(((unsigned)(a >> 4) << 12) |
                                    ((unsigned)(r >> 4) << 8) |
                                    ((unsigned)(g >> 4) << 4) |
                                    (unsigned)(b >> 4));
    }
}

#define EMIT(px, py, r, g, b, a) \
    (s_out[(py) * s_pitch + (px)] = dc_pack((r), (g), (b), (a)))

/* ==========================================================================
 * Source geometry helpers
 * ========================================================================== */

/* Read a big-endian u16 (GC textures store 16-bit pixels in BE order).
 * Verbatim from pc_gx_texture.c:299. */
static u16 read_be16(const u8* p) {
    return (u16)((p[0] << 8) | p[1]);
}

/* Bits-per-pixel for each GC texture format (pc_gx_texture.c:39). */
static int gc_format_bpp(int format) {
    switch (format) {
        case GX_TF_I4: case GX_TF_C4: case GX_TF_CMPR: return 4;
        case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8: return 8;
        case GX_TF_IA8: case GX_TF_RGB565: case GX_TF_RGB5A3: return 16;
        case GX_TF_RGBA8: return 32;
        default: return 8;
    }
}

/* Exact encoded size, rounded up to whole tiles. pc_gx_texture.c uses the
 * un-rounded w*h*bpp/8 for hashing only; here the survey walks the buffer, so
 * the tile-accurate figure is the one that must not lie. */
static int gc_data_size(int w, int h, int format) {
    int tw = 4, th = 4;
    int bpp = gc_format_bpp(format);
    switch (format) {
        case GX_TF_I4: case GX_TF_C4: case GX_TF_CMPR: tw = 8; th = 8; break;
        case GX_TF_I8: case GX_TF_IA4: case GX_TF_C8:  tw = 8; th = 4; break;
        default: break;   /* IA8 / RGB565 / RGB5A3 / RGBA8 are all 4x4 */
    }
    w = (w + tw - 1) / tw * tw;
    h = (h + th - 1) / th * th;
    return (w * h * bpp) / 8;
}

/* ==========================================================================
 * Content hashing (pc_gx_texture.c:51 / :79, FNV-1a, same constants)
 * ========================================================================== */
static unsigned int tex_content_hash(const void* data, int w, int h, int format) {
    int data_size;
    const u8* p;
    unsigned int hv;
    int i;
    if (!data) return 0;
    data_size = gc_data_size(w, h, format);
    p = (const u8*)data;
    hv = 0x811c9dc5u;
    if (data_size <= 512) {
        for (i = 0; i < data_size; i++) { hv ^= p[i]; hv *= 0x01000193u; }
    } else {
        for (i = 0; i < 256; i++)               { hv ^= p[i]; hv *= 0x01000193u; }
        for (i = data_size - 256; i < data_size; i++) { hv ^= p[i]; hv *= 0x01000193u; }
    }
    return hv;
}

static unsigned int tlut_content_hash(const void* data, int tlut_fmt,
                                      int n_entries, int is_be) {
    int bytes, i;
    const u8* p;
    unsigned int hv;
    if (!data || n_entries <= 0) return 0;
    bytes = n_entries * 2;
    if (bytes > 512) bytes = 512;
    p = (const u8*)data;
    hv = 0x811c9dc5u;
    for (i = 0; i < bytes; i++) { hv ^= p[i]; hv *= 0x01000193u; }
    hv ^= (unsigned int)(tlut_fmt & 0xFF);      hv *= 0x01000193u;
    hv ^= (unsigned int)(n_entries & 0xFFFF);   hv *= 0x01000193u;
    hv ^= (unsigned int)(is_be & 1);            hv *= 0x01000193u;
    return hv;
}

/* ==========================================================================
 * TLUT endianness
 * ==========================================================================
 * The backend seam does not carry an is_be flag, but g_gx.tlut[] does, and
 * dc_gx.c derived the (tlut, fmt, count) triple straight out of that array.
 * Recover the flag by matching the pointer back to its slot. ROM/JSystem
 * palettes are BE and emu64's tlutconv output is native LE; getting this wrong
 * byte-swaps every palette entry, so the default when no slot matches is 1
 * (BE), which is also GXLoadTlut's default in dc_gx.c:1943.
 */
static int tlut_is_be(const void* tlut) {
    int i;
    if (!tlut) return 1;
    for (i = 0; i < 16; i++) {
        if (g_gx.tlut[i].data == tlut) return g_gx.tlut[i].is_be;
    }
    return 1;
}

/* ==========================================================================
 * GC format decoders — ports of pc/src/pc_gx_texture.c scalar paths.
 * Tiling is verbatim; only the store and the pitch differ. See the file
 * header for the exact list of changes.
 * ========================================================================== */

/* I4: 8x8 blocks, 4bpp, each byte = 2 pixels. Intensity replicates into
 * alpha, per GX (pc_gx_texture.c:335). */
static void decode_I4(const u8* src, int w, int h) {
    int bw = (w + 7) / 8, bh = (h + 7) / 8;
    int by, bx, x, y;
    for (by = 0; by < bh; by++) {
        for (bx = 0; bx < bw; bx++) {
            for (y = 0; y < 8; y++) {
                for (x = 0; x < 8; x += 2) {
                    u8 val = *src++;
                    int px0 = bx * 8 + x, py = by * 8 + y;
                    int px1 = px0 + 1;
                    u8 i0 = (u8)((val >> 4) | (val & 0xF0));
                    u8 i1 = (u8)((val & 0x0F) | ((val & 0x0F) << 4));
                    if (px0 < w && py < h) EMIT(px0, py, i0, i0, i0, i0);
                    if (px1 < w && py < h) EMIT(px1, py, i1, i1, i1, i1);
                }
            }
        }
    }
}

/* I8: 8x4 blocks, 8bpp (pc_gx_texture.c:363, scalar tail). */
static void decode_I8(const u8* src, int w, int h) {
    int bw = (w + 7) / 8, bh = (h + 3) / 4;
    int by, bx, x, y;
    for (by = 0; by < bh; by++) {
        for (bx = 0; bx < bw; bx++) {
            for (y = 0; y < 4; y++) {
                for (x = 0; x < 8; x++) {
                    u8 val = *src++;
                    int px = bx * 8 + x, py = by * 4 + y;
                    if (px < w && py < h) EMIT(px, py, val, val, val, val);
                }
            }
        }
    }
}

/* IA4: 8x4 blocks, high nibble = alpha, low nibble = intensity
 * (pc_gx_texture.c:414). */
static void decode_IA4(const u8* src, int w, int h) {
    int bw = (w + 7) / 8, bh = (h + 3) / 4;
    int by, bx, x, y;
    for (by = 0; by < bh; by++) {
        for (bx = 0; bx < bw; bx++) {
            for (y = 0; y < 4; y++) {
                for (x = 0; x < 8; x++) {
                    u8 val = *src++;
                    int px = bx * 8 + x, py = by * 4 + y;
                    if (px < w && py < h) {
                        u8 a = (u8)((val >> 4) | (val & 0xF0));
                        u8 i = (u8)((val & 0x0F) | ((val & 0x0F) << 4));
                        EMIT(px, py, i, i, i, a);
                    }
                }
            }
        }
    }
}

/* IA8: 4x4 blocks, 16bpp, alpha byte then intensity byte
 * (pc_gx_texture.c:435, scalar tail). */
static void decode_IA8(const u8* src, int w, int h) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    int by, bx, x, y;
    for (by = 0; by < bh; by++) {
        for (bx = 0; bx < bw; bx++) {
            for (y = 0; y < 4; y++) {
                for (x = 0; x < 4; x++) {
                    u8 a = *src++;
                    u8 i = *src++;
                    int px = bx * 4 + x, py = by * 4 + y;
                    if (px < w && py < h) EMIT(px, py, i, i, i, a);
                }
            }
        }
    }
}

/* Decode a single RGB565 value to RGBA8 (pc_gx_texture.c:731). */
static void decode_rgb565_entry(u16 val, u8* r, u8* g, u8* b, u8* a) {
    *r = (u8)(((val >> 11) & 0x1F) * 255 / 31);
    *g = (u8)(((val >> 5) & 0x3F) * 255 / 63);
    *b = (u8)((val & 0x1F) * 255 / 31);
    *a = 255;
}

/* Decode a single RGB5A3 value to RGBA8 (pc_gx_texture.c:716). */
static void decode_rgb5a3_entry(u16 val, u8* r, u8* g, u8* b, u8* a) {
    if (val & 0x8000) { /* RGB555 opaque */
        *r = (u8)(((val >> 10) & 0x1F) * 255 / 31);
        *g = (u8)(((val >> 5) & 0x1F) * 255 / 31);
        *b = (u8)((val & 0x1F) * 255 / 31);
        *a = 255;
    } else {            /* ARGB3444 */
        *a = (u8)(((val >> 12) & 0x07) * 255 / 7);
        *r = (u8)(((val >> 8) & 0x0F) * 255 / 15);
        *g = (u8)(((val >> 4) & 0x0F) * 255 / 15);
        *b = (u8)((val & 0x0F) * 255 / 15);
    }
}

/* RGB565: 4x4 blocks, 16bpp (pc_gx_texture.c:490, scalar tail). */
static void decode_RGB565(const u8* src, int w, int h) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    int by, bx, x, y;
    for (by = 0; by < bh; by++) {
        for (bx = 0; bx < bw; bx++) {
            for (y = 0; y < 4; y++) {
                for (x = 0; x < 4; x++) {
                    u16 val = (u16)((src[0] << 8) | src[1]);
                    int px = bx * 4 + x, py = by * 4 + y;
                    src += 2;
                    if (px < w && py < h) {
                        u8 r = (u8)(((val >> 11) & 0x1F) * 255 / 31);
                        u8 g = (u8)(((val >> 5) & 0x3F) * 255 / 63);
                        u8 b = (u8)((val & 0x1F) * 255 / 31);
                        EMIT(px, py, r, g, b, 255);
                    }
                }
            }
        }
    }
}

/* RGB5A3: 4x4 blocks, 16bpp. Bit 15=1: RGB555 opaque, bit 15=0: ARGB3444
 * (pc_gx_texture.c:559, scalar tail). */
static void decode_RGB5A3(const u8* src, int w, int h) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    int by, bx, x, y;
    for (by = 0; by < bh; by++) {
        for (bx = 0; bx < bw; bx++) {
            for (y = 0; y < 4; y++) {
                for (x = 0; x < 4; x++) {
                    u16 val = (u16)((src[0] << 8) | src[1]);
                    int px = bx * 4 + x, py = by * 4 + y;
                    src += 2;
                    if (px < w && py < h) {
                        u8 r, g, b, a;
                        decode_rgb5a3_entry(val, &r, &g, &b, &a);
                        EMIT(px, py, r, g, b, a);
                    }
                }
            }
        }
    }
}

/* RGBA8: 4x4 blocks, 32bpp. Two passes per block: AR then GB, 64 B total
 * (pc_gx_texture.c:637). */
static void decode_RGBA8(const u8* src, int w, int h) {
    int bw = (w + 3) / 4, bh = (h + 3) / 4;
    int by, bx, i;
    for (by = 0; by < bh; by++) {
        for (bx = 0; bx < bw; bx++) {
            u8 ar[16][2];               /* AR pass */
            for (i = 0; i < 16; i++) {
                ar[i][0] = *src++;
                ar[i][1] = *src++;
            }
            for (i = 0; i < 16; i++) {  /* GB pass */
                int x = i % 4, y = i / 4;
                int px = bx * 4 + x, py = by * 4 + y;
                u8 g = *src++;
                u8 b = *src++;
                if (px < w && py < h) EMIT(px, py, ar[i][1], g, b, ar[i][0]);
            }
        }
    }
}

/* CMPR (S3TC/DXT1): 8x8 super-blocks of 2x2 sub-blocks, each sub a 4x4 DXT1
 * block with BE endpoints (pc_gx_texture.c:664). */
static void decode_CMPR(const u8* src, int w, int h) {
    int bw = (w + 7) / 8, bh = (h + 7) / 8;
    int by, bx, sub, c, x, y;
    for (by = 0; by < bh; by++) {
        for (bx = 0; bx < bw; bx++) {
            for (sub = 0; sub < 4; sub++) {
                int sx = (sub & 1) * 4, sy = (sub >> 1) * 4;
                u16 c0 = (u16)((src[0] << 8) | src[1]); /* DXT1 block (BE) */
                u16 c1 = (u16)((src[2] << 8) | src[3]);
                u8 palette[4][4];
                src += 4;
                palette[0][0] = (u8)(((c0 >> 11) & 0x1F) * 255 / 31);
                palette[0][1] = (u8)(((c0 >> 5) & 0x3F) * 255 / 63);
                palette[0][2] = (u8)((c0 & 0x1F) * 255 / 31);
                palette[0][3] = 255;
                palette[1][0] = (u8)(((c1 >> 11) & 0x1F) * 255 / 31);
                palette[1][1] = (u8)(((c1 >> 5) & 0x3F) * 255 / 63);
                palette[1][2] = (u8)((c1 & 0x1F) * 255 / 31);
                palette[1][3] = 255;
                if (c0 > c1) {
                    for (c = 0; c < 3; c++) {
                        palette[2][c] = (u8)((2 * palette[0][c] + palette[1][c]) / 3);
                        palette[3][c] = (u8)((palette[0][c] + 2 * palette[1][c]) / 3);
                    }
                    palette[2][3] = palette[3][3] = 255;
                } else {
                    for (c = 0; c < 3; c++)
                        palette[2][c] = (u8)((palette[0][c] + palette[1][c]) / 2);
                    palette[2][3] = 255;
                    palette[3][0] = palette[3][1] = palette[3][2] = 0;
                    palette[3][3] = 0;
                }
                for (y = 0; y < 4; y++) {
                    u8 row = *src++;
                    for (x = 0; x < 4; x++) {
                        int ci = (row >> (6 - x * 2)) & 3;
                        int px = bx * 8 + sx + x, py = by * 8 + sy + y;
                        if (px < w && py < h)
                            EMIT(px, py, palette[ci][0], palette[ci][1],
                                 palette[ci][2], palette[ci][3]);
                    }
                }
            }
        }
    }
}

/* Build a 256-entry RGBA8 palette from TLUT data (pc_gx_texture.c:740).
 * is_be=1 for ROM/JSystem data (BE), 0 for emu64 tlutconv output (native LE). */
static void build_palette(const void* tlut_data, int tlut_fmt, int n_entries,
                          u8 palette[256][4], int is_be) {
    const u16* pal16;
    const u8* pal_bytes;
    int i;
    /* default: grayscale ramp for missing palette */
    for (i = 0; i < 256; i++) {
        palette[i][0] = palette[i][1] = palette[i][2] = (u8)i;
        palette[i][3] = 255;
    }
    if (!tlut_data || n_entries <= 0) return;
    if (n_entries > 256) n_entries = 256;

    pal16 = (const u16*)tlut_data;
    pal_bytes = (const u8*)tlut_data;
    for (i = 0; i < n_entries; i++) {
        u16 val;
        if (is_be) val = read_be16(pal_bytes + i * 2);
        else       val = pal16[i];
        if (tlut_fmt == GX_TL_RGB5A3) {
            decode_rgb5a3_entry(val, &palette[i][0], &palette[i][1],
                                &palette[i][2], &palette[i][3]);
        } else if (tlut_fmt == GX_TL_RGB565) {
            decode_rgb565_entry(val, &palette[i][0], &palette[i][1],
                                &palette[i][2], &palette[i][3]);
        } else { /* GX_TL_IA8: high byte = intensity, low byte = alpha */
            if (is_be) {
                palette[i][0] = palette[i][1] = palette[i][2] = (u8)(val >> 8);
                palette[i][3] = (u8)(val & 0xFF);
            } else {
                /* LE read of BE bytes [I,A] swaps them */
                palette[i][0] = palette[i][1] = palette[i][2] = (u8)(val & 0xFF);
                palette[i][3] = (u8)(val >> 8);
            }
        }
    }
}

/* CI4: 8x8 blocks, 4bpp indexed (pc_gx_texture.c:779). */
static void decode_CI4(const u8* src, int w, int h, const u8 palette[256][4]) {
    int bw = (w + 7) / 8, bh = (h + 7) / 8;
    int by, bx, x, y;
    for (by = 0; by < bh; by++) {
        for (bx = 0; bx < bw; bx++) {
            for (y = 0; y < 8; y++) {
                for (x = 0; x < 8; x += 2) {
                    u8 val = *src++;
                    int px0 = bx * 8 + x, py = by * 8 + y;
                    int px1 = px0 + 1;
                    int ci0 = (val >> 4) & 0xF;
                    int ci1 = val & 0xF;
                    if (px0 < w && py < h)
                        EMIT(px0, py, palette[ci0][0], palette[ci0][1],
                             palette[ci0][2], palette[ci0][3]);
                    if (px1 < w && py < h)
                        EMIT(px1, py, palette[ci1][0], palette[ci1][1],
                             palette[ci1][2], palette[ci1][3]);
                }
            }
        }
    }
}

/* CI8: 8x4 blocks, 8bpp indexed (pc_gx_texture.c:807, scalar tail). */
static void decode_CI8(const u8* src, int w, int h, const u8 palette[256][4]) {
    int bw = (w + 7) / 8, bh = (h + 3) / 4;
    int by, bx, x, y;
    for (by = 0; by < bh; by++) {
        for (bx = 0; bx < bw; bx++) {
            for (y = 0; y < 4; y++) {
                for (x = 0; x < 8; x++) {
                    u8 val = *src++;
                    int px = bx * 8 + x, py = by * 4 + y;
                    if (px < w && py < h)
                        EMIT(px, py, palette[val][0], palette[val][1],
                             palette[val][2], palette[val][3]);
                }
            }
        }
    }
}

static void decode_gc_texture(const void* src, int w, int h, int fmt,
                              const u8 palette[256][4]) {
    switch (fmt) {
        case GX_TF_I4:     decode_I4((const u8*)src, w, h); break;
        case GX_TF_I8:     decode_I8((const u8*)src, w, h); break;
        case GX_TF_IA4:    decode_IA4((const u8*)src, w, h); break;
        case GX_TF_IA8:    decode_IA8((const u8*)src, w, h); break;
        case GX_TF_RGB565: decode_RGB565((const u8*)src, w, h); break;
        case GX_TF_RGB5A3: decode_RGB5A3((const u8*)src, w, h); break;
        case GX_TF_RGBA8:  decode_RGBA8((const u8*)src, w, h); break;
        case GX_TF_C4:     decode_CI4((const u8*)src, w, h, palette); break;
        case GX_TF_C8:     decode_CI8((const u8*)src, w, h, palette); break;
        case GX_TF_CMPR:   decode_CMPR((const u8*)src, w, h); break;
        default: break;   /* scratch was cleared: unhandled = transparent */
    }
}

/* ==========================================================================
 * Alpha survey
 * ==========================================================================
 * Runs over the RAW source bytes. Every GC format places its alpha at a fixed
 * byte/nibble offset inside its tile stream, and the survey does not care WHICH
 * texel a value belongs to — only what values occur — so no tiling logic is
 * needed here and none is duplicated.
 */
static int classify(int any_mid, int any_clear) {
    if (any_mid)   return DC_TEX_GRADED;
    if (any_clear) return DC_TEX_BINARY;
    return DC_TEX_OPAQUE;
}

/* 4-bit alpha channel: 0 -> clear, 15 -> opaque, else graded. */
static void survey_nib(unsigned int n, int* mid, int* clear) {
    if (n == 0)       *clear = 1;
    else if (n != 15) *mid = 1;
}

/* 8-bit alpha channel. */
static void survey_byte(unsigned int v, int* mid, int* clear) {
    if (v == 0)        *clear = 1;
    else if (v != 255) *mid = 1;
}

static int survey_alpha(const void* data, int w, int h, int fmt,
                        const u8 palette[256][4], int pal_entries) {
    const u8* p = (const u8*)data;
    int size = gc_data_size(w, h, fmt);
    int mid = 0, clear = 0;
    int i;

    switch (fmt) {
        case GX_TF_RGB565:
            return DC_TEX_OPAQUE;

        case GX_TF_I4:
            for (i = 0; i < size; i++) {
                survey_nib((unsigned int)(p[i] >> 4), &mid, &clear);
                survey_nib((unsigned int)(p[i] & 0x0F), &mid, &clear);
                if (mid) return DC_TEX_GRADED;
            }
            break;

        case GX_TF_I8:
            for (i = 0; i < size; i++) {
                survey_byte(p[i], &mid, &clear);
                if (mid) return DC_TEX_GRADED;
            }
            break;

        case GX_TF_IA4:   /* high nibble is alpha */
            for (i = 0; i < size; i++) {
                survey_nib((unsigned int)(p[i] >> 4), &mid, &clear);
                if (mid) return DC_TEX_GRADED;
            }
            break;

        case GX_TF_IA8:   /* [A,I] pairs */
            for (i = 0; i + 1 < size; i += 2) {
                survey_byte(p[i], &mid, &clear);
                if (mid) return DC_TEX_GRADED;
            }
            break;

        case GX_TF_RGB5A3:
            for (i = 0; i + 1 < size; i += 2) {
                u16 v = read_be16(p + i);
                if (v & 0x8000) continue;               /* opaque RGB555 */
                /* 3-bit alpha rescaled into the 0..15 the nibble survey
                 * expects: 0 stays clear, 7 stays opaque, the rest are mid. */
                survey_nib(((unsigned int)((v >> 12) & 0x07) * 15u) / 7u,
                           &mid, &clear);
                if (mid) return DC_TEX_GRADED;
            }
            break;

        case GX_TF_RGBA8: /* 64-B blocks: first 32 B are [A,R] pairs */
            for (i = 0; i + 63 < size; i += 64) {
                int k;
                for (k = 0; k < 32; k += 2) {
                    survey_byte(p[i + k], &mid, &clear);
                    if (mid) return DC_TEX_GRADED;
                }
            }
            break;

        case GX_TF_CMPR:  /* 8-B DXT1 blocks; c0 <= c1 enables index 3 = clear */
            for (i = 0; i + 7 < size; i += 8) {
                u16 c0 = read_be16(p + i);
                u16 c1 = read_be16(p + i + 2);
                int k;
                if (c0 > c1) continue;
                for (k = 4; k < 8; k++) {
                    u8 row = p[i + k];
                    if (((row >> 6) & 3) == 3 || ((row >> 4) & 3) == 3 ||
                        ((row >> 2) & 3) == 3 || (row & 3) == 3) {
                        clear = 1;
                        break;
                    }
                }
            }
            break;

        case GX_TF_C4:
        case GX_TF_C8: {  /* the palette carries the alpha */
            int n = pal_entries;
            if (n <= 0) return DC_TEX_OPAQUE;    /* grayscale fallback ramp */
            if (n > 256) n = 256;
            if (fmt == GX_TF_C4 && n > 16) n = 16;
            for (i = 0; i < n; i++) {
                survey_byte(palette[i][3], &mid, &clear);
                if (mid) return DC_TEX_GRADED;
            }
            break;
        }

        default:
            return DC_TEX_GRADED;   /* unknown: assume the worst, stay correct */
    }
    return classify(mid, clear);
}

/* ==========================================================================
 * Handle <-> entry
 * ==========================================================================
 * handle = (gen << 12) | (index + 1). 0 is reserved for "no texture", and a
 * released or evicted entry bumps `gen`, so a handle still sitting in a
 * GXTexObj resolves to NULL instead of a freed VRAM address.
 */
#define DC_TEX_IDX_BITS  12
#define DC_TEX_IDX_MASK  ((1u << DC_TEX_IDX_BITS) - 1u)

static unsigned int make_handle(int idx) {
    return ((unsigned int)s_tex[idx].gen << DC_TEX_IDX_BITS) |
           ((unsigned int)idx + 1u);
}

static dc_tex_entry_t* resolve(unsigned int handle) {
    unsigned int idx;
    dc_tex_entry_t* e;
    if (handle == 0) return NULL;
    idx = (handle & DC_TEX_IDX_MASK);
    if (idx == 0 || idx > DC_PVR_TEX_MAX_ENTRIES) return NULL;
    e = &s_tex[idx - 1];
    if (!e->live) return NULL;
    if ((unsigned int)e->gen != (handle >> DC_TEX_IDX_BITS)) return NULL;
    return e;
}

/* ==========================================================================
 * Content index
 * ========================================================================== */
static unsigned int key_hash(const dc_tex_entry_t* e) {
    unsigned int key[7];
    unsigned int hv = 0x811c9dc5u;
    int i, b;
    key[0] = e->data_ptr;
    key[1] = e->data_hash;
    key[2] = e->tlut_ptr;
    key[3] = e->tlut_hash;
    key[4] = ((unsigned int)e->gx_w << 16) | e->gx_h;
    key[5] = ((unsigned int)e->fmt << 16) | e->ci_fmt;
    key[6] = ((unsigned int)e->tlut_fmt << 16) | e->tlut_count;
    for (i = 0; i < 7; i++) {
        unsigned int v = key[i];
        for (b = 0; b < 4; b++) {
            hv ^= (v >> (b * 8)) & 0xFF;
            hv *= 0x01000193u;
        }
    }
    return hv;
}

static int key_equal(const dc_tex_entry_t* a, const dc_tex_entry_t* b) {
    return a->data_ptr == b->data_ptr && a->data_hash == b->data_hash &&
           a->tlut_ptr == b->tlut_ptr && a->tlut_hash == b->tlut_hash &&
           a->gx_w == b->gx_w && a->gx_h == b->gx_h &&
           a->fmt == b->fmt && a->ci_fmt == b->ci_fmt &&
           a->tlut_fmt == b->tlut_fmt && a->tlut_count == b->tlut_count;
}

static void index_insert(int idx) {
    unsigned int slot = key_hash(&s_tex[idx]) & (DC_PVR_TEX_INDEX_SIZE - 1u);
    while (s_index[slot] != 0)
        slot = (slot + 1u) & (DC_PVR_TEX_INDEX_SIZE - 1u);
    s_index[slot] = (unsigned short)(idx + 1);
}

/* Single-entry removal needs tombstones or a rebuild; eviction is rare enough
 * (VRAM-pressure only) that a rebuild is the cheaper correctness guarantee. */
static void index_rebuild(void) {
    int i;
    memset(s_index, 0, sizeof(s_index));
    for (i = 0; i < DC_PVR_TEX_MAX_ENTRIES; i++)
        if (s_tex[i].live) index_insert(i);
}

static dc_tex_entry_t* index_find(const dc_tex_entry_t* probe) {
    unsigned int slot = key_hash(probe) & (DC_PVR_TEX_INDEX_SIZE - 1u);
    int i;
    for (i = 0; i < DC_PVR_TEX_INDEX_SIZE; i++) {
        unsigned short v = s_index[slot];
        if (v == 0) return NULL;
        if (s_tex[v - 1].live && key_equal(&s_tex[v - 1], probe))
            return &s_tex[v - 1];
        slot = (slot + 1u) & (DC_PVR_TEX_INDEX_SIZE - 1u);
    }
    return NULL;
}

/* ==========================================================================
 * VRAM lifetime
 * ========================================================================== */
static void entry_free(dc_tex_entry_t* e) {
    if (!e->live) return;
    if (e->rec.base) pvr_mem_free(e->rec.base);
    s_bytes_resident -= e->bytes;
    e->live = 0;
    e->refs = 0;
    e->bytes = 0;
    e->rec.base = NULL;
    e->gen++;                    /* every outstanding handle is now stale */
    if (e->gen == 0) e->gen = 1; /* never wrap to a gen a fresh entry uses */
}

/* Drop the least-recently-used live entry. Returns 0 if there was none. */
static int evict_lru(void) {
    int i, victim = -1;
    unsigned int best = 0xFFFFFFFFu;
    for (i = 0; i < DC_PVR_TEX_MAX_ENTRIES; i++) {
        if (!s_tex[i].live) continue;
        if (s_tex[i].lru < best) { best = s_tex[i].lru; victim = i; }
    }
    if (victim < 0) return 0;
    entry_free(&s_tex[victim]);
    s_stat_evictions++;
    index_rebuild();
    return 1;
}

static int alloc_entry_slot(void) {
    int i;
    for (i = 0; i < DC_PVR_TEX_MAX_ENTRIES; i++)
        if (!s_tex[i].live) return i;
    if (!evict_lru()) return -1;
    for (i = 0; i < DC_PVR_TEX_MAX_ENTRIES; i++)
        if (!s_tex[i].live) return i;
    return -1;
}

static int next_pot(int v) {
    int p = DC_PVR_TEX_MIN_DIM;
    while (p < v && p < DC_PVR_TEX_MAX_DIM) p <<= 1;
    return p;
}

/* ==========================================================================
 * Public: lifecycle
 * ========================================================================== */
void dc_pvr_texture_init(void) {
    int i;
    memset(s_tex, 0, sizeof(s_tex));
    memset(s_index, 0, sizeof(s_index));
    for (i = 0; i < DC_PVR_TEX_MAX_ENTRIES; i++) s_tex[i].gen = 1;
    s_clock = 1;
    s_bytes_resident = 0;
    s_stat_uploads = s_stat_hits = s_stat_evictions = s_stat_rejects = 0;
    DC_LOG("[DC/TEX] init: ceiling %u B VRAM, scratch %u B, %d entries\n",
           (unsigned int)DC_PVR_TEX_VRAM_BYTES,
           (unsigned int)(DC_PVR_TEX_SCRATCH_TEXELS * 2),
           (int)DC_PVR_TEX_MAX_ENTRIES);
}

void dc_pvr_texture_shutdown(void) {
    int i;
    for (i = 0; i < DC_PVR_TEX_MAX_ENTRIES; i++) entry_free(&s_tex[i]);
    memset(s_index, 0, sizeof(s_index));
    s_bytes_resident = 0;
}

void dc_pvr_texture_report(void) {
    DC_LOGE("[DC/TEX] uploads=%u hits=%u resident=%u B evictions=%u rejects=%u\n",
            s_stat_uploads, s_stat_hits, s_bytes_resident,
            s_stat_evictions, s_stat_rejects);
}

/* ==========================================================================
 * Public: lookup
 * ========================================================================== */
const dc_pvr_tex_t* dc_pvr_tex_get(unsigned int handle) {
    dc_tex_entry_t* e = resolve(handle);
    if (!e) return NULL;
    e->lru = ++s_clock;
    return &e->rec;
}

/* ==========================================================================
 * Public: upload / release
 * ========================================================================== */
unsigned int dc_gx_backend_texture_upload(const void* data, int w, int h, int fmt,
                                          int ci_fmt, const void* tlut,
                                          int tlut_fmt, int tlut_count) {
    dc_tex_entry_t probe;
    dc_tex_entry_t* hit;
    dc_tex_entry_t* e;
    u8 palette[256][4];
    int is_be, pot_w, pot_h, texels, slot, i;
    unsigned int need;
    pvr_ptr_t base;

    if (!data || w <= 0 || h <= 0) return 0;
    if (w > DC_PVR_TEX_MAX_DIM || h > DC_PVR_TEX_MAX_DIM) {
        s_stat_rejects++;
        DC_LOGE("[DC/TEX] REJECT %dx%d fmt=%d: side exceeds %d\n",
                w, h, fmt, (int)DC_PVR_TEX_MAX_DIM);
        return 0;
    }

    /* --- content key ---------------------------------------------------- */
    is_be = tlut_is_be(tlut);
    memset(&probe, 0, sizeof(probe));
    probe.data_ptr   = (unsigned int)(uintptr_t)data;
    probe.data_hash  = tex_content_hash(data, w, h, fmt);
    probe.tlut_ptr   = (unsigned int)(uintptr_t)tlut;
    probe.tlut_hash  = tlut_content_hash(tlut, tlut_fmt, tlut_count, is_be);
    probe.gx_w       = (unsigned short)w;
    probe.gx_h       = (unsigned short)h;
    probe.fmt        = (unsigned short)fmt;
    probe.ci_fmt     = (unsigned short)ci_fmt;
    probe.tlut_fmt   = (unsigned short)tlut_fmt;
    probe.tlut_count = (unsigned short)(tlut_count < 0 ? 0 : tlut_count);

    hit = index_find(&probe);
    if (hit) {
        s_stat_hits++;
        hit->lru = ++s_clock;
        if (hit->refs < 0xFFFF) hit->refs++;
        return make_handle((int)(hit - s_tex));
    }

    /* --- size check against the one static scratch ---------------------- */
    pot_w = next_pot(w);
    pot_h = next_pot(h);
    texels = pot_w * pot_h;
    if (texels > DC_PVR_TEX_SCRATCH_TEXELS) {
        s_stat_rejects++;
        DC_LOGE("[DC/TEX] REJECT %dx%d fmt=%d: %d padded texels > scratch %d "
                "(raise -DDC_PVR_TEX_SCRATCH_TEXELS)\n",
                w, h, fmt, texels, (int)DC_PVR_TEX_SCRATCH_TEXELS);
        return 0;
    }

    /* VRAM is only allocatable once pvr_init() has run (dc_pvr.h). */
    if (!dc_pvr_ready) {
        s_stat_rejects++;
        return 0;
    }

    /* --- palette + alpha survey ----------------------------------------- */
    if (fmt == GX_TF_C4 || fmt == GX_TF_C8)
        build_palette(tlut, tlut_fmt, tlut_count, palette, is_be);
    else
        build_palette(NULL, 0, 0, palette, 0);

    s_class = survey_alpha(data, w, h, fmt, (const u8 (*)[4])palette,
                           (fmt == GX_TF_C4 || fmt == GX_TF_C8) ? tlut_count : 0);

    /* --- VRAM, with LRU eviction against both ceilings ------------------- */
    need = (unsigned int)texels * 2u;
    while (s_bytes_resident + need > (unsigned int)DC_PVR_TEX_VRAM_BYTES) {
        if (!evict_lru()) break;
    }
    base = pvr_mem_malloc(need);
    while (!base) {
        if (!evict_lru()) break;
        base = pvr_mem_malloc(need);
    }
    if (!base) {
        s_stat_rejects++;
        DC_LOGE("[DC/TEX] REJECT %dx%d fmt=%d: no VRAM for %u B "
                "(resident %u B)\n", w, h, fmt, need, s_bytes_resident);
        return 0;   /* legal "no texture": dc_pvr.c draws untextured */
    }

    slot = alloc_entry_slot();
    if (slot < 0) {
        pvr_mem_free(base);
        s_stat_rejects++;
        DC_LOGE("[DC/TEX] REJECT %dx%d: entry table full\n", w, h);
        return 0;
    }

    /* --- decode into the scratch, then twiddle straight into VRAM -------- */
    s_out = s_scratch;
    s_pitch = pot_w;
    /* Clears the NPOT pad AND gives unhandled formats a transparent image. */
    for (i = 0; i < texels; i++) s_scratch[i] = 0;
    decode_gc_texture(data, w, h, fmt, (const u8 (*)[4])palette);

    /* pvr_txr_load_ex twiddles while it copies; it requires power-of-two w/h,
     * which is exactly why the source was padded above. */
    pvr_txr_load_ex(s_scratch, base, (unsigned int)pot_w, (unsigned int)pot_h,
                    PVR_TXRLOAD_16BPP);

    /* --- publish -------------------------------------------------------- */
    e = &s_tex[slot];
    {
        unsigned short keep_gen = e->gen;
        memcpy(e, &probe, sizeof(dc_tex_entry_t));
        e->gen = keep_gen;
    }
    e->rec.base    = (void*)base;
    e->rec.pvr_fmt = (s_class == DC_TEX_OPAQUE ? (unsigned int)PVR_TXRFMT_RGB565 :
                      s_class == DC_TEX_BINARY ? (unsigned int)PVR_TXRFMT_ARGB1555
                                               : (unsigned int)PVR_TXRFMT_ARGB4444)
                   | (unsigned int)PVR_TXRFMT_TWIDDLED
                   | (unsigned int)PVR_TXRFMT_VQ_DISABLE
                   | (unsigned int)PVR_TXRFMT_POW2_STRIDE;
    e->rec.w         = (unsigned short)pot_w;
    e->rec.h         = (unsigned short)pot_h;
    e->rec.u_scale   = (float)w / (float)pot_w;
    e->rec.v_scale   = (float)h / (float)pot_h;
    e->rec.has_alpha = (unsigned char)(s_class != DC_TEX_OPAQUE);
    e->rec.twiddled  = 1;
    e->bytes = need;
    e->lru   = ++s_clock;
    e->refs  = 1;
    e->live  = 1;

    s_bytes_resident += need;
    s_stat_uploads++;
    index_insert(slot);

    return make_handle(slot);
}

void dc_gx_backend_texture_release(unsigned int handle) {
    dc_tex_entry_t* e = resolve(handle);
    if (!e) return;
    /* The content cache means several GXTexObjs can share one upload; only the
     * last GXDestroyTexObj may actually free the VRAM. */
    if (e->refs > 1) { e->refs--; return; }
    entry_free(e);
    index_rebuild();
}

#else /* DC_PVR_NO_TEXTURES ------------------------------------------------- */

/* Kill switch: every upload fails, so dc_pvr.c draws everything untextured.
 * Nothing here allocates VRAM or .bss. */

unsigned int dc_gx_backend_texture_upload(const void* data, int w, int h, int fmt,
                                          int ci_fmt, const void* tlut,
                                          int tlut_fmt, int tlut_count) {
    (void)data; (void)w; (void)h; (void)fmt;
    (void)ci_fmt; (void)tlut; (void)tlut_fmt; (void)tlut_count;
    return 0;
}

void dc_gx_backend_texture_release(unsigned int handle) { (void)handle; }

const dc_pvr_tex_t* dc_pvr_tex_get(unsigned int handle) {
    (void)handle;
    return NULL;
}

void dc_pvr_texture_init(void) {
    DC_LOGE("[DC/TEX] DC_PVR_NO_TEXTURES: textures disabled at compile time\n");
}
void dc_pvr_texture_shutdown(void) { }
void dc_pvr_texture_report(void) {
    DC_LOGE("[DC/TEX] disabled (DC_PVR_NO_TEXTURES)\n");
}

#endif /* DC_PVR_NO_TEXTURES */
