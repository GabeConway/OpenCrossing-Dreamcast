/* dc_npcdiag.c — N3: ONE INSTRUMENT THAT NAMES THE WALL IN A SINGLE TOWN RUN.
 * =============================================================================
 *
 * WHAT IS ALREADY SETTLED, so that nothing here re-measures it.
 *
 *   * The ROSTER IS FINE. dc/src/dc_npcseed.c printed, in the town:
 *       [DC/NPCSEED] pre ids=6 homed=6 | seeded id=8 home=8 | want=14 max=14
 *     mNpc_SetNpcList (src/game/m_npc.c:2799) then populates Common_Get(npclist)
 *     and sets appear_flag = TRUE.
 *   * The SAVE-PATH diagnosis is FALSIFIED (kb/RESUME.md, memory
 *     dc-town-has-no-villagers), and so is the field-data one.
 *   * The MISSING `return` in aNPC_setupActor_proc
 *     (src/actor/npc/ac_npc_ctrl.c_inc:519) is FIXED — the fix is in the built
 *     scratch tree, in tools/dcstub/make_src_shrink.py's _s1c_rules().
 *
 * So the break is downstream of the list, in ACTOR CONSTRUCTION, and this file
 * exists because the chain from there is five functions long with nine serial
 * gates and NONE of them says anything when it refuses.
 *
 * THE CHAIN (verified against the vendored source 2026-08-10; prefer the
 * SYMBOL to the line number, CLAUDE.md §5):
 *
 *   aSNMgr_actor_ct            ac_set_npc_manager.c:43   the manager exists
 *     -> aSNMgr_actor_move                         :1250  per logic tick
 *          switch (mFI_GetPlayerWade())            :1255  WADE dispatch
 *          (*manager->set_proc)(manager)           :1278  REGULAR or GUEST
 *            -> aSNMgr_set_npc_regular             :1139
 *                 aSNMgr_chk_exist_and_appear      :1155
 *                 aSNMgr_check_in_scope            :1156
 *                 aSNMgr_set_appear_info_regular   :1169
 *                 aSNMgr_get_safe_utnum            :1172
 *                 aSNMgr_set_make_npc              :1176
 *            -> aSNMgr_set_npc_guest               :1199  (the same five)
 *          aSNMgr_make_npc                         :1279
 *            gate CLIP(npc_clip) && setupActor_proc :734
 *            -> aNPC_setupActor_proc      ac_npc_ctrl.c_inc:293
 *                 -> aNPC_setupActor_sub                  :278
 *                      gate aNPC_setupNpc_check           :282
 *                      -> Actor_info_make_actor    m_actor.c:748
 *
 * THE FIVE LIVE HYPOTHESES, AND THE FIELD ON THE LINE THAT KILLS EACH
 * -------------------------------------------------------------------
 * 1. THE REGULAR PASS ONLY RUNS ON AN ACRE TRANSITION. aSNMgr_set_npc_regular
 *    tails into `aSNMgr_setup_set_proc(manager, GUEST)` whenever the wade is
 *    mFI_WADE_NONE (:1189-1196), and only `case mFI_WADE_START:` (:1256) ever
 *    puts REGULAR back. If mFI_GetPlayerWade() (m_field_info.c:2094, fed by
 *    mPlib_check_player_actor_main_index_AllWade) never leaves WADE_NONE, the
 *    town gets exactly ONE regular pass ever, at ct time.
 *    -> `wade: start=0` with `none=` large, and `mode=2` forever.
 *
 * 2. ALL TEN CLOTH BANKS FAILED TO RESERVE. aNPC_keep_cloth_data_area
 *    (ac_npc_cloth.c_inc:226) leaves every slot at mSC_BANK_NONE when
 *    mSc_secure_exchange_keep_bank (m_scene.c:38) runs out of banks or RAM;
 *    aNPC_get_new_cloth_data_area then never returns a slot, so
 *    aNPC_dma_regist_check_cloth_data is FALSE forever and aNPC_setupNpc_check
 *    can never be TRUE.  -> `cloth=0/10`.
 *
 * 3. CLIP(npc_clip) IS NULL. It is assigned in exactly one place,
 *    aNPC_actor_ct_c (ac_npc_ctrl.c_inc:814), so it is NULL iff the NPC control
 *    actor was never constructed — which is what happens when
 *    Actor_info_make_actor bails at m_actor.c:760 (the actor cap) or :765-770
 *    (the TARGET_PC profile / class_size guard, which IS live on this target).
 *    -> `ct: ctl=0` and `mk: ent>0 gate=0`.
 *
 * 4. NO UNIT IS EVER SAFE. aSNMgr_get_safe_utnum_regular (:530) -> the 3x3 scan
 *    -> aSNMgr_check_safe_ut (:380) -> mNpc_CheckNpcSet_fgcol (m_npc.c:4590)
 *    plus !mCoBG_ExistHeightGap_KeepAndNow. Suspicious because dc_npcseed.c's
 *    own header records that this port's FGDATA reserve-marker scan finds
 *    nothing.  -> `reg: appear>0 utnum=0` with `ut: fgcol=0` (field data) or
 *    `ut: fgcol>0 hgap=0` (height gap).
 *
 * 5. THE PLAYER NEVER ENTERED A VILLAGER'S ACRE — benign, and it must not be
 *    mistaken for a bug. The scope is one acre around player_pos.next_block
 *    (aSNMgr_renewal_set_scope, :224).  -> `reg: exist>0 scope=0`, AND the
 *    [DC/NPCDIAG] wade line, which prints the player's block next to every
 *    roster entry's block so "we were never near one" is visible rather than
 *    inferred.
 *
 * ⚠️ 6. A FIRST-PASS CLOTH FAILURE IS NORMAL AND IS NOT THE BUG.
 *    aNPC_dma_regist_check_cloth_data (ac_npc_cloth.c_inc:204) returns FALSE
 *    the first time it is asked about any cloth BY DESIGN — the sibling
 *    aNPC_dma_regist_cloth_data only REGISTERS the transfer, and the DMA
 *    completes a frame later through aNPC_dma_cloth_data (:1) ->
 *    mSc_background_dmacopy_controller (m_scene.c:64) -> dma_flag = FALSE
 *    (:19). So `setup: chk` lagging `setup: ent` by a few is the healthy
 *    steady state. The counters here are CUMULATIVE for exactly this reason:
 *    over hundreds of retries a working cloth path makes `chk` climb, and a
 *    broken one pins it at 0. Judge the RATIO over a long window, never a
 *    single frame.
 *
 * WHY COUNTERS AND NOT A PER-FRAME PRINTF
 * ---------------------------------------
 * KOS busy-waits on the SCIF FIFO, so a print is ~1 ms per 60 characters on
 * hardware. A per-frame line would change the thing being measured and can
 * stall the boot outright. Everything here is a static counter; ONE line goes
 * out every DC_NPCDIAG_PERIOD presented frames (default 300 — ~15 s at the
 * town's 20 FPS), driven from dc/src/dc_vi.c's existing 30-frame report window
 * beside [EMU64C] and [EMU64H]. The one-shot facts — the manager's ct, the
 * controller's ct with its cloth tally, the clip install, and each wade
 * transition — print at the moment they are known and are capped, because a
 * fact that arrives once is worth a line and a fact that arrives a thousand
 * times is worth a counter.
 *
 * WHERE THE CALLS COME FROM
 * -------------------------
 * tools/dcstub/make_src_shrink.py, rule N3, into the DC_SRC_SHRINK scratch tree
 * — the same mechanism that carries R2/R3's load seams and the
 * `return aNPC_setupActor_sub` fix. `src/` is never edited (CLAUDE.md §1).
 * Every injected site is a single dc_npcdiag_*() call, and each rewriter is
 * skipped entirely at DC_NPCDIAG=0.
 */

#include "dc_platform.h"
#include "dc_npcdiag.h"

#if defined(DC_NPCDIAG) && (DC_NPCDIAG) > 0

/* The game's own headers. dc/src/dc_npcseed.c already proves this include path
 * reaches include/ from a dc/src TU and that these are plain C headers with no
 * layout games. m_npc.h is what gives us mNpc_NpcList_c and ANIMAL_NUM_MAX;
 * m_name_table.h gives EMPTY_NO. */
#include "m_npc.h"
#include "m_name_table.h"

/* Restated rather than pulled from m_field_info.h, for the same reason
 * dc_npcseed.c restates npc_def_list: one prototype is a smaller dependency
 * than a header this TU otherwise has no use for. Pinned against
 * include/m_field_info.h:235 — if it ever drifts, this TU stops compiling. */
extern int mFI_Wpos2BlockNum(int* bx, int* bz, xyz_t wpos);

#ifndef DC_NPCDIAG_PERIOD
#define DC_NPCDIAG_PERIOD 300
#endif

/* dc_vi.c offers us a call once per 30 presented frames and no oftener, so the
 * period is quantised to that grid. Round UP: a period of 1 must not mean "no
 * line ever", it must mean "as often as the instrument can speak". */
#define DIAG_WINDOWS ((((DC_NPCDIAG_PERIOD) + 29) / 30) < 1 \
                      ? 1 : (((DC_NPCDIAG_PERIOD) + 29) / 30))

/* How many one-shots of each kind are allowed out before the counters take
 * over.
 *
 * ⚠️ ONE ACRE CROSSING IS FOUR WADE TRANSITIONS, not one:
 * NONE -> START -> INPROGRESS -> END -> NONE (m_field_info.c:2064-2082). So the
 * cap has to be a multiple of four to be worth anything, and the ROSTER — which
 * is ~300 characters and, with no villager actors alive, never changes — is
 * printed in full only for the first few. After that the header alone carries
 * what the later transitions add: which acre the player walked into. At 57600
 * baud on hardware the whole budget is well under a second of console. */
#define DIAG_CT_PRINT_MAX     8
#define DIAG_WADE_PRINT_MAX  48
#define DIAG_ROSTER_PRINT_MAX 4

static unsigned int s_g[DC_NPCDIAG_G_NUM];
static unsigned int s_g_oob;

static unsigned int s_ticks;
static unsigned int s_windows;
static unsigned int s_window_n;

static unsigned int s_mgr_ct;        /* aSNMgr_actor_ct fired                 */
static unsigned int s_ctl_ct;        /* aNPC_actor_ct_c fired                 */
static unsigned int s_clip_set;      /* CLIP(npc_clip) installed              */
static int          s_cloth_ok = -1; /* banks reserved on the LAST keep call  */
static int          s_cloth_n  = -1; /* banks asked for on the LAST keep call */
static int          s_cloth_acc;
static int          s_cloth_tot;

static unsigned int s_wade[8];       /* mFI_WADE_* histogram, 5 used          */
static int          s_wade_last = -1;
static int          s_mode_last = -1;

static unsigned int s_ct_prints;
static unsigned int s_wade_prints;

int dc_npcdiag_gate(int slot, int val)
{
    if ((unsigned int)slot < (unsigned int)DC_NPCDIAG_G_NUM) {
        if (val) {
            s_g[slot]++;
        }
    } else {
        s_g_oob++;
    }

    return val;
}

/* THE LINE THAT SEPARATES HYPOTHESIS 5 FROM A REAL BUG. Printed on the first
 * tick and on every wade transition: the player's acre next to the acre of
 * every roster entry that exists. "We were never near one" then reads off the
 * page instead of being inferred from a zero. */
static void diag_print_roster(const void* npclist,
                              int next_bx, int next_bz, int now_bx, int now_bz,
                              unsigned int exist, unsigned int appear,
                              int wade, int mode)
{
    const mNpc_NpcList_c* list = (const mNpc_NpcList_c*)npclist;
    int i;

    dc_loge_impl("[DC/NPCDIAG] wade %d->%d t=%u mode=%d player next=(%d,%d) "
                 "now=(%d,%d) exist=%04X appear=%04X |",
                 s_wade_last, wade, s_ticks, mode,
                 next_bx, next_bz, now_bx, now_bz,
                 exist & 0xFFFFu, appear & 0xFFFFu);

    if (list == NULL) {
        dc_loge_impl(" npclist=NULL\n");
        return;
    }

    /* The roster is static — nothing is walking, which is the whole problem —
     * so it is worth printing in full a few times and never again. The header
     * above is what the later transitions are for. */
    if (s_wade_prints > DIAG_ROSTER_PRINT_MAX) {
        dc_loge_impl(" (roster as above)\n");
        return;
    }

    for (i = 0; i < ANIMAL_NUM_MAX; i++) {
        int bx = -1;
        int bz = -1;

        if (list[i].name == EMPTY_NO) {
            continue;
        }

        /* mFI_Wpos2BlockNum returns FALSE for a position outside the town
         * grid, and leaves its outputs alone when it does — which is itself a
         * finding, so print the -1 rather than hiding the entry. */
        (void)mFI_Wpos2BlockNum(&bx, &bz, list[i].position);
        dc_loge_impl(" %d:%04X@(%d,%d)%s", i, (unsigned int)list[i].name,
                     bx, bz, list[i].appear_flag ? "" : "!");
    }

    dc_loge_impl("\n");
}

void dc_npcdiag_tick(int wade, int set_mode,
                     int next_bx, int next_bz, int now_bx, int now_bz,
                     unsigned int exist, unsigned int appear,
                     const void* npclist)
{
    s_ticks++;
    if ((unsigned int)wade < (unsigned int)(sizeof(s_wade) / sizeof(s_wade[0]))) {
        s_wade[wade]++;
    }

    if ((wade != s_wade_last || set_mode != s_mode_last) &&
        s_wade_prints < DIAG_WADE_PRINT_MAX) {
        s_wade_prints++;
        diag_print_roster(npclist, next_bx, next_bz, now_bx, now_bz,
                          exist, appear, wade, set_mode);
    }

    s_wade_last = wade;
    s_mode_last = set_mode;
}

void dc_npcdiag_mgr_ct(void)
{
    s_mgr_ct++;
    if (s_ct_prints < DIAG_CT_PRINT_MAX) {
        s_ct_prints++;
        dc_loge_impl("[DC/NPCDIAG] mgr_ct #%u t=%u -- SET_NPC_MANAGER exists\n",
                     s_mgr_ct, s_ticks);
    }
}

void dc_npcdiag_cloth_begin(void)
{
    s_cloth_acc = 0;
    s_cloth_tot = 0;
}

void dc_npcdiag_cloth_bank(int ok)
{
    s_cloth_tot++;
    if (ok) {
        s_cloth_acc++;
    }
}

void dc_npcdiag_ctrl_ct(int is_npc2, int clip_was_already_set)
{
    /* aNPC_keep_cloth_data_area has just returned, so this pair is final for
     * this controller. Latched rather than accumulated: the function runs again
     * on every scene load and a running total would read as 30/10. */
    s_cloth_ok = s_cloth_acc;
    s_cloth_n  = s_cloth_tot;
    s_ctl_ct++;

    if (s_ct_prints < DIAG_CT_PRINT_MAX) {
        s_ct_prints++;
        dc_loge_impl("[DC/NPCDIAG] ctrl_ct #%u t=%u npc2=%d clip_pre=%d "
                     "cloth=%d/%d\n",
                     s_ctl_ct, s_ticks, is_npc2, clip_was_already_set,
                     s_cloth_ok, s_cloth_n);
    }
}

void dc_npcdiag_clip_set(int is_npc2)
{
    s_clip_set++;
    if (s_ct_prints < DIAG_CT_PRINT_MAX) {
        s_ct_prints++;
        /* ⚠️ setupActor_proc is installed ONLY in the non-NPC2 flavour
         * (ac_npc_ctrl.c_inc:817-821). A clip installed by the NPC2 controller
         * satisfies `CLIP(npc_clip) != NULL` and still fails
         * aSNMgr_make_npc's gate, which is exactly the shape hypothesis 3
         * predicts. */
        dc_loge_impl("[DC/NPCDIAG] clip_set #%u t=%u npc2=%d setupActor_proc=%s\n",
                     s_clip_set, s_ticks, is_npc2, is_npc2 ? "NOT INSTALLED" : "yes");
    }
}

void dc_npcdiag_report(void)
{
    s_window_n++;
    if (s_window_n < (unsigned int)DIAG_WINDOWS) {
        return;
    }
    s_window_n = 0;
    s_windows++;

    /* ONE line, CUMULATIVE for the run. Walk it left to right: the first field
     * that is zero while its left neighbour is not is the wall. `oob=` must be
     * zero — a non-zero is a rewriter/header slot drift, not a game fault. */
    dc_loge_impl(
        "[DC/NPCDIAG] w=%u t=%u | ct: mgr=%u ctl=%u clip=%u cloth=%d/%d"
        " | wade: none=%u start=%u prog=%u end=%u err=%u mode=%d"
        " | reg: calls=%u exist=%u scope=%u appear=%u utnum=%u make=%u"
        " | ut: calls=%u col=%u fgcol=%u hgap=%u"
        " | arb[pass]: work=%u intro=%u demo1=%u demo2=%u hallo=%u"
        " | gst: calls=%u arb=%u blkmax=%u exist=%u(ea=%u jevt=%u)"
        " scope=%u appear=%u utnum=%u make=%u"
        " | mk: ent=%u gate=%u slot=%u idx=%u called=%u ret=%u"
        " | setup: ent=%u chk=%u actor=%u | oob=%u\n",
        s_windows, s_ticks,
        s_mgr_ct, s_ctl_ct, s_clip_set, s_cloth_ok, s_cloth_n,
        s_wade[0], s_wade[1], s_wade[2], s_wade[3], s_wade[4], s_mode_last,
        s_g[DC_NPCDIAG_G_REG_CALL], s_g[DC_NPCDIAG_G_REG_EXIST],
        s_g[DC_NPCDIAG_G_REG_SCOPE], s_g[DC_NPCDIAG_G_REG_APPEAR],
        s_g[DC_NPCDIAG_G_REG_UTNUM], s_g[DC_NPCDIAG_G_REG_MAKE],
        s_g[DC_NPCDIAG_G_UT_CALL], s_g[DC_NPCDIAG_G_UT_COL],
        s_g[DC_NPCDIAG_G_UT_FGCOL], s_g[DC_NPCDIAG_G_UT_HGAP],
        s_g[DC_NPCDIAG_G_ARB_ARBEIT], s_g[DC_NPCDIAG_G_ARB_INTRO],
        s_g[DC_NPCDIAG_G_ARB_DEMO1], s_g[DC_NPCDIAG_G_ARB_DEMO2],
        s_g[DC_NPCDIAG_G_ARB_HALLO],
        s_g[DC_NPCDIAG_G_GST_CALL], s_g[DC_NPCDIAG_G_GST_ARBEIT],
        s_g[DC_NPCDIAG_G_GST_BLKMAX], s_g[DC_NPCDIAG_G_GST_EXIST],
        s_g[DC_NPCDIAG_G_GST_EA], s_g[DC_NPCDIAG_G_GST_JEVT],
        s_g[DC_NPCDIAG_G_GST_SCOPE], s_g[DC_NPCDIAG_G_GST_APPEAR],
        s_g[DC_NPCDIAG_G_GST_UTNUM], s_g[DC_NPCDIAG_G_GST_MAKE],
        s_g[DC_NPCDIAG_G_MK_ENT], s_g[DC_NPCDIAG_G_MK_GATE],
        s_g[DC_NPCDIAG_G_MK_SLOT], s_g[DC_NPCDIAG_G_MK_IDX],
        s_g[DC_NPCDIAG_G_MK_CALLED], s_g[DC_NPCDIAG_G_MK_RET],
        s_g[DC_NPCDIAG_G_SU_ENT], s_g[DC_NPCDIAG_G_SU_CHK],
        s_g[DC_NPCDIAG_G_SU_ACTOR], s_g_oob);
}

#else /* DC_NPCDIAG == 0 */

/* Defined unconditionally, for the reason dc/src/dc_npcseed.c gives: a stale
 * dc/build/shrinksrc that still injects the calls then fails to LINK loudly
 * rather than being a confusing undefined reference — and with the calls not
 * injected (the rewriters are skipped at 0) --gc-sections drops every one of
 * these, so the image is byte-identical. */
int  dc_npcdiag_gate(int slot, int val) { (void)slot; return val; }
void dc_npcdiag_tick(int wade, int set_mode,
                     int next_bx, int next_bz, int now_bx, int now_bz,
                     unsigned int exist, unsigned int appear,
                     const void* npclist)
{
    (void)wade; (void)set_mode; (void)next_bx; (void)next_bz;
    (void)now_bx; (void)now_bz; (void)exist; (void)appear; (void)npclist;
}
void dc_npcdiag_mgr_ct(void) { }
void dc_npcdiag_ctrl_ct(int is_npc2, int clip_was_already_set)
{
    (void)is_npc2; (void)clip_was_already_set;
}
void dc_npcdiag_clip_set(int is_npc2) { (void)is_npc2; }
void dc_npcdiag_cloth_begin(void) { }
void dc_npcdiag_cloth_bank(int ok) { (void)ok; }
void dc_npcdiag_report(void) { }

#endif /* DC_NPCDIAG */
