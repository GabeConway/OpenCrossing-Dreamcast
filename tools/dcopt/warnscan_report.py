#!/usr/bin/env python3
"""
warnscan_report.py — reduce dc/build/warnscan.log to the files worth suspecting

The scan itself is `DC_TARGET=warnscan bash dc/build-dc.sh`, which recompiles
every TU at -O2 with the decomp's `-w` removed. It emits ~65,000 warnings, of
which about 60,000 are -Wcomment and -Wunused-function — noise from vendored
decomp that no optimizer acts on.

This tool keeps only the classes that can change BEHAVIOUR when the optimizer
is turned on, ranks the files by them, and can emit a candidate list for
tools/dcopt/bisect_o0.sh.

    python3 tools/dcopt/warnscan_report.py dc/build/warnscan.log
    python3 tools/dcopt/warnscan_report.py dc/build/warnscan.log --paths > cand.txt

WHY THESE CLASSES, in the order they deserve suspicion:

  return-type   Falling off the end of a non-void function. In C the caller
                reads a garbage register; in C++ (four TUs here, CXXFROMC_SRC)
                G++ turns the fall-through edge into __builtin_unreachable and
                DELETES the path, side effects included. There is no flag that
                turns this off. Rank 1.
  uninit        -Wuninitialized / -Wmaybe-uninitialized. At -O0 the local gets
                whatever was on the stack; optimized, value-range propagation
                may fold branches on it. DC_AUTOVAR_INIT=zero is the A/B.
  aliasing      -Wstrict-aliasing. Mostly neutralised by -fno-strict-aliasing
                in UB_GUARDS, so this is informational — EXCEPT where the pun
                is also unaligned, which traps on SH-4 and no flag covers.
  bounds        -Warray-bounds. Real out-of-bounds; no flag covers it.
  sequence      -Wsequence-point. Unsequenced modification, and evaluation
                order genuinely changes with optimization level.

A file appearing here is a SUSPECT, not a defect. The port ran the whole town
at -Os with 35 return-type warnings outstanding. Use this to order a bisect,
not to pre-emptively quarantine (dc/opt-lists.mk says why).
"""
import argparse
import collections
import re
import sys

CLASSES = [
    ("return-type", re.compile(
        r"control reaches end of non-void function"
        r"|no return statement in function returning non-void")),
    ("uninit", re.compile(r"\[-W(?:maybe-)?uninitialized\]")),
    ("bounds", re.compile(r"\[-Warray-bounds")),
    ("sequence", re.compile(r"\[-Wsequence-point\]")),
    ("aliasing", re.compile(r"\[-Wstrict-aliasing")),
]

# A warning line is  <path>:<line>:<col>: warning: <text>
LINE = re.compile(r"^(?P<path>[^:\s]+):(?P<line>\d+):(?P<col>\d+): warning: (?P<msg>.*)$")

# The build runs in the container, where the repo is /work. Scratch-tree paths
# (dc/build/stubsrc/..., dc/build/shrinksrc/...) are mapped back to the source
# they were rewritten from, and .c_inc paths are left alone: they are bodies
# textually included by another TU, and it is the INCLUDER that gets compiled.
STRIP = [
    ("/work/", ""),
    ("dc/build/stubsrc/", ""),
    ("dc/build/shrinksrc/", ""),
    ("include/../", ""),
]


def normalise(path: str) -> str:
    for a, b in STRIP:
        if path.startswith(a):
            path = b + path[len(a):]
    return path


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("log")
    ap.add_argument("--paths", action="store_true",
                    help="emit only the ranked file list, for bisect_o0.sh")
    ap.add_argument("--top", type=int, default=40)
    args = ap.parse_args()

    per_file = collections.defaultdict(collections.Counter)
    sites = collections.defaultdict(list)

    with open(args.log, errors="replace") as fh:
        for raw in fh:
            m = LINE.match(raw.rstrip("\n"))
            if not m:
                continue
            for name, rx in CLASSES:
                if rx.search(m.group("msg")):
                    path = normalise(m.group("path"))
                    per_file[path][name] += 1
                    if name == "return-type":
                        sites[path].append(m.group("line"))
                    break

    if not per_file:
        print("no interesting warnings found — is this a warnscan log?",
              file=sys.stderr)
        return 1

    def weight(counts):
        # return-type first, then uninit; the rest break ties.
        return (counts["return-type"], counts["uninit"],
                counts["bounds"] + counts["sequence"], counts["aliasing"])

    ranked = sorted(per_file.items(), key=lambda kv: weight(kv[1]), reverse=True)

    if args.paths:
        for path, counts in ranked:
            if not path.endswith(".c_inc"):
                print(path)
        return 0

    hdr = f"{'file':<62}" + "".join(f"{n:>13}" for n, _ in CLASSES)
    print(hdr)
    print("-" * len(hdr))
    for path, counts in ranked[:args.top]:
        row = f"{path:<62}" + "".join(f"{counts[n] or '':>13}" for n, _ in CLASSES)
        print(row)

    tot = collections.Counter()
    for counts in per_file.values():
        tot.update(counts)
    print()
    print("totals:", ", ".join(f"{n}={tot[n]}" for n, _ in CLASSES))
    print(f"files:  {len(per_file)}")
    print()
    print("⚠️ .c_inc rows are BODIES, not TUs — quarantine the file that")
    print("   #includes them, not the row. --paths drops them for that reason.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
