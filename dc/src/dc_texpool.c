/* dc_texpool.c — T1.  Textures never reach the SH-4's .bss.
 *
 * TWO THINGS LIVE IN THIS FILE, and they are separately switched:
 *
 *   DC_TEXPOOL_DEMAND=1  (the LOADER, default ON)  the 6,092 texture arrays the
 *                        display lists name by address are stubbed to [1] and
 *                        read off /cd/foresta.rel into ONE staging buffer at the
 *                        moment the PVR needs to decode them.
 *   DC_TEXPOOL_PROBE=1   (the falsification PROBE, default OFF)  counts, per
 *                        bind, whether the loader's premise still holds.  It
 *                        moves no bytes and changes no pixel.
 *
 * Either switch pulls the generated map in; the probe additionally pulls in the
 * row NAMES and ~42 KB of per-row .bss, which is why it is a diagnostic build
 * rather than the shipping one.
 *
 * WHY THIS IS CHEAP — THE THREE FINDINGS IT RESTS ON (kb/levers.md L10)
 * ---------------------------------------------------------------------
 * 1. The seam is already ours: GXLoadTexObj (dc_gx.c) ->
 *    dc_gx_backend_texture_upload (dc_pvr_texture.c).  No src/ rewrite, no
 *    make_src_shrink.py rule, no --wrap.
 * 2. NO N-SLOT POOL IS NEEDED.  The PVR already holds every texture twiddled in
 *    VRAM behind an LRU (uploads=306 hits=894442 evictions=0).  The main-RAM
 *    array was read on EVERY bind for one reason only — to compute the cache
 *    key.  Replace that key with the row INDEX and the array is needed only on
 *    a cache MISS, which is ~306 times a run rather than ~109 times a frame.
 *    One staging buffer, not sixteen slots.
 * 3. The population is tiny and uniform: every row is rom_src=0 and swap=0,
 *    i.e. a pure pread with no byte-swap, and 99.4 % are <= 2,048 B.
 *
 * THE FOUR THINGS THAT WOULD HAVE KILLED IT, AND THE RUN THAT CLEARED THEM
 * ------------------------------------------------------------------------
 *   interior  a bind landing strictly INSIDE a mapped row rather than at its
 *             base.  THE KILLER: a stubbed array is `u8 x[1]`, so an interior
 *             pointer is a pointer into whatever the linker put next, and T1
 *             would serve a neighbour's texture — silently, and prettily.
 *   mutated   a mapped row whose content changed between binds.  The synthetic
 *             key assumes an asset array is immutable once loaded.
 *   oversize  the decoder's own read length for (w,h,fmt) exceeding the row's
 *             recorded size, so a buffer sized from the row would over-read.
 *   aliased   two rows at one address — --allow-multiple-definition is on and
 *             --gc-sections can fold storage, so this is possible in principle
 *             and an identity key could not tell the two apart.
 *
 * ✅ CLEARED 2026-08-09, `smoke-texprobe3-20260809-142619`, deepest scene 9:
 *      [DC/TEXPOOL] VERDICT interior=0 mutated=0 oversize=0 aliased=0
 *      [DC/TEXPOOL] binds=2074009 mapped=560487 interior=0 unmapped=1513522
 *    All four zero over 2.07 M binds WITH THE TOWN EXERCISED.  ⚠️ An earlier
 *    run read interior=4318 and that was a PROBE BUG — it charged a stubbed
 *    row's neighbours to it, because the containment test used the declared
 *    size for a row the stub tree had shrunk to one byte.  Every `interior=`
 *    printed before that fix is void.  The fix is the `!kept` early-out in
 *    lookup() below and it is load-bearing for the measurement, not for the
 *    loader.
 *
 * ⚠️ `unmapped` IS EXPECTED TO BE ~73 % AND MUST NOT BE "FIXED".
 * The map is built from symbols some display list in src/ names as its texture
 * IMAGE operand (make_stub_data.py, TEXPOOL_IMG_ARG).  A texture reached
 * through a POINTER cannot be seen that way — FONT_nes_tex_font1 is the type
 * case (src/game/m_font_main.c_inc:2 is `return FONT_nes_tex_font1;` and the
 * caller binds the returned pointer).  Those binds fall through to the content
 * hash and their arrays stay resident, which is correct: T1 can only stub what
 * it can name.
 *
 * WHY A SORTED-ADDRESS BINARY SEARCH AND NOT A HASH INDEX
 * -------------------------------------------------------
 * index_insert() at dc_pvr_texture.c is the tree's idiom and was the specified
 * design.  It cannot work here: `interior` is a RANGE query — "is `data` inside
 * [base, base+size)?" — and a hash keyed on an exact address can only answer
 * membership, so it would report every interior pointer as `unmapped` and the
 * killer counter would read 0 for the wrong reason.  One binary search answers
 * `mapped` and `interior` together, and at ~12.6 compares against a texture
 * bind it is free.
 *
 * WHERE THE BYTES GO
 * ------------------
 * The map costs ~73 KB of .rodata (6,092 rows x 12 B) plus 12 KB of .bss for
 * the sorted order, against 885,984 B of texture arrays it takes out of .bss
 * and 2,132,352 B of texture content it makes reachable that the keep list
 * could never afford.  The row NAMES are ~120 KB more and are emitted only
 * under the probe.
 */
#include "dc_platform.h"

/* The row shape the generated map is written against. tools/dcstub/
 * make_stub_data.py emits the .inc; this typedef is the other end of that
 * contract, so change both or neither. `size` is 16-bit and the generator
 * hard-errors above 0xFFFF (largest row in the tree today: 4,096 B).
 *
 * ⚠️ THE `name` FIELD EXISTS ONLY UNDER THE PROBE, in the struct AND in the
 * generated table.  6,092 names are ~120 KB of .rodata that no shipping build
 * can spend to make a diagnostic readable. */
typedef struct {
    const void*    src;      /* the array's address — THE LOOKUP KEY */
#if defined(DC_TEXPOOL_PROBE) && DC_TEXPOOL_PROBE
    const char*    name;     /* diagnostic only; never printed on the boot path */
#endif
    unsigned int   rom_off;  /* s_assets[] row; rom_src and swap are 0 by
                              * construction, asserted by the generator */
    unsigned short size;
    unsigned char  kept;     /* 1 = still resident under this build's keep list */
    unsigned char  pad;
} dc_texpool_row_t;

#if (defined(DC_TEXPOOL_DEMAND) && DC_TEXPOOL_DEMAND) || \
    (defined(DC_TEXPOOL_PROBE)  && DC_TEXPOOL_PROBE)
#include "dc/build/stubsrc/dc_texpool_map.inc"
#else
/* Neither half was asked for. No map, no tables, no counters, no .text. */
#define DC_TEXPOOL_MAP_N 0
#endif

#if DC_TEXPOOL_MAP_N > 0

/* Row indices ordered by ascending src address. Built once at first bind:
 * addresses are link-time, so the generator cannot pre-sort. */
static unsigned short s_order[DC_TEXPOOL_MAP_N];
static int s_ready;

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

#if defined(DC_TEXPOOL_PROBE) && DC_TEXPOOL_PROBE
static unsigned int s_aliased;
#define DC_TEXPOOL_NAME(r) (dc_texpool_map[(r)].name)
#else
#define DC_TEXPOOL_NAME(r) "(name: build -DDC_TEXPOOL_PROBE=1)"
#endif

/* Shell sort: no recursion, no auxiliary array, and 6,092 elements once at
 * startup is not worth a better algorithm. Ciura's gaps, truncated. */
static void build_order(void) {
    static const int gaps[] = { 1750, 701, 301, 132, 57, 23, 10, 4, 1 };
    int n = DC_TEXPOOL_MAP_N;
    int gi, i, j;

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

#if defined(DC_TEXPOOL_PROBE) && DC_TEXPOOL_PROBE
    /* Two rows at the SAME address. --allow-multiple-definition is on (1,367
     * multiply-defined data symbols, kb/levers.md L6) and --gc-sections can fold
     * storage, so this is possible in principle. It matters as much as `mutated`
     * does: if two distinct assets share one address, an identity key cannot
     * tell them apart either, and the binary search below would attribute binds
     * to whichever row sorted first. Counted, not assumed away. */
    {
        unsigned int prev = 0;
        for (i = 0; i < n; i++) {
            unsigned int a = row_addr(s_order[i]);
            if (i > 0 && a == prev) s_aliased++;
            prev = a;
        }
    }
#endif
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

    /* 🔴 A STUBBED ROW IS **ONE BYTE** LONG, NOT `size` BYTES — AND WITHOUT THIS
     * TEST THE PROBE REPORTS ITS NEIGHBOURS AS ITS OWN INTERIOR.
     *
     * MEASURED 2026-08-09, and it produced a false falsification of T1 that was
     * one commit away from being believed. A 700 s town run read
     * `interior=4318`, which is the counter this file calls "THE KILLER", and
     * every one of those binds named ONE symbol: `ef_doyon01_00`, at offsets
     * +68, +324 and +480. That symbol is NOT in keeplist-town.txt, so the stub
     * tree declares it `u8 ef_doyon01_00[1]` — one byte — while
     * `dc_texpool_map[].size` still carries its real 1,024. The linker then
     * packs ~50 other small symbols into the 1,024-byte window after it
     * (`dna_win_*_pal`, `ef_ame02_*_v`, `ef_anahikari01_*`, … all inside
     * 0x8c684xxx in the map), and every bind to one of THOSE was attributed to
     * ef_doyon01_00 as an interior pointer.
     *
     * A pointer past a stubbed row's single byte is therefore not "inside" it;
     * it belongs to some other symbol this map cannot name, which is exactly
     * what `unmapped` means. Return -1 and let it be counted there.
     *
     * ⚠️ UNDER THE LOADER (DC_TEXPOOL_DEMAND=1) EVERY ROW IS STUBBED, so this
     * early-out is taken for every non-base pointer and the range test below is
     * dead code in a shipping build. It is kept because the probe still needs
     * it, and because DC_TEXPOOL_DEMAND=0 restores rows to residency. */
    if (!dc_texpool_map[r].kept) return -1;

    if (a - base < (unsigned int)dc_texpool_map[r].size) { *interior = 1; return r; }
    return -1;
}

/* ==========================================================================
 * The loader
 * ========================================================================== */

/* Sized from the generator's own maximum row, rounded up to a cache line. The
 * generator emits DC_TEXPOOL_MAX_B alongside the table, so this cannot drift
 * behind the data: a row that does not fit is a build-time impossibility rather
 * than a runtime over-read. */
#define DC_TEXPOOL_STAGE_B (((DC_TEXPOOL_MAX_B) + 31u) & ~31u)

static unsigned char s_stage[DC_TEXPOOL_STAGE_B] __attribute__((aligned(32)));
/* Which row s_stage currently holds, so two misses on one row — a texture
 * bound with two different TLUTs is the common case — cost one disc read.
 * -1 means empty. Safe to memo because `mutated=0`: a mapped array's content
 * never changes after it is loaded. */
static int s_stage_row = -1;

static unsigned int s_fetch, s_fetch_staged, s_fetch_memo, s_fetch_resident;
static unsigned int s_fetch_fail, s_fetch_bytes, s_fetch_toobig;

/* Called from dc_gx_backend_texture_upload() with the `data` pointer the
 * renderer is about to decode. Returns the row index — a SYNTHETIC CACHE KEY
 * that names the texture without reading a byte of it — or -1 when the pointer
 * is not the base of a mapped row, in which case the caller must fall back to
 * hashing the content. */
/* A direct-mapped pointer -> row cache in front of the binary search.
 *
 * WHY IT IS HERE AND NOT AN OPTIMISATION FOR LATER. This runs on every texture
 * bind, ~109 times a frame, and the search it fronts is ~13 iterations each
 * touching TWO scattered lines — s_order[mid] in 12 KB of .bss and then
 * dc_texpool_map[i].src in 73 KB of .rodata. kb/STATE.md's reading of this
 * frame is that it is MEMORY-BOUND (the memory-shaped stages are 70 % of the
 * split and all the floating-point is 2.6 %), so ~26 random loads per bind is
 * exactly the kind of cost that does not show up in an instruction count and
 * does show up on silicon.
 *
 * The town binds ~127 DISTINCT textures per frame, so a 256-entry table holds
 * the whole working set and every bind after the first is one masked load and
 * one compare. Entries are never invalidated because a row's address is a
 * link-time constant; a collision simply overwrites and re-searches.
 *
 * Miss is encoded as ptr == 0, which is never a valid array address. A pointer
 * the map does NOT contain is cached too, as row -1 — the unmapped binds are
 * 73 % of the traffic (textures reached through a pointer, kb/levers.md L10)
 * and paying the full search for each of them every frame would make this file
 * a regression on its own. */
#define DC_TEXPOOL_MEMO_N 256
static unsigned int s_memo_ptr[DC_TEXPOOL_MEMO_N];
static short        s_memo_row[DC_TEXPOOL_MEMO_N];

int dc_texpool_lookup(const void* data) {
    unsigned int a = (unsigned int)(uintptr_t)data;
    unsigned int h;
    int interior = 0;
    int r;

    if (!s_ready) build_order();

    /* >>5 first: every asset array is at least 4-byte aligned and most are 32,
     * so the low bits carry no entropy and hashing on them would pile the whole
     * working set into a few slots. */
    h = (a >> 5) & (DC_TEXPOOL_MEMO_N - 1u);
    if (s_memo_ptr[h] == a) return (int)s_memo_row[h];

    r = lookup(data, &interior);
    if (r < 0 || interior) r = -1;
    s_memo_ptr[h] = a;
    s_memo_row[h] = (short)r;
    return r;
}

/* Called ONLY on a texture-cache miss. Returns `need` readable bytes of row
 * `row`, or NULL.
 *
 * ⚠️ NULL MEANS ABANDON THE UPLOAD. It must never be turned into "use the
 * original pointer": under DC_ASSET_STUB a non-resident array is one byte long,
 * so decoding from it reads whatever the linker put next and draws a plausible
 * wrong texture. dc_pvr_texture.c returns handle 0 instead, which dc_pvr.c
 * already renders as untextured — a visible, attributable failure. */
const void* dc_texpool_fetch(int row, unsigned int need) {
    const dc_texpool_row_t* r;
    unsigned int have;

    if (row < 0 || row >= DC_TEXPOOL_MAP_N) return NULL;
    r = &dc_texpool_map[row];
    s_fetch++;

    /* S15-7. Name the first N rows the loader actually serves. Without this the
     * only way to find out WHICH texture a garbled frame came from was to guess
     * at the map, and `fetch=133` says nothing about which 133. Needs
     * -DDC_TEXPOOL_PROBE=1 for the name table (DC_TEXPOOL_NAME is a placeholder
     * otherwise), so it costs nothing in a shipping build.
     *
     * ⚠️ THE INTERESTING RESULT IS AN ABSENCE. A texture that renders as noise
     * and does NOT appear here was never fetched — its bind missed the map,
     * `src` stayed pointing at the one-byte stub, and the decoder read whatever
     * the linker put next. That is a lookup failure, not a loader failure, and
     * the two have completely different fixes. */
#if defined(DC_TEXPOOL_TRACE) && (DC_TEXPOOL_TRACE) > 0
    if (s_fetch <= (unsigned int)(DC_TEXPOOL_TRACE)) {
        DC_LOGE("[DC/TEXPOOL] fetch#%u %s row=%d off=%u have=%u need=%u kept=%d\n",
                s_fetch, DC_TEXPOOL_NAME(row), row, r->rom_off,
                (unsigned int)r->size, need, (int)r->kept);
    }
#endif

    /* Still resident — either DC_TEXPOOL_DEMAND=0, or this row is in a TU the
     * keep list holds whole for some other symbol's sake. Nothing to do. */
    if (r->kept) { s_fetch_resident++; return r->src; }

    if (need > (unsigned int)DC_TEXPOOL_STAGE_B) {
        /* Cannot happen with the generator's 0xFFFF row cap and the staging
         * buffer sized from DC_TEXPOOL_MAX_B — unless the decoder's own read
         * length for (w,h,fmt) exceeds the row, which is precisely the probe's
         * `oversize` counter and it measured 0. Loud, and refused. */
        s_fetch_toobig++;
        if (dc_texpool_say(s_fetch_toobig)) {
            DC_LOGE("[DC/TEXPOOL] %s: decoder wants %u B, staging buffer is "
                    "%u B — upload refused\n",
                    DC_TEXPOOL_NAME(row), need, (unsigned int)DC_TEXPOOL_STAGE_B);
        }
        return NULL;
    }

    have = (unsigned int)r->size;
    if (have > (unsigned int)DC_TEXPOOL_STAGE_B) have = (unsigned int)DC_TEXPOOL_STAGE_B;

    if (s_stage_row == row) {
        /* ⚠️ THE MEMO MUST STILL RE-CLEAR THE TAIL. The same row can be bound
         * twice with different (w,h,fmt) — a different `need` — and those are
         * two separate cache entries, so both reach here. Returning early on
         * the second one used to hand the decoder the PREVIOUS texture's bytes
         * beyond `have`, which is a wrong image rather than a missing one. */
        if (need > have) memset(s_stage + have, 0, need - have);
        s_fetch_memo++;
        return s_stage;
    }

    /* If the decoder reads further than the row is long, the tail is zeros
     * rather than stale bytes from the previous texture. A static sweep of all
     * 7,680 resolved operand sites found no case where it does; leaving it
     * undefined is how it stops being reproducible when one appears. */
    if (need > have) memset(s_stage + have, 0, need - have);

    /* rom_src is 0 for every row by construction — make_stub_data.py hard-errors
     * on anything else, because a second source would mean another case here. */
    if (!dc_assetwin_read(0, r->rom_off, s_stage, have)) {
        s_fetch_fail++;
        s_stage_row = -1;
        if (dc_texpool_say(s_fetch_fail)) {
            DC_LOGE("[DC/TEXPOOL] %s: disc read of %u B at %u FAILED — this "
                    "texture will draw untextured\n",
                    DC_TEXPOOL_NAME(row), have, r->rom_off);
        }
        return NULL;
    }

    s_stage_row = row;
    s_fetch_staged++;
    s_fetch_bytes += have;
    return s_stage;
}

/* ==========================================================================
 * The probe
 * ========================================================================== */
#if defined(DC_TEXPOOL_PROBE) && DC_TEXPOOL_PROBE

/* First content hash seen per row — the `mutated` baseline. */
static unsigned int   s_hash[DC_TEXPOOL_MAP_N];
/* bit0: this row has been bound at least once (also the `distinct` tally). */
static unsigned char  s_flags[DC_TEXPOOL_MAP_N];

static unsigned int s_binds, s_mapped, s_interior, s_unmapped;
static unsigned int s_distinct, s_mutated, s_oversize;

/* Called from dc_gx_backend_texture_upload() with values it has ALREADY
 * computed — no work is repeated and nothing is recomputed for the probe.
 * `decoded_bytes` is gc_data_size(), which rounds up to whole 8x8 / 8x4 / 4x4
 * tiles per format and is therefore what the decoder really reads;
 * GXGetTexBufferSize()'s un-rounded w*h*bpp/8 would understate it and hide
 * exactly the over-read `oversize` exists to find.
 *
 * ⚠️ UNDER THE LOADER, `content_hash` IS 0 — dc_pvr_texture.c stops hashing
 * mapped rows, which is the entire saving. `mutated` is therefore only
 * meaningful in a DC_TEXPOOL_DEMAND=0 probe build, and the report says so. */
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
#endif /* DC_TEXPOOL_PROBE */

void dc_texpool_report(void) {
#if !(defined(DC_TEXPOOL_PROBE) && DC_TEXPOOL_PROBE)
    /* ⚠️ RATE-LIMITED, AND NOT AS A COURTESY. dc_pvr_texture_report() runs
     * every 30 presented frames and KOS busy-waits on the SCIF TX FIFO whether
     * or not a cable is attached, so three unconditional lines here would be
     * ~0.5 KB of console every half second at 57,600 baud — the same mechanism
     * that cost this project 8x the frame rate once already (kb/traps.md).
     * The loader's counters go static once the scene's ~306 textures have been
     * staged, so "print only when something changed" is silent for almost the
     * whole run and still catches every new fetch and every failure.
     * The PROBE build prints unconditionally: it is a diagnostic, its numbers
     * are the point, and it is never the build anyone measures FPS on. */
    static unsigned int s_last_fetch = 0xFFFFFFFFu, s_last_fail;
    if (s_fetch == s_last_fetch && s_fetch_fail == s_last_fail) return;
    s_last_fetch = s_fetch;
    s_last_fail  = s_fetch_fail;
#endif
    DC_LOGE("[DC/TEXPOOL] fetch=%u staged=%u memo=%u resident=%u fail=%u "
            "toobig=%u bytes=%u stage=%u B\n",
            s_fetch, s_fetch_staged, s_fetch_memo, s_fetch_resident,
            s_fetch_fail, s_fetch_toobig, s_fetch_bytes,
            (unsigned int)DC_TEXPOOL_STAGE_B);
    DC_LOGE("[DC/TEXPOOL] map=%d rows resident=%d/%d B demand=%d/%d B\n",
            (int)DC_TEXPOOL_MAP_N,
            (int)DC_TEXPOOL_RESIDENT_N, (int)DC_TEXPOOL_RESIDENT_B,
            (int)DC_TEXPOOL_STUBBED_N, (int)DC_TEXPOOL_STUBBED_B);
#if defined(DC_TEXPOOL_PROBE) && DC_TEXPOOL_PROBE
    DC_LOGE("[DC/TEXPOOL] binds=%u mapped=%u interior=%u unmapped=%u "
            "distinct=%u\n",
            s_binds, s_mapped, s_interior, s_unmapped, s_distinct);
    /* The verdict line. interior/mutated/oversize must ALL be 0 for T1 to be
     * built as designed; aliased must be 0 for the same reason mutated must.
     * ⚠️ `mutated` reads 0 for free in a DC_TEXPOOL_DEMAND=1 build because the
     * content hash is no longer computed — run the probe against
     * DC_TEXPOOL_DEMAND=0 if that counter is the question. */
    DC_LOGE("[DC/TEXPOOL] VERDICT interior=%u mutated=%u oversize=%u aliased=%u"
            " (all four must be 0)\n",
            s_interior, s_mutated, s_oversize, s_aliased);
#endif
    dc_assetwin_report();
}

#else  /* DC_TEXPOOL_MAP_N == 0 */

/* Neither half was asked for. The call sites in dc_pvr_texture.c are behind the
 * same #if, so nothing here can be called — but dc_platform.h declares the two
 * loader entry points unconditionally, and a build that turns the loader off
 * must still link. These are the byte-identical "not a mapped texture" answers.
 */
int dc_texpool_lookup(const void* data) { (void)data; return -1; }
const void* dc_texpool_fetch(int row, unsigned int need) {
    (void)row; (void)need; return NULL;
}
void dc_texpool_report(void) {}

#endif /* DC_TEXPOOL_MAP_N */
