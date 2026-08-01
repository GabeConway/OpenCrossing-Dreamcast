/* Deliberately crashing guest program. Used to prove the harness fail path:
 * it must exit non-zero, must NOT hang until the timeout, and must capture the
 * whole KOS "Unhandled exception" register dump into the console log so that
 * sh-elf-addr2line can symbolise the faulting PC on the host.
 *
 * NOTE (verified, see kb/design-harness.md §6): Flycast does NOT trap unaligned
 * accesses or writes to address 0, under either the dynarec or the interpreter.
 * An illegal instruction is the only reliable crash canary here.
 */

#include <kos.h>
#include <stdio.h>
#include <dc/scif.h>

#define HARNESS_BAUD 1562500

int main(int argc, char **argv) {
    (void)argc;
    (void)argv;

    scif_set_parameters(HARNESS_BAUD, 1);
    scif_init();
    dbgio_dev_select("scif");

    printf("OC-DC-HARNESS-BEGIN\n");
    printf("MARK:BOOT_OK\n");
    printf("MARK:ABOUT_TO_CRASH\n");
    scif_flush();

    /* Illegal instruction -> SH-4 general illegal instruction exception. */
    __asm__ __volatile__(".word 0xfffd");

    printf("OC-DC-HARNESS-END rc=0\n"); /* must never be reached */
    return 0;
}
