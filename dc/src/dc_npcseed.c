/* dc_npcseed.c — N2c: PUT VILLAGERS IN THE TOWN WITHOUT A SAVE FILE.
 * =============================================================================
 *
 * WHAT THIS REPLACES, AND WHY THE OLD PLAN WAS AIMED AT THE WRONG BUG.
 *
 * kb/STATE.md action A and kb/RESUME.md §7 item 1 both said: "the town has no
 * villagers, and it is a SAVE bug — mNpc_SetNpcList populates the town from the
 * save's Animal_c animals[], the VMU path is unwired, so [PC] No save file
 * found and not one villager actor is ever constructed." The prescribed fix was
 * N2b, wire the VMU save path.
 *
 * 🔴 THAT CAUSAL CHAIN IS WRONG, and it is wrong in a way that would have made
 * N2b a lot of work for nothing. `animals[]` IS NOT LOADED FROM THE SAVE ON A
 * NEW GAME — IT IS GENERATED. The path is:
 *
 *   mSDI_StartInitNew (m_start_data_init.c:288 — the VERSION=0 copy, :176 is
 *                      compiled out)
 *     -> mNpc_InitNpcAllInfo        (m_npc.c:4161)
 *          Save_Set(now_npc_max, mNpc_LOOKS_NUM), which is 6
 *          mNpc_ClearAnyAnimalInfo(animals, 15)
 *          mNpc_DecideLivingNpcMax  (m_npc.c:2422)  <- picks 6 STARTERS,
 *                                                      one per personality
 *
 * and mSDI_StartInitNew carries NO mFRm_CheckSaveData() gate — only
 * StartInitFrom/NewPlayer/Pak do. So a boot with no save still generates six
 * villagers with valid `id.npc_id`.
 *
 * WHAT ACTUALLY KEEPS THEM OUT OF THE TOWN is one line, mNpc_SetNpcList
 * (m_npc.c:2807):
 *
 *     if (ITEM_NAME_GET_TYPE(npc_id) == NAME_TYPE_NPC &&
 *         animal->home_info.block_x != 0xFF) {
 *
 * `home_info` is left at 0xFF by mNpc_ClearAnimalInfo and is written ONLY by
 * mNpc_SetNpcHome (m_npc.c:2712), which is fed by
 * mNpc_MakeReservedListBeforeFieldct (:2549) — a scan of Save_Get(fg[][]) for
 * SIGN00..SIGN20 markers (mNT_IS_RESERVE, m_name_table.h:4106). If that scan
 * finds nothing, `reserved_num == 0`, mNpc_SetNpcHome returns without writing a
 * byte, all 15 block_x stay 0xFF, and mNpc_SetNpcList skips every slot WITH NO
 * DIAGNOSTIC. The markers come from RESOURCE_FGDATA via
 * mFM_InitFgCombiSaveData (m_field_make.c:1490), i.e. ARAM — nothing to do with
 * the VMU.
 *
 * ⚠️ SO THIS FILE IS ALSO THE INSTRUMENT THAT SETTLES IT. It logs the state it
 * found BEFORE it changes anything (`pre ids= homed= max=`), which distinguishes
 * the two hypotheses on the first run:
 *
 *     pre ids=0  homed=0   the generator never ran   -> a start-path bug
 *     pre ids=6  homed=0   generator ran, HOMES failed -> the FGDATA/reserve
 *                                                        scan, as argued above
 *     pre ids=6  homed>0   neither — something downstream eats the actors
 *
 * DC_NPC_SEED=2 logs and repairs NOTHING, which is the honest way to read that
 * line without this file's own writes contaminating it.
 *
 * WHAT IT DOES. It repairs `animals[]` in place immediately before
 * mNpc_SetNpcList, per slot, and only where the slot is actually broken:
 *
 *   - a slot whose id is not NAME_TYPE_NPC gets mNpc_SetDefAnimal() (extern,
 *     m_npc.c:2317) against our own roster, which fills id.npc_id, id.looks
 *     (from npc_looks_table), cloth, umbrella_id, catchphrase, land_id and
 *     land_name exactly as the game's own generator does;
 *   - a slot whose home_info.block_x is 0xFF gets a home from the roster.
 *
 * A slot the game filled correctly is LEFT ALONE. That matters: the day the
 * FGDATA reserve scan starts working, this file quietly stops doing anything
 * except the logging, and `pre homed=` says so.
 *
 * THE ROSTER IS THE GAME'S OWN. The 14 rows below are demo_npc_list
 * (m_trademark.c:51-67) verbatim — the table mNpc_SetAnimalTitleDemo already
 * feeds to mNpc_SetDefAnimal for the title demo. Using it is not a guess about
 * legal coordinates: block_x in [1,5], block_z in [1,6] is the 1-based FG grid
 * (FG_BLOCK_X_NUM 5, FG_BLOCK_Z_NUM 6, m_field_make.h:17-38) and ut_x/ut_z are
 * the 16x16 unit grid inside an acre; mNpc_SetNpcList turns them into
 * `block * 640.0f + ut * 40.0f` and mFI_UtNum2PosXZInBk (m_field_info.c:467)
 * does no bounds check of its own. The four acres that hold two villagers each
 * keep them >= 3 units apart, so the 3x3 house footprints do not collide.
 *
 * ⚠️ WHAT THIS DOES NOT DO, AND IT IS VISIBLE. mNpc_SetAnimalTitleDemo does not
 * call mNpc_BuildHouseBeforeFieldct either, and neither do we — that function is
 * `static` in m_npc.c, which is in NO rewrite set, so it is unreachable from
 * dc/src. The villagers therefore exist, have world positions, are in
 * Common_Get(npclist), and WALK; their HOUSES are not stamped into
 * Save_Get(fg[][]) and so do not exist as field geometry. Villagers first,
 * houses later — and the house half is exactly what a working FGDATA reserve
 * scan would restore for free.
 *
 * ⚠️ AND IT IS USELESS WITHOUT R2/R3. A villager's textures and skeleton are
 * stubbed unless DC_NPCTEX_POOL=1 and DC_NPCMDL_POOL=1, and measurement rule 8
 * says the stubbed classes are 90,464 B and 5,536 B resident against 1,154,944
 * and 438,640 on paper. Seeding a roster into a build without the pools puts
 * correctly-positioned villagers in the town wearing nothing. The three knobs
 * are meant to be turned on together; this file does not enforce that, because
 * a seed-only build is a legitimate experiment (it isolates "does an actor get
 * constructed" from "does it have art").
 *
 * KNOBS
 *   DC_NPC_SEED=0   default. This file compiles to an empty function and the
 *                   rewriter injects no call. Byte-identical to the old build.
 *   DC_NPC_SEED=1   repair + log.
 *   DC_NPC_SEED=2   LOG ONLY, repair nothing. The diagnosis run.
 *   DC_NPC_SEED_MAX the number of roster slots to fill, 1..14, default 14. Also
 *                   what now_npc_max is raised to, which is what decides how
 *                   many of them WALK: mNpcW_GET_WALK_NUM is now_npc_max / 3
 *                   (m_npc_walk.h:12), and mNpcW_InitNpcWalk reads it at
 *                   m_start_data_init.c:578 — after our seam, so no extra hook
 *                   is needed. 14 -> 4 walkers. Turn it DOWN if villagers cost
 *                   more frame than they are worth; that is a real dial, not a
 *                   safety valve.
 *
 * SEAM. tools/dcstub/make_src_shrink.py injects `dc_npc_seed();` immediately
 * before the mNpc_SetNpcList call in mSDI_StartInitAfter, into the
 * DC_SRC_SHRINK scratch tree — the same anchor and the same entry that already
 * carries R2/R3's pool resets. src/ itself is not edited.
 */

#include "dc_platform.h"

void dc_npc_seed(void);

#if defined(DC_NPC_SEED) && (DC_NPC_SEED) > 0

/* The game's own headers. dc_emu64_cull.cpp already proves the include path
 * reaches include/ from a dc/src TU; these are plain C headers with no class
 * layout games, so they need none of that file's #define private public. */
#include "m_common_data.h"
#include "m_npc.h"
#include "m_name_table.h"

/* npc_def_list has NO header declaration anywhere in the tree — m_npc.c:21
 * declares it locally and src/data/npc/default_list.c:4 defines it. Restated
 * here for the same reason, and it is NOT in stub.list, so it is always
 * resident and always real. */
extern mNpc_Default_Data_c npc_def_list[];

#ifndef DC_NPC_SEED_MAX
#define DC_NPC_SEED_MAX 14
#endif

/* demo_npc_list (m_trademark.c:51-67), minus the trailing EMPTY_NO padding row
 * that only exists to make the array ANIMAL_NUM_MAX long. `u16` for the name
 * because mActor_name_t is u16 and these are all 0xE0xx. */
typedef struct {
    u16 name;
    u8  block_x, block_z, ut_x, ut_z;
} dc_npc_seed_row;

static const dc_npc_seed_row s_roster[] = {
    { NPC_BOB,    1, 2,  3,  7 },
    { NPC_PAOLO,  1, 2,  8, 11 },
    { NPC_VESTA,  1, 4, 12, 11 },
    { NPC_JOEY,   2, 3,  5,  6 },
    { NPC_LOBO,   2, 3,  4, 12 },
    { NPC_CARRIE, 3, 5, 11,  5 },
    { NPC_TANK,   4, 3,  3, 12 },
    { NPC_BUZZ,   4, 4,  3,  4 },
    { NPC_RASHER, 4, 4, 12, 13 },
    { NPC_BIFF,   4, 6,  5,  6 },
    { NPC_SAMSON, 5, 2, 12,  4 },
    { NPC_JANE,   5, 2,  9, 11 },
    { NPC_TYBALT, 5, 4, 11,  4 },
    { NPC_CUBE,   5, 5,  5, 11 },
};
#define DC_NPC_ROSTER_N ((int)(sizeof(s_roster) / sizeof(s_roster[0])))

void dc_npc_seed(void)
{
    Animal_c *animals = Save_GetPointer(animals[0]);
    int want = DC_NPC_SEED_MAX;
    int pre_ids = 0, pre_homed = 0;
    int made_id = 0, made_home = 0;
    int i;

    if (want > DC_NPC_ROSTER_N) want = DC_NPC_ROSTER_N;
    if (want > ANIMAL_NUM_MAX)  want = ANIMAL_NUM_MAX;
    if (want < 1)               want = 1;

    /* THE MEASUREMENT FIRST, BEFORE A SINGLE WRITE. See the header comment: the
     * `pre` pair is the whole diagnosis, and it is worthless if this function
     * has already run. Both counters walk all 15 slots, not `want`, because
     * "the generator produced 6" and "we asked for 14" are different numbers
     * and conflating them would hide the interesting case. */
    for (i = 0; i < ANIMAL_NUM_MAX; i++) {
        if (ITEM_NAME_GET_TYPE(animals[i].id.npc_id) == NAME_TYPE_NPC) {
            pre_ids++;
            if (animals[i].home_info.block_x != 0xFF) pre_homed++;
        }
    }

#if (DC_NPC_SEED) >= 2
    DC_LOGE("[DC/NPCSEED] OBSERVE-ONLY (DC_NPC_SEED=2): pre ids=%d homed=%d "
            "max=%u -- nothing written\n",
            pre_ids, pre_homed, (unsigned)Save_Get(now_npc_max));
    (void)want; (void)made_id; (void)made_home;
    return;
#else

    for (i = 0; i < want; i++) {
        Animal_c *a = &animals[i];

        /* An id the game already chose is KEPT. Its species is a real
         * mNpc_DecideLivingNpcMax pick — one per personality, drawn from
         * npc_grow_list's STARTER set — and that is a better roster than ours.
         * We only ever supply what is missing. */
        if (ITEM_NAME_GET_TYPE(a->id.npc_id) != NAME_TYPE_NPC) {
            /* mNpc_SetDefAnimal (m_npc.c:2317, extern, m_npc.h:390) takes the
             * BASE of the table and indexes it with `npc_id & 0xFFF` itself.
             * It re-checks NAME_TYPE_NPC, so a bad row is a no-op rather than
             * an out-of-bounds read of npc_looks_table[]. It also reaches ARAM
             * through mString_Load_StringFromRom for the catchphrase — that is
             * I/O, not a memcpy, and it is why this loop is bounded by `want`
             * rather than run over all 15 unconditionally. */
            mNpc_SetDefAnimal(a, (mActor_name_t)s_roster[i].name, npc_def_list);
            /* mNpc_SetNpcNameID (m_npc.c:3762) is static, and the villager's
             * display name is DMAd from ARAM keyed on this byte. Three lines
             * of it, open-coded, exactly as m_npc.c:3762 writes it. */
            a->id.name_id = (u8)a->id.npc_id;
            made_id++;
        }

        /* ⚠️ THE GATE ITSELF. mNpc_SetNpcList tests block_x ALONE, so this is
         * the one field that decides whether the villager exists — but write
         * all four, because block_z/ut_x/ut_z are what turn into the world
         * position two lines later and a half-filled home puts a villager at
         * `0xFF * 40.0f`. mNpc_CheckFreeAnimalInfo (m_npc.c:888) also reads
         * block_x AND block_z, with &&, so a half-filled slot would read as
         * occupied to some callers and free to others. */
        if (a->home_info.block_x == 0xFF) {
            a->home_info.type_unused = 0;
            a->home_info.block_x = s_roster[i].block_x;
            a->home_info.block_z = s_roster[i].block_z;
            a->home_info.ut_x    = s_roster[i].ut_x;
            a->home_info.ut_z    = s_roster[i].ut_z;
            made_home++;
        }
    }

    /* now_npc_max is what mNpcW_GET_WALK_NUM divides by 3, and it is clamped to
     * [ANIMAL_NUM_MIN, ANIMAL_NUM_MAX] = [5, 15] by the game's own mutators
     * (m_npc.c:184/:190). Raise it, never lower it: mNpc_InitNpcAllInfo set it
     * to 6 and something else may already have grown it. */
    if ((int)Save_Get(now_npc_max) < want) {
        Save_Set(now_npc_max, (u8)want);
    }

    DC_LOGE("[DC/NPCSEED] pre ids=%d homed=%d | seeded id=%d home=%d | "
            "want=%d max=%u\n",
            pre_ids, pre_homed, made_id, made_home,
            want, (unsigned)Save_Get(now_npc_max));
#endif /* DC_NPC_SEED >= 2 */
}

#else  /* DC_NPC_SEED == 0 */

/* Defined unconditionally so the symbol exists whatever the rewriter did. The
 * rewriter does not inject a call at 0, so this is dead code the linker drops
 * with -ffunction-sections; keeping it makes a stale scratch tree a link
 * success rather than a confusing undefined reference. */
void dc_npc_seed(void) { }

#endif
