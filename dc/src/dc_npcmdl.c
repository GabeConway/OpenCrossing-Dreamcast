/* dc_npcmdl.c — the villager MODEL pool: 32 species' vertex arrays served out
 * of 16 resident slots, read off the disc, with their baked display-list
 * pointers relocated.  (R3; kill switch DC_NPCMDL_POOL=0.)
 *
 * WHAT IS BEING MOVED
 * -------------------
 * src/data/npc/model/mdl/ is 72 files, one `static Vtx <sp>_v[]` each,
 * 438,640 B, all .bss, all resident-or-absent.  The split, taken from
 * npc_draw_data_tbl[]'s own model_skeleton field (ac_npc.h:156) with
 * ALL_NPC_NUM = 238 as the boundary:
 *
 *     rows   0..237    32 distinct VILLAGER species    194,400 B   <- pooled
 *     rows 238..381    40 distinct SPECIAL  species    244,240 B   <- NOT pooled
 *
 * Measured 2026-08-05: those two sets DO NOT INTERSECT.  Not one skeleton is
 * named by both a villager row and a special row, which is why pooling by
 * skeleton pointer is structurally unable to touch Tom Nook, Rover, K.K.,
 * Porter or the raccoons.  The generator hard-errors if that ever changes.
 *
 * Before R3 exactly ONE of the 32 was resident — cbr_1, 5,536 B, because a
 * census run happened to see it — so 31 villager species drew as a black spiky
 * mess no matter how good the texture R2 painted on them (kb/traps.md: an
 * unkept asset loses its VERTICES).  R3 is therefore a CONTENT RESTORATION that
 * COSTS bytes, not a saving; the honest ledger is at the bottom of this comment.
 *
 * ⚠️ WHY THIS ONE NEEDS WORD PATCHING AND R2 DID NOT
 * ---------------------------------------------------
 * A villager texture reaches the RDP through an N64 segment register, bound per
 * draw out of a struct (ac_npc_draw.c_inc:269-278), so moving it is 16 pointer
 * writes and no relocation.  A vertex array does not: every one of the 2,068
 * `gsSPVertex(&<sp>_v[N], …)` in the tree is an R_SH_DIR32 relocation the
 * LINKER resolved into word 1 of an initialised Gfx, and
 *
 *     emu64::seg2k0 (emu64_utility.c:48-51)
 *         if ((segadr >> 28) != 0 || segadr < 0x03000000) return segadr;
 *
 * returns any address with a non-zero upper nibble unchanged.  A KOS .bss
 * address is 0x8Cxxxxxx, so it bypasses the segment table entirely: the word
 * sitting in .data IS the address the vertex fetch uses.  Move the array and
 * that word has to move with it.  933 of them across the 32 villager species —
 * min 21 (shp_1), median 29.5, max 37 (oct_1); 2,068 across all 72.
 *
 * The display lists live in .data (`static Gfx head_cat_model[] = {…}`), so
 * they are writable.  That is what makes this possible at all.
 *
 * HOW THE LISTS ARE REACHED — the skeleton is the only handle
 * -----------------------------------------------------------
 * ⚠️ `<sp>_v` and all of a species' `*_model[]` display lists are `static`, so
 * nothing in dc/ can name them.  The one externally visible symbol in the file
 * is the skeleton, and it is a complete path to every list:
 *
 *     extern cKF_Skeleton_R_c cKF_bs_r_cat_1   (mdl/cat_1.c:586)
 *       -> { u8 num_joints; u8 num_shown_joints; cKF_Joint_R_c* joint_table; }
 *                                              (c_keyframe.h:44-48)
 *       -> cKF_Joint_R_c { Gfx* model; u8 child; u8 flags; s_xyz translation; }
 *                                              (c_keyframe.h:37-42)
 *
 * VERIFIED on all 72 files, not on three: every one declares exactly one
 * skeleton with 26 joints, num_shown_joints (5..16) equals both the number of
 * distinct non-NULL `Gfx*` in its joint table AND the number of `static Gfx`
 * arrays in the file, and no list is named by two joints.  So joint index is a
 * unique, stable name for a display list.  It is also the name the DRAW uses —
 * cKF_Si3_draw_SV_R_child walks `keyframe->skeleton->joint_table[i].model` and
 * gSPDisplayList's it (c_keyframe.c:734, :788).
 *
 * ⚠️ WHY A GENERATED TABLE AND NOT A RUNTIME WALK OF THE DISPLAY LIST
 * -------------------------------------------------------------------
 * A walker looks cheaper — no generator work, no .rodata — and it is WRONG
 * here, because these lists contain words that have no opcode:
 *
 *     gsSPNTriangles_5b   (gbi_extensions.h:1190) packs three or four triangles'
 *                         5-bit vertex indices across w0 and w1.  The top byte
 *                         of w0 is bits 7..14 of the LAST triangle's index
 *                         triple — it reads as G_VTX (0x01) whenever v11 == 0
 *                         and v10 is in 4..7, which is ordinary geometry.
 *     emu64::dl_G_TRIN    (emu64.c:4802-4880) consumes those words BY COUNT,
 *                         from n_faces in the init word, 3 faces on the first
 *                         pass and 4 on each pass after — and switches rule on
 *                         a 5-bit/7-bit flag in w1.
 *
 * So a walker would have to reimplement that consumption rule exactly, and a
 * miss writes a slot address over a triangle packet.  That is corrupt geometry
 * on one limb of one animal, weeks from anything that points at this file.
 * 3,732 B of .rodata is the right price for never having that conversation.
 *
 * The table is computed offline from the macro expansions (see NPCMDL_GFX_WORDS
 * in tools/dcstub/make_stub_data.py — gsDPLoadTextureBlock_4b_Dolphin expands to
 * TWO Gfx, and there are 1,619 of those, so "one macro, one Gfx" would have been
 * wrong too).  AND it is re-checked at runtime: dc_npcmdl_probe() walks a
 * species' whole patch list before a single word is written and requires every
 * entry to name a Gfx whose opcode is G_VTX and whose current operand is the
 * same base plus the table's own offset.  933 independent agreements cannot
 * happen by accident, so a stale or miscounted table refuses to patch that
 * species and says so — the failure is the black spiky mess the species already
 * had, never another animal's geometry.
 *
 * NO EVICTION, SAME RULE AS R2 — AND FOR A DIFFERENT REASON
 * ----------------------------------------------------------
 * R2 never evicts because an actor takes a PRIVATE COPY of the texture pointers
 * (ac_npc_ct.c_inc:256).  The display lists are the opposite: they are GLOBAL
 * and shared by every actor of that species, so a slot reused under a live actor
 * puts another animal's vertices into this one's limbs immediately, for every
 * actor at once.  A species that cannot get a slot keeps its stubbed pointers
 * and draws as it does today.
 *
 * RESET only where no NPC actor can exist — the same two mNpc_SetNpcList() sites
 * R2 uses, m_start_data_init.c:559 and m_trademark.c:76.  The reset writes the
 * ORIGINAL base back into every word it patched (captured by the probe), so a
 * freed slot leaves the species pointing at its own one-byte stub array, not at
 * a slot the next generation will refill.
 *
 * WHAT IT COSTS ON THE DISC.  One species is ONE read of at most 7,552 B, at one
 * rom_off, byte-swapped as DC_STUB_SWAP_VTX — the vertex arrays are file-static,
 * so they have no s_assets[] row and the (rom_off, size, rom_src, swap) comes
 * from the generated per-file loader in the .c itself (mdl/cat_1.c:590).  A full
 * town is at most 16 reads moving <= 120,832 B, spread across the constructions
 * that happen as the player walks into acres.  If it shows up as a per-acre
 * hitch the fix is dc_keep_sweep()'s (dc_main.c:977-1108) — batch in
 * (rom_src, rom_off) order.  MEASURE FIRST: the number to A/B is the GD-ROM
 * command count, not wall clock.
 *
 * THE HONEST LEDGER — R3 SPENDS BYTES, IT DOES NOT SAVE THEM
 * -----------------------------------------------------------
 *     resident villager-model .bss before      5,536 B   (cbr_1, and only it)
 *     pool + bookkeeping                     120,960 B   (16 x 7,552 + ~128)
 *     generated .rodata                       ~4,600 B   (933 x 4 + 32 x 20 +
 *                                                         the 32 name strings)
 *     keep list given back                    -5,536 B   (cbr_1 removed)
 *     ------------------------------------------------------------------
 *     net                                   +115,424 B of .bss, +~4.6 KB
 *                                             .rodata
 *
 * and it buys 31 villager species geometry they did not have.  The comparison
 * that makes the POOL worth having is not against today, it is against the only
 * other way to get those 31: keeping all 32 files costs 194,400 B, so the pool
 * is 73,568 B cheaper than the content it delivers.  16 fixed max-sized slots
 * waste 15,248 B against the 16 largest species packed end to end (105,584 B);
 * a bump arena would recover that, at the price of a second failure axis
 * ("slots free but bytes exhausted"), and is the follow-up lever rather than
 * this change.  DC_NPCMDL_SLOTS is the knob to cut FIRST — 32 models serve 236
 * texture sets, so 16 villagers collide often and the pool rarely fills.
 */
#include "dc_platform.h"

/* The two row shapes the generated map is written against.  tools/dcstub/
 * make_stub_data.py emits the .inc; these typedefs are the other end of that
 * contract, so change both or neither.
 *
 * The widths are not decoration — the generator hard-errors if the tree ever
 * overflows one, because a truncated field is a write into the wrong word.
 * Measured worst cases: joint 25, gfx index 90, offset 7,328. */
typedef struct {
    unsigned short off;    /* byte offset into <sp>_v of this gsSPVertex   */
    unsigned char  gfx;    /* Gfx index within the joint's display list    */
    unsigned char  joint;  /* index into cKF_Skeleton_R_c::joint_table     */
} dc_npcmdl_patch_t;

typedef struct {
    unsigned int   rom_off;
    const char*    name;      /* "cat_1" — diagnostic only                 */
    const void*    skeleton;  /* &cKF_bs_r_<sp> — THE LOOKUP KEY           */
    unsigned short size;      /* bytes of <sp>_v                           */
    unsigned short patch_off; /* first entry in dc_npcmdl_patch[]          */
    unsigned char  patch_n;
    unsigned char  rom_src;
    unsigned char  kept;      /* on DC_STUB_KEEP: resident already, hands off */
} dc_npcmdl_t;

#ifdef DC_ASSET_STUB
#include "dc/build/stubsrc/dc_npcmdl_map.inc"
#else
/* No stub tree, so no [1]-sized arrays: every vertex array is full size and was
 * filled by pc_assets_init(), and every baked gsSPVertex word is already right.
 * There is nothing to pool and nothing to relocate. */
#define DC_NPCMDL_SPECIES 0
#endif

#if DC_NPCMDL_SPECIES > 0

/* dc_main.c, and only compiled there under DC_ASSET_STUB — which is exactly the
 * condition the map is emitted under.  Declared rather than included so this TU
 * does not pull the whole keep-list header in. */
extern void dc_stub_keep_load_one(const char* bin_path, void* dest,
                                  unsigned int size, unsigned int rom_off,
                                  int rom_src, int swap);

/* npc_draw_data_tbl[] and the skeleton/joint structs are reached as raw bytes,
 * for the same reason dc_npctex.c does it: this TU must not include ac_npc.h or
 * c_keyframe.h (between them they drag in the actor system, game.h and m_lib.h,
 * and dc/src builds at a different -O and language level).  Every literal below
 * is pinned by a compile-time assert in the DC_SRC_SHRINK copy of
 * ac_npc_ctrl.c_inc, which does have the headers.  If the vendored structs move,
 * that TU fails to compile; it cannot drift quietly. */
extern unsigned char npc_draw_data_tbl[];
#define DC_NPCMDL_ROW_STRIDE    0x6Cu  /* sizeof(aNPC_draw_data_c)           */
#define DC_NPCMDL_ROW_SKEL_OFF  0x04u  /* offsetof(…, model_skeleton)        */

#define DC_NPCMDL_SKEL_NJOINTS  0u     /* cKF_Skeleton_R_c::num_joints       */
#define DC_NPCMDL_SKEL_TABLE    4u     /* cKF_Skeleton_R_c::joint_table      */
#define DC_NPCMDL_JOINT_STRIDE  12u    /* sizeof(cKF_Joint_R_c)              */
#define DC_NPCMDL_JOINT_MODEL   0u     /* cKF_Joint_R_c::model               */

/* "It is, by law, exactly 64 bits in size" — gbi.h:1863.  w0 carries the
 * opcode in its top byte, w1 the address operand (Gdma::addr, gbi.h:1506). */
#define DC_NPCMDL_GFX_STRIDE    8u
#define DC_NPCMDL_GFX_W1        4u

/* Same ceiling and the same reasoning as DC_NPCTEX_SLOTS (dc_npctex.c:181):
 * ANIMAL_NUM_MAX (m_npc.h:28) + mISL_ISLANDER_NUM (m_island.h:16).  Matching it
 * is deliberate — a species that got a texture slot must be able to get a model
 * slot, or R2 paints a correct skin onto a black spiky mess and the result reads
 * as a renderer bug.
 *
 * It is looser here than there: 16 villagers can name at most 16 species but
 * only 32 distinct MODELS exist for 236 texture sets, so collisions are common
 * and the pool usually holds far fewer.  A build knob rather than a literal so
 * cutting it costs one -D; each slot is DC_NPCMDL_SLOT_BYTES of .bss. */
#ifndef DC_NPCMDL_SLOTS
#define DC_NPCMDL_SLOTS 16
#endif

static unsigned char s_pool[DC_NPCMDL_SLOTS][DC_NPCMDL_SLOT_BYTES]
    __attribute__((aligned(32)));

static short s_slot_sp[DC_NPCMDL_SLOTS];        /* species index, -1 = free   */
static unsigned int s_slot_base[DC_NPCMDL_SLOTS]; /* the STUB base, to undo   */
static int   s_slots_used;
static int   s_words_patched;
static unsigned int s_bytes_loaded;
static unsigned int s_overflow;                 /* requests that got nothing  */
static unsigned int s_refused;                  /* probes that said no        */
static unsigned int s_generation;
static int   s_ready;

/* s_slot_sp[] has to start at -1, not at .bss's 0, or every slot would claim to
 * hold species 0 (ant_1).  Lazy rather than an init hook or a
 * __attribute__((constructor)): this TU then has no boot-order dependency and no
 * dependency on KOS running C constructors at all. */
static void dc_npcmdl_arm(void) {
    int i;

    if (s_ready) {
        return;
    }
    for (i = 0; i < DC_NPCMDL_SLOTS; i++) {
        s_slot_sp[i] = -1;
    }
    s_ready = 1;
}

/* Powers of two only.  Overflow is per CONSTRUCTION, so a town one species over
 * budget would print on every villager spawn for the rest of the run and the log
 * would become the flood.  Call with the POST-increment count. */
static int dc_npcmdl_say(unsigned int n) {
    return (n & (n - 1u)) == 0u;
}

/* The Gfx one patch entry names, or NULL if the skeleton does not lead there.
 * Every step is bounds-checked: this walks vendored .data through offsets this
 * TU cannot see the types of, and a joint table that has moved must produce a
 * refusal rather than a wild write. */
static unsigned char* dc_npcmdl_gfx(const dc_npcmdl_t* m,
                                    const dc_npcmdl_patch_t* p) {
    const unsigned char* skel = (const unsigned char*)m->skeleton;
    unsigned char* jt;
    unsigned char* list;

    if (p->joint >= skel[DC_NPCMDL_SKEL_NJOINTS]) {
        return NULL;
    }
    jt = *(unsigned char* const*)(const void*)(skel + DC_NPCMDL_SKEL_TABLE);
    if (jt == NULL) {
        return NULL;
    }
    list = *(unsigned char* const*)(const void*)(jt
                                                 + (unsigned int)p->joint
                                                   * DC_NPCMDL_JOINT_STRIDE
                                                 + DC_NPCMDL_JOINT_MODEL);
    if (list == NULL) {
        return NULL;
    }
    return list + (unsigned int)p->gfx * DC_NPCMDL_GFX_STRIDE;
}

/* Read-only dry run over a species' whole patch list.  Returns 1 and the base
 * every operand currently shares; 0 if ANY entry disagrees.
 *
 * This is the check that makes the generated table safe to trust.  It asks two
 * things of each of the 17..37 entries: that the word it names is really a G_VTX
 * command, and that its current operand is exactly one shared base plus the
 * offset the table claims.  A table that is stale, short, or shifted by a
 * miscounted macro fails both, on the first entry that moved.  Nothing is
 * written until it has passed in full, so a refusal leaves the species exactly
 * as it was. */
static int dc_npcmdl_probe(const dc_npcmdl_t* m, unsigned int* base_out) {
    const dc_npcmdl_patch_t* p = &dc_npcmdl_patch[m->patch_off];
    unsigned int base = 0;
    int i;

    if (m->patch_n == 0) {
        /* Cannot happen — the generator's measured minimum is 17 and it
         * hard-errors on a species with none — and if it did, `base` would be
         * meaningless and the reset would write it back everywhere. */
        return 0;
    }
    for (i = 0; i < (int)m->patch_n; i++) {
        const unsigned char* g = dc_npcmdl_gfx(m, &p[i]);
        unsigned int w0;
        unsigned int w1;

        if (g == NULL) {
            return 0;
        }
        w0 = *(const unsigned int*)(const void*)g;
        w1 = *(const unsigned int*)(const void*)(g + DC_NPCMDL_GFX_W1);
        if ((w0 >> 24) != (unsigned int)DC_NPCMDL_G_VTX) {
            return 0;
        }
        if (i == 0) {
            base = w1 - p[i].off;
        } else if (w1 - p[i].off != base) {
            return 0;
        }
    }
    *base_out = base;
    return 1;
}

/* Point every gsSPVertex of one species at `base`.  Only ever called after a
 * dc_npcmdl_probe() that returned 1 for the same species — the NULL test is
 * kept anyway because this writes into vendored .data through offsets this TU
 * cannot type-check, and the reset path runs it a second time much later. */
static void dc_npcmdl_apply(const dc_npcmdl_t* m, unsigned int base) {
    const dc_npcmdl_patch_t* p = &dc_npcmdl_patch[m->patch_off];
    int i;

    for (i = 0; i < (int)m->patch_n; i++) {
        unsigned char* g = dc_npcmdl_gfx(m, &p[i]);

        if (g == NULL) {
            continue;
        }
        *(unsigned int*)(void*)(g + DC_NPCMDL_GFX_W1) = base + p[i].off;
        s_words_patched++;
    }
}

/* Called from aNPC_dma_draw_data_proc (the DC_SRC_SHRINK copy of
 * ac_npc_ctrl.c_inc), alongside dc_npctex_ensure() and one statement before the
 * row is mem_copy'd into the actor.  `row` is aNPC_get_draw_data_idx()'s result
 * and may be -1.
 *
 * The seam matters less here than it does for R2 — the display lists are global,
 * so a patch applied at any point before the DRAW would be seen — but the two
 * halves of a villager must arrive together, and this is where the species is
 * named. */
void dc_npcmdl_ensure(int row) {
    const dc_npcmdl_t* m;
    const void* skel;
    unsigned int base;
    int idx = -1;
    int free_slot = -1;
    int i;

    /* -1 is aNPC_get_draw_data_idx()'s "neither NPC nor SPNPC"
     * (ac_npc_ctrl.c_inc:154), and >= DC_NPCMDL_ROWS is a SPECIAL NPC.  The
     * skeleton lookup below would already refuse those — the 40 special species
     * have no map row — but this is the cheap, explicit half of the same
     * promise, and it holds even if the vendored table drifts. */
    if (row < 0 || row >= DC_NPCMDL_ROWS) {
        return;
    }
    dc_npcmdl_arm();

    /* npc_draw_data_tbl[row].model_skeleton — the exact field aNPC_actor_ct()
     * is about to read out of its snapshot and hand to cKF_SkeletonInfo_R_ct
     * (ac_npc_ct.c_inc:274-280).  The row -> model mapping is not invented
     * here; this IS the game's. */
    skel = *(const void* const*)(const void*)(npc_draw_data_tbl
                                              + (unsigned int)row
                                                * DC_NPCMDL_ROW_STRIDE
                                              + DC_NPCMDL_ROW_SKEL_OFF);
    if (skel == NULL) {
        return;
    }

    /* Linear scan: 32 rows, once per NPC construction.  A sorted table plus a
     * bsearch would save ~27 pointer compares against a disc read that costs
     * five orders of magnitude more. */
    for (i = 0; i < DC_NPCMDL_SPECIES; i++) {
        if (dc_npcmdl_species[i].skeleton == skel) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        return;
    }
    m = &dc_npcmdl_species[idx];

    /* A species that is on DC_STUB_KEEP is full size and was already filled by
     * dc_stub_keep_load().  Its baked gsSPVertex words are correct as linked;
     * moving it into a slot would work but would burn one, and un-patching it on
     * the next reset would be pointing a REAL model at its own real array —
     * harmless but pointless.  Leave it alone. */
    if (m->kept) {
        return;
    }

    for (i = 0; i < DC_NPCMDL_SLOTS; i++) {
        if (s_slot_sp[i] == (short)idx) {
            /* Resident and already patched.  Another row of the same species —
             * common, since 236 texture sets share 32 models — or a respawn of
             * one already served.  No I/O and no second patch. */
            return;
        }
        if (s_slot_sp[i] < 0 && free_slot < 0) {
            free_slot = i;
        }
    }

    if (free_slot < 0) {
        /* NO EVICTION, EVER.  These display lists are GLOBAL: reusing a slot
         * would put this species' vertices into every live actor of the species
         * that had it, mid-frame, with nothing to see in the log.  This villager
         * draws as an absent asset instead — which is what it did before R3 —
         * and says so. */
        s_overflow++;
        if (dc_npcmdl_say(s_overflow)) {
            DC_LOGE("[DC/NPCMDL] POOL FULL (%d slots) — %s keeps its stub "
                    "geometry. A town holds ANIMAL_NUM_MAX=15 villagers + 1 "
                    "islander and only %d distinct models exist, so a 17th "
                    "distinct species means an event/mask NPC borrowed a "
                    "villager row. Raise it with -DDC_NPCMDL_SLOTS=%d "
                    "(+%d B .bss)\n",
                    DC_NPCMDL_SLOTS, m->name, DC_NPCMDL_SPECIES,
                    DC_NPCMDL_SLOTS + 1, (int)DC_NPCMDL_SLOT_BYTES);
        }
        return;
    }

    /* PROBE BEFORE ANYTHING IS WRITTEN — see the header.  A refusal costs this
     * species its geometry and costs nothing else; a wrong patch costs a limb of
     * a different animal and a week of looking at dc_pvr.c. */
    if (!dc_npcmdl_probe(m, &base)) {
        s_refused++;
        if (dc_npcmdl_say(s_refused)) {
            DC_LOGE("[DC/NPCMDL] REFUSING %s: its %d patch entries do not agree "
                    "with the live display lists. dc/build/stubsrc/"
                    "dc_npcmdl_map.inc is stale or miscounted against "
                    "src/data/npc/model/mdl/%s.c — re-run "
                    "tools/dcstub/make_stub_data.py. The species keeps its stub "
                    "geometry; nothing was written\n",
                    m->name, (int)m->patch_n, m->name);
        }
        return;
    }

    /* ONE read: the array is contiguous, one rom_src, and swapped whole as
     * DC_STUB_SWAP_VTX (dc_main.c:745-746).  It is file-static, so this tuple
     * came from the .c's own generated loader, not from s_assets[]. */
    dc_stub_keep_load_one(m->name, s_pool[free_slot], (unsigned int)m->size,
                          m->rom_off, (int)m->rom_src, DC_NPCMDL_SWAP_VTX);

    s_slot_sp[free_slot] = (short)idx;
    s_slot_base[free_slot] = base;
    s_slots_used++;
    s_bytes_loaded += m->size;

    dc_npcmdl_apply(m, (unsigned int)(uintptr_t)s_pool[free_slot]);

    DC_LOG("[DC/NPCMDL] %s -> slot %d (%u B, %d words) | %d/%d slots, %u B, "
           "%d words\n",
           m->name, free_slot, (unsigned int)m->size, (int)m->patch_n,
           s_slots_used, DC_NPCMDL_SLOTS, s_bytes_loaded, s_words_patched);
}

/* Called from the two mNpc_SetNpcList() sites that rebuild the whole npclist.
 * ⚠️ ONLY from those two.  It frees every slot, and it is correct only because
 * no NPC actor exists at either point — see dc_npctex.c's header, which carries
 * the full argument for both pools. */
void dc_npcmdl_pool_reset(void) {
    int i;

    dc_npcmdl_arm();

    if (s_slots_used || s_overflow || s_refused) {
        DC_LOGE("[DC/NPCMDL] generation %u: %d/%d slot(s), %u B off the disc, "
                "%d word(s) patched, %u overflow(s), %u refusal(s)\n",
                s_generation, s_slots_used, DC_NPCMDL_SLOTS, s_bytes_loaded,
                s_words_patched, s_overflow, s_refused);
    }

    /* Put every patched word back to the base the probe found — the species'
     * own one-byte stub array.  Leaving them aimed at a slot the next generation
     * will refill is the wrong-geometry failure this file exists to prevent;
     * the stub base reads as a missing asset, which is a failure this project
     * already knows how to see (kb/traps.md). */
    for (i = 0; i < DC_NPCMDL_SLOTS; i++) {
        if (s_slot_sp[i] >= 0) {
            dc_npcmdl_apply(&dc_npcmdl_species[s_slot_sp[i]], s_slot_base[i]);
            s_slot_sp[i] = -1;
        }
    }
    s_slots_used = 0;
    s_words_patched = 0;
    s_bytes_loaded = 0;
    s_overflow = 0;
    s_refused = 0;
    s_generation++;
}

#else /* DC_NPCMDL_SPECIES == 0 */

/* R3 is off — either DC_NPCMDL_POOL=0 or this is a non-stub build.  Either way
 * the vertex arrays that exist are full size and their baked gsSPVertex words
 * are correct as the linker resolved them, so there is nothing to move and
 * nothing to relocate. */
void dc_npcmdl_ensure(int row) { (void)row; }
void dc_npcmdl_pool_reset(void) { }

#endif /* DC_NPCMDL_SPECIES */
