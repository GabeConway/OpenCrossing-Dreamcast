/* dc_texpool.c — T1's falsification probe. It COUNTS. It changes nothing.
 * (Kill switch DC_TEXPOOL_PROBE=0, which is the default and is byte-identical.)
 *
 * WHAT QUESTION THIS ANSWERS
 * --------------------------
 * T1 wants to stop keeping texture destination arrays in .bss and read them off
 * /cd instead. What makes that cheap — much cheaper than R2/R3's 16-slot pools —
 * is that the PVR ALREADY holds every texture twiddled in VRAM behind a
 * content-keyed LRU (dc_pvr_texture.c; observed uploads=306 hits=894442
 * evictions=0). The main-RAM array is not read to draw with. It is read on EVERY
 * bind for one reason only: to compute the cache key.
 *
 *     dc_pvr_texture.c:1082-1083   probe.data_ptr  = data;
 *                                  probe.data_hash = tex_content_hash(data, …);
 *
 * and that hash reads up to 512 B of the array. GXInitTexObj memsets the
 * tex-obj (dc_gx.c:2133), clearing TEXOBJ_BACKEND_TEX (index 15, dc_gx.c:2119),
 * so the dedup early-out at dc_gx.c:2312-2319 never fires for emu64 and all ~109
 * SETTILE_DOLPHIN binds per frame re-enter the hash.
 *
 * If that key can be synthesised from the asset's IDENTITY instead of its
 * CONTENT, the array is needed only on a cache MISS — which means T1 needs ONE
 * staging buffer, not an N-slot pool. This file measures whether identity is a
 * legal substitute for content. It is a measurement, not a step toward the
 * loader: nothing here reads the disc, allocates, or touches a pixel.
 *
 * THE THREE COUNTERS THAT CAN KILL THE DESIGN
 * -------------------------------------------
 *   interior  A bind whose `data` lands strictly INSIDE a mapped row instead of
 *             at its base. THE KILLER. Under DC_ASSET_STUB an unkept array is
 *             `u8 x[1]`, so an interior pointer into it is a pointer into
 *             WHATEVER THE LINKER PUT NEXT — T1 would serve a neighbour's
 *             texture and the failure would be silent and pretty. Non-zero ⇒ the
 *             design is dead as specified and needs a base+offset key.
 *   mutated   A mapped row whose content hash CHANGED between two binds. The
 *             synthetic key assumes an asset array is immutable after load;
 *             non-zero ⇒ it is not, and identity cannot stand in for content.
 *   oversize  The decoder's own read length for (w,h,fmt) exceeds the row's
 *             recorded size, so a staging buffer sized from the row would
 *             over-read. This is R1's mFM_grd_s_beach_tex hazard (dc_bgtex.c:37-48)
 *             in a new place: there, vanilla over-reads 1,024 B past the array on
 *             every call, and reproducing the GameCube meant reading the CALLER's
 *             size. Non-zero here means T1 must size from the decoder, not the row.
 *
 * ⚠️ `unmapped` IS EXPECTED TO BE NON-ZERO. DO NOT "FIX" IT.
 * The map is built from symbols some display list in src/ names as its texture
 * IMAGE operand (tools/dcstub/make_stub_data.py, TEXPOOL_IMG_ARG). A texture
 * reached through a POINTER cannot be seen that way — FONT_nes_tex_font1 is the
 * type case: src/game/m_font_main.c_inc:2 is `return FONT_nes_tex_font1;` and the
 * caller binds the returned pointer. Those binds land here as `unmapped`, which
 * is a measurement of how much of the texture traffic a static operand scan can
 * account for, not a hole. The finding to watch for is one of them showing up as
 * `interior` instead — that is precisely what this probe exists to catch.
 *
 * ⚠️ RESIDENT vs STUBBED, and why the report prints both.
 * Only rows the keep list KEEPS are bytes T1 would give back; the rest are
 * content the image does not have today (CLAUDE.md: "in a stubbed image, an
 * asset class's resident cost is what the KEEP LIST kept, not what the class
 * totals"). Measured 2026-08-06 against tools/dcstub/keeplist-town.txt:
 *     mapped   6,092 rows / 3,018,336 B
 *     resident 1,381 rows /   885,984 B   <- the only part that is a SAVING
 *     stubbed  4,711 rows / 2,132,352 B   <- content, not savings
 * The generator emits the flag per row rather than deriving it here, because
 * residency is a property of the keep list the tree was built with.
 *
 * KNOWN RESIDUAL, so the next person reconciling two counts does not think they
 * have found something: dc/build/stubsrc/dc_stub_keep.inc declares 2,073
 * `_tex`/`_txt`/`_tmem`-suffixed rows; this map accounts for 1,381 resident +
 * 675 unmapped-but-resident = 2,056. The ~20-symbol difference is `_tmem`
 * spelling variants in the suffix grep, not a missing population, and it changes
 * no decision.
 *
 * WHY A SORTED-ADDRESS BINARY SEARCH AND NOT A HASH INDEX
 * -------------------------------------------------------
 * index_insert() at dc_pvr_texture.c:917 is the tree's idiom and was the
 * specified design here. It cannot work for this: `interior` is a RANGE query —
 * "is `data` inside [base, base+size)?" — and a hash keyed on an exact address
 * can only answer membership, so it would report every interior pointer as
 * `unmapped` and the killer counter would read 0 for the wrong reason. One
 * binary search over addresses answers `mapped` and `interior` together. At
 * ~12.6 compares x ~109 binds/frame it is free, and it is the cheaper code.
 */
#include "dc_platform.h"

/* The row shape the generated map is written against. tools/dcstub/
 * make_stub_data.py emits the .inc; this typedef is the other end of that
 * contract, so change both or neither. `size` is 16-bit and the generator
 * hard-errors above 0xFFFF (largest row in the tree today: 4,096 B). */
typedef struct {
    const void*    src;      /* the array's address — THE LOOKUP KEY */
    const char*    name;     /* diagnostic only; never printed on the boot path */
    unsigned int   rom_off;  /* s_assets[] row; rom_src and swap are 0 by
                              * construction, asserted by the generator */
    unsigned short size;
    unsigned char  kept;     /* 1 = resident under this build's keep list */
    unsigned char  pad;
} dc_texpool_row_t;

#if defined(DC_TEXPOOL_PROBE) && DC_TEXPOOL_PROBE
#include "dc/build/stubsrc/dc_texpool_map.inc"
#else
/* The probe was not asked for. No map, no tables, no counters, no .text. */
#define DC_TEXPOOL_MAP_N 0
#endif

#if DC_TEXPOOL_MAP_N > 0

/* Row indices ordered by ascending src address. Built once at first bind:
 * addresses are link-time, so the generator cannot pre-sort. */
static unsigned short s_order[DC_TEXPOOL_MAP_N];
/* First content hash seen per row — the `mutated` baseline. */
static unsigned int   s_hash[DC_TEXPOOL_MAP_N];
/* bit0: this row has been bound at least once (also the `distinct` tally). */
static unsigned char  s_flags[DC_TEXPOOL_MAP_N];
static int s_ready;

static unsigned int s_binds, s_mapped, s_interior, s_unmapped;
static unsigned int s_distinct, s_mutated, s_oversize, s_aliased;

/* Powers of two only. A bind path runs ~109 times per frame; an unconditional
 * message here would BE the console log, and 1,392 per-asset lines already cost
 * this project 15.0 s of dead boot at 57,600 baud (kb/traps.md). Call with the
 * POST-increment count. */
static int dc_texpool_say(unsigned int n) {
    return (n & (n - 1u)) == 0u;
}

static unsigned int row_addr(int i) {
    return (unsigned int)(uintptr_t)dc_texpool_map[i].src;
}

/* Shell sort: no recursion, no auxiliary array, and 6,092 elements once at
 * startup is not worth a better algorithm. Ciura's gaps, truncated. */
static void build_order(void) {
    static const int gaps[] = { 1750, 701, 301, 132, 57, 23, 10, 4, 1 };
    int n = DC_TEXPOOL_MAP_N;
    int gi, i, j;
    unsigned int prev;

    for (i = 0; i < n; i++) s_order[i] = (unsigned short)i;

    for (gi = 0; gi < (int)(sizeof(gaps) / sizeof(gaps[0])); gi++) {
        int g = gaps[gi];
        if (g >= n) continue;
        for (i = g; i < n; i++) {
            unsigned short t = s_order[i];
            unsigned int tv = row_addr(t);
            for (j = i; j >= g && row_addr(s_order[j - g]) > tv; j -= g)
                s_order[j] = s_order[j - g];
            s_order[j] = t;
        }
    }

    /* Two rows at the SAME address. --allow-multiple-definition is on (1,367
     * multiply-defined data symbols, kb/levers.md L6) and --gc-sections can fold
     * storage, so this is possible in principle. It matters as much as `mutated`
     * does: if two distinct assets share one address, an identity key cannot
     * tell them apart either, and the binary search below would attribute binds
     * to whichever row sorted first. Counted, not assumed away. */
    prev = 0;
    for (i = 0; i < n; i++) {
        unsigned int a = row_addr(s_order[i]);
        if (i > 0 && a == prev) s_aliased++;
        prev = a;
    }
    s_ready = 1;
}

/* Greatest row whose base is <= p, then a containment test. Returns the row
 * index and sets *interior, or -1 when p is in no row. */
static int lookup(const void* p, int* interior) {
    unsigned int a = (unsigned int)(uintptr_t)p;
    int lo = 0, hi = DC_TEXPOOL_MAP_N - 1, best = -1;
    int r;
    unsigned int base;

    while (lo <= hi) {
        int mid = lo + ((hi - lo) >> 1);
        if (row_addr(s_order[mid]) <= a) { best = mid; lo = mid + 1; }
        else hi = mid - 1;
    }
    if (best < 0) return -1;

    r = (int)s_order[best];
    base = row_addr(r);
    if (base == a) { *interior = 0; return r; }
    if (a - base < (unsigned int)dc_texpool_map[r].size) { *interior = 1; return r; }
    return -1;
}

/* Called from dc_gx_backend_texture_upload() with values it has ALREADY
 * computed — no work is repeated and nothing is recomputed for the probe.
 * `decoded_bytes` is gc_data_size() (dc_pvr_texture.c:313), which rounds up to
 * whole 8x8 / 8x4 / 4x4 tiles per format and is therefore what the decoder
 * really reads; GXGetTexBufferSize()'s un-rounded w*h*bpp/8 would understate it
 * and hide exactly the over-read `oversize` exists to find. */
void dc_texpool_note_bind(const void* data, unsigned int decoded_bytes,
                          unsigned int content_hash) {
    int interior = 0;
    int r;

    if (!s_ready) build_order();
    s_binds++;

    r = lookup(data, &interior);
    if (r < 0) {
        /* Expected non-zero — see the header. A texture bound through a pointer
         * has no static operand naming it. */
        s_unmapped++;
        return;
    }

    if (interior) {
        s_interior++;
        if (dc_texpool_say(s_interior)) {
            DC_LOGE("[DC/TEXPOOL] INTERIOR bind %08x is +%u into %s (%u B) — a "
                    "synthetic key cannot name this bind\n",
                    (unsigned int)(uintptr_t)data,
                    (unsigned int)((uintptr_t)data - (uintptr_t)dc_texpool_map[r].src),
                    dc_texpool_map[r].name, (unsigned int)dc_texpool_map[r].size);
        }
        return;
    }

    s_mapped++;

    if (!(s_flags[r] & 1u)) {
        s_flags[r] |= 1u;
        s_hash[r] = content_hash;
        s_distinct++;
    } else if (s_hash[r] != content_hash) {
        s_mutated++;
        if (dc_texpool_say(s_mutated)) {
            DC_LOGE("[DC/TEXPOOL] MUTATED %s: hash %08x -> %08x — the array is "
                    "not immutable, so identity cannot replace content\n",
                    dc_texpool_map[r].name, s_hash[r], content_hash);
        }
        s_hash[r] = content_hash;
    }

    if (decoded_bytes > (unsigned int)dc_texpool_map[r].size) {
        s_oversize++;
        if (dc_texpool_say(s_oversize)) {
            DC_LOGE("[DC/TEXPOOL] OVERSIZE %s: decoder reads %u B, s_assets[] "
                    "row is %u B — stage from the decoder's size, not the row's\n",
                    dc_texpool_map[r].name, decoded_bytes,
                    (unsigned int)dc_texpool_map[r].size);
        }
    }
}

void dc_texpool_report(void) {
    DC_LOGE("[DC/TEXPOOL] binds=%u mapped=%u interior=%u unmapped=%u "
            "distinct=%u\n",
            s_binds, s_mapped, s_interior, s_unmapped, s_distinct);
    /* The verdict line. interior/mutated/oversize must ALL be 0 for T1 to be
     * built as designed; aliased must be 0 for the same reason mutated must. */
    DC_LOGE("[DC/TEXPOOL] VERDICT interior=%u mutated=%u oversize=%u aliased=%u"
            " (all four must be 0)\n",
            s_interior, s_mutated, s_oversize, s_aliased);
    DC_LOGE("[DC/TEXPOOL] map=%d rows resident=%d/%d B stubbed=%d/%d B\n",
            (int)DC_TEXPOOL_MAP_N,
            (int)DC_TEXPOOL_RESIDENT_N, (int)DC_TEXPOOL_RESIDENT_B,
            (int)DC_TEXPOOL_STUBBED_N, (int)DC_TEXPOOL_STUBBED_B);
}

#else  /* DC_TEXPOOL_MAP_N == 0 */

/* The probe is off. Nothing is defined, so nothing can be called: the two call
 * sites in dc_pvr_texture.c are behind the same #if. */

#endif /* DC_TEXPOOL_MAP_N */
