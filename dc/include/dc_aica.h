/* dc_aica.h — stage B: jaudio voices on the AICA's own hardware channels.
 *
 * TWO HALVES, TWO FILES, AND THE SPLIT IS DELIBERATE:
 *
 *   dc/src/dc_aica.c       the HARDWARE half. Sound RAM, the aicabank.pak
 *                          index, residency, and the per-channel register
 *                          driver. Includes no decomp header and knows nothing
 *                          about jaudio.
 *   dc/src/dc_aica_seam.c  the JAUDIO half. Two `--wrap` interpositions, the
 *                          divert policy, and the per-update parameter lift out
 *                          of `AG.channels[]`. This is the only file that
 *                          includes jaudio_NES headers.
 *
 * Everything is behind -DDC_AICA=1. At the default 0 both files compile to
 * nothing and the link carries no --wrap, so a shipping image is byte-identical
 * to one built from a tree without them.
 *
 * The design, the measurements behind it, and the four things that are NOT done
 * are `kb/audio-aica-offload.md`. Read §8 (why the ARM command queue is not the
 * transport) and §11 (why this is incremental, and the latency skew that is the
 * one serious objection) before changing anything here.
 */
#ifndef DC_AICA_H
#define DC_AICA_H

#ifdef __cplusplus
extern "C" {
#endif

/* Hardware channels claimed. Lives in the HEADER, not in dc_aica.c, because
 * dc_aica_seam.c derives DC_AICA_MAX_DIVERT from it — and a per-file default
 * that two files disagree about is a bug that compiles.
 *
 * 64 channels exist; snd_stream holds two; jaudio's own ceiling is
 * NA_SPEC_CONFIG[0]._05, which dc/Makefile sets to 12 by default. 16 therefore
 * covers every voice the sequencer can ask for, with slack for the release
 * tails a hardware voice keeps sounding after the sequencer has moved on. */
#ifndef DC_AICA_VOICES
#define DC_AICA_VOICES 16
#endif

/* ---------------------------------------------------------------------------
 * Lifecycle
 * -------------------------------------------------------------------------*/

/* Claim hardware channels, open /cd/aicabank.pak, reserve the sound-RAM arena.
 *
 * ⚠️ MUST RUN AFTER AIInit()'s snd_stream_init/alloc, and MUST NOT be followed
 * by snd_init()/spu_enable()/spu_disable(): `spu_reset_chans()` and the ARM's
 * `aica_init()` stomp all 64 channel register blocks regardless of who owns
 * them (kb/audio-aica-offload.md §8, "Channel allocation").
 *
 * Returns 0 on success, <0 if the offload is unavailable — in which case every
 * other entry point below is a no-op and jaudio keeps every voice in software.
 * A failure here is never fatal: the software path IS the fallback. */
int  dc_aica_init(void);

/* Non-zero once init succeeded and at least one hardware voice exists. */
int  dc_aica_ready(void);

/* Once per audio pump, from dc_audio_pump(). Pushes every dirty voice register
 * under ONE g2 lock and services delayed key-ons. Cheap when nothing changed. */
void dc_aica_service(void);

/* The periodic console lines. Called from dc_audio.c's 600-pump block.
 *
 * ⚠️ dc_aica_seam_report() prints `wrapped=`, and that counter is LOAD-BEARING:
 * a `--wrap` on a symbol the linker cannot find is ignored with no diagnostic,
 * so `wrapped=0` is the only way to learn that the interposition never
 * happened. Read it before reading anything else. */
void dc_aica_report(void);
void dc_aica_seam_report(void);

/* ---------------------------------------------------------------------------
 * Residency — keyed on `device_addr`, the sample's raw byte offset into
 * audiorom.img. That is the join key between the host packer and the runtime:
 * `Nas_StartDma` computes `device_addr + GetNeosRomTop()` and GetNeosRomTop()
 * carries no file-offset component, so a live `smzwavetable.sample` pointer IS
 * the key. kb/audio-aica-offload.md §2.
 * -------------------------------------------------------------------------*/

/* What the pack knows about one sample. `sram` is valid only after a
 * successful dc_aica_ensure(); it is a byte OFFSET into AICA sound RAM (what
 * snd_mem_malloc returns), not an SH-4 pointer. */
typedef struct {
    unsigned int dev;         /* device_addr — the key                       */
    unsigned int sram;        /* sound-RAM offset, or 0 when not resident    */
    unsigned int n_samples;   /* decoded length, in SAMPLES                  */
    unsigned int loop_start;  /* in samples                                  */
    unsigned int loop_end;    /* in samples                                  */
    unsigned int flags;       /* DC_AICA_F_*                                 */
} dc_aica_sample;

#define DC_AICA_F_LOOPS     0x1u   /* the sample loops                        */
#define DC_AICA_F_TOO_LONG  0x2u   /* > 65,534 samples: NO payload in the pak.
                                    * 24 of 1,157, but 21 % of the bytes.
                                    * These stay in software, for ever, by
                                    * design. kb/audio-aica-offload.md §4. */

/* Look the sample up in the index. Returns 0 and fills `out` on a hit (with
 * `sram` set only if it happens to be resident already), <0 on a miss. Pure
 * binary search over main RAM — no disc, no allocation, safe per note-on. */
int  dc_aica_lookup(unsigned int device_addr, dc_aica_sample* out);

/* Make the sample resident in sound RAM, reading it off the disc if needed.
 * Returns 0 on success (and fills `out->sram`), <0 if it cannot be served —
 * unknown key, FLAG_TOO_LONG, arena exhausted, or a short read.
 *
 * ⚠️ THIS CAN BLOCK ON THE CD-R. Never call it from inside a draw. It is
 * called from the note-on path, which already blocks for jaudio's own ARAM
 * fetch, so it replaces a disc read rather than adding one. */
int  dc_aica_ensure(unsigned int device_addr, dc_aica_sample* out);

/* ---------------------------------------------------------------------------
 * Voices
 * -------------------------------------------------------------------------*/

/* Claim / release one hardware channel. Allocation goes through KOS's
 * `snd_sfx_chn_alloc()` so `find_free_channel()` and `snd_sfx_stop_all()` both
 * skip ours, and so snd_stream's channels 0/1 are never taken. */
int  dc_aica_voice_alloc(void);
void dc_aica_voice_free(int v);

/* Program and key on. `freq_hz` is the sample's playback rate in Hz (see
 * dc_aica_seam.c for the derivation from jaudio's Q1.15 `frequency_fixed_point`
 * — it deliberately reproduces the software path's known ~4.5 % sharpness so
 * an A/B compares like with like). `vol` is LINEAR 0..255 and is put through
 * AICA's logarithmic TL table here; `pan` is 0 left / 128 centre / 255 right.
 *
 * If key-on delay is armed (DC_AICA_KEYON_DELAY, default on) the channel is
 * fully programmed but held keyed OFF, and dc_aica_service() keys it on once
 * the software path's ~100 ms of buffering has drained past the note's
 * timestamp. That is `kb/audio-aica-offload.md` §11's fix (1); without it a
 * hybrid plays its hardware voices ~100 ms ahead of its software ones, which
 * for sequenced music is not a subtlety. */
void dc_aica_voice_start(int v, const dc_aica_sample* s,
                         unsigned int freq_hz, unsigned char vol,
                         unsigned char pan);

/* Key off. Immediate, and never queued: a stuck note is worse than an early
 * one, and the release edge has no skew argument (the software voice it
 * replaces is not producing anything either). */
void dc_aica_voice_stop(int v);

/* Per-update parameter push. Writes SHADOWS only — nothing reaches G2 until
 * dc_aica_service() flushes the batch under one lock. Calling this 24 times
 * per update costs 24 stores. */
void dc_aica_voice_update(int v, unsigned int freq_hz, unsigned char vol,
                          unsigned char pan);

/* Is this voice still sounding? Reads the ARM's channel mirror rather than the
 * CA register, which is unreadable from the SH-4 by construction (the ARM
 * stomps MSLC 64 times every ~2.3 ms). Returns 0 for a one-shot that has run
 * out, so the seam can hand the channel back. */
int  dc_aica_voice_playing(int v);

/* ---------------------------------------------------------------------------
 * The latency the delayed key-on has to match. Implemented in dc_audio.c,
 * which owns the ring; declared here because dc_aica.c is the consumer.
 * Returns the number of OUTPUT SAMPLE PAIRS currently buffered ahead of the
 * DAC: our ring's fill plus whatever snd_stream has already copied into SPU
 * RAM. 0 when audio is not running.
 * -------------------------------------------------------------------------*/
unsigned int dc_audio_buffered_frames(void);

/* The output rate those frames are consumed at, in Hz. */
unsigned int dc_audio_output_rate(void);

/* Non-zero while the audio pump is running NESTED inside somebody else's
 * blocking disc read. dc_aica.c must not issue disc I/O of its own then — see
 * the guard in dc_aica_ensure(). Defined in dc_audio.c. */
extern int dc_audio_in_disc_yield;

#ifdef __cplusplus
}
#endif
#endif /* DC_AICA_H */
