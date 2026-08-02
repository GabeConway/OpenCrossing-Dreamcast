#!/usr/bin/env python3
"""
census_resolve.py — turn a DC_ASSET_CENSUS console log into a working-set table.

WHY
---
kb/STATE.md N1 wants the title demo's real working set, and it cannot be read
out of the source: the title demo names its acres through BLOCK_COMBI_* indices
into l_combiID[] and its animals through profile IDs, so a static trace stops
at the ten logo TUs (8,824 B) and sees no acre and no NPC.

dc/src/dc_asset_census.c makes the runtime answer the question instead. It
records two shapes, and they are resolved two different ways:

  POINTS   CENSUS T/P/V <addr>          a texture image (GXLoadTexObj), a
                                        palette (GXLoadTlut), or an indexed
                                        vertex array (GXSetArray — always
                                        empty; this game has no indexed draws).
                                        Each names a symbol's BASE, so nearest-
                                        symbol-at-or-below resolves it.

  BATCHES  CENSUS B M <base> <hi>       one gsSPVertex run of contiguous
                                        vertices, from the OSs16tof32 shim in
                                        dc/include/dc_census_vtx.h. `base` is
                                        usually an INTERIOR pointer —
                                        `gsSPVertex(&obj_train1_1_v[93], …)` —
                                        so nearest-symbol is wrong on a stub
                                        image and the source-side join below is
                                        used instead.

That total is the number S4 has to size its asset pool against, and splitting
it by kind is the measurement kb/research-creative-ram.md T1 needs before
deciding whether textures belong in the pool at all.

THE INTERIOR-POINTER PROBLEM, AND THE gsSPVertex JOIN
-----------------------------------------------------
Under DC_ASSET_STUB every destination array is one element long, but the
display lists that reference them are initialised Gfx[] data and are NOT
stubbed — so a batch's byte length is real while the array it points into is
16 bytes long. `&obj_train1_1_v[93]` therefore lands ~90 symbols past the array
it belongs to, and nearest-symbol-at-or-below names the wrong one.

The fix is a join against the source. Every gsSPVertex in src/ is a literal
`(&NAME[IDX], N, V0)` or `(NAME, N, V0)`, so the set of (symbol, byte offset,
vertex count) triples the game can possibly ask for is enumerable offline.
A batch resolves when `nm[symbol] + byte_offset == base` exactly — an equality
on a 32-bit address, which is very hard to satisfy by accident, and is filtered
further by requiring the source's N to fit inside the batch.

Batches that do not resolve are usually segment-relative
(`gsSPVertex(anime_1_txt + 0x130, …)`), i.e. animation data reached through
emu64's segment table at a runtime address that no static symbol describes.
They are reported separately rather than guessed at.

WHAT THIS IS NOT: for points, a bytes-actually-read figure. A symbol counts in
full if the scene bound it even once. For pool sizing that is the right bias —
the pool must hold whole assets — but do not read it as traffic. For batches
the opposite holds: `hi - base` IS bytes read, and the whole-symbol column next
to it is what a whole-asset pool would have to hold instead.

STUB CAVEAT, AND WHY THE ANSWER IS STILL RIGHT
----------------------------------------------
Under DC_ASSET_STUB the ELF's own st_size for a stubbed symbol is ~1 element,
not the real asset. Identity is unaffected — same symbol either way — but the
sizes are not, so by default this script takes real sizes from a NON-stub ELF
and prints which column it used. Pass --sizes-from with a full-size ELF to get
the number that matters; without it, the size column is labelled "stub" and
must not be quoted as a working-set total.

USAGE
-----
    # nm straight out of the SDK image (the default path)
    python3 tools/dcstub/census_resolve.py CONSOLE.log

    # the real numbers: stub ELF for addresses, full ELF for sizes
    python3 tools/dcstub/census_resolve.py CONSOLE.log \
        --elf dc/build/AnimalCrossing.elf \
        --sizes-from dc/build/nonstub/AnimalCrossing.elf

    # explicit inputs, no docker
    python3 tools/dcstub/census_resolve.py CONSOLE.log --nm nm.txt

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

CENSUS_RE = re.compile(r"^CENSUS ([TPV]) ([0-9a-fA-F]{8})\s*$")
BATCH_RE = re.compile(
    r"^CENSUS B ([A-Z]) ([0-9a-fA-F]{8}) ([0-9a-fA-F]{8})\s*$")
SUM_RE = re.compile(
    r"^CENSUS SUM unique=(\d+) printed=(\d+) seen=(\d+) overflow=(\d+)")
BSUM_RE = re.compile(
    r"^CENSUS BSUM batches=(\d+) notes=(\d+) full=(\d+)")
# `sh-elf-nm -S` prints:  addr size type name   (size absent for sizeless syms)
NM_RE = re.compile(
    r"^([0-9a-fA-F]+)\s+(?:([0-9a-fA-F]+)\s+)?([A-Za-z])\s+(\S+)$")

# gsSPVertex(&name[idx], n, v0) / gsSPVertex(name, n, v0).
# Deliberately does NOT match `name + 0x130` — those are N64 segment bases
# resolved through emu64's segment table, not static symbols.
GSSPVERTEX_RE = re.compile(
    r"gsSPVertex\(\s*(&?)\s*([A-Za-z_]\w*)\s*(?:\[\s*(0[xX][0-9a-fA-F]+|\d+)"
    r"\s*\])?\s*,\s*(\d+)\s*,")

SIZEOF_VTX = 16

KIND_NAME = {"T": "texture", "V": "vertex-array", "P": "palette",
             "M": "model-vertices"}

# Symbols the census legitimately records that are NOT assets and must not be
# counted into a pool size. emu64's texture_buffer_data is its own conversion
# scratch (49,152 B, and it dominates the table if left in); black_texture is
# the 32 B placeholder emu64_init() binds to every unused TEV stage.
NON_ASSET = ("texture_buffer_data", "black_texture")


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

    Correct for POINTS only — every point the census records is a symbol base
    or an offset small enough not to escape its own symbol. Batches use
    join_batches() instead; see the module docstring."""
    i = bisect.bisect_right(addrs, addr) - 1
    if i < 0:
        return None
    a, size, _typ, name = syms[i]
    return name, size, addr - a


def scan_gsspvertex(root):
    """Every literal gsSPVertex in the tree -> {byte_offset_from_symbol: ...}.

    Returns {name: [(byte_off, nvtx), ...]}. ~12,000 sites across src/, which
    is a couple of seconds of regex — cheap next to a 2-minute build, and it
    has to be re-read anyway whenever the data tree changes."""
    table = collections.defaultdict(set)
    for dirpath, dirnames, filenames in os.walk(os.path.join(root, "src")):
        dirnames[:] = [d for d in dirnames if not d.startswith(".")]
        for fn in filenames:
            if not fn.endswith((".c", ".cpp", ".h", ".inc")):
                continue
            path = os.path.join(dirpath, fn)
            try:
                with open(path, errors="replace") as fh:
                    text = fh.read()
            except OSError:
                continue
            if "gsSPVertex" not in text:
                continue
            for m in GSSPVERTEX_RE.finditer(text):
                amp, name, idx, n = m.groups()
                off = int(idx, 0) * SIZEOF_VTX if idx else 0
                if idx and not amp:
                    continue        # name[idx] without & is not an address
                table[name].add((off, int(n)))
    return table


def join_batches(batches, syms, vtx_sites):
    """Resolve each (base, hi) batch to a source symbol by exact address match.

    A batch is `nm[name] + byte_off == base` for some gsSPVertex site. The
    equality is on a full 32-bit address, so a false positive needs a symbol
    whose base is displaced by exactly the right offset — rare in a full image,
    COMMON in a stub one, where every asset array is 16 bytes long and 2,305
    symbols are packed into the space one real model used to occupy. Two
    tie-breaks handle it, in this order:

      1. exact vertex count. A batch that is one gsSPVertex has nvtx == n;
         only a batch that swallowed an adjacent command has nvtx > n. Prefer
         the exact match and only fall back to the `n <= nvtx` set.
      2. corroboration. A model is drawn as a run of batches, so a name that
         some OTHER batch resolved to on its own is far more likely than a name
         that appears nowhere else. Applied to fixpoint, because each
         resolution can unlock the next.

    TIE-BREAK 2 IS A GUESS AND IS REPORTED AS ONE. It is systematically biased
    towards whichever candidate happened to get a clean hit first, and the
    animal models are exactly where that bites: every `xxx_1_v` is 16 bytes
    apart in a stub image and every species shares its rig, so `mnk_1_v + 0x5E0`
    and `mob_1_v + 0x5D0` are the same address with the same vertex count.
    Batches resolved this way come back flagged `inferred` and the caller keeps
    them in a separate column — do not fold them into a certain total.

    Returns (certain, inferred, ambiguous, unresolved); the first two are
    {name: [(base, hi), ...]}."""
    # sh-elf prefixes C symbols with '_', so the source name `obj_train1_1_v`
    # is `_obj_train1_1_v` in nm. Index both spellings rather than guessing.
    sym_addr = {}
    for addr, _size, _typ, name in syms:
        sym_addr.setdefault(name, addr)     # first (lowest) wins; nm -n is sorted
        if name.startswith("_"):
            sym_addr.setdefault(name[1:], addr)

    pending, unresolved = [], []
    for base, hi in batches:
        nvtx = (hi - base) // SIZEOF_VTX
        loose, exact = set(), set()
        for name, sites in vtx_sites.items():
            a = sym_addr.get(name)
            if a is None or a > base:
                continue
            off = base - a
            for site_off, site_n in sites:
                if site_off != off or site_n > nvtx:
                    continue
                loose.add(name)
                if site_n == nvtx:
                    exact.add(name)
        cands = exact if len(exact) == 1 else (exact or loose)
        if cands:
            pending.append((base, hi, cands))
        else:
            unresolved.append((base, hi))

    certain = collections.defaultdict(list)
    inferred = collections.defaultdict(list)
    confirmed = {n for _b, _h, c in pending if len(c) == 1 for n in c}
    guessed = set()
    changed = True
    while changed:
        changed = False
        for i, (base, hi, cands) in enumerate(pending):
            if len(cands) <= 1:
                continue
            narrowed = cands & confirmed
            if len(narrowed) == 1:
                pending[i] = (base, hi, narrowed)
                guessed.add(base)
                changed = True

    ambiguous = []
    for base, hi, cands in pending:
        if len(cands) != 1:
            ambiguous.append((base, hi, sorted(cands)))
        elif base in guessed:
            inferred[next(iter(cands))].append((base, hi))
        else:
            certain[next(iter(cands))].append((base, hi))
    return certain, inferred, ambiguous, unresolved


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("console", help="smoke.sh run console.log")
    ap.add_argument("--elf", default="dc/build/AnimalCrossing.elf",
                    help="ELF the console came from (default: the last build)")
    ap.add_argument("--nm", help="pre-captured `sh-elf-nm -S -n` output")
    ap.add_argument("--sizes-from",
                    help="a NON-stub ELF to take real symbol sizes from")
    ap.add_argument("--sizes-nm", help="pre-captured nm of the NON-stub ELF")
    ap.add_argument("--no-vtx-join", action="store_true",
                    help="skip the gsSPVertex source scan; batches then only "
                         "get a byte total, no symbol names")
    ap.add_argument("--top", type=int, default=40,
                    help="how many symbols to list per table (0 = all)")
    args = ap.parse_args()

    with open(args.console, errors="replace") as fh:
        lines = fh.read().splitlines()

    hits, summary, bsummary = [], None, None
    batch_hi = {}
    for line in lines:
        line = line.strip()
        m = CENSUS_RE.match(line)
        if m:
            hits.append((m.group(1), int(m.group(2), 16)))
            continue
        m = BATCH_RE.match(line)
        if m:
            # A batch grows and is re-emitted; the longest line for a base wins.
            base, hi = int(m.group(2), 16), int(m.group(3), 16)
            if hi > batch_hi.get(base, 0):
                batch_hi[base] = hi
            continue
        m = SUM_RE.match(line)
        if m:
            summary = tuple(int(g) for g in m.groups())
            continue
        m = BSUM_RE.match(line)
        if m:
            bsummary = tuple(int(g) for g in m.groups())

    if not hits and not batch_hi:
        print("no CENSUS records in " + args.console, file=sys.stderr)
        print("  (was the image built with DC_ASSET_CENSUS=1?)", file=sys.stderr)
        return 2

    nm_text = open(args.nm).read() if args.nm else run_nm(args.elf)
    addrs, syms = parse_nm(nm_text)

    real = {}
    if args.sizes_nm:
        _, full = parse_nm(open(args.sizes_nm).read())
        real = {s[3]: s[1] for s in full}
    elif args.sizes_from:
        _, full = parse_nm(run_nm(args.sizes_from))
        real = {s[3]: s[1] for s in full}

    # ---- points ------------------------------------------------------------
    per_sym = {}
    unresolved_pts = 0
    for kind, addr in hits:
        r = resolve(addr, addrs, syms)
        if r is None:
            unresolved_pts += 1
            continue
        name, size, _off = r
        cur = per_sym.setdefault(name, {"kinds": set(), "stub": size, "n": 0})
        cur["kinds"].add(kind)
        cur["n"] += 1

    size_of = (lambda n, s: real.get(n, s["stub"])) if real else \
              (lambda n, s: s["stub"])
    ranked = sorted(per_sym.items(), key=lambda kv: -size_of(*kv))
    label = "real" if real else "stub"

    print("points: %d addresses -> %d symbols (%d unresolved)"
          % (len(hits), len(per_sym), unresolved_pts))
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
    point_total = sum(size_of(n, s) for n, s in per_sym.items())
    scratch = sum(size_of(n, s) for n, s in per_sym.items()
                  if any(k in n for k in NON_ASSET))
    print("\npoint total (%s whole-symbol sizes): %d B" % (label, point_total))
    for k, v in sorted(by_kind.items()):
        print("  %-15s %10d B" % (KIND_NAME.get(k, k), int(v)))
    if scratch:
        print("  %-15s %10d B  (emu64 scratch, NOT an asset — subtract it)"
              % ("of which", scratch))
        print("  %-15s %10d B" % ("asset points", point_total - scratch))
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

    # ---- vertex batches ----------------------------------------------------
    if not batch_hi:
        print("\nno CENSUS B batches in the log — either the scene drew no "
              "geometry or the dc_census_vtx.h shim is not in this image.")
        return 0 if per_sym else 2

    batches = sorted(batch_hi.items())
    read_bytes = sum(hi - base for base, hi in batches)
    print("\nvertex batches: %d distinct gsSPVertex bases" % len(batches))
    if bsummary:
        print("guest counters: batches=%d notes=%d full=%d" % bsummary)
        if bsummary[2]:
            print("  WARNING: %d batches overflowed the guest table."
                  % bsummary[2])
        if bsummary[0] != len(batches):
            print("  WARNING: the guest holds %d batches but only %d reached "
                  "the log; the run ended mid-report." % (bsummary[0],
                                                          len(batches)))
    print("vertex bytes READ: %d B" % read_bytes)
    print("  (exact: a batch's length comes from the display list, which is "
          "real even in a stub image)")

    if args.no_vtx_join:
        return 0

    vtx_sites = scan_gsspvertex(REPO)
    certain, inferred, amb, unres = join_batches(batches, syms, vtx_sites)

    # nm spells C symbols with a leading underscore; the join is keyed on the
    # source spelling. Look both up rather than normalising one of them away.
    def real_size(n):
        return real.get(n, real.get("_" + n, 0))

    merged = collections.defaultdict(list)
    for src in (certain, inferred):
        for n, bl in src.items():
            merged[n].extend(bl)
    v_ranked = sorted(merged.items(),
                      key=lambda kv: -real_size(kv[0]) if real else
                      -sum(h - b for b, h in kv[1]))

    print("\nvertex symbols: %d certain, %d only inferred, %d batches "
          "ambiguous, %d unresolved"
          % (len(certain), len([n for n in merged if n not in certain]),
             len(amb), len(unres)))
    if unres:
        print("  (unresolved is usually segment-relative animation data — "
              "`gsSPVertex(anime_1_txt + 0x130, …)` has no static symbol)")
    if real:
        lo = sum(real_size(n) for n in certain)
        hi_extra = sum(real_size(n) for n in merged if n not in certain)
        hi_extra += sum(max(real_size(n) for n in names)
                        for _b, _h, names in amb)
        print("vertex whole-symbol total: %d B certain, up to %d B if every "
              "inferred and ambiguous attribution names a further symbol"
              % (lo, lo + hi_extra))
        print("  (the certain figure is a LOWER bound on what a whole-asset "
              "pool must hold; %d B is what was actually read)" % read_bytes)
    print("\n%-40s %10s %6s %8s %s"
          % ("symbol", "size", "batch", "read", "how"))
    for name, bl in (v_ranked if args.top == 0 else v_ranked[:args.top]):
        how = "certain" if name in certain else "inferred"
        if name in certain and name in inferred:
            how = "certain+inferred"
        print("%-40s %10d %6d %8d %s"
              % (name[:40], real_size(name), len(bl),
                 sum(h - b for b, h in bl), how))
    if args.top and len(v_ranked) > args.top:
        print("... %d more" % (len(v_ranked) - args.top))

    if amb:
        print("\nambiguous batches (address matched >1 gsSPVertex site):")
        for base, hi, names in amb[:10]:
            print("  %08x..%08x  %s" % (base, hi, ", ".join(names[:4])))
    if unres:
        u_bytes = sum(h - b for b, h in unres)
        print("\nunresolved batches: %d, %d B read. Nearest symbol at or below "
              "(WRONG on a stub image for an interior pointer — shown only to "
              "locate them):" % (len(unres), u_bytes))
        for base, hi in unres[:10]:
            r = resolve(base, addrs, syms)
            print("  %08x..%08x  near %s" % (base, hi, r[0] if r else "?"))

    return 0


if __name__ == "__main__":
    sys.exit(main())
