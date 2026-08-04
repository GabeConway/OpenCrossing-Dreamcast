/* dc_audio.c - AI (Audio Interface) + DSP surface for Dreamcast.
 *
 * Replaces pc/src/pc_audio.c (SDL2). The ring buffer and the AI / DSP surface
 * are portable and kept verbatim in shape; the SDL device becomes KOS
 * snd_stream, fed from AICA.
 *
 * PLAN §3.4 stages this:
 *   Stage A (M3): rspsim runs on SH-4 at 22.05 kHz with effects tiered off,
 *                 feeding snd_stream. Sequenced music keeps working with zero
 *                 format work. Precedent: sm64-dc runs the N64 audio engine on
 *                 SH-4 at full speed.
 *   Stage B (M4, only if A blows the CPU budget): jaudio instrument banks
 *                 convert offline to AICA-native ADPCM in the 2 MB sound RAM;
 *                 sequencing stays on SH-4, synthesis moves to hardware.
 *
 * THE 8.3 MB audiorom NEVER SITS IN MAIN RAM. An offline tool splits it:
 * instrument samples -> AICA sound RAM or disc-streamed; sequence data (small)
 * -> main RAM. Nothing in this file may load audiorom.img.
 *
 * THREADING: the base port runs a real SDL producer thread. This port does
 * NOT start one (see dc_os.c OSDisableInterrupts): the moment a KOS audio
 * thread exists, OSDisableInterrupts / OSInitMessageQueue / OSSendMessage stop
 * being safe as stubs and the jaudio queues race (design doc §3.7).
 */
#include "dc_platform.h"
#include "dc_mem_ledger.h"

/* The GameCube DAC rate the game programs. rspsim will be re-rated to 22050
 * for stage A (PLAN §3.4) via AISetDSPSampleRate; this is only the default. */
#define DC_AUDIO_SAMPLE_RATE 32000

/* mem-budget bucket 9 (C8): "snd_stream double-buffer replaces the 65,536 B
 * SDL ring". 8192 s16 = 16,384 B = ~128 ms at 32 kHz stereo. */
#define RING_BUF_SAMPLES 8192
#define RING_BUF_MASK    (RING_BUF_SAMPLES - 1)

static s16 ring_buffer[RING_BUF_SAMPLES];
static volatile u32 ring_write_pos = 0;
static volatile u32 ring_read_pos  = 0;

static int  audio_open = 0;
static u32  ai_dsp_sample_rate = DC_AUDIO_SAMPLE_RATE;

/* ==========================================================================
 * DC_AUDIO DEFAULTS TO 0, AND THE REASON IS A MEASUREMENT
 * ==========================================================================
 * Audio WORKS as of 2026-08-04 — real synthesis, real AICA playback, non-zero
 * [NEOS_OUT] peak. It is off by default because of what it costs, which was not
 * knowable until it ran:
 *
 *   ONE jaudio DAC frame costs ~19.8 ms of SH-4 (`synth_us=19840`) and yields
 *   ~35 ms of audio. So synthesis runs at ~1.8x real time and needs ~57 % of
 *   the machine just to stay level with playback.
 *
 * Measured over matched 600 s runs at the same commit, differing only by this:
 *
 *   audio off   FPS p50 23.5   deepest_scene 18 (the town)
 *   audio on    FPS p50 13.0   deepest_scene 18
 *   audio on, with a predictive budget   FPS p50 10.9, deepest_scene 4
 *
 * The budgeted run is WORSE, which is worth understanding rather than tuning
 * around: when the ring is starving the budget is deliberately overridden (a
 * late sample beats a gap), and at this cost the ring is starving essentially
 * always, so the override becomes the normal path and MORE frames get
 * synthesised, not fewer. There is no setting of DC_AUDIO_BUDGET_US that buys
 * both smooth sound and a playable frame rate; the cost is the algorithm, and
 * the algorithm is jaudio in src/, which builds at -O0 and may not be edited
 * (CLAUDE.md §1).
 *
 * So this is a product decision, not an engineering one, and the honest default
 * is the one that keeps the game playable. **-DDC_AUDIO=1 turns sound on and
 * costs roughly 45 % of the frame rate.** Everything needed for it is in the
 * tree and working; kb/audio-cpu-cost.md is where the per-voice budget belongs.
 *
 * DC_AUDIO_BUDGET_US caps how long one pump may spend in synthesis. */
#ifndef DC_AUDIO
#define DC_AUDIO 0
#endif
/* How long ONE pump may spend in jaudio synthesis. This is now a real cap (see
 * the predictive loop in dc_audio_pump), so it is a direct FPS-vs-audio dial:
 * measured cost is ~25.7 ms per DAC frame, and the town frame is ~43 ms, so
 * 12,000 us buys roughly half a DAC frame per pump on average and keeps audio
 * from owning the machine. Raise it for smoother sound and fewer FPS. */
#ifndef DC_AUDIO_BUDGET_US
#define DC_AUDIO_BUDGET_US 12000
#endif
/* Below this many samples in the ring, the budget is ignored and a frame is
 * synthesised anyway: a late sample beats a gap. One jaudio DAC frame is
 * 2*DAC_SIZE = 2240 samples (rate.c:7), so this is one frame of slack. */
#ifndef DC_AUDIO_MIN_FILL
#define DC_AUDIO_MIN_FILL 2240
#endif
/* Samples of slack kept free at the top of the ring, i.e. how much room one
 * pump leaves for the next Jac_UpdateDAC push. One jaudio DAC frame is 2*DAC_SIZE
 * samples; 2048 leaves several frames of margin. */
#ifndef DC_AUDIO_HEADROOM
#define DC_AUDIO_HEADROOM 2048
#endif

#if DC_AUDIO && !defined(DC_HOST_STUB)
static snd_stream_hnd_t s_hnd = SND_STREAM_INVALID;
#else
static int s_hnd = -1;
#endif
static u32 s_cb_calls = 0, s_cb_bytes = 0;
static u32 s_pump_calls = 0, s_pump_frames = 0;
static u32 s_pump_budget_hits = 0, s_pump_usec = 0;
static u32 s_poll_fail = 0;
static u32 s_synth_avg_us = 0;   /* EWMA of one pc_audio_process_frame() */

#if DC_AUDIO && !defined(DC_HOST_STUB)
/* THE AICA PLAY POSITION, because every other counter is ambiguous.
 *
 * `cb=2 pulled=... pollfail=0` is the whole audio symptom, and it has exactly
 * two possible causes that no existing counter separates:
 *
 *   (a) the AICA channel is not playing, so `pos` never advances, so
 *       snd_stream_poll() takes its `needed_bytes < buffer_size/2` early
 *       return forever (snd_stream.c:832-836) and never calls the callback;
 *   (b) the channel IS playing and the ring/threshold arithmetic is wrong.
 *
 * KOS reads that position at snd_stream.c:806-808 and exposes no accessor for
 * it, so this reads the same word the same way. The two constants are from the
 * PINNED KOS SHA (dc/Dockerfile) — `sound/arm/aica_cmd_iface.h:29,41`,
 * AICA_MEM_CHANNELS = 0x020000 and sizeof(aica_channel_t) = 16 dwords = 64 —
 * and that header lives outside the installed include path, which is why they
 * are written out here rather than #included.
 *
 * Read-only, from the SH-4 side, on a status block the header itself marks
 * READ-ONLY from the SH-4. It cannot perturb what it measures.
 *
 * Channel 0 is not necessarily OUR channel — snd_stream allocates from the SFX
 * pool — so all four low channels are reported and any one of them moving
 * answers the question. */
#include <dc/g2bus.h>
#include <dc/spu.h>
#define DC_AICA_CH_STRIDE 64u
#define DC_AICA_CH_BASE   0x020000u
#define DC_AICA_POS_OFF   40u   /* offsetof(aica_channel_t, pos): 10 dwords */

static u32 dc_aica_pos(int ch) {
    return g2_read_32(SPU_RAM_UNCACHED_BASE + DC_AICA_CH_BASE +
                      (u32)ch * DC_AICA_CH_STRIDE + DC_AICA_POS_OFF) & 0xFFFFu;
}

/* IS THE ARM7 ALIVE AT ALL? This is the discriminator one level below
 * dc_aica_pos(): KOS's ARM sound driver increments a millisecond counter at
 * AICA_MEM_CLOCK (aica_cmd_iface.h:32, 0x021000) on every pass of its main
 * loop, independently of whether any channel is keyed on. So
 *
 *   clock advancing, pos stuck  -> the ARM runs; the channel never started, or
 *                                  the start command was dropped
 *   clock stuck                 -> the ARM is not executing at all, and no
 *                                  amount of ring/threshold work can help
 *
 * Without this the two are indistinguishable, and they need completely
 * different fixes. */
#define DC_AICA_CLOCK 0x021000u

static u32 dc_aica_clock(void) {
    return g2_read_32(SPU_RAM_UNCACHED_BASE + DC_AICA_CLOCK);
}

/* ...and one level below THAT: are the ARM driver's bytes even in SPU RAM?
 * snd_init() (snd_iface.c) does spu_memset_sq(0,0,AICA_RAM_START) then
 * spu_memload_sq(0, snd_stream_drv_data, size) then spu_enable(), all through
 * the STORE QUEUES. Word 0 of a loaded driver is the ARM reset vector, i.e.
 * a branch, and is never zero.
 *
 *   drv != 0, clock == 0 -> the driver is resident and the ARM7 is halted
 *   drv == 0             -> the load never landed, and the store-queue path is
 *                           the thing to look at, not the AICA
 *
 * These three numbers together (drv, clock, pos) turn "audio is silent" into a
 * single decidable question, which is the whole reason they are printed. */
static u32 dc_aica_drv_word(void) {
    return g2_read_32(SPU_RAM_UNCACHED_BASE + 0);
}

/* ==========================================================================
 * THE CLOCK KICK — why the ARM7 is stopped, and how to get it moving
 * ==========================================================================
 * The three probes above settle what is wrong, and it is NOT "the ARM never
 * started":
 *
 *   drv=EA00002D   KOS's driver is resident (0xEA is the ARM `B` opcode).
 *   no assertion   snd_sh4_to_aica() opens with
 *                  assert_msg(q_cmd->valid, "Queue is not yet valid")
 *                  (snd_iface.c:84), that assert is live in libkallisti, the
 *                  port installs no assert handler, and the console carries no
 *                  ASSERTION line. The ONLY writer of that word is the ARM's
 *                  own arm_main() (arm/main.c:202). So the ARM7 executed.
 *   clk=0          AICA_MEM_CLOCK is incremented in exactly ONE place —
 *                  crt0.s's `fiq_timer`, the AICA Timer-A FIQ handler. The
 *                  main loop never writes it.
 *
 * Put together: the ARM ran, reached `timer_wait(10)` at the bottom of its
 * first main-loop pass (arm/main.c:224), and that is
 * `fin = timer + 10; while (timer <= fin);` — with the FIQ never delivered,
 * timer is pinned at 0 and the ARM spins there forever. `aicapos=0,0,0,0`
 * agrees exactly: main.c:218 ran `aica_get_pos()` once, on that first pass,
 * and never again.
 *
 * So the fix is to advance that word. The SH-4 can: it is plain SPU RAM on the
 * G2 bus, and 16 per pump clears the driver's own 10-tick wait once per call,
 * which is all it needs to keep servicing the command queue and refreshing the
 * 64 channel positions.
 *
 * ⚠️ IT SELF-DISABLES, and that is the load-bearing part. A missing Timer-A
 * FIQ is very likely a Flycast artefact — the port already boots on real
 * hardware, where the FIQ presumably fires — and on such a machine writing this
 * word from two sides would race the real handler. So the kick only ARMS after
 * the clock has been observed unchanged across DC_AICA_KICK_AFTER consecutive
 * pumps. If the FIQ works, the clock advances, the counter resets, and this
 * code never writes anything.
 *
 * Kill switch: -DDC_AICA_CLOCK_KICK=0. Reported as `kick=` so a hardware run
 * says which machine needed it. */
#ifndef DC_AICA_CLOCK_KICK
#define DC_AICA_CLOCK_KICK 1
#endif
#ifndef DC_AICA_KICK_AFTER
#define DC_AICA_KICK_AFTER 60u
#endif

#if DC_AICA_CLOCK_KICK
static u32 s_clk_last = 0, s_clk_stuck = 0, s_clk_drive = 0, s_clk_val = 0;
static u32 s_clk_kicks = 0;

static void dc_aica_clock_kick(void) {
    u32 clk = dc_aica_clock();

    if (!s_clk_drive) {
        if (clk == s_clk_last) {
            if (++s_clk_stuck >= (u32)(DC_AICA_KICK_AFTER)) {
                s_clk_drive = 1;
                s_clk_val = clk;
                DC_LOGE("[DC/AUDIO] AICA clock stuck at %u for %u pumps — "
                        "driving it from the SH-4 (Timer-A FIQ is not being "
                        "delivered to the ARM7)\n",
                        (unsigned)clk, (unsigned)DC_AICA_KICK_AFTER);
            }
        } else {
            s_clk_stuck = 0;
        }
        s_clk_last = clk;
    }

    if (s_clk_drive) {
        s_clk_val += 16u;    /* > the driver's own timer_wait(10) */
        g2_write_32(SPU_RAM_UNCACHED_BASE + DC_AICA_CLOCK, s_clk_val);
        s_clk_kicks++;
    }
}
#endif /* DC_AICA_CLOCK_KICK */
#endif
typedef void (*AIDMACallback)(void);
static AIDMACallback ai_dma_callback = NULL;

/* Volume scales the jaudio side reads (defined in game code under TARGET_PC). */
extern float pc_bgm_volume_scale, pc_se_volume_scale, pc_voice_volume_scale;
extern void  Na_PC_ApplyVolumes(void);

/* jaudio_NES/internal/audiothread.c:92. Declared rather than #included: the
 * header lives under include/jaudio_NES and pulling it in here would drag the
 * decomp's headers into a platform TU for one prototype. */
extern void pc_audio_process_frame(void);

/* ==========================================================================
 * snd_stream callback — the seam to KOS/AICA
 * ==========================================================================
 * KOS calls this asking for `req` bytes and expects a pointer to that many
 * bytes of PCM plus the actual count. It runs on KOS's stream thread, so the
 * ring must stay single-producer/single-consumer.
 *
 * VERIFY (M0): dc/sound/stream.h — snd_stream_init(), snd_stream_alloc(cb,
 * size), snd_stream_start(hnd, freq, stereo), snd_stream_poll(hnd), and the
 * callback signature (older KOS: void*(*)(snd_stream_hnd_t, int, int*)).
 */
/* THE STREAM BUFFER SIZE, AND WHY IT IS NOT SND_STREAM_BUFFER_MAX.
 *
 * This was the second of the two faults that made the port silent, and it is
 * pure arithmetic. `snd_stream_alloc(cb, size)` treats `size` as a PER-CHANNEL
 * buffer (snd_stream.c:410, `snd_mem_malloc(buffer_size * max_channels)`), and
 * snd_stream_fill() asks the callback for `size * chans` bytes
 * (snd_stream.c:693-706). At SND_STREAM_BUFFER_MAX = 65,536 that is a 131,072
 * byte request against a 4,096 byte scratch, so the callback clamped and
 * returned 4,096 — 3 % of what KOS asked for — and KOS advanced its write
 * position by that little. The whole AICA channel therefore kept whatever it
 * was allocated with, and the only bytes ever handed to it were the two prefill
 * calls inside snd_stream_start() (`cb=2 pulled=8192` in the report line, which
 * is exactly 2 x 4096). Those two ran at AIInit time, long before jaudio
 * produced a sample, so both zero-filled.
 *
 * DC_AUDIO_STREAM_BYTES is now sized to the PRODUCER, not to the SPU: the ring
 * above is RING_BUF_SAMPLES * 2 B = 16,384 B, so a per-channel 8,192 makes
 * KOS's largest possible request exactly 8,192 = half the ring. The AICA loop
 * becomes 4,096 samples ~= 128 ms at 32 kHz stereo and refills once ~64 ms has
 * drained, which the frame-driven pump clears even at 10 FPS.
 *
 * It also gives 56 KB back to sbrk: snd_stream_init() unconditionally
 * aligned_alloc(32, 65536) from the libc pool (snd_stream.c:351), which is the
 * STARVED half of the two-pool heap (kb/heap-two-pools.md). snd_stream_init_ex
 * takes the size, so nothing over-allocates. */
#ifndef DC_AUDIO_STREAM_BYTES
#define DC_AUDIO_STREAM_BYTES 8192
#endif

/* 32-byte aligned: stream.h documents "for best performance use 32-byte aligned
 * pointer", and the SPU store-queue path in snd_stream.c copies from it. */
static u8 s_stream_scratch[DC_AUDIO_STREAM_BYTES] __attribute__((aligned(32)));

static void* dc_audio_stream_cb(int hnd, int req, int* done) {
    int total = req / (int)sizeof(s16);
    u32 wp, rp, used;
    int avail, copy, i;
    s16* out = (s16*)s_stream_scratch;

    (void)hnd;
    if (req > (int)sizeof(s_stream_scratch)) {
        req = (int)sizeof(s_stream_scratch);
        total = req / (int)sizeof(s16);
    }

    wp = ring_write_pos;
    rp = ring_read_pos;
    used = wp - rp;
    if (used > RING_BUF_SAMPLES) {           /* producer lapped us */
        rp = wp - RING_BUF_SAMPLES;
        rp &= ~1u;                            /* stereo-align */
        used = wp - rp;
    }

    avail = (int)used & ~1;
    copy = (avail < total) ? avail : total;
    copy &= ~1;

    for (i = 0; i < copy; i++)
        out[i] = ring_buffer[(rp + i) & RING_BUF_MASK];
    if (copy < total)
        memset(&out[copy], 0, (size_t)(total - copy) * sizeof(s16));

    ring_read_pos = rp + (u32)copy;
    s_cb_calls++;
    s_cb_bytes += (u32)req;
    if (done) *done = req;
    return s_stream_scratch;
}

/* ==========================================================================
 * AI
 * ========================================================================== */
void AIInit(u8* stack) {
    (void)stack;
    if (audio_open) return;

    dc_mem_note(DCMEM_AUDIO, (ptrdiff_t)(sizeof(ring_buffer) + sizeof(s_stream_scratch)));

#if DC_AUDIO && !defined(DC_HOST_STUB)
    /* VERIFIED 2026-08-02 against the SDK image's own headers and source, which
     * is what the old "unverified API shape" note was waiting on:
     *   kos/kernel/arch/dreamcast/include/dc/sound/stream.h
     *     typedef void *(*snd_stream_callback_t)(snd_stream_hnd_t, int, int *);
     *     snd_stream_hnd_t is int, SND_STREAM_INVALID is -1.
     *   kos/kernel/arch/dreamcast/sound/snd_stream.c:697-720
     *     get_data(hnd, needed_bytes, &got_bytes) — despite the `smp_req` name
     *     the unit is BYTES, both in and out.
     * dc_audio_stream_cb already matched that contract exactly. */
    if (snd_stream_init_ex(2, DC_AUDIO_STREAM_BYTES) < 0) {
        DC_LOGE("[DC/AUDIO] snd_stream_init FAILED — silent run\n");
    } else {
        s_hnd = snd_stream_alloc(dc_audio_stream_cb, DC_AUDIO_STREAM_BYTES);
        if (s_hnd == SND_STREAM_INVALID) {
            DC_LOGE("[DC/AUDIO] snd_stream_alloc FAILED — silent run\n");
        } else {
            snd_stream_start(s_hnd, ai_dsp_sample_rate, 1 /* stereo */);
            DC_LOGE("[DC/AUDIO] stream up: hnd=%d rate=%u stereo\n",
                    s_hnd, (unsigned)ai_dsp_sample_rate);
        }
    }
#else
    (void)dc_audio_stream_cb;
    DC_LOGE("[DC/AUDIO] DC_AUDIO=0 — output device deliberately not opened\n");
#endif
    audio_open = 1;
    DC_LOGE("[DC/AUDIO] ring %u samples, rate %u\n",
            (unsigned)RING_BUF_SAMPLES, (unsigned)ai_dsp_sample_rate);
}

/* The game hands us a block of finished PCM. On GameCube this armed a DMA;
 * here it is a ring-buffer push, exactly as on PC.
 *
 * NOTE (§3.2): aictrl.c issues DCStoreRange on these DAC buffers, which is a
 * REAL writeback on SH-4. That happens before we get here, so the bytes we
 * read are coherent. */
void AIInitDMA(u32 addr, u32 size) {
    const s16* src = (const s16*)(uintptr_t)addr;
    u32 n = size / (u32)sizeof(s16);
    u32 wp, rp, used, freeSamples, i;

    n &= ~1u;                                 /* whole stereo frames */
    wp = ring_write_pos;
    rp = ring_read_pos;
    used = wp - rp;
    freeSamples = RING_BUF_SAMPLES - used;
    if (n > freeSamples) n = freeSamples & ~1u;

    for (i = 0; i < n; i++)
        ring_buffer[(wp + i) & RING_BUF_MASK] = src[i];

    ring_write_pos = wp + n;
}

void AIStartDMA(void) { }
void AIStopDMA(void)  { }

u32  AIGetDMAStartAddr(void) { return 0; }
u16  AIGetDMALength(void) { return 0; }
u32  AIGetStreamTrigger(void) { return 0; }
u32  AIGetStreamSampleCount(void) { return 0; }
void AISetStreamPlayState(u32 state) { (void)state; }
u32  AIGetStreamPlayState(void) { return 0; }
void AISetStreamSampleRate(u32 rate) { (void)rate; }
u32  AIGetStreamSampleRate(void) { return DC_AUDIO_SAMPLE_RATE; }
void AISetStreamVolLeft(u8 vol) { (void)vol; }
void AISetStreamVolRight(u8 vol) { (void)vol; }
u8   AIGetStreamVolLeft(void) { return 0; }
u8   AIGetStreamVolRight(void) { return 0; }
void AIResetStreamSampleCount(void) { }
void AISetDSPSampleRate(u32 rate) { ai_dsp_sample_rate = rate; }
u32  AIGetDSPSampleRate(void) { return ai_dsp_sample_rate; }

void* AIRegisterDMACallback(void* callback) {
    void* old = (void*)ai_dma_callback;
    ai_dma_callback = (AIDMACallback)callback;
    return old;
}

/* ==========================================================================
 * DSP — rspsim does everything in software, as on PC.
 * ========================================================================== */
void  DSPInit(void) { }
BOOL  DSPCheckMailToDSP(void) { return FALSE; }
BOOL  DSPCheckMailFromDSP(void) { return FALSE; }
u32   DSPReadMailFromDSP(void) { return 0; }
void  DSPSendMailToDSP(u32 mail) { (void)mail; }
void  DSPAssertInt(void) { }
void* DSPAddTask(void* task) { return task; }

/* ==========================================================================
 * Platform-side queries (names kept from the PC port; game code calls them)
 * ========================================================================== */
int pc_audio_get_buffer_fill(void) {
    return (int)(ring_write_pos - ring_read_pos);
}

int pc_audio_is_active(void) { return audio_open; }

void pc_audio_shutdown(void) {
    if (!audio_open) return;
    /* Real version: snd_stream_stop(s_hnd); snd_stream_destroy(s_hnd);
     * snd_stream_shutdown(); */
    audio_open = 0;
}

/* The PC build creates a real producer thread here. Deliberately NOT done on
 * DC — see the THREADING note at the top of the file. Audio is produced from
 * the main loop via audiothread.c's TARGET_PC path, which is what
 * dc_audio_pump() below drives. */
void pc_audio_start_producer_thread(void) {
    DC_LOG("[DC/AUDIO] single-threaded audio production (no producer thread)\n");
}

/* ==========================================================================
 * The pump — why there was no sound at all
 * ==========================================================================
 * MEASURED 2026-08-02: the jaudio synthesis pipeline never ticked ONCE on DC.
 * pc_audio_process_frame() (jaudio_NES/internal/audiothread.c:92) is the only
 * caller of Jac_UpdateDAC (aictrl.c:283), which is the only thing that ever
 * calls AIInitDMA with real PCM (aictrl.c:290). Its only caller in turn was
 * pc_audio_producer_func, the SDL thread in pc/src/pc_audio.c:43, started by
 * pc_audio_start_producer_thread — which DC overrides with the no-op above.
 * src/audio.c:50 had already removed the old per-game-frame call. So the chain
 * was cut at both ends: the single AIInitDMA the DC ever executed was
 * aictrl.c:70's init call with a zeroed buffer. The linker noticed before we
 * did — .text.pc_audio_process_frame was being dropped by --gc-sections.
 *
 * Everything downstream was a symptom of this, including the "[TRG_SE] NO FREE"
 * slot leak: Nap_ReadSubPort returns -1 when the group is disabled
 * (sub_sys.c:426), the sequencer that enables it never ran, and the free
 * condition at game64.c_inc:1026 tests !p5, which -1 never satisfies.
 *
 * BUDGETED, not free-running. The town already runs at ~9 FPS, so synthesis is
 * capped per frame and simply drops an audio frame when it runs out — the game
 * loop must never stall on audio. DC_AUDIO_BUDGET_US tunes it, DC_AUDIO=0
 * removes the whole thing. */
void dc_audio_pump(void) {
#if DC_AUDIO && !defined(DC_HOST_STUB)
    u64 t0;
    int frames = 0;

    if (!audio_open) return;

#if DC_AICA_CLOCK_KICK
    /* Before anything else: if the ARM7 is wedged, nothing downstream of it can
     * make progress, and every counter in this file reads as a ring bug. */
    dc_aica_clock_kick();
#endif

    t0 = dc_time_us();
    /* Fill toward the TOP of the ring, not the middle.
     *
     * MEASURED 2026-08-02: gating on `fill < RING/2` deadlocked. The consumer
     * stalled at fill=4480 (>4096), so synthesis never ran (synth_frames=0),
     * so the ring never changed, so it stayed above the threshold forever.
     * A producer must not refuse to produce because a stalled consumer left the
     * buffer half full — keep a headroom margin instead, so jaudio ticks every
     * frame regardless of what the AICA side is doing. That also matters beyond
     * sound: ticking the sequencer is what lets the SE slot table free itself
     * (game64.c_inc:1026 tests !p5, and Nap_ReadSubPort returns -1 while the
     * group is disabled). */
    /* THE BUDGET IS PREDICTIVE, and it has to be.
     *
     * MEASURED 2026-08-04, the run that made audio audible for the first time:
     * `synth_frames=600 budget_hits=600 us/600=15437894`, i.e. ONE jaudio frame
     * per pump at **25.7 ms each**, against a ~43 ms game frame. The old test
     * ran the frame first and only then asked whether the budget was blown, so
     * a 4,000 us budget was bounding nothing at all — it could not, because the
     * cheapest thing this loop can do already costs six times it.
     *
     * That is the honest shape of jaudio on a 200 MHz SH-4 with src/ at -O0:
     * one DAC frame is ~35 ms of audio for ~25.7 ms of CPU, so synthesis runs
     * at about 1.36x real time and takes ~60 % of the machine to stay level.
     * See kb/audio-cpu-cost.md.
     *
     * So the loop now predicts: it refuses to START a frame it cannot finish
     * inside the budget, using an EWMA of what one frame has actually cost.
     * The one exception is starvation — if the ring is nearly empty, a late
     * frame beats a silent one, so it runs anyway and the budget is exceeded
     * knowingly rather than accidentally. */
    for (;;) {
        int have = pc_audio_get_buffer_fill();
        u32 el, c0, cost;

        if (have >= (int)(RING_BUF_SAMPLES - DC_AUDIO_HEADROOM))
            break;                       /* the ring is full enough */

        el = (u32)(dc_time_us() - t0);
        if (s_synth_avg_us != 0u &&
            el + s_synth_avg_us > (u32)DC_AUDIO_BUDGET_US &&
            have > (int)(DC_AUDIO_MIN_FILL)) {
            s_pump_budget_hits++;
            break;
        }

        c0 = (u32)dc_time_us();
        pc_audio_process_frame();
        cost = (u32)dc_time_us() - c0;
        /* 3:1 EWMA. Fast enough to follow a scene change, slow enough that one
         * unlucky frame does not switch synthesis off for the next second. */
        s_synth_avg_us = s_synth_avg_us ? ((s_synth_avg_us * 3u + cost) >> 2)
                                        : cost;
        frames++;
    }
    s_pump_frames += (u32)frames;
    s_pump_usec += (u32)(dc_time_us() - t0);

    /* The return value matters: cb stuck at 2 for a whole 600 s run says the
     * consumer died early, and an unchecked poll cannot tell us that. */
    if (s_hnd != SND_STREAM_INVALID) {
        if (snd_stream_poll(s_hnd) < 0) s_poll_fail++;
    }

    if (++s_pump_calls >= 600u) {
        DC_LOGE("[DC/AUDIO] synth_us=%u (one jaudio DAC frame; the FPS cost of "
                "sound)\n", (unsigned)s_synth_avg_us);
        DC_LOGE("[DC/AUDIO] pump calls=%u synth_frames=%u budget_hits=%u "
                "cb=%u pulled=%u fill=%d pollfail=%u us/600=%u "
                "aicadrv=%08X aicaclk=%u kick=%u aicapos=%u,%u,%u,%u\n",
                (unsigned)s_pump_calls, (unsigned)s_pump_frames,
                (unsigned)s_pump_budget_hits, (unsigned)s_cb_calls,
                (unsigned)s_cb_bytes, pc_audio_get_buffer_fill(),
                (unsigned)s_poll_fail, (unsigned)s_pump_usec,
                (unsigned)dc_aica_drv_word(), (unsigned)dc_aica_clock(),
#if DC_AICA_CLOCK_KICK
                (unsigned)s_clk_kicks,
#else
                0u,
#endif
                (unsigned)dc_aica_pos(0), (unsigned)dc_aica_pos(1),
                (unsigned)dc_aica_pos(2), (unsigned)dc_aica_pos(3));
        s_pump_calls = 0; s_pump_frames = 0; s_pump_budget_hits = 0;
        s_pump_usec = 0;
    }
#endif
}

void pc_audio_update_volumes(void) {
    pc_bgm_volume_scale   = 1.0f;
    pc_se_volume_scale    = 1.0f;
    pc_voice_volume_scale = 1.0f;
    Na_PC_ApplyVolumes();
}
