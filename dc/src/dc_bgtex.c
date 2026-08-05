/* dc_bgtex.c — the acre ground textures, read off the disc instead of kept in
 * .bss.  (R1; kill switch DC_BGTEX_DEMAND=0.)
 *
 * WHAT IS BEING MOVED
 * -------------------
 * mFM_LoadBGCommonTex() (src/game/m_field_make.c:1101) fills the 27 staging
 * buffers in src/game/m_bg_tex.c — which are always resident and are what the
 * renderer actually samples — by bcopy'ing from one of six 27-entry SOURCE
 * tables: three summer variants and three winter ones.  Those source arrays are
 * 96 distinct symbols under src/data/model/mFM_grd_*.c, and pc/src/pc_assets.c's
 * own row sizes make them:
 *
 *     mFM_grd_s_*   46 symbols    73,312 B
 *     mFM_grd_w_*   41 symbols    70,144 B
 *     shared         9 symbols     7,424 B
 *     ------------------------------------
 *                   96 symbols   150,880 B
 *
 * Every byte of that is written once, at boot, from the disc — and then read
 * once per acre load into a buffer that is already resident.  It is the same
 * bytes stored twice.  The keep list was paying 80,736 B for the summer half
 * and declining to pay the winter half, which is why the town ground turns
 * BLACK in December (kb/station-bugs.md §1 is that same failure, seen in
 * summer): an unkept CI4 texture is all zeros, and index 0 is the transparent
 * palette entry.  Demand-loading pays neither half and both seasons work.
 *
 * THE KEY: A STUBBED SOURCE ARRAY IS STILL A UNIQUE ADDRESS
 * ---------------------------------------------------------
 * tools/dcstub/make_stub_data.py rewrites an unkept asset to `u8 x[1];` — one
 * byte of .bss with its own valid, distinct address.  The six tables in
 * m_field_make.c still name those symbols, so `bg_tex_tbl[i]` is a unique KEY
 * saying which source that slot wants.  That is the whole trick, and it is why
 * nothing in this file knows about seasons or tex_idx: the vendored tables
 * already did the selecting, and the map below is a plain pointer -> ROM row
 * lookup generated from the same s_assets[] rows the keep machinery uses.
 *
 * ⚠️ SIZE IS THE CALLER'S, NOT THE TABLE'S — AND THE TWO DISAGREE ONCE
 * --------------------------------------------------------------------
 * l_bg_tex_common_dummy[15] is `{ beach_tex_dummy, 0x800 }` (m_field_make.c:886)
 * but its source mFM_grd_s_beach_tex is 0x400 (pc_assets.c row, and
 * src/data/model/mFM_grd_s_beach_tex.c:9).  Vanilla over-reads 1,024 B past the
 * end of that array on every single call, in all six tables, at index 15.
 * Reading the DEST size from rom_off reproduces the GameCube's own behaviour —
 * it gets the real adjacent ROM bytes rather than whatever the host linker
 * happened to place after the array — so `size` as passed by the caller is what
 * is read, not the row's size.  The mismatch is logged (rate-limited) rather
 * than clamped, because the day a second one appears it must not be silent.
 *
 * ⚠️ THIS RUNS MID-SCENE, NOT ONLY AT LOAD
 * -----------------------------------------
 * Three call sites: m_field_make.c:1221 (mFM_FieldInit, update_tex=TRUE, all 27
 * slots) and :1745 / :1754 (mFM_toSummer / mFM_returnSeason, update_tex=FALSE,
 * 21 slots each — l_water_permission gates the other six).  The latter pair
 * fires on the island boat trip, from ac_boat_demo_move.c_inc:92-102.  So every
 * load here has to be safe during gameplay, and it is: dc_stub_keep_load_one()
 * re-opens the ROM fd lazily on FILEHND_INVALID (dc_main.c:916-926) and the
 * DC_KEEP_SWEEP recording path is already off by then (s_keep_recording is
 * cleared at dc_main.c:1117), so the read happens inline.
 *
 * WHAT THAT COSTS, AND THE FOLLOW-UP IF IT SHOWS UP AS A STALL.  One full pass
 * is 27 seeks moving 33,632 B (the l_bg_tex_common_dummy sizes, summed; the
 * update_tex=FALSE pass is 21 reads and 27,488 B).  At the ~500 KB/s CD-R
 * figure the payload alone is ~67 ms; the seeks dominate, and
 * dc_main.c's own sweep model prices a scattered seek at 20-100 ms, so the
 * honest range is a few hundred ms to a couple of seconds on hardware and
 * approximately nothing in Flycast with FastGDRomLoad.  The fix, if it matters,
 * is the one dc_keep_sweep() already implements (dc_main.c:977-1108): sort the
 * 27 requests by (rom_src, rom_off) and serve them from one window buffer.
 * That is deliberately NOT done here — 27 requests is not 1,392, and a batch
 * helper that runs mid-scene needs a buffer this port would have to find.
 * MEASURE FIRST: the number to A/B is the GD-ROM command count, not wall clock.
 */
#include "dc_platform.h"

/* The row shape the generated map is written against.  tools/dcstub/
 * make_stub_data.py emits the .inc; this typedef is the other end of that
 * contract, so change both or neither. */
typedef struct {
    const void*  src;      /* the source array's address — THE LOOKUP KEY */
    const char*  path;     /* diagnostic name, same spelling as the keep list */
    unsigned int size;     /* the s_assets[] size; see the size note above */
    unsigned int rom_off;
    int          rom_src;
    int          swap;
} dc_bgtex_row_t;

#ifdef DC_ASSET_STUB
#include "dc/build/stubsrc/dc_bgtex_map.inc"
#else
/* No stub tree, so no [1]-sized arrays: every source is full size and was
 * filled by pc_assets_init().  There is nothing to demand-load. */
#define DC_BGTEX_MAP_N 0
#endif

#if DC_BGTEX_MAP_N > 0

/* dc_main.c, and only compiled there under DC_ASSET_STUB — which is exactly the
 * condition the map is emitted under.  Declared rather than included so this TU
 * does not pull the whole keep-list header in. */
extern void dc_stub_keep_load_one(const char* bin_path, void* dest,
                                  unsigned int size, unsigned int rom_off,
                                  int rom_src, int swap);

static unsigned int s_bgtex_miss;
static unsigned int s_bgtex_mismatch;

/* Powers of two only.  A message on a path that runs 27 times per acre load
 * and 21 times per boat trip would BE the console log, and the flood limiter
 * (dc/src/dc_misc.c) is itself a build knob that can be turned off — so this
 * one backs itself off.  Call with the POST-increment count. */
static int dc_bgtex_say(unsigned int n) {
    return (n & (n - 1u)) == 0u;
}

void dc_bgtex_load(const void* src, void* dest, unsigned int size) {
    unsigned int i;

    if (dest == NULL || size == 0u) {
        return;
    }

    /* Linear scan: 96 rows, 27 lookups per acre load.  A sorted table plus a
     * bsearch would save ~1,200 pointer compares against a disc read that costs
     * five orders of magnitude more. */
    for (i = 0; i < (unsigned int)DC_BGTEX_MAP_N; i++) {
        const dc_bgtex_row_t* r = &dc_bgtex_map[i];

        if (r->src != src) {
            continue;
        }
        if (r->size != size) {
            /* Vanilla's own over-read at slot 15 (see the header).  Reading the
             * caller's size from rom_off is what reproduces the GameCube; the
             * line is here so a SECOND mismatch — which would be new, and a
             * bug — cannot arrive quietly. */
            s_bgtex_mismatch++;
            if (dc_bgtex_say(s_bgtex_mismatch)) {
                DC_LOGE("[DC/BGTEX] %s: dest wants %u B, s_assets[] row is %u B"
                        " — reading %u from ROM, as the GameCube does\n",
                        r->path, size, r->size, size);
            }
        }
        dc_stub_keep_load_one(r->path, dest, size, r->rom_off, r->rom_src,
                              r->swap);
        return;
    }

    /* The map covers all 96 symbols the six vendored tables name, kept or not,
     * so a miss means the tables and pc_assets.c have drifted apart.  If the
     * source happens to be a KEPT (full-size, already loaded) array the memmove
     * below is correct and this is only noise; if it is a stubbed [1] array the
     * memmove reads `size` bytes past a one-byte object and the acre ground
     * renders as garbage — which reads as a renderer bug for hours
     * (kb/traps.md).  So it is loud either way. */
    s_bgtex_miss++;
    if (dc_bgtex_say(s_bgtex_miss)) {
        DC_LOGE("[DC/BGTEX] source %08x (%u B) is not in dc_bgtex_map[%d] — "
                "falling back to memmove; if that array is stubbed this acre's "
                "ground is garbage\n",
                (unsigned int)(uintptr_t)src, size, (int)DC_BGTEX_MAP_N);
    }
    memmove(dest, src, size);
}

#else  /* DC_BGTEX_MAP_N == 0 */

/* R1 is off — either DC_BGTEX_DEMAND=0 or this is a non-stub build.  The source
 * arrays are resident, so this is byte-for-byte what the vendored bcopy did.
 * (bcopy folds to memmove on this target anyway: the libultra declaration is
 * behind #ifndef TARGET_PC and no _bcopy symbol exists in the linked image.) */
void dc_bgtex_load(const void* src, void* dest, unsigned int size) {
    memmove(dest, src, size);
}

#endif /* DC_BGTEX_MAP_N */
