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
 * ==========================================================================
 * ...BUT THE 45 % IS A TOWN NUMBER, AND ONE SWITCH MADE EVERY SCENE PAY IT
 * ==========================================================================
 * MEASURED 2026-08-04, same build, two scenes:
 *
 *   scene                          verts/frame   FPS            gx=
 *   K.K. / player-select (mode 3)      888       29.9 (capped)  4.1 ms
 *   town (mode 9)                    ~6,951      14.9, never up  ~13 ms
 *
 * The K.K. scene sits AT THE FRAME CAP with cycles to spare; the town has
 * none. A single compile-time switch forced the cheap scene to buy the
 * expensive scene's price, so it was set to 0 and the game went silent
 * everywhere — including the one place a human noticed:
 *
 *   ac_npc_p_sel_schedule.c_inc:71 opens the player-select scene with
 *   `p_sel->strum_timer = 440`. At 60 logic Hz that is **7.3 seconds** of K.K.
 *   strumming a guitar, with `[TRG_VOL] slot=0 id=0x044D vol=1.000` firing
 *   every tick, and we render silence. It was reported as "silent loading,
 *   that pause is very odd". It is not loading. It is a song we do not play.
 *
 * DC_AUDIO_SCENES makes the switch per-scene. See the block above
 * dc_audio_gate_update() for why a SCENE gate is a fundamentally different
 * object from the per-frame time budget that already failed. */
#ifndef DC_AUDIO
#define DC_AUDIO 0
#endif

/* ==========================================================================
 * DC_AUDIO_BUDGET_US NOW DEFAULTS TO 0 (OFF), AND THAT IS A CORRECTION
 * ==========================================================================
 * Re-read the three matched 600 s runs in kb/RESUME.md §3:
 *
 *   audio off                            FPS p50 23.5   deepest scene 18
 *   audio on, no budget                  FPS p50 13.0   deepest scene 18
 *   audio on + the predictive budget     FPS p50 10.9   deepest scene 4
 *
 * The 13.0 row is the FREE-RUNNING pump — headroom-stopped only. The 10.9 row
 * is this budget. It shipped as the default anyway, so `-DDC_AUDIO=1` has been
 * building the measured-WORSE of the two configurations. Defaulting it to 0
 * restores the 13.0 configuration; >0 re-arms the predictive loop as an A/B
 * knob and nothing else.
 *
 * WHY IT LOSES, since it is not obvious: the loop must override the budget
 * when the ring is starving (a late sample beats a gap), and at ~19.8 ms per
 * DAC frame the ring starves essentially always, so the override IS the normal
 * path — the budget adds a timer read and an EWMA to a decision it never gets
 * to make, and holds the ring pinned at DC_AUDIO_MIN_FILL where every hiccup
 * is an underrun. It is a control loop whose input (ring fill) is a function of
 * its own output. That feedback is the failure; see dc_audio_gate_update(). */
#ifndef DC_AUDIO_BUDGET_US
#define DC_AUDIO_BUDGET_US 0
#endif
/* THE HARD CAP THAT REPLACES THE BUDGET: at most this many jaudio DAC frames
 * per pump. Unlike the budget it takes no measurement, has no override, and
 * therefore cannot enter a feedback loop — it is a straight integer bound on
 * the worst-case pump. One frame is ~19.8 ms of SH-4 for ~35 ms of audio, so 2
 * bounds a pump at ~40 ms; it only ever binds on the FIRST pump after the gate
 * arms, when the ring is empty and the headroom stop would otherwise ask for
 * three frames (~59 ms) in one call and drop a visible frame on scene entry. */
#ifndef DC_AUDIO_MAX_FRAMES
#define DC_AUDIO_MAX_FRAMES 2
#endif
/* L1/L2 — the two jaudio spec overrides. 0 means "leave the shipped value
 * alone", so an unset build is byte-identical in behaviour. The whole argument
 * for both, and why NEITHER is JAC_DAC_RATE, is at the write site in AIInit. */
#ifndef DC_AUDIO_VOICES
#define DC_AUDIO_VOICES 0       /* 0 = shipped 24 (NA_SPEC_CONFIG[0]._05)   */
#endif
#ifndef DC_AUDIO_MIXRATE
#define DC_AUDIO_MIXRATE 0      /* 0 = shipped 48000 (NA_SPEC_CONFIG[0]._00) */
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

/* ==========================================================================
 * DC_AUDIO_SCENES — WHICH SCENES PAY FOR SOUND
 * ==========================================================================
 * A comma/colon/space separated list of `sou_scene_mode` values, e.g.
 *
 *   DC_AUDIO_SCENES=3        the K.K. / player-select scene only
 *   DC_AUDIO_SCENES=0,3,18   + title and name entry
 *   DC_AUDIO_SCENES=all      every scene (what -DDC_AUDIO=1 has always meant)
 *   DC_AUDIO_SCENES=none     nowhere; the output device is never even opened
 *
 * The default is "all", so a plain -DDC_AUDIO=1 build is unchanged, and DC_AUDIO
 * still defaults to 0, so a default build is unchanged. Both halves of today's
 * behaviour survive untouched; you have to ask for the new one.
 *
 * Parsed ONCE, in AIInit, into a 32-bit mask. Runtime cost of the gate is a
 * shift and a test against a byte the game already maintains. Modes run 0..18
 * (the largest mBGMPsComp_scene_mode() argument in all of src/ is 18), so 32
 * bits is the whole space with room to spare; anything >= 32 in the list is
 * rejected loudly rather than silently wrapping into someone else's bit.
 *
 * KILL SWITCHES, in order of bluntness:
 *   -DDC_AUDIO=0                     the whole subsystem compiles out (default)
 *   -DDC_AUDIO_SCENES=none           linked, but no device and no synthesis
 *   -DDC_AUDIO_SCENES=all            the pre-gate behaviour, verbatim
 *   -DDC_AUDIO_SCENE_SETTLE=0        no debounce on the gate
 */
#ifndef DC_AUDIO_SCENES
#define DC_AUDIO_SCENES "all"
#endif

/* Consecutive pumps a new answer must survive before the gate acts on it.
 *
 * sou_scene_mode is written only by Na_SceneMode (game64.c_inc:509) at scene
 * construction, so it is already constant for the life of a scene and this
 * should never fire. It is two pumps of insurance against a transition that
 * writes the variable more than once on its way to the value it settles on —
 * which m_field_make.c:1292-1362 makes plausible, since the mode is chosen by a
 * chain of ifs and several arms call mBGMPsComp_scene_mode() with different
 * values. Two pumps is ~70 ms in the town, ~33 ms in K.K.: below noticing,
 * above one frame of transient. 0 disables it. */
#ifndef DC_AUDIO_SCENE_SETTLE
#define DC_AUDIO_SCENE_SETTLE 2u
#endif

/* Output-gain ramp, in Q8 steps per stereo frame, applied in the stream
 * callback when the gate flips. 1 step per frame is 256 frames = ~8 ms at
 * 32 kHz / ~11.6 ms at 22.05 kHz — enough to turn a waveform step into a ramp,
 * short enough that no note is audibly clipped. See the ramp comment in
 * dc_audio_stream_cb. */
#ifndef DC_AUDIO_FADE_STEP
#define DC_AUDIO_FADE_STEP 1
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

/* THE PER-TICK AUDIO COST, published for the [STUTTER] line in dc_vi.c.
 *
 * MEASURED: 210 [STUTTER] events in 420 s with DC_AUDIO=1 against 14 in 600 s
 * silent — 21x more per second, and the game visibly hitches ~2x/second on real
 * hardware. But [STUTTER] accounts for swap, gx and tex and nothing else, so on
 * a 67.3 ms frame it left ~24 ms unnamed and the culprit had to be GUESSED.
 * Two guesses were already wrong: disc-cache misses (4x the cache changed
 * nothing on hardware) and a multi-frame synthesis burst (DC_AUDIO_MAX_FRAMES
 * is 2, which caps it near 9.6 ms). These two counters end the guessing — they
 * put audio's real per-frame cost ON the same line as the number it has to
 * explain, so `snd=` either accounts for the gap or exonerates this file.
 *
 * dc_vi.c OWNS THE RESET and this file never clears them, so the accumulation
 * window is exactly the frame window: dc_audio_pump() runs once per LOGIC tick
 * (dc_vi.c:300, before the frameskip early-out), so a presented frame's `snd=`
 * sums every tick in the batch its `total=` measures — including the skipped
 * ones. Clearing here instead would report only the last tick.
 *
 * Unconditional definitions: at DC_AUDIO=0 or under DC_HOST_STUB nothing ever
 * writes them and they read 0, which is the honest answer for a build with no
 * audio, and dc_vi.c needs no #if around the read. */
u32 dc_audio_tick_usec = 0;      /* us inside dc_audio_pump() since the reset */
u32 dc_audio_tick_frames = 0;    /* DAC frames synthesised since the reset    */
u32 dc_audio_tick_synth_max_us = 0; /* worst SINGLE pc_audio_process_frame()  */

/* The voice census (-DDC_AUDIO_VOICELOG). Same unconditional-definition rule as
 * the three above, so dc_vi.c reads them with no #if and a build without the
 * flag prints zeros — which is the honest answer, not a silent omission.
 *
 * `_at_max` is the field the hypothesis actually turns on: it is the voice count
 * of the SAME synthesis call that set dc_audio_tick_synth_max_us, so the
 * [STUTTER] line carries the cost and its cause on one row. `_voice_max` alone
 * would be a second average-shaped number and would repeat measurement rule 7.
 */
u32 dc_audio_tick_voice_max   = 0; /* max voice-updates in any ONE DAC frame  */
u32 dc_audio_tick_voice_at_max = 0; /* voice-updates of the WORST-COST frame  */
u32 dc_audio_tick_filt_at_max = 0; /* of those, how many ran the 8-tap FIR    */
u32 dc_audio_tick_comb_at_max = 0; /* of those, how many ran the comb filter  */

#ifdef DC_AUDIO_VOICELOG
/* cost bucketed by voice count — the part that can refute the hypothesis on its
 * own, printed in the 600-pump block and never reset, so it is a whole-run
 * distribution rather than a window average. */
static u32 s_vhist_n[8]   = {0};
static u32 s_vhist_us[8]  = {0};
static u32 s_vhist_max[8] = {0};
#endif

/* THE OUTPUT GAIN IS OWNED BY THE CALLBACK, THE TARGET BY THE PUMP, and that
 * split is the whole thread-safety argument. The pump writes s_gain_tgt and
 * nothing else; the callback writes s_gain_q8 and ring_read_pos and nothing
 * else. Single-producer/single-consumer on every word, so the ramp is correct
 * whether or not KOS ever runs the callback off our thread (today it does not —
 * snd_stream_poll() calls it synchronously from dc_audio_pump).
 *
 * BOTH START AT ZERO, i.e. muted, and only the scene gate ever raises the
 * target. That makes the fail-safe direction silence: if the gate never arms —
 * DC_AUDIO_SCENES=none, or a scene that is not in the list, or a gate that
 * somehow never runs — the callback emits zeros rather than whatever happens to
 * be in the ring. */
static volatile s16 s_gain_q8  = 0;     /* current gain, Q8: 256 = unity */
static volatile s16 s_gain_tgt = 0;     /* what the callback ramps toward */

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

/* ==========================================================================
 * THE SCENE GATE — and why it is not the per-frame budget wearing a hat
 * ==========================================================================
 * The budget (DC_AUDIO_BUDGET_US, above) failed for a structural reason, not a
 * tuning one, and any replacement has to differ structurally or it will fail
 * the same way. The budget was a CONTROL LOOP WHOSE INPUT IT CONTROLLED: it
 * decided from ring fill, its decision changed ring fill, and its own
 * anti-gap override (synthesise anyway when fill < MIN_FILL) turned the loop
 * positive. Below a certain machine speed there is no stable point, so it
 * re-decided every pump and settled on "synthesise", which is why the budgeted
 * run scored WORSE than the unbudgeted one (10.9 vs 13.0 FPS) and reached a
 * shallower scene (4 vs 18).
 *
 * This gate's input is EXOGENOUS. `sou_scene_mode` (game64.c_inc:504) is the
 * game's own scene state, written only by Na_SceneMode (:509) at scene
 * construction. Nothing this file does can change it. So:
 *
 *   - the answer is constant for the entire life of a scene,
 *   - it cannot oscillate, because nothing in the audio path feeds back into it,
 *   - and there is no override, anywhere: a disarmed scene stays disarmed even
 *     if the ring is starving, because a starving ring in a disarmed scene is
 *     not a fault, it is the point.
 *
 * That is the whole difference. "Skip work when we are running out of time" is
 * the shape that failed; "in THIS scene we have measured that we can afford it,
 * and in that one we have measured that we cannot" is a decision made once,
 * from evidence that exists before the frame starts.
 *
 * ⚠️ sou_scene_mode is a zero-initialised u8 in .bss, so mode 0 is
 * indistinguishable from "Na_SceneMode has never been called". Listing 0 in
 * DC_AUDIO_SCENES therefore arms audio from the very first pump, before the
 * game has chosen a scene at all. That is harmless — pc_audio_process_frame()
 * returns immediately until pc_audio_initialized (audiothread.c:93) — but it is
 * the reason 0 is not in any recommended list.
 *
 * dc/ reads the symbol directly: `8c900888 B _sou_scene_mode` in the linked
 * ELF, an ordinary non-static global, so this needs no src/ edit (CLAUDE.md §1)
 * and no interposition. dc_pad.c's DC_AUTOWALK_SCENE gate reads it the same way.
 * ========================================================================== */
extern u8 sou_scene_mode;

#define DC_AUDIO_SCENE_ALL 0xFFFFFFFFu

/* DC_AUDIO_SCENES IS A STRING, AND THIS MAKES THAT A BUILD ERROR RATHER THAN A
 * WILD POINTER. `DC_XDEFS='-DDC_AUDIO_SCENES=3'` — the quoting anyone would
 * type first — expands to the integer 3, which C is happy to pass to a
 * `const char*` parameter with nothing worse than a warning, and then the
 * parser dereferences address 3 during AIInit. Initialising a char array from
 * it is an ERROR for every non-string form, so the mistake cannot reach a
 * console. dc/Makefile's DC_AUDIO_SCENES knob adds the quotes for you; through
 * DC_XDEFS you must write -DDC_AUDIO_SCENES='"3"' yourself. */
static const char s_scene_list[] = DC_AUDIO_SCENES;

static u32 s_scene_mask = 0;          /* parsed from DC_AUDIO_SCENES in AIInit */
static u8  s_armed = 0;               /* is synthesis running right now?       */
static u8  s_settle_want = 0;         /* the answer being debounced            */
static u32 s_settle_n = 0;
static u32 s_gate_arms = 0, s_gate_disarms = 0;

/* Parse once, at AIInit. Separators are anything that is not a digit, so
 * "0,3,18", "0:3:18" and "0 3 18" all work; the ':' form matches DC_STUB_KEEP.
 * Out-of-range entries are reported rather than wrapped — 1u<<33 is UB on SH-4
 * and would otherwise arm some unrelated scene. */
static u32 dc_audio_parse_scenes(const char* s) {
    u32 mask = 0;

    if (s == NULL) return 0;
    while (*s == ' ' || *s == '\t') s++;
    if (*s == '\0') return 0;                       /* "" == none */
    if (*s == '*') return DC_AUDIO_SCENE_ALL;
    if (*s == 'a' || *s == 'A') return DC_AUDIO_SCENE_ALL;   /* "all" */
    if (*s == 'n' || *s == 'N') return 0;                    /* "none" */

    while (*s) {
        if (*s >= '0' && *s <= '9') {
            u32 v = 0;
            while (*s >= '0' && *s <= '9') { v = v * 10u + (u32)(*s - '0'); s++; }
            if (v < 32u) {
                mask |= 1u << v;
            } else {
                DC_LOGE("[DC/AUDIO] DC_AUDIO_SCENES: scene %u out of range "
                        "(0..31) — ignored\n", (unsigned)v);
            }
        } else {
            s++;                                    /* separator */
        }
    }
    return mask;
}

static int dc_audio_scene_wants(void) {
    u32 m = (u32)sou_scene_mode;
    if (m >= 32u) return 0;
    return (int)((s_scene_mask >> m) & 1u);
}

/* Runs once per pump, before any synthesis. Returns the CURRENT arm state.
 *
 * The debounce (DC_AUDIO_SCENE_SETTLE) is the only per-pump state here, and it
 * is deliberately not a hysteresis on cost or fill — it is a hysteresis on the
 * scene NUMBER, so it cannot become the feedback loop described above. */
static int dc_audio_gate_update(void) {
    int want = dc_audio_scene_wants();

    if ((u8)want == s_armed) {          /* nothing to decide */
        s_settle_n = 0;
        return (int)s_armed;
    }
    if ((u8)want != s_settle_want) {    /* a new answer: restart the debounce */
        s_settle_want = (u8)want;
        s_settle_n = 0;
    }
    if (++s_settle_n < (u32)(DC_AUDIO_SCENE_SETTLE))
        return (int)s_armed;

    s_armed = (u8)want;
    s_settle_n = 0;

    if (s_armed) {
        /* DROP THE RESIDUAL BEFORE THE FIRST SYNTHESIS. The ring has been
         * draining at gain 0 for the whole silent scene, so rp == wp already in
         * every realistic case and this is belt-and-braces for a disarm/arm pair
         * shorter than one drain. It is the one place the pump touches
         * ring_read_pos; the worst case if KOS ever moves the callback onto its
         * own thread is that one callback's worth of ALREADY-MUTED samples is
         * skipped, which is inaudible by construction.
         *
         * It must happen here and not via a flag consumed by the callback: the
         * callback next runs at the snd_stream_poll() at the BOTTOM of this
         * pump, i.e. after the synthesis below has already pushed the new
         * samples, and a flush there would throw those away instead. */
        ring_read_pos = ring_write_pos;
        s_gain_tgt = 256;               /* fade in; see the ramp in the callback */
        s_gate_arms++;
        DC_LOGE("[DC/AUDIO] scene %u: ARMED (synthesis on, ~19.8 ms/DAC frame)\n",
                (unsigned)sou_scene_mode);
    } else {
        /* No stop, no teardown — only the target moves. The stream stays up and
         * the pump keeps polling; see the comment at the top of dc_audio_pump. */
        s_gain_tgt = 0;
        s_gate_disarms++;
        DC_LOGE("[DC/AUDIO] scene %u: DISARMED (synthesis off, stream stays up)\n",
                (unsigned)sou_scene_mode);
    }
    return (int)s_armed;
}
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
 * -DDC_AUDIO_VOICELOG — the voice census (L0)
 * ==========================================================================
 * WHAT THIS IS FOR, and it is one hypothesis and one run.
 *
 * jaudio's per-DAC-frame cost is BIMODAL — ~2.5 ms cheap, ~10 ms expensive,
 * measured 2026-08-06 by `4 * smax == snd` on every [STUTTER] line — and four
 * explanations are already dead (disc-cache misses, a multi-frame burst,
 * snd_stream_poll/G2/the scheduler quantum, and jaudio's MEAN cost, which -O3
 * moved 1.4 %). The surviving suspect is VOICE COUNT, and the arithmetic is
 * structural rather than fitted: the engine synthesises
 * `updates_per_frame(4) * num_samples_per_update(200) = 800` internal samples
 * PER ENABLED VOICE (memory.c:971,:974; driver.c:187-199), so at the
 * NA_SPEC_CONFIG[0]._05 = 24 channel maximum (audioconst.c:10) one DAC frame is
 * 19,200 voice-samples. Against the measured ~10 ms peak that is ~104 cycles
 * per voice-sample at 200 MHz — the middle of kb/audio-cpu-cost.md's 80-180
 * band, arrived at from the other direction. The 2.5 ms mode is then ~6 voices.
 * Nothing else in the frame has a 4x dynamic range.
 *
 * ⚠️ THIS COUNTER IS ALLOWED TO KILL THE HYPOTHESIS. If `voices=` is FLAT
 * across stuttering and non-stuttering frames, voice count is refuted in one
 * run and the search moves to per-voice conditional work — which is why
 * `filt=`/`comb=` ride along: they are the two conditional stages (driver.c:
 * 1183-1188 FIR, +27 % on an affected voice; :1190-1209 comb, +10 %) whose
 * INCIDENCE has never been measured, and counting them is free here.
 *
 * WHERE IT SAMPLES, and why after rather than before: AG.common_channel[] is
 * written by __Nas_PushDrvReg (driver.c:150-169) at the top of the frame and
 * read by Nas_DriveRsp (driver.c:552-575) in the same frame; nothing clears it
 * afterwards. One pc_audio_process_frame() is exactly one Neos_Update() is
 * exactly one Nas_smzAudioFrame (audiothread.c:92 -> aictrl.c:285 -> :237 ->
 * cpubuf.c:103 -> neosthread.c:30), so reading immediately after the call
 * returns the literal `noteCount` that call paid for, summed over all four
 * updates. Sampling BEFORE would report the PREVIOUS frame's voices next to
 * THIS frame's cost, which is the pairing the whole measurement exists to make.
 *
 * ⚠️ This is the second place in dc/ that includes a decomp header, and the
 * justification is dc_vi.c:49-68's verbatim: AG is a 34 KB struct read by field
 * name, a hand-copied offset map would silently read the wrong words the moment
 * audiostruct.h moves, and the whole block is behind a flag no shipping build
 * sets — so a header collision can only break the build of someone who asked
 * for the measurement. Reachable through DC_XDEFS, which needs no Makefile
 * change and therefore cannot repeat the DC_EMU64_HIST forwarding trap
 * (kb/traps.md). */
#ifdef DC_AUDIO_VOICELOG
#include "jaudio_NES/audiowork.h"
#endif

/* NA_SPEC_CONFIG[] for the L1/L2 overrides in AIInit. Unlike audiowork.h above
 * this one is NOT behind a probe flag, because the overrides are a shipping
 * feature rather than an instrument — but it is still inside the DC_AUDIO guard,
 * so a DC_AUDIO=0 build pulls in no decomp audio header at all. */
#if DC_AUDIO && !defined(DC_HOST_STUB)
#include "jaudio_NES/audioconst.h"
#endif

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

    /* ==================================================================
     * THE GAIN RAMP — the last point before the SPU, which is why it is here
     * ==================================================================
     * The scene gate flips synthesis on and off mid-run, and a hard cut on
     * either edge is a step in the waveform, i.e. a click, on every scene
     * change. Fading the RING instead would be too late: snd_stream has
     * already copied up to DC_AUDIO_STREAM_BYTES of it into SPU RAM, and
     * those samples are out of reach. This callback is the last place the
     * SH-4 owns the bytes, so the ramp lives here and covers everything not
     * already committed to the AICA.
     *
     * Three paths, cheapest first, because this runs per sample:
     *   unity    — the steady armed case: the original copy, untouched.
     *   muted    — the steady disarmed case: no read of the ring at all, just
     *              a memset, but rp still advances so the residual drains.
     *   ramping  — only for DC_AUDIO_FADE_STEP-sized transitions, ~8 ms each.
     *
     * ⚠️ BEST-EFFORT ON THE DISARM EDGE. If the ring is empty when the gate
     * disarms — and the ring is empty most of the time, which is the whole
     * measurement in kb/RESUME.md §3 — there is nothing left to fade and the
     * SPU plays out its last buffer at full volume before it hears silence.
     * The fix for that is snd_stream_stop(), which this file deliberately does
     * not call; see dc_audio_pump. */
    {
        int g = (int)s_gain_q8;
        const int tgt = (int)s_gain_tgt;

        if (g == 256 && tgt == 256) {
            for (i = 0; i < copy; i++)
                out[i] = ring_buffer[(rp + i) & RING_BUF_MASK];
        } else if (g == 0 && tgt == 0) {
            memset(out, 0, (size_t)copy * sizeof(s16));
        } else {
            const int step = (DC_AUDIO_FADE_STEP) > 0 ? (DC_AUDIO_FADE_STEP) : 1;
            for (i = 0; i < copy; i += 2) {          /* one step per stereo frame */
                if (g < tgt)      { g += step; if (g > tgt) g = tgt; }
                else if (g > tgt) { g -= step; if (g < tgt) g = tgt; }
                out[i]     = (s16)(((int)ring_buffer[(rp + i)     & RING_BUF_MASK] * g) >> 8);
                out[i + 1] = (s16)(((int)ring_buffer[(rp + i + 1) & RING_BUF_MASK] * g) >> 8);
            }
            /* Nothing to ramp THROUGH: the ring was empty this call, so both
             * ends of the fade are silence and there is no step to smooth.
             * Snap, or the gain never reaches its target on a starving ring and
             * the two fast paths above are never taken again. */
            if (copy == 0) g = tgt;
            s_gain_q8 = (s16)g;
        }
    }
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

    /* THE SCENE LIST IS PARSED BEFORE THE DEVICE IS OPENED, because an empty
     * list has to cost nothing. DC_AUDIO_SCENES=none must be indistinguishable
     * from DC_AUDIO=0 at runtime — no SPU driver, no 2x8192 B of SPU RAM, no
     * per-frame poll — or "turn it off" would not be a real kill switch. */
    /* ---- THE TWO PEAK-COST LEVERS (L1, L2) --------------------------------
     * THE STUTTER IS A BUDGET, NOT A BUG, and these are the only two knobs that
     * change the budget rather than reshuffle it. Measured 2026-08-06: jaudio
     * is bimodal at ~2.5 ms / ~10 ms per DAC frame; at ~10 ms x 2.57 DAC frames
     * per 45 ms game frame that is 57 % of the machine, the frame collapses, the
     * ring falls behind, and the pump spends its 4-frame ceiling catching up.
     * Four other explanations are dead (kb/STATE.md).
     *
     * WHY HERE: AIInit() is reached from Jac_Init() (aictrl.c:69), inside
     * StartAudioThread (audiothread.c:65) — which is BEFORE
     * pc_neos_init_sync() at :82 -> Nas_InitAudio (system.c:1499) ->
     * Nas_SpecChange, the function at memory.c:965-1001 that reads these two
     * fields. So this is the last moment at which a write is still read, and it
     * needs ZERO src/ edits: NA_SPEC_CONFIG is a plain writable .data global
     * (audioconst.c:6, not const).
     *
     * L1 — DC_AUDIO_VOICES, `_05` -> AG.num_channels (memory.c:1001).
     *   Per-voice work is exact and linear: the engine synthesises
     *   updates_per_frame(4) x num_samples_per_update(200) = 800 internal
     *   samples PER ENABLED VOICE, so halving the ceiling halves the WORST
     *   frame while barely moving the mean (typical load is already under it).
     *   ⚠️ It needs no new drop logic. Allocation already runs through
     *   Nas_AllocationOnRequest (channel.c:935) -> __Nas_GetLowerPrio
     *   (channel.c:788-811), which steals lowest-priority-first and REFUSES to
     *   steal from an equal-or-higher priority note. A short pool degrades to
     *   priority-ordered note-stealing, which is the behaviour we want.
     *   Bonus: AG.max_audio_cmds = num_channels*20*updates + 430 = 2,350 at 24
     *   (memory.c:1029), but the DC path writes into pc_task_buf[2][1600]
     *   (neosthread.c:26) and Nas_smzAudioFrame never consults the bound — so
     *   24 voices can overrun .bss by ~6 KB. At 12 the bound is 1,390 < 1600.
     *
     * L2 — DC_AUDIO_MIXRATE, `_00` -> AG.audio_params.sampling_frequency.
     *   This is the INTERNAL mix rate, not the output rate, and halving it
     *   halves num_samples_per_update (memory.c:974) and therefore per-voice
     *   work everywhere — the mean AND the peak.
     *   ⚠️ PITCH IS PRESERVED, and that is not obvious. The effective resample
     *   ratio is basePitch * resample_rate = (sr / JAC_DAC_RATE) *
     *   (33476.156f / sampling_frequency) (oneshot.c:900 x memory.c:993); the
     *   required ratio is sr / sampling_frequency, so the error term is
     *   33476.156 / JAC_DAC_RATE — INDEPENDENT of this field. (That term is the
     *   known ~4.5 % sharpness, and it is not made worse or better here.)
     *   Output rate, DAC_SIZE, JAC_FRAMESAMPLES and the snd_stream open are all
     *   untouched; Jac_Resample16 (rspsim.c:682) computes its step at runtime
     *   from the actual counts, so it upsamples instead of downsampling.
     *   ⚠️ This is the RISKIER of the two — it trades the high band for CPU —
     *   and 24000/60/4 = 100, &~7 = 96, still a multiple of 8 as the engine
     *   requires. Do not pick a rate whose quotient is not.
     *   ⚠️ DO NOT reach for JAC_DAC_RATE (rate.c:4) instead: it cuts no
     *   synthesis work at all and a 22 kHz value shifts every note a perfect
     *   fifth sharp, because the error term above IS a function of it.
     *
     * BOTH DEFAULT TO THE SHIPPED VALUES, so an unset build is byte-identical
     * in behaviour and this block is a no-op that logs one line. */
    {
        na_spec_config *spec = &NA_SPEC_CONFIG[0];
        unsigned old_v = (unsigned)spec->_05, old_r = (unsigned)spec->_00;
        if ((DC_AUDIO_VOICES) > 0 && (DC_AUDIO_VOICES) != (int)spec->_05)
            spec->_05 = (u8)(DC_AUDIO_VOICES);
        if ((DC_AUDIO_MIXRATE) > 0 && (u32)(DC_AUDIO_MIXRATE) != spec->_00)
            spec->_00 = (u32)(DC_AUDIO_MIXRATE);
        DC_LOGE("[DC/AUDIO] spec voices=%u->%u mixrate=%u->%u "
                "(samples/update %u->%u)\n",
                old_v, (unsigned)spec->_05, old_r, (unsigned)spec->_00,
                (unsigned)((old_r / 60u / 4u) & ~7u),
                (unsigned)((spec->_00 / 60u / 4u) & ~7u));
    }

    s_scene_mask = dc_audio_parse_scenes(s_scene_list);
    if (s_scene_mask == 0u) {
        DC_LOGE("[DC/AUDIO] DC_AUDIO_SCENES=\"%s\" selects no scene — "
                "device not opened, synthesis never runs\n", s_scene_list);
        audio_open = 1;
        return;
    }
    DC_LOGE("[DC/AUDIO] scene gate: DC_AUDIO_SCENES=\"%s\" mask=%08X settle=%u\n",
            s_scene_list, (unsigned)s_scene_mask,
            (unsigned)(DC_AUDIO_SCENE_SETTLE));

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
 * removes the whole thing.
 *
 * ==========================================================================
 * THE GATE SKIPS SYNTHESIS ONLY. THE STREAM IS NEVER STOPPED. WHY:
 * ==========================================================================
 * 1. **"Stop pumping" is not silence, it is a buzz.** snd_stream keeps ONE AICA
 *    channel keyed on with a LOOPING sample and rewrites the buffer underneath
 *    it (snd_stream.c's play/write positions). Stop feeding it and it does not
 *    go quiet — it loops the last DC_AUDIO_STREAM_BYTES forever, ~128 ms of the
 *    same fragment, which is a far worse artefact than the silence we are
 *    replacing. So the poll MUST keep running; the callback then zero-fills an
 *    empty ring and the channel plays digital silence.
 *
 * 2. **snd_stream_stop()/start() per scene change is the alternative, and it is
 *    the one that can wedge.** Both go through the SH-4 -> ARM command queue,
 *    and that queue is only serviced because dc_aica_clock_kick() above is
 *    hand-driving AICA_MEM_CLOCK from the SH-4 — the ARM7's Timer-A FIQ is
 *    never delivered, so its main loop is spinning in timer_wait(10) except
 *    when we release it. A stop/start pair therefore depends on the wedge
 *    workaround being armed at exactly the right instant, twice per scene
 *    change; a dropped start leaves the channel keyed off for the rest of the
 *    run with no path back, and a dropped stop leaves it looping. A pop, a
 *    stuck note or a re-wedged ARM7 on every scene change would be worse than
 *    silence, which is the bar this had to clear.
 *
 * 3. **It costs essentially nothing to keep running.** The non-synthesis half
 *    of this function is two G2 accesses (the clock kick) plus
 *    snd_stream_poll(), which early-returns unless the AICA has drained half a
 *    buffer and otherwise copies at most 8 KB. Against 19.8 ms per DAC frame,
 *    gating synthesis alone captures effectively the entire saving.
 *
 * 4. **jaudio does not need a clean stop, because this pump has never been a
 *    clean caller.** pc_audio_process_frame() (audiothread.c:92) drains
 *    `intcount` DSP subframes and calls Jac_UpdateDAC; it is already invoked at
 *    whatever irregular rate the game loop happens to run at, from 30 Hz down
 *    to 9 Hz in the town, and it holds no state that expires. Disarming makes
 *    the gap longer, not different in kind. The one visible consequence is that
 *    the sequencer stops ticking, so the SE slot table stops freeing itself
 *    ([TRG_SE] NO FREE — Nap_ReadSubPort returns -1 while the group is
 *    disabled, game64.c_inc:1026 tests !p5) — which is exactly today's
 *    DC_AUDIO=0 behaviour in every scene, so a disarmed scene is no worse off
 *    than the shipping default. */
void dc_audio_pump(void) {
#if DC_AUDIO && !defined(DC_HOST_STUB)
    u64 t0;
    int frames = 0;
    int armed;

    if (!audio_open) return;
    /* DC_AUDIO_SCENES selected nothing: AIInit never opened a device, so there
     * is no stream to poll and no ARM7 to keep alive. Cost of a linked-in but
     * unselected audio subsystem is this compare. */
    if (s_scene_mask == 0u) return;

#if DC_AICA_CLOCK_KICK
    /* Before anything else: if the ARM7 is wedged, nothing downstream of it can
     * make progress, and every counter in this file reads as a ring bug. */
    dc_aica_clock_kick();
#endif

    /* THE SCENE DECISION, once per pump, before any work is committed. It is
     * cheap enough to re-ask every pump (a shift and a test) and re-asking is
     * what makes the transition prompt, but the ANSWER is a property of the
     * scene, not of the frame — see dc_audio_gate_update(). */
    armed = dc_audio_gate_update();

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
    /* THE BUDGET IS PREDICTIVE, and it has to be — HISTORICAL, and left here
     * because it is the record of why the budget is now off by default
     * (DC_AUDIO_BUDGET_US = 0). What bounds this loop today is the ring
     * headroom and DC_AUDIO_MAX_FRAMES; what bounds it across a RUN is the
     * scene gate. Everything below is still true of the opt-in path.
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
    while (armed) {
        int have = pc_audio_get_buffer_fill();
        u32 c0, cost;
#if (DC_AUDIO_BUDGET_US) > 0
        u32 el;
#endif

        if (have >= (int)(RING_BUF_SAMPLES - DC_AUDIO_HEADROOM))
            break;                       /* the ring is full enough */

        /* The hard cap: no measurement, no override, no feedback. It binds
         * only on the first pump after an arm, where the empty ring would
         * otherwise ask for three DAC frames (~59 ms) in one call. */
        if (frames >= (int)(DC_AUDIO_MAX_FRAMES))
            break;

        /* The predictive budget, now OPT-IN (default 0 = off). Kept because it
         * is the only knob that can reproduce the 10.9 FPS run, not because it
         * is a good idea — read the comment at DC_AUDIO_BUDGET_US before
         * setting it. */
#if (DC_AUDIO_BUDGET_US) > 0
        el = (u32)(dc_time_us() - t0);
        if (s_synth_avg_us != 0u &&
            el + s_synth_avg_us > (u32)DC_AUDIO_BUDGET_US &&
            have > (int)(DC_AUDIO_MIN_FILL)) {
            s_pump_budget_hits++;
            break;
        }
#endif

        c0 = (u32)dc_time_us();
        pc_audio_process_frame();
        cost = (u32)dc_time_us() - c0;
        /* 3:1 EWMA. Fast enough to follow a scene change, slow enough that one
         * unlucky frame does not switch synthesis off for the next second. */
        s_synth_avg_us = s_synth_avg_us ? ((s_synth_avg_us * 3u + cost) >> 2)
                                        : cost;
        /* THE TAIL, not the mean — and the distinction is the whole reason this
         * line exists. MEASURED 2026-08-06: a stuttering frame reports
         * `snd=46.1ms sndf=4` while the mean synthesis cost is 3.78 ms, so four
         * frames should be ~15 ms. snd_stream_poll is exonerated (a
         * DC_AUDIO_MAX_FRAMES=0 run keeps the whole KOS DMA/semaphore path live
         * and costs 0.1 ms), and -O3 on the jaudio tree moved the mean 1.4 %.
         * So ~31 ms is one or two INDIVIDUAL pc_audio_process_frame() calls, and
         * every counter we had reported an average — s_synth_avg_us is an EWMA
         * that already understates the mean by 22 %, which is measurement rule 7
         * (kb/traps.md) committed by the instrument itself.
         * This keeps the per-tick MAXIMUM so the [STUTTER] line can say whether
         * the spike is one expensive frame or many ordinary ones. Cleared by
         * dc_vi.c alongside dc_audio_tick_usec, so the window matches. */
        if (cost > dc_audio_tick_synth_max_us) dc_audio_tick_synth_max_us = cost;

        /* THE VOICE CENSUS — see the DC_AUDIO_VOICELOG block above. Walks the
         * same AG.common_channel[] rows and tests the same `enabled` predicate
         * (audiostruct.h:485) that Nas_DriveRsp tests at driver.c:555, so `n` is
         * the literal noteCount, summed over the four updates.
         *
         * Cost of the probe itself: 4 * 24 = 96 loads per DAC frame, ~2 per
         * pump. Against a 2,500-10,000 us frame that is unmeasurable, so unlike
         * the [GXSPLIT] instrument this one needs no sampling period and no
         * probe= subtraction. It is still behind a flag because it includes a
         * decomp header.
         *
         * `filt`/`comb` are counted, not costed: they answer "what fraction of
         * live voices runs the conditional stages", which is the one unknown
         * standing between L4 (cut FIR/comb) and a number. A small fraction
         * kills that lever for free. */
#ifdef DC_AUDIO_VOICELOG
        {
            int u, i, n = 0, nf = 0, nc = 0;
            int nch = (int)AG.num_channels;
            for (u = 0; u < (int)AG.audio_params.updates_per_frame; u++) {
                for (i = 0; i < nch; i++) {
                    commonch *cc = &AG.common_channel[u * nch + i];
                    if (!cc->enabled) continue;
                    n++;
                    if (cc->filter != NULL) nf++;
                    if (cc->comb_filter_size != 0 && cc->comb_filter_gain != 0)
                        nc++;
                }
            }
            if ((u32)n > dc_audio_tick_voice_max)
                dc_audio_tick_voice_max = (u32)n;
            /* Paired with the cost above, deliberately inside the same `if`
             * condition rather than beside it: these three describe the frame
             * that set smax=, and rewriting them on a cheaper later frame would
             * hand the reader a mismatched row. */
            if (cost >= dc_audio_tick_synth_max_us) {
                dc_audio_tick_voice_at_max = (u32)n;
                dc_audio_tick_filt_at_max  = (u32)nf;
                dc_audio_tick_comb_at_max  = (u32)nc;
            }

            /* THE DECISIVE PART, and it does not depend on a stutter ever
             * firing. Bucketing cost BY voice count turns the hypothesis into a
             * shape the log can refute on its own: if cost rises monotonically
             * with the bucket, voice count is the bimodality and L1 (cap
             * NA_SPEC_CONFIG[0]._05) is priced. If every bucket reports the same
             * mean and the same max, voice count is DEAD and the ~10 ms mode is
             * per-voice conditional work or something outside this loop.
             *
             * Bucket = voice-updates >> 3, so at the 24-channel * 4-update
             * maximum of 96 the top bucket is 12; clamped to 7 (>= 56 voice-
             * updates, i.e. >= 14 concurrent voices), which is above the ~6-voice
             * cheap mode and below the 24-voice ceiling — the split the
             * hypothesis predicts falls inside the range, not at an end of it. */
            {
                int b = n >> 3;
                if (b > 7) b = 7;
                s_vhist_n[b]++;
                s_vhist_us[b] += cost;
                if (cost > s_vhist_max[b]) s_vhist_max[b] = cost;
            }
        }
#endif
        frames++;
    }
    s_pump_frames += (u32)frames;
    s_pump_usec += (u32)(dc_time_us() - t0);

    /* The return value matters: cb stuck at 2 for a whole 600 s run says the
     * consumer died early, and an unchecked poll cannot tell us that. */
    if (s_hnd != SND_STREAM_INVALID) {
        if (snd_stream_poll(s_hnd) < 0) s_poll_fail++;
    }

    /* AFTER the poll, deliberately: s_pump_usec above stops at the synthesis
     * loop, but snd_stream_poll() is the call that copies up to 8 KB into AICA
     * RAM over G2, and a store-queue-less G2 burst is exactly the kind of thing
     * that could own the unattributed ~24 ms. Ending the window before it would
     * rebuild the blind spot this counter exists to remove. The 600-pump log
     * block below is outside it, so DC_LOGE over SCIF is never charged to
     * audio. One extra dc_time_us() per pump, i.e. per logic tick. */
    dc_audio_tick_usec += (u32)(dc_time_us() - t0);
    dc_audio_tick_frames += (u32)frames;

    if (++s_pump_calls >= 600u) {
        DC_LOGE("[DC/AUDIO] synth_us=%u (one jaudio DAC frame; the FPS cost of "
                "sound)\n", (unsigned)s_synth_avg_us);
#ifdef DC_AUDIO_VOICELOG
        /* The whole-run distribution, one row per voice bucket. Read it as a
         * SHAPE, not as eight numbers: mean rising with the bucket confirms the
         * voice-count hypothesis, a flat mean with a large max in every bucket
         * refutes it. `n=` is the population — a bucket with n=0 says nothing
         * and must not be read as "cheap". max/mean within one bucket is the
         * residual bimodality that voice count does NOT explain. */
        {
            int b;
            for (b = 0; b < 8; b++) {
                if (s_vhist_n[b] == 0u) continue;
                DC_LOGE("[DC/VOICE] v>=%2d n=%u mean=%uus max=%uus\n",
                        b * 8, (unsigned)s_vhist_n[b],
                        (unsigned)(s_vhist_us[b] / s_vhist_n[b]),
                        (unsigned)s_vhist_max[b]);
            }
        }
#endif
        /* The gate's own line, kept separate so a log reader can answer "was
         * this window paying for audio at all?" before reading any of the
         * counters below, which mean different things on each side of it. */
        DC_LOGE("[DC/AUDIO] gate scene=%u armed=%u arms=%u disarms=%u gain=%d\n",
                (unsigned)sou_scene_mode, (unsigned)s_armed,
                (unsigned)s_gate_arms, (unsigned)s_gate_disarms,
                (int)s_gain_q8);
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
