/* dc_assetwin.c — ONE read-ahead window for every MID-SCENE asset read.
 * (Kill switch DC_ASSETWIN_B=0, which restores a plain seek+read per request.)
 *
 * WHAT PROBLEM THIS SOLVES, AND WHY IT IS NOT dc_keep_sweep()
 * ----------------------------------------------------------
 * dc_main.c's dc_keep_sweep() already collapses the BOOT keep list's 1,392
 * scattered reads into ~129 by sorting them by (rom_src, rom_off) and serving
 * them through one window buffer.  It can only do that because it sees every
 * request BEFORE any of them runs: the generated dc_stub_keep.inc records the
 * whole list, then the sweep replays it in disc order.
 *
 * Every demand loader in this port is the opposite shape.  R1 (dc_bgtex.c) is
 * handed one acre's 27 ground textures one call at a time, mid-scene; T1
 * (dc_texpool.c) is handed one texture at the moment the renderer binds it.
 * There is no list to sort — the ORDER is the game's.
 *
 * 🔴 AND THE FIRST VERSION OF THIS FILE GOT THAT WRONG, IN A WAY WORTH KEEPING.
 * It read a FULL window on every miss, on the strength of kb/levers.md L10's
 * measurement that the resident texture offsets are clustered — median gap
 * 512 B, 863 of 905 gaps under 32 KB.  That figure describes the offsets
 * **SORTED**.  The runtime order is the order the renderer binds textures in,
 * and the same measurement says those offsets SPAN 10.9 MB.  dc_keep_sweep()
 * earns the clustering by sorting first; a demand loader cannot, and inheriting
 * the conclusion without the sort was the error.
 *
 * What it cost, measured in one run (smoke-t1test-20260809-170652): **4,849,664
 * B moved off disc to deliver ~190 KB of texture** — 148 refills of 32 KB for a
 * ~1.2 KB payload each — a hit rate of only 123/271, and 94 [STUTTER] frames
 * whose 320 ms was in neither `gx` nor `snd`, i.e. in the read.  A human
 * watching the emulator called it before any counter did.
 *
 * ⭐ SO IT READS AHEAD ONLY WHEN THE STREAM LOOKS SEQUENTIAL.  What is genuinely
 * local here is a BURST — R1 pulling one acre's 27 ground textures out of six
 * consecutive vendored tables, or a scene load walking a run of adjacent rows —
 * and a burst is recognisable without a lookahead: it continues where the last
 * request ended.  win_sequential() is that test.  A scattered bind now costs
 * one sector-aligned read rounded up to DC_ASSETWIN_MIN, and nothing more.
 *
 * 🔴 AND THAT STILL LOST, WHICH IS WHY THE DEFAULT IS NOW **OFF**.  A fourth
 * configuration of the same workload — no window at all — moved **325,184 B**,
 * the true payload, in 271 reads with 109 [STUTTER] against a 90-stutter
 * baseline.  The narrow-and-sequential version moved 2,177,024 B for 147
 * stutters and the 32 KB version 4,849,664 B for 94.  **Every window variant
 * paid between 6.7x and 15x the payload to collapse ~120 GD-ROM commands, and
 * none of them beat OFF by enough to be worth it.**
 *
 * The file stays because the emulator CANNOT settle the remaining question
 * (rule 12): on a CD-R, 271 seeks is 5.4-27 s and 4.85 MB of read-ahead is
 * ~9.7 s, the same order, and Flycast models neither — its FastGDRomLoad makes
 * a byte free, which is precisely why it ranked the 32 KB variant first.
 * DC_ASSETWIN_B=32768 runs that experiment on a burn.
 *
 * ⚠️ THE NUMBER TO A/B IS `reads=` AND `bytes=` TOGETHER, NOT WALL CLOCK.
 * Flycast mounts the disc with FastGDRomLoad and models no seek time at all
 * (kb/RESUME.md §6).  Both counters are host-independent.  Judging this on
 * `reads=` alone is what produced the 4.8 MB: fewer commands, far more bytes.
 *
 * WHY A STATIC BUFFER AND NOT malloc()
 * ------------------------------------
 * This runs mid-scene, after OSInit has carved the arena, so a malloc here
 * comes out of libc's sbrk share — the pool kb/heap-two-pools.md is about, the
 * one whose exhaustion looks like `Out of memory ... diff 1449984` on a splash
 * screen rather than like an allocation failure here.  A static array is 32 KB
 * of .bss that the MEMLEDGER line accounts for at link time and that cannot
 * fail at the worst possible moment.
 *
 * WHY IT DOES NOT SHARE dc_main.c's FILE HANDLES
 * ----------------------------------------------
 * dc_stub_keep_assets() closes dc_stub_rom_fd[] when the boot keep list is
 * done, and dc_stub_keep_load_one() re-opens lazily on the next mid-scene call.
 * The two lifetimes do not overlap: the sweep owns the fds during boot, this
 * file owns them afterwards.  Keeping them separate means neither has to know
 * when the other is finished.  The ROM PATHS are defined here once and read by
 * dc_main.c through dc_assetwin_rom_path(), so the two cannot drift.
 */
#include "dc_platform.h"

/* S15-5's counters, declared here rather than by pulling in dc_aram_lru.h —
 * this file needs one function from it and nothing else. Definition and the
 * whole argument are in dc/src/dc_dvd.c's dc_dvd_read_yielding(). */
void dc_dvd_short_stats(unsigned int* out_short, unsigned int* out_fail);

/* ⭐ DEFAULT OFF. The window LOST on every measurement that mattered — see the
 * four-row table in dc/Makefile's DC_ASSETWIN_B block and the post-mortem in
 * this file's header. It is kept, not deleted, because the emulator structurally
 * cannot rank seeks against bytes (rule 12) and a burn still might. */
#ifndef DC_ASSETWIN_B
#define DC_ASSETWIN_B 0u
#endif

/* The refill for a request that does NOT continue the previous one. The full
 * window is reserved for bursts; see the rule-12 note at the use site for why
 * this number cannot be settled in an emulator. 0 means "exactly the sectors
 * asked for". */
#ifndef DC_ASSETWIN_MIN
#define DC_ASSETWIN_MIN 8192u
#endif

/* KOS's iso9660 layer starts a drive-level stream only on a sector-aligned
 * read (fs_iso9660.c:755-777); an unaligned start makes it serve the leading
 * fragment from its single-sector cache and abort the stream, which costs an
 * extra GD-ROM command on every refill.  dc_keep_sweep() pays for this lesson
 * in its own comment; the constant is the same one. */
#define DC_ASSETWIN_SECTOR 2048u

/* Mirrors pc/src/pc_assets.c's file-static enum, exactly as dc_main.c's copy
 * does.  Index into the path table below. */
#define DC_ASSETWIN_SRC_N 2

static const char* const s_rom_path[DC_ASSETWIN_SRC_N] = {
    "/cd/foresta.rel",   /* rom_src 0 — already Yaz0-decompressed */
    "/cd/main.dol"       /* rom_src 1 */
};

const char* dc_assetwin_rom_path(int rom_src) {
    if (rom_src < 0 || rom_src >= DC_ASSETWIN_SRC_N) return NULL;
    return s_rom_path[rom_src];
}

/* Counters.  reads/bytes are the pair the window is judged on: a window that is
 * working moves MORE bytes in FEWER reads. */
static unsigned int s_req, s_hits, s_reads, s_disc_bytes, s_direct, s_fail;
/* Refills that read only the sectors asked for, because the stream did not
 * look sequential. High `narrow` next to low `hits` means the window is doing
 * nothing for this workload and DC_ASSETWIN_B=0 costs nothing. */
static unsigned int s_narrow;

void dc_assetwin_stats(unsigned int* req, unsigned int* hits,
                       unsigned int* reads, unsigned int* disc_bytes) {
    if (req)        *req        = s_req;
    if (hits)       *hits       = s_hits;
    if (reads)      *reads      = s_reads;
    if (disc_bytes) *disc_bytes = s_disc_bytes;
}

#if defined(DC_HOST_STUB) || (defined(DC_ASSETWIN_B) && DC_ASSETWIN_B == 0)

/* ---------------------------------------------------------------------------
 * Kill switch, and the host syntax-check build.  Every request is a plain
 * seek+read, which is byte-for-byte what each caller did before this file
 * existed.  Kept as a separate body rather than as a branch so the .bss and the
 * window logic vanish entirely rather than sitting unreferenced.
 * ------------------------------------------------------------------------- */

#ifdef DC_HOST_STUB
int dc_assetwin_read(int rom_src, unsigned int rom_off, void* dst,
                     unsigned int len) {
    (void)rom_src; (void)rom_off; (void)dst; (void)len;
    return 0;
}
void dc_assetwin_drop(void) {}
void dc_assetwin_report(void) {}
#else

static file_t s_fd[DC_ASSETWIN_SRC_N] = { FILEHND_INVALID, FILEHND_INVALID };

static file_t win_fd(int rom_src) {
    file_t fd;
    if (rom_src < 0 || rom_src >= DC_ASSETWIN_SRC_N) return FILEHND_INVALID;
    fd = s_fd[rom_src];
    if (fd != FILEHND_INVALID) return fd;
    fd = fs_open(s_rom_path[rom_src], O_RDONLY);
    if (fd == FILEHND_INVALID) {
        DC_LOGE("[DC/AWIN] %s MISSING — every demand load from it fails\n",
                s_rom_path[rom_src]);
        return FILEHND_INVALID;
    }
    s_fd[rom_src] = fd;
    return fd;
}

int dc_assetwin_read(int rom_src, unsigned int rom_off, void* dst,
                     unsigned int len) {
    file_t fd = win_fd(rom_src);
    if (fd == FILEHND_INVALID || dst == NULL || len == 0u) { s_fail++; return 0; }
    s_req++;
    s_direct++;
    if ((unsigned int)dc_dvd_read_yielding((unsigned int)fd, dst, len, 1,
                                           rom_off) != len) {
        s_fail++;
        return 0;
    }
    s_reads++;
    s_disc_bytes += len;
    return 1;
}

void dc_assetwin_drop(void) {
    int i;
    for (i = 0; i < DC_ASSETWIN_SRC_N; i++) {
        if (s_fd[i] != FILEHND_INVALID) { fs_close(s_fd[i]); s_fd[i] = FILEHND_INVALID; }
    }
}

void dc_assetwin_report(void) {
    unsigned int sr = 0, sf = 0;
    dc_dvd_short_stats(&sr, &sf);
    /* ⭐ sr=/sf= are S15-5's, and they are the reason this line matters on a
     * BURN rather than in Flycast: sr= counts fs_read returning fewer bytes
     * than asked, which FastGDRomLoad makes structurally impossible in the
     * emulator. sr>0 on hardware with sf=0 means the old `break` was silently
     * truncating asset reads; sf>0 means a caller got a short buffer anyway
     * and must be treated as a fault. */
    DC_LOGE("[DC/AWIN] OFF (DC_ASSETWIN_B=0): req=%u reads=%u bytes=%u fail=%u"
            " narrow=%u sr=%u sf=%u\n",
            s_req, s_reads, s_disc_bytes, s_fail, s_narrow, sr, sf);
}
#endif /* DC_HOST_STUB */

#else  /* the window is on */

static file_t s_fd[DC_ASSETWIN_SRC_N] = { FILEHND_INVALID, FILEHND_INVALID };

/* The window itself.  Static, for the reason in the header. */
static unsigned char s_win[DC_ASSETWIN_B];
static int          s_win_src = -1;      /* which ROM it holds, -1 = empty */
static unsigned int s_win_off;           /* sector-aligned base offset      */
static unsigned int s_win_len;           /* bytes actually present          */

/* End of the previous request, for the locality test below.  -1 = none yet. */
static int          s_prev_src = -1;
static unsigned int s_prev_end;

/* ⭐ READ AHEAD ONLY WHEN THE STREAM LOOKS SEQUENTIAL — MEASURED 2026-08-09.
 *
 * The first version of this file refilled a FULL window on every miss.  On the
 * real request stream that was a large net loss and the human watching it
 * called it before any counter did: run smoke-t1test-20260809-170652 moved
 * **4,849,664 B off disc to deliver ~190 KB of texture**, 148 refills at 32 KB
 * each for a ~1.2 KB payload, and produced 94 [STUTTER] frames whose 320 ms
 * was in neither `gx` nor `snd` — i.e. in the read.  The hit rate was 123/271,
 * so the window was not even paying for itself in seeks.
 *
 * WHY THE PREMISE WAS WRONG.  kb/levers.md L10 measured the resident texture
 * ROM offsets as clustered — median gap 512 B, 863 of 905 gaps under 32 KB —
 * and this file was built on that.  But that figure describes the offsets
 * SORTED.  The runtime order is the order the renderer binds textures in, which
 * is neither sorted nor local: the same measurement also says those offsets
 * SPAN 10.9 MB.  dc_keep_sweep() gets the win because it sorts first; a
 * demand loader cannot, and inheriting the conclusion without the sort was the
 * error.
 *
 * WHAT IS ACTUALLY LOCAL is a BURST: R1 pulls one acre's 27 ground textures out
 * of six consecutive vendored tables, and a scene load walks a run of adjacent
 * rows.  Those are exactly the requests that continue where the last one ended.
 * So: read ahead only when this request starts inside the previous one's
 * neighbourhood, and otherwise read just the sectors it needs.  Bursts keep the
 * window; scattered binds pay one aligned sector-read each and nothing more.
 *
 * The gate is generous on purpose — a burst's rows are adjacent but not
 * contiguous, and a whole window of slack still costs less than one wrong
 * refill. */
static int win_sequential(int rom_src, unsigned int rom_off) {
    if (s_prev_src != rom_src) return 0;
    if (rom_off < s_prev_end) return 0;
    return (rom_off - s_prev_end) < (unsigned int)DC_ASSETWIN_B;
}

static file_t win_fd(int rom_src) {
    file_t fd;
    if (rom_src < 0 || rom_src >= DC_ASSETWIN_SRC_N) return FILEHND_INVALID;
    fd = s_fd[rom_src];
    if (fd != FILEHND_INVALID) return fd;
    fd = fs_open(s_rom_path[rom_src], O_RDONLY);
    if (fd == FILEHND_INVALID) {
        DC_LOGE("[DC/AWIN] %s MISSING — every demand load from it fails\n",
                s_rom_path[rom_src]);
        return FILEHND_INVALID;
    }
    s_fd[rom_src] = fd;
    DC_LOGE("[DC/AWIN] opened %s (%d B)\n", s_rom_path[rom_src],
            (int)fs_total(fd));
    return fd;
}

/* Read `len` bytes at `rom_off` from ROM `rom_src` into `dst`.
 * Returns 1 on success, 0 on any failure — and on failure `dst` is UNTOUCHED
 * rather than partly written, because a half-filled asset renders as something
 * plausible and a zeroed one renders as nothing.  Callers must treat 0 as "do
 * not draw this", never as "draw what you got". */
int dc_assetwin_read(int rom_src, unsigned int rom_off, void* dst,
                     unsigned int len) {
    file_t fd;
    unsigned int base, want;
    ssize_t got;

    if (dst == NULL || len == 0u) return 0;

    fd = win_fd(rom_src);
    if (fd == FILEHND_INVALID) { s_fail++; return 0; }

    s_req++;

    /* Already in the window?  This is the whole point of the file. */
    if (s_win_src == rom_src && rom_off >= s_win_off &&
        rom_off + len <= s_win_off + s_win_len) {
        memcpy(dst, s_win + (rom_off - s_win_off), len);
        s_hits++;
        s_prev_src = rom_src;
        s_prev_end = rom_off + len;
        return 1;
    }

    /* Decided BEFORE the counters below move, and remembered for the next
     * call whatever happens to this one. */
    {
        int seq = win_sequential(rom_src, rom_off);
        s_prev_src = rom_src;
        s_prev_end = rom_off + len;
        if (!seq) {
            /* Scattered: a SMALL refill — the sectors this request covers,
             * rounded up to DC_ASSETWIN_MIN so that near neighbours still come
             * for free, and nothing like the full window.
             *
             * ⚠️ THIS CONSTANT IS A COMPROMISE FLYCAST CANNOT ADJUDICATE, and
             * that is measurement rule 12 in a new place. Three configurations
             * of the same workload:
             *
             *   refill       reads   bytes off disc   [STUTTER]
             *   32 KB always   148        4,849,664      94
             *   sectors only   218        2,177,024     147
             *
             * Fewer, larger reads scored BETTER in the emulator and move 2.2x
             * the bytes. That ranking is an artefact: Flycast mounts with
             * FastGDRomLoad, so a byte costs nothing there and a command still
             * costs something. On a CD-R at ~500 KB/s the 2.7 MB difference is
             * ~5.4 s of transfer against ~70 saved seeks at 20-100 ms — the
             * two are the same order and the emulator cannot tell us which
             * wins. 8 KB is chosen to be wrong by less in either direction;
             * **a burn decides it, not a smoke run.** */
            unsigned int b = rom_off - (rom_off % DC_ASSETWIN_SECTOR);
            unsigned int need = (rom_off + len) - b;
            unsigned int w = (need + DC_ASSETWIN_SECTOR - 1u)
                             / DC_ASSETWIN_SECTOR * DC_ASSETWIN_SECTOR;
            if (w < (unsigned int)DC_ASSETWIN_MIN)
                w = (unsigned int)DC_ASSETWIN_MIN;
            if (w > (unsigned int)DC_ASSETWIN_B)
                w = (unsigned int)DC_ASSETWIN_B;
            if (w < need)
                w = need;   /* the request itself always wins */
            if (w <= (unsigned int)DC_ASSETWIN_B) {
                ssize_t g = (ssize_t)dc_dvd_read_yielding(
                    (unsigned int)fd, s_win, w, 1, b);
                if (g < (ssize_t)((rom_off - b) + len)) {
                    DC_LOGE("[DC/AWIN] short read at %u (%d of %u)\n",
                            b, (int)g, w);
                    s_fail++;
                    s_win_src = -1; s_win_len = 0;
                    return 0;
                }
                s_win_src = rom_src;
                s_win_off = b;
                s_win_len = (unsigned int)g;
                s_reads++;
                s_disc_bytes += (unsigned int)g;
                s_narrow++;
                memcpy(dst, s_win + (rom_off - s_win_off), len);
                return 1;
            }
            /* len alone exceeds the window — fall through to the direct read. */
        }
    }

    /* Bigger than the window: straight into the destination, and the window is
     * stale afterwards because the drive stream has moved. */
    if (len > (unsigned int)DC_ASSETWIN_B) {
        if ((unsigned int)dc_dvd_read_yielding((unsigned int)fd, dst, len, 1,
                                               rom_off) != len) {
            DC_LOGE("[DC/AWIN] short direct read of %u B at %u (src %d)\n",
                    len, rom_off, rom_src);
            s_fail++;
            s_win_src = -1; s_win_len = 0;
            return 0;
        }
        s_direct++;
        s_reads++;
        s_disc_bytes += len;
        s_win_src = -1; s_win_len = 0;
        return 1;
    }

    /* Refill, starting on a sector boundary.  `want` is the full window unless
     * the request straddles further than that, which cannot happen here — len
     * is <= the cap and base is at most SECTOR-1 below rom_off, and the cap is
     * a whole number of sectors. */
    base = rom_off - (rom_off % DC_ASSETWIN_SECTOR);
    want = (unsigned int)DC_ASSETWIN_B;
    if (base + want < rom_off + len)
        want = (rom_off + len) - base;

    got = (ssize_t)dc_dvd_read_yielding((unsigned int)fd, s_win, want, 1, base);
    if (got < (ssize_t)((rom_off - base) + len)) {
        /* A short read at the END of the ROM is legal — the last asset in the
         * file cannot be followed by a full window — but only if it still
         * covered the request, which is what this test asks. */
        DC_LOGE("[DC/AWIN] short window read at %u (%d of %u), request %u+%u\n",
                base, (int)got, want, rom_off, len);
        s_fail++;
        s_win_src = -1; s_win_len = 0;
        return 0;
    }
    s_win_src = rom_src;
    s_win_off = base;
    s_win_len = (unsigned int)got;
    s_reads++;
    s_disc_bytes += (unsigned int)got;

    memcpy(dst, s_win + (rom_off - s_win_off), len);
    return 1;
}

/* Close the handles and forget the window.  Nothing calls this today; it exists
 * so a scene teardown CAN, and so the fds are not a leak by construction. */
void dc_assetwin_drop(void) {
    int i;
    for (i = 0; i < DC_ASSETWIN_SRC_N; i++) {
        if (s_fd[i] != FILEHND_INVALID) { fs_close(s_fd[i]); s_fd[i] = FILEHND_INVALID; }
    }
    s_win_src = -1;
    s_win_len = 0;
}

void dc_assetwin_report(void) {
    /* `reads` is the GD-ROM command count and the number to A/B.  `hits` is how
     * many requests the window answered without one; `direct` is how many were
     * too big for it.  A healthy run has hits >> reads. */
    DC_LOGE("[DC/AWIN] req=%u hits=%u reads=%u narrow=%u direct=%u bytes=%u "
            "fail=%u win=%u B\n",
            s_req, s_hits, s_reads, s_narrow, s_direct, s_disc_bytes, s_fail,
            (unsigned int)DC_ASSETWIN_B);
}

#endif /* DC_ASSETWIN_B */
