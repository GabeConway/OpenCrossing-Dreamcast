# Two audio CPU wins that do NOT need the AICA offload

**Written 2026-08-13**, from the hardware gprof runs in `dc/build/gprof-runs`
and from `tools/dcaudio/`, which can read the bank for the first time.

`kb/audio-aica-offload.md` is the big swing: move synthesis onto AICA's
hardware channels, delete `RspStart`, ~14 ms of a ~76 ms frame. It is also
several sessions, it needs a burn to resolve the loop-state question, it drops
reverb, and it puts at risk the one subsystem a human currently reports as
**good** on hardware.

**These two are not that.** Both attack the same `RspStart` cost, both are much
smaller, and **W1 needs no runtime code at all.**

---

## The denominator, re-derived from the profile rather than quoted

`dc/build/gprof-runs/hw-title-1958f.flat.txt`, 1,889 presented frames:

```
busy (non-idle, non-thd_block_now)   144.4 s   = 76.4 ms/frame
RspStart                              27.27 s  = 18.9 % of busy = 14.4 ms/frame
```

⚠️ **`RspStart`'s internal split is a kb figure, not a measured one here.**
`kb/RESUME.md` §6c attributes it through gprof's 8-byte bins as ADPCM decode
29 %, ENVMIXER 26 %, RESAMPLE 21 %, `Jac_Resample16` 11 %, MIXER 11 %. That is
source-line attribution through a histogram, so treat the sub-shares as
**medium confidence** and the 18.9 % total as solid.

---

## ⭐ W1 — SWITCH THE BANK TO `CODEC_S8` AND THE ADPCM DECODER STOPS RUNNING

**~29 % of `RspStart` ≈ 5.5 % of busy ≈ 4.2 ms/frame**, for an offline data
conversion and **zero lines of runtime code**.

### Why this is possible at all

`CODEC_S8` is not a stub or a dead branch — it is a complete, first-class path
in the shipped engine, and the codec is selected by a **bitfield in the ROM
data**, not by code:

| what | where |
|---|---|
| codec lives in the data | `smzwavetable` bits 30-28 (`audiostruct.h:119-143`) |
| codebook load is SKIPPED for non-ADPCM | `driver.c:815` — the `aLoadADPCM` block is gated on `CODEC_ADPCM \|\| CODEC_SMALL_ADPCM` |
| S8 frame geometry | `driver.c:907` — `frameSize = 16`, `skipInitialSamples = 16`, `zeroOffset = 0` |
| S8 command emission | `driver.c:1043` — `Nas_SetBuffer` + `Nas_PCM8dec` |
| the decoder itself | `rspsim.c:591` — `A_CMD_PCM8DEC` is `for (...) *dst++ = (*src++) << 8;` |

Compare that one load-shift-store per sample against `A_CMD_ADPCM`
(`rspsim.c:107`): per 16 samples it unpacks 16 nibbles and runs 16 filter
dot-products of up to 10 s16 multiplies each — order **10 MACs per sample**
versus **one shift**. Plus the codebook `aLoadADPCM` traffic, which disappears
entirely.

⭐ **AND IT RESPECTS THE HARD RULE.** `src/` is never edited: this is a change
to `audiorom.img`, which is data. The only code-side change is the index
tables, and those go through `tools/dcstub/make_src_shrink.py` — the same
mechanism P3 used.

### The cost, measured

```
VADPCM tbl payload      6,734,836 B
as 8-bit PCM           11,962,409 B     = 1.78x
audiorom.img now        8,300,384 B  ->  13,237,161 B   (+4,936,777)
```

Disc is not a constraint — the CDI is padded to 740 MB for a burn anyway.

**The residency question is the real one, and it answers well:**

```
per-sequence S8 working set: median 101,009 B   mean 149,189 B
246 of 249 sequences fit inside the EXISTING 1,048,576 B DC_ARAM_WINDOW
```

Only 3 sequences exceed it, and they are the same bank-153 offenders that
break every other audio plan (`kb/audio-aica-offload.md` §4). Samples reach the
engine through the disc-backed GC-ARAM LRU window (`dc_aram.c`), so 1.78× the
bytes means more disc traffic — but the window was measured at **106 reads per
420 s** after it grew to 1 MB (`kb/RESUME.md` §5), so there is headroom.

⚠️ **The sample lengths behind these figures were corrected the same day** —
`n_samples` must come from `adpcmloop.loop_end`, not `sample_end`, because
`driver.c:785-789`'s `sample_end` arm is dead code in this bank
(`kb/audio-aica-offload.md` §3). It moved the totals ~0.1 % and changed nothing.

### Quality — it goes UP, not down

8-bit linear PCM is ~48 dB peak SNR. Round-trip SNR of the *existing* 4-bit
VADPCM, measured on real bank samples, is **14–44 dB**
(`kb/audio-aica-offload.md` §7, §10). So S8 is comparable-to-better on this
material.
⚠️ **The noise CHARACTER differs**, and that is the honest risk: ADPCM's
quantisation noise is signal-proportional, 8-bit PCM's is constant, so quiet
sustained passages can hiss where they did not. **This needs ears, not an SNR
table.** It is also trivially A/B-able, because the conversion is per sample —
a mixed bank (some samples S8, some left ADPCM) is legal and needs no extra
machinery, since the codec is per `smzwavetable`.

### ✅ The converter is BUILT and self-verifies — `tools/dcaudio/s8bank.py`

```
wavetable structs patched   2147/2147
audiorom.img  8,300,384 B  ->  13,244,928 B
  tbl extent  7,025,632 B  ->  11,970,176 B
--verify:  OK: 2147 wavetables, 8 payloads, 0 problem(s)
```

`--verify` re-reads the written image, checks every wavetable now declares
`CODEC_S8` with `size == n_samples`, and byte-compares sampled payloads against
a fresh VADPCM decode. **That proves the data transformation end to end with no
console.**

⚠️ **Quantisation is round-to-nearest, not truncation.** The engine
reconstructs `s8 << 8`, so the right value is `round(pcm / 256)`. `pcm >> 8`
would add a half-LSB DC bias to every sample — inaudible alone, a hum once 12
voices sum.

⚠️ **`--verify` must NOT build an `AudioRom` on the new image.** Its structure
lives in tables compiled into the game, which still describe the old layout, so
`AudioRom`'s "extents sum to the file size" guard fires — **correctly**. That
guard is the cheap detector for exactly the drift below; do not weaken it.

### What is still to build

1. 🔴 **The index tables are compiled into the game, so they must move too** —
   `AudiodataHeaderStart` / `AudiowaveHeaderStart` in
   `src/static/jaudio_NES/game/audioheaders.c`
   (`kb/audio-aica-offload.md` §2). A `make_src_shrink.py` rule rewrites them.
   ⚠️ **THIS IS THE ONE REAL COUPLING RISK**: the image and the tables are two
   artefacts that must agree, and if they drift the game reads sample data at
   the wrong offsets and plays noise. Emit a checksum into both and check it at
   boot. `s8bank.py` prints the seven numbers it needs applied and the image's
   sha256:

   ```
   AudiodataHeaderStart entry 2 (tbl):  size 11970176
   AudiowaveHeaderStart 0: addr 0x0      -> 0x0        size   239,936 ->   409,936
   AudiowaveHeaderStart 1: addr 0x3A940  -> 0x64160    size    57,088 ->   101,136
   AudiowaveHeaderStart 2: addr 0x48840  -> 0x7CC80    size   434,304 ->   769,680
   AudiowaveHeaderStart 3: addr 0xB28C0  -> 0x138B20   size 1,189,600 -> 1,623,968
   AudiowaveHeaderStart 4: addr 0x1D4FA0 -> 0x2C52C0   size   986,656 -> 1,749,968
   AudiowaveHeaderStart 5: addr 0x2C5DC0 -> 0x4706A0   size 4,118,048 -> 7,315,424
   ```

2. A kill switch. The natural one is free: **ship both images and pick at build
   time**, since nothing else changes.
3. **A listening pass.** Everything above is arithmetic; the noise-character
   risk is not something a counter can see.

### What is NOT verified

🔴 **Nothing has run.** The image is proven self-consistent and proven to decode
to the right PCM; it has never been in a build, let alone on a console. The
first run is also the first test of the image/table coupling, and its failure
mode is noise rather than a crash — so build it, boot it in Flycast, and check
`[NEOS_OUT] peak` is non-zero and the music is recognisable **before** spending
a CD-R on it.

---

## W2 — `MAC.W` ON THE ADPCM FILTER IS SAFE, AND THAT IS NOW PROVEN

`kb/RESUME.md` §6c blocks this:

> **`MAC.W` the resampler FIRs** — up to ~2.7 %, medium. ⚠️ Do NOT extend it to
> the ADPCM chain without a proven bound: `sp9C[l] << temp_r19` with
> `temp_r19 ∈ 0..15` can exceed s16.

**That is a true statement about the FORMAT and a false one about THIS BANK.**
The scale nibble is four bits so 0..15 is what the encoding admits, and
`-8 << 15` is far outside s16 — but nobody could check what the data actually
contains, because nothing could read the bank. `tools/dcaudio/bounds.py` now
does, over **all 748,255 frames of all 1,157 samples**:

```
scale nibble: MAX OBSERVED 12          (13 is the first value that would break it)
   0:  0.81%   4:  7.18%   8: 16.40%  (peak)
  ...          7: 14.73%  12:  1.07%

MAC.W operand A, scaled nibbles    -32,768 ..   28,672    fits s16: True
MAC.W operand B, codebook coefs    -15,885 ..   17,732    fits s16: True
accumulator                    -71,921,779 .. 73,561,690  fits s32: True (29x headroom)
frames where the C wraps: 0
```

**Both operands fit s16 and the accumulator fits s32 with 29× margin.** The
`-32768` is exact and not a coincidence: nibbles run −8..+7, so the extreme is
`-8 << 12 = -32768`, which is precisely s16's lower bound. One more bit of
scale and this fails — the margin is one bit, so **do not treat the bound as
comfortable.**

The loop is a textbook `MAC.W` target — a dot product of up to 10 s16×s16 terms
(`rspsim.c:154-162`):

```c
s32 accu = (s32)sp7C[k] << 11;
accu += (s32)var_r5 * temp_r16[k + 8];
accu += (s32)var_r0 * temp_r16[k];
for (l = 0; l < k; l++) accu += (s32)sp7C[l] * temp_r16[k - l + 7];
var_r12 = accu >> 11;
```

`var_r5`/`var_r0` are clamped outputs and so are s16 by construction.

### Caveats, both of which matter

- ⚠️ **This bounds the DATA, not the FORMAT.** It is valid only because
  `audiorom.img` is fixed ROM data on the disc. Re-run `bounds.py` if the image
  is ever regenerated — **including by W1 above**, which rewrites it.
- ⚠️ **It does not say GCC will emit `MAC.W`**, only that doing so is correct.
  Whether the compiler cooperates, and whether it helps, needs a hardware run.
- ⚠️ **W1 makes W2 MOOT for the ADPCM chain** — if the bank is S8 there is no
  ADPCM filter left to optimise. They overlap; W2's independent value is the
  *resampler* FIRs, which W1 does not touch.

---

## Ranking, against the offload

| | win | runtime code | risk | resolved by |
|---|---|---|---|---|
| **console mute on a burn** | **~5.25 % of busy** | none, one env var | none (loses crash dumps) | already measured, `kb/RESUME.md` §2 |
| **W1 — S8 bank** | ~5.5 % of busy | **none** | image/table coupling; noise character | ears + a burn |
| W2 — `MAC.W` | ≤2.7 %, medium | a shrink rule | bound is one bit tight | a burn |
| AICA offload | ~19 % of busy | a lot | loop state, reverb loss, latency skew | several sessions + a burn |

⚠️ **All four are the same 18.9 % of busy in the end.** W1 and the offload
attack it at the same place and W1 is a strict subset of what the offload
deletes — **do not count them additively.** W1's argument is that it gets a
third of the offload's win for a small fraction of the work and risk, and that
it is a strictly better place to be if the offload stalls.
