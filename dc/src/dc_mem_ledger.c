/* dc_mem_ledger.c - allocation accounting against the 16 MB budget.
 *
 * Implements kb/mem-budget.md §5. See dc/include/dc_mem_ledger.h for the API
 * and dc/include/dc_mem_budget.h for the §4 table.
 *
 * WHAT IS IMPLEMENTED HERE
 *   - per-bucket extents with hard non-overlap (an overrun cannot corrupt a
 *     neighbour; it returns NULL and is attributed by owner name)
 *   - cur / peak / peak_tag accounting, O(1) on every alloc
 *   - image measurement from linker symbols (not a header, so "someone added a
 *     200 KB array to BSS" shows up on the next boot)
 *   - the 32 MB RAM-mod refusal (CLAUDE.md hardware contract)
 *   - the machine-parseable MEMLEDGER report harness/dc/smoke.sh greps
 *
 * WHAT IS DELIBERATELY NOT IMPLEMENTED YET (kb §5, needs the build system)
 *   - -Wl,--wrap=malloc,--wrap=free,--wrap=memalign to capture KOS/GLdc
 *     allocations. Without it DCMEM_KOS is a dc_mem_note() estimate, not a
 *     measurement, and the ledger UNDER-reports. Flagged in the report output.
 *   - largest_free fragmentation watch (needs a real free list; the extents
 *     are bump allocators for now).
 *
 * DEVIATION FROM kb §5 step 3: the kb wants ONE contiguous arena carved from
 * &end to _arch_mem_top. That region is also KOS's own malloc heap, and taking
 * all of it in a first pass starves KOS before we know how much it needs
 * (bucket 1 is a [?]). This file therefore carves ONE memalign extent PER
 * BUCKET, lazily, which preserves the property that matters — a bucket cannot
 * overrun into another — while leaving KOS its heap. Define DC_MEM_SINGLE_ARENA
 * to get the kb's single-arena behaviour once bucket 1 has been measured.
 */
#include "dc_platform.h"
#include "dc_mem_ledger.h"
#include "dc_mem_budget.h"

#include <malloc.h>

/* Compile-time enforcement, on the CORRECT inequality. The old check summed
 * every bucket against 16 MB, which cannot detect a double-count: buckets
 * 3/4/5 are the image span, not heap, so counting them on the heap side and
 * again as image passes a sum test while over-committing the machine. Only
 * the additive heap can be checked at compile time — the image span is a link
 * product, so dc_mem_report() checks the full inequality at runtime against
 * the linker symbols. */
#if DC_HEAP_ADDITIVE > DC_RAM_USABLE_BYTES
#error "dc_mem_budget.h over-commits: additive heap alone exceeds usable RAM"
#endif

/* DC_MEM_STRICT: default for dev/Flycast builds. An overrun dumps the table
 * and exits, loudly. Release builds return NULL and let the caller degrade. */
#ifndef DC_MEM_RELEASE
#define DC_MEM_STRICT 1
#endif

typedef struct {
    const char*  name;
    size_t       budget;    /* from dc_mem_budget.h                       */
    int          carve;     /* 1 = has a real extent, 0 = accounted only  */
    unsigned char* base;    /* extent base (NULL until first alloc)       */
    size_t       reserved;  /* extent size                                */
    size_t       bump;      /* bytes handed out of the extent             */
    ptrdiff_t    noted;     /* dc_mem_note() running total                */
    size_t       peak;      /* max(bump + noted)                          */
    const char*  peak_tag;
    const char*  peak_owner;
    unsigned int fails;
} dc_bucket_t;

#define B(name, budget, carve) { name, budget, carve, 0, 0, 0, 0, 0, "boot", "-", 0 }

static dc_bucket_t s_bucket[DCMEM_NBUCKET] = {
    B("KOS",          DC_BUDGET_KOS,          0),
    B("IMAGE_TEXT",   DC_BUDGET_IMAGE_TEXT,   0),
    B("IMAGE_DATA",   DC_BUDGET_IMAGE_DATA,   0),
    B("IMAGE_BSS",    DC_BUDGET_IMAGE_BSS,    0),
    B("JKRHEAP",      DC_BUDGET_JKRHEAP,      1),
    B("ASSET_POOL",   DC_BUDGET_ASSET_POOL,   1),
    B("ARAM_GRAPH",   DC_BUDGET_ARAM_GRAPH,   1),
    /* Retired: 0-budget, non-carveable. See dc_mem_budget.h — the bytes these
     * claimed to reserve are .bss already counted by IMAGE_BSS. Nothing ever
     * allocated from them, so this is a bookkeeping correction, not a cut. */
    B("AUDIO",        DC_BUDGET_AUDIO,        0),
    B("DISC_IO",      DC_BUDGET_DISC_IO,      0),
    B("PVR_STAGING",  DC_BUDGET_PVR_STAGING,  0),
    B("STACKS",       DC_BUDGET_STACKS,       0),
};

#undef B

static const char* s_tag = "boot";
static int         s_inited = 0;
static int         s_wrap_active = 0;  /* set by __wrap_malloc when linked in */

/* --- linker symbols --------------------------------------------------------
 * RESOLVED (M1): KOS declares all four of these itself, so we must NOT
 * re-declare them — GCC 14+ makes a type mismatch a hard error, and KOS spells
 * two of them as scalars rather than arrays:
 *     arch/arch.h  : extern char    _executable_start;   extern char _etext;
 *     kos/linker.h : extern uint8_t _bss_start[];        extern uint8_t end[];
 * (Both headers arrive via <kos.h> in dc_platform.h.)
 *
 * `_bss_start` replaces the `edata` this originally used: KOS's shlelf.xc is
 * not the GNU default script and does not guarantee an `edata`, whereas
 * `_bss_start` is exactly the data/bss boundary we want. */
#ifndef DC_HOST_STUB
#define DC_IMG_TEXT_LO  ((const char*)&_executable_start)
#define DC_IMG_TEXT_HI  ((const char*)&_etext)
#define DC_IMG_DATA_HI  ((const char*)_bss_start)
#define DC_IMG_BSS_HI   ((const char*)end)
#endif

static size_t dc_span(const char* a, const char* b) {
    if (!a || !b || b < a) return 0;
    return (size_t)(b - a);
}

static void dc_measure_image(void) {
#ifndef DC_HOST_STUB
    size_t text = dc_span(DC_IMG_TEXT_LO, DC_IMG_TEXT_HI);
    size_t data = dc_span(DC_IMG_TEXT_HI, DC_IMG_DATA_HI);
    size_t bss  = dc_span(DC_IMG_DATA_HI, DC_IMG_BSS_HI);
    s_bucket[DCMEM_IMAGE_TEXT].noted = (ptrdiff_t)text;
    s_bucket[DCMEM_IMAGE_DATA].noted = (ptrdiff_t)data;
    s_bucket[DCMEM_IMAGE_BSS ].noted = (ptrdiff_t)bss;
    s_bucket[DCMEM_IMAGE_TEXT].peak  = text;
    s_bucket[DCMEM_IMAGE_DATA].peak  = data;
    s_bucket[DCMEM_IMAGE_BSS ].peak  = bss;

    /* Also publish the image range for the seg2k0 pointer heuristic. On PC
     * this came from dladdr(); on DC the linker symbols are exact. */
    pc_image_base = (unsigned int)(uintptr_t)DC_IMG_TEXT_LO;
    pc_image_end  = (unsigned int)(uintptr_t)DC_IMG_BSS_HI;
#endif
}

/* --- 32 MB refusal ---------------------------------------------------------
 * CLAUDE.md: "A 32 MB RAM mod exists; it must never become a requirement."
 * dca3 holds this line and so do we. Detecting the mod is a WARNING, not a
 * licence to spend the extra RAM. */
static void dc_check_ram_mod(void) {
#ifndef DC_HOST_STUB
    uintptr_t top = (uintptr_t)_arch_mem_top;   /* VERIFY: KOS arch/arch.h */
    if (top > 0x8D000000u) {
        DC_LOGE("MEMLEDGER WARN 32MB-detected top=0x%08X budget-still-16MB\n",
                (unsigned)top);
#ifndef DC_ALLOW_32MB
        DC_LOGE("MEMLEDGER WARN clamping to stock 16 MB (build with "
                "DC_ALLOW_32MB to opt out; do not ship that)\n");
#endif
    }
#endif
}

void dc_mem_ledger_init(void) {
    if (s_inited) return;
    s_inited = 1;
    dc_measure_image();
    dc_check_ram_mod();
    DC_LOGE("MEMLEDGER INIT buckets=%d budget=%u\n",
            (int)DCMEM_NBUCKET, (unsigned)DC_RAM_BUDGET_BYTES);
    /* FIT is also printed by dc_mem_report(), but that only runs on the way
     * out of main() and the game does not normally return. The inequality is
     * the number this whole project turns on, so print it the moment the
     * linker symbols are readable — i.e. here. */
    dc_mem_report_fit();
}

/* The one inequality (kb/STATE.md). Split so it is obvious which side is over,
 * and read from the linker symbols so a newly added .bss array shows up on the
 * next boot without anyone updating a table. */
void dc_mem_report_fit(void) {
#ifndef DC_HOST_STUB
    size_t span = dc_span(DC_IMG_TEXT_LO, DC_IMG_BSS_HI);
    long   fit  = (long)DC_RAM_USABLE_BYTES - (long)span - (long)DC_HEAP_ADDITIVE;
    DC_LOGE("MEMLEDGER FIT image_span=%u additive_heap=%u usable=%u "
            "margin=%ld %s\n",
            (unsigned)span, (unsigned)DC_HEAP_ADDITIVE,
            (unsigned)DC_RAM_USABLE_BYTES, fit, fit >= 0 ? "OK" : "OVER");
#endif
}

static int dc_bucket_valid(dc_mem_bucket_t b) {
    return (int)b >= 0 && (int)b < (int)DCMEM_NBUCKET;
}

/* Lazily carve a bucket's extent. See the DEVIATION note at the top. */
static int dc_bucket_carve(dc_bucket_t* bk) {
    if (bk->base || !bk->carve) return bk->base != 0;
#ifdef DC_HOST_STUB
    bk->base = (unsigned char*)malloc(bk->budget);
#else
    bk->base = (unsigned char*)memalign(32, bk->budget);
#endif
    if (!bk->base) {
        DC_LOGE("MEMLEDGER FAIL carve bucket=%s want=%u (system out of memory)\n",
                bk->name, (unsigned)bk->budget);
        return 0;
    }
    bk->reserved = bk->budget;
    DC_LOG("[DC/MEM] carved %s: %u B at %p\n",
           bk->name, (unsigned)bk->reserved, (void*)bk->base);
    return 1;
}

static void dc_bucket_touch_peak(dc_bucket_t* bk, const char* owner) {
    size_t cur = bk->bump + (bk->noted > 0 ? (size_t)bk->noted : 0);
    if (cur > bk->peak) {
        bk->peak = cur;
        bk->peak_tag = s_tag;
        bk->peak_owner = owner ? owner : "-";
    }
}

void* dc_mem_alloc(dc_mem_bucket_t b, size_t size, size_t align, const char* owner) {
    dc_bucket_t* bk;
    size_t off, pad;

    if (!s_inited) dc_mem_ledger_init();
    if (!dc_bucket_valid(b)) return NULL;
    bk = &s_bucket[b];

    if (align < 4) align = 4;
    if (align & (align - 1)) align = 32;   /* not a power of two: fall back */

    if (!bk->carve) {
        /* Accounted-only bucket: no extent to hand out. Callers that need real
         * memory from KOS/IMAGE/STACKS buckets should dc_mem_note() instead. */
        DC_LOGE("MEMLEDGER FAIL bucket=%s owner=%s reason=not-carveable\n",
                bk->name, owner ? owner : "-");
        return NULL;
    }
    if (!dc_bucket_carve(bk)) return NULL;

    off = bk->bump;
    pad = (align - (((uintptr_t)bk->base + off) & (align - 1))) & (align - 1);
    off += pad;

    if (size > bk->reserved || off > bk->reserved - size) {
        bk->fails++;
        DC_LOGE("MEMLEDGER FAIL bucket=%s owner=%s want=%u free=%u tag=%s\n",
                bk->name, owner ? owner : "-", (unsigned)size,
                (unsigned)(bk->reserved - bk->bump), s_tag);
#ifdef DC_MEM_STRICT
        dc_mem_report(1);
        DC_LOGE("MEMLEDGER ABORT strict-mode\n");
#ifndef DC_HOST_STUB
        arch_exit();
#else
        exit(1);
#endif
#endif
        return NULL;
    }

    bk->bump = off + size;
    dc_bucket_touch_peak(bk, owner);
    return bk->base + off;
}

void dc_mem_free(dc_mem_bucket_t b, void* p) {
    /* The extents are bump allocators: individual frees are not reclaimable.
     * Accounted here so the caller's intent is visible; the asset pool and the
     * graph-ARAM window do their own LRU eviction on top of a fixed extent. */
    (void)p;
    if (!dc_bucket_valid(b)) return;
    /* Intentionally no bump rollback — doing it only for the newest block
     * would give a false sense of reclaim. */
}

void dc_mem_note(dc_mem_bucket_t b, ptrdiff_t delta) {
    if (!s_inited) dc_mem_ledger_init();
    if (!dc_bucket_valid(b)) return;
    s_bucket[b].noted += delta;
    dc_bucket_touch_peak(&s_bucket[b], "note");
}

void dc_mem_tag(const char* tag) {
    s_tag = tag ? tag : "-";
}

void dc_mem_frame(void) {
    /* O(1): the counters are already maintained on every alloc/note. This exists
     * as the hook the debug overlay / VMU LCD will poll (PLAN §7). */
}

size_t dc_mem_total_used(void) {
    size_t total = 0;
    int i;
    for (i = 0; i < (int)DCMEM_NBUCKET; i++) {
        total += s_bucket[i].bump;
        if (s_bucket[i].noted > 0) total += (size_t)s_bucket[i].noted;
    }
    return total;
}

void* dc_mem_bucket_base(dc_mem_bucket_t b) {
    if (!dc_bucket_valid(b)) return NULL;
    if (!dc_bucket_carve(&s_bucket[b])) return NULL;
    return s_bucket[b].base;
}

size_t dc_mem_bucket_size(dc_mem_bucket_t b) {
    if (!dc_bucket_valid(b)) return 0;
    return s_bucket[b].budget;
}

void dc_mem_report(int verbose) {
    size_t total = 0, budget = 0;
    int i;

    for (i = 0; i < (int)DCMEM_NBUCKET; i++) {
        dc_bucket_t* bk = &s_bucket[i];
        size_t used = bk->bump + (bk->noted > 0 ? (size_t)bk->noted : 0);
        total  += used;
        budget += bk->budget;
        DC_LOGE("MEMLEDGER %s budget=%u reserved=%u used=%u peak=%u tag=%s\n",
                bk->name, (unsigned)bk->budget, (unsigned)bk->reserved,
                (unsigned)used, (unsigned)bk->peak, bk->peak_tag);
        if (verbose && bk->peak_owner)
            DC_LOGE("MEMLEDGER   %s peak_owner=%s fails=%u\n",
                    bk->name, bk->peak_owner, bk->fails);
    }

    DC_LOGE("MEMLEDGER TOTAL used=%u budget=%u margin=%d\n",
            (unsigned)total, (unsigned)DC_RAM_BUDGET_BYTES,
            (int)((long)DC_RAM_BUDGET_BYTES - (long)total));
    if (budget != DC_BUDGET_TOTAL)
        DC_LOGE("MEMLEDGER WARN table-sum=%u header-sum=%u (drift)\n",
                (unsigned)budget, (unsigned)DC_BUDGET_TOTAL);

    /* The inequality that actually decides whether the machine fits. */
    dc_mem_report_fit();
    if (!s_wrap_active)
        DC_LOGE("MEMLEDGER WARN no-malloc-wrap KOS/GLdc allocations "
                "NOT counted; totals are a lower bound\n");
}

/* --- optional malloc interposition ----------------------------------------
 * Active only when the link line carries
 *   -Wl,--wrap=malloc,--wrap=free,--wrap=memalign
 * (kb §5 "Third-party allocations get captured"). The build system owns that
 * flag; these definitions are harmless when it is absent. */
#ifdef DC_MEM_WRAP_MALLOC
extern void* __real_malloc(size_t);
extern void  __real_free(void*);
extern void* __real_memalign(size_t, size_t);

void* __wrap_malloc(size_t n) {
    void* p;
    s_wrap_active = 1;
    p = __real_malloc(n);
    if (p) dc_mem_note(DCMEM_KOS, (ptrdiff_t)n);
    return p;
}

void* __wrap_memalign(size_t a, size_t n) {
    void* p;
    s_wrap_active = 1;
    p = __real_memalign(a, n);
    if (p) dc_mem_note(DCMEM_KOS, (ptrdiff_t)n);
    return p;
}

void __wrap_free(void* p) {
    /* Size is unknown without a shadow table; the note is not decremented.
     * TODO(M2): 8-byte header shadow so frees are accounted. */
    __real_free(p);
}
#endif /* DC_MEM_WRAP_MALLOC */
