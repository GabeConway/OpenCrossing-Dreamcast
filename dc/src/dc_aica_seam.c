/* dc_aica_seam.c — stage B, the JAUDIO half: which voices go to hardware.
 *
 * =============================================================================
 * THE SEAM, AND WHY IT IS WHERE IT IS
 * =============================================================================
 * `src/` is never edited (CLAUDE.md §1), so the offload needs an interposition.
 * There is exactly one place in the engine where a voice can be removed at ZERO
 * cost, and exactly one pair of cross-TU calls that can reach it.
 *
 * THE ZERO-COST GATE — `AG.common_channel[u * nch + i].enabled`.
 * `Nas_DriveRsp` (driver.c:541) does not loop over channels to synthesise. It
 * first compacts the enabled snapshot rows into `noteIndices[]` (driver.c:
 * 552-575) and then walks THAT list twice, calling `Nas_SynthMain` per entry.
 * A voice that is not in the list costs nothing at all: no ADPCM decode, no
 * sample DMA, no resample, no `aEnvMixer2`, no `Acmd` bytes. Every other
 * "mute" the engine has (sub.muted, group.flags.muted, pitch = 0) still runs
 * `Nas_SynthMain` and still emits the envelope mixer, which is the expensive
 * command — so they are not levers, they are attenuators.
 *
 * THE SNAPSHOT'S ONLY WRITER is `__Nas_PushDrvReg` (driver.c:150), which is
 * `static`, and its source is `AG.channels[i].common_ch.enabled`. So the gate
 * is reached by clearing the SEQUENCER's copy just before each push.
 *
 * `Nas_smzAudioFrame` (driver.c:171) runs two complete loops:
 *
 *     for (i = updates; i > 0; i--) { Nas_MySeqMain(i-1); __Nas_PushDrvReg(...); }
 *     for (i = updates; i > 0; i--) { ...; Nas_DriveRsp(...); }
 *
 * ⭐ ALL FOUR PUSHES HAPPEN BEFORE ANY `Nas_DriveRsp`. And `Nas_MySeqMain`
 * (track.c:2121) is called ONLY from driver.c:180 — a genuine cross-TU edge —
 * so a `--wrap` on it fires immediately before every push, including the fourth.
 * That is the whole trick: one wrap covers all four updates.
 *
 * =============================================================================
 * WHY THE CLEARED FLAG MUST BE PUT BACK, AND WHY THE WINDOW IS PROVABLY SAFE
 * =============================================================================
 * `AG.channels[i].common_ch.enabled` is NOT private to the driver. Six other
 * sites read it, and two of them can free a bank or force-release a note:
 *
 *   sub_sys.c:63    Nap_AudioSysProcess, the AUDIOCMD_UNMUTE arm
 *   sub_sys.c:631   Nap_SilenceCheck_Inner, from AUDIOCMD_FORCE_STOP_ALL_GROUPS
 *   memory.c:478/494/556/571   bank/sample residency ("is anyone using this?")
 *   memory.c:911    Nas_SpecChange case 4, the fadeout step
 *
 * ⭐ EVERY ONE OF THEM IS OUTSIDE `Nas_smzAudioFrame` — they hang off the port
 * command drain (`Nap_AudioPortProcess`, which on this port is also called from
 * dc_audio.c's own `dc_audio_drain_port()`) and off the spec-change/init path,
 * and `Nas_SpecChange` returning non-zero makes `CreateAudioTask` skip the audio
 * frame entirely. Verified 2026-08-13.
 *
 * So the flag is cleared inside `Nas_smzAudioFrame` and restored at its exit,
 * and NOTHING that reads it can observe the cleared value. That is why the
 * second wrap exists: `Nas_smzAudioFrame` is called only from sub_sys.c:743,
 * also a genuine cross-TU edge.
 *
 * 🔴 IF YOU EVER MOVE THE RESTORE, RE-DERIVE THAT TABLE. A permanently-cleared
 * `enabled` tells `memory.c` that nobody is using a bank, and jaudio will free
 * it out from under a voice. The failure mode is instruments playing noise with
 * every counter green — which is precisely how W1 wasted an afternoon
 * (kb/audio-cheap-cpu-wins.md, "What this cost").
 *
 * =============================================================================
 * ⚠️ THE SPELLING OF --wrap ON sh-elf, MEASURED RATHER THAN REASONED
 * =============================================================================
 * sh-elf uses a leading-underscore user-label prefix, so `Nas_MySeqMain` is the
 * symbol `_Nas_MySeqMain` in every object file. The obvious conclusion — pass
 * `--wrap=_Nas_MySeqMain` and name the wrapper `__wrap__Nas_MySeqMain` via an
 * `__asm__` label — IS WRONG, AND IT FAILS SILENTLY. It was tried here first
 * and it built, linked and ran as a complete no-op: the wrappers were left
 * unreferenced and `--gc-sections` quietly deleted them, taking the
 * `__real_*` references with them so not even an undefined symbol was reported.
 *
 * ⭐ bfd HANDLES THE PREFIX ITSELF. `--wrap=X` matches the symbol `_X` and
 * redirects it to `___wrap_X`, which is exactly what an ordinary C function
 * named `__wrap_X` emits. So the correct spelling is the naive one:
 *
 *     link:  -Wl,--wrap=Nas_MySeqMain          (NO leading underscore)
 *     code:  void __wrap_Nas_MySeqMain(...)    (an ordinary C name, no asm label)
 *
 * Verified in the SDK image on a four-way matrix of {asm label, plain name} x
 * {--wrap=foo, --wrap=_foo}: only the pair above links with the reference
 * actually redirected. Do not "fix" this back.
 *
 * ⭐ AND BECAUSE THE FAILURE IS SILENT, `wrapped=` ON THE REPORT LINE IS
 * LOAD-BEARING. If it reads 0 after a run with sound, the interposition did not
 * happen and every other number in this file is meaningless.
 *
 * =============================================================================
 * WHAT IS DELIBERATELY NOT DIVERTED (and each one is a counter)
 * =============================================================================
 *   rej_synth   synthesised waves — `is_synth_wave`, no sample to upload
 *   rej_medium  a wavetable whose `sample` is not an audiorom offset
 *   rej_codec   anything that is not CODEC_ADPCM; the pack encoded VADPCM only
 *   rej_dsp     a voice running the 8-tap FIR, the comb filter, the Haas
 *               delay or a reverb send. AICA can do none of them, and dropping
 *               them silently is an audible change, not a transparent one.
 *   rej_pack    not in aicabank.pak, or FLAG_TOO_LONG (24 samples, 21 % of the
 *               bank's bytes — they stay in software for ever, by design)
 *   rej_novoice all hardware channels busy
 *
 * KILL SWITCHES, bluntest first:
 *   -DDC_AICA=0                 (default) nothing here is compiled, no --wrap
 *   -DDC_AICA_DIVERT=0          wrapped, measured, but every voice stays in
 *                               software. This is the A/B control and it is
 *                               what proves the wrappers themselves cost
 *                               nothing.
 *   -DDC_AICA_MAX_DIVERT=<n>    divert at most n voices at once
 */
#include "dc_platform.h"
#include "dc_aica.h"

#ifndef DC_AICA
#define DC_AICA 0
#endif

#if DC_AICA && !defined(DC_HOST_STUB)

#include "jaudio_NES/audiowork.h"
#include "jaudio_NES/audiocommon.h"
#include "jaudio_NES/driver.h"

/* Divert anything at all? 0 keeps the wrappers and every counter but leaves
 * every voice in software — the honest control for the A/B. */
#ifndef DC_AICA_DIVERT
#define DC_AICA_DIVERT 1
#endif

/* ⭐ THE REVERB GATE, AND MEASUREMENT SAYS IT IS THE WHOLE STORY.
 *
 * A voice with a non-zero reverb send cannot be diverted without silently
 * deleting an effect the composer asked for — AICA's DSP send is undesigned
 * here (kb/audio-aica-offload.md §12, "Reverb has no home"). So it is rejected.
 *
 * MEASURED, first working run, title demo, 8,705 DAC frames:
 *
 *     rej_dsp split: fir=0 comb=0 haas=0 reverb=89178
 *
 * ⭐⭐ FIR, COMB AND HAAS NEVER FIRE AT ALL. Every single DSP rejection in the
 * whole run is the reverb send, and it blocks ~58 % of otherwise-eligible
 * voice-updates. That re-prices §12 from "an open item" to THE gate on stage B:
 * three of the four effects this design cannot do are simply never used, and
 * the fourth is on more than half the voices.
 *
 * DC_AICA_ALLOW_REVERB=1 diverts them anyway, dropping the send. It is an
 * AUDIBLE change and it is not a default — it exists so the ladder can be
 * walked and the cost can be listened to rather than argued about. */
#ifndef DC_AICA_ALLOW_REVERB
#define DC_AICA_ALLOW_REVERB 0
#endif

/* Cap concurrent diverted voices. Lower it to walk up the ladder in §11 one
 * rung at a time with the software path live as the oracle underneath. */
#ifndef DC_AICA_MAX_DIVERT
#define DC_AICA_MAX_DIVERT DC_AICA_VOICES
#endif

/* jaudio's own static channel ceiling (driver.c:543's `u8 noteIndices[64]`). */
#define SEAM_MAX_CH 64

typedef struct {
    signed char  hv;          /* hardware voice, -1 = this channel is software */
    unsigned char cleared;    /* we cleared `enabled` this frame               */
    unsigned char loops;      /* the bound sample loops                        */
    unsigned int dev;         /* device_addr currently bound to hv             */
} seam_ch;

static seam_ch s_ch[SEAM_MAX_CH];
static int s_inited = 0;
static int s_ndiv   = 0;      /* diverted voices live right now */

static unsigned int s_wrapped = 0, s_frames = 0, s_div = 0, s_undiv = 0;
static unsigned int s_rej_synth = 0, s_rej_medium = 0, s_rej_codec = 0;
static unsigned int s_rej_dsp = 0, s_rej_pack = 0, s_rej_novoice = 0;
/* rej_dsp SPLIT FOUR WAYS. The aggregate was the single biggest number in the
 * first working run (78,364 against 32,618 diverted updates), and "AICA cannot
 * do DSP" is not an actionable finding — WHICH of the four is what decides
 * whether the next step is an AICA DSP reverb send (kb §12) or something else. */
static unsigned int s_rej_fir = 0, s_rej_comb = 0, s_rej_haas = 0, s_rej_rev = 0;
/* WHY a start happened, split three ways, plus how a voice ended. Without this
 * split "div == undiv and live == 0" is unreadable: it is equally consistent
 * with healthy note churn and with a voice being torn down and rebuilt on every
 * single update, and those need opposite fixes. */
static unsigned int s_re_nohv = 0, s_re_init = 0, s_re_dev = 0;
static unsigned int s_end_disabled = 0, s_end_inelig = 0, s_end_oneshot = 0;
static unsigned int s_upd = 0, s_live_peak = 0;

/* ---------------------------------------------------------------------------
 * The wrap declarations. Ordinary C names, no asm labels — see the spelling
 * note above, which was established by experiment after the clever version
 * silently did nothing.
 * -------------------------------------------------------------------------*/
extern void  __real_Nas_MySeqMain(u32 frames_left);
extern Acmd* __real_Nas_smzAudioFrame(Acmd* cmds, s32* processed, s16* samples,
                                      s32 n);

/* ---------------------------------------------------------------------------
 * Parameter lift: jaudio's per-update numbers -> AICA's
 * -------------------------------------------------------------------------*/

/* THE PLAYBACK RATE, AND IT DELIBERATELY REPRODUCES A KNOWN ERROR.
 *
 * `commonch.frequency_fixed_point` is UQ1.15 — source samples consumed per
 * output sample at the engine's internal mix rate — written by Nas_smzSetPitch
 * (channel.c:145) as `(s32)(rate * 32768.0f)`. When the requested ratio reaches
 * 2.0 the engine halves it and sets `has_two_parts` (channel.c:130/:137), so
 * the effective ratio is `ffp/32768 * (has_two_parts ? 2 : 1)`.
 *
 * The rate those output samples are produced at is
 * `AG.audio_params.sampling_frequency` (memory.c:969 — and note dc/Makefile
 * sets it to 24000 by default via L2, not the shipped 48000). So:
 *
 *     Hz = ffp * (has_two_parts ? 2 : 1) * sampling_frequency / 32768
 *
 * ⚠️ That carries the port's existing ~4.5 % sharpness (the `33476.156 /
 * JAC_DAC_RATE` term folded in at channel.c:341, see dc_audio.c's L2 comment),
 * AND THAT IS ON PURPOSE: a hardware voice must be in tune with the software
 * voices it is mixing against, not with concert pitch. Correcting it here would
 * make the hybrid sound worse, not better. Fix it in both paths or neither. */
static unsigned int seam_freq_hz(const commonch* c) {
    u64 ffp = (u64)c->frequency_fixed_point;
    if (c->has_two_parts) ffp <<= 1;
    return (unsigned int)((ffp * (u64)AG.audio_params.sampling_frequency) >> 15);
}

/* Volume and pan out of the two UQ12 gains.
 *
 * There is NO pan field in `commonch` — pan was already resolved into separate
 * left/right gains upstream (channel.c:62-63, :101-102), and the envelope was
 * pre-multiplied into them (channel.c:343). AICA wants one level plus a coarse
 * pan, so the level is the larger gain and the pan is their ratio.
 *
 * ⚠️ AICA's pan is 4 bits per side. A gain ratio finer than ~1/16 is lost. That
 * is a real fidelity difference from the software mixer and it is why a voice
 * with the Haas/surround effects set is rejected outright rather than
 * approximated. */
static void seam_vol_pan(const commonch* c, unsigned char* vol,
                         unsigned char* pan) {
    unsigned int l = c->target_volume_left;
    unsigned int r = c->target_volume_right;
    unsigned int m = (l > r) ? l : r;

    if (m == 0u) { *vol = 0u; *pan = 128u; return; }

    /* UQ12 0..4095 -> 0..255. The shift is exact: 4095 >> 4 == 255. */
    *vol = (unsigned char)(m >> 4);

    {
        int d = (int)r - (int)l;                 /* + = right, - = left */
        int p = 128 + (127 * d) / (int)m;
        if (p < 0)   p = 0;
        if (p > 255) p = 255;
        *pan = (unsigned char)p;
    }
}

/* ---------------------------------------------------------------------------
 * Policy
 * -------------------------------------------------------------------------*/

/* Can this voice go to hardware at all? Fills `dev` on success.
 *
 * Every `return 0` bumps a counter, because "the offload did nothing" and "the
 * offload was never offered anything" are different bugs with the same symptom.
 */
static int seam_eligible(const commonch* c, unsigned int* dev) {
    const wtstr* ts;
    const smzwavetable* w;

    if (c->is_synth_wave)            { s_rej_synth++;  return 0; }

    /* AICA has no FIR, no comb filter, no Haas delay and (in this design) no
     * reverb send. Diverting a voice that uses one of them does not degrade it,
     * it deletes an effect the composer asked for. */
    if (c->filter != NULL)                       { s_rej_dsp++; s_rej_fir++;  return 0; }
    if (c->comb_filter_size && c->comb_filter_gain)
                                                 { s_rej_dsp++; s_rej_comb++; return 0; }
    if (c->use_haas_effect)                      { s_rej_dsp++; s_rej_haas++; return 0; }
#if !DC_AICA_ALLOW_REVERB
    if ((c->target_reverb_volume & 0x7Fu) != 0u) { s_rej_dsp++; s_rej_rev++;  return 0; }
#else
    if ((c->target_reverb_volume & 0x7Fu) != 0u) { s_rej_rev++; }   /* counted, diverted anyway */
#endif

    ts = c->tuned_sample;
    if (ts == NULL) { s_rej_synth++; return 0; }
    w = ts->wavetable;
    if (w == NULL)  { s_rej_synth++; return 0; }

    /* ⚠️ THE JOIN KEY IS ONLY VALID AFTER RELOCATION. `__WaveTouch`
     * (system.c:2079-2093) adds the wave bank's base to `sample` and overwrites
     * `medium` with the real one; before that the pointer is bank-relative and
     * would look up as garbage — which would SUCCEED sometimes, because the
     * index is dense. Check the flag, not the value. */
    if (!w->is_relocated) { s_rej_medium++; return 0; }

    /* MEDIUM_RAM / MEDIUM_DISK after relocation mean `sample` is a real host
     * pointer, not an audiorom offset. Only the two pass-through media leave it
     * as the `device_addr` that `Nas_StartDma` (system.c:1299) adds
     * `GetNeosRomTop()` to — and that is exactly the key `pack.py` sorts on
     * (kb/audio-aica-offload.md §2). */
    if (w->medium != MEDIUM_CART && w->medium != MEDIUM_DISK_DRIVE) {
        s_rej_medium++; return 0;
    }
    if (w->codec != CODEC_ADPCM) { s_rej_codec++; return 0; }

    *dev = (unsigned int)(uintptr_t)w->sample;
    return 1;
}

static void seam_release(int i) {
    if (s_ch[i].hv >= 0) {
        dc_aica_voice_free(s_ch[i].hv);
        s_ch[i].hv = -1;
        s_ndiv--;
        s_undiv++;
    }
    s_ch[i].dev    = 0u;
    s_ch[i].loops  = 0;
}

/* ---------------------------------------------------------------------------
 * The wrappers
 * -------------------------------------------------------------------------*/

/* Runs immediately before every `__Nas_PushDrvReg`, i.e. four times per DAC
 * frame, once per update. Decides, programs, and clears. */
void __wrap_Nas_MySeqMain(u32 frames_left) {
    int i, nch;

    /* 🔴 RESTORE BEFORE THE SEQUENCER RUNS, NOT ONLY AT FRAME END.
     *
     * This wrapper fires FOUR times per DAC frame, once per update, and each
     * time it clears `enabled` for the diverted channels. The frame-end restore
     * alone is therefore not enough: updates 2, 3 and 4 would see the flag
     * update 1 cleared and read it as "the sequencer stopped this note", tear
     * the hardware voice down, and rebuild it on the next update.
     *
     * MEASURED, first run, and this is what the counters looked like:
     *   start nohv=39722 init=95 devchg=0 | end disabled=39688 ... livepeak=13
     * i.e. 95 real note-ons and 39,722 rebuilds — a key-on per voice per update,
     * ~40,000 of them, every one a full 10-register reprogram over G2. The
     * offload "worked" the whole time; it just re-triggered every note 229
     * times a second, which is a buzz, not an instrument.
     *
     * `enabled` must be TRUE while `__real_Nas_MySeqMain` runs anyway — it is
     * the sequencer's own state, and Nas_UpdateChannel reads it. So the restore
     * belongs here, at the top, and the frame-end wrapper covers the one place
     * this cannot reach: after the fourth and final push. */
    for (i = 0; i < SEAM_MAX_CH; i++) {
        if (!s_ch[i].cleared) continue;
        AG.channels[i].common_ch.enabled = TRUE;
        s_ch[i].cleared = 0;
    }

    __real_Nas_MySeqMain(frames_left);
    s_wrapped++;

    if (!dc_aica_ready()) return;

    if (!s_inited) {
        for (i = 0; i < SEAM_MAX_CH; i++) { s_ch[i].hv = -1; s_ch[i].dev = 0u; }
        s_inited = 1;
    }

    nch = (int)AG.num_channels;
    if (nch > SEAM_MAX_CH) nch = SEAM_MAX_CH;

    for (i = 0; i < nch; i++) {
        channel*  ch = &AG.channels[i];
        commonch* c  = &ch->common_ch;
        unsigned int dev = 0u;
        unsigned char vol, pan;
        unsigned int hz;

        if (!c->enabled) {
            if (s_ch[i].hv >= 0) { s_end_disabled++; seam_release(i); }
            continue;
        }

#if !DC_AICA_DIVERT
        continue;                     /* measured, never diverted: the control */
#else
        if (!seam_eligible(c, &dev)) {
            if (s_ch[i].hv >= 0) { s_end_inelig++; seam_release(i); }
            continue;
        }

        hz = seam_freq_hz(c);
        seam_vol_pan(c, &vol, &pan);

        /* THE NOTE-START EDGE is `needs_init` (audiostruct.h:244), set by
         * Nas_StartVoice via NA_SVCINIT_TABLE[0] (channel.c:157,
         * audiotable.c:295). It is normally cleared by __Nas_PushDrvReg's
         * enabled branch (driver.c:162) — which we are about to skip, so WE
         * must clear it, or every update would look like a fresh note-on and
         * re-key the channel 229 times a second.
         *
         * A changed `dev` on a channel that was already diverted is also a
         * note-on: the sequencer reused the channel for a different
         * instrument without our seeing an init edge. */
        if (s_ch[i].hv < 0 || c->needs_init || dev != s_ch[i].dev) {
            dc_aica_sample smp;

            if (s_ch[i].hv < 0)              s_re_nohv++;
            else if (c->needs_init)          s_re_init++;
            else                             s_re_dev++;

            if (s_ch[i].hv >= 0) seam_release(i);

            if (s_ndiv >= (int)(DC_AICA_MAX_DIVERT)) { s_rej_novoice++; continue; }

            /* ⚠️ THIS CAN BLOCK ON THE CD-R, and it does so exactly where
             * jaudio would have blocked anyway — a note-on whose sample is not
             * cached already costs an ARAM fetch. It replaces a disc read, it
             * does not add one. dc_aica_ensure() yields the audio pump inside
             * the stall. */
            if (dc_aica_ensure(dev, &smp) < 0) { s_rej_pack++; continue; }

            {
                int hv = dc_aica_voice_alloc();
                if (hv < 0) { s_rej_novoice++; continue; }

                dc_aica_voice_start(hv, &smp, hz, vol, pan);
                s_ch[i].hv    = (signed char)hv;
                s_ch[i].dev   = dev;
                s_ch[i].loops = (smp.flags & DC_AICA_F_LOOPS) ? 1u : 0u;

                /* ONE-SHOT LIFETIME, and this is a genuine behavioural gap
                 * rather than an optimisation. `commonch.finished` is the only
                 * feedback the driver gives the sequencer, and it is set ONLY
                 * inside Nas_SynthMain (driver.c:1100) — which a diverted voice
                 * never enters. So a diverted one-shot would never report
                 * running out, and would be reaped only when its envelope
                 * reached ADSR_STATUS_DISABLED. Deadline arithmetic here
                 * restores the edge; it is an estimate, but it is an estimate
                 * of a quantity (sample length / rate) that is exactly known.
                 * ⚠️ The deadline lives in dc_aica.c and starts at KEY-ON, not
                 * here at setup — see dc_aica_voice::life_us for why that
                 * distinction silently ate half the notes. */
                s_ndiv++;
                s_div++;
                if ((unsigned int)s_ndiv > s_live_peak) s_live_peak = (unsigned int)s_ndiv;
            }
        } else {
            /* The steady state: shadows only, no G2 traffic until the pump's
             * dc_aica_service() flushes the whole batch under one lock. */
            dc_aica_voice_update(s_ch[i].hv, hz, vol, pan);
            s_upd++;

            if (!dc_aica_voice_playing(s_ch[i].hv)) {
                c->finished = TRUE;
                s_end_oneshot++;
                seam_release(i);
                /* Fall through and still clear `enabled` for this update — the
                 * software path must not suddenly restart a note that has
                 * already played out on hardware. */
            }
        }

        /* THE GATE. Cleared here, restored in __wrap_Nas_smzAudioFrame(). */
        c->enabled     = FALSE;
        c->needs_init  = FALSE;
        s_ch[i].cleared = 1;
#endif
    }
}

/* Runs once per DAC frame. Its only job is to undo the clearing, and the
 * argument for why that is sufficient is at the top of this file. */
Acmd* __wrap_Nas_smzAudioFrame(Acmd* cmds, s32* processed, s16* samples,
                               s32 n) {
    Acmd* r = __real_Nas_smzAudioFrame(cmds, processed, samples, n);
    int i;

    s_frames++;
    for (i = 0; i < SEAM_MAX_CH; i++) {
        if (!s_ch[i].cleared) continue;
        AG.channels[i].common_ch.enabled = TRUE;
        s_ch[i].cleared = 0;
    }
    return r;
}

void dc_aica_seam_report(void) {
    DC_LOGE("[DC/AICA] seam wrapped=%u frames=%u live=%d div=%u undiv=%u | "
            "rej synth=%u medium=%u codec=%u dsp=%u pack=%u novoice=%u\n",
            s_wrapped, s_frames, s_ndiv, s_div, s_undiv,
            s_rej_synth, s_rej_medium, s_rej_codec,
            s_rej_dsp, s_rej_pack, s_rej_novoice);
    DC_LOGE("[DC/AICA] seam start nohv=%u init=%u devchg=%u | end disabled=%u "
            "inelig=%u oneshot=%u | updates=%u livepeak=%u\n",
            s_re_nohv, s_re_init, s_re_dev,
            s_end_disabled, s_end_inelig, s_end_oneshot, s_upd, s_live_peak);
    DC_LOGE("[DC/AICA] seam rej_dsp split: fir=%u comb=%u haas=%u reverb=%u\n",
            s_rej_fir, s_rej_comb, s_rej_haas, s_rej_rev);
    if (s_wrapped == 0u)
        DC_LOGE("[DC/AICA] *** wrapped=0: --wrap=_Nas_MySeqMain DID NOT FIRE. "
                "The flag must be spelled WITHOUT a leading "
                "underscore and the wrapper as a plain C __wrap_ name — see the "
                "spelling note at the top of dc_aica_seam.c. Every other AICA "
                "number in this log is meaningless. ***\n");
}

#else

void dc_aica_seam_report(void) { }

#endif /* DC_AICA */
