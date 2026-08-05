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
  2. every acre display-list/vertex TU under src/data/field/bg/acre/
  3. every summer town structure src/data/model/obj_s_*.c

Winter (`mFM_grd_w_*`) ground textures are still deliberately absent, exactly
as in keeplist-opening.txt: `mFM_LoadBGCommonTex` (m_field_make.c:1113-1123)
switches to `l_bg_w_tex_segment_table` when the console RTC says winter, and
the entire town ground will go black in December with the same signature as
the bug kb/station-bugs.md §1 fixed. Add them here at the same time as
INDIRECT_SOURCES gets its `mFM_grd_w_*` rows.

⚠️⚠️ COST — THE FULL LIST DOES NOT FIT TODAY. MEASURED 2026-08-04.
------------------------------------------------------------------
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

Until then this file is a MEASUREMENT ARTEFACT and an aspiration, not a build
input. `keeplist-opening.txt` is what boots.

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


def structure_sources():
    """The summer town structures: obj_s_*.c under src/data/model/.

    `obj_s_` is the season prefix the field-make tables use for the
    non-winter set, the same convention as `mFM_grd_s_*`.
    ⚠️ EACH obj_s_*.c CARRIES BOTH SEASONS, so a plain whole-file keep buys
    winter too. obj_s_house1.c is 42,624 B of obj_s_house1_* and 42,720 B of
    obj_w_house1_*; across the 84 files it is 328,736 B of summer against
    223,456 B of winter. The season is chosen at runtime (ac_shop.c:92-94,
    ac_shop_draw.c_inc:52), so in a summer town the winter half can never be
    drawn and is pure dead weight -- and at ~146-181 KB of real headroom
    (kb/RESUME.md §2b) it is the difference between "all 84 structures fit" and
    "they do not".

    So every entry gets make_stub_data.py's exclusion filter '#!obj_w_'.

    ⚠️ EXCLUSION, not the '#obj_s_' inclusion form: 3,680 B across nine of
    these files are season-NEUTRAL and named neither obj_s_ nor obj_w_
    (obj_kanban_pal, hakushi_tex, obj_lotus_leaf_tex_txt,
    obj_shop4_grass_tex_pic_i4 ...). Keeping only 'obj_s_' would stub those,
    and a stubbed palette does not fail loudly -- it renders its model in
    garbage colours.

    ⚠️ DATED TIME BOMB. A winter town built from this list draws every one of
    these 84 structures as a black spiky mess, exactly like the winter ground
    (kb/RESUME.md §4 item 3). Both need a DC_SEASON=winter build.
    """
    out = []
    for fn in sorted(os.listdir(MODEL_DIR)):
        if fn.startswith("obj_s_") and fn.endswith(".c"):
            out.append(rel(os.path.join(MODEL_DIR, fn)) + "#!obj_w_")
    return sorted(out)


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
)

# ---------------------------------------------------------------------------
# INTERIOR acres, excluded by default.
#
# acre_sources() globs the whole tree, which sweeps in building interiors and
# the developers' scratch rooms. They cost 269,312 B -- against 109,936 B for
# every summer structure and roughly 146-181 KB of measured headroom
# (kb/RESUME.md §2b), so keeping both is not a choice this list can make.
#
# The outdoor town is what the port currently walks, so interiors are off by
# default and --interiors turns them on. When the villager pool lands and frees
# room, this is the next thing to switch on.
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

    want_interiors = "--interiors" in sys.argv

    opening = opening_entries()
    acres = acre_sources()
    if not want_interiors:
        acres = [a for a in acres if not is_interior(a)]
    structs = structure_sources()
    extras = [e for e in EXTRA_SOURCES]

    missing = [e for e in extras if not os.path.isfile(os.path.join(ROOT, e))]
    if missing:
        sys.exit("EXTRA_SOURCES names files that do not exist: %s"
                 % ", ".join(missing))

    # ⚠️ DEDUPE ON THE PATH, NOT ON THE WHOLE ENTRY. keeplist-opening.txt names
    # 13 of these structures WITHOUT a filter, and 'foo.c' != 'foo.c#!obj_w_'
    # as strings -- so a naive set would emit both, make_stub_data.py would see
    # the unfiltered one too, and every byte of winter this list exists to drop
    # would come straight back in. The structures are emitted last and win,
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
    print("# ⚠️ Winter ground (mFM_grd_w_*) is still absent on purpose -- see the")
    print("# generator header. The town ground goes black in December.")
    print("")
    print("# ---- censused opening/train/town working set (keeplist-opening.txt) ----")
    n_open = emit(opening)
    print("")
    print("# ---- every acre (%d files): vertex arrays, or the acre draws nothing ----"
          % len(acres))
    n_acre = emit(acres)
    print("")
    print("# ---- every summer town structure (obj_s_*), winter excluded ----")
    n_str = emit(structs)
    print("")
    print("# ---- map overlay, clock HUD, Tom Nook and the raccoons ----")
    print("# Not derivable from a glob. These used to be typed into the")
    print("# generated file by hand, so regenerating it deleted them -- caught")
    print("# by a human noticing Tom Nook. See EXTRA_SOURCES.")
    n_extra = emit(extras)

    sys.stderr.write(
        "keeplist-town: %d entries (%d censused + %d acre + %d structure "
        "+ %d extra)%s\n"
        % (len(seen), n_open, n_acre, n_str, n_extra,
           "" if want_interiors else "  [interiors excluded]"))


if __name__ == "__main__":
    main()
