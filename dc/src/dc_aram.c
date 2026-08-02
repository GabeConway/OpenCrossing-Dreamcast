/* dc_aram.c - the GameCube's 16 MB auxiliary RAM, on a machine that has none.
 *
 * kb/design-platform-api.md §3.4 and §4.3, PLAN §3.1. This is the single seam
 * between the game and 16 MB of memory the Dreamcast does not have, and it is
 * the ONLY place the disc-backed graph window needs implementing.
 *
 * THE CONTRACT (three behaviours callers rely on — do not "clean up"):
 *   1. ARAM addresses are OFFSETS, not pointers. ARInit returns 0,
 *      ARGetBaseAddress() returns 0, ARAlloc is a 32-byte-aligned bump
 *      allocator that never frees.
 *   2. Some callers pass (aram_base + offset) instead of a bare offset. The
 *      normalisation below detects and fixes that. Removing it breaks them.
 *   3. ARAM->MRAM reads of ARAM that has no content ZERO-FILL the destination
 *      (capped at 1 MB) rather than leaving garbage. Game code depends on
 *      getting zeros.
 *
 * ARQPostRequest has an ARGUMENT-ORDER TRAP: its (source, dest) are NOT
 * ARStartDMA's (mram, aram). For type 0 it forwards (source->mram, dest->aram);
 * for type 1 it SWAPS them.
 *
 * WHO USES IT
 *   - graph half, 6.96 MB of RARC archives: JKRAram / JKRAramArchive /
 *     JKRAramPiece. PLAN §3.1's disc-backed LRU is implemented HERE.
 *   - sound half, 8.44 MB: jaudio aramcall.c / dvdthread.c. This half DIES —
 *     samples move to AICA sound RAM / disc streaming (PLAN §3.4) and never sit
 *     in main RAM. The pager maps it for free when jaudio's writes happen to
 *     carry disc provenance, but it never spends pool bytes on it.
 *
 * ===========================================================================
 * PLAN §3.1: THE DISC-BACKED LRU  (kill switch: DC_ARAM_LRU=0)
 * ===========================================================================
 * Read dc/include/dc_aram_lru.h first — it carries the rationale and the
 * measurements. The short version:
 *
 *   Old model: ONE CONTIGUOUS resident window that re-anchored itself onto
 *   whichever region wrote last. MEASURED on the 2026-08-02 title-screen run
 *   (851,968 B window): 15 anchorings, rebases=14 — 9 of them jaudio marching
 *   through the audio half in 851,968 B strides and 5 of them forest_2nd.arc —
 *   and >=5,121 out-of-window reads, every one of which was zero-filled. It
 *   cost 851,968 B of a budget that is 5,431,104 B over and it delivered no
 *   archive content at all.
 *
 *   New model: an ARAM offset range is described by an EXTENT that maps it
 *   affinely onto a byte range of a file on /cd, learned from the write stream
 *   (dc_dvd.c's provenance ring). A read of a mapped range is served by ONE
 *   fs_read straight into the caller's destination buffer — no fixed page
 *   size, no fault granularity, and the disc read is exactly as large as the
 *   game's own request. The resident pool is now only a CACHE plus a home for
 *   the writes that have no disc provenance (m_card.c:1607's three save
 *   blocks), so it no longer has to be as large as the biggest archive.
 *
 * WHY THIS IS NOT THE MMU-PAGING IDEA THAT CAME BACK DEAD
 *   kb/research-mmu-paging.md killed 4 KB demand paging against the CD-R: one
 *   4 KB page is 8.19 ms of transfer, 24.6% of a frame, with no prefetch and
 *   no load-order knowledge. This pager never reads 4 KB. It reads whole RARC
 *   resources, in the order the game asks for them, into the buffer the game
 *   already allocated for them. The only small reads are
 *   JKRAram::aramToMainRam's 32 B compression probes (JKRAram.cpp:197), and
 *   those are turned into one 32 KB cached block so the body read that follows
 *   is sequential from where the head already is.
 *
 * CORRECTNESS BEFORE SIZE
 *   A window that drops a write is not a smaller window, it is a corrupt
 *   archive — that exact bug is why the old anchor code existed. So every byte
 *   of every MRAM->ARAM transfer lands in exactly one of four counted buckets,
 *   and `LOST` is the one that must be zero:
 *     mapped  - has an extent, re-readable from /cd, costs no pool bytes
 *     stored  - anonymous, held in a PINNED pool block
 *     audio   - in jaudio's half, which has no provider on this target at all
 *               (unchanged from before this change; PLAN §3.4 owns it)
 *     LOST    - anonymous graph-half bytes with nowhere to go. MUST BE 0.
 *   The [DC/ARAM] LRU line prints all four, plus the pager's disc cost.
 */
#include "dc_platform.h"
#include "dc_mem_ledger.h"
#include "dc_aram_lru.h"
#include "dolphin/os/OSCache.h"  /* DCStoreRangeNoSync — implemented in dc_os.c */

/* The resident pool. NOT 16 MB — see DC_ARAM_WINDOW_SIZE in dc_platform.h. */
static u8*  aram_window = NULL;
static u32  aram_window_size = 0;
static u32  aram_alloc_ptr = 0;
static int  aram_inited = 0;

/* Diagnostics kept from before: how far into the offset space the game reaches.
 * ARAlloc hands out the FULL 16 MB regardless of what is resident, exactly as
 * GameCube does, so this saturates at 16,777,216 and is only useful as a check
 * that the allocator still behaves. */
static u32  aram_high_water = 0;

/* The audio/graph boundary, learned rather than hardcoded.
 * JKRAram::JKRAram (JKRAram.cpp:50-51) does ARAlloc(audio) and then
 * ARAlloc(graph), with the sizes jsyswrap.cpp:487-489 supplies (0x810000
 * sound / 0x6A3780 graph). So the FIRST allocation is jaudio's half and
 * everything at or above its end is graph/user. Learning it from the
 * allocation sequence rather than from a constant means a build that changes
 * those sizes does not silently mis-classify; if the sequence ever changes the
 * only consequence is that audio traffic is counted as graph traffic, which is
 * loud rather than silent. */
static u32  aram_audio_end = 0;
static int  aram_alloc_seq = 0;

#if DC_ARAM_LRU

/* --- the resident cache ---------------------------------------------------
 * Fully associative over at most DC_ARAM_MAX_BLOCKS blocks; the live count is
 * (pool size / DC_ARAM_BLOCK_SIZE). A linear scan of <=64 u32s per 32 KB
 * segment is nothing next to the memcpy it guards. */
#define ARAM_BLK      DC_ARAM_BLOCK_SIZE
#define ARAM_TAG_FREE 0xFFFFFFFFu

typedef struct {
    u32 tag;      /* ARAM offset / ARAM_BLK, or ARAM_TAG_FREE */
    u32 stamp;    /* LRU clock */
    u8  pinned;   /* holds bytes with no disc backing: must never be evicted */
} dc_aram_blk;

static dc_aram_blk aram_blk[DC_ARAM_MAX_BLOCKS];
static u32 aram_nblk = 0;
static u32 aram_clock = 0;

/* --- the extent map -------------------------------------------------------
 * [lo, lo+len) of the ARAM offset space is byte-for-byte [foff, foff+len) of
 * disc entry `entry`. Writes that are contiguous in BOTH spaces coalesce, so a
 * whole archive mount collapses to one entry: forest_1st.arc's 26 transfers
 * and forest_2nd.arc's 126 each become a single extent. */
typedef struct {
    u32 lo;
    u32 len;
    u32 foff;
    s32 entry;
} dc_aram_ext;

static dc_aram_ext aram_ext[DC_ARAM_MAX_EXTENTS];
static int aram_ext_n = 0;

/* --- counters. The [DC/ARAM] LRU line is the proof, not inspection. -------- */
static u32 c_w_mapped   = 0;   /* MRAM->ARAM bytes that got an extent */
static u32 c_w_stored   = 0;   /* anonymous bytes held in a pinned block */
static u32 c_w_audio    = 0;   /* anonymous bytes in jaudio's half (no provider) */
static u32 c_w_lost     = 0;   /* MUST BE 0: anonymous graph bytes with no home */
static u32 c_r_cache    = 0;   /* read segments served from the pool */
static u32 c_r_disc     = 0;   /* read segments served by an fs_read */
static u32 c_r_zero     = 0;   /* read segments with no mapping (behaviour 3) */
static u32 c_r_zerobyte = 0;
static u32 c_evict      = 0;
static u32 c_pin_peak   = 0;
static u32 c_ext_peak   = 0;
static u32 c_ext_drop   = 0;   /* extent-table overflow: a mapping was given up */
static u32 c_ops        = 0;
static u64 c_last_report_us = 0;

static void aram_report(void) {
    unsigned int reads = 0, bytes = 0, opens = 0, usec = 0;
    dc_dvd_pager_stats(&reads, &bytes, &opens, &usec);
    DC_LOGE("[DC/ARAM] LRU w:mapped=%u stored=%u audio=%u LOST=%u | "
            "r:cache=%u disc=%u zero=%u(%u B) | pool=%u blk pin_peak=%u "
            "evict=%u ext=%u/%u drop=%u | disc %u reads %u B %u opens %u ms\n",
            (unsigned)c_w_mapped, (unsigned)c_w_stored, (unsigned)c_w_audio,
            (unsigned)c_w_lost,
            (unsigned)c_r_cache, (unsigned)c_r_disc, (unsigned)c_r_zero,
            (unsigned)c_r_zerobyte,
            (unsigned)aram_nblk, (unsigned)c_pin_peak, (unsigned)c_evict,
            (unsigned)c_ext_peak, (unsigned)DC_ARAM_MAX_EXTENTS,
            (unsigned)c_ext_drop,
            reads, bytes, opens, usec / 1000u);
}

/* --- block cache ---------------------------------------------------------- */

static int blk_find(u32 tag) {
    u32 i;
    for (i = 0; i < aram_nblk; i++)
        if (aram_blk[i].tag == tag) return (int)i;
    return -1;
}

static void blk_touch(int i) { aram_blk[i].stamp = ++aram_clock; }

/* Get a block for `tag`, evicting the least recently used UNPINNED block when
 * there is no free slot. Returns -1 when every block is pinned — the caller
 * must then COUNT the bytes as lost rather than pretend they landed. */
static int blk_alloc(u32 tag) {
    u32 i;
    int victim = -1;
    for (i = 0; i < aram_nblk; i++) {
        if (aram_blk[i].tag == ARAM_TAG_FREE) { victim = (int)i; break; }
        if (aram_blk[i].pinned) continue;
        if (victim < 0 || aram_blk[i].stamp < aram_blk[victim].stamp)
            victim = (int)i;
    }
    if (victim < 0) return -1;
    if (aram_blk[victim].tag != ARAM_TAG_FREE) c_evict++;
    aram_blk[victim].tag = tag;
    aram_blk[victim].pinned = 0;
    blk_touch(victim);
    return victim;
}

static void blk_pin_census(void) {
    u32 i, n = 0;
    for (i = 0; i < aram_nblk; i++) if (aram_blk[i].pinned) n++;
    if (n > c_pin_peak) c_pin_peak = n;
}

/* --- extent map ----------------------------------------------------------- */

static int ext_lookup(u32 addr) {
    int i;
    for (i = 0; i < aram_ext_n; i++)
        if (addr >= aram_ext[i].lo && addr < aram_ext[i].lo + aram_ext[i].len)
            return i;
    return -1;
}

/* First extent start strictly after `addr` and below `limit`, else `limit`.
 * Bounds a zero-fill so one unmapped hole cannot blank the rest of a read. */
static u32 ext_next_start(u32 addr, u32 limit) {
    int i;
    u32 best = limit;
    for (i = 0; i < aram_ext_n; i++)
        if (aram_ext[i].lo > addr && aram_ext[i].lo < best)
            best = aram_ext[i].lo;
    return best;
}

static void ext_remove(int i) {
    aram_ext[i] = aram_ext[aram_ext_n - 1];
    aram_ext_n--;
}

/* Drop any mapping over [lo, lo+len). Called before every write, because after
 * a write the old (file, offset) for that range is a LIE and serving a read
 * from it would hand the game the wrong bytes — worse than zero-filling. */
static void ext_punch(u32 lo, u32 len) {
    u32 hi = lo + len;
    int i;
    for (i = 0; i < aram_ext_n; i++) {
        u32 elo = aram_ext[i].lo;
        u32 ehi = elo + aram_ext[i].len;
        if (ehi <= lo || elo >= hi) continue;                /* no overlap */
        if (elo >= lo && ehi <= hi) { ext_remove(i); i--; continue; }
        if (elo < lo && ehi > hi) {                          /* punch a hole */
            if (aram_ext_n < DC_ARAM_MAX_EXTENTS) {
                aram_ext[aram_ext_n].lo    = hi;
                aram_ext[aram_ext_n].len   = ehi - hi;
                aram_ext[aram_ext_n].foff  = aram_ext[i].foff + (hi - elo);
                aram_ext[aram_ext_n].entry = aram_ext[i].entry;
                aram_ext_n++;
            } else {
                c_ext_drop++;   /* the tail loses its mapping; reads zero-fill */
            }
            aram_ext[i].len = lo - elo;
            continue;
        }
        if (elo < lo) { aram_ext[i].len = lo - elo; continue; }  /* trim tail */
        aram_ext[i].foff += hi - elo;                            /* trim head */
        aram_ext[i].len  -= hi - elo;
        aram_ext[i].lo    = hi;
    }
}

/* Returns 1 if [lo, lo+len) is mapped afterwards. */
static int ext_add(u32 lo, u32 len, s32 entry, u32 foff) {
    int i;
    ext_punch(lo, len);
    for (i = 0; i < aram_ext_n; i++) {
        if (aram_ext[i].entry != entry) continue;
        if (aram_ext[i].lo + aram_ext[i].len != lo) continue;
        if (aram_ext[i].foff + aram_ext[i].len != foff) continue;
        aram_ext[i].len += len;
        return 1;
    }
    if (aram_ext_n >= DC_ARAM_MAX_EXTENTS) { c_ext_drop++; return 0; }
    aram_ext[aram_ext_n].lo    = lo;
    aram_ext[aram_ext_n].len   = len;
    aram_ext[aram_ext_n].foff  = foff;
    aram_ext[aram_ext_n].entry = entry;
    aram_ext_n++;
    if ((u32)aram_ext_n > c_ext_peak) c_ext_peak = (u32)aram_ext_n;
    return 1;
}

/* --- the two halves of the pager ------------------------------------------ */

static void aram_write(u32 a, const u8* src, u32 len) {
    /* plain int/unsigned, not s32/u32: dc_aram_lru.h stays free of the
     * decomp's typedefs, and u32 is `long unsigned` on this toolchain. */
    int mapped;
    int entry = -1;
    unsigned int foff = 0;
    u32 pos;

    /* jaudio's half is not this pager's job (PLAN §3.4 kills it: samples move
     * to AICA sound RAM / disc streaming and never sit in main RAM). Bail out
     * BEFORE touching the extent table or the pool, for two reasons. The first
     * is budget: pool bytes spent here would evict archive content to hold
     * samples nothing on this target can play. The second is ORDERING, and it
     * is the one that would have bitten — jaudio streams ~8.5 MB into ARAM
     * during sound_initial, which runs BEFORE JW_Init2 mounts forest_1st.arc
     * (MEASURED, 2026-08-02 console log). Let those writes claim extent slots
     * and the table is full by the time the archives arrive, at which point
     * archive writes become anonymous, pin the pool, and get LOST. Counting
     * them and returning keeps audio exactly as unimplemented as it was. */
    if (aram_audio_end != 0 && a + len <= aram_audio_end) {
        c_w_audio += len;
        return;
    }

    mapped = dc_dvd_provenance(src, len, &entry, &foff);
    if (mapped) mapped = ext_add(a, len, (s32)entry, (u32)foff);
    if (mapped) c_w_mapped += len;
    else        ext_punch(a, len);   /* whatever this range meant, it does not now */

    /* Keep the cache coherent, and give anonymous bytes a home. A block that
     * is NOT resident and IS reloadable is deliberately left alone: allocating
     * for it would make a 4 MB archive mount churn the whole pool for nothing
     * — that is the thrash this change exists to remove. */
    for (pos = a; pos < a + len; ) {
        u32 blo = pos & ~(ARAM_BLK - 1u);
        u32 seg_end = blo + ARAM_BLK;
        u32 n;
        int i;
        if (seg_end > a + len) seg_end = a + len;
        n = seg_end - pos;

        i = blk_find(blo / ARAM_BLK);
        if (i < 0) {
            if (mapped) { pos = seg_end; continue; }
            i = blk_alloc(blo / ARAM_BLK);
            if (i < 0) {
                c_w_lost += n;
                if (c_w_lost == n)
                    DC_LOGE("[DC/ARAM] POOL FULL OF PINNED BLOCKS — %u B at "
                            "offset %u has nowhere to go. Raise "
                            "DC_ARAM_WINDOW.\n", (unsigned)n, (unsigned)pos);
                pos = seg_end;
                continue;
            }
            /* Only part of the block is being written and the rest has never
             * been written either, which behaviour 3 says must read as zeros. */
            memset(aram_window + (u32)i * ARAM_BLK, 0, ARAM_BLK);
        }
        memcpy(aram_window + (u32)i * ARAM_BLK + (pos - blo), src + (pos - a), n);
        DCStoreRangeNoSync(aram_window + (u32)i * ARAM_BLK + (pos - blo), n);
        if (!mapped) { aram_blk[i].pinned = 1; c_w_stored += n; }
        else if (n == ARAM_BLK) aram_blk[i].pinned = 0;
        blk_touch(i);
        pos = seg_end;
    }
    if (!mapped) blk_pin_census();
}

static void aram_read(u32 a, u8* dst, u32 len) {
    u32 pos = a;
    u32 end = a + len;

    while (pos < end) {
        u32 blo = pos & ~(ARAM_BLK - 1u);
        u32 seg_end = blo + ARAM_BLK;
        u32 run;
        int i, e;

        if (seg_end > end) seg_end = end;

        i = blk_find(blo / ARAM_BLK);
        if (i >= 0) {
            memcpy(dst + (pos - a), aram_window + (u32)i * ARAM_BLK + (pos - blo),
                   seg_end - pos);
            blk_touch(i);
            c_r_cache++;
            pos = seg_end;
            continue;
        }

        /* Coalesce every non-resident block ahead of us into ONE disc read.
         * This is the read-ahead the CD-R contract asks for: the game's own
         * request is already the natural unit (a whole RARC resource), so the
         * pager never issues a read smaller than it has to. */
        run = seg_end;
        while (run < end && blk_find(run / ARAM_BLK) < 0) {
            u32 nb = (run & ~(ARAM_BLK - 1u)) + ARAM_BLK;
            run = (nb > end) ? end : nb;
        }

        e = ext_lookup(pos);
        if (e < 0) {
            /* Behaviour 3: no content here, hand back zeros. */
            u32 stop = ext_next_start(pos, run);
            memset(dst + (pos - a), 0, stop - pos);
            c_r_zero++;
            c_r_zerobyte += stop - pos;
            pos = stop;
            continue;
        }

        {
            u32 elo  = aram_ext[e].lo;
            u32 ehi  = elo + aram_ext[e].len;
            u32 stop = (run < ehi) ? run : ehi;
            u32 off  = aram_ext[e].foff + (pos - elo);
            u32 n    = stop - pos;

            /* A small whole-DMA read is almost always JKRAram::aramToMainRam's
             * 32 B compression probe (JKRAram.cpp:197), immediately followed by
             * a large read at the same ARAM address. Filling a cache block for
             * it costs one 32 KB read instead of one 32 B read and leaves the
             * drive head exactly where the body read begins. */
            if (len <= ARAM_BLK) {
                int b = blk_alloc(blo / ARAM_BLK);
                if (b >= 0) {
                    u32 flo = (blo > elo) ? blo : elo;
                    u32 fhi = (blo + ARAM_BLK < ehi) ? blo + ARAM_BLK : ehi;
                    memset(aram_window + (u32)b * ARAM_BLK, 0, ARAM_BLK);
                    if (fhi > flo)
                        dc_dvd_pager_read((int)aram_ext[e].entry,
                                          aram_ext[e].foff + (flo - elo),
                                          aram_window + (u32)b * ARAM_BLK +
                                              (flo - blo),
                                          fhi - flo);
                    c_r_disc++;
                    continue;   /* the resident branch above now serves it */
                }
            }

            if (dc_dvd_pager_read((int)aram_ext[e].entry, off,
                                  dst + (pos - a), n) < 0) {
                memset(dst + (pos - a), 0, n);
                c_r_zero++;
                c_r_zerobyte += n;
            } else {
                c_r_disc++;
            }
            pos = stop;
        }
    }
    DCStoreRangeNoSync(dst, len);
}

#else  /* !DC_ARAM_LRU */

/* --- KILL SWITCH PATH: the 2026-08-02 behaviour, byte for byte -------------
 * One contiguous window that re-anchors on the first write it misses. Kept so
 * DC_ARAM_LRU=0 is a true revert and the pager can be bisected against it. Its
 * failure mode is documented at the top of this file: rebases=14 and >=5,121
 * zero-filled reads on the title-screen run. */
static u32 aram_window_base = 0;
static int aram_window_anchored = 0;
static u32 aram_rebase_attempts = 0;
static u32 aram_oob_reads = 0;
static u32 aram_oob_writes = 0;

#endif /* DC_ARAM_LRU */

u32 ARInit(u32* stack_idx_addr, u32 length) {
    (void)stack_idx_addr; (void)length;
    if (!aram_inited) {
        aram_inited = 1;
        aram_window_size = DC_ARAM_WINDOW_SIZE;
#if DC_ARAM_LRU
        /* The pool is a whole number of cache blocks. A remainder could not be
         * tagged and would simply be unreachable memory. */
        aram_window_size -= aram_window_size % DC_ARAM_BLOCK_SIZE;
        if (aram_window_size / DC_ARAM_BLOCK_SIZE > DC_ARAM_MAX_BLOCKS)
            aram_window_size = DC_ARAM_BLOCK_SIZE * (u32)DC_ARAM_MAX_BLOCKS;
#endif
        aram_window = (u8*)dc_mem_alloc(DCMEM_ARAM_GRAPH, aram_window_size, 32,
                                        "ARInit/window");
        if (aram_window) {
            memset(aram_window, 0, aram_window_size);
        } else {
            aram_window_size = 0;
            DC_LOGE("[DC/ARAM] no window: every ARAM read will zero-fill\n");
        }
        aram_alloc_ptr = 0;
#if DC_ARAM_LRU
        {
            u32 i;
            aram_nblk = aram_window_size / DC_ARAM_BLOCK_SIZE;
            for (i = 0; i < aram_nblk; i++) {
                aram_blk[i].tag = ARAM_TAG_FREE;
                aram_blk[i].stamp = 0;
                aram_blk[i].pinned = 0;
            }
        }
        DC_LOGE("[DC/ARAM] disc-backed LRU: %u B pool = %u x %u B blocks over a "
                "%u B address space (PLAN 3.1)\n",
                (unsigned)aram_window_size, (unsigned)aram_nblk,
                (unsigned)DC_ARAM_BLOCK_SIZE, (unsigned)DC_ARAM_SIZE);
#else
        DC_LOGE("[DC/ARAM] window %u B of a %u B address space "
                "(DC_ARAM_LRU=0: disc-backed paging DISABLED)\n",
                (unsigned)aram_window_size, (unsigned)DC_ARAM_SIZE);
#endif
    }
    return 0;   /* offset-based; base is always 0 */
}

u8*  dc_aram_get_base(void) { return aram_window; }
u32  ARGetBaseAddress(void) { return 0; }
u32  ARGetSize(void) { return DC_ARAM_SIZE; }
u32  ARGetInternalSize(void) { return DC_ARAM_SIZE; }
BOOL ARCheckInit(void) { return aram_inited; }

/* 32-byte-aligned bump allocator over the FULL 16 MB offset space, exactly as
 * on GameCube. Handing out offsets far beyond the resident pool is expected and
 * fine — that is what the pager is for. */
u32 ARAlloc(u32 size) {
    u32 aligned = (size + 31) & ~31u;
    u32 addr;
    if (aram_alloc_ptr + aligned > DC_ARAM_SIZE) {
        DC_LOGE("[DC/ARAM] out of ARAM address space: want %u, used %u/%u\n",
                (unsigned)size, (unsigned)aram_alloc_ptr, (unsigned)DC_ARAM_SIZE);
        return 0;
    }
    addr = aram_alloc_ptr;
    aram_alloc_ptr += aligned;
    if (aram_alloc_ptr > aram_high_water) {
        aram_high_water = aram_alloc_ptr;
        dc_mem_note(DCMEM_ARAM_GRAPH, 0);   /* peak attribution hook */
    }
    /* Allocation #1 is jaudio's half — see aram_audio_end above. */
    if (++aram_alloc_seq == 1) {
        aram_audio_end = aram_alloc_ptr;
        DC_LOGE("[DC/ARAM] audio half = [0, %u), graph/user from %u up\n",
                (unsigned)aram_audio_end, (unsigned)aram_audio_end);
    }
    return addr;
}

void ARFree(u32* addr) { (void)addr; }   /* bump allocator, never frees */

/* type 0 = MRAM->ARAM, type 1 = ARAM->MRAM. Params are always
 * (type, mram, aram) — see ARQPostRequest for the caller that disagrees. */
void ARStartDMA(u32 type, u32 mram_addr, u32 aram_addr, u32 length) {
    u32 base;

    if (!aram_inited || length == 0) return;

    /* Behaviour 2: defensive pointer normalisation. ARAM offsets are < 16 MB
     * and the pool lives at 0x8Cxxxxxx, so the two ranges cannot collide. */
    base = (u32)(uintptr_t)aram_window;
    if (aram_window && aram_addr >= base && aram_addr < base + aram_window_size)
        aram_addr -= base;

#if DC_ARAM_LRU
    if (!aram_window) {
        if (type == 1 && mram_addr != 0 && length <= 0x100000)
            memset((void*)(uintptr_t)mram_addr, 0, length);
        return;
    }

#if DC_ARAM_TRACE
    DC_LOGE("[DC/ARAM] dma t=%u aram=%u mram=%08x len=%u\n",
            (unsigned)type, (unsigned)aram_addr, (unsigned)mram_addr,
            (unsigned)length);
#endif

    if (type == 0) {
        aram_write(aram_addr, (const u8*)(uintptr_t)mram_addr, length);
    } else {
        if (mram_addr == 0) return;
        aram_read(aram_addr, (u8*)(uintptr_t)mram_addr, length);
    }

    /* Periodic, not per-op: the SCIF console is the bottleneck on hardware
     * (kb/traps.md).
     *
     * BOTH a count trigger and a TIME trigger, and the time one is not
     * optional. MEASURED 2026-08-02: a run does ~405 DMAs during the two
     * archive mounts and then only ~40 more for the whole rest of the run,
     * because JKRAramArchive::fetchResource caches fileEntry->mData and never
     * asks twice. A count-only trigger therefore prints its last line in the
     * middle of the mount and the READ counters are never seen at all — which
     * is exactly the half of the ledger that has to be checked. */
#ifndef DC_ARAM_REPORT_EVERY
#define DC_ARAM_REPORT_EVERY 64u
#endif
#ifndef DC_ARAM_REPORT_US
#define DC_ARAM_REPORT_US 2000000u
#endif
    ++c_ops;
    {
        u64 now = dc_time_us();
        if ((c_ops % DC_ARAM_REPORT_EVERY) == 1u ||
            now - c_last_report_us >= (u64)DC_ARAM_REPORT_US) {
            c_last_report_us = now;
            aram_report();
        }
    }

#else  /* ---------------- DC_ARAM_LRU=0: the old anchored window ------------ */

    if (aram_window && type == 0 &&
        (aram_addr < aram_window_base ||
         aram_addr > aram_window_base + aram_window_size - length ||
         length > aram_window_size)) {
        if (length <= aram_window_size) {
            if (aram_window_anchored) aram_rebase_attempts++;
            aram_window_base = aram_addr & ~31u;
            aram_window_anchored = 1;
            memset(aram_window, 0, aram_window_size);
            DC_LOGE("[DC/ARAM] window anchored at offset %u, covers "
                    "[%u, %u) of a %u B space (rebases=%u)\n",
                    (unsigned)aram_window_base, (unsigned)aram_window_base,
                    (unsigned)(aram_window_base + aram_window_size),
                    (unsigned)DC_ARAM_SIZE, (unsigned)aram_rebase_attempts);
        } else {
            aram_rebase_attempts++;
            DC_LOGE("[DC/ARAM] TRANSFER %u B EXCEEDS THE %u B WINDOW at offset "
                    "%u — content will be lost\n",
                    (unsigned)length, (unsigned)aram_window_size,
                    (unsigned)aram_addr);
        }
    }

    if (aram_window && aram_addr >= aram_window_base)
        aram_addr -= aram_window_base;
    else if (aram_window && aram_addr < aram_window_base)
        aram_addr = 0xFFFFFFFFu;   /* below the window: force the miss path */

    if (aram_window && length <= aram_window_size &&
        aram_addr <= aram_window_size - length) {
        if (type == 0) {
            memcpy(aram_window + aram_addr, (void*)(uintptr_t)mram_addr, length);
            DCStoreRangeNoSync(aram_window + aram_addr, length);
        } else {
            memcpy((void*)(uintptr_t)mram_addr, aram_window + aram_addr, length);
            DCStoreRangeNoSync((void*)(uintptr_t)mram_addr, length);
        }
        return;
    }

    if (type == 1) {
        aram_oob_reads++;
        if (mram_addr != 0 && length > 0 && length <= 0x100000)
            memset((void*)(uintptr_t)mram_addr, 0, length);
    } else {
        aram_oob_writes++;
    }
    DC_UNIMPLEMENTED_NOTE("graph-ARAM disc paging: map ARAM offsets to "
                          "forest_{1st,2nd}.arc ranges + LRU window (PLAN 3.1)");
    if ((aram_oob_reads + aram_oob_writes) % 256 == 1) {
        DC_LOG("[DC/ARAM] out-of-window traffic: %u reads, %u writes, window "
               "[%u, %u), highest offset allocated %u\n",
               (unsigned)aram_oob_reads, (unsigned)aram_oob_writes,
               (unsigned)aram_window_base,
               (unsigned)(aram_window_base + aram_window_size),
               (unsigned)aram_high_water);
    }
#endif
}

void ARQInit(void) { }

/* ARGUMENT-ORDER TRAP (§3.4): (source, dest) here are not ARStartDMA's
 * (mram, aram). The completion callback fires SYNCHRONOUSLY with the request
 * pointer cast to u32 — every "async" ARAM path in the game therefore
 * completes before the caller's next statement. */
void ARQPostRequest(void* req, u32 owner, u32 type, u32 prio,
                    u32 source, u32 dest, u32 length, void* callback) {
    (void)owner; (void)prio;
    if (type == 0) {
        ARStartDMA(type, source, dest, length);   /* source=mram, dest=aram */
    } else {
        ARStartDMA(type, dest, source, length);   /* SWAPPED */
    }
    if (callback) ((void (*)(u32))callback)((u32)(uintptr_t)req);
}

void ARQFlushQueue(void) { }
