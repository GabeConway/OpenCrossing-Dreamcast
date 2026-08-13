/* dc_profdump.c — P2, gprof off a burned CD-R via the serial console.
 *
 * Read dc/include/dc_profdump.h first: it carries WHY this exists and the two
 * non-obvious facts (link-line-only `-pg`; the mandatory `gprof_init()`
 * override). This file is the mechanism.
 *
 * =========================================================================
 * THE SINK: A VFS THAT STORES NOTHING
 * =========================================================================
 * KOS's libgprof writes gmon.out with plain stdio, in exactly three opens
 * (verified in addons/libgprof/gmon.c):
 *
 *   1. monstartup()      : fopen(path,"w")  -> 20-byte gmon_hdr_t, fclose
 *   2. write_histogram() : fopen(path,"a")  -> 1-byte tag, gmon_hist_hdr_t,
 *                                              ncounters * uint16_t, fclose
 *   3. write_arcs()      : fopen(path,"a")  -> the call-graph arcs, fclose
 *
 * gmon.out is that byte sequence CONCATENATED, so a sink that ignores the
 * filename and the mode and simply appends every byte it is handed, in call
 * order, is a bit-exact gmon.out writer. That is all `prof_write()` does.
 *
 * ⚠️ Open (1) happens AT BOOT and opens (2)/(3) happen thousands of frames
 * later, so the encoded stream is INTERLEAVED with the game's own console
 * output. That is by design and the framing is what makes it recoverable:
 * every payload line carries a monotonic index, so the host decoder greps the
 * prefix out of a noisy console.log, checks the indices are contiguous, and
 * concatenates. Nothing is buffered on the Dreamcast — zero extra RAM is the
 * entire point of this file, because the profile buffers themselves are
 * already 1.5 MB against ~499 KB of headroom (kb/STATE.md).
 *
 * With no `-pg` on any translation unit, step (3) writes NOTHING: `froms[]` is
 * all zeros, so `write_arcs()` opens the file, iterates, and closes. The arcs
 * half of monstartup()'s allocation is therefore dead weight — see THE MEMORY
 * PROBLEM below, and tools/dcprof/README.md for what to do about it.
 *
 * =========================================================================
 * THE WIRE FORMAT — "z0b64"
 * =========================================================================
 * A raw histogram is mostly zeros (one uint16 per 8 bytes of .text, and most
 * of .text never executes in a 30-second town run). Base64 alone would put
 * ~836 KB down a 57,600-baud line: 145 seconds of serial, during which the
 * thing being profiled is not running. So the raw stream is RLE'd FIRST, on
 * zero runs only, and the result is base64'd.
 *
 * ⭐ **MEASURED 2026-08-12, and it is the reason this is affordable at all:
 * 612,433 raw bytes -> 1,585 encoded -> 28 console lines.** The 145-second
 * estimate above is what the naive design would have cost; the real dump is
 * imperceptible. Encoding:
 *
 *   z0 encoding, byte by byte:
 *     b != 0x00                   -> emit b verbatim
 *     a run of N zero bytes       -> emit 0x00, then N as LEB128
 *                                    (7 bits per byte, low group first,
 *                                     high bit set on every byte but the last)
 *
 *   0x00 is the ONLY escape, N >= 1 always, so the decode is unambiguous and
 *   one pass. A lone zero costs 2 bytes; a 600,000-zero run costs 4.
 *
 * Framing (all lines start with the DC_GPROF_TAG prefix):
 *
 *   [GPROF] BEGIN v=1 enc=z0b64 cols=<n> low=<hex> high=<hex> alloc=<bytes>
 *   [GPROF] <idx:06x> <base64 chars> <cksum:02x>
 *   ...
 *   [GPROF] END lines=<n> raw=<bytes> enc=<bytes> crc32=<hex>
 *
 *   idx    monotonic from 0. A gap means the console dropped a line and the
 *          decoder must refuse to write a half file.
 *   cksum  sum of that line's base64 characters, mod 256. Names the corrupt
 *          line instead of failing the whole run's CRC with no location.
 *   raw    bytes BEFORE z0 — i.e. the size of gmon.out. The decoder's contract.
 *   crc32  CRC-32 (reflected, 0xEDB88320) over those same raw bytes.
 *
 * =========================================================================
 * THE MEMORY PROBLEM, STATED IN THE NUMBERS THAT MATTER
 * =========================================================================
 * monstartup() allocates, for a range of `T` bytes (gmon.c:440-470):
 *
 *     histogram  T/8  * 2   = 0.250 * T     (HISTFRACTION 8, uint16 bins)
 *     froms      T/16 * 2   = 0.125 * T     (HASHFRACTION 16)
 *     nodes      T*2/100 * 12 = 0.240 * T   (ARCDENSITY 2, 12-byte node,
 *                                            capped at MAX_NODES 65535)
 *                            ~= 0.615 * T
 *
 * MEASURED on dc/build/AnimalCrossing.elf (2026-08-09 link):
 *   __executable_start 0x8c010000, __etext 0x8c274062  ->  T = 2,506,864
 *   histogram 626,716 + froms 313,358 + nodes 601,644  ->  1,541,792 B.
 *
 * Against ~499,088 B of real headroom (kb/STATE.md) that DOES NOT FIT **at the
 * keeplist-FULL content level**, and a profiling build must buy the difference.
 * A profiling build is a MEASUREMENT build, not the shipping one, so it may.
 *
 * ✅ **SETTLED 2026-08-12: keeplist-town.txt alone pays for it.** Measured on a
 * real boot, ARAM window left at 1 MB, F5 off:
 *
 *   [GPROF] arming: range 8c010000..8c266062, predicted alloc 1506528 B
 *   [GPROF] Total memory allocated: 1506528 bytes      <- gmon.c's own line
 *
 * 🔴 **DO NOT reach for DC_ARAM_WINDOW=131072** — it is the lever the kb used
 * to recommend here and it takes disc reads 106 -> 4,183, which would dominate
 * the profile. DC_GPROF_SPAN is the reserve if content ever grows back.
 * ⚠️ And note SPAN's rationale is weaker than it reads: a profiling build runs
 * with F5 OFF (kb/RESUME.md §6b), so the low end of .text is NOT the packed
 * draw loop and a narrow window is not obviously the hot one.
 *
 * ⚠️ THE HISTOGRAM IS SILENT ABOUT PCs OUTSIDE ITS RANGE. `histogram_callback`
 * drops any sample with pc < lowpc || pc >= highpc — it does not count it
 * anywhere. So a narrowed range does not merely lose resolution, it changes
 * the DENOMINATOR: percentages are of samples-in-range, not of the frame. Say
 * so when quoting a narrowed run, and prefer comparing narrowed-vs-narrowed.
 */

#include "dc_platform.h"
#include "dc_profdump.h"

#if defined(DC_GPROF) && DC_GPROF > 0 && !defined(DC_HOST_STUB)

#include <kos/fs.h>
#include <kos/nmmgr.h>
#include <kos/dbgio.h>
#include <gprof/gmon.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <dc/maple.h>
#include <dc/maple/controller.h>

#ifndef DC_GPROF_SD
#define DC_GPROF_SD 0
#endif

#if (DC_GPROF_SD) > 0
#include <dc/sd.h>
#include <fat/fs_fat.h>
#endif

/* ------------------------------------------------------------------ knobs */

/* Presented frames before the dump fires. 1800 is ~90 s at 20 FPS — long
 * enough that boot and the first scene load are a small fraction, short enough
 * that a burn is not an afternoon. 0 disables the automatic dump entirely
 * (leaving DC_GPROF_FLUSH() as the only trigger). */
#ifndef DC_GPROF_DUMP_FRAME
#define DC_GPROF_DUMP_FRAME 1800
#endif

/* Base64 characters per payload line. 76 -> 57 encoded bytes per line, and a
 * whole line including the prefix and checksum is 91 characters, which fits a
 * terminal and any reasonable log viewer. */
#ifndef DC_GPROF_COLS
#define DC_GPROF_COLS 76
#endif

/* Line prefix. Kept short because every byte of it is serial time, and kept
 * distinctive because the decoder greps for it in a log full of game output. */
#define DC_GPROF_TAG "[GPROF] "

/* Mount point for the console sink. gmon.c builds its path from
 * GMON_OUT_PREFIX, so this only has to agree with DC_GPROF_PREFIX below. */
#ifndef DC_GPROF_MOUNT
#define DC_GPROF_MOUNT "/prof"
#endif

/* DC_GPROF_PREFIX — where gmon.out goes. gmon.c appends ".<pid>".
 *
 * The default routes it to this file's console sink. Set it to something else
 * ("/sd/gmon" once the SD adapter lands — kb/hardware-profiling.md §4) and the
 * /prof VFS is NOT mounted at all; gmon.c's fopen then goes to whatever real
 * filesystem owns that path.
 *
 * ⚠️ AND THAT FILESYSTEM MUST ALREADY BE MOUNTED WHEN gprof_init() RUNS, which
 * is before constructors and before main(). There is no later hook: monstartup()
 * opens the file before it returns. So an /sd sink means bringing SD up right
 * here — sd_init(), sd_blockdev_for_partition(), fs_fat_init(), fs_fat_mount()
 * — inside the marked seam in gprof_init(), not in dc_main.c. Flycast has no SD
 * adapter either way, so the console sink stays the only path that works on
 * BOTH targets, and therefore the only one an A/B can use. */
#ifndef DC_GPROF_PREFIX
#define DC_GPROF_PREFIX DC_GPROF_MOUNT "/gmon"
#endif

/* DC_GPROF_HZ — override the KOS scheduler rate for the profiling run.
 *
 * ⚠️ THIS IS THE SAMPLE-RATE KNOB AND THE DEFAULT IS LOW. libgprof samples one
 * PC per reschedule, and KOS's default is THD_SCHED_HZ = 100
 * (kernel/arch/dreamcast/include/arch/arch.h:72). A 90-second run therefore
 * yields on the order of 9,000 samples — enough to rank the top twenty
 * functions, not enough to trust a 1 % entry. thd_set_hz(1000) is the maximum
 * KOS accepts (thread.c:1049 rejects > 1000) and gives 10x that.
 *
 * The cost is 10x the scheduler entries, which is itself frame time and lands
 * in the profile as scheduler work. That perturbation is the same on both
 * targets, so it largely cancels in a Flycast-vs-hardware DIFF while it does
 * NOT cancel in an absolute FPS number — never quote FPS from a DC_GPROF_HZ
 * run. 0 leaves KOS's default alone. */
#ifndef DC_GPROF_HZ
#define DC_GPROF_HZ 0
#endif

/* ==========================================================================
 * DC_GPROF_SD — the SD-card sink, and WHY IT IS BOLTED ON AT DUMP TIME
 * ==========================================================================
 * The console sink above works on both targets but costs 57,600 baud of serial
 * for ~700 KB, and on a burned CD-R the SD adapter and the coder's cable are
 * the same physical port, so you cannot have both. With DC_GPROF_SD=1 the
 * histogram goes to a FAT32 card instead: no encoding, no console tax, and a
 * file you can read on the host with sh-elf-gprof directly.
 *
 * ⭐ THE CARD IS BROUGHT UP AT DUMP TIME, NOT IN gprof_init(), AND THAT IS THE
 * WHOLE DESIGN. Three separate reasons, each of which on its own would decide
 * it:
 *
 *   1. monstartup() runs BEFORE main() and allocates ~1.54 MB. Deferring the
 *      allocation to mid-run would put it against a fragmented heap in a build
 *      whose entire difficulty is RAM. So the ALLOCATION stays at boot.
 *   2. The SCIF reader re-configures SCIF for bit-banged SPI, which collides
 *      with KOS's SCIF console. Bringing SD up at boot would therefore mean
 *      muting the console at boot — and muting at main() is the documented way
 *      to stop this port booting at all (kb/traps.md; it cost the 2026-08-08
 *      `-f` burn). At dump time the measurement is already over, so the
 *      collision is free.
 *
 * 🔴 3. AND THE ONE THAT DECIDED THE SHAPE: **gmon.c CANNOT BE ALLOWED TO
 *      fopen() THE CARD AT ALL.** gmon.c opens gmon.out with mode "a" for both
 *      the histogram and the arcs, and newlib turns "a" into
 *      O_WRONLY|O_CREAT|O_APPEND. O_APPEND is 0x08
 *      (sh-elf/include/sys/_default_fcntl.h) and KOS's O_MODE_MASK is 0x0f
 *      (kos/fs.h), so O_APPEND lands INSIDE the mode nibble — and
 *      fs_fat_write() (addons/libkosfat/fs_fat.c) opens with
 *          mode = fh[fd].mode & O_MODE_MASK;
 *          if(mode != O_WRONLY && mode != O_RDWR) { errno = EBADF; return -1; }
 *      0x9 is neither, so EVERY append write to a FAT32 card fails with EBADF.
 *      ⚠️ AND IT FAILS SILENTLY IN THE WORST WAY: fs_fat_open() does not check
 *      the mode, so the fopen SUCCEEDS; only the writes fail, gmon.c reports
 *      them through dbglog into a console this code has already disabled, and
 *      you come home from the burn with a 20-byte gmon.out and a receipt
 *      cheerfully saying "20 bytes". ("w" is unaffected — 0x601 & 0x0f == 1 ==
 *      O_WRONLY — which is exactly why the trap is invisible until the burn.)
 *
 * So the sink is NOT switched by GMON_OUT_PREFIX. gmon.c writes to /prof for
 * the whole run, exactly as it always did, and it is THIS FILE'S VFS that
 * decides where the bytes land: console-encoded when there is no card, or
 * fwrite()n straight into an SD file that we opened ourselves in "w" mode and
 * hold open. One writer, one mode, no append.
 *
 * The cost is that monstartup()'s 20-byte gmon header has already gone down the
 * CONSOLE by the time the card comes up, so dc_prof_sd_open() writes those 20
 * bytes to the card itself. They are a constant — the cookie "gmon", version 1
 * little-endian, twelve zero bytes — verified against gmon.c's gmon_hdr_t, and
 * s_raw_bytes is already 20 at that point so prof_tell() stays consistent.
 *
 * ⚠️ IF THE CARD DOES NOT ENUMERATE, THE RUN FALLS BACK TO THE CONSOLE SINK
 * AND SAYS SO. One CDI is then correct with or without a working card, which
 * matters because a burn is a 15-minute round trip.
 *
 *   DC_GPROF_SD_IF   0 = SCIF only, 1 = SCI only, 2 = try SCIF then SCI.
 *                    The jj1odm reader is SCIF and SWAT's is SCI; both hang off
 *                    the same connector, so 2 (the default) probes rather than
 *                    asking the human to know which one they bought.
 */
#ifndef DC_GPROF_SD_MOUNT
#define DC_GPROF_SD_MOUNT "/sd"
#endif

#ifndef DC_GPROF_SD_PREFIX
#define DC_GPROF_SD_PREFIX DC_GPROF_SD_MOUNT "/gmon"
#endif

/* Human-readable receipt, written next to gmon.out. On a burn with the adapter
 * in the serial port there is NO console, so this file is the only channel that
 * can say "it armed, it sampled, here is the range" — the same reason
 * kb/traps.md insists a screen-only image puts a liveness line on the screen. */
#ifndef DC_GPROF_SD_NOTE
#define DC_GPROF_SD_NOTE DC_GPROF_SD_MOUNT "/gprof.txt"
#endif

/* 🔴 DEFAULT IS 0 = SCIF ONLY, AND PROBING SCI IS AN OPT-IN THAT CAN HANG THE
 * MACHINE FOREVER. This was 2 ("try SCIF then SCI") for exactly one session and
 * it cost four Flycast runs and a burn's worth of confusion.
 *
 * KOS's `sci_spi_rw_byte` (kernel/arch/dreamcast/hardware/sci.c) waits for
 * RDRF like this:
 *
 *     do {
 *         status = SCSSR1;
 *         if(status & ORER) { ...; return SCI_ERR_OVERRUN; }
 *     } while(!(status & RDRF));
 *
 * — **UNBOUNDED**. Every TDRE wait in that same file is capped by
 * SCI_MAX_WAIT_CYCLES; all four RDRF waits in the SPI helpers are not. The
 * SH4's SCSSR1 reset value is 0x84 (TDRE|TEND set, RDRF and ORER clear), which
 * passes every bounded TDRE gate, lets sci_init() return SCI_OK, and then pins
 * sd_init_ex()'s first spi_rw_byte(0xFF) forever. Nothing wired to SCI will
 * ever set RDRF or ORER, so this is a permanent silent wedge — and it happens
 * with the console ALREADY MUTED (see dc_profdump_flush_now), so it looks
 * exactly like the game hanging.
 *
 * ⚠️ THIS IS NOT A FLYCAST QUIRK. On a real console with the SCIF-type
 * (jj1odm) reader, any SCIF failure — superfloppy-formatted card, bad seat —
 * used to fall through to SCI and wedge the burn with no receipt and no
 * console. The fall-back-to-console promise this file makes is only true if
 * every probe can FAIL; SCIF's bit-banged path can, SCI's cannot.
 *
 *   0  SCIF only (default). jj1odm-style reader. Always fails cleanly.
 *   1  SCI only. SWAT/Rostovtsev-style reader. ⚠️ Can hang forever if nothing
 *      answers. Set it only when you KNOW the adapter is SCI.
 *   2  SCIF then SCI. ⚠️ Inherits (1)'s hang as a fallback path. Do not use
 *      unless KOS's RDRF loops have been given a timeout.
 */
#ifndef DC_GPROF_SD_IF
#define DC_GPROF_SD_IF 0
#endif

/* DC_GPROF_CHORD=0 disables the L+R+START manual dump trigger. On by default:
 * on hardware the frame cap is a guess at how fast a human reaches the town,
 * and this is the only way to say "profile ends HERE". */
#ifndef DC_GPROF_CHORD
#define DC_GPROF_CHORD 1
#endif

/* ⚠️ DC_GPROF_SPAN — profile only the first N bytes of .text instead of all of
 * it, which scales monstartup()'s allocation by N/T. Unset (or 0) means the
 * whole .text. Set it ONLY when the full range will not fit; read the warning
 * about the denominator in the header comment first. DC_GPROF_LOW overrides
 * the base address if the window must start somewhere other than
 * __executable_start (both are plain integers, so pass hex as 0x...). */
#ifndef DC_GPROF_SPAN
#define DC_GPROF_SPAN 0
#endif

/* ------------------------------------------------- the .text range symbols */

/* Same asm-label trick libgprof uses (gmon.c:61): name the linker symbols
 * literally so no user-label prefix can be inserted. */
extern char dc_prof_text_start __asm__("__executable_start");
extern char dc_prof_text_end   __asm__("__etext");

/* ------------------------------------------------------- encoder state */

/* Everything below is static and tiny by contract. Total .bss for the encoder
 * is one line buffer plus ~40 bytes of counters — if this ever grows a real
 * buffer, the file has lost its reason to exist. */
static uint32_t s_raw_bytes;      /* bytes handed in, i.e. sizeof(gmon.out) */
static uint32_t s_enc_bytes;      /* bytes after z0                         */
static uint32_t s_crc;            /* running CRC-32 over the RAW bytes      */
static uint32_t s_lines;          /* payload lines emitted                  */
static uint32_t s_zrun;           /* pending zero-run length (z0 state)     */

static uint8_t  s_b64acc[3];      /* base64 carry                           */
static uint8_t  s_b64n;

static char     s_line[DC_GPROF_COLS + 1];
static uint8_t  s_linen;

static uint8_t  s_begun;          /* BEGIN emitted                          */
static uint8_t  s_ended;          /* END emitted; every further byte dropped */
static uint8_t  s_armed;          /* monstartup() succeeded                 */

static uintptr_t s_lowpc, s_highpc;
static uint32_t  s_alloc_pred;

#if (DC_GPROF_SD) > 0
/* Declared up here, not down in the SD section, because prof_write() is defined
 * above it and prof_write() is where the sink actually switches. */
static FILE     *s_sd_file;     /* non-NULL => bytes go to the card, not dbgio */
static uint32_t  s_sd_err;      /* short writes; reported in the receipt       */
#endif

/* ------------------------------------------------------------- CRC-32 */

/* Bitwise, no 1 KB table: this runs once per byte of a ~700 KB stream on a
 * 200 MHz SH-4 that is otherwise idle (the game is stalled inside _mcleanup),
 * and a table is 1,024 B of .bss in a build whose whole difficulty is .bss. */
static uint32_t dc_prof_crc32_byte(uint32_t crc, uint8_t b) {
    int k;
    crc ^= b;
    for (k = 0; k < 8; k++)
        crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    return crc;
}

/* ------------------------------------------------- console line emission */

/* dbgio_write_str, NOT printf/DC_LOGE. Two reasons, both load-bearing:
 *   1. RE-ENTRANCY. This is called from inside newlib's fwrite() on the /prof
 *      stream. Going back into stdio for the output would nest one FILE inside
 *      another under the same allocator and the same reentrancy struct.
 *      dbgio is the layer underneath stdout and takes no such lock.
 *   2. THE FLOOD LIMITER. dc_misc.c overrides printf/vprintf with a
 *      per-format-string backoff. This emits thousands of lines from ONE call
 *      site, which is exactly the shape that limiter exists to suppress. */
static void dc_prof_emit(const char *s) {
    dbgio_write_str(s);
}

static void dc_prof_flush_line(void) {
    char out[16 + DC_GPROF_COLS + 8];
    unsigned int sum = 0;
    unsigned int i;

    if (s_linen == 0) return;
    s_line[s_linen] = '\0';

    for (i = 0; i < s_linen; i++)
        sum += (unsigned char)s_line[i];

    snprintf(out, sizeof(out), DC_GPROF_TAG "%06lx %s %02x\n",
             (unsigned long)s_lines, s_line, (unsigned int)(sum & 0xFFu));
    dc_prof_emit(out);

    s_lines++;
    s_linen = 0;
}

static void dc_prof_put_char(char c) {
    s_line[s_linen++] = c;
    if (s_linen >= DC_GPROF_COLS)
        dc_prof_flush_line();
}

/* -------------------------------------------------------------- base64 */

static const char dc_prof_b64[] =
    "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static void dc_prof_b64_byte(uint8_t b) {
    s_b64acc[s_b64n++] = b;
    if (s_b64n == 3) {
        uint32_t v = ((uint32_t)s_b64acc[0] << 16) |
                     ((uint32_t)s_b64acc[1] << 8) |
                      (uint32_t)s_b64acc[2];
        dc_prof_put_char(dc_prof_b64[(v >> 18) & 0x3F]);
        dc_prof_put_char(dc_prof_b64[(v >> 12) & 0x3F]);
        dc_prof_put_char(dc_prof_b64[(v >>  6) & 0x3F]);
        dc_prof_put_char(dc_prof_b64[ v        & 0x3F]);
        s_b64n = 0;
    }
}

static void dc_prof_b64_finish(void) {
    if (s_b64n == 1) {
        uint32_t v = (uint32_t)s_b64acc[0] << 16;
        dc_prof_put_char(dc_prof_b64[(v >> 18) & 0x3F]);
        dc_prof_put_char(dc_prof_b64[(v >> 12) & 0x3F]);
        dc_prof_put_char('=');
        dc_prof_put_char('=');
    } else if (s_b64n == 2) {
        uint32_t v = ((uint32_t)s_b64acc[0] << 16) |
                     ((uint32_t)s_b64acc[1] << 8);
        dc_prof_put_char(dc_prof_b64[(v >> 18) & 0x3F]);
        dc_prof_put_char(dc_prof_b64[(v >> 12) & 0x3F]);
        dc_prof_put_char(dc_prof_b64[(v >>  6) & 0x3F]);
        dc_prof_put_char('=');
    }
    s_b64n = 0;
}

/* ------------------------------------------------------------------ z0 */

static void dc_prof_enc_byte(uint8_t b) {
    s_enc_bytes++;
    dc_prof_b64_byte(b);
}

static void dc_prof_zflush(void) {
    uint32_t n = s_zrun;
    if (n == 0) return;
    s_zrun = 0;
    dc_prof_enc_byte(0x00);
    while (n >= 0x80u) {
        dc_prof_enc_byte((uint8_t)(0x80u | (n & 0x7Fu)));
        n >>= 7;
    }
    dc_prof_enc_byte((uint8_t)n);
}

static void dc_prof_raw_byte(uint8_t b) {
    s_raw_bytes++;
    s_crc = dc_prof_crc32_byte(s_crc, b);
    if (b == 0) {
        s_zrun++;
        return;
    }
    dc_prof_zflush();
    dc_prof_enc_byte(b);
}

/* ------------------------------------------------------- stream control */

static void dc_prof_begin(void) {
    char hdr[192];
    if (s_begun) return;
    s_begun = 1;
    s_crc   = 0xFFFFFFFFu;

    snprintf(hdr, sizeof(hdr),
             DC_GPROF_TAG "BEGIN v=1 enc=z0b64 cols=%d low=%08lx high=%08lx "
             "alloc=%lu\n",
             (int)DC_GPROF_COLS,
             (unsigned long)s_lowpc, (unsigned long)s_highpc,
             (unsigned long)s_alloc_pred);
    dc_prof_emit(hdr);
}

static void dc_prof_end(void) {
    char trl[160];
    if (!s_begun || s_ended) return;
    s_ended = 1;

    dc_prof_zflush();          /* a trailing zero run is still data          */
    dc_prof_b64_finish();      /* pad the last base64 quantum                */
    dc_prof_flush_line();      /* and the partial line                       */

    snprintf(trl, sizeof(trl),
             DC_GPROF_TAG "END lines=%lu raw=%lu enc=%lu crc32=%08lx\n",
             (unsigned long)s_lines,
             (unsigned long)s_raw_bytes,
             (unsigned long)s_enc_bytes,
             (unsigned long)(s_crc ^ 0xFFFFFFFFu));
    dc_prof_emit(trl);
    dbgio_flush();
}

/* ==========================================================================
 * THE VFS
 * ==========================================================================
 * Only open/close/write/seek/tell/total/fstat/stat are real. Everything else
 * is NULL, which KOS's fs.c turns into the right errno for us — a stub that
 * returns 0 would be a lie, a NULL is an honest ENOSYS.
 *
 * The handle is a constant, not an allocation: there is exactly one stream and
 * it has no per-open state, because the three opens are one logical file. */

#define DC_PROF_HND ((void *)0x70726F66)   /* 'prof', never dereferenced */

static void *prof_open(vfs_handler_t *vfs, const char *fn, int mode) {
    (void)vfs;
    (void)fn;

    /* Not a directory and not readable: this sink only ever goes one way. */
    if (mode & O_DIR) {
        errno = ENOTDIR;
        return NULL;
    }
    if ((mode & O_MODE_MASK) == O_RDONLY) {
        errno = EACCES;
        return NULL;
    }

    /* ⚠️ "w" DOES NOT TRUNCATE HERE, deliberately. gmon.c opens "w" once and
     * "a" twice and expects the concatenation; honouring O_TRUNC would throw
     * away the 20-byte gmon header written at boot. */
    dc_prof_begin();
    return DC_PROF_HND;
}

static int prof_close(void *hnd) {
    (void)hnd;
    /* Deliberately NOT the end of the stream: two more opens are coming. The
     * stream is closed by dc_profdump_flush_now() after _mcleanup() returns. */
    return 0;
}

static ssize_t prof_write(void *hnd, const void *buffer, size_t cnt) {
    const uint8_t *p = (const uint8_t *)buffer;
    size_t i;

    (void)hnd;
    if (s_ended) return (ssize_t)cnt;   /* swallow, do not fail the writer */

#if (DC_GPROF_SD) > 0
    /* ⭐ THE SINK SWITCH. Once the card is up every byte gmon.c hands us goes
     * straight into a file WE opened in "w" mode — see reason 3 in the header
     * block for why gmon.c must never open it itself. Deliberately NOT counted
     * through dc_prof_raw_byte(): the console encoder's CRC and z0 state are
     * meaningless on this path, but s_raw_bytes still has to advance because
     * prof_tell()/prof_seek() answer newlib's append-mode lseek with it. */
    if (s_sd_file) {
        size_t n = fwrite(p, 1, cnt, s_sd_file);
        s_raw_bytes += (uint32_t)n;
        if (n != cnt) s_sd_err++;
        return (ssize_t)cnt;
    }
#endif

    for (i = 0; i < cnt; i++)
        dc_prof_raw_byte(p[i]);

    return (ssize_t)cnt;
}

/* newlib's append mode does an lseek(0, SEEK_END) before its first write, and
 * a -1 there turns into a failed fwrite and a silently truncated profile. The
 * stream is append-only, so every whence answers with the bytes so far. */
static off_t prof_seek(void *hnd, off_t offset, int whence) {
    (void)hnd; (void)offset; (void)whence;
    return (off_t)s_raw_bytes;
}

static off_t prof_tell(void *hnd) {
    (void)hnd;
    return (off_t)s_raw_bytes;
}

static size_t prof_total(void *hnd) {
    (void)hnd;
    return (size_t)s_raw_bytes;
}

/* Reported as a plain regular file so newlib picks full buffering. A character
 * device would make it line-buffered, i.e. one prof_write() per byte. */
static int prof_fstat(void *hnd, struct stat *st) {
    (void)hnd;
    memset(st, 0, sizeof(struct stat));
    st->st_mode  = S_IFREG | S_IWUSR;
    st->st_size  = (off_t)s_raw_bytes;
    st->st_nlink = 1;
    st->st_blksize = 1024;
    return 0;
}

static int prof_stat(vfs_handler_t *vfs, const char *path, struct stat *st,
                     int flag) {
    size_t len = strlen(path);
    (void)vfs; (void)flag;

    memset(st, 0, sizeof(struct stat));
    if (len == 0 || (len == 1 && path[0] == '/')) {
        st->st_mode  = S_IFDIR | S_IWUSR | S_IXUSR;
        st->st_size  = -1;
        st->st_nlink = 2;
    } else {
        st->st_mode  = S_IFREG | S_IWUSR;
        st->st_size  = (off_t)s_raw_bytes;
        st->st_nlink = 1;
    }
    return 0;
}

/* Field order follows kos/fs.h's vfs_handler_t exactly; the comments are the
 * member names so a KOS update that inserts a member is visible as a mismatch
 * rather than as a wild function pointer. */
static vfs_handler_t s_prof_vh = {
    {   /* nmmgr */
        DC_GPROF_MOUNT, /* pathname */
        0,              /* pid                                        */
        0x00010000,     /* version 1.0                                */
        0,              /* flags                                      */
        NMMGR_TYPE_VFS,
        NMMGR_LIST_INIT
    },
    0, NULL,            /* cache, privdata */

    prof_open,          /* open      */
    prof_close,         /* close     */
    NULL,               /* read      */
    prof_write,         /* write     */
    prof_seek,          /* seek      */
    prof_tell,          /* tell      */
    prof_total,         /* total     */
    NULL,               /* readdir   */
    NULL,               /* ioctl     */
    NULL,               /* rename    */
    NULL,               /* unlink    */
    NULL,               /* mmap      */
    NULL,               /* complete  */
    prof_stat,          /* stat      */
    NULL,               /* mkdir     */
    NULL,               /* rmdir     */
    NULL,               /* fcntl     */
    NULL,               /* poll      */
    NULL,               /* link      */
    NULL,               /* symlink   */
    NULL,               /* seek64    */
    NULL,               /* tell64    */
    NULL,               /* total64   */
    NULL,               /* readlink  */
    NULL,               /* rewinddir */
    prof_fstat          /* fstat     */
};

/* The fallback mount. gmon.c's compiled-in default is "/pc/gmon.out", and on a
 * CD-R boot nothing owns /pc — so if setenv() ever fails to take, claiming /pc
 * keeps the profile flowing instead of losing the run to a failed fopen. Only
 * registered when /pc is genuinely free, so a dc-tool session is untouched. */
static vfs_handler_t s_prof_vh_pc;

/* ==========================================================================
 * ARMING — the gprof_init() override
 * ========================================================================== */

static uint32_t dc_prof_predict_alloc(uint32_t textsize) {
    /* Mirrors gmon.c:452-470. It is a LOG LINE, not a contract: if libgprof
     * changes its constants this number goes wrong while the run still works,
     * which is the right way round. gmon.c prints the real figure itself
     * ("[GPROF] Total memory allocated"). */
    uint32_t ncounters = (textsize + 7u) / 8u;          /* HISTFRACTION 8   */
    uint32_t nfroms    = (textsize + 15u) / 16u;        /* HASHFRACTION 16  */
    uint32_t nnodes    = (textsize * 2u) / 100u;        /* ARCDENSITY 2     */
    uint32_t a, b, c;
    if (nnodes > 65535u) nnodes = 65535u;               /* MAX_NODES        */
    a = (ncounters * 2u + 31u) & ~31u;
    b = (nfroms * 2u + 31u) & ~31u;
    c = (nnodes * 12u + 31u) & ~31u;                    /* sizeof(gmon_node_t) */
    return a + b + c + 32u;
}

/* ⚠️ STRONG OVERRIDE of libgprof's gprof_init() (gmon.c:549), called by KOS
 * from arch_main() before constructors and before main()
 * (kernel/arch/dreamcast/kernel/init.c:309). See dc_profdump.h for why this
 * cannot be done any later. At this point arch_auto_init() has run, so the
 * VFS, the thread scheduler and malloc are all up — the same preconditions
 * libgprof's own gprof_init() already relies on. */
void gprof_init(void) {
    char msg[224];
    uint32_t textsize;

    /* ⚠️ STAGE MARKERS, AND THEY EARN THEIR KEEP. This function runs before
     * main() and before constructors, so a hang here is a SILENT boot failure:
     * the log simply stops after KOS's own vid_set_mode line with nothing to
     * say which of setenv/nmmgr/thd_set_hz/monstartup swallowed it. That is
     * exactly what the first DC_GPROF=1 image did (2026-08-12). Six emits at
     * ~20 bytes each are nothing next to a 15-minute build-and-smoke round
     * trip. They are unconditional on purpose — there is no knob to lose them
     * behind, because the run where you want them is the run that has already
     * gone wrong. */
    dbglog(DBG_INFO, "[GPROF] g0\n");

    /* Mount the sink FIRST: monstartup() opens the file before it returns.
     *
     * ⚠️ THE SD SINK IS NOT BROUGHT UP HERE, DELIBERATELY. It is switched in at
     * dump time instead, because gmon.c re-reads GMON_OUT_PREFIX on every
     * write — see the DC_GPROF_SD block above for the three reasons. So even a
     * DC_GPROF_SD=1 build mounts /prof here and takes monstartup()'s 20-byte
     * header down the console; only the ~700 KB histogram goes to the card. */
    if (strncmp(DC_GPROF_PREFIX, DC_GPROF_MOUNT "/",
                sizeof(DC_GPROF_MOUNT)) == 0) {
        nmmgr_handler_add(&s_prof_vh.nmmgr);
    }

    dbglog(DBG_INFO, "[GPROF] g1 nmmgr\n");

    if (setenv("GMON_OUT_PREFIX", DC_GPROF_PREFIX, 1) != 0 ||
        getenv("GMON_OUT_PREFIX") == NULL) {
        /* setenv failed (no heap yet, or a newlib without a writable environ).
         * Claim libgprof's compiled-in default path instead. */
        if (nmmgr_lookup("/pc") == NULL) {
            memcpy(&s_prof_vh_pc, &s_prof_vh, sizeof(s_prof_vh_pc));
            strcpy(s_prof_vh_pc.nmmgr.pathname, "/pc");
            nmmgr_handler_add(&s_prof_vh_pc.nmmgr);
            dc_prof_emit("[GPROF] setenv failed; sink also mounted at /pc\n");
        } else {
            dc_prof_emit("[GPROF] FATAL: setenv failed and /pc is taken\n");
            return;
        }
    }

    dbglog(DBG_INFO, "[GPROF] g2 setenv\n");

    s_lowpc  = (uintptr_t)&dc_prof_text_start;
    s_highpc = (uintptr_t)&dc_prof_text_end;

#if defined(DC_GPROF_LOW) && (DC_GPROF_LOW) != 0
    s_lowpc = (uintptr_t)(DC_GPROF_LOW);
#endif
#if (DC_GPROF_SPAN) != 0
    s_highpc = s_lowpc + (uintptr_t)(DC_GPROF_SPAN);
#endif
    if (s_highpc <= s_lowpc) {
        dc_prof_emit("[GPROF] FATAL: empty range, refusing to arm\n");
        return;
    }

    textsize = (uint32_t)(s_highpc - s_lowpc);
    s_alloc_pred = dc_prof_predict_alloc(textsize);

    dbglog(DBG_INFO, "[GPROF] g3 range\n");

#if (DC_GPROF_HZ) > 0
    /* Before monstartup(), so the sampler thread is created into the rate it
     * will run at and the profrate written into the gmon header (gmon.c reads
     * thd_get_hz() at WRITE time, not at start time) matches. */
    if (thd_set_hz((unsigned int)(DC_GPROF_HZ)) != 0)
        dc_prof_emit("[GPROF] thd_set_hz rejected; leaving KOS default\n");
#endif

    dbglog(DBG_INFO, "[GPROF] g4 hz\n");

    snprintf(msg, sizeof(msg),
             "[GPROF] arming: range %08lx..%08lx (%lu B), predicted alloc "
             "%lu B, hz=%u, dump at frame %d\n",
             (unsigned long)s_lowpc, (unsigned long)s_highpc,
             (unsigned long)textsize, (unsigned long)s_alloc_pred,
             thd_get_hz(), (int)DC_GPROF_DUMP_FRAME);
    dc_prof_emit(msg);

    /* This allocates the buffers, writes the 20-byte header through our sink,
     * spawns the sampler thread and turns profiling on. Everything after this
     * line is being profiled. */
    monstartup(s_lowpc, s_highpc);
    dbglog(DBG_INFO, "[GPROF] g5 monstartup returned\n");
    s_armed = 1;
}

/* ==========================================================================
 * THE SD SINK
 * ========================================================================== */

#if (DC_GPROF_SD) > 0

/* File scope, not a local: fs_fat_mount() keeps the pointer it is handed, so a
 * stack blockdev would be a dangling one the moment this function returns. The
 * KOS examples do the same thing for the same reason. */
static kos_blockdev_t s_sd_dev;
static uint8_t        s_sd_up;
static char           s_sd_path[96];

static int dc_prof_sd_try(int iface) {
    sd_init_params_t p;
    uint8_t          ptype = 0;

    p.interface = (iface == 1) ? SD_IF_SCI : SD_IF_SCIF;
    p.check_crc = true;

    if (sd_init_ex(&p) != 0)
        return 0;

    /* ⚠️ MBR ONLY. sd_blockdev_for_partition() reads a partition table; a card
     * formatted as a raw "superfloppy" (no MBR, filesystem at sector 0) does
     * not enumerate here, and that is the single most likely way for a
     * correctly-wired adapter to fail. dc/sd.h says so explicitly. */
    if (sd_blockdev_for_partition(0, &s_sd_dev, &ptype) != 0) {
        sd_shutdown();
        return 0;
    }

    if (fs_fat_init() != 0) {
        sd_shutdown();
        return 0;
    }

    if (fs_fat_mount(DC_GPROF_SD_MOUNT, &s_sd_dev, FS_FAT_MOUNT_READWRITE) != 0) {
        fs_fat_shutdown();
        sd_shutdown();
        return 0;
    }

    return 1;
}

/* The interfaces to probe, in order. An explicit list rather than a loop with
 * #ifs inside it: the loop-with-#if version had a real bug — with
 * DC_GPROF_SD_IF=0 a FAILED sole attempt left the index at 1, which passed the
 * `>= 2` failure test and let the caller march on to fopen() an unmounted path
 * with the console already disabled. The list cannot express that. */
static const int s_sd_ifaces[] = {
#if (DC_GPROF_SD_IF) == 0
    0
#elif (DC_GPROF_SD_IF) == 1
    1
#else
    0, 1
#endif
};

static int s_sd_iface = -1;   /* which one took, for the receipt */

/* Bring the card up and point our VFS at it. Returns 1 if every subsequent byte
 * gmon.c writes to /prof will land in a file on the card.
 *
 * ⚠️ THE CALLER MUST HAVE DISABLED dbgio BEFORE CALLING THIS, and that is not a
 * tidiness rule. The SCIF reader puts SCIF into bit-banged SPI, and KOS's
 * scif_write() then spins 800,000 times on a TDFE that can never assert and
 * sets `serial_enabled = 0` — a latch NOTHING in scif.c ever clears
 * (hardware/scif.c). One stray log line from any thread in that window kills
 * the console permanently, including the crash dump and including the
 * fall-back-to-console path this function's failure return exists to reach.
 * kb/traps.md already records the same latch reached through scif_flush(). */
static int dc_prof_sd_open(void) {
    /* gmon.c's gmon_hdr_t, byte for byte: cookie, int32 version = 1 (SH-4 is
     * little-endian), then 12 bytes of spare. monstartup() already wrote this
     * through the console sink at boot; the card needs its own copy or the file
     * is not a gmon.out. */
    static const uint8_t hdr[20] = {
        'g', 'm', 'o', 'n',
        0x01, 0x00, 0x00, 0x00,
        0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
    };
    FILE *f;
    unsigned int k;

    for (k = 0; k < sizeof(s_sd_ifaces) / sizeof(s_sd_ifaces[0]); k++) {
        if (dc_prof_sd_try(s_sd_ifaces[k])) {
            s_sd_iface = s_sd_ifaces[k];
            break;
        }
    }
    if (s_sd_iface < 0)
        return 0;

    snprintf(s_sd_path, sizeof(s_sd_path), "%s.%d",
             DC_GPROF_SD_PREFIX, (int)getpid());

    /* ⚠️ "w", AND IT STAYS OPEN. Not "a", and not reopened per write: see
     * reason 3 in the header block — an O_APPEND fd is unwritable on fs_fat. */
    f = fopen(s_sd_path, "w");
    if (!f) {
        fs_fat_unmount(DC_GPROF_SD_MOUNT);
        fs_fat_shutdown();
        sd_shutdown();
        return 0;
    }
    if (fwrite(hdr, 1, sizeof(hdr), f) != sizeof(hdr)) {
        fclose(f);
        fs_fat_unmount(DC_GPROF_SD_MOUNT);
        fs_fat_shutdown();
        sd_shutdown();
        return 0;
    }

    s_sd_file = f;   /* from here prof_write() forwards into it */
    s_sd_up   = 1;
    return 1;
}

/* Commit and take the card down. ⚠️ fs_fat postpones inode and block writes
 * until eviction or unmount, so WITHOUT this the file on the card is empty or
 * truncated however well the writes went. */
static void dc_prof_sd_finish(unsigned int frames) {
    struct stat st;
    FILE *f;
    long  size = -1;
    char  msg[224];

    /* Close OUR handle first — everything gmon.c wrote is still in newlib's
     * buffer until this returns. */
    if (s_sd_file) {
        fflush(s_sd_file);
        fclose(s_sd_file);
        s_sd_file = NULL;
    }

    if (stat(s_sd_path, &st) == 0)
        size = (long)st.st_size;

    f = fopen(DC_GPROF_SD_NOTE, "w");
    if (f) {
        fprintf(f,
                "gmon.out   %s\n"
                "size       %ld bytes   (20 alone = the histogram never landed)\n"
                "shortwr    %lu\n"
                "iface      %s\n"
                "range      %08lx..%08lx (%lu B)\n"
                "alloc      %lu bytes (predicted)\n"
                "hz         %u\n"
                "frames     %u presented\n"
                "\n"
                "Decode on the host, inside the SDK container:\n"
                "  sh-elf-gprof -b -p dc/build/AnimalCrossing.elf <this gmon file>\n"
                "The ELF MUST be the one this CDI was built from -- check the\n"
                "matching .cdi.src.json sha256 next to the image.\n"
                "tools/dcprof/decode_gmon.py is NOT for this file; it only\n"
                "decodes the base64 console fallback.\n",
                s_sd_path, size, (unsigned long)s_sd_err,
                (s_sd_iface == 1) ? "SCI" : "SCIF",
                (unsigned long)s_lowpc, (unsigned long)s_highpc,
                (unsigned long)(s_highpc - s_lowpc),
                (unsigned long)s_alloc_pred,
                thd_get_hz(), frames);
        fclose(f);
    }

    fs_fat_sync(DC_GPROF_SD_MOUNT);
    fs_fat_unmount(DC_GPROF_SD_MOUNT);
    fs_fat_shutdown();
    sd_shutdown();

    /* sd_shutdown() -> spi_shutdown() -> scif_spi_shutdown() calls scif_init(),
     * which restores SCSMR2/SCBRR2/SCFCR2/SCSPTR2/SCSCR2 — so the port really is
     * a UART again and this flag flip is enough. ⚠️ It is only enough because
     * NOTHING wrote to SCIF while it was in SPI mode; scif.c's
     * `serial_enabled = 0` latch has no clearer anywhere in KOS. */
    dbgio_enable();

    snprintf(msg, sizeof(msg),
             "[GPROF] SD %s: wrote %s, %ld bytes, %lu short writes, %u frames\n",
             (s_sd_iface == 1) ? "SCI" : "SCIF",
             s_sd_path, size, (unsigned long)s_sd_err, frames);
    dc_prof_emit(msg);
}

#endif /* DC_GPROF_SD */

/* ==========================================================================
 * THE TRIGGER
 * ========================================================================== */

static uint8_t      s_flushed;
static unsigned int s_frames;

void dc_profdump_flush_now(void) {
    if (s_flushed) return;
    s_flushed = 1;

    if (!s_armed) {
        dc_prof_emit("[GPROF] flush requested but never armed\n");
        s_ended = 1;
        return;
    }

#if (DC_GPROF_SD) > 0
    /* 🔴 SILENCE THE CONSOLE FIRST, BEFORE THE CARD COMES UP — NOT AFTER.
     * The SCIF reader puts SCIF into bit-banged SPI; KOS's scif_write() then
     * spins 800,000 times on a TDFE that can never assert and latches
     * `serial_enabled = 0`, which NOTHING in KOS ever clears
     * (hardware/scif.c). One log line from any thread — ours, the audio
     * thread's, a KOS warning — inside that window kills the console
     * permanently, taking the crash dump AND the fall-back-to-console path
     * below with it. kb/traps.md records the same latch via scif_flush().
     * If the card does not come up, this is undone immediately.
     *
     * ⭐ SAY SO FIRST. Everything after this line is invisible, so a wedge in
     * the probe is indistinguishable from the game hanging — which is exactly
     * how KOS's unbounded SCI RDRF loop (see DC_GPROF_SD_IF above) cost four
     * runs before anyone could attribute it. Forty bytes of serial buys the
     * attribution. */
    dc_prof_emit("[GPROF] dump: muting console, probing SD\n");
    dbgio_flush();
    dbgio_disable();

    /* Switch sinks BEFORE _mcleanup(), which is the only moment this can be
     * done: the histogram is written inside it. If the card is not there, fall
     * through to the console path that has worked all along. */
    if (dc_prof_sd_open()) {
        _mcleanup();
        dc_prof_sd_finish(s_frames);
        s_ended = 1;          /* nothing further may reach the console sink */
        return;
    }
    /* The card was the whole plan and it is not there. The console sink is the
     * fallback, so undo any mute that a measuring burn armed — otherwise the
     * run is a total loss rather than a slow one. */
    dbgio_enable();
    dc_prof_emit("[GPROF] SD sink unavailable; falling back to the console\n");
#endif

    /* _mcleanup() stops the sampler, joins its thread, and writes the
     * histogram and the arcs through the /prof sink. It is idempotent
     * (gmon.c:307) and it is the documented flush for a program that never
     * returns from main(), which is exactly this one. */
    _mcleanup();

    /* Whether or not _mcleanup() wrote anything, close the stream out so the
     * decoder sees a terminated frame rather than a hang. */
    if (!s_begun) dc_prof_begin();
    dc_prof_end();
}

/* THE PAD CHORD — L + R + START, held.
 *
 * ⭐ This is what makes a HUMAN-DRIVEN hardware run possible. The frame cap is
 * right for an unattended Flycast run, where DC_AUTOSTART walks the same path
 * every time; on a console with a person holding the controller it is a guess
 * at how long it takes them to reach somewhere worth profiling. The chord lets
 * them say "here, now" — walk to the middle of the town, stand still, pull both
 * triggers and press START.
 *
 * Both triggers AND Start is three deliberate inputs. It reaches the game as
 * Z/L/R plus Start (dc_pad.c's PADRead mapping), which opens the menu — that is
 * accepted, because the dump is the end of the useful part of the run.
 *
 * Read straight off maple rather than through PADRead: this must work whether
 * or not the game is polling, including if it has wedged. */
#if (DC_GPROF_CHORD) > 0
static int dc_prof_chord_down(void) {
    maple_device_t *dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    cont_state_t   *st;

    if (!dev) return 0;
    st = (cont_state_t *)maple_dev_status(dev);
    if (!st) return 0;

    return (st->ltrig > 200) && (st->rtrig > 200) &&
           ((st->buttons & CONT_START) != 0);
}
#endif

void dc_profdump_frame_tick(void) {
    if (s_flushed) return;

    s_frames++;

#if (DC_GPROF_CHORD) > 0
    if (dc_prof_chord_down()) {
        dc_prof_emit("[GPROF] pad chord — dumping now\n");
        dc_profdump_flush_now();
        return;
    }
#endif

#if (DC_GPROF_DUMP_FRAME) > 0
    if (s_frames >= (unsigned int)(DC_GPROF_DUMP_FRAME))
        dc_profdump_flush_now();
#endif
}

#endif /* DC_GPROF */
