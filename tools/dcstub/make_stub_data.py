#!/usr/bin/env python3
"""
make_stub_data.py — build the DC_ASSET_STUB source tree (kb/plan-stages.md, S1).

WHY THIS EXISTS
---------------
The Dreamcast image is over 16 MB and therefore never executes a single
instruction: KOS's startup .bss zeroing runs off the end of physical memory
before scif_init(), so there is not even console output to look at
(kb/state-log.md, "Boot status"). Every RAM estimate in this project assumes a
platform layer that has never been observed running.

S1 breaks that deadlock cheaply: shrink the asset *destination* arrays to one
element so the image fits, boot it, and find out whether dc_main.c's trampoline,
KOS init, the console path, dc_mem_ledger.c's MEMLEDGER FIT line and crash.sh
symbolisation actually work. The game renders garbage the moment it touches an
asset. That is the point — it is a throwaway bring-up image, not a milestone.

WHY A REWRITER AND NOT AN EDIT
------------------------------
CLAUDE.md §1: src/ is vendored decomp and is never edited to make something
build. Nothing here touches it. src/data/**/*.c is *generator output*
(pc/tools/gen_runtime_assets.py), and this script performs the same rewrite that
generator would have performed had it been asked for stub-sized arrays. Output
goes to a scratch tree; the real tree is untouched and nothing is committed.

WHAT IT REWRITES
----------------
gen_runtime_assets.py emits exactly two shapes (see its lines 438-465). Both put
the declaration on the line immediately after `#ifdef TARGET_PC`:

    #ifdef TARGET_PC                       #ifdef TARGET_PC
    Vtx act_ant_v[0xB0 / sizeof(Vtx)];     u8 foo[0x200] ATTRIBUTE_ALIGN(32);
    #else                                  static int foo_loaded = 0;
    Vtx act_ant_v[] = { ... };             if (!foo_loaded) {
    #endif                                     extern void pc_load_asset(...);
                                               pc_load_asset("...", foo, 0x200, ...);
                                               foo_loaded = 1;
                                           }
                                           #else ...

The array bound becomes [1]. In the function-local (lazy) shape the
pc_load_asset() call is ALSO neutralised: it would otherwise memcpy the full
asset into a one-element array and corrupt whatever follows. The central-table
loads in pc_assets.c are handled the other way, by dc_main.c skipping
pc_assets_init() entirely under -DDC_ASSET_STUB.

THE KEEP ALLOWLIST — real assets inside an otherwise-stubbed image
------------------------------------------------------------------
Renderer bring-up needs *something* real on screen. DC_STUB_KEEP names
repo-relative source files this script leaves at FULL SIZE. The built-in
default is the complete Animal Crossing title-logo overlay (~53 KB), which is
what src/actor/ac_animal_logo.c draws:

    logo_us_animal.c / logo_us_cros.c / logo_us_sing.c   the three cKF skeletons
    logo_us_back.c                                       the 4 backdrop tiles
    logo_us_tm.c                                         the (TM) glyph
    log_win_nintendo{1,2,3}_tex.c                        the copyright strip
    log_win_logo{3,4}_tex.c                              "PRESS START"

Kill switch, both directions:

    DC_STUB_KEEP unset   -> the default keep list above
    DC_STUB_KEEP=""      -> keep NOTHING; byte-identical to the old behaviour
    DC_STUB_KEEP=a.c:b.c -> keep exactly those (':' or ',' separated)
    --keep / --keep=""   -> same, and overrides the environment

A path that does not exist is a hard error, not a silent no-op.

HOW KEPT ASSETS ACTUALLY GET DATA IN THEM — the second half of the problem
--------------------------------------------------------------------------
A full-size array is useless if nothing fills it. Under DC_ASSET_STUB,
dc_main.c does NOT call pc_assets_init(), because that walks the whole
s_assets[] table and would memcpy megabytes over [1]-sized destinations. That
skip must stay. So this script generates the *narrow* replacement:

    dc/build/stubsrc/dc_stub_keep.inc

a header defining `static void dc_stub_keep_load(void)` that loads ONLY the
allowlisted destinations. dc_main.c #includes it under DC_ASSET_STUB (resolved
via the Makefile's -I$(ROOT)) and calls it where pc_assets_init() would have
been. It is always generated — with an empty body when the keep list is empty —
so the include never dangles.

Two kinds of destination exist and the .inc handles both:

  1. Externally-visible arrays, loaded from the central table in
     pc/src/pc_assets.c. This script parses that table, matches `dest` symbols
     against the arrays declared in kept files, and emits one call per row with
     the table's own (path, size, rom_off, rom_src, swap) tuple.

  2. File-static arrays, which the generator could not put in the central table
     and instead loads from a per-file `void _pc_load_src_<path>_c(void)`
     appended to the TU. The .inc calls those functions directly.

Neither kind may go through pc_load_asset(): on the Dreamcast that function
would need g_rel_data/g_dol_data resident, i.e. all 15,640,056 B of
foresta.rel in RAM at once (dc_dvd.c's warning), and its .bin fallback uses a
host-relative fopen() that cannot resolve on /cd. So every load is redirected
to `dc_stub_keep_load_one()` in dc/src/dc_main.c, which pread()s exactly `size`
bytes at `rom_off` out of /cd/foresta.rel or /cd/main.dol and byte-swaps in
place. For case 2 the redirect is textual: a kept file is copied into the stub
tree with the identifier `pc_load_asset` renamed, arrays untouched at full
size. That is why a kept file can still appear in stub.list.

THE ACRE GROUND TEXTURES — dc_bgtex_map.inc (R1, --bgtex-demand)
----------------------------------------------------------------
One family of assets does not want to be kept at all. The 96 mFM_grd_* source
arrays (150,880 B: 46 summer, 41 winter, 9 shared) exist only to be bcopy'd, in
mFM_LoadBGCommonTex(), into the 27 always-resident staging buffers in
src/game/m_bg_tex.c. Keeping them stores the same bytes twice, and the keep
list could only ever afford the summer half — which is why the town ground goes
black in December.

So this script ALSO emits

    dc/build/stubsrc/dc_bgtex_map.inc

a table of {source array address, path, size, rom_off, rom_src, swap} for every
mFM_grd_* row in s_assets[], and tools/dcstub/make_src_shrink.py rewrites that
one bcopy into a dc_bgtex_load() call. The stubbed `u8 x[1];` source array is
still a UNIQUE ADDRESS, so the vendored season/variant tables keep doing the
selecting and dc/src/dc_bgtex.c only has to look the pointer up. Read that file
first; it carries the whole argument and the two hazards.

Every mFM_grd_* row is emitted, summer and winter, regardless of keep-list
membership — the point is that none of them are kept.

    --bgtex-demand 0   emit an empty map AND put the 27 summer/shared
                       mFM_grd_*.c files back on the keep list. That is the
                       kill switch, and it must be paired with the same flag on
                       make_src_shrink.py (dc/build-dc.sh passes both).

THE VILLAGER TEXTURES — dc_npctex_map.inc (R2, --npctex-pool)
-------------------------------------------------------------
Same shape, different seam. src/data/npc/model/tex/*.c is 1,154,944 B of .bss
in 276 sets, of which 236 (993,984 B) are VILLAGER sets — and a town holds at
most 15 villagers (ANIMAL_NUM_MAX) plus one islander. So at most 16 of the 236
can be wanted at once and the keep list was paying for the 21 a census run
happened to see, leaving 215 species with no texture at all.

So this script ALSO emits

    dc/build/stubsrc/dc_npctex_map.inc

one row per distinct villager set — {rom_off, size, body offset, mouths,
rom_src} — plus an npc_draw_data_tbl[] row -> set index. dc/src/dc_npctex.c
owns 16 static slots, reads a set into one of them off the disc, and rewrites
the 16 pointers of that table row. Those pointers are never baked into a
display list (the draw binds them through N64 segment registers,
ac_npc_draw.c_inc:269-278), so there is no relocation to do.

Rows at or above ALL_NPC_NUM are the SPECIAL NPCs — Tom Nook, Rover, K.K.,
Porter, the raccoons — and are deliberately absent: they are not
town-randomised, they stay on the keep list via
tools/dcstub/make_keeplist_town.py's EXTRA_SOURCES, and dropping them silently
has already cost this project a session.

    --npctex-pool 0    emit an empty map AND put the 21 censused villager
                       tex files back on the keep list. Same pairing rule as
                       --bgtex-demand.

THE VILLAGER MODELS — dc_npcmdl_map.inc (R3, --npcmdl-pool)
------------------------------------------------------------
R2's other half, and the one that needs RELOCATION. src/data/npc/model/mdl/ is
72 files x one `static Vtx <sp>_v[]`, 438,640 B, of which 32 species / 194,400 B
are reachable from a VILLAGER row of npc_draw_data_tbl[] and 40 species /
244,240 B only from a SPECIAL row. Measured 2026-08-05: the two sets do not
intersect at all, which is why the villager half can be pooled without ever
touching Tom Nook.

Textures were free to move because the draw binds them through segment
registers. Vertices are not: every one of the 2,068 `gsSPVertex(&<sp>_v[N],…)`
in that tree is an R_SH_DIR32 relocation resolved at link time into word 1 of an
initialised Gfx, and emu64::seg2k0 (emu64_utility.c:48-51) returns any address
with a non-zero upper nibble unchanged — so on SH-4 the 0x8Cxxxxxx in .data IS
the address the vertex fetch uses. Moving the array means rewriting those words.

So this script ALSO emits

    dc/build/stubsrc/dc_npcmdl_map.inc

one row per villager species — {&cKF_bs_r_<sp>, rom_off, size, rom_src} — plus
a flat patch table of {joint index, Gfx index, byte offset} triples, 933 of them
for the villager half. dc/src/dc_npcmdl.c owns 16 static slots, reads a species'
vertex array into one off the disc, and walks the patch table writing
slot + offset into each named word. It reaches the display lists through the
extern skeleton (`&cKF_bs_r_<sp>` -> joint_table[j].model), because both
`<sp>_v` and all 13 `*_model[]` lists are `static` and cannot be named from dc/.

⚠️ WHY A TABLE AND NOT A RUNTIME WALK OF THE DISPLAY LIST. gsSPNTriangles_5b
(gbi_extensions.h:1190) packs triangle indices into the TOP BYTE of w0, so a
G_TRIN payload word has no opcode and can read as any command — G_VTX (0x01)
included, whenever v11 == 0 and v10 is in 4..7. emu64::dl_G_TRIN (emu64.c:4802)
consumes those words by COUNT, 3 on the first pass and 4 thereafter, so a walker
would have to reimplement that rule exactly or it would write a slot address
into a neighbouring word and corrupt geometry silently. The table is computed
once, offline, from the macro expansions; and dc_npcmdl.c re-checks every entry
against the live word before it writes anything.

    --npcmdl-pool 0    emit an empty map AND put cbr_1 — the one VILLAGER model
                       the census keep list named — back on the keep list. Same
                       pairing rule as --bgtex-demand.

THE TEXTURE PROBE — dc_texpool_map.inc (T1, --texpool)
-------------------------------------------------------
NOT a pool. This one emits a map for an INSTRUMENT: dc/src/dc_texpool.c counts,
per texture bind, whether the source pointer is the exact base of a mapped asset
row. It moves no bytes and changes no pixel. It exists to answer, before the T1
loader is written, whether the design's central assumption holds — that a
texture's cache key can be synthesised from its asset IDENTITY instead of read
back out of main RAM on every bind (dc_pvr_texture.c:1083's tex_content_hash,
~109 binds/frame).

Three counters kill the design if non-zero: `interior` (a bind landing inside a
row rather than at its base — with arrays stubbed to [1] that would silently
serve a neighbour's texture), `mutated` (a row whose content hash changes between
binds, i.e. it is not immutable) and `oversize` (the decoder reading past the
row, which is R1's mFM_grd_s_beach_tex over-read in a new place).

HOW THE POPULATION IS DERIVED, and why it is not a name heuristic. A row is a
symbol that (a) some display-list macro in src/ names as its TEXTURE IMAGE
operand, and (b) has an s_assets[] row. TEXPOOL_IMG_ARG below is the operand
position of every such macro in the tree — read out of each macro's definition,
never defaulted, exactly like NPCMDL_GFX_WORDS. Comments are stripped first:
src/game/m_play.c:59 contains `gDPSetTextureImage_Dolphin(..., G_IM_SIZ_16b,
...)` inside a comment, which parses as a 3-argument call and would otherwise be
counted as a malformed macro.

FOUR EXCLUSIONS, each for a measured reason and each with its own checksum:

  palettes      38 rows arrive through the texture-image operand because that is
                how a TLUT is loaded on Dolphin (set the texture image to the
                palette, then load it). They are identified by swap != 0
                (SWAP_U16, pc_assets.c:14) rather than by an `_pal` name — a
                data test, not a spelling test, and after it no `_pal`-named row
                survives. Palettes must stay RESIDENT: a CI texture decoded
                against a missing TLUT fails SILENTLY (dc_pvr_texture.c:347).
  gSPSegment     1 row (hnw_tmem_txt) is bound through a segment register rather
                than by address, so its address is not the key the renderer sees.
  file-static    6 rows (ctl_att_w1..w6_tex, src/static/dvderr.c) cannot be named
                `extern` from dc/. R2 hit this and got eight undefined references.
  src/data/npc/  0 rows today. R2's domain (dc/src/dc_npctex.c); asserted at 0 so
                that if the villager tree ever starts naming textures this way the
                build fails instead of the two systems both claiming them.

⚠️ RESIDENCY IS REPORTED, NEVER ASSERTED. Each row carries a `kept` flag saying
whether it is resident TODAY under the keep list, because only resident rows are
a byte saving and the rest are content that does not exist yet. That number moves
every time make_keeplist_town.py is re-run, so it must not be a checksum — a
keep-list-derived hard error would fail the build on every legitimate
regeneration. Checksums are for tree-derived, stable facts only.

    --texpool 0        emit an empty map. dc/src/dc_texpool.c then compiles to
                       nothing and the probe is inert. This is the default: the
                       instrument is off unless asked for by name.

USAGE
-----
    python3 tools/dcstub/make_stub_data.py [--out DIR] [--keep LIST]
                                           [--bgtex-demand 0|1]
                                           [--npctex-pool 0|1]
                                           [--npcmdl-pool 0|1]
                                           [--dry-run] [--quiet]

Default --out is dc/build/stubsrc. The tree mirrors repo-relative paths, so
dc/Makefile can swap src/foo.c for dc/build/stubsrc/src/foo.c and its
$(OBJDIR)/%.c.o: $(ROOT)/%.c rule keeps working with no other change.

Only files that actually contain a rewritten declaration (or a redirected load
call) are written out; every other TU still compiles from src/. Rewriting is
idempotent and content-compared, so an unchanged file is not re-touched and
make does not rebuild it.
"""

import argparse
import os
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "src"
DEFAULT_OUT = REPO / "dc" / "build" / "stubsrc"
ASSET_TABLE = REPO / "pc" / "src" / "pc_assets.c"

# The generated header dc_main.c includes. Name is part of the contract with
# dc/src/dc_main.c — change both or neither.
KEEP_INC_NAME = "dc_stub_keep.inc"
KEEP_LOADER = "dc_stub_keep_load_one"

# ---------------------------------------------------------------------------
# R1 — the acre ground-texture map. See the module docstring.
# ---------------------------------------------------------------------------
# Name and row shape are the contract with dc/src/dc_bgtex.c, which defines the
# matching typedef. Change both or neither.
BGTEX_INC_NAME = "dc_bgtex_map.inc"
BGTEX_PREFIX = "mFM_grd_"

# The file's own checksum, same rule as make_src_shrink.py's match counts: a
# selector that silently stops matching produces a build that looks fine and
# demand-loads nothing, and the acres it dropped render as black ground. 96 is
# the count of mFM_grd_* rows in s_assets[], cross-checked against the six
# segment tables in src/game/m_field_make.c — every symbol those tables name has
# a row, and every mFM_grd_* row is named by them. If the vendored data moves,
# re-derive this; do not relax it.
BGTEX_ROWS_EXPECTED = 96

# --bgtex-demand 0 puts these back on the keep list. This is the list
# keeplist-opening.txt carried until R1 landed, verbatim: the summer half plus
# the nine season-neutral arrays. The winter half was never on it — that is the
# December bug R1 also fixes — so turning the switch off restores the OLD
# behaviour exactly, black winter ground included.
BGTEX_KEEP_RESTORE = (
    "src/data/model/mFM_grd_beachA_tex.c",
    "src/data/model/mFM_grd_beachB_tex.c",
    "src/data/model/mFM_grd_s_beach_tex.c",
    "src/data/model/mFM_grd_s_bridge1.c",
    "src/data/model/mFM_grd_s_bridge1_pal.c",
    "src/data/model/mFM_grd_s_bridge2.c",
    "src/data/model/mFM_grd_s_bridge2_pal.c",
    "src/data/model/mFM_grd_s_bushA.c",
    "src/data/model/mFM_grd_s_bushB.c",
    "src/data/model/mFM_grd_s_cliff.c",
    "src/data/model/mFM_grd_s_earth.c",
    "src/data/model/mFM_grd_s_grass.c",
    "src/data/model/mFM_grd_s_rail.c",
    "src/data/model/mFM_grd_s_river.c",
    "src/data/model/mFM_grd_s_sand.c",
    "src/data/model/mFM_grd_s_station.c",
    "src/data/model/mFM_grd_s_station1_pal.c",
    "src/data/model/mFM_grd_s_stone.c",
    "src/data/model/mFM_grd_s_tekkyo.c",
    "src/data/model/mFM_grd_s_tunnel.c",
    "src/data/model/mFM_grd_sprashA_tex.c",
    "src/data/model/mFM_grd_sprashC_tex.c",
    "src/data/model/mFM_grd_water1_tex.c",
    "src/data/model/mFM_grd_water2_tex.c",
    "src/data/model/mFM_grd_wave1_tex.c",
    "src/data/model/mFM_grd_wave2_tex.c",
    "src/data/model/mFM_grd_wave3_tex.c",
)

# ---------------------------------------------------------------------------
# R2 — the villager texture pool. See dc/src/dc_npctex.c.
# ---------------------------------------------------------------------------
# Name and row shape are the contract with dc/src/dc_npctex.c, which defines the
# matching typedef. Change both or neither.
NPCTEX_INC_NAME = "dc_npctex_map.inc"
NPCTEX_TABLE = "src/data/npc/npc_draw_data.c"
NPCTEX_SRC_DIR = "src/data/npc/model/tex"
NPCTEX_BODY_SUFFIX = "_tmem_txt"

# The vendored shape, all four checked against every s_assets[] row the map
# emits. include/ac_npc.h:128 / :138 are the two counts; the sizes are the
# generator's own measurement across all 236 sets.
NPCTEX_EYE_NUM = 8            # aNPC_EYE_TEX_NUM
NPCTEX_MOUTH_NUM = 6          # aNPC_MOUTH_TEX_NUM
NPCTEX_PAL_SIZE = 32          # 16 entries x u16, the only swapped member
NPCTEX_FACE_SIZE = 256        # every eye and every mouth, in all 236 sets

# The three checksums, same rule as BGTEX_ROWS_EXPECTED: a selector that
# silently stops matching produces a build that looks fine, pools fewer species
# than it thinks, and renders those villagers untextured.
#   382  npc_draw_data_tbl[] rows      (pc/src/pc_model_viewer.c:321 agrees)
#   238  ALL_NPC_NUM, the villager/special boundary
#   236  distinct villager texture sets  (993,984 B; the other 40 sets and
#        160,960 B are the special NPCs and are NOT pooled)
NPCTEX_ROWS_TOTAL = 382
NPCTEX_VILLAGER_ROWS = 238
NPCTEX_SETS_EXPECTED = 236

# --npctex-pool 0 puts these back on the keep list. This is the list
# keeplist-opening.txt carried until R2 landed, verbatim: the 21 VILLAGER
# texture sets a census happened to see. The other 215 were never on it — that
# is the bug R2 fixes — so turning the switch off restores the OLD behaviour
# exactly, 215 untextured villagers included.
#
# ⚠️ The ten SPECIAL sets that were on that list alongside them (end_1, kab_1,
# mnk_1, mob_1, mol_1, mos_1, wip_1, wls_1, xct_1, xsq_1) are NOT here and were
# NOT removed: rows >= ALL_NPC_NUM are never pooled, so they stay on the keep
# list in both directions. Same for Tom Nook and the raccoons in
# make_keeplist_town.py's EXTRA_SOURCES.
NPCTEX_KEEP_RESTORE = (
    "src/data/npc/model/tex/cat_6.c",
    "src/data/npc/model/tex/cat_7.c",
    "src/data/npc/model/tex/cbr_5.c",
    "src/data/npc/model/tex/cbr_9.c",
    "src/data/npc/model/tex/chn_6.c",
    "src/data/npc/model/tex/crd_2.c",
    "src/data/npc/model/tex/dog_4.c",
    "src/data/npc/model/tex/dog_8.c",
    "src/data/npc/model/tex/duk_11.c",
    "src/data/npc/model/tex/duk_8.c",
    "src/data/npc/model/tex/flg_1.c",
    "src/data/npc/model/tex/flg_10.c",
    "src/data/npc/model/tex/flg_11.c",
    "src/data/npc/model/tex/goa_1.c",
    "src/data/npc/model/tex/goa_3.c",
    "src/data/npc/model/tex/goa_6.c",
    "src/data/npc/model/tex/kal_1.c",
    "src/data/npc/model/tex/kal_2.c",
    "src/data/npc/model/tex/wol_1.c",
    "src/data/npc/model/tex/wol_4.c",
    "src/data/npc/model/tex/wol_6.c",
)

# ---------------------------------------------------------------------------
# R3 — the villager MODEL pool. See dc/src/dc_npcmdl.c.
# ---------------------------------------------------------------------------
# Name and row shapes are the contract with dc/src/dc_npcmdl.c, which defines
# the matching typedefs. Change both or neither.
NPCMDL_INC_NAME = "dc_npcmdl_map.inc"
NPCMDL_SRC_DIR = "src/data/npc/model/mdl"

# How many Gfx entries each gs* macro used in that tree expands to. This is the
# ONE number the whole patch table rests on, so every value here was read out of
# the macro definition rather than assumed:
#
#   gsSPMatrix            -> gsDma2p                            gbi.h:1930   1
#   gsSPTexture                                                 gbi.h:2915   1
#   gsSPLoadGeometryMode  -> gsSPGeometryMode                   gbi.h:3023   1
#   gsSPVertex            (F3DEX_GBI_2 form; dc/Makefile:325)   gbi.h:1967   1
#   gsDPSetRenderMode     -> gsSPSetOtherMode                   gbi.h:3071   1
#   gsDPSetCombineLERP                                          gbi.h:3270   1
#   gsDPSetCombineMode    -> gsDPSetCombineLERP                 gbi.h:3295   1
#   gsDPSetTileSize       -> gsDPLoadTileGeneric                gbi.h:3473   1
#   gsDPSetPrimColor                                            gbi.h:3353   1
#   gsSPNTrianglesInit_5b                        gbi_extensions.h:1196       1
#   gsSPNTriangles_5b                            gbi_extensions.h:1190       1
#   gsSPEndDisplayList                                          gbi.h:3002   1
#   gsDPLoadTextureBlock_4b_Dolphin              gbi_extensions.h:1133       2
#
# ⚠️ THE LAST ONE IS WHY THIS TABLE EXISTS. It is a comma pair —
# gsDPSetTextureImage_Dolphin THEN gsDPSetTile_Dolphin — so it contributes TWO
# Gfx, and there are 1,619 of them in the tree. "one macro, one Gfx" would put
# every patch after the first texture load in a list off by one or more, which
# writes a slot address over a triangle packet. A macro that is not in this
# table is a HARD ERROR, never a default of 1.
NPCMDL_GFX_WORDS = {
    "gsSPMatrix": 1,
    "gsSPTexture": 1,
    "gsSPLoadGeometryMode": 1,
    "gsSPVertex": 1,
    "gsDPSetRenderMode": 1,
    "gsDPSetCombineLERP": 1,
    "gsDPSetCombineMode": 1,
    "gsDPSetTileSize": 1,
    "gsDPSetPrimColor": 1,
    "gsSPNTrianglesInit_5b": 1,
    "gsSPNTriangles_5b": 1,
    "gsSPEndDisplayList": 1,
    "gsDPLoadTextureBlock_4b_Dolphin": 2,
}

# sizeof(Vtx) — gbi.h:1096-1100, a union of Vtx_t (16 B) with a long long. The
# .c files spell their bound `<sp>_v[0x1430 / sizeof(Vtx)]`, so this is what
# turns a gsSPVertex element index into a byte offset. Pinned on the other side
# by make_src_shrink.py's npcmdl layout assert, which does have the header.
NPCMDL_VTX_SIZE = 16

# The checksums, same rule as BGTEX_ROWS_EXPECTED. Measured 2026-08-05 across
# all 72 src/data/npc/model/mdl/*.c:
#     72   species files, one `static Vtx <sp>_v[]` each, 438,640 B
#    940   display lists, == the gsSPEndDisplayList count, each named by
#          exactly one joint of exactly one skeleton
#  2,068   gsSPVertex words, all of the form `&<sp>_v[N]`, no exceptions
#     32   species reachable from a VILLAGER row (194,400 B) -- what R3 pools
#     40   species reachable ONLY from a SPECIAL row (244,240 B) -- never pooled
# A selector that stops matching would emit a SHORT patch table, and a species
# whose lists are half-patched draws half its geometry out of another animal's
# slot. That must fail the build, not the frame.
NPCMDL_FILES_EXPECTED = 72
NPCMDL_LISTS_EXPECTED = 940
NPCMDL_VTXCMD_EXPECTED = 2068
NPCMDL_VILLAGER_EXPECTED = 32

# --npcmdl-pool 0 puts this back on the keep list. ONE file: of the twelve
# mdl/*.c the keep lists name, eleven (end_1, hgh_1, kab_1, mnk_1, xct_1 in
# keeplist-opening.txt; rcc_1, rcd_1, rcf_1, rcn_1, rcs_1, tuk_1 in
# make_keeplist_town.py's EXTRA_SOURCES) are SPECIAL-NPC skeletons that R3 never
# pools and that stay kept in both directions. cbr_1 is the only villager model
# that was ever resident, so turning the switch off restores the OLD behaviour
# exactly — 31 villager species drawing as a black spiky mess included.
NPCMDL_KEEP_RESTORE = (
    "src/data/npc/model/mdl/cbr_1.c",
)

# ---------------------------------------------------------------------------
# T1 — the texture PROBE map. See dc/src/dc_texpool.c and the module docstring.
# ---------------------------------------------------------------------------
# Name and row shape are the contract with dc/src/dc_texpool.c, which defines the
# matching typedef. Change both or neither.
TEXPOOL_INC_NAME = "dc_texpool_map.inc"
TEXPOOL_NPC_DIR = "src/data/npc/"

# The TEXTURE IMAGE operand position (0-based) of every display-list macro in
# this tree that names one. Every entry was read out of the macro's own
# definition, never assumed — the same rule as NPCMDL_GFX_WORDS above, and for
# the same reason: a wrong index silently harvests the WRONG ARGUMENT (a format
# enum, a width) and either drops real textures or invents rows for constants.
#
#   gsDPSetTextureImage_Dolphin(fmt, siz, w, h, img)   gbi_extensions.h:1089  -> 4
#   gsDPLoadTextureBlock_4b_Dolphin(timg, ...)         gbi_extensions.h:1133  -> 0
#   gDPLoadTextureBlock_8b_Dolphin(pkt, timg, ...)     gbi_extensions.h:1143  -> 1
#   gDPLoadTextureTile_4b_Dolphin(pkt, timg, ...)      gbi_extensions.h:1149  -> 1
#   gDPLoadTextureBlockS(pkt, timg, ...)               gbi.h:3600             -> 1
#   gDPLoadTextureTile(pkt, timg, ...)                 gbi.h:4066             -> 1
# The `g`-prefixed (packet) forms take `pkt` first, so their index is one higher
# than the `gs`-prefixed (static) form of the same macro. A macro matching
# SetTextureImage/LoadTextureBlock/LoadTextureTile that is NOT in this table is a
# HARD ERROR, never a default — the same discipline NPCMDL_GFX_WORDS uses.
TEXPOOL_IMG_ARG = {
    "gsDPSetTextureImage_Dolphin":      4,
    "gDPSetTextureImage_Dolphin":       5,
    "gsDPSetTextureImage":              3,
    "gDPSetTextureImage":               4,
    "gsDPLoadTextureBlock_4b_Dolphin":  0,
    "gDPLoadTextureBlock_4b_Dolphin":   1,
    "gsDPLoadTextureBlock_8b_Dolphin":  0,
    "gDPLoadTextureBlock_8b_Dolphin":   1,
    "gsDPLoadTextureTile_4b_Dolphin":   0,
    "gDPLoadTextureTile_4b_Dolphin":    1,
    "gsDPLoadTextureBlock_4b":          0,
    "gDPLoadTextureBlock_4b":           1,
    "gsDPLoadTextureBlock":             0,
    "gDPLoadTextureBlock":              1,
    "gsDPLoadTextureBlockS":            0,
    "gDPLoadTextureBlockS":             1,
    "gsDPLoadTextureTile":              0,
    "gDPLoadTextureTile":               1,
}

# The checksums, same rule as BGTEX_ROWS_EXPECTED: a selector that silently stops
# matching produces a probe that measures a SMALLER population than it claims and
# reports a reassuring `interior=0` about textures it never looked at. Measured
# 2026-08-06 against this tree.
#
# ⚠️ NONE of these is keep-list-derived. The resident/stubbed split IS reported
# (and is emitted as a per-row flag), but it is deliberately NOT a checksum: it
# changes every time make_keeplist_town.py is re-run, and a hard error on it
# would break the build on every legitimate regeneration.
TEXPOOL_ROWS_EXPECTED = 6068
TEXPOOL_BYTES_EXPECTED = 2992480
# The four exclusion counts. Asserted individually so that a change in ANY of
# them is attributable, rather than surfacing as a single wrong row total.
TEXPOOL_PAL_EXCLUDED_EXPECTED = 38     # swap != 0, i.e. TLUTs. Must stay resident.
TEXPOOL_SEG_EXCLUDED_EXPECTED = 25     # 1 hnw_tmem_txt + 24 pointer-table
                                       # segment bases; see the note below
TEXPOOL_STATIC_EXCLUDED_EXPECTED = 6   # ctl_att_w1..w6_tex, src/static/dvderr.c
TEXPOOL_NPC_EXCLUDED_EXPECTED = 0      # R2's domain; asserted at 0

# Comments must go before anything is scanned, exactly as R3 does it: this tree
# documents macro calls INSIDE comments (src/game/m_play.c:59 is a
# `gDPSetTextureImage_Dolphin(..., G_IM_SIZ_16b, ...)` with three arguments), and
# such a line parses as a malformed call rather than as prose.
TEXPOOL_COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
TEXPOOL_CALL_RE = re.compile(r"\b(g[sD]?DP[A-Za-z0-9_]*)\s*\(")
TEXPOOL_TEXMACRO_RE = re.compile(
    r"(SetTextureImage|LoadTextureBlock|LoadTextureTile)")
TEXPOOL_IDENT_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*$")
# gSPSegment(pkt, num, base) — the third argument is the symbol a segment
# register is pointed at, and such a symbol is reached through the register
# rather than by its own address.
TEXPOOL_SEGMENT_RE = re.compile(
    r"gSPSegment\(\s*[^,]+,\s*[^,]+,\s*([A-Za-z_][A-Za-z0-9_]*)\s*\)")

# 🔴 AND THE ONE ABOVE IS NOT ENOUGH, FOUND 2026-08-09 BY A STATIC SWEEP.
#
# It only matches a symbol written LITERALLY as gSPSegment()'s third argument.
# 24 texture symbols are written as `tool_pen_table[i]` or `tex` instead —
# collected into a file-static pointer table first, indexed, and only then
# handed to gSPSegment:
#
#   src/game/m_design_ovl.c:2214-2274   six tables (pen/nuri/waku/mark/kao/
#                                       suuji), 21 symbols, the design editor
#   src/actor/ac_shrine_draw.c_inc:40   leaf_texture_table, 3 symbols
#
# They are SAFE TODAY only because every anime-segment placeholder in
# include/libforest/gbi_extensions.h:36-47 is SEGMENT_ADDR(SEG, 0) — offset
# zero, verified by grep — so the address the PVR is handed at bind time equals
# the symbol's own address and T1's lookup matches it anyway. That is a
# property of the game data, not of T1, and if it ever stopped holding the
# failure would be SILENT AND PRETTY: the lookup misses, the loader falls back
# to hashing a one-byte array, and the decoder draws whatever the linker put
# next. 25,856 B is not worth owning that.
#
# So: any eligible symbol that appears as a bare element of a brace initialiser
# whose members are ALL plain identifiers is treated as segment-reached and
# excluded. That is the general shape of "this array is found through a table",
# it self-maintains as the tree moves, and on this tree it selects exactly the
# 24 the sweep found and nothing else.
TEXPOOL_PTRTBL_RE = re.compile(r"=\s*\{([^{}]*)\}", re.S)


def scan_pointer_table_symbols():
    """Symbols reached through a pointer table rather than by their own name."""
    out = set()
    for pat in ("*.c", "*.c_inc"):
        for f in sorted(SRC.rglob(pat)):
            text = f.read_text(encoding="utf-8", errors="surrogateescape")
            if "{" not in text:
                continue
            text = TEXPOOL_COMMENT_RE.sub(
                lambda m: " " * len(m.group(0)), text)
            for m in TEXPOOL_PTRTBL_RE.finditer(text):
                parts = [p.strip() for p in m.group(1).split(",")]
                parts = [p for p in parts if p]
                if parts and all(TEXPOOL_IDENT_RE.match(p) for p in parts):
                    out.update(parts)
    return out


def _texpool_call_args(text, lparen):
    """The argument list of a macro call whose '(' is at `lparen`.

    Depth-aware, so a nested call or a parenthesised expression inside an
    argument does not split it. Returns [] if the call is unterminated.
    """
    depth = 0
    cur = ""
    out = []
    i = lparen
    while i < len(text):
        c = text[i]
        if c == "(":
            depth += 1
            if depth == 1:
                i += 1
                continue
        elif c == ")":
            depth -= 1
            if depth == 0:
                out.append(cur)
                return [a.strip() for a in out]
        if depth == 1 and c == ",":
            out.append(cur)
            cur = ""
        else:
            cur += c
        i += 1
    return []


def scan_texture_operands():
    """Symbols named as a TEXTURE IMAGE operand by some display list in src/.

    Returns {symbol: n_sites}. This is the population T1 would serve, derived
    from the game's own display lists rather than from a name heuristic — the
    suffix `_txt` alone spans animation data and segment bases as well as
    textures, and `_tex` misses the Dolphin tile macros entirely.
    """
    names = {}
    unknown = {}
    for pat in ("*.c", "*.c_inc"):
        for f in SRC.rglob(pat):
            text = TEXPOOL_COMMENT_RE.sub(
                " ", f.read_text(encoding="utf-8", errors="surrogateescape"))
            for m in TEXPOOL_CALL_RE.finditer(text):
                macro = m.group(1)
                if macro not in TEXPOOL_IMG_ARG:
                    if TEXPOOL_TEXMACRO_RE.search(macro):
                        unknown[macro] = unknown.get(macro, 0) + 1
                    continue
                args = _texpool_call_args(text, m.end() - 1)
                idx = TEXPOOL_IMG_ARG[macro]
                if idx >= len(args):
                    unknown["%s (only %d args)" % (macro, len(args))] = \
                        unknown.get("%s (only %d args)" % (macro, len(args)), 0) + 1
                    continue
                operand = args[idx]
                if TEXPOOL_IDENT_RE.match(operand):
                    names[operand] = names.get(operand, 0) + 1
    if unknown:
        raise SystemExit(
            "make_stub_data T1: found texture macro(s) not in TEXPOOL_IMG_ARG:\n"
            + "".join("    %s  x%d\n" % (k, v)
                      for k, v in sorted(unknown.items()))
            + "  Read each macro's definition and add the 0-based position of "
              "its texture\n  IMAGE operand. Defaulting to 0 would harvest a "
              "format enum as if it were a\n  symbol; skipping it would drop "
              "real textures from the probe's population\n  and report a "
              "reassuring interior=0 about binds it never examined.")
    return names


def scan_gsp_segment_symbols():
    """Symbols used as the third argument of gSPSegment anywhere in src/.

    Such a symbol is reached through a segment register, so the address the
    renderer sees is not the symbol's own and it can never be keyed by it.
    """
    out = set()
    for pat in ("*.c", "*.c_inc"):
        for f in SRC.rglob(pat):
            text = TEXPOOL_COMMENT_RE.sub(
                " ", f.read_text(encoding="utf-8", errors="surrogateescape"))
            out.update(TEXPOOL_SEGMENT_RE.findall(text))
    return out


def scan_asset_declarations():
    """Every `#ifdef TARGET_PC` asset array in src/, with its owner and linkage.

    Returns {symbol: (byte_size, repo_relative_owner, is_static)}. .c_inc files
    are scanned alongside .c: the generator has been bitten twice by treating a
    .c_inc as invisible (kb/traps.md — the dialogue balloon and the reply box).
    """
    out = {}
    for pat in ("*.c", "*.c_inc"):
        for f in sorted(SRC.rglob(pat)):
            text = f.read_text(encoding="utf-8", errors="surrogateescape")
            if "#ifdef TARGET_PC" not in text:
                continue
            globs, stats, _ = scan_declarations(text)
            rel = str(f.relative_to(REPO))
            for name, size in globs:
                out[name] = (size, rel, 0)
            for name, size in stats:
                out[name] = (size, rel, 1)
    return out


def texpool_kept_files(keep_paths):
    """repo-relative file -> keep prefixes, for every file the keep list keeps.

    ⚠️ THE CALLER MUST HAVE PARSED THE KEEP LIST WITH parse_keep(), and this
    function deliberately does not re-implement it. MEASURED 2026-08-06, and it
    cost a full analysis cycle: tools/dcstub/keeplist-town.txt is NEWLINE
    separated with '#' comment lines and documents its own consumption on its
    line 15 (`grep -v '^#' … | paste -sd:`). Splitting the raw file on [:,]
    instead — which is what parse_keep() does to an ALREADY-JOINED string —
    collapses its 810 lines into 41 multi-line blobs, none of which equals any
    real path, and every residency test then misses. The result was a confident
    `resident = 0 rows / 0 B`, which is a plausible-looking number and wrong.
    A silent zero that reads as a real answer is the failure class this project
    keeps paying for.

    The .c_inc files a kept TU includes are kept with it, for the reason
    cinc_includes() documents: their arrays are `static` and are filled from
    inside the TU.
    """
    kept = {}
    for rel, prefixes in keep_paths:
        kept[rel] = prefixes
        f = REPO / rel
        text = f.read_text(encoding="utf-8", errors="surrogateescape")
        for cinc_rel, _cinc_path in cinc_includes(text, f):
            kept.setdefault(cinc_rel, prefixes)
    return kept


def texpool_rows(table):
    """The T1-eligible texture rows: [(name, bin_path, size, rom_off, owner)].

    SPLIT OUT OF emit_texpool_map() ON PURPOSE. main() has to know this set
    BEFORE it stubs anything — every symbol here goes into DEMAND_STUB, which
    keep_symbol() consults — and the emitter has to know the residency that
    results, which is downstream of that. One scan, two consumers, no chance of
    the loader and the keep list disagreeing about which symbols T1 owns.

    Runs every checksum the emitter used to run: a selector that silently stops
    matching does not fail, it measures a SMALLER population than it claims.
    """
    operands = scan_texture_operands()
    segment_syms = scan_gsp_segment_symbols() | scan_pointer_table_symbols()
    decls = scan_asset_declarations()

    n_pal = n_seg = n_static = n_npc = 0
    rows = []
    for name in sorted(operands):
        row = table.get(name)
        if row is None:
            # Named by a display list but with no s_assets[] row: not loadable
            # off the disc, so T1 could never serve it. Not an error — 599 such
            # operands exist (scratch buffers, framebuffers, segment bases).
            continue
        decl = decls.get(name)
        if decl is None:
            raise SystemExit(
                "make_stub_data T1: %s has an s_assets[] row but no "
                "`#ifdef TARGET_PC`\n  declaration in src/. The probe keys on "
                "the symbol's ADDRESS, so a row it\n  cannot take the address "
                "of is a contradiction — re-derive the scan rather\n  than "
                "skipping it." % name)
        _size, owner, is_static = decl
        bin_path, tsize, off, rom_src, swap = row

        if swap != 0:
            # A TLUT, reached through the texture-image operand because that is
            # how Dolphin loads one. Palettes stay RESIDENT: a CI texture
            # decoded against a missing TLUT fails SILENTLY
            # (dc_pvr_texture.c:347 and the DC_TEX_LOG note at :1200-1208).
            n_pal += 1
            continue
        if name in segment_syms:
            n_seg += 1
            continue
        if is_static:
            # File-static: dc/ cannot name it, so the map cannot take its
            # address and the loader could never key on it.
            n_static += 1
            continue
        if owner.startswith(TEXPOOL_NPC_DIR):
            n_npc += 1
            continue

        if rom_src != 0:
            raise SystemExit(
                "make_stub_data T1: %s has rom_src=%d, expected 0. Every "
                "texture in this\n  tree lives in foresta.rel and is a pure "
                "pread; a second source means the\n  loader T1 is being sized "
                "for needs another case. Re-derive it rather than\n  emitting "
                "the row." % (name, rom_src))
        if tsize > 0xFFFF:
            raise SystemExit(
                "make_stub_data T1: %s is %d B, which overflows the 16-bit "
                "`size` field of\n  dc_texpool_row_t. Widen that struct in "
                "dc/src/dc_texpool.c and this check\n  together." % (name, tsize))
        if _size != tsize:
            raise SystemExit(
                "make_stub_data T1: %s is declared 0x%X but its s_assets[] row "
                "says 0x%X.\n  The loader stages `size` bytes from the ROW and "
                "the decoder reads what the\n  DECLARATION implies, so a "
                "disagreement is an over-read waiting to happen.\n  Re-derive, "
                "do not pick one." % (name, _size, tsize))

        rows.append((name, bin_path, tsize, off, owner))

    n_bytes = sum(r[2] for r in rows)
    if len(rows) != TEXPOOL_ROWS_EXPECTED or n_bytes != TEXPOOL_BYTES_EXPECTED:
        raise SystemExit(
            "make_stub_data T1: emitted %d rows / %d B, expected %d / %d.\n"
            "  Same checksum rule as BGTEX_ROWS_EXPECTED: a selector that stops\n"
            "  matching does not fail, it measures a SMALLER population than it\n"
            "  claims and then reports a reassuring interior=0 about binds it\n"
            "  never examined. Re-derive TEXPOOL_ROWS_EXPECTED /\n"
            "  TEXPOOL_BYTES_EXPECTED against the tree; do not relax them."
            % (len(rows), n_bytes, TEXPOOL_ROWS_EXPECTED, TEXPOOL_BYTES_EXPECTED))
    for got, want, what in (
            (n_pal, TEXPOOL_PAL_EXCLUDED_EXPECTED, "palette (swap != 0)"),
            (n_seg, TEXPOOL_SEG_EXCLUDED_EXPECTED, "gSPSegment-bound"),
            (n_static, TEXPOOL_STATIC_EXCLUDED_EXPECTED, "file-static"),
            (n_npc, TEXPOOL_NPC_EXCLUDED_EXPECTED, "src/data/npc/**")):
        if got != want:
            raise SystemExit(
                "make_stub_data T1: excluded %d %s row(s), expected %d.\n"
                "  Each exclusion is checked separately so a change is "
                "ATTRIBUTABLE rather\n  than surfacing as a wrong total. Work "
                "out which symbol moved and why\n  before adjusting the "
                "constant." % (got, what, want))
    return rows


def emit_texpool_map(rows, keep_paths, probe, demand):
    """Build the text of dc_texpool_map.inc. Returns (text, n_rows, n_bytes).

    One row per texture the display lists name by address, keyed by that address
    — which is exactly what dc_gx_backend_texture_upload() receives as `data`
    (dc_gx.c -> dc_pvr_texture.c). dc/src/dc_texpool.c is the consumer, defines
    the dc_texpool_row_t this is written against, and does the lookup.

    Neither `probe` nor `demand` emits only the count macro and no table, so
    nothing is left unreferenced and the TU compiles to nothing.

    ⚠️ THE `name` FIELD IS EMITTED ONLY UNDER `probe`, and the struct in
    dc_texpool.c is #if'd to match. 6,092 names are ~120 KB of .rodata; a
    shipping build cannot spend that to make a diagnostic readable, and a
    diagnostic build cannot do without it.
    """
    L = []
    L.append("/* GENERATED by tools/dcstub/make_stub_data.py — DO NOT EDIT. */")
    L.append("/*")
    L.append(" * T1's map. Each row is one texture the display lists name by")
    L.append(" * ADDRESS — the key dc_gx_backend_texture_upload() receives as")
    L.append(" * `data` — joined to the s_assets[] row its bytes live in.")
    L.append(" *")
    L.append(" * Under --texpool-demand=1 (the LOADER) this is the whole")
    L.append(" * mechanism: the row INDEX is the texture's cache key, so the")
    L.append(" * array is never read on a bind and therefore never has to be")
    L.append(" * resident. dc/src/dc_texpool.c stages it off /cd/foresta.rel on")
    L.append(" * a cache miss.")
    L.append(" *")
    L.append(" * Under --texpool=1 (the PROBE) the same map is used to COUNT,")
    L.append(" * per bind, whether that premise still holds. The probe moves no")
    L.append(" * bytes and changes no pixel.")
    L.append(" *")
    L.append(" * `kept` is 1 when the row is still RESIDENT under the keep list")
    L.append(" * this tree was generated with. Under the loader that is 0 for")
    L.append(" * every row by construction — DEMAND_STUB beats a keep-list")
    L.append(" * entry (make_stub_data.py, keep_symbol()).")
    L.append(" */")
    L.append("#ifndef DC_TEXPOOL_MAP_INC_")
    L.append("#define DC_TEXPOOL_MAP_INC_")
    L.append("")

    if not (probe or demand):
        L.append("/* --texpool 0 --texpool-demand 0: neither half was asked")
        L.append(" * for. No table is emitted, so nothing here is left")
        L.append(" * unreferenced and dc/src/dc_texpool.c compiles to nothing. */")
        L.append("#define DC_TEXPOOL_MAP_N 0")
        L.append("")
        L.append("#endif /* DC_TEXPOOL_MAP_INC_ */")
        L.append("")
        return "\n".join(L), 0, 0

    n_bytes = sum(r[2] for r in rows)
    kept_flag = [1 if _texpool_is_resident(name, owner, keep_paths) else 0
                 for name, _bin, _sz, _off, owner in rows]
    n_kept = sum(kept_flag)
    b_kept = sum(r[2] for r, k in zip(rows, kept_flag) if k)
    max_b = max(r[2] for r in rows)

    if demand and n_kept:
        # DEMAND_STUB is consulted by keep_symbol(), which every residency
        # decision runs through, so a row that is BOTH demand-loaded and
        # resident is a contradiction — it would mean 2x the .bss and a disc
        # read nobody needs. Hard-error rather than emit it.
        bad = [r[0] for r, k in zip(rows, kept_flag) if k][:5]
        raise SystemExit(
            "make_stub_data T1: --texpool-demand=1 but %d row(s) are still "
            "resident,\n  e.g. %s. DEMAND_STUB is supposed to beat every keep-"
            "list entry; if one\n  of these is reached some other way, exclude "
            "it from texpool_rows()\n  rather than letting it be loaded twice."
            % (n_kept, ", ".join(bad)))

    L.append("#define DC_TEXPOOL_MAP_N          {}".format(len(rows)))
    L.append("/* The largest row. dc/src/dc_texpool.c sizes its ONE staging")
    L.append(" * buffer from this, so a row that does not fit is a build-time")
    L.append(" * impossibility rather than a runtime over-read. */")
    L.append("#define DC_TEXPOOL_MAX_B          {}".format(max_b))
    L.append("/* Residency under the keep list this tree was generated with. */")
    L.append("#define DC_TEXPOOL_RESIDENT_N     {}".format(n_kept))
    L.append("#define DC_TEXPOOL_RESIDENT_B     {}".format(b_kept))
    L.append("#define DC_TEXPOOL_STUBBED_N      {}".format(len(rows) - n_kept))
    L.append("#define DC_TEXPOOL_STUBBED_B      {}".format(n_bytes - b_kept))
    L.append("")
    # `extern char x[]` regardless of the vendored element type, exactly as
    # dc_bgtex_map.inc declares its u16 palettes: only the ADDRESS is used, no TU
    # sees both spellings, and this .inc must not drag the decomp headers in.
    for name, _bin, _sz, _off, _owner in rows:
        L.append("extern char {}[];".format(name))
    L.append("")
    L.append("static const dc_texpool_row_t dc_texpool_map[] = {")
    for (name, _bin, sz, off, _owner), kept in zip(rows, kept_flag):
        if probe:
            L.append('    {{ (const void*){}, "{}", {}u, {}u, {}, 0 }},'.format(
                name, name, off, sz, kept))
        else:
            L.append('    {{ (const void*){}, {}u, {}u, {}, 0 }},'.format(
                name, off, sz, kept))
    L.append("};")
    L.append("")
    L.append("#endif /* DC_TEXPOOL_MAP_INC_ */")
    L.append("")
    return "\n".join(L), len(rows), n_bytes


def _texpool_is_resident(name, owner, keep_paths):
    """Is `name` full-size in the image the keep list describes?

    Resolved through parse_keep()'s own output and keep_symbol(), never by
    re-parsing the keep-list file — see texpool_kept_files() for the measured
    reason that matters.
    """
    kept = _texpool_is_resident.cache
    if kept is None or _texpool_is_resident.key is not keep_paths:
        kept = texpool_kept_files(keep_paths)
        _texpool_is_resident.cache = kept
        _texpool_is_resident.key = keep_paths
    if owner not in kept:
        return False
    # ⚠️ ALWAYS through keep_symbol(), even for a whole-file keep with no
    # prefixes. It used to short-circuit to True there, which was right until
    # T1: DEMAND_STUB is consulted inside keep_symbol() and a whole-file keep is
    # exactly the case it has to beat.
    return keep_symbol(name, kept[owner])


_texpool_is_resident.cache = None
_texpool_is_resident.key = None


# ---------------------------------------------------------------------------
# The default allowlist: everything src/actor/ac_animal_logo.c draws.
#
# Derived by reading that actor, not guessed. aAL_actor_ct() builds three cKF
# skeletons from cKF_bs_r_logo_us_{animal,cros,sing}; aAL_back_draw() runs
# logo_us_back{A,B,C,D}_model; aAL_tm_draw() runs logo_us_tm_model;
# aAL_copyright_draw() blits log_win_nintendo{1,2,3}_tex; aAL_press_start_draw()
# blits log_win_logo{3,4}_tex. Every gsSPVertex / texture operand inside those
# display lists resolves inside these ten files — verified, there are no
# cross-TU references to chase. The cKF_ba_r_logo_us_* animation tables are
# plain initialised .data with no #ifdef TARGET_PC block, so they are already
# full size and do not belong here.
# ---------------------------------------------------------------------------
DEFAULT_KEEP = (
    "src/data/model/logo_us_animal.c",
    "src/data/model/logo_us_cros.c",
    "src/data/model/logo_us_sing.c",
    "src/data/model/logo_us_back.c",
    "src/data/model/logo_us_tm.c",
    "src/data/model/log_win_nintendo1_tex.c",
    "src/data/model/log_win_nintendo2_tex.c",
    "src/data/model/log_win_nintendo3_tex.c",
    "src/data/model/log_win_logo3_tex.c",
    "src/data/model/log_win_logo4_tex.c",
)

# ---------------------------------------------------------------------------
# NEUTRALISE: passes that write FULL-SIZE data into destinations this tool has
# just shrunk to [1].
#
# This is the same hazard that made dc_main.c skip pc_assets_init() under
# DC_ASSET_STUB, and it has exactly one more instance: boot.c runs four
# endian-fixup passes over asset arrays immediately before the HotStartEntry
# loop. Their sizes are compiled-in constants, so shrinking the arrays does
# not shrink the loops:
#
#   pc_bswap_house_pos_list()    0x978 B of u16 swaps into u8 x[1]  -> 2,423 B
#   pc_bswap_u8_tlut_palettes()  14 palettes x 32 B into u8 x[1]    ->   434 B
#   pc_bswap_raw_display_lists() 3 lists, 112 B total into u8 x[1]  ->   109 B
#   mFM_InitActableEndian()      walks 6 actables to a SENTINEL that
#                                no longer exists -> unbounded
#
# MEASURED CONSEQUENCE, not a theoretical one: with these live, boot.c's own
# `HotStartEntry` was overwritten with 0x64b3418c and the game jumped to it and
# died on an illegal instruction, ~3,000 B away from the arrays being swapped.
# It reproduced with and without the keep list and moved when .bss moved, which
# is what a wild write looks like.
#
# Nothing is lost by skipping them: under DC_ASSET_STUB the bytes they would
# swap are zeros. Keep-list assets are byte-swapped by dc_stub_keep_load_one()
# as they are read, so they must NOT be swapped a second time here either.
#
# Each rule is anchored and declares its match count; a mismatch is a hard
# error, never a warning, because a rule that silently stops matching gives
# back exactly the corruption it was written to prevent.
# ---------------------------------------------------------------------------
NEUTRALISE = {
    "src/static/boot.c": [
        (
            "  pc_bswap_raw_display_lists();\n"
            "  mFM_InitActableEndian();\n"
            "  pc_bswap_u8_tlut_palettes();\n"
            "  pc_bswap_house_pos_list();\n",
            1,
            "  /* Rewritten by tools/dcstub/make_stub_data.py: these four\n"
            "   * passes swap FULL-SIZE asset data in place, and under\n"
            "   * DC_ASSET_STUB every destination is [1]. See NEUTRALISE in\n"
            "   * that file for the measured corruption this prevents. */\n"
            "#ifndef DC_ASSET_STUB\n"
            "  pc_bswap_raw_display_lists();\n"
            "  mFM_InitActableEndian();\n"
            "  pc_bswap_u8_tlut_palettes();\n"
            "  pc_bswap_house_pos_list();\n"
            "#else\n"
            "  OSReport(\"[DC] DC_ASSET_STUB: skipping 4 endian passes over \"\n"
            "           \"stubbed assets (make_stub_data.py NEUTRALISE)\\n\");\n"
            "#endif\n",
        ),
    ],
}


def neutralise_file(rel, text):
    """Apply this file's NEUTRALISE rules. Hard-errors on a count mismatch."""
    n_applied = 0
    for pattern, want, replacement in NEUTRALISE[rel]:
        got = text.count(pattern)
        if got != want:
            raise SystemExit(
                "make_stub_data.py: NEUTRALISE rule for {} matched {} time(s), "
                "expected {}.\nThe source moved under the rule. Do NOT relax "
                "the count -- re-anchor the pattern, or the stub build goes "
                "back to spraying .bss.\nPattern was:\n{}".format(
                    rel, got, want, pattern
                )
            )
        text = text.replace(pattern, replacement)
        n_applied += 1
    return text, n_applied


# The declaration line the generator emits, e.g.
#   Vtx act_ant_v[0xB0 / sizeof(Vtx)];
#   u8 act_ant_tex[0x200] ATTRIBUTE_ALIGN(32);
#   static u16 foo_pal[0x20 / sizeof(u16)] ATTRIBUTE_ALIGN(32);
# group 'size' is always the BYTE size, whether or not a / sizeof() follows.
DECL_RE = re.compile(
    r"^(?P<indent>[ \t]*)"
    r"(?P<head>(?:static[ \t]+)?[A-Za-z_][A-Za-z0-9_ \t\*]*?)"
    r"[ \t]+(?P<name>[A-Za-z_][A-Za-z0-9_]*)"
    r"[ \t]*\[[ \t]*(?P<size>0[xX][0-9A-Fa-f]+)"
    r"(?P<div>[ \t]*/[ \t]*sizeof\([^)]*\))?[ \t]*\]"
    r"(?P<tail>[^;]*);[ \t]*$"
)

IFDEF_RE = re.compile(r"^([ \t]*)#ifdef[ \t]+TARGET_PC[ \t]*$")
LOAD_CALL_RE = re.compile(r"^([ \t]*)pc_load_asset\(")
PC_LOAD_IDENT_RE = re.compile(r"\bpc_load_asset\b")
PER_FILE_INIT_RE = re.compile(
    r"^[ \t]*void[ \t]+(_pc_load_src_[A-Za-z0-9_]*)[ \t]*\([ \t]*void[ \t]*\)"
)

# A .c_inc that a kept TU #includes. The path is written relative in a way that
# only resolves through the -I search list, so only the basename is trusted
# here and it is looked up next to the including .c.
INCLUDE_CINC_RE = re.compile(r'^[ \t]*#[ \t]*include[ \t]+"([^"]*\.c_inc)"')

# One generated load, e.g.
#   pc_load_asset("assets/msg/con_kaiwa2_w2_tex.bin", con_kaiwa2_w2_tex,
#                 0x1000, 0x2E79A0, 0, 0);
PC_LOAD_CALL_RE = re.compile(
    r'pc_load_asset\([ \t]*"(?P<path>[^"]*)"[ \t]*,'
    r"[ \t]*(?P<dest>[A-Za-z_][A-Za-z0-9_]*)[ \t]*,"
    r"[ \t]*(?P<size>0[xX][0-9A-Fa-f]+|\d+)[ \t]*,"
    r"[ \t]*(?P<off>0[xX][0-9A-Fa-f]+|\d+)[ \t]*,"
    r"[ \t]*(?P<src>-?\d+)[ \t]*,"
    r"[ \t]*(?P<swap>-?\d+)[ \t]*\)"
)


def cinc_includes(text, c_path):
    """The .c_inc files a TU #includes that contain pc_load_asset() calls.

    WHY THIS EXISTS. The generator puts some assets' destination arrays AND
    their _pc_load_src_*() loader inside a .c_inc rather than the .c that
    includes it — src/game/m_msg.c is the case that exposed it, with the whole
    dialogue balloon (con_kaiwa2_w1/w2/w3_tex, con_namefuti_TXT, con_kaiwa2_v,
    con_kaiwaname_v) living in m_msg_data.c_inc.

    Such a file is invisible to every other part of this tool: main() globs
    "*.c" only, so the TU never enters stub.list, census_keeplist.py used to
    drop its symbols for not being stubbable, and scan_declarations() on the .c
    finds neither the arrays nor the loader. The arrays end up correctly sized
    in .bss with NOTHING that fills them — which under DC_ASSET_STUB is worse
    than being stubbed, because it looks right in the map and decodes to a
    transparent rectangle at runtime. MEASURED 2026-08-02: the balloon behind
    every line of NPC dialogue was missing for exactly this reason.

    ⚠️ The arrays are `static`, so dc_stub_keep.inc CANNOT reference them
    directly — that was tried and the link failed with eight undefined
    references. They have to be filled from inside the TU, by calling the
    .c_inc's own _pc_load_src_*(), which means the .c_inc itself must get
    keep_file()'d so its pc_load_asset() calls are redirected. main() writes
    the rewritten copy into the stub tree and dc/Makefile shadows it on the
    include path, exactly as DC_SRC_SHRINK already does for its two .c_inc
    files (see dc/Makefile's "Include-path shadow" note).

    Returns [(repo_relative_path, Path)], in include order, deduplicated.
    """
    out = []
    seen = set()
    for line in text.split("\n"):
        m = INCLUDE_CINC_RE.match(line)
        if not m:
            continue
        inc = (c_path.parent / Path(m.group(1)).name).resolve()
        if inc in seen or not inc.is_file():
            continue
        seen.add(inc)
        if not PC_LOAD_IDENT_RE.search(
            inc.read_text(encoding="utf-8", errors="surrogateescape")
        ):
            continue
        out.append((str(inc.relative_to(REPO)), inc))
    return out


#   {"assets/logo_us_a_tex_txt.bin", logo_us_a_tex_txt, 0x800, 0x8C4380, 0, 0},
ASSET_ROW_RE = re.compile(
    r'^\s*\{\s*"(?P<path>[^"]*)"\s*,\s*'
    r"(?P<dest>[A-Za-z_][A-Za-z0-9_]*)\s*,\s*"
    r"(?P<size>0[xX][0-9A-Fa-f]+|\d+)\s*,\s*"
    r"(?P<off>0[xX][0-9A-Fa-f]+|\d+)\s*,\s*"
    r"(?P<src>-?\d+)\s*,\s*"
    r"(?P<swap>-?\d+)\s*\}\s*,?\s*$"
)


def stub_file(text):
    """Rewrite one TU. Returns (new_text, n_arrays, bytes_saved)."""
    lines = text.split("\n")
    out = list(lines)
    n = 0
    saved = 0
    # Names stubbed in this TU; only their pc_load_asset() calls get neutralised.
    stubbed = set()

    for i, line in enumerate(lines):
        if not IFDEF_RE.match(line):
            continue
        if i + 1 >= len(lines):
            continue
        m = DECL_RE.match(lines[i + 1])
        if not m:
            # Per-file init function block, or something the generator did not
            # write. Leave it exactly as-is.
            continue
        size = int(m.group("size"), 16)
        if size <= 1:
            continue
        out[i + 1] = "{indent}{head} {name}[1]{tail};".format(
            indent=m.group("indent"),
            head=m.group("head").strip(),
            name=m.group("name"),
            tail=m.group("tail"),
        )
        stubbed.add(m.group("name"))
        n += 1
        saved += size

    if stubbed:
        for i, line in enumerate(lines):
            cm = LOAD_CALL_RE.match(line)
            if not cm:
                continue
            # pc_load_asset("path", NAME, 0xSIZE, ...) — second argument.
            arg = line.split(",")
            if len(arg) < 2:
                continue
            dest = arg[1].strip()
            if dest in stubbed:
                out[i] = (
                    cm.group(1)
                    + "/* DC_ASSET_STUB: dest is [1], load suppressed. */"
                )

    return "\n".join(out), n, saved


def scan_declarations(text):
    """Arrays a TU declares under #ifdef TARGET_PC.

    Returns (globals, statics, per_file_inits) where globals/statics are lists
    of (name, byte_size) and per_file_inits is the list of
    _pc_load_src_*() functions the generator appended to the TU.
    """
    lines = text.split("\n")
    globs = []
    stats = []
    inits = []

    for i, line in enumerate(lines):
        if IFDEF_RE.match(line) and i + 1 < len(lines):
            m = DECL_RE.match(lines[i + 1])
            if m:
                size = int(m.group("size"), 16)
                entry = (m.group("name"), size)
                if m.group("head").strip().startswith("static"):
                    stats.append(entry)
                else:
                    globs.append(entry)
        fm = PER_FILE_INIT_RE.match(line)
        if fm:
            inits.append(fm.group(1))

    return globs, stats, inits


def keep_file(text):
    """Prepare a KEPT TU: arrays stay full size, loads get redirected.

    The generator's per-file init calls pc_load_asset(), which on the Dreamcast
    cannot work (see the module docstring). Rename the identifier — both the
    local `extern void pc_load_asset(...)` prototype and every call site — to
    KEEP_LOADER, which dc_main.c defines. Returns (new_text, n_redirected).
    """
    new_text, n = PC_LOAD_IDENT_RE.subn(KEEP_LOADER, text)
    return new_text, n


def parse_asset_table(path):
    """dest symbol -> (bin_path, size, rom_off, rom_src, swap) from s_assets[].

    A symbol that appears twice is dropped: the table is the authority on where
    bytes come from and an ambiguous destination is a bug we must not paper
    over by picking one at random.
    """
    if not path.exists():
        return {}
    table = {}
    dupes = set()
    for line in path.read_text(encoding="utf-8", errors="surrogateescape").split("\n"):
        m = ASSET_ROW_RE.match(line)
        if not m:
            continue
        dest = m.group("dest")
        row = (
            m.group("path"),
            int(m.group("size"), 0),
            int(m.group("off"), 0),
            int(m.group("src")),
            int(m.group("swap")),
        )
        if dest in table and table[dest] != row:
            dupes.add(dest)
        table[dest] = row
    for d in dupes:
        del table[d]
    return table


# ---------------------------------------------------------------------------
# T1's keep-list half — the symbols the LOADER takes over (--texpool-demand)
# ---------------------------------------------------------------------------
# Populated in main() when --texpool-demand=1, from exactly the rows emitted
# into dc_texpool_map.inc. A symbol in here is stubbed to [1] and its
# pc_load_asset() line is suppressed EVEN IF THE KEEP LIST NAMES ITS FILE —
# dc/src/dc_texpool.c reads it off the disc at the moment the PVR binds it.
#
# ⚠️ THIS IS THE ONE PLACE A KEEP-LIST ENTRY DOES NOT MEAN "RESIDENT", so it is
# a single global consulted by keep_symbol() rather than a rule sprinkled over
# the four call sites. R1/R2/R3 avoided needing this by removing whole FILES
# from the list; T1 cannot, because a texture array usually shares its TU with
# the vertex arrays and display lists that must stay.
DEMAND_STUB = set()


def keep_symbol(name, prefixes):
    """Does `name` survive a partial keep's prefix filter?

    Inclusion prefixes are a whitelist; '!'-prefixed ones are a blacklist that
    always wins. With only exclusions, everything not excluded is kept.

    T1's DEMAND_STUB always wins over both: the loader owns those symbols and
    a keep-list entry naming their file must not put them back in .bss.
    """
    if name in DEMAND_STUB:
        return False
    inc = [p for p in prefixes if not p.startswith("!")]
    exc = [p[1:] for p in prefixes if p.startswith("!")]
    if any(name.startswith(p) for p in exc):
        return False
    if not inc:
        return True
    return any(name.startswith(p) for p in inc)


def partial_file(text, prefixes):
    """Prepare a PARTIALLY kept TU: keep matching symbols, stub the rest.

    WHY THIS EXISTS. Keeping is per-FILE, and every src/data/model/obj_s_*.c
    carries BOTH seasons: obj_s_house1.c is 42,624 B of summer geometry and
    42,720 B of winter, and the keep list was buying both. Across the 13
    structures the town keep list keeps, 101,216 B is obj_w_* that a summer
    town can never draw — the season is chosen at ac_shop.c:92-94 and
    ac_shop_draw.c_inc:52. That is the cheapest byte in the whole ledger:
    spending it back pays for every summer structure that is currently a black
    spiky mess (Nook's shop, the museum, the tailor, the shrine, the police
    box).

    Two forms, and for the seasons case the EXCLUSION form is the correct one:

        '#obj_s_'   keep ONLY arrays whose name starts with obj_s_
        '#!obj_w_'  keep everything EXCEPT arrays starting with obj_w_

    Use the exclusion form to drop winter. The inclusion form looks equivalent
    and is not: 3,680 B across nine obj_s_*.c files are season-NEUTRAL and
    named neither obj_s_ nor obj_w_ — obj_kanban_pal, hakushi_tex,
    obj_lotus_leaf_tex_txt, obj_shop4_grass_tex_pic_i4 and friends. '#obj_s_'
    would stub those, and a stubbed palette renders its model in garbage
    colours rather than failing loudly.

    An array kept by these rules stays FULL SIZE and its load is redirected to
    KEEP_LOADER, exactly as keep_file() would do. Every other array is stubbed
    to [1] and its load suppressed, exactly as stub_file() would do.

    ⚠️ This function must reproduce BOTH halves faithfully, because the two
    halves disagree about what to do with a pc_load_asset() line and getting it
    backwards is silent: a stubbed array whose load survives is a full-size
    memcpy into a 1-byte destination (the .bss overrun that kb/traps.md's
    NEUTRALISE section is about), and a kept array whose load is suppressed is
    a correctly-sized array full of zeros, which renders as nothing at all and
    reads as a renderer bug.

    Returns (new_text, n_stubbed, bytes_saved, n_kept, n_redirected).
    """
    lines = text.split("\n")
    out = list(lines)
    n_stub = 0
    saved = 0
    n_kept = 0
    stubbed = set()

    for i, line in enumerate(lines):
        if not IFDEF_RE.match(line):
            continue
        if i + 1 >= len(lines):
            continue
        m = DECL_RE.match(lines[i + 1])
        if not m:
            continue
        name = m.group("name")
        if keep_symbol(name, prefixes):
            n_kept += 1
            continue
        size = int(m.group("size"), 16)
        if size <= 1:
            continue
        out[i + 1] = "{indent}{head} {name}[1]{tail};".format(
            indent=m.group("indent"),
            head=m.group("head").strip(),
            name=name,
            tail=m.group("tail"),
        )
        stubbed.add(name)
        n_stub += 1
        saved += size

    for i, line in enumerate(lines):
        cm = LOAD_CALL_RE.match(line)
        if not cm:
            continue
        arg = line.split(",")
        if len(arg) < 2:
            continue
        dest = arg[1].strip()
        if dest in stubbed:
            out[i] = (cm.group(1)
                      + "/* DC_ASSET_STUB: dest is [1], load suppressed. */")

    # Whatever survived the suppression above belongs to a KEPT array, so it
    # gets the same pc_load_asset -> KEEP_LOADER rename keep_file() applies.
    new_text, n_redirect = PC_LOAD_IDENT_RE.subn(KEEP_LOADER, "\n".join(out))
    return new_text, n_stub, saved, n_kept, n_redirect


def parse_keep(cli_value):
    """Resolve the allowlist. CLI beats env; env unset means the default.

    An entry may carry a '#'-separated symbol-prefix filter:

        src/data/model/obj_s_house1.c#obj_s_

    which keeps only the arrays whose names start with `obj_s_` and stubs the
    rest of that TU. Several prefixes may be given, '#'-separated. Without a
    filter the whole file is kept, which is the historical behaviour.

    Returns (list_of (repo_relative_path, prefixes_tuple), provenance_string).
    """
    if cli_value is not None:
        raw, why = cli_value, "--keep"
    elif "DC_STUB_KEEP" in os.environ:
        raw, why = os.environ["DC_STUB_KEEP"], "DC_STUB_KEEP"
    else:
        return [(p, ()) for p in DEFAULT_KEEP], "built-in default"

    parts = [p.strip() for p in re.split(r"[:,]", raw)]
    parts = [p for p in parts if p]
    if not parts:
        return [], why + " (empty — stub everything)"

    out = []
    for p in parts:
        if "#" in p:
            path, _, filt = p.partition("#")
            prefixes = tuple(x for x in filt.split("#") if x)
            if not prefixes:
                sys.stderr.write(
                    "make_stub_data.py: '{}' has a '#' but no symbol prefix "
                    "after it. That would keep NOTHING in the file, which is "
                    "never what is meant -- drop the '#' to keep all of it.\n"
                    .format(p))
                raise SystemExit(2)
            out.append((path.strip(), prefixes))
        else:
            out.append((p, ()))
    return out, why


def emit_keep_inc(keep_paths, table, rewritten_cinc=None):
    """Build the text of dc_stub_keep.inc for the resolved keep list.

    rewritten_cinc, when given, is the set of repo-relative .c_inc paths this
    run actually wrote into the stub tree. Emitting a loader call for a .c_inc
    that was NOT rewritten is the silent failure that cost the reply box (see
    the .c_inc trap note in main()), so it is a hard error rather than a
    warning.
    """
    L = []
    L.append("/* GENERATED by tools/dcstub/make_stub_data.py — DO NOT EDIT. */")
    L.append("/*")
    L.append(" * Loads the DC_ASSET_STUB keep-list destinations, and nothing else.")
    L.append(" * dc/src/dc_main.c includes this instead of calling pc_assets_init(),")
    L.append(" * which would memcpy full-size assets over [1]-sized stubbed arrays.")
    L.append(" * Every load goes through " + KEEP_LOADER + "() (dc_main.c): a targeted")
    L.append(" * read of `size` bytes at `rom_off` from /cd/foresta.rel or /cd/main.dol,")
    L.append(" * never a whole-ROM residency.")
    L.append(" */")
    L.append("#ifndef DC_STUB_KEEP_INC_")
    L.append("#define DC_STUB_KEEP_INC_")
    L.append("")
    L.append("extern void " + KEEP_LOADER +
             "(const char*, void*, unsigned int, unsigned int, int, int);")
    L.append("")

    calls = []
    n_rows = 0
    n_inits = 0
    n_bytes = 0
    n_unmapped = 0
    n_cinc = 0
    emitted = set()

    for rel, prefixes in keep_paths:
        f = REPO / rel
        text = f.read_text(encoding="utf-8", errors="surrogateescape")
        globs, stats, inits = scan_declarations(text)

        # A partial keep must not emit loader calls for the arrays it stubbed:
        # the destination is [1] bytes and the load is a full-size memcpy into
        # it. This is the same overrun class as kb/traps.md's NEUTRALISE table,
        # and it is the single most dangerous way to get partial keeps wrong.
        # ⚠️ UNCONDITIONAL SINCE T1 — it used to run only `if prefixes:`.
        # keep_symbol() also answers DEMAND_STUB now, and a demand-loaded
        # texture in a WHOLE-file keep has no prefixes at all. Emitting its
        # loader call would memcpy the full asset into a [1]-byte destination,
        # which is the .bss overrun kb/traps.md's NEUTRALISE table is about.
        globs = [(n, s) for (n, s) in globs if keep_symbol(n, prefixes)]
        stats = [(n, s) for (n, s) in stats if keep_symbol(n, prefixes)]

        L.append("/* {}{} */".format(
            rel, "  [only {}]".format("|".join(prefixes)) if prefixes else ""))
        calls.append("    /* {} */".format(rel))

        for name, size in globs:
            row = table.get(name)
            if row is None:
                # Not in the central table and not static: the generator must
                # have loaded it some other way. Say so loudly in the output
                # rather than silently shipping a zeroed array.
                L.append("/*   {}: NOT IN s_assets[] — left zeroed */".format(name))
                n_unmapped += 1
                continue
            bin_path, tsize, off, src, swap = row
            if tsize != size:
                L.append("/*   {}: declared 0x{:X}, table says 0x{:X} — using the"
                         " table */".format(name, size, tsize))
            L.append("extern char {}[];".format(name))
            calls.append(
                '    {}("{}", (void*){}, {}u, {}u, {}, {});'.format(
                    KEEP_LOADER, bin_path, name, tsize, off, src, swap
                )
            )
            emitted.add(name)
            n_rows += 1
            n_bytes += tsize

        # Loaders that live in a .c_inc this TU includes rather than in the .c
        # itself. Their destination arrays are `static`, so they can only be
        # filled from inside the TU — see cinc_includes().
        for cinc_rel, cinc_path in cinc_includes(text, f):
            cinc_text = cinc_path.read_text(
                encoding="utf-8", errors="surrogateescape")
            _, cinc_stats, cinc_inits = scan_declarations(cinc_text)
            if rewritten_cinc is not None and cinc_rel not in rewritten_cinc:
                raise SystemExit(
                    "make_stub_data: {} would get a _pc_load_src_*() call in {}"
                    " but was never rewritten into the stub tree, so its"
                    " pc_load_asset() calls still go to the PC loader and the"
                    " assets silently do not load. Kept via: {}".format(
                        cinc_rel, KEEP_INC_NAME, rel))
            for fn in cinc_inits:
                if fn in emitted:
                    continue
                L.append("extern void {}(void);   /* {} */".format(fn, cinc_rel))
                calls.append("    {}();".format(fn))
                emitted.add(fn)
                n_cinc += 1
                n_bytes += sum(s for _, s in cinc_stats)

        for fn in inits:
            if fn in emitted:
                continue
            emitted.add(fn)
            L.append("extern void {}(void);".format(fn))
            calls.append("    {}();".format(fn))
            n_inits += 1
            # The statics this function fills are full size in the kept TU.
            n_bytes += sum(s for _, s in stats)

        L.append("")

    L.append("static void dc_stub_keep_load(void) {")
    if calls:
        L.extend(calls)
    else:
        L.append("    /* keep list is empty — DC_ASSET_STUB stubs everything. */")
    L.append("}")
    L.append("")
    L.append("#endif /* DC_STUB_KEEP_INC_ */")
    L.append("")

    stats_line = (n_rows, n_inits, n_unmapped, n_bytes, n_cinc)
    return "\n".join(L), stats_line


def emit_bgtex_map(table, demand):
    """Build the text of dc_bgtex_map.inc. Returns (text, n_rows, n_bytes).

    Every s_assets[] row whose destination is an mFM_grd_* symbol, keyed by the
    ADDRESS of that symbol — which is what src/game/m_field_make.c's six segment
    tables hand to the loop, and which stays unique when the array is stubbed to
    [1]. dc/src/dc_bgtex.c is the consumer and carries the argument.

    `demand` False emits only the count macro and no table, so the runtime
    compiles to a plain memmove and nothing is left unreferenced.
    """
    L = []
    L.append("/* GENERATED by tools/dcstub/make_stub_data.py — DO NOT EDIT. */")
    L.append("/*")
    L.append(" * R1: the acre ground textures, read off the disc instead of kept")
    L.append(" * resident. Each row maps a mFM_grd_* SOURCE ARRAY'S ADDRESS — the")
    L.append(" * key src/game/m_field_make.c's segment tables hand to")
    L.append(" * mFM_LoadBGCommonTex()'s copy loop — onto the ROM offset the bytes")
    L.append(" * actually live at. Included by dc/src/dc_bgtex.c, which defines the")
    L.append(" * dc_bgtex_row_t this is written against and does the lookup.")
    L.append(" *")
    L.append(" * The arrays these point at are [1] bytes each under DC_ASSET_STUB;")
    L.append(" * that is the point. Only the address is used.")
    L.append(" */")
    L.append("#ifndef DC_BGTEX_MAP_INC_")
    L.append("#define DC_BGTEX_MAP_INC_")
    L.append("")

    if not demand:
        L.append("/* --bgtex-demand 0: the map is empty and the 27 summer/shared")
        L.append(" * mFM_grd_*.c files are back on the keep list, so every source is")
        L.append(" * resident and dc_bgtex_load() is a memmove. No table is emitted,")
        L.append(" * so nothing here is left unreferenced. */")
        L.append("#define DC_BGTEX_MAP_N 0")
        L.append("")
        L.append("#endif /* DC_BGTEX_MAP_INC_ */")
        L.append("")
        return "\n".join(L), 0, 0

    names = sorted(n for n in table if n.startswith(BGTEX_PREFIX))
    if len(names) != BGTEX_ROWS_EXPECTED:
        raise SystemExit(
            "make_stub_data: found {} '{}*' rows in {}, expected {}.\n"
            "  This count is the table's own checksum — a selector that stops\n"
            "  matching would emit a SHORT map, the missing acres would fall\n"
            "  through dc_bgtex_load()'s memmove out of a [1]-sized array, and\n"
            "  the ground would render as garbage rather than fail. Re-derive\n"
            "  BGTEX_ROWS_EXPECTED against src/game/m_field_make.c's six segment\n"
            "  tables; do not relax it.".format(
                len(names), BGTEX_PREFIX, ASSET_TABLE.name, BGTEX_ROWS_EXPECTED))

    n_bytes = 0
    # `extern char x[]` regardless of the vendored element type (three of these
    # are u16 palettes). Only the address is taken, no TU sees both spellings,
    # and dc_stub_keep.inc above already declares kept globals the same way.
    for name in names:
        L.append("extern char {}[];".format(name))
    L.append("")
    L.append("static const dc_bgtex_row_t dc_bgtex_map[] = {")
    for name in names:
        bin_path, size, off, src, swap = table[name]
        n_bytes += size
        L.append('    {{ (const void*){}, "{}", {}u, {}u, {}, {} }},'.format(
            name, bin_path, size, off, src, swap))
    L.append("};")
    L.append("")
    # A LITERAL, not sizeof/sizeof: dc_bgtex.c tests this with #if, and sizeof
    # is not available to the preprocessor.
    L.append("#define DC_BGTEX_MAP_N {}".format(len(names)))
    L.append("")
    L.append("#endif /* DC_BGTEX_MAP_INC_ */")
    L.append("")
    return "\n".join(L), len(names), n_bytes


def _all_npc_num():
    """ALL_NPC_NUM — the villager/special boundary in npc_draw_data_tbl[].

    Read out of include/m_name_table.h rather than typed here: it is what
    aNPC_get_draw_data_idx() (ac_npc_ctrl.c_inc:154-187) adds to a special NPC's
    id, so rows below it are NAME_TYPE_NPC villagers and rows at or above it are
    NAME_TYPE_SPNPC.
    """
    hdr = (REPO / "include" / "m_name_table.h").read_text(
        encoding="utf-8", errors="surrogateescape")
    m = re.search(r"^#define NPC_NUM (\d+)$", hdr, re.MULTILINE)
    if not m:
        raise SystemExit(
            "make_stub_data: cannot find `#define NPC_NUM` in "
            "include/m_name_table.h. That macro is the villager/special "
            "boundary (ALL_NPC_NUM = NPC_NUM + 2); do not guess it.")
    # m_name_table.h:149 -- "+ 2 to include the two test villagers".
    return int(m.group(1)) + 2


def parse_npc_draw_data():
    """The villager half of npc_draw_data_tbl[], as ordered symbol lists.

    Returns (rows, n_total) where rows[i] is the 16 symbol names of villager row
    i in aNPC_draw_tex_data_c order (texture, palette, eye[8], mouth[6]) and
    n_total is the whole table's row count. 'NULL' is a legal entry — 78 of the
    236 villager sets have no mouth textures and ac_npc_draw.c_inc:272 tests for
    it.

    The villager/special boundary is ALL_NPC_NUM, which is what
    aNPC_get_draw_data_idx() (ac_npc_ctrl.c_inc:154-187) adds to a special NPC's
    id: rows below it are NAME_TYPE_NPC villagers, rows at or above it are
    NAME_TYPE_SPNPC. It is read out of m_name_table.h rather than typed here.
    """
    all_npc_num = _all_npc_num()

    text = (REPO / NPCTEX_TABLE).read_text(
        encoding="utf-8", errors="surrogateescape")
    try:
        body = text[text.index("npc_draw_data_tbl[] = {"):]
    except ValueError:
        raise SystemExit(
            "make_stub_data R2: %s no longer defines npc_draw_data_tbl[]."
            % NPCTEX_TABLE)

    raw = re.findall(r"\n    \{\n(.*?)\n    \},", body, re.S)
    rows = []
    for i, r in enumerate(raw):
        m = re.search(
            r"\{\n\s+(\w+),\n\s+(\w+),\n"
            r"\s+\{\n((?:\s+\w+,\n){%d})\s+\},\n"
            r"\s+\{\n((?:\s+\w+,\n){%d})\s+\},"
            % (NPCTEX_EYE_NUM, NPCTEX_MOUTH_NUM), r)
        if not m:
            raise SystemExit(
                "make_stub_data R2: row %d of npc_draw_data_tbl[] does not "
                "match the aNPC_draw_tex_data_c shape (texture, palette, "
                "eye[%d], mouth[%d]). The vendored table has changed; "
                "re-derive the parser rather than skipping the row -- a "
                "skipped row is a villager that renders untextured."
                % (i, NPCTEX_EYE_NUM, NPCTEX_MOUTH_NUM))
        rows.append([m.group(1), m.group(2)]
                    + re.findall(r"(\w+),", m.group(3))
                    + re.findall(r"(\w+),", m.group(4)))
    return rows, all_npc_num


def emit_npctex_map(table, keep_paths, pool):
    """Build the text of dc_npctex_map.inc. Returns (text, n_sets, n_bytes).

    One row per DISTINCT villager texture set, keyed by its position in
    npc_draw_data_tbl[] through dc_npctex_row_set[]. dc/src/dc_npctex.c is the
    consumer and carries the argument.

    THE WHOLE DESIGN RESTS ON ONE MEASURED FACT and this function is where it is
    checked: every villager set is CONTIGUOUS in the ROM, in declaration order
    (palette, eye1..8, mouth1..6, body), from one rom_src, with the palette the
    only byte-swapped member. That makes a set two disc reads and a base
    pointer instead of sixteen rows of (size, offset, swap). If it ever stops
    being true this hard-errors -- it must not degrade into a short map, because
    a set that silently loses its body texture renders as a missing asset and
    reads as a renderer bug for hours (kb/traps.md).

    `pool` False emits only the count macro and no table, so the runtime
    compiles to a pair of empty functions and nothing is left unreferenced.
    """
    L = []
    L.append("/* GENERATED by tools/dcstub/make_stub_data.py — DO NOT EDIT. */")
    L.append("/*")
    L.append(" * R2: the villager texture pool. Each row is one DISTINCT set of")
    L.append(" * 16 textures out of src/data/npc/model/tex/, described by where")
    L.append(" * its bytes live in the ROM rather than by an address — nothing")
    L.append(" * here is resident. dc_npctex_row_set[] maps an")
    L.append(" * npc_draw_data_tbl[] row onto its set; dc/src/dc_npctex.c")
    L.append(" * defines the dc_npctex_set_t this is written against, owns the")
    L.append(" * 16 slots the bytes are read into, and does the pointer")
    L.append(" * rewrite.")
    L.append(" *")
    L.append(" * ONLY the villager range is here. Rows at or above ALL_NPC_NUM")
    L.append(" * are the special NPCs — Tom Nook, Rover, K.K., Porter and the")
    L.append(" * raccoons — which stay RESIDENT via")
    L.append(" * tools/dcstub/make_keeplist_town.py's EXTRA_SOURCES and must")
    L.append(" * never be pooled.")
    L.append(" */")
    L.append("#ifndef DC_NPCTEX_MAP_INC_")
    L.append("#define DC_NPCTEX_MAP_INC_")
    L.append("")

    if not pool:
        L.append("/* --npctex-pool 0: the map is empty and the %d censused"
                 % len(NPCTEX_KEEP_RESTORE))
        L.append(" * villager tex files are back on the keep list, so")
        L.append(" * dc_npctex_ensure() and dc_npctex_pool_reset() are no-ops.")
        L.append(" * No table is emitted, so nothing here is left unreferenced. */")
        L.append("#define DC_NPCTEX_SETS 0")
        L.append("")
        L.append("#endif /* DC_NPCTEX_MAP_INC_ */")
        L.append("")
        return "\n".join(L), 0, 0

    rows, all_npc_num = parse_npc_draw_data()
    if len(rows) != NPCTEX_ROWS_TOTAL:
        raise SystemExit(
            "make_stub_data R2: npc_draw_data_tbl[] has %d rows, expected %d.\n"
            "  This count is the table's own checksum; pc/src/pc_model_viewer.c"
            ":321\n"
            "  carries the same number. Re-derive NPCTEX_ROWS_TOTAL against the"
            " vendored\n"
            "  table; do not relax it." % (len(rows), NPCTEX_ROWS_TOTAL))
    if all_npc_num != NPCTEX_VILLAGER_ROWS:
        raise SystemExit(
            "make_stub_data R2: ALL_NPC_NUM is %d, expected %d. That is the\n"
            "  villager/special boundary — get it wrong high and Tom Nook goes\n"
            "  into the pool (which is the one thing R2 must not do), get it\n"
            "  wrong low and real villagers keep their [1]-sized arrays."
            % (all_npc_num, NPCTEX_VILLAGER_ROWS))

    # A file is "kept" if the keep list names it AT ALL. A partial keep with a
    # symbol filter is treated as kept too: R2 would otherwise repoint the row
    # away from arrays the keep machinery had just filled.
    kept_files = {p for p, _ in keep_paths}

    # ROM order inside a set: palette, eye1..8, mouth1..6, body. Indices into
    # the 16-entry aNPC_draw_tex_data_c order (texture first, palette second).
    rom_order = [1] + list(range(2, 2 + NPCTEX_EYE_NUM)) \
                    + list(range(2 + NPCTEX_EYE_NUM,
                                 2 + NPCTEX_EYE_NUM + NPCTEX_MOUTH_NUM)) + [0]

    sets = []            # (name, rom_off, rom_src, total, tmem_off, tmem_size,
                         #  mouths, kept)
    index_of = {}
    row_set = []
    for i in range(all_npc_num):
        syms = rows[i]
        body_sym = syms[0]
        if not body_sym.endswith(NPCTEX_BODY_SUFFIX):
            raise SystemExit(
                "make_stub_data R2: row %d's texture symbol %r does not end in"
                " %r.\n  The set NAME is derived from it and so is the source"
                " file path; a\n  changed convention must be re-derived, not"
                " guessed."
                % (i, body_sym, NPCTEX_BODY_SUFFIX))
        name = body_sym[:-len(NPCTEX_BODY_SUFFIX)]

        if name in index_of:
            row_set.append(index_of[name])
            continue

        off = None
        cur = None
        src = None
        total = 0
        tmem_off = None
        tmem_size = 0
        mouths = 0
        for pos in rom_order:
            sym = syms[pos]
            if sym == "NULL":
                # Only the mouth block is ever absent, and it is all-or-nothing.
                if not (2 + NPCTEX_EYE_NUM <= pos
                        < 2 + NPCTEX_EYE_NUM + NPCTEX_MOUTH_NUM):
                    raise SystemExit(
                        "make_stub_data R2: %s has NULL at tex_data slot %d. "
                        "Only\n  mouth_texture[] may be NULL (78 of the sets "
                        "are); a NULL palette,\n  eye or body would mean the "
                        "layout assumption is wrong."
                        % (name, pos))
                continue
            if sym not in table:
                raise SystemExit(
                    "make_stub_data R2: %s has no s_assets[] row in %s. Every\n"
                    "  villager texture must be loadable off the disc or the "
                    "pool cannot\n  fill; a missing row means the asset table "
                    "and the draw table have\n  drifted apart."
                    % (sym, ASSET_TABLE.name))
            _bin, size, ro, rs, swap = table[sym]
            if off is None:
                off, cur, src = ro, ro, rs
            if ro != cur:
                raise SystemExit(
                    "make_stub_data R2: %s is NOT contiguous in the ROM — %s "
                    "starts at\n  0x%X, the previous member ended at 0x%X. R2 "
                    "reads a whole set with\n  TWO preads and relies on that "
                    "contiguity; a gap means the two-read\n  loader in "
                    "dc/src/dc_npctex.c would fill the slot with the wrong "
                    "bytes.\n  Re-derive the loader against a per-member map "
                    "rather than relaxing this."
                    % (name, sym, ro, cur))
            if rs != src:
                raise SystemExit(
                    "make_stub_data R2: %s spans two ROM sources (%d and %d). "
                    "The set\n  descriptor carries one rom_src; re-derive the "
                    "loader." % (name, src, rs))
            want_swap = 1 if pos == 1 else 0   # 1 == SWAP_U16, pc_assets.c:14
            if swap != want_swap:
                raise SystemExit(
                    "make_stub_data R2: %s has swap=%d, expected %d. R2 swaps "
                    "the\n  leading 32 B palette and nothing else; a second "
                    "swapped member would\n  be byte-reversed in VRAM and look "
                    "like a palette bug." % (sym, swap, want_swap))
            if pos == 1 and size != NPCTEX_PAL_SIZE:
                raise SystemExit(
                    "make_stub_data R2: palette %s is %d B, expected %d."
                    % (sym, size, NPCTEX_PAL_SIZE))
            if 2 <= pos < 2 + NPCTEX_EYE_NUM and size != NPCTEX_FACE_SIZE:
                raise SystemExit(
                    "make_stub_data R2: eye texture %s is %d B, expected %d. "
                    "The slot\n  layout is computed from fixed eye/mouth "
                    "strides." % (sym, size, NPCTEX_FACE_SIZE))
            if (2 + NPCTEX_EYE_NUM <= pos
                    < 2 + NPCTEX_EYE_NUM + NPCTEX_MOUTH_NUM):
                if size != NPCTEX_FACE_SIZE:
                    raise SystemExit(
                        "make_stub_data R2: mouth texture %s is %d B, expected"
                        " %d." % (sym, size, NPCTEX_FACE_SIZE))
                mouths += 1
            if pos == 0:
                tmem_off = cur - off
                tmem_size = size
            cur += size
            total += size

        if mouths not in (0, NPCTEX_MOUTH_NUM):
            raise SystemExit(
                "make_stub_data R2: %s has %d mouth textures. It is all-or-"
                "nothing in\n  the vendored table and dc_npctex.c's slot layout"
                " assumes that."
                % (name, mouths))
        expect_tmem_off = (NPCTEX_PAL_SIZE
                           + NPCTEX_EYE_NUM * NPCTEX_FACE_SIZE
                           + mouths * NPCTEX_FACE_SIZE)
        if tmem_off != expect_tmem_off:
            raise SystemExit(
                "make_stub_data R2: %s puts its body texture at +%d, computed "
                "+%d."
                % (name, tmem_off, expect_tmem_off))

        src_file = "%s/%s.c" % (NPCTEX_SRC_DIR, name)
        index_of[name] = len(sets)
        row_set.append(index_of[name])
        sets.append((name, off, src, total, tmem_off, tmem_size, mouths,
                     1 if src_file in kept_files else 0))

    if len(sets) != NPCTEX_SETS_EXPECTED:
        raise SystemExit(
            "make_stub_data R2: found %d distinct villager texture sets, "
            "expected %d.\n  Same checksum rule as BGTEX_ROWS_EXPECTED: a "
            "selector that stops matching\n  would emit a SHORT map and the "
            "villagers it dropped would render\n  untextured rather than fail."
            % (len(sets), NPCTEX_SETS_EXPECTED))

    slot_bytes = max(s[3] for s in sets)
    tmem_max = max(s[5] for s in sets)
    rows_per_set_max = max(row_set.count(v) for v in set(row_set))
    # What the pool actually takes off .bss: every set it will serve. A set the
    # keep list still names stays resident and is not R2's to count.
    n_bytes = sum(s[3] for s in sets if not s[7])

    L.append("#define DC_NPCTEX_SETS            {}".format(len(sets)))
    L.append("#define DC_NPCTEX_ROWS            {}".format(all_npc_num))
    L.append("/* The pool slot size: the largest villager set in the tree"
             " ({}). */".format(
                 max(sets, key=lambda s: s[3])[0]))
    L.append("#define DC_NPCTEX_SLOT_BYTES      {}".format(slot_bytes))
    L.append("/* The zero block an unserved row is pointed at: the largest body"
             " texture,")
    L.append(" * which is the largest single read any of the 16 pointers can"
             " take. */")
    L.append("#define DC_NPCTEX_TMEM_MAX        {}".format(tmem_max))
    L.append("/* chn_1 is named by three rows (39, 236, 237 — the two test"
             " villagers at")
    L.append(" * the end of the range reuse it), so the patch list has to be"
             " longer")
    L.append(" * than the slot count. */")
    L.append("#define DC_NPCTEX_ROWS_PER_SET_MAX {}".format(rows_per_set_max))
    L.append("/* Slot layout, fixed for every set and checked against every"
             " s_assets[] row")
    L.append(" * by the generator: palette, eye[8], mouth[6] (or none), body. */")
    L.append("#define DC_NPCTEX_PAL_SIZE        {}".format(NPCTEX_PAL_SIZE))
    L.append("#define DC_NPCTEX_EYE_NUM         {}".format(NPCTEX_EYE_NUM))
    L.append("#define DC_NPCTEX_EYE_SIZE        {}".format(NPCTEX_FACE_SIZE))
    L.append("#define DC_NPCTEX_MOUTH_NUM       {}".format(NPCTEX_MOUTH_NUM))
    L.append("#define DC_NPCTEX_MOUTH_SIZE      {}".format(NPCTEX_FACE_SIZE))
    L.append("#define DC_NPCTEX_MOUTH_OFF       {}".format(
        NPCTEX_PAL_SIZE + NPCTEX_EYE_NUM * NPCTEX_FACE_SIZE))
    L.append("")
    L.append("static const dc_npctex_set_t dc_npctex_set[] = {")
    for name, off, src, total, tmem_off, tmem_size, mouths, kept in sets:
        L.append('    {{ 0x{:X}u, "{}", {}u, {}u, {}u, {}, {}, {}, 0 }},'.format(
            off, name, total, tmem_off, tmem_size, mouths, src, kept))
    L.append("};")
    L.append("")
    L.append("/* npc_draw_data_tbl[] row -> dc_npctex_set[] index. */")
    L.append("static const short dc_npctex_row_set[] = {")
    for i in range(0, len(row_set), 16):
        L.append("    " + " ".join("%d," % v for v in row_set[i:i + 16]))
    L.append("};")
    L.append("")
    L.append("#endif /* DC_NPCTEX_MAP_INC_ */")
    L.append("")
    return "\n".join(L), len(sets), n_bytes


# ---------------------------------------------------------------------------
# R3 — parsing src/data/npc/model/mdl/*.c
# ---------------------------------------------------------------------------
# Comments have to go before anything else is scanned: the files carry
# `/* joint 3 */` markers and `// clang-format off`, and a `gs`-looking word
# inside one would otherwise be counted as a command.
NPCMDL_COMMENT_RE = re.compile(r"/\*.*?\*/|//[^\n]*", re.S)
NPCMDL_VTX_DECL_RE = r"static Vtx %s_v\[(0x[0-9A-Fa-f]+)"
NPCMDL_GFX_ARR_RE = re.compile(
    r"^static Gfx ([A-Za-z_][A-Za-z0-9_]*)\[\] = \{$", re.M)
NPCMDL_SKEL_RE = (
    r"extern cKF_Skeleton_R_c cKF_bs_r_%s\s*=\s*"
    r"\{\s*(\d+)\s*,\s*(\d+)\s*,\s*(\w+)\s*\}")
NPCMDL_JOINT_RE = re.compile(
    r"\{\s*(\w+)\s*,\s*\d+\s*,\s*[\w |]+?\s*,\s*-?\d+\s*,\s*-?\d+\s*,\s*-?\d+\s*\}")
# A command is an identifier starting `gs` that is immediately applied. Anchored
# at paren depth 0 so an argument like `head_gst_model` (which contains the
# substring "gst_model") can never be mistaken for one.
NPCMDL_CMD_RE = re.compile(r"gs[A-Za-z_][A-Za-z0-9_]*(?=\s*\()")


def _npcmdl_braced(text, start):
    """The text between `start` and the `}` that closes the brace before it."""
    depth = 1
    i = start
    while depth > 0:
        depth += (text[i] == "{") - (text[i] == "}")
        i += 1
    return text[start:i - 1]


def parse_npc_mdl_tree():
    """Every src/data/npc/model/mdl/<sp>.c, as R3 needs it.

    Returns {species: {size, rom_off, rom_src, bin, patches}} where `patches` is
    the ordered list of (joint index, Gfx index within that joint's display
    list, byte offset into <sp>_v) for every gsSPVertex in the file.

    ⚠️ EVERYTHING HERE IS A CHECK. A patch table that is quietly short or
    quietly shifted does not fail — it writes a pool address into whatever Gfx
    word the arithmetic landed on, and the result is corrupt geometry that reads
    as a renderer bug. So every step hard-errors rather than skipping:
    an unknown macro, a gsSPVertex whose operand is not `&<sp>_v[N]`, a display
    list no joint names, a list that does not end in gsSPEndDisplayList, an
    offset past the end of the array, and a missing pc_load_asset() line.
    """
    root = REPO / NPCMDL_SRC_DIR
    files = sorted(root.glob("*.c"))
    if len(files) != NPCMDL_FILES_EXPECTED:
        raise SystemExit(
            "make_stub_data R3: %s holds %d .c files, expected %d. That count "
            "is the\n  tree's own checksum; re-derive NPCMDL_FILES_EXPECTED "
            "rather than relaxing it."
            % (NPCMDL_SRC_DIR, len(files), NPCMDL_FILES_EXPECTED))

    out = {}
    n_lists = 0
    n_vtxcmd = 0
    for f in files:
        sp = f.stem
        raw = f.read_text(encoding="utf-8", errors="surrogateescape")
        text = NPCMDL_COMMENT_RE.sub(" ", raw)

        m = re.search(NPCMDL_VTX_DECL_RE % re.escape(sp), text)
        if not m:
            raise SystemExit(
                "make_stub_data R3: %s.c has no `static Vtx %s_v[0x…]` under "
                "#ifdef TARGET_PC.\n  That declaration is what the pool slot is "
                "sized against; a changed\n  convention must be re-derived, not "
                "guessed." % (sp, sp))
        size = int(m.group(1), 16)

        lm = PC_LOAD_CALL_RE.search(text)
        if not lm or lm.group("dest") != sp + "_v":
            raise SystemExit(
                "make_stub_data R3: %s.c has no pc_load_asset(…, %s_v, …).\n"
                "  %s_v is FILE-STATIC, so it has no s_assets[] row and the "
                "generated\n  per-file loader is the ONLY place its (rom_off, "
                "size, rom_src, swap)\n  exists. Without it the slot cannot be "
                "filled." % (sp, sp, sp))
        if int(lm.group("size"), 0) != size:
            raise SystemExit(
                "make_stub_data R3: %s_v is declared %d B but loaded %d B."
                % (sp, size, int(lm.group("size"), 0)))
        if int(lm.group("swap")) != 2:
            raise SystemExit(
                "make_stub_data R3: %s_v loads with swap=%s, expected 2 "
                "(DC_STUB_SWAP_VTX,\n  dc/src/dc_main.c:745-746). A Vtx array "
                "read without the vertex swap is\n  byte-reversed geometry, "
                "which looks like a renderer bug."
                % (sp, lm.group("swap")))

        sm = re.search(NPCMDL_SKEL_RE % re.escape(sp), text)
        if not sm:
            raise SystemExit(
                "make_stub_data R3: %s.c does not define "
                "`extern cKF_Skeleton_R_c cKF_bs_r_%s`.\n  That symbol is the "
                "ONLY externally visible thing in the file, and R3 reaches\n"
                "  every display list through it (c_keyframe.h:44-48 -> :37-42)."
                % (sp, sp))
        n_joints, n_shown, tbl = int(sm.group(1)), int(sm.group(2)), sm.group(3)

        tm = re.search(r"static cKF_Joint_R_c %s\[\]\s*=\s*\{" % re.escape(tbl),
                       text)
        if not tm:
            raise SystemExit(
                "make_stub_data R3: %s.c names joint table %s, which it does "
                "not define." % (sp, tbl))
        joints = NPCMDL_JOINT_RE.findall(_npcmdl_braced(text, tm.end()))
        if len(joints) != n_joints:
            raise SystemExit(
                "make_stub_data R3: cKF_bs_r_%s claims %d joints, %s[] has %d."
                % (sp, n_joints, tbl, len(joints)))
        joint_of = {}
        for ji, name in enumerate(joints):
            if name == "NULL":
                continue
            if name in joint_of:
                # Measured false across all 72 files. If it ever becomes true,
                # the shared list would be patched twice with the same value —
                # harmless — but the joint index in the table would be
                # ambiguous, so stop rather than pick one.
                raise SystemExit(
                    "make_stub_data R3: %s is named by joints %d and %d of "
                    "cKF_bs_r_%s.\n  R3's patch table addresses a list BY "
                    "JOINT; re-derive it before allowing\n  this."
                    % (name, joint_of[name], ji, sp))
            joint_of[name] = ji
        if len(joint_of) != n_shown:
            raise SystemExit(
                "make_stub_data R3: cKF_bs_r_%s claims %d shown joints, %s[] "
                "names %d distinct models." % (sp, n_shown, tbl, len(joint_of)))

        patches = []
        seen_lists = set()
        for am in NPCMDL_GFX_ARR_RE.finditer(text):
            name = am.group(1)
            body = _npcmdl_braced(text, am.end())
            if name not in joint_of:
                raise SystemExit(
                    "make_stub_data R3: display list %s in %s.c is named by no "
                    "joint of\n  cKF_bs_r_%s. R3 can only reach a list through "
                    "the skeleton, so a list\n  nothing points at would keep "
                    "its stubbed vertex pointers while the rest\n  of the model "
                    "moved." % (name, sp, sp))
            seen_lists.add(name)
            n_lists += 1
            joint = joint_of[name]
            idx = 0
            k = 0
            depth = 0
            last = None
            while k < len(body):
                c = body[k]
                if c == "(":
                    depth += 1
                elif c == ")":
                    depth -= 1
                elif depth == 0:
                    cm = NPCMDL_CMD_RE.match(body, k)
                    if cm:
                        mac = cm.group(0)
                        if mac not in NPCMDL_GFX_WORDS:
                            raise SystemExit(
                                "make_stub_data R3: %s.c/%s uses %s(), which is "
                                "not in\n  NPCMDL_GFX_WORDS. Read its definition "
                                "and add the number of Gfx it\n  expands to — "
                                "defaulting to 1 would shift every later patch "
                                "in this\n  list onto the wrong word."
                                % (sp, name, mac))
                        if mac == "gsSPVertex":
                            p = body.index("(", k)
                            vm = re.match(
                                r"\(\s*&%s_v\[(\d+)\]\s*," % re.escape(sp),
                                body[p:])
                            if not vm:
                                raise SystemExit(
                                    "make_stub_data R3: %s.c/%s has a "
                                    "gsSPVertex whose operand is not\n  "
                                    "&%s_v[N]: %r. All 2,068 in the tree are "
                                    "that shape; a different one\n  would need "
                                    "a different relocation."
                                    % (sp, name, sp, body[p:p + 48]))
                            off = int(vm.group(1)) * NPCMDL_VTX_SIZE
                            if off >= size:
                                raise SystemExit(
                                    "make_stub_data R3: %s.c/%s loads %s_v[%s] "
                                    "= +%d, past the %d B\n  array. The slot "
                                    "would be read out of bounds."
                                    % (sp, name, sp, vm.group(1), off, size))
                            patches.append((joint, idx, off))
                            n_vtxcmd += 1
                        idx += NPCMDL_GFX_WORDS[mac]
                        last = mac
                        k += len(mac)
                        continue
                k += 1
            if last != "gsSPEndDisplayList":
                raise SystemExit(
                    "make_stub_data R3: %s.c/%s does not end in "
                    "gsSPEndDisplayList (last was\n  %s). The command walk did "
                    "not reach the end of the array, so every Gfx\n  index it "
                    "produced is suspect." % (sp, name, last))

        missing = set(joint_of) - seen_lists
        if missing:
            raise SystemExit(
                "make_stub_data R3: cKF_bs_r_%s names %s, which %s.c does not "
                "define as a\n  `static Gfx …[] = {`." % (sp, sorted(missing), sp))

        out[sp] = {
            "size": size,
            "rom_off": int(lm.group("off"), 0),
            "rom_src": int(lm.group("src")),
            "bin": lm.group("path"),
            "patches": patches,
        }

    if n_lists != NPCMDL_LISTS_EXPECTED:
        raise SystemExit(
            "make_stub_data R3: found %d display lists, expected %d."
            % (n_lists, NPCMDL_LISTS_EXPECTED))
    if n_vtxcmd != NPCMDL_VTXCMD_EXPECTED:
        raise SystemExit(
            "make_stub_data R3: found %d gsSPVertex commands, expected %d.\n"
            "  Same checksum rule as BGTEX_ROWS_EXPECTED — a short patch table "
            "does not\n  fail, it half-moves a model." % (n_vtxcmd,
                                                          NPCMDL_VTXCMD_EXPECTED))
    return out


def parse_npc_draw_skeletons():
    """npc_draw_data_tbl[] row -> the cKF_bs_r_* species it selects.

    aNPC_actor_ct() reads exactly this field — `skeleton =
    draw_data.model_skeleton` (ac_npc_ct.c_inc:274) out of the row this
    function's caller is about to snapshot — and hands it to
    cKF_SkeletonInfo_R_ct(). So the row -> skeleton mapping is not invented
    here; it IS the game's own, read out of the same table.

    Returns (species_per_row, all_npc_num).
    """
    text = (REPO / NPCTEX_TABLE).read_text(
        encoding="utf-8", errors="surrogateescape")
    try:
        body = text[text.index("npc_draw_data_tbl[] = {"):]
    except ValueError:
        raise SystemExit(
            "make_stub_data R3: %s no longer defines npc_draw_data_tbl[]."
            % NPCTEX_TABLE)
    rows = re.findall(r"\n    \{\n(.*?)\n    \},", body, re.S)
    out = []
    for i, r in enumerate(rows):
        m = re.search(r"^\s+&cKF_bs_r_(\w+),\s*$", r, re.M)
        if not m:
            raise SystemExit(
                "make_stub_data R3: row %d of npc_draw_data_tbl[] names no "
                "&cKF_bs_r_*\n  skeleton. That field (ac_npc.h:156) is what "
                "selects the model; a row\n  without one cannot be mapped."
                % i)
        out.append(m.group(1))
    return out, _all_npc_num()


def emit_npcmdl_map(keep_paths, pool):
    """Build the text of dc_npcmdl_map.inc. Returns (text, n_species, n_bytes).

    One row per VILLAGER species, keyed by the address of its extern skeleton —
    the same pointer aNPC_actor_ct() reads out of the row — plus one flat patch
    table shared by all of them. dc/src/dc_npcmdl.c is the consumer and carries
    the argument.

    THE SPLIT IS THE WHOLE SAFETY ARGUMENT and this function is where it is
    checked: the set of skeletons reachable from a VILLAGER row and the set
    reachable from a SPECIAL row do not intersect. That is what lets R3 pool by
    skeleton pointer and still be structurally incapable of touching Tom Nook,
    Rover, K.K., Porter or the raccoons. If it ever stops being true this
    hard-errors — R3 must not silently start pooling a special NPC, which is
    the one mistake that has already cost this project a session.

    `pool` False emits only the count macro and no tables, so the runtime
    compiles to a pair of empty functions and nothing is left unreferenced.
    """
    L = []
    L.append("/* GENERATED by tools/dcstub/make_stub_data.py — DO NOT EDIT. */")
    L.append("/*")
    L.append(" * R3: the villager model pool. Each row is one species'")
    L.append(" * src/data/npc/model/mdl/<sp>.c vertex array, described by where")
    L.append(" * its bytes live in the ROM, and keyed by &cKF_bs_r_<sp> — the")
    L.append(" * skeleton pointer aNPC_actor_ct() reads out of the")
    L.append(" * npc_draw_data_tbl[] row (ac_npc_ct.c_inc:274).")
    L.append(" *")
    L.append(" * dc_npcmdl_patch[] is the RELOCATION the textures did not need:")
    L.append(" * one entry per gsSPVertex, naming the joint whose display list")
    L.append(" * holds it, the Gfx index within that list, and the byte offset")
    L.append(" * into the vertex array. dc/src/dc_npcmdl.c defines the two")
    L.append(" * structs this is written against, owns the slots, and re-checks")
    L.append(" * every entry against the live word before it writes.")
    L.append(" *")
    L.append(" * ONLY the villager species are here. The 40 reached solely from")
    L.append(" * a row >= ALL_NPC_NUM — Tom Nook, Rover, K.K., Porter and the")
    L.append(" * raccoons — stay RESIDENT via the keep list and have no entry,")
    L.append(" * so the lookup below cannot find them even by accident.")
    L.append(" */")
    L.append("#ifndef DC_NPCMDL_MAP_INC_")
    L.append("#define DC_NPCMDL_MAP_INC_")
    L.append("")

    if not pool:
        L.append("/* --npcmdl-pool 0: the maps are empty and %s is back"
                 % Path(NPCMDL_KEEP_RESTORE[0]).name)
        L.append(" * on the keep list, so dc_npcmdl_ensure() and")
        L.append(" * dc_npcmdl_pool_reset() are no-ops. No table is emitted, so")
        L.append(" * nothing here is left unreferenced. */")
        L.append("#define DC_NPCMDL_SPECIES 0")
        L.append("")
        L.append("#endif /* DC_NPCMDL_MAP_INC_ */")
        L.append("")
        return "\n".join(L), 0, 0

    mdl = parse_npc_mdl_tree()
    skel_of_row, all_npc_num = parse_npc_draw_skeletons()
    if all_npc_num != NPCTEX_VILLAGER_ROWS:
        raise SystemExit(
            "make_stub_data R3: ALL_NPC_NUM is %d, expected %d. That is the\n"
            "  villager/special boundary — get it wrong high and Tom Nook's "
            "model goes\n  into the pool, get it wrong low and real villagers "
            "keep their [1]-sized\n  vertex arrays." % (all_npc_num,
                                                        NPCTEX_VILLAGER_ROWS))
    if len(skel_of_row) != NPCTEX_ROWS_TOTAL:
        raise SystemExit(
            "make_stub_data R3: npc_draw_data_tbl[] has %d rows, expected %d."
            % (len(skel_of_row), NPCTEX_ROWS_TOTAL))

    villager = set(skel_of_row[:all_npc_num])
    special = set(skel_of_row[all_npc_num:])
    both = sorted(villager & special)
    if both:
        raise SystemExit(
            "make_stub_data R3: %s is used by BOTH a villager row and a "
            "special-NPC row.\n  R3 pools by skeleton pointer precisely because "
            "those two sets were disjoint\n  (measured 2026-08-05: 32 villager, "
            "40 special, 0 shared), which is what\n  makes it structurally "
            "unable to pool Tom Nook. Re-derive the selector —\n  do NOT relax "
            "this." % ", ".join(both))
    unknown = sorted((villager | special) - set(mdl))
    if unknown:
        raise SystemExit(
            "make_stub_data R3: npc_draw_data_tbl[] names cKF_bs_r_%s, which "
            "has no\n  %s/<sp>.c." % (unknown[0], NPCMDL_SRC_DIR))
    if len(villager) != NPCMDL_VILLAGER_EXPECTED:
        raise SystemExit(
            "make_stub_data R3: found %d villager species, expected %d. Same\n"
            "  checksum rule as BGTEX_ROWS_EXPECTED: a selector that stops "
            "matching would\n  emit a SHORT map and the species it dropped "
            "would keep drawing as a black\n  spiky mess rather than fail."
            % (len(villager), NPCMDL_VILLAGER_EXPECTED))

    kept_files = {p for p, _ in keep_paths}

    names = sorted(villager)
    patch = []
    rows = []
    for sp in names:
        e = mdl[sp]
        first = len(patch)
        patch += e["patches"]
        src_file = "%s/%s.c" % (NPCMDL_SRC_DIR, sp)
        rows.append((sp, e["rom_off"], e["rom_src"], e["size"], first,
                     len(e["patches"]), 1 if src_file in kept_files else 0))

    slot_bytes = max(r[3] for r in rows)
    max_joint = max(p[0] for p in patch)
    max_gfx = max(p[1] for p in patch)
    max_off = max(p[2] for p in patch)
    max_patches = max(r[5] for r in rows)
    # What the pool actually takes off .bss: every species it will serve. One
    # the keep list still names stays resident and is not R3's to count.
    n_bytes = sum(r[3] for r in rows if not r[6])

    # The generated table's own field widths. Emitted as literals so a future
    # tree that overflows one of them is caught HERE rather than by silent
    # truncation into a wrong Gfx word.
    if max_joint > 0xFF or max_gfx > 0xFF or max_off > 0xFFFF \
            or max_patches > 0xFF or len(patch) > 0xFFFF or slot_bytes > 0xFFFF:
        raise SystemExit(
            "make_stub_data R3: a patch field overflowed its width "
            "(joint %d, gfx %d,\n  offset %d, per-species %d, total %d, slot "
            "%d). Widen dc_npcmdl_patch_t in\n  dc/src/dc_npcmdl.c and this "
            "check together."
            % (max_joint, max_gfx, max_off, max_patches, len(patch),
               slot_bytes))

    L.append("#define DC_NPCMDL_SPECIES         {}".format(len(rows)))
    L.append("#define DC_NPCMDL_PATCHES         {}".format(len(patch)))
    L.append("/* ALL_NPC_NUM. A row at or above this is a SPECIAL NPC and is")
    L.append(" * refused before the skeleton is even looked at. */")
    L.append("#define DC_NPCMDL_ROWS            {}".format(all_npc_num))
    L.append("/* The pool slot size: the largest villager model in the tree"
             " ({}). */".format(max(rows, key=lambda r: r[3])[0]))
    L.append("#define DC_NPCMDL_SLOT_BYTES      {}".format(slot_bytes))
    L.append("/* sizeof(Vtx), gbi.h:1096-1100. Pinned by make_src_shrink.py's")
    L.append(" * npcmdl layout assert, which has the header this file does"
             " not. */")
    L.append("#define DC_NPCMDL_VTX_SIZE        {}".format(NPCMDL_VTX_SIZE))
    L.append("/* G_VTX under -DF3DEX_GBI_2 (gbi.h:122, dc/Makefile:325). Every")
    L.append(" * word R3 is about to write is checked to be one first. */")
    L.append("#define DC_NPCMDL_G_VTX           1")
    L.append("/* pc/src/pc_assets.c:14 — SWAP_VTX. Every one of the 72 vertex")
    L.append(" * arrays loads with it; the generator refuses any that does"
             " not. */")
    L.append("#define DC_NPCMDL_SWAP_VTX        2")
    L.append("")
    L.append("/* One per gsSPVertex, grouped by species (patch_off/patch_n"
             " below). */")
    L.append("static const dc_npcmdl_patch_t dc_npcmdl_patch[] = {")
    for i in range(0, len(patch), 8):
        L.append("    " + " ".join(
            "{%d,%d,%d}," % (o, g, j) for j, g, o in patch[i:i + 8]))
    L.append("};")
    L.append("")
    # `extern char x[]` regardless of the vendored type, exactly as
    # dc_bgtex_map.inc declares its u16 palettes: only the ADDRESS is used, no
    # TU sees both spellings, and this .inc must not drag c_keyframe.h into
    # dc/src (it pulls game.h, m_lib.h and half the actor system with it).
    for sp, _off, _src, _size, _first, _n, _kept in rows:
        L.append("extern char cKF_bs_r_{}[];".format(sp))
    L.append("")
    L.append("static const dc_npcmdl_t dc_npcmdl_species[] = {")
    for sp, off, src, size, first, n, kept in rows:
        L.append('    {{ 0x{:X}u, "{}", cKF_bs_r_{}, {}u, {}u, {}, {}, {} }},'
                 .format(off, sp, sp, size, first, n, src, kept))
    L.append("};")
    L.append("")
    L.append("#endif /* DC_NPCMDL_MAP_INC_ */")
    L.append("")
    return "\n".join(L), len(rows), n_bytes


def write_if_changed(dst, text):
    """Returns True if the file was actually written."""
    dst.parent.mkdir(parents=True, exist_ok=True)
    if dst.exists():
        old = dst.read_text(encoding="utf-8", errors="surrogateescape")
        if old == text:
            return False
    dst.write_text(text, encoding="utf-8", errors="surrogateescape")
    return True


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument(
        "--keep",
        default=None,
        help="':' or ',' separated repo-relative sources to leave at FULL size. "
             "Overrides $DC_STUB_KEEP. Pass '' to stub everything. An entry may "
             "carry a '#'-separated symbol-prefix filter -- "
             "'src/data/model/obj_s_house1.c#obj_s_' keeps only the summer "
             "arrays and stubs the winter ones in the same file.",
    )
    ap.add_argument(
        "--bgtex-demand",
        type=int,
        default=int(os.environ.get("DC_BGTEX_DEMAND", "1") or "1"),
        choices=(0, 1),
        help="R1. 1 (the default, matching dc/Makefile) emits "
             "dc_bgtex_map.inc so the 96 mFM_grd_* ground textures are read off "
             "the disc on demand. 0 emits an empty map and puts the 27 "
             "summer/shared mFM_grd_*.c files back on the keep list. It MUST "
             "match the --bgtex-demand= that make_src_shrink.py was run with, "
             "and dc/Makefile's DC_BGTEX_DEMAND; dc/build-dc.sh passes all "
             "three from one value.",
    )
    ap.add_argument(
        "--npctex-pool",
        type=int,
        default=int(os.environ.get("DC_NPCTEX_POOL", "1") or "1"),
        choices=(0, 1),
        help="R2. 1 (the default, matching dc/Makefile) emits "
             "dc_npctex_map.inc so the 236 villager texture sets (993,984 B) "
             "are served out of 16 resident slots read off the disc. 0 emits "
             "an empty map and puts the 21 censused villager tex files back on "
             "the keep list. It MUST match the --npctex-pool= that "
             "make_src_shrink.py was run with, and dc/Makefile's "
             "DC_NPCTEX_POOL; dc/build-dc.sh passes all three from one value.",
    )
    ap.add_argument(
        "--npcmdl-pool",
        type=int,
        default=int(os.environ.get("DC_NPCMDL_POOL", "1") or "1"),
        choices=(0, 1),
        help="R3. 1 (the default, matching dc/Makefile) emits "
             "dc_npcmdl_map.inc so the 32 villager MODELS (194,400 B) are "
             "served out of 16 resident slots read off the disc, with their "
             "933 baked gsSPVertex words relocated. 0 emits an empty map and "
             "puts cbr_1 — the one villager model the census keep list named — "
             "back on the keep list. It MUST match the --npcmdl-pool= that "
             "make_src_shrink.py was run with, and dc/Makefile's "
             "DC_NPCMDL_POOL; dc/build-dc.sh passes all three from one value.",
    )
    ap.add_argument(
        "--texpool",
        type=int,
        default=int(os.environ.get("DC_TEXPOOL_PROBE", "0") or "0"),
        choices=(0, 1),
        help="T1's PROBE (not a pool). 1 emits dc_texpool_map.inc so "
             "dc/src/dc_texpool.c can count, per texture bind, whether the "
             "source pointer is the exact base of a mapped asset row. It moves "
             "no bytes and changes no pixel. 0 (the default, matching "
             "dc/Makefile's DC_TEXPOOL_PROBE) emits an empty map and the probe "
             "compiles to nothing.",
    )
    ap.add_argument(
        "--texpool-demand",
        type=int,
        default=int(os.environ.get("DC_TEXPOOL_DEMAND", "1") or "1"),
        choices=(0, 1),
        help="T1, THE LOADER. 1 (the default, matching dc/Makefile) stubs all "
             "6,092 texture arrays the display lists name by address — even "
             "the ones whose FILE the keep list keeps — and dc/src/"
             "dc_texpool.c reads each off /cd/foresta.rel into one staging "
             "buffer when the PVR first binds it. 0 leaves them resident and "
             "the renderer hashes their content on every bind, which is the "
             "pre-T1 build. It MUST match dc/Makefile's DC_TEXPOOL_DEMAND; "
             "dc/build-dc.sh passes both from one value.",
    )
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    bgtex_demand = bool(args.bgtex_demand)
    npctex_pool = bool(args.npctex_pool)
    npcmdl_pool = bool(args.npcmdl_pool)
    # T1. TWO switches, and they are independent:
    #   --texpool-demand  the LOADER. Its keep-list half is DEMAND_STUB, applied
    #                     PER SYMBOL rather than per file — see keep_symbol().
    #   --texpool         the falsification PROBE. Restores nothing and frees
    #                     nothing; it only makes the map carry row NAMES and
    #                     compiles the counters in.
    texpool_probe = bool(args.texpool)
    texpool_demand = bool(args.texpool_demand)
    out_root = Path(args.out).resolve()

    keep_paths, keep_why = parse_keep(args.keep)

    # R1's kill switch. Appended rather than expected in the caller's list: the
    # keep list is passed in from a shell (DC_STUB_KEEP), and a kill switch that
    # only works if the human also edits their command line is not one.
    # Whole-file keeps, and only for paths not already named — a partial keep
    # with a symbol filter must not be overridden by a bare duplicate.
    if not bgtex_demand:
        already = {p for p, _ in keep_paths}
        keep_paths += [(p, ()) for p in BGTEX_KEEP_RESTORE if p not in already]

    # R2's, and for the same reason.
    if not npctex_pool:
        already = {p for p, _ in keep_paths}
        keep_paths += [(p, ()) for p in NPCTEX_KEEP_RESTORE if p not in already]

    # R3's, and for the same reason.
    if not npcmdl_pool:
        already = {p for p, _ in keep_paths}
        keep_paths += [(p, ()) for p in NPCMDL_KEEP_RESTORE if p not in already]

    missing = [p for p, _ in keep_paths if not (REPO / p).is_file()]
    if missing:
        sys.stderr.write(
            "make_stub_data.py: keep list ({}) names files that do not "
            "exist:\n".format(keep_why)
        )
        for p in missing:
            sys.stderr.write("    {}\n".format(p))
        return 2

    keep_abs = {(REPO / p).resolve() for p, _ in keep_paths}
    # resolved path -> prefixes. Empty tuple means "keep the whole file".
    keep_filter = {(REPO / p).resolve(): pf for p, pf in keep_paths}

    # A prefix that matches nothing is always a mistake -- a typo silently
    # stubs the WHOLE file, which is the worst outcome available (correctly
    # sized arrays, no loader, renders as nothing). Same rule as the rewrite
    # rules in make_src_shrink.py: hard-error on no-match.
    for p, prefixes in keep_paths:
        if not prefixes:
            continue
        text = (REPO / p).read_text(encoding="utf-8", errors="surrogateescape")
        globs, stats, _ = scan_declarations(text)
        names = [n for n, _s in globs] + [n for n, _s in stats]
        for pref in prefixes:
            # Only INCLUSION prefixes are checked. An exclusion that matches
            # nothing is harmless -- it means the file has no winter half, and
            # 11 of the 84 obj_s_*.c legitimately do not. An inclusion that
            # matches nothing is fatal: it stubs the entire TU silently.
            if pref.startswith("!"):
                continue
            if not any(n.startswith(pref) for n in names):
                sys.stderr.write(
                    "make_stub_data.py: keep filter '{}#{}' matches NO array "
                    "in that file. A typo here stubs the whole TU silently.\n"
                    .format(p, pref))
                return 2
    table = parse_asset_table(ASSET_TABLE)

    # ⭐ T1's KEEP-LIST HALF, AND IT MUST HAPPEN BEFORE THE REWRITE LOOP.
    #
    # texpool_rows() runs every checksum the map is protected by, so a selector
    # that stopped matching fails here rather than quietly demand-loading a
    # smaller population than it claims. DEMAND_STUB is then read by
    # keep_symbol(), which every residency decision below runs through: the
    # rewrite loop (which arrays get [1] and which pc_load_asset() lines get
    # suppressed), emit_keep_inc() (which loader calls get emitted) and
    # _texpool_is_resident() (the `kept` flag in the map itself). One set, four
    # consumers — that is deliberate, because these four disagreeing is how you
    # get a full-size memcpy into a one-byte destination.
    tp_rows = []
    if texpool_demand or texpool_probe:
        tp_rows = texpool_rows(table)
    if texpool_demand:
        DEMAND_STUB.update(name for name, _b, _s, _o, _own in tp_rows)

    files = sorted(SRC.rglob("*.c"))
    total_arrays = 0
    total_saved = 0
    kept_arrays = 0
    kept_bytes = 0
    written = 0
    unchanged = 0
    stub_rel = []
    rewritten_cinc = set()

    neutralised = 0

    for f in files:
        text = f.read_text(encoding="utf-8", errors="surrogateescape")
        if "#ifdef TARGET_PC" not in text:
            # ⚠️ THE SECOND HALF OF THE .c_inc TRAP, found 2026-08-03.
            #
            # cinc_includes() (above) taught this tool that a TU's asset arrays
            # and its _pc_load_src_*() can live in an #included .c_inc. But the
            # .c_inc handling sits BELOW this early-continue, and the guard asks
            # the wrong file: it tests the .c for "#ifdef TARGET_PC" and skips
            # the whole TU when it has none — which is exactly the shape of a TU
            # that keeps ALL of its asset code in the .c_inc.
            #
            # src/game/m_choice.c is the case that exposed it and it is the only
            # one: of the 193 keep-list entries it is the single file with an
            # asset-bearing .c_inc and no TARGET_PC guard of its own.
            # src/game/m_msg.c survived only because it happens to carry one.
            #
            # What it cost: m_choice_draw.c_inc was never written into the stub
            # tree, so the vendored copy compiled, its _pc_load_src_*() still
            # called pc_load_asset(), and on Dreamcast that has g_rel_data ==
            # NULL and no assets/ tree on /cd. Runtime evidence, in every log
            # back to 2026-08-02 and nowhere else — exactly two failures:
            #     [PC] ASSET MISSING: assets/con_waku_swaku3_tex.bin
            #     [PC] ASSET MISSING: assets/con_sentaku2_v.bin
            # con_waku_swaku3_tex is the reply box's ONLY texture (I4 128x64,
            # one filled ellipse; the panel has no 9-slice and no border) and
            # con_sentaku2_v is its four vertices. Zeroed, that is a fully
            # transparent texture on a degenerate zero-area quad: the choice
            # window has no box at all. Reported by a human as "the reply text
            # boxes are messed up". Nothing in the renderer is involved.
            #
            # It stayed hidden because the reply TEXT was missing too until the
            # 2026-08-02 tev_const_alpha fix, so there was no text to notice an
            # absent box around, and because dc_stub_keep.inc declares AND calls
            # _pc_load_src_game_m_choice_draw_c_inc() either way — the generated
            # header looked complete.
            if f.resolve() not in keep_abs or not cinc_includes(text, f):
                continue
        rel = f.relative_to(REPO)

        # NEUTRALISE runs before, and independently of, array stubbing: these
        # files declare no destinations of their own, so stub_file() would
        # return n == 0 and skip them entirely.
        if str(rel) in NEUTRALISE:
            new_text, n_rules = neutralise_file(str(rel), text)
            neutralised += n_rules
            stub_rel.append(str(rel))
            if not args.dry_run:
                if write_if_changed(out_root / rel, new_text):
                    written += 1
                else:
                    unchanged += 1
            continue

        if f.resolve() in keep_abs:
            prefixes = keep_filter.get(f.resolve()) or ()
            globs, stats, _ = scan_declarations(text)

            # ⭐ T1 MADE THE PARTIAL PATH THE COMMON ONE.
            #
            # Before T1 a keep-list entry with no '#' filter meant "this whole
            # TU is resident" and the file only needed its pc_load_asset()
            # calls renamed. T1 breaks that: a texture array usually shares its
            # TU with the vertex arrays and display lists that MUST stay, so a
            # whole-file keep now has to stub the texture symbols inside it.
            # partial_file() already does exactly that; the only new thing is
            # noticing when it is needed. keeplist-town.txt carries no '#'
            # filters at all, so before this line the partial path was dead
            # code and after it, it handles ~500 files.
            has_demand = any(n in DEMAND_STUB for n, _s in globs + stats)

            # A kept TU's loads can live in an #included .c_inc instead of in
            # the .c. Redirect those too and write them into the stub tree;
            # dc/Makefile puts $(STUBDIR)/include at the front of INCLUDES so
            # the rewritten copy shadows the vendored one, the same mechanism
            # DC_SRC_SHRINK already uses for its two .c_inc files.
            #
            # ⚠️ HOISTED ABOVE THE PARTIAL/WHOLE FORK, because it used to sit
            # only in the whole-file arm. With T1 pushing files into the partial
            # arm, leaving it there would stop rewriting their .c_inc — and
            # emit_keep_inc() hard-errors on that for the reason the reply-box
            # bug taught us (a .c_inc that still calls pc_load_asset() loads
            # nothing on Dreamcast, silently).
            # T1 never owns a .c_inc symbol: file-statics are one of its four
            # exclusions, so these still want the plain keep_file() rename.
            for cinc_rel, cinc_path in cinc_includes(text, f):
                cinc_text = cinc_path.read_text(
                    encoding="utf-8", errors="surrogateescape")
                _, cinc_stats, _ = scan_declarations(cinc_text)
                kept_arrays += len(cinc_stats)
                kept_bytes += sum(s for _, s in cinc_stats)
                cinc_new, cinc_n = keep_file(cinc_text)
                if cinc_n == 0 or args.dry_run:
                    continue
                rewritten_cinc.add(cinc_rel)
                if write_if_changed(out_root / cinc_rel, cinc_new):
                    written += 1
                else:
                    unchanged += 1

            if prefixes or has_demand:
                # PARTIAL keep: some arrays full size, the rest stubbed. Always
                # writes into the stub tree — unlike a whole-file keep, a partial
                # keep ALWAYS changes the source, so there is no "compiles
                # straight out of src/" case here.
                new_text, n_stub, saved, n_kept, _n_red = partial_file(
                    text, prefixes)
                total_arrays += n_stub
                total_saved += saved
                kept_arrays += n_kept
                kept_bytes += sum(s for n, s in globs + stats
                                  if keep_symbol(n, prefixes))
                stub_rel.append(str(rel))
                if not args.dry_run:
                    if write_if_changed(out_root / rel, new_text):
                        written += 1
                    else:
                        unchanged += 1
                continue

            # Full size. Only the load calls are redirected, and only if the
            # TU has any — otherwise it compiles straight out of src/.
            kept_arrays += len(globs) + len(stats)
            kept_bytes += sum(s for _, s in globs) + sum(s for _, s in stats)

            new_text, n_redirect = keep_file(text)
            if n_redirect == 0:
                continue
            stub_rel.append(str(rel))
            if args.dry_run:
                continue
            if write_if_changed(out_root / rel, new_text):
                written += 1
            else:
                unchanged += 1
            continue

        new_text, n, saved = stub_file(text)
        if n == 0:
            continue
        total_arrays += n
        total_saved += saved
        stub_rel.append(str(rel))
        if args.dry_run:
            continue
        if write_if_changed(out_root / rel, new_text):
            written += 1
        else:
            unchanged += 1

    inc_text, (n_rows, n_inits, n_unmapped, n_load_bytes, n_cinc) = emit_keep_inc(
        keep_paths, table, None if args.dry_run else rewritten_cinc
    )
    bgtex_text, n_bgtex, n_bgtex_bytes = emit_bgtex_map(table, bgtex_demand)
    npctex_text, n_npctex, n_npctex_bytes = emit_npctex_map(
        table, keep_paths, npctex_pool)
    npcmdl_text, n_npcmdl, n_npcmdl_bytes = emit_npcmdl_map(
        keep_paths, npcmdl_pool)
    texpool_text, n_texpool, n_texpool_bytes = emit_texpool_map(
        tp_rows, keep_paths, texpool_probe, texpool_demand)

    pruned = 0
    if not args.dry_run:
        # PRUNE FIRST. dc/Makefile's stubify swaps src/foo.c for the stub tree
        # on $(wildcard $(STUBDIR)/foo.c) — presence, not stub.list membership.
        # So a file that USED to be stubbed and is now on the keep list would
        # keep compiling from its stale [1]-sized copy, silently. Delete every
        # .c under the out tree that this run did not just produce.
        want = {(out_root / p).resolve() for p in stub_rel}
        for old in out_root.rglob("*.c"):
            if old.resolve() not in want:
                old.unlink()
                pruned += 1

        # The list make reads. One repo-relative path per line, sorted, so the
        # Makefile can $(filter) against its own source list deterministically.
        out_root.mkdir(parents=True, exist_ok=True)
        (out_root / "stub.list").write_text("\n".join(sorted(stub_rel)) + "\n")
        if write_if_changed(out_root / KEEP_INC_NAME, inc_text):
            written += 1
        else:
            unchanged += 1
        if write_if_changed(out_root / BGTEX_INC_NAME, bgtex_text):
            written += 1
        else:
            unchanged += 1
        if write_if_changed(out_root / NPCTEX_INC_NAME, npctex_text):
            written += 1
        else:
            unchanged += 1
        if write_if_changed(out_root / NPCMDL_INC_NAME, npcmdl_text):
            written += 1
        else:
            unchanged += 1
        if write_if_changed(out_root / TEXPOOL_INC_NAME, texpool_text):
            written += 1
        else:
            unchanged += 1

    if not args.quiet:
        print("== DC_ASSET_STUB tree ==")
        print("  out           : {}".format(out_root))
        print("  files stubbed : {}".format(len(stub_rel)))
        print("  arrays stubbed: {}".format(total_arrays))
        print("  bytes removed : {:,}".format(total_saved))
        print("  neutralised   : {} rule(s) in {} file(s)".format(
            neutralised, len(NEUTRALISE)))
        print("  -- keep allowlist ({}) --".format(keep_why))
        print("  files kept    : {}".format(len(keep_paths)))
        print("  arrays kept   : {}".format(kept_arrays))
        print("  bytes kept    : {:,}".format(kept_bytes))
        print("  {:<14}: {} table rows + {} .c_inc rows + {} per-file init"
              " fns ({:,} B)".format(KEEP_INC_NAME, n_rows, n_cinc, n_inits,
                                     n_load_bytes))
        print("  {:<14}: {} rows, {:,} B NOT resident   [R1, "
              "--bgtex-demand={}]".format(
                  BGTEX_INC_NAME, n_bgtex, n_bgtex_bytes, args.bgtex_demand))
        if not bgtex_demand:
            print("                  {} mFM_grd_*.c file(s) forced back onto the"
                  " keep list".format(len(BGTEX_KEEP_RESTORE)))
        print("  {:<14}: {} sets, {:,} B NOT resident  [R2, "
              "--npctex-pool={}]".format(
                  NPCTEX_INC_NAME, n_npctex, n_npctex_bytes, args.npctex_pool))
        if not npctex_pool:
            print("                  {} villager tex file(s) forced back onto"
                  " the keep list".format(len(NPCTEX_KEEP_RESTORE)))
        print("  {:<14}: {} species, {:,} B NOT resident [R3, "
              "--npcmdl-pool={}]".format(
                  NPCMDL_INC_NAME, n_npcmdl, n_npcmdl_bytes, args.npcmdl_pool))
        if not npcmdl_pool:
            print("                  {} villager mdl file(s) forced back onto"
                  " the keep list".format(len(NPCMDL_KEEP_RESTORE)))
        # The probe's row count is printed unconditionally, and so is its
        # resident/stubbed split. A generated list that is silently inert is what
        # left G1 unrun for two sessions (dc/opt-lists.mk), and the split is the
        # number that says how much of T1 is a SAVING and how much is content
        # that does not exist in the image yet.
        print("  {:<14}: {} rows, {:,} B mapped        [T1 "
              "--texpool-demand={} --texpool={}]".format(
                  TEXPOOL_INC_NAME, n_texpool, n_texpool_bytes,
                  args.texpool_demand, args.texpool))
        if texpool_probe or texpool_demand:
            import re as _re
            _m = _re.search(r"RESIDENT_N\s+(\d+)", texpool_text)
            _b = _re.search(r"RESIDENT_B\s+(\d+)", texpool_text)
            _sn = _re.search(r"STUBBED_N\s+(\d+)", texpool_text)
            _sb = _re.search(r"STUBBED_B\s+(\d+)", texpool_text)
            _mx = _re.search(r"MAX_B\s+(\d+)", texpool_text)
            print("                  resident {} rows / {:,} B  (still in .bss)"
                  .format(int(_m.group(1)), int(_b.group(1))))
            print("                  demand   {} rows / {:,} B  ({})".format(
                int(_sn.group(1)), int(_sb.group(1)),
                "read off /cd on first bind" if texpool_demand
                else "content the image does not have today"))
            print("                  staging buffer {:,} B (largest row)"
                  .format(int(_mx.group(1))))
        if n_unmapped:
            print("  WARNING       : {} kept global(s) have no s_assets[] row"
                  " and will stay zeroed".format(n_unmapped))
        if not args.dry_run:
            print("  written       : {}  (unchanged {}, pruned {})".format(
                written, unchanged, pruned))

    return 0


if __name__ == "__main__":
    sys.exit(main())
