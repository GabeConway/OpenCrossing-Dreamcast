#!/usr/bin/env python3
"""Convert `audiorom.img`'s VADPCM bank to an AICA-ADPCM pack for the runtime.

    python3 tools/dcaudio/pack.py --out ~/.cache/oc-dc-discroot/aicabank.pak

Output is one file: a sorted index plus 4-bit AICA ADPCM payloads, each aligned
so it can be uploaded straight into sound RAM.  Nothing about it is
game-specific except the numbers; the format is documented below and in
`kb/asset-pack.md`'s spirit — a contract between a host tool and `dc/`.

THE INDEX KEY IS `device_addr`, the sample's raw byte offset into
`audiorom.img`.  The runtime recovers it from a live pointer as
`sampleAddr - GetNeosRomTop()` (`system.c:1299`, `dummyrom.c:25-27`), so no
build-order coupling exists between this file and the game.  Entries are sorted
by that key so the runtime can binary-search.

WHAT IS DELIBERATELY LEFT OUT, and why:

  * **Samples over 65,534 decoded samples are EXCLUDED.**  An AICA channel's
    LSA/LEA are 16-bit sample offsets, so a longer sample cannot be played by
    one channel at all.  There are 24 of them, all in wave bank 5, and they are
    21 % of the bank's bytes.  They are long-form material and want a streaming
    path, not residency — see `kb/audio-aica-offload.md` §4.  Excluding them is
    also what makes soundfont 153 fit.  The index records them as
    `FLAG_TOO_LONG` with no payload so the runtime can fall back to software
    for those voices rather than going silent.

  * **No residency policy.**  This packs everything playable into one file; it
    does NOT decide what is resident in the 1,900,544 B of sound RAM at any
    moment.  That is the runtime's job, driven per sequence start.

⚠️ ENCODER DEFAULT IS `open-loop`, KOS's arithmetic quantiser, because an
exhaustive 16-way search was MEASURED to be a wash against it (±0.3 dB, and it
loses on one sample — `kb/audio-aica-offload.md` §7).  `--encoder exhaustive`
is there to re-check that claim, not because it is better.

⚠️ `--leak` selects between KOS's encoder (0, the default) and KOS's decoder
(254), which disagree with each other.  Which one matches silicon is UNKNOWN
and needs a burn.  Do not change the default casually — every shipped DC ADPCM
asset went through the no-leak encoder.
"""

from __future__ import annotations

import argparse
import os
import struct
import sys
import time

try:
    from .audiorom import AudioRom
    from . import aica_adpcm as A
except ImportError:
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from dcaudio.audiorom import AudioRom
    from dcaudio import aica_adpcm as A

MAGIC = b"ACAB"
VERSION = 1

# snd_mem_malloc rounds every allocation up to 32 bytes (snd_mem.c:153), and
# g2 block writes want 32-byte granularity, so align payloads to 32.
ALIGN = 32

HEADER_FMT = ">4sIIIII"          # magic, version, n_entries, index_off, data_off, reserved
HEADER_SIZE = struct.calcsize(HEADER_FMT)

# device_addr, data_off, data_len, n_samples, loop_start, loop_end, flags
ENTRY_FMT = ">IIIIIII"
ENTRY_SIZE = struct.calcsize(ENTRY_FMT)

FLAG_LOOPS = 1 << 0
FLAG_TOO_LONG = 1 << 1           # excluded: needs streaming or a software voice


def align_up(n: int, a: int = ALIGN) -> int:
    return (n + a - 1) // a * a


def verify(path: str, rom: AudioRom, encoder: str, leak: int,
           n_probe: int = 8) -> int:
    """Re-derive the pack from the source bank and diff it.

    This is the falsifier, and it is not optional discipline: a packer bug
    produces a file that parses, uploads and plays NOISE, which on hardware is
    indistinguishable from a codec bug, a residency bug or a register bug.
    Catching it here costs seconds; catching it on a burn costs a session.

    ⚠️ `encoder`/`leak` must match what wrote the file. Re-encoding with
    different settings reports a false mismatch — that mistake was made once
    already, with `exhaustive` defaulting on against an `open-loop` pack.
    """
    import random

    with open(path, "rb") as fh:
        p = fh.read()

    magic, ver, n, ioff, doff, _ = struct.unpack_from(HEADER_FMT, p, 0)
    if magic != MAGIC:
        print(f"  FAIL: magic is {magic!r}, expected {MAGIC!r}")
        return 1
    if ver != VERSION:
        print(f"  FAIL: version {ver}, expected {VERSION}")
        return 1

    ents = [
        struct.unpack_from(ENTRY_FMT, p, ioff + i * ENTRY_SIZE)
        for i in range(n)
    ]

    fails = 0

    if not all(ents[i][0] < ents[i + 1][0] for i in range(n - 1)):
        print("  FAIL: index is not sorted by device_addr")
        fails += 1

    payloads = sorted((e[1], e[2]) for e in ents if not (e[6] & FLAG_TOO_LONG))
    if not all(payloads[i][0] + payloads[i][1] <= payloads[i + 1][0]
               for i in range(len(payloads) - 1)):
        print("  FAIL: payloads overlap")
        fails += 1

    if not all(e[2] == 0 for e in ents if e[6] & FLAG_TOO_LONG):
        print("  FAIL: a FLAG_TOO_LONG entry carries a payload")
        fails += 1

    by_addr = {s.file_off: s for s in rom.unique_samples().values()}
    if len(by_addr) != n:
        print(f"  FAIL: pack has {n} entries, bank has {len(by_addr)}")
        fails += 1

    random.seed(1)
    playable = [e for e in ents if not (e[6] & FLAG_TOO_LONG)]
    for e in random.sample(playable, min(n_probe, len(playable))):
        dev, off, ln, ns, ls, le, fl = e
        s = by_addr.get(dev)
        if s is None:
            print(f"  FAIL: device_addr 0x{dev:X} is not in the bank")
            fails += 1
            continue
        if ns != s.n_samples or ln != (ns + 1) // 2:
            print(f"  FAIL: 0x{dev:X} geometry n={ns} len={ln}")
            fails += 1
            continue
        ref = rom.decode_sample(s)
        exp, _ = A.encode(ref, leak=leak,
                          exhaustive=(encoder == "exhaustive"))
        blob = p[doff + off: doff + off + ln]
        if bytes(exp) != blob:
            print(f"  FAIL: 0x{dev:X} payload differs from a fresh encode")
            fails += 1
            continue
        back = A.decode(blob, ns)
        print(f"    0x{dev:X}  n={ns:>6}  snr={A.snr_db(ref, back):6.2f} dB"
              f"  loops={bool(fl & FLAG_LOOPS)}")

    print(f"  {'FAILED' if fails else 'OK'}: {n} entries, "
          f"{sum(1 for e in ents if e[6] & FLAG_TOO_LONG)} excluded, "
          f"{fails} problem(s)")
    return 1 if fails else 0


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    ap.add_argument("--img", default=None)
    ap.add_argument("--headers", default=None)
    ap.add_argument("--out", required=True)
    ap.add_argument("--verify", action="store_true",
                    help="verify an existing --out instead of writing it")
    ap.add_argument("--encoder", choices=("open-loop", "exhaustive"),
                    default="open-loop")
    ap.add_argument("--leak", type=int, default=0, choices=(0, 254))
    ap.add_argument("--limit", type=int, default=0,
                    help="stop after N samples (for a quick check)")
    args = ap.parse_args()

    kw = {}
    if args.img:
        kw["img_path"] = args.img
    if args.headers:
        kw["headers_path"] = args.headers
    rom = AudioRom(**kw)

    if args.verify:
        print(f"verifying {args.out} "
              f"(encoder={args.encoder}, leak={args.leak})")
        return verify(args.out, rom, args.encoder, args.leak)

    samples = sorted(rom.unique_samples().values(), key=lambda s: s.file_off)
    if args.limit:
        samples = samples[: args.limit]

    print(f"packing {len(samples)} samples "
          f"(encoder={args.encoder}, leak={args.leak})")

    entries = []
    blobs = []
    data_cursor = 0
    n_excluded = 0
    n_encoded = 0
    t0 = time.time()

    for i, s in enumerate(samples):
        flags = FLAG_LOOPS if s.loop.loops else 0

        if s.n_samples > A.CHANNEL_MAX_SAMPLES:
            n_excluded += 1
            entries.append((s.file_off, 0, 0, s.n_samples,
                            s.loop.loop_start, s.loop.loop_end,
                            flags | FLAG_TOO_LONG))
            continue

        pcm = rom.decode_sample(s)
        packed, _ = A.encode(
            pcm, leak=args.leak,
            exhaustive=(args.encoder == "exhaustive"),
        )
        off = data_cursor
        entries.append((s.file_off, off, len(packed), s.n_samples,
                        s.loop.loop_start, s.loop.loop_end, flags))
        blobs.append(bytes(packed))
        data_cursor = align_up(off + len(packed))
        n_encoded += 1

        if (i + 1) % 200 == 0:
            el = time.time() - t0
            print(f"  {i + 1}/{len(samples)}  {el:5.1f}s  "
                  f"{data_cursor:,} B")

    index_off = HEADER_SIZE
    data_off = align_up(index_off + len(entries) * ENTRY_SIZE)

    with open(args.out, "wb") as fh:
        fh.write(struct.pack(HEADER_FMT, MAGIC, VERSION, len(entries),
                             index_off, data_off, 0))
        for e in entries:
            fh.write(struct.pack(ENTRY_FMT, *e))
        fh.write(b"\0" * (data_off - (index_off + len(entries) * ENTRY_SIZE)))
        written = 0
        for b in blobs:
            fh.write(b)
            pad = align_up(len(b)) - len(b)
            fh.write(b"\0" * pad)
            written += len(b) + pad

    total = os.path.getsize(args.out)
    print()
    print(f"  encoded   {n_encoded} samples")
    print(f"  excluded  {n_excluded} over {A.CHANNEL_MAX_SAMPLES} samples "
          f"(recorded as FLAG_TOO_LONG, no payload)")
    print(f"  payload   {written:,} B")
    print(f"  file      {total:,} B -> {args.out}")
    print(f"  elapsed   {time.time() - t0:.1f}s")
    return 0


if __name__ == "__main__":
    sys.exit(main())
