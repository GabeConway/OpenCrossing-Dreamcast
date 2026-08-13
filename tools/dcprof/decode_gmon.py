#!/usr/bin/env python3
"""decode_gmon.py — recover a gmon.out from a Dreamcast console.log.

WHAT THIS IS FOR
================
`dc/src/dc_profdump.c` mounts a write-only KOS VFS at /prof and streams every
byte libgprof writes to gmon.out out of the SCIF serial console, RLE'd and
base64'd, as framed `[GPROF]` lines.  That is the only channel off a Dreamcast
booting from a burned CD-R: there is no /pc without a dc-load host connection.
This script is the other end.  It reads a console.log (from a real console's
serial capture or from Flycast's), verifies the framing, and writes gmon.out.

⚠️ IT REFUSES TO WRITE A HALF FILE.  A truncated or corrupt stream is the
normal failure mode of a serial capture, and a gmon.out that is 90 % of a
histogram is not 90 % of a profile — it is a wrong profile that still opens in
gprof and still prints plausible percentages.  Every check below is fatal.

THE WIRE FORMAT (mirror of dc_profdump.c; change one, change the other)
======================================================================
  [GPROF] BEGIN v=1 enc=z0b64 cols=<n> low=<hex> high=<hex> alloc=<bytes>
  [GPROF] <idx:06x> <base64> <cksum:02x>
  ...
  [GPROF] END lines=<n> raw=<bytes> enc=<bytes> crc32=<hex>

  idx     monotonic from 0 across the whole stream.  A gap is a dropped line.
  cksum   sum of that line's base64 characters mod 256.  Names WHICH line the
          serial link corrupted instead of failing one CRC with no location.
  raw     length of gmon.out in bytes.  The contract this script must satisfy.
  crc32   CRC-32 (reflected, poly 0xEDB88320, init/final 0xFFFFFFFF) over those
          same raw bytes.

  z0 decoding, byte by byte:
    b != 0x00  -> literal b
    0x00       -> followed by a LEB128 count N (7 bits per byte, low group
                  first, high bit set on all but the last); emit N zero bytes.

⚠️ THE STREAM IS INTERLEAVED WITH THE GAME'S OWN OUTPUT, BY DESIGN.  The 20-byte
gmon header is written at boot by monstartup() and the histogram thousands of
frames later by _mcleanup(), with the whole run's logging in between.  Lines
are located by searching for the tag anywhere in a line, not by anchoring, so a
game printf that did not end in a newline cannot swallow a payload line.
"""

import argparse
import binascii
import os
import re
import sys

TAG = r"\[GPROF\]"

RE_BEGIN = re.compile(
    TAG + r"\s+BEGIN\s+v=(?P<v>\d+)\s+enc=(?P<enc>\S+)\s+cols=(?P<cols>\d+)"
          r"\s+low=(?P<low>[0-9a-fA-F]+)\s+high=(?P<high>[0-9a-fA-F]+)"
          r"\s+alloc=(?P<alloc>\d+)")
RE_LINE = re.compile(
    TAG + r"\s+(?P<idx>[0-9a-f]{6})\s+(?P<b64>[A-Za-z0-9+/=]+)\s+"
          r"(?P<ck>[0-9a-f]{2})\s*$")
RE_END = re.compile(
    TAG + r"\s+END\s+lines=(?P<lines>\d+)\s+raw=(?P<raw>\d+)"
          r"\s+enc=(?P<enc>\d+)\s+crc32=(?P<crc>[0-9a-fA-F]+)")
RE_ARM = re.compile(
    TAG + r"\s+arming:\s+range\s+(?P<low>[0-9a-f]+)\.\.(?P<high>[0-9a-f]+)")
RE_FATAL = re.compile(TAG + r"\s+(FATAL|flush requested but never armed)")


class Fatal(Exception):
    pass


def die(msg):
    raise Fatal(msg)


def unz0(enc):
    """Undo the zero-run RLE.  Returns bytes."""
    out = bytearray()
    i = 0
    n = len(enc)
    while i < n:
        b = enc[i]
        i += 1
        if b != 0:
            out.append(b)
            continue
        # zero run: LEB128 count follows
        count = 0
        shift = 0
        while True:
            if i >= n:
                die("z0: stream ends inside a zero-run length "
                    "(encoded stream is truncated)")
            c = enc[i]
            i += 1
            count |= (c & 0x7F) << shift
            if not (c & 0x80):
                break
            shift += 7
            if shift > 35:
                die("z0: zero-run length overflows 32 bits — stream corrupt")
        if count == 0:
            die("z0: zero-run of length 0 at encoded offset %d — stream corrupt"
                % (i,))
        out.extend(b"\0" * count)
    return bytes(out)


def collect(path):
    """Parse the log.  Returns (begin, payload_lines, end) for the LAST
    complete frame in the file."""
    frames = []          # list of dicts
    cur = None
    fatal_lines = []

    with open(path, "r", encoding="utf-8", errors="replace") as fh:
        for lineno, line in enumerate(fh, 1):
            if "[GPROF]" not in line:
                continue

            if RE_FATAL.search(line):
                fatal_lines.append((lineno, line.strip()))

            m = RE_BEGIN.search(line)
            if m:
                if cur is not None:
                    frames.append(cur)          # unterminated; kept for report
                cur = {"begin": m.groupdict(), "lines": [], "end": None,
                       "beginno": lineno}
                continue

            m = RE_END.search(line)
            if m:
                if cur is None:
                    die("line %d: END with no preceding BEGIN" % lineno)
                cur["end"] = m.groupdict()
                cur["endno"] = lineno
                frames.append(cur)
                cur = None
                continue

            m = RE_LINE.search(line)
            if m:
                if cur is None:
                    # A payload line before any BEGIN means the capture started
                    # mid-stream.  That is unrecoverable, not ignorable.
                    die("line %d: payload line before any BEGIN — the capture "
                        "started after the console did.  Re-run with the "
                        "capture already attached at power-on." % lineno)
                cur["lines"].append((lineno, m.groupdict()))

    if cur is not None:
        frames.append(cur)

    return frames, fatal_lines


def decode_frame(fr):
    b = fr["begin"]
    e = fr["end"]

    if e is None:
        die("the stream has a BEGIN (log line %d) but no END: the run was cut "
            "short, or DC_GPROF_DUMP_FRAME was never reached.  %d payload "
            "lines were captured." % (fr["beginno"], len(fr["lines"])))

    if b["enc"] != "z0b64":
        die("unknown encoding %r — this decoder speaks z0b64 only" % b["enc"])
    if b["v"] != "1":
        die("unknown framing version %s" % b["v"])

    # --- per-line integrity ------------------------------------------------
    chars = []
    expect_idx = 0
    for lineno, g in fr["lines"]:
        idx = int(g["idx"], 16)
        if idx != expect_idx:
            die("log line %d: payload index 0x%06x, expected 0x%06x — %d "
                "line(s) were dropped by the console link.  The profile "
                "cannot be reconstructed; re-run the capture."
                % (lineno, idx, expect_idx, idx - expect_idx))
        got = sum(ord(c) for c in g["b64"]) & 0xFF
        want = int(g["ck"], 16)
        if got != want:
            die("log line %d (payload index 0x%06x): line checksum %02x, "
                "expected %02x — the serial link corrupted this line."
                % (lineno, idx, got, want))
        chars.append(g["b64"])
        expect_idx += 1

    n_lines = int(e["lines"])
    if expect_idx != n_lines:
        die("END says %d payload lines, %d were captured — %d missing at the "
            "tail of the stream." % (n_lines, expect_idx, n_lines - expect_idx))

    # --- base64 ------------------------------------------------------------
    b64 = "".join(chars)
    try:
        enc = binascii.a2b_base64(b64)
    except binascii.Error as exc:
        die("base64 decode failed: %s" % exc)

    n_enc = int(e["enc"])
    if len(enc) != n_enc:
        die("END says %d encoded bytes, base64 yielded %d" % (n_enc, len(enc)))

    # --- z0 ----------------------------------------------------------------
    raw = unz0(enc)

    n_raw = int(e["raw"])
    if len(raw) != n_raw:
        die("END says %d raw bytes, z0 yielded %d — the RLE stream is corrupt"
            % (n_raw, len(raw)))

    want_crc = int(e["crc"], 16)
    got_crc = binascii.crc32(raw) & 0xFFFFFFFF
    if got_crc != want_crc:
        die("CRC-32 mismatch: computed %08x, stream says %08x.  Every line "
            "passed its own checksum, so this is a decoder/encoder "
            "disagreement, not a link fault." % (got_crc, want_crc))

    # --- sanity: is this actually a gmon.out? ------------------------------
    if len(raw) < 20 or raw[0:4] != b"gmon":
        die("decoded %d bytes but they do not start with the 'gmon' cookie — "
            "the stream decoded cleanly and still is not a profile.  Check "
            "that libgprof was linked (-pg on the LINK line)." % len(raw))

    return raw, b


def summarize(raw, begin):
    """Print what the profile contains, from its own header, so a zero-sample
    run is visible here instead of as a blank gprof report."""
    import struct
    # gmon_hdr_t: char[4] cookie, int32 version, char[12] spare  = 20 bytes
    # then tag byte 0 + gmon_hist_hdr_t:
    #   uintptr lowpc, uintptr highpc, u32 num_bins, u32 profrate,
    #   char units[15], char units_abbrev            = 4+4+4+4+15+1 = 32
    if len(raw) < 20 + 1 + 32:
        print("  (no histogram record in the stream)", file=sys.stderr)
        return
    if raw[20] != 0:
        print("  (first record is not a histogram tag)", file=sys.stderr)
        return
    lowpc, highpc, nbins, profrate = struct.unpack_from("<IIII", raw, 21)
    avail = (len(raw) - (21 + 32)) // 2
    if nbins != avail:
        print("  ⚠️  header claims %d bins but %d are present — the record is "
              "not self-consistent; gprof may reject it." % (nbins, avail),
              file=sys.stderr)
        nbins = min(nbins, avail)
    bins = raw[21 + 32: 21 + 32 + nbins * 2]
    vals = struct.unpack_from("<%dH" % (len(bins) // 2), bins, 0)
    total = sum(vals)
    nonzero = sum(1 for v in vals if v)
    print("  histogram : %08x..%08x, %d bins, profrate %d Hz"
          % (lowpc, highpc, nbins, profrate), file=sys.stderr)
    print("  samples   : %d in %d non-empty bins (~%.1f s of profiled thread "
          "time at %d Hz)"
          % (total, nonzero, (total / profrate) if profrate else 0.0, profrate),
          file=sys.stderr)
    if total == 0:
        print("  ⚠️  ZERO SAMPLES.  The sampler never saw a PC inside the "
              "range.  Either the run never left the range's blind spot or "
              "DC_GPROF_SPAN excluded the code that ran.", file=sys.stderr)
    elif total < 500:
        print("  ⚠️  Fewer than 500 samples: rank the top few entries only. "
              "Raise DC_GPROF_DUMP_FRAME or set DC_GPROF_HZ=1000.",
              file=sys.stderr)


def main():
    ap = argparse.ArgumentParser(
        description="Recover gmon.out from a Dreamcast console.log.")
    ap.add_argument("log", help="console.log captured from SCIF or Flycast")
    ap.add_argument("-o", "--out", default="gmon.out",
                    help="output file (default: gmon.out)")
    ap.add_argument("--elf", default="dc/build/AnimalCrossing.elf",
                    help="ELF the run was built from, for the gprof command "
                         "line printed at the end")
    ap.add_argument("--frame", type=int, default=-1,
                    help="which BEGIN/END frame to decode when the log holds "
                         "more than one (default: the last complete one)")
    args = ap.parse_args()

    try:
        frames, fatals = collect(args.log)

        for lineno, text in fatals:
            print("  guest reported: line %d: %s" % (lineno, text),
                  file=sys.stderr)

        if not frames:
            die("no [GPROF] BEGIN line in %s.\n"
                "  - Was the image built with DC_GPROF=1 and -pg on the LINK "
                "line?\n"
                "  - Is DC_CONSOLE_MUTE set?  It disables dbgio and the dump "
                "goes nowhere.\n"
                "  - Is DC_SCIF_FAST set?  A coder's cable will not sync at "
                "1.5 Mbps and the capture is garbage." % args.log)

        complete = [f for f in frames if f["end"] is not None]
        if not complete:
            # decode_frame will produce the precise diagnostic
            decode_frame(frames[-1])

        if len(complete) > 1:
            print("  note: %d complete streams in this log; decoding #%d"
                  % (len(complete), args.frame if args.frame >= 0 else
                     len(complete) - 1), file=sys.stderr)

        fr = complete[args.frame]
        raw, begin = decode_frame(fr)

    except Fatal as exc:
        print("decode_gmon: FAILED — %s" % exc, file=sys.stderr)
        print("decode_gmon: no output written.", file=sys.stderr)
        return 2

    with open(args.out, "wb") as fh:
        fh.write(raw)

    print("decode_gmon: wrote %s (%d bytes) from %d payload lines"
          % (args.out, len(raw), len(fr["lines"])), file=sys.stderr)
    print("  range     : %s..%s (from BEGIN)"
          % (begin["low"], begin["high"]), file=sys.stderr)
    summarize(raw, begin)
    print(file=sys.stderr)
    print("Next:", file=sys.stderr)
    print("  sh-elf-gprof -b -p %s %s > %s"
          % (args.elf, args.out, os.path.splitext(args.out)[0] + ".txt"),
          file=sys.stderr)
    print("  (inside the SDK container, or with $KOS_GPROF on the host. -p is "
          "the flat profile; there is no call graph — nothing was compiled "
          "-pg, by design.)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    sys.exit(main())
