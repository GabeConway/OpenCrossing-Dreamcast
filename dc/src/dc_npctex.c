/* dc_npctex.c — the villager texture pool: 236 species' texture sets served out
 * of 16 resident slots, read off the disc.  (R2; kill switch DC_NPCTEX_POOL=0.)
 *
 * WHAT IS BEING MOVED
 * -------------------
 * src/data/npc/model/tex/ is 1,154,944 B across 276 sets, all .bss, all
 * resident-or-absent.  The split, taken from npc_draw_data_tbl[]'s own row
 * order (ALL_NPC_NUM = 238 is the boundary, ac_npc_ctrl.c_inc:154-187 is the
 * mapping):
 *
 *     rows   0..237   236 distinct VILLAGER sets     993,984 B   <- pooled
 *     rows 238..381    40 distinct SPECIAL sets      160,960 B   <- NOT pooled
 *
 * A town holds at most 15 villagers (ANIMAL_NUM_MAX, m_npc.h:28) plus one
 * islander (mISL_ISLANDER_NUM, m_island.h:16), so a given save can want at most
 * 16 of those 236 sets and the other 220 rows are storage it can never draw.
 * The keep list was paying 90,464 B for the 21 sets one census run happened to
 * see and declining the rest, which is why 215 of 236 villager species render
 * with no texture at all.
 *
 * The pool is a fixed 16 x 4,832 B static array = 77,312 B — NOT a heap
 * allocation.  With the zero block and the bookkeeping it is 78,872 B of .bss,
 * against 90,464 B of keep list removed and ~6.9 KB of generated .rodata: R2 is
 * roughly break-even on bytes and buys 215 species that had no texture at all.
 * It lands in .bss on purpose, so the additive-heap ceiling
 * (kb/heap-two-pools.md) never enters the arithmetic; putting it on the heap
 * would put it in libc's pool, which is the pool that actually OOMs
 * (kb/RESUME.md rule 6).
 *
 * ⚠️ IT IS THE TEXTURE HALF ONLY, and the other half is now R3.
 * src/data/npc/model/mdl/ is a separate 438,640 B problem — an unkept model
 * loses its Vtx array and draws as a black spiky mess no matter how good its
 * texture is — and dc/src/dc_npcmdl.c pools the 32 villager species of it out
 * of 16 slots of its own.  The two are deliberately coupled: DC_NPCMDL_SLOTS
 * defaults to the same 16 as DC_NPCTEX_SLOTS so a species that gets a texture
 * slot can always get a model slot.  R3 needs word patching and this file does
 * not — see its header for why.
 *
 * WHY VILLAGER TEXTURES ARE THE CHEAPEST POOL TARGET IN THE TREE
 * --------------------------------------------------------------
 * They are never referenced from a display list.  They reach the RDP through
 * N64 segment registers, bound per draw from a struct:
 *
 *     ac_npc_draw.c_inc:269  gSPSegment(gfx++, ANIME_1_TXT_SEG, eye_tex_p);
 *                      :273  gSPSegment(gfx++, ANIME_2_TXT_SEG, mouth_tex_p);
 *                      :277  gSPSegment(gfx++, ANIME_4_TXT_SEG, draw_tex_p->texture);
 *                      :278  gDPLoadTLUT_Dolphin(gfx++, 15, 16, 1, draw_tex_p->palette);
 *
 * So there is no relocation to do: a load is 16 pointer writes into one
 * npc_draw_data_tbl[] row (ac_npc.h:142-146 — texture, palette, eye[8],
 * mouth[6]), and that table is `extern aNPC_draw_data_c npc_draw_data_tbl[] =
 * {...}` (npc_draw_data.c:5304) — non-const, initialised, writable .data.
 * Confirmed in the linked image: `8c7d9a00 0000a128 D _npc_draw_data_tbl`,
 * and 0xA128 / 382 rows = 0x6C = sizeof(aNPC_draw_data_c).
 *
 * ⚠️ THE ROW IS SNAPSHOTTED PER ACTOR — PATCHING IT LATE IS A SILENT NO-OP
 * ------------------------------------------------------------------------
 * kb/STATE.md says the load is "16 pointer writes into npc_draw_data_tbl[]".
 * That is true and incomplete in the way that matters:
 *
 *     ac_npc_ctrl.c_inc:687-692  aNPC_dma_draw_data_proc() mem_copy's the whole
 *                                row into a caller-owned aNPC_draw_data_c
 *     ac_npc_ct.c_inc:255-256    aNPC_actor_ct() then mem_copy's tex_data into
 *                                nactorx->draw.draw_tex_data
 *     ac_npc_draw.c_inc:227      the DRAW reads &nactorx->draw.draw_tex_data —
 *                                the actor's PRIVATE COPY, not the table
 *     m_actor.c:135              the other dispatch, through
 *                                Common_Get(clip).npc_clip->dma_draw_data_proc
 *
 * So a patch applied after an actor exists changes nothing that is ever drawn,
 * and — worse — REUSING a slot under a live actor swaps one villager's skin
 * onto another with no diagnostic.  Both hazards are answered the same way:
 *
 *   LOAD at the snapshot.  dc_npctex_ensure() is called from inside
 *   aNPC_dma_draw_data_proc, one statement before the mem_copy, so the row is
 *   correct by the time it is copied.  It never evicts.
 *
 *   RESET only where no NPC actor can exist.  dc_npctex_pool_reset() is called
 *   from the two mNpc_SetNpcList() sites that rebuild the whole npclist —
 *   m_start_data_init.c:559 (mSDI_StartInitAfter, i.e. player-select / game
 *   start) and m_trademark.c:76 (set_npc_4_title_demo, before its
 *   GAME_GOTO_NEXT into PLAY).  Both run between scene teardowns, so the
 *   generation boundary is provably actor-free.  The other two callers —
 *   ac_boat_demo_move.c_inc:29 and ac_npc_ctrl.c_inc:878, both the islander,
 *   both count 1 — run mid-play and are deliberately NOT reset points; the
 *   islander simply takes the 16th slot.
 *
 * WHY NOT --wrap=mNpc_SetNpcList
 * ------------------------------
 * MEASURED, and it would have been a silent no-op: this toolchain uses a
 * leading-underscore user label prefix.  `sh-elf-nm` on the linked image
 * (dc/build/dedup/syms.txt) shows `8c35f384 00000318 T _mNpc_SetNpcList` and
 * `8c4ba6c4 00000278 T _main`, so ld's --wrap would have to be spelled
 * `--wrap=_mNpc_SetNpcList` against a C function whose asm name is
 * `__wrap__mNpc_SetNpcList` — and --wrap on a symbol that does not exist is
 * ignored without a diagnostic.  The R1 seam (a call-site rewrite in the
 * DC_SRC_SHRINK scratch tree) needs no link-line change and is anchored, so a
 * drift in the vendored source is a hard error instead of a quiet miss.
 *
 * THE ROM LAYOUT DOES THE HARD PART.  Every one of the 236 villager sets is
 * CONTIGUOUS in foresta.rel, in declaration order — palette, eye1..8,
 * mouth1..6, body — with no gaps and one rom_src (SRC_REL) throughout.
 * Verified against all 236 x 16 s_assets[] rows by the generator, which
 * hard-errors if it ever stops being true.  Only the palette is byte-swapped
 * (SWAP_U16), so a whole set is TWO reads: 32 B swapped, then the rest raw.
 * That is 32 disc commands for a full town, not 240.
 *
 * WHAT A MISS LOOKS LIKE, AND WHY IT IS NEVER SOMEONE ELSE'S SKIN
 * ---------------------------------------------------------------
 * A row that cannot get a slot, and a row whose slot was freed by a reset, is
 * pointed at s_npctex_black[] — a never-written zero block sized to the largest
 * body texture.  An all-zero CI4 texture with index 0 transparent is exactly
 * what an unkept asset already looks like (kb/traps.md), so the failure mode is
 * the one this project can already read.  It is NEVER a stale pointer into a
 * reused slot, which would render as a different animal's skin and look like a
 * game bug rather than a missing asset.
 *
 * WHAT IT COSTS ON THE DISC.  A full town is at most 16 species x 2 reads =
 * 32 seeks moving <= 77,312 B, spread across the constructions that happen as
 * the player walks into acres — not one boot-time burst.  At the ~500 KB/s
 * CD-R figure the payload is ~155 ms total; the seeks dominate and dc_main.c's
 * sweep model prices a scattered seek at 20-100 ms.  If that shows up as a
 * per-acre hitch, the fix is dc_keep_sweep()'s (dc_main.c:977-1108): batch the
 * requests in (rom_src, rom_off) order.  MEASURE FIRST — the number to A/B is
 * the GD-ROM command count, not wall clock.
 */
#include "dc_platform.h"

/* The row shape the generated map is written against.  tools/dcstub/
 * make_stub_data.py emits the .inc; this typedef is the other end of that
 * contract, so change both or neither. */
typedef struct {
    unsigned int   rom_off;    /* base of the WHOLE set; it is contiguous     */
    const char*    name;       /* "cat_1" — diagnostic only                   */
    unsigned short total;      /* bytes from rom_off to the end of the body   */
    unsigned short tmem_off;   /* offset of the body texture inside the slot  */
    unsigned short tmem_size;
    unsigned char  mouths;     /* DC_NPCTEX_MOUTH_NUM, or 0 when all NULL     */
    unsigned char  rom_src;
    unsigned char  kept;       /* on DC_STUB_KEEP: resident already, hands off */
    unsigned char  pad_;
} dc_npctex_set_t;

#ifdef DC_ASSET_STUB
#include "dc/build/stubsrc/dc_npctex_map.inc"
#else
/* No stub tree, so no [1]-sized arrays: every villager texture is full size and
 * was filled by pc_assets_init().  There is nothing to pool. */
#define DC_NPCTEX_SETS 0
#endif

#if DC_NPCTEX_SETS > 0

/* dc_main.c, and only compiled there under DC_ASSET_STUB — which is exactly the
 * condition the map is emitted under.  Declared rather than included so this TU
 * does not pull the whole keep-list header in. */
extern void dc_stub_keep_load_one(const char* bin_path, void* dest,
                                  unsigned int size, unsigned int rom_off,
                                  int rom_src, int swap);

/* npc_draw_data_tbl[] is reached as raw bytes on purpose: this TU must not
 * include ac_npc.h (it drags in half the actor system, and dc/src builds at a
 * different -O and language level).  The offsets are ac_npc.h:141-158 and they
 * are NOT trusted from a comment — the DC_SRC_SHRINK copy of
 * ac_npc_ctrl.c_inc, which does have the header, carries a compile-time assert
 * on every one of them plus ALL_NPC_NUM and ANIMAL_NUM_MAX.  If the vendored
 * struct moves, that TU fails to compile; it cannot drift quietly. */
extern unsigned char npc_draw_data_tbl[];
#define DC_NPCTEX_ROW_STRIDE   0x6Cu   /* sizeof(aNPC_draw_data_c)            */
#define DC_NPCTEX_TEXDATA_OFF  0x08u   /* offsetof(aNPC_draw_data_c, tex_data) */

/* The 16 pointers, in the order they appear in aNPC_draw_tex_data_c: texture,
 * palette, eye_texture[8], mouth_texture[6].  0x08..0x44 of the row. */
#define DC_NPCTEX_PTRS 16

/* pc/src/pc_assets.c:14 — enum { SWAP_NONE = 0, SWAP_U16 = 1, ... }.  The
 * palette is the only member of a set that is swapped, in all 236 of them. */
#define DC_NPCTEX_SWAP_NONE 0
#define DC_NPCTEX_SWAP_U16  1

/* ANIMAL_NUM_MAX (m_npc.h:28) + mISL_ISLANDER_NUM (m_island.h:16).  That is the
 * provable ceiling on distinct species alive in one generation: 15 town
 * villagers plus the islander, whose mNpc_SetNpcList() call
 * (ac_npc_ctrl.c_inc:878) is mid-play and therefore an APPEND, not a reset.
 *
 * A 17th distinct requester is possible in principle — an event or mask NPC
 * whose texture_id resolves to a villager row (ac_npc_ctrl.c_inc:158-176) is
 * not in the npclist and is not counted by that ceiling.  It is a build knob
 * rather than a literal so that case costs one -D and not an edit; each extra
 * slot is DC_NPCTEX_SLOT_BYTES of .bss. */
#ifndef DC_NPCTEX_SLOTS
#define DC_NPCTEX_SLOTS 16
#endif

/* Every row that got patched has to be pointed back at s_npctex_black[] on
 * reset, so the list has to be long enough to name them all.  A set may be
 * named by more than one row (chn_1 is used by rows 39, 236 and 237 — the two
 * test villagers at the end of the villager range reuse it), and
 * DC_NPCTEX_ROWS_PER_SET_MAX is the generator's measured worst case. */
#define DC_NPCTEX_PATCHES (DC_NPCTEX_SLOTS * DC_NPCTEX_ROWS_PER_SET_MAX)

static unsigned char s_pool[DC_NPCTEX_SLOTS][DC_NPCTEX_SLOT_BYTES]
    __attribute__((aligned(32)));

/* Never written.  Every one of the 16 pointers of an unserved row points at its
 * base, which is in bounds for all of them: the largest read any of them can
 * take is a body texture, and this is sized to the largest body texture in the
 * villager range (the palette is 32 B, an eye or mouth 256 B). */
static unsigned char s_npctex_black[DC_NPCTEX_TMEM_MAX]
    __attribute__((aligned(32)));

static short s_slot_set[DC_NPCTEX_SLOTS];       /* set index, -1 = free       */
static short s_patched[DC_NPCTEX_PATCHES];      /* rows pointed into the pool */
static int   s_patch_n;
static int   s_slots_used;
static unsigned int s_bytes_loaded;
static unsigned int s_overflow;                 /* requests that got nothing  */
static unsigned int s_generation;
static int   s_ready;

/* s_slot_set[] has to start at -1, not at .bss's 0, or every slot would claim
 * to hold set 0 (cat_1).  Done lazily rather than from an init hook or a
 * __attribute__((constructor)): this TU then has no boot-order dependency and
 * no dependency on KOS running C constructors at all. */
static void dc_npctex_arm(void) {
    int i;

    if (s_ready) {
        return;
    }
    for (i = 0; i < DC_NPCTEX_SLOTS; i++) {
        s_slot_set[i] = -1;
    }
    s_ready = 1;
}

/* Powers of two only.  Overflow is per CONSTRUCTION, so a town that is one
 * species over budget would print on every villager spawn for the rest of the
 * run and the log would become the flood.  Call with the POST-increment count. */
static int dc_npctex_say(unsigned int n) {
    return (n & (n - 1u)) == 0u;
}

/* The 16 pointer slots of one npc_draw_data_tbl[] row, as a writable array. */
static void** dc_npctex_row_ptrs(int row) {
    return (void**)(void*)(npc_draw_data_tbl
                           + (unsigned int)row * DC_NPCTEX_ROW_STRIDE
                           + DC_NPCTEX_TEXDATA_OFF);
}

/* Lay the 16 pointers of one row over a POOL SLOT holding `set`'s bytes. */
static void dc_npctex_point(int row, unsigned char* slot,
                            const dc_npctex_set_t* set) {
    void** p = dc_npctex_row_ptrs(row);
    int i;

    p[0] = slot + set->tmem_off;                        /* texture  */
    p[1] = slot;                                        /* palette  */
    for (i = 0; i < DC_NPCTEX_EYE_NUM; i++) {
        p[2 + i] = slot + DC_NPCTEX_PAL_SIZE + i * DC_NPCTEX_EYE_SIZE;
    }
    for (i = 0; i < DC_NPCTEX_MOUTH_NUM; i++) {
        /* mouth_texture[] is all NULL in 78 of the 236 sets and the draw tests
         * for it (ac_npc_draw.c_inc:272).  Handing those a real pointer would
         * bind a mouth quad on an animal the artists gave no mouth. */
        p[2 + DC_NPCTEX_EYE_NUM + i] =
            set->mouths ? (void*)(slot + DC_NPCTEX_MOUTH_OFF
                                  + i * DC_NPCTEX_MOUTH_SIZE)
                        : (void*)0;
    }
}

/* Point every one of the 16 at the zero block instead.  All of them at its
 * BASE, not at a computed offset: the block is only as long as the largest
 * BODY texture, and set->tmem_off is well past that.  Every read any of the 16
 * can take (32 B palette, 256 B eye or mouth, <= DC_NPCTEX_TMEM_MAX body) is
 * then in bounds and returns zeroes — which is byte-for-byte what an unkept
 * asset looks like, and what kb/traps.md has already taught this project to
 * read.  mouth_texture[] keeps its NULLs so the two states differ in nothing
 * but the address. */
static void dc_npctex_blank(int row, const dc_npctex_set_t* set) {
    void** p = dc_npctex_row_ptrs(row);
    int i;

    for (i = 0; i < DC_NPCTEX_PTRS; i++) {
        p[i] = s_npctex_black;
    }
    if (!set->mouths) {
        for (i = 0; i < DC_NPCTEX_MOUTH_NUM; i++) {
            p[2 + DC_NPCTEX_EYE_NUM + i] = (void*)0;
        }
    }
}

static void dc_npctex_record(int row) {
    int i;

    for (i = 0; i < s_patch_n; i++) {
        if (s_patched[i] == (short)row) {
            return;
        }
    }
    if (s_patch_n >= DC_NPCTEX_PATCHES) {
        /* Cannot happen while DC_NPCTEX_ROWS_PER_SET_MAX is honest — it is the
         * generator's measured maximum and the generator hard-errors if the
         * vendored table drifts past it.  Loud rather than silent because the
         * consequence is a row left pointing into a slot the next generation
         * will reuse: the one failure this file exists to make impossible. */
        DC_LOGE("[DC/NPCTEX] patch list full at %d — row %d will keep a STALE "
                "pointer across the next reset\n", DC_NPCTEX_PATCHES, row);
        return;
    }
    s_patched[s_patch_n++] = (short)row;
}

/* Called from aNPC_dma_draw_data_proc (the DC_SRC_SHRINK copy of
 * ac_npc_ctrl.c_inc), one statement before the row is mem_copy'd into the
 * actor.  `row` is aNPC_get_draw_data_idx()'s result and may be -1. */
void dc_npctex_ensure(int row) {
    const dc_npctex_set_t* set;
    unsigned char* slot;
    int idx;
    int i;
    int free_slot = -1;

    /* -1 is aNPC_get_draw_data_idx()'s "neither NPC nor SPNPC" (:154), and
     * >= DC_NPCTEX_ROWS is a special NPC — Tom Nook, Rover, K.K., Porter and
     * the raccoons live at rows 238..381 and are KEPT RESIDENT by
     * tools/dcstub/make_keeplist_town.py's EXTRA_SOURCES.  Pooling them was the
     * one thing this must not do: they are not town-randomised, they are on
     * screen in the intro, and the project has already lost a session to
     * dropping them silently (kb/RESUME.md, session 3 item 2). */
    if (row < 0 || row >= DC_NPCTEX_ROWS) {
        return;
    }
    dc_npctex_arm();

    idx = dc_npctex_row_set[row];
    if (idx < 0) {
        return;
    }
    set = &dc_npctex_set[idx];

    /* A set that is on DC_STUB_KEEP is full size and was already filled by
     * dc_stub_keep_load().  Its row's vendored pointers are correct; repointing
     * it into the pool would work but would burn a slot, and repointing it at
     * the zero block on the next reset would DESTROY a real texture. */
    if (set->kept) {
        return;
    }

    for (i = 0; i < DC_NPCTEX_SLOTS; i++) {
        if (s_slot_set[i] == (short)idx) {
            /* Resident.  Another row of the same set (only chn_1 today) or a
             * respawn of one already served: repoint and do no I/O. */
            dc_npctex_point(row, s_pool[i], set);
            dc_npctex_record(row);
            return;
        }
        if (s_slot_set[i] < 0 && free_slot < 0) {
            free_slot = i;
        }
    }

    if (free_slot < 0) {
        /* NO EVICTION, EVER.  The actor being constructed right now is about to
         * take a private copy of these pointers (ac_npc_ct.c_inc:256) and so
         * did every actor already alive; reusing a slot would put another
         * animal's skin on one of them with nothing to see in the log.  This
         * villager draws as an absent asset instead, and says so. */
        dc_npctex_blank(row, set);
        dc_npctex_record(row);
        s_overflow++;
        if (dc_npctex_say(s_overflow)) {
            DC_LOGE("[DC/NPCTEX] POOL FULL (%d slots) — %s left untextured. "
                    "A town holds ANIMAL_NUM_MAX=15 villagers + 1 islander; a "
                    "17th distinct species means an event/mask NPC borrowed a "
                    "villager row. Raise it with -DDC_NPCTEX_SLOTS=%d "
                    "(+%d B .bss)\n",
                    DC_NPCTEX_SLOTS, set->name, DC_NPCTEX_SLOTS + 1,
                    (int)DC_NPCTEX_SLOT_BYTES);
        }
        return;
    }

    slot = s_pool[free_slot];

    /* TWO reads, not sixteen: the whole set is contiguous in the ROM and only
     * the leading 32 B palette is byte-swapped.  See the header. */
    dc_stub_keep_load_one(set->name, slot, DC_NPCTEX_PAL_SIZE, set->rom_off,
                          (int)set->rom_src, DC_NPCTEX_SWAP_U16);
    dc_stub_keep_load_one(set->name, slot + DC_NPCTEX_PAL_SIZE,
                          (unsigned int)set->total - DC_NPCTEX_PAL_SIZE,
                          set->rom_off + DC_NPCTEX_PAL_SIZE,
                          (int)set->rom_src, DC_NPCTEX_SWAP_NONE);

    s_slot_set[free_slot] = (short)idx;
    s_slots_used++;
    s_bytes_loaded += set->total;

    dc_npctex_point(row, slot, set);
    dc_npctex_record(row);

    DC_LOG("[DC/NPCTEX] %s -> slot %d (%u B) | %d/%d slots, %u B\n",
           set->name, free_slot, (unsigned int)set->total, s_slots_used,
           DC_NPCTEX_SLOTS, s_bytes_loaded);
}

/* Called from the two mNpc_SetNpcList() sites that rebuild the whole npclist.
 * ⚠️ ONLY from those two.  It frees every slot, and it is correct only because
 * no NPC actor exists at either point — see the header. */
void dc_npctex_pool_reset(void) {
    int i;

    dc_npctex_arm();

    if (s_slots_used || s_overflow) {
        DC_LOGE("[DC/NPCTEX] generation %u: %d/%d slot(s), %u B off the disc, "
                "%u overflow(s)\n",
                s_generation, s_slots_used, DC_NPCTEX_SLOTS, s_bytes_loaded,
                s_overflow);
    }

    /* Point every patched row at the zero block BEFORE the slots are freed.
     * Leaving them aimed at a slot the next generation will refill is the
     * wrong-skin failure; the zero block reads as a missing asset, which is a
     * failure this project already knows how to see (kb/traps.md). */
    for (i = 0; i < s_patch_n; i++) {
        int row = s_patched[i];
        dc_npctex_blank(row, &dc_npctex_set[dc_npctex_row_set[row]]);
    }
    s_patch_n = 0;

    for (i = 0; i < DC_NPCTEX_SLOTS; i++) {
        s_slot_set[i] = -1;
    }
    s_slots_used = 0;
    s_bytes_loaded = 0;
    s_overflow = 0;
    s_generation++;
}

#else /* DC_NPCTEX_SETS == 0 */

/* R2 is off — either DC_NPCTEX_POOL=0 or this is a non-stub build.  Every
 * villager texture is resident (the 21 census entries are back on the keep list
 * at that setting; a non-stub build has all 236), so the vendored
 * npc_draw_data_tbl[] pointers are already right and there is nothing to do. */
void dc_npctex_ensure(int row) { (void)row; }
void dc_npctex_pool_reset(void) { }

#endif /* DC_NPCTEX_SETS */
