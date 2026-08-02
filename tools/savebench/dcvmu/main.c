/* dcvmu — guest-side proof that the VMU save backend works.
 *
 * WHY THIS EXISTS AND IS NOT PART OF THE GAME BUILD
 * -------------------------------------------------
 * The game image is 21.4 MB against a 16 MB machine (kb/STATE.md), so it does
 * not execute a single instruction — a build of it produces zero bytes of
 * console output and there is no way to see whether dc_card.c works. This is a
 * ~40 KB KOS program that links the SAME dc/src/dc_card.c with
 * -DDC_CARD_STANDALONE, boots in Flycast, and runs dc_card_selftest() against
 * the emulator's VMUs. When the image fits, the identical self-test runs from
 * CARDInit() with no code change.
 *
 * It is a real write to a real (emulated) VMU: Flycast persists it to
 * <run-dir>/home/.flycast/data/vmu_save_A1.bin, so the round-trip is going
 * through vmufs, the FAT, the directory and the maple bus, not a memcpy.
 *
 * ⚠ Any TIMING this prints is emulator timing. Flycast does not model VMU
 * flash-write latency, so it says nothing about the 10 ms/block floor or the
 * ~83 ms/block ceiling in kb/save-budget.md §5. Only a real VMU settles that.
 *
 * Harness protocol (harness/dc/README.md): BEGIN, typed one-line records,
 * END rc=<n>. HARD RULE: never call scif_flush() — it permanently kills the
 * Flycast console (kb/traps.md).
 */
#include <kos.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <dc/scif.h>
#include <dc/maple.h>
#include <dc/vmufs.h>
#include <arch/timer.h>

#include "dc_card.h"

/* Emulator-only: 50 MHz / 32 is the SH-4 SCIF maximum. A real coder's cable
 * will not sync here, so this stays behind the harness build. */
#define HARNESS_BAUD 1562500

static int failures = 0;

static void check(const char *name, int ok) {
    if(!ok) failures++;
    printf("ASSERT %s %s\n", ok ? "ok" : "fail", name);
}

/* The same three-regime pattern dc_card.c's self-test uses: a run of zeros
 * (sector padding, empty slots), low-entropy text (letters, diary) and PRNG
 * bytes (design art at its worst). A save is all three, and a test against
 * only one of them proves the wrong thing. */
static void fill(unsigned char *p, unsigned n, unsigned seed) {
    unsigned i, s = seed ? seed : 1;
    for(i = 0; i < n; i++) {
        if(i < n / 3)          p[i] = 0;
        else if(i < 2 * n / 3) p[i] = (unsigned char)('a' + (i % 26));
        else { s = s * 1103515245u + 12345u; p[i] = (unsigned char)(s >> 16); }
    }
}

/* Round-trip an arbitrary size through the real card and report it. This is
 * the measurement kb/save-budget.md §5 asks for: real packed size and real
 * block count for a known payload, plus the emulator's write cost. */
static void roundtrip(unsigned n, const char *label) {
    unsigned char *src = malloc(n);
    unsigned char *back = malloc(n);
    uint64_t t0, tw, tr;
    unsigned got = 0;
    int rc;
    char nm[16];

    snprintf(nm, sizeof(nm), "OXRT.TMP");

    if(!src || !back) {
        printf("MARK:SKIP_%s malloc_failed\n", label);
        free(src); free(back);
        return;
    }
    fill(src, n, 0xACAC0000u + n);
    memset(back, 0xA5, n);

    t0 = timer_us_gettime64();
    rc = dc_save_store(0, nm, src, n);
    tw = timer_us_gettime64() - t0;
    if(rc != DC_SAVE_OK) {
        printf("MARK:RT_%s store_rc=%d %s\n", label, rc, dc_save_strerror(rc));
        check(label, 0);
        free(src); free(back);
        return;
    }

    t0 = timer_us_gettime64();
    rc = dc_save_load(0, nm, back, n, &got);
    tr = timer_us_gettime64() - t0;

    printf("PERF rt_%s_bytes=%u write_us=%lu read_us=%lu\n", label, n,
           (unsigned long)tw, (unsigned long)tr);
    check(label, rc == DC_SAVE_OK && got == n && memcmp(src, back, n) == 0);
    if(rc == DC_SAVE_OK && got == n && memcmp(src, back, n))
        printf("MARK:RT_%s BYTES_DIFFER\n", label);

    dc_save_erase(0, nm);
    free(src);
    free(back);
}

int main(int argc, char **argv) {
    maple_device_t *dev;
    int i, nvmu = 0;

    (void)argc; (void)argv;

    scif_set_parameters(HARNESS_BAUD, 1);
    scif_init();
    dbgio_dev_select("scif");

    printf("OC-DC-HARNESS-BEGIN\n");
    printf("MARK:BOOT_OK\n");

    for(i = 0; (dev = maple_enum_type(i, MAPLE_FUNC_MEMCARD)) != NULL; i++) {
        printf("MEM vmu%d=%c%d free_blocks=%d\n", i, 'A' + dev->port,
               dev->unit, vmufs_free_blocks(dev));
        nvmu++;
    }
    printf("MARK:VMU_COUNT %d\n", nvmu);

    /* A console with no memory card is a console, not a failure. If the
     * harness ever runs without VMUs we still want a clean END. */
    if(nvmu == 0) {
        printf("MARK:NO_VMU\n");
        check("no_vmu_is_not_a_crash", dc_save_load(0, NULL, NULL, 0, NULL)
                                       == DC_SAVE_ENODEV);
        printf("OC-DC-HARNESS-END rc=%d\n", failures ? 1 : 0);
        for(;;) thd_sleep(1000);
    }

    /* The self-test that the game build runs from CARDInit(). Same code. */
    dc_card_selftest();

    /* Sizes that mean something:
     *   8192   = one GameCube memory-card sector (mCD_MEMCARD_SECTORSIZE)
     *   47780  = mCD_keep_mail_c, the smallest of the three keep-blocks
     *   102400 = the entire VMU user area, i.e. the fit boundary itself */
    roundtrip(8192,   "sector8k");
    roundtrip(47780,  "keepmail");
    roundtrip(102400, "vmufull100k");

    printf("OC-DC-HARNESS-END rc=%d\n", failures ? 1 : 0);

    /* Sit still; the host runner kills us on the END marker. Returning from
     * main lands in KOS shutdown, which prints extra noise. */
    for(;;) thd_sleep(1000);
    return failures ? 1 : 0;
}
