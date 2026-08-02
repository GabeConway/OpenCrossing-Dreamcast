#!/usr/bin/env python3
"""
make_stub_data.py — build the DC_ASSET_STUB source tree (kb/STATE.md, step S1).

WHY THIS EXISTS
---------------
The Dreamcast image is over 16 MB and therefore never executes a single
instruction: KOS's startup .bss zeroing runs off the end of physical memory
before scif_init(), so there is not even console output to look at
(kb/STATE.md, "Boot status"). Every RAM estimate in this project assumes a
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

USAGE
-----
    python3 tools/dcstub/make_stub_data.py [--out DIR] [--keep LIST]
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

# One row of pc/src/pc_assets.c's s_assets[]:
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


def parse_keep(cli_value):
    """Resolve the allowlist. CLI beats env; env unset means the default.

    Returns (list_of_repo_relative_paths, provenance_string).
    """
    if cli_value is not None:
        raw, why = cli_value, "--keep"
    elif "DC_STUB_KEEP" in os.environ:
        raw, why = os.environ["DC_STUB_KEEP"], "DC_STUB_KEEP"
    else:
        return list(DEFAULT_KEEP), "built-in default"

    parts = [p.strip() for p in re.split(r"[:,]", raw)]
    parts = [p for p in parts if p]
    if not parts:
        return [], why + " (empty — stub everything)"
    return parts, why


def emit_keep_inc(keep_paths, table):
    """Build the text of dc_stub_keep.inc for the resolved keep list."""
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

    for rel in keep_paths:
        f = REPO / rel
        text = f.read_text(encoding="utf-8", errors="surrogateescape")
        globs, stats, inits = scan_declarations(text)

        L.append("/* {} */".format(rel))
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
            n_rows += 1
            n_bytes += tsize

        for fn in inits:
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

    stats_line = (n_rows, n_inits, n_unmapped, n_bytes)
    return "\n".join(L), stats_line


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
             "Overrides $DC_STUB_KEEP. Pass '' to stub everything.",
    )
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    out_root = Path(args.out).resolve()

    keep_paths, keep_why = parse_keep(args.keep)

    missing = [p for p in keep_paths if not (REPO / p).is_file()]
    if missing:
        sys.stderr.write(
            "make_stub_data.py: keep list ({}) names files that do not "
            "exist:\n".format(keep_why)
        )
        for p in missing:
            sys.stderr.write("    {}\n".format(p))
        return 2

    keep_abs = {(REPO / p).resolve() for p in keep_paths}
    table = parse_asset_table(ASSET_TABLE)

    files = sorted(SRC.rglob("*.c"))
    total_arrays = 0
    total_saved = 0
    kept_arrays = 0
    kept_bytes = 0
    written = 0
    unchanged = 0
    stub_rel = []

    neutralised = 0

    for f in files:
        text = f.read_text(encoding="utf-8", errors="surrogateescape")
        if "#ifdef TARGET_PC" not in text:
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
            # Full size. Only the load calls are redirected, and only if the
            # TU has any — otherwise it compiles straight out of src/.
            globs, stats, _ = scan_declarations(text)
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

    inc_text, (n_rows, n_inits, n_unmapped, n_load_bytes) = emit_keep_inc(
        keep_paths, table
    )

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
        print("  {:<14}: {} table rows + {} per-file init fns"
              " ({:,} B)".format(KEEP_INC_NAME, n_rows, n_inits, n_load_bytes))
        if n_unmapped:
            print("  WARNING       : {} kept global(s) have no s_assets[] row"
                  " and will stay zeroed".format(n_unmapped))
        if not args.dry_run:
            print("  written       : {}  (unchanged {}, pruned {})".format(
                written, unchanged, pruned))

    return 0


if __name__ == "__main__":
    sys.exit(main())
