#!/usr/bin/env python3
"""Differential test: `tools/dcaudio/vadpcm.py` against the shipped C decoder.

    python3 tools/dcaudio/tests/test_vadpcm.py

The oracle is `ref_adpcm.c`, whose frame loop is lifted verbatim out of
`A_CMD_ADPCM` in `src/static/jaudio_NES/internal/rspsim.c` (TARGET_PC arm, the
one this port compiles).  Nothing here re-derives the codec from a spec; the
whole point is that the host tool and the console agree sample for sample,
because the re-encoder in the AICA offload is scored against this decoder's
output and an error here would be invisible and would sound like a bad encoder.

⚠️ The oracle MUST be built `-fwrapv`.  `accu` is `s32` and a large scale
nibble times a full-width codebook entry leaves the range; signed overflow is
UB, so without `-fwrapv` the compiler is free to do something other than the
two's-complement wrap the SH-4 actually performs, and the test would be
measuring the host compiler instead of the console.  `vadpcm._w32()` is the
Python side of the same contract.

Coverage note: this drives the full 0..15 scale range and s16-wide
coefficients, so the s32 wrap and both `_clamp_s16` arms are exercised.  Real
bank data is much tamer (order 2, 2 predictors), so passing here is a stronger
statement than passing on the bank.  The predictor index is held to 0..7
because `ADPCM_BOOKBUF` is `[8][16]` (rspsim.c:10) and the C would read out of
bounds above that — the real bank never exceeds 1.
"""

import os
import random
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(os.path.dirname(HERE)))

from dcaudio.vadpcm import decode_frame  # noqa: E402

TRIALS = 200
FRAMES = 24


def build_oracle() -> str:
    src = os.path.join(HERE, "ref_adpcm.c")
    exe = os.path.join(HERE, "ref_adpcm")
    if not os.path.exists(exe) or os.path.getmtime(exe) < os.path.getmtime(src):
        subprocess.run(
            ["cc", "-O2", "-fwrapv", "-o", exe, src], check=True
        )
    return exe


def main() -> int:
    exe = build_oracle()
    random.seed(0xACDC)
    bad = 0

    for t in range(TRIALS):
        frames = []
        for _ in range(FRAMES):
            hdr = (random.randrange(16) << 4) | random.randrange(8)
            frames.append(
                bytes([hdr]) + bytes(random.randrange(256) for _ in range(8))
            )
        book = [
            [random.randrange(-0x8000, 0x8000) for _ in range(16)]
            for _ in range(8)
        ]

        inp = [str(FRAMES)]
        for f in frames:
            inp.extend(str(b) for b in f)
        for row in book:
            inp.extend(str(c) for c in row)

        ref = [
            int(x)
            for x in subprocess.run(
                [exe], input="\n".join(inp), capture_output=True,
                text=True, check=True,
            ).stdout.split()
        ]

        got = []
        h1 = h2 = 0
        for f in frames:
            s, h1, h2 = decode_frame(f, book, h1, h2, is_small=False)
            got.extend(s)

        if got != ref:
            bad += 1
            if bad == 1:
                for i, (a, b) in enumerate(zip(got, ref)):
                    if a != b:
                        print(
                            f"trial {t}: first mismatch at sample {i}: "
                            f"py={a} c={b}"
                        )
                        break

    total = TRIALS * FRAMES * 16
    print(
        f"{TRIALS - bad}/{TRIALS} trials bit-exact "
        f"({total:,} samples compared)"
    )
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
