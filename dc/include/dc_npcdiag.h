/* dc_npcdiag.h — N3: WHY IS NO VILLAGER ACTOR EVER CONSTRUCTED?
 * =============================================================================
 *
 * ONE INSTRUMENT, ONE RUN, FIVE HYPOTHESES. dc/src/dc_npcdiag.c is the
 * documentation; this header is the contract between it, dc/src/dc_vi.c (which
 * drives the periodic print) and tools/dcstub/make_src_shrink.py (which injects
 * the call sites into the DC_SRC_SHRINK scratch tree — `src/` is never edited).
 *
 * ⚠️ THE ENUM BELOW IS THE SINGLE SOURCE OF TRUTH FOR THE GATE SLOTS.
 * tools/dcstub/make_src_shrink.py PARSES THIS FILE at generation time and emits
 * the matching `#define`s into each rewritten TU (see _npcdiag_slots() there).
 * Do not restate the numbers anywhere else, and do not renumber casually: a
 * rewriter emitting a slot this header does not declare is a hard error in the
 * generator, and a slot number that drifts silently would relabel a counter
 * rather than break a build. Adding a row at the END is always safe.
 *
 * KNOBS (dc/Makefile)
 *   DC_NPCDIAG=0        default. Every function here is an empty definition,
 *                       no call site is injected, and --gc-sections drops the
 *                       lot: byte-identical image.
 *   DC_NPCDIAG=1        armed.
 *   DC_NPCDIAG_PERIOD=N presented frames between [DC/NPCDIAG] lines, default
 *                       300. Rounded UP to a multiple of 30 — dc_vi.c only
 *                       offers the instrument a call once per 30-frame window.
 */

#ifndef DC_NPCDIAG_H
#define DC_NPCDIAG_H

#ifdef __cplusplus
extern "C" {
#endif

/* --- The gate slots --------------------------------------------------------
 * Every one is a COUNT OF TRUE OUTCOMES, cumulative for the whole run — the
 * question this instrument answers is "did this ever happen", not "how often".
 * Read them top to bottom: the first row that is zero while the row above it is
 * non-zero IS the wall.
 *
 * ⚠️ The `_CALL` rows are entries, not outcomes: they are bumped with a
 * constant 1 so a gate whose predicate never even runs is distinguishable from
 * one that runs and always fails. */
enum {
    /* aSNMgr_set_npc_regular (src/actor/ac_set_npc_manager.c) — the five
     * serial gates the villagers have to walk through, in source order. */
    DC_NPCDIAG_G_REG_CALL = 0,  /* the pass ran at all                        */
    DC_NPCDIAG_G_REG_EXIST,     /* aSNMgr_chk_exist_and_appear == TRUE        */
    DC_NPCDIAG_G_REG_SCOPE,     /* aSNMgr_check_in_scope == TRUE              */
    DC_NPCDIAG_G_REG_APPEAR,    /* aSNMgr_set_appear_info_regular == TRUE     */
    DC_NPCDIAG_G_REG_UTNUM,     /* aSNMgr_get_safe_utnum == TRUE              */
    DC_NPCDIAG_G_REG_MAKE,      /* aSNMgr_set_make_npc != -1                  */

    /* aSNMgr_set_npc_guest — the pass that actually runs every tick once the
     * player has stopped changing acre (see aSNMgr_set_npc_regular's tail
     * switch). Its two outer gates count BLOCKED, not passed. */
    DC_NPCDIAG_G_GST_CALL,
    DC_NPCDIAG_G_GST_ARBEIT,    /* aSNMgr_chk_arbeit_and_demo_and_halloween   */
    DC_NPCDIAG_G_GST_BLKMAX,    /* aSNMgr_check_in_block_max                  */
    DC_NPCDIAG_G_GST_EXIST,
    DC_NPCDIAG_G_GST_SCOPE,
    DC_NPCDIAG_G_GST_APPEAR,    /* aSNMgr_set_appear_info_guest == TRUE       */
    DC_NPCDIAG_G_GST_UTNUM,
    DC_NPCDIAG_G_GST_MAKE,

    /* aSNMgr_make_npc — the make list drained into actual spawn calls. */
    DC_NPCDIAG_G_MK_ENT,        /* the function ran                           */
    DC_NPCDIAG_G_MK_GATE,       /* CLIP(npc_clip) && ->setupActor_proc        */
    DC_NPCDIAG_G_MK_SLOT,       /* a make slot held an NPC / SPNPC name       */
    DC_NPCDIAG_G_MK_IDX,        /* ...and its animal index was not -1         */
    DC_NPCDIAG_G_MK_CALLED,     /* setupActor_proc actually invoked           */
    DC_NPCDIAG_G_MK_RET,        /* ...and it returned TRUE                    */

    /* aNPC_setupActor_sub (src/actor/npc/ac_npc_ctrl.c_inc) — the last two
     * gates before an ACTOR exists. */
    DC_NPCDIAG_G_SU_ENT,
    DC_NPCDIAG_G_SU_CHK,        /* aNPC_setupNpc_check == TRUE (the cloth)    */
    DC_NPCDIAG_G_SU_ACTOR,      /* Actor_info_make_actor != NULL              */

    /* aSNMgr_check_safe_ut — the inside of REG_UTNUM / GST_UTNUM, split so a
     * unit rejected by the field data is distinguishable from one rejected by
     * the height-gap test. */
    DC_NPCDIAG_G_UT_CALL,
    DC_NPCDIAG_G_UT_COL,        /* col_p != NULL                              */
    DC_NPCDIAG_G_UT_FGCOL,      /* mNpc_CheckNpcSet_fgcol                     */
    DC_NPCDIAG_G_UT_HGAP,       /* ...and !mCoBG_ExistHeightGap_KeepAndNow    */

    /* aSNMgr_chk_exist_and_appear_and_event — the inside of GST_EXIST.
     * ⭐ ADDED 2026-08-13, and it is the whole point of the second N3 run.
     * The first run (kb/villagers-n3-result.md) found GST_EXIST at ZERO over
     * 12,048 guest calls while the REGULAR pass's plain chk_exist_and_appear
     * passed 28 times. The two differ by exactly one term, so the wrapper's
     * two halves fail for completely different reasons and the single counter
     * cannot say which:
     *   GST_EA   the inner aSNMgr_chk_exist_and_appear, called with
     *            mNpcW_APPEAR_STATUS_REGULAR -- zero here means an APPEAR-TYPE
     *            mismatch, nothing to do with events.
     *   GST_JEVT ((manager->npc_info.joint_event >> idx) & 1) == 0 -- zero here
     *            (with EA non-zero) means the villagers are flagged as
     *            joint-event participants and the guest pass is excluding them
     *            on purpose.
     * The two are wrapped as `EA && JEVT`, so JEVT is only evaluated when EA
     * passed: EA==0 alone is decisive, and EA>0 with JEVT==0 is decisive the
     * other way. */
    DC_NPCDIAG_G_GST_EA,
    DC_NPCDIAG_G_GST_JEVT,

    DC_NPCDIAG_G_NUM
};

/* THE ONE CALL THE VENDORED TUs MAKE. Returns `val` untouched, so it can be
 * wrapped around any predicate without changing a single branch:
 *     if (dc_npcdiag_gate(SLOT, <predicate>)) { ... }
 * A slot outside [0, DC_NPCDIAG_G_NUM) is counted as `oob=` on the report line
 * rather than scribbling — that is the tripwire for a rewriter/header drift the
 * generator's own parse somehow let through. */
int dc_npcdiag_gate(int slot, int val);

/* Per-logic-tick state, from aSNMgr_actor_move's head — BEFORE its wade switch,
 * so next_block/now_block are the values the last set pass actually used.
 * `npclist` is a `mNpc_NpcList_c*`; it is void* here so no consumer of this
 * header has to pull in m_npc.h. */
void dc_npcdiag_tick(int wade, int set_mode,
                     int next_bx, int next_bz, int now_bx, int now_bz,
                     unsigned int exist, unsigned int appear,
                     const void* npclist);

/* aSNMgr_actor_ct ran: the manager exists. Without this, every counter above
 * staying zero means nothing at all. */
void dc_npcdiag_mgr_ct(void);

/* aNPC_actor_ct_c ran (`is_npc2` from the aNPC_NPC2 build flavour), and
 * aNPC_keep_cloth_data_area has just finished, so the cloth tally is final.
 * Prints its one-shot immediately — this is a fact, not a rate. */
void dc_npcdiag_ctrl_ct(int is_npc2, int clip_was_already_set);

/* CLIP(npc_clip) was installed by this controller. One-shot. */
void dc_npcdiag_clip_set(int is_npc2);

/* aNPC_keep_cloth_data_area: begin() at the top, then one bank(ok) per slot. */
void dc_npcdiag_cloth_begin(void);
void dc_npcdiag_cloth_bank(int ok);

/* Driven from dc/src/dc_vi.c's 30-frame report window. Prints at most one line
 * per DC_NPCDIAG_PERIOD presented frames. */
void dc_npcdiag_report(void);

#ifdef __cplusplus
}
#endif

#endif /* DC_NPCDIAG_H */
