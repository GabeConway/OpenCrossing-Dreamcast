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
 *   3. Out-of-range ARAM->MRAM reads ZERO-FILL the destination (capped at
 *      1 MB) rather than leaving garbage. Game code depends on getting zeros.
 *
 * ARQPostRequest has an ARGUMENT-ORDER TRAP: its (source, dest) are NOT
 * ARStartDMA's (mram, aram). For type 0 it forwards (source->mram, dest->aram);
 * for type 1 it SWAPS them.
 *
 * WHO USES IT
 *   - graph half, 6.96 MB of RARC archives: JKRAram / JKRAramArchive /
 *     JKRAramPiece. PLAN §3.1 turns this into a disc-backed LRU window; that
 *     window is implemented HERE and nowhere else.
 *   - sound half, 8.44 MB: jaudio aramcall.c. This half DIES — samples move to
 *     AICA sound RAM / disc streaming (PLAN §3.4) and never sit in main RAM.
 */
#include "dc_platform.h"
#include "dc_mem_ledger.h"
#include "dolphin/os/OSCache.h"  /* DCStoreRangeNoSync — implemented in dc_os.c */

/* The resident window. NOT 16 MB — see DC_ARAM_WINDOW_SIZE in dc_platform.h
 * (mem-budget bucket 8, 512,000 B, UNMEASURED: probe 3 at M1). */
static u8*  aram_window = NULL;
static u32  aram_window_size = 0;
static u32  aram_alloc_ptr = 0;
static int  aram_inited = 0;

/* The ARAM offset the window currently covers: it maps
 * [aram_window_base, aram_window_base + aram_window_size).
 *
 * It is NOT 0. JKRAram::JKRAram allocates the sound half first
 * (jsyswrap.cpp:487, 0x810000 = 8,454,144 B) and the graph half second
 * (0x6A3780), so every RARC archive lives at offset >= 8,454,144. A window
 * anchored at 0 sits entirely inside jaudio's region and misses every archive
 * access — MEASURED on the first boot with real disc content, where
 * forest_1st.arc mounted, wrote 851,744 B, and had all 257 writes silently
 * dropped as out-of-window.
 *
 * BRING-UP BEHAVIOUR, not the final design: the window anchors itself to the
 * first out-of-window WRITE, aligned down to 32. That is enough to hold the
 * boot archive and get the game running, and it is honest about what it is —
 * PLAN §3.1's disc-backed LRU over forest_{1st,2nd}.arc is still owed, and
 * jaudio's half still has no provider at all. When a second region starts
 * competing for the window, this will thrash; the counters below are what will
 * show it. */
static u32  aram_window_base = 0;
static int  aram_window_anchored = 0;
static u32  aram_rebase_attempts = 0;

/* Diagnostics: how far outside the window the game actually reaches. This is
 * the number that sizes bucket 8 for real. */
static u32  aram_high_water = 0;
static u32  aram_oob_reads = 0;
static u32  aram_oob_writes = 0;

u32 ARInit(u32* stack_idx_addr, u32 length) {
    (void)stack_idx_addr; (void)length;
    if (!aram_inited) {
        aram_inited = 1;
        aram_window_size = DC_ARAM_WINDOW_SIZE;
        aram_window = (u8*)dc_mem_alloc(DCMEM_ARAM_GRAPH, aram_window_size, 32,
                                        "ARInit/window");
        if (aram_window) {
            memset(aram_window, 0, aram_window_size);
        } else {
            aram_window_size = 0;
            DC_LOGE("[DC/ARAM] no window: every ARAM read will zero-fill\n");
        }
        aram_alloc_ptr = 0;
        DC_LOGE("[DC/ARAM] window %u B of a %u B address space "
                "(disc-backed LRU paging is NOT implemented yet)\n",
                (unsigned)aram_window_size, (unsigned)DC_ARAM_SIZE);
    }
    return 0;   /* offset-based; base is always 0 */
}

u8*  dc_aram_get_base(void) { return aram_window; }
u32  ARGetBaseAddress(void) { return 0; }
u32  ARGetSize(void) { return DC_ARAM_SIZE; }
u32  ARGetInternalSize(void) { return DC_ARAM_SIZE; }
BOOL ARCheckInit(void) { return aram_inited; }

/* 32-byte-aligned bump allocator over the FULL 16 MB offset space, exactly as
 * on GameCube. Handing out offsets beyond the resident window is expected and
 * fine — that is what the paging layer is for. The high-water mark is the
 * measurement PLAN §3.1 needs. */
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
    return addr;
}

void ARFree(u32* addr) { (void)addr; }   /* bump allocator, never frees */

/* type 0 = MRAM->ARAM, type 1 = ARAM->MRAM. Params are always
 * (type, mram, aram) — see ARQPostRequest for the caller that disagrees. */
void ARStartDMA(u32 type, u32 mram_addr, u32 aram_addr, u32 length) {
    u32 base;

    if (!aram_inited) return;

    /* Behaviour 2: defensive pointer normalisation. */
    base = (u32)(uintptr_t)aram_window;
    if (aram_window && aram_addr >= base && aram_addr < base + aram_window_size)
        aram_addr -= base;

    /* Anchor the window on the first write. Until this happens the window
     * covers [0, size), which is inside jaudio's half of the address space and
     * therefore misses every archive. Anchoring on a WRITE and not a read is
     * deliberate: a read of never-written ARAM should keep zero-filling
     * (behaviour 3), and re-anchoring there would hand back a window full of
     * unrelated bytes instead of the zeros callers rely on. */
    if (aram_window && type == 0 &&
        (aram_addr < aram_window_base ||
         aram_addr > aram_window_base + aram_window_size - length ||
         length > aram_window_size)) {
        if (length <= aram_window_size) {
            /* Re-anchor on EVERY missed write, not just the first. The first
             * write is a small one from elsewhere in the address space, and
             * anchoring on it strands the archive that arrives later. Whatever
             * the old window held was going to be lost anyway — an
             * out-of-window write is dropped on the floor today — so moving it
             * strictly increases the number of bytes that survive.
             * aram_rebase_attempts is the thrash counter: if it climbs during
             * gameplay, that is the signal that the real LRU (PLAN §3.1) can
             * no longer be deferred. */
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
            /* A single transfer bigger than the whole window. Nothing to do
             * but report it — this is the number that sizes bucket 8. */
            aram_rebase_attempts++;
            DC_LOGE("[DC/ARAM] TRANSFER %u B EXCEEDS THE %u B WINDOW at offset "
                    "%u — content will be lost\n",
                    (unsigned)length, (unsigned)aram_window_size,
                    (unsigned)aram_addr);
        }
    }

    /* Window-relative from here on. */
    if (aram_window && aram_addr >= aram_window_base)
        aram_addr -= aram_window_base;
    else if (aram_window && aram_addr < aram_window_base)
        aram_addr = 0xFFFFFFFFu;   /* below the window: force the miss path */

    /* Inside the resident window: a plain copy, exactly like GameCube DMA.
     * Cache maintenance matters here on SH-4 — the destination is read by the
     * CPU immediately after, and JKRAramPiece.cpp:97 issues its own
     * DCInvalidateRange on completion, so we only need the writeback side. */
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

    /* Outside the window. THIS is where PLAN §3.1's disc-backed LRU paging
     * goes: map (aram_addr, length) to a byte range inside forest_1st.arc /
     * forest_2nd.arc laid out on the outer tracks, read it through the
     * dc_dvd.c read-ahead layer, and evict by LRU.
     *
     * Until then, behaviour 3 keeps the game alive: zero-fill the destination
     * so callers get zeros, not garbage. Expect missing/blank archive content
     * rather than a crash. */
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
