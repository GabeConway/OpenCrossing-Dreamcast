/* dc_asset_census.c — which asset bytes does a scene ACTUALLY touch?
 *
 * WHY THIS EXISTS
 * ---------------
 * kb/STATE.md N1 asks for the title demo's real working set, and the static
 * answer is not obtainable: the title demo names its acres through
 * BLOCK_COMBI_GRD_* indices into l_combiID[] and its 15 NPCs through profile
 * IDs, so tracing the src/data C files by grep stops at the logo overlay (8,824 B in
 * ten TUs) and cannot see one acre or one animal. That was measured, not
 * assumed — the search is written up in kb/STATE.md.
 *
 * The runtime knows the answer even though the source does not. Every asset
 * the renderer consumes arrives at the GX layer as an address:
 *
 *   GXLoadTexObj  -> the texture image pointer being bound          kind 'T'
 *   GXLoadTlut    -> the palette being bound, and its entry count   kind 'P'
 *   GXSetArray    -> the base of an indexed vertex array            kind 'V'
 *
 * Those addresses are link-time constants inside .bss (or .data), so recording
 * the distinct ones and resolving them against the ELF symbol table on the
 * host names the exact destination arrays a scene reached for, and — via each
 * symbol's st_size — how many bytes they are. That is the working-set number
 * S4 has to size its pool against, and it is the measurement
 * kb/levers.md L10 T1 ("textures are never pooled") needs before
 * the pool's shape can be decided.
 *
 * IT WORKS IN A STUB BUILD, WHICH IS THE POINT. Under DC_ASSET_STUB the
 * destinations are [1]-sized and full of garbage, so the frame is wrong — but
 * for a pointer that names a symbol's BASE the identity is unaffected, because
 * the symbol it dereferences is the same symbol either way. So the working set
 * can be measured now, on the image that boots, rather than after S4 lands.
 *
 * THE VERTEX HALF — WHY IT IS BATCHES, NOT POINTS AND NOT RANGES
 * --------------------------------------------------------------
 * MEASURED 2026-08-02: GXSetArray records ZERO hits, in this scene and in any
 * other, because this game never uses GX indexed vertex fetch. The only caller
 * anywhere in src/ is GXInit.c:252's own reset loop; every triangle the game
 * draws comes from an N64 display list that emu64 walks itself, dereferencing
 * the Vtx* inside src/ where the GX layer never sees it. Kind 'V' is dead and
 * is kept only so an old log still parses.
 *
 * The seam that does exist is OSs16tof32(). emu64::dl_G_VTX() reads each source
 * vertex's x/y/z through it (emu64.c ~4674) and those are the only three calls
 * to it in the whole TU, so a shim on that one inline — force-included into
 * emu64.c alone by dc/include/dc_census_vtx.h — sees exactly the vertex stream
 * and nothing else. See that header for why it is shaped the way it is.
 *
 * That seam fires once per vertex COMPONENT, so the point table above is the
 * wrong container: one 200-Vtx array would burn 200 slots and 200 console
 * lines. The first design coalesced them into unioned [lo,hi) ranges, and that
 * was WRONG IN A STUB BUILD in a way worth recording, because the same trap is
 * waiting for anything else that measures spans on a stub image:
 *
 *   gsSPVertex takes an INTERIOR pointer — `gsSPVertex(&obj_train1_1_v[93], 15,
 *   0)` — and the display lists are real in a stub image (they are initialised
 *   Gfx[] data, which the stub rewriter does not touch), so the *offset* and
 *   the *vertex count* are the real ones while the *array* is one element long.
 *   Every read therefore runs far past its own symbol and over its neighbours.
 *   Unioning those spans merged unrelated arrays wholesale: 665,136 component
 *   reads collapsed to TEN ranges, an undercount of unknown size, and the
 *   identity of everything but the first array in each run was lost.
 *
 * A BATCH is immune to that. dl_G_VTX walks its vertices with vtx_p++, so one
 * gsSPVertex is one contiguous run of addresses; a gap larger than one Vtx ends
 * it. Each batch is recorded as (base, vertex count) and deduplicated on base
 * alone, never merged with a neighbour. The count is real, so 16 * count is the
 * real number of bytes that gsSPVertex read — true in a stub image and in a
 * full one. Summing distinct batches is therefore the working-set number;
 * unioning them is only meaningful once the arrays are full size.
 *
 * OUTPUT — incremental, never a dump
 * ----------------------------------
 * At most DC_CENSUS_BURST new point entries and DC_CENSUS_BBURST changed
 * batches are printed per report call:
 *
 *   CENSUS T 8c1e4020
 *   CENSUS P 8c2f0100
 *   CENSUS B M 8c3a0000 8c3a0860
 *   CENSUS SUM unique=418 seen=52310 overflow=0
 *   CENSUS BSUM batches=214 notes=1183402 full=0
 *
 * A BATCH LINE IS A SNAPSHOT, NOT A FINAL VALUE. A batch's high end grows if a
 * later frame loads more vertices from the same base, and it is re-emitted when
 * it does, so an earlier line for the same base is stale and strictly contained
 * in the later one. census_resolve.py keeps the longest line per base.
 *
 * Rate matters: the serial console is the bottleneck in this harness, and a
 * one-shot dump of several thousand lines would either stall the frame loop or
 * be truncated by the run's wall clock. tools/dcstub/census_resolve.py turns
 * the log into a symbol table.
 *
 * COST, AND WHY IT IS OFF BY DEFAULT
 * ----------------------------------
 * 8,192-slot point set + 1,024-entry batch table ≈ 110 KB of .bss, which this
 * port cannot afford in a shipping image and can easily afford in a probe one.
 * A point costs a masked linear probe on a pointer hash. A vertex component
 * costs two compares on the open batch, which a contiguous walk hits every
 * time. MEASURED: 29.3 -> 15.6 FPS on the title screen with the vertex shim in,
 * which is fine for a probe and is why -DDC_ASSET_CENSUS=1 (DC_ASSET_CENSUS in
 * dc/Makefile) is the only thing that compiles any of it in.
 */
#include "dc_platform.h"

#if defined(DC_ASSET_CENSUS) && DC_ASSET_CENSUS

/* Power of two. 8,192 slots holds the whole title path with room to spare;
 * the overflow counter is printed so a scene that outgrows it says so instead
 * of quietly reporting a short list. */
#define DC_CENSUS_SLOTS   8192u
#define DC_CENSUS_BURST   64

typedef struct {
    u32 addr;      /* 0 = empty slot */
    u8  kind;      /* 'T' texture image, 'P' palette, 'V' vertex-array base */
    u8  printed;
} census_ent;

static census_ent s_tab[DC_CENSUS_SLOTS];
static u32 s_order[DC_CENSUS_SLOTS];   /* insertion order, for stable output */
static u32 s_unique;
static u32 s_seen;
static u32 s_overflow;
static u32 s_printed;

/* ---- vertex batch table (kind 'M') ----------------------------------------
 *
 * 1,024 batches * 12 B = 12,288 B of .bss on top of the point table. One entry
 * per distinct gsSPVertex base the scene executed; the title screen used 21
 * batch+palette entries in the range-shaped prototype, so 1,024 is generous.
 * s_bat_full says so out loud if a scene outgrows it.
 *
 * A batch ends when the address stream jumps by more than one Vtx. dl_G_VTX
 * emits ob[0], ob[1], ob[2] at +0/+2/+4 and then steps to the next vertex at
 * +16, so "still inside" is `0 <= a - last <= 16`. Two gsSPVertex commands that
 * are exactly adjacent in the same array (the common `&v[93],15` then
 * `&v[108],26` pattern) merge into one batch, which is correct — those bytes
 * are read once and must be counted once.
 *
 * Residual imprecision, stated so it is not rediscovered: in a STUB image the
 * arrays are 16 B apart, so two consecutive commands reading different arrays
 * can also land within one Vtx of each other and merge. Measured on the title
 * screen this did not bite — all 58 batches still matched a source gsSPVertex
 * site — but it is why census_resolve.py accepts `site_n <= nvtx` rather than
 * demanding equality. */
#define DC_CENSUS_BATCHES 1024u
#define DC_CENSUS_VSTEP   16u   /* sizeof(Vtx) */
#define DC_CENSUS_BBURST  48

typedef struct {
    u32 base;      /* first byte of the first vertex in the run */
    u32 hi;        /* one past the last byte of the last vertex */
    u32 phi;       /* last printed hi; != hi means "reprint me" */
} census_batch;

static census_batch s_bat[DC_CENSUS_BATCHES];
static u32 s_nbat;        /* batches in use, kept sorted by base */
static u32 s_bat_full;    /* batches dropped because the table was full */
static u32 s_bat_scan;    /* round-robin cursor for the report */
static u32 s_vtx_notes;   /* raw component reads — the seam's canary */
static u32 s_cur_base;    /* the open batch, 0 = none */
static u32 s_cur_last;

/* Fibonacci hash. Asset addresses are 32-byte aligned by ATTRIBUTE_ALIGN(32),
 * so the low five bits are always zero and a plain mask would collide every
 * time. */
static u32 census_slot(u32 addr) {
    return ((addr >> 5) * 2654435761u) >> 19;   /* -> 13 bits, DC_CENSUS_SLOTS */
}

void dc_asset_census_note(const void* addr, int kind) {
    u32 a = (u32)(uintptr_t)addr;
    u32 i, probe;

    if (!a) return;
    s_seen++;

    i = census_slot(a) & (DC_CENSUS_SLOTS - 1u);
    for (probe = 0; probe < DC_CENSUS_SLOTS; probe++) {
        census_ent* e = &s_tab[(i + probe) & (DC_CENSUS_SLOTS - 1u)];
        if (e->addr == a) return;                /* already recorded */
        if (!e->addr) {
            e->addr = a;
            e->kind = (u8)kind;
            e->printed = 0;
            s_order[s_unique++] = (i + probe) & (DC_CENSUS_SLOTS - 1u);
            return;
        }
    }
    s_overflow++;
}

/* Fold the open batch into the table. Keyed on base alone and idempotent, so
 * calling it while the batch is still growing (which dc_asset_census_report
 * does, or the last batch of the run would never be seen) costs nothing. */
static void census_batch_commit(void) {
    u32 hi, first, last, mid, i, k;

    if (!s_cur_base) return;

    /* The last vertex STARTS at base + 16*floor((last-base)/16) whatever the
     * component was, so this is alignment-independent — Vtx only carries
     * 8-byte alignment (its union's `long long`), and the array need not be
     * 16-byte aligned. */
    hi = s_cur_last - ((s_cur_last - s_cur_base) % DC_CENSUS_VSTEP)
       + DC_CENSUS_VSTEP;

    first = 0; last = s_nbat;
    while (first < last) {
        mid = (first + last) >> 1;
        if (s_bat[mid].base < s_cur_base) first = mid + 1; else last = mid;
    }
    i = first;

    if (i < s_nbat && s_bat[i].base == s_cur_base) {
        if (hi > s_bat[i].hi) s_bat[i].hi = hi;   /* a later frame loaded more */
        return;
    }
    if (s_nbat >= DC_CENSUS_BATCHES) { s_bat_full++; return; }
    for (k = s_nbat; k > i; k--) s_bat[k] = s_bat[k - 1];
    s_bat[i].base = s_cur_base;
    s_bat[i].hi = hi;
    s_bat[i].phi = 0;
    s_nbat++;
}

void dc_asset_census_vtx(const void* addr) {
    u32 a = (u32)(uintptr_t)addr;

    if (!a) return;
    s_vtx_notes++;

    /* The hot path, and the reason this is not a table lookup: a gsSPVertex of
     * n vertices makes 3n calls and 3n-1 of them land here. */
    if (s_cur_base && a >= s_cur_last && a - s_cur_last <= DC_CENSUS_VSTEP) {
        s_cur_last = a;
        return;
    }

    census_batch_commit();
    s_cur_base = a;
    s_cur_last = a;
}

void dc_asset_census_report(void) {
    int burst = 0;
    u32 scanned;

    while (s_printed < s_unique && burst < DC_CENSUS_BURST) {
        census_ent* e = &s_tab[s_order[s_printed++]];
        if (!e->printed) {
            e->printed = 1;
            DC_LOGE("CENSUS %c %08x\n", (char)e->kind, (unsigned)e->addr);
            burst++;
        }
    }

    /* Without this the batch still open when the run ends — and a run of this
     * game never ends on purpose — would never be reported. Idempotent. */
    census_batch_commit();

    /* A batch's hi grows if a later frame loads more vertices from the same
     * base, so "printed once" is not a valid stopping condition. Round-robin
     * from where the last report stopped, so a table larger than one burst
     * converges instead of starving its tail. */
    burst = 0;
    for (scanned = 0; scanned < s_nbat && burst < DC_CENSUS_BBURST; scanned++) {
        census_batch* b;
        if (s_bat_scan >= s_nbat) s_bat_scan = 0;
        b = &s_bat[s_bat_scan++];
        if (b->hi != b->phi) {
            b->phi = b->hi;
            DC_LOGE("CENSUS B M %08x %08x\n",
                    (unsigned)b->base, (unsigned)b->hi);
            burst++;
        }
    }

    DC_LOGE("CENSUS SUM unique=%u printed=%u seen=%u overflow=%u\n",
            (unsigned)s_unique, (unsigned)s_printed, (unsigned)s_seen,
            (unsigned)s_overflow);
    DC_LOGE("CENSUS BSUM batches=%u notes=%u full=%u\n",
            (unsigned)s_nbat, (unsigned)s_vtx_notes, (unsigned)s_bat_full);
}

#else

void dc_asset_census_note(const void* addr, int kind) { (void)addr; (void)kind; }
void dc_asset_census_vtx(const void* addr) { (void)addr; }
void dc_asset_census_report(void) { }

#endif /* DC_ASSET_CENSUS */
