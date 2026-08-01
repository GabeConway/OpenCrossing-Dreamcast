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
