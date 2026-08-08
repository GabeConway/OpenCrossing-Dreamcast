#!/usr/bin/env python3
"""icache_map.py — how a hot symbol set lands in the SH-4's instruction cache.

WHY. The SH7750's instruction cache is 8 KB, DIRECT-MAPPED, 32-byte lines:
256 lines, and line index = bits [12:5] of the address. Two functions whose
addresses are congruent mod 8192 evict each other every time control passes
between them, no matter how much cache is free elsewhere. Our `.text` is
~2.88 MB and Flycast models no instruction cache at all, so nothing the
emulator has ever reported can see this (kb/RESUME.md §0h).

This is the HOST-SIDE half of that investigation, and it is free: no burn, no
emulator, seconds to run. It cannot measure stalls — only dc/src/dc_pmcr.c on
real hardware can do that — but it can do two things that matter before a burn
is spent:

  1. SIZE THE HOT SET. If the per-frame hot working set exceeds 8 KB, conflict
     misses are arithmetically guaranteed and the only question left is how
     expensive they are. If it fits, the aliasing hypothesis is dead and the
     hardware gap is somewhere else.
  2. NAME THE COLLIDING PAIRS. Line-index occupancy says which hot functions
     are fighting over the same 32 bytes of cache — which is exactly the input
     a --symbol-ordering-file or an ld order script needs.

⚠️ WHAT IT DOES NOT KNOW. It has no call profile: it treats every symbol
matched by --hot as equally hot and assumes each is entered once per frame.
A collision between two functions that never run in the same frame costs
nothing. Read the output as "candidates to order", never as "misses".

USAGE
    sh-elf-nm -S --defined-only build/AnimalCrossing.elf > nm.txt
    python3 tools/dcopt/icache_map.py nm.txt
    python3 tools/dcopt/icache_map.py nm.txt --hot '^_dl_G_' --hot '^_dc_gx_'
    python3 tools/dcopt/icache_map.py nm.txt --top 40 --size 8192 --line 32

No third-party modules.
"""

import argparse
import re
import sys
from collections import defaultdict

# The default hot set: the town frame as this project has measured it.
#   - emu64's dispatch loop and the two opcodes that are 73.7 % of the draw
#     (kb/research-sh4zam-gap.md §3)
#   - our own GX state machine and the PVR backend it feeds
#   - G3's cull, which now runs at TRIN entry on every batch
#   - the audio pump, which costs ~2.3 ms + ~265 us/voice every tick
# sh-elf prefixes C symbols with '_' (kb/RESUME.md session 4 item 10), so every
# pattern here starts with it.
DEFAULT_HOT = [
    r"^_emu64_taskstart",
    r"^_dl_G_",
    r"^_emu64",
    r"^_dc_gx_",
    r"^_GX[A-Z]",
    r"^_dc_pvr_",
    r"^_cu_trin",
    r"^_dc_emu64_cull",
    r"^_PSMTX",
    r"^_C_MTX",
    r"^_Nas_",
    r"^_pc_audio_",
]


def parse_nm(path):
    """Yield (addr, size, kind, name) for sized text symbols.

    `nm -S` prints `addr size type name`; symbols with no size print three
    fields. Only 't'/'T' are code — a data symbol shares no cache with them
    (the SH-4's caches are split), and counting one would inflate the hot set
    with something that cannot cause an instruction-cache conflict.
    """
    out = []
    with open(path) as fh:
        for line in fh:
            f = line.split()
            if len(f) != 4:
                continue
            addr, size, kind, name = f
            if kind not in ("t", "T"):
                continue
            try:
                out.append((int(addr, 16), int(size, 16), kind, name))
            except ValueError:
                continue
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("nm", help="output of sh-elf-nm -S --defined-only")
    ap.add_argument("--hot", action="append", default=None,
                    help="regex for a hot symbol (repeatable). Replaces the "
                         "built-in set.")
    ap.add_argument("--size", type=int, default=8192, help="icache bytes")
    ap.add_argument("--line", type=int, default=32, help="line bytes")
    ap.add_argument("--top", type=int, default=20,
                    help="how many worst-contended lines to print")
    a = ap.parse_args()

    nlines = a.size // a.line
    pats = [re.compile(p) for p in (a.hot if a.hot else DEFAULT_HOT)]

    syms = parse_nm(a.nm)
    if not syms:
        sys.exit("no sized text symbols in %s — was it `nm -S`?" % a.nm)

    text_bytes = sum(s for _, s, _, _ in syms)
    hot = [t for t in syms if any(p.search(t[3]) for p in pats)]
    hot_bytes = sum(s for _, s, _, _ in hot)

    print("== .text ==")
    print("  sized text symbols : %d" % len(syms))
    print("  sized text bytes   : %d (%.2f MB)" % (text_bytes,
                                                   text_bytes / 1048576.0))
    print("  icache             : %d B, direct-mapped, %d B lines, %d lines"
          % (a.size, a.line, nlines))
    print()
    print("== the hot set ==")
    print("  patterns           : %s" % " ".join(p.pattern for p in pats))
    print("  matched symbols    : %d" % len(hot))
    print("  hot bytes          : %d (%.1f KB)" % (hot_bytes,
                                                   hot_bytes / 1024.0))
    if a.size:
        print("  PRESSURE           : %.1fx the whole instruction cache"
              % (hot_bytes / float(a.size)))
    print()

    if hot_bytes <= a.size:
        print("  ⇒ the hot set FITS in the icache. Conflict misses are then a")
        print("    question of layout only, not of capacity.")
    else:
        print("  ⇒ the hot set does NOT fit. Capacity misses are guaranteed")
        print("    before any question of aliasing is asked: the set must")
        print("    shrink (less inlining, -Os on hot TUs) or be ordered so")
        print("    that what runs together lives together.")
    print()

    # Line-index occupancy. A symbol occupies every line its byte range covers,
    # modulo the cache size — which is what makes this direct-mapped rather
    # than a set count.
    occ = defaultdict(list)
    for addr, size, _, name in hot:
        first = addr // a.line
        last = (addr + max(size, 1) - 1) // a.line
        span = last - first + 1
        if span >= nlines:
            # A single symbol larger than the whole cache: it evicts itself.
            for i in range(nlines):
                occ[i].append(name)
            continue
        for i in range(span):
            occ[(first + i) % nlines].append(name)

    counts = [(len(occ[i]), i) for i in range(nlines)]
    counts.sort(reverse=True)
    empty = sum(1 for c, _ in counts if c == 0)

    print("== icache line occupancy over the hot set ==")
    print("  lines with no hot code : %d of %d" % (empty, nlines))
    print("  worst line             : %d distinct hot symbols" % counts[0][0])
    print()
    print("  the %d most contended lines (line: symbols sharing it):" % a.top)
    for c, i in counts[:a.top]:
        if c == 0:
            break
        names = sorted(set(occ[i]))
        shown = ", ".join(names[:6])
        more = "" if len(names) <= 6 else " (+%d more)" % (len(names) - 6)
        print("    %3d  x%-3d  %s%s" % (i, c, shown, more))

    print()
    print("⚠️ These are CANDIDATES TO ORDER, not measured misses. This tool has")
    print("   no call profile and no idea which of these run in the same")
    print("   frame. Only dc/src/dc_pmcr.c on real hardware prices a stall.")


if __name__ == "__main__":
    main()
