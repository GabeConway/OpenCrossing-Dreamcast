/* dc_os.c - Dolphin OS replacement for Dreamcast/KallistiOS.
 * Arena, timers, calendar, cache maintenance, address translation, threads,
 * message queues, alarms, reset.
 *
 * Ported from pc/src/pc_os.c. Everything that was pure arithmetic there is
 * copied verbatim (it is Dolphin's own OSTime.c math). Everything that touched
 * SDL, mmap or the host clock is rewritten against KOS.
 *
 * THREE THINGS IN HERE ARE LANDMINES (kb/design-platform-api.md §4):
 *   1. OSPhysicalToCached(0) + 0x28 is the JKRHeap memory-size word. Get the
 *      mapping wrong and every heap in the game is created with a garbage size,
 *      at boot, silently.                                              (§3.1)
 *   2. The cache ops below are NO-OPS on x86/ARM and REAL on SH-4. 66 call
 *      sites, several of them on buffers the PVR and AICA read directly.
 *      Invalidate is line-granular and DISCARDS dirty lines.           (§3.2)
 *   3. The tick rate is 40.5 MHz and must not change. Only the source does.
 *                                                                      (§3.3)
 */
#include "dc_platform.h"
#include "dc_mem_ledger.h"
#include "dc_mem_budget.h"

/* The arena block is bucket 6. If these ever disagree the ledger lies. */
#if DC_MAIN_MEMORY_SIZE != DC_BUDGET_JKRHEAP
#error "DC_MAIN_MEMORY_SIZE must equal DC_BUDGET_JKRHEAP (mem-budget bucket 6)"
#endif

/* ==========================================================================
 * Memory arena
 * ========================================================================== */
static u8* arena_memory = NULL;
static u8* arena_lo = NULL;
static u8* arena_hi = NULL;

/* Exported for the seg2k0 pointer heuristic (PLAN §11.6). emu64 uses these to
 * tell a real pointer from an N64 segment address. On DC the arena lives
 * wherever KOS's allocator put it inside 0x8C000000+, which does NOT collide
 * with the N64 0x00-0x0F segment range the way a low mmap would — but the
 * heuristic still needs the bounds. */
u8* pc_arena_base = NULL;
u8* pc_arena_end  = NULL;

/* ==========================================================================
 * Arena high-water probe (kb/STATE.md N4)
 * ==========================================================================
 * The arena and KOS's sbrk heap are carved from the SAME region, so every byte
 * given to the arena is a byte libc can never hand out — and libc is the pool
 * that actually ran dry. Bucket 6's real peak has never been measured, so
 * DC_MAIN_MEMORY_SIZE is a guess defended only by "no arena-side OOM has been
 * seen". Bisecting it costs one full build per data point; this costs one.
 *
 * OSInit() memsets the whole arena to zero, so a nonzero byte means something
 * wrote there. Scanning at 1 KB granularity and keeping the running maximum
 * gives the touched high-water for free.
 *
 * WHAT THIS IS NOT: an allocator census. A block that is allocated and then
 * only ever written with zeroes reads as untouched, and freed memory keeps
 * counting as touched. So `peak` is a LOWER bound on the arena's high-water
 * and an UPPER bound on what can safely be handed back to libc only if it is
 * treated with margin. Report it as measured, not as a licence.
 *
 * brk is reported on the same line because the two pools are one inequality:
 * the interesting number is arena slack against libc's peak demand, not either
 * alone. */
#ifndef DC_ARENA_PROBE
#define DC_ARENA_PROBE 0
#endif

#if DC_ARENA_PROBE > 0
#include <unistd.h>   /* sbrk — the libc side of the two-pool inequality */

/* src/game/m_malloc.c. Declared here rather than through the decomp headers:
 * this file is platform code and must not start depending on game headers for
 * a probe that is normally compiled out. */
extern int  zelda_MallocIsInitalized(void);
extern void zelda_GetFreeArena(size_t* max_free, size_t* free_total,
                               size_t* used);

void dc_arena_probe(void) {
    static u32 peak_granules = 0;
    static u32 probe_no = 0;
    static u8* brk_base = NULL;
    const u32 granule = 1024u;
    u32 n = DC_MAIN_MEMORY_SIZE / granule;
    u32 touched = 0, top = 0, i;
    u8* brk_now;

    if (!arena_memory) return;

    for (i = 0; i < n; i++) {
        const u32* p = (const u32*)(arena_memory + i * granule);
        const u32* e = p + granule / sizeof(u32);
        for (; p < e; p++) {
            if (*p) { touched++; top = i + 1; break; }
        }
    }
    if (touched > peak_granules) peak_granules = touched;

    brk_now = (u8*)sbrk(0);
    if (!brk_base) brk_base = brk_now;

    DC_LOGE("[DC/ARENA] probe=%u touched=%u peak=%u top=%u of %u B "
            "| lo_used=%u brk=%p brk_used=%u\n",
            probe_no++, (unsigned)(touched * granule),
            (unsigned)(peak_granules * granule), (unsigned)(top * granule),
            (unsigned)DC_MAIN_MEMORY_SIZE,
            (unsigned)(arena_lo - arena_memory - DC_ARENA_LOW_RESERVE),
            (void*)brk_now, (unsigned)(brk_now - brk_base));

    /* The touched scan cannot see an allocation that only ever had zeroes
     * written into it, and the game zeroes a lot of what it allocates. The
     * decomp's own allocator does know, so ask it: __osGetFreeArena via
     * m_malloc.c reports used / free / largest-free across the zelda arena.
     * These two numbers disagreeing is informative — used >> touched means
     * the arena is full of zero-filled blocks, which are real occupancy. */
    if (zelda_MallocIsInitalized()) {
        size_t max_free = 0, free_total = 0, used = 0;
        zelda_GetFreeArena(&max_free, &free_total, &used);
        DC_LOGE("[DC/ARENA] zelda used=%u free=%u largest_free=%u\n",
                (unsigned)used, (unsigned)free_total, (unsigned)max_free);
    }
}
#else
void dc_arena_probe(void) { }
#endif

void* OSGetArenaLo(void) { return arena_lo; }
void* OSGetArenaHi(void) { return arena_hi; }
void  OSSetArenaLo(void* lo) { arena_lo = (u8*)lo; }
void  OSSetArenaHi(void* hi) { arena_hi = (u8*)hi; }

/* ==========================================================================
 * Time
 * ========================================================================== */
u32 pc_OSBusClock  = GC_BUS_CLOCK;
u32 pc_OSCoreClock = GC_CORE_CLOCK;

static u64 time_base_start_us = 0;      /* monotonic base, captured lazily  */
static s64 gc_epoch_offset_ticks = 0;   /* ticks from GC epoch to that base */
static int time_inited = 0;

/* Free-running microseconds. Safe to call before OSInit(): boot.c's very first
 * statement is InitialStartTime = osGetTime(), one line ahead of OSInit()
 * (§1 ordering rule 6). */
u64 dc_time_us(void) {
#ifdef DC_HOST_STUB
    return 0;
#else
    /* VERIFY (M0): KOS arch/timer.h. timer_us_gettime64() exists in KOS 2.x;
     * older trees only have timer_ms_gettime64(). If this does not resolve,
     * fall back to timer_ms_gettime64() * 1000 and accept 1 ms granularity —
     * the game only needs monotonicity plus the 40.5 MHz scaling below. */
    return (u64)timer_us_gettime64();
#endif
}

static void dc_time_init_base(void) {
    if (time_inited) return;
    time_inited = 1;
    time_base_start_us = dc_time_us();
    gc_epoch_offset_ticks = 0;
}

s64 osGetTime(void) {
    u64 now_us, diff_us;
    s64 elapsed;

    if (!time_inited) dc_time_init_base();

    now_us  = dc_time_us();
    diff_us = now_us - time_base_start_us;

    /* Split whole seconds from the remainder before scaling to GC ticks.
     * pc_os.c does this because diff * GC_TIMER_CLOCK overflows u64 after
     * ~455 s at a 1 GHz counter; at 1 MHz we would survive far longer, but
     * KEEP THE SPLIT — it costs nothing and it is the shape the base port
     * validated over a full playthrough. */
    elapsed = (s64)((diff_us / 1000000ull) * (u64)GC_TIMER_CLOCK
                  + (diff_us % 1000000ull) * (u64)GC_TIMER_CLOCK / 1000000ull);
    return gc_epoch_offset_ticks + elapsed;
}

s64 OSGetTime(void) { return osGetTime(); }
s64 __OSGetSystemTime(void) { return OSGetTime(); }

/* Low 32 bits. The game does OSDiffTick((s32)a - (s32)b), which wraps every
 * ~106 s at 40.5 MHz. Anything measuring longer than that is already broken on
 * real GameCube hardware too. */
u32 osGetCount(void) { return (u32)osGetTime(); }
u32 OSGetTick(void)  { return osGetCount(); }

/* --- Calendar time: ported verbatim from Dolphin's OSTime.c via pc_os.c.
 * Pure integer math, no platform dependency. Do not touch. --------------- */
#define OS_TIMER_CLOCK          GC_TIMER_CLOCK
#define OSSecondsToTicks(sec)   ((s64)(sec) * (s64)OS_TIMER_CLOCK)
#define OSTicksToSeconds(ticks) ((s64)(ticks) / (s64)OS_TIMER_CLOCK)
#define OSMillisecondsToTicks(msec)  ((s64)(msec) * (s64)(OS_TIMER_CLOCK / 1000))
#define OSTicksToMilliseconds(ticks) ((s64)(ticks) / (s64)(OS_TIMER_CLOCK / 1000))
#define OSMicrosecondsToTicks(usec)  (((s64)(usec) * (s64)(OS_TIMER_CLOCK / 125000)) / 8)
#define OSTicksToMicroseconds(ticks) (((s64)(ticks) * 8) / (s64)(OS_TIMER_CLOCK / 125000))

#define MONTH_MAX    12
#define WEEK_DAY_MAX 7
#define YEAR_DAY_MAX 365
#define USEC_MAX     1000
#define MSEC_MAX     1000
#define SECS_IN_MIN  60
#define SECS_IN_HOUR (60 * 60)
#define SECS_IN_DAY  (60 * 60 * 24)
#define SECS_IN_YEAR (SECS_IN_DAY * 365)
#define BIAS         0xB2575   /* days from March 1 year 0 to 2000-01-01 */

static int YearDays_os[MONTH_MAX]
    = { 0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334 };
static int LeapYearDays_os[MONTH_MAX]
    = { 0, 31, 60, 91, 121, 152, 182, 213, 244, 274, 305, 335 };

static int IsLeapYear(int year) {
    return (year % 4 == 0 && year % 100 != 0) || (year % 400 == 0);
}

static int GetYearDays(int year, int mon) {
    int* md = IsLeapYear(year) ? LeapYearDays_os : YearDays_os;
    return md[mon];
}

static int GetLeapDays(int year) {
    if (year < 1) return 0;
    return (year + 3) / 4 - (year - 1) / 100 + (year - 1) / 400;
}

static void GetDates(int days, void* td_ptr) {
    int* td = (int*)td_ptr;
    int year, n, month;
    int* md;

    td[6] = (days + 6) % WEEK_DAY_MAX;

    for (year = days / YEAR_DAY_MAX;
         days < (n = year * YEAR_DAY_MAX + GetLeapDays(year)); year--)
        ;

    days -= n;
    td[5] = year;
    td[7] = days;

    md = IsLeapYear(year) ? LeapYearDays_os : YearDays_os;
    for (month = MONTH_MAX; days < md[--month]; )
        ;
    td[4] = month;
    td[3] = days - md[month] + 1;
}

void OSTicksToCalendarTime(s64 ticks, void* td_ptr) {
    int* td = (int*)td_ptr;
    int days, secs;
    s64 d;

    d = ticks % OSSecondsToTicks(1);
    if (d < 0) d += OSSecondsToTicks(1);

    td[9] = (int)(OSTicksToMicroseconds(d) % USEC_MAX);
    td[8] = (int)(OSTicksToMilliseconds(d) % MSEC_MAX);

    ticks -= d;

    days = (int)(OSTicksToSeconds(ticks) / SECS_IN_DAY) + BIAS;
    secs = (int)(OSTicksToSeconds(ticks) % SECS_IN_DAY);
    if (secs < 0) { days -= 1; secs += SECS_IN_DAY; }

    GetDates(days, td);
    td[2] = secs / 60 / 60;
    td[1] = secs / 60 % 60;
    td[0] = secs % 60;
}

s64 OSCalendarTimeToTicks(void* td_ptr) {
    int* td = (int*)td_ptr;
    s64 secs;
    int ov_mon, mon, year;

    ov_mon = td[4] / MONTH_MAX;
    mon = td[4] - (ov_mon * MONTH_MAX);
    if (mon < 0) { mon += MONTH_MAX; ov_mon--; }

    year = td[5] + ov_mon;

    secs = (s64)SECS_IN_YEAR * year +
           (s64)SECS_IN_DAY * (GetLeapDays(year) + GetYearDays(year, mon) + td[3] - 1) +
           (s64)SECS_IN_HOUR * td[2] +
           (s64)SECS_IN_MIN * td[1] +
           td[0] -
           (s64)0xEB1E1BF80ULL;

    return OSSecondsToTicks(secs) + OSMillisecondsToTicks((s64)td[8]) +
           OSMicrosecondsToTicks((s64)td[9]);
}

void OSGetSaveRegion(void** start, void** end_) { *start = NULL; *end_ = NULL; }

/* ==========================================================================
 * Threads (the port is single-threaded and the game knows — §3.7)
 * ========================================================================== */
typedef struct OSThread_DC {
    void* stackBase;
    void* stackEnd;
    int   priority;
    u8    _pad[512];
} OSThread_DC;

/* boot.c DEREFERENCES the result of OSGetCurrentThread() to paint the stack
 * with 0xFD between stackBase and stackEnd. Both are NULL here, exactly as on
 * PC, so the paint loop is a no-op instead of scribbling over KOS. */
static OSThread_DC main_thread;
static OSThread_DC* current_thread = &main_thread;

void OSInitMessageQueue(void* queue, void** msgArray, int msgCount);
BOOL OSSendMessage(void* queue, void* msg, int flags);
BOOL OSReceiveMessage(void* queue, void** msgPtr, int flags);

void osCreateMesgQueue(void* mq, void* buf, int count) {
    OSInitMessageQueue(mq, (void**)buf, count);
}
int osSendMesg(void* mq, void* msg, int flags) {
    return OSSendMessage(mq, msg, flags) ? 0 : -1;
}
int osRecvMesg(void* mq, void** msg, int flags) {
    return OSReceiveMessage(mq, msg, flags) ? 0 : -1;
}

static void (*pending_thread_entry)(void*) = NULL;
static void*  pending_thread_arg = NULL;

void osCreateThread2(void* thread, int id, void (*entry)(void*), void* arg,
                     void* stack, int stack_size, int priority) {
    (void)thread; (void)id; (void)stack; (void)stack_size; (void)priority;
    pending_thread_entry = entry;
    pending_thread_arg = arg;
}

void osStartThread(void* thread) {
    /* Deliberately does not start anything. src/static/jaudio_NES/internal/
     * audiothread.c's TARGET_PC path drives audio from the main loop instead. */
    (void)thread;
    pending_thread_entry = NULL;
    pending_thread_arg = NULL;
}

void osSetThreadPri(void* thread, int pri) { (void)thread; (void)pri; }
void* OSGetCurrentThread(void) { return current_thread; }
void* OSGetStackPointer(void) { return &arena_memory; }

/* --- Interrupts ------------------------------------------------------------
 * DEFAULT: PC semantics (no-ops returning 0). The port is single-threaded, so
 * disabling interrupts buys nothing and risks holding them across a blocking
 * KOS call. The moment a real KOS audio thread lands (PLAN §3.4 stage A),
 * define DC_REAL_IRQ and these become irq_disable()/irq_restore() — otherwise
 * the jaudio message queues race (§3.7). */
BOOL OSDisableInterrupts(void) {
#ifdef DC_REAL_IRQ
    return (BOOL)irq_disable();
#else
    return 0;
#endif
}
BOOL OSEnableInterrupts(void) {
#ifdef DC_REAL_IRQ
    irq_enable();
#endif
    return 0;
}
BOOL OSRestoreInterrupts(BOOL level) {
#ifdef DC_REAL_IRQ
    irq_restore((irq_mask_t)level);
#endif
    return level;
}

/* ==========================================================================
 * Cache maintenance — REAL on SH-4 (§3.2)
 * ==========================================================================
 * KOS arch/cache.h mapping (VERIFY at M0 — these three names and their
 * (start, count) signature are the single most important API assumption in
 * this file):
 *     dcache_flush_range  -> writeback           (OCBWB)
 *     dcache_inval_range  -> invalidate          (OCBI)  DISCARDS DIRTY LINES
 *     dcache_purge_range  -> writeback+invalidate(OCBP)
 *     icache_flush_range  -> instruction cache
 */
#ifdef DC_HOST_STUB
#define DC_DCACHE_WB(a, l)    ((void)(a), (void)(l))
#define DC_DCACHE_INV(a, l)   ((void)(a), (void)(l))
#define DC_DCACHE_PURGE(a, l) ((void)(a), (void)(l))
#define DC_ICACHE_INV(a, l)   ((void)(a), (void)(l))
#else
#define DC_DCACHE_WB(a, l)    dcache_flush_range((uintptr_t)(a), (size_t)(l))
#define DC_DCACHE_INV(a, l)   dcache_inval_range((uintptr_t)(a), (size_t)(l))
#define DC_DCACHE_PURGE(a, l) dc_dcache_purge_range((uintptr_t)(a), (size_t)(l))
#define DC_ICACHE_INV(a, l)   icache_flush_range((uintptr_t)(a), (size_t)(l))

/* Hand-rolled purge — DO NOT go back to KOS's dcache_purge_range().
 *
 * arch/cache.h:208-221 in the SDK image sends any range >= 39,936 B into
 * arch_dcache_purge_all(), and that inline (arch/cache.h:187-206) carries
 *
 *     alignas(32) static char buffer[ARCH_CACHE_L1_DCACHE_SIZE];   // 16,384 B
 *
 * on the `if(__is_defined(__OPTIMIZE_SIZE__)) … else …` else-branch. We build
 * at -O0, so __OPTIMIZE_SIZE__ is never defined and the else-branch is always
 * the one compiled. The buffer landed in .bss as `.bss.buffer.4` at 0x8d469000,
 * 0x4000 B, in dc/src/dc_os.c.o — this was the ONLY translation unit in the
 * whole 3917-TU image that reached it (verified in AnimalCrossing.map).
 * Calling arch_dcache_purge_line() directly never instantiates purge_all, so
 * the 16,384 B disappears. `ocbp` is writeback + invalidate for one line, which
 * is exactly what purge_range promises.
 *
 * The tradeoff, stated honestly: above KOS's threshold this is now a per-line
 * loop rather than a whole-cache sweep, so a very large range issues more ocbp
 * than the 512 lines the 16 KB dcache can actually hold. It is ~1 cycle each
 * and every DCFlushRange* call site in the tree is an audio buffer, a card
 * block or a JUTDirectPrint span — all well under 39,936 B — so the fast path
 * was never being taken anyway. The alternative (KOS's __OPTIMIZE_SIZE__ path,
 * writing zeroes through the memory-mapped dcache address array at 0xF4000008)
 * is untested on our hardware and was deliberately not adopted here. */
static void dc_dcache_purge_range(uintptr_t start, size_t count) {
    uintptr_t end = start + count;
    start &= ~(uintptr_t)0x1f;
    for (; start < end; start += 32)
        arch_dcache_purge_line((void*)start);
}
#endif

/* Bring-up guard: SH-4 invalidate is line-granular (32 B) and will drop
 * neighbouring dirty data if the range is not aligned. GameCube's
 * DCInvalidateRange had the same constraint and the game's call sites are
 * BELIEVED aligned — but this is exactly the class of bug that shows up as
 * "the audio buffer has one wrong sample every N frames". Warn once per site.
 * Compile with DC_NO_CACHE_ALIGN_CHECK to remove the check entirely. */
#ifndef DC_NO_CACHE_ALIGN_CHECK
static void dc_cache_align_warn(const char* who, void* addr, u32 len) {
    if ((((uintptr_t)addr) & 31u) || (len & 31u)) {
        static int warned = 0;
        if (warned < 16) {
            warned++;
            DC_LOGE("[DC/CACHE] %s unaligned addr=%p len=%u "
                    "(SH-4 invalidate is 32B-granular and discards dirty lines)\n",
                    who, addr, (unsigned)len);
        }
    }
}
#else
#define dc_cache_align_warn(w, a, l) ((void)0)
#endif

/* 12 call sites: m_card.c, jaudio dspinterface/fxinterface, JUTDirectPrint. */
void DCFlushRange(void* addr, u32 len)       { DC_DCACHE_PURGE(addr, len); }
/* 10 call sites, jaudio. Same as the above minus the trailing sync. */
void DCFlushRangeNoSync(void* addr, u32 len) { DC_DCACHE_PURGE(addr, len); }
/* 10 call sites. emu64.c:965 (decoded texture) and emu64.c:1004 (TLUT) are the
 * game telling the platform "these bytes are ready" — the correct hook for the
 * VRAM upload. aictrl.c DAC buffers, cpubuf, dspbuf are the rest. */
void DCStoreRange(void* addr, u32 len)       { DC_DCACHE_WB(addr, len); }
/* 25 call sites — the most-used cache op in the codebase. jaudio + JKRAram. */
void DCStoreRangeNoSync(void* addr, u32 len) { DC_DCACHE_WB(addr, len); }
/* 9 call sites: JKRAramPiece DMA completion, dspinterface, JFWDisplay XFB. */
void DCInvalidateRange(void* addr, u32 len) {
    dc_cache_align_warn("DCInvalidateRange", addr, len);
    DC_DCACHE_INV(addr, len);
}
/* 6 call sites; a prefetch hint, correct as a no-op. */
void DCTouchRange(void* addr, u32 len) { (void)addr; (void)len; }
/* The real op allocates cache lines without reading memory; memset is a
 * correct (slower) substitute. 1 call site. */
void DCZeroRange(void* addr, u32 len) { memset(addr, 0, len); }

void ICFlashInvalidate(void) { }
void ICInvalidateRange(void* addr, u32 len) { DC_ICACHE_INV(addr, len); }
/* GameCube locked-cache. 0 call sites in compiled code; kept so nothing that
 * still references them fails to link. */
void LCEnable(void) { }
void LCDisable(void) { }

/* ==========================================================================
 * Address translation (§3.1 option A — the recommended one)
 * ==========================================================================
 * "Physical 0" IS the base of our arena, exactly as on PC. That keeps
 *   JKRHeap::initArena: mMemorySize = *(u32*)(OSPhysicalToCached(0) + 0x28)
 * inside memory we own, keeps OSCachedToPhysical a plain subtraction (which
 * emu64's pointer plumbing depends on), and costs 44 bytes.
 *
 * Cached<->uncached is a REAL operation on SH-4, unlike PC where it was the
 * identity: P1 (0x8Cxxxxxx, cached) <-> P2 (0xACxxxxxx, uncached). Any buffer
 * handed to PVR/AICA DMA must use the P2 alias OR be flushed. */
#define DC_P1_TO_P2(p) ((void*)(((uintptr_t)(p)) | 0x20000000u))
#define DC_P2_TO_P1(p) ((void*)(((uintptr_t)(p)) & ~0x20000000u))

void* OSPhysicalToCached(u32 paddr)   { return (void*)(arena_memory + paddr); }
void* OSPhysicalToUncached(u32 paddr) { return DC_P1_TO_P2(arena_memory + paddr); }
u32   OSCachedToPhysical(void* caddr) {
    return (u32)((u8*)DC_P2_TO_P1(caddr) - arena_memory);
}
u32   OSUncachedToPhysical(void* ucaddr) {
    return (u32)((u8*)DC_P2_TO_P1(ucaddr) - arena_memory);
}
void* OSCachedToUncached(void* caddr)   { return DC_P1_TO_P2(caddr); }
void* OSUncachedToCached(void* ucaddr)  { return DC_P2_TO_P1(ucaddr); }

s32 osAppNMIBuffer[64] = { 0 };

/* ==========================================================================
 * Init
 * ========================================================================== */
void OSInit(void) {
    if (!arena_memory) {
        arena_memory = (u8*)dc_mem_alloc(DCMEM_JKRHEAP, DC_MAIN_MEMORY_SIZE, 32,
                                         "OSInit/arena");
        if (!arena_memory) {
            DC_LOGE("[DC/OS] FATAL: could not reserve the %u B arena\n",
                    (unsigned)DC_MAIN_MEMORY_SIZE);
            dc_mem_report(1);
#ifndef DC_HOST_STUB
            arch_exit();
#else
            exit(1);
#endif
            return;
        }
        memset(arena_memory, 0, DC_MAIN_MEMORY_SIZE);

        pc_arena_base = arena_memory;
        pc_arena_end  = arena_memory + DC_MAIN_MEMORY_SIZE;

        /* THE 0x28 WORD (§3.1). Every JKRHeap in the game inherits
         * mMemorySize from here; JUTProcBar's heap bar reads it. It must agree
         * with OSGetPhysicalMemSize(). */
        *(u32*)(arena_memory + 0x28) = DC_MAIN_MEMORY_SIZE;

        arena_lo = arena_memory + DC_ARENA_LOW_RESERVE;
        arena_hi = arena_memory + DC_MAIN_MEMORY_SIZE;

        DC_LOGE("[DC/OS] Arena %p - %p (%u KB, system heap %u KB)\n",
                (void*)arena_memory, (void*)arena_hi,
                (unsigned)(DC_MAIN_MEMORY_SIZE / 1024),
                (unsigned)(DC_SYSTEM_HEAP_SIZE / 1024));
        /* boot.c's adjustOSArena() bzero()s this entire span on every cold
         * boot. At 4 MB that is a measurable but acceptable memset; at the
         * base port's 24 MB it would not be. */
    }

    dc_time_init_base();

    /* Wall clock. The Dreamcast has a battery-backed RTC; KOS converts its
     * 1950-01-01 epoch to Unix seconds for us.
     * VERIFY (M0): rtc_unix_secs() name/header (arch/rtc.h).
     * NOTE: unlike the PC port there is no timezone fold-in here — the DC RTC
     * is set to LOCAL time by the BIOS, which is what Animal Crossing's
     * calendar/event system wants. If the in-game clock is off by the UTC
     * offset on hardware, this is the line to look at. */
#ifndef DC_HOST_STUB
    {
        s64 unix_now = (s64)rtc_unix_secs();
        s64 gc_secs;

        if (g_pc_time_override >= 0) {
            /* Same override the PC build exposes on the command line; on DC it
             * is a settings/compile-time value used by the harness. */
            s64 day = unix_now - (unix_now % 86400);
            s64 h = g_pc_time_override;
            s64 m = (g_pc_min_override >= 0) ? g_pc_min_override : 0;
            s64 s = (g_pc_sec_override >= 0) ? g_pc_sec_override : 0;
            unix_now = day + h * 3600 + m * 60 + s;
        }

        gc_secs = unix_now - GC_UNIX_EPOCH_DIFF;
        gc_epoch_offset_ticks = gc_secs * (s64)GC_TIMER_CLOCK;
        DC_LOG("[DC/OS] RTC unix=%lld -> GC ticks=%lld\n",
               (long long)unix_now, (long long)gc_epoch_offset_ticks);
    }
#endif
}

void OSInitAlarm(void) { }
void __osInitialize_common(void) { }
void __OSPSInit(void) { }
void __OSFPRInit(void) { }
void __OSCacheInit(void) { }

u32 OSGetConsoleType(void) { return 0x10000004; /* OS_CONSOLE_DEVHW1 */ }
u32 OSGetPhysicalMemSize(void) { return DC_MAIN_MEMORY_SIZE; }

/* ==========================================================================
 * Diagnostics
 * ========================================================================== */
void OSPanic(const char* file, int line, const char* msg, ...) {
    va_list args;
    dc_loge_impl("[DC/OSPanic] %s:%d: ", file, line);
    va_start(args, msg);
    vprintf(msg, args);
    va_end(args);
    dc_loge_impl("\n");
}

/* Every OSReport in the game funnels here. On hardware this goes out over
 * dbgio at 57600 baud — leaving it on destroys frame time, so it stays gated
 * on g_pc_verbose exactly as on PC. */
void OSReport(const char* fmt, ...) {
    va_list args;
    if (!g_pc_verbose) return;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}

void OSVReport(const char* fmt, va_list list) {
    if (!g_pc_verbose) return;
    vprintf(fmt, list);
}

void OSReportDisable(void) { }

/* ==========================================================================
 * Alarms — no-ops, same as the base port. Nothing in AC depends on them
 * firing; the game polls instead.
 * ========================================================================== */
typedef struct OSAlarm { u8 _pad[64]; } OSAlarm;
void OSSetAlarm(OSAlarm* alarm, s64 tick, void (*callback)(OSAlarm*, void*)) {
    (void)alarm; (void)tick; (void)callback;
}
void OSSetPeriodicAlarm(OSAlarm* alarm, s64 start, s64 period,
                        void (*callback)(OSAlarm*, void*)) {
    (void)alarm; (void)start; (void)period; (void)callback;
}
void OSCancelAlarm(OSAlarm* alarm) { (void)alarm; }
void OSCreateAlarm(OSAlarm* alarm) { memset(alarm, 0, sizeof(OSAlarm)); }

/* ==========================================================================
 * Reset / shutdown
 * ========================================================================== */
BOOL OSGetResetSwitchState(void) { return FALSE; }

void OSResetSystem(int reset, u32 resetCode, BOOL forceMenu) {
    (void)reset; (void)resetCode; (void)forceMenu;
    DC_LOGE("[DC/OS] OSResetSystem -> leaving the game loop\n");
    g_pc_running = 0;
}

u32  OSGetResetCode(void) { return 0; }
void OSChangeBootMode(u32 mode) { (void)mode; }
int  __osResetSwitchPressed = 0;

void osShutdownStart(int type) { (void)type; g_pc_running = 0; }
int  osShutdown = 0;

void ReconfigBATs(void) {
    /* PowerPC block address translation. Nothing to do on SH-4; the SH-4 MMU
     * is left in KOS's default (P1/P2 straight-mapped) configuration. */
}

void OSSetStringTable(void* table) { (void)table; }

typedef struct OSSramEx { u8 data[64]; } OSSramEx;
OSSramEx* __OSLockSramEx(void) {
    static OSSramEx sramex = { { 0 } };
    return &sramex;
}
void __OSUnlockSramEx(BOOL commit) { (void)commit; }

void msleep(int ms) {
#ifdef DC_HOST_STUB
    (void)ms;
#else
    if (ms > 0) thd_sleep(ms);   /* VERIFY: KOS kos/thread.h thd_sleep(int ms) */
#endif
}

/* ==========================================================================
 * Mutexes — single-threaded, a flag is enough (same as PC).
 * ========================================================================== */
typedef struct OSMutex { int lock; } OSMutex;
void OSInitMutex(OSMutex* mutex)   { mutex->lock = 0; }
void OSLockMutex(OSMutex* mutex)   { mutex->lock = 1; }
void OSUnlockMutex(OSMutex* mutex) { mutex->lock = 0; }
BOOL OSTryLockMutex(OSMutex* mutex) {
    if (mutex->lock) return FALSE;
    mutex->lock = 1;
    return TRUE;
}

/* ==========================================================================
 * Message queues — LAYOUT-CRITICAL (§3.7). The game allocates these structs
 * itself, so the field offsets must match dolphin/os/OSMessage.h exactly:
 * 16 bytes of thread queues, then msgArray / msgCount / firstIndex / usedCount.
 * OSSendMessage is NON-BLOCKING and returns FALSE when full; jaudio's
 * TARGET_PC path depends on that (real Dolphin blocks on OS_MESSAGE_BLOCK).
 * ========================================================================== */
typedef struct {
    u8     _threadQueues[16];
    void** msgArray;
    int    msgCount;
    int    firstIndex;
    int    usedCount;
} OSMessageQueue_DC;

void OSInitMessageQueue(void* queue, void** msgArray, int msgCount) {
    OSMessageQueue_DC* mq = (OSMessageQueue_DC*)queue;
    memset(mq->_threadQueues, 0, 16);
    mq->msgArray = msgArray;
    mq->msgCount = msgCount;
    mq->firstIndex = 0;
    mq->usedCount = 0;
}

BOOL OSSendMessage(void* queue, void* msg, int flags) {
    OSMessageQueue_DC* mq = (OSMessageQueue_DC*)queue;
    int idx;
    (void)flags;
    if (mq->usedCount >= mq->msgCount) return FALSE;
    idx = (mq->firstIndex + mq->usedCount) % mq->msgCount;
    mq->msgArray[idx] = msg;
    mq->usedCount++;
    return TRUE;
}

BOOL OSReceiveMessage(void* queue, void** msgPtr, int flags) {
    OSMessageQueue_DC* mq = (OSMessageQueue_DC*)queue;
    (void)flags;
    if (mq->usedCount == 0) return FALSE;
    if (msgPtr) *msgPtr = mq->msgArray[mq->firstIndex];
    mq->firstIndex = (mq->firstIndex + 1) % mq->msgCount;
    mq->usedCount--;
    return TRUE;
}

BOOL OSJamMessage(void* queue, void* msg, int flags) {
    OSMessageQueue_DC* mq = (OSMessageQueue_DC*)queue;
    (void)flags;
    if (mq->usedCount >= mq->msgCount) return FALSE;
    mq->firstIndex = (mq->firstIndex - 1 + mq->msgCount) % mq->msgCount;
    mq->msgArray[mq->firstIndex] = msg;
    mq->usedCount++;
    return TRUE;
}

/* ==========================================================================
 * Heap shims
 * ========================================================================== */
volatile int __OSCurrHeap = -1;

/* Pure arithmetic: skip past maxHeaps heap descriptors, round up to 32.
 * Compiles unchanged from Dolphin. */
void* OSInitAlloc(void* arenaStart, void* arenaEnd, int maxHeaps) {
    uintptr_t arraySize = (uintptr_t)maxHeaps * 24;
    uintptr_t newStart = ((uintptr_t)arenaStart + arraySize + 0x1F) & ~(uintptr_t)0x1F;
    (void)arenaEnd;
    return (void*)newStart;
}

/* The PC build cheated with malloc(). On DC these must come out of somewhere
 * ledgered, or they are invisible to the budget. __OSCurrHeap is ignored, as
 * it is on PC — the game only ever uses one. */
void* OSAllocFromHeap(int heap, u32 size) {
    (void)heap;
    return dc_mem_alloc(DCMEM_JKRHEAP, (size_t)size, 32, "OSAllocFromHeap");
}
void OSFreeToHeap(int heap, void* ptr) {
    dc_mem_free(DCMEM_JKRHEAP, ptr);
    (void)heap;
}
int OSCreateHeap(void* lo, void* hi)  { (void)lo; (void)hi; return 0; }
int OSSetCurrentHeap(int heap)        { (void)heap; return 0; }

typedef struct OSContext { u8 _pad[1024]; } OSContext;
void OSClearContext(OSContext* ctx) { memset(ctx, 0, sizeof(OSContext)); }

/* ==========================================================================
 * Exception handling
 * ==========================================================================
 * THIS IS THE SEAM PLAN §3.2 NEEDS. SH-4 raises a CPU exception on unaligned
 * access, and the -O2 triage plan is: install a handler, log PC + faulting
 * address, triage per-TU exactly as the Anbernic repo did for its ARM SIGBUS.
 *
 * Left as a loud stub because the KOS irq API (irq_set_handler / EXC_*
 * constants / handler signature, which gained a user-data parameter in newer
 * KOS) has NOT been verified against the tree we will build with. Guessing it
 * would break the build, which is the one thing this pass must not do. */
void* __OSSetExceptionHandler(u8 type, void* handler) {
    DC_UNIMPLEMENTED_NOTE("install KOS irq_set_handler for the SH-4 unaligned/"
                          "TLB exceptions; PLAN 3.2 -O2 triage depends on it");
    (void)type; (void)handler;
    return NULL;
}
