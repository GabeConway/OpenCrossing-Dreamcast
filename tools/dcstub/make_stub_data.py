#!/usr/bin/env python3
"""
make_stub_data.py — build the DC_ASSET_STUB source tree (kb/STATE.md, step S1).

WHY THIS EXISTS
---------------
The Dreamcast image is over 16 MB by 8,273,108 B and therefore never executes a
single instruction: KOS's startup .bss zeroing runs off the end of physical
memory before scif_init(), so there is not even console output to look at
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

The per-file `void _pc_load_src_..._c(void)` init functions the generator
appends are left alone: they are only ever reached from pc_assets_init(), which
the stub build does not call.

USAGE
-----
    python3 tools/dcstub/make_stub_data.py [--out DIR] [--dry-run] [--quiet]

Default --out is dc/build/stubsrc. The tree mirrors repo-relative paths, so
dc/Makefile can swap src/foo.c for dc/build/stubsrc/src/foo.c and its
$(OBJDIR)/%.c.o: $(ROOT)/%.c rule keeps working with no other change.

Only files that actually contain a rewritten declaration are written out; every
other TU still compiles from src/. Rewriting is idempotent and content-compared,
so an unchanged file is not re-touched and make does not rebuild it.
"""

import argparse
import os
import re
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
SRC = REPO / "src"
DEFAULT_OUT = REPO / "dc" / "build" / "stubsrc"

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


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--out", default=str(DEFAULT_OUT))
    ap.add_argument("--dry-run", action="store_true")
    ap.add_argument("--quiet", action="store_true")
    args = ap.parse_args()

    out_root = Path(args.out).resolve()

    files = sorted(SRC.rglob("*.c"))
    total_arrays = 0
    total_saved = 0
    written = 0
    unchanged = 0
    stub_rel = []

    for f in files:
        text = f.read_text(encoding="utf-8", errors="surrogateescape")
        if "#ifdef TARGET_PC" not in text:
            continue
        new_text, n, saved = stub_file(text)
        if n == 0:
            continue
        total_arrays += n
        total_saved += saved
        rel = f.relative_to(REPO)
        stub_rel.append(str(rel))
        if args.dry_run:
            continue
        dst = out_root / rel
        dst.parent.mkdir(parents=True, exist_ok=True)
        if dst.exists() and dst.read_text(
            encoding="utf-8", errors="surrogateescape"
        ) == new_text:
            unchanged += 1
            continue
        dst.write_text(new_text, encoding="utf-8", errors="surrogateescape")
        written += 1

    if not args.dry_run:
        # The list make reads. One repo-relative path per line, sorted, so the
        # Makefile can $(filter) against its own source list deterministically.
        out_root.mkdir(parents=True, exist_ok=True)
        (out_root / "stub.list").write_text("\n".join(stub_rel) + "\n")

    if not args.quiet:
        print("== DC_ASSET_STUB tree ==")
        print("  out           : {}".format(out_root))
        print("  files stubbed : {}".format(len(stub_rel)))
        print("  arrays stubbed: {}".format(total_arrays))
        print("  bytes removed : {:,}".format(total_saved))
        if not args.dry_run:
            print("  written       : {}  (unchanged {})".format(written, unchanged))

    return 0


if __name__ == "__main__":
    sys.exit(main())
