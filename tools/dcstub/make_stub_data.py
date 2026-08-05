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

USAGE
-----
    python3 tools/dcstub/make_stub_data.py [--out DIR] [--keep LIST]
                                           [--bgtex-demand 0|1]
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


def keep_symbol(name, prefixes):
    """Does `name` survive a partial keep's prefix filter?

    Inclusion prefixes are a whitelist; '!'-prefixed ones are a blacklist that
    always wins. With only exclusions, everything not excluded is kept.
    """
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
        if prefixes:
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
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    bgtex_demand = bool(args.bgtex_demand)
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

        if f.resolve() in keep_abs and keep_filter.get(f.resolve()):
            # PARTIAL keep: some arrays full size, the rest stubbed. Always
            # writes into the stub tree — unlike a whole-file keep, a partial
            # keep ALWAYS changes the source, so there is no "compiles straight
            # out of src/" case here.
            prefixes = keep_filter[f.resolve()]
            new_text, n_stub, saved, n_kept, _n_red = partial_file(text, prefixes)
            total_arrays += n_stub
            total_saved += saved
            kept_arrays += n_kept
            globs, stats, _ = scan_declarations(text)
            kept_bytes += sum(s for n, s in globs + stats
                              if keep_symbol(n, prefixes))
            stub_rel.append(str(rel))
            if not args.dry_run:
                if write_if_changed(out_root / rel, new_text):
                    written += 1
                else:
                    unchanged += 1
            continue

        if f.resolve() in keep_abs:
            # Full size. Only the load calls are redirected, and only if the
            # TU has any — otherwise it compiles straight out of src/.
            globs, stats, _ = scan_declarations(text)
            kept_arrays += len(globs) + len(stats)
            kept_bytes += sum(s for _, s in globs) + sum(s for _, s in stats)

            # A kept TU's loads can live in an #included .c_inc instead of in
            # the .c. Redirect those too and write them into the stub tree;
            # dc/Makefile puts $(STUBDIR)/include at the front of INCLUDES so
            # the rewritten copy shadows the vendored one, the same mechanism
            # DC_SRC_SHRINK already uses for its two .c_inc files.
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
        if n_unmapped:
            print("  WARNING       : {} kept global(s) have no s_assets[] row"
                  " and will stay zeroed".format(n_unmapped))
        if not args.dry_run:
            print("  written       : {}  (unchanged {}, pruned {})".format(
                written, unchanged, pruned))

    return 0


if __name__ == "__main__":
    sys.exit(main())
