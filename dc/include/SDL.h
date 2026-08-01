/* SDL.h - MINIMAL SHIM for the Dreamcast build. This is not SDL.
 *
 * AUDITED 2026-08-01: verified against the tree, kept. Exactly ONE file under
 * src/ includes <SDL.h>, and it uses exactly the six symbols below:
 *
 *   src/static/jaudio_NES/internal/os.c   (inside its TARGET_PC block)
 *       SDL_mutex, SDL_CreateMutex, SDL_DestroyMutex,
 *       SDL_LockMutex, SDL_UnlockMutex, SDL_Delay
 *
 * The design doc's recommendation is to keep TARGET_PC defined for M1 so the
 * link closes without reopening the 84 conditionally-compiled game files. That
 * makes this shim the cheapest correct answer. The bodies live in
 * dc/src/dc_stubs.c and are counters, not real mutexes, because the port is
 * single-threaded (dc_os.c).
 *
 * BUILD REQUIREMENT: os.c uses ANGLE BRACKETS, so `dc/include` must be on the
 * -I search path (a quote-only include path is not enough), and it must come
 * before any real SDL2 include directory.
 *
 * KNOWN HAZARD, not fixed here: Z_osRecvMesg() with OS_MESG_BLOCK spins
 *     while (!mq->validCount) { unlock; SDL_Delay(1); lock; }
 * (os.c:81-85). On the base port a real SDL audio producer thread filled that
 * queue. This port has no producer thread, so if the game ever reaches that
 * path it hangs forever rather than deadlocking visibly. If boot stops with
 * the console alive and no further output, look here first. The fix is either
 * a real KOS audio thread (PLAN §3.4 stage A) or a TARGET_DC branch in os.c —
 * NOT a bigger fake SDL.
 *
 * DO NOT GROW THIS FILE. More SDL surface means TARGET_DC in the affected game
 * file, not more fake SDL.
 */
#ifndef DC_SDL_SHIM_H
#define DC_SDL_SHIM_H

#ifdef __cplusplus
extern "C" {
#endif

typedef struct SDL_mutex SDL_mutex;

SDL_mutex* SDL_CreateMutex(void);
void       SDL_DestroyMutex(SDL_mutex* m);
int        SDL_LockMutex(SDL_mutex* m);
int        SDL_UnlockMutex(SDL_mutex* m);
void       SDL_Delay(unsigned int ms);

#ifdef __cplusplus
}
#endif

#endif /* DC_SDL_SHIM_H */
