/* dc_aica.c — stage B, the HARDWARE half: AICA channels, sound RAM, the bank.
 *
 * =============================================================================
 * WHY THIS FILE EXISTS
 * =============================================================================
 * The first hardware gprof of this port (2026-08-12) put `RspStart` at 14.4 ms
 * of a 76.4 ms frame — 18.9 % of busy CPU, the biggest single symbol on the
 * machine, and 2.13x its Flycast share. It is the only subsystem that
 * measurably gets WORSE on silicon. `RspStart` is jaudio's software RSP: it
 * decodes VADPCM, resamples, envelopes and mixes, per voice, per update, four
 * updates per DAC frame.
 *
 * The AICA has 64 hardware channels that do exactly that, in silicon, for free.
 * This file drives them.
 *
 * =============================================================================
 * THE THREE DESIGN DECISIONS THAT ARE NOT OBVIOUS
 * =============================================================================
 *
 * 1. ⭐ IT IS INCREMENTAL, NOT A CUTOVER (kb/audio-aica-offload.md §11).
 *    AICA sums all 64 channels to the DAC and the software path already ends at
 *    two of them (snd_stream holds 0 and 1). So a voice played on a hardware
 *    channel and a voice rendered by RspStart into the stream MIX IN HARDWARE,
 *    for free. Every voice moved is CPU saved; there is no cliff edge at 100 %
 *    coverage, and the software path stays live as the oracle throughout.
 *    The 24 samples the AICA structurally cannot play just never move.
 *
 * 2. 🔴 THE ARM COMMAND QUEUE IS NOT THE TRANSPORT (§8, and this contradicts
 *    the obvious approach). `snd_sh4_to_aica()` (snd_iface.c:76-117) never
 *    compares `head` against `tail` — outrun the ARM and it silently overwrites
 *    unread packets, after which the ARM's `while (head != tail)` walks 32 KB of
 *    garbage as commands. There is no error return and no recovery. Service
 *    latency is ~2.3-2.7 ms against a 229 Hz update tick, i.e. no margin. And
 *    `AICA_CH_CMD_UPDATE` honours only FREQ/VOL/PAN, so anything else needs a
 *    full START, which key-offs and key-ons.
 *    ⇒ Volume/pan/pitch go by DIRECT G2 REGISTER WRITES against SH-4 shadows,
 *      batched under ONE g2 lock per service call. KOS itself reads channel
 *      registers straight from the SH-4 while the ARM driver runs
 *      (snd_iface.c:217) and crt0.s:50-62 handles bus arbitration, so this is
 *      an architected path, not a trick.
 *
 * 3. ⚠️ KEY-ON IS ALSO DIRECT, AND THAT IS A DELIBERATE DEVIATION FROM §8.
 *    §8 keeps key-on/key-off on the ARM queue "where its latency is what you
 *    actually want". Two facts about THIS port argue otherwise:
 *      - The ARM7's Timer-A FIQ is not delivered under Flycast, so its main
 *        loop is parked in `timer_wait(10)` except when dc_audio.c's clock kick
 *        releases it (see dc_audio.c, dc_aica_clock_kick). A transport whose
 *        liveness depends on that workaround firing at the right instant is not
 *        a transport; a dropped key-on is a note that never sounds and a
 *        dropped key-off is a note that never stops.
 *      - The queue's `timestamp` field — §11's other candidate fix for latency
 *        skew — is denominated in the SAME wedged jiffy clock.
 *    So key-on is a register write, and the ~100 ms skew is fixed on the SH-4
 *    side by an explicit delay (see s_pend below), which is §11 fix (1) and is
 *    the half of the problem the queue could not have solved anyway.
 *    `DC_AICA_KEYON_DELAY=0` disables the delay for an A/B.
 *
 * =============================================================================
 * WHAT THIS FILE DOES NOT DO
 * =============================================================================
 * - No reverb. The software path's `Nas_CpuFX` still runs for the voices that
 *   stay in software; a diverted voice loses its reverb send. That is an
 *   AUDIBLE change, not a transparent one, and nobody has costed an AICA DSP
 *   send (kb/audio-aica-offload.md §12). Diverting only the reverb-less voices
 *   is the seam's policy question, not this file's.
 * - No arbitrary envelopes. AICA's 4-stage hardware EG cannot represent
 *   jaudio's ADSR_HANG / ADSR_GOTO / ADSR_RESTART, so the EG is pinned open
 *   (AR=31, RR=31) and the envelope is delivered as a 229 Hz stream of TL
 *   writes from the SH-4, exactly as §8 specifies.
 * - No cleverness about the arena. Allocation is a bump pointer and running out
 *   resets the whole thing (see dc_aica_arena_alloc). `resets=` on the report
 *   line says whether that is costing anything; if it is, the fix is the
 *   per-sequence residency policy sketched in §9, which is deliberately NOT
 *   built yet because nothing has measured that it is needed.
 *
 * KILL SWITCH: -DDC_AICA=0 (the default). This file then compiles to nothing,
 * the link carries no --wrap, and the image is byte-identical.
 */
#include "dc_platform.h"
#include "dc_mem_ledger.h"
#include "dc_aica.h"

#ifndef DC_AICA
#define DC_AICA 0
#endif

#if DC_AICA && !defined(DC_HOST_STUB)

#include <dc/g2bus.h>
#include <dc/spu.h>
#include <dc/sound/sound.h>
#include <dc/sound/sfxmgr.h>

/* ===========================================================================
 * Knobs
 * ===========================================================================*/

/* DC_AICA_VOICES is defined in dc_aica.h — the seam needs the same number. */

/* Bytes of AICA sound RAM reserved for sample payloads.
 *
 * The ceiling is 2,097,152 − 196,608 (KOS reserve) = 1,900,544 allocatable
 * (snd_mem.c:100-103), MINUS snd_stream's two 8,192 B buffers. 1,572,864 leaves
 * ~310 KB of slack for snd_stream, the SFX pool and any future streamer.
 * ⚠️ snd_mem_available() returns the LARGEST FREE BLOCK, not the total. */
#ifndef DC_AICA_ARENA
#define DC_AICA_ARENA 1572864
#endif

/* Main-RAM staging buffer for a payload on its way from the disc to sound RAM.
 * 32,768 is not arbitrary: a payload is 4 bits/sample and the AICA's LSA/LEA
 * are 16-bit sample offsets, so the largest sample the pack can carry is 65,534
 * samples = 32,767 B. One buffer therefore holds ANY single payload, and the
 * chunk loop below exists only for the disc yield, not for capacity. */
#ifndef DC_AICA_STAGE_B
#define DC_AICA_STAGE_B 32768
#endif

#ifndef DC_AICA_BANK_PATH
#define DC_AICA_BANK_PATH "/cd/aicabank.pak"
#endif

/* Delay a hardware key-on to match the software path's buffering (§11). */
#ifndef DC_AICA_KEYON_DELAY
#define DC_AICA_KEYON_DELAY 1
#endif

/* ⚠️ AN ESTIMATE, AND THE ONLY ONE IN THIS FILE. dc_audio.c knows our ring's
 * fill exactly, but not how much of what snd_stream already copied into SPU RAM
 * is still unplayed — KOS exposes a play position, not a queue depth, and the
 * two are not the same number while the channel is looping over a double
 * buffer. Half the per-channel stream buffer is the expectation of a uniformly
 * distributed play position, which is the honest guess.
 *
 * THIS IS THE KNOB YOU TUNE BY EAR. Too small and offloaded voices lead;
 * too large and they lag. It is in output frames (stereo pairs). */
#ifndef DC_AICA_SPU_LATENCY_FRAMES
#define DC_AICA_SPU_LATENCY_FRAMES 2048
#endif

/* ===========================================================================
 * The AICA, as seen from the SH-4
 * ===========================================================================
 * ⚠️ TWO ADDRESS SPACES, AND CONFLATING THEM IS THE CLASSIC MISTAKE.
 *   sound RAM  : SH-4 0xA0800000 (SPU_RAM_UNCACHED_BASE), AICA-relative 0.
 *   registers  : SH-4 0xA0700000. Channel blocks start AT 0x00700000 with a
 *                0x80 stride; 0x2800 is the COMMON register area, not channel
 *                80. KOS reads a channel register exactly this way at
 *                snd_iface.c:217.
 * A sample's address in register 0x00/0x04 is its sound-RAM OFFSET, which is
 * what snd_mem_malloc() returns. */
#define AICA_CHN_BASE   (MEM_AREA_P2_BASE + 0x00700000u)
#define AICA_CHN(ch)    (AICA_CHN_BASE + 0x80u * (unsigned)(ch))

#define AICA_R_CTRL     0x00u   /* KEYONEX|KEYONB|LPCTL|PCMS|SA[22:16]        */
#define AICA_R_SA_LO    0x04u   /* SA[15:0]                                   */
#define AICA_R_LSA      0x08u   /* loop start, IN SAMPLES for every format    */
#define AICA_R_LEA      0x0Cu   /* loop end,   IN SAMPLES for every format    */
#define AICA_R_AEG      0x10u   /* D2R/D1R/AR                                 */
#define AICA_R_EG2      0x14u   /* D2L/RR                                     */
#define AICA_R_PITCH    0x18u   /* OCT (b14-11, signed) | FNS (b10-0)         */
#define AICA_R_PAN      0x24u   /* byte 36 DIPAN, byte 37 DISDL               */
#define AICA_R_VOL      0x28u   /* byte 40 LPF ctl, byte 41 TL (0 = loudest)  */

#define AICA_KEYONEX    0x8000u
#define AICA_KEYONB     0x4000u
#define AICA_LOOP       0x0200u

#define AICA_SM_ADPCM       2u  /* Yamaha 4-bit ADPCM                          */
#define AICA_SM_ADPCM_LS    3u  /* ...the "long stream" variant KOS's looping
                                 * ring-buffer streamer uses exclusively
                                 * (snd_stream.c:602-603). See s_sm below. */

/* ⚠️ WHICH ADPCM MODE FOR A LOOPED INSTRUMENT IS AN OPEN HARDWARE QUESTION.
 * kb/audio-aica-offload.md §6: if the AICA resets its ADPCM decoder state at
 * the loop point, ~163 of the 451 looping samples can click; if it carries the
 * state, ZERO can, regardless of the encoding. The circumstantial evidence
 * points to CARRIED for mode 3 — KOS drives its looping stream in mode 3 over a
 * double buffer refilled in time order, which cannot work if state is reset —
 * but that is an inference, not a documented hardware statement, and Flycast's
 * ADPCM is a reimplementation so Flycast cannot settle it (measurement rule 12).
 * Mode 3 is therefore the default for LOOPING samples and mode 2 for one-shots,
 * and DC_AICA_SM_LOOP exists so a burn can A/B the two on identical data. */
#ifndef DC_AICA_SM_LOOP
#define DC_AICA_SM_LOOP AICA_SM_ADPCM_LS
#endif

/* AICA's TL is LOGARITHMIC and 0 is loudest. Verbatim from the ARM driver
 * (aica.c:65-86), which generates it as logs[i] = 16*log2(255/i). It lives here
 * because the ARM's copy is on the other side of the G2 bus and this file never
 * asks the ARM for anything. */
static const unsigned char s_logs[256] = {
    255, 127, 111, 102,  95,  90,  86,  82,  79,  77,  74,  72,  70,  68,  66,  65,
     63,  62,  61,  59,  58,  57,  56,  55,  54,  53,  52,  51,  50,  50,  49,  48,
     47,  47,  46,  45,  45,  44,  43,  43,  42,  42,  41,  41,  40,  40,  39,  39,
     38,  38,  37,  37,  36,  36,  35,  35,  34,  34,  34,  33,  33,  33,  32,  32,
     31,  31,  31,  30,  30,  30,  29,  29,  29,  28,  28,  28,  27,  27,  27,  27,
     26,  26,  26,  25,  25,  25,  25,  24,  24,  24,  24,  23,  23,  23,  23,  22,
     22,  22,  22,  21,  21,  21,  21,  20,  20,  20,  20,  20,  19,  19,  19,  19,
     18,  18,  18,  18,  18,  17,  17,  17,  17,  17,  17,  16,  16,  16,  16,  16,
     15,  15,  15,  15,  15,  15,  14,  14,  14,  14,  14,  14,  13,  13,  13,  13,
     13,  13,  12,  12,  12,  12,  12,  12,  11,  11,  11,  11,  11,  11,  11,  10,
     10,  10,  10,  10,  10,  10,   9,   9,   9,   9,   9,   9,   9,   8,   8,   8,
      8,   8,   8,   8,   8,   7,   7,   7,   7,   7,   7,   7,   7,   6,   6,   6,
      6,   6,   6,   6,   6,   5,   5,   5,   5,   5,   5,   5,   5,   5,   4,   4,
      4,   4,   4,   4,   4,   4,   4,   3,   3,   3,   3,   3,   3,   3,   3,   3,
      2,   2,   2,   2,   2,   2,   2,   2,   2,   2,   1,   1,   1,   1,   1,   1,
      1,   1,   1,   1,   1,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0,   0
};

/* 0 left / 128 centre / 255 right -> DIPAN (b4 side, b3-0 magnitude).
 * Verbatim from aica.c's calc_aica_pan. */
static unsigned int dc_aica_pan_bits(unsigned int x) {
    if (x == 0x80u)  return 0u;
    if (x <  0x80u)  return 0x10u | ((0x7Fu - x) >> 3);
    return (x - 0x80u) >> 3;
}

/* freq(Hz) -> OCT/FNS, where freq = 44100 * 2^OCT * (1 + FNS/1024).
 * The same search the ARM runs in aica_freq(), moved to the SH-4 so a pitch
 * change is one register write instead of a queue packet. */
static unsigned int dc_aica_pitch_bits(unsigned int freq) {
    unsigned int base = 5644800u;          /* 44100 << 7 */
    int hi = 7;
    unsigned int lo;

    if (freq == 0u) return 0u;
    while (freq < base && hi > -8) { base >>= 1; hi--; }
    lo = (freq << 10) / base;
    return (((unsigned int)(hi & 0xF)) << 11) | (lo & 1023u);
}

/* ===========================================================================
 * Voice state — SHADOWS, because every register here is write-only in practice
 * ===========================================================================
 * Two reasons the shadows are mandatory rather than an optimisation:
 *   - 0x28 holds the LPF control byte AND the TL byte in one dword, and KOS
 *     only ever touches AICA registers from the SH-4 with 32-BIT accesses
 *     (8-bit G2 writes to registers are unverified). Writing TL therefore means
 *     writing a dword we must already know the rest of.
 *   - Reading back over G2 to do a read-modify-write would cost a bus turnaround
 *     per voice per update, which is the traffic this design exists to avoid.
 */
typedef struct {
    unsigned char in_use;
    unsigned char ch;          /* the hardware channel index, 0..63           */
    unsigned char dirty;       /* DIRTY_* below                               */
    unsigned char keyed;       /* KEYONB is currently set                     */

    unsigned int  r_ctrl;      /* 0x00 without the key bits                   */
    unsigned int  r_pitch;     /* 0x18                                        */
    unsigned int  r_pan;       /* 0x24                                        */
    unsigned int  r_vol;       /* 0x28                                        */

    unsigned int  sram;        /* sound-RAM offset of the sample it holds     */
    unsigned int  due_us;      /* delayed key-on deadline, 0 = none pending   */
    /* ONE-SHOT LIFETIME, AND IT IS MEASURED FROM KEY-ON, NOT FROM SETUP.
     * The first version armed the deadline in dc_aica_voice_start(), i.e. up to
     * DC_AICA_SPU_LATENCY_FRAMES before the note is actually allowed to sound —
     * so every one-shot shorter than the ~130 ms skew correction was reaped
     * while still pending and NEVER PLAYED. MEASURED: keyon=448 against
     * keyoff=853, i.e. about half the notes silently cancelled. `life_us` is
     * the duration; `end_us` is only set once the channel is really keyed on. */
    unsigned int  life_us;     /* 0 = loops, never expires                    */
    unsigned int  end_us;      /* absolute, valid only while keyed            */
} dc_aica_voice;

#define DIRTY_PITCH  0x1u
#define DIRTY_PAN    0x2u
#define DIRTY_VOL    0x4u

static dc_aica_voice s_v[DC_AICA_VOICES];
static int s_nvoices = 0;
static int s_ready   = 0;

/* Counters. Every one of these exists to make a specific failure legible on the
 * console rather than as "it sounds wrong". */
static unsigned int s_keyons = 0, s_keyoffs = 0, s_pend_late = 0;
/* Which key-on path ran. `now` should be ~0 whenever the ring has anything in
 * it — a large `now` count means the latency-skew correction is not arming and
 * every offloaded voice is playing ~100 ms early (§11). */
static unsigned int s_start_now = 0, s_start_delayed = 0;
static unsigned int s_lat_rate = 0, s_lat_buf = 0, s_lat_us = 0;  /* last computed */
static unsigned int s_loads = 0, s_load_bytes = 0, s_load_fail = 0;
static unsigned int s_lookups = 0, s_miss = 0, s_toolong = 0;
static unsigned int s_resets = 0, s_flushes = 0, s_writes = 0;
static unsigned int s_evicted = 0;
static unsigned int s_load_defer = 0;  /* refused: nested inside a disc read */

/* ===========================================================================
 * The register batch — ONE g2 lock for the whole thing
 * ===========================================================================
 * NEVER g2_write_32() per voice: each takes a full lock, which is an IRQ
 * disable plus three DMA-suspend register writes plus a FIFO drain. One lock
 * per batch is 229 locks/s and ~5,500 raw writes/s (§8).
 *
 * g2_fifo_wait() every 8 writes because the G2 write FIFO is 32 bytes.
 */
static void dc_aica_flush(void) {
    int i, n = 0;

    if (!s_ready) return;

    {   g2_lock_scoped();
        for (i = 0; i < s_nvoices; i++) {
            dc_aica_voice* v = &s_v[i];
            unsigned int base;

            if (!v->in_use || !v->dirty) continue;
            base = AICA_CHN(v->ch);

            if (v->dirty & DIRTY_PITCH) {
                if ((n++ & 7) == 0) g2_fifo_wait();
                g2_write_32_raw(base + AICA_R_PITCH, v->r_pitch);
            }
            if (v->dirty & DIRTY_PAN) {
                if ((n++ & 7) == 0) g2_fifo_wait();
                g2_write_32_raw(base + AICA_R_PAN, v->r_pan);
            }
            if (v->dirty & DIRTY_VOL) {
                if ((n++ & 7) == 0) g2_fifo_wait();
                g2_write_32_raw(base + AICA_R_VOL, v->r_vol);
            }
            v->dirty = 0;
        }
    }
    s_writes += (unsigned int)n;
    if (n) s_flushes++;
}

/* ===========================================================================
 * The bank: aicabank.pak
 * ===========================================================================
 * Layout (tools/dcaudio/pack.py, ALL BIG-ENDIAN — the packer is a host tool and
 * the SH-4 is little-endian, so every word below is byte-swapped on read):
 *
 *   header, 24 B   'ACAB' | version=1 | n_entries | index_off=24 | data_off |
 *                  reserved
 *   index, 28 B x n_entries, STRICTLY ASCENDING BY device_addr:
 *                  device_addr | data_off (relative to header.data_off) |
 *                  data_len | n_samples | loop_start | loop_end | flags
 *   payloads       raw AICA 4-bit ADPCM, low nibble first, no block structure
 *                  at all, each padded to a 32-byte boundary.
 *
 * The index is loaded whole into main RAM (1,157 x 28 = 32,396 B) because the
 * alternative is a binary search over a CD-R, i.e. ~11 seeks of 100-200 ms per
 * note-on. The payloads stay on the disc and come in on demand.
 */
#define PAK_MAGIC   0x41434142u          /* 'ACAB' */
#define PAK_VERSION 1u
#define PAK_ENT_W   7u                   /* words per index entry */

static file_t       s_fd = -1;
static unsigned int s_data_off = 0;      /* file offset of the payload region */
static unsigned int s_nent = 0;
static unsigned int* s_idx  = NULL;      /* s_nent * PAK_ENT_W words, host order */
static unsigned int* s_sram = NULL;      /* s_nent sound-RAM offsets, 0 = absent */

static unsigned int s_arena_base = 0, s_arena_size = 0, s_arena_used = 0;

static unsigned char s_stage[DC_AICA_STAGE_B] __attribute__((aligned(32)));

static unsigned int be32(const unsigned char* p) {
    return ((unsigned int)p[0] << 24) | ((unsigned int)p[1] << 16) |
           ((unsigned int)p[2] << 8)  |  (unsigned int)p[3];
}

/* Binary search the index. Returns the entry number, or -1. */
static int dc_aica_find(unsigned int dev) {
    int lo = 0, hi = (int)s_nent - 1;

    while (lo <= hi) {
        int mid = (lo + hi) >> 1;
        unsigned int k = s_idx[(unsigned int)mid * PAK_ENT_W];
        if (k == dev) return mid;
        if (k <  dev) lo = mid + 1; else hi = mid - 1;
    }
    return -1;
}

/* ⚠️ A BUMP ALLOCATOR THAT RESETS, AND THE HONEST NAME FOR THAT IS "CRUDE".
 *
 * Variable-size samples with an LRU is a real allocator with real
 * fragmentation, and nothing has yet measured that this port needs one. What it
 * needs first is a number: `resets=` says how often the arena actually fills.
 * Until that number is non-trivial, a scheme whose whole failure mode is
 * "everything reloads" is preferable to one whose failure mode is a subtle
 * coalescing bug that plays a neighbour's sample.
 *
 * 🔴 A RESET INVALIDATES EVERY RESIDENT SAMPLE, INCLUDING ONES BEING PLAYED, so
 * it MUST key off every voice first — otherwise a channel keeps reading an
 * address that now holds a different instrument, which is exactly the "plays
 * noise but nothing asserts" failure that cost this project an afternoon on W1.
 */
static unsigned int dc_aica_arena_alloc(unsigned int bytes) {
    unsigned int need = (bytes + 31u) & ~31u;
    unsigned int off;
    int i;

    if (need > s_arena_size) return 0;               /* can never fit */

    if (s_arena_used + need > s_arena_size) {
        for (i = 0; i < s_nvoices; i++)
            if (s_v[i].in_use) dc_aica_voice_stop(i);
        memset(s_sram, 0, s_nent * sizeof(unsigned int));
        s_evicted += s_arena_used;
        s_arena_used = 0;
        s_resets++;
        DC_LOGE("[DC/AICA] arena full — reset (%u B reclaimed, reset #%u)\n",
                (unsigned)s_arena_size, (unsigned)s_resets);
    }

    off = s_arena_base + s_arena_used;
    s_arena_used += need;
    return off;
}

int dc_aica_lookup(unsigned int dev, dc_aica_sample* out) {
    int e;
    const unsigned int* r;

    if (!s_ready) return -1;
    s_lookups++;
    e = dc_aica_find(dev);
    if (e < 0) { s_miss++; return -1; }

    r = &s_idx[(unsigned int)e * PAK_ENT_W];
    out->dev        = r[0];
    out->sram       = s_sram[e];
    out->n_samples  = r[3];
    out->loop_start = r[4];
    out->loop_end   = r[5];
    out->flags      = r[6];
    return 0;
}

int dc_aica_ensure(unsigned int dev, dc_aica_sample* out) {
    int e;
    const unsigned int* r;
    unsigned int dlen, dstart, sram, done;

    if (!s_ready) return -1;
    s_lookups++;
    e = dc_aica_find(dev);
    if (e < 0) { s_miss++; return -1; }

    r    = &s_idx[(unsigned int)e * PAK_ENT_W];
    dlen = r[2];

    /* The 24 that structurally cannot play on a hardware channel: AICA's
     * LSA/LEA are 16-bit SAMPLE offsets, so 65,534 is the ceiling and the pack
     * carries these with metadata but no payload. They are 21 % of the bank's
     * bytes and include the longest musical material in the game, so "go
     * silent" would be very audible — the contract is that they stay in
     * software, which the seam implements by simply not diverting them. */
    if ((r[6] & DC_AICA_F_TOO_LONG) || dlen == 0u) { s_toolong++; return -1; }

    out->dev        = r[0];
    out->n_samples  = r[3];
    out->loop_start = r[4];
    out->loop_end   = r[5];
    out->flags      = r[6];

    if (s_sram[e]) { out->sram = s_sram[e]; return 0; }

    /* 🔴 NO NESTED DISC I/O. If we are here because the audio pump was called
     * from inside somebody else's blocking read (dc_audio_disc_yield), the
     * fs_iso9660 driver already has a request in flight and a second one
     * re-enters it. The outer read then returns the wrong bytes — silently, and
     * the asset it was fetching draws as garbage. That is an audio change
     * corrupting the picture, which is exactly the shape of what was reported
     * on 2026-08-13.
     *
     * ⚠️ UNVERIFIED AS THE CAUSE. The store-queue bug fixed above is proven by
     * construction; this one is the strongest remaining candidate and it has
     * NOT been shown to be the fault. It is guarded anyway because nested CD
     * reads are wrong regardless of whether they are what broke the frame.
     *
     * Cost of the guard: the note stays in software for this frame. It is a
     * hybrid by design (§11) — that is the fallback working, not a failure. */
    if (dc_audio_in_disc_yield) { s_load_defer++; return -1; }

    sram = dc_aica_arena_alloc(dlen);
    if (!sram) { s_load_fail++; return -1; }

    /* ⚠️ The reset inside dc_aica_arena_alloc() may have cleared s_sram[e]
     * (it clears all of them), which is fine — we are about to set it. */

    dstart = s_data_off + r[1];
    if (fs_seek(s_fd, (off_t)dstart, SEEK_SET) < 0) { s_load_fail++; return -1; }

    for (done = 0; done < dlen; ) {
        unsigned int want = dlen - done;
        ssize_t got;

        if (want > (unsigned int)sizeof(s_stage)) want = (unsigned int)sizeof(s_stage);
        got = fs_read(s_fd, s_stage, (size_t)want);

        /* 🔴 A SHORT READ IS NOT AN ERROR ON THIS PLATFORM — it is the normal
         * behaviour of fs_iso9660 on a CD-R across a sector boundary, and
         * treating it as failure is the bug S15-5 fixed elsewhere in this tree.
         * Loop on what we got; only <= 0 is fatal. */
        if (got <= 0) { s_load_fail++; return -1; }

        /* 🔴 `spu_memload()`, NEVER `spu_memload_sq()`. THIS IS NOT A
         * PERFORMANCE CHOICE — THE SQ VARIANT CORRUPTS THE FRAME.
         *
         * REPORTED AS "the graphics are wrong", 2026-08-13, from an audio-only
         * change, which is what makes it worth writing down at length.
         *
         * `spu_memload_sq()` -> `sq_cpy()` -> `sq_lock()`, and `sq_lock()`
         * (hardware/sq.c:68) does TWO things this file has no right to do:
         *   1. `SET_QACR_REGS(...)` — it REPOINTS QACR0/QACR1 at sound RAM.
         *   2. it writes through SQ0/SQ1 themselves, the two 32-byte store
         *      queues.
         *
         * dc_pvr.c's own header says where that lands: `pvr_list_begin()` has
         * already done `sq_lock((void *)PVR_TA_INPUT)` for the open list and
         * `pvr_list_finish()` does the unlock, so "QACR is therefore set up for
         * the TA across exactly the window in which this function can run", and
         * `pvr_dr_target()`/`pvr_dr_commit()` alternate SQ0/SQ1 inside it. The
         * SQ holds the PREVIOUS primitive's words until overwritten.
         *
         * And this function runs INSIDE that window, by two routes: the audio
         * pump on a logic tick, and — much worse — `dc_audio_disc_yield()`,
         * which fires from inside any blocking disc read, i.e. from anywhere at
         * all, including mid-draw. So a sample load could repoint QACR at sound
         * RAM and overwrite a half-filled vertex, and the TA would receive
         * 4-bit ADPCM as geometry. Garbled polygons from a change that touches
         * no rendering code.
         *
         * `spu_memload()` (hardware/spu.c:43) is plain `g2_write_block_32` with
         * a `g2_fifo_wait` — no QACR, no store queue, no `sq_mutex`. The whole
         * bank's resident set is ~61 KB per run across ~9 loads, so the G2 path
         * costs nothing measurable and is the only correct choice here.
         *
         * ⚠️ THE SAME BAN APPLIES TO ANY FUTURE UPLOAD PATH IN THIS FILE,
         * including a "faster" one. If the store queues ever become necessary,
         * they must be taken outside the PVR list, not inside it. */
        spu_memload((uintptr_t)(sram + done), s_stage, (size_t)(((unsigned int)got + 3u) & ~3u));
        done += (unsigned int)got;

        /* Produce audio inside the stall. We are almost certainly already
         * inside pc_audio_process_frame() here (the seam calls us from a
         * note-on), so this takes dc_audio.c's poll-only path — which is the
         * one that stops the AICA repeating its last fragment during a
         * 100-200 ms seek. */
        dc_audio_disc_yield();
    }

    s_sram[e] = sram;
    s_loads++;
    s_load_bytes += dlen;
    out->sram = sram;
    return 0;
}

/* ===========================================================================
 * Voices
 * ===========================================================================*/

int dc_aica_voice_alloc(void) {
    int i;
    if (!s_ready) return -1;
    for (i = 0; i < s_nvoices; i++)
        if (!s_v[i].in_use) { s_v[i].in_use = 1; s_v[i].dirty = 0; return i; }
    return -1;
}

void dc_aica_voice_free(int v) {
    if (!s_ready || v < 0 || v >= s_nvoices) return;
    dc_aica_voice_stop(v);
    s_v[v].in_use = 0;
}

/* Key off. aica_stop()'s exact sequence: clear KEYONB, set KEYONEX to execute.
 *
 * ⚠️ KEYONEX IS A GLOBAL STROBE. Writing it examines EVERY channel's KEYONB and
 * starts/stops accordingly. Channels already in the state their KEYONB asks for
 * are not retriggered, which is why KOS gets away with doing this per channel —
 * but it does mean a key-off here also commits any KEYONB another writer has
 * left pending, including snd_stream's. That is the same exposure KOS itself
 * has and it is why the delayed key-on below sets KEYONB and the strobe in one
 * write rather than staging them. */
void dc_aica_voice_stop(int v) {
    unsigned int base;

    if (!s_ready || v < 0 || v >= s_nvoices) return;
    base = AICA_CHN(s_v[v].ch);

    {   g2_lock_scoped();
        g2_fifo_wait();
        g2_write_32_raw(base + AICA_R_CTRL, (s_v[v].r_ctrl & ~AICA_KEYONB) | AICA_KEYONEX);
    }
    s_v[v].keyed  = 0;
    s_v[v].due_us = 0;
    s_v[v].end_us = 0;
    s_v[v].dirty  = 0;
    s_keyoffs++;
}

void dc_aica_voice_update(int v, unsigned int freq_hz, unsigned char vol,
                          unsigned char pan) {
    dc_aica_voice* p;
    unsigned int rp, rv, rn;

    if (!s_ready || v < 0 || v >= s_nvoices || !s_v[v].in_use) return;
    p = &s_v[v];

    rp = dc_aica_pitch_bits(freq_hz);
    /* byte 40 = LPF control (0x24 turns the filter off, as aica_play does),
     * byte 41 = TL. Little-endian SH-4, so byte 41 is bits 8-15. */
    rv = 0x24u | ((unsigned int)s_logs[vol] << 8);
    /* byte 36 = DIPAN, byte 37 = DISDL (0xF = full direct send). */
    rn = dc_aica_pan_bits(pan) | (0x0Fu << 8);

    if (rp != p->r_pitch) { p->r_pitch = rp; p->dirty |= DIRTY_PITCH; }
    if (rv != p->r_vol)   { p->r_vol   = rv; p->dirty |= DIRTY_VOL;   }
    if (rn != p->r_pan)   { p->r_pan   = rn; p->dirty |= DIRTY_PAN;   }
}

void dc_aica_voice_start(int v, const dc_aica_sample* s, unsigned int freq_hz,
                         unsigned char vol, unsigned char pan) {
    dc_aica_voice* p;
    unsigned int base, lsa, lea, mode, loops;

    if (!s_ready || v < 0 || v >= s_nvoices || !s_v[v].in_use) return;
    p = &s_v[v];

    loops = (s->flags & DC_AICA_F_LOOPS) ? 1u : 0u;
    mode  = loops ? (unsigned int)(DC_AICA_SM_LOOP) : AICA_SM_ADPCM;

    /* LSA/LEA are in SAMPLES for every format, so no /2 for 4-bit ADPCM. Both
     * are 16-bit registers, which is exactly why FLAG_TOO_LONG exists — a
     * sample longer than 65,534 cannot address its own end. */
    lsa = loops ? s->loop_start : 0u;
    lea = s->n_samples;
    if (lea > 65534u) lea = 65534u;
    if (lsa > lea)    lsa = 0u;

    p->sram   = s->sram;
    p->r_ctrl = (mode << 7) | ((s->sram >> 16) & 0x7Fu) | (loops ? AICA_LOOP : 0u);
    p->r_pitch = dc_aica_pitch_bits(freq_hz);
    p->r_vol   = 0x24u | ((unsigned int)s_logs[vol] << 8);
    p->r_pan   = dc_aica_pan_bits(pan) | (0x0Fu << 8);
    p->dirty   = 0;
    p->end_us  = 0;
    p->life_us = (loops || freq_hz == 0u) ? 0u
               : (unsigned int)(((u64)s->n_samples * 1000000ull) / (u64)freq_hz);

    base = AICA_CHN(p->ch);

    /* THE FULL PROGRAM, IN aica_play()'s ORDER. Order matters at one point
     * only — the control word carrying the key bits is written LAST, so the
     * channel never starts on a half-written address. Everything else is
     * ordinary register setup. */
    {   g2_lock_scoped();
        g2_fifo_wait();
        g2_write_32_raw(base + AICA_R_CTRL,  AICA_KEYONEX);       /* key off  */
        g2_write_32_raw(base + AICA_R_LSA,   lsa & 0xFFFFu);
        g2_write_32_raw(base + AICA_R_LEA,   lea & 0xFFFFu);
        g2_write_32_raw(base + AICA_R_PITCH, p->r_pitch);
        g2_write_32_raw(base + AICA_R_PAN,   p->r_pan);
        g2_write_32_raw(base + AICA_R_VOL,   p->r_vol);
        g2_fifo_wait();
        /* AR = 31, RR = 31: the hardware envelope generator pinned wide open.
         * jaudio's envelopes are arbitrary piecewise curves with HANG/GOTO/
         * RESTART opcodes that a 4-stage AR/D1R/D2R/RR EG cannot represent, so
         * the envelope arrives as TL writes at 229 Hz instead and the EG must
         * not colour them. aica_play() writes exactly these two values. */
        g2_write_32_raw(base + AICA_R_AEG,   0x001Fu);
        g2_write_32_raw(base + AICA_R_EG2,   0x001Fu);
        g2_write_32_raw(base + AICA_R_SA_LO, s->sram & 0xFFFFu);
        g2_write_32_raw(base + AICA_R_CTRL,  p->r_ctrl);          /* armed, off */
    }

#if DC_AICA_KEYON_DELAY
    {
        unsigned int rate = dc_audio_output_rate();
        unsigned int buf  = dc_audio_buffered_frames() + (unsigned int)(DC_AICA_SPU_LATENCY_FRAMES);
        unsigned int us   = rate ? (unsigned int)(((u64)buf * 1000000ull) / (u64)rate) : 0u;

        s_lat_rate = rate; s_lat_buf = buf; s_lat_us = us;

        if (us) {
            p->due_us = (unsigned int)dc_time_us() + us;
            p->keyed  = 0;
            s_start_delayed++;
            return;                       /* dc_aica_service() keys it on */
        }
    }
#endif
    s_start_now++;

    {   g2_lock_scoped();
        g2_fifo_wait();
        g2_write_32_raw(base + AICA_R_CTRL, p->r_ctrl | AICA_KEYONEX | AICA_KEYONB);
    }
    p->r_ctrl |= AICA_KEYONB;
    p->keyed = 1;
    p->due_us = 0;
    p->end_us = p->life_us ? ((unsigned int)dc_time_us() + p->life_us) : 0u;
    s_keyons++;
}

/* Read the play position out of the ARM's channel mirror.
 *
 * 🔴 NOT the CA register at 0x2814: reading that requires selecting the channel
 * through MSLC first, and the ARM driver stomps MSLC 64 times every ~2.3 ms in
 * aica_get_pos(), so any SH-4 read of CA is a race by construction. The mirror
 * at AICA_MEM_CHANNELS (0x020000 + ch*64) is what the ARM refreshes for exactly
 * this purpose, and dc_audio.c's dc_aica_pos() already reads it the same way. */
int dc_aica_voice_playing(int v) {
    dc_aica_voice* p;

    if (!s_ready || v < 0 || v >= s_nvoices || !s_v[v].in_use) return 0;
    p = &s_v[v];

    /* A voice waiting out the latency delay is LIVE — it has not sounded yet,
     * which is exactly why it must not be reaped. This is the bug that silently
     * ate half the one-shots. */
    if (!p->keyed) return p->due_us != 0u;
    if (!p->end_us) return 1;                              /* loops */
    return ((int)((unsigned int)dc_time_us() - p->end_us) < 0) ? 1 : 0;
}

/* ===========================================================================
 * Service — once per audio pump
 * ===========================================================================*/
void dc_aica_service(void) {
    int i;
    unsigned int now;

    if (!s_ready) return;

#if DC_AICA_KEYON_DELAY
    now = (unsigned int)dc_time_us();
    for (i = 0; i < s_nvoices; i++) {
        dc_aica_voice* p = &s_v[i];
        unsigned int base;

        if (!p->in_use || !p->due_us) continue;
        /* Unsigned wrap-safe compare: `now - due` stays small until the
         * deadline passes. dc_time_us() truncated to 32 bits wraps every ~71
         * minutes, which this handles and a plain `now >= due` would not. */
        if ((int)(now - p->due_us) < 0) continue;

        /* How late the pump was relative to the deadline. Must be read BEFORE
         * due_us is cleared — computing it afterwards compares against 0 and
         * reports every key-on as late, which is how this counter read for its
         * first run. A non-trivial count means the pump rate (once per logic
         * tick) is coarser than the skew correction needs, and the delay should
         * move to the ARM queue's timestamp field after all (§11 fix 2). */
        if ((unsigned int)(now - p->due_us) > 20000u) s_pend_late++;

        base = AICA_CHN(p->ch);
        {   g2_lock_scoped();
            g2_fifo_wait();
            g2_write_32_raw(base + AICA_R_CTRL, p->r_ctrl | AICA_KEYONEX | AICA_KEYONB);
        }
        p->r_ctrl |= AICA_KEYONB;
        p->keyed  = 1;
        p->due_us = 0;
        p->end_us = p->life_us ? (now + p->life_us) : 0u;
        s_keyons++;
    }
#else
    (void)now; (void)i;
#endif

    dc_aica_flush();
}

/* ===========================================================================
 * Init
 * ===========================================================================*/
static int dc_aica_open_bank(void) {
    unsigned char hdr[24];
    unsigned char* raw;
    unsigned int i, n, idx_bytes;
    ssize_t got;

    s_fd = fs_open(DC_AICA_BANK_PATH, O_RDONLY);
    if (s_fd < 0) {
        DC_LOGE("[DC/AICA] %s not on the disc — offload disabled, every voice "
                "stays in software. Build it with "
                "`python3 tools/dcaudio/pack.py --out <discroot>/aicabank.pak`\n",
                DC_AICA_BANK_PATH);
        return -1;
    }
    if (fs_read(s_fd, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr)) goto bad;

    if (be32(hdr) != PAK_MAGIC) {
        DC_LOGE("[DC/AICA] %s: bad magic %08X (want 'ACAB')\n",
                DC_AICA_BANK_PATH, (unsigned)be32(hdr));
        goto bad;
    }
    if (be32(hdr + 4) != PAK_VERSION) {
        DC_LOGE("[DC/AICA] %s: version %u, this runtime speaks %u\n",
                DC_AICA_BANK_PATH, (unsigned)be32(hdr + 4), (unsigned)PAK_VERSION);
        goto bad;
    }

    n          = be32(hdr + 8);
    s_data_off = be32(hdr + 16);
    idx_bytes  = n * 28u;

    if (n == 0u || n > 65536u) goto bad;

    /* Read the index as bytes, then byte-swap IN PLACE into the same buffer we
     * keep. 32,396 B for the real bank. It has to be resident: the alternative
     * is a binary search over a CD-R, ~11 seeks x 100-200 ms, per note-on. */
    raw = (unsigned char*)malloc(idx_bytes);
    if (raw == NULL) {
        DC_LOGE("[DC/AICA] cannot allocate %u B for the index\n", (unsigned)idx_bytes);
        goto bad;
    }
    if (fs_seek(s_fd, (off_t)be32(hdr + 12), SEEK_SET) < 0) { free(raw); goto bad; }

    {
        unsigned int done = 0;
        while (done < idx_bytes) {
            got = fs_read(s_fd, raw + done, (size_t)(idx_bytes - done));
            if (got <= 0) { free(raw); goto bad; }
            done += (unsigned int)got;
        }
    }

    s_idx = (unsigned int*)raw;                 /* same storage, swapped below */
    for (i = 0; i < n * PAK_ENT_W; i++)
        s_idx[i] = be32(raw + i * 4u);

    s_sram = (unsigned int*)malloc(n * sizeof(unsigned int));
    if (s_sram == NULL) { free(raw); s_idx = NULL; goto bad; }
    memset(s_sram, 0, n * sizeof(unsigned int));

    s_nent = n;
    dc_mem_note(DCMEM_AUDIO,
                (ptrdiff_t)(idx_bytes + n * sizeof(unsigned int) + sizeof(s_stage)));
    DC_LOGE("[DC/AICA] bank: %u samples, index %u B resident, payloads at +%u\n",
            (unsigned)n, (unsigned)idx_bytes, (unsigned)s_data_off);
    return 0;

bad:
    if (s_fd >= 0) { fs_close(s_fd); s_fd = -1; }
    return -1;
}

int dc_aica_init(void) {
    int i, got = 0;

    if (s_ready) return 0;

    if (dc_aica_open_bank() < 0) return -1;

    /* ⚠️ snd_mem_available() is the LARGEST FREE BLOCK, not the total — the
     * distinction matters here because snd_stream has already taken two 8 KB
     * buffers off the front. Ask for the arena and take what we get. */
    s_arena_size = (unsigned int)(DC_AICA_ARENA);
    s_arena_base = snd_mem_malloc((size_t)s_arena_size);
    if (s_arena_base == 0u) {
        unsigned int avail = (unsigned int)snd_mem_available();
        DC_LOGE("[DC/AICA] %u B of sound RAM refused (largest free block %u) — "
                "retrying at that size\n", (unsigned)s_arena_size, (unsigned)avail);
        if (avail > 65536u) {
            s_arena_size = avail & ~31u;
            s_arena_base = snd_mem_malloc((size_t)s_arena_size);
        }
    }
    if (s_arena_base == 0u) {
        DC_LOGE("[DC/AICA] no sound RAM — offload disabled\n");
        return -1;
    }
    s_arena_used = 0;

    /* snd_sfx_chn_alloc() sets a bit in KOS's sfx_inuse, so find_free_channel()
     * and snd_sfx_stop_all() both skip ours — and, because snd_stream allocated
     * FIRST (AIInit ran before us), channels 0 and 1 are already spoken for. */
    for (i = 0; i < (int)(DC_AICA_VOICES); i++) {
        int ch = snd_sfx_chn_alloc();
        if (ch < 0) break;
        s_v[got].ch     = (unsigned char)ch;
        s_v[got].in_use = 0;
        got++;
    }
    s_nvoices = got;
    if (s_nvoices == 0) {
        DC_LOGE("[DC/AICA] no free AICA channels — offload disabled\n");
        return -1;
    }

    s_ready = 1;
    DC_LOGE("[DC/AICA] ready: %d voices (ch %u..%u), arena %u B at sound-RAM +%u\n",
            s_nvoices, (unsigned)s_v[0].ch, (unsigned)s_v[s_nvoices - 1].ch,
            (unsigned)s_arena_size, (unsigned)s_arena_base);
    return 0;
}

int dc_aica_ready(void) { return s_ready; }

void dc_aica_report(void) {
    if (!s_ready) return;
    DC_LOGE("[DC/AICA] start now=%u delayed=%u (lat rate=%u buf=%u -> %u us)\n",
            s_start_now, s_start_delayed, s_lat_rate, s_lat_buf, s_lat_us);
    DC_LOGE("[DC/AICA] keyon=%u keyoff=%u late=%u | look=%u miss=%u toolong=%u | "
            "loads=%u bytes=%u fail=%u defer=%u | arena %u/%u resets=%u evicted=%u | "
            "flush=%u writes=%u\n",
            s_keyons, s_keyoffs, s_pend_late,
            s_lookups, s_miss, s_toolong,
            s_loads, s_load_bytes, s_load_fail, s_load_defer,
            s_arena_used, s_arena_size, s_resets, s_evicted,
            s_flushes, s_writes);
}

#else  /* !DC_AICA */

int  dc_aica_init(void) { return -1; }
int  dc_aica_ready(void) { return 0; }
void dc_aica_service(void) { }
void dc_aica_report(void) { }
int  dc_aica_lookup(unsigned int d, dc_aica_sample* o) { (void)d; (void)o; return -1; }
int  dc_aica_ensure(unsigned int d, dc_aica_sample* o) { (void)d; (void)o; return -1; }
int  dc_aica_voice_alloc(void) { return -1; }
void dc_aica_voice_free(int v) { (void)v; }
void dc_aica_voice_start(int v, const dc_aica_sample* s, unsigned int f,
                         unsigned char vo, unsigned char p)
{ (void)v; (void)s; (void)f; (void)vo; (void)p; }
void dc_aica_voice_stop(int v) { (void)v; }
void dc_aica_voice_update(int v, unsigned int f, unsigned char vo, unsigned char p)
{ (void)v; (void)f; (void)vo; (void)p; }
int  dc_aica_voice_playing(int v) { (void)v; return 0; }

#endif /* DC_AICA */
