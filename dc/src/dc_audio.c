/* dc_audio.c - AI (Audio Interface) + DSP surface for Dreamcast.
 *
 * Replaces pc/src/pc_audio.c (SDL2). The ring buffer and the AI*/DSP* surface
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
typedef void (*AIDMACallback)(void);
static AIDMACallback ai_dma_callback = NULL;

/* Volume scales the jaudio side reads (defined in game code under TARGET_PC). */
extern float pc_bgm_volume_scale, pc_se_volume_scale, pc_voice_volume_scale;
extern void  Na_PC_ApplyVolumes(void);

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
static u8 s_stream_scratch[4096];

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

    /* Real version:
     *     snd_stream_init();
     *     s_hnd = snd_stream_alloc(dc_audio_stream_cb, SND_STREAM_BUFFER_MAX);
     *     snd_stream_start(s_hnd, DC_AUDIO_SAMPLE_RATE, 1);
     * left unwired because the exact snd_stream API shape is unverified and a
     * wrong callback signature is a silent memory corruption, not a compile
     * error. The ring buffer below is fully live in the meantime, so the game's
     * audio timing still advances and AIInitDMA still consumes samples. */
    (void)dc_audio_stream_cb;
    DC_UNIMPLEMENTED_NOTE("KOS snd_stream_init/alloc/start (see PLAN 3.4 stage A)");
    audio_open = 1;
    DC_LOGE("[DC/AUDIO] ring %u samples, rate %u (no output device yet)\n",
            (unsigned)RING_BUF_SAMPLES, (unsigned)DC_AUDIO_SAMPLE_RATE);
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
 * the main loop via audiothread.c's TARGET_PC path. */
void pc_audio_start_producer_thread(void) {
    DC_LOG("[DC/AUDIO] single-threaded audio production (no producer thread)\n");
}

void pc_audio_update_volumes(void) {
    pc_bgm_volume_scale   = 1.0f;
    pc_se_volume_scale    = 1.0f;
    pc_voice_volume_scale = 1.0f;
    Na_PC_ApplyVolumes();
}
