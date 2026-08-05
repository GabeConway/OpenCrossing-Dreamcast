#!/usr/bin/env python3
"""
shot_diff.py — the VISUAL regression gate. Diff two runs' framebuffer dumps.

WHY
---
`tools/dcqa/run_report.py` is the floor: it proves nothing got slower and
nothing stopped loading. It cannot see colour, and kb/traps.md records a change
that passed every counter and still turned a textured canopy into a flat teal
slab. The standing rule is "judge a renderer change on a screenshot pair at the
same probe index" — this is that comparison, done mechanically instead of by a
human alt-tabbing between two PNGs.

WHAT IT DOES
------------
Parses the `FBIMG BEGIN / FBROW / FBIMG END` blocks out of two console logs
(the same format `fbimg_to_png.py` decodes), pairs frames by their `frame=`
index, and for each pair reports:

    changed%   share of pixels whose colour moved by more than --thresh
    meanabs    mean absolute per-channel difference, 0-255
    maxabs     the largest single-channel difference in the frame
    nz_a/nz_b  nonzero-pixel counts on each side, because "both frames went
               black" is a regression that a pure diff scores as PERFECT

and writes a contact sheet PNG per pair: baseline | candidate | amplified diff.

USAGE
-----
    python3 tools/dcfb/shot_diff.py BASE/console.log CAND/console.log \\
        --out /tmp/shotdiff

    # gate a build: nonzero exit if any paired frame moved more than 2 %
    python3 tools/dcfb/shot_diff.py BASE/console.log CAND/console.log \\
        --fail-over 2.0

⚠️ Both runs must be built with the SAME `DC_FB_PROBE=<n>` / `DC_FB_IMAGE=<n>`,
or the frame indices name different moments in the game and the diff is
meaningless. The tool warns when the index sets do not intersect and falls back
to ordinal pairing, which is weaker evidence — say so if you quote it.

⚠️ A high `changed%` is NOT automatically a regression: the game is not
deterministic across boots (`sys_math.c:7` seeds the town from
`osGetCount()`), so two runs of the SAME build differ in the town. Establish
the same-build noise floor before reading a number as a verdict. Indoor and
title scenes are near-deterministic and are the sharp instrument.

No third-party modules.
"""

import argparse
import base64
import re
import struct
import sys
import zlib
from pathlib import Path

BEGIN_RE = re.compile(r"^FBIMG BEGIN (\d+) (\d+) (\S+) frame=(\d+)")
ROW_RE = re.compile(r"^FBROW (\d+) (\S+)$")


def parse_frames(path):
    """-> {frame_no: (w, h, rgb888 bytes)}, in log order."""
    frames = {}
    cur = None
    for line in Path(path).read_text(errors="replace").splitlines():
        m = BEGIN_RE.match(line)
        if m:
            cur = (int(m.group(4)), int(m.group(1)), int(m.group(2)), {})
            continue
        if line.startswith("FBIMG END"):
            if cur:
                frames[cur[0]] = finish(cur)
            cur = None
            continue
        if cur is not None:
            m = ROW_RE.match(line)
            if m:
                b64 = m.group(2)
                b64 = b64[:len(b64) - len(b64) % 4]
                try:
                    row = base64.b64decode(b64)
                except Exception:
                    continue
                need = cur[1] * 2
                if len(row) < need:
                    row = row + bytes(need - len(row))
                cur[3][int(m.group(1))] = row[:need]
    if cur and cur[3]:                      # run killed mid-dump; keep it
        frames[cur[0]] = finish(cur)
    return frames


def finish(cur):
    _, w, h, rows = cur
    rgb = bytearray()
    for y in range(h):
        r = rows.get(y)
        rgb += rgb565_row(r, w) if r else bytes(w * 3)
    return (w, h, bytes(rgb))


def rgb565_row(data, w):
    out = bytearray(w * 3)
    for x in range(w):
        v = (data[x * 2] << 8) | data[x * 2 + 1]
        r = (v >> 11) & 0x1F
        g = (v >> 5) & 0x3F
        b = v & 0x1F
        out[x * 3] = (r << 3) | (r >> 2)
        out[x * 3 + 1] = (g << 2) | (g >> 4)
        out[x * 3 + 2] = (b << 3) | (b >> 2)
    return out


def write_png(path, w, h, rgb):
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    Path(path).write_bytes(png)


def compare(a, b, thresh):
    """a, b: rgb888 bytes of equal length. -> stats dict + diff image."""
    n = len(a)
    px = n // 3
    diff = bytearray(n)
    total = 0
    maxabs = 0
    changed = 0
    nz_a = 0
    nz_b = 0
    for i in range(0, n, 3):
        da = abs(a[i] - b[i])
        db = abs(a[i + 1] - b[i + 1])
        dc = abs(a[i + 2] - b[i + 2])
        m = da if da > db else db
        if dc > m:
            m = dc
        total += da + db + dc
        if m > maxabs:
            maxabs = m
        if m > thresh:
            changed += 1
        if a[i] or a[i + 1] or a[i + 2]:
            nz_a += 1
        if b[i] or b[i + 1] or b[i + 2]:
            nz_b += 1
        # amplified, clamped — a 4x gain makes a 10-level shift visible
        diff[i] = min(255, da * 4)
        diff[i + 1] = min(255, db * 4)
        diff[i + 2] = min(255, dc * 4)
    return {
        "changed_pct": 100.0 * changed / px,
        "meanabs": total / float(n),
        "maxabs": maxabs,
        "nz_a": nz_a,
        "nz_b": nz_b,
        "px": px,
    }, bytes(diff)


def contact_sheet(w, h, a, b, d):
    """Three frames side by side, 4 px black gutters."""
    gut = 4
    ow = w * 3 + gut * 2
    out = bytearray(ow * h * 3)
    for y in range(h):
        row_o = y * ow * 3
        row_i = y * w * 3
        out[row_o:row_o + w * 3] = a[row_i:row_i + w * 3]
        off = (w + gut) * 3
        out[row_o + off:row_o + off + w * 3] = b[row_i:row_i + w * 3]
        off = (w * 2 + gut * 2) * 3
        out[row_o + off:row_o + off + w * 3] = d[row_i:row_i + w * 3]
    return ow, bytes(out)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("base", help="baseline run's console.log")
    ap.add_argument("cand", help="candidate run's console.log")
    ap.add_argument("--out", default=None, help="directory for contact sheets")
    ap.add_argument("--thresh", type=int, default=8,
                    help="per-channel delta that counts as a changed pixel "
                         "(default 8; RGB565 quantisation alone is up to 8)")
    ap.add_argument("--fail-over", type=float, default=None,
                    help="exit 1 if any paired frame's changed%% exceeds this")
    ap.add_argument("--max-frames", type=int, default=12,
                    help="cap on contact sheets written (default 12)")
    args = ap.parse_args()

    fa = parse_frames(args.base)
    fb = parse_frames(args.cand)
    if not fa or not fb:
        sys.exit("no FBIMG blocks in %s — build with DC_FB_IMAGE=<n> "
                 "DC_FB_PROBE=<n> and run with --fb-writeback"
                 % (args.base if not fa else args.cand))

    shared = sorted(set(fa) & set(fb))
    ordinal = False
    if not shared:
        ordinal = True
        ka, kb = sorted(fa), sorted(fb)
        pairs = list(zip(ka, kb))
        print("!! no shared frame indices — falling back to ORDINAL pairing. "
              "Weaker evidence: say so if you quote this.", file=sys.stderr)
    else:
        pairs = [(k, k) for k in shared]

    outdir = Path(args.out) if args.out else None
    if outdir:
        outdir.mkdir(parents=True, exist_ok=True)

    print("%-8s %-8s %9s %9s %7s %10s %10s" %
          ("base", "cand", "changed%", "meanabs", "maxabs", "nz_base", "nz_cand"))
    worst = 0.0
    written = 0
    for ka, kb in pairs:
        wa, ha, ra = fa[ka]
        wb, hb, rb = fb[kb]
        if (wa, ha) != (wb, hb):
            print("%-8d %-8d  SKIP: %dx%d vs %dx%d" % (ka, kb, wa, ha, wb, hb))
            continue
        st, dimg = compare(ra, rb, args.thresh)
        worst = max(worst, st["changed_pct"])
        print("%-8d %-8d %8.2f%% %9.3f %7d %10d %10d" %
              (ka, kb, st["changed_pct"], st["meanabs"], st["maxabs"],
               st["nz_a"], st["nz_b"]))
        if outdir and written < args.max_frames:
            ow, sheet = contact_sheet(wa, ha, ra, rb, dimg)
            p = outdir / ("diff-%04d.png" % ka)
            write_png(p, ow, ha, sheet)
            written += 1

    if outdir:
        print("\n%d contact sheet(s) in %s  (base | cand | diff x4)"
              % (written, outdir))
    if ordinal:
        print("PAIRING: ordinal (frame indices did not match)")
    print("worst changed%%: %.2f" % worst)

    if args.fail_over is not None and worst > args.fail_over:
        sys.exit(1)


if __name__ == "__main__":
    main()
