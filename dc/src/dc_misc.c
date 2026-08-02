/* dc_misc.c - hardware register arrays, PPC/EXI/SI/DB shims, libc64 malloc
 * arena bookkeeping, N64 fixed-point trig, settings, logging.
 *
 * Ported from pc/src/pc_misc.c plus the parts of pc_settings.c / pc_prof.c
 * that the game references by name. Folded into one file on purpose: the DC
 * build's source list is owned by another agent and every extra .c is a chance
 * for a symbol to go missing at link time.
 */
#include "dc_platform.h"
#include "dc_mem_ledger.h"
#include "pc_settings.h"      /* PCSettings layout — game code reads g_pc_settings */

/* ==========================================================================
 * Logging and the unimplemented-stub registry
 * ==========================================================================
 * The whole point of this pass: every stub announces itself ONCE and continues.
 * The first boot attempt then produces a to-do list on the console instead of a
 * black screen, and dc_unimpl_dump() prints the roll-up at exit.
 */
#define DC_UNIMPL_MAX 192

static const char* s_unimpl_fn[DC_UNIMPL_MAX];
static const char* s_unimpl_file[DC_UNIMPL_MAX];
static int         s_unimpl_line[DC_UNIMPL_MAX];
static int         s_unimpl_n = 0;

/* --------------------------------------------------------------------------
 * Console flood limiter (kb/boot-blockers.md item 2)
 *
 * MEASURED: 85.3 % of one 3,956-line console log was jaudio's
 * "SendStart::Mesg Full Queue" (OSReport, sub_sys.c:274), and after the title
 * screen was reached a second flood appeared — game64.c_inc's [TRG_VOL] and
 * [WALK] lines, which call printf DIRECTLY and so cannot be caught inside
 * OSReport. Every future log is unreadable, and on hardware dbgio runs at
 * 57600 baud, so each line is real frame time.
 *
 * The sink is therefore a `printf` OVERRIDE in DC-owned code, not an edit to
 * src/ (CLAUDE.md §1). Our own diagnostics are unaffected because DC_LOG /
 * DC_LOGE go through dc_log_impl below, which calls vprintf directly.
 *
 * The key is the FORMAT STRING POINTER: one call site is one pointer, so no
 * formatting is needed to consult the table. Emission backs off to powers of
 * two per call site, so a rare line stays fully visible, the first few of a
 * flood survive, and the rest become a periodic heartbeat carrying the running
 * count — never silence. DC_CONSOLE_LIMIT=0 is the kill switch.
 * -------------------------------------------------------------------------- */
#ifndef DC_CONSOLE_LIMIT
#define DC_CONSOLE_LIMIT 1
#endif

#if DC_CONSOLE_LIMIT
#define DC_FLOOD_SLOTS    128u   /* power of two; 1,024 B of .bss */
#define DC_FLOOD_VERBATIM 4u     /* first N per call site print as-is */

static const char* s_flood_fmt[DC_FLOOD_SLOTS];
static u32         s_flood_count[DC_FLOOD_SLOTS];

/* Direct-mapped, REPLACE on miss — deliberately not open-addressed.
 * ⚠️ The first version probed for a free slot and gave up ("print it") once
 * the table filled. Boot alone produces more than 32 distinct call sites, so
 * the table was full before the flood even started and the limiter did nothing
 * at all — 741 unsuppressed lines in the very next run. Replacement is the
 * property that matters: a flooding call site re-claims its slot every time
 * and its count keeps climbing, while an evicted one-shot line simply prints
 * again. */
int dc_console_admit(const char* fmt, u32* out_count) {
    u32 slot;
    u32 n;

    *out_count = 0;
    if (fmt == NULL) return 1;

    {   /* pointer hash; format strings are .rodata, SH-4 pointers are 32-bit */
        u32 p = (u32)(unsigned long)fmt;
        slot = ((p >> 2) ^ (p >> 9)) & (DC_FLOOD_SLOTS - 1u);
    }
    if (s_flood_fmt[slot] != fmt) {
        s_flood_fmt[slot] = fmt;
        s_flood_count[slot] = 0;
    }

    n = ++s_flood_count[slot];
    *out_count = n;
    if (n <= DC_FLOOD_VERBATIM) return 1;
    return (n & (n - 1u)) == 0u;   /* 8, 16, 32, 64, ... */
}

/* Overrides newlib's printf for the whole image. Our objects precede libc on
 * the link line, so this definition wins; the link already carries
 * --allow-multiple-definition for the decomp's 1,367 duplicate data symbols,
 * so a pulled-in libc member cannot turn this into a link error either. */
int printf(const char* fmt, ...) {
    va_list ap;
    int r;
    u32 count = 0;

    if (!dc_console_admit(fmt, &count)) return 0;
    /* fprintf, not printf: this function IS printf. */
    if (count > DC_FLOOD_VERBATIM) fprintf(stdout, "[x%u] ", (unsigned)count);

    va_start(ap, fmt);
    r = vprintf(fmt, ap);
    va_end(ap);
    return r;
}
#else
int dc_console_admit(const char* fmt, u32* out_count) {
    (void)fmt; *out_count = 0; return 1;
}
#endif /* DC_CONSOLE_LIMIT */

void dc_log_impl(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void dc_loge_impl(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);
}

void dc_unimpl_report(const char* fn, const char* file, int line) {
    const char* base = file;
    const char* p;
    for (p = file; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;

    if (s_unimpl_n < DC_UNIMPL_MAX) {
        s_unimpl_fn[s_unimpl_n] = fn;
        s_unimpl_file[s_unimpl_n] = base;
        s_unimpl_line[s_unimpl_n] = line;
        s_unimpl_n++;
    }
    dc_loge_impl("[DC/TODO] %s (%s:%d)\n", fn, base, line);
}

int dc_unimpl_count(void) { return s_unimpl_n; }

void dc_unimpl_dump(void) {
    int i;
    dc_loge_impl("[DC/TODO] ==== %d unimplemented entry points were hit ====\n",
                 s_unimpl_n);
    for (i = 0; i < s_unimpl_n; i++)
        dc_loge_impl("[DC/TODO]   %-40s %s:%d\n",
                     s_unimpl_fn[i], s_unimpl_file[i], s_unimpl_line[i]);
    if (s_unimpl_n >= DC_UNIMPL_MAX)
        dc_loge_impl("[DC/TODO]   (list truncated at %d)\n", DC_UNIMPL_MAX);
}

/* ==========================================================================
 * Hardware register arrays
 * ==========================================================================
 * The decomp reads and writes these directly. They are plain memory on every
 * port; nothing on the Dreamcast is behind them.
 */
volatile u16 __VIRegs[59]    = { 0 };
volatile u32 __PIRegs[12]    = { 0 };
volatile u16 __MEMRegs[64]   = { 0 };
volatile u16 __DSPRegs[32]   = { 0 };
volatile u32 __DIRegs[16]    = { 0 };
volatile u32 __SIRegs[0x100] = { 0 };
volatile u32 __EXIRegs[0x40] = { 0 };
volatile u32 __AIRegs[8]     = { 0 };

/* These must agree with OSGetPhysicalMemSize() and the 0x28 word (§3.1). */
u32 __OSPhysicalMemSize   = DC_MAIN_MEMORY_SIZE;
u32 __OSSimulatedMemSize  = DC_MAIN_MEMORY_SIZE;
volatile int __OSTVMode   = 0;
u32 __OSBusClock          = GC_BUS_CLOCK;
u32 __OSCoreClock         = GC_CORE_CLOCK;
volatile u16 __OSDeviceCode = 0;

/* Video-mode descriptors JFWDisplay::createManager picks from. The port never
 * reads their contents (dc_vi.c owns the real mode), but the game takes their
 * addresses, so they must exist. */
u8 GXNtsc480IntDf[64]      = { 0 };
u8 GXNtsc480Int[64]        = { 0 };
u8 GXMpal480IntDf[64]      = { 0 };
u8 GXPal528IntDf[64]       = { 0 };
u8 GXEurgb60Hz480IntDf[64] = { 0 };

void* __gUnkThread1 = NULL;
void* __gCurrentThread = NULL;
s32   __gUnknown800030C0[2] = { 0 };
u8    __gUnknown800030E3 = 0;

/* ==========================================================================
 * EXI / SI — GameCube serial buses. Nothing behind them; 0 live call sites
 * that care about the result.
 * ========================================================================== */
BOOL EXILock(s32 chan, u32 dev, void* cb) { (void)chan; (void)dev; (void)cb; return TRUE; }
BOOL EXIUnlock(s32 chan) { (void)chan; return TRUE; }
BOOL EXISelect(s32 chan, u32 dev, u32 freq) { (void)chan; (void)dev; (void)freq; return TRUE; }
BOOL EXIDeselect(s32 chan) { (void)chan; return TRUE; }
BOOL EXIImm(s32 chan, void* data, s32 len, u32 type, void* cb) {
    (void)chan; (void)data; (void)len; (void)type; (void)cb; return TRUE;
}
BOOL EXIDma(s32 chan, void* data, s32 len, u32 type, void* cb) {
    (void)chan; (void)data; (void)len; (void)type; (void)cb; return TRUE;
}
BOOL EXISync(s32 chan) { (void)chan; return TRUE; }
void EXIInit(void) { }
BOOL EXIAttach(s32 chan, void* cb) { (void)chan; (void)cb; return TRUE; }
BOOL EXIDetach(s32 chan) { (void)chan; return TRUE; }
BOOL EXIProbe(s32 chan) { (void)chan; return FALSE; }
BOOL EXIProbeEx(s32 chan) { (void)chan; return FALSE; }
s32  EXIGetID(s32 chan, u32 dev, u32* id) { (void)chan; (void)dev; if (id) *id = 0; return 0; }
void EXISetExiCallback(s32 chan, void* cb) { (void)chan; (void)cb; }

void SIInit(void) { }
u32  SIGetType(s32 chan) { (void)chan; return 0x09000000; /* standard controller */ }
u32  SIGetTypeAsync(s32 chan, void* cb) { (void)cb; return SIGetType(chan); }
BOOL SITransfer(s32 chan, void* out, u32 outLen, void* in, u32 inLen,
                void* cb, s64 time) {
    (void)chan; (void)out; (void)outLen; (void)in; (void)inLen; (void)cb; (void)time;
    return TRUE;
}
u32  SISetXY(u32 x, u32 y) { (void)x; (void)y; return 0; }
u32  SIEnablePolling(u32 poll) { (void)poll; return 0; }
u32  SIDisablePolling(u32 poll) { (void)poll; return 0; }
void SISetSamplingRate(u32 rate) { (void)rate; }
BOOL SIIsChanBusy(s32 chan) { (void)chan; return FALSE; }
void SIRefreshSamplingRate(void) { }
BOOL SIRegisterPollingHandler(void* h) { (void)h; return TRUE; }
BOOL SIUnregisterPollingHandler(void* h) { (void)h; return TRUE; }

/* ==========================================================================
 * PowerPC intrinsics
 * ==========================================================================
 * Mostly dead. THE TWO THAT ARE NOT (design doc §5):
 *   PPCMfmsr/PPCMtmsr — 2 live call sites, JUTException.cpp masks interrupts
 *     through the MSR. On SH-4 the equivalent is the SR register; leaving these
 *     inert means the exception path does not actually mask, which is fine
 *     while the port is single-threaded.
 *   PPCSync — 2 live call sites (emu64.c:5912, dspdriver.c:373), memory
 *     barriers. Made REAL below, because SH-4 with store queues and AICA DMA
 *     genuinely needs them.
 */
u32  PPCMfmsr(void) { DC_UNIMPLEMENTED_NOTE("SH-4 SR read (JUTException masks IRQs)"); return 0; }
void PPCMtmsr(u32 msr) { (void)msr; DC_UNIMPLEMENTED_NOTE("SH-4 SR write"); }
u32  PPCMfhid0(void) { return 0; }
void PPCMthid0(u32 v) { (void)v; }
u32  PPCMfhid2(void) { return 0; }
void PPCMthid2(u32 v) { (void)v; }
void PPCHalt(void) {
    DC_LOGE("[DC] PPCHalt\n");
#ifndef DC_HOST_STUB
    arch_exit();
#else
    exit(1);
#endif
}
u32  PPCMfl2cr(void) { return 0; }
void PPCMtl2cr(u32 v) { (void)v; }
void PPCMtdec(u32 v) { (void)v; }

void PPCSync(void) {
    /* Real barrier: SH-4 reorders around store queues and DMA. */
    __sync_synchronize();
}

void PPCMtmmcr0(u32 v) { (void)v; }
void PPCMtmmcr1(u32 v) { (void)v; }
void PPCMtpmc1(u32 v) { (void)v; }
void PPCMtpmc2(u32 v) { (void)v; }
void PPCMtpmc3(u32 v) { (void)v; }
void PPCMtpmc4(u32 v) { (void)v; }
u32  PPCMfpmc1(void) { return 0; }
u32  PPCMfpmc2(void) { return 0; }
u32  PPCMfpmc3(void) { return 0; }
u32  PPCMfpmc4(void) { return 0; }

void OSInitFastCast(void) { }
void __OSSetupFPU(void) { }
void __sync(void) { __sync_synchronize(); }
void __isync(void) { __sync_synchronize(); }

/* ==========================================================================
 * Debugger / MetroTRK
 * ========================================================================== */
void DBInit(void) { }
BOOL DBIsDebuggerPresent(void) { return FALSE; }
void DBPrintf(const char* fmt, ...) {
    va_list args;
    if (!g_pc_verbose) return;
    va_start(args, fmt);
    vprintf(fmt, args);
    va_end(args);
}
void InitMetroTRK(void) { }
void InitMetroTRK_BBA(void) { }

/* ==========================================================================
 * OS reset functions / REL module loader
 * ========================================================================== */
typedef BOOL (*OSResetFunction)(BOOL final);
typedef struct OSResetFunctionInfo {
    OSResetFunction func;
    u32 priority;
    struct OSResetFunctionInfo* next;
    struct OSResetFunctionInfo* prev;
} OSResetFunctionInfo;

void OSRegisterResetFunction(OSResetFunctionInfo* info) { (void)info; }
void OSUnregisterResetFunction(OSResetFunctionInfo* info) { (void)info; }

typedef struct OSModuleInfo {
    u32 id;
    void* link_next;
    void* link_prev;
    u32 numSections;
    u32 sectionInfoOffset;
    u32 nameOffset;
    u32 nameSize;
    u32 version;
} OSModuleInfo;

typedef struct OSModuleHeader {
    OSModuleInfo info;
    u32 bssSize;
    u32 relOffset;
    u32 impOffset;
    u32 impSize;
    u8  prologSection;
    u8  epilogSection;
    u8  unresolvedSection;
    u8  bssSection;
    u32 prolog;
    u32 epilog;
    u32 unresolved;
} OSModuleHeader;

OSModuleHeader* BaseModule = NULL;
struct { void* head; void* tail; } __OSModuleList = { 0, 0 };
void* __OSStringTable = NULL;
u8 DiskID[32] = { 0 };

/* foresta.rel's code is linked in statically rather than relocated at runtime,
 * exactly as on PC. 1 call site, in boot.c. */
BOOL OSLink(void* info, void* bss) { (void)info; (void)bss; return TRUE; }
BOOL OSUnlink(void* info) { (void)info; return TRUE; }

/* ==========================================================================
 * libc64 malloc-arena bookkeeping
 * ==========================================================================
 * libc64/malloc.c is excluded from the build; these wrap the game's arena
 * accounting. jsyswrap.cpp calls MallocInit(gameheap_base, gameheap_len) where
 * gameheap = system-heap free minus 0x10000.
 *
 * mem-budget C5 wants this to stop receiving "the whole remainder" and get a
 * fixed, ledgered extent instead. That change belongs in jsyswrap.cpp, not
 * here — but the numbers are reported so M1 can see what it is actually given.
 */
static int   malloc_initialized = 0;
static void* malloc_arena_base = NULL;
static unsigned long malloc_arena_size = 0;

void MallocInit(void* base, unsigned long size) {
    malloc_initialized = 1;
    malloc_arena_base = base;
    malloc_arena_size = size;
    DC_LOGE("[DC/MEM] game malloc arena: %lu B at %p (mem-budget bucket 6)\n",
            size, base);
    dc_mem_note(DCMEM_JKRHEAP, 0);   /* already inside the arena extent */
}
void MallocCleanup(void) { malloc_initialized = 0; }
int  MallocIsInitalized(void) { return malloc_initialized; }
void GetFreeArena(unsigned long* max, unsigned long* free_size, unsigned long* alloc) {
    if (max) *max = malloc_arena_size;
    if (free_size) *free_size = malloc_arena_size;
    if (alloc) *alloc = 0;
}
void DisplayArena(void) { }
int  CheckArena(void) { return 0; }

/* ==========================================================================
 * N64 fixed-point trig
 * ==========================================================================
 * pc_misc.c calls double sin()/cos() per invocation. On SH-4 that is a soft-FP
 * double call in a hot path, so this is a 1024-entry quarter-wave s16 table
 * (2 KB) — the "replace with a 16-bit LUT" note in the design doc.
 */
#define DC_SIN_QUAD 1024
static s16 s_sin_quad[DC_SIN_QUAD + 1];
static int s_sin_ready = 0;

static void dc_sin_table_init(void) {
    int i;
    if (s_sin_ready) return;
    s_sin_ready = 1;
    for (i = 0; i <= DC_SIN_QUAD; i++) {
        double a = (double)i * (DC_PI * 0.5) / (double)DC_SIN_QUAD;
        s_sin_quad[i] = (short)(sin(a) * 32767.0);
    }
}

static short dc_sins_u16(unsigned short angle) {
    unsigned int idx, quad;
    if (!s_sin_ready) dc_sin_table_init();
    /* angle is a full turn in 65536 steps -> 16384 per quadrant. */
    quad = (angle >> 14) & 3u;
    idx = (unsigned int)(angle & 0x3FFFu) * DC_SIN_QUAD / 16384u;
    switch (quad) {
        case 0: return s_sin_quad[idx];
        case 1: return s_sin_quad[DC_SIN_QUAD - idx];
        case 2: return (short)-s_sin_quad[idx];
        default: return (short)-s_sin_quad[DC_SIN_QUAD - idx];
    }
}

short sins(unsigned short angle) { return dc_sins_u16(angle); }
short coss(unsigned short angle) { return dc_sins_u16((unsigned short)(angle + 0x4000)); }

/* ==========================================================================
 * Unhandled exceptions
 * ==========================================================================
 * PLAN §3.2: SH-4 raises a CPU exception on unaligned access and the -O2
 * triage plan is "log PC + faulting address, triage per-TU". This is the
 * GameCube-side entry point; the KOS-side handler that would call it is
 * stubbed in dc_os.c (__OSSetExceptionHandler).
 */
void __OSUnhandledException(u8 type, void* ctx, u32 dsisr, u32 dar) {
    (void)ctx;
    DC_LOGE("[DC/EXC] unhandled exception type=%d dsisr=0x%08X dar=0x%08X\n",
            (int)type, (unsigned)dsisr, (unsigned)dar);
    dc_unimpl_dump();
    dc_mem_report(1);
}

/* ==========================================================================
 * Settings
 * ==========================================================================
 * pc/src/pc_settings.c cannot be reused: it parses an ini file and calls SDL
 * for resolution autodetection. Game code reads g_pc_settings fields directly
 * (m_actor.c, m_field_info.c, graph.c, the weather actors, ac_animal_logo.c's
 * options menu), so the STRUCT must be the real PCSettings and the defaults
 * must be sane for a fixed 640x480 console.
 *
 * There is no settings file yet. When one lands it should live on the VMU, not
 * the disc — the disc is read-only.
 */
PCSettings g_pc_settings;

int pc_settings_get_nes_aspect(void) { return g_pc_settings.nes_aspect; }

void pc_settings_load(void) {
    memset(&g_pc_settings, 0, sizeof(g_pc_settings));
    g_pc_settings.window_width  = DC_SCREEN_WIDTH;
    g_pc_settings.window_height = DC_SCREEN_HEIGHT;
    g_pc_settings.fullscreen    = 1;
    g_pc_settings.vsync         = 1;
    g_pc_settings.msaa          = 0;
    g_pc_settings.preload_textures = 0;
    g_pc_settings.verbose       = 0;
    g_pc_settings.show_fps      = 0;
    g_pc_settings.master_volume = 100;
    g_pc_settings.bgm_volume    = 100;
    g_pc_settings.sfx_volume    = 100;
    g_pc_settings.voice_volume  = 100;
    g_pc_settings.zoom_enabled  = 0;
    /* fps_target: 3 == 30 fps. PLAN §1 — 30 is the DESIGN TARGET. */
    g_pc_settings.fps_target    = 3;
    g_pc_settings.render_scale  = 100;
    g_pc_settings.window_size   = 2;   /* 640x480 */
    g_pc_settings.scale_mode    = 0;
    g_pc_settings.dpad_as_stick = 0;   /* the DC pad has a real analog stick */
    g_pc_settings.left_deadzone = 10;
    g_pc_settings.right_deadzone= 10;
    g_pc_settings.swap_ab_xy    = 0;
    /* Perf knobs start conservative: the SH-4 budget is the M3 go/no-go gate. */
    g_pc_settings.frustum_cull  = 1;
    g_pc_settings.frustum_cull_z_margin = 0;
    g_pc_settings.frustum_cull_max_distance = 0;
    g_pc_settings.shadow_quality = 3;  /* player + NPC */
    g_pc_settings.reduce_acre_draw = 1;/* cross (orthogonal acres only) */
    g_pc_settings.particle_quality = 2;/* 50 % */
    g_pc_settings.disable_resetti = 0;
    g_pc_settings.nes_aspect    = 1;

    g_pc_fps_target = 30;
}

void pc_settings_save(void) {
    DC_UNIMPLEMENTED_NOTE("persist settings to the VMU (the disc is read-only)");
}

void pc_settings_apply(void) {
    switch (g_pc_settings.fps_target) {
        case 0: g_pc_fps_target = 60; break;
        case 1: g_pc_fps_target = 50; break;
        case 2: g_pc_fps_target = 40; break;
        case 3: g_pc_fps_target = 30; break;
        case 4: g_pc_fps_target = 20; break;
        case 5: g_pc_fps_target = 0;  break;   /* unlimited */
        default: break;                        /* 6 == dynamic, dc_vi.c owns it */
    }
    g_pc_verbose = g_pc_settings.verbose ? 1 : g_pc_verbose;
}

void pc_settings_autodetect_resolution(void) { }
void pc_settings_reset_controllers(void) { }

/* Called from m_actor.c on every actor cull test — keep it branch-light. */
float pc_settings_cull_limit_xz(float cull_distance, float cull_radius) {
    float lim = cull_distance + cull_radius +
                (float)g_pc_settings.frustum_cull_z_margin;
    if (g_pc_settings.frustum_cull_max_distance > 0) {
        float cap = (float)g_pc_settings.frustum_cull_max_distance;
        if (lim > cap) lim = cap;
    }
    return lim;
}

/* ==========================================================================
 * pc_prof — the slow-phase profiler the decomp wraps call sites with.
 * ========================================================================== */
static long long g_prof_thresh_us = 50000;   /* 50 ms, as on PC */

unsigned long long pc_prof_now_us(void) { return (unsigned long long)dc_time_us(); }

void pc_prof_report(const char* tag, int id, unsigned long long t0_us) {
    unsigned long long dt = pc_prof_now_us() - t0_us;
    if ((long long)dt >= g_prof_thresh_us)
        DC_LOGE("[PROF] %s(0x%X): %llu.%03llums\n", tag, (unsigned)id,
                dt / 1000, dt % 1000);
}
