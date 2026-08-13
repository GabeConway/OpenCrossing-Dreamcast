"""Yamaha/AICA 4-bit ADPCM — the codec the hardware channels actually speak.

The reference is KOS's `utils/wav2adpcm/wav2adpcm.c`, whose own header records
that AICA ADPCM is YMZ280B ADPCM with the nibbles swapped, and whose codec is
superctr's public-domain `ymz_codec.c`.  This module reimplements the decoder
exactly and replaces the encoder with something better (see below).

STATE, and why it is the whole story for a looping sample:

    history   = 0     the predictor
    step_size = 127   the quantiser scale

    per 4-bit code:
        sign  = code & 8 ; delta = code & 7
        diff  = clamp(((1 + (delta << 1)) * step_size) >> 3, 0, 32767)
        nstep = (step_table[delta] * step_size) >> 8
        history   = clamp(history + (-diff if sign else +diff), -32768, 32767)
        step_size = clamp(nstep, 127, 24576)

There is NO block structure — no headers, no periodic resets, no alignment
beyond nibble packing.  State runs from the first sample to the last.  That is
precisely why a loop point is dangerous: the decoder arrives at LSA carrying
whatever state LEA left it with.

⚠️ NIBBLE ORDER IS LOW-FIRST, AND KOS GOT THIS WRONG UNTIL RECENTLY.  Sample 0
goes in the low nibble of byte 0, sample 1 in the high nibble.  Older
`wav2adpcm` output has them swapped, which shifts every sample by one and drops
the last.  Any pre-existing `.adpcm` asset is suspect.

⚠️ KOS'S ENCODER AND DECODER DISAGREE, AND NOBODY KNOWS WHICH MATCHES SILICON.
`adpcm2pcm` applies a leaky high-pass `history = history * 254 / 256` before
every step; `pcm2adpcm` does not model it.  Only one of those can be the
hardware.  This module makes the leak a parameter rather than picking silently:

    leak=0    no leak — matches KOS's ENCODER, and therefore matches every
              piece of DC ADPCM content that has ever shipped and sounded
              correct.  This is the default, on the grounds that shipping
              content is evidence and an unused host-side decode path is not.
    leak=254  the leak KOS's own decoder applies.

  🔴 UNRESOLVED, AND ONLY A BURN CAN RESOLVE IT.  The two differ audibly only
  on sustained near-DC content — which is exactly what a held instrument loop
  is.  `kb/RESUME.md` rule 12's logic applies with full force: an emulator
  agreeing with either hypothesis is evidence about the emulator, because
  Flycast's ADPCM is a reimplementation and not the silicon.

ENCODER: two are provided.  `exhaustive=True` tries all 16 codes per sample and
keeps the lowest squared error; `exhaustive=False` reproduces KOS's open-loop
quantiser (wav2adpcm.c:115-134).  Both are closed-loop in the sense that they
step the real decoder, so the state they propagate is the state the hardware
will be in.

⚠️ MEASURED, AND IT CONTRADICTS THE OBVIOUS EXPECTATION: the exhaustive search
is a WASH, not an improvement.  Over five real bank samples it scored 22.09 /
14.01 / 15.96 / 28.25 / 23.46 dB against the open-loop encoder's 22.07 / 13.74
/ 16.08 / 28.01 / 23.36 — inside ±0.3 dB, and it LOST on one.  The reason is
that a per-sample greedy search is myopic: the code that minimises this
sample's error also sets `step_size` for the next one, and the arithmetic
quantiser happens to track the signal envelope about as well.  Beating it needs
a search with lookahead (Viterbi over the step-size state), which is a real
project and is NOT justified until something shows 4-bit ADPCM quality is the
thing hurting this port.  Do not re-propose "just search harder" without that
evidence.
"""

from __future__ import annotations

from typing import List, Optional, Sequence, Tuple

# wav2adpcm.c:65 — indexed by MAGNITUDE alone (0..7), not by the full code.
STEP_TABLE: List[int] = [230, 230, 230, 230, 307, 409, 512, 614]

STEP_MIN = 127
STEP_MAX = 24576

# aica_comm.h:156-159. Mode 2 is what KOS uses for one-shot sound effects;
# mode 3 ("long stream") is what its looping ring-buffer streamer uses, which
# is the only evidence available that mode 3 carries decoder state across the
# loop point. See the module docstring and kb/audio-aica-offload.md.
AICA_SM_16BIT = 0
AICA_SM_8BIT = 1
AICA_SM_ADPCM = 2
AICA_SM_ADPCM_LS = 3

# LSA/LEA are 16-bit and hold SAMPLE counts for every format (snd_stream.c:
# 125-147). KOS clamps to 65534 (snd_sfxmgr.c:766) and backs its own ADPCM
# stream ceiling off by a further 128 samples for reasons it does not record.
CHANNEL_MAX_SAMPLES = 65534


def _clamp(v: int, lo: int, hi: int) -> int:
    return lo if v < lo else (hi if v > hi else v)


class AdpcmState:
    """The decoder's two registers, exposed so a loop's ends can be compared."""

    __slots__ = ("history", "step_size")

    def __init__(self, history: int = 0, step_size: int = STEP_MIN):
        self.history = history
        self.step_size = step_size

    def copy(self) -> "AdpcmState":
        return AdpcmState(self.history, self.step_size)

    def __eq__(self, other: object) -> bool:
        return (
            isinstance(other, AdpcmState)
            and self.history == other.history
            and self.step_size == other.step_size
        )

    def __repr__(self) -> str:
        return f"AdpcmState(history={self.history}, step={self.step_size})"


def step(code: int, st: AdpcmState, leak: int = 0) -> int:
    """Advance the decoder by one 4-bit code; return the emitted sample.

    `leak` of 254 applies KOS's decoder-side `history * 254 / 256` before the
    step; 0 disables it.  See the module docstring — this is the unresolved
    encoder/decoder asymmetry, not a tuning knob.
    """
    if leak:
        # C integer division truncates toward zero; Python's // floors. On a
        # negative history those differ by one, and this runs on every sample.
        h = st.history * leak
        st.history = int(h / 256)

    sign = code & 8
    delta = code & 7

    diff = _clamp(((1 + (delta << 1)) * st.step_size) >> 3, 0, 32767)
    nstep = (STEP_TABLE[delta] * st.step_size) >> 8

    st.history = _clamp(
        st.history - diff if sign else st.history + diff, -32768, 32767
    )
    st.step_size = _clamp(nstep, STEP_MIN, STEP_MAX)
    return st.history


def decode(data: bytes, n_samples: int, leak: int = 0,
           state: Optional[AdpcmState] = None) -> List[int]:
    """Decode packed AICA ADPCM to PCM16. Low nibble first."""
    st = state.copy() if state else AdpcmState()
    out: List[int] = []
    for i in range(n_samples):
        b = data[i >> 1]
        code = (b & 0x0F) if (i & 1) == 0 else (b >> 4)
        out.append(step(code, st, leak))
    return out


def encode(
    pcm: Sequence[int],
    leak: int = 0,
    state: Optional[AdpcmState] = None,
    exhaustive: bool = True,
) -> Tuple[bytearray, AdpcmState]:
    """Encode PCM16 to packed AICA ADPCM.

    Returns (packed bytes, the decoder state after the last sample).  That
    trailing state is what a loop analysis needs, so it is returned rather than
    discarded.

    `exhaustive=False` reproduces KOS's open-loop quantiser (wav2adpcm.c:
    115-134) instead, for A/B.
    """
    st = state.copy() if state else AdpcmState()
    out = bytearray((len(pcm) + 1) // 2)

    for i, target in enumerate(pcm):
        if exhaustive:
            best_code = 0
            best_err = None
            for code in range(16):
                probe = st.copy()
                got = step(code, probe, leak)
                err = (got - target) ** 2
                if best_err is None or err < best_err:
                    best_err = err
                    best_code = code
            code = best_code
        else:
            # wav2adpcm.c:115-121, the open-loop path.
            delta = (target & ~7) - st.history
            code = _clamp((abs(delta) << 16) // (st.step_size << 14), 0, 7)
            if delta < 0:
                code |= 8

        step(code, st, leak)

        if (i & 1) == 0:
            out[i >> 1] = code & 0x0F
        else:
            out[i >> 1] |= (code & 0x0F) << 4

    return out, st


def snr_db(reference: Sequence[int], got: Sequence[int]) -> float:
    """Signal-to-noise ratio of a round trip, in dB. Higher is better."""
    import math

    n = min(len(reference), len(got))
    sig = 0
    err = 0
    for i in range(n):
        sig += reference[i] * reference[i]
        d = reference[i] - got[i]
        err += d * d
    if err == 0:
        return float("inf")
    if sig == 0:
        return float("-inf")
    return 10.0 * math.log10(sig / err)


def analyse_loop(
    pcm: Sequence[int],
    loop_start: int,
    loop_end: int,
    leak: int = 0,
) -> dict:
    """Measure how badly a loop's decoder state fails to converge.

    THE QUESTION THIS ANSWERS is `kb/audio-plan-of-record.md` §7 open item 5 —
    "convert the 451 looping samples, listen, and count".  Listening is still
    required for the verdict, but this bounds the search: a sample whose state
    at `loop_end` already matches its state at `loop_start` cannot click, no
    matter what the hardware does at the loop point, because both the
    carry-state and reset-state hypotheses give the same decoder.

    Returns the two states and the audible consequence, expressed as the
    first-sample error the mismatch would cause on the second pass.
    """
    packed, _ = encode(pcm, leak=leak)

    # Walk to loop_start, remembering the state, then on to loop_end.
    st = AdpcmState()
    at_start: Optional[AdpcmState] = None
    n = min(len(pcm), loop_end if loop_end else len(pcm))
    for i in range(n):
        if i == loop_start:
            at_start = st.copy()
        b = packed[i >> 1]
        code = (b & 0x0F) if (i & 1) == 0 else (b >> 4)
        step(code, st, leak)

    if at_start is None:
        at_start = AdpcmState()

    d_hist = st.history - at_start.history
    d_step = st.step_size - at_start.step_size

    return {
        "state_at_loop_start": at_start,
        "state_at_loop_end": st,
        "history_delta": d_hist,
        "step_delta": d_step,
        # A rough audibility proxy: the predictor jump is a step discontinuity
        # in the output, so express it against full scale.
        "history_delta_dbfs": (
            float("-inf") if d_hist == 0
            else 20.0 * __import__("math").log10(abs(d_hist) / 32768.0)
        ),
        "converged": d_hist == 0 and d_step == 0,
    }
