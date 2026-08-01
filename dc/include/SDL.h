/* SDL.h - MINIMAL SHIM for the Dreamcast build. This is not SDL.
 *
 * src/static/jaudio_NES/internal/os.c does `#include <SDL.h>` inside its
 * TARGET_PC block and uses exactly six SDL symbols to guard the jaudio message
 * queues:
 *     SDL_mutex, SDL_CreateMutex, SDL_DestroyMutex,
 *     SDL_LockMutex, SDL_UnlockMutex, SDL_Delay
 *
 * The design doc's recommendation is to keep TARGET_PC defined for M1 so the
 * link closes without reopening the 84 conditionally-compiled game files. That
 * makes this shim the cheapest correct answer: six symbols, implemented in
 * dc/src/dc_stubs.c against KOS.
 *
 * DO NOT grow this file. If the DC port ever needs more of SDL's surface, the
 * right move is TARGET_DC in the affected game file, not a bigger fake SDL.
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
