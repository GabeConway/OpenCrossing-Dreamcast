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

COST
----
Real bytes, and the fit inequality is already over by ~4.7 MB, so this is only
affordable because a DC_ASSET_STUB image has margin the shipping image will
not. Read `MEMLEDGER FIT` on the build; if margin goes negative, the town
image needs S4, not a bigger keep list.

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
    """
    out = []
    for fn in sorted(os.listdir(MODEL_DIR)):
        if fn.startswith("obj_s_") and fn.endswith(".c"):
            out.append(rel(os.path.join(MODEL_DIR, fn)))
    return sorted(out)


def main():
    for d in (ACRE_DIR, MODEL_DIR):
        if not os.path.isdir(d):
            sys.exit("missing tree: %s" % d)

    opening = opening_entries()
    acres = acre_sources()
    structs = structure_sources()

    seen = set()
    def emit(items):
        n = 0
        for it in items:
            if it in seen:
                continue
            seen.add(it)
            print(it)
            n += 1
        return n

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
    print("# ---- every summer town structure (obj_s_*) ----")
    n_str = emit(structs)

    sys.stderr.write(
        "keeplist-town: %d entries (%d censused + %d acre + %d structure)\n"
        % (len(seen), n_open, n_acre, n_str))


if __name__ == "__main__":
    main()
