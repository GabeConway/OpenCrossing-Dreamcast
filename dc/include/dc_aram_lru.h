/* dc_aram_lru.h - tunables and the dc_dvd.c seam for the disc-backed graph-ARAM
 * pager (PLAN §3.1). Implementation: dc/src/dc_aram.c + dc/src/dc_dvd.c.
 *
 * WHY THIS EXISTS AT ALL
 *
 * The GameCube treats 16 MB of ARAM as a second address space: archives are
 * DMA'd in and read back on demand. The Dreamcast has none, so dc_aram.c used
 * to keep ONE CONTIGUOUS resident window of main RAM and pretend. Two measured
 * problems with that (kb/STATE.md, kb/heap-two-pools.md):
 *
 *   1. It thrashes. The window anchors itself to whichever region wrote last,
 *      and mounting forest_2nd.arc after forest_1st.arc rebased it 13 times in
 *      one run — every rebase throws away everything resident.
 *   2. It costs 851,968 B of a budget that is 5,431,104 B over, and it has to,
 *      because a contiguous window smaller than the largest contiguous ARAM
 *      EXTENT drops that whole extent on the floor.
 *
 * THE OBSERVATION THAT MAKES THIS CHEAP
 *
 * Almost everything written to graph ARAM was, moments earlier, read off the
 * disc — and the DMA source buffer IS the DVDRead destination buffer, with
 * nothing in between:
 *
 *     JKRAramStream::writeToAram()               (JKRAramStream.cpp:92-103)
 *         command->mStream->read(buffer, length);   -> DVDRead into `buffer`
 *         JKRAramPcs(0, (u32)buffer, destination, length, nullptr);
 *
 * So the pager does not have to STORE those bytes. It records the affine map
 *   (ARAM range) -> (disc file, file offset)
 * and re-reads from /cd on a miss. The backing store is the disc, which is
 * exactly what "disc-backed" was always supposed to mean, and the resident
 * pool shrinks to a cache plus a home for the few writes that have no disc
 * provenance (m_card.c:1607's three save blocks, and jaudio).
 *
 * MEASURED (2026-08-02, this worktree; see the report):
 *   - forest_1st.arc mounts as 26 transfers of <=32,768 B covering ONE
 *     contiguous 851,744 B ARAM extent at offset 8,454,144. It is NOT "one
 *     851,744 B transfer" — kb/heap-two-pools.md says that and it is wrong.
 *     The 851,968 floor was about the EXTENT being contiguous, not about any
 *     single DMA, which is why a block cache removes it.
 *   - All three archives on the disc are plain RARC ("RARC" magic, no Yaz0),
 *     so every archive byte written to ARAM has disc provenance.
 *
 * MEASURED RESULT (2026-08-02, DC_ASSET_STUB title-screen run, Flycast with
 * FastGDRomLoad=yes, DC_ARAM_WINDOW=131072):
 *
 *     [DC/ARAM] LRU w:mapped=4982400 stored=0 audio=8300384 LOST=0 |
 *               r:cache=44 disc=9 zero=0(0 B) | pool=4 blk pin_peak=0
 *               evict=4 ext=2/32 drop=0 | disc 9 reads 423552 B 1 opens 79 ms
 *
 *   4,982,400 B is exactly forest_1st (851,744) + forest_2nd (4,130,656): every
 *   graph-half byte mapped, none lost, and NOT ONE read zero-filled — against
 *   >=5,121 zero-filled reads and rebases=14 on the same run with the window.
 *   Two extents for two archives is the coalescer working. `pin_peak=0` says
 *   nothing anonymous was written, which is true only until the save path
 *   runs — see the DC_ARAM_WINDOW note in the report / kb/heap-two-pools.md.
 *
 * GRANULARITY, AND WHY IT IS NOT 4 KB
 *
 * CLAUDE.md's hardware contract: CD-R streams at ~500 KB/s and every seek is
 * expensive. A demand pager that faults 4 KB at a time would be slower than
 * the thing it replaces (kb/research-mmu-paging.md killed exactly that idea).
 * This one never faults a fixed page: a read miss is served by ONE fs_read of
 * the whole missing run, straight into the caller's destination buffer, and
 * the game's own read granularity is a whole RARC resource. The block size
 * below is only the CACHE's tag granularity, and it is 32,768 to match
 * JKRAramStream's own 0x8000 transfer chunk exactly.
 */
#ifndef DC_ARAM_LRU_H
#define DC_ARAM_LRU_H

#ifdef __cplusplus
extern "C" {
#endif

/* --- KILL SWITCH -----------------------------------------------------------
 * DC_ARAM_LRU=0 reverts to the 2026-08-02 behaviour exactly: one contiguous
 * window that re-anchors on every missed write, no disc backing, out-of-window
 * reads zero-filled. Required by CLAUDE.md §1 ("every optimization gets a kill
 * switch"). Plumbed as DC_ARAM_LRU in dc/Makefile — see the report for the
 * three lines to add there; this #ifndef is what makes the code build without
 * them. */
#ifndef DC_ARAM_LRU
#define DC_ARAM_LRU 1
#endif

/* Cache tag granularity. 32,768 because that is JKRAramStream's transfer chunk
 * (JKRAramStream.cpp:64, bufferSize defaults to 0x8000), so a write lands on
 * at most two blocks, and because 32 KB is ~65 ms at the 500 KB/s CD-R figure
 * — a sane minimum unit of disc work. Must be a power of two. */
#ifndef DC_ARAM_BLOCK_SIZE
#define DC_ARAM_BLOCK_SIZE 32768u
#endif

/* Descriptor slots for the resident pool. 64 x 32 KB = 2 MB, comfortably above
 * any window we would ever set; the live count is DC_ARAM_WINDOW_SIZE /
 * DC_ARAM_BLOCK_SIZE. 64 descriptors cost 768 B of .bss. */
#ifndef DC_ARAM_MAX_BLOCKS
#define DC_ARAM_MAX_BLOCKS 64
#endif

/* Extent-map slots. Writes that are contiguous in BOTH the ARAM offset space
 * and the source file coalesce into one extent, so an entire archive mount is
 * a single entry: forest_1st.arc's 26 transfers become one, forest_2nd.arc's
 * 126 become one. 32 slots x 16 B = 512 B of .bss and is far more than the
 * measured demand; overflow is counted, never silent. */
#ifndef DC_ARAM_MAX_EXTENTS
#define DC_ARAM_MAX_EXTENTS 32
#endif

/* DC_ARAM_TRACE=1 logs every ARAM DMA (type, offset, length, provenance).
 * Off by default: a single archive mount is ~150 lines and the SCIF console
 * is the bottleneck on real hardware. */
#ifndef DC_ARAM_TRACE
#define DC_ARAM_TRACE 0
#endif

/* --- the dc_dvd.c seam -----------------------------------------------------
 * Two functions, both implemented in dc/src/dc_dvd.c, both no-ops on a build
 * with DC_ARAM_LRU=0. */

/* Did the bytes at [buf, buf+len) arrive from the disc? Returns 1 and fills
 * *out_entry / *out_off (a DVDConvertPathToEntrynum index and a byte offset in
 * that file) when [buf, buf+len) is contained in the destination of a recent
 * DVDRead. The ring is deliberately tiny — the DMA follows its read
 * immediately, with no other disc traffic in between. */
int dc_dvd_provenance(const void* buf, unsigned int len,
                      int* out_entry, unsigned int* out_off);

/* Read len bytes at file offset `off` from the interned entry `entry` into
 * dst. Opens (and caches) its OWN file handle: the pager outlives the game's
 * JKRDvdFile, which is deleted when the archive unmounts. Returns bytes read,
 * or -1. */
int dc_dvd_pager_read(int entry, unsigned int off, void* dst, unsigned int len);

/* Cumulative pager disc cost, for the report line dc_aram.c prints. */
void dc_dvd_pager_stats(unsigned int* out_reads, unsigned int* out_bytes,
                        unsigned int* out_opens, unsigned int* out_usec);

/* S15-5. `out_short` = fs_read returned fewer bytes than requested and
 * dc_dvd_read_yielding CONTINUED rather than treating it as EOF; `out_fail` =
 * the read still finished short, which must be 0. ⚠️ Both are structurally
 * unreachable under Flycast's FastGDRomLoad — they only ever move on a burn.
 * dc_assetwin_report() prints them as sr=/sf=. */
void dc_dvd_short_stats(unsigned int* out_short, unsigned int* out_fail);

#ifdef __cplusplus
}
#endif

#endif /* DC_ARAM_LRU_H */
