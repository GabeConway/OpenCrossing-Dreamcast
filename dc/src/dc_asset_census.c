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
 *   GXLoadTexObj  -> the texture image pointer being bound
 *   GXSetArray    -> the base of a vertex / normal / colour / texcoord array
 *
 * Those addresses are link-time constants inside .bss (or .data), so recording
 * the distinct ones and resolving them against the ELF symbol table on the
 * host names the exact destination arrays a scene reached for, and — via each
 * symbol's st_size — how many bytes they are. That is the working-set number
 * S4 has to size its pool against, and it is the measurement
 * kb/research-creative-ram.md T1 ("textures are never pooled") needs before
 * the pool's shape can be decided.
 *
 * IT WORKS IN A STUB BUILD, WHICH IS THE POINT. Under DC_ASSET_STUB the
 * destinations are [1]-sized and full of garbage, so the frame is wrong — but
 * the *identity* of what the game asked for is unaffected, because the symbol
 * it dereferences is the same symbol either way. So the working set can be
 * measured now, on the image that boots, rather than after S4 lands.
 *
 * OUTPUT — incremental, never a dump
 * ----------------------------------
 * At most DC_CENSUS_BURST new entries are printed per report call, oldest
 * first, and each is printed exactly once:
 *
 *   CENSUS T 8c1e4020
 *   CENSUS V 8c2f0100
 *   CENSUS SUM unique=418 seen=52310 overflow=0
 *
 * Rate matters: the serial console is the bottleneck in this harness, and a
 * one-shot dump of several thousand lines would either stall the frame loop or
 * be truncated by the run's wall clock. tools/dcstub/census_resolve.py turns
 * the log into a symbol table.
 *
 * COST, AND WHY IT IS OFF BY DEFAULT
 * ----------------------------------
 * The table is a fixed 8,192-slot open-addressed set — 32,768 B of .bss, which
 * this port cannot afford in a shipping image and can easily afford in a probe
 * one. Lookup is a masked linear probe on a pointer hash, so the per-call cost
 * is a handful of instructions on a path that already does texture conversion.
 * -DDC_ASSET_CENSUS=1 (DC_ASSET_CENSUS in dc/Makefile) is the only thing that
 * compiles any of it in; otherwise this file contributes two empty functions.
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
    u8  kind;      /* 'T' texture image, 'V' vertex-array base */
    u8  printed;
} census_ent;

static census_ent s_tab[DC_CENSUS_SLOTS];
static u32 s_order[DC_CENSUS_SLOTS];   /* insertion order, for stable output */
static u32 s_unique;
static u32 s_seen;
static u32 s_overflow;
static u32 s_printed;

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

void dc_asset_census_report(void) {
    int burst = 0;

    while (s_printed < s_unique && burst < DC_CENSUS_BURST) {
        census_ent* e = &s_tab[s_order[s_printed++]];
        if (!e->printed) {
            e->printed = 1;
            DC_LOGE("CENSUS %c %08x\n", (char)e->kind, (unsigned)e->addr);
            burst++;
        }
    }
    DC_LOGE("CENSUS SUM unique=%u printed=%u seen=%u overflow=%u\n",
            (unsigned)s_unique, (unsigned)s_printed, (unsigned)s_seen,
            (unsigned)s_overflow);
}

#else

void dc_asset_census_note(const void* addr, int kind) { (void)addr; (void)kind; }
void dc_asset_census_report(void) { }

#endif /* DC_ASSET_CENSUS */
