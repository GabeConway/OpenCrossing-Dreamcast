#!/usr/bin/env python3
"""Prove the arithmetic bounds of the VADPCM filter over the WHOLE shipped bank.

    python3 tools/dcaudio/bounds.py

`kb/RESUME.md` §6c blocks the `MAC.W` optimisation of the ADPCM chain with:

    "Do NOT extend it to the ADPCM chain without a proven bound:
     `sp9C[l] << temp_r19` with `temp_r19 in 0..15` can exceed s16."

That is correct as a statement about the ENCODING — the scale nibble is four
bits, so 0..15 is what the format admits, and `-8 << 15` is far outside s16.
It is a statement about the format, not about this game's data, and nobody
could check the data because nothing could read the bank.

This checks the data.  It walks every frame of every sample, reproducing the
exact filter from `rspsim.c`'s `TARGET_PC` arm, and reports the true ranges of
the two quantities that decide whether `MAC.W` is legal:

  * the **operands** — `sp7C`/`sp5C`, the scaled nibbles, and the codebook
    coefficients.  `MAC.W` multiplies s16 x s16, so both must fit s16.
  * the **accumulator** — SH-4's `MAC.W` accumulates into the 64-bit MACH:MACL
    pair, so it cannot overflow; but the C reads a 32-bit result, so the two
    agree only where the C itself does not wrap.

⚠️ THIS IS A BOUND ON THIS GAME'S BANK, NOT A BOUND ON THE FORMAT.  It is valid
because the bank is fixed ROM data that ships on the disc and cannot change.
Re-run it if `audiorom.img` is ever regenerated or replaced, and do not carry
the conclusion to another game.

⚠️ AND IT DOES NOT SAY GCC WILL EMIT `MAC.W`.  It says doing so would be
correct.  Whether the compiler does, and whether it helps, is a separate
question that only a hardware run answers.
"""

from __future__ import annotations

import os
import sys

try:
    from .audiorom import AudioRom
    from .vadpcm import AD4, _w32, _clamp_s16
except ImportError:
    sys.path.insert(0, os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    from dcaudio.audiorom import AudioRom
    from dcaudio.vadpcm import AD4, _w32, _clamp_s16

S16_MIN, S16_MAX = -32768, 32767
S32_MIN, S32_MAX = -(2 ** 31), 2 ** 31 - 1


def main() -> int:
    rom = AudioRom()
    samples = list(rom.unique_samples().values())

    scale_hist = {}
    op_lo = op_hi = 0
    coef_lo = coef_hi = 0
    acc_lo = acc_hi = 0
    wraps = 0
    n_frames = 0

    for s in samples:
        blob = rom.img[s.file_off: s.file_off + s.size]
        for row in s.book:
            for c in row:
                coef_lo = min(coef_lo, c)
                coef_hi = max(coef_hi, c)

        h1 = h2 = 0
        for f in range(len(blob) // 9):
            off = f * 9
            hdr = blob[off]
            sh = hdr >> 4
            coef = s.book[hdr & 0xF]
            scale_hist[sh] = scale_hist.get(sh, 0) + 1
            n_frames += 1

            raw = []
            for k in range(8):
                b = blob[off + 1 + k]
                raw.append(AD4[(b >> 4) & 0xF])
                raw.append(AD4[b & 0xF])

            for half in range(2):
                base = half * 8
                scaled = [raw[base + k] << sh for k in range(8)]
                for v in scaled:
                    op_lo = min(op_lo, v)
                    op_hi = max(op_hi, v)
                for k in range(8):
                    accu = (scaled[k] << 11) + h1 * coef[k + 8] + h2 * coef[k]
                    for l in range(k):
                        accu += scaled[l] * coef[k - l + 7]
                    acc_lo = min(acc_lo, accu)
                    acc_hi = max(acc_hi, accu)
                    if accu != _w32(accu):
                        wraps += 1
                    raw[base + k] = _clamp_s16(_w32(accu) >> 11)
                h1 = raw[base + 7]
                h2 = raw[base + 6]

    ops_fit = op_lo >= S16_MIN and op_hi <= S16_MAX
    coefs_fit = coef_lo >= S16_MIN and coef_hi <= S16_MAX
    acc_fits = acc_lo >= S32_MIN and acc_hi <= S32_MAX

    print(f"samples {len(samples)}   frames {n_frames:,}")
    print()
    print("scale nibble (header >> 4):")
    for k in sorted(scale_hist):
        print(f"   {k:>2}: {scale_hist[k]:>9,}  {scale_hist[k]/n_frames*100:5.2f}%")
    hi = max(scale_hist)
    print(f"   MAX OBSERVED {hi}  (the format admits 15; -8 << 13 would "
          f"already leave s16)")
    print()
    print(f"MAC.W operand A, scaled nibbles   {op_lo:>8,} .. {op_hi:>8,}"
          f"   fits s16: {ops_fit}")
    print(f"MAC.W operand B, codebook coefs   {coef_lo:>8,} .. {coef_hi:>8,}"
          f"   fits s16: {coefs_fit}")
    print(f"accumulator                    {acc_lo:>11,} .. {acc_hi:>11,}"
          f"   fits s32: {acc_fits}")
    print(f"   headroom to s32: {S32_MAX // max(abs(acc_lo), abs(acc_hi), 1)}x")
    print(f"   frames where the C wraps: {wraps}")
    print()
    ok = ops_fit and coefs_fit and acc_fits and wraps == 0
    if ok:
        print("VERDICT: MAC.W is SAFE for this bank. Both operands fit s16, the")
        print("         accumulator fits s32 with margin, and the C never wraps,")
        print("         so a 64-bit MAC read back as s32 is bit-identical.")
        print("         This bounds the DATA, not the FORMAT -- re-run if")
        print("         audiorom.img is ever replaced.")
    else:
        print("VERDICT: NOT SAFE -- see the failing line above.")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
