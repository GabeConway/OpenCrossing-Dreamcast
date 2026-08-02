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

/* Bucket 1  — KOS runtime heap ONLY.
 *             CORRECTED: KOS/newlib/libstdc++ CODE is statically linked INTO
 *             our ELF (304,829 B in AnimalCrossing.map), so it is already
 *             counted by buckets 3-5. Reserving 1,000,000 here double-counted
 *             it. Only KOS's own runtime allocations are additive.
 *             kb/research-budget-premises.md 3.2. */
#define DC_BUDGET_KOS               262144u
/* Bucket 2  — reserved low RAM 0x8C000000-0x8C010000. Not spendable.      */
#define DC_BUDGET_LOWRAM             65536u
/* Bucket 3  — game+platform .text.                      [?] measure at M1 */
#define DC_BUDGET_IMAGE_TEXT       2600000u
/* Bucket 4  — game+platform .rodata + .data                              */
#define DC_BUDGET_IMAGE_DATA       1800000u
/* Bucket 5  — game+platform .bss (true runtime state)                    */
#define DC_BUDGET_IMAGE_BSS        1950000u
/* Bucket 6  — JKRHeap system heap + __osMalloc game arena.
 *             STILL THE SINGLE BIGGEST UNKNOWN (kb §4.2): the __osMalloc
 *             high-water mark has never been measured; see
 *             kb/research-budget-premises.md §2.4 for the recipe. What HAS
 *             been settled is that 1,294,496 B of it was dead weight (XFB +
 *             GX FIFO, both now killed at source under TARGET_DC), so this
 *             cut leaves __osMalloc's usable pool exactly where it was.
 *             MUST equal DC_MAIN_MEMORY_SIZE — dc_os.c static-asserts it.  */
#define DC_BUDGET_JKRHEAP          2705504u
/* Bucket 7  — asset residency pool (replaces 8.77 MB of static BSS)       */
#define DC_BUDGET_ASSET_POOL       1500000u
/* Bucket 8  — graph-ARAM (RARC) window.  [MEASURED-FLOOR] see dc_platform.h
 *
 * Was 512,000 and it was BOTH too small and pointed at the wrong region: graph
 * ARAM starts at offset 8,454,144 (jaudio's 0x810000 is allocated first), and
 * the boot archive forest_1st.arc alone is 851,744 B. dc_aram.c now anchors
 * the window on first use; this is the floor that lets the anchor hold a whole
 * archive. Keep it equal to DC_ARAM_WINDOW_SIZE — ARInit allocates from this
 * bucket and a mismatch is a boot-time MEMLEDGER FAIL, not a warning. */
#define DC_BUDGET_ARAM_GRAPH        1048576u
/* Buckets 9/10/11 — RETIRED, and they were never real.
 *
 * All three were phantom reservations: nothing ever called
 * dc_mem_alloc(DCMEM_AUDIO/DISC_IO/PVR_STAGING), so their extents were never
 * carved and they cost 0 runtime bytes — while the storage they claimed to
 * budget for is already .bss inside the image, i.e. already counted by
 * bucket 5. Reserving for them a second time is exactly the double-count that
 * made the old ledger report 8,035,072 B of image budget instead of
 * 10,306,464 B. Kept as 0-valued names so the DCMEM_* enum in dc_mem_ledger.h
 * and the s_bucket[] table do not have to be renumbered.
 *   9  audio      -> jaudio_NES, 1,265,101 B of .bss
 *   10 disc I/O   -> dc_dvd.c.o, 13,320 B of .bss (no read-ahead ring exists)
 *   11 PVR stage  -> g_gx in dc_gx.c.o. The 334,764 B this line used to quote
 *                   is dc_gx.c.o's WHOLE .bss, not g_gx: sh-elf-nm puts g_gx
 *                   itself at 334,088 B and the other ~676 B are file statics.
 *                   The 2026-08-01 vertex-buffer shrink (2040 verts x 32 B
 *                   instead of 8192 x 40 B, see dc_gx_internal.h) takes the TU
 *                   to 72,352 B and g_gx to 71,688 B.
 * kb/research-budget-premises.md §3.4. */
#define DC_BUDGET_AUDIO                  0u
#define DC_BUDGET_DISC_IO                0u
#define DC_BUDGET_PVR_STAGING            0u
/* Bucket 12 — thread stacks. KOS's own 64 KB kernel stack is already inside
 *             bucket 2's reserved low RAM, so only our threads are additive. */
#define DC_BUDGET_STACKS             65536u

#define DC_BUDGET_TOTAL (                       \
      DC_BUDGET_KOS          + DC_BUDGET_LOWRAM \
    + DC_BUDGET_IMAGE_TEXT   + DC_BUDGET_IMAGE_DATA \
    + DC_BUDGET_IMAGE_BSS    + DC_BUDGET_JKRHEAP    \
    + DC_BUDGET_ASSET_POOL   + DC_BUDGET_ARAM_GRAPH \
    + DC_BUDGET_AUDIO        + DC_BUDGET_DISC_IO    \
    + DC_BUDGET_PVR_STAGING  + DC_BUDGET_STACKS)
/* DC_BUDGET_TOTAL is NOT the fit criterion and never was — summing all
 * buckets is what allowed buckets 9/10/11 to be counted twice. The real
 * inequality, which dc_mem_ledger.c now checks at runtime against the linker
 * symbols, is:
 *
 *     (image span: _executable_start .. end) + (genuinely additive heap)
 *         <= DC_RAM_USABLE_BYTES
 *
 * where "additive heap" is buckets 1 + 6 + 8 + 12 only. Buckets 3/4/5 ARE the
 * image span, so adding them to the heap side double-counts them.
 *
 *   usable      16,646,144   (16 MB - 64 KB low RAM - 65 KB KOS reserve)
 *   image span  21,374,996   as linked today  -> OVER by 9,568,532 B
 *
 * kb/STATE.md carries the current numbers and the ranked levers. */
#define DC_RAM_USABLE_BYTES        16646144u

#define DC_HEAP_ADDITIVE (                      \
      DC_BUDGET_KOS + DC_BUDGET_JKRHEAP         \
    + DC_BUDGET_ARAM_GRAPH + DC_BUDGET_STACKS)

#endif /* DC_MEM_BUDGET_H */
