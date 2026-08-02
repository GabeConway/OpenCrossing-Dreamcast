#!/usr/bin/env python3
"""
measure_dedup.py — how much is `kb/levers.md` L6 (table dedup) actually worth?

L6 says: src/data is generator output, so hashing table contents and aliasing
the duplicates inside gen_runtime_assets.py is a *generator* change, legal under
the -O0 rule. It has never been measured. This answers it.

TWO POPULATIONS, MEASURED SEPARATELY — they land in different sections and are
fixed by different code:

  .data / .rodata   initialised tables that are IN the image. Measured from the
                    linked ELF, which is exact and catches every duplicate
                    regardless of which generator or hand-written file emitted
                    it. Aliasing duplicates shrinks the image directly.

  .bss              the asset destination arrays. They have no content in the
                    ELF, so their duplicates are found from pc_assets.c's
                    s_assets[] table instead: two entries reading the SAME
                    (rom_src, rom_off, size) are byte-identical by construction
                    and could share one destination. This is a lower bound —
                    two different ROM offsets holding identical bytes are not
                    counted, because that needs the ROM itself (see --rom).

WHAT "ALIASABLE" MEANS HERE, because it caps the saving
------------------------------------------------------
GNU aliases only work within one translation unit: you cannot alias a symbol
that another TU defines. So realising this saving means the generator emitting
the canonical copies into ONE generated TU and turning every duplicate into an
alias there. That is a real generator change, not a link-time trick — `--icf`
is unavailable on SH (kb/closed.md). The numbers here are what that change
would be worth, before the cost of making it.

Symbols smaller than --min-size are ignored: aliasing thousands of 4-byte
tables costs generator complexity and buys nothing.

USAGE
    # produce the inputs first (inside the SDK container):
    #   sh-elf-nm -S --defined-only -n ELF          > dc/build/dedup/syms.txt
    #   sh-elf-objcopy -O binary --only-section=.data   ELF dc/build/dedup/data.bin
    #   sh-elf-objcopy -O binary --only-section=.rodata ELF dc/build/dedup/rodata.bin
    python3 tools/dcstub/measure_dedup.py [--dir dc/build/dedup] [--min-size 16]
"""

import argparse
import collections
import hashlib
import re
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]

# `sh-elf-nm -S --defined-only -n` lines: "addr size type name". Symbols with no
# size are printed as "addr type name" and are skipped — a zero-size symbol is a
# label, not a table.
NM_RE = re.compile(r"^([0-9a-f]+)\s+([0-9a-f]+)\s+(\S)\s+(\S+)\s*$")

# {"path", dest, size, rom_off, rom_src, swap}
ASSET_RE = re.compile(
    r'^\s*\{\s*(?:"([^"]*)"|NULL)\s*,\s*([A-Za-z_]\w*)\s*,'
    r'\s*(0[xX][0-9A-Fa-f]+|\d+)\s*,\s*(0[xX][0-9A-Fa-f]+|\d+)\s*,'
    r'\s*(-?\d+)\s*,\s*(-?\d+)\s*\}'
)


def human(n):
    return "{:,}".format(n)


def load_sections(d):
    """Return {section: (base_addr, bytes)} for the two dumped sections."""
    out = {}
    for name, fn in ((".data", "data.bin"), (".rodata", "rodata.bin")):
        p = d / fn
        if p.exists():
            out[name] = p.read_bytes()
    return out


def section_bases(syms_path):
    """nm gives absolute addresses; objcopy gives section-relative bytes. Recover
    each base as the lowest address of any symbol in that section — exact enough
    because both sections start with a symbol at offset 0 in this image, and the
    result is validated against the dump length below."""
    return None  # filled by the caller from readelf-free heuristics


def measure_image(d, min_size):
    syms = []
    for line in (d / "syms.txt").read_text().splitlines():
        m = NM_RE.match(line)
        if not m:
            continue
        addr, size, typ, name = m.groups()
        syms.append((int(addr, 16), int(size, 16), typ, name))

    sections = load_sections(d)
    results = {}

    for sec, letters in ((".data", "Dd"), (".rodata", "Rr")):
        blob = sections.get(sec)
        if blob is None:
            continue
        in_sec = [s for s in syms if s[2] in letters and s[1] >= min_size]
        if not in_sec:
            continue
        # Section base: nm addresses are absolute, the objcopy dump is not.
        base = min(s[0] for s in in_sec)
        # Sanity: nothing may read past the dump.
        hi = max(s[0] + s[1] for s in in_sec)
        if hi - base > len(blob):
            base = hi - len(blob)

        groups = collections.defaultdict(list)
        total = 0
        for addr, size, typ, name in in_sec:
            off = addr - base
            if off < 0 or off + size > len(blob):
                continue
            h = hashlib.blake2b(blob[off:off + size], digest_size=16).digest()
            groups[(size, h)].append(name)
            total += size

        # Two very different things look alike in this data and must not be
        # added together:
        #   redefined  the SAME symbol name, defined more than once, surviving
        #              because the link carries -Wl,--allow-multiple-definition
        #              (decomp headers declare functions bare `inline`, so in
        #              gnu89 every TU may emit its own copy). Only one copy is
        #              ever reachable. This is dead weight, and removing it is
        #              not aliasing — it is deleting a redundant definition.
        #   aliasable  DIFFERENT symbol names holding identical bytes. Both are
        #              reachable, so this needs one object under two names,
        #              i.e. the generator change L6 describes.
        redefined = aliasable = 0
        redefined_n = aliasable_n = 0
        biggest = []
        for (size, _h), names in groups.items():
            if len(names) < 2:
                continue
            counts = collections.Counter(names)
            rep = sum(v - 1 for v in counts.values())
            redefined += rep * size
            redefined_n += rep
            if len(counts) > 1:
                aliasable += (len(counts) - 1) * size
                aliasable_n += len(counts) - 1
            biggest.append((size * (len(names) - 1), size, len(names),
                            sorted(names)[0]))
        biggest.sort(reverse=True)
        results[sec] = {
            "symbols": len(in_sec),
            "bytes": total,
            "dup_bytes": redefined + aliasable,
            "dup_symbols": redefined_n + aliasable_n,
            "redefined": redefined,
            "redefined_n": redefined_n,
            "aliasable": aliasable,
            "aliasable_n": aliasable_n,
            "top": biggest[:12],
        }
    return results


# pc_assets.c:13 — enum { SRC_REL = 0, SRC_DOL = 1, SRC_NONE = 2 };
SRC_REL, SRC_DOL, SRC_NONE = 0, 1, 2

# The lazy per-TU loads the generator writes into src/ itself, plus the ones in
# pc_assets.c's per-file init functions:
#   pc_load_asset("assets/foo.bin", foo, 0x200, 0x355740, 0, 1);
CALL_RE = re.compile(
    r'pc_load_asset\(\s*(?:"([^"]*)"|NULL)\s*,\s*([A-Za-z_]\w*)\s*,'
    r'\s*(0[xX][0-9A-Fa-f]+|\d+)\s*,\s*(0[xX][0-9A-Fa-f]+|\d+)\s*,'
    r'\s*(-?\d+)\s*,\s*(-?\d+)\s*\)'
)


def collect_assets():
    """Every asset load in the build: the central s_assets[] table, plus every
    pc_load_asset() call site (per-file init functions and the lazy function-
    local loads the generator writes into src/). Returns (path, dest, size,
    rom_off, rom_src) tuples, deduplicated by destination symbol — a symbol
    loaded from two places is still one array."""
    entries = {}
    files = [REPO / "pc" / "src" / "pc_assets.c"]
    files += sorted((REPO / "src").rglob("*.c"))
    files += sorted((REPO / "src").rglob("*.cpp"))
    for f in files:
        text = f.read_text(errors="surrogateescape")
        if "pc_load_asset" not in text and "s_assets" not in text:
            continue
        for line in text.splitlines():
            m = ASSET_RE.match(line) or CALL_RE.search(line)
            if not m:
                continue
            path, dest, size, rom_off, rom_src, _swap = m.groups()
            entries.setdefault(
                dest, (path, dest, int(size, 0), int(rom_off, 0), int(rom_src)))
    return list(entries.values())


def measure_bss(min_size, rom=None):
    """Duplicate asset destination arrays.

    Without --rom the key is (rom_src, rom_off, size): two entries reading the
    same ROM range are identical by construction. That is a LOWER BOUND — it
    cannot see two different offsets holding identical bytes, which is exactly
    what L6 is about. With --rom the key is a hash of the actual bytes, which
    is the real answer."""
    entries = collect_assets()

    groups = collections.defaultdict(list)
    total = 0
    unsourced = 0
    for path, dest, size, rom_off, rom_src in entries:
        total += size
        if rom_src == SRC_NONE:
            # No ROM provenance: content unknown without the .bin. Never deduped.
            unsourced += 1
            continue
        if size < min_size:
            continue
        if rom:
            blob = rom[SRC_REL] if rom_src == SRC_REL else rom[SRC_DOL]
            if rom_off + size > len(blob):
                continue
            key = (size, hashlib.blake2b(blob[rom_off:rom_off + size],
                                         digest_size=16).digest())
        else:
            key = (rom_src, rom_off, size)
        groups[key].append(dest)

    dup_bytes = 0
    dup_syms = 0
    biggest = []
    for key, names in groups.items():
        size = key[-1] if len(key) == 3 else key[0]
        if len(names) > 1:
            dup_bytes += size * (len(names) - 1)
            dup_syms += len(names) - 1
            biggest.append((size * (len(names) - 1), size, len(names),
                            sorted(names)[0]))
    biggest.sort(reverse=True)
    return {
        "entries": len(entries),
        "bytes": total,
        "unsourced": unsourced,
        "dup_bytes": dup_bytes,
        "dup_symbols": dup_syms,
        "top": biggest[:12],
    }


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--dir", default=str(REPO / "dc" / "build" / "dedup"))
    ap.add_argument("--min-size", type=int, default=16)
    ap.add_argument("--rom", metavar="DISCROOT",
                    help="a `dcasset extract` output dir. With it, .bss "
                         "destinations are deduped by ACTUAL ROM BYTES rather "
                         "than by (source, offset) — this is the L6 answer.")
    args = ap.parse_args()
    d = Path(args.dir)

    rom = None
    if args.rom:
        r = Path(args.rom)
        rom = {SRC_DOL: (r / "sys" / "main.dol").read_bytes(),
               SRC_REL: (r / "files" / "foresta.rel").read_bytes()}
        print("rom: main.dol {} B, foresta.rel {} B\n".format(
            human(len(rom[SRC_DOL])), human(len(rom[SRC_REL]))))

    print("== L6: duplicate table measurement (min-size {} B) ==\n"
          .format(args.min_size))

    if (d / "syms.txt").exists():
        for sec, r in measure_image(d, args.min_size).items():
            pct = 100.0 * r["dup_bytes"] / r["bytes"] if r["bytes"] else 0.0
            print("{:<9} {:>7} symbols  {:>12} B".format(
                sec, r["symbols"], human(r["bytes"])))
            print("          duplicates: {:>7} symbols  {:>12} B  ({:.1f}%)"
                  .format(r["dup_symbols"], human(r["dup_bytes"]), pct))
            print("            of which redefined (same name, one reachable): "
                  "{:>5} syms {:>10} B".format(
                      r["redefined_n"], human(r["redefined"])))
            print("            of which aliasable (distinct names, same bytes):"
                  " {:>4} syms {:>10} B".format(
                      r["aliasable_n"], human(r["aliasable"])))
            for saved, size, n, name in r["top"][:6]:
                print("            {:>10} B  {:>2} x {:>8} B  {}".format(
                    human(saved), n, human(size), name))
            print()
    else:
        print("(no {} — skipping the image measurement)\n".format(d / "syms.txt"))

    b = measure_bss(args.min_size, rom)
    pct = 100.0 * b["dup_bytes"] / b["bytes"] if b["bytes"] else 0.0
    print(".bss dest {:>7} arrays   {:>12} B  ({} with no ROM provenance)  "
          "keyed by {}".format(
              b["entries"], human(b["bytes"]), b["unsourced"],
              "CONTENT" if rom else "(source, offset)"))
    print("          duplicates: {:>7} symbols  {:>12} B  ({:.1f}%)"
          .format(b["dup_symbols"], human(b["dup_bytes"]), pct))
    for saved, size, n, name in b["top"][:6]:
        print("            {:>10} B  {:>2} x {:>8} B  {}".format(
            human(saved), n, human(size), name))


if __name__ == "__main__":
    main()
