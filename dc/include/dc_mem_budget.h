/* dc_mem_budget.h - the kb/mem-budget.md §4 table, as constants.
 *
 * DO NOT edit these numbers without editing kb/mem-budget.md §4 in the same
 * commit. mem-budget.md §5 wants this file generated from the kb table by a
 * host script so doc and code cannot drift; until that script exists, the
 * numbers are transcribed by hand and this comment is the contract.
 *
 * Values marked [?] in the kb are unmeasured guesses. They are marked here too.
 */
#ifndef DC_MEM_BUDGET_H
#define DC_MEM_BUDGET_H

#define DC_RAM_BUDGET_BYTES        (16u * 1024u * 1024u)   /* 16,777,216 */

/* Bucket 1  — KOS kernel + newlib + drivers.            [?] measure at M0 */
#define DC_BUDGET_KOS              1000000u
/* Bucket 2  — reserved low RAM 0x8C000000-0x8C010000. Not spendable.      */
#define DC_BUDGET_LOWRAM             65536u
/* Bucket 3  — game+platform .text.                      [?] measure at M1 */
#define DC_BUDGET_IMAGE_TEXT       2600000u
/* Bucket 4  — game+platform .rodata + .data                              */
#define DC_BUDGET_IMAGE_DATA       1800000u
/* Bucket 5  — game+platform .bss (true runtime state)                    */
#define DC_BUDGET_IMAGE_BSS        1950000u
/* Bucket 6  — JKRHeap system heap + __osMalloc game arena.
 *             THE SINGLE BIGGEST UNKNOWN IN THE BUDGET (kb §4.2).         */
#define DC_BUDGET_JKRHEAP          4000000u
/* Bucket 7  — asset residency pool (replaces 8.77 MB of static BSS)       */
#define DC_BUDGET_ASSET_POOL       1500000u
/* Bucket 8  — graph-ARAM (RARC) LRU window.             [?] probe 3 at M1 */
#define DC_BUDGET_ARAM_GRAPH        512000u
/* Bucket 9  — audio work RAM, SH-4 side                                  */
#define DC_BUDGET_AUDIO             700000u
/* Bucket 10 — disc read-ahead ring + KOS VFS buffers (3 x 128 KB)         */
#define DC_BUDGET_DISC_IO           384000u
/* Bucket 11 — PVR vertex staging / TA buffers in main RAM                 */
#define DC_BUDGET_PVR_STAGING       384000u
/* Bucket 12 — thread stacks (main 64K, audio 32K, read-ahead 16K, KOS)    */
#define DC_BUDGET_STACKS            131072u

#define DC_BUDGET_TOTAL (                       \
      DC_BUDGET_KOS          + DC_BUDGET_LOWRAM \
    + DC_BUDGET_IMAGE_TEXT   + DC_BUDGET_IMAGE_DATA \
    + DC_BUDGET_IMAGE_BSS    + DC_BUDGET_JKRHEAP    \
    + DC_BUDGET_ASSET_POOL   + DC_BUDGET_ARAM_GRAPH \
    + DC_BUDGET_AUDIO        + DC_BUDGET_DISC_IO    \
    + DC_BUDGET_PVR_STAGING  + DC_BUDGET_STACKS)
/* kb §4 total: 15,026,608 B (89.6 %), margin 1,750,608 B (10.4 %) */

#endif /* DC_MEM_BUDGET_H */
