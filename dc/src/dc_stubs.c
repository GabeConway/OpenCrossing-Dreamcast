/* dc_stubs.c - definitions the decomp declares but the Dreamcast port does not
 * implement. Ported from pc/src/pc_stubs.c, with every DC-relevant stub made
 * LOUD: it logs its own name once and continues, so the first boot attempt
 * produces a to-do list rather than a black screen.
 *
 * The genuinely inert ones (GBA link cable, NES emulator, libultra controller
 * API) stay silent — they were already dead on the base port and firing a log
 * line for them would only bury the real findings.
 */
#include "dc_platform.h"
#include "SDL.h"                 /* dc/include/SDL.h — the 6-symbol shim, not SDL */
#include "dolphin/os/OSCache.h"  /* DCStoreRangeNoSync etc. */

/* msleep() lives in dc_os.c. Declared by hand rather than via
 * <libc64/sleep.h>, because that header pulls <dolphin/os.h> and the real
 * Dolphin prototypes conflict with the deliberately-placeholder (void*)
 * signatures this file uses for the thread/DVD-dir stubs below. */
void msleep(int ms);

typedef s32 OSPriority;

/* ==========================================================================
 * Data symbols
 * ==========================================================================
 * c_keyframe.c's gnu89 `inline` definitions also emit out-of-line copies; GNU
 * ld resolves the clash via --allow-multiple-definition and these win by link
 * order. sh-elf uses GNU ld, so the base port's Linux behaviour applies (the
 * macOS/ld64 carve-out in pc_stubs.c does not).
 */
unsigned char cKF_Animation_R_getDataTable[4] = { 0 };
unsigned char cKF_Animation_R_getFixedTable[4] = { 0 };
unsigned char cKF_Animation_R_getFlagTable[4] = { 0 };
unsigned char cKF_Animation_R_getKeyTable[4] = { 0 };

char _e_data = 0;
char _f_rodata = 0;

void* my_malloc_current = NULL;
u8    save_game_image = 0;

/* ==========================================================================
 * SDL shim (see dc/include/SDL.h for why this exists)
 * ==========================================================================
 * src/static/jaudio_NES/internal/os.c guards the jaudio message queues with an
 * SDL mutex. The port is single-threaded (dc_os.c), so a counter is a correct
 * mutex here. The moment a real KOS audio thread lands these must become
 * KOS mutex_t — flagged in dc_os.c's OSDisableInterrupts comment too.
 */
struct SDL_mutex { int locked; };
static struct SDL_mutex s_sdl_mutexes[4];
static int s_sdl_mutex_n = 0;

SDL_mutex* SDL_CreateMutex(void) {
    if (s_sdl_mutex_n >= (int)(sizeof(s_sdl_mutexes) / sizeof(s_sdl_mutexes[0])))
        return NULL;
    s_sdl_mutexes[s_sdl_mutex_n].locked = 0;
    return &s_sdl_mutexes[s_sdl_mutex_n++];
}
void SDL_DestroyMutex(SDL_mutex* m) { if (m) m->locked = 0; }
int  SDL_LockMutex(SDL_mutex* m)   { if (m) m->locked = 1; return 0; }
int  SDL_UnlockMutex(SDL_mutex* m) { if (m) m->locked = 0; return 0; }
void SDL_Delay(unsigned int ms)    { msleep((int)ms); }

/* ==========================================================================
 * DVD directory enumeration — the game never walks directories on any port.
 * ========================================================================== */
BOOL DVDCheckDisk(void) { return 0; }
BOOL DVDOpenDir(char* dirName, void* dir) { (void)dirName; (void)dir; return 0; }
BOOL DVDReadDir(void* dir, void* dirEntry) { (void)dir; (void)dirEntry; return 0; }
BOOL DVDCloseDir(void* dir) { (void)dir; return 0; }

/* ==========================================================================
 * GBA link cable — non-goal, inert on every port.
 * ========================================================================== */
void GBAInit(void) { }
s32 GBAGetStatus(s32 chan, u8* status) { (void)chan; (void)status; return -1; }
s32 GBAGetProcessStatus(s32 chan, u8* p) { (void)chan; (void)p; return -1; }
s32 GBARead(s32 chan, u8* dst, u8* status) { (void)chan; (void)dst; (void)status; return -1; }
s32 GBAWrite(s32 chan, u8* src, u8* status) { (void)chan; (void)src; (void)status; return -1; }
s32 GBAReset(s32 chan, u8* status) { (void)chan; (void)status; return -1; }
s32 GBAJoyBootAsync(s32 chan, s32 palette_color, s32 palette_speed, u8* programp,
                    s32 length, u8* status, void* callback) {
    (void)chan; (void)palette_color; (void)palette_speed; (void)programp;
    (void)length; (void)status; (void)callback;
    return -1;
}

/* ==========================================================================
 * OS threads
 * ==========================================================================
 * OSCreateThread returning FALSE is LOAD-BEARING, not laziness: JKRThread and
 * JKRDecomp call it and the port relies on the thread never starting (design
 * doc §3.7). JKRDecomp in particular assumes a suspended decompression thread
 * it can resume; on this port decompression happens inline on the caller.
 */
BOOL OSCreateThread(void* thread, void* (*func)(void*), void* param,
                    void* stack, u32 stackSize, OSPriority priority, u16 attr) {
    (void)thread; (void)func; (void)param; (void)stack;
    (void)stackSize; (void)priority; (void)attr;
    return 0;
}
void OSCancelThread(void* thread) { (void)thread; }
void OSDetachThread(void* thread) { (void)thread; }
s32  OSResumeThread(void* thread) { (void)thread; return 0; }
s32  OSSuspendThread(void* thread) { (void)thread; return 0; }
BOOL OSIsThreadTerminated(void* thread) { (void)thread; return 1; }
s32  OSEnableScheduler(void) { return 0; }
void OSYieldThread(void) { }
long OSCheckActiveThreads(void) { return 0; }
void OSFillFPUContext(void* context) { (void)context; }

/* ==========================================================================
 * OS misc
 * ========================================================================== */
u16  OSGetFontEncode(void) { return 0; }
u32  OSGetProgressiveMode(void) { return 0; }
void OSSetProgressiveMode(u32 on) { (void)on; }
/* On GameCube this is SRAM-backed. Returning 0 = OS_SOUND_MODE_MONO made
 * sAdo_SetOutMode (src/audio.c:147) force Na_SetOutMode(1) unconditionally, so
 * the port hard-locked itself to mono against kb/audio-plan-of-record.md §9.1
 * ("22.05 kHz, STEREO locked"), and ac_npc_p_sel.c:102 showed the wrong
 * setting in the options menu. The Dreamcast is a stereo machine; default to
 * stereo and honour the player's choice for the rest of the session.
 * (Persisting it across boots is dc_misc.c:403's settings stub, item 11.) */
static u32 s_sound_mode = 1;   /* OS_SOUND_MODE_STEREO */
u32  OSGetSoundMode(void) { return s_sound_mode; }
void OSSetSoundMode(u32 mode) { s_sound_mode = (mode != 0) ? 1u : 0u; }
void OSProtectRange(u32 chan, void* addr, u32 nBytes, u32 control) {
    (void)chan; (void)addr; (void)nBytes; (void)control;
}
void* OSSetErrorHandler(u16 error, void* handler) {
    (void)error; (void)handler;
    return NULL;
}
void OSReportEnable(void) { }

void VIConfigurePan(u16 x, u16 y, u16 w, u16 h) {
    (void)x; (void)y; (void)w; (void)h;
}

int  __abs(int x) { return x < 0 ? -x : x; }
void _strip(float x) { (void)x; }

/* ==========================================================================
 * Famicom (NES emulator)
 * ==========================================================================
 * The core is PowerPC assembly and was already excluded on the base port.
 * PLAN "Non-goals": fixNES on SH-4 is a post-1.0 curiosity. famicom_init
 * announces itself once so a player report of "the NES does nothing" maps to a
 * known decision rather than a bug hunt.
 */
void famicom_1frame(void) { }
int  famicom_cleanup(void) { return 0; }
int  famicom_external_data_save(void) { return 0; }
int  famicom_external_data_save_check(void) { return 0; }
int  famicom_getErrorChan(void) { return 0; }
int  famicom_get_disksystem_titles(int* n_games, char* buf, int bufsize) {
    (void)n_games; (void)buf; (void)bufsize;
    return 0;
}
int famicom_init(int rom_idx, void* malloc_info, int player_no) {
    (void)rom_idx; (void)malloc_info; (void)player_no;
    DC_UNIMPLEMENTED_NOTE("NES emulator is a documented non-goal (PPC asm core)");
    return 0;
}
int  famicom_internal_data_load(void) { return 0; }
int  famicom_internal_data_save(void) { return 0; }
void famicom_mount_archive(void) { }
int  famicom_mount_archive_end_check(void) { return 1; }
int  famicom_rom_load_check(void) { return 0; }
void famicom_setCallback_getSaveChan(void* proc) { (void)proc; }

/* ==========================================================================
 * libultra controller / thread / timer API — replaced by the Dolphin PAD path.
 * ========================================================================== */
void osContGetQuery(void* status) { (void)status; }
void osContGetReadData(void* pad) { (void)pad; }
s32  osContInit(void* mq, u8* pattern_p, void* status) {
    (void)mq; (void)pattern_p; (void)status; return 0;
}
s32  osContSetCh(u8 n) { (void)n; return 0; }
s32  osContStartQuery(void* mq) { (void)mq; return 0; }
s32  osContStartReadData(void* mq) { (void)mq; return 0; }
void osDestroyThread(void* t) { (void)t; }
s32  osGetThreadId(void* thread) { (void)thread; return 0; }
int  osSetTimer(void* t, s64 countdown, s64 interval, void* mq, void* msg) {
    (void)t; (void)countdown; (void)interval; (void)mq; (void)msg;
    return 0;
}
void osSyncPrintf(const char* fmt, ...) { (void)fmt; }

/* REAL on SH-4 (§3.2): 8 call sites in compiled libultra-flavoured game code.
 * Same treatment as DCStoreRange — writeback, no invalidate. */
void osWritebackDCache(void* vaddr, u32 nbytes) {
    DCStoreRangeNoSync(vaddr, nbytes);
}

int vaprintf(void* func, const char* fmt, va_list ap) {
    (void)func; (void)fmt; (void)ap;
    return 0;
}

/* ==========================================================================
 * Dropped PC subsystems that game code still names
 * ==========================================================================
 * Each of these is a deliberate cut (design doc §2c "Rewrite from scratch" ->
 * drop). They exist so the link closes; they log so a surprise dependency
 * shows up in the boot to-do list.
 */

/* Keyboard typing for the in-game text editor. src/game/m_editor_ovl.c calls
 * this through a local `extern int pc_typing_queue_pop(int*)`. PLAN §7 wants
 * the real DC keyboard peripheral via maple eventually — a native fit for AC
 * letters — but the in-game on-screen keyboard works without it. */
int pc_typing_queue_pop(int* out) {
    if (out) *out = 0;
    return 0;
}

/* Debug model viewer (pc_model_viewer.c, 891 lines) — dev tool, dropped.
 * graph.c and m_game_dlftbls.c reference these through pc_model_viewer.h. */
void pc_model_viewer_init(void* game) {
    (void)game;
    DC_UNIMPLEMENTED_NOTE("model viewer is a dropped dev tool");
}
void pc_model_viewer_cleanup(void* game) { (void)game; }

/* GL overlay renderer (pc_overlay.c, 1247 lines) — dropped for now. A DC debug
 * overlay is cheap once the PVR backend exists (mem-budget budgets 12,288 B
 * for it in bucket 5) and would also drive the VMU LCD display in PLAN §7. */
void pc_overlay_init(void) { }
void pc_overlay_shutdown(void) { }
void pc_overlay_draw(void) { }
void pc_overlay_update(double fps, double speed_pct) { (void)fps; (void)speed_pct; }
void pc_overlay_menu_toggle(void) { }
void pc_overlay_boot_splash(const char* msg) {
    if (msg) DC_LOGE("[DC] %s\n", msg);
}

/* HD texture packs (pc_texture_pack.c, 1239 lines) — explicit non-goal. */
void pc_texture_pack_init(void) { }
void pc_texture_pack_shutdown(void) { }
void pc_texture_pack_preload_all(void) { }

/* Keybindings (pc_keybindings.c) — the DC pad mapping is fixed in dc_pad.c. */
void pc_keybindings_load(void) { }
void pc_keybindings_save(void) { }
int  pc_keybindings_uses_gamepad(void) { return 1; }

/* NES emulator GL-state restore hook, called from graph.c on the PC port. */
void pc_gx_restore_after_nes(void) { }
