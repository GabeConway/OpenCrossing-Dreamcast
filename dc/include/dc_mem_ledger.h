/* dc_mem_ledger.h - allocation accounting against the 16 MB budget.
 *
 * Design: kb/mem-budget.md §5. Purpose: make the §4 table a runtime object so
 * the budget is ENFORCED rather than hoped, and an overrun is a loud greppable
 * failure rather than a mysterious crash three scenes later.
 *
 * Output format (harness/dc/smoke.sh greps these):
 *   MEMLEDGER <bucket> budget=<B> reserved=<B> used=<B> peak=<B> tag=<s>
 *   MEMLEDGER TOTAL used=<B> budget=16777216 margin=<B>
 *   MEMLEDGER FAIL bucket=<n> owner=<s> want=<B> free=<B>
 */
#ifndef DC_MEM_LEDGER_H
#define DC_MEM_LEDGER_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DCMEM_KOS = 0,
    DCMEM_IMAGE_TEXT,
    DCMEM_IMAGE_DATA,
    DCMEM_IMAGE_BSS,
    DCMEM_JKRHEAP,
    DCMEM_ASSET_POOL,
    DCMEM_ARAM_GRAPH,
    DCMEM_AUDIO,
    DCMEM_DISC_IO,
    DCMEM_PVR_STAGING,
    DCMEM_STACKS,
    DCMEM_NBUCKET
} dc_mem_bucket_t;

/* Call FIRST in main(), before any other allocation. */
void  dc_mem_ledger_init(void);

/* Bump-allocate from a bucket's own extent. Never crosses into another
 * bucket: an overrun returns NULL (or aborts under DC_MEM_STRICT) and is
 * attributed to `owner` by name. `align` must be a power of two. */
void* dc_mem_alloc(dc_mem_bucket_t b, size_t size, size_t align, const char* owner);

/* Free is currently a no-op accounting adjustment for the bump extents; the
 * asset pool and the ARAM window do their own eviction on top. */
void  dc_mem_free(dc_mem_bucket_t b, void* p);

/* Account for memory an allocator we do not own took (KOS, GLdc, newlib
 * malloc). Positive to add, negative to release. */
void  dc_mem_note(dc_mem_bucket_t b, ptrdiff_t delta);

/* Attribute subsequent peaks to a place: "town", "house", "shop", "island". */
void  dc_mem_tag(const char* tag);

/* O(1) per-frame poll. Feeds the debug overlay / VMU LCD later. */
void  dc_mem_frame(void);

/* Print the whole table. verbose != 0 adds per-bucket owner detail. */
void  dc_mem_report(int verbose);

/* Print the MEMLEDGER FIT line on its own. Called from dc_mem_ledger_init() —
 * dc_mem_report() only runs when main() returns, which the game never does. */
void  dc_mem_report_fit(void);

/* Total currently accounted bytes across all buckets. */
size_t dc_mem_total_used(void);

/* Bucket base/extent, for code that wants to sub-allocate itself (the JKRHeap
 * arena in dc_os.c does this). Returns NULL if the bucket has no extent. */
void*  dc_mem_bucket_base(dc_mem_bucket_t b);
size_t dc_mem_bucket_size(dc_mem_bucket_t b);

#ifdef __cplusplus
}
#endif

#endif /* DC_MEM_LEDGER_H */
