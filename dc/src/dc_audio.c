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

/* DC_AUDIO=0 is the kill switch: no output device, no synthesis, byte-identical
 * to the silent build. DC_AUDIO_BUDGET_US caps how long one frame may spend in
 * jaudio synthesis before giving up until the next frame. */
#ifndef DC_AUDIO
#define DC_AUDIO 1
#endif
#ifndef DC_AUDIO_BUDGET_US
#define DC_AUDIO_BUDGET_US 4000
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
/* 32-byte aligned: stream.h documents "for best performance use 32-byte aligned
 * pointer", and the SPU store-queue path in snd_stream.c copies from it. */
static u8 s_stream_scratch[4096] __attribute__((aligned(32)));

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
    if (snd_stream_init() < 0) {
        DC_LOGE("[DC/AUDIO] snd_stream_init FAILED — silent run\n");
    } else {
        s_hnd = snd_stream_alloc(dc_audio_stream_cb, SND_STREAM_BUFFER_MAX);
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
    while (pc_audio_get_buffer_fill() <
           (int)(RING_BUF_SAMPLES - DC_AUDIO_HEADROOM)) {
        pc_audio_process_frame();
        frames++;
        if ((u32)(dc_time_us() - t0) >= (u32)DC_AUDIO_BUDGET_US) {
            s_pump_budget_hits++;
            break;
        }
    }
    s_pump_frames += (u32)frames;
    s_pump_usec += (u32)(dc_time_us() - t0);

    /* The return value matters: cb stuck at 2 for a whole 600 s run says the
     * consumer died early, and an unchecked poll cannot tell us that. */
    if (s_hnd != SND_STREAM_INVALID) {
        if (snd_stream_poll(s_hnd) < 0) s_poll_fail++;
    }

    if (++s_pump_calls >= 600u) {
        DC_LOGE("[DC/AUDIO] pump calls=%u synth_frames=%u budget_hits=%u "
                "cb=%u pulled=%u fill=%d pollfail=%u us/600=%u\n",
                (unsigned)s_pump_calls, (unsigned)s_pump_frames,
                (unsigned)s_pump_budget_hits, (unsigned)s_cb_calls,
                (unsigned)s_cb_bytes, pc_audio_get_buffer_fill(),
                (unsigned)s_poll_fail, (unsigned)s_pump_usec);
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
