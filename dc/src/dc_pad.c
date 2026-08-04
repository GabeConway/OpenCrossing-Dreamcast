/* dc_pad.c - GameCube controller emulation over Dreamcast maple.
 *
 * Replaces pc/src/pc_pad.c (SDL GameController). PLAN §7 is the mapping spec.
 *
 * WHAT THE DREAMCAST PAD DOES NOT HAVE:
 *   - a C-stick   -> D-pad substitutes (camera)
 *   - a Z button  -> L trigger substitutes
 *   - a second analog stick or shoulder clicks
 * so the GameCube's substick is driven from the D-pad and PAD_TRIGGER_Z comes
 * from the analog L trigger. Analog R stays a real analog trigger.
 *
 * PADClamp is the ONE Dolphin SDK source file the port keeps
 * (src/static/dolphin/pad/Padclamp.c, called by JUTGamePad.cpp) — the DC build
 * must add it back to the source list exactly as pc/CMakeLists.txt does.
 */
#include "dc_platform.h"
#include <dolphin/pad.h>

/* GameCube stick range the game expects. The base port used 80 for its
 * digital-to-analog synthesis; keep it so walk speed matches. */
#define DC_STICK_MAGNITUDE   80
#define DC_TRIGGER_THRESHOLD 100

static int s_pad_present = 0;
static int s_pad_logged  = 0;

/* --------------------------------------------------------------------------
 * THE BUTTON LATCH — why a press can be dropped entirely on real hardware.
 *
 * KOS refreshes cont_state_t at 60 Hz off the vblank IRQ, and it does that
 * whatever the frame rate. This port, however, samples it exactly ONCE per
 * game logic tick: padmgr_RequestPadData (src/padmgr.c:328-341) gates on
 * pc_frame_counter, which dc_vi.c increments once per VIWaitForRetrace.
 *
 * That is not how the GameCube did it, and src/padmgr.c:309-314 says so in as
 * many words: "On GC, the padmgr thread runs asynchronously and accumulates
 * triggers between reads." The PC port dropped the accumulation because at
 * 60 FPS nothing could fall between two samples.
 *
 * The gates that matter are EDGES, not levels. src/padmgr.c:156-159 derives
 * them from a single-sample XOR of consecutive reads, and the dialogue advance
 * is chkTrigger(BUTTON_A) || chkTrigger(BUTTON_B) (m_msg_normal.c_inc:2), which
 * reads pads[0].on.button. So a press that goes down AND back up between two
 * samples produces no edge at all and is simply lost.
 *
 * In Flycast at 22-30 FPS the sample period is 33-45 ms and a human tap
 * (~80-120 ms) always straddles a sample. **On hardware in the town the port
 * runs at ~11 FPS — a 91 ms period — and if the CD-R is also paging, worse.**
 * A quick tap then has a real chance of landing entirely between samples. The
 * symptom is exactly "I press A and nothing happens", which is what a stall at
 * the K.K. Slider scene looks like: the one input that scene needs is a rising
 * edge of A or B on channel 0.
 *
 * The fix is to put the GameCube's accumulation back where it belongs — at
 * 60 Hz, below the game's tick rate. A vblank handler ORs every button it ever
 * sees into a sticky mask; PADRead consumes and clears it. A tap between ticks
 * therefore arrives as pressed on the next tick (rising edge) and released on
 * the one after (falling edge), which is what GX's padmgr would have produced.
 *
 * Safe in IRQ context: maple_dev_status() is non-blocking in KOS 2.3 — it
 * returns dev->status after checking dev->valid && dev->drv (maple_enum.c:98)
 * — and the accumulate is a single OR into a volatile word, so a torn read is
 * not possible on SH-4 for a 32-bit aligned store. It cannot fight KOS's own
 * maple polling either: that runs from the same vblank IRQ, and ORing is
 * monotonic, so ordering between the two does not change the result.
 *
 * Deliberately buttons ONLY. Sticks and triggers are levels, not events, and
 * latching a level would make a released stick appear held for a whole tick.
 * That is also why the L-trigger auto-advance path (chkButton(BUTTON_L),
 * m_msg_normal.c_inc:4) is unaffected by any of this — it is a held test.
 *
 * Kill switch: -DDC_PAD_NO_LATCH restores the pre-2026-08-03 behaviour, in
 * which case this compiles to nothing. */
#ifndef DC_PAD_NO_LATCH
#ifndef DC_HOST_STUB
#include <dc/vblank.h>

static volatile unsigned int s_btn_latch;
static int s_latch_hnd = -1;

static void dc_pad_vblank(uint32_t code, void* data) {
    maple_device_t* dev;
    cont_state_t* st;

    (void)code; (void)data;

    dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    if (!dev)
        return;
    st = (cont_state_t*)maple_dev_status(dev);
    if (!st)
        return;

    s_btn_latch |= (unsigned int)st->buttons;
}

/* Take everything seen since the last call and start accumulating again. */
static unsigned int dc_pad_latch_take(void) {
    unsigned int v = s_btn_latch;
    s_btn_latch = 0;
    return v;
}
#else
static unsigned int dc_pad_latch_take(void) { return 0; }
#endif /* !DC_HOST_STUB */
#endif /* !DC_PAD_NO_LATCH */

/* --------------------------------------------------------------------------
 * DC_AUTOSTART=<N> — synthesise button presses so an unattended run can get
 * past a menu (kb/boot-blockers.md item 4).
 *
 * The title demo loops forever in aAL_game_start_wait (ac_animal_logo.c:245)
 * waiting for START or A, and nothing in the harness can press a button, so
 * every blocker after the title has only ever been evaluated by reading code.
 * This is the cheapest way to make them observable, and unlike a Flycast input
 * script it also works on hardware.
 *
 * Semantics: from PADRead call N onward, emit a pulse of DC_AUTOSTART_HOLD
 * calls every DC_AUTOSTART_PERIOD calls, mostly A with every
 * DC_AUTOSTART_START_EVERY'th pulse being START. The knob is absent by default,
 * so a normal build is byte-identical to before (kill switch by construction).
 *
 * The mix is not arbitrary. Past the title (which takes either button), A is
 * the button that advances everything: dialogue pages take A or B only
 * (m_msg_normal.c_inc:2), and every choice menu defaults to index 0, which A
 * accepts. START is needed at exactly one place on the path to the town —
 * mED_COMMAND_END_EDIT in the name-entry keyboard (m_editor_ovl.c:447) — and
 * that keyboard also rejects an all-blank name (m_editor_ovl.c:1165), so the
 * sequence has to be "some A presses, then a START". A 1:1 alternation, which
 * is what this emitted before, wasted half of every run's presses.
 * DC_AUTOSTART_START_EVERY=2 restores the old pattern exactly.
 * -------------------------------------------------------------------------- */
#ifdef DC_AUTOSTART
#ifndef DC_AUTOSTART_PERIOD
#define DC_AUTOSTART_PERIOD 90u
#endif
#ifndef DC_AUTOSTART_HOLD
#define DC_AUTOSTART_HOLD 6u
#endif
#ifndef DC_AUTOSTART_START_EVERY
#define DC_AUTOSTART_START_EVERY 4u
#endif

static u32 s_autostart_calls = 0;

static u16 dc_autostart_buttons(void) {
    u32 n = s_autostart_calls++;
    u32 since, phase, pulse;
    u16 button;

    if (n < (u32)(DC_AUTOSTART)) return 0;

    since = n - (u32)(DC_AUTOSTART);
    phase = since % DC_AUTOSTART_PERIOD;
    if (phase >= DC_AUTOSTART_HOLD) return 0;

    pulse  = since / DC_AUTOSTART_PERIOD;
    {
        u32 is_start = ((pulse + 1u) % (u32)(DC_AUTOSTART_START_EVERY)) == 0u;
        button = is_start ? PAD_BUTTON_START : PAD_BUTTON_A;
        if (phase == 0) {
            DC_LOGE("[DC/PAD] autostart pulse %u: %s (call %u)\n",
                    (unsigned)pulse, is_start ? "START" : "A", (unsigned)n);
        }
    }
    return button;
}
#endif /* DC_AUTOSTART */

BOOL PADInit(void) {
#ifndef DC_HOST_STUB
    maple_device_t* dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
    s_pad_present = (dev != NULL);
#ifndef DC_PAD_NO_LATCH
    /* Register once. INIT_MAPLE_ALL means arch init already ran
     * maple_wait_scan() before main (init.c:226), so the bus is up and this
     * cannot race enumeration. */
    if (s_latch_hnd < 0) {
        s_latch_hnd = vblank_handler_add(dc_pad_vblank, NULL);
        DC_LOGE("[DC/PAD] button latch %s\n",
                s_latch_hnd >= 0 ? "armed (60 Hz)" : "FAILED to arm");
    }
#endif
#endif
    DC_LOGE("[DC/PAD] controller %s\n", s_pad_present ? "found" : "NOT found");
    return TRUE;
}

u32 PADRead(PADStatus* status) {
    u16 buttons = 0;
    s8 stickX = 0, stickY = 0;
    s8 cstickX = 0, cstickY = 0;
    u8 ltrig = 0, rtrig = 0;
#ifndef DC_HOST_STUB
    unsigned int cur = 0;
#endif

    memset(status, 0, sizeof(PADStatus) * 4);

#ifdef DC_AUTOSTART
    /* Counted once per PADRead, on every path — including the no-controller
     * early return below, which is what a bare Flycast run takes. */
    buttons |= dc_autostart_buttons();
#endif

#ifndef DC_HOST_STUB
    {
        maple_device_t* dev = maple_enum_type(0, MAPLE_FUNC_CONTROLLER);
        cont_state_t* st = dev ? (cont_state_t*)maple_dev_status(dev) : NULL;

        if (!dev || !st) {
            /* No pad: report channel 0 present with neutral input rather than
             * PAD_ERR_NO_CONTROLLER. The base port learned that the game's
             * error path is not worth exercising on a handheld, and the same
             * is true when a DC controller is hot-unplugged mid-scene. */
            s_pad_present = 0;
            status[0].button = buttons;   /* DC_AUTOSTART, or 0 */
            status[0].err = PAD_ERR_NONE;
            return PAD_CHAN0_BIT;
        }
        if (!s_pad_present && !s_pad_logged) {
            s_pad_logged = 1;
            DC_LOGE("[DC/PAD] controller attached\n");
        }
        s_pad_present = 1;

        /* Fold in every button seen since the last tick, so a press that went
         * down and back up between two samples still produces an edge. See the
         * latch note at the top of this file — this is the one line that makes
         * a tap survive an 11 FPS frame. */
#ifndef DC_PAD_NO_LATCH
        cur = (unsigned int)st->buttons | dc_pad_latch_take();
#else
        cur = (unsigned int)st->buttons;
#endif

        /* --- face buttons (PLAN §7 mapping) --- */
        if (cur & CONT_A)     buttons |= PAD_BUTTON_A;
        if (cur & CONT_B)     buttons |= PAD_BUTTON_B;
        if (cur & CONT_X)     buttons |= PAD_BUTTON_X;
        if (cur & CONT_Y)     buttons |= PAD_BUTTON_Y;
        if (cur & CONT_START) buttons |= PAD_BUTTON_START;

        /* --- triggers ---
         * L trigger stands in for the GameCube Z button (the menu/inventory
         * modifier); R trigger is the run/hold analog trigger. */
        ltrig = (u8)st->ltrig;
        rtrig = (u8)st->rtrig;
        if (ltrig > DC_TRIGGER_THRESHOLD) buttons |= PAD_TRIGGER_Z | PAD_TRIGGER_L;
        if (rtrig > DC_TRIGGER_THRESHOLD) buttons |= PAD_TRIGGER_R;

        /* --- analog stick ---
         * KOS reports joyx/joyy as signed -128..127 with Y DOWN-positive;
         * GameCube wants Y UP-positive. */
        stickX = (s8)st->joyx;
        stickY = (s8)(-(int)st->joyy > 127 ? 127 : -(int)st->joyy);

        /* --- D-pad -> C-stick substitute (camera) ---
         * NOT latched: the C-stick is a LEVEL, and a latched direction would
         * keep the camera swinging for a tick after the player let go.
         * PLAN §7: "dpad=camera/C-stick substitute". Held D-pad therefore does
         * NOT reach the game as PAD_BUTTON_*; if a menu turns out to need the
         * digital D-pad, this is the block to make mode-dependent. */
        if (st->buttons & CONT_DPAD_UP)    cstickY =  DC_STICK_MAGNITUDE;
        if (st->buttons & CONT_DPAD_DOWN)  cstickY = -DC_STICK_MAGNITUDE;
        if (st->buttons & CONT_DPAD_LEFT)  cstickX = -DC_STICK_MAGNITUDE;
        if (st->buttons & CONT_DPAD_RIGHT) cstickX =  DC_STICK_MAGNITUDE;

        /* Second D-pad (some pads / the arcade stick) mirrors the digital
         * directions the game menus expect. */
        if (cur & CONT_DPAD2_UP)    buttons |= PAD_BUTTON_UP;
        if (cur & CONT_DPAD2_DOWN)  buttons |= PAD_BUTTON_DOWN;
        if (cur & CONT_DPAD2_LEFT)  buttons |= PAD_BUTTON_LEFT;
        if (cur & CONT_DPAD2_RIGHT) buttons |= PAD_BUTTON_RIGHT;
    }
#endif

    status[0].button      = buttons;
    status[0].stickX      = stickX;
    status[0].stickY      = stickY;
    status[0].substickX   = cstickX;
    status[0].substickY   = cstickY;
    status[0].triggerLeft = ltrig;
    status[0].triggerRight= rtrig;
    status[0].err         = PAD_ERR_NONE;

    return PAD_CHAN0_BIT;
}

void PADControlMotor(s32 chan, u32 command) {
    (void)chan; (void)command;
    /* KOS has dc/maple/purupuru.h for the Jump Pack. Not wired up because the
     * effect encoding (purupuru_effect_t) has not been verified, and a wrong
     * one can leave the pack buzzing indefinitely. */
    DC_UNIMPLEMENTED_NOTE("Jump Pack rumble via dc/maple/purupuru.h");
}

void PADControlAllMotors(const u32* commands) {
    if (commands) PADControlMotor(0, commands[0]);
}

void PADCleanup(void) { s_pad_present = 0; }

BOOL PADReset(u32 mask)        { (void)mask; return TRUE; }
BOOL PADRecalibrate(u32 mask)  { (void)mask; return TRUE; }
BOOL PADSync(void)             { return TRUE; }
void PADSetSpec(u32 spec)      { (void)spec; }
void PADSetAnalogMode(u32 mode){ (void)mode; }
BOOL PADGetType(s32 chan, u32* type) {
    (void)chan;
    if (type) *type = 0x09000000;   /* standard controller */
    return TRUE;
}
u32  PADGetSpec(void) { return PAD_SPEC_5; }
void PADSetSamplingRate(u32 msec) { (void)msec; }
void __PADTestSamplingRate(u32 tvmode) { (void)tvmode; }
PADSamplingCallback PADSetSamplingCallback(PADSamplingCallback callback) {
    (void)callback;
    return NULL;
}
