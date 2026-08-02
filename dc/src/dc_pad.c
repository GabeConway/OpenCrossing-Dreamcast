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
#endif
    DC_LOGE("[DC/PAD] controller %s\n", s_pad_present ? "found" : "NOT found");
    return TRUE;
}

u32 PADRead(PADStatus* status) {
    u16 buttons = 0;
    s8 stickX = 0, stickY = 0;
    s8 cstickX = 0, cstickY = 0;
    u8 ltrig = 0, rtrig = 0;

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

        /* --- face buttons (PLAN §7 mapping) --- */
        if (st->buttons & CONT_A)     buttons |= PAD_BUTTON_A;
        if (st->buttons & CONT_B)     buttons |= PAD_BUTTON_B;
        if (st->buttons & CONT_X)     buttons |= PAD_BUTTON_X;
        if (st->buttons & CONT_Y)     buttons |= PAD_BUTTON_Y;
        if (st->buttons & CONT_START) buttons |= PAD_BUTTON_START;

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
         * PLAN §7: "dpad=camera/C-stick substitute". Held D-pad therefore does
         * NOT reach the game as PAD_BUTTON_*; if a menu turns out to need the
         * digital D-pad, this is the block to make mode-dependent. */
        if (st->buttons & CONT_DPAD_UP)    cstickY =  DC_STICK_MAGNITUDE;
        if (st->buttons & CONT_DPAD_DOWN)  cstickY = -DC_STICK_MAGNITUDE;
        if (st->buttons & CONT_DPAD_LEFT)  cstickX = -DC_STICK_MAGNITUDE;
        if (st->buttons & CONT_DPAD_RIGHT) cstickX =  DC_STICK_MAGNITUDE;

        /* Second D-pad (some pads / the arcade stick) mirrors the digital
         * directions the game menus expect. */
        if (st->buttons & CONT_DPAD2_UP)    buttons |= PAD_BUTTON_UP;
        if (st->buttons & CONT_DPAD2_DOWN)  buttons |= PAD_BUTTON_DOWN;
        if (st->buttons & CONT_DPAD2_LEFT)  buttons |= PAD_BUTTON_LEFT;
        if (st->buttons & CONT_DPAD2_RIGHT) buttons |= PAD_BUTTON_RIGHT;
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
