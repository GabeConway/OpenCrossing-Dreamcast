/* Deliberately crashing guest program. Used to prove the harness fail path:
 * it must exit non-zero, must NOT hang until the timeout, and must capture the
 * whole KOS "Unhandled exception" register dump into the console log so that
 * sh-elf-addr2line can symbolise the faulting PC on the host.
 *
 * NOTE (verified, see harness/dc/README.md): Flycast does NOT trap unaligned
 * accesses or writes to address 0, under either the dynarec or the interpreter.
 * An illegal instruction is the only reliable crash canary here.
 *
 * NOTE 2 (verified 2026-08-01, KOS 2.3.0 + Flycast v2.6): NEVER call
 * scif_flush() from guest code. KOS's scif_flush() clears TEND and then spins
 * waiting for it to come back; Flycast never re-raises TEND on an already-idle
 * TX FIFO, so the spin times out and KOS latches `serial_enabled = 0` --
 * silently killing ALL further serial output, including the panic register
 * dump. A crash after an explicit scif_flush() is invisible to the harness and
 * shows up only as a wall-clock timeout. printf()/scif_write_buffer() flush on
 * their own; you never need the explicit call.
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

    /* Illegal instruction -> SH-4 general illegal instruction exception. */
    __asm__ __volatile__(".word 0xfffd");

    printf("OC-DC-HARNESS-END rc=0\n"); /* must never be reached */
    return 0;
}
