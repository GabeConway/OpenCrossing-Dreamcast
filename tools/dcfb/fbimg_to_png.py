#!/usr/bin/env python3
"""
fbimg_to_png.py — turn a DC_FB_IMAGE console log into PNG screenshots.

WHY
---
Every rendering question past "is the frame black" — is this model inside-out,
is this the right texture, is the sky where the ground should be — used to be
answered by a human watching Flycast and typing what they saw. That serialises
on the human and cannot be diffed between two builds. `DC_FB_IMAGE=<1|2|4>`
(dc/src/dc_pvr.c) makes the guest send the whole frame out over the console
instead, box-filtered by that factor:

    FBIMG BEGIN <w> <h> rgb565 frame=<n>
    FBROW <y> <base64 of w RGB565 pixels, big-endian>
    FBIMG END

This decodes those into real PNGs, so two builds can be compared side by side
and a screenshot can be attached to a claim.

USAGE
-----
    DC_FB_PROBE=300 DC_FB_IMAGE=2 ... bash dc/build-dc.sh
    bash harness/dc/smoke.sh dc/build/OpenCrossing.cdi --fb-writeback
    python3 tools/dcfb/fbimg_to_png.py <run>/console.log --out /tmp/shots

    # or just the last frame:
    python3 tools/dcfb/fbimg_to_png.py <run>/console.log --last

⚠️ `--fb-writeback` is REQUIRED (kb/traps.md): without it Flycast never writes
the rendered frame back into emulated VRAM and every row decodes to black.

No third-party modules — PNG is written with zlib and struct so this runs on a
bare python3.
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


def write_png(path, w, h, rgb):
    """rgb: bytes, 3 bytes per pixel, w*h pixels."""
    raw = bytearray()
    stride = w * 3
    for y in range(h):
        raw.append(0)                      # filter type 0 (None)
        raw += rgb[y * stride:(y + 1) * stride]

    def chunk(tag, data):
        return (struct.pack(">I", len(data)) + tag + data +
                struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF))

    png = b"\x89PNG\r\n\x1a\n"
    png += chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(bytes(raw), 9))
    png += chunk(b"IEND", b"")
    Path(path).write_bytes(png)


def rgb565_to_rgb888(data, w):
    """Expand one row. 5/6/5 -> 8/8/8 by bit replication, not by <<3."""
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


def w_expect(cur):
    """Bytes in one complete row of the frame being accumulated."""
    return cur[1] * 2


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("console")
    ap.add_argument("--out", default=None, help="output directory")
    ap.add_argument("--last", action="store_true", help="only the last frame")
    ap.add_argument("--prefix", default="frame")
    args = ap.parse_args()

    outdir = Path(args.out) if args.out else Path(args.console).parent / "shots"
    outdir.mkdir(parents=True, exist_ok=True)

    frames = []          # (frame_no, w, h, {y: rowbytes})
    cur = None
    for line in Path(args.console).read_text(errors="replace").splitlines():
        m = BEGIN_RE.match(line)
        if m:
            cur = (int(m.group(4)), int(m.group(1)), int(m.group(2)), {})
            continue
        if line.startswith("FBIMG END"):
            if cur:
                frames.append(cur)
            cur = None
            continue
        if cur is not None:
            m = ROW_RE.match(line)
            if m:
                # A run killed mid-row leaves a partial base64 payload. Trim to
                # a whole number of 4-char groups and keep what decoded rather
                # than losing the whole frame to "Incorrect padding".
                b64 = m.group(2)
                b64 = b64[:len(b64) - len(b64) % 4]
                try:
                    row = base64.b64decode(b64)
                except Exception:
                    continue
                if len(row) < w_expect(cur) :
                    row = row + bytes(w_expect(cur) - len(row))
                cur[3][int(m.group(1))] = row[:w_expect(cur)]

    # An emulator killed mid-dump leaves a BEGIN with no END. Keep it — the
    # last frame is usually the most interesting one.
    if cur and cur[3]:
        frames.append(cur)

    if not frames:
        sys.exit("no FBIMG blocks in %s — was the build made with "
                 "DC_FB_IMAGE=<n> and DC_FB_PROBE=<n>, and the run given "
                 "--fb-writeback?" % args.console)

    if args.last:
        frames = frames[-1:]

    for frame_no, w, h, rows in frames:
        missing = [y for y in range(h) if y not in rows]
        rgb = bytearray()
        for y in range(h):
            r = rows.get(y)
            # A truncated run (the emulator killed mid-dump) loses trailing
            # rows. Emit them black and SAY SO rather than dropping the frame:
            # a partial screenshot still answers most questions.
            rgb += rgb565_to_rgb888(r, w) if r else bytes(w * 3)
        path = outdir / ("%s-%04d.png" % (args.prefix, frame_no))
        write_png(path, w, h, bytes(rgb))
        note = "" if not missing else "  (%d rows missing, filled black)" % len(missing)
        print("%s  %dx%d%s" % (path, w, h, note))


if __name__ == "__main__":
    main()
