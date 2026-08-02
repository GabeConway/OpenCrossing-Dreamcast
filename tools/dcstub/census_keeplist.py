#!/usr/bin/env python3
"""
census_keeplist.py — turn a resolved census into a DC_STUB_KEEP list.

WHY
---
census_resolve.py answers "which asset SYMBOLS does this scene touch". The
stubber (make_stub_data.py) works in units of SOURCE FILES: DC_STUB_KEEP names
repo-relative .c files it leaves at full size. This closes that gap, so the
keep list is derived from a measurement instead of assembled by hand — which
is the part kb/STATE.md N1 says cannot be done statically, because the title
demo names its acres by BLOCK_COMBI index and its animals by profile ID.

    symbol  --(the linked map's `.bss.<sym>` section line)-->  object path
            --(strip the objdir prefix)-->                     src/....c
            --(intersect with stub.list)-->                    keepable

The map is the authority on purpose: it is link output, so it cannot disagree
with the build the way a grep over src/ can.

TWO FILTERS, BOTH LOAD-BEARING
------------------------------
1. **Non-stubbable files are kept, not dropped.** ⚠️ This filter used to
   exclude any source outside stub.list, reasoning that "files outside it are
   already full size, so naming them would be a no-op at best". **That is
   false, and it cost a session.** Under DC_ASSET_STUB the keep list is not a
   "do not shrink" list — it is the ONLY asset-loading path that runs at all,
   because dc_main.c skips pc_assets_init() entirely. A file that is full size
   but unlisted gets a correctly-sized .bss buffer and NO loader, which is
   strictly worse than being stubbed: the map looks right and the texture
   decodes to a transparent rectangle at runtime.

   This bites the .c_inc files specifically — make_stub_data.py globs "*.c",
   so a TU whose arrays and _pc_load_src_*() live in an #included .c_inc never
   enters stub.list. src/game/m_msg.c is the case that exposed it: the entire
   dialogue balloon was missing. make_stub_data.py's scan_cinc_loads() now
   emits those loads, so naming such a file here is exactly what makes it work.
   Files with nothing to contribute cost one comment line in the generated
   .inc.
2. **Non-asset symbols are dropped by name**: emu64's `texture_buffer_data`
   and `black_texture` scratch and `mFM_pal_area` are censused because they
   pass through GXLoadTexObj/GXLoadTlut, but they are engine state, not
   loadable assets. census_resolve.py already subtracts the first one from its
   own total.

USAGE
-----
    python3 tools/dcstub/census_resolve.py <console.log> \
        --sizes-from dc/build/nonstub/AnimalCrossing.elf --top 0 > census.txt
    python3 tools/dcstub/census_keeplist.py census.txt

    # then, verbatim:
    DC_STUB_KEEP="$(python3 tools/dcstub/census_keeplist.py census.txt --colon)" \
        DC_ASSET_STUB=1 bash dc/build-dc.sh

Options:
    --map PATH      linked map to resolve symbols through
                    (default dc/build/nonstub/AnimalCrossing.map, else
                     dc/build/AnimalCrossing.map)
    --stub-list P   default dc/build/stubsrc/stub.list
    --colon         print one ':'-joined line (what DC_STUB_KEEP wants)
    --with-default  also include make_stub_data.py's built-in logo keep list
    --verbose       report every symbol that did not resolve, and why

⚠️ Attribution on a stub image is not exact. Display lists are real even when
their destination arrays are [1] long, so a `gsSPVertex(&x_v[93], …)` lands
past its own symbol; census_resolve.py flags those batches "ambiguous". The
documented loop is: keep what resolves, rebuild, re-census — with the models
at their real sizes and spacing the join becomes exact and the second list is
the one to trust.
"""

import argparse
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# Engine scratch that flows through the GX entry points the census taps, but is
# not an asset and has no loadable source file.
NOT_ASSETS = {
    "texture_buffer_data",
    "black_texture",
    "mFM_pal_area",
    "end",
}

# ---------------------------------------------------------------------------
# INDIRECTION ALIASES — the census's one structural blind spot.
#
# The census resolves the address that was DRAWN. When the game draws out of a
# scratch buffer that some earlier frame filled by bcopy, the copy severs the
# attribution: the census names the buffer's own TU and never sees the source
# array. The result looks like a working keep list and renders black.
#
# The instance that cost a trace (kb/station-bugs.md §1): every acre draws its
# ground from the *_dummy buffers in src/game/m_bg_tex.c — bare .bss, no data,
# nothing stubbable — which m_field_make.c:1128 fills from the mFM_grd_*
# arrays and m_field_make.c:1092 fills from the bg palettes. The generated
# list therefore kept m_bg_tex.c, which is a no-op, and dropped every real
# texture and palette in the town.
#
# Keys are censused symbols; values are the source files that must be kept for
# that symbol to contain anything. Adding a row here is cheaper than the trace.
#
# ⚠️ SUMMER ONLY. m_field_make.c:1119 forks on season and picks the
# mFM_grd_w_* set for winter; add those rows before any winter test.
# ⚠️ Grep for other `bcopy(…, *_dummy…)` sites before trusting a new census.
_GRD = "src/data/model/mFM_grd_%s.c"
_BGPAL = "src/data/field/bg/%s_pal.c"

INDIRECT_SOURCES = {
    # ground/water textures — l_bg_tex_common_dummy <- l_bg_tex_segment_rom_start_s_*
    "earth_tex_dummy":     [_GRD % "s_earth"],
    "cliff_tex_dummy":     [_GRD % "s_cliff"],
    "bush_a_tex_dummy":    [_GRD % "s_bushA"],
    "bush_b_tex_dummy":    [_GRD % "s_bushB"],
    "grass_tex_dummy":     [_GRD % "s_grass"],
    "rail_tex_dummy":      [_GRD % "s_rail"],
    "station_tex_dummy":   [_GRD % "s_station"],
    "stone_tex_dummy":     [_GRD % "s_stone"],
    "river_tex_dummy":     [_GRD % "s_river"],
    "water_1_tex_dummy":   [_GRD % "water1_tex"],
    "water_2_tex_dummy":   [_GRD % "water2_tex"],
    "bridge_1_tex_dummy":  [_GRD % "s_bridge1"],
    "bridge_2_tex_dummy":  [_GRD % "s_bridge2"],
    "tekkyo_tex_dummy":    [_GRD % "s_tekkyo"],
    "tunnel_tex_dummy":    [_GRD % "s_tunnel"],
    "beach_tex_dummy":     [_GRD % "s_beach_tex"],
    "beach1_tex_dummy2":   [_GRD % "beachA_tex"],
    "beach2_tex_dummy2":   [_GRD % "beachB_tex"],
    "sand_tex_dummy":      [_GRD % "s_sand"],
    "wave1_tex_dummy":     [_GRD % "wave1_tex"],
    "wave2_tex_dummy":     [_GRD % "wave2_tex"],
    "wave3_tex_dummy":     [_GRD % "wave3_tex"],
    "sprashA_tex_dummy":   [_GRD % "sprashA_tex"],
    "sprashC_tex_dummy":   [_GRD % "sprashC_tex"],
    # palettes that travel in the same table as the textures
    "bridge_1_pal_dummy":  [_GRD % "s_bridge1_pal"],
    "bridge_2_pal_dummy":  [_GRD % "s_bridge2_pal"],
    "station_pal_dummy":   [_GRD % "s_station1_pal"],
    # monthly ground palettes — l_bg_pal_common_dummy <- l_bg_pal_segment_rom_start
    "earth_pal_dummy":     [_BGPAL % "earth"],
    "cliff_pal_dummy":     [_BGPAL % "cliff"],
    "bush_pal_dummy":      [_BGPAL % "bush"],
    "rail_pal_dummy":      [_BGPAL % "rail"],
    "beach_pal_dummy2":    [_BGPAL % "beach"],
    # m_field_make.c:613-620 — flower / weed / tree palettes copied into mFM_pal_t
    "mFM_pal_area": [
        "src/data/model/obj_a_01_flower.c",
        "src/data/model/mFM_obj_01_zassou.c",
        "src/data/model/mFm_obj_tree_01.c",
        "src/data/model/mFm_obj_tree_01_dol.c",
        "src/data/model/mFm_obj_palm_01.c",
        "src/data/model/obj_gold_01.c",
    ],
}

# `.bss.foo`, `.data.foo`, `.rodata.foo` … ⚠️ ld emits this in TWO shapes and
# a parser that knows only one silently loses symbols: when the section name
# is short enough it stays on the same line as the address/size/object,
# otherwise the name is alone and the rest is on the next line. Missing the
# one-line form cost exactly the 12 NPC vertex arrays (`grl_1_v` and friends)
# the first time this ran — the symbols the keep list most needs.
SEC_RE = re.compile(
    r"^\s\.(?:bss|data|rodata|sbss|sdata)\.([A-Za-z_.$][\w.$]*)\s*"
    r"(?:0x[0-9a-f]+\s+0x[0-9a-f]+\s+(\S+)\s*)?$")
OBJ_RE = re.compile(r"^\s+0x[0-9a-f]+\s+0x[0-9a-f]+\s+(\S+)\s*$")

# census_resolve.py table rows: symbol first, then numeric columns. The point
# table carries nm names (leading '_'); the vertex table carries source names.
ROW_RE = re.compile(r"^(\S+)\s+(\d+)\s")


def parse_census(path):
    """Every symbol named by either census_resolve table, nm underscore removed."""
    syms = []
    for line in Path(path).read_text(errors="replace").splitlines():
        m = ROW_RE.match(line)
        if not m:
            continue
        sym = m.group(1)
        if sym.startswith("__Z") or sym.startswith("_ZZ"):
            continue                      # C++ mangled: emu64 internals
        if sym.startswith("_"):
            sym = sym[1:]                 # this toolchain prefixes C symbols
        # An alias key survives the NOT_ASSETS filter: mFM_pal_area is engine
        # state with no source file of its own, but it is also the evidence
        # that the flower/tree palettes were copied into it.
        if sym in NOT_ASSETS and sym not in INDIRECT_SOURCES:
            continue
        syms.append(sym)
    return syms


def parse_map(path):
    """symbol -> object path, from the linker map's per-symbol section lines."""
    out = {}
    pending = None
    for line in Path(path).read_text(errors="replace").splitlines():
        m = SEC_RE.match(line)
        if m:
            if m.group(2):                       # one-line form: name + object
                out.setdefault(m.group(1), m.group(2))
                pending = None
            else:                                # two-line form: object next
                pending = m.group(1)
            continue
        if pending is not None:
            o = OBJ_RE.match(line)
            if o:
                out.setdefault(pending, o.group(1))
            pending = None
    return out


def obj_to_src(obj):
    """/work/dc/build/obj/src/data/…/x.c.o -> src/data/…/x.c"""
    if not obj.endswith(".o"):
        return None
    p = obj[:-2]
    i = p.find("/src/")
    if i < 0:
        return None
    src = p[i + 1:]
    # The stub and shrink trees mirror repo-relative paths under dc/build/.
    for tree in ("dc/build/stubsrc/", "dc/build/shrinksrc/"):
        if src.startswith(tree):
            src = src[len(tree):]
    return src


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("census", help="census_resolve.py output")
    ap.add_argument("--map", default=None)
    ap.add_argument("--stub-list", default=str(REPO / "dc/build/stubsrc/stub.list"))
    ap.add_argument("--colon", action="store_true")
    ap.add_argument("--with-default", action="store_true")
    ap.add_argument("--verbose", action="store_true")
    args = ap.parse_args()

    mapfile = args.map
    if mapfile is None:
        for c in ("dc/build/nonstub/AnimalCrossing.map", "dc/build/AnimalCrossing.map"):
            if (REPO / c).exists():
                mapfile = str(REPO / c)
                break
    if mapfile is None:
        sys.exit("no linker map found; pass --map")

    syms = parse_census(args.census)
    sym2obj = parse_map(mapfile)
    stubbable = set()
    sl = Path(args.stub_list)
    if sl.exists():
        stubbable = {l.strip() for l in sl.read_text().splitlines() if l.strip()}

    keep, missing, not_stubbed = [], [], []
    seen = set()
    aliased = []
    for s in syms:
        # Follow the indirection first: a censused scratch buffer names the
        # sources that fill it as well as (or instead of) its own TU.
        for src in INDIRECT_SOURCES.get(s, ()):
            if not (REPO / src).exists():
                sys.exit("alias target does not exist: %s (for %s)" % (src, s))
            if src not in seen:
                seen.add(src)
                keep.append(src)
                aliased.append((s, src))
        if s in NOT_ASSETS:
            continue                       # alias key only; no source of its own
        obj = sym2obj.get(s)
        if obj is None:
            missing.append(s)
            continue
        src = obj_to_src(obj)
        if src is None:
            missing.append(s)
            continue
        # Recorded for the report only. NOT skipped — see filter 1 in the
        # module docstring: outside stub.list is where the .c_inc assets live,
        # and those are precisely the ones with no loader unless named here.
        if stubbable and src not in stubbable:
            not_stubbed.append((s, src))
        if src not in seen:
            seen.add(src)
            keep.append(src)

    if args.with_default:
        sys.path.insert(0, str(REPO / "tools" / "dcstub"))
        from make_stub_data import DEFAULT_KEEP           # noqa: E402
        for src in DEFAULT_KEEP:
            if src not in seen:
                seen.add(src)
                keep.append(src)

    keep.sort()

    if args.verbose:
        print("# symbols censused:      %d" % len(syms), file=sys.stderr)
        print("# files to keep:         %d" % len(keep), file=sys.stderr)
        print("# unresolved symbols:    %d" % len(missing), file=sys.stderr)
        for s in missing[:40]:
            print("#   unresolved %s" % s, file=sys.stderr)
        print("# added via indirection: %d" % len(aliased), file=sys.stderr)
        for s, src in aliased:
            print("#   alias %-22s -> %s" % (s, src), file=sys.stderr)
        print("# already full size:     %d" % len(not_stubbed), file=sys.stderr)
        for s, src in not_stubbed[:40]:
            print("#   not stubbed %-40s %s" % (s, src), file=sys.stderr)

    if args.colon:
        print(":".join(keep))
    else:
        for k in keep:
            print(k)


if __name__ == "__main__":
    main()
