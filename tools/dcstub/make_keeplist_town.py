#!/usr/bin/env python3
"""Generate tools/dcstub/keeplist-town.txt -- the WIDE keep list.

WHY THIS EXISTS, AND WHY A CENSUS CANNOT REPLACE IT
===================================================
`keeplist-opening.txt` is censused: it names what two specific 600 s runs
actually DREW. That is exactly the right tool for the title screen and the
train intro, which are scripted and identical every time. It is the wrong
tool for the town, and not by a little:

  * `mFM_DecideAcre` builds the town layout from the save's random seed, so
    two runs of the same build visit different acres. A census from run A is
    not a valid keep list for run B, and censusing harder does not converge.
  * An unkept acre does not degrade -- it VANISHES. The acre `.c` files stub
    their vertex array (`grd_s_t_st1_2.c:15-16` is
    `static Vtx grd_s_t_st1_2_v[0xF00 / sizeof(Vtx)]` under TARGET_PC), so
    every triangle in the acre's unstubbed display list collapses to the
    origin and the acre renders nothing at all. Same shape for the `obj_s_*`
    town structures.

`keeplist-opening.txt` covers 18 of 268 acres and 11 of 84 summer structures.
That is the "missing and weird textures" report: most of the town is not
missing a texture, it is missing its GEOMETRY.

So this list is enumerated from the tree, not measured from a run. It is a
UNION with the censused list -- never a replacement (see that file's header;
a town census would drop the animal textures the player-select scene needs).

WHAT IT KEEPS
-------------
  1. every entry of keeplist-opening.txt, verbatim
  2. every acre display-list/vertex TU under src/data/field/bg/acre/,
     INTERIORS INCLUDED (`--no-interiors` drops them again)
  3. every town structure src/data/model/obj_s_*.c, BOTH SEASONS
     (`--no-winter` restores the summer-only `#!obj_w_` filter)
  4. EXTRA_SOURCES: the map overlay, the clock HUD, Tom Nook and the
     raccoons, and the 132-file gyroid set -- none of them reachable by any
     glob this generator runs, all of them geometry that VANISHES if stubbed

Ground textures -- summer AND winter -- are absent from this list on purpose
and must NOT be added back. R1 demand-loads all 96 `mFM_grd_*` source arrays
off the disc (dc/src/dc_bgtex.c), so both seasons work and neither costs a
byte of `.bss`. Keeping them here would put 80,736 B back and buy nothing.
That also retires the December time bomb this header used to carry: the season
fork at `m_field_make.c:1120` picks `l_bg_w_tex_segment_table` from the
console RTC, and until R1 the winter half was simply unaffordable.

VILLAGER textures are absent for the same reason and must NOT be added back
either. R2 serves all 236 `src/data/npc/model/tex/` villager sets (993,984 B)
out of 16 resident slots read off the disc (dc/src/dc_npctex.c), which is why
the 21 that keeplist-opening.txt used to name are gone from it. Re-adding one
makes the pool decline to pool that set, so the `.bss` comes back and nothing
is gained. ⚠️ The SPECIAL NPCs are the exception in both directions: rows at
or above ALL_NPC_NUM are never pooled, so Tom Nook and the raccoons in
EXTRA_SOURCES below -- and the ten `end_1`/`kab_1`/`mnk_1`/`mob_1`/`mol_1`/
`mos_1`/`wip_1`/`wls_1`/`xct_1`/`xsq_1` sets in keeplist-opening.txt -- have to
stay on this list or they draw untextured.

VILLAGER MODELS are absent for the third time, and must NOT be added back
either. R3 serves all 32 villager species in `src/data/npc/model/mdl/`
(194,400 B) out of 16 resident slots read off the disc, relocating the 933
`gsSPVertex` words that name them (dc/src/dc_npcmdl.c), which is why the one
this list used to name — cbr_1 — is gone from keeplist-opening.txt. Re-adding
one makes the pool decline to pool that species and the `.bss` comes back for
nothing. ⚠️ Again the SPECIAL NPCs are the exception: the 40 skeletons only
rows >= ALL_NPC_NUM use are never pooled, so the six raccoon models in
EXTRA_SOURCES below -- and end_1/hgh_1/kab_1/mnk_1/xct_1 in
keeplist-opening.txt -- have to stay or Tom Nook loses his geometry.

⚠️ R3 COSTS BYTES, unlike R1 and R2. Only 5,536 B of villager model was ever
resident, so the 16-slot pool is a NET +115,296 B of `.bss` that buys 31
species the geometry they never had. The number that makes the pool the right
shape is the alternative, not today: keeping all 32 is 194,400 B.
`DC_NPCMDL_SLOTS=<N>` cuts it without turning it off.

COST — THE WIDE LIST FITS. MEASURED 2026-08-06.
-----------------------------------------------
The `-O0` ban was reversed (CLAUDE.md §1): `src/` at `-Os` + an `-O3` hot list
dropped `.text` by 2,826,288 B, and that bought ~2.48 MB of measured real
headroom. The two content exclusions this generator carried -- interiors and
the winter half of every structure -- were both scarcity decisions, and the
scarcity is gone. Built and run with BOTH switched on:

    .bss          3,945,484 -> 4,428,076   (+482,592)
    image span    8,926,124 -> 9,446,380
    MEMLEDGER FIT ... margin=5541012 OK,  ASSET MISSING 0

It reaches the town, screenshot-verified -- and unlike the 2026-08-04 run
below, this OK was checked against a run, not just against the ledger.

So both are ON by default now. The kill switches are `--no-interiors` (drops
the 269,312 B of interior/scratch acres) and `--no-winter` (restores the
`#!obj_w_` filter, dropping 223,456 B of winter structure). `--interiors` is
still accepted as a no-op so old invocations keep working.

⚠️⚠️ SUPERSEDED 2026-08-06 by the measurement above — the `-Os`/`-O3` reversal
is what superseded it, not any change to this list. Kept because the METHOD is
still the rule: a ledger `OK` is not a boot (kb/heap-two-pools.md), and the
headroom number that matters comes from an OOM pair.

COST — THE FULL LIST DOES NOT FIT TODAY. MEASURED 2026-08-04.
-------------------------------------------------------------
Built and run: `.bss` 3,296,236 -> 4,804,620, image span 11,084,460 ->
12,681,100, and `MEMLEDGER FIT ... margin=1606292 **OK**`.

**It said OK and the game died on the splash screen.**

    Out of memory. Requested sbrk_base 8d0be000, was 8cf5c000, diff 1449984

This is `kb/heap-two-pools.md` exactly: the margin the ledger reports IS libc's
pool, and the ledger does not model libc's demand, so "OK" here means only
"the image and the fixed reserves fit", not "the program runs". The real
arithmetic:

    libc peak demand   ~= 1,606,292 + 1,449,984  =  3,056,276
    margin at the opening keep list                 3,202,932
    ------------------------------------------------------------
    headroom actually available for a bigger keep list ~= 146,656 B

**~146 KB, not 1.6 MB.** So the wide list costs an order of magnitude more than
there is room for, and no amount of trimming acre-by-acre closes that at the
current `DC_ARENA_BYTES=1900000`.

The two ways forward, in order:

  1. **Cut the arena and hand the difference to libc.** `DC_ARENA_PROBE`
     measured the game's own allocator using **256,192 B of a 1,412,704 B
     zelda arena** inside the 1,900,000 knob -- but at the TITLE SCREEN only.
     A loaded town is unmeasured, and that measurement is the gate. Do not cut
     the shipping arena on the title figure.
  2. **S4.** `kb/plan-stages.md` -- demand-load the asset destination arrays
     instead of keeping them resident. This list is a stopgap for a stub
     image; S4 is the answer for a real one.

Route 1 is what happened, by a different door: the optimizer profile handed the
image back 2.83 MB and route 2 (S4) is still the answer for a real one. This
file stopped being a measurement artefact on 2026-08-06 -- it is the list the
town build uses. `keeplist-opening.txt` stays the list for size experiments and
title-screen work.

    python3 tools/dcstub/make_keeplist_town.py > tools/dcstub/keeplist-town.txt

    DC_STUB_KEEP="$(grep -v '^#' tools/dcstub/keeplist-town.txt | paste -sd: -)"
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", ".."))
OPENING = os.path.join(HERE, "keeplist-opening.txt")

ACRE_DIR = os.path.join(ROOT, "src", "data", "field", "bg", "acre")
MODEL_DIR = os.path.join(ROOT, "src", "data", "model")


def rel(path):
    return os.path.relpath(path, ROOT).replace(os.sep, "/")


def opening_entries():
    out = []
    with open(OPENING, "r") as f:
        for line in f:
            s = line.strip()
            if s and not s.startswith("#"):
                out.append(s)
    return out


def acre_sources():
    """Every .c under the acre tree.

    Deliberately ALL of them, not just `<dir>/<dir>.c`. The `_evw_anime.c`
    siblings are the animated variants of the same acre (waterfalls, the
    train crossing) and they carry their own vertex arrays; keeping the base
    acre and stubbing its animation is the half-fix that renders a hole.
    """
    out = []
    for dirpath, _dirnames, filenames in os.walk(ACRE_DIR):
        for fn in sorted(filenames):
            if fn.endswith(".c"):
                out.append(rel(os.path.join(dirpath, fn)))
    return sorted(out)


def structure_sources(no_winter=False):
    """The town structures: obj_s_*.c under src/data/model/, BOTH seasons.

    `obj_s_` is the season prefix the field-make tables use for the
    non-winter set, the same convention as `mFM_grd_s_*`.
    ⚠️ EACH obj_s_*.c CARRIES BOTH SEASONS, so a plain whole-file keep buys
    winter too. obj_s_house1.c is 42,624 B of obj_s_house1_* and 42,720 B of
    obj_w_house1_*; across the 84 files it is 328,736 B of summer against
    223,456 B of winter.

    DEFAULT: emit the bare path, NO filter -- keep everything. The 223,456 B
    of winter fits since the `-Os`/`-O3` reversal (header, 2026-08-06), and
    keeping it retires the dated time bomb below instead of rescheduling it.

    `--no-winter` restores the old behaviour: make_stub_data.py's EXCLUSION
    filter '#!obj_w_' on every entry. That was the right call while headroom
    was ~146-181 KB (kb/RESUME.md §2b), when the winter half -- unreachable at
    runtime in a summer town (ac_shop.c:92-94, ac_shop_draw.c_inc:52) -- was
    the difference between "all 84 structures fit" and "they do not".
    SUPERSEDED as a default 2026-08-06 by the measured 2.48 MB of headroom;
    kept as the kill switch.

    ⚠️ WHEN FILTERING, EXCLUSION, never the '#obj_s_' inclusion form: 3,680 B
    across nine of these files are season-NEUTRAL and named neither obj_s_ nor
    obj_w_ (obj_kanban_pal, hakushi_tex, obj_lotus_leaf_tex_txt,
    obj_shop4_grass_tex_pic_i4 ...). Keeping only 'obj_s_' would stub those,
    and a stubbed palette does not fail loudly -- it renders its model in
    garbage colours. Dropping the filter entirely, which is what the default
    now does, keeps them for the same reason and cannot hit this hazard.

    ⚠️ The dated time bomb -- a winter town drawing all 84 structures as a
    black spiky mess (kb/RESUME.md §4 item 3) -- is armed only under
    `--no-winter` now. The winter GROUND is separately handled by R1.
    """
    suffix = "#!obj_w_" if no_winter else ""
    out = []
    for fn in sorted(os.listdir(MODEL_DIR)):
        if fn.startswith("obj_s_") and fn.endswith(".c"):
            out.append(rel(os.path.join(MODEL_DIR, fn)) + suffix)
    return sorted(out)


# ---------------------------------------------------------------------------
# --full-model — spend T1's freed bytes on the rest of src/data/model
# ---------------------------------------------------------------------------
# T1 (dc/src/dc_texpool.c) takes 885,984 B of texture arrays out of .bss and
# reads them off the disc instead. That money has to go somewhere, and the
# cheapest content in the tree is the model files this list has never named:
# 1,545 of them, holding 1,957,224 B of NON-texture residue — vertex arrays and
# display lists, which cannot be demand-loaded because their pointers are baked
# into the display lists that reference them (R3 needed a 933-entry relocation
# table to do it for 32 villager models; there is no such table here).
#
# ⚠️ AN UNKEPT MODEL DOES NOT LOSE A TEXTURE, IT LOSES ITS GEOMETRY.
# make_stub_data.py rewrites its Vtx array to [1], so every triangle in the
# unstubbed display list collapses to the origin. That is the "black spiky mess"
# in kb/RESUME.md, and it is why this is worth buying at all: these bytes are
# not a quality improvement, they are the difference between a building existing
# and not existing.
#
# ⚠️ mFM_grd_* IS EXCLUDED AND MUST STAY EXCLUDED. R1 already demand-loads all
# 96 ground-texture sources (dc/src/dc_bgtex.c), both seasons, for no .bss.
# Adding them here costs 150,880 B and buys nothing.
#
# THE BUDGET IS NOT OPTIONAL AND IT IS NOT DECORATIVE. Measured 2026-08-09 the
# fit inequality leaves ~1,413,832 B of headroom after T1 (kb/STATE.md), against
# 1,806,344 B of eligible residue. The whole tree DOES NOT FIT — it is over by
# ~392 KB — so something has to be dropped, and the choice is between dropping
# it deliberately here or discovering it as `margin=-NNN OVER` and a build that
# does not boot. Files are taken in ASCENDING residue order, which maximises the
# NUMBER of complete content classes that fit; what did not fit is printed to
# stderr, because a silent cap reads as "covered everything" when it did not.
# ⚠️ MEASURED, NOT CHOSEN. At 1,300,000 the linked image came out at
# _end - 0x8c010000 = 11,012,828 B against a derived ceiling of 11,013,612 —
# 784 bytes, which is not a margin, it is a coincidence. And the ceiling itself
# rests on a libc peak measured on 2026-08-04 against a much smaller keep list
# and never re-derived (kb/STATE.md says so in as many words). 900,000 puts the
# span near 10.6 MB and leaves ~375 KB, which is enough to absorb being wrong
# about the peak. RAISE IT ONLY AGAINST AN OOM PAIR, never against arithmetic —
# `MEMLEDGER FIT ... OK` is not a statement that the image boots (rule 6).
MODEL_BUDGET_DEFAULT = 900000

# Files that must be kept whatever the budget says, because T1 EXCLUDED symbols
# they own. make_stub_data.py's scan_pointer_table_symbols() drops 24 texture
# symbols out of the loader's map — they are reached through a pointer table and
# then gSPSegment, so an address-keyed loader cannot vouch for them — which
# means the only way those textures exist is if their file is resident.
# Six files, 25,856 B. Four are already covered by a priority prefix; f_tree3.c
# and obj_s_tree3.c are not, and dropping either turns a tree's leaves into
# whatever the linker put next.
MODEL_REQUIRED = (
    "des_suuji.c", "des_tool.c", "des_tool2.c",
    "f_tree3.c", "obj_s_tree3.c", "obj_w_tree3.c",
)

# ⚠️ RANKING BY SIZE ALONE IS THE WRONG OBJECTIVE, AND IT WAS TRIED FIRST.
# Cheapest-first maximises the NUMBER of content classes that fit, and on this
# tree that meant 1,484 pieces of furniture at the price of `player_tool.c`
# (the held net, axe and rod), `rom_shop*` (Nook's shop interiors),
# `rom_myhome*_wall` (your own house), `dia_win` (the DIALOGUE BOX) and the
# whole inventory UI. Those are not decoration — they are the things a player
# walks into within a minute of leaving Nook, and the budget was quietly
# spending their bytes on trees.
#
# So: these families are taken FIRST, whole, in tree order, before the
# cheapest-first pass gets the remainder. Each is here because the game puts it
# in front of the player on the critical path, not because of what it costs.
#
#   player_      the tools the player holds
#   rom_         ROOM interiors: the shops, the museum, your house's walls
#   obj_shop_    the shop's merchandise displays and signage
#   inv_         the inventory / pockets UI
#   dia_ des_ req_ pas_ pwd_ sel_   dialogue, design, request, passwd panels
#   cal_ clk_ tim_ onp_ kei_ ctl_ sav_ clg_ kan_ gba_ shi_ mra_ fkm_ hni_ tyo_
#                the HUD and menu windows. The clock/date pair is behind choice
#                index 1 and is invisible to every census ever taken
#                (kb/RESUME.md §8), which is exactly why it has to be named.
#   obj_w_       ⭐ THE WINTER STRUCTURES, and they are the cheapest item in
#                this whole file: 43 files for 19,024 B after T1. They are the
#                other half of kb/RESUME.md §7 item 5 — R1 made the winter
#                GROUND demand-loadable but left these 84 structures stubbed,
#                so a winter town still draws every building as a black spiky
#                mess. Cheapest-first dropped them on the first run of this
#                ranking, which is exactly why the priority list exists.
MODEL_PRIORITY_PREFIXES = (
    "obj_w_",
    "player_", "rom_", "obj_shop_", "inv_",
    "dia_", "des_", "req_", "pas_", "pwd_", "sel_",
    "cal_", "clk_", "tim_", "onp_", "kei_", "ctl_", "sav_", "clg_",
    "kan_", "gba_", "shi_", "mra_", "fkm_", "hni_", "tyo_",
)


def _texpool_owned():
    """The symbol set T1 demand-loads, from make_stub_data.py itself.

    Imported rather than re-derived: the two must agree about which symbols are
    free after T1, and a second implementation of that scan is a second thing to
    keep in step. Returns an empty set if the generator cannot be imported, and
    says so — a silent empty set would overstate every residue below and
    under-fill the budget, which is the safe direction but a lie.
    """
    import importlib.util
    path = os.path.join(HERE, "make_stub_data.py")
    spec = importlib.util.spec_from_file_location("_mkstub", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    table = mod.parse_asset_table(mod.ASSET_TABLE)
    rows = mod.texpool_rows(table)
    return {name for name, _b, _s, _o, _own in rows}, mod


def full_model_sources(budget, already):
    """Every remaining src/data/model/*.c that fits, cheapest first.

    Returns (kept_paths, n_dropped, bytes_kept, bytes_dropped).
    `already` is the set of repo-relative paths the earlier sections emit, so
    the residue arithmetic prices only what this section ADDS.
    """
    owned, mod = _texpool_owned()

    cand = []
    for fn in sorted(os.listdir(MODEL_DIR)):
        if not fn.endswith(".c"):
            continue
        if fn.startswith("mFM_grd_"):
            continue                      # R1's, see the note above
        r = rel(os.path.join(MODEL_DIR, fn))
        if r in already:
            continue
        text = open(os.path.join(MODEL_DIR, fn), "r",
                    encoding="utf-8", errors="surrogateescape").read()
        if "#ifdef TARGET_PC" not in text:
            continue                      # nothing stubbed, nothing to buy back
        globs, stats, _ = mod.scan_declarations(text)
        residue = sum(s for n, s in globs + stats if n not in owned)
        if residue == 0:
            # Pure texture file: T1 already covers every byte of it, so keeping
            # it costs nothing and buys nothing. Emit it anyway — it is free,
            # and leaving it out would make the count disagree with the tree.
            cand.append((0, r))
            continue
        cand.append((residue, r))

    # Priority families first, whole and in tree order; then everything else
    # cheapest-first. `prio` sorts before `rest` and neither is re-sorted
    # against the other, which is the point — see MODEL_PRIORITY_PREFIXES.
    def is_prio(path):
        stem = path.rsplit("/", 1)[-1]
        return stem in MODEL_REQUIRED or stem.startswith(
            MODEL_PRIORITY_PREFIXES)

    prio = [c for c in cand if is_prio(c[1])]
    rest = sorted(c for c in cand if not is_prio(c[1]))

    kept, dropped, b_kept, b_drop = [], [], 0, 0
    for residue, r in prio + rest:
        if b_kept + residue > budget:
            b_drop += residue
            dropped.append((residue, r))
            continue
        kept.append(r)
        b_kept += residue

    # ⚠️ NO SILENT CAPS. What did not fit still renders as nothing, so the ten
    # biggest casualties are named rather than summarised — a count alone reads
    # as "a few small things" whatever it dropped.
    if dropped:
        sys.stderr.write("keeplist-full: budget dropped, largest first:\n")
        for residue, r in sorted(dropped, reverse=True)[:10]:
            sys.stderr.write("    %8d B  %s\n" % (residue, r))
        if len(dropped) > 10:
            sys.stderr.write("    ... and %d more\n" % (len(dropped) - 10))
    return sorted(kept), len(dropped), b_kept, b_drop


# ---------------------------------------------------------------------------
# HAND-ADDED ENTRIES. These are NOT derivable from a directory glob, and they
# were previously typed straight into keeplist-town.txt -- which meant that
# regenerating the file silently DELETED them. That regression was caught by a
# human noticing Tom Nook, so it is now the generator's job to emit them.
#
# Each group is here for a reason a glob cannot express:
#
#   kan_* / kan_tizu2 / mMP_house_pos_list   the START map overlay
#   clk_win / clk_jikan                      the clock / date-time HUD, which
#                                            lives behind choice index 1 and is
#                                            therefore invisible to every
#                                            census ever taken (kb/RESUME.md §3)
#   rcn/rcc/rcd/rcf/rcs/tuk _1               Tom Nook and the raccoon family.
#                                            tuk_1 is Tanukichi himself; the rc*
#                                            set is the shop staff. Unkept, an
#                                            NPC model loses its VERTEX array
#                                            and draws as a black spiky mess.
#                                            ⚠️ These six mdl/ entries are NOT
#                                            made redundant by R3: all six
#                                            skeletons are named ONLY by rows
#                                            >= ALL_NPC_NUM, so R3's map has no
#                                            entry for them and the pool cannot
#                                            serve them. Deleting them here is
#                                            deleting Tom Nook.
#   hnw_* / *_haniwa / int_hnw*              the GYROIDS. Reported by a human
#                                            as rendering wrong in the town:
#                                            only 12 gyroid files were kept out
#                                            of the 132 in the tree (5 base +
#                                            127 int_hnw*), so the rest were
#                                            stubbed, their Vtx arrays are
#                                            zero-filled and every triangle
#                                            collapses to the origin -- the
#                                            identical failure shape as the Tom
#                                            Nook black spiky mess. hnw_model.c
#                                            is the one the TOWN gyroid draws
#                                            (hnw_v, 276 Vtx, ac_haniwa.c:117
#                                            -> cKF_Si3_draw_R_SV) and is the
#                                            reported bug; hnw_face.c is its
#                                            palette, and ⚠️ a stubbed palette
#                                            does not fail loudly, it renders
#                                            the model in garbage colours (the
#                                            same hazard structure_sources()'s
#                                            filter block warns about).
#                                            COST 432,160 B of image span
#                                            (.bss +363,808, .text +60,824),
#                                            from the LINK, 2026-08-06 --
#                                            against ~2.48 MB of measured real
#                                            headroom that day, leaving ~2.05.
#                                            ⚠️ AN EARLIER FIGURE OF 155,360 B
#                                            IN THIS COMMENT WAS 2.8x LOW. It
#                                            summed the Vtx arrays only (base
#                                            4,544 + 127 int_hnw* 150,816); each
#                                            int_hnw*.c also carries textures
#                                            and display lists. Cost a keep-list
#                                            addition from two links, never from
#                                            summing the arrays you went looking
#                                            for.
#                                            ⚠️ DO NOT "FIX" THE STILLNESS.
#                                            Gyroids animate in sync with the
#                                            music via
#                                            sAdo_GetRhythmAnimCounter(); at
#                                            DC_AUDIO=0 get_rhythm_buffer()
#                                            returns -2.0f (rhythm.c:148-155)
#                                            and ac_hnw_common.c:456-458
#                                            correctly reads that as "hold the
#                                            idle cKF_ba_r_int_hnw_off pose".
#                                            That is the intended fallback, the
#                                            same shape as K.K.'s frozen strum
#                                            (kb/RESUME.md §5b). Restoring the
#                                            geometry is the whole fix.
# ---------------------------------------------------------------------------
EXTRA_SOURCES = (
    # START map overlay
    "src/data/model/kan_eki.c",
    "src/data/model/kan_fune.c",
    "src/data/model/kan_gomi.c",
    "src/data/model/kan_hyouji.c",
    "src/data/model/kan_hyouji2.c",
    "src/data/model/kan_hyouji3.c",
    "src/data/model/kan_tizu.c",
    "src/data/model/kan_waku.c",
    "src/data/model/kan_win.c",
    "src/data/model/mMP_house_pos_list.c",
    "src/data/submenu/map/kan_tizu2.c",
    # clock / date-time HUD
    "src/data/model/clk_win.c",
    "src/data/model/clk_jikan.c",
    # Tom Nook and the raccoons, model + texture
    "src/data/npc/model/mdl/rcc_1.c",
    "src/data/npc/model/mdl/rcd_1.c",
    "src/data/npc/model/mdl/rcf_1.c",
    "src/data/npc/model/mdl/rcn_1.c",
    "src/data/npc/model/mdl/rcs_1.c",
    "src/data/npc/model/mdl/tuk_1.c",
    "src/data/npc/model/tex/rcc_1.c",
    "src/data/npc/model/tex/rcd_1.c",
    "src/data/npc/model/tex/rcf_1.c",
    "src/data/npc/model/tex/rcn_1.c",
    "src/data/npc/model/tex/rcs_1.c",
    "src/data/npc/model/tex/tuk_1.c",
    # the gyroids: the five base files. hnw_model.c is the town gyroid's
    # geometry, hnw_face.c its palette, hnw_move.c its motion data; the other
    # two are the dropped-item and inventory-icon forms.
    "src/data/model/hnw_face.c",
    "src/data/model/hnw_model.c",
    "src/data/model/hnw_move.c",
    "src/data/model/obj_item_haniwa.c",
    "src/data/model/inv_mwin_haniwa.c",
)


def gyroid_interior_sources():
    """The 127 indoor gyroid variants: src/data/model/int_hnw*.c.

    Globbed rather than typed as 127 literals, but kept HERE next to
    EXTRA_SOURCES rather than folded into acre_sources()/structure_sources():
    these are hand-added for a reported bug, not part of either sweep, and the
    intent has to stay visible. They go through the same existence check as the
    rest of EXTRA_SOURCES, so a rename fails loudly.
    """
    out = []
    for fn in sorted(os.listdir(MODEL_DIR)):
        if fn.startswith("int_hnw") and fn.endswith(".c"):
            out.append(rel(os.path.join(MODEL_DIR, fn)))
    return sorted(out)


# ---------------------------------------------------------------------------
# INTERIOR acres, INCLUDED by default since 2026-08-06.
#
# acre_sources() globs the whole tree, which sweeps in building interiors and
# the developers' scratch rooms. They cost 269,312 B, and they are kept:
# the `-Os`/`-O3` reversal bought ~2.48 MB and a build with interiors AND
# winter structures measured margin=5541012 OK, ASSET MISSING 0, reaching the
# town (header, 2026-08-06). `--no-interiors` is the kill switch.
#
# SUPERSEDED, kept for the reasoning: the exclusion existed because 269,312 B
# stood against 109,936 B for every summer structure and roughly 146-181 KB of
# measured headroom (kb/RESUME.md §2b), so keeping both was not a choice this
# list could make, and the outdoor town was what the port walked. What
# superseded it is the optimizer profile, not any change to the acres.
# `--interiors` stays accepted as a no-op alias so old invocations still work.
# ---------------------------------------------------------------------------
INTERIOR_PREFIXES = (
    "rom_",      # museum, tailor, shop interiors, lighthouse, tent, fortune
    "room",      # room01
    "tmp",       # tmp, tmp2..4, tmpr, tmpr2..4 -- scratch/temp interiors
    "myr_etc",
    "grd_post_office",
    "grd_yamishop",
)


def is_interior(rel_path):
    """True for an acre TU that is a building interior or a scratch room."""
    tail = rel_path.split("/acre/", 1)[-1]
    head = tail.split("/", 1)[0]
    return any(head.startswith(p) for p in INTERIOR_PREFIXES)


def main():
    for d in (ACRE_DIR, MODEL_DIR):
        if not os.path.isdir(d):
            sys.exit("missing tree: %s" % d)

    # `--interiors` is a NO-OP alias: interiors are on by default now, and
    # accepting the old flag keeps every existing invocation working.
    want_interiors = "--no-interiors" not in sys.argv
    want_winter = "--no-winter" not in sys.argv
    # --full-model: everything else under src/data/model that fits. OPT-IN, so
    # regenerating keeplist-town.txt is unchanged and only the new
    # keeplist-full.txt carries it.
    want_full_model = "--full-model" in sys.argv
    model_budget = MODEL_BUDGET_DEFAULT
    for a in sys.argv[1:]:
        if a.startswith("--model-budget="):
            model_budget = int(a.split("=", 1)[1])

    opening = opening_entries()
    acres = acre_sources()
    if not want_interiors:
        acres = [a for a in acres if not is_interior(a)]
    structs = structure_sources(no_winter=not want_winter)
    extras = list(EXTRA_SOURCES) + gyroid_interior_sources()

    missing = [e for e in extras if not os.path.isfile(os.path.join(ROOT, e))]
    if missing:
        sys.exit("EXTRA_SOURCES names files that do not exist: %s"
                 % ", ".join(missing))

    # ⚠️ DEDUPE ON THE PATH, NOT ON THE WHOLE ENTRY. keeplist-opening.txt names
    # 13 of these structures WITHOUT a filter, and 'foo.c' != 'foo.c#!obj_w_'
    # as strings -- so a naive set would emit both, make_stub_data.py would see
    # the unfiltered one too, and every byte of winter --no-winter exists to
    # drop would come straight back in. (With winter kept, the default since
    # 2026-08-06, there is no filter to lose and this is a no-op -- it is the
    # --no-winter path that still needs it.) The structures are emitted last,
    # which is why this keeps the LATER entry for a path already seen.
    # Pass 1: resolve. A path may appear in more than one section, and the
    # filtered form must win wherever it does.
    resolved = {}
    for it in opening + acres + structs + extras:
        path = it.split("#", 1)[0]
        if path not in resolved or ("#" in it and "#" not in resolved[path]):
            resolved[path] = it

    # Pass 2: print each path once, in section order, using the resolved form.
    printed = set()
    def emit(items):
        n = 0
        for it in items:
            path = it.split("#", 1)[0]
            if path in printed:
                continue
            printed.add(path)
            print(resolved[path])
            n += 1
        return n
    seen = printed

    print("# DC_STUB_KEEP -- the WIDE (town) keep list. GENERATED, do not edit.")
    print("#")
    print("#   python3 tools/dcstub/make_keeplist_town.py > tools/dcstub/keeplist-town.txt")
    print("#")
    print("# Rationale, and why a census cannot produce this list, is in the")
    print("# header of the generator. Short version: the town layout is drawn")
    print("# from the save's random seed, so a census names the acres ONE run")
    print("# happened to visit; and an unkept acre does not lose a texture, it")
    print("# loses its vertex array and renders nothing at all.")
    print("#")
    print("# This is a UNION with keeplist-opening.txt, which stays the list to")
    print("# use for size experiments and for title-screen work.")
    print("#")
    print("# Use:")
    print('#   DC_STUB_KEEP="$(grep -v \'^#\' tools/dcstub/keeplist-town.txt | paste -sd: -)"')
    print("#")
    print("# ⚠️ The mFM_grd_* ground textures are absent on purpose: R1 reads")
    print("# them off the disc (dc/src/dc_bgtex.c), both seasons, for no .bss.")
    print("# Adding them back costs 80,736 B and buys nothing.")
    print("#")
    print("# ⚠️ So are the VILLAGER npc/model/tex sets: R2 serves all 236 of")
    print("# them out of 16 slots (dc/src/dc_npctex.c). Re-adding one makes the")
    print("# pool skip that set, so the .bss returns for nothing. The SPECIAL")
    print("# NPC sets below (Tom Nook, the raccoons, end_1, kab_1, ...) are NOT")
    print("# pooled and MUST stay.")
    print("#")
    print("# ⚠️ And so are the VILLAGER npc/model/mdl species: R3 serves all 32")
    print("# out of 16 slots and relocates the 933 display-list words that name")
    print("# them (dc/src/dc_npcmdl.c). Same rule -- re-adding one makes the")
    print("# pool skip that species. The SPECIAL NPC models below are NOT")
    print("# pooled (their skeletons appear in no villager row) and MUST stay.")
    print("# ⚠️ R3, unlike R1 and R2, COSTS ~115,296 B of .bss: it restores")
    print("# geometry 31 species never had rather than freeing anything.")
    print("")
    print("# ---- censused opening/train/town working set (keeplist-opening.txt) ----")
    n_open = emit(opening)
    print("")
    print("# ---- every acre (%d files, interiors %s): vertex arrays, or the acre draws nothing ----"
          % (len(acres), "INCLUDED" if want_interiors else "EXCLUDED"))
    n_acre = emit(acres)
    print("")
    if want_winter:
        print("# ---- every town structure (obj_s_*.c), BOTH seasons, no filter ----")
    else:
        print("# ---- every summer town structure (obj_s_*), winter excluded (--no-winter) ----")
    n_str = emit(structs)
    print("")
    print("# ---- map overlay, clock HUD, Tom Nook and the raccoons, the gyroids ----")
    print("# Not derivable from a glob. These used to be typed into the")
    print("# generated file by hand, so regenerating it deleted them -- caught")
    print("# by a human noticing Tom Nook. See EXTRA_SOURCES.")
    print("# The gyroids joined them 2026-08-06: hnw_model.c is the town")
    print("# gyroid's geometry (ac_haniwa.c:117), hnw_face.c its palette, and")
    print("# the 127 int_hnw* are the indoor variants. Stubbed, they collapse")
    print("# to the origin. Their stillness at DC_AUDIO=0 is the intended idle")
    print("# pose (rhythm.c:148-155), not a bug -- do not chase it.")
    n_extra = emit(extras)

    n_full = 0
    if want_full_model:
        full, n_drop, b_kept, b_drop = full_model_sources(
            model_budget, set(resolved))
        # emit() prints through `resolved`, which was built before this section
        # existed. These are all bare paths with no '#' filter, so registering
        # them is a plain identity insert — but it has to happen, or emit()
        # KeyErrors on the first one.
        for r in full:
            resolved.setdefault(r, r)
        print("")
        print("# ---- the rest of src/data/model, cheapest-first, up to the")
        print("#      %d B budget (--full-model). These are the models the town"
              % model_budget)
        print("#      list never named: unkept they lose their VERTEX arrays and")
        print("#      collapse to the origin, which is the black spiky mess.")
        print("#      Affordable only because T1 (dc/src/dc_texpool.c) took")
        print("#      885,984 B of texture arrays out of .bss.")
        print("#      ⚠️ NOT EVERYTHING FITS. What the budget dropped still")
        print("#      renders as nothing; the kept/dropped byte counts go to")
        print("#      the generator's STDERR rather than into this file, so a")
        print("#      stale number cannot outlive the tree it was measured on.")
        n_full = emit(full)
        sys.stderr.write(
            "keeplist-full: model residue kept %d B in %d files, "
            "DROPPED %d B in %d files (budget %d)\n"
            % (b_kept, n_full, b_drop, n_drop, model_budget))

    sys.stderr.write(
        "keeplist-town: %d entries (%d censused + %d acre + %d structure "
        "+ %d extra + %d model)  [interiors %s, winter %s]\n"
        % (len(seen), n_open, n_acre, n_str, n_extra, n_full,
           "in" if want_interiors else "OUT",
           "in" if want_winter else "OUT"))


if __name__ == "__main__":
    main()
