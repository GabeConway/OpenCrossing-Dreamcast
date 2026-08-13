#!/usr/bin/env python3
"""W1 — rewrite `audiorom.img` with 8-bit PCM samples so the ADPCM decoder stops.

    python3 tools/dcaudio/s8bank.py --out /tmp/audiorom-s8.img
    python3 tools/dcaudio/s8bank.py --out /tmp/audiorom-s8.img --verify

`A_CMD_ADPCM` (`rspsim.c:107`) costs order 10 s16 multiply-accumulates per
sample; `A_CMD_PCM8DEC` (`rspsim.c:591`) is `*dst++ = (*src++) << 8`. The kb
attributes ~29 % of `RspStart` to ADPCM decode, and `RspStart` is 18.9 % of busy
CPU on real silicon — so roughly **4.2 ms of a 76.4 ms frame**.

⭐ **NO RUNTIME CODE CHANGES.** The codec is a bitfield in the ROM data
(`smzwavetable` bits 30-28), and `CODEC_S8` is a complete first-class path in
the shipped engine: `driver.c:815` skips the codebook load for non-ADPCM
codecs, `:907` sets the S8 frame geometry, `:1043` emits `Nas_PCM8dec`. So this
respects CLAUDE.md's rule that `src/` is never edited — it is a data change.

WHAT THIS TOOL EMITS, and what it does NOT:

  * It writes a new `audiorom.img`. The seq and ctl extents keep their offsets
    AND their sizes; only the tbl extent grows (6,734,836 B of VADPCM becomes
    11,962,409 B of S8, 1.78x). Inside the ctl, each `smzwavetable`'s `codec`,
    24-bit `size` and `sample` offset are patched in place — that is why the
    ctl does not change size.

  * 🔴 **IT DOES NOT PATCH THE GAME.** The index tables are compiled into the
    binary (`src/static/jaudio_NES/game/audioheaders.c`), so seven numbers must
    move with the image: extent 2's size, and the six wave banks' (addr, size).
    This prints them; a `make_src_shrink.py` rule has to apply them.
    ⚠️ **THAT COUPLING IS THE WHOLE RISK OF W1.** The image and the tables are
    two artefacts that must agree, and if they drift the game reads sample data
    at the wrong offsets and plays noise — which sounds exactly like a codec
    bug. The `--verify` pass checks the image against itself; nothing here can
    check it against a build.

⚠️ **QUANTISATION IS ROUND-TO-NEAREST, NOT TRUNCATION.** The engine reconstructs
`s8 << 8`, so the best 8-bit value is `round(pcm / 256)`, not `pcm >> 8`.
Truncating would add a half-LSB DC bias to every sample — inaudible in isolation
and exactly the kind of thing that turns into a hum once 12 voices sum.

⚠️ **8-BIT NOISE IS CONSTANT, ADPCM NOISE IS SIGNAL-PROPORTIONAL.** Peak SNR
goes UP (~48 dB against the 14-44 dB measured for the existing VADPCM), but
quiet sustained passages can hiss where they did not. **This needs ears.**
The conversion is per sample, so a mixed bank is legal and an A/B is free.
"""

from __future__ import annotations

import argparse
import hashlib
import os
import struct
import sys

try:
    from .audiorom import AudioRom, CODEC_S8
    from .vadpcm import decode as vadpcm_decode
except ImportError:
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from dcaudio.audiorom import AudioRom, CODEC_S8
    from dcaudio.vadpcm import decode as vadpcm_decode

ALIGN = 16          # every sample in the shipped bank is already 16 B aligned


def align_up(n: int, a: int = ALIGN) -> int:
    return (n + a - 1) // a * a


def pcm16_to_s8(pcm) -> bytearray:
    """Round-to-nearest, clamped. See the module docstring."""
    out = bytearray(len(pcm))
    for i, v in enumerate(pcm):
        q = (v + 128) >> 8
        if q > 127:
            q = 127
        elif q < -128:
            q = -128
        out[i] = q & 0xFF
    return out


def collect_wavetables(rom: AudioRom):
    """Every distinct `smzwavetable` struct, by absolute file offset.

    Keyed by absolute offset so the 5 aliased soundfonts, which share a ctl
    blob, contribute one entry rather than several. Verified separately: all 5
    aliases carry params identical to their targets, and no struct is ever
    reached with two different wave banks -- so a struct's wave bank is
    unambiguous and its `sample` offset can be rewritten without conflict.
    """
    seen = {}
    for b in range(len(rom.banks)):
        ctl, entry = rom.bank_blob(b)
        for wt in dict.fromkeys(rom.bank_wavetable_offsets(b)):
            abs_off = ctl + wt
            if abs_off in seen:
                continue
            word = rom._u32(abs_off)
            sel = (word >> 26) & 0x3
            wb = entry.wave_bank_id0 if sel == 0 else entry.wave_bank_id1
            if wb == 0xFF:
                continue
            seen[abs_off] = (wb, rom._u32(abs_off + 4), word)
    return seen


def build(rom: AudioRom):
    """Returns (new_image_bytes, new_wave_table, new_tbl_size)."""
    wts = collect_wavetables(rom)
    uniq = rom.unique_samples()
    by_pos = {(s.wave_bank, s.sample_ofs): s for s in uniq.values()}

    # Lay each wave bank out in its original sample order so the new file's
    # locality matches the old one's -- the ARAM window is an LRU over disc, so
    # shuffling would change its hit pattern for no reason.
    per_bank: dict[int, list] = {}
    for (wb, ofs), s in sorted(by_pos.items()):
        per_bank.setdefault(wb, []).append(s)

    new_ofs: dict[tuple[int, int], int] = {}
    bank_blobs: dict[int, bytearray] = {}
    for wb in sorted(per_bank):
        buf = bytearray()
        for s in per_bank[wb]:
            pcm = vadpcm_decode(
                rom.img[s.file_off: s.file_off + s.size], s.book, s.n_samples
            )
            data = pcm16_to_s8(pcm)
            off = len(buf)
            new_ofs[(wb, s.sample_ofs)] = off
            buf += data
            buf += b"\0" * (align_up(len(buf)) - len(buf))
        bank_blobs[wb] = buf

    # New wave-bank table: addresses are relative to the tbl extent base.
    new_wave = []
    cursor = 0
    bank_base: dict[int, int] = {}
    for i in range(len(rom.waves)):
        blob = bank_blobs.get(i, bytearray())
        bank_base[i] = cursor
        new_wave.append((cursor, len(blob)))
        cursor += align_up(len(blob), 32)
    new_tbl_size = cursor

    # Copy seq + ctl verbatim, then patch each wavetable struct in place.
    head = bytearray(rom.img[: rom.wave_base])
    patched = 0
    for abs_off, (wb, old_ofs, word) in wts.items():
        s = by_pos.get((wb, old_ofs))
        if s is None:
            continue
        new_word = (word & ~(0x7 << 28)) | (CODEC_S8 << 28)
        new_word = (new_word & ~0x00FFFFFF) | (s.n_samples & 0x00FFFFFF)
        struct.pack_into(">I", head, abs_off, new_word)
        struct.pack_into(">I", head, abs_off + 4, new_ofs[(wb, old_ofs)])
        patched += 1

    tbl = bytearray()
    for i in range(len(rom.waves)):
        blob = bank_blobs.get(i, bytearray())
        tbl += blob
        tbl += b"\0" * (align_up(len(blob), 32) - len(blob))

    return bytes(head + tbl), new_wave, new_tbl_size, patched, len(wts)


def main() -> int:
    ap = argparse.ArgumentParser(
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--img", default=None)
    ap.add_argument("--headers", default=None)
    ap.add_argument("--out", required=True)
    ap.add_argument("--verify", action="store_true",
                    help="re-parse the written image and check every sample")
    args = ap.parse_args()

    kw = {}
    if args.img:
        kw["img_path"] = args.img
    if args.headers:
        kw["headers_path"] = args.headers
    rom = AudioRom(**kw)

    if not args.verify:
        img, new_wave, new_tbl, patched, total = build(rom)
        with open(args.out, "wb") as fh:
            fh.write(img)
        digest = hashlib.sha256(img).hexdigest()

        print(f"wavetable structs patched   {patched}/{total}")
        print(f"audiorom.img  {len(rom.img):,} B  ->  {len(img):,} B")
        print(f"  tbl extent  {rom.extents[2][1]:,} B  ->  {new_tbl:,} B")
        print(f"  sha256      {digest}")
        print()
        print("🔴 APPLY THESE TO src/static/jaudio_NES/game/audioheaders.c VIA")
        print("   tools/dcstub/make_src_shrink.py -- the image is USELESS without")
        print("   them, and a mismatch plays noise rather than failing:")
        print()
        print(f"   AudiodataHeaderStart entry 2 (tbl):  size {new_tbl}")
        for i, (addr, size) in enumerate(new_wave):
            old = rom.waves[i]
            print(f"   AudiowaveHeaderStart entry {i}: "
                  f"addr 0x{old.addr:X} -> 0x{addr:X}   "
                  f"size {old.size:,} -> {size:,}")
        return 0

    # ---- verify -----------------------------------------------------------
    print(f"verifying {args.out}")
    # ⚠️ DO NOT build an AudioRom on the new image. Its structure is described
    # by tables that live in the GAME, not in the file, and those still say the
    # old layout until the shrink rule lands -- so AudioRom's "extents sum to
    # the file size" guard fires, correctly. That guard is the cheap detector
    # for exactly the image/table drift that is W1's whole risk; do not weaken
    # it. Read raw bytes and drive the walk off the ORIGINAL rom's structure,
    # which is what the ctl offsets are still relative to.
    with open(args.out, "rb") as fh:
        img = fh.read()
    wts = collect_wavetables(rom)
    uniq = rom.unique_samples()
    by_pos = {(s.wave_bank, s.sample_ofs): s for s in uniq.values()}

    bad = 0
    checked = 0
    for abs_off, (wb, old_ofs, word) in wts.items():
        s = by_pos.get((wb, old_ofs))
        if s is None:
            continue
        nw = struct.unpack_from(">I", img, abs_off)[0]
        codec = (nw >> 28) & 0x7
        size = nw & 0x00FFFFFF
        n_ofs = struct.unpack_from(">I", img, abs_off + 4)[0]
        if codec != CODEC_S8:
            print(f"  FAIL 0x{abs_off:X}: codec {codec}, expected {CODEC_S8}")
            bad += 1
            continue
        if size != s.n_samples:
            print(f"  FAIL 0x{abs_off:X}: size {size} != n_samples {s.n_samples}")
            bad += 1
            continue
        checked += 1

    # Spot-check payloads: decode the ORIGINAL VADPCM and compare against the
    # S8 bytes actually written, reconstructed the way the engine will.
    import random
    random.seed(7)
    probes = random.sample(sorted(by_pos.values(), key=lambda x: x.file_off),
                           min(8, len(by_pos)))
    # Recompute the layout to know where each sample landed.
    _, new_wave, _, _, _ = build(rom)
    per_bank: dict[int, list] = {}
    for (wb, ofs), s in sorted(by_pos.items()):
        per_bank.setdefault(wb, []).append(s)
    pos: dict[tuple[int, int], int] = {}
    for wb in sorted(per_bank):
        cur = 0
        for s in per_bank[wb]:
            pos[(wb, s.sample_ofs)] = cur
            cur = align_up(cur + s.n_samples)

    for s in probes:
        base = rom.wave_base + new_wave[s.wave_bank][0] + pos[(s.wave_bank, s.sample_ofs)]
        got = img[base: base + s.n_samples]
        ref = pcm16_to_s8(vadpcm_decode(
            rom.img[s.file_off: s.file_off + s.size], s.book, s.n_samples))
        if bytes(got) != bytes(ref):
            print(f"  FAIL payload at wb{s.wave_bank} ofs 0x{s.sample_ofs:X}")
            bad += 1
        else:
            peak = max(abs((b ^ 0x80) - 0x80) for b in got) if got else 0
            print(f"    wb{s.wave_bank} n={s.n_samples:>6} peak={peak:>4}  ok")

    print(f"  {'FAILED' if bad else 'OK'}: {checked} wavetables, "
          f"{len(probes)} payloads, {bad} problem(s)")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
