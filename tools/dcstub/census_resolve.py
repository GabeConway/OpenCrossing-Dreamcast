#!/usr/bin/env python3
"""
census_resolve.py — turn a DC_ASSET_CENSUS console log into a working-set table.

WHY
---
kb/STATE.md N1 wants the title demo's real working set, and it cannot be read
out of the source: the title demo names its acres through BLOCK_COMBI_* indices
into l_combiID[] and its animals through profile IDs, so a static trace stops
at the ten logo TUs (8,824 B) and sees no acre and no NPC.

dc/src/dc_asset_census.c makes the runtime answer the question instead. Every
asset the renderer consumes arrives at the GX layer as an address — a texture
image at GXLoadTexObj, a vertex/normal/colour array base at GXSetArray — and
those addresses are link-time constants. This script resolves them against the
ELF symbol table and reports, per symbol, what was touched and how big it is.

That total is the number S4 has to size its asset pool against, and splitting
it by kind is the measurement kb/research-creative-ram.md T1 needs before
deciding whether textures belong in the pool at all.

WHAT IT IS NOT: a bytes-actually-read figure. A symbol counts in full if the
scene bound or indexed it even once. For pool sizing that is the right bias —
the pool must hold whole assets — but do not read it as traffic.

STUB CAVEAT, AND WHY THE ANSWER IS STILL RIGHT
----------------------------------------------
Under DC_ASSET_STUB every destination array is [1]-sized, so the ELF's own
st_size for a stubbed symbol is ~1 element, not the real asset. The identity of
what the scene touched is unaffected — same symbol either way — but the sizes
are not, so by default this script takes the real sizes from a NON-stub ELF or
from the s_assets[] table and prints both columns. Pass --sizes-from with a
full-size ELF to get the number that matters; without it, the size column is
labelled "stub" and must not be quoted as a working-set total.

USAGE
-----
    # nm straight out of the SDK image (the default path)
    python3 tools/dcstub/census_resolve.py CONSOLE.log

    # explicit inputs, no docker
    python3 tools/dcstub/census_resolve.py CONSOLE.log --nm nm.txt
    python3 tools/dcstub/census_resolve.py CONSOLE.log \
        --elf dc/build/AnimalCrossing.elf --sizes-from dc/build/full.elf

Exit status is 0 when at least one address resolved, 2 when the log contains no
CENSUS records (a run that never got far enough is not a successful census).
"""

import argparse
import bisect
import collections
import os
import re
import subprocess
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
SDK_IMAGE = os.environ.get("DC_SDK_IMAGE", "opencrossing-dc:sdk")

CENSUS_RE = re.compile(r"^CENSUS ([TV]) ([0-9a-fA-F]{8})\s*$")
SUM_RE = re.compile(
    r"^CENSUS SUM unique=(\d+) printed=(\d+) seen=(\d+) overflow=(\d+)")
# `sh-elf-nm -S` prints:  addr size type name   (size absent for sizeless syms)
NM_RE = re.compile(
    r"^([0-9a-fA-F]+)\s+(?:([0-9a-fA-F]+)\s+)?([A-Za-z])\s+(\S+)$")

KIND_NAME = {"T": "texture", "V": "vertex-array"}


def run_nm(elf):
    """sh-elf-nm out of the SDK image. The host has no sh-elf toolchain; the
    container is where it lives, and dc/build-dc.sh already mounts the repo at
    /work, so the same convention is reused verbatim here."""
    rel = os.path.relpath(os.path.abspath(elf), REPO)
    if rel.startswith(".."):
        sys.exit("--elf must live inside the repo so the container can see it: "
                 + elf)
    cmd = ["docker", "run", "--rm", "--platform", "linux/arm64",
           "-v", REPO + ":/work", SDK_IMAGE,
           # bash -c, never bash -lc: kb/traps.md.
           "bash", "-c", "sh-elf-nm -S -n /work/" + rel]
    out = subprocess.run(cmd, capture_output=True, text=True)
    if out.returncode != 0:
        sys.exit("sh-elf-nm failed:\n" + out.stderr.strip())
    return out.stdout


def parse_nm(text):
    """-> (sorted addr list, [(addr, size, type, name)]) for bisect lookup."""
    syms = []
    for line in text.splitlines():
        m = NM_RE.match(line.strip())
        if not m:
            continue
        addr = int(m.group(1), 16)
        size = int(m.group(2), 16) if m.group(2) else 0
        syms.append((addr, size, m.group(3), m.group(4)))
    syms.sort(key=lambda s: s[0])
    return [s[0] for s in syms], syms


def resolve(addr, addrs, syms):
    """Nearest symbol at or below addr. Returns (name, size, offset) or None.

    Interior addresses matter: GXSetArray is often handed a pointer partway
    into a table, so an exact-match lookup would drop real hits on the floor.
    A hit beyond the symbol's recorded size is reported with its offset rather
    than discarded, because a stub build's sizes are deliberately wrong."""
    i = bisect.bisect_right(addrs, addr) - 1
    if i < 0:
        return None
    a, size, _typ, name = syms[i]
    return name, size, addr - a


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("console", help="smoke.sh run console.log")
    ap.add_argument("--elf", default="dc/build/AnimalCrossing.elf",
                    help="ELF the console came from (default: the last build)")
    ap.add_argument("--nm", help="pre-captured `sh-elf-nm -S -n` output")
    ap.add_argument("--sizes-from",
                    help="a NON-stub ELF to take real symbol sizes from")
    ap.add_argument("--top", type=int, default=40,
                    help="how many symbols to list (0 = all)")
    args = ap.parse_args()

    with open(args.console, errors="replace") as fh:
        lines = fh.read().splitlines()

    hits, summary = [], None
    for line in lines:
        m = CENSUS_RE.match(line.strip())
        if m:
            hits.append((m.group(1), int(m.group(2), 16)))
            continue
        m = SUM_RE.match(line.strip())
        if m:
            summary = tuple(int(g) for g in m.groups())

    if not hits:
        print("no CENSUS records in " + args.console, file=sys.stderr)
        print("  (was the image built with DC_ASSET_CENSUS=1?)", file=sys.stderr)
        return 2

    nm_text = open(args.nm).read() if args.nm else run_nm(args.elf)
    addrs, syms = parse_nm(nm_text)

    real = {}
    if args.sizes_from:
        _, full = parse_nm(run_nm(args.sizes_from))
        real = {s[3]: s[1] for s in full}

    per_sym = {}
    unresolved = 0
    for kind, addr in hits:
        r = resolve(addr, addrs, syms)
        if r is None:
            unresolved += 1
            continue
        name, size, _off = r
        cur = per_sym.setdefault(name, {"kinds": set(), "stub": size, "n": 0})
        cur["kinds"].add(kind)
        cur["n"] += 1

    size_of = (lambda n, s: real.get(n, s["stub"])) if real else \
              (lambda n, s: s["stub"])
    ranked = sorted(per_sym.items(), key=lambda kv: -size_of(*kv))

    label = "real" if real else "stub"
    print("working set: %d distinct addresses -> %d symbols (%d unresolved)"
          % (len(hits), len(per_sym), unresolved))
    if summary:
        print("guest counters: unique=%d printed=%d seen=%d overflow=%d"
              % summary)
        if summary[0] != summary[1]:
            print("  WARNING: the run ended before %d of %d entries were "
                  "printed; the list below is short."
                  % (summary[0] - summary[1], summary[0]))
        if summary[3]:
            print("  WARNING: %d addresses overflowed the guest table and were "
                  "never recorded." % summary[3])

    by_kind = collections.Counter()
    for name, s in per_sym.items():
        for k in s["kinds"]:
            by_kind[k] += size_of(name, s) / len(s["kinds"])
    total = sum(size_of(n, s) for n, s in per_sym.items())
    print("\ntotal (%s sizes): %d B" % (label, total))
    for k, v in sorted(by_kind.items()):
        print("  %-13s %10d B" % (KIND_NAME.get(k, k), int(v)))
    if not real:
        print("  NOTE: stub sizes. Re-run with --sizes-from <full-size ELF> "
              "before quoting this as a pool size.")

    print("\n%-44s %10s %6s %5s" % ("symbol", "size", "kind", "binds"))
    for name, s in (ranked if args.top == 0 else ranked[:args.top]):
        print("%-44s %10d %6s %5d"
              % (name[:44], size_of(name, s), "".join(sorted(s["kinds"])),
                 s["n"]))
    if args.top and len(ranked) > args.top:
        print("... %d more" % (len(ranked) - args.top))
    return 0


if __name__ == "__main__":
    sys.exit(main())
