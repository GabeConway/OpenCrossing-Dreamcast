/* OpenCrossing-Dreamcast harness self-test guest program.
 *
 * This is the reference implementation of the guest side of the harness
 * protocol documented in harness/dc/README.md.  Every DC test program the
 * project writes should follow the same shape:
 *
 *   1. raise the SCIF baud rate (serial is emulated at real baud; at the KOS
 *      default of 57600 you get ~5.8 KB/s and ~190 bytes eats a 30fps frame),
 *   2. force dbgio onto scif so the channel cannot be stolen,
 *   3. print OC-DC-HARNESS-BEGIN,
 *   4. emit typed one-line records (MARK:/ASSERT/PERF/FBHASH/FBTHUMB/MEM),
 *   5. print OC-DC-HARNESS-END rc=<n> and stop.
 *
 * Flycast never exits on its own, so the host-side runner kills it as soon as
 * it sees the END line.  Never print the END line before you are done.
 *
 * HARD RULE (verified 2026-08-01, KOS 2.3.0 + Flycast v2.6): never call
 * scif_flush() from guest code.  It clears TEND and spins waiting for it to
 * come back; Flycast never re-raises TEND on an idle TX FIFO, the spin times
 * out, and KOS latches `serial_enabled = 0` -- permanently killing all further
 * serial output including the crash dump.  printf() already flushes.
 */

#include <kos.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#include <dc/scif.h>
#include <dc/video.h>
#include <arch/timer.h>

/* Emulator-only: 50 MHz / 32 = 1562500 is the SH-4 SCIF maximum.  A real
 * coder's cable will not sync here, so this stays behind the harness build. */
#define HARNESS_BAUD 1562500

#define FB_W 640
#define FB_H 480
#define THUMB_W 16
#define THUMB_H 12

static const char b64tab[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void b64_emit(const uint8_t *p, int n) {
    /* Builds into a static buffer then prints once: one write, one line. */
    static char out[((THUMB_W * THUMB_H * 2 + 2) / 3) * 4 + 8];
    int i = 0, o = 0;
    while(i + 2 < n) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8) | p[i + 2];
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = b64tab[(v >> 6) & 63];
        out[o++] = b64tab[v & 63];
        i += 3;
    }
    if(n - i == 1) {
        uint32_t v = (uint32_t)p[i] << 16;
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if(n - i == 2) {
        uint32_t v = ((uint32_t)p[i] << 16) | ((uint32_t)p[i + 1] << 8);
        out[o++] = b64tab[(v >> 18) & 63];
        out[o++] = b64tab[(v >> 12) & 63];
        out[o++] = b64tab[(v >> 6) & 63];
        out[o++] = '=';
    }
    out[o] = 0;
    printf("FBTHUMB %dx%d %s\n", THUMB_W, THUMB_H, out);
}

/* FNV-1a over the visible framebuffer. Deterministic across runs -> usable as
 * a golden value checked into the repo (a hex string, not an image file). */
static uint32_t fb_hash(void) {
    uint32_t h = 2166136261u;
    const uint8_t *p = (const uint8_t *)vram_s;
    int n = FB_W * FB_H * 2;
    int i;
    for(i = 0; i < n; i++) {
        h ^= p[i];
        h *= 16777619u;
    }
    return h;
}

static void fb_report(int frame) {
    static uint8_t thumb[THUMB_W * THUMB_H * 2];
    int x, y, o = 0;
    for(y = 0; y < THUMB_H; y++) {
        for(x = 0; x < THUMB_W; x++) {
            int sx = (x * FB_W) / THUMB_W + (FB_W / THUMB_W) / 2;
            int sy = (y * FB_H) / THUMB_H + (FB_H / THUMB_H) / 2;
            uint16_t px = vram_s[sy * FB_W + sx];
            thumb[o++] = (uint8_t)(px & 0xff);        /* RGB565, little endian */
            thumb[o++] = (uint8_t)((px >> 8) & 0xff);
        }
    }
    printf("MARK:FRAME %d\n", frame);
    printf("FBHASH %08lx\n", (unsigned long)fb_hash());
    b64_emit(thumb, sizeof(thumb));
}

/* Deterministic CPU-drawn test pattern. Chosen so that a wrong pixel format,
 * wrong stride or wrong vram base all change the hash. */
static void draw_pattern(int frame) {
    int x, y;
    for(y = 0; y < FB_H; y++) {
        for(x = 0; x < FB_W; x++) {
            int r = ((x + frame * 8) >> 3) & 31;
            int g = (y >> 3) & 63;
            int b = ((x ^ y) >> 4) & 31;
            vram_s[y * FB_W + x] = (uint16_t)((r << 11) | (g << 5) | b);
        }
    }
}

static int failures = 0;
static void check(const char *name, int ok) {
    printf("ASSERT %s %s\n", ok ? "ok" : "fail", name);
    if(!ok) failures++;
}

int main(int argc, char **argv) {
    uint64_t t0, t1;
    volatile uint32_t acc = 0;
    uint32_t i;
    int frame;

    (void)argc;
    (void)argv;

    scif_set_parameters(HARNESS_BAUD, 1);
    scif_init();
    dbgio_dev_select("scif");

    printf("OC-DC-HARNESS-BEGIN\n");
    printf("MARK:BOOT_OK\n");

    /* Hardware-contract assertions. If a harness ever runs with the 32 MB RAM
     * mod enabled by mistake, this is where it gets caught. */
    check("ptr_is_32bit", sizeof(void *) == 4);
    check("little_endian", (*(const uint16_t *)"\x01\x02") == 0x0201);
    check("char_is_signed", (char)-1 < 0);
    /* RAM-size probe: on a stock 16 MB machine, area-3 RAM mirrors, so
     * 0x8D000000 reads back as 0x8C000000.  With the 32 MB mod they differ.
     * Read-only, so it cannot corrupt anything.  Reported as MEM (informational)
     * rather than ASSERT because the mirror is not guaranteed to be modelled by
     * every emulator; the authoritative 16 MB enforcement is the host-side
     * -config config:Dreamcast.RamMod32MB=no that every harness passes. */
    {
        volatile const uint32_t *lo = (volatile const uint32_t *)0x8C000000u;
        volatile const uint32_t *hi = (volatile const uint32_t *)0x8D000000u;
        printf("MEM ram_mirror=%s lo=%08lx hi=%08lx\n",
               (*lo == *hi) ? "yes" : "no",
               (unsigned long)*lo, (unsigned long)*hi);
    }

    /* Deterministic emulated-time measurement: same number every run. */
    t0 = timer_us_gettime64();
    for(i = 0; i < 1000000u; i++) acc += i;
    t1 = timer_us_gettime64();
    printf("PERF loop1m_us=%lu acc=%lu\n", (unsigned long)(t1 - t0),
           (unsigned long)acc);

    /* Framebuffer path: draw, then report hash + thumbnail per frame. */
    vid_set_mode(DM_640x480, PM_RGB565);
    for(frame = 0; frame < 3; frame++) {
        draw_pattern(frame);
        vid_waitvbl();
        fb_report(frame);
    }

    printf("OC-DC-HARNESS-END rc=%d\n", failures ? 1 : 0);

    /* Sit still; the host runner kills us on the END marker. Returning from
     * main lands in KOS shutdown, which prints extra noise. */
    for(;;) thd_sleep(1000);
    return failures ? 1 : 0;
}
