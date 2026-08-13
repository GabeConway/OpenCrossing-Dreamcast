"""VADPCM decoder — the reference half of the AICA offload toolchain.

This is a *transliteration* of the decoder the game itself runs, not a
reimplementation from a spec.  The authority is `A_CMD_ADPCM` in
`src/static/jaudio_NES/internal/rspsim.c` (the `case` opens at rspsim.c:107),
and specifically its `#ifdef TARGET_PC` arm — the port builds with
`-DTARGET_PC`, so that arm is the code that actually produces the samples a
human hears today.  Anything this file emits that differs from that arm is a
bug in this file.

Why a host-side decoder exists at all: stage B moves synthesis onto the AICA's
hardware channels, which speak Yamaha 4-bit ADPCM and nothing else.  Getting
there is decode-then-reencode, so the decoder is the fixed point that the
re-encoder is scored against.  It is also the only way to answer, with data
rather than arithmetic, the questions `kb/audio-engine.md` §3.5 leaves open
(how long each sample really is, which ones exceed a hardware channel, what a
sequence's working set really costs).

TWO PRECISION TRAPS, both load-bearing:

  * The `TARGET_PC` arm accumulates every filter term at full width and applies
    a single arithmetic `>> 11`.  The GameCube arm divides each term by 0x800
    separately, which truncates toward zero per term.  These give different
    samples.  rspsim.c:151-153 records the choice and the reason.  Python's `>>`
    on a negative int floors, which is what an arithmetic shift does, so the
    accumulator must stay a plain Python int and be shifted, never divided.

  * `sp9C` is `s16` but `sp7C`/`sp5C` are `s32` (rspsim.c:85-87).  The shifted
    nibbles therefore do NOT wrap at 16 bits even though the nibble that seeded
    them did.  `kb/RESUME.md` flags the same asymmetry as the reason the
    `MAC.W` idea must not be extended into the ADPCM chain.

Frame layout, from the two branches at rspsim.c:133-147:

  4-bit ("CODEC_ADPCM", `flags & 4` clear)  9 bytes -> 16 samples
      byte 0      scale in the high nibble, predictor index in the low nibble
      bytes 1..8  16 nibbles, high nibble first, each a signed 4-bit value
  2-bit ("CODEC_SMALL_ADPCM", `flags & 4` set)  5 bytes -> 16 samples
      byte 0      as above
      bytes 1..4  16 crumbs, most significant pair first, signed 2-bit

`kb/audio-engine.md` §3.3 reports all 1157 samples in this game are the 4-bit
codec with order 2 and 2 predictors, so the 2-bit path here is for completeness
and is exercised by the self-test rather than by the bank.
"""

from __future__ import annotations

from typing import List, Sequence

# rspsim.c:37-40 and :35.  Both tables are plain sign-extension of an n-bit
# field; they are kept as tables rather than as arithmetic so that a diff
# against the C stays literal.
AD4: List[int] = [0, 1, 2, 3, 4, 5, 6, 7, -8, -7, -6, -5, -4, -3, -2, -1]
AD2: List[int] = [0, 1, -2, -1]

# Samples per encoded frame.  Called ADPCMFSIZE in the engine and reused as
# SAMPLES_PER_FRAME by include/jaudio_NES/driver.h:12.
FRAME_SAMPLES = 16

CODEC_ADPCM = 0
CODEC_SMALL_ADPCM = 3


def frame_bytes(is_small: bool) -> int:
    """Encoded size of one 16-sample frame."""
    return 5 if is_small else 9


def _w32(v: int) -> int:
    """Wrap to two's-complement s32.

    `accu`, `sp7C` and `sp5C` are all `s32` in rspsim.c:85-87 and :155.  A
    large scale nibble times a large coefficient can leave that range: the
    shift is 0..15 (rspsim.c:131 reads it out of a byte's high nibble) and a
    codebook entry is a full s16, so `(nibble << 15) * 0x7FFF` is ~8.6e9.
    Python ints are unbounded and would silently disagree with the console.

    Signed overflow is UB in C, but the shipped code is what it is: GCC on SH-4
    wraps two's complement, so wrapping is what the player hears.  This is only
    reachable at extreme scale/coefficient combinations; `kb/RESUME.md` flags
    the same range problem as the reason the `MAC.W` idea must not be pushed
    into the ADPCM chain without a proven bound.
    """
    return ((v + 0x80000000) & 0xFFFFFFFF) - 0x80000000


def _clamp_s16(v: int) -> int:
    if v > 0x7FFF:
        return 0x7FFF
    if v < -0x8000:
        return -0x8000
    return v


def decode_frame(
    frame: Sequence[int],
    book: Sequence[Sequence[int]],
    hist_m1: int,
    hist_m2: int,
    is_small: bool = False,
) -> tuple[List[int], int, int]:
    """Decode one frame to 16 s16 samples.

    `book` is the predictor table as `A_CMD_LOADADPCM` leaves it in
    `ADPCM_BOOKBUF` (rspsim.c:342): a list indexed by predictor, each entry 16
    s16.  Entries 0..7 are the order-2 coefficient row and 8..15 the order-1
    row — that is how rspsim.c:156-157 indexes it (`temp_r16[k]` against the
    older sample, `temp_r16[k + 8]` against the newer one).

    `hist_m1` / `hist_m2` are the previous two output samples, newest first.
    They are rspsim's `var_r5` and `var_r0`, which it seeds from the 16-sample
    history block at `DMEMOut` (rspsim.c:120-122) and then carries forward.

    Returns (samples, new_hist_m1, new_hist_m2).
    """
    header = frame[0]
    shift = header >> 4          # rspsim.c:131 `temp_r19`
    predictor = header & 0xF     # rspsim.c:132
    coef = book[predictor]

    # --- unpack the 16 signed nibbles/crumbs (rspsim.c:133-147) -------------
    raw: List[int] = [0] * 16
    if is_small:
        for k in range(4):
            b = frame[1 + k]
            raw[k * 4 + 0] = AD2[(b >> 6) & 3]
            raw[k * 4 + 1] = AD2[(b >> 4) & 3]
            raw[k * 4 + 2] = AD2[(b >> 2) & 3]
            raw[k * 4 + 3] = AD2[(b >> 0) & 3]
    else:
        for k in range(8):
            b = frame[1 + k]
            raw[k * 2 + 0] = AD4[(b >> 4) & 0xF]
            raw[k * 2 + 1] = AD4[(b >> 0) & 0xF]

    out: List[int] = list(raw)

    # --- the two half-frames (rspsim.c:148-206) -----------------------------
    # Both halves run the identical filter; they differ only in which slice of
    # `raw` seeds them and in that the second half sees the first half's
    # history.  The scaled nibbles (`sp7C` / `sp5C`) stay s32 and are what the
    # inner sum reads back — NOT the decoded output.
    for half in range(2):
        base = half * 8
        scaled = [_w32(raw[base + k] << shift) for k in range(8)]
        for k in range(8):
            accu = _w32(scaled[k] << 11)
            accu = _w32(accu + hist_m1 * coef[k + 8])
            accu = _w32(accu + hist_m2 * coef[k])
            for l in range(k):
                accu = _w32(accu + scaled[l] * coef[k - l + 7])
            out[base + k] = _clamp_s16(accu >> 11)
        hist_m1 = out[base + 7]
        hist_m2 = out[base + 6]

    return out, hist_m1, hist_m2


def decode(
    data: bytes,
    book: Sequence[Sequence[int]],
    n_samples: int,
    is_small: bool = False,
    initial_state: Sequence[int] | None = None,
) -> List[int]:
    """Decode `n_samples` samples from a whole VADPCM stream.

    `initial_state` is the 16-entry `predictor_state` an `adpcmloop` carries
    (audiostruct.h:109).  rspsim only ever reads its last two entries — see the
    `flags & 2` branch at rspsim.c:112-114, which copies the block wholesale
    and then picks up `var_r17[-1]` / `var_r17[-2]`.  Passing None is the
    `flags & 1` "clear history" case (rspsim.c:109-111), i.e. a cold start.
    """
    fsz = frame_bytes(is_small)
    if initial_state is None:
        hist_m1 = hist_m2 = 0
    else:
        hist_m1 = initial_state[15]
        hist_m2 = initial_state[14]

    out: List[int] = []
    n_frames = (n_samples + FRAME_SAMPLES - 1) // FRAME_SAMPLES
    for f in range(n_frames):
        off = f * fsz
        frame = data[off:off + fsz]
        if len(frame) < fsz:
            # A truncated tail is a real condition in this bank: `size` in the
            # smzwavetable bitfield is a byte count that need not be a whole
            # number of frames.  Pad rather than raise, and let the caller
            # decide whether the shortfall matters.
            frame = bytes(frame) + bytes(fsz - len(frame))
        samples, hist_m1, hist_m2 = decode_frame(
            frame, book, hist_m1, hist_m2, is_small
        )
        out.extend(samples)

    return out[:n_samples]


def parse_book(raw: bytes, order: int, n_predictors: int) -> List[List[int]]:
    """Split an `adpcmbook` codebook blob into per-predictor coefficient rows.

    audiostruct.h:113-117 gives the length as `order * n_predictors * 8` s16.
    `A_CMD_LOADADPCM` (rspsim.c:342) copies that blob straight into
    `ADPCM_BOOKBUF[8][16]`, so for the order-2 case each predictor owns 16
    consecutive s16 — the order-2 row then the order-1 row.
    """
    import struct

    total = order * n_predictors * 8
    if len(raw) < total * 2:
        raise ValueError(
            f"codebook blob is {len(raw)} B, need {total * 2} B "
            f"for order={order} n_predictors={n_predictors}"
        )
    flat = list(struct.unpack(f">{total}h", raw[: total * 2]))
    per = order * 8
    return [flat[p * per:(p + 1) * per] for p in range(n_predictors)]
