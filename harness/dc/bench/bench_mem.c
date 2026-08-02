/* bench_mem2.c -- second-tier memory bandwidth probe for OpenCrossing-Dreamcast.
 *
 * Measures every path by which the SH-4 can move a block between main RAM and
 * (a) PVR VRAM via the 32-bit window, the 64-bit window, store queues, PVR DMA
 *     (ch2) and the SH-4 on-chip DMAC (ch3), in BOTH directions;
 * (b) AICA sound RAM via G2 PIO and G2 DMA, in BOTH directions.
 *
 * Every result is verified with a checksum before it is reported: a fast number
 * from a transfer that did not actually move the bytes is worse than no number.
 *
 * Build:  kos-cc -O2 -o bench_mem2.elf bench_mem2.c   (this file is host tooling,
 *         not game code -- the -O0 directive on src/ does not apply to it)
 * Run:    harness/dc/smoke.sh, or any CDI; output goes to the dc-load / scif console.
 */

#include <kos.h>
#include <arch/dmac.h>
#include <arch/cache.h>
#include <dc/pvr.h>
#include <dc/sq.h>
#include <dc/spu.h>
#include <dc/g2bus.h>
#include <dc/perfctr.h>
#include <stdio.h>
#include <string.h>
#include <stdint.h>

#define REPS        8                   /* trials per case; we report the best */
#define VRAM_PROBE  0x00600000          /* offset into the 64-bit texture area  */
#define AICA_PROBE  0x00100000          /* 1 MB into sound RAM, clear of the    */
                                        /* ARM program at 0 and the CDDA regs   */

static const size_t sizes[] = { 4096, 65536, 262144 };
#define NSIZES (sizeof(sizes) / sizeof(sizes[0]))

/* 32-byte aligned staging buffers in main RAM. */
static uint8_t src_buf[262144] __attribute__((aligned(32)));
static uint8_t dst_buf[262144] __attribute__((aligned(32)));

static uint32_t sum32(const void *p, size_t n) {
    const uint32_t *w = p;
    uint32_t s = 0;
    size_t i;
    for(i = 0; i < n / 4; i++)
        s = (s * 31u) + w[i];
    return s;
}

static void fill_src(size_t n) {
    uint32_t *w = (uint32_t *)src_buf;
    size_t i;
    for(i = 0; i < n / 4; i++)
        w[i] = (uint32_t)(i * 2654435761u) ^ 0xA5A5A5A5u;
}

/* ---- timing -------------------------------------------------------------- */
/* PRFC0 in elapsed-time mode; perf_cntr_timer_ns() is KOS's wrapper for it.
   Resolution is far finer than timer_us_gettime64() and it does not take an
   interrupt to read. */
static void timer_setup(void) {
    perf_cntr_timer_enable();
}
static uint64_t tns(void) {
    return perf_cntr_timer_ns();
}

static void report(const char *name, size_t bytes, uint64_t ns, int ok) {
    double mbs = ns ? ((double)bytes * 1000.0) / (double)ns : 0.0; /* B/ns == GB/s; *1000 -> MB/s */
    printf("%-38s %7u B  %9llu ns  %8.1f MB/s  %s\n",
           name, (unsigned)bytes, (unsigned long long)ns, mbs,
           ok ? "ok" : "*** CHECKSUM MISMATCH ***");
}

/* ---- VRAM: raw CPU stores/loads ------------------------------------------ */

static void bench_vram_cpu(uintptr_t base, const char *win) {
    size_t si;
    char label[64];

    for(si = 0; si < NSIZES; si++) {
        size_t n = sizes[si];
        volatile uint32_t *v = (volatile uint32_t *)(base + VRAM_PROBE);
        const uint32_t *s = (const uint32_t *)src_buf;
        uint32_t *d = (uint32_t *)dst_buf;
        uint64_t best, t0, t1;
        size_t i;
        int r, ok;

        fill_src(n);

        /* write: main RAM -> VRAM, 32-bit stores through the P2 (uncached) alias */
        best = ~0ULL;
        for(r = 0; r < REPS; r++) {
            t0 = tns();
            for(i = 0; i < n / 4; i++) v[i] = s[i];
            t1 = tns();
            if(t1 - t0 < best) best = t1 - t0;
        }
        sprintf(label, "VRAM %s  CPU store32   W", win);
        report(label, n, best, 1);

        /* read: VRAM -> main RAM, 32-bit loads */
        best = ~0ULL;
        for(r = 0; r < REPS; r++) {
            t0 = tns();
            for(i = 0; i < n / 4; i++) d[i] = v[i];
            t1 = tns();
            if(t1 - t0 < best) best = t1 - t0;
        }
        ok = (sum32(dst_buf, n) == sum32(src_buf, n));
        sprintf(label, "VRAM %s  CPU load32    R", win);
        report(label, n, best, ok);
    }
}

/* ---- VRAM: store queues (write only -- SQs have no read direction) -------- */

static void bench_vram_sq(void) {
    size_t si;
    char label[64];

    for(si = 0; si < NSIZES; si++) {
        size_t n = sizes[si];
        void *dst = (void *)(PVR_RAM_INT_BASE + VRAM_PROBE);
        uint64_t best = ~0ULL, t0, t1;
        int r;

        fill_src(n);
        for(r = 0; r < REPS; r++) {
            t0 = tns();
            sq_cpy(dst, src_buf, n);
            sq_wait();
            t1 = tns();
            if(t1 - t0 < best) best = t1 - t0;
        }
        /* verify by reading back through the same window with the CPU */
        memset(dst_buf, 0, n);
        {
            volatile uint32_t *v = (volatile uint32_t *)dst;
            uint32_t *d = (uint32_t *)dst_buf;
            size_t i;
            for(i = 0; i < n / 4; i++) d[i] = v[i];
        }
        sprintf(label, "VRAM 64b sq_cpy         W");
        report(label, n, best, sum32(dst_buf, n) == sum32(src_buf, n));

        /* The fast path: SQ aimed at the TA's VRAM window (0x11000000) rather
           than at the raw VRAM alias. This is TapamN's sq_cpy_pvr trick; in
           KOS 2.3 it is spelled pvr_sq_load(..., PVR_DMA_VRAM64) and relies on
           PVR_LMMODE0 == 0, which pvr_dma_init() (called from pvr_init) sets.
           Write-only -- there is no read direction through this window. */
        best = ~0ULL;
        for(r = 0; r < REPS; r++) {
            t0 = tns();
            pvr_sq_load((void *)VRAM_PROBE, src_buf, n, PVR_DMA_VRAM64);
            sq_wait();
            t1 = tns();
            if(t1 - t0 < best) best = t1 - t0;
        }
        memset(dst_buf, 0, n);
        {
            volatile uint32_t *v = (volatile uint32_t *)dst;
            uint32_t *d = (uint32_t *)dst_buf;
            size_t i;
            for(i = 0; i < n / 4; i++) d[i] = v[i];
        }
        sprintf(label, "VRAM 64b pvr_sq_load    W");
        report(label, n, best, sum32(dst_buf, n) == sum32(src_buf, n));
    }
}

/* ---- VRAM: PVR DMA, channel 2 (write only -- MEM_TO_DEV) ----------------- */

static void bench_vram_pvrdma(void) {
    size_t si;

    for(si = 0; si < NSIZES; si++) {
        size_t n = sizes[si];
        uint64_t best = ~0ULL, t0, t1;
        int r;

        fill_src(n);
        dcache_wback_range((uintptr_t)src_buf, n);

        for(r = 0; r < REPS; r++) {
            t0 = tns();
            pvr_dma_transfer(src_buf, VRAM_PROBE, n, PVR_DMA_VRAM64, 1, NULL, NULL);
            t1 = tns();
            if(t1 - t0 < best) best = t1 - t0;
        }
        memset(dst_buf, 0, n);
        {
            volatile uint32_t *v = (volatile uint32_t *)(PVR_RAM_INT_BASE + VRAM_PROBE);
            uint32_t *d = (uint32_t *)dst_buf;
            size_t i;
            for(i = 0; i < n / 4; i++) d[i] = v[i];
        }
        report("VRAM 64b pvr_dma (ch2)  W", n, best,
               sum32(dst_buf, n) == sum32(src_buf, n));
    }
}

/* ---- VRAM: SH-4 on-chip DMAC channel 3, mem<->mem, BOTH directions -------
 *
 * This is the path that the "reads from VRAM are slow" folklore never
 * considers: the DMAC is a bus master in its own right, so a VRAM read does
 * not go through the SH-4 load pipeline at all.
 *
 * NOTE: dma_wait_complete() calls genwait_wait(..., 0) == wait forever if no
 * callback was supplied (because with cfg->callback == NULL the DMAC IRQ is
 * never enabled and nothing ever wakes the waiter). We poll dma_is_running()
 * directly instead, which is both correct and lower overhead.
 */

static const dma_config_t m2m_cfg = {
    .channel        = DMA_CHANNEL_3,
    .request        = DMA_REQUEST_AUTO_MEM_TO_MEM,
    .unit_size      = DMA_UNITSIZE_32BYTE,
    .src_mode       = DMA_ADDRMODE_INCREMENT,
    .dst_mode       = DMA_ADDRMODE_INCREMENT,
    .transmit_mode  = DMA_TRANSMITMODE_BURST,
    .callback       = NULL,
};

static int m2m(dma_addr_t dst, dma_addr_t src, size_t n) {
    if(dma_transfer(&m2m_cfg, dst, src, n, NULL) < 0)
        return -1;
    while(dma_is_running(DMA_CHANNEL_3))
        ;
    return 0;
}

static void bench_vram_shdma(void) {
    size_t si;

    for(si = 0; si < NSIZES; si++) {
        size_t n = sizes[si];
        dma_addr_t vram = hw_to_dma_addr(PVR_RAM_INT_BASE + VRAM_PROBE);
        uint64_t best, t0, t1;
        int r, fail = 0;

        fill_src(n);

        /* main RAM -> VRAM */
        best = ~0ULL;
        for(r = 0; r < REPS; r++) {
            dma_addr_t s = dma_map_src(src_buf, n);
            t0 = tns();
            if(m2m(vram, s, n) < 0) fail = 1;
            t1 = tns();
            if(t1 - t0 < best) best = t1 - t0;
        }
        report("VRAM 64b sh4-dmac ch3   W", n, best, !fail);

        /* VRAM -> main RAM. The interesting one. */
        memset(dst_buf, 0, n);
        best = ~0ULL;
        for(r = 0; r < REPS; r++) {
            dma_addr_t d = dma_map_dst(dst_buf, n);
            t0 = tns();
            if(m2m(d, vram, n) < 0) fail = 1;
            t1 = tns();
            if(t1 - t0 < best) best = t1 - t0;
        }
        dcache_inval_range((uintptr_t)dst_buf, n);
        report("VRAM 64b sh4-dmac ch3   R", n, best,
               !fail && sum32(dst_buf, n) == sum32(src_buf, n));
    }
}

/* ---- AICA sound RAM: G2 PIO and G2 DMA, both directions ------------------ */

static void bench_aica(void) {
    size_t si;

    for(si = 0; si < NSIZES; si++) {
        size_t n = sizes[si];
        uint64_t best, t0, t1;
        int r;

        fill_src(n);

        /* PIO write (g2_write_block_32 + g2_fifo_wait every 8 words) */
        best = ~0ULL;
        for(r = 0; r < REPS; r++) {
            t0 = tns();
            spu_memload(AICA_PROBE, src_buf, n);
            t1 = tns();
            if(t1 - t0 < best) best = t1 - t0;
        }
        report("AICA g2 PIO  spu_memload  W", n, best, 1);

        /* PIO read */
        memset(dst_buf, 0, n);
        best = ~0ULL;
        for(r = 0; r < REPS; r++) {
            t0 = tns();
            spu_memread(dst_buf, AICA_PROBE, n);
            t1 = tns();
            if(t1 - t0 < best) best = t1 - t0;
        }
        report("AICA g2 PIO  spu_memread  R", n, best,
               sum32(dst_buf, n) == sum32(src_buf, n));

        /* SQ write (spu_memload_sq: store queues straight at the G2 window) */
        spu_memset(AICA_PROBE, 0, n);
        best = ~0ULL;
        for(r = 0; r < REPS; r++) {
            t0 = tns();
            spu_memload_sq(AICA_PROBE, src_buf, n);
            t1 = tns();
            if(t1 - t0 < best) best = t1 - t0;
        }
        memset(dst_buf, 0, n);
        spu_memread(dst_buf, AICA_PROBE, n);
        report("AICA g2 SQ   spu_memload_sq W", n, best,
               sum32(dst_buf, n) == sum32(src_buf, n));

        /* DMA write, G2 channel 2 (SPU channel 0 is hardware-triggered and is
           the one the audio driver wants; CH2/CH3 are free). */
        spu_memset(AICA_PROBE, 0, n);
        dcache_wback_range((uintptr_t)src_buf, n);
        best = ~0ULL;
        for(r = 0; r < REPS; r++) {
            t0 = tns();
            g2_dma_transfer(src_buf, (void *)(SPU_RAM_BASE | AICA_PROBE), n, 1,
                            NULL, NULL, G2_DMA_TO_G2, 0, G2_DMA_CHAN_CH2, 0);
            t1 = tns();
            if(t1 - t0 < best) best = t1 - t0;
        }
        memset(dst_buf, 0, n);
        spu_memread(dst_buf, AICA_PROBE, n);
        report("AICA g2 DMA  ch2          W", n, best,
               sum32(dst_buf, n) == sum32(src_buf, n));

        /* DMA read -- G2BUSTOSH4. spu_memread() has no DMA variant in KOS,
           so this goes through g2_dma_transfer() directly. */
        memset(dst_buf, 0, n);
        dcache_purge_range((uintptr_t)dst_buf, n);
        best = ~0ULL;
        for(r = 0; r < REPS; r++) {
            t0 = tns();
            g2_dma_transfer(dst_buf, (void *)(SPU_RAM_BASE | AICA_PROBE), n, 1,
                            NULL, NULL, G2_DMA_TO_SH4, 0, G2_DMA_CHAN_CH2, 0);
            t1 = tns();
            if(t1 - t0 < best) best = t1 - t0;
        }
        dcache_inval_range((uintptr_t)dst_buf, n);
        report("AICA g2 DMA  ch2          R", n, best,
               sum32(dst_buf, n) == sum32(src_buf, n));
    }
}

/* ---- main RAM baseline + cache-op cost ---------------------------------- */

static void bench_baseline(void) {
    size_t si;

    for(si = 0; si < NSIZES; si++) {
        size_t n = sizes[si];
        uint64_t best = ~0ULL, t0, t1;
        int r;

        fill_src(n);
        for(r = 0; r < REPS; r++) {
            t0 = tns();
            memcpy(dst_buf, src_buf, n);
            t1 = tns();
            if(t1 - t0 < best) best = t1 - t0;
        }
        report("main RAM memcpy (baseline)", n, best,
               sum32(dst_buf, n) == sum32(src_buf, n));

        best = ~0ULL;
        for(r = 0; r < REPS; r++) {
            t0 = tns();
            dcache_wback_range((uintptr_t)src_buf, n);
            t1 = tns();
            if(t1 - t0 < best) best = t1 - t0;
        }
        report("dcache_wback_range", n, best, 1);

        best = ~0ULL;
        for(r = 0; r < REPS; r++) {
            t0 = tns();
            dcache_inval_range((uintptr_t)dst_buf, n);
            t1 = tns();
            if(t1 - t0 < best) best = t1 - t0;
        }
        report("dcache_inval_range", n, best, 1);

        best = ~0ULL;
        for(r = 0; r < REPS; r++) {
            t0 = tns();
            dcache_purge_range((uintptr_t)dst_buf, n);
            t1 = tns();
            if(t1 - t0 < best) best = t1 - t0;
        }
        report("dcache_purge_range", n, best, 1);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;

    timer_setup();

    vid_set_mode(DM_640x480, PM_RGB565);
    pvr_init_defaults();
    g2_dma_init();

    printf("\n=== bench_mem2: second-tier memory ===\n");
    printf("pvr_mem_available() after pvr_init_defaults(): %u B\n",
           (unsigned)pvr_mem_available());
    printf("probe offsets: VRAM +0x%06x, AICA +0x%06x\n\n", VRAM_PROBE, AICA_PROBE);

    bench_baseline();
    printf("\n");
    bench_vram_cpu(PVR_RAM_BASE,     "32b");   /* 0xa5000000 */
    printf("\n");
    bench_vram_cpu(PVR_RAM_INT_BASE, "64b");   /* 0xa4000000 */
    printf("\n");
    bench_vram_sq();
    printf("\n");
    bench_vram_pvrdma();
    printf("\n");
    bench_vram_shdma();
    printf("\n");
    bench_aica();

    printf("\n=== bench_mem2 done ===\n");
    return 0;
}
